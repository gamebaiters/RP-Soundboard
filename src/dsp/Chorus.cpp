#include "Chorus.h"
#include <iterator>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
inline float readHermite(const float *buf, int bufSize, int writePos, float delaySamples) {
    int i0 = static_cast<int>(delaySamples);
    float f = delaySamples - static_cast<float>(i0);
    auto at = [&](int lag) -> float {
        int idx = writePos - lag;
        idx %= bufSize;
        if (idx < 0) idx += bufSize;
        return buf[idx];
    };
    float ym1 = at(i0 - 1);
    float y0  = at(i0);
    float y1  = at(i0 + 1);
    float y2  = at(i0 + 2);
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * f + c2) * f + c1) * f + y0;
}
} // namespace

void Chorus::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_bufSize = static_cast<int>(m_sampleRate);
    if (m_bufSize > kMaxBufSize) m_bufSize = kMaxBufSize;
    if (m_bufSize < 8) m_bufSize = 8;
    m_lpC = static_cast<float>(1.0 - std::exp(-2.0 * M_PI * 8500.0 / m_sampleRate));
    m_hpC = static_cast<float>(1.0 - std::exp(-2.0 * M_PI * 110.0 / m_sampleRate));
    m_smCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * m_sampleRate)));
}

void Chorus::setParams(float rateHz, float depthMs, float baseDelayMs, int voices, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, depthMs);
    m_baseDelay = std::max(1.0f, baseDelayMs);
    m_voices = std::max(1, std::min(voices, static_cast<int>(kMaxVoices)));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    if (!m_primed) { m_primed = true; m_depthS = m_depth; m_baseDelayS = m_baseDelay; m_mixS = m_mix; }
}

void Chorus::processStereo(float &l, float &r) {
    if (m_bufSize <= 4) return;

    m_depthS     += m_smCoeff * (m_depth - m_depthS);
    m_baseDelayS += m_smCoeff * (m_baseDelay - m_baseDelayS);
    m_mixS       += m_smCoeff * (m_mix - m_mixS);

    m_bufL[m_writePos] = l + 0.14f * m_wetLpR;
    m_bufR[m_writePos] = r - 0.14f * m_wetLpL;

    float wetL = 0.0f, wetR = 0.0f;
    const float msToSamp = 0.001f * static_cast<float>(m_sampleRate);
    const float maxDel = static_cast<float>(m_bufSize - 4);

    for (int v = 0; v < m_voices; ++v) {
        // Slightly spread voice LFO rates for lush ensemble motion (zero beating locks).
        double voiceRateMul = 1.0 + 0.09 * (static_cast<double>(v) - 0.5 * (m_voices - 1));
        double pL = m_phase[v];
        // Quadrature + voice spread between L and R creates true stereo width even on mono voice.
        double pR = pL + 0.25 + 0.125 * static_cast<double>(v);
        if (pR >= 1.0) pR -= 1.0;

        float lfoL = static_cast<float>(std::sin(2.0 * M_PI * pL));
        float lfoR = static_cast<float>(std::sin(2.0 * M_PI * pR));

        m_phase[v] += (m_rate * voiceRateMul) / m_sampleRate;
        if (m_phase[v] >= 1.0) m_phase[v] -= 1.0;

        // Voice stagger in base delay prevents identical comb overlaps across voices.
        float voiceBaseMs = m_baseDelayS + static_cast<float>(v) * 2.3f;
        float delL = std::max(1.0f, std::min(maxDel, (voiceBaseMs + m_depthS * lfoL) * msToSamp));
        float delR = std::max(1.0f, std::min(maxDel, (voiceBaseMs + m_depthS * lfoR) * msToSamp));

        wetL += readHermite(m_bufL, m_bufSize, m_writePos, delL);
        wetR += readHermite(m_bufR, m_bufSize, m_writePos, delR);
    }

    // Equal-power normalization keeps multi-voice thickness strong without dropping -6 dB.
    const float norm = 1.0f / std::sqrt(static_cast<float>(m_voices));
    wetL *= norm;
    wetR *= norm;

    // Classic BBD warmth: gentle 8.5 kHz LPF + 110 Hz HPF on wet path prevents sub mud.
    m_wetLpL += m_lpC * (wetL - m_wetLpL);
    m_wetLpR += m_lpC * (wetR - m_wetLpR);
    m_wetHpL += m_hpC * (m_wetLpL - m_wetHpL);
    m_wetHpR += m_hpC * (m_wetLpR - m_wetHpR);
    // Anti-denormal flush of the recursive wet filters (they also feed the cross-feedback above).
    if (std::fabs(m_wetLpL) < 1e-15f) m_wetLpL = 0.0f;
    if (std::fabs(m_wetLpR) < 1e-15f) m_wetLpR = 0.0f;
    if (std::fabs(m_wetHpL) < 1e-15f) m_wetHpL = 0.0f;
    if (std::fabs(m_wetHpR) < 1e-15f) m_wetHpR = 0.0f;
    float cleanWetL = m_wetLpL - m_wetHpL;
    float cleanWetR = m_wetLpR - m_wetHpR;

    // Cross-stereo wet blend keeps center anchor clear at mix=0.5 and full detune at mix=1.0.
    float dryGain = 1.0f - 0.55f * m_mixS;
    float wetGain = m_mixS * 0.95f;
    l = l * dryGain + cleanWetL * wetGain;
    r = r * dryGain + cleanWetR * wetGain;

    m_writePos = (m_writePos + 1) % m_bufSize;
}

void Chorus::reset() {
    std::fill(std::begin(m_bufL), std::end(m_bufL), 0.0f);
    std::fill(std::begin(m_bufR), std::end(m_bufR), 0.0f);
    m_writePos = 0;
    m_wetLpL = m_wetLpR = 0.0f;
    m_wetHpL = m_wetHpR = 0.0f;
    m_depthS = m_depth;
    m_baseDelayS = m_baseDelay;
    m_mixS = m_mix;
    for (int i = 0; i < kMaxVoices; i++) {
        m_phase[i] = static_cast<double>(i) / kMaxVoices;
    }
}
