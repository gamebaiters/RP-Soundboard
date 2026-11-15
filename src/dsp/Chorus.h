#pragma once

#include <cmath>
#include <algorithm>

class Chorus {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depthMs, float baseDelayMs, int voices, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxVoices = 4;
    static constexpr int kMaxBufSize = 48000;

    double m_sampleRate = 48000.0;
    float m_rate = 1.0f;
    float m_depth = 3.0f;
    float m_baseDelay = 10.0f;
    int m_voices = 2;
    float m_mix = 0.5f;

    float m_bufL[kMaxBufSize] = {};
    float m_bufR[kMaxBufSize] = {};
    int m_writePos = 0;
    int m_bufSize = 48000;

    double m_phase[kMaxVoices] = {};
    float m_wetLpL = 0.0f;
    float m_wetLpR = 0.0f;
    float m_wetHpL = 0.0f;
    float m_wetHpR = 0.0f;

    // Wet-path filter coefficients (8.5 kHz LP, 110 Hz HP), precomputed in setSampleRate.
    float m_lpC = 0.6715f;
    float m_hpC = 0.0143f;

    // Smoothed copies (one-pole, ~30 ms) of depth/base delay/mix: no clicks on slider drags.
    float m_depthS = 3.0f;
    float m_baseDelayS = 10.0f;
    float m_mixS = 0.5f;
    float m_smCoeff = 0.0007f;
    bool m_primed = false; // first setParams snaps the smoothed values (no ramp from defaults)
};
