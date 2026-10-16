// =============================================================================
//  クラッシュレポータ (src/Core/CrashReporter.cpp)
// -----------------------------------------------------------------------------
//  ヒープ破壊 (0xC0000374) / 不正アクセス (0xC0000005) を VEH で受け取り、
//  プロセス内に残る全スレッドのスタックをログへ出す。
//
//  [最重要] 0xC0000374 が一度でも発火すると、その瞬間から**プロセスのヒープは
//  使用不可**になる (以降のあらゆる malloc が同じ例外を投げる)。
//  よって発火後のシンボル化に dbghelp (内部で malloc する) を使うと途端に死ぬ。
//  そこで:
//   ・起動時 (ヒープ健全) に SymEnumSymbols で自プロセスの関数名テーブルを
//     静的領域へ作り上げておく → 発火後は二分探索だけ (ヒープ不使用)。
//   ・他モジュール (FFmpeg / ucrtbase / ntdll) は PE エクスポート表を直接辿る
//     (これもヒープ不使用)。
//   ・アンワインドは ntdll!RtlVirtualUnwind。
//   ・他スレッドは Suspend → GetThreadContext → 即 Resume (停止したまま他所を
//     呼ぶとヒープロックで死束しうる)。
//   ・出力は自前 fputs (log::write の mutex は掴めない可能性がある)。
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/CrashReporter.hpp"

#include <dbghelp.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ntdll.lib")

// RtlLookupFunctionEntry / RtlVirtualUnwind は winnt.h (_AMD64_) で宣言済み

namespace nanoalpha::crash {
namespace {

constexpr int kMaxFrames = 40;
constexpr int kPeerFrames = 16;
constexpr int kMaxThreads = 32;
constexpr int kMaxSymbols = 16384;
constexpr int kNameChars = 96;

std::atomic<bool> g_installed{false};
std::atomic<int> g_handling{0};
bool g_symbolsReady = false;

// --- 起動時に作る自プロセスのシンボル索引 (静的領域・発火後は読み取りのみ) ---
struct SymbolEntry {
    DWORD64 address = 0;
    char name[kNameChars] = {};
};
SymbolEntry g_symbols[kMaxSymbols];
int g_symbolCount = 0;

struct ThreadSnap {
    DWORD tid = 0;
    bool valid = false;
    CONTEXT ctx{};
    DWORD64 pcs[kMaxFrames] = {};
    int count = 0;
};
ThreadSnap g_snaps[kMaxThreads];
int g_snapCount = 0;

// ---------------------------------------------------------------- ロックなし出力
void emit(const char* text) {
    if (!text) return;
    OutputDebugStringA("[NanoAlphaPlayer][crash] ");
    OutputDebugStringA(text);
    OutputDebugStringA("\r\n");
    std::fputs("[NanoAlphaPlayer][crash] ", stdout);
    std::fputs(text, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void emitf(const char* fmt, ...) {
    char buffer[768];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    emit(buffer);
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case 0xC0000005: return "STATUS_ACCESS_VIOLATION";
    case 0xC000008C: return "STATUS_ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008D: return "STATUS_FLOAT_DENORMAL_OPERAND";
    case 0xC000008E: return "STATUS_FLOAT_INEXACT_RESULT";
    case 0xC000008F: return "STATUS_FLOAT_INVALID_OPERATION";
    case 0xC0000090: return "STATUS_FLOAT_OVERFLOW";
    case 0xC0000091: return "STATUS_FLOAT_UNDERFLOW";
    case 0xC0000092: return "STATUS_INTEGER_OVERFLOW";
    case 0xC0000093: return "STATUS_PRIVILEGED_INSTRUCTION";
    case 0xC0000094: return "STATUS_INTEGER_DIVIDE_BY_ZERO";
    case 0xC00000FD: return "STATUS_STACK_OVERFLOW";
    case 0xC00001A5: return "STATUS_DATATYPE_MISALIGNMENT";
    case 0xC00002B4: return "STATUS_FLOAT_DIVIDE_BY_ZERO";
    case 0xC0000374: return "STATUS_HEAP_CORRUPTION";
    case 0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN / FAIL_FAST";
    case 0xC0000417: return "STATUS_INVALID_CRUNTIME_PARAMETER";
    case 0xE06D7363: return "C++ EXCEPTION (unhandled)";
    default: return "STATUS_UNKNOWN";
    }
}

// 呼び出してよい (握っている鎖がない) 破壊的例外だけを選ぶ。
// 0x40010006 (OutputDebugString) や C++ 例外は必ず除外すること。
bool isFatal(DWORD code) {
    switch (code) {
    case 0xC0000005: // AV
    case 0xC000008C: // 配列境界
    case 0xC0000094: // 0 除算
    case 0xC00000FD: // スタック溢れ
    case 0xC00001A5: // 非整合
    case 0xC00002B4: // float 0 除算
    case 0xC0000374: // ヒープ破壊 ★
    case 0xC0000409: // /GS 失敗 / fail fast
    case 0xC0000417: // CRT パラメータ不正
        return true;
    default: return false;
    }
}

// ---------------------------------------------------------------- スタック展開
// ヒープを使わない (ntdll のみ)。壊れたフレームで AV しないよう __try で包む。
int unwind(const CONTEXT& start, DWORD64* pcs, int maxFrames) {
    int count = 0;
    __try {
        CONTEXT ctx = start;
        DWORD64 lastRsp = start.Rsp;
        for (int i = 0; i < maxFrames; ++i) {
            const DWORD64 pc = ctx.Rip;
            if (pc == 0) break;

            // 読めないスタックを辿らない (ガードページ / 未コミット)
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(ctx.Rsp), &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State != MEM_COMMIT) break;
            if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) break;

            HMODULE module = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCSTR>(pc), &module)) {
                break; // モジュール外 = 展開破損
            }

            pcs[count++] = pc;
            if (count >= maxFrames) break;

            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            if (fn != nullptr) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, fn, &ctx, &handlerData,
                                 &establisher, nullptr);
            } else {
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += sizeof(DWORD64);
            }

            if (ctx.Rip == 0) break;
            if (ctx.Rsp <= lastRsp) break; // スタックが減った = 展開破損
            lastRsp = ctx.Rsp;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return count;
    }
    return count;
}

// ------------------------------------------------------- 他スレッドの文脈採取
// ここでは印刷しない (stdout のロックを他スレッドが持つ可能性があるため)。
void captureThreads() {
    g_snapCount = 0;
    const DWORD self = GetCurrentThreadId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == self) {
                continue;
            }
            if (g_snapCount >= kMaxThreads) break;

            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                           THREAD_QUERY_INFORMATION,
                                       FALSE, entry.th32ThreadID);
            if (thread == nullptr) continue;

            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_FULL;
                const BOOL ok = GetThreadContext(thread, &context);
                ResumeThread(thread); // 停止時間は GetThreadContext の数マイクロ秒のみ
                if (ok) {
                    ThreadSnap& snap = g_snaps[g_snapCount++];
                    snap.tid = entry.th32ThreadID;
                    snap.ctx = context;
                    snap.valid = true;
                    snap.count = 0;
                }
            }
            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
}

// ------------------------------------------------- ヒープを使わない名前解決
const char* baseName(const char* path) {
    if (path == nullptr) return "?";
    const char* slash = std::strrchr(path, '\\');
    const char* forward = std::strrchr(path, '/');
    if (forward != nullptr && (slash == nullptr || forward > slash)) slash = forward;
    return slash != nullptr ? slash + 1 : path;
}

HMODULE moduleOf(DWORD64 address) {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(address), &module);
    return module;
}

// 自プロセス (NanoAlphaPlayer.exe) の関数名索引を二分探索で引く。
const char* localSymbol(DWORD64 address, DWORD64* displacement) {
    if (g_symbolCount <= 0) return nullptr;
    int lo = 0;
    int hi = g_symbolCount - 1;
    int best = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (g_symbols[mid].address <= address) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    if (best < 0) return nullptr;
    // 1 メガバイト以上離れているなら無関係のシンボル
    const DWORD64 delta = address - g_symbols[best].address;
    if (delta > 0x100000) return nullptr;
    if (displacement != nullptr) *displacement = delta;
    return g_symbols[best].name;
}

// PE のエクスポート表を直接辿る (ntdll / ucrtbase / FFmpeg のエクスポート用)。
// malloc を一切使わない。
const char* exportSymbol(HMODULE module, DWORD64 address, DWORD64* displacement) {
    if (module == nullptr) return nullptr;
    auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (dir.VirtualAddress == 0 || dir.Size == 0) return nullptr;

    const DWORD rva = static_cast<DWORD>(address - reinterpret_cast<DWORD64>(module));
    const auto* exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + exp->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + exp->AddressOfNames);
    const auto* ordinals = reinterpret_cast<const WORD*>(base + exp->AddressOfNameOrdinals);

    // AddressOfFunctions は昇順とは約束されていない → 線形探索で最寄りを選ぶ
    int best = -1;
    for (DWORD i = 0; i < exp->NumberOfFunctions; ++i) {
        if (functions[i] == 0) continue;
        if (functions[i] <= rva) {
            if (best < 0 || functions[i] > functions[best]) best = static_cast<int>(i);
        }
    }
    if (best < 0) return nullptr;
    const DWORD funcRva = functions[best];
    // 転送先 (string) は無視
    if (funcRva >= dir.VirtualAddress && funcRva < dir.VirtualAddress + dir.Size) return nullptr;
    if (rva - funcRva > 0x40000) return nullptr; // 遠すぎる = 別関数

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        if (static_cast<int>(ordinals[i]) != best) continue;
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (displacement != nullptr) *displacement = rva - funcRva;
        return name;
    }
    return nullptr;
}

// ---------------------------------------------------------------- 組み立て
void printFrames(const char* tag, DWORD tid, const DWORD64* pcs, int count) {
    emitf("--- %s (thread %lu) %d frames ---", tag, static_cast<unsigned long>(tid), count);
    if (count <= 0 || pcs == nullptr) {
        emit("    <no frames>");
        return;
    }

    for (int i = 0; i < count; ++i) {
        const DWORD64 pc = pcs[i];
        char path[MAX_PATH] = "?";
        const HMODULE module = moduleOf(pc);
        if (module == nullptr || GetModuleFileNameA(module, path, MAX_PATH) == 0) path[0] = '?';
        const DWORD64 moduleBase = reinterpret_cast<DWORD64>(module);
        const DWORD64 offset = (pc > moduleBase) ? pc - moduleBase : 0;

        DWORD64 displacement = 0;
        // 自プロセスは起動時索引、それ以外はエクスポート表 (どちらもヒープ不使用)
        const char* symbol = localSymbol(pc, &displacement);
        if (symbol == nullptr) symbol = exportSymbol(module, pc, &displacement);

        if (symbol != nullptr) {
            emitf("  #%02d %016llX  %s+0x%llx  %s+0x%llx", i, pc, baseName(path), offset, symbol,
                  displacement);
        } else {
            emitf("  #%02d %016llX  %s+0x%llx", i, pc, baseName(path), offset);
        }
    }
}

// ---------------------------------------------------------------- 本体
void dump(const EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    emitf("=== FATAL EXCEPTION 0x%08lX (%s)  address=0x%p  thread=%lu ===",
          static_cast<unsigned long>(code), exceptionName(code),
          info->ExceptionRecord->ExceptionAddress,
          static_cast<unsigned long>(GetCurrentThreadId()));

    if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2) {
        emitf("  access: %s  target=0x%p",
              info->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" : "READ",
              reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    if (info->ContextRecord != nullptr) {
        emitf("  rip=0x%llx  rsp=0x%llx  rbp=0x%llx", info->ContextRecord->Rip,
              info->ContextRecord->Rsp, info->ContextRecord->Rbp);
    }

    // 1) 他スレッドの文脈だけ採取して即再開 (以降で他スレッドは止まっていない)
    captureThreads();

    // 2) アンワインド (ヒープ不使用)
    DWORD64 faultPcs[kMaxFrames] = {};
    int faultCount = 0;
    if (info->ContextRecord != nullptr) {
        faultCount = unwind(*info->ContextRecord, faultPcs, kMaxFrames);
    }
    for (int i = 0; i < g_snapCount; ++i) {
        g_snaps[i].count = unwind(g_snaps[i].ctx, g_snaps[i].pcs, kMaxFrames);
    }

    // 3) 名前解決して出力 (ここもヒープ不使用 = 破壊後でも必ず通る)
    printFrames("faulting thread", GetCurrentThreadId(), faultPcs, faultCount);
    for (int i = 0; i < g_snapCount; ++i) {
        if (!g_snaps[i].valid) continue;
        printFrames("peer thread", g_snaps[i].tid, g_snaps[i].pcs,
                    g_snaps[i].count > kPeerFrames ? kPeerFrames : g_snaps[i].count);
    }
    emit("=== end of crash report ===");
}

// まず観測する例外 (VEH / 未捕捉フィルタ)
LONG WINAPI observe(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr || info->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const int previous = g_handling.fetch_add(1, std::memory_order_acq_rel);
    if (previous > 0) {
        // ハンドラ内部で再び例外 → 外側の __except に委ねる
        return EXCEPTION_CONTINUE_SEARCH;
    }

    __try {
        dump(info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        emit("crash reporter: dump 途中で例外 (中断)");
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

// VEH: 破壊的例外のみ観測し、報告後に即終了 (プロセスはもう信用できない)
LONG WINAPI onFatalException(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    if (!isFatal(info->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;

    const DWORD code = info->ExceptionRecord->ExceptionCode;
    observe(info);
    TerminateProcess(GetCurrentProcess(), code);
    Sleep(INFINITE);
    return EXCEPTION_CONTINUE_SEARCH;
}

// 未捕捉例外 (C++ 例外の未処理など): 報告のみ (WER には従来どおり記録させる)
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    emitf("=== UNHANDLED 0x%08lX (%s) ===",
          static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode),
          exceptionName(info->ExceptionRecord->ExceptionCode));
    observe(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------- 起動時: 関数名索引の構築
void copyName(char* dst, size_t dstSize, const char* src) {
    size_t i = 0;
    for (; i + 1 < dstSize && src != nullptr && src[i] != '\0'; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

BOOL CALLBACK onEnumSymbol(PSYMBOL_INFO info, ULONG, PVOID) {
    if (info == nullptr || info->Name == nullptr) return TRUE;
    if (g_symbolCount >= kMaxSymbols) return FALSE;
    SymbolEntry& entry = g_symbols[g_symbolCount++];
    entry.address = info->Address;
    copyName(entry.name, sizeof(entry.name), info->Name);
    return TRUE;
}

void buildSymbolIndex() {
    const HMODULE self = GetModuleHandleW(nullptr);
    const DWORD64 base = reinterpret_cast<DWORD64>(self);
    g_symbolCount = 0;
    // Mask=NULL で全シンボル列挙。失敗したら "*" で再試行。
    if (!SymEnumSymbols(GetCurrentProcess(), base, nullptr, &onEnumSymbol, nullptr)) {
        g_symbolCount = 0;
        SymEnumSymbols(GetCurrentProcess(), base, "*", &onEnumSymbol, nullptr);
    }
    std::sort(g_symbols, g_symbols + g_symbolCount,
              [](const SymbolEntry& a, const SymbolEntry& b) { return a.address < b.address; });
    emitf("crash reporter: symbol index = %d entries", g_symbolCount);
}

} // namespace

void install() noexcept {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) return;

    char enabled[16] = {};
    const DWORD length = GetEnvironmentVariableA("NANOALPHA_CRASH_REPORT", enabled, sizeof(enabled));
    if (length > 0 && enabled[0] == '0') {
        emit("crash reporter: disabled (NANOALPHA_CRASH_REPORT=0)");
        return;
    }

    // WER のダイアログを出さない (ヘッドレス検証のため)
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

    // PDB は exe と同じディレクトリに置かれる (CMake が /DEBUG を付与)
    char directory[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, directory, MAX_PATH);
    char* slash = std::strrchr(directory, '\\');
    if (slash != nullptr) *slash = '\0';

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME |
                  SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);
    g_symbolsReady = SymInitialize(GetCurrentProcess(), directory, TRUE) == TRUE;
    emitf("crash reporter: installed (symbols=%s) pdb-dir=%s", g_symbolsReady ? "on" : "off",
          directory);
    if (g_symbolsReady) buildSymbolIndex();

    AddVectoredExceptionHandler(1, &onFatalException);
    SetUnhandledExceptionFilter(&onUnhandledException);
}

} // namespace nanoalpha::crash
