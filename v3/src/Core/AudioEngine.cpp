// =============================================================================
//  WASAPI 32bit Float レンダラ + 10-Band Biquad EQ (src/Core/AudioEngine.cpp)
//
//  パイプライン:
//    AudioDecoder (FFmpeg / swresample)
//        --submit(FLTP -> interleaved float, pts)-->
//      リングバッファ (バックプレッシャ付き)
//        --renderMain (イベント駆動 / MMCSS "Pro Audio")-->
//      ステレオモード -> 10-Band EQ -> 音量 -> WASAPI (32bit Float)
//        |
//        +--> MediaClock.publish(いま聞こえている位置)  <- 映像のマスタークロック
// =============================================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "Core/AudioEngine.hpp"

#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <avrt.h> // AvSetMmThreadCharacteristics

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

#pragma comment(lib, "avrt.lib")

namespace nanoalpha {
namespace {

constexpr UINT kChannels = 2;                // 内部処理チャンネル数 (Float32/2ch を要求)
constexpr double kRequestedBufferMs = 100.0; // 共有モード要求バッファ長 (100ns 単位で換算)

// ---- RBJ ピーキング EQ (biquad) ----
void designPeaking(float* outB0, float* outB1, float* outB2, float* outA1, float* outA2,
                   float frequency, float gainDb, float sampleRate, float q) {
    const float a = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = 2.0f * 3.14159265358979f * frequency / sampleRate;
    const float cosW0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * q);

    const float b0 = 1.0f + alpha * a;
    const float b1 = -2.0f * cosW0;
    const float b2 = 1.0f - alpha * a;
    const float a0 = 1.0f + alpha / a;
    const float a1 = -2.0f * cosW0;
    const float a2 = 1.0f - alpha / a;

    *outB0 = b0 / a0;
    *outB1 = b1 / a0;
    *outB2 = b2 / a0;
    *outA1 = a1 / a0;
    *outA2 = a2 / a0;
}

} // namespace

struct AudioEngine::Impl {
    // ---- WASAPI ----
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    WAVEFORMATEXTENSIBLE ours{};
    UINT32 bufferFrames = 0;
    bool comInitialized = false;
    DWORD comThreadId = 0; // CoInitializeEx を呼んだスレッド (均衡は同一スレッドのみ)
    bool comInitializedRender = false;

    std::thread renderThread;
    std::atomic<bool> running{false};
    std::atomic<bool> stopRender{false}; // 出力差し替え時だけレンダラを止める (m_stop は最終停止用)

    // ---- リングバッファ (インターリーブ float / kChannels) ----
    std::mutex mutex;
    std::condition_variable spaceCv; // submit 側の空き待ち
    std::vector<float> ring;
    UINT capacity = 0; // フレーム単位
    UINT pos = 0;      // 読み出し位置
    UINT count = 0;    // 蓄積フレーム数
    double headPts = 0.0;
    bool headPtsValid = false;

    // ---- EQ 状態 ----
    float b0[AudioEngine::kEqBands]{};
    float b1[AudioEngine::kEqBands]{};
    float b2[AudioEngine::kEqBands]{};
    float a1[AudioEngine::kEqBands]{};
    float a2[AudioEngine::kEqBands]{};
    float cachedGain[AudioEngine::kEqBands];
    bool eqInitialized = false;
    float z1[AudioEngine::kEqBands][kChannels]{};
    float z2[AudioEngine::kEqBands][kChannels]{};

    Impl() {
        for (float& g : cachedGain) g = 1.0e30f; // 初回必ず設計されるように
    }

    void resetEqState() {
        std::memset(z1, 0, sizeof(z1));
        std::memset(z2, 0, sizeof(z2));
    }
};

AudioEngine::AudioEngine() noexcept {
    for (auto& gain : m_eqGainDb) gain.store(0.0f, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine() { shutdown(); }

bool AudioEngine::init(UINT sampleRateHint, UINT channels) noexcept {
    shutdown();
    m_stop.store(false, std::memory_order_relaxed);
    m_impl = std::make_unique<Impl>();

    (void)channels; // 内部は常に 2ch (共有モードが自動ダウンミックスする)

    if (!openDevice()) {
        closeDevice();
        m_impl.reset();
        return false;
    }

    const UINT rate = m_sampleRate.load(std::memory_order_relaxed);
    if (rate == 0 || m_impl->bufferFrames == 0) {
        closeDevice();
        m_impl.reset();
        return false;
    }

    // リングは約 0.75 秒分 (デコード途切れへの耐性)
    m_impl->capacity = rate * 3 / 4;
    m_impl->ring.assign(static_cast<size_t>(m_impl->capacity) * kChannels, 0.0f);

    if (!ensureRenderThread()) {
        closeDevice();
        m_impl.reset();
        return false;
    }

    m_ready.store(true, std::memory_order_release);
    (void)sampleRateHint;
    log::info("AudioEngine: WASAPI 32bit Float OK  %uHz / %uch / %s  buffer=%u frames (%.0f ms)",
              rate, m_channels.load(std::memory_order_relaxed),
              m_exclusive.load(std::memory_order_relaxed) ? "exclusive" : "shared",
              m_impl->bufferFrames,
              m_impl->bufferFrames * 1000.0 / static_cast<double>(rate));
    return true;
}

bool AudioEngine::openDevice() noexcept {
    Impl& d = *m_impl;
    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&](const char* stage) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        if (ms >= 2) log::info("AudioEngine: init %s = %lld ms", stage, ms);
    };

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com)) {
        d.comInitialized = true; // S_FALSE も CoUninitialize で均衡させる
        d.comThreadId = GetCurrentThreadId();
    } else if (com != RPC_E_CHANGED_MODE) {
        log::info("AudioEngine: CoInitializeEx 失敗 hr=0x%08lX", static_cast<unsigned long>(com));
        return false;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&d.enumerator));
    if (FAILED(hr) || !d.enumerator) {
        log::info("AudioEngine: MMDeviceEnumerator 取得失敗 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }

    hr = d.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &d.device);
    if (FAILED(hr) || !d.device) {
        log::info("AudioEngine: 既定の出力デバイスが見つかりません hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }

    hr = d.device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                            reinterpret_cast<void**>(&d.client));
    if (FAILED(hr) || !d.client) {
        log::info("AudioEngine: IAudioClient 取得失敗 hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    elapsed("COM+endpoint");

    WAVEFORMATEX* mix = nullptr;
    hr = d.client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        log::info("AudioEngine: GetMixFormat 失敗 hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }
    d.mixFormat = mix;
    elapsed("GetMixFormat");

    const UINT rate = mix->nSamplesPerSec;

    // 常に 32bit Float / 2ch で要求し、共有モードの自動変換 (AUTOCONVERTPCM) に任せる
    std::memset(&d.ours, 0, sizeof(d.ours));
    d.ours.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    d.ours.Format.nSamplesPerSec = rate;
    d.ours.Format.nChannels = kChannels;
    d.ours.Format.wBitsPerSample = 32;
    d.ours.Format.nBlockAlign = static_cast<WORD>(kChannels * 4);
    d.ours.Format.nAvgBytesPerSec = rate * d.ours.Format.nBlockAlign;
    d.ours.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    d.ours.Samples.wValidBitsPerSample = 32;
    d.ours.dwChannelMask = 0x3; // SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT
    d.ours.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const LONGLONG requested = static_cast<LONGLONG>(kRequestedBufferMs * 10000.0); // 100ms

    const bool wantExclusive =
        m_outputMode.load(std::memory_order_relaxed) ==
        static_cast<std::uint8_t>(OutputMode::Exclusive);

    bool exclusiveOk = false;
    if (wantExclusive) {
        WAVEFORMATEX* closest = nullptr;
        // 排他は自動変換不可。デバイスが Float32/2ch を受け入れる場合のみ採用する。
        if (d.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &d.ours.Format, &closest) ==
            S_OK) {
            const HRESULT ihr =
                d.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                     requested, 0, &d.ours.Format, nullptr);
            if (SUCCEEDED(ihr)) {
                exclusiveOk = true;
            } else {
                log::info("AudioEngine: 排他初期化失敗 hr=0x%08lX → 共有へフォールバック",
                          static_cast<unsigned long>(ihr));
            }
        } else {
            log::info("AudioEngine: 排他モードは Float32/2ch 未サポート → 共有へフォールバック");
        }
        if (closest) CoTaskMemFree(closest);
    }

    if (!exclusiveOk) {
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        HRESULT shr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, requested, 0,
                                           &d.ours.Format, nullptr);
        if (shr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED || FAILED(shr)) {
            // 要求長が非対称なデバイスでは既定長で再試行する
            shr = d.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &d.ours.Format,
                                       nullptr);
        }
        if (FAILED(shr)) {
            log::info("AudioEngine: 共有モード初期化失敗 hr=0x%08lX",
                      static_cast<unsigned long>(shr));
            return false;
        }
    }
    m_exclusive.store(exclusiveOk, std::memory_order_relaxed);
    elapsed("Initialize");

    UINT32 bufferFrames = 0;
    if (FAILED(d.client->GetBufferSize(&bufferFrames)) || bufferFrames == 0) {
        log::info("AudioEngine: GetBufferSize 失敗");
        return false;
    }
    d.bufferFrames = bufferFrames;

    d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!d.event) return false;
    hr = d.client->SetEventHandle(d.event);
    if (FAILED(hr)) {
        log::info("AudioEngine: SetEventHandle 失敗 hr=0x%08lX", static_cast<unsigned long>(hr));
        return false;
    }

    hr = d.client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&d.render));
    if (FAILED(hr) || !d.render) {
        log::info("AudioEngine: IAudioRenderClient 取得失敗 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }

    hr = d.client->Start();
    if (FAILED(hr)) {
        log::info("AudioEngine: IAudioClient::Start 失敗 hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        return false;
    }
    elapsed("Start");

    m_sampleRate.store(rate, std::memory_order_relaxed);
    m_channels.store(kChannels, std::memory_order_relaxed);
    return true;
}

void AudioEngine::closeDevice() noexcept {
    if (!m_impl) return;
    Impl& d = *m_impl;
    if (d.client) d.client->Stop();
    if (d.client) d.client->Reset();
    if (d.render) { d.render->Release(); d.render = nullptr; }
    if (d.client) { d.client->Release(); d.client = nullptr; }
    if (d.device) { d.device->Release(); d.device = nullptr; }
    if (d.enumerator) { d.enumerator->Release(); d.enumerator = nullptr; }
    if (d.mixFormat) { CoTaskMemFree(d.mixFormat); d.mixFormat = nullptr; }
    if (d.event) { CloseHandle(d.event); d.event = nullptr; }
    // CoUninitialize は CoInitializeEx を呼んだスレッドでのみ有効
    if (d.comInitialized && d.comThreadId == GetCurrentThreadId()) {
        CoUninitialize();
        d.comInitialized = false;
    }
    d.comThreadId = 0;
}

bool AudioEngine::ensureRenderThread() noexcept {
    Impl& d = *m_impl;
    d.running.store(true, std::memory_order_release);
    d.renderThread = std::thread([this] { renderMain(); });
    return true;
}

void AudioEngine::shutdown() noexcept {
    m_stop.store(true, std::memory_order_release);
    m_ready.store(false, std::memory_order_release);
    m_clock.invalidate();

    if (m_impl) {
        {
            std::lock_guard<std::mutex> lock(m_impl->mutex);
            m_impl->spaceCv.notify_all();
        }
        if (m_impl->event) SetEvent(m_impl->event);
        if (m_impl->renderThread.joinable()) m_impl->renderThread.join();
        m_impl->running.store(false, std::memory_order_release);
        closeDevice();
        m_impl.reset();
    }
}

bool AudioEngine::reopenOutput() noexcept {
    if (!m_impl) return false;
    if (m_stop.load(std::memory_order_acquire)) return false; // 最終停止後は不可
    if (m_reopening.exchange(true, std::memory_order_acq_rel)) return false; // 重複防止

    Impl& d = *m_impl;
    m_clock.invalidate();

    // 1. レンダラスレッドだけを止める。Impl は残すので submit 中のワーカーは
    //    m_reopening を見て待機するだけで、解放済みメモリを指すことはない。
    d.stopRender.store(true, std::memory_order_release);
    if (d.event) SetEvent(d.event);
    if (d.renderThread.joinable()) d.renderThread.join();
    d.running.store(false, std::memory_order_release);

    // 2. デバイスを閉じて出力設定で開き直す
    closeDevice();
    bool ok = openDevice();
    if (!ok) {
        // 排他で失敗した場合は共有を強制して 1 回だけ再試行する
        m_outputMode.store(static_cast<std::uint8_t>(OutputMode::Shared),
                           std::memory_order_relaxed);
        ok = openDevice();
    }

    if (ok) {
        const UINT rate = m_sampleRate.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(d.mutex);
        if (rate > 0) {
            const UINT capacity = rate * 3 / 4;
            if (capacity != d.capacity) {
                // サンプルレートが変わったのでリングを作り直す。
                // submit も同一 mutex の中で memcpy しているため直列化される。
                d.capacity = capacity;
                d.ring.assign(static_cast<size_t>(capacity) * kChannels, 0.0f);
            }
            d.pos = 0;
            d.count = 0;
            d.headPtsValid = false;
            m_buffered.store(0, std::memory_order_relaxed);
        }
    }

    d.stopRender.store(false, std::memory_order_release);
    if (ok) ensureRenderThread();

    m_reopening.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.spaceCv.notify_all(); // 待機中の submit を起こす
    }
    m_ready.store(ok, std::memory_order_release);

    log::info("AudioEngine: 出力再オープン %s  %uHz / %uch / %s", ok ? "OK" : "失敗",
              m_sampleRate.load(std::memory_order_relaxed),
              m_channels.load(std::memory_order_relaxed),
              m_exclusive.load(std::memory_order_relaxed) ? "exclusive" : "shared");
    return ok;
}

void AudioEngine::reset() noexcept {
    if (!m_impl) return;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->pos = 0;
    m_impl->count = 0;
    m_impl->headPtsValid = false;
    m_impl->resetEqState();
    m_buffered.store(0, std::memory_order_relaxed);
    m_clock.invalidate();
    m_impl->spaceCv.notify_all();
}

AudioEngine::SubmitResult AudioEngine::submit(const float* pcm, UINT frames, double pts) noexcept {
    if (!m_impl || !pcm || frames == 0) return SubmitResult::Closed;

    Impl& d = *m_impl;
    std::unique_lock<std::mutex> lock(d.mutex);
    while (true) {
        if (m_stop.load(std::memory_order_acquire)) return SubmitResult::Closed;
        if (m_reopening.load(std::memory_order_acquire)) {
            // 出力差し替え中: リングは減らないので空きを待たず、完了通知を待つ。
            // Closed を返すとデコードワーカーが終了してしまうため絶対に戻らない。
            d.spaceCv.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }
        if (!d.running.load(std::memory_order_acquire)) return SubmitResult::Closed;
        if (d.count + frames <= d.capacity) break;

        d.spaceCv.wait_for(lock, std::chrono::milliseconds(20), [this, &d, frames] {
            return m_stop.load(std::memory_order_acquire) ||
                   m_reopening.load(std::memory_order_acquire) ||
                   d.count + frames <= d.capacity;
        });

        // 一時停止中はリングが満杯のまま。データはまだ格納していないので、
        // 呼び出し側 (AudioDecoder) が停止 / シークを監視しながら再試行できるよう
        // ここで中断を返す。空きができた場合はループ先頭で break する。
        if (m_paused.load(std::memory_order_acquire) && d.count + frames > d.capacity) {
            return SubmitResult::Interrupted;
        }
    }

    if (d.count == 0) {
        d.headPts = pts;
        d.headPtsValid = true;
    }

    const UINT tail = (d.pos + d.count) % d.capacity;
    const UINT first = (tail + frames <= d.capacity) ? frames : (d.capacity - tail);
    std::memcpy(d.ring.data() + static_cast<size_t>(tail) * kChannels, pcm,
                static_cast<size_t>(first) * kChannels * sizeof(float));
    if (first < frames) {
        std::memcpy(d.ring.data(), pcm + static_cast<size_t>(first) * kChannels,
                    static_cast<size_t>(frames - first) * kChannels * sizeof(float));
    }
    d.count += frames;
    m_buffered.store(d.count, std::memory_order_relaxed);
    return SubmitResult::Ok;
}

void AudioEngine::renderMain() noexcept {
    Impl& d = *m_impl;

    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(com)) d.comInitializedRender = true;

    // MMCSS: レンダラスレッドを「Pro Audio」優先度に置く (ドロップ / ジッタ防止)
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (!mmcss) mmcss = AvSetMmThreadCharacteristicsW(L"Playback", &taskIndex);

    const UINT rate = m_sampleRate.load(std::memory_order_relaxed);
    const double rateReciprocal = (rate > 0) ? 1.0 / static_cast<double>(rate) : 0.0;

    // 一時停止: IAudioClient::Stop() でデバイスを止め、リング (headPts) と
    // MediaClock を凍結する。バッファに残っている音は自然に再生し終わるため、
    // 復帰時に audible = headPts - padding が正しく計算できる。
    bool wasPaused = false;
    IAudioClient* lastClient = nullptr;

    while (!m_stop.load(std::memory_order_acquire) &&
           !d.stopRender.load(std::memory_order_acquire)) {
        const bool paused = m_paused.load(std::memory_order_acquire);
        const DWORD wait = WaitForSingleObject(d.event, paused ? 50u : 1000u);
        if (m_stop.load(std::memory_order_acquire) ||
            d.stopRender.load(std::memory_order_acquire)) {
            break;
        }

        // 一時停止状態の変化、または「一時停止中にデバイスが張り替えられた」場合のみ
        // Stop/Start を行う。IFileAudio::Start は init()/reopen 時に既に済んでいる
        // ため、通常運用で二重 Start を発生させない (AUDCLNT_E_* の誤検出を防ぐ)。
        const bool clientChanged = (d.client != lastClient);
        if (paused != wasPaused || (clientChanged && paused)) {
            wasPaused = paused;
            if (d.client) {
                const HRESULT state = paused ? d.client->Stop() : d.client->Start();
                if (FAILED(state) && !paused) {
                    log::info("AudioEngine: IAudioClient::Start 失敗 hr=0x%08lX",
                              static_cast<unsigned long>(state));
                }
            }
            if (paused) {
                // 一時停止中はクロックを発行しない → 最後の値がそのまま止まる
                log::info("AudioEngine: paused (clock frozen at %.3f s)",
                          m_clock.valid() ? m_clock.pts() : -1.0);
            }
        }
        lastClient = d.client;
        if (paused) continue;

        if (wait != WAIT_OBJECT_0) continue;
        if (!d.client || !d.render) break;

        UINT32 padding = 0;
        if (FAILED(d.client->GetCurrentPadding(&padding)) || padding > d.bufferFrames) break;
        const UINT32 available = d.bufferFrames - padding;
        if (available == 0) continue;

        BYTE* data = nullptr;
        HRESULT hr = d.render->GetBuffer(available, &data);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                log::info("AudioEngine: デバイスが無効化されました hr=0x%08lX",
                          static_cast<unsigned long>(hr));
                m_ready.store(false, std::memory_order_release);
            }
            break;
        }

        float* out = reinterpret_cast<float*>(data);
        double audible = 0.0;
        bool clockValid = false;
        UINT take = 0;
        const double mediaStep =
            rateReciprocal * static_cast<double>(m_mediaSpeed.load(std::memory_order_relaxed));

        // --- リングから転送 + マスタークロック算出 ---
        {
            std::lock_guard<std::mutex> lock(d.mutex);
            if (d.headPtsValid) {
                // 提出済みデータの終端 PTS − デバイスに積まれている残量 = 聴こえている位置
                // (残量も媒体時刻へ換算して、速度変更時も単位を揃える)
                audible = d.headPts - static_cast<double>(padding) * mediaStep;
                clockValid = true;
            }
            take = (available < d.count) ? available : d.count;
            if (take > 0) {
                const UINT first = (d.pos + take <= d.capacity) ? take : (d.capacity - d.pos);
                std::memcpy(out, d.ring.data() + static_cast<size_t>(d.pos) * kChannels,
                            static_cast<size_t>(first) * kChannels * sizeof(float));
                if (first < take) {
                    std::memcpy(out + static_cast<size_t>(first) * kChannels, d.ring.data(),
                                static_cast<size_t>(take - first) * kChannels * sizeof(float));
                }
                d.pos = (d.pos + take) % d.capacity;
                d.count -= take;
                d.headPts += static_cast<double>(take) * mediaStep;
                m_buffered.store(d.count, std::memory_order_relaxed);
            } else if (d.headPtsValid && d.count == 0) {
                m_underruns.fetch_add(1, std::memory_order_relaxed);
            }
        }

        // 不足分はゼロ埋め (無音)
        if (take < available) {
            std::memset(out + static_cast<size_t>(take) * kChannels, 0,
                        static_cast<size_t>(available - take) * kChannels * sizeof(float));
        }

        // --- ステレオモード ---
        const auto stereo = static_cast<StereoMode>(m_stereoMode.load(std::memory_order_relaxed));
        switch (stereo) {
        case StereoMode::LeftOnly:
            for (UINT i = 0; i < available; ++i) out[i * kChannels + 1] = out[i * kChannels];
            break;
        case StereoMode::RightOnly:
            for (UINT i = 0; i < available; ++i) out[i * kChannels] = out[i * kChannels + 1];
            break;
        case StereoMode::Reversed:
            for (UINT i = 0; i < available; ++i) std::swap(out[i * kChannels], out[i * kChannels + 1]);
            break;
        case StereoMode::Stereo:
            break;
        }

        // --- 10-Band EQ (ゲイン変化時のみ係数再設計) ---
        if (m_eqEnabled.load(std::memory_order_relaxed)) {
            for (int band = 0; band < kEqBands; ++band) {
                const float gain = m_eqGainDb[band].load(std::memory_order_relaxed);
                if (!d.eqInitialized || gain != d.cachedGain[band]) {
                    designPeaking(&d.b0[band], &d.b1[band], &d.b2[band], &d.a1[band], &d.a2[band],
                                  kEqBandFrequencies[band], gain, static_cast<float>(rate),
                                  1.414f);
                    d.cachedGain[band] = gain;
                }
            }
            d.eqInitialized = true;

            for (int band = 0; band < kEqBands; ++band) {
                const float b0 = d.b0[band], b1 = d.b1[band], b2 = d.b2[band];
                const float a1 = d.a1[band], a2 = d.a2[band];
                for (UINT c = 0; c < kChannels; ++c) {
                    float z1 = d.z1[band][c];
                    float z2 = d.z2[band][c];
                    for (UINT i = 0; i < available; ++i) {
                        const float x = out[i * kChannels + c];
                        const float y = b0 * x + z1;
                        z1 = b1 * x - a1 * y + z2;
                        z2 = b2 * x - a2 * y;
                        out[i * kChannels + c] = y;
                    }
                    d.z1[band][c] = z1;
                    d.z2[band][c] = z2;
                }
            }
        }

        // --- 音量 (1.0 未満は減衰 / 1.0 超はブースト + ソフトクリップ) ---
        const float volume = m_volume.load(std::memory_order_relaxed);
        const size_t total = static_cast<size_t>(available) * kChannels;
        if (volume >= 0.0f && volume < 0.999f) {
            for (size_t i = 0; i < total; ++i) out[i] *= volume;
        } else if (volume > 1.001f) {
            // ブースト時: 増幅すると ±1.0 を超えてハードクリップでボロボロ音になる。
            // 0.7 を境に tanh で滑らかに飽和させ、|1.0| へ収束させて耳障りな
            // 破綻を抑える (最大 400% を想定、数値指定でそれを超えても安全)。
            constexpr float knee = 0.7f;
            constexpr float head = 1.0f - knee;
            for (size_t i = 0; i < total; ++i) {
                const float boosted = out[i] * volume;
                if (boosted > knee) {
                    out[i] = knee + head * std::tanh((boosted - knee) / head);
                } else if (boosted < -knee) {
                    out[i] = -knee - head * std::tanh((-boosted - knee) / head);
                } else {
                    out[i] = boosted;
                }
            }
        }

        const HRESULT rel = d.render->ReleaseBuffer(available, 0);
        if (FAILED(rel)) break;

        m_framesPlayed.fetch_add(available, std::memory_order_relaxed);
        if (clockValid) m_clock.publish(audible);
    }

    if (d.client) d.client->Stop();
    m_ready.store(false, std::memory_order_release);
    m_clock.invalidate();
    d.running.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.spaceCv.notify_all(); // submit 中のスレッドを解放
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (d.comInitializedRender) {
        CoUninitialize();
        d.comInitializedRender = false;
    }
}

} // namespace nanoalpha
