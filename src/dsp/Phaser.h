#pragma once

#include <cmath>
#include <algorithm>

class Phaser {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depth, float feedback, int stages, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxStages = 12;

    struct AllpassStage {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float process(float x, float a) {
            float y = -a * x + x1 + a * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    double m_sampleRate = 48000.0;
    float m_rate = 0.5f;
    float m_depth = 0.7f;
    float m_feedback = 0.3f;
    int m_stages = 6;
    float m_mix = 0.5f;

    float m_minFreq = 200.0f;
    float m_maxFreq = 4000.0f;

    AllpassStage m_apL[kMaxStages] = {};
    AllpassStage m_apR[kMaxStages] = {};

    double m_phase = 0.0;
    float m_lastOutL = 0.0f;
    float m_lastOutR = 0.0f;
};
