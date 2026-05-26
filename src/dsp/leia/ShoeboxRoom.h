#pragma once
// ShoeboxRoom.h -- Early reflections via the image-source method
// in a rectangular ("shoebox") room.

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
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
        float delaySamples  = 0.0f;   // target delay, recomputed per block
        float prevDelaySamples = -1.0f;  // previous block's delay; <0 = first use
        float gain          = 1.0f;
        float azimuthDeg    = 0.0f;
        float elevationDeg  = 0.0f;
    };
    ReflectionTap m_taps[kNumWalls];

    // Scratch buffers ---------------------------------------------------------
    std::vector<float> m_monoScratch;
    std::vector<float> m_leftScratch;
    std::vector<float> m_rightScratch;
    // Per-frame fractional delay ramp fed to RingBuffer::readFractional.
    // Sized to blockSize at init.
    std::vector<float> m_delayRamp;

    // ---- Room presets -------------------------------------------------------
    struct RoomPreset {
        const char* name;
        float       absorption[6];
    };
    static const RoomPreset kPresets[];
    static const int        kNumPresets;

    // ---- Internals ----------------------------------------------------------
    void computeReflections(float srcAz, float srcEl);
    void computeImageSource(int wallIdx, float srcAz, float srcEl,
                            float& outAz, float& outEl, float& outDist);
};
