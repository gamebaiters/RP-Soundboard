#pragma once
// ShoeboxRoom.h -- Early reflections via the image-source method
// in a rectangular ("shoebox") room.

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

#include "RingBuffer.h"
#include "IIRSmoother.h"

class ShoeboxRoom {
public:
    ShoeboxRoom();
    ~ShoeboxRoom();

    ShoeboxRoom(const ShoeboxRoom&)            = delete;
    ShoeboxRoom& operator=(const ShoeboxRoom&) = delete;

    void init(float sampleRate, int blockSize, const std::string& sofaPath);

    // ------------------------------------------------------------------
    // Parameter setters
    // ------------------------------------------------------------------
    void setRoomSize(float meters);         // 7..50
    void setReflectionLevel(float dB);      // -25..20
    void setRoomType(int type);             // preset index (0..kNumPresets-1)
    void setEnabled(bool on);

    // ------------------------------------------------------------------
    // Process: add reflections into existing stereo buffers (in-place).
    //   leftIO / rightIO   -- stereo ear buffers (summed into)
    //   sourceAzimuthDeg   -- current direct-path azimuth
    //   sourceElevationDeg -- current direct-path elevation
    // ------------------------------------------------------------------
    void process(float* leftIO, float* rightIO, int frames,
                 float sourceAzimuthDeg, float sourceElevationDeg);

    void reset();

    // ------------------------------------------------------------------
    // Preset queries
    // ------------------------------------------------------------------
    static int         numPresets();
    static const char* presetName(int idx);

private:
    float m_sampleRate  = 48000.0f;
    int   m_blockSize   = 0;
    bool  m_enabled     = true;

    // Room dimensions (metres) ------------------------------------------------
    float m_width  = 10.0f;
    float m_height = 3.0f;
    float m_depth  = 10.0f;

    // Wall absorption (6 walls: +X, -X, +Y, -Y, +Z ceiling, -Z floor) --------
    float m_absorption[6] = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f };

    // Active preset character bias. setRoomType copies these from the
    // selected RoomPreset so the per-block computeReflections + late
    // tail can shape the sound beyond what raw absorption can express.
    // Atomic so a GUI-thread setRoomType cannot tear with an audio-
    // thread compute. plain-float reads happen to be tear-free on
    // x86 but UB by the C++ memory model - using atomics removes
    // the data race report and lets the audio thread see consistent
    // preset character across the block.
    std::atomic<float> m_presetLateFeedback{0.55f};
    std::atomic<float> m_presetLateDamp    {0.45f};
    std::atomic<float> m_presetLateMix     {0.30f};
    std::atomic<float> m_presetTapLpHz     {16000.0f};
    std::atomic<float> m_presetErDelayScale{1.0f};

    // DC blocker on the Schroeder tail output. A long-feedback comb
    // chain accumulates DC offset that biases the post-engine limiter
    // and reduces musical headroom - the blocker removes the offset
    // without affecting audible content.
    float m_dcLastInL = 0.0f, m_dcLastInR = 0.0f;
    float m_dcLastOutL = 0.0f, m_dcLastOutR = 0.0f;

    // IIR smoothers for interpolating room dimensions -------------------------
    IIRSmoother m_smoothWidth;
    IIRSmoother m_smoothHeight;
    IIRSmoother m_smoothDepth;

    // Reflection level (linear gain) ------------------------------------------
    float m_reflLevel = 1.0f;

    // One tap per wall --------------------------------------------------------
    static constexpr int kNumWalls = 6;

    struct ReflectionTap {
        RingBuffer buffer;
        float delaySamples  = 0.0f;
        float prevDelaySamples = -1.0f;
        float gain          = 1.0f;
        float azimuthDeg    = 0.0f;
        float elevationDeg  = 0.0f;
        // Per-tap one-pole low-pass simulating frequency-dependent
        // wall absorption. Hard surfaces keep HF; absorbent surfaces
        // cut HF more. Cutoff derived from absorption per block.
        float lpStateL      = 0.0f;
        float lpStateR      = 0.0f;
        float lpCoef        = 0.5f;
        // Per-sample-ramped stereo gains. The pan + tap gain are
        // smoothed across the block instead of step-changing at every
        // block boundary - that step was the rotation-rate clicking
        // the user was hearing on rotating sources.
        float prevGainL     = 0.0f;
        float prevGainR     = 0.0f;
    };
    ReflectionTap m_taps[kNumWalls];

    // ---- Schroeder diffuse late tail ---------------------------------------
    // Four parallel comb filters with damped feedback + two series
    // allpasses. Topology lifted from Freeverb (Jezar) but with the
    // gains/delays tuned for a "spatial room tail" sound, not a generic
    // reverb. Produces the smooth diffuse decay the discrete image-source
    // taps cannot supply - they only give the first 6 echoes.
    static constexpr int kNumCombs    = 4;
    static constexpr int kNumAllpass  = 2;
    struct CombFilter {
        std::vector<float> bufL, bufR;
        int   idx     = 0;
        float dampL   = 0.0f;
        float dampR   = 0.0f;
        float feedback = 0.5f;
        float damp    = 0.2f;  // per-sample HF damping inside the loop
    };
    struct AllPass {
        std::vector<float> bufL, bufR;
        int   idx      = 0;
        float feedback = 0.5f;
    };
    CombFilter m_combs[kNumCombs];
    AllPass    m_allpass[kNumAllpass];
    float      m_lateLevel = 0.6f;   // late-tail send relative to early ER level

    // Scratch buffers ---------------------------------------------------------
    std::vector<float> m_monoScratch;
    std::vector<float> m_leftScratch;
    std::vector<float> m_rightScratch;
    // Per-frame fractional delay ramp fed to RingBuffer::readFractional.
    // Sized to blockSize at init.
    std::vector<float> m_delayRamp;

    // ---- Room presets -------------------------------------------------------
    // Each preset bundles its own absorption AND a full "character"
    // signature so radically different surfaces (Bathroom, Underwater,
    // Outdoor, Cathedral, ...) actually sound radically different. Pure
    // absorption alone could not capture e.g. underwater's LP-heavy
    // muffle or bathroom's bright ring.
    struct RoomPreset {
        const char* name;
        float       absorption[6];
        float       lateFeedback;    // 0..0.95
        float       lateDamp;        // 0..0.85
        float       lateMix;         // 0..1   final wet send
        float       tapLpHz;         // global LP cutoff bias for early refl
        float       erDelayScale;    // multiplier on image-source distance
    };
    static const RoomPreset kPresets[];
    static const int        kNumPresets;

    // ---- Internals ----------------------------------------------------------
    void computeReflections(float srcAz, float srcEl);
    void computeImageSource(int wallIdx, float srcAz, float srcEl,
                            float& outAz, float& outEl, float& outDist);
};
