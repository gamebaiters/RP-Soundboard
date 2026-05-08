#include "Limiter.h"
#include <iterator>

void Limiter::setSampleRate(double sr) {
    m_sampleRate = (sr > 0) ? sr : 48000.0;
}

void Limiter::setParams(float ceilingDb, float lookaheadMs, float releaseMs,
                         Mode mode, float ratio, float gateThreshDb) {
    m_ceiling = std::pow(10.0f, ceilingDb / 20.0f);
    if (m_ceiling < 0.001f) m_ceiling = 0.001f;
    m_lookaheadSamples = static_cast<int>(lookaheadMs * 0.001f * m_sampleRate);
    if (m_lookaheadSamples < 1) m_lookaheadSamples = 1;
    if (m_lookaheadSamples > kMaxLookahead) m_lookaheadSamples = kMaxLookahead;
    m_releaseCoeff = 1.0f - std::exp(-1.0f / (std::max(1.0f, releaseMs) * 0.001f * static_cast<float>(m_sampleRate)));
    m_mode = mode;
    m_ratio = std::max(1.0f, ratio);
    m_gateThresh = std::pow(10.0f, gateThreshDb / 20.0f);
}

void Limiter::processStereo(float &l, float &r) {
    m_delayL[m_delayPos] = l;
    m_delayR[m_delayPos] = r;

    float peak = std::max(std::abs(l), std::abs(r));
    m_peakBuf[m_delayPos] = peak;

    float maxPeak = 0.0f;
    for (int i = 0; i < m_lookaheadSamples; i++) {
        int idx = (m_delayPos - i + kMaxLookahead) % kMaxLookahead;
        if (m_peakBuf[idx] > maxPeak) maxPeak = m_peakBuf[idx];
    }

    float targetGainDb = 0.0f;
    if (m_mode == LimiterMode || m_mode == CompressorMode) {
        if (maxPeak > m_ceiling) {
            float overDb = 20.0f * std::log10(maxPeak / m_ceiling);
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
    float gain = std::pow(10.0f, m_gainDb / 20.0f);
    l = m_delayL[readPos] * gain;
    r = m_delayR[readPos] * gain;

    m_delayPos = (m_delayPos + 1) % kMaxLookahead;
}

void Limiter::reset() {
    std::fill(std::begin(m_delayL), std::end(m_delayL), 0.0f);
    std::fill(std::begin(m_delayR), std::end(m_delayR), 0.0f);
    std::fill(std::begin(m_peakBuf), std::end(m_peakBuf), 0.0f);
    m_delayPos = 0;
    m_gainDb = 0.0f;
}
