#pragma once

// Single peaking-EQ biquad section (RBJ Audio EQ Cookbook). One band of
// the EQ rack. Direct Form I state - cheap and stable enough for the
// gain magnitudes we expose (-24..+24 dB).
//
// Coefficient ramping: setParams stores the new coefficients as a target
// and ramps the live coefficients over kRampSamples samples. The Direct
// Form I delay line stays continuous across the change, which kills the
// click/transient the old "reset state when delta>2dB" code produced.
// Lerping coefficients on a stable IIR is safe at small per-sample
// deltas (the filter remains stable along the path between any two
// valid sets of coefficients of the same topology) - this is the same
// technique used in most pro-audio EQs.
class BiquadPeaking {
public:
    static constexpr int kRampSamples = 1024;  // ~21 ms at 48 kHz

    // Recompute coefficients from f0 (Hz), Q, gain (dB), sampleRate (Hz).
    void setParams(double f0, double q, double gainDb, double sampleRate);

    // In-place sample tick. Advances the coefficient ramp first.
    inline float process(float x) {
        if (m_rampRemaining > 0) {
            b0 += m_db0; b1 += m_db1; b2 += m_db2;
            a1 += m_da1; a2 += m_da2;
            --m_rampRemaining;
            if (m_rampRemaining == 0) {
                b0 = m_tb0; b1 = m_tb1; b2 = m_tb2;
                a1 = m_ta1; a2 = m_ta2;
            }
        }
        double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x;
        y2 = y1; y1 = y;
        return static_cast<float>(y);
    }

    void reset();

private:
    double b0 = 1, b1 = 0, b2 = 0;
    double a1 = 0, a2 = 0;
    double x1 = 0, x2 = 0;
    double y1 = 0, y2 = 0;

    // Coefficient-ramp target + per-sample delta.
    double m_tb0 = 1, m_tb1 = 0, m_tb2 = 0;
    double m_ta1 = 0, m_ta2 = 0;
    double m_db0 = 0, m_db1 = 0, m_db2 = 0;
    double m_da1 = 0, m_da2 = 0;
    int    m_rampRemaining = 0;
};
