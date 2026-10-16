#pragma once
#ifndef NANOALPHA_CORE_VIDEO_RENDERER_HPP
#define NANOALPHA_CORE_VIDEO_RENDERER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <chrono>
#include <future>
#include <map>
#include <tuple>

#include "Core/PlayerController.hpp"
#include "Core/VideoDecoder.hpp"

namespace nanoalpha {

// =============================================================================
//  映像レンダラ (Zero-Copy + v2 画質 / フレーム補間 + v3 FSR)
// -----------------------------------------------------------------------------
//  デコード済みサーフェス (NV12 / P010) を CPU へ出さず、HLSL で RGB へ変換して
//  レンダーターゲットへ直接描画する。
//  回転 (0/90/180/270) / 上下左右反転 / クロップ / アスペクト比はすべてシェーダと
//  頂点生成で処理するため、PlayerController の操作は 1 フレームで反映される。
//
//  [v2 画質] ブライトネス / コントラスト / 彩度 / 色相 / シャープネス / スケーリング
//             / レンジ上書き はすべてシェーダまたはサンプラで処理する。
//  [v2 フレーム補間] 有効時は中間 RT を 2 枚 (prev / curr) 用意し、
//             前フレームと現フレームをブレンドして出力する (Frame Generation)。
//  [v3 フレーム補間の修正] 旧実装は毎フレーム swap していたため、デコード更新直後の
//             1 表示フレームだけ補間し、以降は prev==curr で補間が発生しなかった。
//             pts が更新された時 (= 新しいフレームをデコードした時) だけ swap し、
//             同じフレームが続く表示フレームでは位相 (blend) だけを進める。
//  [v3 FSR]  ScaleFilter::Fsr で拡大表示する場合は
//             ①mainPS → 入力 RT (実寸) ②EASU → 中間 RT ③RCAS → 描画先
//             の 3 パスで FidelityFX FSR1 を通す (縮小時は従来経路へフォールバック)。
//  (メインスレッド専用 / ID3D11DeviceContext は ContextLock の内側で使うこと)
// =============================================================================
class VideoRenderer {
public:
    // HLSL の実行時コンパイル (デバイス不要・CPU のみ)。init() より先に別スレッドで
    // 呼んでおくと、シェーダコンパイルがウィンドウ / D3D11 生成と重なって起動が短くなる。
    //  (FSR 用シェーダは重いので起動経路には含めず、選択された時に遅延コンパイルする)
    bool compile() noexcept;
    bool init(ID3D11Device* device, ID3D11DeviceContext* context);
    void shutdown() noexcept;
    bool ready() const noexcept { return m_ready; }

    // mediaTime は Frame Generation 有効時のみ使用する媒体クロック (秒)。
    // 補間重みは renderer 側で保持している prev/curr の実 pts から算出する
    // (映像は全フレームの時刻が事前に分かっているため、CFR 前提の
    //  fps グリッドではなく実測間隔で正確に補間できる)。
    // videoArea は UI (上部タブバー / 右ドック) を避けた映像描画領域 (ピクセル)。
    // 幅か高さが 0 なら全面として扱う。
    void draw(const VideoDecoder::FrameView& frame, const PlayerController::Snapshot& control,
              UINT renderTargetWidth, UINT renderTargetHeight, const RECT& videoArea,
              double mediaTime = 0.0);

private:
    struct SrvEntry {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> y;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> uv;
    };

    // フレーム補間用の中間ターゲット (prev / curr のピンポン)
    // FSR 用の中間ターゲット (入力実寸 / 出力解像度) も同じ形を流用する。
    struct Target {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    // FSR 用シェーダの遅延コンパイル結果 (バックグラウンドで生成される)
    struct FsrBlobs {
        Microsoft::WRL::ComPtr<ID3DBlob> easu;
        Microsoft::WRL::ComPtr<ID3DBlob> rcas;
        bool ok = false;
    };

    SrvEntry* acquireSrvs(const VideoDecoder::FrameView& frame);
    bool createTarget(Target& target, UINT width, UINT height);
    bool ensureTargets(UINT width, UINT height);
    bool ensureFsrTargets(UINT srcWidth, UINT srcHeight, UINT dstWidth, UINT dstHeight);
    void requestFsrCompile();
    void pollFsrCompile();
    void drawVideo(const VideoDecoder::FrameView& frame,
                   const PlayerController::Snapshot& control, const RECT& area, UINT rtWidth,
                   UINT rtHeight);
    // VideoCB を目的矩形つきで組み立てて書き込む (成功したら true)
    bool uploadVideoCb(const VideoDecoder::FrameView& frame,
                       const PlayerController::Snapshot& control, float x0, float y0, float x1,
                       float y1);
    // mainPS を RT 正規化座標 (x0,y0)-(x1,y1) の矩形へ 1 パス描画する。
    // FSR ①パスでは (0,0)-(1,1) を渡して入力 RT 全面へ描画する。
    // FSR 用シェーダーをバックグラウンドでコンパイルする (起動経路から分離するため)。
    static FsrBlobs compileFsrShaders();
    void drawVideoQuad(const VideoDecoder::FrameView& frame,
                       const PlayerController::Snapshot& control, const SrvEntry& srvs,
                       float x0, float y0, float x1, float y1, int samplerIndex);
    void updateFsrBuffer(const std::uint32_t (&con0)[4], const std::uint32_t (&con1)[4],
                         const std::uint32_t (&con2)[4], const std::uint32_t (&con3)[4],
                         const std::uint32_t (&rcas)[4], int originX, int originY, int width,
                         int height);
    void blendPass(float blend);
    // 媒体クロック mediaTime に対する補間重み (0..1) を求める。
    // 表示は prev (m_prevPts) → curr (m_lastPts) へ連続でクロスフェードする。
    float interpolationWeight(double mediaTime) const noexcept;

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_blendShader; // Frame Generation 用
    Microsoft::WRL::ComPtr<ID3DBlob> m_vsBlob; // compile() 結果 (init で解放)
    Microsoft::WRL::ComPtr<ID3DBlob> m_psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_blendBlob;
    // 0=Linear 1=Point 2=Anisotropic (画質タブのスケーリング切替)
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_samplers[3];
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;

    // ---- v3: FSR ----
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_easuShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_rcasShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_fsrBuffer; // b1 (FsrCB)
    std::shared_future<FsrBlobs> m_fsrCompile;
    bool m_fsrRequested = false;   // 遅延コンパイル要求済み
    bool m_fsrReady = false;       // PS 生成まで完了
    bool m_fsrFailed = false;      // コンパイル / 生成失敗 (以降フォールバック)
    Target m_fsrSrc;               // EASU 入力 (実寸)
    Target m_fsrDst;               // EASU 出力 (= RCAS 入力)
    UINT m_fsrSrcW = 0, m_fsrSrcH = 0;
    UINT m_fsrDstW = 0, m_fsrDstH = 0;
    // 寸法が変わったときだけ "FSR viewport" を 1 行出すための直前値
    UINT m_fsrLogSrcW = 0xFFFFFFFFu, m_fsrLogSrcH = 0xFFFFFFFFu;
    UINT m_fsrLogDstW = 0xFFFFFFFFu, m_fsrLogDstH = 0xFFFFFFFFu;

    Target m_prev;
    Target m_curr;
    bool m_havePrev = false;
    bool m_haveCurr = false;   // m_curr に有効フレームが入っている
    double m_lastPts = 0.0;    // 最後に描画したフレームの pts
    double m_prevPts = 0.0;    // その 1 フレーム前の pts (補間の基準点)
    const void* m_lastTexture = nullptr;      // 新規フレーム検出の補助シグナル
    int m_lastArrayIndex = -1;
    double m_frameInterval = 1.0 / 60.0; // pts 差分の EMA (カット検出の閾値に使用)
    double m_displayInterval = 1.0 / 60.0; // 描画間隔の EMA (目標倍率の判定に使用)
    int m_frameGenFactor = 4;               // 補間の目標倍率 (2/4/8/16)
    std::chrono::steady_clock::time_point m_lastDrawTime{};

    // キーに width/height を含める (テクスチャポインタの再利用で古い SRV を
    // 返さないため。ソフトウェア経路は 2 枚をローテーションする)
    std::map<std::tuple<const void*, int, DXGI_FORMAT, UINT, UINT>, SrvEntry> m_srvCache;
    bool m_ready = false;
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_VIDEO_RENDERER_HPP
