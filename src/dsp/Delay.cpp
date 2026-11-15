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
        m_curDelaySamples = static_cast<float>(m_delaySamples);
    }
}

void Delay::setParams(float delayMs, float feedback, float mix, float dampingHz, bool pingPong) {
    m_delayMs = std::max(1.0f, std::min(3000.0f, delayMs));
    m_feedback = std::max(0.0f, std::min(0.95f, feedback));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    m_pingPong = pingPong;
    float target = m_delayMs * 0.001f * static_cast<float>(m_sampleRate);
    float maxSamp = static_cast<float>(m_bufL.size() > 2 ? (m_bufL.size() - 2) : 1);
    if (target > maxSamp) target = maxSamp;
    if (target < 1.0f) target = 1.0f;
    m_targetDelaySamples = target;
    m_delaySamples = static_cast<int>(target);
    if (m_curDelaySamples < 1.0f) {
        m_curDelaySamples = target;
        m_xfadeTarget = target;
        m_xfade = 0.0f;
    } else if (m_xfade <= 0.0f && std::fabs(target - m_curDelaySamples) > 8.0f) {
        m_xfadeTarget = target;
        m_xfade = 0.0001f;
        m_xfadeStep = 1.0f / std::max(64.0f, 0.030f * static_cast<float>(m_sampleRate));
    }
    m_dampCoeff = static_cast<float>(std::exp(-2.0 * M_PI * dampingHz / m_sampleRate));
}

void Delay::processStereo(float &l, float &r) {
    if (m_bufL.empty()) return;

    int bufSz = static_cast<int>(m_bufL.size());
    auto readFrac = [&](float d, float &outL, float &outR) {
        int d0 = static_cast<int>(d);
        float frac = d - static_cast<float>(d0);
        int r0 = ((m_writePos - d0) % bufSz + bufSz) % bufSz;
        int r1 = ((r0 - 1) % bufSz + bufSz) % bufSz;
        outL = m_bufL[r0] * (1.0f - frac) + m_bufL[r1] * frac;
        outR = m_bufR[r0] * (1.0f - frac) + m_bufR[r1] * frac;
    };

    float delL = 0.0f, delR = 0.0f;
    if (m_xfade > 0.0f) {
        float lA = 0.0f, rA = 0.0f, lB = 0.0f, rB = 0.0f;
        readFrac(m_curDelaySamples, lA, rA);
        readFrac(m_xfadeTarget, lB, rB);
        float t = std::min(1.0f, m_xfade);
        float w = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * t); // smooth raised-cosine fade
        delL = lA * (1.0f - w) + lB * w;
        delR = rA * (1.0f - w) + rB * w;
        m_xfade += m_xfadeStep;
        if (m_xfade >= 1.0f) {
            m_curDelaySamples = m_xfadeTarget;
            m_xfade = 0.0f;
            if (std::fabs(m_targetDelaySamples - m_curDelaySamples) > 8.0f) {
                m_xfadeTarget = m_targetDelaySamples;
                m_xfade = 0.0001f;
            }
        }
    } else {
        float diff = m_targetDelaySamples - m_curDelaySamples;
        float step = std::max(-0.20f, std::min(0.20f, diff * 0.002f));
        m_curDelaySamples += step;
        readFrac(m_curDelaySamples, delL, delR);
    }

    m_dampStateL = m_dampStateL * m_dampCoeff + delL * (1.0f - m_dampCoeff);
    m_dampStateR = m_dampStateR * m_dampCoeff + delR * (1.0f - m_dampCoeff);

    float dampedL = std::tanh(m_dampStateL);
    float dampedR = std::tanh(m_dampStateR);

    if (m_pingPong) {
        float mono = (l + r) * 0.707f;
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
    m_curDelaySamples = m_targetDelaySamples;
    m_xfadeTarget = m_targetDelaySamples;
    m_xfade = 0.0f;
    m_dampStateL = 0.0f;
    m_dampStateR = 0.0f;
}
