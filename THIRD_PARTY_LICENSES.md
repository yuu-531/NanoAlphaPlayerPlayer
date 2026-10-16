# サードパーティーライセンス / Third-party licenses

NanoAlphaPlayer（本リポジトリ）は、以下のサードパーティーソフトウェアを利用しています。
本プログラム自体は [GNU General Public License version 3 以降](LICENSE) で公開します。

アプリ内（`F1`、または情報タブの「ライセンス表示」）でも同じ内容と
ライセンス本文の全文を確認できます。実行ファイルと同じ場所には
`LICENSE` / `THIRD_PARTY_LICENSES.md` も同梱されます。

## 一覧

| コンポーネント | バージョン | ライセンス | 利用形態 |
| --- | --- | --- | --- |
| [FFmpeg](https://ffmpeg.org/) | n9.0.2-22-g46d8f462ee | **LGPL-3.0-or-later** | DLL による**動的リンク**（`v3/third_party/ffmpeg`、`bin/` の DLL と CLI も同梱） |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.9 | **MIT** | ソースを同梱して静的リンク（`v3/third_party/imgui`） |
| [AMD FidelityFX FSR 1.0](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | 1.0 | **MIT** | ヘッダを同梱して組み込み（`v3/third_party/ffx-fsr`） |
| [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) | — | **GPL-3.0-or-later**（OpenRGB 本体） | `127.0.0.1:6742` のネットワークプロトコルと互換のクライアントを**独自実装**。OpenRGB 本体のコードは同梱していません。 |
| [libVLC / VLC media player](https://www.videolan.org/) | ユーザー環境のもの | **LGPL-2.1-or-later**（libVLC） / **GPL-2.0-or-later**（VLC） | 暗号化 DVD の再生時のみ**実行時にロード**。同梱はしません（VLC を導入した環境のみ利用可）。 |
| Windows SDK / DirectX 11 / WASAPI | OS 同梱 | Microsoft Software License Terms | OS に同梱のコンポーネントのみ使用し、再配布しません。 |

---

## FFmpeg（LGPL-3.0-or-later）

- 同梱ビルドは**未変更**です（自前のパッチは入れていません）。
- 構成オプションは `--enable-version3` で **`--enable-gpl` は未指定**、
  GPL 専用の部品（`--disable-libx264` / `--disable-libx265` / `--disable-libdvdread`
  など）は無効化されています。このため全体のライセンスは **LGPL-3.0** となります
  （同梱の `v3/third_party/ffmpeg/LICENSE.txt` が LGPLv3 本文であることが根拠です）。
- **動的リンク**（DLL）です。LGPL の要求する再リンク可能性を満たします。
- 対応するソースコードは [FFmpeg のダウンロードページ](https://ffmpeg.org/download.html) および
  [FFmpeg の Git](https://github.com/FFmpeg/FFmpeg) から入手できます
  （同梱ビルドはタグ `n9.0.2` より 22 コミットの `46d8f462ee`、ビルド日 2026-10-05）。
- バイナリが変更されていないこと、およびソースコードの写しを 3 年間有効な
  書面による提供で請求できることを、ここに明記します。
  ご請求の際は `yuudayoo.public@gmail.com` までご連絡ください。
- ライセンス本文は `v3/third_party/ffmpeg/LICENSE.txt`、およびアプリ内 `F1` に表示されます。

## Dear ImGui（MIT）

Copyright (c) 2014-2026 Omar Cornut — <https://github.com/ocornut/imgui>
本文は `v3/third_party/imgui/LICENSE.txt` およびアプリ内 `F1` に表示されます。

## AMD FidelityFX FSR 1.0（MIT）

Copyright (c) 2021 Advanced Micro Devices, Inc.
本文は `v3/third_party/ffx-fsr/license.txt` およびアプリ内 `F1` に表示されます。

## OpenRGB 互換クライアント

OpenRGB 本体は GPL-3.0-or-later ですが、本アプリには OpenRGB のソースコードを
一切同梱していません。SDK サーバー（`127.0.0.1:6742`）のネットワークプロトコルと
互換なクライアントを独自に実装しているだけで、OpenRGB を同梱・再配布しません。
（OpenRGB 本体を使う場合は、OpenRGB 側のライセンス条件に従ってください。）

## libVLC / VLC

暗号化 DVD を検出した場合のみ、ユーザーが導入した VLC の `libvlc.dll` を
実行時にロードします。VLC 本体・DLL は同梱も再配布もしていません。
組み込みは動的ロードのみで、`libVLC` は LGPL-2.1-or-later です。

## 過去版（リポジトリ内の旧ツリー）について

`third_party/`（初代）と `v2/third_party/` も参照用に同梱していますが、
使うのは同じライブラリです。

| 位置 | 内容 | ライセンス |
| --- | --- | --- |
| `third_party/ffmpeg`, `v2/third_party/ffmpeg`, `v3/third_party/ffmpeg` | FFmpeg | **LGPL-3.0**（3 とも `--enable-version3` / `--enable-gpl` なし） |
| `third_party/imgui`, `v2/third_party/imgui`, `v3/third_party/imgui` | Dear ImGui | **MIT** |
| `*/third_party/imgui/examples/libs/glfw` | GLFW（ImGui のサンプルに付属） | **MIT / X11**（`COPYING.txt`） |
| `v3/third_party/ffx-fsr` | FidelityFX FSR 1.0 | **MIT** |

初代・第 2 版のソースも本プログラムと同様に GPLv3 以降で公開します。

---

## GPLv3 との互換性について

本プログラムを GPLv3 以降で公開するにあたり、同梱ライブラリとの互換性を確認しました。

| もの | 互換性 |
| --- | --- |
| FFmpeg（LGPL-3.0） | **可** — LGPLv3 は GPLv3 に包含されたライセンスで、結合した作品は GPLv3 で公開できます（FSF の互換表・FAQ も「Combination is under GPLv3」と明記）。 |
| libVLC（LGPL-2.1） | **可** — LGPLv2.1 §3 により GPL へリライセンスできるため、GPLv3 と結合できます（実際は同梱しません）。 |
| Dear ImGui / FSR（MIT） | **可** — MIT は GPLv3 互換です。著作権表示を保持しています。 |
| OpenRGB（GPL-3.0） | **可** — 同一ライセンスです（コードは非同梱）。 |

GPLv3 が求める表示・出所（source）の条件については、
[README.md](README.md) の「ライセンス」節と本ファイルに対応する記載をしています。
