// =============================================================================
//  VlcVideo: 暗号化 DVD の再生バックエンド (VLC の dll を動的ロードする)
// -----------------------------------------------------------------------------
//  DVD-Video は CSS で暗号化されていることがあり、libdvdcss を持たない
//  FFmpeg ビルドでは復号できない。
//
//   v3 の方針 (ユーザー確認済み):
//     「DVD を読み込んで暗号化を検知したら、VLC の dll を読み込んで再生する」
//   すなわち
//     - 暗号化なし → 従来どおり FFmpeg 経路 (既存コードを変更しない)
//     - 暗号化あり → libvlc.dll を動的ロードし、libVLC に DVD を再生させる
//     - VLC が無い環境 → エラーとして明示する (黙って黒画面にならない)
//
//  映像は libVLC の video callback で BGRA として受け取り、自前の
//  D3D11 テクスチャへ載せて既存の VideoRenderer に流し込む (UI / 画質調整 /
//  FSR / 字幕レイアウトはそのまま使える)。
//  音声は libVLC 自身の出力 (WASAPI) を使うので AudioEngine は止めたまま。
//
//  検出 (detectDvd) とロード (findLibvlc / load) は VLC が無くても動作する。
// =============================================================================
#ifndef NANOALPHA_EXTENSIONS_VLC_VIDEO_HPP
#define NANOALPHA_EXTENSIONS_VLC_VIDEO_HPP

#include <windows.h>

#include <d3d11.h>

#include <mutex>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "Core/VideoDecoder.hpp"

namespace nanoalpha {
namespace vlc {

// -----------------------------------------------------------------------------
//  DVD 構造の検出と暗号化の判定
// -----------------------------------------------------------------------------
struct DvdInfo {
    bool isDvd = false;          // VIDEO_TS / .IFO / .VOB / .ISO / ドライブルート
    bool encrypted = false;      // VOB が CSS でスクランブルされている
    bool detectable = false;     // VOB を実際に読んで判定できた
    std::wstring root;           // DVD ルート (VIDEO_TS の親 / ドライブルート)
    std::wstring videoTs;        // VIDEO_TS ディレクトリ
    std::wstring probeFile;      // 暗号判定に使った VOB
    int vobCount = 0;            // VIDEO_TS 内の .VOB 数
    bool isIso = false;          // .ISO (シーク不能なため未判定のまま VLC へ渡す)
};

// path が DVD 構造なら isDvd を立て、可能なら暗号化も判定する。
// 呼び出し元のパス (ファイル / ディレクトリ / ドライブルート) を受け取る。
DvdInfo detectDvd(const std::wstring& path);

// -----------------------------------------------------------------------------
//  libvlc.dll の探索
// -----------------------------------------------------------------------------
//  見つからればフルパス、無ければ空文字。
//  探索順: HKLM/HKCU の VideoLAN\VLC InstallDir → 標準インストールパス。
std::wstring findLibvlc();

// -----------------------------------------------------------------------------
//  libVLC 再生バックエンド
// -----------------------------------------------------------------------------
class Video {
public:
    Video() = default;
    ~Video();

    Video(const Video&) = delete;
    Video& operator=(const Video&) = delete;

    // libvlc.dll を動的ロードする (既に読込済みなら true)。
    // 失敗すると false と理由 (UTF-8) を返す。
    bool load(std::string* error = nullptr);
    bool loaded() const noexcept { return m_lib != nullptr; }
    std::string version() const; // libvlc_get_version() (未読込なら空)

    // target を libVLC で開く。open していなければ false。
    bool open(const std::wstring& target);
    void close();
    bool isOpen() const noexcept { return m_player != nullptr; }

    void play();
    void setPaused(bool paused);
    void seekMs(long long ms);
    void setVolumePercent(int percent);
    bool playing() const;
    bool hasError() const noexcept { return m_error; }

    long long timeMs() const;
    long long durationMs() const;

    // --- 映像フレーム -------------------------------------------------------
    //  libVLC から届いた最新フレームを D3D11 テクスチャへ載せ、描画ビューを返す。
    //  まだフレームが無いときは texture == nullptr。
    //  (ContextLock の内側で呼ぶこと)
    VideoDecoder::FrameView present(ID3D11Device* device, ID3D11DeviceContext* context,
                                    double timeSec);

    // 情報タブ / シークバーへ渡すメディア情報
    VideoDecoder::Info mediaInfo() const;

private:
    static unsigned formatSetupCb(void** opaque, char* chroma, unsigned* width, unsigned* height,
                                  unsigned* pitches, unsigned* lines);
    static void formatCleanupCb(void* opaque);
    static void* lockCb(void** opaque, void** planes);
    static void unlockCb(void* opaque, void* const* planes, const unsigned* sizes);
    static void displayCb(void* opaque, void* picture);

    void allocateBuffersLocked();

    HMODULE m_lib = nullptr;
    void* m_instance = nullptr; // libvlc_instance_t*
    void* m_media = nullptr;    // libvlc_media_t*
    void* m_player = nullptr;   // libvlc_media_player_t*
    bool m_error = false;

    // ---- フレームバッファ (VLC 書き込み / アプリ読み出しの 2 枚) ----
    mutable std::mutex m_mutex;
    std::vector<unsigned char> m_buffers[2];
    int m_readIndex = 0;  // アプリが読む側 (display で公開される)
    int m_writeIndex = 1; // VLC が書く側
    unsigned m_frameW = 0;
    unsigned m_frameH = 0;
    unsigned m_pitch = 0;
    unsigned m_lines = 0;
    bool m_newFrame = false;

    // ---- D3D テクスチャ ----
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_texture;
    UINT m_texW = 0;
    UINT m_texH = 0;
};

} // namespace vlc
} // namespace nanoalpha

#endif // NANOALPHA_EXTENSIONS_VLC_VIDEO_HPP
