#pragma once

#include "SandboxState.h"
#include "Positional.h"
#include "SpatialLeia.h"
#include "EqRack.h"
#include "Paulstretch.h"
#include "Reverb.h"
#include "Compressor.h"
#include "Saturator.h"
#include "Chorus.h"
#include "Flanger.h"
#include "Flangus.h"
#include "Phaser.h"
#include "Delay.h"
#include "Limiter.h"
#include "Bitcrusher.h"
#include "GenerationLoss.h"


#include <vector>
#include <atomic>
#include <cstdint>

// Per-slot DSP block. Lazy-allocated by the Sampler when a channel
// first turns on its sandbox toggle so slots without sandbox keep zero
// memory overhead.
//
// Pipeline (stereo in, stereo out, in-place on a short buffer):
//   short -> float
//   EQ     (16 bands ISO 2/3 oct, Q=2.145, -12..+12 dB)
//   Spatial (Off / L-R Pan / 3D HRTF dual virtual speaker / 8D preset)
//   soft limiter (tanh past +/-0.85)
//   float -> short (clamped to int16)
//
// Paulstretch lives on a separate input feed: when stretchEnabled the
// Sampler pre-feeds decoded float frames via feedStretchFloat() and
// pulls processed output via produceStretchedShort(); the regular
// process() chain runs on top of that.
class SlotDsp
{
public:
    SlotDsp();

    void setSampleRate(double sr);
    void reset();
    // Same as reset() but preserves the rotation phase on both paths.
    // Used by the reverse-toggle swap path so a 3D Rotate / 8D source
    // doesn't snap back to the front-azimuth start the moment the
    // user flips direction mid-playback.
    void resetPreservingRotation();
    void applyState(const SandboxState &s);

    // GUI-thread-only Leia bring-up. Calls SpatialLeia::ensureInit on
    // both playback + capture paths. The first call hits disk (SOFA
    // load), builds the FFT plan, and runs a noise probe for the
    // makeup-gain calibration - typically ~1 second the FIRST time
    // the user picks Leia. After that it is a cached early-return.
    //
    // It must be called OUTSIDE the Sampler's audio mutex - the heavy
    // work used to run inside applyState() while m_mutex was held, so
    // engine swap froze both the audio callback and the GUI thread for
    // the whole init duration. Doing it here first means applyState's
    // own ensureInit call inside hot DSP setup degenerates to a no-op.
    void prepareLeia(double fs);

    bool isActive() const { return m_active; }
    const SandboxState &state() const { return m_state; }

    // In-place transform of an interleaved short PCM block. `channels`
    // is what the host wants written (1 or 2). The DSP itself works
    // entirely in stereo float internally; mono output sums L+R after
    // processing. Returns the peak |L|, |R| in 0..1 for the meter.
    // isCapture selects per-path DSP state.
    void process(short *interleaved, int frames, int channels,
                 float &peakL, float &peakR, bool isCapture);

    // ---- Paulstretch (streaming, dual-path) ----
    bool isStretchEnabled() const { return m_state.enabled && m_state.stretchEnabled; }
    // Frames the host should consume from the source ring per output
    // window. = framesOut when stretch is off, ceil(framesOut/factor)
    // otherwise. During priming, returns outputFrames to fill the
    // source buffer at real-time rate.
    int  inputFramesNeededFor(int outputFrames) const;
    // Current paulstretch read position in seconds (playback path).
    double stretchPlaybackPosition() const;
    // True when the capture-side paulstretch has read through all
    // fed source frames at least once.
    bool stretchCaptureDone() const;
    // Push N decoded short stereo frames into one of the paulstretch
    // feed rings. isCapture selects which side: false = playback ring
    // (local), true = capture ring (server).
    void feedStretchShort(const short *interleaved, int frames, bool isCapture);
    // Produce N output frames into a stereo short buffer using the
    // appropriate paulstretch instance, then run EQ + Spatial + soft
    // limiter on top. Peaks reported only for the capture path so the
    // meter always reflects what the server hears.
    void produceStretchedShort(short *out, int frames, int channels,
                               float &peakL, float &peakR, bool isCapture);
    void resetPeak();

    // FxPanel reverb (per-button "wet" knob in the existing custom-FX
    // strip) is now applied at the END of the per-slot DSP chain so
    // the user gets a "room reverb" effect after spatial / EQ. Sampler
    // calls this whenever the slot's FxPanel reverb slider changes;
    // the value is summed with the sandbox Ambience wet (clamped 0..1)
    // and pushed to the per-path Reverb stage.
    void setFxReverbWet(float wet);

    // Rolling DSP CPU percent. Audio thread accumulates per-block
    // wall-clock time in process()/produceStretchedShort(); GUI thread
    // reads the rolling value via these getters. cpuPercent() returns
    // % of real-time spent in the chain over the last ~500 ms window;
    // it self-resets after read so the next poll covers the next
    // window. Cheap to maintain (one steady_clock per block).
    double cpuPercent();

    // Read the playback path's EQ band levels (0..1) into out[16].
    // Lock-free; the audio thread updates atomics from runFftAnalysis.
    void getEqBandLevels(float out[16]) const;

private:
    // Per-path DSP state. Capture path (server-bound) and playback
    // path (local-bound) keep INDEPENDENT EQ + Positional + Reverb
    // state. Without this, both paths fed the same audio through one
    // shared stateful filter twice per audio cycle - filter state
    // advanced at 2x rate, capture and playback got divergent output,
    // and toggling "mute on my client" flipped between 1x and 2x state
    // advancement, producing the robotic transient the user heard.
    struct PathState {
        EqRack     eq;
        Compressor comp;
        Saturator  sat;
        Positional posL;
        Positional posR;
        SpatialLeia leia;        // measured-HRTF engine (alt to posL/posR)
        Chorus     chorus;
        Flanger    flanger;
        Flangus    flangus;
        Phaser     phaser;
        Delay      delay;
        Reverb     reverb;
        Limiter    limiter;
        Bitcrusher     bitcrusher;
        GenerationLoss genLoss;
        double     rotPhase = 0.0;
        int        rotBlockCounter = 0;
    };

    void pushSpeakerPair(PathState &p, float cx, float cy, float cz);
    void advanceRotationIfNeeded(PathState &p);
    void updateLeiaDirection(PathState &p);     // control-rate az/el feed
    void applyStage(int stage, PathState &p, float &l, float &r);

    double m_fs = 0.0;
    bool   m_active = false;
    SandboxState m_state;

    PathState m_play;
    PathState m_cap;

    // Rolling CPU measurement. m_cpuNs accumulates wall-clock ns spent
    // in process/produceStretchedShort. m_cpuFrames counts the frames
    // those calls processed. cpuPercent() converts to percent of real
    // time and resets both atomics. Atomic load + clear so the GUI
    // poll is lock-free against the audio thread.
    std::atomic<int64_t> m_cpuNs    {0};
    std::atomic<int64_t> m_cpuFrames{0};

    // Per-slot streaming paulstretch state. We keep TWO independent
    // instances - one fed by the playback path (local listener), one
    // fed by the capture path (server). Sharing a single instance
    // sounded great in theory but breaks in practice: both paths
    // consume their own sb ring at slow needIn rate, which would
    // double-feed a shared stretchBuf and turn the audio into noise
    // every other window. With two instances each path stays in sync
    // with its own decoder ring; phase-randomisation differs slightly
    // between the two outputs but neither listener notices.
    struct StretchState {
        Paulstretch        ps;
        std::vector<float> buf;
        int                write = 0;
        bool               inited = false;
    };
    StretchState m_stretchPlay;
    StretchState m_stretchCap;
    void initStretchStateIfNeeded(StretchState &s);

    static constexpr int kRotateUpdateBlock = 32;

    float  m_peakL = 0.0f;
    float  m_peakR = 0.0f;
    float  m_peakDecay = 0.999f;     // recomputed from sample rate

    float  m_fxReverbWet = 0.0f;     // FxPanel reverb routed here
    void   refreshReverbWet();
    void   recomputeActive();
};
