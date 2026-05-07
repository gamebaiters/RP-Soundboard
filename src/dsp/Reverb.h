#pragma once

#include <vector>

// Schroeder-Moorer / Freeverb-style stereo reverb. The classic recipe:
// 8 lowpass-feedback comb filters in parallel per channel, followed by
// 4 allpass filters in series. The right-channel comb lengths are
// offset by a small "stereo spread" (~23 samples at 44.1 kHz) so the
// two channels decorrelate even when fed identical input.
//
// Why this matters for the 8D illusion: a dry HRTF placement always
// reads as "in the head" because the brain sees no environmental
// reflections. Add a real stereo reverb tail and the same HRTF cues
// snap "out of the head" - that's the secret sauce all 8D remix tracks
// rely on.
class Reverb {
public:
    Reverb();
    void setSampleRate(double sr);

    void setWet(float w);       // 0..1 dry/wet mix
    void setRoomSize(float s);  // 0..1 -> feedback 0.70..0.98
    void setDamping(float d);   // 0..1 -> highs roll off faster
    void setWidth(float w);     // 0..1 -> stereo spread on output

    void process(float &l, float &r);
    void reset();

private:
    static constexpr int kNumCombs    = 8;
    static constexpr int kNumAllpass  = 4;

    struct Comb {
        std::vector<float> buf;
        int   idx     = 0;
        float feedback = 0.84f;
        float damp    = 0.5f;
        float lpf     = 0.0f;

        void setSize(int n) { buf.assign(n, 0.0f); idx = 0; lpf = 0.0f; }
        inline float process(float x) {
            int n = static_cast<int>(buf.size());
            float out = buf[idx];
            lpf = (1.0f - damp) * out + damp * lpf;
            buf[idx] = x + lpf * feedback;
            ++idx;
            if (idx >= n) idx = 0;
            return out;
        }
    };

    struct Allpass {
        std::vector<float> buf;
        int   idx = 0;
        float feedback = 0.5f;

        void setSize(int n) { buf.assign(n, 0.0f); idx = 0; }
        inline float process(float x) {
            int n = static_cast<int>(buf.size());
            float v = buf[idx];
            float out = -x + v;
            buf[idx] = x + v * feedback;
            ++idx;
            if (idx >= n) idx = 0;
            return out;
        }
    };

    void rebuildBuffers();

    Comb     m_combL[kNumCombs];
    Comb     m_combR[kNumCombs];
    Allpass  m_apL[kNumAllpass];
    Allpass  m_apR[kNumAllpass];

    double m_fs = 48000.0;
    float  m_wet = 0.0f;
    float  m_dry = 1.0f;
    float  m_roomSize = 0.5f;
    float  m_damp = 0.5f;
    float  m_width = 1.0f;
    float  m_wetGainL1 = 0.0f, m_wetGainL2 = 0.0f;
    float  m_wetGainR1 = 0.0f, m_wetGainR2 = 0.0f;
};
