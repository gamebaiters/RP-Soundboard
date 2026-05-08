#include "Saturator.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void Saturator::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Saturator::setParams(float drive, float mix, float toneHz, Mode mode) {
    m_drive = std::max(1.0f, std::min(20.0f, drive));
    m_mix = std::max(0.0f, std::min(1.0f, mix));
    m_mode = mode;
    m_toneCoeff = static_cast<float>(std::exp(-2.0 * M_PI * toneHz / m_sampleRate));
}

float Saturator::shape(float x) const {
    float driven = x * m_drive;
    switch (m_mode) {
        case Soft:
            return std::tanh(driven);
        case Tube:
            if (driven >= 0.0f)
                return 1.0f - std::exp(-driven);
            else
                return -(1.0f - std::exp(driven)) * 0.8f;
        case Tape:
            if (std::abs(driven) <= 1.0f)
                return 1.5f * driven * (1.0f - driven * driven / 3.0f);
            else
                return (driven > 0.0f) ? 1.0f : -1.0f;
        case Hard:
            return std::max(-1.0f, std::min(1.0f, driven));
    }
    return driven;
}

void Saturator::processStereo(float &l, float &r) {
    float satL = shape(l);
    float satR = shape(r);

    m_toneStateL = m_toneStateL * m_toneCoeff + satL * (1.0f - m_toneCoeff);
    m_toneStateR = m_toneStateR * m_toneCoeff + satR * (1.0f - m_toneCoeff);

    l = l * (1.0f - m_mix) + m_toneStateL * m_mix;
    r = r * (1.0f - m_mix) + m_toneStateR * m_mix;
}

void Saturator::reset() {
    m_toneStateL = 0.0f;
    m_toneStateR = 0.0f;
}
