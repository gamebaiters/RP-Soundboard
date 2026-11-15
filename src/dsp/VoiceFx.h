#pragma once

#include "SandboxState.h"
#include "PitchShiftGrain.h"
#include "AutoTune.h"

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
class VoiceFx {
public:
    VoiceFx();
    ~VoiceFx();

    void setSampleRate(double sr);
    void setParams(const SandboxState &s);
    void processStereo(float &l, float &r);
    void reset();

    bool anySubEnabled() const;

    // LFO-matrix live modulation hooks (audio-thread, cheap setters;
    // values re-baselined from setParams on every state push).
    void modRingFreq(float hz)   { m_liveRingFreq = hz; }
    void modWahBias(float bias)  { m_liveWahBias = bias; }
    void modTremRate(float hz)   { m_liveTremRate = hz; }
    void modVibDepth(float d)    { m_liveVibDepth = d; }

    const AutoTune &autoTune() const { return m_autoTune; }

private:
    // ---- Formant (Cepstral Envelope FFT OLA) ----
    void  processFormantSample(float &m);
    void  formantFrame();

    // ---- Vocoder (20-band 4th-order BPF + AGC + Unvoiced Sibilance) ----
    void  processVocoder(float &m);

    // ---- Per-sample stereo subs ----
    void  processRing(float &l, float &r);
    void  processTrem(float &l, float &r);
    void  processVib(float &l, float &r);
    void  processWah(float &l, float &r);
    void  processExciter(float &l, float &r);
    void  processRevDelay(float &l, float &r);
    void  processShimmer(float &l, float &r);

    double m_fs = 48000.0;
    SandboxState m_p;

    // Live LFO-matrix overrides.
    float m_liveRingFreq = 440.0f;
    float m_liveWahBias  = 0.0f;
    float m_liveTremRate = 5.0f;
    float m_liveVibDepth = 0.5f;

    // ---- Ring mod ----
    double m_ringPhase = 0.0;

    // ---- Tremolo ----
    double m_tremPhase = 0.0;
    float  m_tremGainSm = 1.0f;
    float  m_tremSmCoeff = 0.02f;

    // ---- Vibrato ----
    static constexpr int kVibMax = 2048;   // ~42 ms at 48 kHz
    float  m_vibBufL[kVibMax] = {};
    float  m_vibBufR[kVibMax] = {};
    int    m_vibW = 0;
    double m_vibPhase = 0.0;

    // ---- Auto-wah ----
    float m_wahEnv = 0.0f;
    float m_wahEnvAtk = 0.0f, m_wahEnvRel = 0.0f;
    float m_wahB0 = 0, m_wahB1 = 0, m_wahB2 = 0, m_wahA1 = 0, m_wahA2 = 0;
    float m_wahX1L = 0, m_wahX2L = 0, m_wahY1L = 0, m_wahY2L = 0;
    float m_wahX1R = 0, m_wahX2R = 0, m_wahY1R = 0, m_wahY2R = 0;
    int   m_wahCoeffCountdown = 0;
    void  wahRecalc(float centerHz);

    // ---- Exciter ----
    float m_excHpL = 0.0f, m_excHpR = 0.0f;
    float m_excHpCoeff = 0.0f;
    float m_excPrevL = 0.0f, m_excPrevR = 0.0f;

    // ---- Stereo image preservation for Tune/Formant (mid processed, side delayed by the same latency) ----
    static constexpr int kSideRing = 8192;          // > AutoTune::latencySamples() + kFormN even @ 96 kHz
    static constexpr int kSideRingMask = kSideRing - 1;
    float    m_sideRing[kSideRing] = {};
    uint32_t m_sideW = 0;

    // ---- Autotune (YIN + phase-locked TD-PSOLA) ----
    AutoTune m_autoTune;

    // ---- Formant (Cepstral OLA + matched dry latency) ----
    static constexpr int kFormN   = 1024;
    static constexpr int kFormHop = 256;
    std::unique_ptr<SimpleFFT> m_formFft;
    std::vector<float> m_formIn;
    std::vector<float> m_formOut;
    std::vector<float> m_formWin;
    std::vector<float> m_formFreq;
    std::vector<float> m_formCepFreq;
    std::vector<float> m_formEnv, m_formEnvW;
    std::vector<float> m_formTime;
    std::vector<float> m_formDryRing;
    int   m_formDryIdx = 0;
    int   m_formFill = 0;
    int   m_formInW = 0;
    int   m_formReadIdx = 0;
    bool  m_formPrimed = false;

    // ---- Vocoder (20-band 4th-order BPF) ----
    static constexpr int kVocBands = 20;
    struct VocBiquad {
        float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        float process(float x, float b0, float b1, float b2, float a1, float a2) {
            float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            if (y < 1e-15f && y > -1e-15f) y = 0.0f; // anti-denormal flush (modulator is exactly 0 under the gate)
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
        void reset() { x1 = x2 = y1 = y2 = 0.0f; }
    };
    struct VocBand {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        VocBiquad mod1, mod2;
        VocBiquad car1, car2;
        float env = 0.0f;
        float bandGain = 1.0f;
    };
    VocBand  m_voc[kVocBands];
    double   m_vocSawPhase = 0.0;
    double   m_vocSubPhase = 0.0;
    uint32_t m_vocNoise = 0x12345678u;
    float    m_vocAtk = 0.0f, m_vocRel = 0.0f;
    float    m_vocRmsAtk = 0.0f, m_vocRmsRel = 0.0f;   // modulator AGC envelope (5 ms / 180 ms)
    float    m_vocLfC = 0.0f, m_vocHfC = 0.0f;          // 1 kHz / 3.2 kHz one-pole coefficients
    float    m_vocModRms = 0.1f;
    float    m_vocHfEnv = 0.0f, m_vocLfEnv = 0.0f;
    float    m_vocHpState = 0.0f, m_vocLpState = 0.0f;
    float    m_vocSibHpState = 0.0f;
    float    m_vocOutLp = 0.0f;
    float    m_vocLastPitch = -1.0f;
    void     vocRebuild();

    // ---- Reverse delay (50% staggered Hann-windowed reversed grains, read straight from a ring) ----
    // The ring (2 * max grain length) is allocated ONCE in setSampleRate. Every grain starts at the
    // current write position and plays the preceding m_revN samples backwards; its length is
    // latched at its start, so a Time slider change only affects grains that start afterwards:
    // no buffer clearing, no copying, no discontinuity (every grain starts and ends at window 0).
    static constexpr int kRevGrains = 4;
    struct RevGrain { int start = 0; int len = 0; int pos = 0; bool active = false; };
    std::vector<float> m_revCapL, m_revCapR;   // rings
    RevGrain m_revG[kRevGrains];
    int   m_revN = 0;          // active grain length (samples), <= m_revMax
    int   m_revMax = 0;        // 2000 ms at the current sample rate (even)
    int   m_revRing = 0;       // ring size = 2 * m_revMax
    int   m_revW = 0;          // ring write position
    int   m_revLast = -1;      // slot of the most recently started grain (-1: none)
    float m_revLastTimeMs = -1.0f;
    void  revRebuild();

    // ---- Shimmer ----
    std::vector<float> m_shimDlyL, m_shimDlyR;
    int   m_shimWL = 0, m_shimWR = 0;
    float m_shimLpL = 0.0f, m_shimLpR = 0.0f;
    PitchShiftGrain m_shimShiftL, m_shimShiftR;
    std::vector<float> m_shimApL, m_shimApR;
    int   m_shimApWL = 0, m_shimApWR = 0;
};
