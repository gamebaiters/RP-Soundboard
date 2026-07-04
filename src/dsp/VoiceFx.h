#pragma once

#include "SandboxState.h"
#include "PitchShiftGrain.h"

#include <vector>
#include <memory>
#include <cstdint>

class SimpleFFT;

// Voice FX macro-module (Stage_VoiceFx): ten classic effects sharing a
// single pipeline stage. Every sub-effect is individually toggleable
// from the sandbox UI; disabled subs cost zero CPU. Internal order is
// fixed and musically sane:
//
//   Autotune -> Formant -> Vocoder -> RingMod -> AutoWah -> Vibrato ->
//   Tremolo -> Exciter -> ReverseDelay -> Shimmer
//
// (pitch first, then spectral rewrites, then modulators, then space.)
//
// The three "voice-centric" processors (Autotune / Formant / Vocoder)
// fold the signal to mono internally - they are voice effects, voice
// is mono, and running dual FFT chains per sample would double the
// cost for no audible benefit. Everything else is true stereo.
class VoiceFx {
public:
    VoiceFx();
    ~VoiceFx();

    void setSampleRate(double sr);
    // Copies the vfx* fields out of the sandbox state and refreshes
    // derived coefficients. Cheap; call from applyState.
    void setParams(const SandboxState &s);
    void processStereo(float &l, float &r);
    void reset();

    bool anySubEnabled() const;

    // LFO-matrix live modulation hooks (audio-thread, cheap setters;
    // values re-baselined from setParams on every state push).
    void modRingFreq(float hz)   { m_liveRingFreq = hz; }
    void modWahBias(float bias)  { m_liveWahBias = bias; }   // -1..+1 sweep offset
    void modTremRate(float hz)   { m_liveTremRate = hz; }
    void modVibDepth(float d)    { m_liveVibDepth = d; }

private:
    // ---- Autotune ----
    void  processTune(float &m);
    float detectPitch();                  // Hz, <= 0 when unvoiced
    float nearestScaleFreq(float hz) const;

    // ---- Formant (FFT OLA) ----
    void  processFormantSample(float &m);
    void  formantFrame();

    // ---- Vocoder ----
    void  processVocoder(float &m);

    // Cheap per-sample subs.
    void  processRing(float &l, float &r);
    void  processTrem(float &l, float &r);
    void  processVib(float &l, float &r);
    void  processWah(float &l, float &r);
    void  processExciter(float &l, float &r);
    void  processRevDelay(float &l, float &r);
    void  processShimmer(float &l, float &r);

    double m_fs = 48000.0;
    SandboxState m_p;      // only the vfx* fields are read

    // Live LFO-matrix overrides (NaN-free: seeded from params).
    float m_liveRingFreq = 440.0f;
    float m_liveWahBias  = 0.0f;
    float m_liveTremRate = 5.0f;
    float m_liveVibDepth = 0.5f;

    // ---- Ring mod ----
    double m_ringPhase = 0.0;

    // ---- Tremolo ----
    double m_tremPhase = 0.0;
    float  m_tremGainSm = 1.0f;

    // ---- Vibrato ----
    static constexpr int kVibMax = 1024;   // ~21 ms at 48 kHz
    float  m_vibBufL[kVibMax] = {};
    float  m_vibBufR[kVibMax] = {};
    int    m_vibW = 0;
    double m_vibPhase = 0.0;

    // ---- Auto-wah ----
    float m_wahEnv = 0.0f;
    float m_wahEnvAtk = 0.0f, m_wahEnvRel = 0.0f;
    // RBJ bandpass state (stereo shares coeffs, separate state).
    float m_wahB0 = 0, m_wahB1 = 0, m_wahB2 = 0, m_wahA1 = 0, m_wahA2 = 0;
    float m_wahX1L = 0, m_wahX2L = 0, m_wahY1L = 0, m_wahY2L = 0;
    float m_wahX1R = 0, m_wahX2R = 0, m_wahY1R = 0, m_wahY2R = 0;
    int   m_wahCoeffCountdown = 0;
    void  wahRecalc(float centerHz);

    // ---- Exciter ----
    float m_excHpL = 0.0f, m_excHpR = 0.0f;   // 1-pole HP state
    float m_excHpCoeff = 0.0f;
    // 2x oversample memory for the shaper (hqOversampling).
    float m_excPrevL = 0.0f, m_excPrevR = 0.0f;

    // ---- Autotune ----
    static constexpr int kTuneBuf = 2048;     // detection window
    float  m_tuneRing[kTuneBuf] = {};
    int    m_tuneW = 0;
    int    m_tuneHopCounter = 0;
    float  m_tuneRatioTarget = 1.0f;
    float  m_tuneRatioSm = 1.0f;
    float  m_tuneSmCoeff = 0.05f;
    PitchShiftGrain m_tuneShift;

    // ---- Formant ----
    static constexpr int kFormN   = 1024;
    static constexpr int kFormHop = 256;
    std::unique_ptr<SimpleFFT> m_formFft;
    std::vector<float> m_formIn;       // input accumulation (kFormN)
    std::vector<float> m_formOut;      // OLA output (kFormN + kFormHop)
    std::vector<float> m_formWin;      // Hann
    std::vector<float> m_formFreq;     // kFormN+2
    std::vector<float> m_formEnv, m_formEnvW;
    std::vector<float> m_formTime;
    int   m_formFill = 0;
    int   m_formInW = 0;       // ring write index into m_formIn
    int   m_formReadIdx = 0;
    bool  m_formPrimed = false;

    // ---- Vocoder ----
    static constexpr int kVocBands = 16;
    struct VocBand {
        // Bandpass coeffs (shared carrier/mod per band) + two states.
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float mx1 = 0, mx2 = 0, my1 = 0, my2 = 0;   // modulator state
        float cx1 = 0, cx2 = 0, cy1 = 0, cy2 = 0;   // carrier state
        float env = 0.0f;
    };
    VocBand m_voc[kVocBands];
    double  m_vocSawPhase = 0.0;
    uint32_t m_vocNoise = 0x12345678u;
    float   m_vocAtk = 0.0f, m_vocRel = 0.0f;
    float   m_vocLastPitch = -1.0f;
    void    vocRebuild();

    // ---- Reverse delay ----
    std::vector<float> m_revBufL, m_revBufR;
    int   m_revN = 0;          // active grain length (samples)
    int   m_revIdx = 0;        // position within current grain
    float m_revLastTimeMs = -1.0f;
    void  revRebuild();

    // ---- Shimmer ----
    std::vector<float> m_shimDlyL, m_shimDlyR;
    int   m_shimWL = 0, m_shimWR = 0;
    float m_shimLpL = 0.0f, m_shimLpR = 0.0f;
    PitchShiftGrain m_shimShiftL, m_shimShiftR;
    // Small input diffusion allpasses.
    std::vector<float> m_shimApL, m_shimApR;
    int   m_shimApWL = 0, m_shimApWR = 0;
};
