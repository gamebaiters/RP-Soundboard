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
        // Pro-audio dB scaling, no per-band tilt.
        //
        // Earlier iterations tried a pink-noise tilt (subtract 3 dB/oct
        // from low bands so a bass-heavy mix wouldn't pin the low LEDs
        // to the ceiling). It overshot: a 0 dBFS / clipping bass would
        // get -17 dB worth of compensation subtracted and end up around
        // 60 % on the LED instead of saturating. The user reported
        // "bass clipping clearly, LEDs never past half" - the tilt was
        // hiding real signal level.
        //
        // Reverted to flat dB scale. Bass-heavy mixes will simply show
        // higher low bands than high bands - that's what the spectrum
        // actually IS. Clipping content reads as clipping. Quiet
        // content still gets honest visual range via the dB window
        // below.
        //
        // Full-scale-sine reference. A 0 dBFS sine through Hann window
        // of length kFftSize produces a peak bin magnitude of
        // ~kFftSize/4 (coherent gain 0.5 + magnitude-of-complex). In
        // practice the peak we observe on real "loud" material is
        // ~6 dB lower than that theoretical max: softLimit caps the
        // time-domain peak at ~0.95 (~-0.45 dB) and clipped/distorted
        // content spreads its fundamental energy across several bins
        // so the strongest bin tops out around -3 to -6 dB. That left
        // the LED stuck around 87 % even on real earrape - "6 notches
        // below the top" in the user's words.
        //
        // Set the reference at kFftSize/8 instead so the actual
        // observed peak on loud audio reaches +3 dB displayed (clamped
        // to ceiling -> LED top). Equivalent to a +6 dB sensitivity
        // calibration. Quiet content keeps the full -48..0 dBFS range
        // because the floor moves with it.
        constexpr double kFullScaleMag = double(kFftSize) / 8.0;  // 256 at kFftSize=2048
        constexpr double kFloorDb      = -48.0;
        constexpr double kCeilDb       =   3.0;
        double db = kFloorDb;
        if (peakMag > 1e-12) {
            db = 20.0 * std::log10(peakMag / kFullScaleMag);
            if (db < kFloorDb) db = kFloorDb;
            if (db > kCeilDb)  db = kCeilDb;
        }
        float level = float((db - kFloorDb) / (kCeilDb - kFloorDb));
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
    m_gainDb[band] = gainDb;
    recompute(band);
    // No state reset: BiquadPeaking::setParams now ramps coefficients
    // over kRampSamples. The cascading IIR transient that motivated
    // the old >2 dB reset is gone, so the reset itself (which was a
    // step in the y[n] sequence and the original cause of the spread-
    // spectrum impulse that frye'd the downstream Spatial HRTF
    // convolution) is no longer needed.
}

float EqRack::bandGainDb(int band) const {
    if (band < 0 || band >= kNumBands) return 0.0f;
    return m_gainDb[band];
}

void EqRack::recompute(int band) {
    double f0 = bandFrequency(band);
    // Frequency-adaptive Q. The 2/3-octave default (kQ = 2.145) is the
    // right number for the mid + high register: narrow enough to be
    // selective, just wide enough that adjacent bands sum cleanly at
    // the seams. At the sub/low-bass end (20..100 Hz) that Q is too
    // tight - the 20 Hz slider only affects ~14..26 Hz, which is
    // below the content of nearly every real audio file.
    //
    // First attempt set Q = 0.7 on bands 0..4. That widened them too
    // much - five adjacent low bands at Q=0.7 overlap so heavily that
    // a smile-curve preset (+12 dB on all of them at once) summed to
    // +20+ dB in the 30..80 Hz region and instantly slammed softLimit
    // into a hard-clip earrape. Q = 1.4 is the compromise: each low
    // band still catches roughly 2x its old span (20 Hz band now
    // covers ~13..27 Hz vs 14..26 before), the slider FEELS like it
    // does something on real bass content, AND adjacent bands stack
    // gently enough for the classic smile EQ to stay musical.
    const double q = (band <= 4) ? 1.4 : kQ;
    // Skip bands that fall above Nyquist - leave them at unity so the
    // cascade is well-defined even at low sample rates.
    if (f0 >= m_sampleRate * 0.45) {
        m_left[band].setParams(m_sampleRate * 0.4, q, 0.0, m_sampleRate);
        m_right[band].setParams(m_sampleRate * 0.4, q, 0.0, m_sampleRate);
        return;
    }
    m_left[band].setParams(f0, q, m_gainDb[band], m_sampleRate);
    m_right[band].setParams(f0, q, m_gainDb[band], m_sampleRate);
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
    // up front). Mono mix into the ring; trigger an FFT every
    // m_fftHopInterval samples for the band-level atomics the GUI reads.
    float mono = (l + r) * 0.5f;
    m_fftRing[m_fftWrite] = mono;
    m_fftWrite = (m_fftWrite + 1) % kFftSize;
    if (++m_fftHop >= m_fftHopInterval) {
        m_fftHop = 0;
        runFftAnalysis();
    }
}

void EqRack::setStageActive(bool active) {
    // Active: 512-sample hop (~94 Hz update at 48 kHz) for smooth LEDs.
    // Inactive: 2048-sample hop (~24 Hz, one FFT per buffer length) so
    // the analyser does not steal audio-thread cycles when the EQ
    // panel is collapsed and the user is not watching the bars.
    m_fftHopInterval = active ? 512 : 2048;
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
