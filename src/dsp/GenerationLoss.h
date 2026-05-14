#pragma once

#include <cmath>
#include <cstring>
#include <random>

class GenerationLoss {
public:
    void setSampleRate(double sr) {
        m_fs = static_cast<float>(sr);
        recompute();
    }

    void reset() {
        std::memset(m_lpL, 0, sizeof(m_lpL));
        std::memset(m_lpR, 0, sizeof(m_lpR));
        m_holdL = 0.f; m_holdR = 0.f;
        m_holdCount = 0;
        m_blockPhase = 0;
        m_dcPrevInL = 0.f; m_dcPrevInR = 0.f;
        m_dcPrevOutL = 0.f; m_dcPrevOutR = 0.f;
        m_rng.seed(42);
    }

    void setGenerations(int g) {
        if (g == m_gens) return;
        m_gens = g < 1 ? 1 : g;
        recompute();
    }

    int generations() const { return m_gens; }

    void processStereo(float &l, float &r) {
        // 1. Cascaded LPF — 4 one-pole stages = steep bandwidth reduction.
        //    Cutoff drops exponentially with generation count so 1000 gens
        //    leaves only sub-bass.
        for (int i = 0; i < kStages; ++i) {
            m_lpL[i] += m_lpAlpha * (l - m_lpL[i]);
            l = m_lpL[i];
            m_lpR[i] += m_lpAlpha * (r - m_lpR[i]);
            r = m_lpR[i];
        }

        // 2. Sample-and-hold decimation — creates aliasing artifacts like
        //    real codec sample-rate conversion would.
        if (m_decimFactor > 1) {
            if (++m_holdCount >= m_decimFactor) {
                m_holdCount = 0;
                m_holdL = l;
                m_holdR = r;
            }
            l = m_holdL;
            r = m_holdR;
        }

        // 3. Quantization — reduces amplitude resolution, simulating
        //    codec's frequency-domain quantization mapped back to time.
        if (m_quantLevels < 32768.f) {
            l = std::round(l * m_quantLevels) / m_quantLevels;
            r = std::round(r * m_quantLevels) / m_quantLevels;
        }

        // 4. Block-rate gain modulation — simulates the pumping you hear
        //    at MDCT frame boundaries in real lossy codecs.
        if (m_pumpDepth > 0.0001f) {
            if (++m_blockPhase >= kBlockSize) m_blockPhase = 0;
            float t = static_cast<float>(m_blockPhase) / static_cast<float>(kBlockSize);
            float pump = 1.0f - m_pumpDepth * (0.5f - 0.5f * std::cos(t * 6.2831853f));
            l *= pump;
            r *= pump;
        }

        // 5. Soft saturation — codec normalization/brick-wall limiting
        //    compresses dynamics more with each generation.
        if (m_drive > 1.001f) {
            l = std::tanh(l * m_drive) * m_driveNorm;
            r = std::tanh(r * m_drive) * m_driveNorm;
        }

        // 6. Noise injection — accumulated hiss from dithering + rounding.
        if (m_noiseAmt > 0.00001f) {
            std::uniform_real_distribution<float> dist(-m_noiseAmt, m_noiseAmt);
            l += dist(m_rng);
            r += dist(m_rng);
        }

        // 7. DC blocker — prevents slow drift from quantization asymmetry.
        //    y[n] = x[n] - x[n-1] + R * y[n-1]
        float tmpL = l, tmpR = r;
        l = tmpL - m_dcPrevInL + 0.9975f * m_dcPrevOutL;
        r = tmpR - m_dcPrevInR + 0.9975f * m_dcPrevOutR;
        m_dcPrevInL = tmpL;  m_dcPrevOutL = l;
        m_dcPrevInR = tmpR;  m_dcPrevOutR = r;
    }

private:
    void recompute() {
        float g = static_cast<float>(m_gens);

        // Bandwidth: exponential decay. At gen=1 ~18kHz, gen=100 ~10.8kHz,
        // gen=500 ~1.5kHz, gen=1000 ~120Hz (clamped to 60Hz).
        float cutoff = 18000.f * std::pow(0.995f, g);
        if (cutoff < 60.f) cutoff = 60.f;
        float w = 2.f * 3.14159265f * cutoff / m_fs;
        m_lpAlpha = w / (1.f + w);

        // Decimation: integer factor, increases with generations.
        // gen=1: 1, gen=80: 2, gen=400: 6, gen=1000: 13
        m_decimFactor = 1 + static_cast<int>(g / 80.f);
        if (m_decimFactor > 48) m_decimFactor = 48;

        // Quantization: bit depth drops exponentially.
        // gen=1: ~16bit, gen=200: ~8.4bit, gen=500: ~3.2bit, gen=1000: ~1.5bit
        float bits = 16.f * std::pow(0.997f, g);
        if (bits < 1.5f) bits = 1.5f;
        m_quantLevels = std::pow(2.f, bits);

        // Block pumping depth: subtle at low gens, strong at high.
        m_pumpDepth = g * 0.0004f;
        if (m_pumpDepth > 0.4f) m_pumpDepth = 0.4f;

        // Saturation drive: gentle compression accumulating per generation.
        m_drive = 1.f + g * 0.004f;
        if (m_drive > 6.f) m_drive = 6.f;
        m_driveNorm = 1.f / std::tanh(m_drive);

        // Noise: rises with generations.
        m_noiseAmt = g * 0.00015f;
        if (m_noiseAmt > 0.15f) m_noiseAmt = 0.15f;
    }

    static const int kStages = 4;
    static const int kBlockSize = 1024;

    float m_fs = 48000.f;
    int   m_gens = 1;

    float m_lpAlpha = 1.f;
    float m_lpL[4] = {};
    float m_lpR[4] = {};

    int   m_decimFactor = 1;
    int   m_holdCount = 0;
    float m_holdL = 0.f, m_holdR = 0.f;

    float m_quantLevels = 65536.f;

    float m_pumpDepth = 0.f;
    int   m_blockPhase = 0;

    float m_drive = 1.f;
    float m_driveNorm = 1.f;

    float m_noiseAmt = 0.f;
    std::mt19937 m_rng{42};

    float m_dcPrevInL = 0.f, m_dcPrevInR = 0.f;
    float m_dcPrevOutL = 0.f, m_dcPrevOutR = 0.f;
};
