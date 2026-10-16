#include "Core/VideoRenderer.hpp"

#include "Core/Log.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// ---- FidelityFX FSR1 (AMD 超解像度) の CPU 定数生成 --------------------------
//  A_CPU を定義してから ffx_a.h / ffx_fsr1.h を取り込むと、
//  FsrEasuCon() / FsrRcasCon() が static 関数として得られる。
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"

namespace nanoalpha {
namespace {

// -----------------------------------------------------------------------------
//  ピクセルシェーダー本体
//   - mainPS : NV12 (8bit) / P010 (10bit) -> RGB (BT.709) + 画質補正
//   - blendPS: Frame Generation 用。前フレームと現フレームをブレンドする
// -----------------------------------------------------------------------------
constexpr const char* kShaderSource = R"HLSL(
cbuffer VideoCB : register(b0)
{
    float4 gDest;      // 目的矩形 (0..1, y は上起点)
    float4 gCrop;      // 左, 上, 右, 下 (0..1)
    uint   gRotation;  // 0/1/2/3 = 0/90/180/270 度 (時計回り)
    uint   gFlags;     // bit0=反転H bit1=反転V bit2=P010 bit3=リミテッド bit5=BGRA
    float2 gUvScale;   // コンテンツ領域 / サーフェス (デコーダのパディング補正)
    float4 gAdjust;    // x=ブライトネス y=コントラスト z=彩度 w=色相(rad)
    float4 gAdjust2;   // x=シャープネス y=texelW z=texelH w=ブレンド t
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// 頂点 0..3 の三角形ストリップで目的矩形を覆う (VB 不要)
VSOut mainVS(uint vid : SV_VertexID)
{
    float2 corner = float2((vid & 1), ((vid >> 1) & 1)); // (0,0) (1,0) (0,1) (1,1)
    float2 tl = float2(gDest.x * 2.0 - 1.0, 1.0 - gDest.y * 2.0);
    float2 br = float2(gDest.z * 2.0 - 1.0, 1.0 - gDest.w * 2.0);
    VSOut o;
    o.pos = float4(lerp(tl, br, corner), 0.0, 1.0);
    o.uv  = corner;
    return o;
}

Texture2D<float4> tY  : register(t0);
Texture2D<float4> tUV : register(t1);
SamplerState      smp : register(s0);

float2 mapUv(float2 uv)
{
    // クロップ
    uv = lerp(float2(gCrop.x, gCrop.y), float2(1.0 - gCrop.z, 1.0 - gCrop.w), uv);
    // 反転
    if (gFlags & 1) uv.x = 1.0 - uv.x;
    if (gFlags & 2) uv.y = 1.0 - uv.y;
    // 回転 (90 度単位・時計回り)。逆写像でサンプル位置を求める
    if (gRotation == 1)      uv = float2(uv.y,        1.0 - uv.x);
    else if (gRotation == 2) uv = float2(1.0 - uv.x,  1.0 - uv.y);
    else if (gRotation == 3) uv = float2(1.0 - uv.y,  uv.x);
    return uv;
}

// CSS HueRotate と同式の色相回転行列
float3 hueRotate(float3 rgb, float a)
{
    const float c = cos(a);
    const float s = sin(a);
    const float3x3 m = float3x3(
        0.213 + c * 0.787 - s * 0.213, 0.715 - c * 0.715 - s * 0.715, 0.072 - c * 0.072 + s * 0.928,
        0.213 - c * 0.213 + s * 0.143, 0.715 + c * 0.285 + s * 0.140, 0.072 - c * 0.072 - s * 0.283,
        0.213 - c * 0.213 - s * 0.787, 0.715 - c * 0.715 + s * 0.715, 0.072 + c * 0.928 + s * 0.072);
    return mul(m, rgb);
}

float4 mainPS(VSOut i) : SV_Target
{
    // コンテンツ座標 (0..1) で回転/反転/クロップしてから、サーフェス実座標へ拡縮する
    // (デコーダは高さをアライン分パディングするため。例: 1080 -> 1152)
    float2 uv  = mapUv(i.uv) * gUvScale;

    // gFlags bit5 = BGRA 入力 (ソフトウェアデコード経路。フルレンジ RGB)
    if (gFlags & 32)
    {
        float3 bgr = tY.Sample(smp, uv).rgb;
        // 輝度ベースのアンシャープマスク
        if (gAdjust2.x > 0.0)
        {
            const float2 tx = gAdjust2.yz;
            const float lum = dot(bgr, float3(0.2126, 0.7152, 0.0722));
            const float blur = dot(
                (tY.Sample(smp, uv + float2(tx.x, 0.0)).rgb +
                 tY.Sample(smp, uv - float2(tx.x, 0.0)).rgb +
                 tY.Sample(smp, uv + float2(0.0, tx.y)).rgb +
                 tY.Sample(smp, uv - float2(0.0, tx.y)).rgb) * 0.25,
                float3(0.2126, 0.7152, 0.0722));
            const float nl = saturate(lum + (lum - blur) * gAdjust2.x * 4.0);
            bgr *= (lum > 0.0001) ? (nl / lum) : 1.0;
        }
        float3 rgb = bgr;
        // ---- 色相 / 彩度 / ブライトネス / コントラスト ----
        if (abs(gAdjust.w) > 0.0001) rgb = hueRotate(rgb, gAdjust.w);
        if (abs(gAdjust.z - 1.0) > 0.0001)
        {
            const float lum2 = dot(rgb, float3(0.2126, 0.7152, 0.0722));
            rgb = lum2 + gAdjust.z * (rgb - lum2);
        }
        rgb = (rgb - 0.5) * gAdjust.y + 0.5 + gAdjust.x;
        return float4(saturate(rgb), 1.0);
    }

    float  y   = tY.Sample(smp, uv).r;
    float2 chroma = tUV.Sample(smp, uv).rg;

    if (gFlags & 4)
    {
        // P010: 10bit 値が上位ビット (<<6) に格納されている
        const float scale = 65535.0 / 64.0 / 1023.0;
        y     *= scale;
        chroma *= scale;
    }

    // ---- シャープネス (Y 平面へのアンシャープマスク) ----
    if (gAdjust2.x > 0.0)
    {
        const float2 tx = gAdjust2.yz;
        const float blur = (tY.Sample(smp, uv + float2(tx.x, 0.0)).r +
                            tY.Sample(smp, uv - float2(tx.x, 0.0)).r +
                            tY.Sample(smp, uv + float2(0.0, tx.y)).r +
                            tY.Sample(smp, uv - float2(0.0, tx.y)).r) * 0.25;
        y = saturate(y + (y - blur) * gAdjust2.x * 4.0);
    }

    float2 c = chroma - 0.5;
    float3 rgb;
    if (gFlags & 8)
    {
        // BT.709 リミテッドレンジ
        y = (y - 0.0625) * 1.164;
        rgb = float3(y + 1.793 * c.y,
                     y - 0.213 * c.x - 0.533 * c.y,
                     y + 2.113 * c.x);
    }
    else
    {
        // フルレンジ
        rgb = float3(y + 1.5748 * c.y,
                     y - 0.1873 * c.x - 0.4681 * c.y,
                     y + 1.8556 * c.x);
    }

    // ---- 色相 / 彩度 / ブライトネス / コントラスト ----
    if (abs(gAdjust.w) > 0.0001) rgb = hueRotate(rgb, gAdjust.w);
    if (abs(gAdjust.z - 1.0) > 0.0001)
    {
        const float lum = dot(rgb, float3(0.2126, 0.7152, 0.0722));
        rgb = lum + gAdjust.z * (rgb - lum);
    }
    rgb = (rgb - 0.5) * gAdjust.y + 0.5 + gAdjust.x;

    return float4(saturate(rgb), 1.0);
}

// ---- Frame Generation: 前フレーム (t0) と現フレーム (t1) のブレンド ----
Texture2D<float4> tPrev : register(t0);
Texture2D<float4> tCurr : register(t1);

float4 blendPS(VSOut i) : SV_Target
{
    const float4 p = tPrev.Sample(smp, i.uv);
    const float4 c = tCurr.Sample(smp, i.uv);
    return lerp(p, c, saturate(gAdjust2.w));
}
)HLSL";

// -----------------------------------------------------------------------------
//  FidelityFX FSR1 (AMD 超解像度) のピクセルシェーダー
//   - easuPS : 12tap アンチエイリアス付き拡大 (EASU)
//   - rcasPS : 適応的シャープニング (RCAS)
//  実体は third_party/ffx-fsr/ffx_a.h, ffx_fsr1.h に含まれるため、
//  D3DCompile にインクルードハンドラ (exe 目録基準) を渡して取り込む。
//  シェーダ本体は起動を止めないよう選択された時に遅延コンパイルする。
// -----------------------------------------------------------------------------
constexpr const char* kFsrHlsl = R"HLSL(
cbuffer FsrCB : register(b1)
{
    uint4 gFsrCon0;   // FsrEasuCon() の結果 (float ビット列)
    uint4 gFsrCon1;
    uint4 gFsrCon2;
    uint4 gFsrCon3;
    uint4 gFsrRcas;   // x=FsrRcasCon() y=描画矩形左上 X z=同 Y w=未使用
    uint4 gFsrSize;   // x=出力幅 y=出力高さ (RCAS の端数クランプに使用)
};

Texture2D<float4> tFsrSrc : register(t0);   // EASU 入力 / RCAS 入力 (再バインド)
SamplerState      sFsr    : register(s0);   // Linear + Clamp

#define A_GPU 1
#define A_HLSL 1
#include "ffx_a.h"

// EASU: 入力テクスチャから gather4 で 12 タップ展開する
AF4 FsrEasuRF(AF2 p) { return tFsrSrc.GatherRed(sFsr, p); }
AF4 FsrEasuGF(AF2 p) { return tFsrSrc.GatherGreen(sFsr, p); }
AF4 FsrEasuBF(AF2 p) { return tFsrSrc.GatherBlue(sFsr, p); }

// RCAS: EASU の出力を 3x3 近傍で読む (端はクランプ)
AF4 FsrRcasLoadF(ASU2 p)
{
    const int2 size = int2(gFsrSize.x, gFsrSize.y);
    const int2 q = clamp(int2(p.x, p.y), int2(0, 0), size - int2(1, 1));
    return tFsrSrc.Load(int3(q.x, q.y, 0));
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}

#define FSR_EASU_F 1
#define FSR_RCAS_F 1
#include "ffx_fsr1.h"

float4 easuPS(float4 pos : SV_Position) : SV_Target
{
    float3 c = float3(0.0, 0.0, 0.0);
    FsrEasuF(c, uint2(pos.x, pos.y), gFsrCon0, gFsrCon1, gFsrCon2, gFsrCon3);
    return float4(c, 1.0);
}

float4 rcasPS(float4 pos : SV_Position) : SV_Target
{
    // SV_Position は描画先 (バックバッファ) 座標なので、原点を引いて
    // EASU 出力テクスチャの座標へ写像する。
    const uint2 ip = uint2(pos.x, pos.y) - gFsrRcas.yz;
    float r = 0.0, g = 0.0, b = 0.0;
    FsrRcasF(r, g, b, ip, gFsrRcas);
    return float4(r, g, b, 1.0);
}
)HLSL";

struct alignas(16) VideoCB {
    float dest[4];
    float crop[4];
    std::uint32_t rotation;
    std::uint32_t flags;
    float uvScale[2];   // コンテンツ / サーフェス (パディング補正)
    float adjust[4];    // brightness, contrast, saturation, hue(rad)
    float adjust2[4];   // sharpness, texelW, texelH, blend t
};
static_assert(sizeof(VideoCB) == 80, "cbuffer は 16 バイト単位でなければならない");

// FSR 用定数バッファ (b1)。値は ffx_fsr1.h の FsrEasuCon / FsrRcasCon が書き出す
// float ビット列をそのまま格納する (シェーダ側で asfloat される)。
struct alignas(16) FsrCB {
    std::uint32_t con0[4];
    std::uint32_t con1[4];
    std::uint32_t con2[4];
    std::uint32_t con3[4];
    std::uint32_t rcas[4];  // x=sharpness ビット列 y=原点X z=原点Y w=未使用
    std::uint32_t size[4];  // x=幅 y=高さ z=0 w=0
};
static_assert(sizeof(FsrCB) == 96, "FsrCB は 16 バイト単位でなければならない");

// D3DCompile が #include を解決するためのハンドラ (exe 目録を基準に読む)。
// CMake の POST_BUILD で ffx_a.h / ffx_fsr1.h を exe と同じ場所へコピーしている。
class FileInclude final : public ID3DInclude {
public:
    FileInclude() = default;
    // ID3DInclude には仮想デストラクタが無いため override は付けない
    ~FileInclude() {
        for (std::string* text : m_owned) delete text;
    }
    FileInclude(const FileInclude&) = delete;
    FileInclude& operator=(const FileInclude&) = delete;

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR fileName, LPCVOID,
                                   LPCVOID* data, UINT* bytes) override {
        if (!fileName || !data || !bytes) return E_INVALIDARG;
        const std::string path = moduleDirectory() + "\\" + fileName;
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            log::info("VideoRenderer: FSR インクルードが見つからない: %s", fileName);
            return E_FAIL;
        }
        auto* text =
            new std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        m_owned.push_back(text);
        *data = text->data();
        *bytes = static_cast<UINT>(text->size());
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID) override { return S_OK; }

private:
    static std::string moduleDirectory() {
        wchar_t buffer[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) return std::string();
        std::wstring path(buffer, length);
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) path.resize(slash);

        std::string narrow;
        const int converted = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0,
                                                  nullptr, nullptr);
        if (converted > 1) {
            narrow.resize(static_cast<std::size_t>(converted - 1));
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, narrow.data(), converted, nullptr,
                                nullptr);
        }
        return narrow;
    }

    std::vector<std::string*> m_owned;
};

// -----------------------------------------------------------------------------
//  HLSL コンパイル結果のディスクキャッシュ (Phase 1 の 100ms 予算を安定させる)
// -----------------------------------------------------------------------------
//  D3DCompile (O3) は mainVS / mainPS / blendPS の 3 枚で合計 ~90ms を要し、
//  ウィンドウ生成 + D3D11 + ImGui と並行しても Phase 1 の律速になっていた
//  (実測 89〜92ms で予算 100ms の余裕がほとんど無い)。
//  バイト列はソースとフラグが同じなら常に同一なので、初回だけコンパイルして
//  %APPDATA%\NanoAlphaPlayer\shaders.bin へ保存し、2 回目以降は読み込みだけで済む。
//  キャッシュが無い・壊れている場合は黙って D3DCompile へ戻る (再生には影響しない)。
constexpr std::uint32_t kShaderCacheMagic = 0x53474541u; // "AEGS"
constexpr std::uint32_t kShaderCacheVersion = 3u;

std::uint64_t hashShaderSource() {
    std::uint64_t hash = 14695981039346656037ull; // FNV-1a
    for (const char* p = kShaderSource; *p != '\0'; ++p) {
        hash ^= static_cast<std::uint8_t>(*p);
        hash *= 1099511628211ull;
    }
    // コンパイルフラグ (OPTIMIZATION_LEVEL3 / STRICTNESS) の変更でも無効化する
    hash ^= (static_cast<std::uint64_t>(D3DCOMPILE_ENABLE_STRICTNESS) << 32) |
            static_cast<std::uint64_t>(D3DCOMPILE_OPTIMIZATION_LEVEL3);
    return hash;
}

std::wstring shaderCachePath() {
    wchar_t base[1024]{};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", base, 1024);
    if (length == 0 || length >= 1024) return std::wstring();
    std::wstring directory(base);
    directory += L"\\NanoAlphaPlayer";
    CreateDirectoryW(directory.c_str(), nullptr);
    directory += L"\\shaders.bin";
    return directory;
}

// キャッシュを読み込み 3 blob に張り付ける。vs/ps が揃っているときだけ true。
// blend は無くてもよい (Frame Generation 用は失敗容赦だが)。
bool loadShaderCache(Microsoft::WRL::ComPtr<ID3DBlob>& vs, Microsoft::WRL::ComPtr<ID3DBlob>& ps,
                     Microsoft::WRL::ComPtr<ID3DBlob>& blend) {
    const std::wstring path = shaderCachePath();
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    auto readU32 = [&file](std::uint32_t& value) {
        file.read(reinterpret_cast<char*>(&value), sizeof(value));
        return static_cast<bool>(file);
    };

    std::uint32_t magic = 0, version = 0, flags = 0;
    if (!readU32(magic) || !readU32(version) || !readU32(flags)) return false;
    if (magic != kShaderCacheMagic || version != kShaderCacheVersion) return false;
    if ((flags & 0x3u) != 0x3u) return false; // vs / ps は必須

    std::uint64_t hash = 0;
    file.read(reinterpret_cast<char*>(&hash), sizeof(hash));
    if (!file || hash != hashShaderSource()) return false;

    ID3DBlob* blobs[3] = {nullptr, nullptr, nullptr};
    const int count = 3;
    for (int i = 0; i < count; ++i) {
        if ((flags & (1u << i)) == 0) continue;
        std::uint32_t size = 0;
        if (!readU32(size) || size == 0 || size > (64u << 20)) break;
        ID3DBlob* blob = nullptr;
        if (FAILED(D3DCreateBlob(size, &blob))) break;
        file.read(static_cast<char*>(blob->GetBufferPointer()), size);
        if (!file) {
            blob->Release();
            break;
        }
        blobs[i] = blob;
    }

    if (blobs[0] == nullptr || blobs[1] == nullptr) {
        for (ID3DBlob* blob : blobs) {
            if (blob) blob->Release();
        }
        return false;
    }
    vs.Attach(blobs[0]);
    ps.Attach(blobs[1]);
    blend.Attach(blobs[2]);
    return true;
}

void saveShaderCache(ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* blend) {
    if (!vs || !ps) return;
    const std::wstring path = shaderCachePath();
    if (path.empty()) return;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return;

    const std::uint32_t magic = kShaderCacheMagic;
    const std::uint32_t version = kShaderCacheVersion;
    const std::uint32_t flags = 0x3u | (blend ? 0x4u : 0u);
    const std::uint64_t hash = hashShaderSource();
    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
    file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
    ID3DBlob* blobs[3] = {vs, ps, blend};
    for (ID3DBlob* blob : blobs) {
        if (!blob) continue;
        const std::uint32_t size = static_cast<std::uint32_t>(blob->GetBufferSize());
        file.write(reinterpret_cast<const char*>(&size), sizeof(size));
        file.write(static_cast<const char*>(blob->GetBufferPointer()), size);
    }
    file.flush();
}

HRESULT compileShader(const char* entry, const char* profile, ID3DBlob** outBlob,
                      const char* source = kShaderSource,
                      const char* sourceName = "nanoalpha_video.hlsl",
                      ID3DInclude* include = nullptr) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#endif
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr =
        D3DCompile(source, std::strlen(source), sourceName, nullptr, include, entry, profile,
                   flags, 0, &blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            log::info("VideoRenderer: HLSL コンパイル失敗 (%s): %s", entry,
                      static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            log::info("VideoRenderer: HLSL コンパイル失敗 (%s) hr=0x%08lX", entry,
                      static_cast<unsigned long>(hr));
        }
        return hr;
    }
    *outBlob = blob.Detach();
    return hr;
}


UINT flagsForFlip(PlayerController::Flip flip, bool tenBit, bool limited) {
    UINT flags = 0;
    if (flip == PlayerController::Flip::Horizontal) flags |= 1u;
    else if (flip == PlayerController::Flip::Vertical) flags |= 2u;
    else if (flip == PlayerController::Flip::Both) flags |= 3u;
    if (tenBit) flags |= 4u;
    if (limited) flags |= 8u;
    return flags;
}

double aspectFor(PlayerController::Aspect aspect, UINT width, UINT height) {
    const double source = (height > 0) ? static_cast<double>(width) / height : 16.0 / 9.0;
    switch (aspect) {
    case PlayerController::Aspect::R16x9: return 16.0 / 9.0;
    case PlayerController::Aspect::R4x3: return 4.0 / 3.0;
    case PlayerController::Aspect::R21x9: return 21.0 / 9.0;
    case PlayerController::Aspect::R1x1: return 1.0;
    case PlayerController::Aspect::Auto:
    default: return source;
    }
}

constexpr float kPi = 3.14159265358979f;

} // namespace

// FSR 用シェーダーをバックグラウンドでコンパイルする (起動経路から分離するため)。
VideoRenderer::FsrBlobs VideoRenderer::compileFsrShaders() {
    FsrBlobs result;
    FileInclude include;
    ID3DBlob* easu = nullptr;
    ID3DBlob* rcas = nullptr;
    const HRESULT hr1 = compileShader("easuPS", "ps_5_0", &easu, kFsrHlsl, "nanoalpha_fsr.hlsl",
                                      &include);
    const HRESULT hr2 = compileShader("rcasPS", "ps_5_0", &rcas, kFsrHlsl, "nanoalpha_fsr.hlsl",
                                      &include);
    if (FAILED(hr1) || FAILED(hr2)) {
        if (easu) easu->Release();
        if (rcas) rcas->Release();
        return result;
    }
    result.easu.Attach(easu);
    result.rcas.Attach(rcas);
    result.ok = true;
    return result;
}

bool VideoRenderer::compile() noexcept {
    if ((m_vertexShader && m_pixelShader) || (m_vsBlob && m_psBlob)) return true;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> blendBlob;

    const auto startedAt = std::chrono::steady_clock::now();
    if (loadShaderCache(vsBlob, psBlob, blendBlob)) {
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - startedAt)
                                 .count();
        log::info("VideoRenderer: HLSL シェーダをキャッシュから復元 (%lld ms)", ms);
    } else {
        ID3DBlob* vs = nullptr;
        ID3DBlob* ps = nullptr;
        ID3DBlob* blend = nullptr;
        if (FAILED(compileShader("mainVS", "vs_5_0", &vs))) return false;
        if (FAILED(compileShader("mainPS", "ps_5_0", &ps))) {
            if (vs) vs->Release();
            return false;
        }
        // Frame Generation 用シェーダは失敗しても再生経路には影響しない
        if (FAILED(compileShader("blendPS", "ps_5_0", &blend))) {
            log::info("VideoRenderer: Frame Generation シェーダが無いため補間は無効");
            blend = nullptr;
        }
        vsBlob.Attach(vs);
        psBlob.Attach(ps);
        blendBlob.Attach(blend);
        // 次回の起動では D3DCompile を省く (Phase 1 の律速だったため)
        saveShaderCache(vsBlob.Get(), psBlob.Get(), blendBlob.Get());
    }

    m_vsBlob = vsBlob;
    m_psBlob = psBlob;
    m_blendBlob = blendBlob;
    return true;
}

bool VideoRenderer::init(ID3D11Device* device, ID3D11DeviceContext* context) {
    shutdown();
    if (!device || !context) return false;
    m_device = device;
    m_context = context;

    if (!compile()) return false;

    HRESULT hr = device->CreateVertexShader(m_vsBlob->GetBufferPointer(),
                                            m_vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (SUCCEEDED(hr)) {
        hr = device->CreatePixelShader(m_psBlob->GetBufferPointer(), m_psBlob->GetBufferSize(),
                                       nullptr, &m_pixelShader);
    }
    if (SUCCEEDED(hr) && m_blendBlob) {
        device->CreatePixelShader(m_blendBlob->GetBufferPointer(),
                                  m_blendBlob->GetBufferSize(), nullptr, &m_blendShader);
        if (!m_blendShader) {
            log::info("VideoRenderer: Frame Generation ピクセルシェーダ生成に失敗");
        }
    }
    m_vsBlob.Reset();
    m_psBlob.Reset();
    m_blendBlob.Reset();
    if (FAILED(hr)) {
        log::info("VideoRenderer: シェーダ生成に失敗 hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    // スケーリング品質 (Linear / Point / Anisotropic)
    {
        const D3D11_FILTER filters[3] = {
            D3D11_FILTER_MIN_MAG_MIP_LINEAR,
            D3D11_FILTER_MIN_MAG_MIP_POINT,
            D3D11_FILTER_ANISOTROPIC,
        };
        for (int i = 0; i < 3; ++i) {
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = filters[i];
            sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxAnisotropy = 16;
            sampler.MaxLOD = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sampler, &m_samplers[i]))) return false;
        }
    }

    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = sizeof(VideoCB);
    buffer.Usage = D3D11_USAGE_DYNAMIC;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&buffer, nullptr, &m_constantBuffer))) return false;

    // FSR 用定数バッファ (b1)。生成に失敗しても FSR が使えないだけなので続行する。
    {
        D3D11_BUFFER_DESC fsr{};
        fsr.ByteWidth = sizeof(FsrCB);
        fsr.Usage = D3D11_USAGE_DYNAMIC;
        fsr.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        fsr.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&fsr, nullptr, &m_fsrBuffer))) {
            log::info("VideoRenderer: FSR 定数バッファ生成に失敗 (FSR 無効)");
            m_fsrBuffer.Reset();
        }
    }

    m_ready = true;
    log::info("VideoRenderer: 初期化 OK (HLSL NV12/P010 -> RGB, BT.709, 画質補正)");
    return true;
}

void VideoRenderer::shutdown() noexcept {
    m_fsrCompile = std::shared_future<FsrBlobs>(); // 未完了なら完了を待つ (std::async の性質)
    m_fsrRequested = false;
    m_fsrReady = false;
    m_fsrFailed = false;
    m_easuShader.Reset();
    m_rcasShader.Reset();
    m_fsrBuffer.Reset();
    m_fsrSrc = Target{};
    m_fsrDst = Target{};
    m_fsrSrcW = m_fsrSrcH = m_fsrDstW = m_fsrDstH = 0;
    m_prev = Target{};
    m_curr = Target{};
    m_havePrev = false;
    m_haveCurr = false;
    m_lastPts = 0.0;
    m_prevPts = 0.0;
    m_lastTexture = nullptr;
    m_lastArrayIndex = -1;
    m_frameInterval = 1.0 / 60.0;
    m_displayInterval = 1.0 / 60.0;
    m_lastDrawTime = {};
    m_srvCache.clear();
    m_constantBuffer.Reset();
    for (auto& s : m_samplers) s.Reset();
    m_blendShader.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_blendBlob.Reset();
    m_psBlob.Reset();
    m_vsBlob.Reset();
    m_ready = false;
    m_device = nullptr;
    m_context = nullptr;
}

VideoRenderer::SrvEntry* VideoRenderer::acquireSrvs(const VideoDecoder::FrameView& frame) {
    if (!frame.texture) return nullptr;

    const auto key = std::make_tuple(static_cast<const void*>(frame.texture), frame.arrayIndex,
                                     frame.format, frame.textureWidth, frame.textureHeight);
    const auto it = m_srvCache.find(key);
    if (it != m_srvCache.end()) return &it->second;

    D3D11_TEXTURE2D_DESC desc{};
    frame.texture->GetDesc(&desc);
    if (desc.ArraySize <= 1) {
        // 配列でない場合はスライス指定不可
        if (frame.arrayIndex != 0) return nullptr;
    }

    // --- BGRA (ソフトウェアデコード経路) ---
    // 単一の SRV を y / uv の両方にセットする (シェーダは bit5 で tY のみ読む)
    if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        SrvEntry entry{};
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = desc.Format;
        if (desc.ArraySize > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(frame.arrayIndex);
            srvDesc.Texture2DArray.ArraySize = 1;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        }
        const HRESULT hr = m_device->CreateShaderResourceView(frame.texture, &srvDesc,
                                                              entry.y.GetAddressOf());
        if (FAILED(hr)) {
            log::info("VideoRenderer: BGRA SRV 生成失敗 hr=0x%08lX",
                      static_cast<unsigned long>(hr));
            return nullptr;
        }
        entry.uv = entry.y;
        const auto inserted = m_srvCache.emplace(key, std::move(entry));
        log::info("VideoRenderer: SRV 生成 (BGRA) fmt=0x%08X %ux%u",
                  static_cast<unsigned long>(desc.Format), desc.Width, desc.Height);
        return &inserted.first->second;
    }

    const DXGI_FORMAT planeY = frame.tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT planeUV =
        frame.tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;

    SrvEntry entry{};
    const DXGI_FORMAT formats[2] = {planeY, planeUV};
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>* views[2] = {&entry.y, &entry.uv};

    for (int i = 0; i < 2; ++i) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = formats[i];
        if (desc.ArraySize > 1) {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = static_cast<UINT>(frame.arrayIndex);
            srvDesc.Texture2DArray.ArraySize = 1;
        } else {
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = 0;
            srvDesc.Texture2D.MipLevels = 1;
        }
        const HRESULT hr = m_device->CreateShaderResourceView(frame.texture, &srvDesc,
                                                              views[i]->GetAddressOf());
        if (FAILED(hr)) {
            log::info("VideoRenderer: SRV 生成失敗 (plane %d) hr=0x%08lX", i,
                      static_cast<unsigned long>(hr));
            return nullptr;
        }
    }

    const auto inserted = m_srvCache.emplace(key, std::move(entry));
    log::info("VideoRenderer: SRV 生成 resource=0x%08X %ux%u array=%u bind=0x%08X slice=%d",
              static_cast<unsigned long>(desc.Format), desc.Width, desc.Height, desc.ArraySize,
              static_cast<unsigned long>(desc.BindFlags), frame.arrayIndex);
    return &inserted.first->second;
}

namespace {

// 描画先 RT のピクセルサイズへビューポートを切り替え、破棄時に元へ戻すガード。
//
//  [v3 修正] FSR は中間 RT へ描画するが、従来はビューポートを更新していなかった。
//  ビューポートがバックスバッファ (= ウィンドウ) のままだと、クワッドがカバーする
//  範囲もウィンドウサイズのままになり、
//    - 入力 RT (映像の実寸) がウィンドウより小さいと
//      クワッドが RT でクリップされて「映像の左上隅だけ」が書き込まれ、
//      EASU がそれを全面に引き伸ばす → 画面外まで拡大されたように見える。
//    - 入力 RT がウィンドウより大きいと未書き込み領域が残り、
//      ギザギザ・破損が出る。
//  各パスの RT 実寸をビューポートに指定し、ラスタライズ範囲と RT を一致させる。
class ViewportScope {
public:
    ViewportScope(ID3D11DeviceContext* context, UINT width, UINT height)
        : m_context(context) {
        if (!m_context) return;
        UINT count = 1;
        m_context->RSGetViewports(&count, &m_saved);
        m_valid = (count >= 1);
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &viewport);
    }
    ~ViewportScope() {
        if (m_context && m_valid) m_context->RSSetViewports(1, &m_saved);
    }
    ViewportScope(const ViewportScope&) = delete;
    ViewportScope& operator=(const ViewportScope&) = delete;

private:
    ID3D11DeviceContext* m_context = nullptr;
    D3D11_VIEWPORT m_saved{};
    bool m_valid = false;
};

} // namespace

bool VideoRenderer::createTarget(Target& target, UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return false;
    target = Target{};
    D3D11_TEXTURE2D_DESC tex{};
    tex.Width = width;
    tex.Height = height;
    tex.MipLevels = 1;
    tex.ArraySize = 1;
    tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex.SampleDesc.Count = 1;
    tex.Usage = D3D11_USAGE_DEFAULT;
    tex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&tex, nullptr, &target.tex))) return false;
    if (FAILED(m_device->CreateRenderTargetView(target.tex.Get(), nullptr, &target.rtv)))
        return false;
    if (FAILED(m_device->CreateShaderResourceView(target.tex.Get(), nullptr, &target.srv)))
        return false;
    return true;
}

bool VideoRenderer::ensureTargets(UINT width, UINT height) {
    if (!m_device || width == 0 || height == 0) return false;
    if (m_prev.tex && m_curr.tex) {
        D3D11_TEXTURE2D_DESC desc{};
        m_prev.tex->GetDesc(&desc);
        if (desc.Width == width && desc.Height == height) return true;
    }

    if (!createTarget(m_prev, width, height) || !createTarget(m_curr, width, height)) {
        log::info("VideoRenderer: フレーム補間用 RT 生成に失敗 (%ux%u)", width, height);
        m_prev = Target{};
        m_curr = Target{};
        return false;
    }

    float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    m_context->ClearRenderTargetView(m_prev.rtv.Get(), clear);
    m_context->ClearRenderTargetView(m_curr.rtv.Get(), clear);
    m_havePrev = false; // 直後にブレンドしても黒が混ざらないよう 1 フレーム無効化
    log::info("VideoRenderer: フレーム補間用 RT を %ux%u で作成", width, height);
    return true;
}

// FSR 用の中間ターゲット (入力 = 映像の実寸 / 出力 = 表示矩形のピクセル数)
bool VideoRenderer::ensureFsrTargets(UINT srcWidth, UINT srcHeight, UINT dstWidth,
                                     UINT dstHeight) {
    if (!m_device || !m_context) return false;
    if (srcWidth == 0 || srcHeight == 0 || dstWidth == 0 || dstHeight == 0) return false;

    bool recreate = false;
    if (!m_fsrSrc.tex || !m_fsrDst.tex) recreate = true;
    if (m_fsrSrc.tex && (m_fsrSrcW != srcWidth || m_fsrSrcH != srcHeight)) recreate = true;
    if (m_fsrDst.tex && (m_fsrDstW != dstWidth || m_fsrDstH != dstHeight)) recreate = true;

    if (recreate) {
        if (!createTarget(m_fsrSrc, srcWidth, srcHeight) ||
            !createTarget(m_fsrDst, dstWidth, dstHeight)) {
            log::info("VideoRenderer: FSR 用 RT 生成に失敗 (%ux%u -> %ux%u)", srcWidth, srcHeight,
                      dstWidth, dstHeight);
            m_fsrSrc = Target{};
            m_fsrDst = Target{};
            m_fsrSrcW = m_fsrSrcH = m_fsrDstW = m_fsrDstH = 0;
            return false;
        }
        m_fsrSrcW = srcWidth;
        m_fsrSrcH = srcHeight;
        m_fsrDstW = dstWidth;
        m_fsrDstH = dstHeight;
        float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->ClearRenderTargetView(m_fsrSrc.rtv.Get(), clear);
        m_context->ClearRenderTargetView(m_fsrDst.rtv.Get(), clear);
        log::info("VideoRenderer: FSR 用 RT を %ux%u -> %ux%u で作成", srcWidth, srcHeight,
                  dstWidth, dstHeight);
    }
    return true;
}

void VideoRenderer::requestFsrCompile() {
    if (m_fsrRequested) return;
    m_fsrRequested = true;
    m_fsrCompile = std::async(std::launch::async, [] { return VideoRenderer::compileFsrShaders(); })
                       .share();
    log::info("VideoRenderer: FSR1 シェーダをバックグラウンドでコンパイル開始");
}

void VideoRenderer::pollFsrCompile() {
    if (m_fsrReady || m_fsrFailed || !m_fsrRequested) return;
    if (!m_fsrCompile.valid()) return;
    if (m_fsrCompile.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;

    const FsrBlobs blobs = m_fsrCompile.get();
    if (!blobs.ok || !blobs.easu || !blobs.rcas || !m_device) {
        m_fsrFailed = true;
        log::info("VideoRenderer: FSR1 シェーダのコンパイルに失敗 -> FSR 無効");
        return;
    }

    HRESULT hr = m_device->CreatePixelShader(blobs.easu->GetBufferPointer(),
                                             blobs.easu->GetBufferSize(), nullptr, &m_easuShader);
    if (SUCCEEDED(hr)) {
        hr = m_device->CreatePixelShader(blobs.rcas->GetBufferPointer(),
                                         blobs.rcas->GetBufferSize(), nullptr, &m_rcasShader);
    }
    if (FAILED(hr) || !m_easuShader || !m_rcasShader) {
        m_easuShader.Reset();
        m_rcasShader.Reset();
        m_fsrFailed = true;
        log::info("VideoRenderer: FSR1 ピクセルシェーダ生成に失敗 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return;
    }
    m_fsrReady = true;
    log::info("VideoRenderer: FSR1 (EASU+RCAS) 準備完了");
}

void VideoRenderer::updateFsrBuffer(const std::uint32_t (&con0)[4],
                                    const std::uint32_t (&con1)[4],
                                    const std::uint32_t (&con2)[4],
                                    const std::uint32_t (&con3)[4],
                                    const std::uint32_t (&rcas)[4], int originX, int originY,
                                    int width, int height) {
    if (!m_fsrBuffer || !m_context) return;
    FsrCB data{};
    for (int i = 0; i < 4; ++i) {
        data.con0[i] = con0[i];
        data.con1[i] = con1[i];
        data.con2[i] = con2[i];
        data.con3[i] = con3[i];
        data.rcas[i] = rcas[i];
    }
    data.rcas[1] = static_cast<std::uint32_t>(originX);
    data.rcas[2] = static_cast<std::uint32_t>(originY);
    data.rcas[3] = 0;
    data.size[0] = static_cast<std::uint32_t>(width);
    data.size[1] = static_cast<std::uint32_t>(height);
    data.size[2] = 0;
    data.size[3] = 0;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_fsrBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    std::memcpy(mapped.pData, &data, sizeof(data));
    m_context->Unmap(m_fsrBuffer.Get(), 0);
}

void VideoRenderer::drawVideo(const VideoDecoder::FrameView& frame,
                              const PlayerController::Snapshot& control, const RECT& area,
                              UINT rtWidth, UINT rtHeight) {
    if (!m_ready || !frame.texture || rtWidth == 0 || rtHeight == 0) return;

    SrvEntry* srvs = acquireSrvs(frame);
    if (!srvs || !srvs->y || !srvs->uv) return;

    const LONG areaWidth = area.right - area.left;
    const LONG areaHeight = area.bottom - area.top;
    if (areaWidth <= 0 || areaHeight <= 0) return;

    // --- 目的矩形 (映像領域内でレターボックス / ピラーーボックス) ---
    UINT srcWidth = frame.width;
    UINT srcHeight = frame.height;
    const int rotation = (control.rotationDeg / 90) & 3;
    if ((rotation & 1) != 0) {
        std::swap(srcWidth, srcHeight); // 90 / 270 度は縦横が入れ替わる
    }
    const double aspect = aspectFor(control.aspect, srcWidth, srcHeight);
    const double areaAspect = static_cast<double>(areaWidth) / static_cast<double>(areaHeight);

    double destWidth = 1.0;
    double destHeight = 1.0;
    if (aspect > areaAspect) {
        destHeight = areaAspect / aspect; // 上下に黒帯
    } else {
        destWidth = aspect / areaAspect;  // 左右に黒帯
    }

    // ラスタライズ範囲と厳密に一致させるため、まず整数ピクセル矩形へ丸める。
    // (FSR の RCAS が原点オフセットで読むため、境界の 1px ずれが命取りになる)
    int ox = area.left + static_cast<int>(std::lround((1.0 - destWidth) * 0.5 * areaWidth));
    int oy = area.top + static_cast<int>(std::lround((1.0 - destHeight) * 0.5 * areaHeight));
    int ex = ox + std::max(1, static_cast<int>(std::lround(destWidth * areaWidth)));
    int ey = oy + std::max(1, static_cast<int>(std::lround(destHeight * areaHeight)));
    if (ox < 0) ox = 0;
    if (oy < 0) oy = 0;
    if (ex > static_cast<int>(rtWidth)) ex = static_cast<int>(rtWidth);
    if (ey > static_cast<int>(rtHeight)) ey = static_cast<int>(rtHeight);
    if (ex <= ox) ex = std::min(static_cast<int>(rtWidth), ox + 1);
    if (ey <= oy) ey = std::min(static_cast<int>(rtHeight), oy + 1);
    if (ex <= ox || ey <= oy) return;

    const float nx0 = static_cast<float>(ox) / static_cast<float>(rtWidth);
    const float ny0 = static_cast<float>(oy) / static_cast<float>(rtHeight);
    const float nx1 = static_cast<float>(ex) / static_cast<float>(rtWidth);
    const float ny1 = static_cast<float>(ey) / static_cast<float>(rtHeight);

    int samplerIndex = 0;
    if (control.scaleFilter == PlayerController::ScaleFilter::Point) samplerIndex = 1;
    else if (control.scaleFilter == PlayerController::ScaleFilter::Anisotropic) samplerIndex = 2;

    // 常にレンダーターゲット実寸へビューポートを合わせておく (後述の FSR 中間 RT も同じ考え方)。
    const ViewportScope baseViewport(m_context, rtWidth, rtHeight);

    // ---- v3 FSR: 拡大表示のときだけ 3 パスに通す (縮小は従来経路へフォールバック) ----
    const UINT dstWidth = static_cast<UINT>(ex - ox);
    const UINT dstHeight = static_cast<UINT>(ey - oy);
    bool useFsr = false;
    if (control.scaleFilter == PlayerController::ScaleFilter::Fsr &&
        dstWidth > srcWidth && dstHeight > srcHeight) {
        requestFsrCompile(); // 初回のみ起動 (それ以降はノーパス)
        pollFsrCompile();
        if (m_fsrReady && m_fsrBuffer) {
            useFsr = ensureFsrTargets(srcWidth, srcHeight, dstWidth, dstHeight);
        }
    }

    if (!useFsr) {
        drawVideoQuad(frame, control, *srvs, nx0, ny0, nx1, ny1, samplerIndex);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> original;
    m_context->OMGetRenderTargets(1, &original, nullptr);
    if (!original) { // 描画先が無い (あり得ないが安全側)
        drawVideoQuad(frame, control, *srvs, nx0, ny0, nx1, ny1, samplerIndex);
        return;
    }

    // v3: 寸法が変わったときだけ記録する (毎フレームは流石に多すぎる)。
    //  pass1 のビューポートが「ウィンドウ実寸」ではなく FSR 中間 RT の実寸で
    //  あることをテストから確認するため (画面外まで拡大される不具合の原因)。
    if (m_fsrLogSrcW != m_fsrSrcW || m_fsrLogSrcH != m_fsrSrcH ||
        m_fsrLogDstW != m_fsrDstW || m_fsrLogDstH != m_fsrDstH) {
        m_fsrLogSrcW = m_fsrSrcW;
        m_fsrLogSrcH = m_fsrSrcH;
        m_fsrLogDstW = m_fsrDstW;
        m_fsrLogDstH = m_fsrDstH;
        log::info("VideoRenderer: FSR viewport easu=%ux%u -> %ux%u (rt=%ux%u)", m_fsrSrcW,
                  m_fsrSrcH, m_fsrDstW, m_fsrDstH, rtWidth, rtHeight);
    }

    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    // ① mainPS → 入力 RT (映像の実寸)。レターボックスは作らない。
    {
        const ViewportScope viewport(m_context, m_fsrSrcW, m_fsrSrcH);
        ID3D11RenderTargetView* target = m_fsrSrc.rtv.Get();
        m_context->OMSetRenderTargets(1, &target, nullptr);
        drawVideoQuad(frame, control, *srvs, 0.0f, 0.0f, 1.0f, 1.0f, 0);
    }

    // ② EASU → 出力解像度の中間 RT
    std::uint32_t con0[4]{};
    std::uint32_t con1[4]{};
    std::uint32_t con2[4]{};
    std::uint32_t con3[4]{};
    std::uint32_t rcas[4]{};
    FsrEasuCon(con0, con1, con2, con3, static_cast<float>(srcWidth),
               static_cast<float>(srcHeight), static_cast<float>(m_fsrSrcW),
               static_cast<float>(m_fsrSrcH), static_cast<float>(m_fsrDstW),
               static_cast<float>(m_fsrDstH));
    FsrRcasCon(rcas, 0.0f); // 0.0 = 最大シャープネス
    updateFsrBuffer(con0, con1, con2, con3, rcas, ox, oy, static_cast<int>(dstWidth),
                    static_cast<int>(dstHeight));

    m_context->ClearRenderTargetView(m_fsrDst.rtv.Get(), clear);
    {
        // ② EASU: 中間 RT (= 表示矩形の実寸) へ描画するので、その実寸でラスタライズする
        const ViewportScope viewport(m_context, m_fsrDstW, m_fsrDstH);
        ID3D11RenderTargetView* target = m_fsrDst.rtv.Get();
        m_context->OMSetRenderTargets(1, &target, nullptr);
        if (!uploadVideoCb(frame, control, 0.0f, 0.0f, 1.0f, 1.0f)) {
            m_context->OMSetRenderTargets(1, original.GetAddressOf(), nullptr);
            return;
        }
        ID3D11Buffer* buffers[2] = {m_constantBuffer.Get(), m_fsrBuffer.Get()};
        ID3D11ShaderResourceView* resource = m_fsrSrc.srv.Get();
        ID3D11SamplerState* sampler = m_samplers[0].Get(); // Linear + Clamp
        m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        m_context->PSSetShader(m_easuShader.Get(), nullptr, 0);
        m_context->VSSetConstantBuffers(0, 1, buffers);
        m_context->PSSetConstantBuffers(0, 2, buffers);
        m_context->PSSetShaderResources(0, 1, &resource);
        m_context->PSSetSamplers(0, 1, &sampler);
        m_context->IASetInputLayout(nullptr);
        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_context->Draw(4, 0);
        ID3D11ShaderResourceView* unbind[2] = {nullptr, nullptr};
        m_context->PSSetShaderResources(0, 2, unbind);
    }

    // ③ RCAS → 元の描画先の目的矩形へ (座標はビューポート基準なので RT 実寸に戻す)
    m_context->OMSetRenderTargets(1, original.GetAddressOf(), nullptr);
    if (!uploadVideoCb(frame, control, nx0, ny0, nx1, ny1)) return;

    ID3D11Buffer* buffers[2] = {m_constantBuffer.Get(), m_fsrBuffer.Get()};
    ID3D11ShaderResourceView* resource = m_fsrDst.srv.Get();
    ID3D11SamplerState* sampler = m_samplers[0].Get();
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_rcasShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, buffers);
    m_context->PSSetConstantBuffers(0, 2, buffers);
    m_context->PSSetShaderResources(0, 1, &resource);
    m_context->PSSetSamplers(0, 1, &sampler);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->Draw(4, 0);

    ID3D11ShaderResourceView* unbind[2] = {nullptr, nullptr};
    m_context->PSSetShaderResources(0, 2, unbind);
}

// VideoCB を目的矩形つきで組み立てて書き込む
bool VideoRenderer::uploadVideoCb(const VideoDecoder::FrameView& frame,
                                  const PlayerController::Snapshot& control, float x0, float y0,
                                  float x1, float y1) {
    if (!m_constantBuffer || !m_context) return false;

    bool limited = frame.limitedRange;
    if (control.rangeMode == PlayerController::RangeMode::Limited) limited = true;
    else if (control.rangeMode == PlayerController::RangeMode::Full) limited = false;

    VideoCB data{};
    data.dest[0] = x0;
    data.dest[1] = y0;
    data.dest[2] = x1;
    data.dest[3] = y1;
    data.crop[0] = control.crop[0];
    data.crop[1] = control.crop[1];
    data.crop[2] = control.crop[2];
    data.crop[3] = control.crop[3];
    data.rotation = static_cast<std::uint32_t>((control.rotationDeg / 90) & 3);
    data.flags = flagsForFlip(control.flip, frame.tenBit, limited);
    // BGRA (ソフトウェアデコード経路) → bit5
    if (frame.format == DXGI_FORMAT_B8G8R8A8_UNORM ||
        frame.format == DXGI_FORMAT_R8G8B8A8_UNORM) {
        data.flags |= 32u;
    }
    // デコーダのパディング (例 1080 -> 1152) を UV で補正する
    data.uvScale[0] = (frame.textureWidth > 0)
                          ? static_cast<float>(frame.width) / static_cast<float>(frame.textureWidth)
                          : 1.0f;
    data.uvScale[1] =
        (frame.textureHeight > 0)
            ? static_cast<float>(frame.height) / static_cast<float>(frame.textureHeight)
            : 1.0f;
    data.adjust[0] = control.brightness;
    data.adjust[1] = control.contrast;
    data.adjust[2] = control.saturation;
    data.adjust[3] = control.hueDeg * (kPi / 180.0f);
    data.adjust2[0] = control.sharpness;
    data.adjust2[1] = (frame.textureWidth > 0) ? 1.0f / static_cast<float>(frame.textureWidth) : 0.0f;
    data.adjust2[2] = (frame.textureHeight > 0) ? 1.0f / static_cast<float>(frame.textureHeight) : 0.0f;
    data.adjust2[3] = 1.0f;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &data, sizeof(data));
    m_context->Unmap(m_constantBuffer.Get(), 0);
    return true;
}

// mainPS を指定した正規化矩形へ描画する (1 パス)
void VideoRenderer::drawVideoQuad(const VideoDecoder::FrameView& frame,
                                  const PlayerController::Snapshot& control, const SrvEntry& srvs,
                                  float x0, float y0, float x1, float y1, int samplerIndex) {
    if (!uploadVideoCb(frame, control, x0, y0, x1, y1)) return;
    if (samplerIndex < 0 || samplerIndex > 2) samplerIndex = 0;

    ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
    ID3D11ShaderResourceView* resources[2] = {srvs.y.Get(), srvs.uv.Get()};
    ID3D11SamplerState* sampler = m_samplers[samplerIndex].Get();

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, &constantBuffer);
    m_context->PSSetConstantBuffers(0, 1, &constantBuffer);
    m_context->PSSetShaderResources(0, 2, resources);
    m_context->PSSetSamplers(0, 1, &sampler);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->Draw(4, 0);

    // デコーダが次のフレームで書けるよう必ずアンバインドする
    ID3D11ShaderResourceView* unbind[2] = {nullptr, nullptr};
    m_context->PSSetShaderResources(0, 2, unbind);
}

void VideoRenderer::blendPass(float blend) {
    if (!m_blendShader || !m_prev.srv || !m_curr.srv) return;

    VideoCB data{};
    data.dest[0] = 0.0f;
    data.dest[1] = 0.0f;
    data.dest[2] = 1.0f;
    data.dest[3] = 1.0f;
    data.uvScale[0] = 1.0f;
    data.uvScale[1] = 1.0f;
    data.adjust[1] = 1.0f; // コントラスト = 1 (パススルー)
    data.adjust[2] = 1.0f; // 彩度 = 1
    data.adjust2[3] = blend;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return;
    std::memcpy(mapped.pData, &data, sizeof(data));
    m_context->Unmap(m_constantBuffer.Get(), 0);

    ID3D11Buffer* constantBuffer = m_constantBuffer.Get();
    ID3D11ShaderResourceView* resources[2] = {m_prev.srv.Get(), m_curr.srv.Get()};
    ID3D11SamplerState* sampler = m_samplers[0].Get();

    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_blendShader.Get(), nullptr, 0);
    m_context->VSSetConstantBuffers(0, 1, &constantBuffer);
    m_context->PSSetConstantBuffers(0, 1, &constantBuffer);
    m_context->PSSetShaderResources(0, 2, resources);
    m_context->PSSetSamplers(0, 1, &sampler);
    m_context->IASetInputLayout(nullptr);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    m_context->Draw(4, 0);

    ID3D11ShaderResourceView* unbind[2] = {nullptr, nullptr};
    m_context->PSSetShaderResources(0, 2, unbind);
}

void VideoRenderer::draw(const VideoDecoder::FrameView& frame,
                         const PlayerController::Snapshot& control, UINT renderTargetWidth,
                         UINT renderTargetHeight, const RECT& videoArea, double mediaTime) {
    if (!m_ready || !frame.texture || renderTargetWidth == 0 || renderTargetHeight == 0) return;

    // 描画間隔の EMA: 「目標倍率 N」が表示フレーム数より小さいときだけ
    // N 分割へ丸める判断に使う (詳細は interpolationWeight)。
    m_frameGenFactor = control.frameGenFactor;
    {
        const auto now = std::chrono::steady_clock::now();
        if (m_lastDrawTime.time_since_epoch().count() != 0) {
            const double dt =
                std::chrono::duration<double>(now - m_lastDrawTime).count();
            if (dt > 1.0e-4 && dt < 0.5) {
                m_displayInterval += (dt - m_displayInterval) * 0.1;
            }
        }
        m_lastDrawTime = now;
    }

    // ---- 描画領域 (UI を避けた映像矩形)。指定が無ければ全面 ----
    RECT area = videoArea;
    area.left = std::max<LONG>(0, area.left);
    area.top = std::max<LONG>(0, area.top);
    area.right = std::min<LONG>(static_cast<LONG>(renderTargetWidth), area.right);
    area.bottom = std::min<LONG>(static_cast<LONG>(renderTargetHeight), area.bottom);
    if (area.right - area.left <= 0 || area.bottom - area.top <= 0) {
        area.left = 0;
        area.top = 0;
        area.right = static_cast<LONG>(renderTargetWidth);
        area.bottom = static_cast<LONG>(renderTargetHeight);
    }

    // ---- Frame Generation 無効: 従来どおり現在のレンダーターゲットへ直描き ----
    const bool frameGenAvailable = control.frameGen && m_blendShader;
    if (!frameGenAvailable) {
        m_havePrev = false;
        m_haveCurr = false;
        drawVideo(frame, control, area, renderTargetWidth, renderTargetHeight);
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> backbuffer;
    m_context->OMGetRenderTargets(1, &backbuffer, nullptr);
    if (!backbuffer || !ensureTargets(renderTargetWidth, renderTargetHeight)) {
        m_havePrev = false;
        m_haveCurr = false;
        drawVideo(frame, control, area, renderTargetWidth, renderTargetHeight);
        return;
    }

    // ---- v3: 新しいフレームをデコードした時だけ prev/curr を入れ替える ----
    // 旧実装は毎フレーム swap していたため、デコード更新直後の 1 表示フレームだけ
    // 補間し、以降は prev == curr でブレンドが無意味になっていた (体感できない原因)。
    const bool newFrame = !m_haveCurr || frame.pts != m_lastPts ||
                          frame.texture != m_lastTexture ||
                          frame.arrayIndex != m_lastArrayIndex;
    if (newFrame) {
        const bool haveValidPrev = m_haveCurr;
        bool cut = false;
        if (haveValidPrev) {
            const double delta = frame.pts - m_lastPts;
            const double gap = (delta < 0.0) ? -delta : delta;
            if (delta > 0.0 && delta < 1.0) {
                m_frameInterval += (delta - m_frameInterval) * 0.1; // 間隔の EMA
            }
            double interval = m_frameInterval;
            if (interval < 1.0 / 240.0) interval = 1.0 / 240.0;
            // シーク / カットで_pts が大きく飛んだら補間を打ち切る
            const double limit = (std::max)(0.5, 3.0 * interval);
            if (gap > limit) cut = true;
        }

        std::swap(m_prev, m_curr);
        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        m_context->ClearRenderTargetView(m_curr.rtv.Get(), clear);
        ID3D11RenderTargetView* target = m_curr.rtv.Get();
        m_context->OMSetRenderTargets(1, &target, nullptr);
        drawVideo(frame, control, area, renderTargetWidth, renderTargetHeight);
        m_context->OMSetRenderTargets(1, backbuffer.GetAddressOf(), nullptr);

        m_prevPts = m_lastPts; // 補間の基準点 (直前のフレーム)
        m_haveCurr = true;
        m_havePrev = haveValidPrev && !cut;
        m_lastPts = frame.pts;
        m_lastTexture = frame.texture;
        m_lastArrayIndex = frame.arrayIndex;
    }

    // 同じフレームが続く表示フレームでは重みだけを進めて連続補間する。
    // 一時停止中は位相が止まったまま混ざるのを避けて 1.0 固定にする。
    const bool interpolate = m_havePrev && !control.paused;
    blendPass(interpolate ? interpolationWeight(mediaTime) : 1.0f);
}

// 媒体クロック mediaTime に対する補間重み (0..1)。
//
//  構成: 保持している 2 フレームは prev (m_prevPts) と curr (m_lastPts)。
//  新しいフレームが届いた瞬間 t ≈ m_lastPts で重み 0 (= prev 表示) から始まり、
//  次のフレームが届くまでに 1.0 (= curr 表示) へ到達する。
//  サイズも時刻も既知なので、間隔は「fps 一定」と仮定せず実測値 (m_lastPts - m_prevPts) を使う。
//  これにより VFR やクロック揺らぎでも境界で連続、かつ量子化されない。
float VideoRenderer::interpolationWeight(double mediaTime) const noexcept {
    if (!m_haveCurr) return 1.0f;
    double span = m_lastPts - m_prevPts;
    if (!(span > 1.0e-5) || span > 0.5) { // 初回・シーク直後は EMA の値で代用
        span = (m_frameInterval > 1.0e-5 && m_frameInterval < 0.5) ? m_frameInterval
                                                                   : 1.0 / 60.0;
    }
    double weight = (mediaTime - m_lastPts) / span;
    if (!(weight >= 0.0)) weight = 0.0; // NaN / 負
    if (weight > 1.0) weight = 1.0;

    // 目標倍率 (2/4/8/16) は「出力をソースの N 倍にする」ための制約。
    // 1 ソースフレームに現れる表示フレーム数より N が大きいときは表示が追いつかないので
    // 丸めない (連続補間が最善)。逆に N のほうが少ないときだけ N 分割へ落とす。
    // → 表示レートと倍率が一致しないまま常に丸めていた旧実装の
    //   位相の跳ね (歩調ズレ) を避けつつ、「2/4 倍」の指定は守られる。
    const int factor = m_frameGenFactor;
    if (factor >= 2 && m_displayInterval > 1.0e-4) {
        const double perSourceFrame = span / m_displayInterval;
        if (static_cast<double>(factor) + 0.25 < perSourceFrame) {
            weight = std::floor(weight * factor + 0.5) / factor;
            if (!(weight >= 0.0)) weight = 0.0;
            if (weight > 1.0) weight = 1.0;
        }
    }
    return static_cast<float>(weight);
}

} // namespace nanoalpha
