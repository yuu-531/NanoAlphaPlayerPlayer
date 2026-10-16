#pragma once
#ifndef NANOALPHA_UI_IMGUI_OVERLAY_HPP
#define NANOALPHA_UI_IMGUI_OVERLAY_HPP

#ifndef NANOALPHA_HAS_IMGUI
#define NANOALPHA_HAS_IMGUI 0
#endif

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
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "Core/AudioEngine.hpp"
#include "Core/Licenses.hpp"
#include "Core/Log.hpp"
#include "Core/NvidiaSupport.hpp"
#include "Core/PlayerController.hpp"
#include "Core/Settings.hpp"
#include "Core/VideoDecoder.hpp"
#include "Extensions/ExtensionManager.hpp"
#include "Extensions/BuiltinLoaders.hpp"
#include "Extensions/MediaTools.hpp"

#if NANOALPHA_HAS_IMGUI
#include <imgui.h>
#include <imgui_internal.h>     // 検証ログ用に NavId / ActiveId を読む (internal)
#include <imgui_impl_dx11.h>   // ImGui 1.92 以降の命名 (旧: imgui_impl_d3d11.h)
#include <imgui_impl_win32.h>

// ImGui は <windows.h> 依存を避けるため本宣言を #if 0 で隠している (仕様どおり)。
// アプリ側でコピーして宣言し、WndProc から呼び出す。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam, LPARAM lParam);
#endif

namespace nanoalpha::ui {

// Phase 1 起動の実測値 (100ms 予算の達成状況を UI に表示する)
struct BootStats {
    std::chrono::milliseconds phase1{0};
    bool d3dReady = false;
    bool audioReady = false;
    bool videoReady = false;
    // ReadyHook がメインスレッドで UI へバインドした拡張数 (アトミックに観測可能)
    std::atomic<std::uint32_t> boundExtensions{0};
};

// =============================================================================
//  UI がメインループへ要求するアクション
// -----------------------------------------------------------------------------
//  ファイルダイアログや GPU 読み出しは描画 (ContextLock 内側) から呼べないため、
//  オーバーレイは「要求フラグ」を立て、main 側が ContextLock を抜けてから実行する。
// =============================================================================
enum class UiAction : int {
    None = 0,
    OpenFile,      // 単一ファイル選択ダイアログを開く
    OpenFiles,     // 複数ファイル選択 (プレイリストへ追加のみ — 既存の再生は維持)
    OpenFilesPlay, // 複数ファイル選択 (追加して先頭の 1 件から再生を始める)
    OpenUrl,       // payload に入力された URL を開く
    Snapshot,      // 現在のフレームを PNG 保存
    SetWallpaper,  // 直近のスナップショットを壁紙にする
    PlaylistNext,  // プレイリストを 1 件進める
    PlaylistPrev,  // プレイリストを 1 件戻る
    PlaylistJump,  // payload = 選択した番号 (10 進) のトラックを再生
    ToggleFullscreen, // 全画面表示の切替 (映像を最大化する)
};

// =============================================================================
//  Dear ImGui 統合オーバーレイ (Phase 1 / v2: 上部ツールバー + タブ構成)
// -----------------------------------------------------------------------------
//  ・Phase 1 の 100ms 予算内に初期化を完了させる
//  ・毎フレーム beginFrame() -> draw() -> render() の順で呼ぶ
//  ・NANOALPHA_HAS_IMGUI=0 の場合は全メソッドが無害な no-op になる
// =============================================================================
class ImGuiOverlay {
public:
    bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context,
              std::vector<char> fontData = {}) {
#if NANOALPHA_HAS_IMGUI
        if (m_initialized) return true;
        if (hwnd == nullptr || device == nullptr || context == nullptr) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        // v3: imgui.ini を絶対に書き出さない (レイアウトは自前で固定し、
        //     設定は %APPDATA%\NanoAlphaPlayer\settings.ini へ保存する)。
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // v3: EQ / 操作パネルを独立 OS ウィンドウとして出すためにマルチビューポート有効。
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.WindowBorderSize = 1.0f;

        // 日本語ゴシックを既定フォントに差し替える (無ければ既定フォントのまま)。
        // データは atlas が破棄されるまで本クラスが保持する (FontDataOwnedByAtlas=false)。
        m_fontData = std::move(fontData);
        if (!m_fontData.empty()) {
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 1;
            cfg.FontDataOwnedByAtlas = false;
            ImFont* font = io.Fonts->AddFontFromMemoryTTF(
                m_fontData.data(), static_cast<int>(m_fontData.size()), 16.0f, &cfg,
                io.Fonts->GetGlyphRangesJapanese());
            if (font == nullptr) {
                m_fontData.clear();
                log::info("Overlay: 日本語フォントの登録に失敗 → 既定フォントを使用");
            }
        }

        if (!ImGui_ImplWin32_Init(hwnd)) {
            ImGui::DestroyContext();
            return false;
        }
        if (!ImGui_ImplDX11_Init(device, context)) {
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
            return false;
        }
        m_initialized = true;
        std::memset(m_urlBuffer, 0, sizeof(m_urlBuffer));
        return true;
#else
        (void)hwnd;
        (void)device;
        (void)context;
        (void)fontData;
        return false;
#endif
    }

    void shutdown() {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized) return;
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        m_initialized = false;
#endif
    }

    void beginFrame() {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized) return;
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
#endif
    }

    // プレイリスト表示・操作 (null 可)。生存は main 側が保証する。
    // 選択 / 削除 / 並べ替えは UI から直接行う (ファイルを開く操作のみアクション経由)。
    void setPlaylist(tools::Playlist* playlist) noexcept { m_playlist = playlist; }

    // キーボード (N / P) からプレイリスト送りを要求する
    void requestPlaylist(UiAction action) noexcept {
        m_pendingPayload.clear();
        m_action = action;
    }

    // 操作パネル (上部タブバー + 右ドック) の非表示 / 表示切替 (Ctrl+H)。
    // 非表示中はバーもドックも描画せず、映像領域は全面になる。
    void togglePanel() noexcept {
        m_panelHidden = !m_panelHidden;
        log::info("panel -> %s", m_panelHidden ? "hidden" : "shown");
    }
    bool panelHidden() const noexcept { return m_panelHidden; }

    // 従来の操作パネルを別ウィンドウで開閉 (Ctrl+P / 上部バーのボタン)。
    void togglePanelWindow() noexcept {
        m_panelWindowOpen = !m_panelWindowOpen;
        log::info("panel window -> %s", m_panelWindowOpen ? "opened" : "closed");
    }
    bool panelWindowOpen() const noexcept { return m_panelWindowOpen; }

    // 全画面表示 (下部バーの「全画面」/ F11 / Esc で切替)。
    // ウィンドウ本体のスタイル・サイズ切替は main 側で行う。
    void setFullscreen(bool on) noexcept {
#if NANOALPHA_HAS_IMGUI
        if (m_fullscreen == on) return;
        m_fullscreen = on;
        if (on && m_initialized && ImGui::GetCurrentContext() != nullptr) {
            m_barRevealUntil = ImGui::GetTime() + kBarRevealSeconds;
            m_lastMousePos = ImGui::GetIO().MousePos;
        }
        log::info("UI fullscreen -> %s", on ? "on" : "off");
#endif
    }
    bool fullscreen() const noexcept { return m_fullscreen; }

    // キー入力があったことを UI へ知らせる (全画面中のコントロールバー再表示)
    void notifyInput() noexcept {
#if NANOALPHA_HAS_IMGUI
        if (!m_fullscreen) return;
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return;
        m_barRevealUntil = ImGui::GetTime() + kBarRevealSeconds;
#endif
    }

    // EQ 別ウィンドウ (上部タブの「EQ」で開く)
    bool eqWindowOpen() const noexcept { return m_eqWindowOpen; }

    // ライセンス / このアプリについて (F1 または情報タブのボタンで開閉)。
    // 同梱ライブラリのライセンス本文まで表示する (GPLv3 公開時の表示要件)。
    void toggleLicenses() noexcept {
        m_licensesOpen = !m_licensesOpen;
        if (m_licensesOpen) m_licensesFirstFrame = true;
        log::info("licenses window -> %s", m_licensesOpen ? "opened" : "closed");
    }
    bool licensesOpen() const noexcept { return m_licensesOpen; }

    // UI を避けた映像描画領域 (ピクセル)。未初期化 / 非表示時は幅高さ 0 = 全面。
    RECT videoArea() const noexcept { return m_videoArea; }

    // マルチビューポートの描画 (メイン窓の present の後、ContextLock 内で呼ぶ)
    void renderPlatformWindows() {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized) return;
        ImGui::UpdatePlatformWindows();

        // 二次ビューポートの OS ウィンドウへ内容に合わせたタイトルを付ける
        // (既定では "Untitled" のままなので、タスクバー / Alt-Tab で判別できない)
        applyWindowTitle(m_eqHwnd, L"NanoAlphaPlayer EQ", m_eqTitleDirty);
        applyWindowTitle(m_panelHwnd, L"NanoAlphaPlayer Popout", m_panelTitleDirty);

        ImGui::RenderPlatformWindowsDefault();
#endif
    }

    void draw(ExtensionManager& extensions, PlayerController& player, const BootStats& boot,
              const VideoDecoder& video, AudioEngine& audio) {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized) return;
        m_focusNow = nullptr;

        const PlayerController::Snapshot snap = player.snapshot();
        const VideoDecoder::Info info = video.info();
        const double duration = info.duration;
        double position = video.currentTime();

        // ---- v3: 映像描画領域 (UI を避けた部分)。0 = 全面 ----
        m_videoArea.left = 0;
        m_videoArea.top = 0;
        m_videoArea.right = 0;
        m_videoArea.bottom = 0;

        const ImGuiViewport* vp = ImGui::GetMainViewport();

        // ---- VLC レイアウト: 上部タブバー / 右ドック / 下部コントロールバー ----
        //   コンテキスト:
        //     通常          = タブバー + ドック + 下部バー
        //     Ctrl+H        = 下部バーのみ (タブバーとドックを隠す)
        //     ポップアウト   = 操作パネル全体が別ウィンドウへ移るのでメイン側は映像のみ
        //     全画面        = 何も出さない (映像のみ / マウス操作で下部バーを表示)
        // [v3 修正] ポップアウト中は元のパネルを描画しない (同じ操作系が二重に出るため)。
        const bool chrome = !m_panelHidden && !m_fullscreen && !m_panelWindowOpen;
        if (m_fullscreen) {
            // 全画面中はマウスが動いたり押されたりキーが押されると 4 秒間だけバーを出す。
            //  [v3 追加] クリックでも再表示する (停止・シーク・画面クリックで戻る)。
            //  表示 / 非表示の遷移を 1 回だけログして、テストから追えるようにする。
            const ImGuiIO& io = ImGui::GetIO();
            const bool wasVisible = ImGui::GetTime() < m_barRevealUntil;
            const bool moved = io.MousePos.x != m_lastMousePos.x ||
                               io.MousePos.y != m_lastMousePos.y;
            const bool clicked =
                io.MouseClicked[0] || io.MouseClicked[1] || io.MouseClicked[2];
            if (moved) m_lastMousePos = io.MousePos;
            if (moved || clicked) {
                m_barRevealUntil = ImGui::GetTime() + kBarRevealSeconds;
            }
            if (ImGui::GetTime() >= m_barRevealUntil) {
                if (m_barRevealUntil != 0.0) log::info("UI fullscreen bar -> hidden");
                m_barRevealUntil = 0.0;
            } else if (!wasVisible && clicked) {
                log::info("UI fullscreen bar -> revealed by click");
            }
        }
        // ポップアウト中は元の下部バーも出さない (コントロールは別ウィンドウへ移動済み)。
        // 停止・シーク・画面クリックなどの入力は notifyInput() / 上のマウス判定が拾う。
        const bool showBottomBar = !m_panelWindowOpen &&
                                   (!m_fullscreen || ImGui::GetTime() < m_barRevealUntil);

        // 映像領域 = UI を避けた部分 (レンダラの RT 座標)
        m_videoArea.left = 0;
        m_videoArea.top = chrome ? static_cast<LONG>(kBarHeight) : 0;
        m_videoArea.right = chrome ? static_cast<LONG>(vp->Size.x) - static_cast<LONG>(kDockWidth)
                                   : static_cast<LONG>(vp->Size.x);
        // 下部バーがレイアウトを占有する間だけ高さを削る (全画面はオーバーレイなので 0)
        m_videoArea.bottom =
            static_cast<LONG>(vp->Size.y) -
            ((!m_fullscreen && !m_panelWindowOpen) ? static_cast<LONG>(kBottomBarHeight) : 0);

        // 字幕の下端 (画面座標)。下部バーが画面下を占める間は、その上に上げる
        m_subtitleBottom = vp->WorkPos.y + vp->WorkSize.y -
                           (showBottomBar ? kBottomBarHeight : 0.0f);

        // ---- 操作パネル (非表示中は描画しない) ----
        if (chrome) {
        // 上部タブバー (VLC と同じ操作: タブで画面を切り替える)
        drawTopBar(extensions, player, video, audio, snap, position, duration);

        // 右ドック (タブ内容を表示する操作パネル本体)
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x + vp->Size.x - kDockWidth, vp->Pos.y + kBarHeight),
            ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(kDockWidth, vp->Size.y - kBarHeight - kBottomBarHeight),
                                 ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
        if (ImGui::Begin("##nanoalpha_dock", nullptr,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove)) {
            // タブ内容はスクロール領域に収める。
            // NavFlattened: 子ウィンドウは既定で独立したフォーカススコープを持ち、
            // Tab が中に入れないため、親とスコープを共有させてキーボード操作を可能にする。
            ImGui::BeginChild("##dock_content", ImVec2(0.0f, 0.0f),
                              ImGuiChildFlags_NavFlattened);

                // ---- タブ内容 (上部タブバーで選択) ----
                // タブ切替直後は前タブの NavId が残って Tab が固まるため、
                // この子ウィンドウの先頭可能項目へフォーカスを移して再起動する。
                if (m_focusResetPending) {
                    m_focusResetPending = false;
                    // 旧タブの NavId を捨て、カーソルを非表示にする。次に押される Tab は
                    // dir=0 として「先頭の可能項目へラップ」するため、新しいタブ内容から
                    // 再開できる。SetKeyboardFocusHere は入力可能な項目を起動して
                    // (ActiveId が立ち) 以後のショートカットが抑止されるので使わない。
                    if (ImGuiContext* g = ImGui::GetCurrentContext()) {
                        g->NavId = 0;
                        g->NavCursorVisible = false;
                    }
                }
                drawTabContent(m_tab, extensions, player, boot, video, audio, snap, info,
                               position, duration);
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleVar(); // WindowPadding (dock)
        } // chrome

        // ---- 下部コントロールバー (VLC 同様 / 全画面中はオーバーレイ) ----
        if (showBottomBar) {
            drawBottomBar(player, video, audio, snap, position, duration);
        }

        // ナビゲーションフォーカスの移動を検出 (自動検証用の診断ログ)
        if (m_focusShown != m_focusNow) {
            if (m_focusNow) log::info("UI focus: %s", m_focusNow);
            m_focusShown = m_focusNow;
        }

        // ---------------- EQ (別ウィンドウ / 上部タブ「EQ」から開く) ----------------
        if (m_eqWindowOpen) {
            drawEqWindow(audio);
        } else {
            m_eqHwnd = nullptr; // 閉じられたら次回は新しい HWND を取り直す
        }

        // ---------------- 従来の操作パネル (別ウィンドウ / Ctrl+P) ----------------
        if (m_panelWindowOpen) {
            drawPanelWindow(extensions, player, boot, video, audio, snap, info, position,
                            duration);
        } else {
            m_panelHwnd = nullptr;
        }

        // ---------------- ライセンス / このアプリについて (F1) ----------------
        if (m_licensesOpen) {
            drawLicensesWindow();
        }

        // ---------------- 字幕 (前景・下部中央) ----------------
        if (snap.subtitleVisible && video.isReady()) {
            const std::string subText = video.subtitleAt(position);
            if (!subText.empty()) drawSubtitle(subText);
        }
#else
        (void)extensions;
        (void)player;
        (void)boot;
        (void)video;
        (void)audio;
#endif
    }

    void render() {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized) return;

        // ---- 診断: 同一 ID のアイテムが同じウィンドウに複数あるか ----
        // ImGui は io.ConfigDebugHighlightIdConflicts (既定 true) で、ホバー中の
        // アイテムに同じ ID が複数あると "Programmer error: N visible items with
        // conflicting ID!" というツールチップを出す。ただしツールチップはホバー
        // して初めて目に入るため、自動検証できるよう 1 行ログにも残す。
        // (同じラベルのボタンを同じウィンドウへ並べたときに発生する。修正は
        //  PushID() で ID シードを分ける、または "##xx" を付ける。)
        if (ImGui::GetCurrentContext() != nullptr) {
            const ImGuiContext* ctx = ImGui::GetCurrentContext();
            const ImGuiID conflict = ctx->DebugDrawIdConflictsId;
            if (conflict != 0 && conflict != m_lastIdConflict) {
                m_lastIdConflict = conflict;
                log::info("UI: id conflict %08X  hoveredItems=%d",
                          static_cast<unsigned>(conflict),
                          ctx->HoveredIdPreviousFrameItemCount);
            } else if (conflict == 0) {
                m_lastIdConflict = 0;
            }
        }

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
#endif
    }

    // 字幕を画面下部中央へ描画する (前景 drawlist で映像の上に重ねる)。
    // 複数行・自動折り返し対応。背景は半透明の角丸ボックスで可読性を確保。
    // 中央揃えは映像領域 (UI を避けた部分) を基準にする。
    void drawSubtitle(const std::string& text) {
#if NANOALPHA_HAS_IMGUI
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        ImFont* font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * 1.7f;
        const ImGuiViewport* vp = ImGui::GetMainViewport();

        float areaLeft = 0.0f;
        float areaWidth = vp->WorkSize.x;
        float areaBottom = vp->WorkPos.y + vp->WorkSize.y;
        if (m_videoArea.right > m_videoArea.left && m_videoArea.bottom > m_videoArea.top) {
            areaLeft = static_cast<float>(m_videoArea.left);
            areaWidth = static_cast<float>(m_videoArea.right - m_videoArea.left);
        }
        // 下端は draw() で決めた「下部コントロールバーを避けた位置」を使う
        if (m_subtitleBottom > 0.0f) areaBottom = m_subtitleBottom;
        const float wrapWidth = areaWidth * 0.78f;
        const ImVec2 textSize =
            font->CalcTextSizeA(fontSize, wrapWidth, wrapWidth, text.c_str());
        const float x = areaLeft + (areaWidth - textSize.x) * 0.5f;
        const float y = areaBottom - textSize.y - 20.0f;
        draw->AddRectFilled(ImVec2(x - 10.0f, y - 5.0f),
                            ImVec2(x + textSize.x + 10.0f, y + textSize.y + 5.0f),
                            IM_COL32(0, 0, 0, 150), 6.0f);
        draw->AddText(font, fontSize, ImVec2(x, y), IM_COL32(255, 255, 255, 255),
                      text.c_str(), nullptr, wrapWidth);
#else
        (void)text;
#endif
    }

    // WndProc フック。true を返したメッセージはウィンドウ側で処理を打ち切る。
    bool handleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result) {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return false;
        result = ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        return result != 0;
#else
        (void)hwnd;
        (void)msg;
        (void)wParam;
        (void)lParam;
        (void)result;
        return false;
#endif
    }

    bool enabled() const noexcept { return m_initialized; }

    // アプリ側ショートカット判定の直前に呼ぶ。Tab で起動されたスライダ等の一時
    // 入力 (TempInput) を閉じる。閉じていないと以降のキーが全て入力ボックスに
    // 吸われて「タブが切り替えられない」ように見える (v3 の重大バグ)。
    // 実際の InputText (URL 欄など) は TempInputId が 0 のため影響しない。
    // key: 今届いたキー。Enter / Esc は「一時入力ボックス」の確定・取消そのものなので
    // 閉じてしまわない (閉じると値が ImGui に渡らず確定できなくなる)。
    void releaseActiveWidget(unsigned key) noexcept {
#if NANOALPHA_HAS_IMGUI
        if (key == 0x0D /* VK_RETURN */ || key == 0x1B /* VK_ESCAPE */) return;
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return;
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (g->TempInputId != 0 && g->TempInputId == g->ActiveId) {
            ImGui::ClearActiveID();
            g->TempInputId = 0;
        }
#else
        (void)key;
#endif
    }

    // ショートカットを抑止すべき状態か (キーごとに判定する)。
    // 注意: WantCaptureKeyboard は NavEnableKeyboard 指定時に恒常 true になるため
    //       使わない。また IsAnyItemActive() を丸ごと抑止条件にすると、
    //   Tab でスライダ等にフォーカスが乗った瞬間から 1-6 (タブ切替) や
    //       F/H/R/S などの全ショートカットが無効化されて UI が固まる
    //       (v3 の重大バグ) だった。抑止は「ImGui が実際にそのキーを
    //       消費する場合」に限定する。
    // - コンボ / コンテキストメニューの開閉中 → 全キー抑止 (リスト移動に使う)
    // - InputText の編集中 (InputTextState が ActiveId と一致) → 全キー抑止
    // - それ以外でアイテムがアクティブ (スライダの掴み中 / キー編集中)
    //   → 矢印・Enter・Space など ImGui が使うキーのみ抑止し、
    //     数字・アルファベット・Ctrl 系はアプリ側ショートカットへ通す。
    bool wantsKeyboard(unsigned key) const noexcept {
#if NANOALPHA_HAS_IMGUI
        if (m_panelHidden) return false; // 操作パネル非表示中はショートカット優先
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return false;
        // コンボなどのポップアップ中は矢印キーをリスト移動に使う (音量に割り込ませない)
        if (ImGui::IsPopupOpen(static_cast<ImGuiID>(0), ImGuiPopupFlags_AnyPopup)) return true;
        const ImGuiContext* ctx = ImGui::GetCurrentContext();
        // URL 欄など、実際にテキストを入力している間だけ全抑止。
        // ただしスライダ等が Tab でアクティブ化したときの一時入力 (TempInput) は
        // 除外する。ImGui は Tab で Inputable 項目をそのまま起動してしまうため、
        // これを「入力中」とみなすと 1-6 (タブ切替) などが押せなくなって UI が固まる。
        if (ctx->InputTextState.ID != 0 && ctx->InputTextState.ID == ctx->ActiveId &&
            !(ctx->TempInputId != 0 && ctx->TempInputId == ctx->ActiveId)) {
            return true;
        }
        if (!ImGui::IsAnyItemActive()) return false;
        switch (key) {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_RETURN:
        case VK_ESCAPE:
        case VK_SPACE:
        case VK_TAB:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_DELETE:
        case VK_BACK:
            return true; // スライダ調整 / ナビゲーションに割り込ませない
        default:
            return false;
        }
#else
        (void)key;
        return false;
#endif
    }

    // ショートカットが抑止された理由をログに残す (抑止時のみ呼ぶこと)
    void logKeyCapture() const noexcept {
#if NANOALPHA_HAS_IMGUI
        if (!m_initialized || ImGui::GetCurrentContext() == nullptr) return;
        const ImGuiContext* ctx = ImGui::GetCurrentContext();
        const bool activeItem = ImGui::IsAnyItemActive();
        const bool textInput = ImGui::GetIO().WantTextInput;
        const bool mouseDown = ImGui::IsAnyMouseDown();
        log::info("UI capture: activeItem=%d wantTextInput=%d mouseDown=%d "
                  "activeId=%08X activeWin=%s textEditId=%08X "
                  "navId=%08X navWin=%s",
                  activeItem ? 1 : 0, textInput ? 1 : 0, mouseDown ? 1 : 0,
                  static_cast<unsigned>(ctx->ActiveId),
                  ctx->ActiveIdWindow ? ctx->ActiveIdWindow->Name : "null",
                  static_cast<unsigned>(ctx->InputTextState.ID),
                  static_cast<unsigned>(ctx->NavId),
                  ctx->NavWindow ? ctx->NavWindow->Name : "null");
        const ImGuiWindow* nw = ctx->NavWindow;
        const unsigned lastInWin =
            (nw && ctx->NavLayer < IM_ARRAYSIZE(nw->NavLastIds))
                ? static_cast<unsigned>(nw->NavLastIds[ctx->NavLayer])
                : 0u;
        log::info("UI nav: scoring=%d tabDir=%d tabCount=%d tabFirst=%s tabFirstId=%08X "
                  "resultId=%08X navLayer=%d focusScope=%08X curFocusScope=%08X navIdAlive=%d",
                  ctx->NavMoveScoringItems ? 1 : 0, static_cast<int>(ctx->NavTabbingDir),
                  static_cast<int>(ctx->NavTabbingCounter),
                  ctx->NavTabbingResultFirst.Window ? ctx->NavTabbingResultFirst.Window->Name : "none",
                  static_cast<unsigned>(ctx->NavTabbingResultFirst.ID),
                  static_cast<unsigned>(ctx->NavMoveResultLocal.ID),
                  static_cast<int>(ctx->NavLayer),
                  static_cast<unsigned>(ctx->NavFocusScopeId),
                  static_cast<unsigned>(ctx->CurrentFocusScopeId),
                  (lastInWin == static_cast<unsigned>(ctx->NavId)) ? 1 : 0);
#endif
    }

    // ---- ナビゲーションフォーカスの診断 (自動検証用) ----
    // 各ウィジェットの直後で呼び、フォーカス中のみ記録する。
    void trackFocus(const char* tag) noexcept {
#if NANOALPHA_HAS_IMGUI
        if (ImGui::IsItemFocused()) m_focusNow = tag;
#else
        (void)tag;
#endif
    }

    // ---- ウィジェット矩形の診断 (クリック自動検証用) ----
    // クリック操作の自動テストは実座標が必要なため、ImGui の実配置 (スクリーン座標) を
    // ログで受け渡す。座標が変わった時だけ出力する (毎フレームのログ爆発を避ける)。
#if NANOALPHA_HAS_IMGUI
    void noteRect(const char* tag, const ImVec2& mn, const ImVec2& mx) noexcept {
        // タブは tab_appearing の初回フレームだけ空矩形を ItemAdd するため、実寸 2px 未満は無視
        if (!(mx.x - mn.x >= 2.0f) || !(mx.y - mn.y >= 2.0f)) return;
        UiRect* slot = nullptr;
        for (UiRect& e : m_rectLog) {
            if (e.tag == tag) { slot = &e; break; }          // 同一タグは同一リテラル
            if (slot == nullptr && e.tag == nullptr) slot = &e;
        }
        if (slot == nullptr) return;
        const int l = static_cast<int>(mn.x + 0.5f);
        const int t = static_cast<int>(mn.y + 0.5f);
        const int r = static_cast<int>(mx.x + 0.5f);
        const int b = static_cast<int>(mx.y + 0.5f);
        if (slot->set && slot->l == l && slot->t == t && slot->r == r && slot->b == b) return;
        slot->tag = tag;
        slot->l = l;
        slot->t = t;
        slot->r = r;
        slot->b = b;
        slot->set = true;
        log::info("UI rect %s=%d,%d,%d,%d", tag, l, t, r, b);
    }
#endif

    // ---- タブ切替 (キーボード 1-6 / 上部タブバーから呼ばれる) ----
    // v3: コンテンツは別ウィンドウではなく右ドックへ描画するため、
    //     選択状態をそのまま保持する (BeginTabBar は使わない)。
    void selectTab(int index) noexcept {
        if (index < 0 || index >= kTabCount) return;
        if (m_tab != index) {
            static const char* kTabs[kTabCount] = {"再生", "映像", "音声", "拡張", "情報", "EQ"};
            // idx は自動検証用 (ログは UTF-8 のためテストは ASCII の idx だけを読む)
            log::info("UI tab -> %s (idx=%d)", kTabs[index], index);
            // タブ内容が入れ替わると NavId は前タブの項目 ID のまま残り、
            // ImGui の Tab ラップ判定 (counter==1) が満たされないため Tab が固まる。
            // 次に描画される最初の項目へ明示的にフォーカスを移す。
            m_focusResetPending = true;
        }
        m_tab = index;
        // EQ は別ウィンドウで調整する
        if (index == 5) m_eqWindowOpen = true;
    }

    // ---- メインループへ渡すアクション要求 (1 件ずつ消費する) ----
    UiAction takeAction(std::wstring& payload) noexcept {
        payload.clear();
        const UiAction action = m_action;
        if (action == UiAction::None) return UiAction::None;
        m_action = UiAction::None;
        payload.swap(m_pendingPayload);
        m_pendingPayload.clear();
        return action;
    }

private:
    static std::wstring utf8ToWide(const char* text) {
        if (text == nullptr || *text == '\0') return {};
        const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (length <= 0) return {};
        std::wstring wide(static_cast<size_t>(length - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
        return wide;
    }

    static void seekBy(PlayerController& player, const VideoDecoder& video, double delta) {
        player.requestSeek(video.currentTime() + delta);
    }

    // =========================================================================
    //  v3: 上部タブバー / 右ドック / EQ・旧パネルの別ウィンドウ
    // =========================================================================

    // ---- レイアウト寸法 (ピクセル) ----
    static constexpr float kBarHeight = 44.0f;        // 上部タブバーの高さ
    static constexpr float kDockWidth = 460.0f;       // 右ドックの幅
    static constexpr float kBottomBarHeight = 80.0f;  // 下部コントロールバーの高さ
    static constexpr double kBarRevealSeconds = 4.0;  // 全画面中にバーを出す秒数
    // EQ 別ウィンドウ (操作しやすい大きめサイズ)
    static constexpr float kEqWidth = 476.0f;
    static constexpr float kEqHeight = 500.0f;
    static constexpr float kEqSliderW = 76.0f;
    static constexpr float kEqSliderH = 190.0f;
    static constexpr int kTabCount = 6;               // 再生/映像/音声/拡張/情報/EQ
    // ポップアウト (別ウィンドウ)。メインと同等の操作 (タブ 1-6 / EQ / transport) を
    // 収めるため、旧パネル (500x560) より少し大きめにしている。
    static constexpr float kPopoutWidth = 520.0f;
    static constexpr float kPopoutHeight = 640.0f;

    // 拡張状態の色 (拡張タブと共用)
    static const ImVec4& stateColorOf(ExtensionState state) {
        static const ImVec4 kGreen{0.35f, 0.85f, 0.45f, 1.0f};
        static const ImVec4 kYellow{1.00f, 0.83f, 0.25f, 1.0f};
        static const ImVec4 kRed{0.95f, 0.35f, 0.35f, 1.0f};
        static const ImVec4 kGray{0.62f, 0.62f, 0.68f, 1.0f};
        switch (state) {
        case ExtensionState::Ready:     return kGreen;
        case ExtensionState::Loading:   return kYellow;
        case ExtensionState::Failed:    return kRed;
        case ExtensionState::TimedOut:  return kRed;
        case ExtensionState::Pending:   break;
        case ExtensionState::Cancelled: break;
        }
        return kGray;
    }

    // 拡張 ID → ナビゲーション診断タグ (自動検証用、文字列リテラルのみ)
    static const char* extFocusTag(ExtensionId id) {
        switch (id) {
        case ExtensionId::FrameGeneration: return "ext.framegen";
        case ExtensionId::OpenRgb:         return "ext.rgb";
        case ExtensionId::VlcFeatures:     return "ext.vlc";
        case ExtensionId::Disc:            return "ext.disc";
        case ExtensionId::NetworkStream:   return "ext.network";
        default: break;
        }
        return "ext.none";
    }

    static float labelWidth(const char* label) {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }

    // ---- リピート再生 (VLC のループボタン相当) ----
    //   オフ = プレイリスト末尾で停止 / 全曲 = 全体をループ / 1曲 = 同じ曲を繰り返す
    static const char* repeatLabel(PlayerController::RepeatMode mode) {
        switch (mode) {
        case PlayerController::RepeatMode::All: return "リピート: 全曲";
        case PlayerController::RepeatMode::One: return "リピート: 1曲";
        default: break;
        }
        return "リピート: オフ";
    }

    static const char* repeatName(PlayerController::RepeatMode mode) {
        switch (mode) {
        case PlayerController::RepeatMode::All: return "all";
        case PlayerController::RepeatMode::One: return "one";
        default: break;
        }
        return "off";
    }

    // UI / キーボードから呼ばれる共通の変更経路 (settings.ini にも残す)
    void applyRepeatMode(PlayerController& player, PlayerController::RepeatMode mode) {
        player.setRepeatMode(mode);
        settings::g_values.repeatMode = static_cast<int>(mode);
        settings::save();
        log::info("repeat -> %s", repeatName(mode));
    }

    // 下部バー / ポップアウト transport 共用のサイクルボタン
    void drawRepeatCycleButton(PlayerController& player,
                               const PlayerController::Snapshot& snap) {
        if (ImGui::Button(repeatLabel(snap.repeatMode))) {
            applyRepeatMode(player, player.cycleRepeatMode());
        }
    }

    // 別ビューポートの OS ウィンドウへタイトルを書き込む (1 度だけ)
    static void applyWindowTitle(HWND hwnd, const wchar_t* title, bool& dirty) {
        if (!dirty || hwnd == nullptr) return;
        wchar_t current[128]{};
        GetWindowTextW(hwnd, current, 128);
        if (lstrcmpW(current, title) != 0) SetWindowTextW(hwnd, title);
        dirty = false;
    }

    // 現在 Begin しているウィンドウのビューポート HWND を控える
    static void captureViewportHwnd(HWND& out, bool& dirty) {
        ImGuiViewport* viewport = ImGui::GetWindowViewport();
        HWND hwnd = (viewport != nullptr) ? static_cast<HWND>(viewport->PlatformHandle) : nullptr;
        if (hwnd != nullptr && hwnd != out) {
            out = hwnd;
            dirty = true;
        }
    }

    // [Added] Minimize the pop-out viewport's OS window.
    //         m_panelHwnd is latched by captureViewportHwnd() at the end of the
    //         previous frame, so it is valid long before the user can click.
    void minimizePanelWindow() {
        if (m_panelHwnd == nullptr) return;
        if (IsIconic(m_panelHwnd)) return;
        // SW_MINIMIZE is equivalent to SC_MINIMIZE but posts the window to the
        // minimized state directly, so the click is handled within this frame
        // and no WM_SYSCOMMAND round-trip back into our own message pump.
        ShowWindow(m_panelHwnd, SW_MINIMIZE);
    }

    // 拡張 ID → settings の ON/OFF フィールド (nullptr = 切替不可)
    static bool* settingsField(ExtensionId id) {
        settings::Values& v = settings::g_values;
        switch (id) {
        case ExtensionId::FrameGeneration: return &v.extFrameGen;
        case ExtensionId::OpenRgb:         return &v.extRgb;
        case ExtensionId::VlcFeatures:     return &v.extVlc;
        case ExtensionId::Disc:            return &v.extDisc;
        case ExtensionId::NetworkStream:   return &v.extNetwork;
        default: break;
        }
        return nullptr;
    }

    // 別ウィンドウの配置位置 (メイン窓の外 → 右 → 左 → モニター右端の順で試す)
    static ImVec2 sidePos(float width, float height) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        const float monitorW = static_cast<float>(GetSystemMetrics(SM_CXSCREEN));
        const float monitorH = static_cast<float>(GetSystemMetrics(SM_CYSCREEN));
        const float margin = 8.0f;

        float x = vp->Pos.x + vp->Size.x + margin;              // メイン窓の右
        if (x + width > monitorW - margin) x = vp->Pos.x - width - margin; // メイン窓の左
        if (x < margin) x = monitorW - width - margin;           // モニター右端
        if (x < 0.0f) x = 0.0f;

        float y = vp->Pos.y + margin;
        if (y + height > monitorH - margin) y = monitorH - height - margin;
        if (y < 0.0f) y = 0.0f;
        return ImVec2(x, y);
    }

    // ---- 上部タブバー (VLC と同じ: 画面切り替わりはここで行う) ----
    void drawTopBar(ExtensionManager& extensions, PlayerController& player,
                    const VideoDecoder& video, AudioEngine& audio,
                    const PlayerController::Snapshot& snap, double position, double duration) {
        (void)extensions;
        (void)player;
        (void)snap;
        (void)position;
        (void)duration;

        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x, vp->Pos.y), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kBarHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 5.0f));

        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##nanoalpha_tabbar", nullptr, flags)) {
            // Tab 列の右端。
            //  ItemSize() (Button の内部) は CursorPos.x を「次の行の行頭」へ
            //  リセットしてしまうため、描画直後の GetCursorPosX() は必ず
            //  行頭 (WindowPadding.x = 8) を返す。すると
            //    urlX        = 8 + spacing*1.5 = 20  → URL 欄がタブ列の下に
            //    クランプ判定 = rightX < 8 + 8       → 常に不発
            //  となり、URL 欄と右端ボタンがタブ列と重なる。
            //  SameLine() で「前ボタンの右端 + ItemSpacing」へ移した後に取ること。
            float tabsEnd = 8.0f;
            auto tabButton = [this, &tabsEnd](const char* label, int index, const char* tag) {
                const bool active = (m_tab == index);
                if (active) {
                    ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TabActive));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                          ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
                }
                if (ImGui::Button(label)) selectTab(index);
                noteRect(tag, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                if (active) ImGui::PopStyleColor(2);
                ImGui::SameLine();
                tabsEnd = ImGui::GetCursorPosX();
            };

            static const char* kTabs[kTabCount] = {"再生", "映像", "音声", "拡張", "情報", "EQ"};
            static const char* kTabTags[kTabCount] = {"top-tab-0", "top-tab-1", "top-tab-2",
                                                      "top-tab-3", "top-tab-4", "top-tab-5"};
            for (int i = 0; i < kTabCount; ++i) tabButton(kTabs[i], i, kTabTags[i]);

            // ---- 右側の操作ボタン ----
            // 「操作パネル」は別 OS ウィンドウへのポップアウト操作なので表記を統一
            const char* kRight[4] = {"開く…", "複数開く…", "ポップアウト", "パネル非表示"};
            float rightWidth = 0.0f;
            for (const char* label : kRight) rightWidth += labelWidth(label);
            rightWidth += ImGui::GetStyle().ItemSpacing.x * 3.0f;

            float rightX = ImGui::GetWindowWidth() - rightWidth - 8.0f;
            if (rightX < tabsEnd + 8.0f) rightX = tabsEnd + 8.0f;

            // ---- 中央: URL 欄 (ポップアウト側と同じ操作をメインでも行えるように) ----
            const float spacing = ImGui::GetStyle().ItemSpacing.x;
            const float urlOpenW = labelWidth("URL 開く") + spacing;
            const float urlX = tabsEnd + spacing * 1.5f;
            const float urlW = rightX - spacing * 1.5f - urlX - urlOpenW;
            if (urlW >= 180.0f) {
                ImGui::SameLine(urlX);
                ImGui::SetNextItemWidth(urlW);
                const bool urlEntered = ImGui::InputText(
                    "##top_url", m_urlBuffer, sizeof(m_urlBuffer),
                    ImGuiInputTextFlags_EnterReturnsTrue);
                noteRect("top-url", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                ImGui::SameLine();
                if (ImGui::Button("URL 開く") || (urlEntered && m_urlBuffer[0] != '\0')) {
                    m_pendingPayload = utf8ToWide(m_urlBuffer);
                    m_action = UiAction::OpenUrl;
                }
            }

            ImGui::SameLine(rightX);

            if (ImGui::Button(kRight[0])) m_action = UiAction::OpenFile;
            trackFocus("open-button");
            noteRect("top-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button(kRight[1])) m_action = UiAction::OpenFilesPlay;
            trackFocus("open-files-button");
            noteRect("top-openfiles", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button(kRight[2])) togglePanelWindow();
            noteRect("top-popout", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button(kRight[3])) togglePanel();
            noteRect("top-panelhide", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        }
        ImGui::End();
        ImGui::PopStyleVar();

        (void)video;
        (void)audio;
    }

    // ---- 下部コントロールバー (VLC と同じ配置 / 全画面中はオーバーレイ) ----
    //  1 行目: 経過時間 + シークバー + 残り時間
    //  2 行目: 再生・巻き戻し・早送り …… 速度 / 大きめ音量 / EQ / 全画面
    void drawBottomBar(PlayerController& player, const VideoDecoder& video, AudioEngine& audio,
                       const PlayerController::Snapshot& snap, double position, double duration) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - kBottomBarHeight), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, kBottomBarHeight), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 7.0f));

        // NoFocusOnAppearing: ImGui は「前フレームから 2 フレーム以上非表示だったウィンドウが
        // 再び出現した瞬間」に FocusWindow() を呼ぶ (imgui.cpp window_just_activated_by_user)。
        // 下部バーは 3 番目 (最後) に Begin されるため、これが付いていないと起動直後に
        // Nav フォーカスを右ドックから奪ってしまい、Tab キーが下部バー内だけで
        // 回り続けて拡張/映像タブのウィジェットに到達できなくなる (v3 の回帰)。
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                       ImGuiWindowFlags_NoScrollWithMouse |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##nanoalpha_transport", nullptr, flags)) {
            const float spacing = ImGui::GetStyle().ItemSpacing.x;

            // ---- 1 行目: 経過時間 + シークバー + 残り時間 ----
            // ドラッグ中はシーク先を左側ラベルに出す (スライダ本体は値を表示しない)
            const double shownPos = m_seeking ? m_seekPos : position;
            char elapsed[32];
            char total[32];
            std::snprintf(elapsed, sizeof(elapsed), "%.1f s", shownPos);
            std::snprintf(total, sizeof(total), "%.1f s", duration);
            ImGui::TextUnformatted(elapsed);
            ImGui::SameLine();
            float seekW =
                ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(total).x - spacing;
            if (seekW < 80.0f) seekW = 80.0f;
            ImGui::SetNextItemWidth(seekW);
            if (duration > 0.0) {
                // 移動式シークバー。ドラッグ中は前の値を保持し、指を離した瞬間だけ seek。
                static const double kZero = 0.0;
                double seekTarget = shownPos;
                const bool changed = ImGui::SliderScalar("##bottom_seek", ImGuiDataType_Double,
                                                          &seekTarget, &kZero, &duration, " ",
                                                          ImGuiSliderFlags_AlwaysClamp);
                const bool active = ImGui::IsItemActive();
                if (changed) {
                    m_seeking = true;
                    m_seekPos = seekTarget;
                } else if (active && !m_seeking) {
                    m_seeking = true;
                    m_seekPos = seekTarget;
                } else if (m_seeking && !active) {
                    player.requestSeek(m_seekPos);
                    m_seeking = false;
                }
            } else {
                ImGui::Dummy(ImVec2(seekW, ImGui::GetFrameHeight()));
            }
            trackFocus("bottom-seek");
            ImGui::SameLine();
            ImGui::TextUnformatted(total);

            // ---- 2 行目 (左) : transport ----
            if (ImGui::Button(snap.paused ? "再生" : "一時停止")) {
                player.setPaused(!snap.paused);
                log::info("UI: bottom pause -> %s", !snap.paused ? "paused" : "playing");
            }
            trackFocus("bottom-play");
            noteRect("bottom-play", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button("-5s")) seekBy(player, video, -5.0);
            ImGui::SameLine();
            if (ImGui::Button("+5s")) seekBy(player, video, 5.0);
            trackFocus("bottom-forward");
            const float leftEnd = ImGui::GetCursorPosX();

            // ---- 2 行目 (右) : 速度 / 大きめ音量 / リピート / EQ / 全画面 ----
            const float speedW = 130.0f;
            const float volumeW = 200.0f;
            const char* kEq = "EQ";
            const char* kFs = m_fullscreen ? "全画面解除" : "全画面";
            const char* kPanel = "パネル表示";
            const char* kRepeat = repeatLabel(snap.repeatMode);
            float rightW = speedW + volumeW + labelWidth(kRepeat) + labelWidth(kEq) +
                           labelWidth(kFs) + spacing * 5.0f;
            if (m_panelHidden) rightW += labelWidth(kPanel) + spacing;
            float rightX = ImGui::GetWindowWidth() - rightW - 10.0f;
            if (rightX < leftEnd + spacing) rightX = leftEnd + spacing;
            ImGui::SameLine(rightX);

            float speed = snap.speed;
            ImGui::SetNextItemWidth(speedW);
            if (ImGui::SliderFloat("##bottom_speed", &speed, 0.25f, 4.0f, "%.2fx")) {
                player.setSpeed(speed);
            }
            trackFocus("bottom-speed");
            ImGui::SameLine();

            // 音量 (操作性のため VLC 同様に大きめにする)
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 8.0f));
            float volume = audio.volume() * 100.0f;
            ImGui::SetNextItemWidth(volumeW);
            if (ImGui::SliderFloat("##bottom_volume", &volume, 0.0f, 400.0f, "%.0f %%")) {
                audio.setVolume(volume * 0.01f);
            }
            ImGui::PopStyleVar();
            trackFocus("bottom-volume");
            ImGui::SameLine();

            // リピート再生 (オフ -> 全曲 -> 1曲 と切り替わる)
            drawRepeatCycleButton(player, snap);
            trackFocus("bottom-repeat");
            noteRect("bottom-repeat", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();

            if (ImGui::Button(kEq)) m_eqWindowOpen = true;
            trackFocus("bottom-eq");
            noteRect("bottom-eq", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            if (ImGui::Button(kFs)) m_action = UiAction::ToggleFullscreen;
            trackFocus("bottom-fullscreen");
            noteRect("bottom-full", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            // Ctrl+H でバー自体は残るが、ドックを戻す入口を必ず用意する
            if (m_panelHidden) {
                ImGui::SameLine();
                if (ImGui::Button(kPanel)) togglePanel();
                trackFocus("bottom-panel");
                noteRect("bottom-panel", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(); // WindowPadding (bottom bar)
    }

    // ---- シークバー + transport (旧パネルで共用) ----
    void drawTransport(PlayerController& player, const VideoDecoder& video, AudioEngine& audio,
                       const PlayerController::Snapshot& snap, double position, double duration) {
        ImGui::Separator();
        if (ImGui::Button(snap.paused ? "再生" : "一時停止")) {
            player.setPaused(!snap.paused);
            log::info("UI: panel pause -> %s", !snap.paused ? "paused" : "playing");
        }
        noteRect("panel-play", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (ImGui::Button("-5s")) seekBy(player, video, -5.0);
        ImGui::SameLine();
        if (ImGui::Button("+5s")) seekBy(player, video, 5.0);
        ImGui::SameLine();
        ImGui::Text("%.1f / %.1f s", position, duration);

        // 移動式シークバー。ドラッグ中は前の値を保持し、指を離した瞬間だけ seek する。
        if (duration > 0.0) {
            static const double kZero = 0.0;
            double seekTarget = m_seeking ? m_seekPos : position;
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed =
                ImGui::SliderScalar("##seekbar", ImGuiDataType_Double, &seekTarget, &kZero,
                                    &duration, "%.1f s", ImGuiSliderFlags_AlwaysClamp);
            const bool active = ImGui::IsItemActive();
            if (changed) {
                m_seeking = true;
                m_seekPos = seekTarget;
            } else if (active && !m_seeking) {
                m_seeking = true;
                m_seekPos = seekTarget;
            } else if (m_seeking && !active) {
                player.requestSeek(m_seekPos);
                m_seeking = false;
            }
        }

        // 速度 / 音量 (左右2分割)
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        float speed = snap.speed;
        ImGui::SetNextItemWidth(half);
        if (ImGui::SliderFloat("##speed", &speed, 0.25f, 4.0f, "%.2fx")) {
            player.setSpeed(speed);
        }
        ImGui::SameLine();
        float volume = audio.volume() * 100.0f;
        ImGui::SetNextItemWidth(half);
        if (ImGui::SliderFloat("##volume", &volume, 0.0f, 400.0f, "%.0f %%")) {
            audio.setVolume(volume * 0.01f);
        }

        // ---- 3 行目: 下部コントロールバーと同等の操作 ----
        // ポップアウト側にも同じ操作を揃え、どちらでも同じことができるようにする。
        drawRepeatCycleButton(player, snap);
        noteRect("panel-repeat", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (ImGui::Button(m_fullscreen ? "全画面解除" : "全画面")) {
            m_action = UiAction::ToggleFullscreen;
        }
        noteRect("panel-full", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (ImGui::Button(m_panelHidden ? "パネル表示" : "パネル非表示")) togglePanel();
        noteRect("panel-hide", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine();
        if (ImGui::Button("EQ ウィンドウ")) m_eqWindowOpen = true;
        noteRect("panel-eq", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    // ---- 選択中のタブ内容 (ドックと旧パネルで共用) ----
    void drawTabContent(int tab, ExtensionManager& extensions, PlayerController& player,
                        const BootStats& boot, const VideoDecoder& video, AudioEngine& audio,
                        const PlayerController::Snapshot& snap, const VideoDecoder::Info& info,
                        double position, double duration) {
        switch (tab) {
        case 0:
            drawPlaybackTab(player, snap, info, position, duration);
            break;
        case 1:
            drawVideoTab(player, snap);
            break;
        case 2:
            drawAudioPanel(audio);
            break;
        case 3:
            drawExtensionTab(extensions, audio);
            break;
        case 4:
            drawInfoTab(boot, video, info, position, duration, audio);
            break;
        case 5:
            // インライン EQ (ドック / ポップアウト両方で同じ調整ができるように)
            drawEqControls(audio);
            ImGui::Separator();
            ImGui::TextDisabled("大きめの EQ は別ウィンドウ「NanoAlphaPlayer EQ」で操作できます");
            if (ImGui::SmallButton("EQ ウィンドウを開く")) m_eqWindowOpen = true;
            ImGui::SameLine();
            ImGui::TextDisabled("上部タブの「EQ」でも開きます");
            break;
        default:
            break;
        }
    }

    // ---- EQ 別ウィンドウ (大きめ: 5 段 x 2 列 / スライダは縦長) ----
    void drawEqWindow(AudioEngine& audio) {
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet =
            ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoFocusOnAppearing;
        ImGui::SetNextWindowClass(&windowClass);
        ImGui::SetNextWindowPos(sidePos(kEqWidth, kEqHeight), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(kEqWidth, kEqHeight), ImGuiCond_Appearing);
        if (ImGui::Begin("NanoAlphaPlayer EQ", &m_eqWindowOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
            drawEqControls(audio);
        }
        captureViewportHwnd(m_eqHwnd, m_eqTitleDirty);
        ImGui::End();
    }

    // ---- ポップアウト (旧: 従来の操作パネル / 別ウィンドウ) ----
    // v3: メイン画面 (上部タブバー + 右ドック + 下部バー) と「同じレベルの操作」が
    //     できるように、タブ (1-6)・URL・transport・全画面・リピートを全て揃えた。
    void drawPanelWindow(ExtensionManager& extensions, PlayerController& player,
                         const BootStats& boot, const VideoDecoder& video, AudioEngine& audio,
                         const PlayerController::Snapshot& snap, const VideoDecoder::Info& info,
                         double position, double duration) {
        ImGuiWindowClass windowClass;
        windowClass.ViewportFlagsOverrideSet =
            ImGuiViewportFlags_NoAutoMerge | ImGuiViewportFlags_NoFocusOnAppearing;
        ImGui::SetNextWindowClass(&windowClass);
        ImGui::SetNextWindowPos(sidePos(kPopoutWidth, kPopoutHeight), ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(kPopoutWidth, kPopoutHeight), ImGuiCond_Appearing);
        if (ImGui::Begin("NanoAlphaPlayer Popout", &m_panelWindowOpen,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing)) {
            // ---- 入力行 (v2 互換) ----
            if (ImGui::Button("開く…")) m_action = UiAction::OpenFile;
            ImGui::SameLine();
            if (ImGui::Button("複数開く…")) m_action = UiAction::OpenFilesPlay;
            ImGui::SameLine();
            if (ImGui::SmallButton("ポップアウトを閉じる")) m_panelWindowOpen = false;
            noteRect("panel-close", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SameLine();
            // [追加] ポップアウトは ImGui の別ビューポート = 別トップレベル窓だが、
            //        ConfigViewportsNoDecoration のため縁 (タイトルバー) が無く、
            //        OS の最小化ボタンが存在しない。最小化できるようにする。
            //        ラベルは行幅 (kPopoutWidth) に収まるよう「最小化」に畳み、
            //        補足はツールチップで示す。m_panelHwnd は前フレームの
            //        captureViewportHwnd で確定済み。
            if (ImGui::SmallButton("最小化")) minimizePanelWindow();
            noteRect("panel-min", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::IsItemHovered()) ImGui::SetItemTooltip("ポップアウトを最小化");
            ImGui::SameLine();
            // [修正] ここと drawTransport() の「全画面」は同じ "NanoAlphaPlayer Popout"
            //        ウィンドウの中にあり、同じラベル = 同じ ID になっていた。
            //        ImGui の io.ConfigDebugHighlightIdConflicts が
            //        "Programmer error: N visible items with conflicting ID!"
            //        と警告するうえ、ホバー/押下状態も 2 つで共有されてしまう。
            //        ラベルを変えない (レイアウトと noteRect の座標を守る) ので
            //        ID シードだけを PushID で分離する。
            ImGui::PushID("panel_header_fs");
            if (ImGui::SmallButton(m_fullscreen ? "全画面解除" : "全画面")) {
                m_action = UiAction::ToggleFullscreen;
            }
            ImGui::PopID();
            noteRect("panel-full2", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

            ImGui::SetNextItemWidth(-1.0f);
            const bool urlEntered = ImGui::InputText(
                "##panel_url", m_urlBuffer, sizeof(m_urlBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("URL 開く") || (urlEntered && m_urlBuffer[0] != '\0')) {
                m_pendingPayload = utf8ToWide(m_urlBuffer);
                m_action = UiAction::OpenUrl;
            }

            // ---- transport / シークバー ----
            drawTransport(player, video, audio, snap, position, duration);

            // ---- タブ (メインと同じ 1-6 / EQ まで揃える) ----
            ImGui::Separator();
            if (ImGui::BeginTabBar("nanoalpha_panel_tabs", ImGuiTabBarFlags_Reorderable)) {
                static const char* kTabs[6] = {"再生", "映像", "音声", "拡張", "情報", "EQ"};
                static const char* kTags[6] = {"panel-tab-0", "panel-tab-1", "panel-tab-2",
                                               "panel-tab-3", "panel-tab-4", "panel-tab-5"};
                // 強制選択 (SetSelected) フラグを毎フレーム渡すと、クリックで
                // 選ばれた新しいタブが「次のフレームで古いタブへ戻されて」
                // タブ切り替えが 1 フレームだけ動いて元に戻る (v3 の重大バグ)。
                // 選択状態はタブバー自身が保持し、表示中のタブの戻り値だけを使う。
                int visible = -1;
                for (int i = 0; i < 6; ++i) {
                    const bool selected = ImGui::BeginTabItem(kTabs[i]);
                    noteRect(kTags[i], ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
                    if (selected) {
                        visible = i;
                        ImGui::BeginChild("##panel_content", ImVec2(0.0f, 0.0f),
                                          ImGuiChildFlags_NavFlattened);
                        drawTabContent(i, extensions, player, boot, video, audio, snap, info,
                                       position, duration);
                        ImGui::EndChild();
                        ImGui::EndTabItem();
                    }
                }
                ImGui::EndTabBar();
                if (visible >= 0 && visible != m_panelTab) {
                    m_panelTab = visible;
                    // idx は自動検証用 (ログは UTF-8 のためテストは ASCII の idx だけを読む)
                    log::info("UI panel tab -> %s (idx=%d)", kTabs[visible], visible);
                    // メインの上部タブ「EQ」と同じ挙動 (別ウィンドウを開く)
                    if (visible == 5) m_eqWindowOpen = true;
                }
            }
        }
        captureViewportHwnd(m_panelHwnd, m_panelTitleDirty);
        ImGui::End();
    }

    // ---- 10-Band EQ (音声タブ / EQ ウィンドウで共用) ----
    void drawEqControls(AudioEngine& audio) {
        bool eqEnabled = audio.eqEnabled();
        if (ImGui::Checkbox("10-Band EQ", &eqEnabled)) audio.setEqEnabled(eqEnabled);
        ImGui::SameLine();
        if (ImGui::SmallButton("EQ reset")) {
            for (int band = 0; band < AudioEngine::kEqBands; ++band) {
                audio.setEqGainDb(band, 0.0f);
            }
        }

        ImGui::BeginDisabled(!eqEnabled);
        // 10 段を 5 段 x 2 列に並べ、スライダを縦長にして操作しやすくする
        if (ImGui::BeginTable("nanoalpha_eq", 5,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            constexpr int kCols = 5;
            for (int band = 0; band < AudioEngine::kEqBands; ++band) {
                if (band % kCols == 0) ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(band);
                float gain = audio.eqGainDb(band);
                if (ImGui::VSliderFloat("##gain", ImVec2(kEqSliderW, kEqSliderH), &gain, -24.0f,
                                        24.0f, "%+.1f")) {
                    audio.setEqGainDb(band, gain);
                }
                ImGui::TextDisabled("%.5g Hz", AudioEngine::kEqBandFrequencies[band]);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
    }

    // ---------------------------------------------------------- 再生タブ
    void drawPlaybackTab(PlayerController& player, const PlayerController::Snapshot& snap,
                         const VideoDecoder::Info& info, double position, double duration) {
        ImGui::Text("現在位置  %.3f s / %.3f s", position, duration);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", snap.paused ? "(一時停止中)" : "");

        ImGui::Text("再生速度");
        ImGui::SameLine();
        float speed = snap.speed;
        // ボタン分を差し引いた残り幅を使う (固定幅だと右端で切れるため)
        float speedW = ImGui::GetContentRegionAvail().x - labelWidth("1.0x に戻す") -
                       ImGui::GetStyle().ItemSpacing.x * 2.0f;
        if (speedW < 80.0f) speedW = 80.0f;
        ImGui::SetNextItemWidth(speedW);
        if (ImGui::SliderFloat("##speed2", &speed, 0.25f, 4.0f, "%.2fx")) {
            player.setSpeed(speed);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("1.0x に戻す")) player.setSpeed(1.0f);

        ImGui::Separator();
        bool subtitleVisible = snap.subtitleVisible;
        if (ImGui::Checkbox("字幕を表示", &subtitleVisible)) {
            player.setSubtitleVisible(subtitleVisible);
        }
        ImGui::SameLine();
        bool abRepeat = snap.abRepeat;
        if (ImGui::Checkbox("A-B リピート", &abRepeat)) player.setAbRepeat(abRepeat);
        ImGui::SameLine();
        ImGui::TextDisabled("A=%.2fs  B=%.2fs", snap.pointA, snap.pointB);
        // A/B 設定ボタンは別行に置く (同行だと右端で切れて押せなくなる)
        if (ImGui::SmallButton("A 設定")) player.setPointA(position);
        ImGui::SameLine();
        if (ImGui::SmallButton("B 設定")) player.setPointB(position);

        ImGui::Separator();
        ImGui::Text("トラック切替 (-1 = 自動)");
        const float trackW = 130.0f;
        int videoTrack = snap.videoTrack;
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::DragInt("映像", &videoTrack, 0.05f, -1, 63)) player.setVideoTrack(videoTrack);
        ImGui::SameLine();
        ImGui::TextDisabled("%u x %u  %.2f fps", info.width, info.height, info.fps);
        int audioTrack = snap.audioTrack;
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::DragInt("音声", &audioTrack, 0.05f, -1, 63)) player.setAudioTrack(audioTrack);
        int subtitleTrack = snap.subtitleTrack;
        ImGui::SetNextItemWidth(trackW);
        if (ImGui::DragInt("字幕", &subtitleTrack, 0.05f, -1, 63)) player.setSubtitleTrack(subtitleTrack);

        ImGui::Separator();
        drawPlaylistSection(player);
    }

    // ---------------------------------------------------------- 映像タブ (画質)
    void drawVideoTab(PlayerController& player, const PlayerController::Snapshot& snap) {
        // ---- 幾何 ----
        ImGui::Text("回転");
        ImGui::SameLine();
        static constexpr int kRotations[] = {0, 90, 180, 270};
        for (const int deg : kRotations) {
            ImGui::PushID(deg);
            if (deg != 0) ImGui::SameLine();
            char label[16];
            std::snprintf(label, sizeof(label), "%d", deg);
            if (ImGui::RadioButton(label, snap.rotationDeg == deg)) player.setRotation(deg);
            ImGui::PopID();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("90° 追加")) player.rotate(90);

        const char* kFlipItems[] = {"なし", "左右反転", "上下反転", "両方向"};
        int flipIndex = static_cast<int>(snap.flip);
        if (ImGui::Combo("反転", &flipIndex, kFlipItems, 4)) {
            player.setFlip(static_cast<PlayerController::Flip>(flipIndex));
        }
        ImGui::SameLine();
        const char* kAspectItems[] = {"自動", "16:9", "4:3", "21:9", "1:1"};
        int aspectIndex = static_cast<int>(snap.aspect);
        if (ImGui::Combo("アスペクト", &aspectIndex, kAspectItems, 5)) {
            player.setAspect(static_cast<PlayerController::Aspect>(aspectIndex));
        }

        float crop[4] = {snap.crop[0], snap.crop[1], snap.crop[2], snap.crop[3]};
        // 負の幅は「右側をその分だけ空ける」→ ラベルが窓外に切られない
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::DragFloat4("クロップ L,T,R,B", crop, 0.005f, 0.0f, 0.9f, "%.3f")) {
            player.setCrop(crop[0], crop[1], crop[2], crop[3]);
        }
        trackFocus("crop-drag");

        // ---- 画質 (v2) ----
        ImGui::Separator();
        ImGui::TextUnformatted("画質調整");

        float brightness = snap.brightness;
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::SliderFloat("明るさ", &brightness, -0.5f, 0.5f, "%.2f")) {
            player.setBrightness(brightness);
            log::info("Quality: brightness -> %.2f", static_cast<double>(brightness));
        }
        trackFocus("brightness");
        float contrast = snap.contrast;
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::SliderFloat("コントラスト", &contrast, 0.5f, 1.5f, "%.2f")) {
            player.setContrast(contrast);
            log::info("Quality: contrast -> %.2f", static_cast<double>(contrast));
        }
        trackFocus("contrast");
        float saturation = snap.saturation;
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::SliderFloat("彩度", &saturation, 0.0f, 2.0f, "%.2f")) {
            player.setSaturation(saturation);
            log::info("Quality: saturation -> %.2f", static_cast<double>(saturation));
        }
        trackFocus("saturation");
        float hue = snap.hueDeg;
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::SliderFloat("色相", &hue, -180.0f, 180.0f, "%.1f deg")) {
            player.setHue(hue);
            log::info("Quality: hue -> %.1f deg", static_cast<double>(hue));
        }
        trackFocus("hue");
        float sharpness = snap.sharpness;
        ImGui::SetNextItemWidth(-300.0f);
        if (ImGui::SliderFloat("シャープネス", &sharpness, 0.0f, 1.0f, "%.2f")) {
            player.setSharpness(sharpness);
            log::info("Quality: sharpness -> %.2f", static_cast<double>(sharpness));
        }
        trackFocus("sharpness");
        if (ImGui::SmallButton("画質をリセット")) {
            player.setBrightness(0.0f);
            player.setContrast(1.0f);
            player.setSaturation(1.0f);
            player.setHue(0.0f);
            player.setSharpness(0.0f);
            log::info("Quality: reset");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("レンダリング");

        // ---- 超解像度 (v3: AMD FSR1 / NVIDIA 理論対応) ----
        {
            const char* kUpscaleItems[] = {"無効 (標準フィルタ)",
                                           "FSR1 (AMD 超解像度)",
                                           "自動 (NVIDIA 検出 → FSR1 へフォールバック)"};
            int upscaler = settings::g_values.upscaler;
            if (ImGui::Combo("超解像度", &upscaler, kUpscaleItems, 3)) {
                settings::g_values.upscaler = upscaler;
                applyUpscaler(player, true);
                settings::save();
            }
            trackFocus("upscaler");
            if (upscaler == 2) {
                const nvidia::Info& nv = nvidia::probeCached();
                ImGui::TextDisabled("NVIDIA GPU=%s  nvngx=%s  dlssg=%s  nvcuda=%s  nvapi=%s",
                                    nv.nvidiaGpu ? "yes" : "no", nv.nvngx ? "yes" : "no",
                                    nv.nvdlssg ? "yes" : "no", nv.nvcuda ? "yes" : "no",
                                    nv.nvapi ? "yes" : "no");
                ImGui::TextDisabled("DLSS 初期化は未検証のため常に失敗し、FSR1 で代替します");
            } else if (upscaler == 1) {
                ImGui::TextDisabled("FidelityFX FSR 1.0 (EASU + RCAS) を適用中");
            }
        }

        const char* kFilterItems[] = {"リニア (標準)", "ニアレスト (ピクセル)", "アニソトロピック"};
        int filterIndex = settings::g_values.scaleFilter;
        ImGui::BeginDisabled(settings::g_values.upscaler != 0);
        if (ImGui::Combo("拡大縮小フィルタ", &filterIndex, kFilterItems, 3)) {
            settings::g_values.scaleFilter = filterIndex;
            applyUpscaler(player, false);
            settings::save();
        }
        trackFocus("scale-filter");
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("ピクセルアートはニアレストが有利");

        const char* kRangeItems[] = {"自動 (デコード判定)", "リミテッド (16-235)", "フル (0-255)"};
        int rangeIndex = static_cast<int>(snap.rangeMode);
        if (ImGui::Combo("カラーレンジ", &rangeIndex, kRangeItems, 3)) {
            player.setRangeMode(static_cast<PlayerController::RangeMode>(rangeIndex));
        }

        const char* kQualityItems[] = {"高速 (デブロック省略)", "標準", "高画質 (デブロック実行)"};
        int qualityIndex = static_cast<int>(snap.decodeQuality);
        if (ImGui::Combo("デコード品質", &qualityIndex, kQualityItems, 3)) {
            player.setDecodeQuality(static_cast<PlayerController::DecodeQuality>(qualityIndex));
        }
        ImGui::SameLine();
        ImGui::TextDisabled("skip_loop_filter / skip_frame を切り替えます");

        bool frameGen = snap.frameGen;
        if (ImGui::Checkbox("フレーム補間 (Frame Generation)", &frameGen)) {
            player.setFrameGen(frameGen);
        }
        trackFocus("framegen");
        ImGui::SameLine();
        ImGui::TextDisabled("前フレームと現フレームをブレンドして表示レートを引き上げます");

        // ---- 補間倍率 (v3: 2 / 4 / 8 / 16) ----
        ImGui::TextDisabled("補間倍率");
        static const int kFactors[] = {2, 4, 8, 16};
        static const char* kFactorTags[] = {"fg-2", "fg-4", "fg-8", "fg-16"};
        for (int fi = 0; fi < 4; ++fi) {
            const int factor = kFactors[fi];
            ImGui::PushID(fi);
            ImGui::SameLine();
            char label[16];
            std::snprintf(label, sizeof(label), "%dx", factor);
            const bool picked = ImGui::RadioButton(label, snap.frameGenFactor == factor);
            trackFocus(kFactorTags[fi]);
            if (picked) {
                player.setFrameGenFactor(factor);
                settings::g_values.frameGenFactor = factor;
                settings::save();
                log::info("FrameGen factor -> %dx", factor);
            }
            ImGui::PopID();
        }
        ImGui::TextDisabled("倍率が高いほど滑らかになります (GPU 負荷は増加)");
    }

    // ---- 超解像度設定を player へ反映する ----
    void applyUpscaler(PlayerController& player, bool logChange) {
        const int mode = settings::g_values.upscaler;
        if (mode == 0) {
            player.setScaleFilter(static_cast<PlayerController::ScaleFilter>(
                settings::g_values.scaleFilter));
        } else if (mode == 2) {
            // 自動: NVIDIA DLSS を試す (実機未検証のため常に false → FSR1 へフォールバック)
            if (!nvidia::tryDlssInit()) player.setScaleFilter(PlayerController::ScaleFilter::Fsr);
        } else {
            player.setScaleFilter(PlayerController::ScaleFilter::Fsr);
        }
        if (logChange) {
            log::info("Upscaler -> %s", mode == 0 ? "off" : (mode == 1 ? "fsr1" : "auto"));
        }
    }

    // ---------------------------------------------------------- 拡張タブ
    void drawExtensionTab(ExtensionManager& extensions, AudioEngine& audio) {
        const auto stateColor = [](ExtensionState state) -> const ImVec4& {
            return stateColorOf(state);
        };
        (void)audio;

        // ---- スナップショット / 壁紙 (VLC 機能群) ----
        ImGui::TextUnformatted("スナップショット & 壁紙 (VLC Features)");
        if (ImGui::Button("スナップショット保存…")) m_action = UiAction::Snapshot;
        ImGui::SameLine();
        if (ImGui::Button("最後の画像を壁紙にする")) m_action = UiAction::SetWallpaper;
        ImGui::SameLine();
        ImGui::TextDisabled("保存先はダイアログで選択します");

        ImGui::Separator();
        ImGui::Text("Phase 2 extensions (background, budget 3000 ms)");
        ImGui::TextDisabled("UI へバインド済みの拡張: %u",
                            m_boundExtensions ? m_boundExtensions->load(std::memory_order_relaxed)
                                              : 0u);
        ImGui::TextDisabled("OFF にすると次回起動時も読み込まれません");

        if (ImGui::BeginTable("nanoalpha_extensions", 5,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("有効", ImGuiTableColumnFlags_WidthFixed, 46.0f);
            ImGui::TableSetupColumn("拡張", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("状態", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("所要", ImGuiTableColumnFlags_WidthFixed, 64.0f);
            ImGui::TableSetupColumn("備考", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableHeadersRow();

            for (int i = 0; i < static_cast<int>(ExtensionId::Count); ++i) {
                const auto id = static_cast<ExtensionId>(i);
                const std::string_view label = extensions.name(id);
                const ExtensionState state = extensions.state(id);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool* field = settingsField(id);
                if (field == nullptr) {
                    ImGui::TextDisabled("—");
                } else {
                    ImGui::PushID(i);
                    bool on = extensions.enabled(id);
                    if (ImGui::Checkbox("##on", &on)) {
                        // 起動時に除外した拡張は、有効化した瞬間にローダを (後から) 登録する
                        if (on && !extensions.registered(id)) {
                            nanoalpha::extensions::registerBuiltinLoader(extensions, id);
                        }
                        *field = on;
                        extensions.setEnabled(id, on);
                        settings::save();
                        log::info("Extension %.*s -> %s", static_cast<int>(label.size()),
                                  label.data(), on ? "enabled" : "disabled");
                    }
                    ImGui::PopID();
                    trackFocus(extFocusTag(id));
                }

                ImGui::TableNextColumn();
                ImGui::Text("%.*s", static_cast<int>(label.size()), label.data());

                ImGui::TableNextColumn();
                if (!extensions.enabled(id)) {
                    ImGui::TextDisabled("無効");
                } else if (!extensions.registered(id)) {
                    ImGui::TextDisabled("停止中");
                } else {
                    const std::string_view stateLabel = toString(state);
                    ImGui::TextColored(stateColor(state), "%.*s",
                                       static_cast<int>(stateLabel.size()), stateLabel.data());
                }

                ImGui::TableNextColumn();
                if (extensions.registered(id)) {
                    ImGui::Text("%lld ms",
                                static_cast<long long>(extensions.elapsed(id).count()));
                }

                ImGui::TableNextColumn();
                const std::string note = extensions.message(id);
                if (!note.empty()) ImGui::TextWrapped("%s", note.c_str());
            }
            ImGui::EndTable();
        }
    }

    // ---------------------------------------------------------- 情報タブ
    void drawInfoTab(const BootStats& boot, const VideoDecoder& video,
                     const VideoDecoder::Info& info, double position, double duration,
                     AudioEngine& audio) {
        const long long phase1Ms = boot.phase1.count();
        const bool withinBudget = phase1Ms <= 100;
        ImGui::Text("Phase 1 boot: %lld ms / 100 ms budget", phase1Ms);
        ImGui::SameLine();
        ImGui::TextColored(withinBudget ? kGreen0() : kRed0(), "%s",
                           withinBudget ? "OK" : "OVER BUDGET");
        ImGui::TextDisabled("D3D11: %s   WASAPI: %s   FFmpeg D3D11VA: %s",
                            boot.d3dReady ? "ready" : "failed",
                            boot.audioReady ? "ready" : "stub",
                            boot.videoReady ? "ready" : "stub");

        ImGui::Separator();
        if (video.isReady() && info.width > 0) {
            ImGui::Text("Video: %ux%u  %.3f fps  %dbit  %s  %.1f / %.1f sec",
                        info.width, info.height, info.fps, info.pixelFormatBits,
                        info.hardwareDecoding ? "D3D11VA Zero-Copy" : "software", position,
                        duration);
        } else {
            ImGui::TextDisabled("Video: 入力なし (「開く…」または起動引数で指定)");
        }

        ImGui::TextDisabled("clock: %s%.3f s", audio.clock().valid() ? "" : "~",
                            audio.clock().valid() ? audio.clock().pts() : 0.0);
        ImGui::TextDisabled("underruns=%u  buffered=%u frames", audio.underruns(),
                            audio.bufferedFrames());

        ImGui::Separator();
        ImGui::TextUnformatted("ショートカット");
        ImGui::TextDisabled("Space 一時停止   Ctrl+O 開く   Left/Right ±5s   Up/Down 音量");
        ImGui::TextDisabled("[ ] 速度   R 回転   S 字幕   A/B A-B ポイント   F フレーム補間");
        ImGui::TextDisabled("L リピート切替 (オフ/全曲/1曲)   N/P プレイリスト 次へ/前へ");
        ImGui::TextDisabled("O 出力モード   1-6 タブ切替   6 = EQ 別ウィンドウ   H この一覧");
        ImGui::TextDisabled("Ctrl+H パネル表示切替   Ctrl+P ポップアウト   F11 / Esc 全画面");

        ImGui::Separator();
        ImGui::TextUnformatted("ライセンス");
        ImGui::SameLine();
        if (ImGui::SmallButton("表示 (F1)")) {
            m_licensesOpen = true;
            m_licensesFirstFrame = true;
        }
        ImGui::TextDisabled("同梱ライブラリ (FFmpeg / ImGui / FSR など) のライセンス全文を表示します");
    }

    // ---------------------------------------------------------- ライセンス窓
    //  F1 または情報タブの「表示 (F1)」で開く。同梱ライブラリの識別情報に加え、
    //  選択したコンポーネントのライセンス本文まで表示する。
    void drawLicensesWindow() {
        using namespace nanoalpha::licenses;

        ImGui::SetNextWindowSize(ImVec2(720.0f, 540.0f), ImGuiCond_FirstUseEver);
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));

        if (!ImGui::Begin("Licenses##nanoalpha", &m_licensesOpen,
                          ImGuiWindowFlags_NoCollapse)) {
            ImGui::End();
            return;
        }

        // ---- アプリ自身 ----
        ImGui::Text("%s %s", kAppName, kAppVersion);
        // 窓が実際に描画されたことを検証できるよう矩形を記録する
        noteRect("lic-open", ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::TextDisabled("Copyright (C) 2026 %s <%s>", kAuthor, kEmail);
        ImGui::Text("License: %s", kProjectLicense);
        ImGui::TextDisabled("%s", kProjectUrl);
        ImGui::TextDisabled(
            "Source: https://github.com/yuu-531 (complete corresponding source)");
        ImGui::Separator();

        // ---- 同梱ライブラリ一覧 ----
        ImGui::TextUnformatted("Bundled components");
        ImGui::BeginChild("lic_list", ImVec2(0.0f, 200.0f), true);
        for (int i = 0; i < kComponentCount; ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(kComponents[i].name, m_licensesSelected == i)) {
                m_licensesSelected = i;
                m_licensesFirstFrame = true;
            }
            ImGui::PopID();
        }
        ImGui::EndChild();

        // ---- 選択中コンポーネントの詳細 ----
        if (m_licensesSelected < 0 || m_licensesSelected >= kComponentCount) {
            m_licensesSelected = 0;
        }
        const Component& c = kComponents[m_licensesSelected];
        ImGui::Separator();
        ImGui::TextWrapped("%s  %s", c.name, c.version);
        ImGui::Text("License: %s", c.license);
        ImGui::TextWrapped("%s", c.copyright);
        ImGui::TextWrapped("%s", c.url);
        if (c.note != nullptr && c.note[0] != '\0') ImGui::TextWrapped("%s", c.note);

        ImGui::BeginChild("lic_text", ImVec2(0.0f, 0.0f), true);
        if (m_licensesFirstFrame) {
            ImGui::SetScrollY(0.0f); // 選択が変わったら本文の先頭へ戻す
            m_licensesFirstFrame = false;
        }
        if (c.text != nullptr) {
            ImGui::TextWrapped("%s", c.text);
        } else {
            ImGui::TextDisabled("No license text is bundled for this component.");
            ImGui::TextWrapped(
                "The license text is provided by the component itself or by the "
                "operating system / the user's installation.");
        }
        ImGui::EndChild();

        ImGui::End();
    }

    // ---------------------------------------------------------- プレイリスト / リピート
    void drawPlaylistSection(PlayerController& player) {
        ImGui::Separator();
        ImGui::TextUnformatted("プレイリスト");
        ImGui::SameLine();
        if (ImGui::SmallButton("ファイルを追加…")) m_action = UiAction::OpenFiles;
        ImGui::SameLine();
        if (ImGui::SmallButton("前へ")) m_action = UiAction::PlaylistPrev;
        ImGui::SameLine();
        if (ImGui::SmallButton("次へ")) m_action = UiAction::PlaylistNext;

        // ---- リピート再生 (オフ / 全曲ループ / 1曲ループ) ----
        const PlayerController::Snapshot self = player.snapshot();
        ImGui::TextUnformatted("リピート再生");
        ImGui::SameLine();
        static const PlayerController::RepeatMode kModes[3] = {
            PlayerController::RepeatMode::Off, PlayerController::RepeatMode::All,
            PlayerController::RepeatMode::One};
        static const char* kNames[3] = {"オフ", "全曲ループ", "1曲ループ"};
        for (int i = 0; i < 3; ++i) {
            if (i != 0) ImGui::SameLine();
            ImGui::PushID(i);
            const bool selected = (self.repeatMode == kModes[i]);
            if (ImGui::RadioButton(kNames[i], selected)) applyRepeatMode(player, kModes[i]);
            ImGui::PopID();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("終了時は %s", repeatName(self.repeatMode));

        if (m_playlist == nullptr) {
            ImGui::TextDisabled("プレイリストは VLC Features のロード完了後に利用できます");
            return;
        }

        const std::vector<std::wstring> items = m_playlist->items();
        if (items.empty()) {
            ImGui::TextDisabled("(空 — 「ファイルを追加…」で投入してください)");
            return;
        }

        size_t current = 0;
        size_t count = 0;
        m_playlist->status(count, current);
        ImGui::TextDisabled("%zu 件中 %zu 番目を再生中", count,
                            (current < count) ? current + 1 : count);

        ImGui::BeginChild("##playlist", ImVec2(0.0f, 150.0f), true);
        for (size_t i = 0; i < items.size(); ++i) {
            std::string label = std::to_string(i + 1);
            label += (i == current) ? ". ▶ " : ".  ";
            label += wideToUtf8(items[i]);
            bool jump = false;
            ImGui::PushID(static_cast<int>(i));
            if (i == current) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen0());
                jump = ImGui::Selectable(label.c_str(), true);
                ImGui::PopStyleColor();
            } else {
                jump = ImGui::Selectable(label.c_str(), false);
            }
            ImGui::PopID();
            if (jump) { // クリック / Enter でその曲を開く
                m_pendingPayload = std::to_wstring(i);
                m_action = UiAction::PlaylistJump;
            }
        }
        ImGui::EndChild();

        // ---- 選択中の 1 件を管理 (並べ替え / 削除 / クリア) ----
        if (ImGui::SmallButton("この曲を開く")) {
            m_pendingPayload = std::to_wstring(current);
            m_action = UiAction::PlaylistJump;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("上へ")) m_playlist->reorder(-1);
        ImGui::SameLine();
        if (ImGui::SmallButton("下へ")) m_playlist->reorder(+1);
        ImGui::SameLine();
        if (ImGui::SmallButton("選択を削除")) {
            if (m_playlist->removeAt(current)) log::info("playlist: %zu 件目を削除", current + 1);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("クリア")) {
            m_playlist->clear();
            log::info("playlist: cleared");
        }
    }

    // 右ドックに描く「音声」タブの中身 (v2 の Audio output 独立ウィンドウを統合)
    void drawAudioPanel(AudioEngine& audio) {
        static const ImVec4 kRed{0.95f, 0.35f, 0.35f, 1.0f};

        if (!audio.isReady()) {
            ImGui::TextColored(kRed, "%s",
                               audio.reopening() ? "デバイス再オープン中..."
                                                 : "オーディオエンジン未初期化");
        } else {
            ImGui::Text("%u Hz  %uch  %s", audio.sampleRate(), audio.channels(),
                        audio.exclusive() ? "exclusive" : "shared");
            ImGui::TextDisabled("buffered=%u frames  underruns=%u  played=%llu",
                                audio.bufferedFrames(), audio.underruns(),
                                static_cast<unsigned long long>(audio.framesPlayed()));
        }

        // 音量 (UI 上はパーセント表示、エンジンへは倍率で渡す)
        //   0..400% はスライダー範囲。Ctrl+クリックの数値入力は上限なし
        //   (setVolume は UI の制約を持たない)。100% 超はソフトクリップ付きブースト。
        float volumePercent = audio.volume() * 100.0f;
        if (ImGui::SliderFloat("Volume", &volumePercent, 0.0f, 400.0f, "%.0f %%")) {
            audio.setVolume(volumePercent * 0.01f);
        }
        if (volumePercent > 100.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "ブースト中 (ソフトクリップ)");
        }
        ImGui::TextDisabled("100%% 超はブースト。Ctrl+クリックで数値入力 (上限なし)");

        // v3: EQ は上部タブ「EQ」から開く別ウィンドウ「NanoAlphaPlayer EQ」で調整する
        ImGui::Separator();
        ImGui::TextUnformatted("10-Band EQ");
        ImGui::TextDisabled("イコライザは別ウィンドウ「NanoAlphaPlayer EQ」で調整します");
        if (ImGui::SmallButton("EQ ウィンドウを開く")) m_eqWindowOpen = true;
        ImGui::SameLine();
        ImGui::TextDisabled("有効: %s", audio.eqEnabled() ? "on" : "off");

        // 出力モードは IAudioClient の初期化時にしか効かないため、変更時は即再オープンする
        const char* kOutputItems[] = {"Shared", "Exclusive"};
        int outputIndex = static_cast<int>(audio.outputMode());
        if (ImGui::Combo("Output mode", &outputIndex, kOutputItems, 2)) {
            audio.setOutputMode(static_cast<AudioEngine::OutputMode>(outputIndex));
            audio.reopenOutput();
        }

        const char* kStereoItems[] = {"Stereo", "Left only", "Right only", "Reverse"};
        int stereoIndex = static_cast<int>(audio.stereoMode());
        if (ImGui::Combo("Stereo mode", &stereoIndex, kStereoItems, 4)) {
            audio.setStereoMode(static_cast<AudioEngine::StereoMode>(stereoIndex));
        }

        const bool clockValid = audio.clock().valid();
        ImGui::TextDisabled("%s   clock: %s%.3f s", audio.exclusive() ? "実効: exclusive"
                                                                     : "実効: shared",
                            clockValid ? "" : "~", clockValid ? audio.clock().pts() : 0.0);
    }

    // ---------------------------------------------------------- ユーティリティ
    static const ImVec4& kGreen0() {
        static const ImVec4 k{0.35f, 0.85f, 0.45f, 1.0f};
        return k;
    }
    static const ImVec4& kRed0() {
        static const ImVec4 k{0.95f, 0.35f, 0.35f, 1.0f};
        return k;
    }

    static std::string wideToUtf8(const std::wstring& text) {
        if (text.empty()) return {};
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                               static_cast<int>(text.size()), nullptr, 0,
                                               nullptr, nullptr);
        if (length <= 0) return {};
        std::string out(static_cast<size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                            length, nullptr, nullptr);
        return out;
    }

    bool m_initialized = false;
    // 操作パネルの非表示 (Ctrl+H で切替)。非表示中は上部タブバーも右ドックも
    // 描画せず、映像領域は全面になる。
    bool m_panelHidden = false;
    // 現在選択中の上部タブ (0=再生 1=映像 2=音声 3=拡張 4=情報 5=EQ)
    int m_tab = 0;
    // タブ切替直後: 古い NavId を打ち消して最初の項目へフォーカスを移すための要求
    bool m_focusResetPending = false;
    // 別ウィンドウの表示状態 (OS の ✕ ボタンでも閉じられる)
    bool m_eqWindowOpen = false;
    bool m_panelWindowOpen = false;
    // ライセンス窓 (F1 / 情報タブ) と、その中で選択中のコンポーネント
    bool m_licensesOpen = false;
    bool m_licensesFirstFrame = false;
    int m_licensesSelected = 0;
    // 従来パネル (別ウィンドウ) 内のタブ
    int m_panelTab = 0;
    // UI を避けた映像描画領域 (RT 座標)。幅高さ 0 = 全面
    RECT m_videoArea{0, 0, 0, 0};
    // 字幕を置く下端 (画面座標)。下部コントロールバーと重ならないようにする
    float m_subtitleBottom = 0.0f;
    // 別ビューポート (EQ / 従来パネル) の OS ウィンドウとタイトル更新フラグ
    HWND m_eqHwnd = nullptr;
    bool m_eqTitleDirty = false;
    HWND m_panelHwnd = nullptr;
    bool m_panelTitleDirty = false;
    // AddFontFromMemoryTTF(FontDataOwnedByAtlas=false) は所有権を持たないため、
    // アトラスの生存期間と同じくここに保持する。
    std::vector<char> m_fontData;

    // ---- UI -> main のアクション要求 ----
    UiAction m_action = UiAction::None;
    std::wstring m_pendingPayload;
    char m_urlBuffer[512] = {};
    tools::Playlist* m_playlist = nullptr;
    std::atomic<std::uint32_t>* m_boundExtensions = nullptr;

    // シークバーのドラッグ状態 (離したときに 1 回だけシークする)
    bool m_seeking = false;
    double m_seekPos = 0.0;

    // 全画面表示 (UI 全面を消して映像のみにする)
    bool m_fullscreen = false;
    // 全画面中にコントロールバーを出す期限 (0 = 出さない)
    double m_barRevealUntil = 0.0;
    // マウス移動の検出用 (前回位置)
    ImVec2 m_lastMousePos{-1.0e6f, -1.0e6f};
    // 次のフレームで選択するタブ (-1 なら通常どおり)。旧 API 互換のため残す
    int m_pendingTab = -1;

    // ナビゲーションフォーカス中のウィジェット名 (検証用の診断ログ)
    const char* m_focusNow = nullptr;
    const char* m_focusShown = nullptr;

#if NANOALPHA_HAS_IMGUI
    // クリック自動検証用: ウィジェット矩形 (スクリーン座標) を tag で覚えておく
    struct UiRect {
        const char* tag = nullptr;
        int l = 0, t = 0, r = 0, b = 0;
        bool set = false;
    };
    UiRect m_rectLog[32]{};

    // 同一 ID のアイテムが複数あるときだけ立つ ImGui の検出 ID。
    // 連続フレームで同じものを 1 回だけログにするための直前値。
    ImGuiID m_lastIdConflict = 0;
#endif

public:
    // BootStats::boundExtensions を拡張タブへ渡す (main が初期化時に呼ぶ)
    void bindBootStats(BootStats& boot) noexcept { m_boundExtensions = &boot.boundExtensions; }
};

} // namespace nanoalpha::ui

#endif // NANOALPHA_UI_IMGUI_OVERLAY_HPP
