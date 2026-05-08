#include "Flanger.h"
#include <iterator>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Flanger::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Flanger::setParams(float rateHz, float depth, float feedback, float baseDelayMs, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(-0.95f, std::min(0.95f, feedback));
    m_baseDelay = std::max(0.1f, baseDelayMs);
    m_mix = std::max(0.0f, std::min(1.0f, mix));
}

void Flanger::processStereo(float &l, float &r) {
    float lfo = static_cast<float>(std::sin(2.0 * M_PI * m_phase));
    m_phase += m_rate / m_sampleRate;
    if (m_phase >= 1.0) m_phase -= 1.0;

    float delaySamples = (m_baseDelay + m_baseDelay * m_depth * lfo) * 0.001f * static_cast<float>(m_sampleRate);
    delaySamples = std::max(1.0f, std::min(delaySamples, static_cast<float>(kMaxBufSize - 2)));

    m_bufL[m_writePos] = l + m_feedback * m_lastOutL;
    m_bufR[m_writePos] = r + m_feedback * m_lastOutR;

    int idxI = static_cast<int>(delaySamples);
    float frac = delaySamples - idxI;
    int idx0 = ((m_writePos - idxI) % kMaxBufSize + kMaxBufSize) % kMaxBufSize;
    int idx1 = ((idx0 - 1) % kMaxBufSize + kMaxBufSize) % kMaxBufSize;

    float delayedL = m_bufL[idx0] * (1.0f - frac) + m_bufL[idx1] * frac;
    float delayedR = m_bufR[idx0] * (1.0f - frac) + m_bufR[idx1] * frac;

    m_lastOutL = delayedL;
    m_lastOutR = delayedR;

    l = l * (1.0f - m_mix) + delayedL * m_mix;
    r = r * (1.0f - m_mix) + delayedR * m_mix;

    m_writePos = (m_writePos + 1) % kMaxBufSize;
}

void Flanger::reset() {
    std::fill(std::begin(m_bufL), std::end(m_bufL), 0.0f);
    std::fill(std::begin(m_bufR), std::end(m_bufR), 0.0f);
    m_writePos = 0;
    m_phase = 0.0;
    m_lastOutL = 0.0f;
    m_lastOutR = 0.0f;
}
