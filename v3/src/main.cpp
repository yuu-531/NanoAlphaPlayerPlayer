// =============================================================================
//  NanoAlphaPlayer — 二段階起動エントリポイント (src/main.cpp)
//
//  Phase 1 (メインスレッド / 100ms 予算)
//    Win32 ウィンドウ・D3D11 デバイス・Dear ImGui・WASAPI・FFmpeg 再生エンジン。
//    先行起動し、以下を即時有効化する。
//      映像: 回転 / 反転 / アスペクト / クロップ / 映像トラック切替。
//      音声: 32bit 10-Band EQ / 音声トラック / ステレオモード。
//      制御: 字幕 / 速度 (0.25x-4.0x) / A-B リピート。
//  Phase 2 (バックグラウンド / 3 秒予算 / UI スタッター禁止)
//    ExtensionManager が std::jthread + std::async で拡張を遅延ロードし、
//    完了処理はメインスレッドの tick() 内でアトミックに UI へバインドする。
//      Frame Generation / OpenRGB / CD-DVD / ストリーミング / VLC 機能群
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/AudioDecoder.hpp"
#include "Core/AudioEngine.hpp"
#include "Core/CrashReporter.hpp"
#include "Core/Log.hpp"
#include "Core/NvidiaSupport.hpp"
#include "Core/PlayerController.hpp"
#include "Core/Settings.hpp"
#include "Core/VideoDecoder.hpp"
#include "Core/VideoRenderer.hpp"
#include "Core/Window.hpp"
#include "Extensions/BuiltinLoaders.hpp"
#include "Extensions/ExtensionManager.hpp"
#include "UI/FileDialog.hpp"
#include "UI/ImGuiOverlay.hpp"
#include "Extensions/MediaTools.hpp"
#include "Extensions/OpenRgbClient.hpp"
#include "Extensions/VlcVideo.hpp"

#include <shellapi.h>
#include <mmsystem.h> // timeBeginPeriod (サブ 16ms のフレーム制御に必要)
#pragma comment(lib, "winmm.lib")

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

#ifndef NANOALPHA_CONSOLE
#define NANOALPHA_CONSOLE 0
#endif

namespace {

using namespace nanoalpha;
using Clock = std::chrono::steady_clock;

// メインスレッドが COM (MTA) を初期化した状態 (終了処理に残しておくため)
bool g_comMainInitialized = false;

// Phase 1 が所有するコアサービス群
struct Phase1Core {
    Window window;
    AudioEngine audio;
    AudioDecoder sound;
    VideoDecoder video;
    VideoRenderer renderer;
    PlayerController player;
    ui::ImGuiOverlay overlay;
    ui::BootStats boot;
    ExtensionHost host;

    // ---- Phase 2 で ReadyHook が受け取る実体 (ExtensionManager が保持する) ----
    std::shared_ptr<tools::VlcFeatures> vlc;  // プレイリスト / スナップショット / 壁紙
    std::shared_ptr<rgb::Client> rgb;         // OpenRGB SDK クライアント
    std::shared_ptr<void> frameGen;           // フレーム補間の検証結果

    // ---- 暗号化 DVD の再生 (VLC の dll を動的ロードして libVLC に任せる) ----
    //   名前が紛らわしいが core.vlc はメディアツール拡張 (プレイリスト等) であり、
    //   こちらは VideoLAN の libvlc.dll を読む再生バックエンド。
    vlc::Video dvdPlayer;
    bool dvdPaused = false;                  // libVLC へ渡した一時停止状態のエッジ
    int dvdVolumePercent = -1;               // libVLC へ渡した音量のエッジ
    std::uint64_t dvdSeekRevision = 0;       // VLC が消費したシーク要求のリビジョン
    bool dvdWasPlaying = false;              // 終端検出用 (再生してた → 止まった)
    // LED 更新は別スレッドから行うため生ポインタをアトミックで共有する
    // (実体の生存は ExtensionManager が保証する)
    std::atomic<rgb::Client*> rgbPtr{nullptr};

    // キーボード (Ctrl+O) からのファイル選択要求
    bool pendingOpenFile = false;

    // 再生終了 (EOF) を検出した。ContextLock の外で処理するため要求フラグのみ持つ。
    bool pendingEndOfTrack = false;

    // 直前にオープンしたメディア (字幕サイドカー以外)。
    // Phase 2 の遅延ロード後にプレイリストを埋めるために使う。
    std::wstring lastInput;

    // メッセージフックで観測した修飾キーの状態。
    // GetKeyState() は物理キーボードからの入力のみを反映するため、
    // SendMessage 経由のキー送信 (自動検証) では立たない。WM_KEYDOWN/UP を
    // フック側で拾って併用することで双方で Ctrl+O が効くようにする。
    bool ctrlHeld = false;
};

// 起動引数 (1 番目) に再生ファイルが渡されている場合
std::wstring commandLineInput() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return {};
    std::wstring path;
    if (argc > 1 && argv[1]) path = argv[1];
    LocalFree(argv);
    return path;
}

// ---------------------------------------------------------------------------
//  UI 用日本語フォントの読み込み
// -----------------------------------------------------------------------------
//  既定の ImGui フォント (ProggyClean) は ASCII のみ対応で、日本語ラベルが全て
//  "?" になってしまいます。Meiryo / Yu Gothic 系をアトラスへ登録して直す。
//  10MB 級の読み込みになるので Phase 1 の並行タスクで済ませ、
//  ラスタライズ (数ms x 数十グリフ) は最初の描画時に行う。
std::vector<char> loadUiFont() {
    static const char* kCandidates[] = {
        "C:\\Windows\\Fonts\\YuGothR.ttc", // Yu Gothic
        "C:\\Windows\\Fonts\\meiryo.ttc",  // Meiryo
        "C:\\Windows\\Fonts\\msgothic.ttc" // MS Gothic
    };

    for (const char* path : kCandidates) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) continue;
        const std::streamsize size = in.tellg();
        if (size <= 0 || size > 64 * 1024 * 1024) continue;
        in.seekg(0, std::ios::beg);
        std::vector<char> data(static_cast<size_t>(size));
        if (in.read(data.data(), size)) {
            log::info("Overlay: UI font = %s (%lld KB)", path,
                      static_cast<long long>(size / 1024));
            return data;
        }
    }
    log::info("Overlay: 日本語フォントが見つからないため既定フォントを使用します");
    return {};
}

// ---------------------------------------------------------------------------
//  キーボードショートカット
// -----------------------------------------------------------------------------
//  WM_KEYDOWN のみを扱い、ウィンドウがフォーカスされていない時を除き
//  PostMessage 経由で届くため、外部操作にも使える。
// 全画面表示の切替 (UI 状態とウィンドウ枠を同時に切り替える)
void toggleFullscreenUi(Phase1Core& core) {
    const bool on = !core.overlay.fullscreen();
    core.overlay.setFullscreen(on);
    core.window.setFullscreen(on);
}

bool handleShortcuts(Phase1Core& core, UINT msg, WPARAM key, bool overlayWantsKeys) {
    if (msg != WM_KEYDOWN) return false;
    if (key == VK_TAB) core.overlay.logKeyCapture(); // Tab 移動の追跡 (自動検証用)
    if (overlayWantsKeys) { // ImGui が入力に応じている間は抑止
        core.overlay.logKeyCapture();
        log::info("Shortcut: suppressed by UI  key=0x%02IX",
                  static_cast<unsigned long long>(key));
        return false;
    }

    const PlayerController::Snapshot snap = core.player.snapshot();
    const bool ctrlDown = core.ctrlHeld || ((GetKeyState(VK_CONTROL) & 0x8000) != 0);

    // Ctrl+O: ファイル選択ダイアログ (ContextLock の外で開くため要求フラグのみ)
    if (ctrlDown && (key == 'O' || key == 'o')) {
        core.pendingOpenFile = true;
        log::info("Shortcut: open file dialog");
        return true;
    }

    // Ctrl+P: 従来の操作パネルを別ウィンドウで開閉 (旧ユーザー向け)
    if (ctrlDown && (key == 'P' || key == 'p')) {
        core.overlay.togglePanelWindow();
        return true;
    }

    // Ctrl+H: 操作パネルの非表示 / 表示切替
    if (ctrlDown && (key == 'H' || key == 'h')) {
        core.overlay.togglePanel();
        return true;
    }

    switch (key) {
    case VK_SPACE:
        core.player.setPaused(!snap.paused);
        log::info("Shortcut: pause -> %s", snap.paused ? "playing" : "paused");
        return true;
    case VK_F1:
        core.overlay.toggleLicenses();
        return true;
    case VK_F11:
        toggleFullscreenUi(core);
        return true;
    case VK_ESCAPE:
        // 全画面中だけ Esc で解除 (通常時は ImGui へ渡す)
        if (core.overlay.fullscreen()) {
            toggleFullscreenUi(core);
            return true;
        }
        return false;
    case 'F':
    case 'f':
        core.player.setFrameGen(!snap.frameGen);
        log::info("Shortcut: frame generation -> %s", snap.frameGen ? "off" : "on");
        return true;
    case VK_LEFT:
    case VK_RIGHT: {
        const double delta = (key == VK_LEFT) ? -5.0 : 5.0;
        const double target = core.video.currentTime() + delta;
        core.player.requestSeek(target);
        log::info("Shortcut: seek %+.0f s -> %.1f s", delta, target);
        return true;
    }
    case VK_UP:
    case VK_DOWN: {
        float volume = core.audio.volume() + ((key == VK_UP) ? 0.05f : -0.05f);
        if (volume < 0.0f) volume = 0.0f;
        if (volume > 4.0f) volume = 4.0f; // 0..400% (スライダーと同じ範囲)
        core.audio.setVolume(volume);
        log::info("Shortcut: volume -> %.0f %%", static_cast<double>(volume) * 100.0);
        return true;
    }
    case VK_OEM_4: // '['
    case VK_OEM_6: // ']'
    {
        float speed = snap.speed + ((key == VK_OEM_6) ? 0.25f : -0.25f);
        if (speed < 0.25f) speed = 0.25f;
        if (speed > 4.0f) speed = 4.0f;
        core.player.setSpeed(speed);
        log::info("Shortcut: speed -> %.2fx", static_cast<double>(speed));
        return true;
    }
    case 'R':
        core.player.rotate(90);
        log::info("Shortcut: rotate -> %d deg", core.player.snapshot().rotationDeg);
        return true;
    case 'S':
        core.player.setSubtitleVisible(!snap.subtitleVisible);
        log::info("Shortcut: subtitles -> %s",
                  snap.subtitleVisible ? "off" : "on");
        return true;
    case 'A':
        core.player.setPointA(core.video.currentTime());
        core.player.setAbRepeat(true);
        log::info("Shortcut: A-B repeat  A=%.2f s (B を設定すると開始)",
                  core.player.snapshot().pointA);
        return true;
    case 'B': {
        const double t = core.video.currentTime();
        if (t <= snap.pointA) {
            log::info("Shortcut: B (%.2f s) は A (%.2f s) より後にしてください", t, snap.pointA);
            return true;
        }
        core.player.setPointB(t);
        core.player.setAbRepeat(true);
        log::info("Shortcut: A-B repeat  A=%.2f s -> B=%.2f s", snap.pointA, t);
        return true;
    }
    case 'O': {
        // 出力モードの切替 = IAudioClient の作り直し (reopenOutput)
        const AudioEngine::OutputMode next =
            core.audio.outputMode() == AudioEngine::OutputMode::Shared
                ? AudioEngine::OutputMode::Exclusive
                : AudioEngine::OutputMode::Shared;
        core.audio.setOutputMode(next);
        const bool ok = core.audio.reopenOutput();
        log::info("Shortcut: output mode -> %s (%s)",
                  next == AudioEngine::OutputMode::Exclusive ? "exclusive" : "shared",
                  ok ? "reopen ok" : "reopen failed");
        return true;
    }
    case 'L': {
        // リピート再生の切り替え (オフ -> 全曲ループ -> 1曲ループ -> オフ)
        if (ctrlDown) return false;
        const PlayerController::RepeatMode mode = core.player.cycleRepeatMode();
        settings::g_values.repeatMode = static_cast<int>(mode);
        settings::save();
        log::info("Shortcut: repeat -> %s",
                  mode == PlayerController::RepeatMode::All
                      ? "all"
                      : (mode == PlayerController::RepeatMode::One ? "one" : "off"));
        return true;
    }
    case 'N':
        if (ctrlDown) return false;
        core.overlay.requestPlaylist(ui::UiAction::PlaylistNext);
        log::info("Shortcut: playlist next");
        return true;
    case 'P':
        if (ctrlDown) return false;
        core.overlay.requestPlaylist(ui::UiAction::PlaylistPrev);
        log::info("Shortcut: playlist prev");
        return true;
    case '1': case '2': case '3': case '4': case '5': case '6': {
        const int index = static_cast<int>(key) - static_cast<int>('1');
        core.overlay.selectTab(index);
        log::info("Shortcut: tab -> %d", index + 1);
        return true;
    }
    case 'H':
        log::info("Shortcut: Space pause  Ctrl+O open  Ctrl+H panel  Ctrl+P popout  "
                  "<-> seek  ^/v volume  [ ] speed  R rotate  S subtitles  "
                  "A/B A-B point  F framegen  L repeat  N/P playlist  O output mode  "
                  "1-6 tab (6=EQ)  F11 / Esc fullscreen  F1 licenses");
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
//  Phase 1: コア起動 (メインスレッド / 100ms 予算)
// ---------------------------------------------------------------------------
bool bootPhase1(Phase1Core& core, HINSTANCE instance, int showCmd) {
    const auto startedAt = Clock::now();
    auto elapsedMs = [&]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt)
            .count();
    };

    // 0. メインスレッドを COM の MTA に参加させておく。WASAPI 初期化スレッドや
    //    レンダラスレッドが抜けてもプロセスの MTA が生き続けるようにするため、
    g_comMainInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    // 1. 以下の 3 つは互いに依存しないため、ウィンドウ生成と並行して走らせる
    //    - WASAPI 初期化 (IAudioClient::Initialize の初回は数十ms を要することがある)
    //    - D3D11 デバイス生成 (ウィンドウに非依存)
    //    - HLSL 実行時コンパイル (デバイスに非依存、CPU のみ)
    auto audioTask = std::async(std::launch::async, [&core] { return core.audio.init(); });
    auto deviceTask = std::async(std::launch::async, [&core] { return core.window.createDevice(); });
    auto shaderTask = std::async(std::launch::async, [&core] { return core.renderer.compile(); });
    auto fontTask = std::async(std::launch::async, [] { return loadUiFont(); });

    // 2. Win32 ウィンドウ (並行タスクと競合しない)
    if (!core.window.createWindow(instance, showCmd)) {
        log::info("Phase 1: ウィンドウ初期化に失敗");
        audioTask.wait();
        deviceTask.wait();
        shaderTask.wait();
        fontTask.wait();
        return false;
    }
    const long long msWindow = elapsedMs();

    // 3. デバイス生成の完了とスワップチェーン (ウィンドウが既に存在するので直列で短い)
    if (!deviceTask.get()) {
        log::info("Phase 1: D3D11 初期化に失敗");
        audioTask.wait();
        shaderTask.wait();
        fontTask.wait();
        return false;
    }
    if (!core.window.createSwapChain()) {
        log::info("Phase 1: スワップチェーン初期化に失敗");
        audioTask.wait();
        shaderTask.wait();
        fontTask.wait();
        return false;
    }
    core.boot.d3dReady = true;
    core.window.logSetup();
    const long long msSwap = elapsedMs();

    // 5. Dear ImGui (D3D11 バックエンド)。無効でもプレイヤーは動作を継続する。
    core.overlay.init(core.window.hwnd(), core.window.device(), core.window.context(),
                      fontTask.get());
    core.overlay.bindBootStats(core.boot);
    core.window.setMessageHook([&core](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                       LRESULT& result) {
        // 修飾キーの状態をメッセージから観測しておく (SendMessage 経由の
        // キー送信でも GetKeyState() が立たないため、両方で判定する)
        if (msg == WM_KEYDOWN || msg == WM_KEYUP) {
            if (wParam == VK_CONTROL) core.ctrlHeld = (msg == WM_KEYDOWN);
        } else if (msg == WM_ACTIVATE || msg == WM_KILLFOCUS) {
            core.ctrlHeld = false; // フォーカス喪失時は状態をリセット
        }
        // ショートカットを先に処理し、処理したキーは ImGui へ渡さない。
        // (渡すとナビゲーションが Space / 矢印と競合し、ActiveId が残ったまま
        //  ショートカットが抑止され続ける原因になる)
        if (msg == WM_KEYDOWN) {
            // 全画面中はキー入力でコントロールバーを一時表示する
            core.overlay.notifyInput();
            // Tab で起動されたスライダの一時入力 (TempInput) はキーが届いた時点で
            // 必ず閉じる。閉じていないと 1-6 (タブ切替) などが入力ボックスに
            // 吸われて UI が固まった (v3 の重大バグ)。
            core.overlay.releaseActiveWidget(static_cast<unsigned>(wParam));
            if (handleShortcuts(core, msg, wParam,
                                core.overlay.wantsKeyboard(static_cast<unsigned>(wParam)))) {
                return true;
            }
        }
        return core.overlay.handleMessage(hwnd, msg, wParam, lParam, result);
    });
    core.window.setResizeHook([&core](UINT width, UINT height) {
        // TODO(phase1): VideoDecoder の描画ビューポート更新
        (void)width;
        (void)height;
    });
    const long long msOverlay = elapsedMs();

    // 6. Zero-Copy 描画パス (シェーダは t=0 から並行コンパイル済み、生成のみ)
    const bool rendererReady =
        shaderTask.get() && core.renderer.init(core.window.device(), core.window.context());
    const long long msRenderer = elapsedMs();

    // 7. FFmpeg D3D11VA Zero-Copy 再生エンジン
    core.boot.videoReady = core.video.init(core.window.device(), core.window.context());
    const long long msVideo = elapsedMs();

    // 8. WASAPI 初期化と完了待ち (ウィンドウ / D3D 生成と並行しているため)
    core.boot.audioReady = audioTask.get();
    const long long msAudio = elapsedMs();

    core.boot.phase1 =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - startedAt);

    core.host = ExtensionHost{core.window.device(), core.window.context(), core.window.hwnd(),
                              &core.audio, &core.video, &core.player};

    log::info("Phase 1 timeline (ms): window=%lld  d3d+swap=%lld  imgui=%lld  render=%lld  "
              "video=%lld  audio=%lld",
              msWindow, msSwap - msWindow, msOverlay - msSwap, msRenderer - msOverlay,
              msVideo - msRenderer, msAudio - msVideo);
    log::info("Phase 1 boot: %lld ms (budget 100 ms)  overlay=%s audio=%s video=%s renderer=%s",
              static_cast<long long>(core.boot.phase1.count()),
              core.overlay.enabled() ? "on" : "off",
              core.boot.audioReady ? "ready" : "stub",
              core.boot.videoReady ? "ready" : "stub",
              rendererReady ? "ready" : "failed");

    if (core.boot.phase1.count() > 100) {
        log::info("Phase 1: 100ms 予算を超過しました");
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Phase 2: 拡張の遅延ロード開始 (ここでメインスレッドは待たない)
// ---------------------------------------------------------------------------
void startPhase2(ExtensionManager& extensions, Phase1Core& core) {
    extensions.setReadyHook([&core](ExtensionId id, const ExtensionHost& host,
                                    const std::shared_ptr<void>& instance) {
        // tick() 内 (メインスレッド) で呼ばれる。UI は state()==Ready を見た時点で
        // 本フックの配線が完了している、と仮定する不変条件が保たれる。
        core.boot.boundExtensions.fetch_add(1, std::memory_order_relaxed);
        log::info("Phase 2 bound: %.*s  host=%p instance=%p",
                  static_cast<int>(toString(id).size()), toString(id).data(),
                  static_cast<const void*>(&host), instance.get());

        switch (id) {
        case ExtensionId::VlcFeatures:
            // プレイリスト / スナップショット / 壁紙の実体
            core.vlc = std::static_pointer_cast<tools::VlcFeatures>(instance);
            if (core.vlc) {
                core.overlay.setPlaylist(&core.vlc->playlist);
                // 起動引数のオープンが Phase 2 より先だった場合は、
                // ここでプレイリストを埋めておく (1 件)。
                if (core.vlc->playlist.empty() && !core.lastInput.empty()) {
                    core.vlc->playlist.setOrAdd(core.lastInput);
                    log::info("playlist: seeded with the current file (1 item)");
                }
            }
            break;
        case ExtensionId::OpenRgb:
            // 127.0.0.1:6742 の SDK クライアント (LED 更新は別スレッドから)
            core.rgb = std::static_pointer_cast<rgb::Client>(instance);
            core.rgbPtr.store(core.rgb.get(), std::memory_order_release);
            break;
        case ExtensionId::FrameGeneration:
            // 補間カーネルの GPU 検証結果。映像タブの「フレーム補間」が有効
            core.frameGen = instance;
            break;
        default:
            break;
        }
    });

    // v3: 設定で無効の拡張はローダ自体を登録しない (完全に読み込まれない)
    struct Gate {
        ExtensionId id;
        bool enabled;
        const char* key;
    };
    const settings::Values& sv = settings::g_values;
    const Gate gates[] = {
        {ExtensionId::FrameGeneration, sv.extFrameGen, "ext.framegen"},
        {ExtensionId::OpenRgb, sv.extRgb, "ext.rgb"},
        {ExtensionId::VlcFeatures, sv.extVlc, "ext.vlc"},
        {ExtensionId::Disc, sv.extDisc, "ext.disc"},
        {ExtensionId::NetworkStream, sv.extNetwork, "ext.network"},
    };

    int registeredCount = 0;
    int skippedCount = 0;
    for (const Gate& gate : gates) {
        if (gate.enabled) {
            if (nanoalpha::extensions::registerBuiltinLoader(extensions, gate.id)) ++registeredCount;
        } else {
            ++skippedCount;
            log::info("Phase 2: %s -> disabled by settings", gate.key);
        }
    }
    log::info("Phase 2: loaders registered=%d skipped=%d", registeredCount, skippedCount);

    extensions.start(core.host);
}

// ---------------------------------------------------------------------------
//  入力 (ファイル / URL) のオープン — 起動引数と UI の両方から呼ばれる
// -----------------------------------------------------------------------------
//  デコードスレッドの起動を伴うため、ContextLock の外側から呼ぶこと。
// ---------------------------------------------------------------------------
void openInput(Phase1Core& core, const std::wstring& input) {
    if (input.empty()) return;

    const auto openStarted = Clock::now();

    // ---- 字幕ファイル (SRT / ASS / SSA) ----
    // 再生中のメディアへサイドカーとして適用する (再生は止めない)。
    // メディアを開いていない場合は字幕のみ開く (黒画面 / duration 0)。
    // ※ 必ず sound.open より前で分岐すること (音声を閉じてしまうため)
    if (VideoDecoder::isSubtitlePath(input)) {
        if (core.video.isReady()) {
            if (core.video.attachSubtitleFile(input)) {
                core.player.setSubtitleVisible(true);
                log::info("input: subtitle attached  %Iu cue  (%lld ms)",
                          core.video.subtitleCount(),
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::now() - openStarted)
                              .count());
            } else {
                log::info("input: subtitle 読込失敗 (%lld ms)",
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              Clock::now() - openStarted)
                              .count());
            }
            return;
        }

        core.video.setClock(&core.audio.clock());
        const bool opened = core.video.open(input, &core.player);
        const long long openMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - openStarted)
                .count();
        if (opened) {
            core.player.setSubtitleVisible(true);
            log::info("input: subtitle open OK  %Iu cue  (%lld ms)",
                      core.video.subtitleCount(), openMs);
        } else {
            log::info("input: subtitle open 失敗 (%lld ms)", openMs);
        }
        return;
    }

    // 音声マスタークロックをここに接続 (音声が無くても動く、壁時計フォールバック)
    core.lastInput = input; // プレイリストへの反映は openInput 内で行う
    core.video.setClock(&core.audio.clock());

    // libVLC で再生中なら止め、通常の FFmpeg 経路へ移る
    if (core.dvdPlayer.isOpen()) {
        core.dvdPlayer.close();
        core.dvdPaused = false;
        core.dvdVolumePercent = -1;
        core.dvdWasPlaying = false;
        core.video.setExternalStatus(false, 0.0, VideoDecoder::Info{});
        log::info("VLC: closed (stopped the libVLC stream)");
    }

    // ---- DVD の検出と暗号化判定 (v3) ------------------------------------------------
    //  VIDEO_TS / .IFO / .VOB / .ISO / ドライブルートを受け取ったら構造を解析し、
    //  CSS でスクランブルされていれば VLC の libvlc.dll を読み込んで再生する
    //  (ユーザー確認済みの方針)。暗号が無ければ従来どおり FFmpeg 経路を使う。
    //  'vlc:<path>' は強制的に libVLC で開く指定 (検証用)。
    std::wstring ffmpegTarget = input;
    std::wstring vlcTarget;
    bool useVlc = false;
    if (input.size() > 4 && input.compare(0, 4, L"vlc:") == 0) {
        useVlc = true;
        vlcTarget = input.substr(4);
        log::info("dvd: 'vlc:' scheme -> forcing the libVLC path");
    } else {
        const vlc::DvdInfo dvd = vlc::detectDvd(input);
        if (dvd.isDvd) {
            const auto narrow = [](const std::wstring& text) {
                if (text.empty()) return std::string();
                const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                                       static_cast<int>(text.size()), nullptr, 0,
                                                       nullptr, nullptr);
                if (needed <= 0) return std::string();
                std::string out(static_cast<std::size_t>(needed), '\0');
                WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                    static_cast<int>(text.size()), out.data(), needed, nullptr,
                                    nullptr);
                return out;
            };
            log::info("dvd: detected root='%s' video_ts='%s' vob=%d encrypted=%s detect=%s%s",
                      narrow(dvd.root).c_str(), narrow(dvd.videoTs).c_str(), dvd.vobCount,
                      dvd.encrypted ? "yes" : "no", dvd.detectable ? "yes" : "no",
                      dvd.isIso ? " (iso)" : "");
            if (dvd.encrypted) {
                useVlc = true;
                vlcTarget = dvd.root;
                log::info("dvd: encrypted -> loading the VLC dll for playback");
            } else if (dvd.detectable && !dvd.probeFile.empty()) {
                ffmpegTarget = dvd.probeFile;
                log::info("dvd: not encrypted -> using the FFmpeg path  '%s'",
                          narrow(ffmpegTarget).c_str());
            } else {
                log::info("dvd: undetectable -> using the FFmpeg path");
            }
        }
    }

    if (useVlc) {
        const long long openMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - openStarted)
                .count();
        if (vlcTarget.empty()) {
            log::info("input: open failed (libVLC): empty target (%lld ms)", openMs);
            return;
        }
        // 二重音声を避けるため FFmpeg 側は閉じてから libVLC に渡す
        core.video.close();
        core.sound.close();

        std::string error;
        if (!core.dvdPlayer.load(&error)) {
            log::info("input: open failed (libVLC): %s (%lld ms)", error.c_str(), openMs);
            return;
        }
        if (!core.dvdPlayer.open(vlcTarget)) {
            log::info("input: open failed (libVLC): could not open the media (%lld ms)", openMs);
            return;
        }

        const PlayerController::Snapshot snap = core.player.snapshot();
        const int volumePercent = static_cast<int>(core.audio.volume() * 100.0f + 0.5f);
        core.dvdVolumePercent = volumePercent;
        core.dvdPaused = snap.paused;
        core.dvdSeekRevision = core.player.seekRevision(); // 過去の要求は引き継がない
        core.dvdPlayer.setVolumePercent(volumePercent);
        core.dvdPlayer.play();
        core.dvdPlayer.setPaused(snap.paused);

        // libVLC の再生開始は非同期 (入力スレッドが別スレッドで起動する)。
        // 「再生が始まった / 長さが判明した」のどちらかで成功とみなす。
        bool ready = false;
        for (int attempt = 0; attempt < 30 && !ready; ++attempt) {
            ready = core.dvdPlayer.playing() || core.dvdPlayer.durationMs() > 0;
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const long long vlcMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - openStarted)
                .count();

        if (!ready) {
            log::info("input: open failed (libVLC): playback did not start (%lld ms)", vlcMs);
            core.dvdPlayer.close();
            core.video.setExternalStatus(false, 0.0, VideoDecoder::Info{});
            return;
        }

        core.video.setExternalStatus(true, 0.0, core.dvdPlayer.mediaInfo());
        const VideoDecoder::Info info = core.video.info();
        log::info("input: open OK (libVLC)  duration=%.1fs  audio=vlc  (%lld ms)", info.duration,
                  vlcMs);
        if (core.vlc != nullptr) {
            const bool wasEmpty = core.vlc->playlist.empty();
            core.vlc->playlist.setOrAdd(input);
            if (wasEmpty) log::info("playlist: seeded with the current file (1 item)");
        }
        return;
    }

    // 音声トラックをここに開く (映像と同じファイル・同じシーク要求を共有するため)
    core.sound.attach(&core.audio, &core.player);
    const PlayerController::Snapshot snap0 = core.player.snapshot();
    const bool soundOpened = core.sound.open(ffmpegTarget, snap0.audioTrack);

    const bool opened = core.video.open(ffmpegTarget, &core.player);
    // 字幕 cue がロード済み (サイドカー / 埋め込み) なら表示を有効化
    if (opened && core.video.hasSubtitles()) {
        core.player.setSubtitleVisible(true);
    }
    const long long openMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - openStarted)
            .count();
    if (opened) {
        // 単一ファイル再生でもプレイリストを常に 1 件以上にしておくと、
        // リピート再生・前へ/次へ・終了時の自動送りが「開いた直後」から使える。
        // 加えて「再生中のファイル = 現在位置」を常に成立させる。
        //  (Ctrl+O / URL 欄で別ファイルを開いた直後は index が据え置きのまま
        //   だと、前へ/次へが再生中ファイルとズレた位置から動いてしまう)
        if (core.vlc != nullptr) {
            const bool wasEmpty = core.vlc->playlist.empty();
            core.vlc->playlist.setOrAdd(input);
            if (wasEmpty) log::info("playlist: seeded with the current file (1 item)");
        }
        const VideoDecoder::Info info = core.video.info();
        log::info("input: open OK  %ux%u  %.3f fps  %dbit  %s  audio=%s  (%lld ms)",
                  info.width, info.height, info.fps, info.pixelFormatBits,
                  info.hardwareDecoding ? "D3D11VA" : "software", soundOpened ? "on" : "off",
                  openMs);
    } else {
        log::info("input: open 失敗 (%lld ms)", openMs);
    }
}

// ---------------------------------------------------------------------------
//  Frame Generation の補間は VideoRenderer 内で行う
// ---------------------------------------------------------------------------
//  旧実装は mediaClock * fps の小数部 (CFR 前提) を補間倍率 N へ量子化していたが、
//  表示レートと N が一致しないと位相が跳ねて歩調がズレるうえ、VFR やクロック揺らぎに
//  弱かった。映像は全フレームの時刻が事前に分かっているので、実際に保持している
//  prev/curr の pts 差で連続補間する方が正確である (quantize は行わない)。
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  OpenRGB の LED 色を再生状態から決める (0xRRGGBB)
// ---------------------------------------------------------------------------
std::uint32_t hsvToRgb(double hueDeg, double saturation, double value) {
    hueDeg = std::fmod(hueDeg, 360.0);
    if (hueDeg < 0.0) hueDeg += 360.0;
    const double c = value * saturation;
    const double x = c * (1.0 - std::fabs(std::fmod(hueDeg / 60.0, 2.0) - 1.0));
    const double m = value - c;
    double r = 0.0, g = 0.0, b = 0.0;
    if (hueDeg < 60.0) { r = c; g = x; }
    else if (hueDeg < 120.0) { r = x; g = c; }
    else if (hueDeg < 180.0) { g = c; b = x; }
    else if (hueDeg < 240.0) { g = x; b = c; }
    else if (hueDeg < 300.0) { r = x; b = c; }
    else { r = c; b = x; }
    const auto toByte = [m](double v) {
        const double s = (v + m) * 255.0;
        return static_cast<std::uint32_t>(s < 0.0 ? 0.0 : (s > 255.0 ? 255.0 : s));
    };
    return (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
}

// 停止 = 暗い青 / 一時停止 = アンバー / 再生 = 速度と音量で色相・明度
void updateRgbFromPlayback(Phase1Core& core, std::atomic<std::uint32_t>& color) {
    if (core.rgbPtr.load(std::memory_order_acquire) == nullptr) return;

    const PlayerController::Snapshot snap = core.player.snapshot();
    std::uint32_t value = 0x1E3A5Cu;
    if (snap.paused) {
        value = 0xE08A1Eu;
    } else if (core.video.isReady() && core.video.currentTime() > 0.0) {
        const double speedScale = (static_cast<double>(snap.speed) - 0.25) / 3.75;
        const double hue = speedScale * 260.0 + std::fmod(core.video.currentTime() * 8.0, 60.0);
        // 音量ブースト (1.0 超) では LED 明るさが 1.0 を超えないよう殺す
        const double volumeLevel = static_cast<double>(core.audio.volume());
        const double brightness =
            0.30 + 0.70 * ((volumeLevel > 1.0) ? 1.0 : volumeLevel);
        value = hsvToRgb(hue, 0.85, brightness);
    }
    color.store(value, std::memory_order_release);
}

// ---------------------------------------------------------------------------
//  再生終了時の送り (リピート再生 / プレイリストの自動進行)
// -----------------------------------------------------------------------------
//  検出はメインループの毎フレームで行い、処理自体は ContextLock の外から呼ぶ。
//    オフ   : プレイリスト末尾 (または単一ファイル) で停止
//    全曲   : プレイリストをループして次へ進む / 単一ファイルは先頭へ戻る
//    1曲    : 同じ曲を先頭から繰り返す
// ---------------------------------------------------------------------------
void advanceAfterEnd(Phase1Core& core) {
    const PlayerController::Snapshot snap = core.player.snapshot();
    const PlayerController::RepeatMode mode = snap.repeatMode;

    if (mode == PlayerController::RepeatMode::One) {
        core.sound.clearEnded();
        core.player.requestSeek(0.0);
        log::info("repeat: one -> 0.0 s");
        return;
    }

    tools::Playlist* list = (core.vlc != nullptr) ? &core.vlc->playlist : nullptr;
    const bool wrap = (mode == PlayerController::RepeatMode::All);
    if (list != nullptr && list->next(wrap)) {
        const std::wstring next = list->current();
        log::info("repeat: %s  playlist -> %ls", wrap ? "all" : "off", next.c_str());
        openInput(core, next);
        return;
    }

    core.sound.clearEnded();
    if (wrap) {
        // 全曲リピートでも送れない (1 件しかない等) → 先頭へ戻して再生を続ける
        core.player.requestSeek(0.0);
        log::info("repeat: all -> 0.0 s");
        return;
    }
    core.player.setPaused(true);
    log::info("repeat: off -> end of playlist, paused");
}

// ---------------------------------------------------------------------------
//  UI / キーボードからの要求を実行する (ContextLock の外側で呼ぶ)
// ---------------------------------------------------------------------------
void processUiActions(Phase1Core& core) {
    if (core.pendingOpenFile) {
        core.pendingOpenFile = false;
        const std::wstring path = ui::pickMediaFile(core.window.hwnd());
        if (!path.empty()) openInput(core, path);
    }

    for (int pass = 0; pass < 8; ++pass) {
        std::wstring payload;
        const ui::UiAction action = core.overlay.takeAction(payload);
        if (action == ui::UiAction::None) break;

        switch (action) {
        case ui::UiAction::OpenFile: {
            const std::wstring path = ui::pickMediaFile(core.window.hwnd());
            if (!path.empty()) openInput(core, path);
            break;
        }
        case ui::UiAction::OpenFiles:
        case ui::UiAction::OpenFilesPlay: {
            const std::vector<std::wstring> paths = ui::pickMediaFiles(core.window.hwnd());
            if (paths.empty()) break;
            if (core.vlc) {
                const bool wasEmpty = core.vlc->playlist.empty();
                core.vlc->playlist.addMany(paths);
                //  「複数開く…」は必ず先頭の 1 件から再生を始める。
                //  「ファイルを追加…」は既存の再生を止めない (空のときだけ開始)。
                if (action == ui::UiAction::OpenFilesPlay || wasEmpty) {
                    openInput(core, paths.front());
                }
                log::info("playlist: %zu 件を追加 (計 %zu 件)", paths.size(),
                          core.vlc->playlist.size());
            } else {
                openInput(core, paths.front());
            }
            break;
        }
        case ui::UiAction::OpenUrl:
            if (!payload.empty()) openInput(core, payload);
            break;
        case ui::UiAction::Snapshot: {
            const std::wstring suggested = L"nanoalpha_snapshot.png";
            const std::wstring path = ui::saveFile(core.window.hwnd(), suggested, L"*.png");
            if (path.empty()) break;
            // バックバッファの読み出しは即時コンテキストを使うため ContextLock 内側
            {
                const std::lock_guard<ContextLock> guard(core.video.contextMutex());
                if (tools::saveBackbufferPng(core.window.swapChain(), path) && core.vlc) {
                    core.vlc->lastSnapshot = path;
                }
            }
            break;
        }
        case ui::UiAction::SetWallpaper:
            if (!core.vlc || core.vlc->lastSnapshot.empty()) {
                log::info("wallpaper: 先にスナップショットを保存してください");
                break;
            }
            core.vlc->wallpaperEnabled = tools::setDesktopWallpaper(core.vlc->lastSnapshot);
            break;
        case ui::UiAction::PlaylistNext:
        case ui::UiAction::PlaylistPrev: {
            if (!core.vlc) break;
            const int delta = (action == ui::UiAction::PlaylistNext) ? 1 : -1;
            if (core.vlc->playlist.move(delta)) {
                const std::wstring next = core.vlc->playlist.current();
                log::info("playlist: %ls", next.c_str());
                openInput(core, next);
            }
            break;
        }
        case ui::UiAction::PlaylistJump: {
            if (!core.vlc || payload.empty()) break;
            wchar_t* end = nullptr;
            const unsigned long index = std::wcstoul(payload.c_str(), &end, 10);
            if (end == payload.c_str()) break;
            if (core.vlc->playlist.seekTo(static_cast<size_t>(index))) {
                const std::wstring next = core.vlc->playlist.current();
                log::info("playlist: jump %lu -> %ls", index, next.c_str());
                openInput(core, next);
            }
            break;
        }
        case ui::UiAction::ToggleFullscreen:
            toggleFullscreenUi(core);
            break;
        default:
            break;
        }
    }
}

// ---------------------------------------------------------------------------
//  FFmpeg DLL の事前ロード (起動高速化)
// -----------------------------------------------------------------------------
//  FFmpeg を /DELAYLOAD 化したため、プロセス起動時のインポート解決から
//  外れている。Phase 1 の初期化と並行して別スレッドで依存順
//  (avutil → swresample → swscale → avcodec → avformat) に LoadLibraryW
//  し、初回 FFmpeg 呼び出しの遅延を隠す。
// ---------------------------------------------------------------------------
void preloadFfmpegDlls() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash + 1);

    static const wchar_t* kOrder[] = {
        L"avutil-",   L"swresample-", L"swscale-",
        L"avcodec-",  L"avformat-",
    };

    const auto startedAt = Clock::now();
    int loaded = 0;
    for (const wchar_t* prefix : kOrder) {
        WIN32_FIND_DATAW fd = {};
        const std::wstring pattern = dir + prefix + L"*.dll";
        HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (LoadLibraryW((dir + fd.cFileName).c_str()) != nullptr) ++loaded;
        } while (FindNextFileW(find, &fd) != 0);
        FindClose(find);
    }
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - startedAt)
            .count();
    log::info("preload: FFmpeg DLL %d 件をロード (%lld ms)", loaded, ms);
}

// ---------------------------------------------------------------------------
//  メインループ (Phase 1 の操作応答を一元で処理する)
// ---------------------------------------------------------------------------
int runApplication(HINSTANCE instance, int showCmd) {
#if NANOALPHA_CONSOLE
    // コンソールを保持しているときだけ CP を変える (GUI サブシステムで親の
    // コンソールのコードページを書き換えないため)。stdout へはバイト列を直接
    // 書いているので、リダイレクト先へは CP に関係なく UTF-8 で届く。
    if (GetConsoleWindow() != nullptr) {
        SetConsoleOutputCP(CP_UTF8); // UTF-8 ログを正しく表示するため
    }
#endif
    // ここから先のヒープ破壊 / AV は、各スレッドのソース行付きスタックとして報告される
    crash::install();

    // FFmpeg DLL を Phase 1 と並行してプリロード (delay-load の先回り)
    std::thread ffmpegPreload(preloadFfmpegDlls);

    Phase1Core core;
    if (!bootPhase1(core, instance, showCmd)) {
        ffmpegPreload.join();
        return 1;
    }

    // ---- v3: 設定の読み込みと適用 (imgui.ini の代わりに settings.ini を使う) ----
    settings::load();
    settings::save(); // 初回は既定値を書き出す (ファイル有無を検証できるようにする)
    {
        const settings::Values& sv = settings::g_values;
        core.player.setFrameGenFactor(sv.frameGenFactor);
        core.player.setRepeatMode(static_cast<PlayerController::RepeatMode>(sv.repeatMode));

        int effectiveFilter = sv.scaleFilter;
        const char* upscalerName = "off";
        if (sv.upscaler == 2) {
            // 自動: NVIDIA DLSS を試す (実機未検証のため常に false → FSR1)
            effectiveFilter = nvidia::tryDlssInit()
                                  ? static_cast<int>(sv.scaleFilter)
                                  : static_cast<int>(PlayerController::ScaleFilter::Fsr);
            upscalerName = "auto";
        } else if (sv.upscaler == 1) {
            effectiveFilter = static_cast<int>(PlayerController::ScaleFilter::Fsr);
            upscalerName = "fsr1";
        }
        core.player.setScaleFilter(static_cast<PlayerController::ScaleFilter>(effectiveFilter));

        log::info("settings: loaded=%s filter=%d upscaler=%d framegen.factor=%d "
                  "ext.rgb=%d ext.vlc=%d ext.framegen=%d ext.disc=%d ext.network=%d",
                  settings::g_loaded ? "yes" : "no", sv.scaleFilter, sv.upscaler,
                  sv.frameGenFactor, sv.extRgb ? 1 : 0, sv.extVlc ? 1 : 0, sv.extFrameGen ? 1 : 0,
                  sv.extDisc ? 1 : 0, sv.extNetwork ? 1 : 0);
        // 実効的な拡大フィルタ (超解像度が有効なら Fsr=3 になる)
        log::info("upscaler: mode=%s -> scaleFilter=%d frameGenFactor=%d", upscalerName,
                  effectiveFilter, sv.frameGenFactor);
        // NVIDIA 検出結果をログに出す (理論対応であることを確認するため)
        const nvidia::Info& nv = nvidia::probeCached();
        log::info("NVIDIA: gpu=%d nvngx=%d dlssg=%d nvcuda=%d nvapi=%d -> dlss=%s",
                  nv.nvidiaGpu ? 1 : 0, nv.nvngx ? 1 : 0, nv.nvdlssg ? 1 : 0, nv.nvcuda ? 1 : 0,
                  nv.nvapi ? 1 : 0, nv.dlssReady() ? "candidate" : "unavailable");
    }

    ExtensionManager::Config config;
    config.totalBudget = std::chrono::milliseconds(3000); // Phase 2: 3 秒以内
    config.maxConcurrent = 3;
    config.failRemainingOnTimeout = true;

    ExtensionManager extensions(config);
    startPhase2(extensions, core);

    // 起動引数のファイルは Phase 2 のバックグラウンドロードと並行して開く。
    // 引数が無い場合はファイル選択ダイアログを起動時に表示する。
    const std::wstring input = commandLineInput();
    if (input.empty()) {
        log::info("input: 起動引数なし → ファイル選択ダイアログを表示");
        core.pendingOpenFile = true; // ループ開始後の ContextLock 外で開く
    } else {
        openInput(core, input);
    }

    std::uint32_t frameCount = 0;
    auto lastHeartbeat = Clock::now();
    // 再生終了 (EOF) のエッジ検出用。終端で止まっている間は一度だけ反応する。
    bool endHandled = false;

    // ソフトウェアフレーム制御の間隔。
    // syncInterval=1 の Present を使うと D3D11VA の送信がブロックしてしまう
    // ので、その代わりにここでのスリープで描画レートを抑える。
    //
    // v3: 目標レートは「リフレッシュレート固定」ではなく、下のループで毎フレーム
    //     コンテンツの実測 fps (VideoDecoder が測る pts 差分の EMA) に合わせて
    //     再計算する。60fps 超の素材でも出力 fps が上がるようにするため。
    std::chrono::microseconds frameInterval{0};
    int refreshHz = 60;
    Clock::time_point nextFrame = Clock::now();
    {
        HDC screen = GetDC(nullptr);
        if (screen != nullptr) {
            const int hz = GetDeviceCaps(screen, VREFRESH);
            ReleaseDC(nullptr, screen);
            if (hz > 0 && hz <= 1000) refreshHz = hz;
            frameInterval = std::chrono::microseconds(1000000 / refreshHz);
            log::info("main: software frame pacing  target=%d Hz", refreshHz);
        }
    }
    if (frameInterval.count() <= 0) {
        refreshHz = 60;
        frameInterval = std::chrono::microseconds(16667);
    }
    // Present を許す最高レート (Hz)。フレーム生成の倍率との積が暴走しないようにする。
    constexpr double kMaxPresentHz = 500.0;
    int lastTargetHz = refreshHz;
    // Windows の既定タイマ分解能は 15.6ms で、240fps (4.17ms) のスリープは
    // そのままでは絶対に合わない。timeBeginPeriod(1) で 1ms 粒度にしてから
    // 「sleep で粗く待ち + 残りを QPC スピン」で合わせる。
    timeBeginPeriod(1);

    // OpenRGB の LED 更新は ACK 待ち (数百 ms) を伴うためメインループから分離する。
    // 200ms 間隔で再生状態を反映した色を送信する。
    std::atomic<std::uint32_t> rgbColor{0x1E3A5Cu};
    std::atomic<bool> rgbRunning{true};
    std::jthread rgbWorker([&](std::stop_token token) {
        while (!token.stop_requested() && rgbRunning.load(std::memory_order_acquire)) {
            for (int i = 0; i < 10; ++i) {
                if (token.stop_requested() || !rgbRunning.load(std::memory_order_acquire)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (token.stop_requested() || !rgbRunning.load(std::memory_order_acquire)) break;

            rgb::Client* client = core.rgbPtr.load(std::memory_order_acquire);
            if (client == nullptr) continue;

            const std::uint32_t color = rgbColor.load(std::memory_order_acquire);
            client->setAll(static_cast<unsigned char>((color >> 16) & 0xFFu),
                           static_cast<unsigned char>((color >> 8) & 0xFFu),
                           static_cast<unsigned char>(color & 0xFFu));
        }
    });

    while (true) {
        // D3D11 即時コンテキストはデコードスレッド (FFmpeg) と共有される。
        // 即時コンテキストを使う区間だけを同じ再帰ロックで直列化する。
        bool alive = true;
        {
            const std::lock_guard<ContextLock> contextGuard(core.video.contextMutex());

            alive = core.window.pumpMessages();
            if (alive) {
                ++frameCount;
                const auto now = Clock::now();
                if (now - lastHeartbeat > std::chrono::seconds(1)) {
                    lastHeartbeat = now;
                    const MediaClock& clock = core.audio.clock();
                    log::info("main: heartbeat  frame=%u  video=%.1f s  audio=%.1f s  "
                              "snd=%s ring=%u under=%u",
                              frameCount, core.video.currentTime(),
                              clock.valid() ? clock.pts() : -1.0, core.sound.stage(),
                              core.audio.bufferedFrames(), core.audio.underruns());
                }

                extensions.tick(); // 完了デキュー + UI バインド (ノンブロッキング)

                // 一時停止 / デコード品質を毎フレーム音声・映像へ反映する
                const PlayerController::Snapshot control = core.player.snapshot();
                core.audio.setPaused(control.paused);
                core.video.setDecodeQuality(static_cast<int>(control.decodeQuality));

                // ---- libVLC (暗号化 DVD / 'vlc:' 指定) の状態を毎フレーム反映 ----
                //  位置・長さは VideoDecoder へ上書きして渡すことで、UI のシークバー /
                //  情報タブ / 終端検出をそのまま使えるようにしている。
                if (core.dvdPlayer.isOpen()) {
                    core.video.setExternalStatus(
                        true, static_cast<double>(core.dvdPlayer.timeMs()) / 1000.0,
                        core.dvdPlayer.mediaInfo());

                    if (core.dvdPaused != control.paused) {
                        core.dvdPaused = control.paused;
                        core.dvdPlayer.setPaused(control.paused);
                        log::info("Shortcut: %s (libVLC)",
                                  control.paused ? "pause" : "resume");
                    }
                    if (core.player.takeSeek(core.dvdSeekRevision)) {
                        const double target = core.player.seekTargetSeconds();
                        core.dvdPlayer.seekMs(static_cast<long long>(target * 1000.0));
                        log::info("VLC: seek -> %.1f s", target);
                    }
                    const int volumePercent =
                        static_cast<int>(core.audio.volume() * 100.0f + 0.5f);
                    if (volumePercent != core.dvdVolumePercent) {
                        core.dvdVolumePercent = volumePercent;
                        core.dvdPlayer.setVolumePercent(volumePercent);
                    }
                }

                // ---- リピート再生 / プレイリストの自動送り (エッジ検出) ----
                // 音声デコーダの EOF フラグと、残り時間がほぼ 0 になったことを両方見る
                // (映像のみの入力などで音声側が終了しないケースを拾うため)。
                {
                    const double pos = core.video.currentTime();
                    const double total = core.video.info().duration;
                    const bool atEnd = core.sound.ended() ||
                                       (total > 0.0 && pos >= total - 0.05);
                    if (!atEnd) {
                        endHandled = false;
                    } else if (!endHandled && !control.paused) {
                        endHandled = true;
                        core.sound.clearEnded();
                        core.pendingEndOfTrack = true;
                    }
                }

                core.overlay.beginFrame();
                core.overlay.draw(extensions, core.player, core.boot, core.video, core.audio);

                core.window.clear();

                // UI (上部タブバー / 右ドック) を避けた領域だけに映像を描画する
                const RECT videoArea = core.overlay.videoArea();

                // Zero-Copy 描画: デコード済みサーフェスをそのままシェーダへ渡す
                const MediaClock& mediaClock = core.audio.clock();
                const double mediaTime =
                    mediaClock.valid() ? mediaClock.pts() : core.video.currentTime();
                if (core.dvdPlayer.isOpen() && core.renderer.ready()) {
                    // 暗号化 DVD: libVLC が届けた BGRA フレームを D3D テクスチャへ載せて描画
                    const VideoDecoder::FrameView vlcFrame = core.dvdPlayer.present(
                        core.window.device(), core.window.context(), mediaTime);
                    if (vlcFrame.texture) {
                        core.renderer.draw(vlcFrame, control, core.window.width(),
                                           core.window.height(), videoArea, mediaTime);
                    }
                } else if (core.video.isReady() && core.renderer.ready()) {
                    const VideoDecoder::FrameView frame = core.video.acquireFrame();
                    if (frame.texture) {
                        // Frame Generation: 媒体クロックをそのまま renderer へ渡す。
                        // 補間重みは renderer が保持する prev/curr の実 pts から算出する。
                        core.renderer.draw(frame, control, core.window.width(),
                                           core.window.height(), videoArea, mediaTime);
                    }
                }

                core.overlay.render();

                // 同期待ちをしない Present (syncInterval=0)。
                // VSync 待ち込みの Present は D3D11VA の送信
                // (avcodec_send_packet 内の ID3D11VideoContext_*) を 1 描画フレームに
                // 1 度しか進めず、デコードが描画レート (60fps) に張り付いて
                // 2.0x を超えると映像が音声に追いつかなくなる (実測: 送信 1 回あたり
                // 16ms、レートは描画レート固定)。同期待ちは下のスリープが担う。
                // デコードと同じ即時コンテキストを使うため ContextLock の内側で呼ぶ。
                core.window.present(false);

                // v3: EQ / 従来パネルの別 OS ウィンドウを描画・更新する
                // (メインビューポートはスキップされる。present の後でも問題ない)
                core.overlay.renderPlatformWindows();
            }
        }

        if (!alive) break;

        // ファイル選択ダイアログ / スナップショット保存など、ContextLock の外で
        // しか実行できない要求を消化する (ダイアログ中もデコードは継続する)。
        processUiActions(core);
        if (core.pendingEndOfTrack) {
            core.pendingEndOfTrack = false;
            advanceAfterEnd(core);
        }
        updateRgbFromPlayback(core, rgbColor);

        // 描画レートの制御は VSync 待ちではなくこのスリープで行う。
        // ウィンドウモードでは DWM 合成が常時走るので syncInterval=0 でも
        // 画面破綻 (ティアリング) は発生しない。
        //
        // v3: 目標レートをリフレッシュ固定にしない。
        //     target = clamp(ソース瞬間fps × (フレーム生成 ? factor : 1),
        //                    リフレッシュ, kMaxPresentHz)
        //     ソース瞬間 fps は VideoDecoder のデコードスレッドが測った
        //     (コンテンツ順の全フレームの pts 差分の EMA) を使うので、
        //     コンテナ平均 fps ではなく「今再生している区間」の値になり、
        //     描画レートを下げていても測れない (自己制限しない)。
        {
            const PlayerController::Snapshot ctl = core.player.snapshot();
            // ソース瞬間 fps は FFmpeg のデコードスレッドが測る (VLC/DVD 経路では
            // 使わない。外部バックエンド中はリフレッシュ目標のままにする)。
            const bool contentRate = core.video.isReady() && !core.video.externalActive();
            double sourceHz = 0.0;
            double targetHz = static_cast<double>(refreshHz);
            if (!ctl.paused && contentRate && core.renderer.ready()) {
                const double span = core.video.contentFrameInterval();
                if (span > 1.0e-5 && span < 5.0) { // 0.2fps 未満 / 100kHz 超は異常値として無視
                    sourceHz = 1.0 / span;
                    double hz = sourceHz;
                    if (ctl.frameGen && ctl.frameGenFactor > 1)
                        hz *= static_cast<double>(ctl.frameGenFactor);
                    if (hz < targetHz) hz = targetHz;
                    if (hz > kMaxPresentHz) hz = kMaxPresentHz;
                    targetHz = hz;
                }
            }
            const int targetHzInt = static_cast<int>(targetHz + 0.5);
            // EMA の揺らぎで ±1Hz 行き来するだけの変化ではログを出さない
            // (ログ行の肥大化を防ぐ。レート自体は毎フレーム更新している)。
            if (targetHzInt - lastTargetHz > 1 || lastTargetHz - targetHzInt > 1) {
                lastTargetHz = targetHzInt;
                log::info("main: present target -> %d Hz  source=%.0f fps  frameGen=%s x%d",
                          targetHzInt, sourceHz, ctl.frameGen ? "on" : "off",
                          ctl.frameGen ? ctl.frameGenFactor : 1);
            }
            frameInterval =
                std::chrono::microseconds(static_cast<long long>(1000000.0 / targetHz + 0.5));
            if (frameInterval.count() <= 0)
                frameInterval = std::chrono::microseconds(16667);

            const auto tick = Clock::now();
            if (nextFrame < tick - frameInterval * 3) nextFrame = tick;
            nextFrame += frameInterval;
            if (nextFrame > tick) {
                // 残り 0.5ms までは Sleep (timeBeginPeriod(1) で 1ms 粒度)、
                // 最後は QPC のスピンで合わせる (分解能の揺らぎを吸収)。
                const auto spinAt = nextFrame - std::chrono::microseconds(500);
                if (spinAt > tick) std::this_thread::sleep_until(spinAt);
                while (Clock::now() < nextFrame) {
                    YieldProcessor(); // spin: Sleep だけではサブ ms に届かない
                }
            }
        }
    }

    timeEndPeriod(1);

    // ---- 終了処理: UI 破棄とロードを収束させる ----
    rgbRunning.store(false, std::memory_order_release);
    extensions.requestStop();
    const bool settled = extensions.waitForSettled(std::chrono::milliseconds(500));
    if (!settled) log::info("Phase 2: shutdown 時点で未完了ロードがありました (終了継続)");
    extensions.join();

    // まずデコードスレッドを止めて D3D11 コンテキストを単独所有してから破棄する
    core.video.close();
    core.sound.close();   // WASAPI リングへの投げ出しを止めてからエンジンを落とす
    core.dvdPlayer.close(); // libVLC (暗号化 DVD) の解放 (映像ストリームを止める)
    core.video.setExternalStatus(false, 0.0, VideoDecoder::Info{});
    core.renderer.shutdown();
    core.overlay.shutdown();
    core.audio.shutdown();
    core.window.destroy();

    if (g_comMainInitialized) CoUninitialize(); // メインスレッドの MTA を均衡する

    // プリロードスレッドの収束 (scope 末尾 join)
    if (ffmpegPreload.joinable()) ffmpegPreload.join();

    log::info("NanoAlphaPlayer: shutdown complete");
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
//  エントリポイント
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    return runApplication(instance, showCmd);
}

#if NANOALPHA_CONSOLE
int wmain(int, wchar_t**) {
    return runApplication(GetModuleHandleW(nullptr), SW_SHOWDEFAULT);
}
#endif
