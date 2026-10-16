#pragma once
#ifndef NANOALPHA_CORE_AUDIO_ENGINE_HPP
#define NANOALPHA_CORE_AUDIO_ENGINE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "Core/Log.hpp"
#include "Core/MediaClock.hpp"

namespace nanoalpha {

// =============================================================================
//  WASAPI 32-bit Float オーディオエンジン + 10-Band EQ (Phase 1)
// -----------------------------------------------------------------------------
//  [仕様]
//   ・デコード PCM は libswresample で AV_SAMPLE_FMT_FLTP (32bit Float Planar) に統一
//   ・10-Band Biquad EQ はすべて float 高精度演算 (31.5Hz .. 16kHz, オクターブ帯域)
//   ・WASAPI へ KSDATAFORMAT_SUBTYPE_IEEE_FLOAT (32bit Float) でダイレクト伝送
//   ・Shared / Exclusive モードを切替可能 (排他で失敗した場合は共有へ自動フォールバック)
//   ・EQ ゲイン / 音量 / ステレオモードはアトミックで保持し、
//     UI 操作 → 音声スレッドへ即時反映 (100ms 応答)
//
//  [構成]
//   AudioDecoder (FFmpeg) --submit()--> リングバッファ --renderMain()--> WASAPI
//                                        ^                      |
//                                        +---- MediaClock ------+
//                                             (映像が参照するマスタークロック)
// =============================================================================
class AudioEngine {
public:
    static constexpr int kEqBands = 10;
    static constexpr float kEqBandFrequencies[kEqBands] = {
        31.5f, 63.0f, 125.0f, 250.0f, 500.0f,
        1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f
    };

    enum class OutputMode : std::uint8_t { Shared = 0, Exclusive };
    enum class StereoMode : std::uint8_t { Stereo = 0, LeftOnly, RightOnly, Reversed };

    // submit() の結果。Interrupted は「まだ格納していない」ことを意味するので、
    // 呼び出し元は同じデータを保持したまま再試行してもよい (一時停止中の満杯時)。
    enum class SubmitResult : std::uint8_t { Ok = 0, Interrupted, Closed };

    AudioEngine() noexcept; // 定義は .cpp (PIMPL のためコンパイラ生成の例外伝播コードが必要)

    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Phase 1: 100ms 予算内で WASAPI デバイス取得 → レンダラスレッド起動まで完了させる。
    // mixFormat (既定は 48kHz / 2ch / 32bit Float) の共有モードで初期化し、
    // AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM で入力フォーマットを自動変換する。
    bool init(UINT sampleRateHint = 0, UINT channels = 2) noexcept;

    // レンダラスレッド停止・COM 後片付け (呼び出し元はメインスレッド)
    void shutdown() noexcept;

    // 出力設定 (Shared/Exclusive) をデバイスへ適用し直す。
    // Impl (リング / ミューテックス / CV) は破棄せず、レンダラスレッドと
    // IAudioClient だけを差し替える。その間 submit() は m_reopening を見て
    // ブロックし、false を返さないのでデコードワーカーは生き続ける。
    // (メインスレッドから同期的に呼ぶ。所要はデバイス初期化と同じ数十 ms。)
    bool reopenOutput() noexcept;
    bool reopening() const noexcept { return m_reopening.load(std::memory_order_relaxed); }

    // シーク時: リングバッファとマスタークロックを破棄する (音声スレッドから呼ぶ)
    void reset() noexcept;

    // デコード済み PCM (インターリーブ float / ch チャンネル) をリングへ積む。
    // 満杯時は空きができるまでブロックする (バックプレッシャ)。
    // pts は先頭サンプルの媒体時刻 (秒)。
    SubmitResult submit(const float* pcm, UINT frames, double pts) noexcept;

    // ---- 一時停止 (v2) ----
    // 有効時はレンダラスレッドが IAudioClient::Stop() をかけ、リングと
    // MediaClock を完全に凍結する (映像のマスタークロックが止まる)。
    // レンダラが止まるためリングは満杯になり、submit() は Interrupted を返す。
    void setPaused(bool paused) noexcept {
        m_paused.store(paused, std::memory_order_release);
    }
    bool paused() const noexcept { return m_paused.load(std::memory_order_acquire); }

    bool isReady() const noexcept { return m_ready.load(std::memory_order_acquire); }
    OutputMode outputMode() const noexcept {
        return static_cast<OutputMode>(m_outputMode.load(std::memory_order_relaxed));
    }
    StereoMode stereoMode() const noexcept {
        return static_cast<StereoMode>(m_stereoMode.load(std::memory_order_relaxed));
    }
    UINT sampleRate() const noexcept { return m_sampleRate.load(std::memory_order_relaxed); }
    UINT channels() const noexcept { return m_channels.load(std::memory_order_relaxed); }
    bool exclusive() const noexcept { return m_exclusive.load(std::memory_order_relaxed); }

    // 映像側が参照するマスタークロック (= いま聞こえている位置)
    const MediaClock& clock() const noexcept { return m_clock; }

    // 再生速度 (0.25..4.0)。マスタークロックを「媒体時刻」で保つため、
    // リング消費時に headPts を (フレーム数 / レート) × 速度 だけ進める。
    void setMediaSpeed(float speed) noexcept {
        if (speed < 0.25f) speed = 1.0f;
        if (speed > 4.0f) speed = 4.0f;
        m_mediaSpeed.store(speed, std::memory_order_relaxed);
    }
    float mediaSpeed() const noexcept { return m_mediaSpeed.load(std::memory_order_relaxed); }

    // ---- 音量 (線形 / 1.0 = 100%) ----
    // [v3] 音量ブースト対応: 1.0 を超える値を許可する (スライダーは 0..400%)。
    // 上限は UI 側の制約であってエンジン側では設けない (数値指定は無制限)。
    // ただし NaN / inf は排除し、異常値による音声スレッドの暴走を防ぐ。
    void setVolume(float volume) noexcept {
        if (!(volume >= 0.0f)) volume = 0.0f; // NaN / 負値
        else if (!(volume < 1.0e6f)) volume = 1.0e6f; // inf のみ遮断 (UI での上限ではない)
        m_volume.store(volume, std::memory_order_relaxed);
    }
    float volume() const noexcept { return m_volume.load(std::memory_order_relaxed); }

    // ---- 10-Band EQ (dB, -24.0 .. +24.0) ----
    // UI スレッドから呼んでも音声スレッドが relaxed 読みするだけで安全。
    void setEqGainDb(int band, float decibels) noexcept {
        if (band < 0 || band >= kEqBands) return;
        if (decibels < -24.0f) decibels = -24.0f;
        if (decibels > 24.0f) decibels = 24.0f;
        m_eqGainDb[band].store(decibels, std::memory_order_relaxed);
    }

    float eqGainDb(int band) const noexcept {
        if (band < 0 || band >= kEqBands) return 0.0f;
        return m_eqGainDb[band].load(std::memory_order_relaxed);
    }

    void setEqEnabled(bool enabled) noexcept {
        m_eqEnabled.store(enabled, std::memory_order_relaxed);
    }
    bool eqEnabled() const noexcept { return m_eqEnabled.load(std::memory_order_relaxed); }

    void setOutputMode(OutputMode mode) noexcept {
        m_outputMode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }

    void setStereoMode(StereoMode mode) noexcept {
        m_stereoMode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }

    // ---- 診断 (UI 表示 / ログ用) ----
    std::uint64_t framesPlayed() const noexcept {
        return m_framesPlayed.load(std::memory_order_relaxed);
    }
    std::uint32_t underruns() const noexcept {
        return m_underruns.load(std::memory_order_relaxed);
    }
    UINT bufferedFrames() const noexcept {
        return m_buffered.load(std::memory_order_relaxed);
    }

private:
    struct Impl;

    bool openDevice() noexcept;   // COM/MMDevice/IAudioClient 構築 (init の中で呼ぶ)
    void closeDevice() noexcept;
    void renderMain() noexcept;   // WASAPI イベント駆動レンダラスレッド
    bool ensureRenderThread() noexcept;

    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_paused{false}; // 一時停止中 (WASAPI Stop + クロック凍結)
    std::atomic<bool> m_reopening{false}; // デバイス差し替え中 (submit は待機する)
    std::atomic<bool> m_eqEnabled{true};
    std::atomic<float> m_eqGainDb[kEqBands];
    std::atomic<std::uint8_t> m_outputMode{static_cast<std::uint8_t>(OutputMode::Shared)};
    std::atomic<std::uint8_t> m_stereoMode{static_cast<std::uint8_t>(StereoMode::Stereo)};
    std::atomic<UINT> m_sampleRate{0};
    std::atomic<UINT> m_channels{2};
    std::atomic<bool> m_exclusive{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_mediaSpeed{1.0f};

    std::atomic<std::uint64_t> m_framesPlayed{0};
    std::atomic<std::uint32_t> m_underruns{0};
    std::atomic<UINT> m_buffered{0};

    MediaClock m_clock;

    std::unique_ptr<Impl> m_impl;
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_AUDIO_ENGINE_HPP
