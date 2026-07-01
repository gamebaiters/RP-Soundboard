#include "BiquadPeaking.h"
#include <cmath>

void BiquadPeaking::setParams(double f0, double q, double gainDb, double sampleRate) {
    if (sampleRate <= 0.0 || f0 <= 0.0 || q <= 0.0) return;
    double A = std::pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * f0 / sampleRate;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double alpha = sinw0 / (2.0 * q);

    double bb0 = 1.0 + alpha * A;
    double bb1 = -2.0 * cosw0;
    double bb2 = 1.0 - alpha * A;
    double aa0 = 1.0 + alpha / A;
    double aa1 = -2.0 * cosw0;
    double aa2 = 1.0 - alpha / A;

    // Resolve any in-flight ramp first so the new target is reached
    // from a clean baseline.
    if (m_rampRemaining > 0) {
        b0 = m_tb0; b1 = m_tb1; b2 = m_tb2;
        a1 = m_ta1; a2 = m_ta2;
        m_rampRemaining = 0;
    }

    m_tb0 = bb0 / aa0;
    m_tb1 = bb1 / aa0;
    m_tb2 = bb2 / aa0;
    m_ta1 = aa1 / aa0;
    m_ta2 = aa2 / aa0;

    // First-time set (b0 == 1 default, no live state) -> snap. Otherwise
    // ramp the coefficients across kRampSamples so the EQ slider
    // produces no transient. Resets are still allowed via reset().
    bool firstSet = (b0 == 1.0 && b1 == 0.0 && b2 == 0.0 &&
                     a1 == 0.0 && a2 == 0.0 &&
                     x1 == 0.0 && y1 == 0.0);
    if (firstSet) {
        b0 = m_tb0; b1 = m_tb1; b2 = m_tb2;
        a1 = m_ta1; a2 = m_ta2;
        m_db0 = m_db1 = m_db2 = m_da1 = m_da2 = 0.0;
        m_rampRemaining = 0;
    } else {
        const double inv = 1.0 / static_cast<double>(kRampSamples);
        m_db0 = (m_tb0 - b0) * inv;
        m_db1 = (m_tb1 - b1) * inv;
        m_db2 = (m_tb2 - b2) * inv;
        m_da1 = (m_ta1 - a1) * inv;
        m_da2 = (m_ta2 - a2) * inv;
        m_rampRemaining = kRampSamples;
    }
}

void BiquadPeaking::reset() {
    x1 = x2 = y1 = y2 = 0.0;
    m_rampRemaining = 0;
}
