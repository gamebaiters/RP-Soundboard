#pragma once

#include <vector>

// Real-time parametric HRTF for stereo headphones (Brown-Duda 1998
// structural model, plus a few perceptual extensions).
//
// Pipeline per ear, mono input -> stereo:
//   1. 1/r distance gain + air absorption shelf
//   2. ITD fractional delay (Woodworth, max ~656 us)
//   3. Head-shadow 1-pole/1-zero filter, alpha = 1 + cos(theta_inc)
//   4. Pinna 5-event sparse FIR (parametric delays cos(theta/2)*sin(...))
//   5. Shoulder + torso single-tap reflections
//   6. Front/back disambiguation peaking notch
//   7. Mix in a tiny Schroeder tail when the source is behind
//
// Coordinate convention: x = right, y = back, z = up. Listener at origin.
// All in meters. Distance is sqrt(x*x + y*y + z*z).

class Positional {
public:
    Positional();

    void setSampleRate(double sr);
    void setPosition(float x, float y, float z);    // meters

    // Optional head-sway LFO. Tiny ~0.3 Hz position wobble that fakes
    // the natural micro-movements humans make when listening, breaking
    // the "cone of confusion" front/back ambiguity. ~+/-1.5 deg azimuth
    // at default amount. Set to 0 to disable.
    void setHeadSwayAmount(float deg);

    // Mono sample in, two ears out.
    void process(float mono, float &outL, float &outR);

    void reset();

private:
    void recomputeCoeffs();
    void smoothTick();

    static constexpr int kMaxItdSamples = 96;   // 2 ms at 48 kHz, safe margin
    static constexpr int kPinnaBufLen   = 64;   // covers up to 1.3 ms of taps
    static constexpr int kMaxTapBuf     = 256;  // shoulder/torso ring
    static constexpr int kCombCount     = 4;

    double m_fs = 48000.0;

    // Smoothed position (one-pole exponential, ~20 ms).
    float m_xT = 0.0f, m_yT = -1.0f, m_zT = 0.0f;
    float m_x  = 0.0f, m_y  = -1.0f, m_z  = 0.0f;
    float m_smoothA = 0.998f;     // recomputed from sample rate

    // ====== Stage 1: distance + air absorption ======
    float m_gDist = 1.0f;
    float m_airAlpha = 0.0f;      // 1-pole LP coefficient (pre-mix amount)
    float m_airMix   = 1.0f;      // 1.0 = bypass, <1 = HF roll-off
    float m_airLpL = 0.0f, m_airLpR = 0.0f;

    // ====== Stage 2: per-ear fractional delay (ITD) ======
    std::vector<float> m_delayBufL;
    std::vector<float> m_delayBufR;
    int   m_delayWriteL = 0, m_delayWriteR = 0;
    float m_itdSamplesL = 0.0f;
    float m_itdSamplesR = 0.0f;

    // ====== Stage 3: head-shadow 1p/1z biquad per ear ======
    // y[n] = b0*x[n] + b1*x[n-1] - a1*y[n-1]
    float m_shB0L = 1, m_shB1L = 0, m_shA1L = 0;
    float m_shB0R = 1, m_shB1R = 0, m_shA1R = 0;
    float m_shXL = 0, m_shYL = 0;
    float m_shXR = 0, m_shYR = 0;

    // ====== Stage 4: pinna 5-event sparse FIR (per ear) ======
    // delays/gains per event; events {2,3,4,5,6} from Brown-Duda 1998.
    static constexpr int kPinnaEvents = 5;
    std::vector<float> m_pinnaBufL;
    std::vector<float> m_pinnaBufR;
    int  m_pinnaWriteL = 0, m_pinnaWriteR = 0;
    float m_pinnaTapL[kPinnaEvents] = {0};
    float m_pinnaTapR[kPinnaEvents] = {0};
    float m_pinnaGainL[kPinnaEvents] = {0};
    float m_pinnaGainR[kPinnaEvents] = {0};

    // ====== Stage 5: shoulder + torso single-tap echoes (per ear) ======
    std::vector<float> m_tapBufL;
    std::vector<float> m_tapBufR;
    int   m_tapWriteL = 0, m_tapWriteR = 0;
    float m_shoulderTapL = 0, m_shoulderTapR = 0;
    float m_shoulderGainL = 0, m_shoulderGainR = 0;
    float m_torsoTapL = 0, m_torsoTapR = 0;
    float m_torsoGainL = 0, m_torsoGainR = 0;

    // ====== Stage 6: front/back peaking notch (biquad per ear) ======
    // Coefficients live here; updated when angle changes.
    float m_fbB0L = 1, m_fbB1L = 0, m_fbB2L = 0, m_fbA1L = 0, m_fbA2L = 0;
    float m_fbB0R = 1, m_fbB1R = 0, m_fbB2R = 0, m_fbA1R = 0, m_fbA2R = 0;
    float m_fbXL1 = 0, m_fbXL2 = 0, m_fbYL1 = 0, m_fbYL2 = 0;
    float m_fbXR1 = 0, m_fbXR2 = 0, m_fbYR1 = 0, m_fbYR2 = 0;

    // ====== Stage 6b: concha spectral cue (peaking biquad ~3 kHz) ======
    // Hebrank-Wright / Blauert: real pinnae boost ~2-4 kHz for sources
    // in front and roll the same band off for sources behind. That is
    // the dominant monaural spectral cue distinguishing front from back
    // ("tinnier in front, bassier behind"), so we add a peaking biquad
    // at 3 kHz Q=1.0 swung +/-4 dB by source front-ness.
    float m_chB0 = 1, m_chB1 = 0, m_chB2 = 0, m_chA1 = 0, m_chA2 = 0;
    float m_chXL1 = 0, m_chXL2 = 0, m_chYL1 = 0, m_chYL2 = 0;
    float m_chXR1 = 0, m_chXR2 = 0, m_chYR1 = 0, m_chYR2 = 0;

    // ====== Stage 7: tiny Schroeder back-tail ======
    // 4 comb filters in parallel. Mix amount depends on cos(theta_az):
    // back = wetter, front = dry. Feedback intentionally low (0.45..0.55)
    // and short delays - any longer/hotter and the rack rings on dense
    // music material like a metallic plate.
    std::vector<float> m_combBuf[kCombCount];
    int   m_combIdx[kCombCount] = {0};
    int   m_combLen[kCombCount] = {617, 727, 853, 941};   // ~13..20 ms @ 48k
    float m_combFb[kCombCount]  = {0.45f, 0.48f, 0.51f, 0.54f};
    float m_tailMix = 0.0f;

    // ====== Control-rate scheduling ======
    // Recomputing every per-sample re-derives 5 trig calls + biquad coeffs
    // 48000 times per second. Worse, abrupt coefficient changes against a
    // stateful biquad cause clicks and ringing. Update every kCtrlBlock
    // samples instead - the position smoother in setPosition gives us a
    // continuous trajectory anyway.
    static constexpr int kCtrlBlock = 32;
    int m_ctrlCounter = 0;

    // ====== Head-sway LFO ======
    // Slow azimuth wobble that simulates the natural micro-movements
    // listeners make. Without it, perfectly static sources sit in a
    // "cone of confusion" - front and back at the same azimuth produce
    // near-identical ear signals. With it, the ITD/ILD pattern dances
    // slightly, and the brain triangulates the static cone.
    float m_swayAmountDeg = 0.0f;     // 0 = disabled
    double m_swayPhase = 0.0;

    // Per-ear sign: pinna shapes are mirrored.
    static constexpr float kEarOffsetDeg = 10.0f;  // 10 deg backward bias
    static constexpr float kHeadRadius   = 0.0875f; // m
    static constexpr float kSpeedSound   = 343.0f;  // m/s
};
