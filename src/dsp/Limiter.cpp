#include "Limiter.h"
#include "../AudioUtils.h"
#include <iterator>

void Limiter::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Limiter::setParams(float ceilingDb, float lookaheadMs, float releaseMs,
                         Mode mode, float ratio, float gateThreshDb) {
    m_ceiling = AudioUtils::dbToLinear(ceilingDb);
    if (m_ceiling < 0.001f) m_ceiling = 0.001f;
    m_lookaheadSamples = static_cast<int>(lookaheadMs * 0.001f * m_sampleRate);
    if (m_lookaheadSamples < 1) m_lookaheadSamples = 1;
    if (m_lookaheadSamples > kMaxLookahead) m_lookaheadSamples = kMaxLookahead;
    m_releaseCoeff = 1.0f - std::exp(-1.0f / (std::max(1.0f, releaseMs) * 0.001f * static_cast<float>(m_sampleRate)));
    m_mode = mode;
    m_ratio = std::max(1.0f, ratio);
    m_gateThresh = AudioUtils::dbToLinear(gateThreshDb);
}

// Max |value| over the 3 inter-sample points between h[1] and h[2]
// (Catmull-Rom through h[0..3]), plus the samples themselves. h is
// chronological: h[0] oldest.
float Limiter::truePeakOf(const float *h) const {
    float peak = std::max(std::fabs(h[1]), std::fabs(h[2]));
    for (int k = 1; k <= 3; ++k) {
        float t = 0.25f * k;
        float t2 = t * t, t3 = t2 * t;
        float v = 0.5f * ((2.0f * h[1]) +
                          (-h[0] + h[2]) * t +
                          (2.0f * h[0] - 5.0f * h[1] + 4.0f * h[2] - h[3]) * t2 +
                          (-h[0] + 3.0f * h[1] - 3.0f * h[2] + h[3]) * t3);
        float av = std::fabs(v);
        if (av > peak) peak = av;
    }
    return peak;
}

void Limiter::processStereo(float &l, float &r) {
    m_delayL[m_delayPos] = l;
    m_delayR[m_delayPos] = r;

    float peak;
    if (m_truePeak) {
        // Shift the 4-sample histories and estimate the inter-sample
        // peak of the segment that just became fully defined. The one-
        // sample detection delay this introduces is far inside the
        // limiter's own lookahead window.
        m_tpHistL[0] = m_tpHistL[1]; m_tpHistL[1] = m_tpHistL[2];
        m_tpHistL[2] = m_tpHistL[3]; m_tpHistL[3] = l;
        m_tpHistR[0] = m_tpHistR[1]; m_tpHistR[1] = m_tpHistR[2];
        m_tpHistR[2] = m_tpHistR[3]; m_tpHistR[3] = r;
        peak = std::max(truePeakOf(m_tpHistL), truePeakOf(m_tpHistR));
    } else {
        peak = std::max(std::abs(l), std::abs(r));
    }
    m_peakBuf[m_delayPos] = peak;

    float maxPeak = 0.0f;
    for (int i = 0; i < m_lookaheadSamples; i++) {
        int idx = (m_delayPos - i + kMaxLookahead) % kMaxLookahead;
        if (m_peakBuf[idx] > maxPeak) maxPeak = m_peakBuf[idx];
    }

    float targetGainDb = 0.0f;
    if (m_mode == LimiterMode || m_mode == CompressorMode) {
        if (maxPeak > m_ceiling) {
            float overDb = AudioUtils::linearToDb(maxPeak / m_ceiling);
            float effectiveRatio = (m_mode == LimiterMode) ? 100.0f : m_ratio;
            targetGainDb = -(1.0f - 1.0f / effectiveRatio) * overDb;
        }
    }

    if (m_mode == GateMode && maxPeak < m_gateThresh) {
        targetGainDb = m_gateAttenuationDb;
    }

    if (targetGainDb < m_gainDb) {
        m_gainDb = targetGainDb;
    } else {
        m_gainDb += m_releaseCoeff * (targetGainDb - m_gainDb);
    }

    int readPos = (m_delayPos - m_lookaheadSamples + kMaxLookahead) % kMaxLookahead;
    float gain = AudioUtils::dbToLinear(m_gainDb);
    l = m_delayL[readPos] * gain;
    r = m_delayR[readPos] * gain;

    m_delayPos = (m_delayPos + 1) % kMaxLookahead;
}

void Limiter::reset() {
    std::fill(std::begin(m_delayL), std::end(m_delayL), 0.0f);
    std::fill(std::begin(m_delayR), std::end(m_delayR), 0.0f);
    std::fill(std::begin(m_peakBuf), std::end(m_peakBuf), 0.0f);
    std::fill(std::begin(m_tpHistL), std::end(m_tpHistL), 0.0f);
    std::fill(std::begin(m_tpHistR), std::end(m_tpHistR), 0.0f);
    m_delayPos = 0;
    m_gainDb = 0.0f;
}
