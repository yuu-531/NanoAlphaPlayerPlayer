#pragma once
#ifndef NANOALPHA_CORE_SETTINGS_HPP
#define NANOALPHA_CORE_SETTINGS_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// =============================================================================
//  設定の永続化 (v3)
// -----------------------------------------------------------------------------
//  Dear ImGui の imgui.ini を出さないために io.IniFilename = nullptr とする
//  ため、UI 状態・拡張の ON/OFF・画質プリセットは本ファイルへ保存する。
//
//    場所 : %APPDATA%\NanoAlphaPlayer\settings.ini
//    形式 : ASCII の key=value (1 行 1 項目、# はコメント)
//
//  保存は UI 操作の直後 (値が変わった時) にのみ書き込み、毎フレーム書き込みはしない。
// =============================================================================
namespace nanoalpha::settings {

struct Values {
    // ---- 拡張の ON/OFF (v3: 基本機能以外を切り替え可能にする) ----
    // false の拡張は起動時にローダ自体を登録しない (完全に読み込まれない)。
    //  OpenRGB は通常使うものではないため既定で無効。拡張タブで有効化できる。
    bool extRgb = false;       // OpenRGB 照明連携 (既定: 無効)
    bool extVlc = true;        // VLC / libVLC フィルタ群
    bool extFrameGen = true;   // フレーム生成 (Phase 2)
    bool extDisc = true;       // CD-DVD ドライブ
    bool extNetwork = true;    // ネットワーク配信 / ストリーミング

    // ---- 画質・表示 ----
    int scaleFilter = 0;       // PlayerController::ScaleFilter と同じ数値 (0=Linear)
    int frameGenFactor = 4;    // 2 / 4 / 8 / 16
    int upscaler = 0;          // 0=無効 / 1=FSR1 (AMD 超解像度) / 2=自動 (NVIDIA 検出 → FSR1)

    // ---- 再生 ----
    int repeatMode = 0;        // PlayerController::RepeatMode (0=オフ / 1=全曲 / 2=1曲)
};

inline Values g_values{};
inline bool g_loaded = false;

// ---- パス ------------------------------------------------------------------
inline std::wstring filePath() {
    wchar_t base[1024]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", base, 1024);
    if (length == 0 || length >= 1024) return std::wstring();

    std::wstring directory(base);
    directory += L"\\NanoAlphaPlayer";
    CreateDirectoryW(directory.c_str(), nullptr);
    directory += L"\\settings.ini";
    return directory;
}

inline bool parseInt(const char* text, int& out) {
    if (!text || !*text) return false;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text) return false;
    out = static_cast<int>(value);
    return true;
}

inline bool parseBool(const char* text, bool& out) {
    if (!text) return false;
    if (std::strcmp(text, "1") == 0 || std::strcmp(text, "true") == 0) {
        out = true;
        return true;
    }
    if (std::strcmp(text, "0") == 0 || std::strcmp(text, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

// 値の妥当性保証 (破損した ini でも安全側に落ちる)
inline void clampValues() {
    Values& v = g_values;
    if (v.scaleFilter < 0 || v.scaleFilter > 3) v.scaleFilter = 0;
    if (v.frameGenFactor != 2 && v.frameGenFactor != 4 && v.frameGenFactor != 8 &&
        v.frameGenFactor != 16)
        v.frameGenFactor = 4;
    if (v.upscaler < 0 || v.upscaler > 2) v.upscaler = 0;
    if (v.repeatMode < 0 || v.repeatMode > 2) v.repeatMode = 0;
}

// ---- 読み込み (起動時に 1 回) ----------------------------------------------
inline bool load() {
    g_loaded = false;
    const std::wstring path = filePath();
    if (path.empty()) return false;

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return false;

    char line[256];
    while (std::fgets(line, sizeof(line), file)) {
        // 行の末尾を除去
        line[strcspn(line, "\r\n")] = '\0';

        char* cursor = line;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == '\0' || *cursor == '#') continue;

        char* eq = std::strchr(cursor, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = cursor;
        const char* value = eq + 1;
        // キー右・値左の空白を除去
        char* keyEnd = eq;
        while (keyEnd > cursor && (keyEnd[-1] == ' ' || keyEnd[-1] == '\t')) *--keyEnd = '\0';
        while (*value == ' ' || *value == '\t') ++value;

        Values& v = g_values;
        if (std::strcmp(key, "ext.rgb") == 0) parseBool(value, v.extRgb);
        else if (std::strcmp(key, "ext.vlc") == 0) parseBool(value, v.extVlc);
        else if (std::strcmp(key, "ext.framegen") == 0) parseBool(value, v.extFrameGen);
        else if (std::strcmp(key, "ext.disc") == 0) parseBool(value, v.extDisc);
        else if (std::strcmp(key, "ext.network") == 0) parseBool(value, v.extNetwork);
        else if (std::strcmp(key, "scale.filter") == 0) parseInt(value, v.scaleFilter);
        else if (std::strcmp(key, "framegen.factor") == 0) parseInt(value, v.frameGenFactor);
        else if (std::strcmp(key, "upscaler.mode") == 0) parseInt(value, v.upscaler);
        else if (std::strcmp(key, "repeat.mode") == 0) parseInt(value, v.repeatMode);
    }
    std::fclose(file);

    clampValues();
    g_loaded = true;
    return true;
}

// ---- 書き込み (値が変わった時) --------------------------------------------
inline bool save() {
    const std::wstring path = filePath();
    if (path.empty()) return false;

    clampValues();
    const Values& v = g_values;

    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || !file) return false;

    std::fprintf(file, "# NanoAlphaPlayer settings (ASCII key=value)\n");
    std::fprintf(file, "ext.rgb=%d\n", v.extRgb ? 1 : 0);
    std::fprintf(file, "ext.vlc=%d\n", v.extVlc ? 1 : 0);
    std::fprintf(file, "ext.framegen=%d\n", v.extFrameGen ? 1 : 0);
    std::fprintf(file, "ext.disc=%d\n", v.extDisc ? 1 : 0);
    std::fprintf(file, "ext.network=%d\n", v.extNetwork ? 1 : 0);
    std::fprintf(file, "scale.filter=%d\n", v.scaleFilter);
    std::fprintf(file, "framegen.factor=%d\n", v.frameGenFactor);
    std::fprintf(file, "upscaler.mode=%d\n", v.upscaler);
    std::fprintf(file, "repeat.mode=%d\n", v.repeatMode);
    std::fclose(file);
    return true;
}

} // namespace nanoalpha::settings

#endif // NANOALPHA_CORE_SETTINGS_HPP
