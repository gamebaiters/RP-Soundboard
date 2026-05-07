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

    b0 = bb0 / aa0;
    b1 = bb1 / aa0;
    b2 = bb2 / aa0;
    a1 = aa1 / aa0;
    a2 = aa2 / aa0;
}

void BiquadPeaking::reset() {
    x1 = x2 = y1 = y2 = 0.0;
}
