#include "Delay.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Delay::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    int needed = static_cast<int>(m_sampleRate * 3.0);
    if (needed > kMaxDelaySamples) needed = kMaxDelaySamples;
    if (static_cast<int>(m_bufL.size()) != needed) {
        m_bufL.assign(needed, 0.0f);
        m_bufR.assign(needed, 0.0f);
        m_writePos = 0;
    }
}

void Delay::setParams(float delayMs, float feedback, float mix, float dampingHz, bool pingPong) {
    m_delayMs = std::max(1.0f, std::min(3000.0f, delayMs));
    m_feedback = std::max(0.0f, std::min(0.95f, feedback));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    m_pingPong = pingPong;
    m_delaySamples = static_cast<int>(m_delayMs * 0.001f * m_sampleRate);
    int maxSamp = static_cast<int>(m_bufL.size());
    if (m_delaySamples >= maxSamp) m_delaySamples = maxSamp - 1;
    if (m_delaySamples < 1) m_delaySamples = 1;
    m_dampCoeff = static_cast<float>(std::exp(-2.0 * M_PI * dampingHz / m_sampleRate));
}

void Delay::processStereo(float &l, float &r) {
    if (m_bufL.empty()) return;

    int bufSz = static_cast<int>(m_bufL.size());
    int readPos = ((m_writePos - m_delaySamples) % bufSz + bufSz) % bufSz;

    float delL = m_bufL[readPos];
    float delR = m_bufR[readPos];

    m_dampStateL = m_dampStateL * m_dampCoeff + delL * (1.0f - m_dampCoeff);
    m_dampStateR = m_dampStateR * m_dampCoeff + delR * (1.0f - m_dampCoeff);

    float dampedL = m_dampStateL;
    float dampedR = m_dampStateR;

    if (m_pingPong) {
        float mono = (l + r) * 0.5f;
        m_bufL[m_writePos] = mono + m_feedback * dampedR;
        m_bufR[m_writePos] =        m_feedback * dampedL;
    } else {
        m_bufL[m_writePos] = l + m_feedback * dampedL;
        m_bufR[m_writePos] = r + m_feedback * dampedR;
    }

    l = l * (1.0f - m_mix) + delL * m_mix;
    r = r * (1.0f - m_mix) + delR * m_mix;

    m_writePos = (m_writePos + 1) % bufSz;
}

void Delay::reset() {
    if (!m_bufL.empty()) {
        std::fill(m_bufL.begin(), m_bufL.end(), 0.0f);
        std::fill(m_bufR.begin(), m_bufR.end(), 0.0f);
    }
    m_writePos = 0;
    m_dampStateL = 0.0f;
    m_dampStateR = 0.0f;
}
