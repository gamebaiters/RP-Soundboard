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
        // First-order allpass H(z) = (-a + z^-1) / (1 - a z^-1): pole at z = +a,
        // a = (1 - t) / (1 + t), t = tan(pi * fc / fs) -> -90 deg phase exactly at fc.
        // (Equivalent form: y = b*x + x1 - b*y1 with b = (t - 1) / (t + 1) = -a.)
        float process(float x, float a) {
            float y = -a * x + x1 + a * y1;
            if (std::fabs(y) < 1e-15f) y = 0.0f; // anti-denormal flush
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

    // Smoothed copies of depth/feedback/mix (one-pole, ~30 ms) avoid clicks on slider drags.
    float m_depthS = 0.7f;
    float m_fbS = 0.3f;
    float m_mixS = 0.5f;
    float m_smCoeff = 0.0007f;
    bool m_primed = false; // first setParams snaps the smoothed values (no ramp from defaults)

    // Allpass coefficients (tan/pow) refreshed every kCoefInterval samples, linear in between.
    static constexpr int kCoefInterval = 16;
    float m_aL = 0.0f, m_aR = 0.0f;
    float m_aStepL = 0.0f, m_aStepR = 0.0f;
    int m_coefCtr = 0;
    bool m_coefInit = false;
};
