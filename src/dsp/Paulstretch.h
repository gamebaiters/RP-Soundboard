#pragma once

#include <vector>
#include <complex>
#include <random>
#include <atomic>

// Paulstretch: extreme-quality time-stretch (Paul Nasca, 2006). The
// classic algorithm:
//
//   1. Read a windowed chunk of input PCM (overlapping with previous).
//   2. FFT.
//   3. Randomize the phase of every bin while preserving magnitudes.
//   4. IFFT.
//   5. Apply the analysis window again to the result.
//   6. Overlap-add into the output ring buffer.
//   7. Advance the input read position by (N/2) / stretch_factor; the
//      output write position advances by N/2 each iteration.
//
// Phase randomization is what makes this work. Standard time-stretch
// algorithms (phase vocoder, granular) lock the phase across windows
// to preserve transients - which is what audibly fails on very large
// stretch factors. Paulstretch THROWS AWAY phase coherence: every bin
// gets a fresh random phase per window. The result is an evolving
// frozen-in-amber drone that magically still sounds like the source.
//
// Stereo: L and R channels get INDEPENDENT random phases, which gives
// paulstretch its signature wide diffuse texture.
class Paulstretch {
public:
    Paulstretch();

    void setSampleRate(double sr);

    // Bind an interleaved stereo source. Holding the pointer is OK as
    // long as the audio thread keeps running while it's valid.
    void setSource(const float *interleavedStereo, int frameCount);
    // Update the source frame count without resetting state. Used by
    // SlotDsp's streaming feed where the buffer keeps growing as the
    // decoder produces new samples.
    void updateSourceFrames(int frameCount);

    void setEnabled(bool on)              { m_enabled.store(on); }
    bool isEnabled() const                { return m_enabled.load(); }
    bool hasOutput() const                { return m_outFill > 0 || !m_enabled.load(); }

    // 1.0 = no stretch (just phase-scrambled), grows from there.
    void setStretchFactor(float s);

    // Window length in milliseconds. Bigger = smoother / more "frozen
    // in time" character; smaller = more transient grit. Typical 100-800.
    void setWindowMs(float ms);

    // Restart at the beginning of the source.
    void reset();

    // Seek the input read pointer (the position the next window will be
    // pulled from). Used when the upstream Source toggles paulstretch
    // on/off so playback continues from the current PCM frame instead
    // of jumping back to the start of the file.
    void seekToFrame(int frame);
    // Current input frame (floor of inputPos, modulo source length).
    int  currentFrame() const;

    // True while the source buffer has less data than one FFT window.
    // During priming, synthOneWindow outputs silence.
    bool isPriming() const { return m_srcFrames < m_size; }

    // True once the cumulative source consumption >= source frames.
    // Used to detect end-of-file with paulstretch enabled (the ring
    // drains long before paulstretch finishes reading the source).
    bool hasProcessedAllSource() const;

    // Fill `frames` of stereo output (de-interleaved into outL / outR).
    // Caller has already zeroed the buffers.
    void fillStereo(float *outL, float *outR, int frames);

private:
    void rebuildWindow();
    void synthOneWindow();

    // ---- FFT (radix-2 Cooley-Tukey, in-place) ----
    void fftForward(std::vector<std::complex<float>> &x) const;
    void fftInverse(std::vector<std::complex<float>> &x) const;
    void fftBitReverse(std::vector<std::complex<float>> &x) const;
    void rebuildTwiddles();

    // Largest FFT size we will ever use. Buffers are pre-allocated to
    // this in the constructor so changing the window slider at runtime
    // never reallocates - the audio thread can read from m_window /
    // m_specL / m_outL safely while the GUI thread updates m_size.
    static constexpr int kMaxSize = 65536;

    double m_fs = 48000.0;
    // Atomic: GUI thread mutates, audio thread reads.
    std::atomic<bool>  m_enabled{false};
    int    m_size = 8192;            // FFT size (power of two), <= kMaxSize
    int    m_hop  = 4096;            // = m_size / 2

    std::atomic<float> m_stretchFactor{4.0f};
    double m_inputPos = 0.0;         // floating to support sub-sample stride

    const float *m_src = nullptr;
    int    m_srcFrames = 0;

    // Window function (analysis = synthesis). Paulstretch uses
    // (1 - x^2)^1.25 over x in [-1, +1].
    std::vector<float> m_window;

    // FFT scratch / twiddles
    std::vector<std::complex<float>> m_specL;
    std::vector<std::complex<float>> m_specR;
    std::vector<std::complex<float>> m_twiddles;

    // Output ring (L/R). Size > 2 * window so we always have at least
    // one window of headroom for the audio callback.
    std::vector<float> m_outL;
    std::vector<float> m_outR;
    int   m_outBufLen = 0;
    int   m_outWriteIdx = 0;
    int   m_outReadIdx  = 0;
    int   m_outFill     = 0;

    double m_totalConsumed = 0.0;

    std::mt19937 m_rng;
};
