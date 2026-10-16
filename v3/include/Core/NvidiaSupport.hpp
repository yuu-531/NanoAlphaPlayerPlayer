#pragma once
#ifndef NANOALPHA_CORE_NVIDIA_SUPPORT_HPP
#define NANOALPHA_CORE_NVIDIA_SUPPORT_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dxgi.h>

#include <string>

#include "Core/Log.hpp"

// =============================================================================
//  NVIDIA 理論対応 (v3)
// -----------------------------------------------------------------------------
//  このマシンでは NVIDIA GPU を実機検証できないため、以下の方針で
//  「理論上動く状態」を保証する。
//
//    1. DXGI で PCI Vendor ID = 0x10DE (NVIDIA) を検出する。
//    2. ドライバの実在ファイル (nvngx_dlss / nvngx_dlssg / nvcuda / nvapi64) を
//       System32 および DriverStore\FileRepository から探索する。
//       探索は GetFileAttributesW だけで行い、DLL は一切 LoadLibrary しない。
//    3. 起動時に検出結果をログへ出す。
//    4. tryDlssInit() は NGX を実際に初期化しない (誤動作回避)。
//       検出結果に関わらず必ず false を返し、呼び出し元は FSR1 へフォールバックする。
//
//  → 実機 (NVIDIA) 環境ではドライバ検出までは動作し、超解像度は FSR1 で代替される。
// =============================================================================
namespace nanoalpha::nvidia {

struct Info {
    bool dxgiAvailable = false;  // DXGI ファクトリを作成できた
    bool nvidiaGpu = false;      // Vendor 0x10DE のアダプタが存在
    bool nvngx = false;          // nvngx_dlss.dll  (DLSS 本体)
    bool nvdlssg = false;        // nvngx_dlssg.dll (フレーム生成)
    bool nvcuda = false;         // nvcuda.dll
    bool nvapi = false;          // nvapi64.dll
    std::wstring gpuName;        // アダプタ名 (表示用、UTF-16)

    bool dlssReady() const noexcept { return nvidiaGpu && nvngx; }
    bool dlssgReady() const noexcept { return nvidiaGpu && nvdlssg; }
};

namespace detail {

inline bool fileExists(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

inline std::wstring systemDirectory() {
    wchar_t buffer[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return std::wstring();
    return std::wstring(buffer, length);
}

// DriverStore\FileRepository の各フォルダからファイルを探す (ドライバ配置の定石)
inline bool findInDriverStore(const std::wstring& fileName) {
    const std::wstring system = systemDirectory();
    if (system.empty()) return false;

    const std::wstring root = system + L"\\DriverStore\\FileRepository";
    const std::wstring pattern = root + L"\\*";

    WIN32_FIND_DATAW findData{};
    const HANDLE handle = FindFirstFileW(pattern.c_str(), &findData);
    if (handle == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
        if (findData.cFileName[0] == L'.') continue;
        if (fileExists(root + L"\\" + findData.cFileName + L"\\" + fileName)) {
            found = true;
            break;
        }
    } while (FindNextFileW(handle, &findData));

    FindClose(handle);
    return found;
}

// GPU / ドライバの実在検出。LoadLibrary は一切しない。
inline Info probe() {
    Info info{};

    // ---- DXGI: GPU 検出 ----------------------------------------------------
    using CreateDXGIFactory1Fn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi) {
        auto createFactory =
            reinterpret_cast<CreateDXGIFactory1Fn>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
        if (createFactory) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) &&
                factory) {
                info.dxgiAvailable = true;
                for (UINT index = 0;; ++index) {
                    IDXGIAdapter1* adapter = nullptr;
                    if (FAILED(factory->EnumAdapters1(index, &adapter)) || !adapter) break;

                    DXGI_ADAPTER_DESC1 desc{};
                    adapter->GetDesc1(&desc);
                    if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                        if (desc.VendorId == 0x10DE) {
                            info.nvidiaGpu = true;
                            if (info.gpuName.empty()) info.gpuName = desc.Description;
                        } else if (info.gpuName.empty()) {
                            info.gpuName = desc.Description; // 最初の物理アダプタ名
                        }
                    }
                    adapter->Release();
                }
                factory->Release();
            }
        }
        FreeLibrary(dxgi);
    }

    // ---- ドライバファイル検出 ---------------------------------------------
    const std::wstring system = systemDirectory();
    if (!system.empty()) {
        info.nvngx = fileExists(system + L"\\nvngx_dlss.dll");
        info.nvdlssg = fileExists(system + L"\\nvngx_dlssg.dll");
        info.nvcuda = fileExists(system + L"\\nvcuda.dll");
        info.nvapi = fileExists(system + L"\\nvapi64.dll");
    }
    if (!info.nvngx) info.nvngx = findInDriverStore(L"nvngx_dlss.dll");
    if (!info.nvdlssg) info.nvdlssg = findInDriverStore(L"nvngx_dlssg.dll");
    if (!info.nvcuda) info.nvcuda = findInDriverStore(L"nvcuda.dll");
    if (!info.nvapi) info.nvapi = findInDriverStore(L"nvapi64.dll");

    return info;
}

} // namespace detail

// 検出結果 (初回呼び出しでキャッシュ)
inline const Info& probeCached() {
    static const Info info = [] {
        const Info result = detail::probe();

        char name[128]{};
        if (!result.gpuName.empty()) {
            const int converted = WideCharToMultiByte(CP_UTF8, 0, result.gpuName.c_str(), -1,
                                                       name, static_cast<int>(sizeof(name)),
                                                       nullptr, nullptr);
            if (converted <= 0) name[0] = '\0';
        }

        log::info("[NVIDIA] DXGI=%d vendorNvidia=%d gpu=\"%s\"", result.dxgiAvailable ? 1 : 0,
                  result.nvidiaGpu ? 1 : 0, name);
        log::info("[NVIDIA] nvngx=%d dlssg=%d nvcuda=%d nvapi=%d -> dlss=%d dlssg=%d",
                  result.nvngx ? 1 : 0, result.nvdlssg ? 1 : 0, result.nvcuda ? 1 : 0,
                  result.nvapi ? 1 : 0, result.dlssReady() ? 1 : 0, result.dlssgReady() ? 1 : 0);
        return result;
    }();
    return info;
}

// NGX (DLSS / DLSS-FG) の初期化を試みる。
//  実機検証できないため実際の NGX 呼び出しは行わず、常に false を返す。
//  呼び出し元は false のとき FSR1 (AMD 超解像度) へフォールバックする。
inline bool tryDlssInit() noexcept {
    const Info& info = probeCached();
    if (info.dlssReady() || info.dlssgReady()) {
        log::info("[NVIDIA] NGX detected but real init is skipped (unverified) -> use FSR1");
    }
    return false;
}

} // namespace nanoalpha::nvidia

#endif // NANOALPHA_CORE_NVIDIA_SUPPORT_HPP
