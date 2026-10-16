#pragma once
#ifndef NANOALPHA_CORE_SPEED_STRETCHER_HPP
#define NANOALPHA_CORE_SPEED_STRETCHER_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace nanoalpha {

// =============================================================================
//  SpeedStretcher — ピッチ維持タイムストレッチ (WSOLA)
// -----------------------------------------------------------------------------
//  [従来]
//   線形補間でサンプル位置を (speed x) 進めただけだったため、2.0x なら音程が
//   1 オクターブ上がってしまう (0.5x なら下がる)。
//
//  [WSOLA (Waveform Similarity Overlap-Add)]
//   ・解析側: 入力を「窓長 kWindow」で切り出し、位置を (speed x kHop) ずつ進める
//   ・合成側: 一定間隔 kHop で窓を掛けてオーバーラップ加算する
//   ・解析位置は前回フレームと位相が合う位置を相関探索で選ぶ
//     → 波形の周期性が保たれたまま再生時間が伸縮するため、音程は変わらない
//
//  [窓とホップ]
//   窓は hann/2、ホップは窓の 1/4。4 枚の重ね和は全位置でちょうど 1 になる
//   (COLA 条件) ため、speed == 1.0 では解析位置は常に名目位置に一致し、
//   出力は入力の完全なパススルーになる。
//
//  [入出力]
//   push() は FLTP (planar) をインターリーブにして fifo へ追加し、
//   produce() が窓長ぶんの入力が貯まってから合成ブロックを吐き出す。
//   firstOutPts は出力先頭サンプルの媒体時刻 (秒) で、
//   「入力位置 m_pos の媒体時刻 m_pts」をそのまま返す。
//
//  スレッド: デコードワーカーのみが触る (他のスレッドから触らない)
// =============================================================================
class SpeedStretcher {
public:
    // 解析/合成窓 (フレーム = 32ms @48k)。ホップはその 1/4 (8ms)。
    static constexpr int kWindow = 1536;
    static constexpr int kHop = kWindow / 4;
    // 解析位置の探索幅 (+-384 = +-8ms → 125Hz 以上の周期を 1 周期分カバー)
    static constexpr int kSearch = kHop;
    // 相関を取る長さ (5.3ms)。重なり領域 (kWindow - kHop) より十分短ければ良い。
    static constexpr int kCorr = 256;

    SpeedStretcher() { reset(); }

    void reset() {
        m_fifo.clear();
        m_pos = 0;
        m_pts = 0.0;
        m_haveRef = false;
        m_ola.assign(static_cast<std::size_t>(kWindow) * 2, 0.0f);
        m_ref.assign(static_cast<std::size_t>(kCorr) * 2, 0.0f);
    }

    // FLTP -> インターリーブへ展開して fifo へ追加 (pts は先頭サンプルの媒体時刻)
    void push(const float* left, const float* right, int samples, double pts) {
        if (samples <= 0 || left == nullptr || right == nullptr) return;

        const std::size_t oldFrames = m_fifo.size() / 2;

        // fifo はインターリーブなので要素数 (float) はフレーム数の 2 倍。
        const std::size_t newFrames = oldFrames + static_cast<std::size_t>(samples);
        m_fifo.resize(newFrames * 2);
        float* dst = m_fifo.data() + oldFrames * 2;
        for (int i = 0; i < samples; ++i) {
            dst[i * 2] = left[i];
            dst[i * 2 + 1] = right[i];
        }

        // fifo が空 (= reset / seek 直後) の最初の 1 回だけ、時刻と合成状態を張り直す。
        // それ以外では m_pos / m_pts は produce() が正確に追従しているため触らない。
        // ※「m_pos が fifo の末尾に達したか」で判定してはいけない。stepIn が
        //   kWindow 以上 (speed >= 4) だと名目位置が毎回 fifo 末尾に届いてしまい、
        //   位相探索の参照と OLA の重なり尻が毎回破棄されて WSOLA が無効になる。
        if (oldFrames == 0) {
            m_pos = 0;
            m_pts = pts;
            m_haveRef = false;
            m_ola.assign(static_cast<std::size_t>(kWindow) * 2, 0.0f);
        }
        compact();
    }

    // 出力 (インターリーブ) を生成。firstOutPts は先頭サンプルの媒体時刻。
    std::size_t produce(std::vector<float>& out, double& firstOutPts, float speed,
                        int sampleRate) {
        out.clear();
        if (sampleRate <= 0) return 0;

        const std::size_t frames = m_fifo.size() / 2;
        if (frames < static_cast<std::size_t>(kWindow)) return 0;
        const std::size_t maxPos = frames - static_cast<std::size_t>(kWindow);

        const double s = (speed > 0.0f) ? static_cast<double>(speed) : 1.0;
        const long long stepIn =
            std::max<long long>(1, std::llround(s * static_cast<double>(kHop)));
        const double invRate = 1.0 / static_cast<double>(sampleRate);
        const float* win = window().data();

        bool first = false;
        while (m_pos + static_cast<std::size_t>(kWindow) <= frames) {
            const std::size_t a = choose(m_pos, maxPos);
            m_lastOffset =
                static_cast<int>(static_cast<long long>(a) - static_cast<long long>(m_pos));
            m_lastA = a;

            // --- 窓掛けてオーバーラップ加算 ---
            const float* src = m_fifo.data() + a * 2;
            float* ola = m_ola.data();
            for (int i = 0; i < kWindow; ++i) {
                const float w = win[static_cast<std::size_t>(i)];
                ola[i * 2] += src[i * 2] * w;
                ola[i * 2 + 1] += src[i * 2 + 1] * w;
            }

            // 次のステップで使う参照ベクトル (前回フレームの重なり側) を退避する。
            // 後続の push() が行う刈り取りに影響されないよう、fifo ではなく
            // 専用バッファへコピーしておく。
            const float* ref = m_fifo.data() + (a + kHop) * 2;
            std::copy(ref, ref + static_cast<std::size_t>(kCorr) * 2, m_ref.begin());
            m_haveRef = true;

            // --- 先頭 kHop フレームを出力へ取り出す ---
            if (!first) {
                firstOutPts = m_pts;
                first = true;
            }
            out.insert(out.end(), ola, ola + static_cast<std::size_t>(kHop) * 2);

            // 合成バッファを kHop 分だけ前へずらし、末尾を 0 で埋める
            std::move(ola + static_cast<std::size_t>(kHop) * 2,
                      ola + static_cast<std::size_t>(kWindow) * 2, ola);
            std::fill(ola + static_cast<std::size_t>(kWindow - kHop) * 2,
                      ola + static_cast<std::size_t>(kWindow) * 2, 0.0f);

            // --- 解析位置と媒体時刻を進める ---
            m_pos += static_cast<std::size_t>(stepIn);
            m_pts += static_cast<double>(stepIn) * invRate;
        }
        return out.size() / 2;
    }

    // 現在の解析位置の媒体時刻 (秒)。デバッグ / テスト用
    double positionSeconds() const noexcept { return m_pts; }

    // 直前の解析位置が名目位置からどれだけずれたか (フレーム)。診断用
    int lastOffset() const noexcept { return m_lastOffset; }

    // 直前に出力したブロックに使った解析位置 (フレーム)。診断用
    std::size_t lastAnalysisIndex() const noexcept { return m_lastA; }

private:
    // 参照ベクトルとの正規化相互関が最大になる解析位置を選ぶ。
    // |d| の昇順に走査するため、同点 (周期信号でよく起きる) なら名目位置 a0 を優先する。
    std::size_t choose(std::size_t a0, std::size_t maxPos) const {
        if (!m_haveRef) return (std::min)(a0, maxPos);

        const float* r = m_ref.data();
        double refEnergy = 0.0;
        for (int i = 0; i < kCorr * 2; ++i) {
            const double v = r[i];
            refEnergy += v * v;
        }
        const double invRef = (refEnergy > 1.0e-20) ? 1.0 / std::sqrt(refEnergy) : 0.0;
        if (invRef == 0.0) return a0;

        double best = correlation(a0, invRef, maxPos);
        std::size_t bestA = a0;
        for (int d = 1; d <= kSearch; ++d) {
            const std::size_t du = static_cast<std::size_t>(d);
            if (a0 + du <= maxPos) {
                const double sc = correlation(a0 + du, invRef, maxPos);
                if (sc > best) {
                    best = sc;
                    bestA = a0 + du;
                }
            }
            if (a0 >= du) {
                const double sc = correlation(a0 - du, invRef, maxPos);
                if (sc > best) {
                    best = sc;
                    bestA = a0 - du;
                }
            }
        }
        return bestA;
    }

    double correlation(std::size_t a, double invRef, std::size_t maxPos) const {
        if (a > maxPos) return -2.0;
        const float* c = m_fifo.data() + a * 2;
        const float* r = m_ref.data();
        double dot = 0.0;
        double energy = 0.0;
        for (int i = 0; i < kCorr * 2; ++i) {
            const double y = c[i];
            dot += r[i] * y;
            energy += y * y;
        }
        if (energy < 1.0e-20) return 0.0; // 無音候補は相関なし扱い
        return dot * invRef / std::sqrt(energy);
    }

    // 使い切り分の先頭を刈り取る。解析候補の最小位置は m_pos - kSearch なので、
    // それより前はもう一切参照されない。
    void compact() {
        const std::size_t margin = static_cast<std::size_t>(kSearch + 4096);
        if (m_pos <= margin) return;
        const std::size_t cut = m_pos - static_cast<std::size_t>(kSearch);
        const std::size_t frames = m_fifo.size() / 2;
        // speed > 4 で m_pos が一時的に fifo の末尾を越えることがある。
        // その間は刈り取らず、push() でデータが追いついてから刈る。
        if (cut >= frames) return;
        m_fifo.erase(m_fifo.begin(), m_fifo.begin() + static_cast<std::ptrdiff_t>(cut) * 2);
        m_pos -= cut;
    }

    // hann/2。ホップ = 窓/4 のとき 4 枚の重ね和が全位置で 1 になる (COLA)。
    static const std::vector<float>& window() {
        static const std::vector<float> w = [] {
            std::vector<float> v(static_cast<std::size_t>(kWindow));
            constexpr double kPi = 3.14159265358979323846;
            for (int i = 0; i < kWindow; ++i) {
                const double hann = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kWindow);
                v[static_cast<std::size_t>(i)] = static_cast<float>(hann * 0.5);
            }
            return v;
        }();
        return w;
    }

    std::vector<float> m_fifo;                 // インターリーブ入力
    std::vector<float> m_ola;                  // 合成オーバーラップ加算バッファ
    std::vector<float> m_ref;                  // 次ステップの相関参照 (刈り取り非依存)
    std::size_t m_pos = 0;                     // 名目解析位置 (フレーム)
    double m_pts = 0.0;                        // m_pos の媒体時刻 (秒)
    bool m_haveRef = false;
    int m_lastOffset = 0;
    std::size_t m_lastA = 0;
};

} // namespace nanoalpha

#endif // NANOALPHA_CORE_SPEED_STRETCHER_HPP
