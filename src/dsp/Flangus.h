#pragma once

#include <cmath>
#include <algorithm>

class Flangus {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depth, float feedback, int voices,
                   float spread, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxVoices = 4;
    static constexpr int kMaxBufSize = 48000;

    double m_sampleRate = 48000.0;
    float m_rate = 0.8f;
    float m_depth = 0.5f;
    float m_feedback = 0.3f;
    int m_voices = 3;
    float m_spread = 0.5f;
    float m_mix = 0.5f;

    float m_minDelayMs = 0.6f;
    float m_maxDelayMs = 11.0f;

    float m_bufL[kMaxBufSize] = {};
    float m_bufR[kMaxBufSize] = {};
    int m_writePos = 0;
    int m_bufSize = 48000;
    double m_phase[kMaxVoices] = {};
    float m_lastWetL = 0.0f;
    float m_lastWetR = 0.0f;

    // Smoothed copies (one-pole, ~30 ms) of depth/spread/feedback/mix: no clicks on slider drags.
    float m_depthS = 0.5f;
    float m_spreadS = 0.5f;
    float m_fbS = 0.3f;
    float m_mixS = 0.5f;
    float m_smCoeff = 0.0007f;
    bool m_primed = false; // first setParams snaps the smoothed values (no ramp from defaults)
};
