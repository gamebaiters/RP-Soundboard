#include "DeEsser.h"
#include "../AudioUtils.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void DeEsser::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void DeEsser::setParams(float freqHz, float qFactor, float thresholdDb,
                        float rangeDb, float attackMs, float releaseMs)
{
    // Bandpass filter (RBJ Audio EQ Cookbook, constant 0 dB peak gain).
    // Coefficients stored as normalised {b0,b1,b2,a1,a2}.
    if (freqHz < 1000.0f) freqHz = 1000.0f;
    if (freqHz > 12000.0f) freqHz = 12000.0f;
    if (qFactor < 0.5f) qFactor = 0.5f;
    if (qFactor > 12.0f) qFactor = 12.0f;

    double w0    = 2.0 * M_PI * freqHz / m_sampleRate;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * qFactor);

    double b0 =  alpha;
    double b1 =  0.0;
    double b2 = -alpha;
    double a0 =  1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 =  1.0 - alpha;

    m_b0 = b0 / a0;
    m_b1 = b1 / a0;
    m_b2 = b2 / a0;
    m_a1 = a1 / a0;
    m_a2 = a2 / a0;

    m_thresholdDb = thresholdDb;
    if (rangeDb > 0.0f) rangeDb = 0.0f;
    if (rangeDb < -30.0f) rangeDb = -30.0f;
    m_rangeDb = rangeDb;

    m_attackCoeff  = 1.0f - std::exp(-1.0f / (std::max(0.1f, attackMs)
                                             * 0.001f * static_cast<float>(m_sampleRate)));
    m_releaseCoeff = 1.0f - std::exp(-1.0f / (std::max(1.0f, releaseMs)
                                             * 0.001f * static_cast<float>(m_sampleRate)));
}

void DeEsser::processStereo(float &l, float &r) {
    // Sidechain source: bandpass the input to isolate sibilance.
    double bL = m_b0 * l + m_b1 * m_xL1 + m_b2 * m_xL2 - m_a1 * m_yL1 - m_a2 * m_yL2;
    double bR = m_b0 * r + m_b1 * m_xR1 + m_b2 * m_xR2 - m_a1 * m_yR1 - m_a2 * m_yR2;
    m_xL2 = m_xL1; m_xL1 = l;
    m_yL2 = m_yL1; m_yL1 = bL;
    m_xR2 = m_xR1; m_xR1 = r;
    m_yR2 = m_yR1; m_yR1 = bR;

    float band = std::max(static_cast<float>(std::abs(bL)),
                          static_cast<float>(std::abs(bR)));
    if (m_scEnv > 1e-6f) band = m_scEnv;   // cross-slot override

    float inDb = (band > 1e-6f) ? AudioUtils::linearToDb(band) : -96.0f;
    float coeff = (inDb > m_envDb) ? m_attackCoeff : m_releaseCoeff;
    m_envDb += coeff * (inDb - m_envDb);

    float overDb = m_envDb - m_thresholdDb;
    float gainDb = 0.0f;
    if (overDb > 0.0f) {
        // Depth-scaled: gain drops by up to m_rangeDb when overshoot >= 6 dB.
        float attenuation = overDb / 6.0f;
        if (attenuation > 1.0f) attenuation = 1.0f;
        gainDb = m_rangeDb * attenuation;
    }
    float gain = AudioUtils::dbToLinear(gainDb);
    l *= gain;
    r *= gain;
}

void DeEsser::reset() {
    m_xL1 = m_xL2 = m_yL1 = m_yL2 = 0.0;
    m_xR1 = m_xR2 = m_yR1 = m_yR2 = 0.0;
    m_envDb = -96.0f;
    m_scEnv = 0.0f;
}
