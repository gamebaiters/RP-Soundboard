#pragma once
// IIRSmoother.h -- Critically-damped 2nd-order spring system for smooth
// parameter interpolation (avoids zipper noise).  Single header.

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>

struct IIRSmoother {
    float  velocity  = 0.0f;
    float  position  = 0.0f;
    double omega_sq  = 0.0;   // w0^2
    double two_omega = 0.0;   // 2 * w0
    float  target    = 0.0f;

    // ------------------------------------------------------------------
    // Initialise the smoother.
    //   sampleRate       -- audio sample rate (Hz)
    //   smoothTimeSeconds -- time constant in seconds
    //   initialValue     -- starting value (no ramp from zero)
    // ------------------------------------------------------------------
    void init(float sampleRate, float smoothTimeSeconds, float initialValue)
    {
        // Natural frequency: one full period = smoothTime * sampleRate samples.
        double w0 = 2.0 * M_PI / (static_cast<double>(sampleRate) * static_cast<double>(smoothTimeSeconds));
        omega_sq  = w0 * w0;
        two_omega = 2.0 * w0;

        position = initialValue;
        target   = initialValue;
        velocity = 0.0f;
    }

    // ------------------------------------------------------------------
    void setTarget(float t) { target = t; }

    // ------------------------------------------------------------------
    // Advance one sample and return the smoothed value.
    // ------------------------------------------------------------------
    float process()
    {
        velocity += static_cast<float>(
            static_cast<double>(target - position) * omega_sq
          - static_cast<double>(velocity)          * two_omega);
        position += velocity;
        return position;
    }

    // ------------------------------------------------------------------
    // Fill an output buffer with `count` smoothed values.
    // ------------------------------------------------------------------
    void processBlock(float* output, int count)
    {
        for (int i = 0; i < count; ++i)
            output[i] = process();
    }

    // ------------------------------------------------------------------
    float current() const { return position; }
};
