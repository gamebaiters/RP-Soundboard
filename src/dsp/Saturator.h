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
};
