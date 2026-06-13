#include "Compressor.h"
#include "../AudioUtils.h"

void Compressor::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Compressor::setParams(float thresholdDb, float ratio, float attackMs, float releaseMs,
                           float kneeDb, float makeupDb) {
    m_thresholdDb = thresholdDb;
    m_ratio = std::max(1.0f, ratio);
    m_kneeDb = std::max(0.0f, kneeDb);
    m_makeupGain = AudioUtils::dbToLinear(makeupDb);
    m_attackCoeff = 1.0f - std::exp(-1.0f / (std::max(0.1f, attackMs) * 0.001f * static_cast<float>(m_sampleRate)));
    m_releaseCoeff = 1.0f - std::exp(-1.0f / (std::max(1.0f, releaseMs) * 0.001f * static_cast<float>(m_sampleRate)));
}

void Compressor::processStereo(float &l, float &r) {
    float peak = std::max(std::abs(l), std::abs(r));
    float inputDb = (peak > 1e-6f) ? AudioUtils::linearToDb(peak) : -96.0f;

    float coeff = (inputDb > m_envDb) ? m_attackCoeff : m_releaseCoeff;
    m_envDb = m_envDb + coeff * (inputDb - m_envDb);

    float overDb = m_envDb - m_thresholdDb;
    float gainDb = 0.0f;
    if (m_kneeDb > 0.0f && overDb > -m_kneeDb / 2.0f && overDb < m_kneeDb / 2.0f) {
        float x = overDb + m_kneeDb / 2.0f;
        gainDb = -(1.0f - 1.0f / m_ratio) * x * x / (2.0f * m_kneeDb);
    } else if (overDb > 0.0f) {
        gainDb = -(1.0f - 1.0f / m_ratio) * overDb;
    }

    float gain = AudioUtils::dbToLinear(gainDb) * m_makeupGain;
    l *= gain;
    r *= gain;
}

void Compressor::reset() {
    m_envDb = -96.0f;
}
