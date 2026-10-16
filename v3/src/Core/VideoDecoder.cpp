#include "Core/VideoDecoder.hpp"

#include "Core/Log.hpp"
#include "Core/PlayerController.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// FFmpeg ヘッダはサードパーティなので自前の警告レベルに混ぜない
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4267 4305)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswscale/swscale.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace nanoalpha {
namespace {

std::string ffError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(size - 1)); // 末尾の NUL を除去
    return out;
}

// D3D11VA が AV_PIX_FMT_D3D11 を選ぶようにする。
// D3D11 が候補に無い場合 (ソフトウェアデコーダ / D3D11VA 未提供) は
// AV_PIX_FMT_NONE を返すとデコード全体が失敗するので、先頭の非 hwaccel
// フォーマットへフォールバックする。
enum AVPixelFormat getHwFormat(AVCodecContext*, const enum AVPixelFormat* formats) {
    for (const enum AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        if (*fmt == AV_PIX_FMT_D3D11) return *fmt;
    }
    for (const enum AVPixelFormat* fmt = formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(*fmt);
        if (desc != nullptr && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) return *fmt;
    }
    return AV_PIX_FMT_NONE;
}

// D3D11VA の hw config を持つデコーダを優先して選ぶ。
// avcodec_find_decoder() は登録順で libdav1d (ソフトウェア専用) を返すため
// AV1 でソフトウェアフレームが D3D11 テクスチャとして誤解釈されクラッシュ
// していた。同 codec_id のネイティブデコーダ (av1) に切り替える。
const AVCodec* pickDecoder(AVCodecID id) {
    const AVCodec* fallback = avcodec_find_decoder(id);
    auto hasD3d11 = [](const AVCodec* codec) -> bool {
        if (codec == nullptr) return false;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (cfg == nullptr) break;
            if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
                (cfg->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX |
                                 AV_CODEC_HW_CONFIG_METHOD_AD_HOC)) != 0) {
                return true;
            }
        }
        return false;
    };
    if (hasD3d11(fallback)) return fallback;
    void* iter = nullptr;
    const AVCodec* codec = nullptr;
    while ((codec = av_codec_iterate(&iter)) != nullptr) {
        if (!av_codec_is_decoder(codec) || codec->id != id) continue;
        if (hasD3d11(codec)) return codec;
    }
    return fallback;
}

} // namespace

// ---------------------------------------------------------------------------
//  字幕 (v2) — サイドカー候補とテキスト字幕判定
// ---------------------------------------------------------------------------
namespace {

// 媒体パスから basename.{srt,ass,ssa} と path + ".srt" の候補を作る。
std::vector<std::wstring> sidecarCandidates(const std::wstring& path) {
    std::vector<std::wstring> out;
    const std::size_t dot = path.find_last_of(L'.');
    const std::size_t sep = path.find_last_of(L"/\\");
    const bool hasExt =
        dot != std::wstring::npos && (sep == std::wstring::npos || dot > sep);
    if (hasExt) {
        const std::wstring stem = path.substr(0, dot);
        out.push_back(stem + L".srt");
        out.push_back(stem + L".ass");
        out.push_back(stem + L".ssa");
    }
    out.push_back(path + L".srt"); // video.mkv.srt 形式
    return out;
}

// ビットマップ字幕 (PGS / DVD / DVB / XSUB) は描画できないため除外する。
// テキスト字幕 (SUBRIP / ASS / SSA / MOV_TEXT / WEBVTT 等) のみ開く。
bool isTextSubtitleCodec(AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_DVD_SUBTITLE:
    case AV_CODEC_ID_DVB_SUBTITLE:
    case AV_CODEC_ID_HDMV_PGS_SUBTITLE:
    case AV_CODEC_ID_XSUB:
        return false;
    default:
        return true;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// 実装本体
// ---------------------------------------------------------------------------
struct VideoDecoder::Impl {
    VideoDecoder& owner;

    explicit Impl(VideoDecoder& self) : owner(self) {}

    // ---- D3D11 即時コンテキストの直列化 ----
    // FFmpeg のデコードスレッドとメインスレッドの描画が同一 ID3D11DeviceContext を
    // 共有するため、FFmpeg の lock/unlock とアプリ側の描画で同じ (FIFO 公平・再帰)
    // ロックを使う。
    ContextLock d3dLock;

    static void ffLock(void* ctx) { static_cast<Impl*>(ctx)->d3dLock.lock(); }
    static void ffUnlock(void* ctx) { static_cast<Impl*>(ctx)->d3dLock.unlock(); }

    // ---- FFmpeg (デコードスレッドのみが触る) ----
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVBufferRef* hwDevice = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* work = nullptr;      // avcodec_receive_frame の受け皿
    AVFrame* pending = nullptr;   // 未表示の新着フレーム
    AVFrame* current = nullptr;   // 描画中 (メインスレッドが保持)

    AVStream* videoStream = nullptr;
    int videoStreamIndex = -1;

    // ---- 同期 ----
    std::mutex mutex;
    std::condition_variable consumeCv;  // pending が消費されたとき
    std::condition_variable clockCv;    // 停止 / シーク要求
    bool hasPending = false;
    // 表示寸法 (アスペクト用)。テクスチャはデコーダのアライン分パディングされる
    // (例: 1920x1080 → 1920x1152) ので、これと実サイズで UV を補正する。
    int contentWidth = 0;
    int contentHeight = 0;
    bool flushSent = false;
    double seekTarget = -1.0;
    bool stopRequested = false;
    double pendingPts = 0.0;
    double currentPts = 0.0;
    std::atomic<bool> limitedRange{true};
    // pending/current がソフトウェアフレーム (D3D11VA 未対応コーデック) か
    bool pendingIsSoftware = false;
    bool currentIsSoftware = false;

    // ---- ソフトウェアフレームのアップロード経路 (sws_scale → BGRA → GPU) ----
    // D3D11VA でないフレームは data[0] がテクスチャではなく CPU バッファなので、
    // BGRA へ変換して BGRA テクスチャへ転送する (2 枚で ping-pong)。
    struct SwUpload {
        SwsContext* sws = nullptr;
        ID3D11Texture2D* tex[2] = {nullptr, nullptr};
        int writeIdx = 0;    // 次に書き込むインデックス
        int viewIdx = -1;    // 直近に書き出したインデックス
        std::vector<std::uint8_t> buffer;
    } sw;
    int uploadedSwIdx = -1;  // 直近にアップロードした sw.tex のインデックス

    // ---- 情報 (open 中にのみ書き込む / bits は描画時に実 texture で更新) ----
    UINT width = 0;
    UINT height = 0;
    double fps = 0.0;
    double duration = 0.0;
    std::atomic<int> bits{8};
    // D3D11VA hw config を持つデコーダを選んだか (Info.hardwareDecoding)
    bool hwCapableCodec = false;
    // 実フレームの状態 0=不明 1=hw 2=sw (初回フレームで確定)
    std::atomic<int> frameHwState{0};
    // 音声のみファイル (映像ストリーム無し)
    std::atomic<bool> audioOnly{false};

    // ---- 字幕 (v2: 埋め込みテキスト字幕) ----
    // サイドカーがロード済みの間は埋め込み cue を捨てる (sidecar 優先)
    AVCodecContext* subDecoder = nullptr;
    AVStream* subStream = nullptr;
    int subStreamIndex = -1;
    bool subtitleCueLogged = false;
    bool subtitleEnabledLogged = false;

    // ---- 再生制御 ----
    PlayerController* controller = nullptr;
    // PlayerController::takeSeek() を映像側で消費するためのリビジョン (音声とは別持ち)
    std::uint64_t seenSeekRevision = 0;
    std::chrono::steady_clock::time_point anchorWall{};
    double anchorPts = 0.0;
    float lastSpeed = 1.0f;
    bool resync = true;
    std::chrono::steady_clock::time_point audioStallUntil{}; // 音声停滞時の自律ペース解除時刻
    bool firstFrameLogged = false;
    double lastLoggedPts = -100.0;
    // ソースの瞬間フレーム間隔を測るための直前フレーム pts (デコードスレッドのみ)
    double lastPublishedPts = -1.0;
    bool firstViewLogged = false;
    int appliedQuality = -1; // PlayerController::DecodeQuality の反映済み値

    std::thread worker;

    // ---------------------------------------------------------------- 生成 / 破棄
    bool createHwDevice(ID3D11Device* device) {
        if (hwDevice) return true;
        AVBufferRef* ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!ref) {
            log::info("VideoDecoder: av_hwdevice_ctx_alloc 失敗");
            return false;
        }
        auto* hwContext = reinterpret_cast<AVHWDeviceContext*>(ref->data);
        auto* d3dContext = reinterpret_cast<AVD3D11VADeviceContext*>(hwContext->hwctx);

        // アプリ側デバイスを注入する。解放時に FFmpeg が Release するため事前に AddRef する。
        device->AddRef();
        d3dContext->device = device;
        // サーフェスをシェーダから読むため SRV バインドを要求 (Zero-Copy の要)
        d3dContext->BindFlags = D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
        d3dContext->MiscFlags = 0;
        // FFmpeg 側の device_context / video_context 呼び出しをアプリの描画と直列化する
        d3dContext->lock = &Impl::ffLock;
        d3dContext->unlock = &Impl::ffUnlock;
        d3dContext->lock_ctx = this;

        const int err = av_hwdevice_ctx_init(ref);
        if (err < 0) {
            log::info("VideoDecoder: av_hwdevice_ctx_init 失敗: %s", ffError(err).c_str());
            av_buffer_unref(&ref);
            return false;
        }
        hwDevice = ref;
        log::info("VideoDecoder: D3D11VA デバイス初期化 OK (BindFlags=DECODER|SRV)");
        return true;
    }

    void destroyFrames() {
        // GPU リソースの解放は描画スレッドと直列化する (closeFile は worker 停止後に呼ぶ)
        const std::lock_guard<ContextLock> guard(owner.contextMutex());
        releaseCurrent();
        if (pending) av_frame_unref(pending);
        hasPending = false;
        pendingIsSoftware = false;
        currentIsSoftware = false;
        for (ID3D11Texture2D*& tex : sw.tex) {
            if (tex) {
                tex->Release();
                tex = nullptr;
            }
        }
        sw.viewIdx = -1;
        sw.writeIdx = 0;
        uploadedSwIdx = -1;
        sw.buffer.clear();
        if (sw.sws) {
            sws_freeContext(sw.sws);
            sw.sws = nullptr;
        }
    }

    void releaseCurrent() {
        if (current) av_frame_unref(current);
        owner.m_hasFrame.store(false, std::memory_order_release);
    }

    // ---- ソフトウェアフレーム → BGRA テクスチャ変換 ----
    // ContextLock の内側 (メインスレッド) で呼ぶこと。2 枚の BGRA テクスチャで
    // ping-pong し、書き出したテクスチャの配列インデックスを返す (失敗時は -1)。
    int uploadSoftwareFrame(const AVFrame* frame) {
        if (owner.m_context == nullptr) return -1;
        const int w = frame->width;
        const int h = frame->height;
        if (w <= 0 || h <= 0) return -1;

        sw.sws = sws_getCachedContext(sw.sws, w, h,
                                      static_cast<AVPixelFormat>(frame->format), w, h,
                                      AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr,
                                      nullptr);
        if (!sw.sws) {
            log::info("VideoDecoder: sws_getCachedContext 失敗 (fmt=%d)",
                      static_cast<int>(frame->format));
            return -1;
        }

        // サイズが変わったらテクスチャを再生成
        const int idx = sw.writeIdx;
        ID3D11Texture2D* tex = sw.tex[idx];
        if (tex) {
            D3D11_TEXTURE2D_DESC desc{};
            tex->GetDesc(&desc);
            if (desc.Width != static_cast<UINT>(w) || desc.Height != static_cast<UINT>(h)) {
                tex->Release();
                sw.tex[idx] = nullptr;
                tex = nullptr;
            }
        }
        if (!tex) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(w);
            desc.Height = static_cast<UINT>(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(owner.m_device->CreateTexture2D(&desc, nullptr, &tex)) || !tex) {
                log::info("VideoDecoder: BGRA テクスチャ生成失敗");
                return -1;
            }
            sw.tex[idx] = tex;
        }

        const std::size_t stride = static_cast<std::size_t>(w) * 4u;
        if (sw.buffer.size() < stride * static_cast<std::size_t>(h)) {
            sw.buffer.resize(stride * static_cast<std::size_t>(h));
        }
        std::uint8_t* dstData[4] = {sw.buffer.data(), nullptr, nullptr, nullptr};
        int dstLinesize[4] = {static_cast<int>(stride), 0, 0, 0};
        sws_scale(sw.sws, frame->data, frame->linesize, 0, h, dstData, dstLinesize);

        owner.m_context->UpdateSubresource(tex, 0, nullptr, sw.buffer.data(),
                                           static_cast<UINT>(stride), 0);
        sw.viewIdx = idx;
        sw.writeIdx = 1 - idx;
        return idx;
    }

    // ---------------------------------------------------------------- デコードスレッド
    void workerMain() {
        using namespace std::chrono;

        while (true) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (stopRequested) break;

                // --- シーク要求 (内部 / PlayerController 経由) ---
                double seekTo = -1.0;
                if (seekTarget >= 0.0) {
                    seekTo = seekTarget;
                    seekTarget = -1.0;
                } else if (controller && controller->takeSeek(seenSeekRevision)) {
                    // UI / A-B リピートの requestSeek() を映像側も同じリビジョンから
                    // 独立に消費する (音声側とは seenSeekRevision が別なので両方動く)。
                    seekTo = controller->seekTargetSeconds();
                }
                if (seekTo >= 0.0) {
                    lock.unlock();
                    performSeek(seekTo);
                    lock.lock();
                }

                // --- フレーム供給のバックプレッシャ ---
                // ここでは待たない。表示ペースは pace() が media/speed で厳密に制御
                // しており、先行は 1 フレームぶんに収まる。逆に「pending が消費される
                // まで待つ」とすると待つのが描画レート (60fps) 側になり、
                // 再生速度が 2.0x を超えると (30fps x 4.0x = 120fps が必要なため)
                // 映像の発行が 60fps に張り付いて音声に追いつかなくなる。
            }

            int ret = avcodec_receive_frame(decoder, work);
            if (ret == 0) {
                publishFrame(work);
                continue;
            }
            if (ret == AVERROR_EOF) {
                // 再生終了: シークまたは停止まで待機
                std::unique_lock<std::mutex> lock(mutex);
                clockCv.wait_for(lock, milliseconds(50), [this] {
                    return stopRequested || seekTarget >= 0.0;
                });
                if (stopRequested) break;
                continue;
            }
            if (ret != AVERROR(EAGAIN)) {
                log::info("VideoDecoder: avcodec_receive_frame: %s", ffError(ret).c_str());
                break;
            }

            if (flushSent) {
                // フラッシュ済みで EAGAIN = 入力待ち (EOF 後のサイレント再生)
                std::unique_lock<std::mutex> lock(mutex);
                clockCv.wait_for(lock, milliseconds(50), [this] {
                    return stopRequested || seekTarget >= 0.0;
                });
                if (stopRequested) break;
                continue;
            }

            const int read = av_read_frame(format, packet);
            if (read < 0) {
                avcodec_send_packet(decoder, nullptr); // デコーダへフラッシュ
                flushSent = true;
                continue;
            }
            if (packet->stream_index == videoStreamIndex) {
                // 画質プリセット (v2): 値が変わったときだけデコーダへ反映する。
                // skip_loop_filter / skip_frame は次のパケットから有効で、
                // D3D11VA の経路でもポストフィルタ側 (ソフトウェア後処理) に効く。
                const int quality = owner.m_decodeQuality.load(std::memory_order_relaxed);
                if (quality != appliedQuality) {
                    appliedQuality = quality;
                    switch (quality) {
                    case 0: // 高速: デブロック省略 + 非参照フレーム破棄
                        decoder->skip_loop_filter = AVDISCARD_ALL;
                        decoder->skip_frame = AVDISCARD_NONREF;
                        break;
                    case 2: // 高画質: デブロックを必ず実行
                        decoder->skip_loop_filter = AVDISCARD_NONE;
                        decoder->skip_frame = AVDISCARD_DEFAULT;
                        break;
                    default: // 標準
                        decoder->skip_loop_filter = AVDISCARD_DEFAULT;
                        decoder->skip_frame = AVDISCARD_DEFAULT;
                        break;
                    }
                    log::info("VideoDecoder: decode quality -> %d (skip_loop_filter=%d)",
                              quality, static_cast<int>(decoder->skip_loop_filter));
                }
                const int sent = avcodec_send_packet(decoder, packet);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    log::info("VideoDecoder: avcodec_send_packet: %s", ffError(sent).c_str());
                }
            } else if (subStreamIndex >= 0 && packet->stream_index == subStreamIndex) {
                decodeSubtitlePacket(packet);
            }
            av_packet_unref(packet);
        }
    }

    // デコード済みフレームを再生速度に合わせて待ち、pending へ渡す
    void publishFrame(AVFrame* frame) {
        using namespace std::chrono;

        const double pts = framePts(frame);

        // --- ソースの瞬間フレーム間隔 (EMA) ---------------------------------
        // publishFrame はコンテンツ順の全フレームに対して呼ばれる
        // (avcodec_receive_frame → ここで pace() → pending へ)。つまりこの差分は
        // 「動画そのもののフレーム間隔」であって、描画 (Present) レートとは無関係。
        // 描画を 60fps に固定したままでも 240fps 素材なら 1/240 を正しく拾える。
        // メインループが Present レートの目標 (1/disp * factor) に使う。
        if (lastPublishedPts >= 0.0) {
            const double gap = pts - lastPublishedPts;
            if (gap > 1.0e-5 && gap < 1.0) { // 0.1fps 未満 / 100fps 超は異常値として無視
                const double prev =
                    owner.m_contentInterval.load(std::memory_order_relaxed);
                owner.m_contentInterval.store(prev + (gap - prev) * 0.1,
                                              std::memory_order_relaxed);
            }
        }
        lastPublishedPts = pts;
        PlayerController::Snapshot snap;
        if (controller) snap = controller->snapshot();

        // --- 診断: 先頭フレームと 5 秒毎の進捗 ---
        const bool isSoftware = frame->format != AV_PIX_FMT_D3D11;
        if (!firstFrameLogged) {
            firstFrameLogged = true;
            frameHwState.store(isSoftware ? 2 : 1, std::memory_order_relaxed);
            log::info("VideoDecoder: 先頭フレーム pts=%.3f  avfmt=%d  w=%d h=%d  idx=%d  range=%d  path=%s",
                      pts, static_cast<int>(frame->format), frame->width, frame->height,
                      static_cast<int>(reinterpret_cast<std::intptr_t>(frame->data[1])),
                      static_cast<int>(frame->color_range), isSoftware ? "sw" : "hw");
        } else if (pts - lastLoggedPts >= 5.0) {
            lastLoggedPts = pts;
            // source= は直近 2 コマの実 pts 間隔から求めた「今区間の瞬間 fps」。
            // コンテナ平均 fps と食い違うことがある (VFR / 区間変化) ので並記する。
            log::info("VideoDecoder: 再生中 t=%.1f s  source=%.1f fps", pts,
                      1.0 / owner.m_contentInterval.load(std::memory_order_relaxed));
        }

        // A-B リピート
        if (snap.abRepeat && snap.pointB > snap.pointA && pts >= snap.pointB) {
            resync = true;
            performSeek(snap.pointA);
            return;
        }

        pace(pts, snap.speed);

        const bool limited = frame->color_range != AVCOL_RANGE_JPEG;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopRequested) return;
            const int contentW = frame->width;
            const int contentH = frame->height;
            if (hasPending) av_frame_unref(pending); // 表示が追いつかない場合は破棄
            av_frame_move_ref(pending, frame);
            pendingPts = pts;
            pendingIsSoftware = isSoftware;
            limitedRange = limited;
            contentWidth = contentW;
            contentHeight = contentH;
            hasPending = true;
        }
        owner.m_clock.store(pts, std::memory_order_relaxed);
        consumeCv.notify_all();
    }

    double framePts(const AVFrame* frame) const {
        const int64_t stamp = frame->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE || !videoStream) {
            return owner.m_clock.load(std::memory_order_relaxed);
        }
        return stamp * av_q2d(videoStream->time_base);
    }

    void pace(double pts, float speed) {
        using clock = std::chrono::steady_clock;
        if (speed < 0.25f) speed = 1.0f;

        std::unique_lock<std::mutex> lock(mutex);
        if (stopRequested || seekTarget >= 0.0) { resync = true; return; }

        // --- 一時停止 (v2) ---
        // AudioEngine が IAudioClient::Stop() をかけた間は MediaClock が凍結される。
        // ここでは音声クロックを待たず、復帰するまで (またはシーク/停止が来るまで)
        // 待つ。待っている間も takeSeek を監視するので、一時停止中でもシークに追従する。
        if (controller && controller->snapshot().paused) {
            while (!stopRequested && seekTarget < 0.0) {
                if (!controller->snapshot().paused) break;
                if (controller->takeSeek(seenSeekRevision)) {
                    seekTarget = controller->seekTargetSeconds();
                    break;
                }
                clockCv.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return stopRequested || seekTarget >= 0.0;
                });
            }
            resync = true; // 復帰後は壁時計アンカーを張り直す
            anchorWall = clock::now();
            anchorPts = pts;
            return;
        }

        // --- 音声マスタークロックによる同期 ---
        // AudioEngine は「いま聞こえている位置」を MediaClock へ発行している。
        // その位置がフレームの PTS に達するまで待つことで A/V 同期を実現する。
        // MediaClock は再生速度に関わらず媒体時刻を返すため、速度変更時でも
        // この判定は有効である (逆に 1.0x のみに限定すると、速度を変えた瞬間から
        // 映像が壁時計ベースで自律ペースとなり、ラップ中のズレが回復不能になる)。
        if (owner.m_clockSource && owner.m_clockSource->valid() &&
            clock::now() >= audioStallUntil) {
            const auto startedAt = clock::now();
            while (!stopRequested && seekTarget < 0.0) {
                if (owner.m_clockSource->pts() >= pts) break;
                if (controller && controller->takeSeek(seenSeekRevision)) {
                    // 待っている最中にシークが入った。ここで待つと後追いした音声に
                    // 追いつくまで (最大 2 秒) 映像が止まって見えるので打ち切り、
                    // 次のループで seekTarget 経由に引き継ぐ。
                    seekTarget = controller->seekTargetSeconds();
                    break;
                }
                if (clock::now() - startedAt > std::chrono::seconds(2)) {
                    // 音声が止まっている (EOF / 未再生) → 2 秒間は壁時計で自律ペース
                    audioStallUntil = clock::now() + std::chrono::seconds(2);
                    break;
                }
                clockCv.wait_for(lock, std::chrono::milliseconds(4), [this] {
                    return stopRequested || seekTarget >= 0.0;
                });
            }
            // 復帰時に壁時計アンカーを張り直す (音声が失われた場合に備える)
            resync = true;
            anchorWall = clock::now();
            anchorPts = pts;
            return;
        }

        if (resync || speed != lastSpeed) {
            anchorWall = clock::now();
            anchorPts = pts;
            lastSpeed = speed;
            resync = false;
        }

        const double delta = (pts - anchorPts) / static_cast<double>(speed);
        const auto target = anchorWall + std::chrono::duration_cast<clock::duration>(
                                             std::chrono::duration<double>(delta));
        const auto now = clock::now();

        if (target > now) {
            clockCv.wait_until(lock, target, [this] {
                return stopRequested || seekTarget >= 0.0;
            });
        } else if (now - target > std::chrono::milliseconds(300)) {
            resync = true; // 大きく遅れた場合は再同期 (デコード遅延のスパイラル回避)
        }

        anchorWall = target;
        anchorPts = pts;
    }

    void performSeek(double seconds) {
        if (!format || !videoStream) return;
        if (seconds < 0.0) seconds = 0.0;

        const double base = av_q2d(videoStream->time_base);
        const int64_t ts = static_cast<int64_t>(seconds / base);
        int ret = avformat_seek_file(format, videoStreamIndex, INT64_MIN, ts, ts, 0);
        if (ret < 0) ret = av_seek_frame(format, videoStreamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            log::info("VideoDecoder: seek 失敗: %s", ffError(ret).c_str());
            return;
        }
        // ここからの flush / unref は D3D11 テクスチャや ID3D11VideoContext に触れる。
        // 描画スレッド (renderer.draw) と同じ即時コンテキストを共有しているので、
        // ContextLock で直列化しないと解放と描画が競合してドライバがクラッシュする。
        const std::lock_guard<ContextLock> guard(owner.contextMutex());
        avcodec_flush_buffers(decoder);
        flushSent = false;
        resync = true;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (hasPending) {
                av_frame_unref(pending);
                hasPending = false;
            }
            consumeCv.notify_all();
        }
        owner.m_clock.store(seconds, std::memory_order_relaxed);
    }

    void requestSeek(double seconds) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            seekTarget = seconds;
        }
        clockCv.notify_all();
        consumeCv.notify_all();
    }

    // ---------------------------------------------------------------- open / close
    bool openFile(const std::wstring& path, PlayerController* ctl) {
        controller = ctl;

        const std::string utf8Path = wideToUtf8(path);
        if (utf8Path.empty()) {
            log::info("VideoDecoder: パス変換に失敗");
            return false;
        }

        AVFormatContext* fmt = nullptr;
        int ret = avformat_open_input(&fmt, utf8Path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            log::info("VideoDecoder: avformat_open_input 失敗: %s", ffError(ret).c_str());
            return false;
        }
        ret = avformat_find_stream_info(fmt, nullptr);
        if (ret < 0) {
            log::info("VideoDecoder: find_stream_info 失敗: %s", ffError(ret).c_str());
            avformat_close_input(&fmt);
            return false;
        }

        // ---- 字幕 (v2) -----------------------------------------------------
        // サイドカー (basename.srt / .ass / .ssa) を優先して全件ロードする。
        // 単体字幕ファイル (.srt 等) は自分自身を読み込む。
        // サイドカーがロードできた場合は埋め込み字幕をデコードしない。
        owner.m_subtitles.clear();
        const bool subtitleInput = SubtitleSource::isSubtitlePath(path);
        bool sidecarLoaded = false;
        {
            const std::vector<std::wstring> candidates =
                subtitleInput ? std::vector<std::wstring>{path}
                               : sidecarCandidates(path);
            for (const std::wstring& cand : candidates) {
                if (GetFileAttributesW(cand.c_str()) == INVALID_FILE_ATTRIBUTES)
                    continue;
                if (owner.m_subtitles.loadFile(cand)) {
                    sidecarLoaded = true;
                    log::info("VideoDecoder: sidecar subtitle  %zu cue  '%s'",
                              owner.m_subtitles.count(),
                              wideToUtf8(cand).c_str());
                    if (controller != nullptr) {
                        controller->setSubtitleVisible(true);
                    }
                    break;
                }
            }
        }

        const int index =
            av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (index < 0) {
            // 映像が無いファイル: 音声のみ / 字幕のみを許容する (黒画面で再生)
            const int audioIndex =
                av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            const int subIndex =
                av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
            if (audioIndex < 0 && subIndex < 0) {
                log::info("VideoDecoder: 映像ストリームが見つかりません");
                avformat_close_input(&fmt);
                return false;
            }
            audioOnly.store(true, std::memory_order_relaxed);
            frameHwState.store(0, std::memory_order_relaxed);
            duration = (fmt->duration > 0)
                           ? static_cast<double>(fmt->duration) / AV_TIME_BASE
                           : 0.0;
            width = 0;
            height = 0;
            fps = 0.0;
            bits.store(8, std::memory_order_relaxed);
            owner.m_clock.store(0.0, std::memory_order_relaxed);
            // 映像が無いのでソース fps も不明。前のファイルの値を引き継いだと
            // メインループが「高 fps 素材」と誤認して描画レートを上げてしまう。
            owner.m_contentInterval.store(1.0 / 60.0, std::memory_order_relaxed);
            owner.m_ready.store(true, std::memory_order_release);
            avformat_close_input(&fmt);
            log::info("VideoDecoder: open OK (audio only)  %.1f sec  hw=none  '%s'",
                      duration, utf8Path.c_str());
            return true;
        }

        const AVCodec* codec = pickDecoder(fmt->streams[index]->codecpar->codec_id);
        if (!codec) {
            log::info("VideoDecoder: コーデックが見つかりません (id=%d)",
                      static_cast<int>(fmt->streams[index]->codecpar->codec_id));
            avformat_close_input(&fmt);
            return false;
        }

        AVCodecContext* dec = avcodec_alloc_context3(codec);
        if (!dec) {
            avformat_close_input(&fmt);
            return false;
        }
        AVStream* stream = fmt->streams[index];
        avcodec_parameters_to_context(dec, stream->codecpar);
        dec->pkt_timebase = stream->time_base;
        dec->hw_device_ctx = av_buffer_ref(hwDevice);
        dec->get_format = getHwFormat;
        dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
        dec->skip_frame = AVDISCARD_DEFAULT;
        // D3D11VA hw config を持つか (Info.hardwareDecoding)
        hwCapableCodec = false;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg) break;
            if (cfg->device_type == AV_HWDEVICE_TYPE_D3D11VA) {
                hwCapableCodec = true;
                break;
            }
        }

        ret = avcodec_open2(dec, codec, nullptr);
        if (ret < 0) {
            log::info("VideoDecoder: avcodec_open2 失敗: %s", ffError(ret).c_str());
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frameWork = av_frame_alloc();
        AVFrame* framePending = av_frame_alloc();
        AVFrame* frameCurrent = av_frame_alloc();
        if (!pkt || !frameWork || !framePending || !frameCurrent) {
            av_packet_free(&pkt);
            av_frame_free(&frameWork);
            av_frame_free(&framePending);
            av_frame_free(&frameCurrent);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        format = fmt;
        decoder = dec;
        packet = pkt;
        work = frameWork;
        pending = framePending;
        current = frameCurrent;
        videoStream = stream;
        videoStreamIndex = index;
        flushSent = false;
        resync = true;
        audioStallUntil = std::chrono::steady_clock::time_point{};
        lastSpeed = 1.0f;
        stopRequested = false;
        seekTarget = -1.0;
        firstFrameLogged = false;
        lastLoggedPts = -100.0;
        lastPublishedPts = -1.0;
        owner.m_contentInterval.store(1.0 / 60.0, std::memory_order_relaxed);
        firstViewLogged = false;
        audioOnly.store(false, std::memory_order_relaxed);
        frameHwState.store(0, std::memory_order_relaxed);
        pendingIsSoftware = false;
        currentIsSoftware = false;
        sw.writeIdx = 0;
        sw.viewIdx = -1;
        uploadedSwIdx = -1;
        const AVRational rate = av_guess_frame_rate(fmt, stream, nullptr);
        width = static_cast<UINT>(dec->width);
        height = static_cast<UINT>(dec->height);
        fps = (rate.num > 0 && rate.den > 0) ? av_q2d(rate) : 0.0;
        duration = (fmt->duration > 0) ? static_cast<double>(fmt->duration) / AV_TIME_BASE : 0.0;
        // ピクセルフォーマットビット数 (P010 = 10bit)。open 直後は sw_pix_fmt 未設定の
        // ことがあるため hw_frames_ctx も確認する (実 texture の値でも描画時に補正する)
        int pixelBits = (dec->sw_pix_fmt == AV_PIX_FMT_P010) ? 10 : 8;
        if (pixelBits == 8 && dec->hw_frames_ctx) {
            const auto* frames =
                reinterpret_cast<const AVHWFramesContext*>(dec->hw_frames_ctx->data);
            if (frames && frames->sw_format == AV_PIX_FMT_P010) pixelBits = 10;
        }
        bits.store(pixelBits, std::memory_order_relaxed);

        // ---- 埋め込みテキスト字幕 (サイドカーが無い場合のみ) ----
        if (!sidecarLoaded) {
            const int subIdx =
                av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
            if (subIdx >= 0 && isTextSubtitleCodec(fmt->streams[subIdx]->codecpar->codec_id)) {
                const AVCodec* subCodec =
                    avcodec_find_decoder(fmt->streams[subIdx]->codecpar->codec_id);
                AVCodecContext* subCtx = subCodec != nullptr ? avcodec_alloc_context3(subCodec) : nullptr;
                if (subCtx != nullptr &&
                    avcodec_parameters_to_context(subCtx, fmt->streams[subIdx]->codecpar) >= 0 &&
                    avcodec_open2(subCtx, subCodec, nullptr) >= 0) {
                    subDecoder = subCtx;
                    subStream = fmt->streams[subIdx];
                    subStreamIndex = subIdx;
                    log::info("VideoDecoder: subtitle stream  idx=%d  codec=%s",
                              subIdx, subCodec->name);
                } else {
                    if (subCtx != nullptr) avcodec_free_context(&subCtx);
                }
            }
        }

        worker = std::thread([this] { workerMain(); });
        owner.m_clock.store(0.0, std::memory_order_relaxed);
        owner.m_ready.store(true, std::memory_order_release);

        log::info("VideoDecoder: open OK  %ux%u  %.3f fps  %.1f sec  %dbit  hw=%s  '%s'",
                  width, height, fps, duration, pixelBits, codec->name, utf8Path.c_str());
        return true;
    }

    // ---------------------------------------------------------------- 字幕
    // 埋め込みテキスト字幕パケットを 1 件デコードして cue へ変換する。
    void decodeSubtitlePacket(const AVPacket* pkt) {
        if (subDecoder == nullptr || subStream == nullptr) return;
        if (owner.m_subtitles.hasFileCues()) return; // サイドカー優先

        AVSubtitle sub{};
        int got = 0;
        if (avcodec_decode_subtitle2(subDecoder, &sub, &got, pkt) < 0 ||
            got == 0) {
            avsubtitle_free(&sub);
            return;
        }

        // 基準時刻: sub.pts (AV_TIME_BASE) → 無ければパケットの pts
        double base = -1.0;
        if (sub.pts != AV_NOPTS_VALUE) {
            base = static_cast<double>(sub.pts) / AV_TIME_BASE;
        }
        if (base < 0.0 && pkt->pts != AV_NOPTS_VALUE) {
            base = pkt->pts * av_q2d(subStream->time_base);
        }
        if (base < 0.0) {
            base = owner.m_clock.load(std::memory_order_relaxed);
        }

        const double tb = av_q2d(subStream->time_base);
        const double pktDur =
            (pkt->duration > 0) ? pkt->duration * tb : 0.0;
        double start = base + sub.start_display_time / 1000.0;
        double end = base + sub.end_display_time / 1000.0;
        if (end <= start) {
            end = start + (pktDur > 0.0 ? pktDur : 5.0);
        }

        for (unsigned i = 0; i < sub.num_rects; ++i) {
            const AVSubtitleRect* rect = sub.rects[i];
            if (rect == nullptr || rect->type == SUBTITLE_BITMAP) continue;
            std::string text = SubtitleSource::cueText(rect->text, rect->ass);
            if (text.empty()) continue;
            owner.m_subtitles.add(start, end, text);
            if (!subtitleCueLogged) {
                subtitleCueLogged = true;
                log::info("VideoDecoder: subtitle cue  %.2f..%.2f sec  '%s'",
                          start, end, text.c_str());
            }
        }
        avsubtitle_free(&sub);

        // 最初の cue が届いたら字幕表示を自動で有効化する
        if (!subtitleEnabledLogged && owner.m_subtitles.count() > 0) {
            subtitleEnabledLogged = true;
            if (controller != nullptr) controller->setSubtitleVisible(true);
        }
    }

    void stopWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = true;
        }
        clockCv.notify_all();
        consumeCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void closeFile() {
        stopWorker();
        destroyFrames();

        if (work) av_frame_free(&work);
        if (packet) av_packet_free(&packet);
        if (decoder) avcodec_free_context(&decoder);
        if (subDecoder) avcodec_free_context(&subDecoder);
        if (format) avformat_close_input(&format);

        videoStream = nullptr;
        videoStreamIndex = -1;
        subStream = nullptr;
        subStreamIndex = -1;
        flushSent = false;
        controller = nullptr;
        audioOnly.store(false, std::memory_order_relaxed);
        frameHwState.store(0, std::memory_order_relaxed);
        hwCapableCodec = false;
        subtitleCueLogged = false;
        subtitleEnabledLogged = false;
        owner.m_subtitles.clear();
    }
};

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------

VideoDecoder::VideoDecoder() : m_impl(std::make_unique<Impl>(*this)) {}

VideoDecoder::~VideoDecoder() {
    close();
    if (m_impl && m_impl->hwDevice) {
        av_buffer_unref(&m_impl->hwDevice); // 注入した ID3D11Device の参照をここで解放
    }
}

void VideoDecoder::attach(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    m_device = device;
    m_context = context;
}

bool VideoDecoder::init(ID3D11Device* device, ID3D11DeviceContext* context) noexcept {
    attach(device, context);
    if (!device) {
        log::info("VideoDecoder: D3D11 デバイス未設定");
        return false;
    }
    av_log_set_level(AV_LOG_WARNING);
    return m_impl->createHwDevice(device);
}

bool VideoDecoder::open(const std::wstring& source, PlayerController* controller) {
    if (!m_impl->hwDevice) {
        log::info("VideoDecoder: init() 未完了のため開けません");
        return false;
    }
    close();
    m_source = source;
    return m_impl->openFile(source, controller);
}

void VideoDecoder::close() noexcept {
    m_ready.store(false, std::memory_order_release);
    if (m_impl) m_impl->closeFile();
    m_hasFrame.store(false, std::memory_order_release);
}

bool VideoDecoder::attachSubtitleFile(const std::wstring& path) {
    if (!m_impl) return false;
    if (!m_subtitles.loadFile(path)) {
        log::info("VideoDecoder: subtitle 読込失敗  '%s'",
                  wideToUtf8(path).c_str());
        return false;
    }
    log::info("VideoDecoder: subtitle attached  %zu cue  '%s'",
              m_subtitles.count(), wideToUtf8(path).c_str());
    // 再生中のメディアへ適用 (再生は継続)
    if (m_impl->controller != nullptr) {
        m_impl->controller->setSubtitleVisible(true);
    }
    return true;
}

std::string VideoDecoder::subtitleAt(double seconds) const {
    return m_subtitles.textAt(seconds);
}

bool VideoDecoder::hasSubtitles() const {
    return !m_subtitles.empty();
}

std::size_t VideoDecoder::subtitleCount() const {
    return m_subtitles.count();
}

VideoDecoder::FrameView VideoDecoder::acquireFrame() noexcept {
    FrameView view;
    if (!m_impl) return view;

    AVFrame* frame = nullptr;
    bool needUpload = false;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (m_impl->hasPending) {
            if (m_impl->current) av_frame_unref(m_impl->current);
            av_frame_move_ref(m_impl->current, m_impl->pending);
            m_impl->currentPts = m_impl->pendingPts;
            m_impl->currentIsSoftware = m_impl->pendingIsSoftware;
            m_impl->hasPending = false;
            needUpload = m_impl->currentIsSoftware;
            frame = m_impl->current;
        } else {
            frame = m_impl->current;
        }
        m_impl->consumeCv.notify_all();
    }
    if (!frame || !frame->data[0]) return view;

    // --- ソフトウェアフレーム (D3D11VA 非対応コーデック) のアップロード ---
    // data[0] が CPU バッファなので BGRA へ変換して GPU テクスチャへ渡す。
    // ContextLock は再帰可能なため、既にメインスレッドが保持していても安全。
    if (m_impl->currentIsSoftware) {
        const std::lock_guard<ContextLock> guard(contextMutex());
        if (needUpload) m_impl->uploadedSwIdx = m_impl->uploadSoftwareFrame(frame);
        const int idx = m_impl->uploadedSwIdx;
        if (idx < 0 || !m_impl->sw.tex[idx]) return FrameView{};

        view.texture = m_impl->sw.tex[idx];
        view.arrayIndex = 0;
        view.pts = m_impl->currentPts;
        view.limitedRange = false; // sws でフルレンジ RGB へ変換済み

        D3D11_TEXTURE2D_DESC desc{};
        view.texture->GetDesc(&desc);
        view.format = desc.Format;
        view.textureWidth = desc.Width;
        view.textureHeight = desc.Height;
        view.width = desc.Width;
        view.height = desc.Height;
        view.tenBit = false;
        m_impl->bits.store(8, std::memory_order_relaxed);

        if (!m_impl->firstViewLogged) {
            m_impl->firstViewLogged = true;
            log::info("VideoDecoder: 描画ビュー (software/BGRA) fmt=0x%08X 内容=%ux%u "
                      "pts=%.3f",
                      static_cast<unsigned long>(view.format), view.width, view.height,
                      view.pts);
        }
        m_hasFrame.store(true, std::memory_order_release);
        return view;
    }

    view.texture = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    view.arrayIndex = static_cast<int>(reinterpret_cast<std::intptr_t>(frame->data[1]));
    view.pts = m_impl->currentPts;
    view.limitedRange = m_impl->limitedRange;
    if (!view.texture) return FrameView{};

    D3D11_TEXTURE2D_DESC desc{};
    view.texture->GetDesc(&desc);
    view.format = desc.Format;
    view.textureWidth = desc.Width;
    view.textureHeight = desc.Height;
    // 縦横比には映像の実サイズを使い、UV はテクスチャ実サイズで正規化する
    view.width = (m_impl->contentWidth > 0) ? static_cast<UINT>(m_impl->contentWidth)
                                            : desc.Width;
    view.height = (m_impl->contentHeight > 0) ? static_cast<UINT>(m_impl->contentHeight)
                                              : desc.Height;
    view.tenBit = (desc.Format == DXGI_FORMAT_P010);

    // 実際のサーフェス形式でビット数を補正 (open 直後は推定のままであるため)
    m_impl->bits.store(view.tenBit ? 10 : 8, std::memory_order_relaxed);

    if (!m_impl->firstViewLogged) {
        m_impl->firstViewLogged = true;
        log::info("VideoDecoder: 描画ビュー fmt=0x%08X idx=%d 内容=%ux%u サーフェス=%ux%u "
                  "tenBit=%d range=%s pts=%.3f",
                  static_cast<unsigned long>(view.format), view.arrayIndex, view.width,
                  view.height, view.textureWidth, view.textureHeight, view.tenBit ? 1 : 0,
                  view.limitedRange ? "limited" : "full", view.pts);
    }

    m_hasFrame.store(true, std::memory_order_release);
    return view;
}

void VideoDecoder::releaseFrame() noexcept {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->releaseCurrent();
}

void VideoDecoder::seek(double seconds) noexcept {
    if (m_impl) m_impl->requestSeek(seconds);
}

double VideoDecoder::currentTime() const noexcept {
    if (m_extActive.load(std::memory_order_acquire)) {
        return m_extTime.load(std::memory_order_relaxed); // 外部バックエンド (VLC)
    }
    if (m_impl && m_impl->audioOnly.load(std::memory_order_relaxed) && m_clockSource &&
        m_clockSource->valid()) {
        return m_clockSource->pts(); // 映像クロックが無い → 音声クロックを返す
    }
    return m_clock.load(std::memory_order_relaxed);
}

void VideoDecoder::setExternalStatus(bool active, double timeSec, const Info& status) noexcept {
    if (!active) { // 先にフラグを倒してから値を捨てる
        m_extActive.store(false, std::memory_order_release);
        return;
    }
    m_extTime.store(timeSec, std::memory_order_relaxed);
    m_extDuration.store(status.duration, std::memory_order_relaxed);
    m_extWidth.store(status.width, std::memory_order_relaxed);
    m_extHeight.store(status.height, std::memory_order_relaxed);
    m_extFps.store(status.fps, std::memory_order_relaxed);
    m_extActive.store(true, std::memory_order_release);
}

ContextLock& VideoDecoder::contextMutex() const noexcept {
    return m_impl->d3dLock;
}

VideoDecoder::Info VideoDecoder::info() const {
    Info out;
    if (m_extActive.load(std::memory_order_acquire)) { // 外部バックエンド (VLC)
        out.width = m_extWidth.load(std::memory_order_relaxed);
        out.height = m_extHeight.load(std::memory_order_relaxed);
        out.fps = m_extFps.load(std::memory_order_relaxed);
        out.duration = m_extDuration.load(std::memory_order_relaxed);
        out.hardwareDecoding = false;
        out.videoStreamIndex = 0;
        out.videoTrackCount = 1;
        out.pixelFormatBits = 8;
        return out;
    }
    if (!m_impl) return out;
    out.width = m_impl->width;
    out.height = m_impl->height;
    out.fps = m_impl->fps;
    out.duration = m_impl->duration;
    out.videoStreamIndex = m_impl->videoStreamIndex;
    // D3D11VA hw config を持つデコーダで、かつ実際に hw フレームを得ている場合のみ
    // ハードウェアデコードとみなす (sw フレームが来たら false)
    const int hwState = m_impl->frameHwState.load(std::memory_order_relaxed);
    out.hardwareDecoding = m_impl->hwDevice != nullptr && m_impl->hwCapableCodec &&
                           hwState != 2;
    out.pixelFormatBits = m_impl->bits;
    out.videoTrackCount = 1; // TODO(phase1): 複数映像トラックの列挙
    return out;
}

} // namespace nanoalpha
