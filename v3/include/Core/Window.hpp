#pragma once
#ifndef NANOALPHA_CORE_WINDOW_HPP
#define NANOALPHA_CORE_WINDOW_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <functional>
#include <chrono>

#include "Core/Log.hpp"

namespace nanoalpha {

// =============================================================================
//  Win32 ウィンドウ + Direct3D 11 デバイス / スワップチェーン (Phase 1)
// -----------------------------------------------------------------------------
//  起動 100ms 予算内にウィンドウ生成 → デバイス生成 → 描画開始までを完結させる。
//  フレームレートを一切消費しない同期 I/O を含まないこと。
// =============================================================================
class Window {
public:
    // 他 (Dear ImGui 等) へ先に処理チャンスを与えるフック。
    // true を返したメッセージは DefWindowProc へ渡さずここで完結する。
    using MessageHook = std::function<bool(HWND, UINT, WPARAM, LPARAM, LRESULT&)>;
    using ResizeHook = std::function<void(UINT, UINT)>;

    Window() = default;
    ~Window() { destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(HINSTANCE instance, int showCmd = SW_SHOWDEFAULT,
                UINT width = 1600, UINT height = 900);

    // Phase 1 の並行化: ウィンドウ生成と D3D11 デバイス生成は互いに依存しないため、
    //   1) auto deviceTask = async([&]{ return window.createDevice(); });
    //   2) window.createWindow(...)      // ウィンドウ (デバイス非依存)
    //   3) deviceTask.get() -> window.createSwapChain()
    // の順に呼ぶと、両者の所要時間が重なって Phase 1 が短くなる。
    // v3: 1408x792 (16:9)。右側に EQ / 操作パネルの別ウィンドウを置ける幅を残す。
    bool createWindow(HINSTANCE instance, int showCmd = SW_SHOWDEFAULT,
                      UINT width = 1408, UINT height = 792);
    bool createDevice() noexcept;    // ウィンドウ非依存 (別スレッドから呼べる)
    bool createSwapChain() noexcept; // createWindow + createDevice 完了後に呼ぶ
    void logSetup() const noexcept;  // "Phase 1: window=..." の行を出す

    void destroy() noexcept;

    bool pumpMessages() noexcept;   // WM_QUIT 受信で false

    void clear(const float* rgba = nullptr) noexcept; // nullptr = 既定の暗色
    void present(bool vsync) noexcept;

    // 全画面表示 (VLC 同様)。ウィンドウ枠を消してモニター全面に広げ、
    // 解除時は保存しておいた配置 / スタイルへ戻す。
    void setFullscreen(bool on) noexcept;
    bool fullscreen() const noexcept { return m_fullscreen; }

    HWND hwnd() const noexcept { return m_hwnd; }
    UINT width() const noexcept { return m_width; }
    UINT height() const noexcept { return m_height; }
    bool minimized() const noexcept { return m_minimized; }
    D3D_FEATURE_LEVEL featureLevel() const noexcept { return m_featureLevel; }

    ID3D11Device* device() const noexcept { return m_device.Get(); }
    ID3D11DeviceContext* context() const noexcept { return m_context.Get(); }
    IDXGISwapChain1* swapChain() const noexcept { return m_swapChain.Get(); }
    ID3D11RenderTargetView* renderTarget() const noexcept { return m_rtv.Get(); }

    void setMessageHook(MessageHook hook) { m_messageHook = std::move(hook); }
    void setResizeHook(ResizeHook hook) { m_resizeHook = std::move(hook); }

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    static bool enablePerMonitorDpi() noexcept;

    bool registerWindowClass(HINSTANCE instance) noexcept;
    void createRenderTarget() noexcept;
    void releaseRenderTarget() noexcept;
    void resize(UINT width, UINT height) noexcept;

    HINSTANCE m_instance = nullptr;
    HWND m_hwnd = nullptr;
    UINT m_width = 0;
    UINT m_height = 0;
    bool m_minimized = false;
    bool m_quitRequested = false;

    // 全画面表示 (スタイル・配置の退避先)
    bool m_fullscreen = false;
    DWORD m_savedStyle = WS_OVERLAPPEDWINDOW;
    DWORD m_savedExStyle = 0;
    WINDOWPLACEMENT m_savedPlacement{sizeof(WINDOWPLACEMENT)};

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;

    MessageHook m_messageHook;
    ResizeHook m_resizeHook;
};

// ---------------------------------------------------------------------------
// 実装
// ---------------------------------------------------------------------------

inline bool Window::enablePerMonitorDpi() noexcept {
#if defined(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetDpiContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto setContext = reinterpret_cast<SetDpiContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return true;
        }
    }
#endif
    return SetProcessDPIAware() != FALSE;
}

inline bool Window::registerWindowClass(HINSTANCE instance) noexcept {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::wndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // 毎フレーム自前でクリアする
    wc.lpszClassName = L"NanoAlphaPlayerWindow";

    if (RegisterClassExW(&wc) != 0) return true;
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

inline bool Window::createDevice() noexcept {
    const auto t0 = std::chrono::steady_clock::now();
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    const UINT debugFlags = flags | D3D11_CREATE_DEVICE_DEBUG;
#else
    const UINT debugFlags = flags;
#endif
    static constexpr D3D_FEATURE_LEVEL kLevelsWith11_1[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };
    static constexpr D3D_FEATURE_LEVEL kLevelsLegacy[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

    // プライマリアダプタを明示して渡す。ハイブリッド構成 (iGPU + dGPU) では
    // 既定アダプタの探索に時間がかかることがあり、明示すると数十 ms 早くなる。
    Microsoft::WRL::ComPtr<IDXGIFactory1> probeFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> primaryAdapter;
    D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
    IDXGIAdapter* adapterArg = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(probeFactory.ReleaseAndGetAddressOf()))) &&
        SUCCEEDED(probeFactory->EnumAdapters1(0, primaryAdapter.ReleaseAndGetAddressOf()))) {
        DXGI_ADAPTER_DESC1 adapterDesc{};
        if (primaryAdapter &&
            SUCCEEDED(primaryAdapter->GetDesc1(&adapterDesc)) &&
            (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            adapterArg = primaryAdapter.Get();
            driverType = D3D_DRIVER_TYPE_UNKNOWN;
        }
    }

    auto attempt = [&](UINT createFlags, const D3D_FEATURE_LEVEL* levels, UINT count) -> HRESULT {
        return D3D11CreateDevice(adapterArg, driverType, nullptr, createFlags, levels, count,
                                 D3D11_SDK_VERSION, m_device.ReleaseAndGetAddressOf(), &obtained,
                                 m_context.ReleaseAndGetAddressOf());
    };

    HRESULT hr = attempt(debugFlags, kLevelsWith11_1, 4);
    if (hr == E_INVALIDARG) hr = attempt(debugFlags, kLevelsLegacy, 3); // FL 11.1 非対応
    if (FAILED(hr) && adapterArg) {
        // 明示アダプタが使えない場合は既定アダプタで再試行
        adapterArg = nullptr;
        driverType = D3D_DRIVER_TYPE_HARDWARE;
        hr = attempt(debugFlags, kLevelsWith11_1, 4);
        if (hr == E_INVALIDARG) hr = attempt(debugFlags, kLevelsLegacy, 3);
    }
#if defined(_DEBUG)
    if (FAILED(hr)) hr = attempt(flags, kLevelsWith11_1, 4);            // デバッグレイヤー無し
    if (FAILED(hr) && hr == E_INVALIDARG) hr = attempt(flags, kLevelsLegacy, 3);
#endif
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    if (ms >= 2) log::info("Window: D3D11CreateDevice = %lld ms", ms);
    if (FAILED(hr)) {
        log::info("D3D11CreateDevice failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    m_featureLevel = obtained;
    return true;
}

inline bool Window::createSwapChain() noexcept {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory1;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory1.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        log::info("CreateDXGIFactory1 failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    hr = factory1->QueryInterface(IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        log::info("IDXGIFactory2 unavailable: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // 低遅延・ティアリング非使用
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr,
                                           m_swapChain.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        // FL 9.x 等の非対応環境は従来モデルへフォールバック
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        desc.BufferCount = 1;
        hr = m_factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &desc, nullptr, nullptr,
                                               m_swapChain.ReleaseAndGetAddressOf());
    }
    if (FAILED(hr)) {
        log::info("CreateSwapChainForHwnd failed: 0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);
    createRenderTarget();
    return m_rtv != nullptr;
}

inline void Window::createRenderTarget() noexcept {
    releaseRenderTarget();
    if (!m_swapChain || !m_context) return;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf())))) {
        log::info("GetBuffer(back) failed");
        return;
    }
    if (FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr,
                                                m_rtv.ReleaseAndGetAddressOf()))) {
        log::info("CreateRenderTargetView failed");
        return;
    }

    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(m_width);
    viewport.Height = static_cast<float>(m_height);
    viewport.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &viewport);
}

inline void Window::releaseRenderTarget() noexcept {
    if (m_context) {
        ID3D11RenderTargetView* nullRtv = nullptr;
        m_context->OMSetRenderTargets(1, &nullRtv, nullptr);
    }
    m_rtv.Reset();
}

inline void Window::resize(UINT width, UINT height) noexcept {
    if (width == 0 || height == 0) {
        m_minimized = true;
        return;
    }
    m_minimized = false;
    if (width == m_width && height == m_height && m_rtv) return;

    m_width = width;
    m_height = height;
    if (!m_swapChain) return;

    releaseRenderTarget();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
    if (m_resizeHook) m_resizeHook(width, height);
}

inline void Window::setFullscreen(bool on) noexcept {
    if (m_hwnd == nullptr || on == m_fullscreen) return;

    if (on) {
        m_savedStyle = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_STYLE));
        m_savedExStyle = static_cast<DWORD>(GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE));
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        GetWindowPlacement(m_hwnd, &wp);
        m_savedPlacement = wp;

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        const HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(mon, &mi);

        SetWindowLongPtrW(m_hwnd, GWL_STYLE,
                          static_cast<LONG_PTR>((m_savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP));
        SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        log::info("window fullscreen -> on");
    } else {
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, static_cast<LONG_PTR>(m_savedStyle));
        SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(m_savedExStyle));
        SetWindowPlacement(m_hwnd, &m_savedPlacement);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED |
                         SWP_SHOWWINDOW);
        log::info("window fullscreen -> off");
    }
    m_fullscreen = on;
}

inline bool Window::createWindow(HINSTANCE instance, int showCmd, UINT width, UINT height) {
    m_instance = instance;
    enablePerMonitorDpi();

    if (!registerWindowClass(instance)) {
        log::info("RegisterClassExW failed: %lu", GetLastError());
        return false;
    }

    RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

    // v3: 右側に EQ / 従来パネルの別ウィンドウを置けるよう、左寄せで出す。
    // (中央寄せだと左右の空きが足りず、別ウィンドウが映像に重なるため)
    const int frameW = static_cast<int>(rc.right - rc.left);
    const int frameH = static_cast<int>(rc.bottom - rc.top);
    const int monitorW = GetSystemMetrics(SM_CXSCREEN);
    const int monitorH = GetSystemMetrics(SM_CYSCREEN);
    int posX = 8;
    if (posX + frameW > monitorW) posX = 0;
    int posY = (monitorH - frameH) / 2;
    if (posY < 0) posY = 0;

    m_hwnd = CreateWindowExW(0, L"NanoAlphaPlayerWindow", L"NanoAlphaPlayer", WS_OVERLAPPEDWINDOW,
                             posX, posY, frameW, frameH,
                             nullptr, nullptr, instance, this);
    if (!m_hwnd) {
        log::info("CreateWindowExW failed: %lu", GetLastError());
        return false;
    }

    RECT client{};
    GetClientRect(m_hwnd, &client);
    m_width = static_cast<UINT>(client.right - client.left);
    m_height = static_cast<UINT>(client.bottom - client.top);

    // ス왑チェーン生成前の黒画面でも先にウィンドウを出す (present/resize はガード済み)
    ShowWindow(m_hwnd, showCmd);
    UpdateWindow(m_hwnd);
    return true;
}

inline void Window::logSetup() const noexcept {
    log::info("Phase 1: window=%ux%u  D3D feature level=0x%04X", m_width, m_height,
              static_cast<unsigned>(m_featureLevel));
}

inline bool Window::create(HINSTANCE instance, int showCmd, UINT width, UINT height) {
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsedMs = [&](const char* stage) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms >= 2) log::info("Window: init %s = %lld ms", stage, ms);
    };

    if (!createWindow(instance, showCmd, width, height)) return false;
    elapsedMs("CreateWindowEx");

    if (!createDevice()) { destroy(); return false; } // 所要時間は createDevice 内で出力
    if (!createSwapChain()) { destroy(); return false; }
    elapsedMs("SwapChain");

    logSetup();
    return true;
}

inline void Window::destroy() noexcept {
    releaseRenderTarget();
    m_swapChain.Reset();
    if (m_context) {
        m_context->ClearState();
        m_context->Flush();
    }
    m_context.Reset();
    m_factory.Reset();
    m_device.Reset();

    if (m_hwnd) {
        HWND hwnd = m_hwnd;
        m_hwnd = nullptr;
        DestroyWindow(hwnd);   // → WM_DESTROY → PostQuitMessage
    }
}

inline bool Window::pumpMessages() noexcept {
    if (m_quitRequested) return false;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            m_quitRequested = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

inline void Window::clear(const float* rgba) noexcept {
    static constexpr float kDefaultColor[4] = {0.043f, 0.047f, 0.055f, 1.0f};
    if (!m_context || !m_rtv) return;
    const float* color = (rgba != nullptr) ? rgba : kDefaultColor;
    ID3D11RenderTargetView* rtv = m_rtv.Get();
    m_context->OMSetRenderTargets(1, &rtv, nullptr);
    m_context->ClearRenderTargetView(rtv, color);
}

inline void Window::present(bool vsync) noexcept {
    if (!m_swapChain || m_minimized) return;
    m_swapChain->Present(vsync ? 1u : 0u, 0u);
}

inline LRESULT CALLBACK Window::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Window* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    if (self != nullptr && self->m_messageHook) {
        LRESULT result = 0;
        if (self->m_messageHook(hwnd, msg, wParam, lParam, result)) return result;
    }

    if (self != nullptr) {
        switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                self->m_minimized = true;
            } else {
                self->resize(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;
        case WM_ERASEBKGND:
            return 1; // 常に自前で描画するため消去不要
        case WM_DESTROY:
            self->m_hwnd = nullptr;
            self->m_minimized = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace nanoalpha

#endif // NANOALPHA_CORE_WINDOW_HPP
