#pragma once

// Single peaking-EQ biquad section (RBJ Audio EQ Cookbook). One band of
// the EQ rack. Direct Form I state - cheap and stable enough for the
// gain magnitudes we expose (-24..+24 dB).
class BiquadPeaking {
public:
    // Recompute coefficients from f0 (Hz), Q, gain (dB), sampleRate (Hz).
    void setParams(double f0, double q, double gainDb, double sampleRate);

    // In-place sample tick.
    inline float process(float x) {
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
};
