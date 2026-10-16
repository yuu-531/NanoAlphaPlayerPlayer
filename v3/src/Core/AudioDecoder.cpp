// =============================================================================
//  音声デコーダ (src/Core/AudioDecoder.cpp)
//
//  FFmpeg デコード -> libswresample (FLTP / ステレオ / 出力レート) ->
//  速度補正 (1.0x は無補正パス) -> AudioEngine::submit() -> WASAPI
//
//  シークは PlayerController::requestSeek() 経由で映像と同時に走る。
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/AudioDecoder.hpp"

#include "Core/AudioEngine.hpp"
#include "Core/Log.hpp"
#include "Core/PlayerController.hpp"
#include "Core/SpeedStretcher.hpp"

// FFmpeg ヘッダはサードパーティなので自前の警告レベルに混ぜない
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4245 4267 4305)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace nanoalpha {
namespace {

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (length <= 0) return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(),
                        length, nullptr, nullptr);
    return out;
}

std::string ffError(int code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof(buffer));
    return buffer;
}

} // namespace

struct AudioDecoder::Impl {
    AudioDecoder& owner;

    explicit Impl(AudioDecoder& self) : owner(self) {}

    AudioEngine* engine = nullptr;
    PlayerController* controller = nullptr;

    // ---- FFmpeg (デコードスレッドのみが触る) ----
    AVFormatContext* format = nullptr;
    AVCodecContext* decoder = nullptr;
    AVStream* stream = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    int streamIndex = -1;
    int inRate = 0;
    int outRate = 0;

    // ---- スレッド制御 ----
    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopRequested = false;
    double seekTarget = -1.0;
    double pendingSeek = -1.0;
    std::uint64_t seenSeekRevision = 0;
    bool flushSent = false;
    bool eof = false; // デコード枯渇 (シークで解除)

    // ---- 変換 / 速度補正 ----
    SpeedStretcher stretcher;
    std::vector<float> planar[2];
    std::vector<float> outPcm;
    float speed = 1.0f;

    // ---------------------------------------------------------------- 開閉
    void closeFile() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = true;
        }
        cv.notify_all();
        if (worker.joinable()) worker.join();

        if (swr) { swr_free(&swr); swr = nullptr; }
        if (decoder) { avcodec_free_context(&decoder); decoder = nullptr; }
        if (format) { avformat_close_input(&format); format = nullptr; }
        if (packet) { av_packet_free(&packet); packet = nullptr; }
        if (frame) { av_frame_free(&frame); frame = nullptr; }
        stream = nullptr;
        streamIndex = -1;
        stretcher.reset();
        owner.m_ready.store(false, std::memory_order_release);
        owner.m_ended.store(false, std::memory_order_release);
        owner.m_stage.store("idle", std::memory_order_relaxed);
    }

    bool openFile(const std::wstring& path, int trackIndex) {
        AudioEngine* audioEngine = engine;
        if (!audioEngine || !audioEngine->isReady()) {
            log::info("AudioDecoder: 音声エンジン未初期化のため無音で起動します");
            return false;
        }
        const int deviceRate = static_cast<int>(audioEngine->sampleRate());
        if (deviceRate <= 0) return false;

        const std::string utf8Path = wideToUtf8(path);
        AVFormatContext* fmt = avformat_alloc_context();
        if (!fmt) return false;

        int ret = avformat_open_input(&fmt, utf8Path.c_str(), nullptr, nullptr);
        if (ret < 0) {
            // 失敗時は FFmpeg 自身がコンテキストを解放し NULL で返す
            log::info("AudioDecoder: 入力を開けません: %s", ffError(ret).c_str());
            return false;
        }
        ret = avformat_find_stream_info(fmt, nullptr);
        if (ret < 0) {
            log::info("AudioDecoder: find_stream_info 失敗: %s", ffError(ret).c_str());
            avformat_close_input(&fmt);
            return false;
        }

        int index = -1;
        if (trackIndex >= 0) {
            // 指定インデックスが音声ならそれを使う
            if (trackIndex < static_cast<int>(fmt->nb_streams) &&
                fmt->streams[trackIndex]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                index = trackIndex;
            } else {
                log::info("AudioDecoder: 指定トラック %d は音声ではありません (自動選択)",
                          trackIndex);
            }
        }
        if (index < 0) {
            index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        }
        if (index < 0) {
            log::info("AudioDecoder: 音声ストリームが見つかりません (映像のみ)");
            avformat_close_input(&fmt);
            return false;
        }

        AVStream* audioStream = fmt->streams[index];
        const AVCodec* codec = avcodec_find_decoder(audioStream->codecpar->codec_id);
        if (!codec) {
            log::info("AudioDecoder: コーデックが見つかりません (id=%d)",
                      static_cast<int>(audioStream->codecpar->codec_id));
            avformat_close_input(&fmt);
            return false;
        }

        AVCodecContext* dec = avcodec_alloc_context3(codec);
        if (!dec) {
            avformat_close_input(&fmt);
            return false;
        }
        avcodec_parameters_to_context(dec, audioStream->codecpar);
        dec->pkt_timebase = audioStream->time_base;
        ret = avcodec_open2(dec, codec, nullptr);
        if (ret < 0) {
            log::info("AudioDecoder: avcodec_open2 失敗: %s", ffError(ret).c_str());
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        if (dec->sample_rate <= 0 || dec->ch_layout.nb_channels <= 0) {
            log::info("AudioDecoder: 入力フォーマット不明 (rate=%d ch=%d)", dec->sample_rate,
                      dec->ch_layout.nb_channels);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        // --- swresample: 入力 -> 32bit Float Planar / ステレオ / 出力レート ---
        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, 2);
        SwrContext* resampler = nullptr;
        ret = swr_alloc_set_opts2(&resampler, &outLayout, AV_SAMPLE_FMT_FLTP, deviceRate,
                                  &dec->ch_layout, dec->sample_fmt, dec->sample_rate, 0, nullptr);
        av_channel_layout_uninit(&outLayout);
        if (ret < 0 || !resampler || swr_init(resampler) < 0) {
            log::info("AudioDecoder: swresample 初期化失敗: %s",
                      ret < 0 ? ffError(ret).c_str() : "unknown");
            if (resampler) swr_free(&resampler);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        AVPacket* pkt = av_packet_alloc();
        AVFrame* f = av_frame_alloc();
        if (!pkt || !f) {
            av_packet_free(&pkt);
            av_frame_free(&f);
            swr_free(&resampler);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            return false;
        }

        format = fmt;
        decoder = dec;
        swr = resampler;
        packet = pkt;
        frame = f;
        stream = audioStream;
        streamIndex = index;
        inRate = dec->sample_rate;
        this->outRate = deviceRate;
        flushSent = false;
        stretcher.reset();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopRequested = false;
            seekTarget = -1.0;
            pendingSeek = -1.0;
            seenSeekRevision = 0;
        }

        owner.m_ready.store(true, std::memory_order_release);
        owner.m_ended.store(false, std::memory_order_release);
        owner.m_stage.store("start", std::memory_order_relaxed);
        worker = std::thread([this] { workerMain(); });

        log::info("AudioDecoder: open OK  track=%d  %s  %dHz -> %dHz  ch=%d  (%s)",
                  index, codec->name, inRate, deviceRate, dec->ch_layout.nb_channels,
                  av_get_sample_fmt_name(dec->sample_fmt));
        return true;
    }

    // ---------------------------------------------------------------- 再生位置
    double framePts(const AVFrame* f) const {
        const int64_t stamp = f->best_effort_timestamp;
        if (stamp == AV_NOPTS_VALUE || !stream) {
            return owner.m_clock.load(std::memory_order_relaxed);
        }
        return stamp * av_q2d(stream->time_base);
    }

    void performSeek(double seconds) {
        if (!format || !stream) return;
        if (seconds < 0.0) seconds = 0.0;

        owner.m_stage.store("seek", std::memory_order_relaxed);
        const double base = av_q2d(stream->time_base);
        const int64_t ts = static_cast<int64_t>(seconds / base);
        int ret = avformat_seek_file(format, streamIndex, INT64_MIN, ts, ts, 0);
        if (ret < 0) ret = av_seek_frame(format, streamIndex, ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            log::info("AudioDecoder: seek 失敗: %s", ffError(ret).c_str());
            owner.m_stage.store("idle", std::memory_order_relaxed);
            return;
        }
        avcodec_flush_buffers(decoder);
        if (swr) { // リサンプラ内部状態を破棄
            swr_close(swr);
            swr_init(swr);
        }
        flushSent = false;
        stretcher.reset();
        if (engine) engine->reset(); // リングとマスタークロックを破棄
        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
        owner.m_ended.store(false, std::memory_order_release);
        owner.m_clock.store(seconds, std::memory_order_relaxed);
        owner.m_stage.store("idle", std::memory_order_relaxed);
        log::info("AudioDecoder: seek -> %.3f s", seconds);
    }

    // ---------------------------------------------------------------- 出力
    void outputFrame(AVFrame* f) {
        if (!engine || !swr || outRate <= 0) return;

        const int delay = static_cast<int>(swr_get_delay(swr, inRate));
        const int outSamples =
            static_cast<int>(av_rescale(delay + f->nb_samples, outRate, inRate));
        if (outSamples <= 0) return;

        if (static_cast<int>(planar[0].size()) < outSamples) {
            planar[0].resize(static_cast<size_t>(outSamples));
            planar[1].resize(static_cast<size_t>(outSamples));
        }
        uint8_t* planes[2] = {reinterpret_cast<uint8_t*>(planar[0].data()),
                              reinterpret_cast<uint8_t*>(planar[1].data())};
        const int produced =
            swr_convert(swr, planes, outSamples, const_cast<const uint8_t**>(f->extended_data),
                        f->nb_samples);
        if (produced <= 0) return;

        const double pts = framePts(f);

        // 速度に応じた前進量をエンジンへ通知 (マスタークロックを媒体時刻で保つため)
        PlayerController::Snapshot snap;
        if (controller) snap = controller->snapshot();
        speed = snap.speed;
        engine->setMediaSpeed(speed);

        stretcher.push(planar[0].data(), planar[1].data(), produced, pts);

        double firstPts = 0.0;
        const size_t frames = stretcher.produce(outPcm, firstPts, speed, outRate);
        if (frames == 0) return;

        // 一時停止中はレンダラが止まるためリングが満杯になり Interrupted が返る。
        // この時点ではまだリングへ格納していないので、出力データを保持したまま
        // 停止 / シーク要求を監視しつつ空きができるまで再試行する。
        // (ここで待つ間もワーカーは動いているので、一時停止中でもシークに追従できる)
        for (;;) {
            const AudioEngine::SubmitResult res =
                engine->submit(outPcm.data(), static_cast<UINT>(frames), firstPts);
            if (res == AudioEngine::SubmitResult::Ok) break;

            if (res == AudioEngine::SubmitResult::Closed) {
                // エンジン停止 (デバイス喪失など) → スレッドを終える
                std::lock_guard<std::mutex> lock(mutex);
                stopRequested = true;
                return;
            }

            owner.m_stage.store("pause-wait", std::memory_order_relaxed);
            std::unique_lock<std::mutex> lock(mutex);
            if (stopRequested) return;
            if (seekTarget >= 0.0) return;
            if (controller && controller->takeSeek(seenSeekRevision)) {
                // 一時停止中にシークされた → 現在の出力は捨てて先へ進む
                pendingSeek = controller->seekTargetSeconds();
                cv.notify_all();
                return;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        owner.m_stage.store("idle", std::memory_order_relaxed);
        // m_clock は媒体時刻。出力 samples 分で進む媒体時間は (frames * speed / rate)。
        owner.m_clock.store(firstPts + static_cast<double>(frames) * speed / outRate,
                            std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- デコード
    void workerMain() {
        using namespace std::chrono;

        while (true) {
            // --- シーク要求 (内部 / PlayerController 経由) ---
            double seekTo = -1.0;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (stopRequested) break;
                if (seekTarget >= 0.0) {
                    seekTo = seekTarget;
                    seekTarget = -1.0;
                } else if (pendingSeek >= 0.0) {
                    seekTo = pendingSeek;
                    pendingSeek = -1.0;
                } else if (controller && controller->takeSeek(seenSeekRevision)) {
                    seekTo = controller->seekTargetSeconds();
                }

                if (seekTo < 0.0 && eof) {
                    // EOF: シークか停止が来るまで待つ (CPU スピン防止)
                    owner.m_stage.store("eof-wait", std::memory_order_relaxed);
                    cv.wait_for(lock, milliseconds(100), [this] {
                        if (stopRequested || seekTarget >= 0.0 || pendingSeek >= 0.0) return true;
                        if (controller && controller->takeSeek(seenSeekRevision)) {
                            pendingSeek = controller->seekTargetSeconds();
                            return true;
                        }
                        return false;
                    });
                    if (stopRequested) break;
                    if (seekTo < 0.0 && seekTarget < 0.0 && pendingSeek < 0.0) continue;
                    if (seekTo < 0.0) {
                        if (seekTarget >= 0.0) { seekTo = seekTarget; seekTarget = -1.0; }
                        else { seekTo = pendingSeek; pendingSeek = -1.0; }
                    }
                }
            }
            if (seekTo >= 0.0) {
                eof = false;
                performSeek(seekTo);
                continue;
            }

            // --- デコード ---
            owner.m_stage.store("receive", std::memory_order_relaxed);
            const int ret = avcodec_receive_frame(decoder, frame);
            owner.m_stage.store("idle", std::memory_order_relaxed);

            if (ret == 0) {
                outputFrame(frame);
                av_frame_unref(frame);
                continue;
            }
            if (ret == AVERROR_EOF) {
                eof = true;
                owner.m_ended.store(true, std::memory_order_release);
                continue;
            }
            if (ret != AVERROR(EAGAIN)) {
                log::info("AudioDecoder: avcodec_receive_frame: %s", ffError(ret).c_str());
                std::this_thread::sleep_for(milliseconds(5));
                continue;
            }

            owner.m_stage.store("read", std::memory_order_relaxed);
            const int read = av_read_frame(format, packet);
            owner.m_stage.store("idle", std::memory_order_relaxed);

            if (read < 0) {
                if (!flushSent) {
                    owner.m_stage.store("flush", std::memory_order_relaxed);
                    avcodec_send_packet(decoder, nullptr); // デコーダへフラッシュ
                    owner.m_stage.store("idle", std::memory_order_relaxed);
                    flushSent = true;
                }
                continue;
            }

            if (packet->stream_index == streamIndex) {
                owner.m_stage.store("send", std::memory_order_relaxed);
                const int sent = avcodec_send_packet(decoder, packet);
                owner.m_stage.store("idle", std::memory_order_relaxed);
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    log::info("AudioDecoder: avcodec_send_packet: %s", ffError(sent).c_str());
                }
            }
            av_packet_unref(packet);
        }

        owner.m_stage.store("stopped", std::memory_order_relaxed);
    }
};

AudioDecoder::AudioDecoder() : m_impl(std::make_unique<Impl>(*this)) {}

AudioDecoder::~AudioDecoder() { close(); }

void AudioDecoder::attach(AudioEngine* engine, PlayerController* controller) noexcept {
    if (m_impl) {
        m_impl->engine = engine;
        m_impl->controller = controller;
    }
}

bool AudioDecoder::open(const std::wstring& source, int trackIndex) {
    if (!m_impl) return false;
    close();
    m_impl->stopRequested = false;
    const bool ok = m_impl->openFile(source, trackIndex);
    if (!ok) {
        m_ready.store(false, std::memory_order_release);
        log::info("AudioDecoder: オープン失敗 (無音で続行)");
    }
    return ok;
}

void AudioDecoder::close() noexcept {
    if (m_impl) m_impl->closeFile();
    m_ready.store(false, std::memory_order_release);
    m_ended.store(false, std::memory_order_release);
}

void AudioDecoder::seek(double seconds) noexcept {
    if (!m_impl) return;
    if (m_impl->controller) {
        m_impl->controller->requestSeek(seconds); // 映像と同時に動かす
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->seekTarget = seconds;
    }
    m_impl->cv.notify_all();
}

} // namespace nanoalpha
