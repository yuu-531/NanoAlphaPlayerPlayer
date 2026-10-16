#include "Extensions/VlcVideo.hpp"

#include "Core/Log.hpp"

#include <cstring>
#include <cwctype>
#include <type_traits>

namespace nanoalpha {
namespace vlc {
namespace {

// libVLC の video callback は opaque を経由して来るが、format setup の
// `void **opaque` がどのオブジェクトを指すかは VLC の内部実装依存になる。
// アプリでは同時に VLC で再生するのは 1 つだけなので、現在アクティブな
// インスタンスを静的に持ち、callback はそれを参照する (最も素直で安全)。
Video* g_active = nullptr;

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return std::string();
    const int needed =
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        needed, nullptr, nullptr);
    return out;
}

DWORD attributesOf(const std::wstring& path) {
    return path.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path.c_str());
}

bool isDirectory(const std::wstring& path) {
    const DWORD a = attributesOf(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool pathExists(const std::wstring& path) {
    return attributesOf(path) != INVALID_FILE_ATTRIBUTES;
}

std::wstring upperCopy(std::wstring text) {
    if (!text.empty()) CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

std::wstring trimTrailing(std::wstring text) {
    while (!text.empty() &&
           (text.back() == L' ' || text.back() == L'\t' || text.back() == L'"' ||
            text.back() == L'\'' || text.back() == L'/')) {
        text.pop_back();
    }
    return text;
}

std::wstring joinPath(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    if (directory.back() == L'\\' || directory.back() == L'/') return directory + name;
    return directory + L"\\" + name;
}

// "C:\a\b\c" -> "C:\a\b"   /  "C:\VIDEO_TS" -> "C:\"
std::wstring parentOf(const std::wstring& path) {
    if (path.empty()) return std::wstring();
    std::wstring trimmed = trimTrailing(path);
    std::size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 0) return trimmed.substr(0, 1);              // "C:\foo" -> "C:\"
    if (slash == 2 && trimmed.size() >= 3 && trimmed[1] == L':')
        return trimmed.substr(0, 3);                          // "C:\foo" -> "C:\"
    return trimmed.substr(0, slash);
}

std::wstring baseNameOf(const std::wstring& path) {
    const std::wstring trimmed = trimTrailing(path);
    const std::size_t slash = trimmed.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return trimmed;
    return trimmed.substr(slash + 1);
}

std::wstring extensionOf(const std::wstring& path) {
    const std::wstring name = baseNameOf(path);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return std::wstring();
    return upperCopy(name.substr(dot));
}

// 指定ディレクトリ配下の *.VOB を列挙する
int countVobs(const std::wstring& directory, std::wstring* largestOut) {
    int count = 0;
    ULONGLONG largestSize = 0;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = joinPath(directory, L"*.VOB");
    HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
    if (handle == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (extensionOf(fd.cFileName) != L".VOB") continue;
        ++count;
        const ULONGLONG size =
            (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (size > largestSize) {
            largestSize = size;
            if (largestOut) *largestOut = joinPath(directory, fd.cFileName);
        }
    } while (FindNextFileW(handle, &fd));
    FindClose(handle);
    return count;
}

// -----------------------------------------------------------------------------
//  暗号化 (CSS スクランブル) の判定
// -----------------------------------------------------------------------------
//  DVD の各 2048 バイトセクターは MPEG-PS のパックヘッダ (00 00 01 BA) から
//  始まる。CSS でスクランブルされるとその並びが壊れるので、先頭から複数の
//  セクターを読んで「1 つでもパックヘッダが見つかるか」で判定する。
//    - 見つかる       → 復号不要 (FFmpeg 経路)
//    - 1 つも無い     → スクランブル済み (libVLC へ)
//    - 読めない       → 判定不能 (isDvd は立てるが detectable=false)
// -----------------------------------------------------------------------------
bool probeVobEncryption(const std::wstring& file, bool& encrypted) {
    HANDLE handle =
        CreateFileW(file.c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;

    constexpr DWORD kSector = 2048;
    constexpr DWORD kSectors = 64; // 128 KB 分を見れば十分判定できる
    std::vector<unsigned char> buffer(static_cast<std::size_t>(kSector) * kSectors);
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                             nullptr);
    CloseHandle(handle);
    if (!ok || read < kSector) return false;

    const DWORD sectors = read / kSector;
    for (DWORD i = 0; i < sectors; ++i) {
        const unsigned char* sector = buffer.data() + (static_cast<std::size_t>(i) * kSector);
        if (sector[0] == 0x00 && sector[1] == 0x00 && sector[2] == 0x01 && sector[3] == 0xBA) {
            encrypted = false;
            return true; // パックヘッダあり = 暗号なし
        }
    }
    encrypted = true;
    return true; // 1 セクターも読めなかった → スクランブルとみなす
}

// -----------------------------------------------------------------------------
//  libvlc.dll の探索
// -----------------------------------------------------------------------------
std::wstring queryInstallDir(HKEY root, const wchar_t* subkey) {
    const REGSAM views[] = {KEY_READ | KEY_WOW64_64KEY, KEY_READ | KEY_WOW64_32KEY};
    for (REGSAM view : views) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey, 0, view, &key) != ERROR_SUCCESS) continue;
        wchar_t buffer[1024] = {};
        DWORD type = 0;
        DWORD size = sizeof(buffer) - sizeof(wchar_t);
        const LSTATUS status =
            RegQueryValueExW(key, L"InstallDir", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buffer), &size);
        RegCloseKey(key);
        if (status != ERROR_SUCCESS) continue;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        if (buffer[0] == L'\0') continue;
        const std::wstring directory(buffer);
        const std::wstring candidate = joinPath(directory, L"libvlc.dll");
        if (pathExists(candidate)) return candidate;
    }
    return std::wstring();
}

} // namespace

// =============================================================================
//  DVD 検出
// =============================================================================
DvdInfo detectDvd(const std::wstring& rawPath) {
    DvdInfo info;
    const std::wstring path = trimTrailing(rawPath);
    if (path.empty()) return info;

    const DWORD attr = attributesOf(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return info;

    const std::wstring extension = extensionOf(path);
    const bool directory = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;

    if (!directory && extension == L".ISO") {
        // ISO の中身はマウントしないと読めないため暗号判定はできない。
        // libVLC は ISO をそのまま読めるので、検査不能として VLC へ渡す。
        info.isDvd = true;
        info.isIso = true;
        info.detectable = false;
        info.encrypted = true;
        info.root = path;
        return info;
    }

    std::wstring videoTs;
    if (directory) {
        const std::wstring name = upperCopy(baseNameOf(path));
        if (name == L"VIDEO_TS") {
            videoTs = path;
        } else if (isDirectory(joinPath(path, L"VIDEO_TS"))) {
            videoTs = joinPath(path, L"VIDEO_TS");
        } else {
            return info; // VIDEO_TS を持たない → DVD ではない
        }
    } else if (extension == L".IFO") {
        // VIDEO_TS.IFO / VTS_01_0.IFO
        videoTs = parentOf(path);
    } else if (extension == L".VOB") {
        const std::wstring parent = parentOf(path);
        if (upperCopy(baseNameOf(parent)) == L"VIDEO_TS") {
            videoTs = parent;
        } else if (pathExists(joinPath(parent, L"VIDEO_TS.IFO"))) {
            videoTs = parent;
        } else {
            return info; // 遊びの VOB → 従来どおり FFmpeg が扱う
        }
    } else {
        return info;
    }

    if (videoTs.empty() || !isDirectory(videoTs)) return info;

    info.isDvd = true;
    info.videoTs = videoTs;
    info.root = parentOf(videoTs);
    if (info.root.empty()) info.root = videoTs;

    // VIDEO_TS.IFO が無くても .IFO が 1 つでもあれば DVD とみなす
    std::wstring largestVob;
    info.vobCount = countVobs(videoTs, &largestVob);
    if (info.vobCount == 0 && !pathExists(joinPath(videoTs, L"VIDEO_TS.IFO"))) {
        info.isDvd = false;
        return info;
    }

    // 判定に使う VOB: まず VTS_xx_1.VOB (本編の先頭)、無ければ最大の VOB
    std::wstring probe = largestVob;
    {
        WIN32_FIND_DATAW fd{};
        const std::wstring pattern = joinPath(videoTs, L"VTS_*_1.VOB");
        HANDLE handle = FindFirstFileW(pattern.c_str(), &fd);
        if (handle != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                probe = joinPath(videoTs, fd.cFileName);
                break;
            } while (FindNextFileW(handle, &fd));
            FindClose(handle);
        }
    }

    if (!probe.empty()) {
        info.probeFile = probe;
        bool encrypted = false;
        if (probeVobEncryption(probe, encrypted)) {
            info.detectable = true;
            info.encrypted = encrypted;
        }
    }
    return info;
}

// =============================================================================
//  libvlc.dll の探索
// =============================================================================
std::wstring findLibvlc() {
    const wchar_t* subkeys[] = {L"SOFTWARE\\VideoLAN\\VLC",
                                L"SOFTWARE\\WOW6432Node\\VideoLAN\\VLC"};
    for (const wchar_t* subkey : subkeys) {
        std::wstring candidate = queryInstallDir(HKEY_LOCAL_MACHINE, subkey);
        if (!candidate.empty()) return candidate;
        candidate = queryInstallDir(HKEY_CURRENT_USER, subkey);
        if (!candidate.empty()) return candidate;
    }

    const wchar_t* envVars[] = {L"ProgramFiles", L"ProgramFiles(x86)"};
    for (const wchar_t* envVar : envVars) {
        wchar_t buffer[1024] = {};
        const DWORD length = GetEnvironmentVariableW(envVar, buffer, 1024);
        if (length == 0 || length >= 1024) continue;
        const std::wstring candidate =
            joinPath(joinPath(buffer, L"VideoLAN\\VLC"), L"libvlc.dll");
        if (pathExists(candidate)) return candidate;
    }
    return std::wstring();
}

// =============================================================================
//  Video (libVLC バックエンド)
// =============================================================================
namespace {

// ---- libVLC API (いずれも libvlc.dll から動的解決) ----
using NewFn = void* (*)(int, const char* const*);
using ReleaseFn = void (*)(void*);
using GetVersionFn = const char* (*)();
using NewPathFn = void* (*)(void*, const char*);
using NewPlayerFn = void* (*)(void*);
using SetMediaFn = void (*)(void*, void*);
using PlayFn = int (*)(void*);
using SetPauseFn = int (*)(void*, int);
using IsPlayingFn = int (*)(void*);
using StopFn = void (*)(void*);
using SetTimeFn = void (*)(void*, long long);
using GetTimeFn = long long (*)(void*);
using GetLengthFn = long long (*)(void*);
using SetVolumeFn = int (*)(void*, int);
using ErrmsgFn = const char* (*)();
using SetFormatCallbacksFn = void (*)(void*, unsigned (*)(void**, char*, unsigned*, unsigned*,
                                                         unsigned*, unsigned*),
                                      void (*)(void*));
using SetCallbacksFn = void (*)(void*, void* (*)(void**, void**),
                                void (*)(void*, void* const*, const unsigned*),
                                void (*)(void*, void*), void*);

struct Api {
    NewFn createInstance = nullptr;
    ReleaseFn releaseInstance = nullptr;
    GetVersionFn getVersion = nullptr;
    NewPathFn newMediaPath = nullptr;
    ReleaseFn releaseMedia = nullptr;
    NewPlayerFn newPlayer = nullptr;
    ReleaseFn releasePlayer = nullptr;
    SetMediaFn setMedia = nullptr;
    PlayFn play = nullptr;
    SetPauseFn setPause = nullptr;
    IsPlayingFn isPlaying = nullptr;
    StopFn stop = nullptr;
    SetTimeFn setTime = nullptr;
    GetTimeFn getTime = nullptr;
    GetLengthFn getLength = nullptr;
    SetVolumeFn setVolume = nullptr;
    ErrmsgFn errmsg = nullptr;
    SetFormatCallbacksFn setFormatCallbacks = nullptr;
    SetCallbacksFn setCallbacks = nullptr;
};

Api g_api;

} // namespace

Video::~Video() {
    close();
    if (m_lib != nullptr) {
        FreeLibrary(m_lib);
        m_lib = nullptr;
    }
}

bool Video::load(std::string* error) {
    if (m_lib != nullptr) return true;

    const std::wstring path = findLibvlc();
    if (path.empty()) {
        if (error) *error = "libvlc.dll not found (VLC is not installed)";
        return false;
    }

    // libvlccore.dll と plugins\ を libvlc.dll の所在を基準に解決させる
    m_lib = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m_lib == nullptr) {
        if (error)
            *error = "failed to load libvlc.dll (GetLastError=" +
                     std::to_string(GetLastError()) + ")";
        return false;
    }

    std::string missing;
    auto resolve = [&missing](HMODULE module, const char* name, auto& fn) {
        fn = reinterpret_cast<std::decay_t<decltype(fn)>>(GetProcAddress(module, name));
        if (fn == nullptr && missing.empty()) missing = name;
    };
    resolve(m_lib, "libvlc_new", g_api.createInstance);
    resolve(m_lib, "libvlc_release", g_api.releaseInstance);
    resolve(m_lib, "libvlc_get_version", g_api.getVersion);
    resolve(m_lib, "libvlc_media_new_path", g_api.newMediaPath);
    resolve(m_lib, "libvlc_media_release", g_api.releaseMedia);
    resolve(m_lib, "libvlc_media_player_new", g_api.newPlayer);
    resolve(m_lib, "libvlc_media_player_release", g_api.releasePlayer);
    resolve(m_lib, "libvlc_media_player_set_media", g_api.setMedia);
    resolve(m_lib, "libvlc_media_player_play", g_api.play);
    resolve(m_lib, "libvlc_media_player_set_pause", g_api.setPause);
    resolve(m_lib, "libvlc_media_player_is_playing", g_api.isPlaying);
    resolve(m_lib, "libvlc_media_player_stop", g_api.stop);
    resolve(m_lib, "libvlc_media_player_set_time", g_api.setTime);
    resolve(m_lib, "libvlc_media_player_get_time", g_api.getTime);
    resolve(m_lib, "libvlc_media_player_get_length", g_api.getLength);
    resolve(m_lib, "libvlc_audio_set_volume", g_api.setVolume);
    resolve(m_lib, "libvlc_video_set_format_callbacks", g_api.setFormatCallbacks);
    resolve(m_lib, "libvlc_video_set_callbacks", g_api.setCallbacks);
    resolve(m_lib, "libvlc_errmsg", g_api.errmsg);

    if (!missing.empty()) {
        if (error) *error = "required symbol missing in libvlc.dll: " + missing;
        FreeLibrary(m_lib);
        m_lib = nullptr;
        return false;
    }

    log::info("VLC: libvlc.dll loaded  '%s'  version=%s", wideToUtf8(path).c_str(),
              version().c_str());
    return true;
}

std::string Video::version() const {
    if (m_lib == nullptr || g_api.getVersion == nullptr) return std::string();
    const char* text = g_api.getVersion();
    return text ? std::string(text) : std::string();
}

bool Video::open(const std::wstring& target) {
    close();
    if (!load(nullptr)) return false;
    if (target.empty()) return false;

    const char* args[] = {"--no-video-title-show"};
    m_instance = g_api.createInstance(1, args);
    if (m_instance == nullptr) {
        log::info("VLC: libvlc_new failed");
        return false;
    }
    g_active = this; // video callback はここを参照する

    const std::string utf8 = wideToUtf8(target);
    m_media = g_api.newMediaPath(m_instance, utf8.c_str());
    if (m_media == nullptr) {
        log::info("VLC: media_new_path failed  '%s'", utf8.c_str());
        close();
        return false;
    }

    m_player = g_api.newPlayer(m_instance);
    if (m_player == nullptr) {
        log::info("VLC: media_player_new failed");
        close();
        return false;
    }

    g_api.setMedia(m_player, m_media);
    g_api.setFormatCallbacks(m_player, &Video::formatSetupCb, &Video::formatCleanupCb);
    g_api.setCallbacks(m_player, &Video::lockCb, &Video::unlockCb, &Video::displayCb, this);
    log::info("VLC: media attached  '%s'", utf8.c_str());
    return true;
}

void Video::close() {
    if (m_player != nullptr) {
        // stop は入力スレッドを待つ (遅いことがある) が、解放すると自動で止まる
        g_api.releasePlayer(m_player);
        m_player = nullptr;
    }
    if (m_media != nullptr) {
        g_api.releaseMedia(m_media);
        m_media = nullptr;
    }
    if (m_instance != nullptr) {
        g_api.releaseInstance(m_instance);
        m_instance = nullptr;
    }
    if (g_active == this) g_active = nullptr; // ここで初めて callback を無効化

    std::lock_guard<std::mutex> lock(m_mutex);
    m_buffers[0].clear();
    m_buffers[1].clear();
    m_frameW = m_frameH = m_pitch = m_lines = 0;
    m_newFrame = false;
    m_error = false;
    m_readIndex = 0;
    m_writeIndex = 1;
    m_texture.Reset();
    m_texW = m_texH = 0;
}

void Video::play() {
    if (m_player == nullptr) return;
    const int rc = g_api.play(m_player);
    m_error = (rc != 0);
    if (m_error) {
        log::info("VLC: play failed rc=%d  %s", rc,
                  g_api.errmsg ? (g_api.errmsg() ? g_api.errmsg() : "") : "");
    }
}

void Video::setPaused(bool paused) {
    if (m_player != nullptr) g_api.setPause(m_player, paused ? 1 : 0);
}

void Video::seekMs(long long ms) {
    if (m_player == nullptr) return;
    if (ms < 0) ms = 0;
    g_api.setTime(m_player, ms);
}

void Video::setVolumePercent(int percent) {
    if (m_player == nullptr) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100; // libVLC の上限 (ブーストはエンジン側のみ)
    g_api.setVolume(m_player, percent);
}

bool Video::playing() const {
    return m_player != nullptr && g_api.isPlaying(m_player) != 0;
}

long long Video::timeMs() const {
    return (m_player != nullptr) ? g_api.getTime(m_player) : 0;
}

long long Video::durationMs() const {
    return (m_player != nullptr) ? g_api.getLength(m_player) : 0;
}

// -----------------------------------------------------------------------------
//  映像 callback
// -----------------------------------------------------------------------------
void Video::allocateBuffersLocked() {
    const std::size_t bytes =
        static_cast<std::size_t>(m_pitch) * static_cast<std::size_t>(m_lines);
    if (bytes == 0) return;
    if (m_buffers[0].size() != bytes || m_buffers[1].size() != bytes) {
        m_buffers[0].assign(bytes, 0);
        m_buffers[1].assign(bytes, 0);
        m_newFrame = false;
    }
}

unsigned Video::formatSetupCb(void** opaque, char* chroma, unsigned* width, unsigned* height,
                              unsigned* pitches, unsigned* lines) {
    Video* self = g_active;
    if (self == nullptr || chroma == nullptr || width == nullptr || height == nullptr ||
        pitches == nullptr || lines == nullptr) {
        return 0;
    }
    if (*width == 0 || *height == 0) return 0;
    if (opaque != nullptr) *opaque = self;

    // 32bit BGRA を要求する (レンダラのソフトウェア経路でそのまま使える)
    chroma[0] = 'B';
    chroma[1] = 'G';
    chroma[2] = 'R';
    chroma[3] = 'A';
    const unsigned pitch = ((*width) * 4u + 15u) & ~15u;

    std::lock_guard<std::mutex> lock(self->m_mutex);
    self->m_frameW = *width;
    self->m_frameH = *height;
    self->m_pitch = pitch;
    self->m_lines = *height;
    self->allocateBuffersLocked();
    pitches[0] = pitch;
    lines[0] = *height;
    return 1;
}

void Video::formatCleanupCb(void* opaque) {
    (void)opaque;
    // バッファは close() / 次の formatSetup で作り直す
}

void* Video::lockCb(void** opaque, void** planes) {
    (void)opaque;
    Video* self = g_active;
    if (self == nullptr || planes == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(self->m_mutex);
    std::vector<unsigned char>& buffer = self->m_buffers[self->m_writeIndex];
    if (buffer.empty()) {
        *planes = nullptr;
        return nullptr;
    }
    *planes = buffer.data();
    return buffer.data();
}

void Video::unlockCb(void* opaque, void* const* planes, const unsigned* sizes) {
    (void)opaque;
    (void)planes;
    (void)sizes;
}

void Video::displayCb(void* opaque, void* picture) {
    (void)opaque;
    (void)picture;
    Video* self = g_active;
    if (self == nullptr) return;
    std::lock_guard<std::mutex> lock(self->m_mutex);
    // 書き込み済みバッファを公開側へ切り替える (2 枚板を毎フレーム交換)
    const int published = self->m_writeIndex;
    self->m_writeIndex = self->m_readIndex;
    self->m_readIndex = published;
    self->m_newFrame = true;
}

VideoDecoder::FrameView Video::present(ID3D11Device* device, ID3D11DeviceContext* context,
                                       double timeSec) {
    VideoDecoder::FrameView view;
    if (device == nullptr) return view;

    UINT width = 0;
    UINT height = 0;
    UINT contentW = 0;
    UINT contentH = 0;
    bool created = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frameW == 0 || m_frameH == 0 || m_buffers[m_readIndex].empty() ||
            m_pitch == 0) {
            return view;
        }
        if (!m_texture || m_texW != m_frameW || m_texH != m_frameH) {
            m_texture.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = m_frameW;
            desc.Height = m_frameH;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_texture))) {
                m_texture.Reset();
                return view;
            }
            m_texW = m_frameW;
            m_texH = m_frameH;
            created = true;
        }
        if ((m_newFrame || created) && context) {
            const D3D11_BOX box{0, 0, 0, m_frameW, m_frameH, 1};
            context->UpdateSubresource(m_texture.Get(), 0, &box, m_buffers[m_readIndex].data(),
                                       m_pitch, 0);
            m_newFrame = false;
        }
        width = m_texW;
        height = m_texH;
        contentW = m_frameW;
        contentH = m_frameH;
    }
    if (!m_texture) return view;

    view.texture = m_texture.Get();
    view.arrayIndex = 0;
    view.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    view.width = contentW;
    view.height = contentH;
    view.textureWidth = width;
    view.textureHeight = height;
    view.tenBit = false;
    view.limitedRange = false; // BGRA は既に RGB 変換済み
    view.pts = timeSec;
    return view;
}

VideoDecoder::Info Video::mediaInfo() const {
    VideoDecoder::Info info;
    info.duration = static_cast<double>(durationMs()) / 1000.0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        info.width = m_frameW;
        info.height = m_frameH;
    }
    info.fps = 0.0;
    info.hardwareDecoding = false;
    info.videoStreamIndex = 0;
    info.videoTrackCount = 1;
    info.pixelFormatBits = 8;
    return info;
}

} // namespace vlc
} // namespace nanoalpha
