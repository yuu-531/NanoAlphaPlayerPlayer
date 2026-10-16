#pragma once
#ifndef NANOALPHA_CORE_MEDIA_CLOCK_HPP
#define NANOALPHA_CORE_MEDIA_CLOCK_HPP

#include <atomic>

namespace nanoalpha {

// =============================================================================
//  メディアクロック (音声マスター)
// -----------------------------------------------------------------------------
//  WASAPI へ書き出したサンプルの PTS を AudioEngine が発行し、
//  VideoDecoder の pace() がそれを読んでフレーム表示タイミングを決定する。
//  音声が存在しない / 速度変更中 (音声が追いつけない) 時は valid() == false となり、
//  映像は従来どおり steady_clock アンカーで自律ペーシングする。
//
//  すべて lock-free (atomic) なので UI / 音声 / 映像スレッドから無条件に読める。
// =============================================================================
class MediaClock {
public:
    void invalidate() noexcept { m_valid.store(false, std::memory_order_release); }

    void publish(double pts) noexcept {
        m_pts.store(pts, std::memory_order_relaxed);
        m_valid.store(true, std::memory_order_release);
    }

    bool valid() const noexcept { return m_valid.load(std::memory_order_acquire); }

    double pts() const noexcept { return m_pts.load(std::memory_order_relaxed); }

private:
    std::atomic<double> m_pts{0.0};
    std::atomic<bool> m_valid{false};
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_MEDIA_CLOCK_HPP
