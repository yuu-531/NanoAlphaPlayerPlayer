#pragma once
#ifndef NANOALPHA_CORE_AUDIO_DECODER_HPP
#define NANOALPHA_CORE_AUDIO_DECODER_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>

namespace nanoalpha {

class AudioEngine;
class PlayerController;

// =============================================================================
//  音声デコーダ (Phase 1)
// -----------------------------------------------------------------------------
//  メインスレッドから open/close、デコードは専用スレッドで行う。
//
//    FFmpeg デコード (AV_SAMPLE_FMT_FLTP)
//        -> libswresample (任意の入力を 32bit Float Planar / ステレオ / 出力レートへ)
//        -> 速度補正 (0.25x..4.0x、速度 1.0x は無補正パス)
//        -> AudioEngine::submit() (リングバッファ + WASAPI)
//
//  シークは PlayerController::requestSeek() 経由で映像と同時に実行される。
// =============================================================================
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    // engine は init 済みであること (sampleRate を取得する)。
    void attach(AudioEngine* engine, PlayerController* controller) noexcept;

    bool open(const std::wstring& source, int trackIndex = -1);
    void close() noexcept;

    // 単独シーク (映像と揃える必要がある場合は PlayerController::requestSeek を使う)
    void seek(double seconds) noexcept;

    bool isReady() const noexcept { return m_ready.load(std::memory_order_acquire); }
    bool ended() const noexcept { return m_ended.load(std::memory_order_acquire); }
    // 再生終了を扱い終えた側から呼ぶ (停止 / ループ終了時)。
    // ここを跨ぐと毎フレーム「終了」が再検出されて止まる / 送りが止まらなくなる。
    void clearEnded() noexcept { m_ended.store(false, std::memory_order_release); }
    double currentTime() const noexcept { return m_clock.load(std::memory_order_relaxed); }

    // 診断用: デコードスレッドの現在位置
    const char* stage() const noexcept { return m_stage.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_ended{false};
    std::atomic<double> m_clock{0.0};
    std::atomic<const char*> m_stage{"idle"};
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_AUDIO_DECODER_HPP
