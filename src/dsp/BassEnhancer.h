#pragma once

#include <cmath>
#include <algorithm>

// Psychoacoustic bass enhancer (Stage_BassEnh).
//
// MaxxBass-style recipe: extract the sub band below the crossover,
// drive it through an asymmetric waveshaper to generate a harmonic
// series (2f, 3f, ...), band-limit the result, and mix it on top of
// the dry signal. The brain reconstructs the missing fundamental from
// the harmonics, so laptop speakers and earbuds "hear" bass they
// physically cannot reproduce - and on real subwoofers it reads as
// extra weight/growl rather than mud because the added content sits
// ABOVE the crossover.
//
// The shaper runs 2x oversampled when hq is set (same crude-but-
// effective midpoint scheme as the Saturator/Exciter shapers).
class BassEnhancer {
public:
    void setSampleRate(double sr) {
        m_fs = (sr > 0) ? sr : 48000.0;
        refresh();
        reset();
    }

    void setParams(float crossoverHz, float drive, float mix, bool hq) {
        m_freq  = std::min(300.0f, std::max(60.0f, crossoverHz));
        m_drive = std::min(10.0f, std::max(1.0f, drive));
        m_mix   = std::min(1.0f, std::max(0.0f, mix));
        m_hq    = hq;
        refresh();
    }

    void processStereo(float &l, float &r) {
        // Mono sub band (bass is mono below ~150 Hz anyway).
        float m = 0.5f * (l + r);

        // 2-pole lowpass isolates the sub band.
        m_lp1 += m_lpC * (m - m_lp1);
        m_lp2 += m_lpC * (m_lp1 - m_lp2);
        float sub = m_lp2;

        // Asymmetric shaper: even + odd harmonics. The 0.15 bias adds
        // 2nd-harmonic content (warmth), tanh adds the odd series.
        auto shape = [this](float x) {
            float d = x * m_drive;
            return std::tanh(d + 0.15f * d * d);
        };
        float harm;
        if (m_hq) {
            float mid = 0.5f * (m_prevSub + sub);
            harm = 0.5f * (shape(mid) + shape(sub));
        } else {
            harm = shape(sub);
        }
        m_prevSub = sub;

        // Kill the DC the asymmetric shaper introduces, then bandpass
        // the harmonics: HP above the crossover so the fundamental
        // doesn't double, LP at ~6x crossover to keep it from becoming
        // a distortion pedal.
        m_dcState += m_dcC * (harm - m_dcState);
        harm -= m_dcState;
        m_hpState += m_hpC * (harm - m_hpState);
        float band = harm - m_hpState;      // above crossover
        m_bandLp += m_bandLpC * (band - m_bandLp);
        band = m_bandLp;                    // below ~6x crossover

        float add = m_mix * 1.5f * band;
        l += add;
        r += add;
    }

    void reset() {
        m_lp1 = m_lp2 = 0.0f;
        m_prevSub = 0.0f;
        m_dcState = 0.0f;
        m_hpState = 0.0f;
        m_bandLp = 0.0f;
    }

private:
    void refresh() {
        constexpr double kTwoPi = 6.28318530717958647692;
        m_lpC     = static_cast<float>(1.0 - std::exp(-kTwoPi * m_freq / m_fs));
        m_hpC     = m_lpC;
        m_bandLpC = static_cast<float>(1.0 - std::exp(-kTwoPi * std::min(6.0 * m_freq, 2000.0) / m_fs));
        m_dcC     = static_cast<float>(1.0 - std::exp(-kTwoPi * 10.0 / m_fs));
    }

    double m_fs   = 48000.0;
    float m_freq  = 120.0f;
    float m_drive = 3.0f;
    float m_mix   = 0.4f;
    bool  m_hq    = true;

    float m_lpC = 0.0f, m_hpC = 0.0f, m_bandLpC = 0.0f, m_dcC = 0.0f;
    float m_lp1 = 0.0f, m_lp2 = 0.0f;
    float m_prevSub = 0.0f;
    float m_dcState = 0.0f;
    float m_hpState = 0.0f;
    float m_bandLp = 0.0f;
};
