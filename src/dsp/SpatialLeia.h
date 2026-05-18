#pragma once

#include "leia/LeiaEngine.h"
#include <atomic>
#include <vector>

// SpatialLeia - per-sample stereo adapter around the block-based Leia
// HRTF engine (measured SOFA convolution + image-source reflections).
//
// The soundboard DSP chain (SlotDsp) runs sample-by-sample, but Leia is
// an FFT overlap-add engine that needs whole blocks. This adapter
// buffers kBlock samples, runs LeiaEngine, then serves the output with
// a matched dry delay so the spatialMix wet/dry crossfade stays phase
// coherent. Added latency = kBlock samples (~5 ms at 48 kHz) - inaudible
// for a soundboard, and the dry path is delayed to match.
//
// Heavy init (SOFA file load + FFT plan) is lazy: ensureInit() must be
// called from the GUI thread before audio uses the instance. If the
// SOFA resource cannot be loaded, ready() stays false and process()
// passes audio through untouched - the caller (SlotDsp) then falls back
// to the Classic engine, so a broken HRTF file can never break audio.
class SpatialLeia
{
public:
    static constexpr int kBlock = 256;

    SpatialLeia();

    // Lazy heavy init. GUI-thread only. Re-inits on sample-rate change.
    // Returns ready().
    bool ensureInit(double sampleRate);
    // Read from the audio thread; written by ensureInit on the GUI
    // thread - atomic so the audio thread never observes a half-built
    // instance (it sees ready() flip true only once every buffer is
    // allocated and the engine calibrated).
    bool ready() const { return m_ready.load(std::memory_order_acquire); }

    void setSampleRate(double sr);
    void reset();

    // Direction: CW-positive azimuth (0 = front, +90 = right ear),
    // elevation in degrees (+ = up).
    void setDirection(float azimuthDeg, float elevationDeg);

    // Reflection / room parameters.
    void setReflections(bool on, float levelDb, float roomSizeM, int roomType);
    void setClarity(float pct);          // 0..100
    void setWidth(float pct);            // 0..100 (reflection amount)

    // Wet/dry crossfade against the untouched stereo input (0..1).
    void setMix(float mix);

    // Per-sample stereo process, in place. Safe to call when !ready()
    // (acts as a passthrough).
    void process(float &l, float &r);

    int  roomTypeCount() const { return m_engine.roomTypeCount(); }
    const char *roomTypeName(int i) const { return m_engine.roomTypeName(i); }

private:
    LeiaEngine m_engine;
    std::atomic<bool> m_ready{false};
    double m_fs    = 0.0;
    float  m_mix   = 1.0f;

    std::vector<float> m_inBuf;   // kBlock*2 interleaved input accumulator
    std::vector<float> m_outBuf;  // kBlock*2 interleaved engine output
    std::vector<float> m_dryBuf;  // kBlock*2 interleaved dry delay ring
    int   m_inFill   = 0;
    int   m_outPos   = 0;
    bool  m_outValid = false;
    int   m_dryPos   = 0;

    // Click-free engagement ramp: dry -> wet mix over kEngageSamples
    // once the first processed block is available.
    float m_engage   = 0.0f;
    float m_engageInc = 0.0f;

    // Measured HRIR datasets sit far below unity gain (no_norm SOFA
    // HRIRs peak ~0.001-0.03). calibrateMakeup() runs a short noise
    // probe through the engine at init and returns the linear makeup
    // gain that restores rough loudness parity with the dry signal, so
    // Leia never ships near-silent regardless of the SOFA file used.
    float calibrateMakeup();
};
