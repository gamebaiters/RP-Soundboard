#pragma once

#include <cmath>
#include <algorithm>

// Full-band noise gate with attack / hold / release timing.
//
// State machine:
//   Closed  -> input below open threshold: gain -> range (typ. silent).
//   Opening -> input crossed open threshold: gain slews toward 1.0.
//   Open    -> gain at 1.0, hold timer resets on every crossing.
//   Closing -> hold expired: gain slews toward range.
//
// Hysteresis: open threshold != close threshold (open ~3 dB above
// close) so tiny fluctuations around the threshold do not chatter the
// gate open/closed.
//
// Optional cross-slot sidechain (feedSidechain): when >0, replaces
// the internal envelope so another slot's energy can trigger the gate.
class NoiseGate {
public:
    void setSampleRate(double sr);
    // thresholdDb: gate opens when envelope crosses this (typ. -40..-20).
    // rangeDb: max gain reduction when closed (0..-80).
    // attackMs / holdMs / releaseMs: open / hold / close timing.
    void setParams(float thresholdDb, float rangeDb,
                   float attackMs, float holdMs, float releaseMs);
    void processStereo(float &l, float &r);
    void reset();
    void feedSidechain(float envLinear) { m_scEnv = envLinear; }

private:
    double m_sampleRate = 48000.0;

    float m_thresholdDb   = -40.0f;
    float m_closeDb       = -43.0f;  // hysteresis
    float m_rangeLinear   = 0.0f;
    float m_attackCoeff   = 0.0f;
    float m_releaseCoeff  = 0.0f;
    int   m_holdSamples   = 0;

    float m_envDb    = -96.0f;
    float m_gain     = 1.0f;
    float m_targetGain = 1.0f;
    int   m_holdCounter = 0;

    float m_scEnv = 0.0f;
};
