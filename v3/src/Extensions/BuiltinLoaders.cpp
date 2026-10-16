#include "Extensions/BuiltinLoaders.hpp"

#include "Core/Log.hpp"
#include "Core/VideoDecoder.hpp"
#include "Extensions/MediaTools.hpp"
#include "Extensions/OpenRgbClient.hpp"

#include <windows.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

// CMake からは NANOALPHA_ENABLE_xxx = 0/1 が必ず定義される (素のコンパイル時の既定値)
#ifndef NANOALPHA_ENABLE_FRAMEGEN
#define NANOALPHA_ENABLE_FRAMEGEN 1
#endif
#ifndef NANOALPHA_ENABLE_OPENRGB
#define NANOALPHA_ENABLE_OPENRGB 1
#endif
#ifndef NANOALPHA_ENABLE_DISC
#define NANOALPHA_ENABLE_DISC 1
#endif
#ifndef NANOALPHA_ENABLE_NETWORK
#define NANOALPHA_ENABLE_NETWORK 1
#endif
#ifndef NANOALPHA_HAS_FFMPEG
#define NANOALPHA_HAS_FFMPEG 0
#endif

#if NANOALPHA_HAS_FFMPEG
extern "C" {
#include <libavformat/avio.h>
}
#endif

namespace nanoalpha::extensions {
namespace {

using namespace std::chrono_literals;

// =============================================================================
//  Phase 2 ローダ: ネットワーク配信 (対応プロトコルの実列挙)
// -----------------------------------------------------------------------------
//  avio_enum_protocols() は FFmpeg が実際にハンドルするプロトコルを列挙する。
//  これが列挙できたプロトコルはそのまま avformat_open_input() で開ける
//  (main.cpp の「URL 開く」がそのまま使う)。
// =============================================================================
struct NetworkCapabilities {
    std::vector<std::string> protocols;
    bool ffmpeg = false;
};

LoadResult loadNetwork(const LoadContext& ctx) {
    auto caps = std::make_shared<NetworkCapabilities>();

    // 起動引数 / UI の URL 欄が扱う schemes のうち、FFmpeg が保持していないもの
    static const char* kFallback[] = {"http", "https", "tcp", "udp", "rtp",  "rtsp",
                                      "rtmp", "ws",   "ftp",  "smb", "file"};
    for (const char* proto : kFallback) {
        if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");
        if (ctx.expired()) return LoadResult::timeout("ロード予算を超過");
        caps->protocols.emplace_back(proto);
    }

#if NANOALPHA_HAS_FFMPEG
    caps->ffmpeg = true;
    caps->protocols.clear();
    void* opaque = nullptr;
    for (const char* proto = avio_enum_protocols(&opaque, 0); proto != nullptr;
         proto = avio_enum_protocols(&opaque, 0)) {
        if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");
        if (ctx.expired()) return LoadResult::timeout("ロード予算を超過");
        caps->protocols.emplace_back(proto);
    }
#endif

    if (caps->protocols.empty()) {
        return LoadResult::error("利用可能なネットワークプロトコルがありません");
    }

    std::string summary;
    for (size_t i = 0; i < caps->protocols.size() && i < 10; ++i) {
        if (!summary.empty()) summary += " ";
        summary += caps->protocols[i];
    }
    if (caps->protocols.size() > 10) summary += " ...";

    return LoadResult::success(
        caps, "対応プロトコル " + std::to_string(caps->protocols.size()) + " 種 (" +
                  (caps->ffmpeg ? "FFmpeg 実測: " : "内定: ") + summary + ")");
}

// =============================================================================
//  Phase 2 ローダ: VLC 機能群 (プレイリスト / スナップショット / 壁紙)
// -----------------------------------------------------------------------------
//  実体は MediaTools.hpp の VlcFeatures。ここで唯一のインスタンスを作り、
//  ReadyHook が main 側へ受け取ってスナップショット / 壁紙操作に使う。
// =============================================================================
LoadResult loadVlcFeatures(const LoadContext& ctx) {
    if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");

    auto features = std::make_shared<tools::VlcFeatures>();

    // 壁紙 API が実際に呼べるか (引数 0 の問い合わせのみ・設定はしない)
    wchar_t currentWallpaper[MAX_PATH] = {};
    const BOOL wallpaperApi = ::SystemParametersInfoW(SPI_GETDESKWALLPAPER,
                                                      static_cast<UINT>(MAX_PATH),
                                                      currentWallpaper, 0);
    if (!wallpaperApi) {
        return LoadResult::error("壁紙 API (SystemParametersInfo) が利用できません");
    }

    return LoadResult::success(
        features, std::string("利用可能: ") + tools::VlcFeatures::kCapabilities +
                      " / 未対応: " + tools::VlcFeatures::kNotImplemented);
}

// =============================================================================
//  Phase 2 ローダ: Frame Generation (フレーム補間パスの検証)
// -----------------------------------------------------------------------------
//  HLSL の補間カーネルをコンパイルし、D3D11 の実ディスパッチで
//  lerp(prev, curr, t) の結果を読み戻して一致を確かめる。
//  ここで成功した場合のみ、UI のフレーム補間トグルを有効化できる。
//  (即時コンテキストはデコードスレッドと共有されるので ContextLock の内側で動かす)
// =============================================================================
constexpr const char* kInterpolationKernel = R"HLSL(
Texture2D<float4> tPrev : register(t0);
Texture2D<float4> tCurr : register(t1);
Texture2D<float4> tUnused : register(t2);
SamplerState      smp : register(s0);
RWTexture2D<float4> uOut : register(u0);
cbuffer Params : register(b0)
{
    float4 gParams; // x = blend t
};

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w = 0;
    uint h = 0;
    uOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float4 a = tPrev.SampleLevel(smp, uv, 0);
    float4 b = tCurr.SampleLevel(smp, uv, 0);
    uOut[id.xy] = lerp(a, b, saturate(gParams.x));
}
)HLSL";

struct FrameGenCaps {
    bool computeReady = false;
    bool dispatchVerified = false;
    unsigned threadGroups = 0;
};

bool compileKernel(const char* entry, const char* profile, ID3DBlob** blob) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kInterpolationKernel,
                                  std::strlen(kInterpolationKernel), "nanoalpha_interp.hlsl",
                                  nullptr, nullptr, entry, profile, flags, 0, blob, &errors);
    if (FAILED(hr)) {
        if (errors) {
            log::info("FrameGen: HLSL コンパイル失敗: %s",
                      static_cast<const char*>(errors->GetBufferPointer()));
        } else {
            log::info("FrameGen: HLSL コンパイル失敗 hr=0x%08lX",
                      static_cast<unsigned long>(hr));
        }
        return false;
    }
    return true;
}

// 8x8 のテストテクスチャで補間カーネルを 1 回ディスパッチし、結果を検証する
bool verifyDispatch(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context) return false;

    constexpr UINT kW = 8;
    constexpr UINT kH = 8;

    auto makeTexture = [&](DXGI_FORMAT format, UINT bind) -> Microsoft::WRL::ComPtr<ID3D11Texture2D> {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = kW;
        desc.Height = kH;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = bind;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &tex))) return nullptr;
        return tex;
    };

    auto prev = makeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto curr = makeTexture(DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
    auto out = makeTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
                           D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    if (!prev || !curr || !out) return false;

    // prev = 赤, curr = 青 (blend=0.5 → 期待値は r=b=128 あたり)
    struct RGBA { std::uint8_t r, g, b, a; };
    std::vector<RGBA> red(kW * kH, RGBA{255, 0, 0, 255});
    std::vector<RGBA> blue(kW * kH, RGBA{0, 0, 255, 255});
    context->UpdateSubresource(prev.Get(), 0, nullptr, red.data(), kW * 4, 0);
    context->UpdateSubresource(curr.Get(), 0, nullptr, blue.data(), kW * 4, 0);

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> prevSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> currSrv;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> outUav;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> outSrv;
    if (FAILED(device->CreateShaderResourceView(prev.Get(), nullptr, &prevSrv))) return false;
    if (FAILED(device->CreateShaderResourceView(curr.Get(), nullptr, &currSrv))) return false;
    if (FAILED(device->CreateUnorderedAccessView(out.Get(), nullptr, &outUav))) return false;
    if (FAILED(device->CreateShaderResourceView(out.Get(), nullptr, &outSrv))) return false;

    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    if (!compileKernel("CSMain", "cs_5_0", &csBlob)) return false;

    Microsoft::WRL::ComPtr<ID3D11ComputeShader> cs;
    if (FAILED(device->CreateComputeShader(csBlob->GetBufferPointer(),
                                           csBlob->GetBufferSize(), nullptr, &cs)) ||
        !cs) {
        return false;
    }

    struct Params { float t, pad0, pad1, pad2; } params{0.5f, 0.0f, 0.0f, 0.0f};
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(Params);
    cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA cbData{&params, 0, 0};
    Microsoft::WRL::ComPtr<ID3D11Buffer> cb;
    if (FAILED(device->CreateBuffer(&cbDesc, &cbData, &cb))) return false;

    D3D11_SAMPLER_DESC sDesc{};
    sDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.MaxLOD = D3D11_FLOAT32_MAX;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&sDesc, &sampler))) return false;

    // パイプラインを組んでディスパッチ
    context->CSSetShader(cs.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {prevSrv.Get(), currSrv.Get()};
    context->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uavs[1] = {outUav.Get()};
    UINT initial[1] = {0xFFFFFFFFu};
    context->CSSetUnorderedAccessViews(0, 1, uavs, initial);
    context->CSSetConstantBuffers(0, 1, cb.GetAddressOf());
    ID3D11SamplerState* samplers[1] = {sampler.Get()};
    context->CSSetSamplers(0, 1, samplers);
    context->Dispatch((kW + 7) / 8, (kH + 7) / 8, 1);

    // ディスパッチの結果を回収 (CPU 待ちは 8x8 なので実質瞬時)
    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    {
        D3D11_TEXTURE2D_DESC desc{};
        out->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)) || !staging) return false;
    }

    // 後片付け (テスト中に描画側とコンテキストを汚さないようディスパッチ直後に解く)
    ID3D11ShaderResourceView* nullSrvs[2] = {nullptr, nullptr};
    context->CSSetShaderResources(0, 2, nullSrvs);
    ID3D11UnorderedAccessView* nullUav[1] = {nullptr};
    UINT zero[1] = {0};
    context->CSSetUnorderedAccessViews(0, 1, nullUav, zero);
    ID3D11SamplerState* nullSamp[1] = {nullptr};
    context->CSSetSamplers(0, 1, nullSamp);
    context->CSSetShader(nullptr, nullptr, 0);

    context->CopyResource(staging.Get(), out.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const auto* row = static_cast<const std::uint8_t*>(mapped.pData);
    const int r = row[0];
    const int b = row[2];
    context->Unmap(staging.Get(), 0);

    const bool ok = (r >= 120 && r <= 136) && (b >= 120 && b <= 136);
    if (!ok) {
        log::info("FrameGen: 補間カーネルの検証値が想定外 (r=%d b=%d, 期待 ~128)", r, b);
    }
    return ok;
}

LoadResult loadFrameGeneration(const LoadContext& ctx) {
    if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");
    if (ctx.expired()) return LoadResult::timeout("ロード予算を超過");
    if (!ctx.host.device || !ctx.host.context) {
        return LoadResult::error("D3D11 デバイスが未初期化のためフレーム補間は利用できません");
    }

    auto caps = std::make_shared<FrameGenCaps>();

    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    if (!compileKernel("CSMain", "cs_5_0", &blob)) {
        return LoadResult::error("フレーム補間カーネルのコンピュートシェーダ化に失敗");
    }
    caps->computeReady = true;

    // 即時コンテキストはデコードスレッドと共有されるため ContextLock 内で実行する
    bool verified = false;
    {
        VideoDecoder* video = ctx.host.video;
        if (video) {
            const std::lock_guard<ContextLock> guard(video->contextMutex());
            verified = verifyDispatch(ctx.host.device, ctx.host.context);
        }
    }

    if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");

    caps->dispatchVerified = verified;
    if (!verified) {
        return LoadResult::error("補間カーネルの GPU 検証に失敗 (SM5.0 compute 未対応?)");
    }

    log::info("FrameGen: 補間カーネルのディスパッチ検証 OK (8x8, t=0.5)");
    return LoadResult::success(caps, "compute kernel 検証済み / 映像タブの"
                                     "「フレーム補間」で有効化できます");
}

// =============================================================================
//  Phase 2 ローダ: OpenRGB Sync (127.0.0.1:6742 への実接続)
// =============================================================================
#if NANOALPHA_ENABLE_OPENRGB
LoadResult loadOpenRgb(const LoadContext& ctx) {
    if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");
    if (ctx.expired()) return LoadResult::timeout("ロード予算を超過");

    auto client = std::make_shared<rgb::Client>();
    std::string error;

    if (!client->connect("127.0.0.1", 6742, 1200ms, error)) {
        return LoadResult::error("OpenRGB SDK に接続できません (" + error +
                                 ") — OpenRGB を起動してください");
    }
    if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");

    if (!client->enumerate(ctx.stop, error)) {
        client->disconnect();
        return LoadResult::error("OpenRGB デバイス列挙に失敗: " + error);
    }

    const unsigned count = client->deviceCount();
    unsigned leds = 0;
    for (unsigned i = 0; i < count; ++i) leds += client->ledCount(i);

    log::info("OpenRGB: 接続 OK  protocol=%u  devices=%u  leds=%u",
              client->protocolVersion(), count, leds);
    return LoadResult::success(client, "OpenRGB: " + std::to_string(count) + " デバイス / " +
                                           std::to_string(leds) + " LED (protocol v" +
                                           std::to_string(client->protocolVersion()) + ")");
}
#endif

// 実装済みローダ: 光学ドライブ即時探索
//  - 予算内に完結する軽量処理のみを行う
//  - stop / deadline を毎イテレーションで監視する
LoadResult loadDisc(const LoadContext& ctx) {
    auto probe = std::make_shared<DiscProbe>();
    for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
        if (ctx.stopped()) return LoadResult::error("停止要求によりキャンセル");
        if (ctx.expired()) return LoadResult::timeout("ロード予算を超過");

        wchar_t root[4] = {letter, L':', L'\\', L'\0'};
        if (GetDriveTypeW(root) == DRIVE_CDROM) probe->roots.emplace_back(root);
    }

    if (probe->roots.empty()) return LoadResult::error("光学ドライブが見つかりません");
    return LoadResult::success(
        probe, "光学ドライブ " + std::to_string(probe->roots.size()) + " 台を検出");
}

} // namespace

bool registerBuiltinLoader(ExtensionManager& manager, ExtensionId id) {
    switch (id) {
    case ExtensionId::FrameGeneration:
#if NANOALPHA_ENABLE_FRAMEGEN
        return manager.add(ExtensionId::FrameGeneration, LoadPriority::Normal, loadFrameGeneration,
                           "Radeon Frame Generation");
#else
        return false;
#endif
    case ExtensionId::OpenRgb:
#if NANOALPHA_ENABLE_OPENRGB
        return manager.add(ExtensionId::OpenRgb, LoadPriority::High, loadOpenRgb, "OpenRGB Sync");
#else
        return false;
#endif
    case ExtensionId::Disc:
#if NANOALPHA_ENABLE_DISC
        return manager.add(ExtensionId::Disc, LoadPriority::Normal, loadDisc, "CD-DA / DVD-Video");
#else
        return false;
#endif
    case ExtensionId::NetworkStream:
#if NANOALPHA_ENABLE_NETWORK
        return manager.add(ExtensionId::NetworkStream, LoadPriority::Normal, loadNetwork,
                           "Network Streaming");
#else
        return false;
#endif
    case ExtensionId::VlcFeatures:
        return manager.add(ExtensionId::VlcFeatures, LoadPriority::Low, loadVlcFeatures,
                           "VLC Features");
    case ExtensionId::Count:
        break;
    }
    return false;
}

void registerBuiltinLoaders(ExtensionManager& manager) {
    for (std::size_t index = 0; index < static_cast<std::size_t>(ExtensionId::Count); ++index) {
        registerBuiltinLoader(manager, static_cast<ExtensionId>(index));
    }

    log::info("Phase 2: %zu extension loader(s) registered", manager.registeredCount());
}

} // namespace nanoalpha::extensions
