#pragma once
#ifndef NANOALPHA_CORE_PLAYER_CONTROLLER_HPP
#define NANOALPHA_CORE_PLAYER_CONTROLLER_HPP

#include <atomic>
#include <cstdint>

namespace nanoalpha {

// =============================================================================
//  再生制御プレーン (Phase 1 / v2 画質制御を追加)
// -----------------------------------------------------------------------------
//  すべての操作はロックレスで 100ms 応答を保証する。
//  描画 / 音声スレッドは snapshot() で一貫した状態 (seqlock) を取得する。
//  制御 UI は setter を直接叩けば、次フレームからパイプラインへ反映される。
// =============================================================================
class PlayerController {
public:
    enum class Aspect : std::uint8_t { Auto = 0, R16x9, R4x3, R21x9, R1x1 };
    enum class Flip : std::uint8_t { None = 0, Horizontal, Vertical, Both };
    enum class StereoMode : std::uint8_t { Stereo = 0, LeftOnly, RightOnly, Reversed };

    // ---- 画質 (v2) ---------------------------------------------------------
    // Fsr (v3): 拡大時のみ FidelityFX FSR1 (EASU+RCAS) を経由する。
    // 縮小や非拡大時は Linear へ自動フォールバックする (レンダラ側で判定)。
    enum class ScaleFilter : std::uint8_t { Linear = 0, Point, Anisotropic, Fsr };
    enum class RangeMode : std::uint8_t { Auto = 0, Limited, Full };
    // デコード品質プリセット: H.264 のデブロック / 非参照フレームの扱いを切り替える
    enum class DecodeQuality : std::uint8_t { Fast = 0, Normal, High };

    // 再生終了時のリピート動作 (VLC のループボタン相当)
    //   Off = 詰まったら停止 / All = プレイリスト全体をループ / One = 1曲をループ
    enum class RepeatMode : std::uint8_t { Off = 0, All = 1, One = 2 };

    // 描画/音声スレッドが読む一貫スナップショット
    struct Snapshot {
        std::uint64_t revision = 0;      // 偶数 = 書き込み中でない
        float speed = 1.0f;              // 0.25x .. 4.00x (ピッチ維持)
        std::int32_t rotationDeg = 0;    // 0 / 90 / 180 / 270
        Flip flip = Flip::None;
        Aspect aspect = Aspect::Auto;
        float crop[4] = {0, 0, 0, 0};    // 正規化 L,T,R,B (除去する量)
        bool subtitleVisible = false;
        bool abRepeat = false;
        double pointA = 0.0;             // 秒
        double pointB = 0.0;
        RepeatMode repeatMode = RepeatMode::Off; // 再生終了時のリピート (Off/All/One)
        StereoMode stereo = StereoMode::Stereo;
        std::int32_t videoTrack = -1;    // -1 = 自動
        std::int32_t audioTrack = -1;
        std::int32_t subtitleTrack = -1;

        // ---- v2: 一時停止 / 画質 ----
        bool paused = false;
        bool frameGen = false;           // フレーム補間 (前フレームとのブレンド)
        // 補間倍率 (v3): 表示レートを "この倍率" 倍にする。
        // N 個の中間位相を量子化して描画するため、実効表示レートは N 分の 1 刻みになる。
        std::int32_t frameGenFactor = 4; // 2 / 4 / 8 / 16
        float brightness = 0.0f;         // -0.50 .. +0.50
        float contrast = 1.0f;           //  0.50 .. +1.50
        float saturation = 1.0f;         //  0.00 .. +2.00
        float hueDeg = 0.0f;             // -180 .. +180
        float sharpness = 0.0f;          //  0.00 .. +1.00 (アンシャープマスク)
        ScaleFilter scaleFilter = ScaleFilter::Linear;
        RangeMode rangeMode = RangeMode::Auto;
        DecodeQuality decodeQuality = DecodeQuality::Normal;
    };

    PlayerController() noexcept {
        for (auto& c : m_crop) c.store(0.0f, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- 映像系
    void setRotation(int degrees) noexcept {
        int normalized = degrees % 360;
        if (normalized < 0) normalized += 360;
        normalized = (normalized / 90) * 90; // 90° 単位に丸め
        WriteGuard guard(*this);
        m_rotationDeg.store(normalized, std::memory_order_relaxed);
    }

    void rotate(int deltaDegrees) noexcept {
        setRotation(m_rotationDeg.load(std::memory_order_relaxed) + deltaDegrees);
    }

    void setFlip(Flip flip) noexcept {
        WriteGuard guard(*this);
        m_flip.store(static_cast<std::uint8_t>(flip), std::memory_order_relaxed);
    }

    void setAspect(Aspect aspect) noexcept {
        WriteGuard guard(*this);
        m_aspect.store(static_cast<std::uint8_t>(aspect), std::memory_order_relaxed);
    }

    void setCrop(float left, float top, float right, float bottom) noexcept {
        WriteGuard guard(*this);
        m_crop[0].store(clampf(left, 0.0f, 0.9f), std::memory_order_relaxed);
        m_crop[1].store(clampf(top, 0.0f, 0.9f), std::memory_order_relaxed);
        m_crop[2].store(clampf(right, 0.0f, 0.9f), std::memory_order_relaxed);
        m_crop[3].store(clampf(bottom, 0.0f, 0.9f), std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- 音声系
    void setStereoMode(StereoMode mode) noexcept {
        WriteGuard guard(*this);
        m_stereo.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- 再生系
    void setSpeed(float speed) noexcept {   // 0.25x .. 4.00x (ピッチ維持)
        WriteGuard guard(*this);
        m_speed.store(clampf(speed, 0.25f, 4.0f), std::memory_order_relaxed);
    }

    // 一時停止: 映像は pace() が停止し、音声は AudioEngine が WASAPI を Stop する。
    // MediaClock は停止中の値をそのまま保持する (＝映像のマスターが止まる)。
    void setPaused(bool paused) noexcept {
        WriteGuard guard(*this);
        m_paused.store(paused, std::memory_order_relaxed);
    }

    void setSubtitleVisible(bool visible) noexcept {
        WriteGuard guard(*this);
        m_subtitleVisible.store(visible, std::memory_order_relaxed);
    }

    void setAbRepeat(bool enabled) noexcept {
        WriteGuard guard(*this);
        m_abRepeat.store(enabled, std::memory_order_relaxed);
    }

    void setPointA(double seconds) noexcept {
        WriteGuard guard(*this);
        m_pointA.store(seconds < 0.0 ? 0.0 : seconds, std::memory_order_relaxed);
    }

    void setPointB(double seconds) noexcept {
        WriteGuard guard(*this);
        m_pointB.store(seconds < 0.0 ? 0.0 : seconds, std::memory_order_relaxed);
    }

    // 再生終了時のリピート動作 (オフ / 全曲ループ / 1曲ループ)
    void setRepeatMode(RepeatMode mode) noexcept {
        WriteGuard guard(*this);
        m_repeatMode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }

    // UI のサイクルボタン / キー 'L' 用: Off -> All -> One -> Off
    RepeatMode cycleRepeatMode() noexcept {
        const int next = (static_cast<int>(m_repeatMode.load(std::memory_order_relaxed)) + 1) % 3;
        setRepeatMode(static_cast<RepeatMode>(next));
        return static_cast<RepeatMode>(next);
    }

    // ------------------------------------------------------------- トラック切替
    void setVideoTrack(int index) noexcept {
        WriteGuard guard(*this);
        m_videoTrack.store(index, std::memory_order_relaxed);
    }

    void setAudioTrack(int index) noexcept {
        WriteGuard guard(*this);
        m_audioTrack.store(index, std::memory_order_relaxed);
    }

    void setSubtitleTrack(int index) noexcept {
        WriteGuard guard(*this);
        m_subtitleTrack.store(index, std::memory_order_relaxed);
    }

    // ------------------------------------------------------------- 画質 (v2)
    void setBrightness(float value) noexcept {
        WriteGuard guard(*this);
        m_brightness.store(clampf(value, -0.5f, 0.5f), std::memory_order_relaxed);
    }

    void setContrast(float value) noexcept {
        WriteGuard guard(*this);
        m_contrast.store(clampf(value, 0.5f, 1.5f), std::memory_order_relaxed);
    }

    void setSaturation(float value) noexcept {
        WriteGuard guard(*this);
        m_saturation.store(clampf(value, 0.0f, 2.0f), std::memory_order_relaxed);
    }

    void setHue(float degrees) noexcept {
        WriteGuard guard(*this);
        float v = degrees;
        while (v > 180.0f) v -= 360.0f;
        while (v < -180.0f) v += 360.0f;
        m_hueDeg.store(v, std::memory_order_relaxed);
    }

    void setSharpness(float value) noexcept {
        WriteGuard guard(*this);
        m_sharpness.store(clampf(value, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void setScaleFilter(ScaleFilter filter) noexcept {
        WriteGuard guard(*this);
        m_scaleFilter.store(static_cast<std::uint8_t>(filter), std::memory_order_relaxed);
    }

    void setRangeMode(RangeMode mode) noexcept {
        WriteGuard guard(*this);
        m_rangeMode.store(static_cast<std::uint8_t>(mode), std::memory_order_relaxed);
    }

    void setDecodeQuality(DecodeQuality quality) noexcept {
        WriteGuard guard(*this);
        m_decodeQuality.store(static_cast<std::uint8_t>(quality), std::memory_order_relaxed);
    }

    // フレーム補間 (Phase 2 Frame Generation)。有効時はレンダラが中間 RT を
    // 1 枚挟み、前フレームとのブレンドで表示レートを引き上げる。
    void setFrameGen(bool enabled) noexcept {
        WriteGuard guard(*this);
        m_frameGen.store(enabled, std::memory_order_relaxed);
    }

    // フレーム補間倍率 (v3): 2 / 4 / 8 / 16 のいずれかに丸める。
    void setFrameGenFactor(int factor) noexcept {
        int value = factor;
        if (value <= 2) value = 2;
        else if (value <= 4) value = 4;
        else if (value <= 8) value = 8;
        else value = 16;
        WriteGuard guard(*this);
        m_frameGenFactor.store(value, std::memory_order_relaxed);
    }

    // ---------------------------------------------------------------- シーク
    // 映像 / 音声の両デコーダが独立に同じ要求を消費する (リビジョン方式)。
    // UI や A-B リピートは requestSeek() を呼ぶだけで両トラックが揃って移動する。
    void requestSeek(double seconds) noexcept {
        m_seekTarget.store(seconds < 0.0 ? 0.0 : seconds, std::memory_order_relaxed);
        m_seekRevision.fetch_add(1, std::memory_order_release);
    }

    // seenRevision はデコーダごとの保持変数。新しい要求があれば true (1 回だけ)。
    bool takeSeek(std::uint64_t& seenRevision) const noexcept {
        const std::uint64_t rev = m_seekRevision.load(std::memory_order_acquire);
        if (rev == seenRevision) return false;
        seenRevision = rev;
        return true;
    }

    // 現在のシーク要求リビジョン。外部バックエンド (VLC) が立ち上がる直前に
    // 「これまでに出ていた要求は引き継がない」ための基準点として使う。
    std::uint64_t seekRevision() const noexcept {
        return m_seekRevision.load(std::memory_order_acquire);
    }

    double seekTargetSeconds() const noexcept {
        return m_seekTarget.load(std::memory_order_relaxed);
    }

    // ------------------------------------------------------------ 読み取り系
    // 描画 / 音声スレッドは毎フレーム本関数で全状態を一括取得する (ノンブロッキング)
    Snapshot snapshot() const noexcept {
        Snapshot s{};
        for (int attempt = 0; attempt < 128; ++attempt) {
            const std::uint64_t r1 = m_revision.load(std::memory_order_acquire);
            if ((r1 & 1u) != 0u) continue; // 書き込み中 → 再試行

            s.speed = m_speed.load(std::memory_order_relaxed);
            s.rotationDeg = m_rotationDeg.load(std::memory_order_relaxed);
            s.flip = static_cast<Flip>(m_flip.load(std::memory_order_relaxed));
            s.aspect = static_cast<Aspect>(m_aspect.load(std::memory_order_relaxed));
            for (int i = 0; i < 4; ++i) s.crop[i] = m_crop[i].load(std::memory_order_relaxed);
            s.subtitleVisible = m_subtitleVisible.load(std::memory_order_relaxed);
            s.abRepeat = m_abRepeat.load(std::memory_order_relaxed);
            s.pointA = m_pointA.load(std::memory_order_relaxed);
            s.pointB = m_pointB.load(std::memory_order_relaxed);
            s.repeatMode = static_cast<RepeatMode>(m_repeatMode.load(std::memory_order_relaxed));
            s.stereo = static_cast<StereoMode>(m_stereo.load(std::memory_order_relaxed));
            s.videoTrack = m_videoTrack.load(std::memory_order_relaxed);
            s.audioTrack = m_audioTrack.load(std::memory_order_relaxed);
            s.subtitleTrack = m_subtitleTrack.load(std::memory_order_relaxed);

            s.paused = m_paused.load(std::memory_order_relaxed);
            s.frameGen = m_frameGen.load(std::memory_order_relaxed);
            s.frameGenFactor = m_frameGenFactor.load(std::memory_order_relaxed);
            s.brightness = m_brightness.load(std::memory_order_relaxed);
            s.contrast = m_contrast.load(std::memory_order_relaxed);
            s.saturation = m_saturation.load(std::memory_order_relaxed);
            s.hueDeg = m_hueDeg.load(std::memory_order_relaxed);
            s.sharpness = m_sharpness.load(std::memory_order_relaxed);
            s.scaleFilter =
                static_cast<ScaleFilter>(m_scaleFilter.load(std::memory_order_relaxed));
            s.rangeMode = static_cast<RangeMode>(m_rangeMode.load(std::memory_order_relaxed));
            s.decodeQuality =
                static_cast<DecodeQuality>(m_decodeQuality.load(std::memory_order_relaxed));

            const std::uint64_t r2 = m_revision.load(std::memory_order_acquire);
            if (r1 == r2) {
                s.revision = r1;
                return s;
            }
        }
        s.revision = m_revision.load(std::memory_order_acquire); // 極小確率のフォールバック
        return s;
    }

    // 変更検出 (値比較の代わりにリビジョンで判定できる)
    std::uint64_t revision() const noexcept {
        return m_revision.load(std::memory_order_acquire);
    }

private:
    // seqlock 書き込みガード: 参照カウントと同様に偶奇を反転させる
    struct WriteGuard {
        explicit WriteGuard(PlayerController& self) noexcept
            : m_self(self) {
            m_self.m_revision.fetch_add(1, std::memory_order_acq_rel); // 奇数化
        }
        ~WriteGuard() noexcept {
            m_self.m_revision.fetch_add(1, std::memory_order_release); // 偶数化
        }
        WriteGuard(const WriteGuard&) = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;

    private:
        PlayerController& m_self;
    };

    static float clampf(float value, float low, float high) noexcept {
        return value < low ? low : (value > high ? high : value);
    }

    std::atomic<std::uint64_t> m_revision{0};
    std::atomic<std::uint64_t> m_seekRevision{0};
    std::atomic<double> m_seekTarget{0.0};
    std::atomic<float> m_speed{1.0f};
    std::atomic<std::int32_t> m_rotationDeg{0};
    std::atomic<std::uint8_t> m_flip{0};
    std::atomic<std::uint8_t> m_aspect{0};
    std::atomic<float> m_crop[4];
    std::atomic<bool> m_subtitleVisible{false};
    std::atomic<bool> m_abRepeat{false};
    std::atomic<double> m_pointA{0.0};
    std::atomic<double> m_pointB{0.0};
    std::atomic<std::uint8_t> m_repeatMode{0}; // PlayerController::RepeatMode
    std::atomic<std::uint8_t> m_stereo{0};
    std::atomic<std::int32_t> m_videoTrack{-1};
    std::atomic<std::int32_t> m_audioTrack{-1};
    std::atomic<std::int32_t> m_subtitleTrack{-1};

    // ---- v2 ----
    std::atomic<bool> m_paused{false};
    std::atomic<bool> m_frameGen{false};
    std::atomic<std::int32_t> m_frameGenFactor{4};
    std::atomic<float> m_brightness{0.0f};
    std::atomic<float> m_contrast{1.0f};
    std::atomic<float> m_saturation{1.0f};
    std::atomic<float> m_hueDeg{0.0f};
    std::atomic<float> m_sharpness{0.0f};
    std::atomic<std::uint8_t> m_scaleFilter{0};
    std::atomic<std::uint8_t> m_rangeMode{0};
    std::atomic<std::uint8_t> m_decodeQuality{1}; // Normal
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_PLAYER_CONTROLLER_HPP
