#include "Phaser.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Phaser::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Phaser::setParams(float rateHz, float depth, float feedback, int stages, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(0.0f, std::min(0.95f, feedback));
    m_stages = std::max(2, std::min(stages, kMaxStages));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
}

void Phaser::processStereo(float &l, float &r) {
    float lfoVal = 0.5f + 0.5f * static_cast<float>(std::sin(2.0 * M_PI * m_phase));
    m_phase += m_rate / m_sampleRate;
    if (m_phase >= 1.0) m_phase -= 1.0;

    float fc = m_minFreq * std::pow(m_maxFreq / m_minFreq, lfoVal * m_depth);
    float t = std::tan(static_cast<float>(M_PI) * fc / static_cast<float>(m_sampleRate));
    float a = (t - 1.0f) / (t + 1.0f);

    float inL = l + m_feedback * m_lastOutL;
    float inR = r + m_feedback * m_lastOutR;

    float outL = inL;
    float outR = inR;
    for (int s = 0; s < m_stages; s++) {
        outL = m_apL[s].process(outL, a);
        outR = m_apR[s].process(outR, a);
    }

    m_lastOutL = outL;
    m_lastOutR = outR;

    l = l * (1.0f - m_mix) + outL * m_mix;
    r = r * (1.0f - m_mix) + outR * m_mix;
}

void Phaser::reset() {
    for (int i = 0; i < kMaxStages; i++) {
        m_apL[i] = AllpassStage{};
        m_apR[i] = AllpassStage{};
    }
    m_phase = 0.0;
    m_lastOutL = 0.0f;
    m_lastOutR = 0.0f;
}
