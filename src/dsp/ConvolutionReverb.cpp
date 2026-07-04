#include "ConvolutionReverb.h"
#include "leia/SimpleFFT.h"
#include "../inputfile.h"
#include "../SampleProducer.h"

#include <QByteArray>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

// Collects decoded shorts from an InputFile into a float vector.
struct CollectProducer : public SampleProducer {
    std::vector<float> data;   // interleaved stereo
    size_t maxSamples;         // cap (interleaved count)
    explicit CollectProducer(size_t cap) : maxSamples(cap) { data.reserve(cap); }
    void produce(const short *samples, int count) override {
        constexpr float kInv = 1.0f / 32768.0f;
        for (int i = 0; i < count * 2 && data.size() < maxSamples; ++i)
            data.push_back(samples[i] * kInv);
    }
};

uint32_t rngNext(uint32_t &s) {
    s = s * 1664525u + 1013904223u;
    return s;
}
float rngFloat(uint32_t &s) {   // -1..1
    return (static_cast<float>(rngNext(s) >> 9) / 4194304.0f) - 1.0f;
}

} // namespace

ConvolutionReverb::ConvolutionReverb() {
    m_fft.reset(new SimpleFFT(kFft));
    m_tailL.assign(kBlock, 0.0f);
    m_tailR.assign(kBlock, 0.0f);
    m_accum.assign(2 * kBins, 0.0f);
    m_time.assign(kFft, 0.0f);
}

ConvolutionReverb::~ConvolutionReverb() = default;

void ConvolutionReverb::setSampleRate(double sr) {
    double fs = (sr > 0) ? sr : 48000.0;
    if (std::fabs(fs - m_fs) < 0.5) return;
    m_fs = fs;
    // Force IR rebuild at the new rate on the next prepare().
    std::lock_guard<std::mutex> g(m_lock);
    m_ready.store(false, std::memory_order_release);
    m_loadedPreset = -1;
    m_loadedPath.clear();
}

void ConvolutionReverb::setWet(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_wet.store(w, std::memory_order_relaxed);
}

void ConvolutionReverb::prepare(int preset, const QString &irPath) {
    if (preset < 0) preset = 0;
    if (preset > 3) preset = 3;
    if (m_ready.load(std::memory_order_acquire) &&
        preset == m_loadedPreset && irPath == m_loadedPath)
        return;   // already loaded

    std::vector<float> irL, irR;
    bool haveFile = false;
    if (!irPath.isEmpty())
        haveFile = loadIrFile(irPath, irL, irR);
    if (!haveFile)
        synthesizeIr(preset, irL, irR);

    std::lock_guard<std::mutex> g(m_lock);
    m_ready.store(false, std::memory_order_release);
    buildPartitions(irL, irR);
    m_loadedPreset = preset;
    m_loadedPath   = haveFile ? irPath : QString();
    m_ready.store(true, std::memory_order_release);
}

bool ConvolutionReverb::loadIrFile(const QString &path,
                                   std::vector<float> &outL,
                                   std::vector<float> &outR) const {
    InputFileOptions opt;
    opt.outputChannelLayout = InputFileOptions::STEREO;
    opt.outputSampleRate = static_cast<int>(m_fs);
    InputFile *f = CreateInputFileFFmpeg(opt);
    if (!f) return false;

    QByteArray utf8 = path.toUtf8();
    bool ok = false;
    try {
        if (f->open(utf8.constData()) == 0) {
            size_t cap = static_cast<size_t>(kMaxIrSeconds * m_fs) * 2;
            CollectProducer collector(cap);
            while (!f->done() && collector.data.size() < cap) {
                if (f->readSamples(&collector) <= 0) break;
            }
            f->close();
            size_t frames = collector.data.size() / 2;
            if (frames >= 128) {
                outL.resize(frames);
                outR.resize(frames);
                for (size_t i = 0; i < frames; ++i) {
                    outL[i] = collector.data[i * 2 + 0];
                    outR[i] = collector.data[i * 2 + 1];
                }
                ok = true;
            }
        }
    } catch (...) {
        ok = false;
    }
    delete f;

    if (ok)
        normalizeIr(outL, outR);
    return ok;
}

void ConvolutionReverb::normalizeIr(std::vector<float> &outL,
                                    std::vector<float> &outR) const {
    // Two-constraint normalization:
    //  - ENERGY target: sets the PERCEIVED wet level, comparable to
    //    the algorithmic engine. (A pure peak-|H| normalization was
    //    tried: the peak of a long ragged tail spectrum sits far above
    //    its mean, so scaling by it crushed the average gain and the
    //    engine became barely audible.)
    //  - PEAK cap on max|H(f)|: no input frequency may be amplified
    //    past kPeakCap, whatever the IR. This is what actually caused
    //    the distortion report - resonant 3..6x spectral peaks (spring
    //    comb, user IRs) drove sustained tones into permanent
    //    softLimit saturation. The smaller of the two gains wins:
    //    smooth tails get the full energy level, spiky IRs get tamed.
    const size_t n = std::min(outL.size(), outR.size());
    if (n == 0) return;

    double e = 0.0;
    for (size_t i = 0; i < n; ++i)
        e += 0.5 * (outL[i] * outL[i] + outR[i] * outR[i]);
    double gEnergy = (e > 1e-12) ? 0.35 / std::sqrt(e) : 1.0;

    int N = 1024;
    while (static_cast<size_t>(N) < n) N <<= 1;
    SimpleFFT fft(N);
    std::vector<float> time(N, 0.0f);
    std::vector<float> freq(N + 2, 0.0f);
    double maxMag = 0.0;
    for (int ch = 0; ch < 2; ++ch) {
        const std::vector<float> &src = ch ? outR : outL;
        std::fill(time.begin(), time.end(), 0.0f);
        std::copy(src.begin(), src.begin() + n, time.begin());
        fft.forward(time.data(), freq.data());
        const int bins = N / 2 + 1;
        for (int k = 0; k < bins; ++k) {
            double re = freq[2 * k], im = freq[2 * k + 1];
            double m = std::sqrt(re * re + im * im);
            if (m > maxMag) maxMag = m;
        }
    }
    constexpr double kPeakCap = 1.4;
    double gPeak = (maxMag > 1e-9) ? kPeakCap / maxMag : gEnergy;

    float g = static_cast<float>(std::min(gEnergy, gPeak));
    for (auto &v : outL) v *= g;
    for (auto &v : outR) v *= g;
}

void ConvolutionReverb::synthesizeIr(int preset, std::vector<float> &outL,
                                     std::vector<float> &outR) const {
    // Per-preset recipe. The presets used to differ only in decay time
    // and damping - four flavours of the same noise tail, near-
    // indistinguishable in a mix. Each one now has a signature:
    //   Hall   - long smooth tail, warm damping, wide stereo.
    //   Church - huge dark stone space: very slow decay, heavy HF
    //            loss, long pre-delay, sparse late slap reflections.
    //   Room   - tiny bright box: short decay, near-zero pre-delay,
    //            dense early reflections, NARROW stereo.
    //   Spring - dispersive chirp train ("boing"): repeating downward
    //            chirps replace the noise tail entirely, mono-ish.
    double lenS = 2.0, t60 = 1.9;
    double fcStart = 7500.0, fcEnd = 1600.0;
    double preDelayMs = 22.0;
    int    erCount = 5;  double erWindowMs = 50.0; float erGain = 0.5f;
    float  width = 1.0f;         // 1 = decorrelated L/R, 0 = mono
    bool   springMode = false;
    switch (preset) {
    case 1:   // Church
        lenS = 2.0; t60 = 3.4; fcStart = 4500.0; fcEnd = 500.0;
        preDelayMs = 48.0; erCount = 3; erWindowMs = 90.0; erGain = 0.7f;
        width = 1.0f;
        break;
    case 2:   // Room
        lenS = 0.5; t60 = 0.32; fcStart = 10000.0; fcEnd = 3800.0;
        preDelayMs = 4.0; erCount = 12; erWindowMs = 22.0; erGain = 0.8f;
        width = 0.35f;
        break;
    case 3:   // Spring
        lenS = 1.4; t60 = 1.2; fcStart = 3200.0; fcEnd = 900.0;
        preDelayMs = 10.0; erCount = 0;
        width = 0.15f;
        springMode = true;
        break;
    default:  // Hall
        break;
    }

    const int n = static_cast<int>(std::min(kMaxIrSeconds, lenS) * m_fs);
    const int pre = static_cast<int>(preDelayMs * 0.001 * m_fs);
    outL.assign(n, 0.0f);
    outR.assign(n, 0.0f);

    auto synthChannel = [&](std::vector<float> &out, uint32_t seed) {
        uint32_t s = seed;
        float lp = 0.0f;
        const double decayK = 6.907755 / (t60 * m_fs);   // ln(1000)/T60
        // Spring: the tail is a decaying train of downward chirps -
        // the dispersive "boing" of a real spring tank (high
        // frequencies travel slower through the coil, so every echo
        // smears into a falling sweep). A weak noise floor sits under
        // it; everything else (noise-only tail) never reads as spring.
        const float noiseLevel = springMode ? 0.22f : 1.0f;
        for (int i = pre; i < n; ++i) {
            double t01 = static_cast<double>(i - pre) / std::max(1, n - pre);
            // Time-varying lowpass: tail gets darker as it decays.
            double fc = fcStart * std::pow(fcEnd / fcStart, t01);
            float c = static_cast<float>(
                1.0 - std::exp(-6.28318530717958647692 * fc / m_fs));
            float noise = rngFloat(s);
            lp += c * (noise - lp);
            float env = static_cast<float>(std::exp(-decayK * (i - pre)));
            out[i] = lp * env * noiseLevel;
        }
        if (springMode) {
            const double gapS   = 0.042;   // echo spacing in the coil
            const double chirpS = 0.030;   // one dispersive sweep
            const int chirpN = static_cast<int>(chirpS * m_fs);
            for (int p = 0; ; ++p) {
                int base = pre + static_cast<int>(p * gapS * m_fs);
                if (base >= n) break;
                float amp = static_cast<float>(
                    std::exp(-decayK * (base - pre))) * 0.85f;
                if (amp < 0.003f) break;
                double phase = 0.0;
                for (int i = 0; i < chirpN && base + i < n; ++i) {
                    double frac = static_cast<double>(i) / chirpN;
                    // Instantaneous frequency sweeps 3.5 kHz -> 300 Hz.
                    double f = 3500.0 * std::pow(300.0 / 3500.0, frac);
                    phase += 6.28318530717958647692 * f / m_fs;
                    float fade = static_cast<float>(1.0 - frac);
                    out[base + i] += amp * fade *
                        static_cast<float>(std::sin(phase));
                }
            }
        }
        // Early reflections: count / window / strength are per-preset
        // (dense+tight = small room, sparse+late = stone church).
        int erEnd = std::min(n, pre + static_cast<int>(
                                        erWindowMs * 0.001 * m_fs));
        for (int k = 0; k < erCount; ++k) {
            int idx = pre + static_cast<int>((rngNext(s) % 2048) / 2048.0
                                             * std::max(1, erEnd - pre));
            if (idx < n) out[idx] += erGain * rngFloat(s);
        }
    };
    synthChannel(outL, 0xC0FFEE01u + preset * 7919u);
    synthChannel(outR, 0xBADC0DE5u + preset * 104729u);

    // Stereo width: mid/side blend of the two decorrelated channels.
    // Room collapses toward mono (a small box has little envelopment),
    // Spring is nearly mono (one physical tank), halls stay wide.
    if (width < 0.999f) {
        for (int i = 0; i < n; ++i) {
            float m = 0.5f * (outL[i] + outR[i]);
            float sd = 0.5f * (outL[i] - outR[i]) * width;
            outL[i] = m + sd;
            outR[i] = m - sd;
        }
    }

    normalizeIr(outL, outR);
}

void ConvolutionReverb::buildPartitions(const std::vector<float> &irL,
                                        const std::vector<float> &irR) {
    const int n = static_cast<int>(std::min(irL.size(), irR.size()));
    const int P = std::max(1, (n + kBlock - 1) / kBlock);

    m_irFreqL.assign(P, std::vector<float>(2 * kBins, 0.0f));
    m_irFreqR.assign(P, std::vector<float>(2 * kBins, 0.0f));
    m_histL.assign(P, std::vector<float>(2 * kBins, 0.0f));
    m_histR.assign(P, std::vector<float>(2 * kBins, 0.0f));
    m_histIdx = 0;

    std::vector<float> chunk(kFft, 0.0f);
    for (int p = 0; p < P; ++p) {
        std::fill(chunk.begin(), chunk.end(), 0.0f);
        int base = p * kBlock;
        int len = std::min(kBlock, n - base);
        for (int i = 0; i < len; ++i) chunk[i] = irL[base + i];
        m_fft->forward(chunk.data(), m_irFreqL[p].data());
        std::fill(chunk.begin(), chunk.end(), 0.0f);
        for (int i = 0; i < len; ++i) chunk[i] = irR[base + i];
        m_fft->forward(chunk.data(), m_irFreqR[p].data());
    }

    std::fill(m_tailL.begin(), m_tailL.end(), 0.0f);
    std::fill(m_tailR.begin(), m_tailR.end(), 0.0f);
    std::memset(m_inL, 0, sizeof(m_inL));
    std::memset(m_inR, 0, sizeof(m_inR));
    std::memset(m_outL, 0, sizeof(m_outL));
    std::memset(m_outR, 0, sizeof(m_outR));
    m_pos = 0;
}

void ConvolutionReverb::processBlockLocked() {
    const int P = static_cast<int>(m_irFreqL.size());
    if (P == 0) return;

    auto convolveChannel = [this, P](const float *inBlock,
                                     std::vector<std::vector<float>> &hist,
                                     const std::vector<std::vector<float>> &irF,
                                     std::vector<float> &tail,
                                     float *outBlock) {
        // FFT of the zero-padded input block into the history ring.
        std::fill(m_time.begin(), m_time.end(), 0.0f);
        std::memcpy(m_time.data(), inBlock, kBlock * sizeof(float));
        m_fft->forward(m_time.data(), hist[m_histIdx].data());

        // Frequency-domain multiply-accumulate over all partitions.
        std::fill(m_accum.begin(), m_accum.end(), 0.0f);
        for (int p = 0; p < P; ++p) {
            const float *X = hist[(m_histIdx - p + P) % P].data();
            const float *H = irF[p].data();
            float *Y = m_accum.data();
            for (int k = 0; k < kBins; ++k) {
                float xr = X[2 * k], xi = X[2 * k + 1];
                float hr = H[2 * k], hi = H[2 * k + 1];
                Y[2 * k]     += xr * hr - xi * hi;
                Y[2 * k + 1] += xr * hi + xi * hr;
            }
        }

        m_fft->inverse(m_accum.data(), m_time.data());
        const float invN = 1.0f / static_cast<float>(kFft);
        for (int i = 0; i < kBlock; ++i) {
            outBlock[i] = m_time[i] * invN + tail[i];
            tail[i] = m_time[kBlock + i] * invN;
        }
    };

    convolveChannel(m_inL, m_histL, m_irFreqL, m_tailL, m_outL);
    convolveChannel(m_inR, m_histR, m_irFreqR, m_tailR, m_outR);
    m_histIdx = (m_histIdx + 1) % P;
}

void ConvolutionReverb::process(float &l, float &r) {
    if (!m_ready.load(std::memory_order_acquire)) return;
    float wet = m_wet.load(std::memory_order_relaxed);

    // Read the wet sample computed one block ago, stage the input.
    float wl = m_outL[m_pos];
    float wr = m_outR[m_pos];
    m_inL[m_pos] = l;
    m_inR[m_pos] = r;

    if (++m_pos >= kBlock) {
        m_pos = 0;
        // Never block the audio thread: if a rebuild holds the lock,
        // emit silence for this wet block instead of waiting.
        if (m_lock.try_lock()) {
            processBlockLocked();
            m_lock.unlock();
        } else {
            std::memset(m_outL, 0, sizeof(m_outL));
            std::memset(m_outR, 0, sizeof(m_outR));
        }
    }

    l += wet * wl;
    r += wet * wr;
}

void ConvolutionReverb::reset() {
    if (!m_lock.try_lock()) return;   // rebuild in progress - skip
    for (auto &v : m_histL) std::fill(v.begin(), v.end(), 0.0f);
    for (auto &v : m_histR) std::fill(v.begin(), v.end(), 0.0f);
    std::fill(m_tailL.begin(), m_tailL.end(), 0.0f);
    std::fill(m_tailR.begin(), m_tailR.end(), 0.0f);
    std::memset(m_inL, 0, sizeof(m_inL));
    std::memset(m_inR, 0, sizeof(m_inR));
    std::memset(m_outL, 0, sizeof(m_outL));
    std::memset(m_outR, 0, sizeof(m_outR));
    m_pos = 0;
    m_lock.unlock();
}
