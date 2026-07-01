#include "NoiseGate.h"
#include "../AudioUtils.h"

void NoiseGate::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void NoiseGate::setParams(float thresholdDb, float rangeDb,
                          float attackMs, float holdMs, float releaseMs)
{
    m_thresholdDb = thresholdDb;
    m_closeDb     = thresholdDb - 3.0f;  // 3 dB hysteresis
    if (rangeDb > 0.0f) rangeDb = 0.0f;
    if (rangeDb < -80.0f) rangeDb = -80.0f;
    m_rangeLinear = AudioUtils::dbToLinear(rangeDb);
    m_attackCoeff  = 1.0f - std::exp(-1.0f / (std::max(0.1f, attackMs)
                                             * 0.001f * static_cast<float>(m_sampleRate)));
    m_releaseCoeff = 1.0f - std::exp(-1.0f / (std::max(1.0f, releaseMs)
                                             * 0.001f * static_cast<float>(m_sampleRate)));
    m_holdSamples  = static_cast<int>(std::max(0.0f, holdMs) * 0.001f
                                     * static_cast<float>(m_sampleRate));
}

void NoiseGate::processStereo(float &l, float &r) {
    float peak = std::max(std::abs(l), std::abs(r));
    if (m_scEnv > 1e-6f) peak = m_scEnv;

    float inDb = (peak > 1e-6f) ? AudioUtils::linearToDb(peak) : -96.0f;
    // Envelope always trails the signal for state decisions.
    float envCoeff = (inDb > m_envDb) ? m_attackCoeff : m_releaseCoeff;
    m_envDb += envCoeff * (inDb - m_envDb);

    // Open / close decision with hysteresis.
    if (m_envDb >= m_thresholdDb) {
        m_targetGain = 1.0f;
        m_holdCounter = m_holdSamples;
    } else if (m_envDb < m_closeDb) {
        if (m_holdCounter > 0) --m_holdCounter;
        else m_targetGain = m_rangeLinear;
    }

    // Slew gain toward target.
    float coeff = (m_targetGain > m_gain) ? m_attackCoeff : m_releaseCoeff;
    m_gain += coeff * (m_targetGain - m_gain);

    l *= m_gain;
    r *= m_gain;
}

void NoiseGate::reset() {
    m_envDb = -96.0f;
    m_gain = 1.0f;
    m_targetGain = 1.0f;
    m_holdCounter = 0;
    m_scEnv = 0.0f;
}
