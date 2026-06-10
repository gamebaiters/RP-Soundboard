#include "EqRack.h"
#include <cmath>
#include <algorithm>

namespace {
// 16-band ISO-aligned 2/3-octave centers, plus a 20 Hz extension so the
// rack covers the full audible range. The rest is the standard ISO R10
// preferred-numbers list (25, 40, 63, ... 16k).
constexpr double kFreq[EqRack::kNumBands] = {
       20.0,    25.0,    40.0,    63.0,
      100.0,   160.0,   250.0,   400.0,
      630.0,  1000.0,  1600.0,  2500.0,
     4000.0,  6300.0, 10000.0, 16000.0
};
}

EqRack::EqRack() {
    for (int i = 0; i < kNumBands; ++i) recompute(i);
    // FFT analysis buffers. Hann window precomputed; ring zeroed.
    m_fftRing.assign(kFftSize, 0.0f);
    m_fftWin .assign(kFftSize, 0.0f);
    m_fftFreq.assign(kFftSize + 2, 0.0f);
    for (int i = 0; i < kFftSize; ++i) {
        float t = float(i) / float(kFftSize - 1);
        m_fftWin[i] = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * t);
    }
    m_fft.reset(new SimpleFFT(kFftSize));
    for (int i = 0; i < kNumBands; ++i)
        m_bandLevel[i].store(0.0f, std::memory_order_relaxed);
}

float EqRack::bandLevel(int b) const {
    if (b < 0 || b >= kNumBands) return 0.0f;
    return m_bandLevel[b].load(std::memory_order_relaxed);
}

void EqRack::runFftAnalysis() {
    static thread_local std::vector<float> buf;
    if ((int)buf.size() < kFftSize) buf.assign(kFftSize, 0.0f);
    // Linearise the ring starting from the oldest sample, applying
    // the Hann window in one pass.
    for (int i = 0; i < kFftSize; ++i) {
        int idx = (m_fftWrite + i) % kFftSize;
        buf[i] = m_fftRing[idx] * m_fftWin[i];
    }
    m_fft->forward(buf.data(), m_fftFreq.data());
    const int    bins   = kFftSize / 2 + 1;
    const double binF   = m_sampleRate / double(kFftSize);
    if (binF <= 0.0) return;
    for (int b = 0; b < kNumBands; ++b) {
        double fc = bandFrequency(b);
        // 2/3-octave window (~factor 1.26) clamped to neighbouring bins.
        int binLo = int(fc * 0.79 / binF);
        int binHi = int(fc * 1.26 / binF);
        if (binLo < 1)     binLo = 1;
        if (binHi >= bins) binHi = bins - 1;
        if (binHi < binLo) binHi = binLo;
        // PEAK bin magnitude across the 2/3-octave window, NOT the
        // mean. High bands (16 k, 8 k) span 150+ FFT bins at our
        // sample rate / FFT size; even a full-scale sine at the band
        // centre only lights 1-3 of those, so the mean is ~peak/100
        // and the meter never saturated (the user-reported bug
        // "earrape never reaches max"). Peak keeps the band-shape
        // response sharp regardless of how many bins the window
        // covers.
        double peakMag = 0.0;
        for (int k = binLo; k <= binHi; ++k) {
            float re = m_fftFreq[k * 2];
            float im = m_fftFreq[k * 2 + 1];
            double mag = std::sqrt(double(re) * re + double(im) * im);
            if (mag > peakMag) peakMag = mag;
        }
        // Map peak magnitude to [0..1]. Full-scale sine through a Hann
        // window of length kFftSize produces a peak bin magnitude of
        // ~ kFftSize / 4 (coherent gain 0.5 then magnitude-of-complex).
        // /160 keeps the meter saturating slightly BEFORE 0 dBFS so
        // realistic loud material (RMS ~ -6 dBFS, peak ~ 0 dBFS) hits
        // the top of the strip cleanly.
        float level = float(peakMag / 160.0);
        if (level > 1.0f) level = 1.0f;
        if (level < 0.0f) level = 0.0f;
        float prev = m_bandLevel[b].load(std::memory_order_relaxed);
        float smoothed = (level > prev)
            ? prev + 0.55f * (level - prev)
            : prev + 0.12f * (level - prev);
        m_bandLevel[b].store(smoothed, std::memory_order_relaxed);
    }
}

double EqRack::bandFrequency(int band) {
    if (band < 0) band = 0;
    if (band >= kNumBands) band = kNumBands - 1;
    return kFreq[band];
}

void EqRack::setSampleRate(double sr) {
    if (sr <= 0.0) return;
    m_sampleRate = sr;
    for (int i = 0; i < kNumBands; ++i) recompute(i);
}

void EqRack::setBandGainDb(int band, float gainDb) {
    if (band < 0 || band >= kNumBands) return;
    if (gainDb < kMinDb) gainDb = kMinDb;
    if (gainDb > kMaxDb) gainDb = kMaxDb;
    if (m_gainDb[band] == gainDb) return;
    float oldGain = m_gainDb[band];
    m_gainDb[band] = gainDb;
    recompute(band);
    // Reset filter state when the gain change is large (>2 dB) to kill
    // the cascading IIR transient that produced the audible "ronzio"
    // every time the user moved a slider. Small drags leave state
    // alone so dragging a slider stays smooth.
    if (std::fabs(gainDb - oldGain) > 2.0f) {
        m_left[band].reset();
        m_right[band].reset();
    }
}

float EqRack::bandGainDb(int band) const {
    if (band < 0 || band >= kNumBands) return 0.0f;
    return m_gainDb[band];
}

void EqRack::recompute(int band) {
    double f0 = bandFrequency(band);
    // Skip bands that fall above Nyquist - leave them at unity so the
    // cascade is well-defined even at low sample rates.
    if (f0 >= m_sampleRate * 0.45) {
        m_left[band].setParams(m_sampleRate * 0.4, kQ, 0.0, m_sampleRate);
        m_right[band].setParams(m_sampleRate * 0.4, kQ, 0.0, m_sampleRate);
        return;
    }
    m_left[band].setParams(f0, kQ, m_gainDb[band], m_sampleRate);
    m_right[band].setParams(f0, kQ, m_gainDb[band], m_sampleRate);
}

void EqRack::processStereo(float &l, float &r) {
    for (int i = 0; i < kNumBands; ++i) {
        if (std::abs(m_gainDb[i]) < 0.05f) continue;
        l = m_left[i].process(l);
        r = m_right[i].process(r);
    }
}

void EqRack::feedAnalysis(float l, float r) {
    // Input already in float-normalised domain (SlotDsp scales 1/32768
    // up front). Mono mix into the ring; trigger an FFT every kHop
    // samples for the band-level atomics the GUI reads.
    float mono = (l + r) * 0.5f;
    m_fftRing[m_fftWrite] = mono;
    m_fftWrite = (m_fftWrite + 1) % kFftSize;
    if (++m_fftHop >= 512) {
        m_fftHop = 0;
        runFftAnalysis();
    }
}

void EqRack::reset() {
    for (int i = 0; i < kNumBands; ++i) {
        m_left[i].reset();
        m_right[i].reset();
    }
}

float EqRack::positiveSumDb() const {
    float sum = 0.0f;
    for (int i = 0; i < kNumBands; ++i)
        if (m_gainDb[i] > 0.0f) sum += m_gainDb[i];
    return sum;
}
