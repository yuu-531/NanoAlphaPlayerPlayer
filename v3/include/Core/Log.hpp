#pragma once
#ifndef NANOALPHA_CORE_LOG_HPP
#define NANOALPHA_CORE_LOG_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <mutex>

#ifndef NANOALPHA_CONSOLE
#define NANOALPHA_CONSOLE 0
#endif

// フレームループを止めない軽量ログ。
// 出力先: DebuggerView / Visual Studio 出力ウィンドウ (+ NANOALPHA_CONSOLE=1 時は stdout)
namespace nanoalpha::log {

inline void write(const char* text) noexcept {
    if (!text) return;

    // 起動からの経過秒 (ストール / デッドロック位置の特定に使う)
    static const auto startedAt = std::chrono::steady_clock::now();
    static std::mutex outputMutex;

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startedAt).count();
    char line[1160];
    std::snprintf(line, sizeof(line), "[%9.3fs] %s", elapsed, text);

    // DebuggerView / VS 出力ウィンドウ向けには UTF-16 で出す (日本語が文字化けしない)
    wchar_t wide[1200];
    const int wideCapacity = static_cast<int>(sizeof(wide) / sizeof(wide[0]));
    const int length = MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, wideCapacity);

    const std::lock_guard<std::mutex> guard(outputMutex);
    if (length > 0) {
        OutputDebugStringW(L"[NanoAlphaPlayer] ");
        OutputDebugStringW(wide);
        OutputDebugStringW(L"\n");
    } else {
        OutputDebugStringA("[NanoAlphaPlayer] ");
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
    }

#if NANOALPHA_CONSOLE
    std::fputs("[NanoAlphaPlayer] ", stdout);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout); // リダイレクト先でも欠落しないよう毎回フラッシュ
#endif
}

inline void info(const char* fmt, ...) noexcept {
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    write(buffer);
}

} // namespace nanoalpha::log

#endif // NANOALPHA_CORE_LOG_HPP
