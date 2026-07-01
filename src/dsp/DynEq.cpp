#include "DynEq.h"
#include "../AudioUtils.h"

namespace {
float onePoleCoeff(float ms, double sr) {
    return 1.0f - std::exp(-1.0f / (std::max(0.1f, ms) * 0.001f * static_cast<float>(sr)));
}
}

DynEq::DynEq() {
    for (int b = 0; b < kNumBands; ++b) {
        m_cfg[b].freq        = (b == 0) ?  120.0f
                             : (b == 1) ?  600.0f
                             : (b == 2) ? 3000.0f
                             :            8000.0f;
        recomputeBand(b);
    }
}

void DynEq::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
    for (int b = 0; b < kNumBands; ++b) recomputeBand(b);
}

void DynEq::setBand(int b, const BandConfig &cfg) {
    if (b < 0 || b >= kNumBands) return;
    m_cfg[b] = cfg;
    recomputeBand(b);
}

DynEq::BandConfig DynEq::band(int b) const {
    if (b < 0 || b >= kNumBands) return BandConfig{};
    return m_cfg[b];
}

void DynEq::recomputeBand(int b) {
    // Baseline audio-path biquad set to static gain; updateBandGain
    // will later mix in the dynamic offset.
    const auto &c = m_cfg[b];
    m_bpL[b].setParams(c.freq, c.q, c.staticGainDb, m_sampleRate);
    m_bpR[b].setParams(c.freq, c.q, c.staticGainDb, m_sampleRate);
    // Sidechain analyser: +12 dB peaking centred on the same band with
    // the same Q so its response magnitude scales with the amount of
    // band content in the incoming signal.
    m_scL[b].setParams(c.freq, c.q, 12.0f, m_sampleRate);
    m_scR[b].setParams(c.freq, c.q, 12.0f, m_sampleRate);
    m_attackCoeff [b] = onePoleCoeff(c.attackMs,  m_sampleRate);
    m_releaseCoeff[b] = onePoleCoeff(c.releaseMs, m_sampleRate);
    m_lastGainDb[b] = c.staticGainDb;
}

void DynEq::updateBandGain(int b) {
    // Recompute biquad at the new (static + dynamic) gain. Because
    // BiquadPeaking ramps coefficients internally, per-sample calls
    // sound smooth despite the gain moving.
    const auto &c = m_cfg[b];
    float envDb = (m_env[b] > 1e-6f) ? AudioUtils::linearToDb(m_env[b]) : -96.0f;
    float overDb = envDb - c.thresholdDb;
    float dynDb = 0.0f;
    if (overDb > 0.0f) {
        // Fraction of range applied based on ratio (higher ratio = more).
        float t = overDb / 12.0f;   // full range at 12 dB overshoot
        if (t > 1.0f) t = 1.0f;
        dynDb = c.dynamicDb * (1.0f - 1.0f / c.ratio) * t;
    }
    float targetDb = c.staticGainDb + dynDb;
    if (std::abs(targetDb - m_lastGainDb[b]) > 0.05f) {
        m_bpL[b].setParams(c.freq, c.q, targetDb, m_sampleRate);
        m_bpR[b].setParams(c.freq, c.q, targetDb, m_sampleRate);
        m_lastGainDb[b] = targetDb;
    }
}

void DynEq::processStereo(float &l, float &r) {
    for (int b = 0; b < kNumBands; ++b) {
        if (!m_cfg[b].enabled) continue;
        // Sidechain analyser: parallel path measuring band content.
        // The RESULT does NOT feed the output - only its magnitude
        // drives the envelope + dynamic gain update.
        float aL = m_scL[b].process(l);
        float aR = m_scR[b].process(r);
        float bandPeak = std::max(std::abs(aL), std::abs(aR));
        float coeff = (bandPeak > m_env[b]) ? m_attackCoeff[b] : m_releaseCoeff[b];
        m_env[b] += coeff * (bandPeak - m_env[b]);
        // Update biquad every ~16 samples for CPU budget - the internal
        // BiquadPeaking coefficient ramp smooths sub-sample updates so
        // slower control-rate updates read as musical breathing.
        static int s_updateCounter = 0;
        if ((s_updateCounter++ & 15) == 0) updateBandGain(b);
        l = m_bpL[b].process(l);
        r = m_bpR[b].process(r);
    }
}

void DynEq::reset() {
    for (int b = 0; b < kNumBands; ++b) {
        m_bpL[b].reset();
        m_bpR[b].reset();
        m_scL[b].reset();
        m_scR[b].reset();
        m_env[b] = 0.0f;
        m_lastGainDb[b] = m_cfg[b].staticGainDb;
    }
}
