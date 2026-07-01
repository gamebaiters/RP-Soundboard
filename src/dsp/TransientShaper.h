#pragma once

#include <cmath>
#include <algorithm>

// Transient shaper: two envelope followers with different time constants.
// The "fast" envelope tracks attacks; the "slow" envelope tracks the
// sustain body. The DIFFERENCE (fast - slow) isolates transient content;
// the "slow" isolates sustain.
//
// Output gain = 1 + attackGain * (fast/slow - 1)   when fast > slow
//             + sustainGain * gate(fast/slow)
//
// Simplified per-sample formulation:
//   env_fast fast attack, medium release   ~5 ms / ~200 ms
//   env_slow slow attack, slow release     ~50 ms / ~500 ms
//   trans_ratio = env_fast / max(env_slow, tiny)
//   attack_boost   = 1 + attackDb_lin * clamp(trans_ratio - 1, 0..2)
//   sustain_shape  = 1 + sustainDb_lin * clamp(1 - trans_ratio*0.5, 0..1)
class TransientShaper {
public:
    void setSampleRate(double sr);
    // attackDb: attack transient gain (-20..+20). Positive = punchier.
    // sustainDb: sustain body gain (-20..+20). Negative = tighter.
    void setParams(float attackDb, float sustainDb);
    void processStereo(float &l, float &r);
    void reset();

private:
    double m_sampleRate = 48000.0;
    float m_attackScale  = 0.0f;   // linear equivalent of attackDb - 1
    float m_sustainScale = 0.0f;
    float m_fastAtkCoeff = 0.0f;
    float m_fastRelCoeff = 0.0f;
    float m_slowAtkCoeff = 0.0f;
    float m_slowRelCoeff = 0.0f;
    float m_envFast = 0.0f;
    float m_envSlow = 0.0f;
};
