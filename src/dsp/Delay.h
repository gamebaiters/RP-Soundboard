#pragma once

#include <cmath>
#include <algorithm>
#include <vector>

class Delay {
public:
    void setSampleRate(double sr);
    void setParams(float delayMs, float feedback, float mix, float dampingHz, bool pingPong);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxDelaySamples = 48000 * 3;

    double m_sampleRate = 48000.0;
    float m_delayMs = 300.0f;
    float m_feedback = 0.4f;
    float m_mix = 0.3f;
    bool m_pingPong = false;

    std::vector<float> m_bufL;
    std::vector<float> m_bufR;
    int m_writePos = 0;
    int m_delaySamples = 0;

    float m_dampCoeff = 0.0f;
    float m_dampStateL = 0.0f;
    float m_dampStateR = 0.0f;
};
