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
    std::vector<float> m_irLeftFreq;     // left  HRIR in freq domain  (fftSize+2)
    std::vector<float> m_irRightFreq;    // right HRIR in freq domain  (fftSize+2)

    std::vector<float> m_convLeft;       // left  conv result (freq)   (fftSize+2)
    std::vector<float> m_convRight;      // right conv result (freq)   (fftSize+2)

    std::vector<float> m_outLeft;        // left  output (time, fftSize)
    std::vector<float> m_outRight;       // right output (time, fftSize)

    std::vector<float> m_overlapLeft;    // OLA overlap (fftSize)
    std::vector<float> m_overlapRight;

    // Cached direction (to avoid redundant HRIR lookups) ---------------------
    float m_lastAzimuth   = -9999.0f;
    float m_lastElevation = -9999.0f;

    // Helpers ----------------------------------------------------------------
    void lookupHRIR(float azDeg, float elDeg);

    static void  complexMultiply(float* dst, const float* a,
                                 const float* b, int complexBins);
    static int   nextPow2(int n);
};
