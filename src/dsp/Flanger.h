#pragma once

#include <cmath>
#include <algorithm>

class Flanger {
public:
    void setSampleRate(double sr);
    void setParams(float rateHz, float depth, float feedback, float baseDelayMs, float mix);
    void processStereo(float &l, float &r);
    void reset();

private:
    static constexpr int kMaxBufSize = 4800;

    double m_sampleRate = 48000.0;
    float m_rate = 0.5f;
    float m_depth = 0.7f;
    float m_feedback = 0.5f;
    float m_baseDelay = 2.0f;
    float m_mix = 0.5f;

    float m_bufL[kMaxBufSize] = {};
    float m_bufR[kMaxBufSize] = {};
    int m_writePos = 0;
    double m_phase = 0.0;

    float m_lastOutL = 0.0f;
    float m_lastOutR = 0.0f;

    // Smoothed copies (one-pole, ~30 ms) of depth/base delay/feedback/mix: no clicks on slider drags.
    float m_depthS = 0.7f;
    float m_baseDelayS = 2.0f;
    float m_fbS = 0.5f;
    float m_mixS = 0.5f;
    float m_smCoeff = 0.0007f;
    bool m_primed = false; // first setParams snaps the smoothed values (no ramp from defaults)

    // Delay times (pow) refreshed every kCoefInterval samples, linear in between.
    static constexpr int kCoefInterval = 16;
    float m_delL = 96.0f, m_delR = 96.0f;
    float m_delStepL = 0.0f, m_delStepR = 0.0f;
    int m_coefCtr = 0;
    bool m_coefInit = false;
};
