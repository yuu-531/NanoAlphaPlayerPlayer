#pragma once
#ifndef NANOALPHA_CORE_CRASH_REPORTER_HPP
#define NANOALPHA_CORE_CRASH_REPORTER_HPP

namespace nanoalpha::crash {

// =============================================================================
//  クラッシュレポータ (診断装置)
// -----------------------------------------------------------------------------
//  ヒープ破壊 (0xC0000374) / 不正アクセス (0xC0000005) などの「致命的な例外」を
//  ベクタード例外ハンドラで受け取り、PDB からソース行までシンボル化した
//  スタックトレースをログへ出してからプロセスを終了する。
//
//  WER のみでは「どのコードが破壊したか」が判明しないため、調査用に用意した。
//
//  [実装上の要点]
//   ・他スレッドは (Suspend → GetThreadContext → Resume) で文脈だけ採取し、
//     すぐに再開する。停止したまま dbghelp を呼ぶと CRT ヒープロックで死束する。
//   ・アンワインドは ntdll の RtlVirtualUnwind (ヒープ不使用) で行い、
//     シンボル化は全スレッド再開後に行う。
//   ・ログ出力はロックを使わない (log::write の mutex は掴めない可能性がある)。
// =============================================================================
void install() noexcept;

} // namespace nanoalpha::crash

#endif // NANOALPHA_CORE_CRASH_REPORTER_HPP
