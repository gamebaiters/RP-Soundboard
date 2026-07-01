#pragma once

#include <cmath>
#include <algorithm>

class Saturator {
public:
    enum Mode { Soft = 0, Tube = 1, Tape = 2, Hard = 3 };

    void setSampleRate(double sr);
    void setParams(float drive, float mix, float toneHz, Mode mode);
    void processStereo(float &l, float &r);
    void reset();

private:
    double m_sampleRate = 48000.0;
    float m_drive = 2.0f;
    float m_mix = 0.5f;
    Mode m_mode = Soft;

    float m_toneCoeff = 0.0f;
    float m_toneStateL = 0.0f;
    float m_toneStateR = 0.0f;

    float shape(float x) const;

    // 2x oversampling state. Each input sample is upsampled (linear
    // interpolation with previous), shaped at 2x rate, then a 4-tap
    // moving-average half-band downsample keeps the band-limited
    // result. Cuts the saturator's broadband alias content roughly
    // in half - enough that EQ-boosted HF fed downstream into the
    // Spatial HRTF convolution stops folding back into the audible
    // band as inharmonic frying-like noise.
    float m_osPrevInL = 0.0f, m_osPrevInR = 0.0f;
    float m_osH1L = 0.0f, m_osH2L = 0.0f, m_osH3L = 0.0f;
    float m_osH1R = 0.0f, m_osH2R = 0.0f, m_osH3R = 0.0f;
};
