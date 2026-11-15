#include "Phaser.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
inline float triWave01(double p) {
    p -= std::floor(p);
    return static_cast<float>(p < 0.5 ? (p * 2.0) : (2.0 - p * 2.0));
}
} // namespace

void Phaser::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    m_smCoeff = static_cast<float>(1.0 - std::exp(-1.0 / (0.030 * m_sampleRate)));
    m_coefInit = false;
}

void Phaser::setParams(float rateHz, float depth, float feedback, int stages, float mix) {
    m_rate = std::max(0.01f, rateHz);
    m_depth = std::max(0.0f, std::min(1.0f, depth));
    m_feedback = std::max(0.0f, std::min(0.95f, feedback));
    m_stages = std::max(2, std::min(stages, static_cast<int>(kMaxStages)));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    if (!m_primed) { m_primed = true; m_depthS = m_depth; m_fbS = m_feedback; m_mixS = m_mix; }
}

void Phaser::processStereo(float &l, float &r) {
    m_depthS += m_smCoeff * (m_depth - m_depthS);
    m_fbS    += m_smCoeff * (m_feedback - m_fbS);
    m_mixS   += m_smCoeff * (m_mix - m_mixS);

    double pL = m_phase;
    double pR = m_phase + 0.25; // quadrature stereo swirl
    if (pR >= 1.0) pR -= 1.0;

    m_phase += m_rate / m_sampleRate;
    if (m_phase >= 1.0) m_phase -= 1.0;

    // Triangle wave spends equal time per octave across 200..4000 Hz instead of stalling at extremes.
    // Allpass coefficient a = (1 - t) / (1 + t), t = tan(pi * fc / fs).
    auto coeffAt = [&](double phase) -> float {
        float lfo = triWave01(phase);
        float fc = m_minFreq * std::pow(m_maxFreq / m_minFreq, lfo * m_depthS);
        float w = static_cast<float>(M_PI) * std::min(fc, 0.42f * static_cast<float>(m_sampleRate)) /
                  static_cast<float>(m_sampleRate);
        float t = std::tan(w);
        return (1.0f - t) / (1.0f + t);
    };

    if (!m_coefInit) {
        m_aL = coeffAt(pL);
        m_aR = coeffAt(pR);
        m_coefCtr = 0;
        m_coefInit = true;
    }
    if (m_coefCtr <= 0) {
        m_coefCtr = kCoefInterval;
        const double ahead = kCoefInterval * m_rate / m_sampleRate;
        const float invN = 1.0f / static_cast<float>(kCoefInterval);
        m_aStepL = (coeffAt(pL + ahead) - m_aL) * invN;
        m_aStepR = (coeffAt(pR + ahead) - m_aR) * invN;
    }
    --m_coefCtr;
    m_aL += m_aStepL;
    m_aR += m_aStepR;
    const float aL = m_aL;
    const float aR = m_aR;

    float inL = l + m_fbS * 0.85f * std::tanh(m_lastOutL);
    float inR = r + m_fbS * 0.85f * std::tanh(m_lastOutR);

    float outL = inL;
    float outR = inR;
    for (int s = 0; s < m_stages; s++) {
        outL = m_apL[s].process(outL, aL);
        outR = m_apR[s].process(outR, aR);
    }

    m_lastOutL = (std::fabs(outL) < 1e-15f) ? 0.0f : outL;
    m_lastOutR = (std::fabs(outR) < 1e-15f) ? 0.0f : outR;

    // Classic phaser cancellation: dry + allpass gives deepest notches at 180-deg points,
    // so mix=1.0 produces the strongest possible sweep rather than flat unity allpass!
    float wetPhasedL = 0.65f * (l + outL);
    float wetPhasedR = 0.65f * (r + outR);

    l = l * (1.0f - m_mixS) + wetPhasedL * m_mixS;
    r = r * (1.0f - m_mixS) + wetPhasedR * m_mixS;
}

void Phaser::reset() {
    for (int i = 0; i < kMaxStages; i++) {
        m_apL[i] = AllpassStage{};
        m_apR[i] = AllpassStage{};
    }
    m_phase = 0.0;
    m_lastOutL = 0.0f;
    m_lastOutR = 0.0f;
    m_depthS = m_depth;
    m_fbS = m_feedback;
    m_mixS = m_mix;
    m_coefInit = false;
}
