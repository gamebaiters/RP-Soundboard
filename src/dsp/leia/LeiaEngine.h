#pragma once
// LeiaEngine.h -- Top-level orchestrator for the Leia spatial audio engine.
// Combines HrtfProcessor (direct binaural path) with ShoeboxRoom (early
// reflections via the image-source method).

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <atomic>
#include <string>
#include <vector>

#include "HrtfProcessor.h"
#include "ShoeboxRoom.h"
#include "GainRamp.h"

class LeiaEngine {
public:
    LeiaEngine();
    ~LeiaEngine();

    LeiaEngine(const LeiaEngine&)            = delete;
    LeiaEngine& operator=(const LeiaEngine&) = delete;

    // ------------------------------------------------------------------
    // Initialise the engine.
    //   sofaPath   -- path to a SOFA-format HRTF file
    //   sampleRate -- audio sample rate (Hz)
    //   blockSize  -- maximum frames per processBlock() call
    // ------------------------------------------------------------------
    bool init(const std::string& sofaPath, float sampleRate, int blockSize);

    // True when both HRTF processors loaded a valid SOFA file.
    bool ready() const { return m_ready; }

    // ------------------------------------------------------------------
    // Process one block: stereo interleaved in --> stereo interleaved out.
    // ------------------------------------------------------------------
    void processBlock(const float* stereoIn, float* stereoOut, int frames);

    // ------------------------------------------------------------------
    // Thread-safe parameter setters (called from any thread).
    // ------------------------------------------------------------------
    void setAzimuth(float deg);           // -180..180
    void setElevation(float deg);         // -90..90
    void setReflectionEnable(bool on);
    void setReflectionLevel(float dB);    // -25..20
    void setRoomSize(float meters);       // 7..50
    void setRoomType(int type);           // preset index
    void setClarity(float pct);           // 0..100
    void setWidth(float pct);             // 0..100
    void setGain(float linearGain);       // master output gain

    void setSampleRate(double sr);
    void reset();

    // ------------------------------------------------------------------
    // Thread-safe parameter readers.
    // ------------------------------------------------------------------
    float azimuth()   const { return m_azimuth.load(std::memory_order_relaxed); }
    float elevation() const { return m_elevation.load(std::memory_order_relaxed); }

    // ------------------------------------------------------------------
    // Room preset queries (static data -- always safe to call).
    // ------------------------------------------------------------------
    int         roomTypeCount() const;
    const char* roomTypeName(int idx) const;

private:
    bool  m_ready      = false;
    float m_sampleRate = 48000.0f;
    int   m_blockSize  = 0;

    // Parameters (atomic for lock-free GUI <-> audio thread comms) ---------
    std::atomic<float> m_azimuth   { 0.0f };
    std::atomic<float> m_elevation { 0.0f };
    std::atomic<bool>  m_reflEnable{ true };
    std::atomic<float> m_reflLevel { -6.0f };
    std::atomic<float> m_roomSize  { 10.0f };
    std::atomic<int>   m_roomType  { 0 };
    std::atomic<float> m_clarity   { 100.0f };
    std::atomic<float> m_width     { 100.0f };

    // DSP components -------------------------------------------------------
    HrtfProcessor m_hrtfLeft;    // processes left input channel
    HrtfProcessor m_hrtfRight;   // processes right input channel
    ShoeboxRoom   m_room;
    GainRamp      m_gainRamp;

    // Post-engine soft peak limiter state. Keeps the wet stereo
    // bounded below the outer SlotDsp::softLimit's hard cap so hot
    // reflection / clarity / width combos can't drive it into clipping.
    float m_postLimGain = 1.0f;

    // DC blocker state applied post-HRTF sum + reflection mix. The
    // Schroeder tail has its own blocker inside ShoeboxRoom; this one
    // catches the residual DC that leaks through the HRIR path itself
    // when the mysofa dataset contains small non-zero mean coefficients
    // (common for measured HRIR sets). Left alone that bias asymmetrically
    // clamps bass peaks in the memoryless soft saturator below, producing
    // the "physical limiter on bass" symptom the user reported after the
    // v2.2.16 fixes.
    float m_dcLastInL  = 0.0f;
    float m_dcLastInR  = 0.0f;
    float m_dcLastOutL = 0.0f;
    float m_dcLastOutR = 0.0f;

    // Scratch buffers (allocated once in init, reused every block) ---------
    std::vector<float> m_monoL, m_monoR;           // deinterleaved input
    std::vector<float> m_hrtfOutLL, m_hrtfOutLR;   // L chan -> L ear, R ear
    std::vector<float> m_hrtfOutRL, m_hrtfOutRR;   // R chan -> L ear, R ear
    std::vector<float> m_mixL, m_mixR;             // mixed output

    // Internal: process one chunk of at most m_blockSize frames ---------------
    void processChunk(const float* stereoIn, float* stereoOut, int frames);

    // FPU state management (flush denormals) --------------------------------
    unsigned int m_savedMXCSR = 0;
    void setFTZDAZ();
    void restoreFPU();
};
