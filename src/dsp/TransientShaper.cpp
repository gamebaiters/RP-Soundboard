#include "TransientShaper.h"
#include "../AudioUtils.h"

namespace {
float onePoleCoeff(float ms, double sr) {
    return 1.0f - std::exp(-1.0f / (std::max(0.1f, ms) * 0.001f * static_cast<float>(sr)));
}
}

void TransientShaper::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_fastAtkCoeff = onePoleCoeff(5.0f,   m_sampleRate);
    m_fastRelCoeff = onePoleCoeff(150.0f, m_sampleRate);
    m_slowAtkCoeff = onePoleCoeff(60.0f,  m_sampleRate);
    m_slowRelCoeff = onePoleCoeff(500.0f, m_sampleRate);
}

void TransientShaper::setParams(float attackDb, float sustainDb) {
    if (attackDb < -20.0f) attackDb = -20.0f;
    if (attackDb >  20.0f) attackDb =  20.0f;
    if (sustainDb < -20.0f) sustainDb = -20.0f;
    if (sustainDb >  20.0f) sustainDb =  20.0f;
    m_attackScale  = AudioUtils::dbToLinear(attackDb)  - 1.0f;
    m_sustainScale = AudioUtils::dbToLinear(sustainDb) - 1.0f;
}

void TransientShaper::processStereo(float &l, float &r) {
    float peak = std::max(std::abs(l), std::abs(r));
    // Fast envelope: quick response to transients.
    float fc = (peak > m_envFast) ? m_fastAtkCoeff : m_fastRelCoeff;
    m_envFast += fc * (peak - m_envFast);
    // Slow envelope: sustain body.
    float sc = (peak > m_envSlow) ? m_slowAtkCoeff : m_slowRelCoeff;
    m_envSlow += sc * (peak - m_envSlow);

    // Difference isolates transient content; ratio scales it.
    float ratio = m_envFast / std::max(m_envSlow, 1e-6f);
    float transient = ratio - 1.0f;
    if (transient < 0.0f) transient = 0.0f;
    if (transient > 2.0f) transient = 2.0f;
    float sustain = 1.0f - transient * 0.5f;
    if (sustain < 0.0f) sustain = 0.0f;
    if (sustain > 1.0f) sustain = 1.0f;

    float gain = 1.0f + m_attackScale * transient + m_sustainScale * sustain;
    if (gain < 0.0f) gain = 0.0f;
    l *= gain;
    r *= gain;
}

void TransientShaper::reset() {
    m_envFast = 0.0f;
    m_envSlow = 0.0f;
}
