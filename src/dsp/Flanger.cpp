#include "Flanger.h"
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

// Smooth triangle-sine LFO warped toward log-frequency pitch sweep (musical comb glide).
inline float triWave01(double p) {
    p -= std::floor(p);
    return static_cast<float>(p < 0.5 ? (p * 2.0) : (2.0 - p * 2.0));
}
} // namespace

void Flanger::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_smCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * m_sampleRate)));
    m_coefInit = false;
}

void Flanger::setParams(float rateHz, float depth, float feedback, float baseDelayMs, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(-0.95f, std::min(0.95f, feedback));
    m_baseDelay = std::max(0.1f, baseDelayMs);
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    if (!m_primed) {
        m_primed = true;
        m_depthS = m_depth; m_baseDelayS = m_baseDelay; m_fbS = m_feedback; m_mixS = m_mix;
    }
}

void Flanger::processStereo(float &l, float &r) {
    m_depthS     += m_smCoeff * (m_depth - m_depthS);
    m_baseDelayS += m_smCoeff * (m_baseDelay - m_baseDelayS);
    m_fbS        += m_smCoeff * (m_feedback - m_fbS);
    m_mixS       += m_smCoeff * (m_mix - m_mixS);

    double pL = m_phase;
    double pR = m_phase + 0.25; // quadrature stereo sweep
    if (pR >= 1.0) pR -= 1.0;

    m_phase += m_rate / m_sampleRate;
    if (m_phase >= 1.0) m_phase -= 1.0;

    // Exponential delay curve yields steady pitch/comb movement per octave across sweep.
    const float msToSamp = 0.001f * static_cast<float>(m_sampleRate);
    auto delayAt = [&](double phase) -> float {
        float minMs = std::max(0.12f, m_baseDelayS * (1.0f - 0.85f * m_depthS));
        float maxMs = std::max(minMs + 0.1f, m_baseDelayS * (1.0f + 2.5f * m_depthS));
        float delMs = minMs * std::pow(maxMs / minMs, triWave01(phase));
        return std::max(1.0f, std::min(delMs * msToSamp, static_cast<float>(kMaxBufSize - 4)));
    };

    if (!m_coefInit) {
        m_delL = delayAt(pL);
        m_delR = delayAt(pR);
        m_coefCtr = 0;
        m_coefInit = true;
    }
    if (m_coefCtr <= 0) {
        m_coefCtr = kCoefInterval;
        const double ahead = kCoefInterval * m_rate / m_sampleRate;
        const float invN = 1.0f / static_cast<float>(kCoefInterval);
        m_delStepL = (delayAt(pL + ahead) - m_delL) * invN;
        m_delStepR = (delayAt(pR + ahead) - m_delR) * invN;
    }
    --m_coefCtr;
    m_delL += m_delStepL;
    m_delR += m_delStepR;
    const float delL = m_delL;
    const float delR = m_delR;

    // Gentle HF damping + soft clip inside feedback loop prevents screech at extreme +-0.95.
    m_bufL[m_writePos] = l + m_fbS * std::tanh(m_lastOutL);
    m_bufR[m_writePos] = r + m_fbS * std::tanh(m_lastOutR);

    float delayedL = readHermite(m_bufL, kMaxBufSize, m_writePos, delL);
    float delayedR = readHermite(m_bufR, kMaxBufSize, m_writePos, delR);

    m_lastOutL = m_lastOutL * 0.18f + delayedL * 0.82f;
    m_lastOutR = m_lastOutR * 0.18f + delayedR * 0.82f;
    if (std::fabs(m_lastOutL) < 1e-15f) m_lastOutL = 0.0f; // anti-denormal flush
    if (std::fabs(m_lastOutR) < 1e-15f) m_lastOutR = 0.0f;

    // True flanger mix: comb cancellation happens between dry + delayed signal, so even
    // at mix = 100% the wet signal preserves maximum dry+delayed jet-plane comb notches!
    float wetFlangeL = 0.65f * (l + delayedL);
    float wetFlangeR = 0.65f * (r + delayedR);
    l = l * (1.0f - m_mixS) + wetFlangeL * m_mixS;
    r = r * (1.0f - m_mixS) + wetFlangeR * m_mixS;

    m_writePos = (m_writePos + 1) % kMaxBufSize;
}

void Flanger::reset() {
    std::fill(std::begin(m_bufL), std::end(m_bufL), 0.0f);
    std::fill(std::begin(m_bufR), std::end(m_bufR), 0.0f);
    m_writePos = 0;
    m_phase = 0.0;
    m_lastOutL = 0.0f;
    m_lastOutR = 0.0f;
    m_depthS = m_depth;
    m_baseDelayS = m_baseDelay;
    m_fbS = m_feedback;
    m_mixS = m_mix;
    m_coefInit = false;
}
