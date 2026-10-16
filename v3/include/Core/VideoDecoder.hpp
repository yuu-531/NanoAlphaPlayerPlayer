#pragma once
#ifndef NANOALPHA_CORE_VIDEO_DECODER_HPP
#define NANOALPHA_CORE_VIDEO_DECODER_HPP

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
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "Core/MediaClock.hpp"
#include "Core/SubtitleSource.hpp"

namespace nanoalpha {

// =============================================================================
//  D3D11 即時コンテキスト共有ロック (再帰可能・FIFO 公平)
// -----------------------------------------------------------------------------
//  FFmpeg のデコードスレッドとアプリ側の描画が同一 ID3D11DeviceContext を共有する
//  ため、両者はこのロックで直列化しなければならない (同時使用は未定義動作)。
//
//  ただし std::mutex / Windows SRWLOCK は「解放直後にまた取得するスレッド」を
//  飛ばして待ち行列へ割り込む (バージング) ので、毎フレームロックを取る描画側が
//  デコード側を飢餓状態にすることがある (実測: HEVC の send が数秒ブロック)。
//  そこで待ち行列を明示的に管理した FIFO の再帰ロックを使用する。
// =============================================================================
class ContextLock {
public:
    ContextLock() = default;
    ~ContextLock() = default;

    ContextLock(const ContextLock&) = delete;
    ContextLock& operator=(const ContextLock&) = delete;

    void lock() {
        const std::thread::id self = std::this_thread::get_id();
        std::unique_lock<std::mutex> guard(m_mutex);
        if (m_owner == self) {
            ++m_depth; // 再帰 (FFmpeg は入れ子でロックする)
            return;
        }
        const std::uint64_t ticket = m_ticket++;
        // FIFO: 番号順に、かつ空きになったときだけ獲得できる
        m_condition.wait(guard, [this, ticket] {
            return !m_owned() && m_serving == ticket;
        });
        m_owner = self;
        m_depth = 1;
        ++m_serving;
    }

    void unlock() {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (--m_depth > 0) return;
        m_owner = std::thread::id{};
        m_depth = 0;
        m_condition.notify_all();
    }

private:
    bool m_owned() const noexcept { return m_owner != std::thread::id{}; }

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::thread::id m_owner{};
    int m_depth = 0;
    std::uint64_t m_ticket = 0;  // 次に発行する番号
    std::uint64_t m_serving = 0; // 次に獲得できる番号
};

class PlayerController;

// =============================================================================
//  FFmpeg D3D11VA Zero-Copy 映像デコードエンジン (Phase 1)
// -----------------------------------------------------------------------------
//  ・AV_HWDEVICE_TYPE_D3D11VA でハードウェアデコード
//    (アプリ側の ID3D11Device を AVD3D11VADeviceContext へ注入し、同一デバイス上で完結)
//  ・AV_PIX_FMT_D3D11 のサーフェスを CPU へコピー (av_hwframe_transfer_data) せず、
//    配列テクスチャのまま HLSL (NV12 / P010 -> RGB) へ渡す
//  ・デモュレクス / デコードは常駐スレッド。描画はメインスレッドのみ
//  ・再生ピッチ・A-B リピートは PlayerController からロックレスに取得して反映
//
//  スレッド契約:
//    open/close/acquireFrame/seek  → メインスレッド
//    av_read_frame / avcodec_*      → デコードスレッドのみ
// =============================================================================
class VideoDecoder {
public:
    struct Info {
        UINT width = 0;
        UINT height = 0;
        double fps = 0.0;
        double duration = 0.0;            // 秒
        bool hardwareDecoding = false;
        int videoStreamIndex = -1;
        int videoTrackCount = 1;
        int pixelFormatBits = 8;          // 8 or 10
    };

    // 描画用ビュー。texture はデコード済みサーフェス (CPU 転送なし)
    struct FrameView {
        ID3D11Texture2D* texture = nullptr;
        int arrayIndex = 0;               // 配列テクスチャのスライス
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;                   // 映像の実サイズ (縦横比に使用)
        UINT height = 0;
        UINT textureWidth = 0;            // サーフェス実サイズ (UV 正規化に使用)
        UINT textureHeight = 0;           //   デコーダは高さをアライン分パディングする
        bool tenBit = false;              // P010
        bool limitedRange = true;
        double pts = 0.0;                 // 秒
    };

    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    void attach(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;

    // Phase 1: D3D11VA デバイスコンテキストをアプリのデバイスへ接続する (100ms 予算内)
    bool init(ID3D11Device* device, ID3D11DeviceContext* context) noexcept;

    // ファイルを開き、デコードスレッドを起動する
    bool open(const std::wstring& source, PlayerController* controller = nullptr);
    void close() noexcept;

    // 音声マスタークロックを接続する (open 前に設定)。有効な間は pace() が
    // 音声の再生位置を基準にフレーム表示タイミングを決める (A/V 同期の本体)。
    void setClock(const MediaClock* clock) noexcept { m_clockSource = clock; }

    // 描画: 新着フレームがあれば差し替え、無ければ現在のフレームを維持する
    FrameView acquireFrame() noexcept;
    void releaseFrame() noexcept;         // サーフェスをプールへ返却

    void seek(double seconds) noexcept;

    bool isReady() const noexcept { return m_ready.load(std::memory_order_acquire); }
    bool hasFrame() const noexcept { return m_hasFrame.load(std::memory_order_acquire); }
    // 音声のみファイルでは映像クロックが更新されないので音声クロックを使う
    double currentTime() const noexcept;

    // 直近 2 フレームの実 pts 間隔の EMA (秒)。1.0 / これが「今再生している
    // 区間の瞬間フレームレート」。デコードスレッドが publishFrame (= コンテンツ
    // 順に並んだ全フレーム) で更新するため、描画 (Present) レートには依存しない。
    //   - 描画を 60fps に抑えていても 240fps 素材なら 1/240 を拾える
    //   - コンテナ平均 fps (info().fps) ではなく VFR / 区間変化にも追随する
    // 既定値 1/60 (映像が無い / リセット直後)。
    double contentFrameInterval() const noexcept {
        return m_contentInterval.load(std::memory_order_relaxed);
    }

    Info info() const;
    const std::wstring& source() const noexcept { return m_source; }

    // ---- 外部バックエンド (VLC / 暗号化 DVD) の状態上書き ---------------------
    //  FFmpeg 経路では currentTime() / info() が正規の値を返すが、暗号化 DVD は
    //  libVLC が再生するため位置とメディア情報を持ち主が違う。
    //  active の間だけ currentTime() / info() がこの値を返すようにして、
    //  UI のシークバー / 情報タブ / 終端検出をそのまま動かせる。
    void setExternalStatus(bool active, double timeSec, const Info& status) noexcept;
    bool externalActive() const noexcept { return m_extActive.load(std::memory_order_acquire); }

    // ---- 字幕 (v2: SRT / ASS / SSA) ------------------------------------------------
    // サイドカー字幕ファイルを読み込み、現在再生中のメディアへ適用する。
    // サイドカーは埋め込み字幕より優先される (読み込みに成功すると埋め込み cue は捨てる)。
    bool attachSubtitleFile(const std::wstring& path);

    // 現在時刻に対応する字幕テキスト (UTF-8)。表示対象が無ければ空。
    std::string subtitleAt(double seconds) const;
    bool hasSubtitles() const;
    std::size_t subtitleCount() const;

    // .srt / .ass / .ssa / .vtt と判定する (openInput で先に分岐するため)
    static bool isSubtitlePath(const std::wstring& path) {
        return SubtitleSource::isSubtitlePath(path);
    }

    // D3D11 即時コンテキストの共有ロック。
    // FFmpeg のデコードスレッドはこのロックを内部で取得する (AVD3D11VADeviceContext の
    // lock/unlock) ので、アプリ側の描画 (clear / draw / present) も同じロックで
    // 直列化しなければならない。ID3D11DeviceContext を 2 スレッドで同時に使うのは
    // 未定義動作 (実機クラッシュ)。
    ContextLock& contextMutex() const noexcept;

    // デコード品質プリセット (v2)。0=高速 / 1=標準 / 2=高画質。
    // デブロックフィルタと非参照フレームの扱いを切り替える。
    // デコードスレッドが次のパケット送信時に反映する (アトミック経由)。
    void setDecodeQuality(int quality) noexcept {
        if (quality < 0) quality = 0;
        if (quality > 2) quality = 2;
        m_decodeQuality.store(quality, std::memory_order_relaxed);
    }
    int decodeQuality() const noexcept {
        return m_decodeQuality.load(std::memory_order_relaxed);
    }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    SubtitleSource m_subtitles; // サイドカー / 埋め込みの字幕キュー
    const MediaClock* m_clockSource = nullptr;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_hasFrame{false};
    std::atomic<double> m_clock{0.0};
    // ソースの瞬間フレーム間隔 (EMA / 秒)。publishFrame (デコードスレッド) が更新。
    std::atomic<double> m_contentInterval{1.0 / 60.0};
    // 外部バックエンド (VLC) の状態上書き
    std::atomic<bool> m_extActive{false};
    std::atomic<double> m_extTime{0.0};
    std::atomic<double> m_extDuration{0.0};
    std::atomic<UINT> m_extWidth{0};
    std::atomic<UINT> m_extHeight{0};
    std::atomic<double> m_extFps{0.0};
    std::atomic<int> m_decodeQuality{1}; // 0=高速 1=標準 2=高画質
    std::wstring m_source;
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_VIDEO_DECODER_HPP
