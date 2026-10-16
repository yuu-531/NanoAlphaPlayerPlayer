#pragma once
#ifndef NANOALPHA_EXTENSIONS_EXTENSION_MANAGER_HPP
#define NANOALPHA_EXTENSIONS_EXTENSION_MANAGER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace nanoalpha {

class AudioEngine;
class VideoDecoder;
class PlayerController;

// Phase 1 が所有するサービス群への借用参照 (非所有・生存は main 側が保証)。
struct ExtensionHost {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HWND hwnd = nullptr;
    AudioEngine* audio = nullptr;
    VideoDecoder* video = nullptr;
    PlayerController* player = nullptr;
};

enum class ExtensionId : std::uint8_t {
    FrameGeneration = 0,   // Radeon Compute Shader によるフレーム生成 (24fps -> 60/120/144)
    OpenRgb,               // OpenRGB SDK (127.0.0.1:6742) ライティング同期
    Disc,                  // CD-DA (32bit アップコンバート) / DVD-Video
    NetworkStream,         // RTSP / RTMP / HTTP / HLS / SMB / FTP
    VlcFeatures,           // 変換・配信・キャプチャ・壁紙モード・VR・プレイリスト
    Count
};

enum class ExtensionState : std::uint8_t {
    Pending,     // 未着手 (またはオプションで無効)
    Loading,     // バックグラウンドでロード中
    Ready,       // ロード完了 + メインスレッドで UI バインド済み
    Failed,      // ロード失敗
    TimedOut,    // 3 秒予算超過で打ち切り
    Cancelled    // シャットダウン等によるキャンセル
};

enum class LoadPriority : std::uint8_t {
    Critical = 0,
    High,
    Normal,
    Low
};

std::string_view toString(ExtensionId id) noexcept;
std::string_view toString(ExtensionState state) noexcept;

// -----------------------------------------------------------------------------
// ローダの結果。ok のとき instance はメインスレッドで ReadyHook へ渡される。
// instance の型は ExtensionId ごとに固有 (各拡張ヘッダで定義)。
// -----------------------------------------------------------------------------
struct LoadResult {
    bool ok = false;
    bool timedOut = false;
    std::string message;
    std::shared_ptr<void> instance;

    static LoadResult success(std::string message = {}) {
        return {true, false, std::move(message), {}};
    }
    static LoadResult success(std::shared_ptr<void> object, std::string message = {}) {
        return {true, false, std::move(message), std::move(object)};
    }
    static LoadResult error(std::string message) {
        return {false, false, std::move(message), {}};
    }
    static LoadResult timeout(std::string message) {
        return {false, true, std::move(message), {}};
    }
};

// -----------------------------------------------------------------------------
// ローダに渡される実行文脈。
// 実装は必ず stop / deadline を監視し、3 秒予算内 (目安 100ms/個) で返ること。
// -----------------------------------------------------------------------------
struct LoadContext {
    const ExtensionHost& host;
    std::stop_token stop;
    std::chrono::steady_clock::time_point deadline;

    bool stopped() const noexcept { return stop.stop_requested(); }
    bool expired() const noexcept {
        return std::chrono::steady_clock::now() >= deadline;
    }
    std::chrono::milliseconds remaining() const noexcept {
        using namespace std::chrono;
        const auto left = deadline - steady_clock::now();
        if (left <= steady_clock::duration::zero()) return 0ms;
        return duration_cast<milliseconds>(left);
    }
};

using LoaderFn = std::function<LoadResult(const LoadContext&)>;

// ロード完了後、メインスレッド (tick 内) で UI / 制御系へ配線するために呼ばれる。
// ローダが生成した実体は ExtensionManager (Entry::instance) が保持する。
using ReadyHook =
    std::function<void(ExtensionId, const ExtensionHost&, const std::shared_ptr<void>&)>;

// =============================================================================
//  ExtensionManager — Phase 2 遅延ロード基盤
// -----------------------------------------------------------------------------
//  ・バックグラウンドワーカ (std::jthread) が各ローダを std::async で並列実行
//  ・全体予算は既定 3000ms。超過分は TimedOut として打ち切る
//  ・メインスレッドは毎フレーム tick() を呼ぶだけ (ノンブロッキング)
//  ・state / isReady はアトミック読みのみ → UI が即座に安全に参照できる
//  ・Ready は「メインスレッドでバインド済み」を意味する (配線前に公開しない)
//
//  スレッド契約:
//    add() / start() / tick() / state() ほか UI 用 API → メインスレッド
//    loader 実行                                              → ワーカースレッド群
// =============================================================================
class ExtensionManager {
public:
    struct Config {
        std::chrono::milliseconds totalBudget{3000};  // Phase 2 全体の遅延ロード予算
        std::size_t maxConcurrent = 3;                // 同時実行ローダ数上限
        bool failRemainingOnTimeout = true;           // 予算超過後の残りを TimedOut 扱いにする
    };

    explicit ExtensionManager(Config config = {}) noexcept : m_config(config) {
        // スロットは生成時に固定する (ワーカースレッドからのインデックス参照と
        // メインスレッドの add() が競合しないように、vector は再配置しない)。
        m_slots.resize(static_cast<std::size_t>(ExtensionId::Count));
    }
    ~ExtensionManager();

    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    // ---- 登録 (Phase 1 中・メインスレッド・start() より前) ----
    //  start() 後の登録も許可する (v3: 起動後に有効化された拡張を後から加えるため)。
    //  既にワーカが待機中の場合、登録を知らせて即座にロードを再開させる。
    bool add(ExtensionId id, LoadPriority priority, LoaderFn loader, std::string name = {});

    // ---- 拡張の ON/OFF (v3) ----
    //  メインスレッドからのみ呼ぶ。ノンブロッキング。
    //    無効化: Pending/Ready → Cancelled (Ready は instance を保持し、
    //            再有効化時に ReadyHook を再発火して即座に復帰する)
    //    有効化: 未ロードなら Pending に戻してワーカへ通知 (ロード再開)
    bool setEnabled(ExtensionId id, bool enabled) noexcept;
    bool enabled(ExtensionId id) const noexcept;

    // ---- 起動 / 停止 ----
    bool start(ExtensionHost host);
    void requestStop() noexcept;  // ローダは stop トークンを監視して速やかに脱退する
    void join();                  // ワーカ完了待ち (拡張破棄前に必ず呼ぶ)

    // ---- メインスレッド API (毎フレーム・ノンブロッキング) ----
    // 完了通知のデキュー + ReadyHook 実行 (UI バインド) のみを行う。
    void tick();

    // ---- 状態問い合わせ (アトミック読みのみ / UI から即時利用可) ----
    ExtensionState state(ExtensionId id) const noexcept;
    bool isReady(ExtensionId id) const noexcept;
    bool registered(ExtensionId id) const noexcept;
    std::size_t readyCount() const noexcept;
    std::size_t registeredCount() const noexcept;
    bool allSettled() const noexcept;

    // ---- 詳細情報 (tick() と同じメインスレッドからのみ呼ぶ) ----
    std::string_view name(ExtensionId id) const noexcept;
    std::string message(ExtensionId id) const;
    std::chrono::milliseconds elapsed(ExtensionId id) const noexcept;
    std::chrono::milliseconds totalElapsed() const noexcept;

    // ---- UI バインド ----
    void setReadyHook(ReadyHook hook);

    // シャットダウン / テスト用: settle するか timeout まで待つ (メインスレッドから呼ぶ)
    bool waitForSettled(std::chrono::milliseconds timeout);

    const Config& config() const noexcept { return m_config; }

private:
    struct Entry {
        ExtensionId id{};
        LoadPriority priority = LoadPriority::Normal;
        std::string name;
        LoaderFn loader;
        std::atomic<ExtensionState> state{ExtensionState::Pending};
        // v3: 有効/無効 (無効の Entry はワーカが一切触らない)
        std::atomic<bool> enabled{true};
        // ワーカが処着手済みかのフラグ (ロード再開時に取り直す)
        std::atomic<bool> claimed{false};
        // 以下はメインスレッド (デキュー側) のみが書き込む
        std::string message;
        std::chrono::milliseconds elapsed{0};
        // ローダが生成した実体 (LoadResult::instance)。生存期間をここで保持し、
        // ReadyHook へ生ポインタではなく shared_ptr として渡す。
        std::shared_ptr<void> instance;
    };

    struct Completion {
        std::size_t index = 0;
        LoadResult result;
        std::chrono::milliseconds elapsed{0};
        ExtensionState finalState = ExtensionState::Failed;
    };

    void run(std::stop_token token);
    void executeOne(std::size_t index, std::stop_token token,
                    std::chrono::steady_clock::time_point deadline);
    void enqueue(Completion completion);
    void drain();                     // tick() / join() / waitForSettled() から呼ぶ
    std::vector<std::size_t> sortedOrder() const;
    Entry* slot(ExtensionId id) noexcept;
    const Entry* slot(ExtensionId id) const noexcept;
    bool hasClaimableWork() const noexcept;   // ワーカが拾える未着手 Entry があるか
    void notifyWorker() noexcept;             // 待機中のワーカを起こす
    void requestWork() noexcept;              // m_settled を解除してワーカへ通知

    Config m_config;
    std::vector<std::unique_ptr<Entry>> m_slots;  // ExtensionId::Count 分 (生成時に固定)
    std::vector<std::size_t> m_regOrder;          // 登録順 (優先度同点時の順序を保持)
    mutable std::mutex m_regMutex;                // m_regOrder / スロット書込の排他
    ExtensionHost m_host{};

    std::jthread m_worker;
    std::atomic<bool> m_started{false};

    mutable std::mutex m_mutex;       // m_queue と待ち合わせの排他
    std::condition_variable m_cv;
    std::vector<Completion> m_queue;

    std::atomic<bool> m_settled{false};
    std::atomic<bool> m_workRequested{false};  // 待機中のワーカへの再開通知
    ReadyHook m_readyHook;            // メインスレッドのみが設定・実行
    bool m_totalCaptured = false;
    std::chrono::steady_clock::time_point m_startTime{};
    std::chrono::milliseconds m_totalElapsed{0};
};

} // namespace nanoalpha

#endif // NANOALPHA_EXTENSIONS_EXTENSION_MANAGER_HPP
