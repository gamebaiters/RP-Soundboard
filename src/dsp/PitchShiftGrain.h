#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// Granular (dual-tap crossfade delay line) pitch shifter.
//
// The classic "Whammy" recipe: a ring buffer written at 1x, read by two
// taps whose delay ramps at (1 - ratio) samples per sample so the read
// heads move through the material at `ratio` speed. Each tap is gained
// by a triangle window in tap phase, so a tap always crosses its wrap
// discontinuity at zero gain; the two windows are half a period apart
// and sum to unity.
//
// Zero external dependencies, O(1) per sample, constant latency of
// window/2 on average. Quality is very good for voice at 30-60 ms
// windows; extreme ratios (> 2 octaves) start to warble - which for
// the soundboard use cases (autotune snap, shimmer +12, live mic
// pitch) is exactly the famous sound people expect.
//
// Used by: VoiceFx (autotune, shimmer feedback), MicFx (live 1:1
// pitch). One instance per channel of audio.
class PitchShiftGrain {
public:
    void setSampleRate(double sr) {
        m_fs = (sr > 0) ? sr : 48000.0;
        rebuild();
    }

    // Grain window length. Longer = smoother/behind, shorter = tighter
    // but more warble. 50 ms default suits voice.
    void setWindowMs(float ms) {
        float w = std::min(120.0f, std::max(10.0f, ms));
        if (std::fabs(w - m_windowMs) < 0.01f) return;
        m_windowMs = w;
        rebuild();
    }

    // Pitch ratio (2.0 = +1 octave, 0.5 = -1 octave). Smoothed a little
    // internally so live slider drags / autotune retunes don't click.
    void setRatio(float r) {
        m_targetRatio = std::min(4.0f, std::max(0.25f, r));
    }
    float ratio() const { return m_ratio; }

    float process(float in) {
        if (m_buf.empty()) rebuild();
        const int N = static_cast<int>(m_buf.size());

        // Ratio smoothing (~2 ms) keeps autotune retune glides clean.
        m_ratio += (m_targetRatio - m_ratio) * m_ratioCoeff;

        m_buf[m_w] = in;

        // Advance tap phase. p in [0,1): tap delay = p * window.
        m_p += (1.0f - m_ratio) / m_win;
        m_p -= std::floor(m_p);

        float p2 = m_p + 0.5f;
        p2 -= std::floor(p2);

        float d1 = m_p * m_win;
        float d2 = p2  * m_win;

        // Triangle gains: zero exactly when the tap wraps.
        float g1 = 2.0f * std::min(m_p, 1.0f - m_p);
        float g2 = 2.0f * std::min(p2,  1.0f - p2);

        float out = g1 * readFrac(d1, N) + g2 * readFrac(d2, N);

        m_w = (m_w + 1) % N;
        return out;
    }

    void reset() {
        std::fill(m_buf.begin(), m_buf.end(), 0.0f);
        m_w = 0;
        m_p = 0.0f;
        m_ratio = m_targetRatio;
    }

private:
    void rebuild() {
        m_win = static_cast<float>(m_windowMs * 0.001 * m_fs);
        if (m_win < 64.0f) m_win = 64.0f;
        int n = static_cast<int>(m_win) + 8;
        m_buf.assign(static_cast<size_t>(n), 0.0f);
        m_w = 0;
        m_p = 0.0f;
        // ~2 ms one-pole for the ratio glide.
        m_ratioCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.002 * m_fs)));
    }

    float readFrac(float delay, int N) const {
        float pos = static_cast<float>(m_w) - delay;
        while (pos < 0.0f) pos += N;
        int i0 = static_cast<int>(pos);
        int i1 = i0 + 1; if (i1 >= N) i1 = 0;
        float f = pos - static_cast<float>(i0);
        return m_buf[i0] * (1.0f - f) + m_buf[i1] * f;
    }

    std::vector<float> m_buf;
    double m_fs        = 48000.0;
    float  m_windowMs  = 50.0f;
    float  m_win       = 2400.0f;
    int    m_w         = 0;
    float  m_p         = 0.0f;
    float  m_ratio     = 1.0f;
    float  m_targetRatio = 1.0f;
    float  m_ratioCoeff  = 0.01f;
};
