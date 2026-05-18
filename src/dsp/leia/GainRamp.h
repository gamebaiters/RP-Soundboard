#pragma once
// GainRamp.h -- Linear per-sample gain interpolation.
// Single header, no .cpp needed.

struct GainRamp {
    float currentGain   = 1.0f;
    float targetGain    = 1.0f;
    float gainDelta     = 0.0f;
    int   rampRemaining = 0;

    // ------------------------------------------------------------------
    // Schedule a gain ramp from the current value to `target` over
    // `rampSamples` samples.  If rampSamples <= 0 the gain jumps
    // immediately.
    // ------------------------------------------------------------------
    void setTarget(float target, int rampSamples)
    {
        targetGain = target;
        if (rampSamples <= 0) {
            currentGain   = target;
            gainDelta     = 0.0f;
            rampRemaining = 0;
        } else {
            gainDelta     = (target - currentGain) / static_cast<float>(rampSamples);
            rampRemaining = rampSamples;
        }
    }

    // ------------------------------------------------------------------
    // Advance one sample and return the gain for this sample.
    // ------------------------------------------------------------------
    float next()
    {
        if (rampRemaining > 0) {
            currentGain += gainDelta;
            --rampRemaining;
            if (rampRemaining == 0)
                currentGain = targetGain;   // snap to exact target
        }
        return currentGain;
    }

    // ------------------------------------------------------------------
    // Apply gain (with ramping) to an interleaved multi-channel buffer.
    // `frames`   = number of sample frames
    // `channels` = number of interleaved channels per frame
    // ------------------------------------------------------------------
    void apply(float* buffer, int frames, int channels)
    {
        for (int f = 0; f < frames; ++f) {
            float g = next();
            for (int c = 0; c < channels; ++c)
                buffer[f * channels + c] *= g;
        }
    }
};
