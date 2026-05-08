#include "Flangus.h"
#include <iterator>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Flangus::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_bufSize = static_cast<int>(m_sampleRate);
    if (m_bufSize > kMaxBufSize) m_bufSize = kMaxBufSize;
    if (m_bufSize < 1) m_bufSize = 1;
}

void Flangus::setParams(float rateHz, float depth, float feedback, int voices,
                         float spread, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(-0.95f, std::min(0.95f, feedback));
    m_voices = std::max(1, std::min(voices, kMaxVoices));
    m_spread = std::max(0.0f, std::min(1.0f, spread));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
}

void Flangus::processStereo(float &l, float &r) {
    if (m_bufSize <= 0) return;

    m_bufL[m_writePos] = l + m_feedback * m_lastWetL;
    m_bufR[m_writePos] = r + m_feedback * m_lastWetR;

    float wetL = 0.0f, wetR = 0.0f;
    float phaseStep = static_cast<float>(m_rate / m_sampleRate);

    for (int v = 0; v < m_voices; v++) {
        float lfo = static_cast<float>(std::sin(2.0 * M_PI * m_phase[v]));
        m_phase[v] += phaseStep;
        if (m_phase[v] >= 1.0) m_phase[v] -= 1.0;

        float delayMs = m_minDelayMs + (m_maxDelayMs - m_minDelayMs) * (0.5f + 0.5f * m_depth * lfo);
        float delaySamples = delayMs * 0.001f * static_cast<float>(m_sampleRate);
        delaySamples = std::max(1.0f, std::min(delaySamples, static_cast<float>(m_bufSize - 2)));

        int idxI = static_cast<int>(delaySamples);
        float frac = delaySamples - idxI;
        int idx0 = ((m_writePos - idxI) % m_bufSize + m_bufSize) % m_bufSize;
        int idx1 = ((idx0 - 1) % m_bufSize + m_bufSize) % m_bufSize;

        wetL += m_bufL[idx0] * (1.0f - frac) + m_bufL[idx1] * frac;
        wetR += m_bufR[idx0] * (1.0f - frac) + m_bufR[idx1] * frac;
    }

    wetL /= m_voices;
    wetR /= m_voices;

    m_lastWetL = wetL;
    m_lastWetR = wetR;

    l = l * (1.0f - m_mix) + wetL * m_mix;
    r = r * (1.0f - m_mix) + wetR * m_mix;

    m_writePos = (m_writePos + 1) % m_bufSize;
}

void Flangus::reset() {
    std::fill(std::begin(m_bufL), std::end(m_bufL), 0.0f);
    std::fill(std::begin(m_bufR), std::end(m_bufR), 0.0f);
    m_writePos = 0;
    m_lastWetL = 0.0f;
    m_lastWetR = 0.0f;
    for (int i = 0; i < kMaxVoices; i++) {
        m_phase[i] = static_cast<double>(i) * m_spread / kMaxVoices;
    }
}
