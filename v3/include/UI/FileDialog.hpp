#pragma once
#ifndef NANOALPHA_UI_FILE_DIALOG_HPP
#define NANOALPHA_UI_FILE_DIALOG_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>   // GetOpenFileNameW / GetSaveFileNameW
#include <shobjidl.h>  // IFileOpenDialog / IFileSaveDialog
#include <process.h>   // _beginthreadex (STA スレッド起動)

#include <cwchar>
#include <string>
#include <utility>
#include <vector>

#include <wrl/client.h>  // Microsoft::WRL::ComPtr

// コモンダイアログのオーナー指定に必要 (windows.h が既定のため未定義の場合がある)
#ifndef OFN_ALLOWMULTISELECT
#define OFN_ALLOWMULTISELECT 0x00000200
#endif

namespace nanoalpha::ui {

// =============================================================================
//  メディアファイル選択ダイアログ (v2)
// -----------------------------------------------------------------------------
//  IFileOpenDialog (Vista 以降のスタイル) を第一候補とし、COM が使えない環境では
//  旧来の GetOpenFileNameW へフォールバックする。
//  どちらもメインスレッド上でモーダルに動作するため、呼ぶのは ContextLock の
//  外側 (main のループ内で、描画スコープを抜いた後) でなければならない。
// =============================================================================

// 再生対象の拡張子一覧 (name / pattern)
inline const std::vector<std::pair<std::wstring, std::wstring>>& mediaFilters() noexcept {
    static const std::vector<std::pair<std::wstring, std::wstring>> filters = {
        {L"すべてのメディアファイル",
         L"*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.m4v;"
         L"*.3gp;*.ogv;*.rm;*.rmvb;*.vob;*.asf;*.divx;*.f4v;*.mxf;*.ogm;*.iso;"
         L"*.mp3;*.flac;*.wav;*.aac;*.m4a;*.ogg;*.opus;*.wma;*.ape;*.aiff;*.amr;*.mka;"
         L"*.srt;*.ass;*.ssa;*.vtt;*.sub"},
        {L"映像ファイル",
         L"*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.flv;*.webm;*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;"
         L"*.m4v;*.3gp;*.ogv;*.rm;*.rmvb;*.vob;*.asf;*.divx;*.f4v;*.mxf;*.ogm;*.iso"},
        {L"音声ファイル",
         L"*.mp3;*.flac;*.wav;*.aac;*.m4a;*.ogg;*.opus;*.wma;*.ape;*.aiff;*.amr;*.mka"},
        {L"字幕ファイル", L"*.srt;*.ass;*.ssa;*.vtt;*.sub"},
        {L"すべてのファイル", L"*.*"},
    };
    return filters;
}

namespace detail {

// コールバックからプラットフォームを越えてパターンを取り出すための区切り変換
inline std::wstring semicolonsToSpaces(std::wstring text) noexcept {
    for (auto& ch : text) {
        if (ch == L';') ch = L' ';
    }
    return text;
}

// COM は初回に一度だけ初期化すれば足りる。既に初期化済み (S_FALSE) や
// モード不一致 (RPC_E_CHANGED_MODE) でも以降の CoCreateInstance は動作する。
inline HRESULT ensureCom() noexcept {
    static const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    return hr;
}

inline void applyFileTypes(IFileDialog* dialog) {
    const auto& filters = mediaFilters();
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(filters.size());
    for (const auto& filter : filters) {
        specs.push_back({filter.first.c_str(), filter.second.c_str()});
    }
    if (specs.empty()) return;
    dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(L"mp4");
}

inline std::wstring pathFromResult(IShellItem* item) noexcept {
    if (!item) return {};
    PWSTR raw = nullptr;
    const HRESULT hr = item->GetDisplayName(SIGDN_FILESYSPATH, &raw);
    std::wstring path = (SUCCEEDED(hr) && raw) ? raw : L"";
    if (raw) CoTaskMemFree(raw);
    return path;
}

// ---------------------------------------------------------------------------
//  STA 実行ヘルパ
// -----------------------------------------------------------------------------
//  IFileOpenDialog / IFileSaveDialog は STA でしか確実に動作しない。本体の
//  メインスレッドは WASAPI 初期化の都合で MTA (main.cpp: COINIT_MULTITHREADED)
//  のままなので、そのまま CoCreateInstance + Show を呼ぶとダイアログの窓が
//  生成される前に無限に待って応答不能になる (実測で再現)。
//
//  そこで呼び出しスレッドが MTA のときだけ、ダイアログ処理を一時的な STA
//  スレッドへ移して実行する。ダイアログはオーナー窓へ WM_ENABLE 等を送って
//  くるため、待つ側でもメッセージを回して応答する必要がある。
template <typename Fn>
struct StaTask {
    using Result = decltype(std::declval<Fn>()());

    explicit StaTask(Fn f) : fn(std::move(f)) {}

    Fn fn;
    Result value{};
    HANDLE event = nullptr;

    static unsigned __stdcall run(void* raw) noexcept {
        auto* task = static_cast<StaTask*>(raw);
        ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        task->value = task->fn();
        ::CoUninitialize();
        ::SetEvent(task->event);
        return 0;
    }
};

template <typename Fn>
inline auto runModalOnSta(Fn&& fn) -> decltype(std::declval<Fn>()()) {
    using Task = StaTask<std::decay_t<Fn>>;
    using Result = typename Task::Result;

    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (com != RPC_E_CHANGED_MODE) {
        // S_OK / S_FALSE: このスレッドは STA なのでその場で実行してよい
        Result value = fn();
        ::CoUninitialize();
        return value;
    }

    // RPC_E_CHANGED_MODE: スレッドは MTA。ダイアログだけを STA スレッドへ移す。
    Task task(std::forward<Fn>(fn));
    task.event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    uintptr_t thread = 0;
    if (task.event) {
        thread = ::_beginthreadex(nullptr, 0, &Task::run, &task, 0, nullptr);
    }
    if (thread == 0) {
        if (task.event) ::CloseHandle(task.event);
        return Result{}; // ダイアログを出せない → キャンセル扱い (固まらない)
    }

    MSG quit{};
    bool hasQuit = false;
    for (;;) {
        const DWORD state = ::WaitForSingleObject(task.event, 10);
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit = msg;
                hasQuit = true;
                continue;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (state != WAIT_TIMEOUT) break;
    }
    ::CloseHandle(reinterpret_cast<HANDLE>(thread));
    ::CloseHandle(task.event);
    if (hasQuit) ::PostQuitMessage(static_cast<int>(quit.wParam));
    return std::move(task.value);
}

} // namespace detail

// ---------------------------------------------------------------------------
//  単一ファイル選択
// ---------------------------------------------------------------------------
namespace detail {

inline std::wstring pickMediaFileImpl(HWND owner) noexcept {
    detail::ensureCom();

    // --- 第一候補: IFileOpenDialog ---
    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT hr =
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM;
            dialog->SetOptions(options);
            detail::applyFileTypes(dialog.Get());
            dialog->SetTitle(L"メディアファイルを開く");

            hr = dialog->Show(owner);
            if (SUCCEEDED(hr)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    std::wstring path = detail::pathFromResult(item);
                    item->Release();
                    return path;
                }
            }
            // キャンセルならここで終了。それ以外 (COM の問題など) は下へフォールバック。
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT) return {};
        }
    }

    // --- フォールバック: 旧スタイルの共通ダイアログ ---
    std::wstring filter;
    for (const auto& entry : mediaFilters()) {
        filter += entry.first;
        filter.push_back(L'\0');
        filter += detail::semicolonsToSpaces(entry.second);
        filter.push_back(L'\0');
    }
    filter.push_back(L'\0');

    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"メディアファイルを開く";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        return std::wstring(buffer.c_str());
    }
    return {};
}

} // namespace detail

inline std::wstring pickMediaFile(HWND owner) noexcept {
    return detail::runModalOnSta([owner] { return detail::pickMediaFileImpl(owner); });
}

// ---------------------------------------------------------------------------
//  複数ファイル選択 (プレイリスト投入用)
// ---------------------------------------------------------------------------
namespace detail {

inline std::vector<std::wstring> pickMediaFilesImpl(HWND owner) noexcept {
    std::vector<std::wstring> result;
    detail::ensureCom();

    {
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT hr =
            CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            options |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM |
                       FOS_ALLOWMULTISELECT;
            dialog->SetOptions(options);
            detail::applyFileTypes(dialog.Get());
            dialog->SetTitle(L"メディアファイルを複数選択");

            hr = dialog->Show(owner);
            if (SUCCEEDED(hr)) {
                IShellItemArray* items = nullptr;
                if (SUCCEEDED(dialog->GetResults(&items)) && items) {
                    DWORD count = 0;
                    items->GetCount(&count);
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* item = nullptr;
                        if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                            std::wstring path = detail::pathFromResult(item);
                            item->Release();
                            if (!path.empty()) result.push_back(std::move(path));
                        }
                    }
                    items->Release();
                }
                return result;
            }
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT) return result;
        }
    }

    std::wstring filter;
    for (const auto& entry : mediaFilters()) {
        filter += entry.first;
        filter.push_back(L'\0');
        filter += detail::semicolonsToSpaces(entry.second);
        filter.push_back(L'\0');
    }
    filter.push_back(L'\0');

    // 旧ダイアログはバッフ先頭が大文字ならパスなしで NUL 区切り + 二重 NUL 終端
    std::wstring buffer(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrTitle = L"メディアファイルを複数選択";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT;

    if (!GetOpenFileNameW(&ofn)) return result;

    const wchar_t* cursor = buffer.c_str();
    const std::wstring dir(cursor);
    cursor += dir.size() + 1;
    if (*cursor == L'\0') {
        result.push_back(dir); // 単一選択
        return result;
    }
    while (*cursor != L'\0') {
        result.push_back(dir + L"\\" + cursor);
        cursor += std::wcslen(cursor) + 1;
    }
    return result;
}

} // namespace detail

inline std::vector<std::wstring> pickMediaFiles(HWND owner) noexcept {
    return detail::runModalOnSta([owner] { return detail::pickMediaFilesImpl(owner); });
}

// ---------------------------------------------------------------------------
//  保存先選択 (スナップショット用)
// ---------------------------------------------------------------------------
namespace detail {

inline std::wstring saveFileImpl(HWND owner, const std::wstring& suggestedName,
                                 const std::wstring& filterSpec) noexcept {
    detail::ensureCom();

    {
        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        HRESULT hr =
            CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            options |= FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT;
            dialog->SetOptions(options);

            COMDLG_FILTERSPEC spec{L"PNG 画像", L"*.png"};
            dialog->SetFileTypes(1, &spec);
            dialog->SetFileTypeIndex(1);
            dialog->SetDefaultExtension(L"png");
            if (!suggestedName.empty()) dialog->SetFileName(suggestedName.c_str());
            dialog->SetTitle(L"スナップショットの保存");

            hr = dialog->Show(owner);
            if (SUCCEEDED(hr)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    std::wstring path = detail::pathFromResult(item);
                    item->Release();
                    return path;
                }
            }
            if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED) || hr == E_ABORT) return {};
        }
    }

    std::wstring filter = L"PNG 画像";
    filter.push_back(L'\0');
    filter += filterSpec.empty() ? L"*.png" : filterSpec;
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    std::wstring buffer(suggestedName.empty() ? L"snapshot.png" : suggestedName);
    buffer.resize(32768, L'\0');
    buffer[suggestedName.size()] = L'\0';

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = L"スナップショットの保存";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn)) return std::wstring(buffer.c_str());
    return {};
}

} // namespace detail

inline std::wstring saveFile(HWND owner, const std::wstring& suggestedName,
                             const std::wstring& filterSpec) noexcept {
    return detail::runModalOnSta(
        [owner, suggestedName, filterSpec] {
            return detail::saveFileImpl(owner, suggestedName, filterSpec);
        });
}

} // namespace nanoalpha::ui

#endif // NANOALPHA_UI_FILE_DIALOG_HPP
