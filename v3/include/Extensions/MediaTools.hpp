#pragma once
#ifndef NANOALPHA_EXTENSIONS_MEDIA_TOOLS_HPP
#define NANOALPHA_EXTENSIONS_MEDIA_TOOLS_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Core/Log.hpp"

namespace nanoalpha::tools {

// =============================================================================
//  VLC 機能群 (v2) — スナップショット / 壁紙 / プレイリスト
// -----------------------------------------------------------------------------
//  いずれもヘッダのみで完結させる (CMake の追加ソースを増やさないため)。
//  GPU へ触れる関数は呼び出し元が ContextLock の内側から呼ぶこと。
// =============================================================================

// ---------------------------------------------------------------------------
//  スナップショット: バックバッファを PNG として保存する
// -----------------------------------------------------------------------------
//  映像フレーム (NV12 / P010) を直接読むと色変換が要るが、スワップチェーンの
//  バックバッファは既に RGB なので WIC へそのまま書き出せる。
// ---------------------------------------------------------------------------
inline bool saveBackbufferPng(IDXGISwapChain1* swapChain, const std::wstring& path) {
    if (!swapChain || path.empty()) return false;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer))) || !backbuffer) {
        log::info("MediaTools: スナップショット失敗 (バックバッファ取得不可)");
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backbuffer->GetDesc(&desc);
    if (desc.SampleDesc.Count > 1) {
        log::info("MediaTools: スナップショットは MSAA 非対応 (count=%u)",
                  desc.SampleDesc.Count);
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    backbuffer->GetDevice(&device);
    if (!device) return false;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context) return false;

    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.MipLevels = 1;
    staging.ArraySize = 1;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> cpu;
    if (FAILED(device->CreateTexture2D(&staging, nullptr, &cpu)) || !cpu) {
        log::info("MediaTools: スナップショット失敗 (ステージングテクスチャ)");
        return false;
    }

    // GPU レンダリングの完了を待ってからコピーする (バックバッファは読むだけ)
    context->CopyResource(cpu.Get(), backbuffer.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(cpu.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        log::info("MediaTools: スナップショット失敗 (Map)");
        return false;
    }

    const UINT width = desc.Width;
    const UINT height = desc.Height;
    const UINT stride = width * 4;
    std::vector<std::uint8_t> pixels(static_cast<size_t>(stride) * height);

    // バックバッファは R8G8B8A8 なので、WIC の 32bppBGRA へ並べ替える
    const bool isBGRA = (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    for (UINT y = 0; y < height; ++y) {
        const auto* src = static_cast<const std::uint8_t*>(mapped.pData) +
                          static_cast<size_t>(y) * mapped.RowPitch;
        std::uint8_t* dst = pixels.data() + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < width; ++x) {
            const std::uint8_t r = src[x * 4 + 0];
            const std::uint8_t g = src[x * 4 + 1];
            const std::uint8_t b = src[x * 4 + 2];
            const std::uint8_t a = src[x * 4 + 3];
            if (isBGRA) {
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
            } else {
                dst[x * 4 + 0] = r;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = b;
            }
            dst[x * 4 + 3] = a;
        }
    }
    context->Unmap(cpu.Get(), 0);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr; // 既に初期化済み (RPC_E_CHANGED_MODE) でも以降は動作する

    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        log::info("MediaTools: WIC ファクトリ生成失敗 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || !stream) return false;
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        log::info("MediaTools: 保存先を開けません: %ls", path.c_str());
        return false;
    }

    Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) || !encoder)
        return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;

    Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || !frame) return false;
    if (FAILED(frame->Initialize(nullptr))) return false;

    frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
    if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA,
                                               stride,
                                               static_cast<UINT>(pixels.size()), pixels.data(),
                                               &bitmap)) ||
        !bitmap) {
        return false;
    }

    if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;

    log::info("MediaTools: スナップショット保存  %ls  (%ux%u)", path.c_str(), width, height);
    return true;
}

// ---------------------------------------------------------------------------
//  壁紙モード: 保存済み PNG をデスクトップ壁紙にする
// -----------------------------------------------------------------------------
inline bool setDesktopWallpaper(const std::wstring& path) {
    if (path.empty()) return false;
    const BOOL ok = SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0,
                                          const_cast<wchar_t*>(path.c_str()),
                                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    if (!ok) {
        log::info("MediaTools: 壁紙の設定に失敗 (path=%ls)", path.c_str());
        return false;
    }
    log::info("MediaTools: 壁紙を設定しました  %ls", path.c_str());
    return true;
}

// ---------------------------------------------------------------------------
//  プレイリスト
// -----------------------------------------------------------------------------
//  VLC 風の簡易プレイリスト。単一ファイル再生でも本体は動作するが、
//  拡張が握ることで「次へ / 前へ」操作と UI 表示を可能にする。
// ---------------------------------------------------------------------------
class Playlist {
public:
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.clear();
        m_index = 0;
    }

    void add(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.push_back(path);
    }

    void addMany(const std::vector<std::wstring>& paths) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_items.insert(m_items.end(), paths.begin(), paths.end());
    }

    //  再生中のファイルを「現在位置」として記録する。
    //  既に登録済みならその番号へ、未登録なら末尾へ追加してから番号を合わせる。
    //  Ctrl+O / URL 欄で別ファイルを開いたとき、前へ・次へが
    //  再生中ファイルとズレた位置から動いてしまうのを防ぐ。
    bool setOrAdd(const std::wstring& path) {
        if (path.empty()) return false;
        std::lock_guard<std::mutex> lock(m_mutex);
        //  既に現在位置が一致していれば何もしない (同名アイテムが複数あっても
        //  「次へ」が前に跳ね返らない)。
        if (m_index < m_items.size() && m_items[m_index] == path) return true;
        for (size_t i = 0; i < m_items.size(); ++i) {
            if (m_items[i] == path) {
                m_index = i;
                return true;
            }
        }
        m_items.push_back(path);
        m_index = m_items.size() - 1;
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items.size();
    }

    size_t index() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_index;
    }

    std::wstring current() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_index >= m_items.size()) return {};
        return m_items[m_index];
    }

    // 相対移動 (ループする)。移動先が変われば true。
    bool move(int delta) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) return false;
        const long long count = static_cast<long long>(m_items.size());
        long long next = static_cast<long long>(m_index) + delta;
        next %= count;
        if (next < 0) next += count;
        if (next == static_cast<long long>(m_index)) return false;
        m_index = static_cast<size_t>(next);
        return true;
    }

    // 再生終了時の自動送り。wrap=false では末尾で止まる (リピート OFF)。
    // 1 件しかないリストを wrap しても自分自身になるため false を返し、
    // 呼び側で「先頭へシーク」に切り替える。
    bool next(bool wrap) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) return false;
        const size_t count = m_items.size();
        if (m_index + 1 < count) {
            ++m_index;
            return true;
        }
        if (!wrap || count < 2) return false;
        m_index = 0;
        return true;
    }

    // 選択中の 1 件を削除 (再生中ファイルが消える可能性は呼び側の判断)
    bool removeAt(size_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= m_items.size()) return false;
        m_items.erase(m_items.begin() + static_cast<std::ptrdiff_t>(index));
        if (m_index > index) {
            --m_index;
        } else if (m_index >= m_items.size()) {
            m_index = m_items.empty() ? 0 : m_items.size() - 1;
        }
        return true;
    }

    // 選択中の 1 件を上下に並べ替え (-1 = 上へ / +1 = 下へ)
    bool reorder(long long delta) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_items.empty()) return false;
        const long long from = static_cast<long long>(m_index);
        const long long to = from + delta;
        if (to < 0 || to >= static_cast<long long>(m_items.size())) return false;
        using std::swap;
        swap(m_items[static_cast<size_t>(from)], m_items[static_cast<size_t>(to)]);
        m_index = static_cast<size_t>(to);
        return true;
    }

    // 件数と選択位置 (UI の「3 / 12」表示用)
    void status(size_t& count, size_t& index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        count = m_items.size();
        index = m_index;
    }

    bool seekTo(size_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= m_items.size()) return false;
        m_index = index;
        return true;
    }

    // コピーを取る (UI 表示用)
    std::vector<std::wstring> items() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_items;
    }

private:
    mutable std::mutex m_mutex;
    std::vector<std::wstring> m_items;
    size_t m_index = 0;
};

// ---------------------------------------------------------------------------
//  VLCFeatures: 上記の機能群を 1 つのライフタイムとして束ねる
// -----------------------------------------------------------------------------
//  Phase 2 のローダが生成し、ReadyHook が main 側へ受け渡す。
// ---------------------------------------------------------------------------
struct VlcFeatures {
    Playlist playlist;
    std::wstring lastSnapshot;
    bool wallpaperEnabled = false;

    // 機能の実装状況 (UI の拡張タブに表示する)
    static constexpr const char* kCapabilities = "プレイリスト / スナップショット / 壁紙";
    static constexpr const char* kNotImplemented = "トランスコード / 画面キャスト / VR";
};

} // namespace nanoalpha::tools

#endif // NANOALPHA_EXTENSIONS_MEDIA_TOOLS_HPP
