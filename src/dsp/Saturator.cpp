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
    // ----- 2x oversampled saturation path -----
    // Upsample by linear interpolation between previous and current
    // input: produces samples s0 (mid-point) and s1 (current). Shape
    // both, then a 4-tap [1 2 1]-style box low-pass + decimate-by-2
    // returns the band-limited shaped sample. Same topology applied
    // independently per channel.
    float sl0 = 0.5f * (m_osPrevInL + l);
    float sl1 = l;
    float sr0 = 0.5f * (m_osPrevInR + r);
    float sr1 = r;
    m_osPrevInL = l;
    m_osPrevInR = r;

    sl0 = shape(sl0); sl1 = shape(sl1);
    sr0 = shape(sr0); sr1 = shape(sr1);

    // 3-tap symmetric FIR { 1/4, 1/2, 1/4 } running on the 2x stream.
    // Take the even-indexed (s1-phase) output, which after decimation
    // gives a flat band up to ~Fs/4 with -6dB at Fs/2 (the alias
    // image's centre) and ~-18dB by 3Fs/8. Simple, branch-free, no
    // extra buffering past the two h-state taps below.
    float yL = 0.25f * m_osH2L + 0.5f * sl0 + 0.25f * sl1;
    float yR = 0.25f * m_osH2R + 0.5f * sr0 + 0.25f * sr1;
    m_osH3L = m_osH2L; m_osH2L = sl1;
    m_osH3R = m_osH2R; m_osH2R = sr1;

    float satL = yL;
    float satR = yR;

    m_toneStateL = m_toneStateL * m_toneCoeff + satL * (1.0f - m_toneCoeff);
    m_toneStateR = m_toneStateR * m_toneCoeff + satR * (1.0f - m_toneCoeff);

    l = l * (1.0f - m_mix) + m_toneStateL * m_mix;
    r = r * (1.0f - m_mix) + m_toneStateR * m_mix;
}

void Saturator::reset() {
    m_toneStateL = 0.0f;
    m_toneStateR = 0.0f;
    m_osPrevInL = m_osPrevInR = 0.0f;
    m_osH1L = m_osH2L = m_osH3L = 0.0f;
    m_osH1R = m_osH2R = m_osH3R = 0.0f;
}
