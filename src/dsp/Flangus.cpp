#include "Flangus.h"
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

// Transparent below 0.9, smoothly saturating towards 1.0 above: resonant feedback combs of
// correlated voices can otherwise sum to ~1.6 even though the loop itself is stable.
inline float softLimit(float x) {
    constexpr float knee = 0.9f;
    float a = std::fabs(x);
    if (a <= knee) return x;
    float y = knee + (1.0f - knee) * std::tanh((a - knee) / (1.0f - knee));
    return x < 0.0f ? -y : y;
}
} // namespace

void Flangus::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_bufSize = static_cast<int>(m_sampleRate);
    if (m_bufSize > kMaxBufSize) m_bufSize = kMaxBufSize;
    if (m_bufSize < 8) m_bufSize = 8;
    m_smCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * m_sampleRate)));
}

void Flangus::setParams(float rateHz, float depth, float feedback, int voices,
                         float spread, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(-0.95f, std::min(0.95f, feedback));
    m_voices = std::max(1, std::min(voices, static_cast<int>(kMaxVoices)));
    m_spread = std::max(0.0f, std::min(1.0f, spread));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    if (!m_primed) {
        m_primed = true;
        m_depthS = m_depth; m_spreadS = m_spread; m_fbS = m_feedback; m_mixS = m_mix;
    }
}

void Flangus::processStereo(float &l, float &r) {
    if (m_bufSize <= 4) return;

    m_depthS  += m_smCoeff * (m_depth - m_depthS);
    m_spreadS += m_smCoeff * (m_spread - m_spreadS);
    m_fbS     += m_smCoeff * (m_feedback - m_fbS);
    m_mixS    += m_smCoeff * (m_mix - m_mixS);

    m_bufL[m_writePos] = l + m_fbS * std::tanh(m_lastWetL);
    m_bufR[m_writePos] = r + m_fbS * std::tanh(m_lastWetR);

    float wetL = 0.0f, wetR = 0.0f;
    const float msToSamp = 0.001f * static_cast<float>(m_sampleRate);
    const float maxDel = static_cast<float>(m_bufSize - 4);
    const float centerMs = 0.5f * (m_minDelayMs + m_maxDelayMs);
    const float spanMs   = 0.5f * (m_maxDelayMs - m_minDelayMs);

    for (int v = 0; v < m_voices; ++v) {
        // Live spread drives BOTH stereo phase divergence (L vs R) AND subtle voice rate detune.
        double detune = 1.0 + 0.16 * m_spreadS * (static_cast<double>(v) - 0.5 * (m_voices - 1));
        double pL = m_phase[v];
        double rOffset = 0.5 * m_spreadS * (static_cast<double>(v + 1) / m_voices);
        double pR = pL + rOffset;
        if (pR >= 1.0) pR -= std::floor(pR);

        float lfoL = static_cast<float>(std::sin(2.0 * M_PI * pL));
        float lfoR = static_cast<float>(std::sin(2.0 * M_PI * pR));

        m_phase[v] += (m_rate * detune) / m_sampleRate;
        if (m_phase[v] >= 1.0) m_phase[v] -= std::floor(m_phase[v]);

        float delMsL = centerMs + spanMs * m_depthS * lfoL;
        float delMsR = centerMs + spanMs * m_depthS * lfoR;
        float delL = std::max(1.0f, std::min(delMsL * msToSamp, maxDel));
        float delR = std::max(1.0f, std::min(delMsR * msToSamp, maxDel));

        wetL += readHermite(m_bufL, m_bufSize, m_writePos, delL);
        wetR += readHermite(m_bufR, m_bufSize, m_writePos, delR);
    }

    // Feedback path uses the MEAN of the voices (1/N): loop gain stays <= |feedback| < 1 for any
    // voice count. (The audible comb output keeps the 1/sqrt(N) equal-power normalization.)
    const float fbNorm = 1.0f / static_cast<float>(m_voices);
    m_lastWetL = m_lastWetL * 0.15f + (wetL * fbNorm) * 0.85f;
    m_lastWetR = m_lastWetR * 0.15f + (wetR * fbNorm) * 0.85f;
    if (std::fabs(m_lastWetL) < 1e-15f) m_lastWetL = 0.0f; // anti-denormal flush
    if (std::fabs(m_lastWetR) < 1e-15f) m_lastWetR = 0.0f;

    const float norm = 1.0f / std::sqrt(static_cast<float>(m_voices));
    wetL *= norm;
    wetR *= norm;

    // Include dry+delayed sum in wet so flanger-style comb sweeping remains deep up to mix = 1.0.
    float combWetL = softLimit(0.65f * (l + wetL));
    float combWetR = softLimit(0.65f * (r + wetR));
    l = l * (1.0f - m_mixS) + combWetL * m_mixS;
    r = r * (1.0f - m_mixS) + combWetR * m_mixS;

    m_writePos = (m_writePos + 1) % m_bufSize;
}

void Flangus::reset() {
    std::fill(std::begin(m_bufL), std::end(m_bufL), 0.0f);
    std::fill(std::begin(m_bufR), std::end(m_bufR), 0.0f);
    m_writePos = 0;
    m_lastWetL = 0.0f;
    m_lastWetR = 0.0f;
    m_depthS = m_depth;
    m_spreadS = m_spread;
    m_fbS = m_feedback;
    m_mixS = m_mix;
    for (int i = 0; i < kMaxVoices; i++) {
        m_phase[i] = static_cast<double>(i) / kMaxVoices;
    }
}
