# NanoAlphaPlayer

**VLC 完全上位互換を目指した、Windows 向けオープンソースメディアプレイヤー**

Copyright (C) 2026 [yuu-531](https://github.com/yuu-531)
<yuudayoo.public@gmail.com>
License: [GPL-3.0-or-later](LICENSE)

> **このリポジトリについて** — 本リポジトリは現行版 (**v3** = NanoAlphaPlayer) の
> ソース・同梱ライブラリ (FFmpeg / Dear ImGui / FidelityFX FSR)・インストーラー
> 定義・文書のみを収録しています。旧バージョン (初代 / v2) と検証用メディア
> (`v3/testdata/`) は含みません。ビルドは下記の手順どおり `v3/` をソースに
> 指定してください。

---

## 特長

- **Direct3D 11 描画** / **FFmpeg** による再生（D3D11VA のハードウェアデコード、ゼロコピー）
- **AMD FidelityFX FSR 1.0** によるスケーリング・アップスケーリング
- **フレーム補間（Frame Generation）** — コンテンツのフレームレートから分割数を自動で決め、
  表示リフレッシュレートに合わせてプレゼントレートを追従させる
- **10 バンド EQ**（別ウィンドウ）
- 操作パネルの**ポップアウト**（Ctrl+P / 縁なし窓のため「最小化」ボタンで最小化）、**全画面**（F11 / Esc）、**音量ブースト**（最大 400 %）
- 起動を **100 ms 予算**で設計（シェーダのディスクキャッシュで再起動は約 60 ms）
- **拡張機構**（起動時に無効のものはローダ自体を登録しません）
  - OpenRGB 照明同期（**既定は無効** / 拡張タブで有効化）
  - VLC / libVLC フィルタ群、CD-DVD、ネットワークストリーミング
- **暗号化 DVD** — 通常は FFmpeg で読み込み、暗号を検知したときだけ
  ユーザー環境の `libvlc.dll` をロードして再生します（VLC は同梱しません）
- **ライセンス表示** — `F1` で同梱ライブラリのライセンス本文まで確認できます

## 必要環境

- Windows 10 / 11（x64）
- Visual Studio 2022（MSVC）+ Windows SDK 10
- CMake 3.24 以上
- 依存ライブラリは `v3/third_party/` に同梱済み（追加ダウンロードは不要）

## ビルド

```bat
cmake -S v3 -B v3/build -DCMAKE_BUILD_TYPE=Release
cmake --build v3/build --config Release --parallel
```

成果物は `v3/build/Release/NanoAlphaPlayer.exe` に生成されます。
実行ファイルと同じ場所に FFmpeg の DLL・FSR のヘッダ・ライセンス文書が
自動でコピーされます。

### オプション

| オプション | 既定 | 説明 |
| --- | --- | --- |
| `NANOALPHA_ENABLE_FFMPEG` | ON | FFmpeg（D3D11VA ゼロコピー） |
| `NANOALPHA_ENABLE_IMGUI` | ON | Dear ImGui オーバーレイ |
| `NANOALPHA_ENABLE_FRAMEGEN` | ON | Compute Shader フレーム生成 |
| `NANOALPHA_ENABLE_OPENRGB` | ON | OpenRGB SDK 同期（**実行時の既定は無効**） |
| `NANOALPHA_ENABLE_DISC` | ON | CD-DA / DVD 再生 |
| `NANOALPHA_ENABLE_NETWORK` | ON | ネットワーク配信・ストリーミング |
| `NANOALPHA_CONSOLE` | OFF | ログを stdout に出力する（テスト / デバッグ用） |
| `NANOALPHA_CONSOLE_WINDOW` | ON | コンソールウィンドウ自体を開く |
| `NANOALPHA_WERROR` | OFF | 警告をエラーとして扱う |

`NANOALPHA_CONSOLE=ON` でも `NANOALPHA_CONSOLE_WINDOW=OFF` にすれば、
コンソールウィンドウを出さずにログだけを stdout へ流せます。配布用の
ビルド（`build-pkg`）はこの構成にしてあるので、ユーザーの画面に窓が
出ない一方、パイプで起動したとき（テストやデバッグ）にはログが読めます。

## インストーラー

配布用の MSI は WiX Toolset v5 で作っています。**現在のユーザーへ**
（per-user）入れるので管理者権限（UAC）は不要です。

```bat
powershell -ExecutionPolicy Bypass -File v3\installer\build_installer.ps1
```

成果物は `v3/dist/NanoAlphaPlayer-<version>-x64.msi` です。
`build-pkg` が無い場合はスクリプトが自動で構築します（静的 CRT の
配布用ビルド）。

### インストールされるもの

| 項目 | 内容 |
| --- | --- |
| 配置先 | `%LOCALAPPDATA%\Programs\NanoAlphaPlayer` |
| ファイル | `NanoAlphaPlayer.exe` / FFmpeg の DLL 5 本 / FSR ヘッダー / `LICENSE` / `THIRD_PARTY_LICENSES.md` |
| ショートカット | スタートメニューやデスクトップ（チェックボックスで選択可） |
| 関連付け | 動画 18 種 + 音声 12 種の `OpenWithProgids` と既定プログラム登録 |
| 既定アプリ候補 | `設定 > アプリ > 既定のアプリ` に `NanoAlphaPlayer` が並ぶ |
| アンインストーラー | `設定 > アプリ > アプリと機能` に表示される |

関連付けは「`プログラムから開く` への常時登録」と「既定として割り当てる」
を別機能に分けてあります。**Windows 10 / 11 では既定アプリの最終決定は
必ず利用者本人の操作で行われる**ため、インストール後も最初の起動時は
Windows 側の確認が入ります。

アンインストールではインストールしたファイルと関連付けだけを消去し、
ユーザーデータ（`%APPDATA%\NanoAlphaPlayer` の `settings.ini` /
`shaders.bin`）は残します。

### インストーラーの回帰テスト

```bat
powershell -ExecutionPolicy Bypass -File v3\installer\test_installer.ps1
```

インストール → 関連付け・ショートカット・追加/削除プログラムの確認 →
同梱バイナリの起動（コンソールウィンドウが出ていないこと・ログが stdout に
出ることの検査を含む）→ アンインストール → 完全消去、までを一括で検証します。
成功時は終了コード 0 と `All installer checks passed.` を返します。

### per-user 採用の理由

`per-machine`（Program Files + HKLM）にすると、管理者権限が無い環境では
**エラー 1603** で失敗します。現在のユーザーへ入れる `per-user` なら
UAC が一切不要です。

なお Windows Installer は per-user で入れた製品であっても「プログラムの追加と
削除」のキーを **HKLM 側に書く**ことがあります（配置先とレジストリは HKCU なのに
ARP だけ全ユーザー側に行く）。これだとアンインストール時に UAC が出てしまうため、
MSI 自身のエントリは `ARPSYSTEMCOMPONENT=1` で非表示にし、HKCU に完全版
（名前・アイコン・バージョン・アンインストール文字列）をインストーラーが
直接書く構成にしています。

## 使い方

```bat
v3\build\Release\NanoAlphaPlayer.exe "動画ファイル"
```

URL を起動引数に渡すとネットワークストリーミングとして開きます。

### ショートカット

| キー | 動作 |
| --- | --- |
| `Space` | 一時停止 / 再生 |
| `Ctrl+O` | ファイルを開く |
| `←` / `→` | ±5 秒シーク |
| `↑` / `↓` | 音量 |
| `[` / `]` | 再生速度 |
| `F` | フレーム補間の ON/OFF |
| `L` | リピート切替（オフ / 全曲 / 1 曲） |
| `S` / `A` `B` / `R` | 字幕 / A-B ポイント / 回転 |
| `N` / `P` | プレイリスト 次へ / 前へ |
| `O` | 出力モード |
| `1` – `6` | 上部タブ切替（`6` = EQ 別ウィンドウ） |
| `Ctrl+H` | 操作パネルの表示切替 |
| `Ctrl+P` | 操作パネルを別ウィンドウへポップアウト |
| `F11` / `Esc` | 全画面切替 |
| **`F1`** | **ライセンス表示** |
| `H` | ショートカット一覧をログに出力 |

## リポジトリの構成

```
mediaplayer/
├── v3/                 現行版 = NanoAlphaPlayer（本体・ビルド対象）
│   ├── src/ include/   ソース
│   ├── third_party/    FFmpeg / Dear ImGui / FidelityFX FSR 1.0（同梱）
│   ├── testdata/       検証用メディア
│   └── BUGS.txt        修正履歴・既知の制約
├── src/ include/       初代バージョン（旧 Aegis Engine、参考用）
├── v2/                 第 2 版（参考用）
├── testdata/           検証用メディア
├── README.md           本ファイル
├── LICENSE             GNU GPL version 3
└── THIRD_PARTY_LICENSES.md  同梱ライブラリのライセンス一覧
```

## ライセンス

本プログラムは **[GNU General Public License version 3 以降](LICENSE)**
（GPL-3.0-or-later）で公開します。ソースコードは
<https://github.com/yuu-531> で公開しています。

同梱している FFmpeg / Dear ImGui / FidelityFX FSR などのライセンスは
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) を参照してください。
互換性の確認結果と FFmpeg のソースコード提供についても同じ文書に記載しています。

アプリを起動して `F1` を押すと、同梱ライブラリの一覧と
ライセンス本文の全文をアプリ内で表示できます。

不明な点は `yuudayoo.public@gmail.com` までご連絡ください。
