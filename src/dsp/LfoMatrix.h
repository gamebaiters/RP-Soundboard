#pragma once

#include "SandboxState.h"
#include <cmath>
#include <cstdint>
#include <algorithm>

// Per-channel LFO modulation matrix (M1).
//
// Two free-running LFOs, four routing rows, a curated list of safe
// modulation targets. The matrix itself only produces VALUES - the
// actual application happens in SlotDsp::process once per audio block:
// it computes each routed target's modulated value from the BASE value
// stored in SandboxState and pushes it through the cheap live setters
// of the affected DSP objects. Base values are never mutated, so
// disabling a route instantly restores the slider positions.
class LfoMatrix {
public:
    // Curated targets. Values are persisted in SandboxState - append
    // only, never renumber.
    enum Target {
        Target_None = 0,
        Target_RingFreq,      // VoiceFx ring modulator frequency
        Target_WahSweep,      // VoiceFx auto-wah sweep bias
        Target_TremRate,      // VoiceFx tremolo rate
        Target_VibDepth,      // VoiceFx vibrato depth
        Target_ChorusMix,
        Target_ReverbWet,
        Target_SatDrive,
        Target_CrushRate,     // bitcrusher sample rate
        Target_Pan,           // output balance
        Target_Volume,        // output gain 0..1
        Target_DelayFeedback,
        Target_COUNT
    };

    static const char *targetName(int t) {
        static const char *names[Target_COUNT] = {
            "None", "Ring Freq", "Wah Sweep", "Tremolo Rate",
            "Vibrato Depth", "Chorus Mix", "Reverb Wet", "Sat Drive",
            "Crush Rate", "Pan", "Volume", "Delay Feedback"
        };
        if (t < 0 || t >= Target_COUNT) return "?";
        return names[t];
    }

    void setSampleRate(double sr) { m_fs = (sr > 0) ? sr : 48000.0; }

    void setParams(const SandboxState &s) {
        for (int i = 0; i < 2; ++i) {
            m_enabled[i] = s.lfoEnabled[i];
            m_rate[i]    = std::min(20.0f, std::max(0.05f, s.lfoRateHz[i]));
            m_shape[i]   = s.lfoShape[i];
        }
    }

    bool anyEnabled() const { return m_enabled[0] || m_enabled[1]; }

    // Advance both LFOs by `frames` samples and refresh the current
    // output values (-1..+1). Call once per audio block.
    void advance(int frames) {
        for (int i = 0; i < 2; ++i) {
            if (!m_enabled[i]) { m_value[i] = 0.0f; continue; }
            double inc = m_rate[i] / m_fs * frames;
            m_phase[i] += inc;
            if (m_phase[i] >= 1.0) {
                m_phase[i] -= std::floor(m_phase[i]);
                // Sample & hold refreshes on each cycle wrap.
                m_noise = m_noise * 1664525u + 1013904223u;
                m_sh[i] = (static_cast<float>(m_noise >> 9) / 4194304.0f) - 1.0f;
            }
            float p = static_cast<float>(m_phase[i]);
            switch (m_shape[i]) {
            case 1:  // triangle
                m_value[i] = (p < 0.5f) ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
                break;
            case 2:  // square (soft edges are applied target-side)
                m_value[i] = (p < 0.5f) ? 1.0f : -1.0f;
                break;
            case 3:  // sample & hold
                m_value[i] = m_sh[i];
                break;
            default: // sine
                m_value[i] = std::sin(6.28318530717958647692f * p);
                break;
            }
        }
    }

    float value(int lfo) const {
        return (lfo >= 0 && lfo < 2) ? m_value[lfo] : 0.0f;
    }

    void reset() {
        m_phase[0] = m_phase[1] = 0.0;
        m_value[0] = m_value[1] = 0.0f;
        m_sh[0] = m_sh[1] = 0.0f;
    }

private:
    double m_fs = 48000.0;
    bool   m_enabled[2] = { false, false };
    float  m_rate[2]    = { 1.0f, 0.25f };
    int    m_shape[2]   = { 0, 0 };
    double m_phase[2]   = { 0.0, 0.0 };
    float  m_value[2]   = { 0.0f, 0.0f };
    float  m_sh[2]      = { 0.0f, 0.0f };
    uint32_t m_noise    = 0x87654321u;
};
