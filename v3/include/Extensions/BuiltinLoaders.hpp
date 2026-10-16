#pragma once
#ifndef NANOALPHA_EXTENSIONS_BUILTIN_LOADERS_HPP
#define NANOALPHA_EXTENSIONS_BUILTIN_LOADERS_HPP

#include "Extensions/ExtensionManager.hpp"

#include <string>
#include <vector>

namespace nanoalpha::extensions {

// ExtensionId::Disc の instance として ReadyHook へ渡る探索結果。
// Phase 2 で DiscPlayer (CD-DA 32bit アップコンバート / libdvdnav メニュー) へ発展させる。
struct DiscProbe {
    std::vector<std::wstring> roots; // "D:\\" 形式の光学ドライブ
};

// Phase 2 拡張ローダを ExtensionManager へ登録する。
// 各ローダは 3 秒予算内で完結し、LoadContext::stop / deadline を監視すること。
void registerBuiltinLoaders(ExtensionManager& manager);

// 1 個だけ登録する (v3)。登録できたら true。
//  - 起動時に設定で無効の拡張を除外したい場合
//  - 起動後に有効化された拡張を後から登録し直したい場合 に使う。
bool registerBuiltinLoader(ExtensionManager& manager, ExtensionId id);

} // namespace nanoalpha::extensions

#endif // NANOALPHA_EXTENSIONS_BUILTIN_LOADERS_HPP
