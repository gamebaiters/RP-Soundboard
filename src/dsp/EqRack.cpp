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
    // Bypass flat bands entirely. Cascading 16 unity-gain biquads is
    // mathematically a passthrough but each one carries IIR state that
    // can pick up tiny FP residues at runtime and feed them forward;
    // 16 cascaded copies of that residue rendered as an audible buzz.
    for (int i = 0; i < kNumBands; ++i) {
        if (std::abs(m_gainDb[i]) < 0.05f) continue;
        l = m_left[i].process(l);
        r = m_right[i].process(r);
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
