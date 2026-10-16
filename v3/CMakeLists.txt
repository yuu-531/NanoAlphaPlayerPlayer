# =============================================================================
#  NanoAlphaPlayer  —  VLC 完全上位互換 次世代メディアプレイヤー
#  C++20 / DirectX 11 / FFmpeg / Dear ImGui / OpenRGB SDK
# -----------------------------------------------------------------------------
#  [二段階起動モデル]
#    Phase 1 (メインスレッド, <= 100ms)
#      Win32 ウィンドウ / D3D11 デバイス / Dear ImGui / WASAPI 32bit / FFmpeg 再生
#    Phase 2 (バックグラウンド, <= 3s, UI スタッター禁止)
#      Frame Generation / OpenRGB / CD-DVD / ストリーミング / VLC 機能群
# =============================================================================
cmake_minimum_required(VERSION 3.24)

project(NanoAlphaPlayer
        VERSION     0.1.0
        DESCRIPTION "NanoAlphaPlayer - next generation media player"
        LANGUAGES   CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ------------------------------------------------------------------- オプション
option(NANOALPHA_ENABLE_FFMPEG   "FFmpeg (D3D11VA Zero-Copy) を有効化"          ON)
option(NANOALPHA_ENABLE_IMGUI    "Dear ImGui オーバーレイを有効化"              ON)
option(NANOALPHA_ENABLE_OPENRGB  "OpenRGB SDK 同期 (Phase 2)"                   ON)
option(NANOALPHA_ENABLE_FRAMEGEN "Compute Shader フレーム生成 (Phase 2)"        ON)
option(NANOALPHA_ENABLE_DISC     "CD-DA / DVD 再生 (Phase 2)"                   ON)
option(NANOALPHA_ENABLE_NETWORK  "ネットワーク配信・ストリーミング (Phase 2)"     ON)
option(NANOALPHA_CONSOLE         "ログを stdout に出力する (テスト / デバッグ用)"  OFF)
option(NANOALPHA_CONSOLE_WINDOW "コンソールウィンドウ自体を開く (OFF なら stdout のみ)" ON)
option(NANOALPHA_WERROR          "警告をエラーとして扱う"                       OFF)

# ------------------------------------------------------- 共通コンパイル設定
add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    # /W3 はサードパーティ (imgui) にも流す基準値。NanoAlphaPlayer 自前コードは /W4。
    # /Z7 は PDB を使わず .obj 内に行番号情報を持たせる (/MP との併用が安全)。
    # リンク時に /DEBUG と合わせて最終 PDB (NanoAlphaPlayer.pdb) へ集約され、
    # CrashReporter がヒープ破壊などのスタックをソース行まで展開できる。
    add_compile_options(/W3 /MP /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /Z7)
endif()

# ---------------------------------------------------------------------- FFmpeg
#  探索順:
#    1) NANOALPHA_FFMPEG_ROOT (include/ lib/ bin/ を持つ配布物。third_party/ffmpeg も自動検出)
#    2) vcpkg / plasma 等の CONFIG パッケージ
#    3) pkg-config
#  見つからなければスタブデコーダでビルドが完結する (フェーズ分割を止めない)。
set(NANOALPHA_HAS_FFMPEG OFF)
set(NANOALPHA_FFMPEG_TARGETS "")
set(NANOALPHA_FFMPEG_LIBS "")
set(NANOALPHA_FFMPEG_INCLUDE_DIRS "")
set(NANOALPHA_FFMPEG_ROOT "" CACHE PATH "FFmpeg 配布物のルート (include/, lib/, bin/)")

if(NANOALPHA_ENABLE_FFMPEG AND NOT NANOALPHA_FFMPEG_ROOT
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/include/libavcodec/avcodec.h")
    set(NANOALPHA_FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg")
endif()

if(NANOALPHA_ENABLE_FFMPEG AND NANOALPHA_FFMPEG_ROOT)
    if(EXISTS "${NANOALPHA_FFMPEG_ROOT}/include/libavcodec/avcodec.h")
        set(NANOALPHA_HAS_FFMPEG ON)
        set(NANOALPHA_FFMPEG_INCLUDE_DIRS "${NANOALPHA_FFMPEG_ROOT}/include")
        foreach(_lib IN ITEMS avformat avcodec avutil swresample swscale)
            if(EXISTS "${NANOALPHA_FFMPEG_ROOT}/lib/${_lib}.lib")
                list(APPEND NANOALPHA_FFMPEG_LIBS "${NANOALPHA_FFMPEG_ROOT}/lib/${_lib}.lib")
            endif()
        endforeach()
        message(STATUS "[NanoAlphaPlayer] FFmpeg (bundled): ${NANOALPHA_FFMPEG_ROOT}")
    else()
        message(WARNING "[NanoAlphaPlayer] NANOALPHA_FFMPEG_ROOT が不正です: ${NANOALPHA_FFMPEG_ROOT}")
    endif()
endif()

if(NANOALPHA_ENABLE_FFMPEG AND NOT NANOALPHA_HAS_FFMPEG)
    find_package(FFmpeg CONFIG QUIET)
    foreach(_prefix IN ITEMS FFmpeg ffmpeg)
        if(TARGET ${_prefix}::avcodec)
            list(APPEND NANOALPHA_FFMPEG_TARGETS
                 ${_prefix}::avcodec  ${_prefix}::avformat  ${_prefix}::avutil
                 ${_prefix}::swresample ${_prefix}::swscale)
            set(NANOALPHA_HAS_FFMPEG ON)
            break()
        endif()
    endforeach()

    if(NOT NANOALPHA_HAS_FFMPEG)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(NANOALPHA_FFMPEG QUIET IMPORTED_TARGET
                              libavformat libavcodec libavutil libswresample libswscale)
            if(TARGET PkgConfig::NANOALPHA_FFMPEG)
                set(NANOALPHA_FFMPEG_TARGETS PkgConfig::NANOALPHA_FFMPEG)
                set(NANOALPHA_HAS_FFMPEG ON)
            endif()
        endif()
    endif()

    if(NOT NANOALPHA_HAS_FFMPEG)
        message(WARNING "[NanoAlphaPlayer] FFmpeg が見つかりません (-DNANOALPHA_FFMPEG_ROOT=/path) "
                        "-> スタブデコーダでビルドします")
    endif()
endif()

# -------------------------------------------------------------------- Dear ImGui
#  1) NANOALPHA_IMGUI_DIR  2) third_party/imgui  3) FetchContent (git)  の順に探索。
set(NANOALPHA_HAS_IMGUI OFF)
if(NANOALPHA_ENABLE_IMGUI)
    set(NANOALPHA_IMGUI_DIR "" CACHE PATH "Dear ImGui のソースルート (imgui.h のある場所)")

    if(NOT NANOALPHA_IMGUI_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/imgui.h")
        set(NANOALPHA_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
    endif()

    if(NOT NANOALPHA_IMGUI_DIR)
        find_package(Git QUIET)
        if(GIT_FOUND)
            include(FetchContent)
            FetchContent_Declare(imgui
                GIT_REPOSITORY https://github.com/ocornut/imgui.git
                GIT_TAG        v1.92.9-docking
                GIT_SHALLOW    TRUE)
            FetchContent_MakeAvailable(imgui)
            set(NANOALPHA_IMGUI_DIR "${imgui_SOURCE_DIR}")
        endif()
    endif()

    # ImGui 1.92 以降は D3D11 バックエンドが imgui_impl_dx11.* に改名されている
    if(NANOALPHA_IMGUI_DIR
       AND EXISTS "${NANOALPHA_IMGUI_DIR}/imgui.h"
       AND EXISTS "${NANOALPHA_IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
        set(NANOALPHA_HAS_IMGUI ON)
    else()
        message(WARNING "[NanoAlphaPlayer] Dear ImGui が見つかりません "
                        "(-DNANOALPHA_IMGUI_DIR=/path/to/imgui) -> UI 無効でビルドします")
    endif()
endif()

if(NANOALPHA_HAS_IMGUI)
    set(NANOALPHA_IMGUI_SOURCES
        "${NANOALPHA_IMGUI_DIR}/imgui.cpp"
        "${NANOALPHA_IMGUI_DIR}/imgui_draw.cpp"
        "${NANOALPHA_IMGUI_DIR}/imgui_tables.cpp"
        "${NANOALPHA_IMGUI_DIR}/imgui_widgets.cpp"
        "${NANOALPHA_IMGUI_DIR}/backends/imgui_impl_win32.cpp"
        "${NANOALPHA_IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
    if(EXISTS "${NANOALPHA_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp")
        list(APPEND NANOALPHA_IMGUI_SOURCES "${NANOALPHA_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp")
    endif()

    add_library(nanoalpha_imgui STATIC ${NANOALPHA_IMGUI_SOURCES})
    target_include_directories(nanoalpha_imgui PUBLIC
        "${NANOALPHA_IMGUI_DIR}"
        "${NANOALPHA_IMGUI_DIR}/backends")
    target_compile_features(nanoalpha_imgui PUBLIC cxx_std_20)
    target_compile_definitions(nanoalpha_imgui PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(nanoalpha_imgui PUBLIC d3d11 dxgi)
    if(MSVC)
        target_compile_options(nanoalpha_imgui PRIVATE /W3 /MP /utf-8)
    endif()
endif()

# ------------------------------------------------------------------------ 対象
set(NANOALPHA_SOURCES
    src/main.cpp
    src/Core/AudioDecoder.cpp
    src/Core/AudioEngine.cpp
    src/Core/CrashReporter.cpp
    src/Core/VideoDecoder.cpp
    src/Core/VideoRenderer.cpp
    src/Core/SubtitleSource.cpp
    src/Extensions/ExtensionManager.cpp
    src/Extensions/BuiltinLoaders.cpp
    src/Extensions/OpenRgbClient.cpp
    src/Extensions/VlcVideo.cpp
)

set(NANOALPHA_HEADERS
    include/Core/Log.hpp
    include/Core/Licenses.hpp
    include/Core/Window.hpp
    include/Core/AudioEngine.hpp
    include/Core/AudioDecoder.hpp
    include/Core/MediaClock.hpp
    include/Core/VideoDecoder.hpp
    include/Core/VideoRenderer.hpp
    include/Core/SubtitleSource.hpp
    include/Core/PlayerController.hpp
    include/Core/Settings.hpp
    include/Core/NvidiaSupport.hpp
    include/Extensions/ExtensionManager.hpp
    include/Extensions/BuiltinLoaders.hpp
    include/Extensions/OpenRgbClient.hpp
    include/Extensions/MediaTools.hpp
    include/UI/FileDialog.hpp
    include/UI/ImGuiOverlay.hpp
)

# ------------------------------------------------------------------- リソース
#  アプリアイコンと VERSIONINFO (エクスプローラー / タスクバー / アンインストーラ
#  に表示される版数と発行元)。.ico は resources\make_icon.ps1 で再生成できる。
if(WIN32)
    enable_language(RC)
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /I \"${CMAKE_CURRENT_SOURCE_DIR}/resources\"")
    list(APPEND NANOALPHA_SOURCES resources/app.rc)
endif()

add_executable(NanoAlphaPlayer WIN32 ${NANOALPHA_SOURCES} ${NANOALPHA_HEADERS})

target_include_directories(NanoAlphaPlayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")

# FidelityFX FSR1 (AMD 超解像度)。CPU 側はここで、HLSL 側は exe 目録からの
# 直接読み込み (FileInclude) で取り込むため、両方の場所へ配置する。
set(NANOALPHA_FSR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-fsr")
target_include_directories(NanoAlphaPlayer PRIVATE "${NANOALPHA_FSR_DIR}")
add_custom_command(TARGET NanoAlphaPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NANOALPHA_FSR_DIR}/ffx_a.h"   "$<TARGET_FILE_DIR:NanoAlphaPlayer>/ffx_a.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NANOALPHA_FSR_DIR}/ffx_fsr1.h" "$<TARGET_FILE_DIR:NanoAlphaPlayer>/ffx_fsr1.h"
    COMMENT "Copying FidelityFX FSR headers next to NanoAlphaPlayer.exe")

# ライセンス文書を成果物へ同梱する (GPLv3: 配布するバイナリにライセンス全文と
# 第三者ライセンスの表示を添える)。文書はリポジトリ直下に置く。
foreach(_nanoalpha_doc
        "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_LICENSES.md")
    if(EXISTS "${_nanoalpha_doc}")
        add_custom_command(TARGET NanoAlphaPlayer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_nanoalpha_doc}" "$<TARGET_FILE_DIR:NanoAlphaPlayer>"
            COMMENT "Copying license documents next to NanoAlphaPlayer.exe")
    endif()
endforeach()

# 各ヘッダの #if NANOALPHA_xxx を 0/1 に確定させる (既定値も同じ意味)
target_compile_definitions(NanoAlphaPlayer PRIVATE
    NANOALPHA_HAS_IMGUI=$<BOOL:${NANOALPHA_HAS_IMGUI}>
    NANOALPHA_HAS_FFMPEG=$<BOOL:${NANOALPHA_HAS_FFMPEG}>
    NANOALPHA_ENABLE_OPENRGB=$<BOOL:${NANOALPHA_ENABLE_OPENRGB}>
    NANOALPHA_ENABLE_FRAMEGEN=$<BOOL:${NANOALPHA_ENABLE_FRAMEGEN}>
    NANOALPHA_ENABLE_DISC=$<BOOL:${NANOALPHA_ENABLE_DISC}>
    NANOALPHA_ENABLE_NETWORK=$<BOOL:${NANOALPHA_ENABLE_NETWORK}>
    NANOALPHA_CONSOLE=$<BOOL:${NANOALPHA_CONSOLE}>
)

set(NANOALPHA_LIBS
    d3d11        # Direct3D 11
    dxgi         # スワップチェーン
    d3dcompiler  # HLSL 実行時コンパイル (Ambilight / FrameGen Compute)
    dwmapi       # WorkerW 壁紙モード・サムネイル
    ole32 avrt   # WASAPI (COM / MMCSS)
    winmm
    ws2_32       # OpenRGB SDK TCP / ストリーミング
    shell32 shlwapi propsys
    user32 gdi32
    comdlg32    # GetOpenFileNameW / GetSaveFileNameW のフォールバック
    uuid        # CLSID_FileOpenDialog / CLSID_WICImagingFactory
    windowscodecs # WIC (PNG スナップショット保存)
    dbghelp       # CrashReporter のスタックシンボル化
)

if(NANOALPHA_HAS_FFMPEG)
    if(NANOALPHA_FFMPEG_LIBS)
        # 配布物 (include/ + lib/*.lib) を直接リンク
        target_include_directories(NanoAlphaPlayer PRIVATE ${NANOALPHA_FFMPEG_INCLUDE_DIRS})
        list(APPEND NANOALPHA_LIBS ${NANOALPHA_FFMPEG_LIBS})
        if(EXISTS "${NANOALPHA_FFMPEG_ROOT}/bin")
            add_custom_command(TARGET NanoAlphaPlayer POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${NANOALPHA_FFMPEG_ROOT}/bin" "$<TARGET_FILE_DIR:NanoAlphaPlayer>"
                COMMENT "Copying FFmpeg runtime DLLs")
        endif()
    else()
        list(APPEND NANOALPHA_LIBS ${NANOALPHA_FFMPEG_TARGETS})
    endif()
endif()
if(NANOALPHA_HAS_IMGUI)
    list(APPEND NANOALPHA_LIBS nanoalpha_imgui)
endif()

# ----------------------------------------------------------------------
#  FFmpeg DLL の delay-load 化 (v2: 起動高速化)
# -----------------------------------------------------------------------------
#  インポート解決をプロセス起動時から外し、初回 FFmpeg 呼び出しまで
#  遅らせる。依存 DLL の事前ロードは runApplication 冒頭のスレッドが
#  依存順 (avutil → swresample → swscale → avcodec → avformat) で行う。
#
#  注意: 配布物の avformat.lib 等は GNU 形式のインポート ライブラリで、
#  MSVC リンクの /DELAYLOAD 照合に引っかからない (LNK4199 で無視される)
#  ため、同梱の .def から lib.exe で MSVC 形式のインポート ライブラリを
#  生成して差し替える。生成に失敗した場合は delay-load を行わない。
# ----------------------------------------------------------------------
set(NANOALPHA_FFMPEG_DELAYLOAD OFF)
if(MSVC AND NANOALPHA_HAS_FFMPEG AND NANOALPHA_FFMPEG_ROOT AND
   EXISTS "${NANOALPHA_FFMPEG_ROOT}/lib")
    # lib.exe はコンパイラと同じ bin ディレクトリにある
    get_filename_component(_nanoalpha_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_nanoalpha_msvc_lib_dir "${CMAKE_BINARY_DIR}/ffmpeg-msvc-lib")
    file(MAKE_DIRECTORY "${_nanoalpha_msvc_lib_dir}")

    set(_nanoalpha_gen_libs "")
    set(_nanoalpha_delay_opts "")
    foreach(_comp IN ITEMS avformat avcodec avutil swresample swscale)
        file(GLOB _def "${NANOALPHA_FFMPEG_ROOT}/lib/${_comp}-*.def")
        if(NOT _def)
            continue()
        endif()
        get_filename_component(_def_name "${_def}" NAME_WE) # 例: avformat-63
        set(_gen_lib "${_nanoalpha_msvc_lib_dir}/${_def_name}.lib")
        # MSVC ツールは /def:"path" の形式で引用符付きパスを渡すと
        # 引用符が値の一部として解釈される (LNK1104) ので、
        # 応答ファイル経由で呼び出す。
        set(_nanoalpha_rsp "${_nanoalpha_msvc_lib_dir}/${_def_name}.rsp")
        file(WRITE "${_nanoalpha_rsp}"
             "/nologo\n/machine:x64\n"
             "/def:\"${_def}\"\n/out:\"${_gen_lib}\"\n")
        execute_process(
            COMMAND "${_nanoalpha_msvc_bin}/lib.exe" "@${_nanoalpha_rsp}"
            RESULT_VARIABLE _res
            OUTPUT_VARIABLE _oout
            ERROR_VARIABLE _oerr)
        if(NOT _res EQUAL 0 OR NOT EXISTS "${_gen_lib}")
            message(WARNING "[NanoAlphaPlayer] FFmpeg インポート ライブラリ生成に失敗: ${_def}"
                            " (res=${_res} err=${_oout})")
            set(_nanoalpha_gen_libs "")
            set(_nanoalpha_delay_opts "")
            break()
        endif()
        list(APPEND _nanoalpha_gen_libs "${_gen_lib}")
        list(APPEND _nanoalpha_delay_opts "/DELAYLOAD:${_def_name}.dll")
    endforeach()

    if(_nanoalpha_gen_libs)
        # 生成した MSVC 形式ライブラリへ差し替え (依存順に並ぶ)
        list(REMOVE_ITEM NANOALPHA_LIBS ${NANOALPHA_FFMPEG_LIBS})
        set(NANOALPHA_FFMPEG_LIBS ${_nanoalpha_gen_libs})
        list(APPEND NANOALPHA_LIBS ${_nanoalpha_gen_libs})
        target_link_options(NanoAlphaPlayer PRIVATE ${_nanoalpha_delay_opts})
        list(APPEND NANOALPHA_LIBS delayimp)
        set(NANOALPHA_FFMPEG_DELAYLOAD ON)
        message(STATUS "[NanoAlphaPlayer] FFmpeg delay-load: ${_nanoalpha_delay_opts}")
    endif()
endif()

target_link_libraries(NanoAlphaPlayer PRIVATE ${NANOALPHA_LIBS})

if(MSVC)
    target_compile_options(NanoAlphaPlayer PRIVATE /W4 $<$<BOOL:${NANOALPHA_WERROR}>:/WX>)
    # /Z7 で埋め込んだ行番号情報を最終 PDB へ集約する (CrashReporter 用)
    target_link_options(NanoAlphaPlayer PRIVATE /DEBUG)
endif()

set_target_properties(NanoAlphaPlayer PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# コンソールログ (NANOALPHA_CONSOLE) とコンソールウィンドウ (NANOALPHA_CONSOLE_WINDOW) は
# 独立している。配布物は WIN32_EXECUTABLE=TRUE (WINDOWS サブシステム) のまま、
# stdout が引き継がれているときだけログが出る形にして、ユーザーには窓を出さない。
if(NANOALPHA_CONSOLE AND NANOALPHA_CONSOLE_WINDOW)
    set_target_properties(NanoAlphaPlayer PROPERTIES WIN32_EXECUTABLE FALSE)
endif()

# ----------------------------------------------------------------------
#  ビルド例:
#    cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#    cmake --build build --config Release
# ----------------------------------------------------------------------
