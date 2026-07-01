#pragma once
// HrtfProcessor.h -- HRTF convolution engine.
// Uses libmysofa for HRIR lookup and SimpleFFT for overlap-add convolution.

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "mysofa.h"
#include "SimpleFFT.h"

class HrtfProcessor {
public:
    HrtfProcessor();
    ~HrtfProcessor();

    // Non-copyable.
    HrtfProcessor(const HrtfProcessor&)            = delete;
    HrtfProcessor& operator=(const HrtfProcessor&) = delete;

    // ------------------------------------------------------------------
    // Initialise with a SOFA file and audio parameters.
    // Returns false on failure (bad file, etc.).
    // ------------------------------------------------------------------
    bool init(const std::string& sofaPath, float sampleRate, int blockSize);

    bool ready() const { return m_sofa != nullptr; }

    // ------------------------------------------------------------------
    // Process one block of audio.
    //   monoIn   -- mono input buffer  (frames samples)
    //   leftOut  -- left-ear output    (frames samples, overwritten)
    //   rightOut -- right-ear output   (frames samples, overwritten)
    //   azimuthDeg   -- source azimuth   (-180..180)
    //   elevationDeg -- source elevation (-90..90)
    // ------------------------------------------------------------------
    void process(const float* monoIn, float* leftOut, float* rightOut,
                 int frames, float azimuthDeg, float elevationDeg);

    // ------------------------------------------------------------------
    int filterLength() const { return m_filterLength; }

    // ------------------------------------------------------------------
    void reset();

private:
    MYSOFA_EASY* m_sofa        = nullptr;
    int          m_filterLength = 0;
    float        m_sampleRate   = 48000.0f;
    int          m_blockSize    = 0;
    int          m_fftSize      = 0;       // next power of 2 >= blockSize + filterLength
    int          m_complexBins  = 0;       // fftSize / 2 + 1

    std::unique_ptr<SimpleFFT> m_fft;

    // Pre-allocated work buffers ---------------------------------------------
    std::vector<float> m_fftInput;       // zero-padded input for FFT  (fftSize)
    std::vector<float> m_fftFreq;        // frequency-domain input     (fftSize+2)

    std::vector<float> m_irLeft;         // current left  HRIR (time, filterLength)
    std::vector<float> m_irRight;        // current right HRIR (time, filterLength)
    std::vector<float> m_irLeftFreq;     // ACTIVE left  HRIR (freq) - STATIC between fades
    std::vector<float> m_irRightFreq;    // ACTIVE right HRIR (freq) - STATIC between fades
    // Pending chain: loaded with the new IR when lookupHRIR fires; runs
    // in parallel with the active chain during the crossfade; promoted
    // to active when the fade completes. This dual-conv design replaces
    // the previous per-block IR lerp - the lerp was a partial fix that
    // mutated the IR every block while overlap-add still summed in a
    // tail computed against the PREVIOUS IR, leaving a small OLA
    // discontinuity at every block boundary (audible as ~187 Hz frying
    // on broadband material). With two parallel chains each holding
    // its own static IR + overlap state, every OLA sum is self-
    // consistent; the crossfade only blends OUTPUT samples, so no
    // discontinuity can appear at a block boundary regardless of how
    // fast direction changes.
    std::vector<float> m_irLeftFreqPending;
    std::vector<float> m_irRightFreqPending;
    std::vector<float> m_irLeftFreqTarget;
    std::vector<float> m_irRightFreqTarget;
    bool               m_irTargetValid = false;

    std::vector<float> m_convLeft;       // left  conv result (freq)   (fftSize+2)
    std::vector<float> m_convRight;      // right conv result (freq)   (fftSize+2)
    std::vector<float> m_convLeftPending;
    std::vector<float> m_convRightPending;

    std::vector<float> m_outLeft;        // left  output (time, fftSize)
    std::vector<float> m_outRight;       // right output (time, fftSize)
    std::vector<float> m_outLeftPending;
    std::vector<float> m_outRightPending;

    std::vector<float> m_overlapLeft;    // OLA overlap (fftSize)
    std::vector<float> m_overlapRight;
    std::vector<float> m_overlapLeftPending;
    std::vector<float> m_overlapRightPending;

    // Output crossfade state. m_fadeAlpha == 1.0 = single-chain steady
    // state (only active chain processes). 0..1 = fade in progress
    // (both chains process; out = (1-a)*active + a*pending; a += inc
    // per output sample; on a >= 1 swap pending->active).
    float m_fadeAlpha    = 1.0f;
    float m_fadeAlphaInc = 0.0f;
    bool  m_fadePending  = false;

    // Cached direction (to avoid redundant HRIR lookups) ---------------------
    float m_lastAzimuth   = -9999.0f;
    float m_lastElevation = -9999.0f;

    // Helpers ----------------------------------------------------------------
    void lookupHRIR(float azDeg, float elDeg);

    static void  complexMultiply(float* dst, const float* a,
                                 const float* b, int complexBins);
    static int   nextPow2(int n);
};
