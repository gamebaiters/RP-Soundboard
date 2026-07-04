#pragma once

#include <QString>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

class SimpleFFT;

// Convolution reverb (Q6) - the optional alternative engine of the
// sandbox Reverb stage (reverbConvMode == 1).
//
// Uniform partitioned convolution: the IR is chopped into 256-sample
// partitions, each pre-transformed to the frequency domain; per input
// block one FFT + P complex multiply-accumulates + one IFFT produce
// the wet signal with a fixed 256-sample (~5 ms) latency that reads
// as natural pre-delay. IRs up to 2 s (375 partitions) are supported.
//
// IR sources:
//   - 4 procedural presets (Hall / Church / Room / Spring) synthesized
//     at prepare() time from shaped exponentially-decaying noise -
//     zero assets shipped;
//   - any audio file the user picks (decoded via the existing FFmpeg
//     input path, truncated to 2 s).
//
// prepare() is heavy (IR synthesis/decode + partition FFTs, tens of
// ms) and must be called OFF the audio thread - same contract as
// SlotDsp::prepareLeia. The audio thread try-locks per block; while a
// rebuild holds the lock the wet path is simply skipped, so audio
// never blocks or glitches.
class ConvolutionReverb {
public:
    ConvolutionReverb();
    ~ConvolutionReverb();

    void setSampleRate(double sr);

    // Build (or rebuild) the IR partitions. preset 0=Hall 1=Church
    // 2=Room 3=Spring; non-empty irPath overrides the preset with a
    // decoded audio file. No-op when the requested IR is already
    // loaded. GUI/worker thread only.
    void prepare(int preset, const QString &irPath);

    void setWet(float w);
    bool ready() const { return m_ready.load(std::memory_order_acquire); }

    // Per-sample. Adds wet convolution output on top of the dry
    // signal (dry always passes at unity).
    void process(float &l, float &r);

    // Clear streaming state (input history, tails). Keeps the IR.
    void reset();

private:
    static constexpr int kBlock = 256;
    static constexpr int kFft   = 512;
    static constexpr int kBins  = kFft / 2 + 1;
    static constexpr double kMaxIrSeconds = 2.0;

    void buildPartitions(const std::vector<float> &irL,
                         const std::vector<float> &irR);
    void synthesizeIr(int preset, std::vector<float> &outL,
                      std::vector<float> &outR) const;
    bool loadIrFile(const QString &path, std::vector<float> &outL,
                    std::vector<float> &outR) const;
    // Scale the IR so the peak of its transfer function |H(f)| hits a
    // fixed sub-unity target: no input frequency can ever be amplified
    // by the wet path (the old energy normalization left 3..6x
    // resonant peaks = constant softLimit saturation).
    void normalizeIr(std::vector<float> &outL,
                     std::vector<float> &outR) const;
    void processBlockLocked();

    double m_fs = 48000.0;
    std::atomic<float> m_wet{0.0f};
    std::atomic<bool>  m_ready{false};

    // IR + streaming state, guarded by m_lock (audio side try-locks).
    std::mutex m_lock;
    std::unique_ptr<SimpleFFT> m_fft;
    std::vector<std::vector<float>> m_irFreqL, m_irFreqR;  // P x 2*kBins
    std::vector<std::vector<float>> m_histL, m_histR;      // P x 2*kBins
    int m_histIdx = 0;
    std::vector<float> m_tailL, m_tailR;                   // kBlock overlap
    std::vector<float> m_accum, m_time;                    // scratch

    // Per-sample staging (audio thread only).
    float m_inL[kBlock] = {}, m_inR[kBlock] = {};
    float m_outL[kBlock] = {}, m_outR[kBlock] = {};
    int   m_pos = 0;

    // Identity of the currently loaded IR.
    int     m_loadedPreset = -1;
    QString m_loadedPath;
};
