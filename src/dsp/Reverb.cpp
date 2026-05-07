#include "Reverb.h"
#include <algorithm>

namespace {
// Freeverb tuning constants (Jezar at Dreampoint, 2000) - public-domain
// reference. Lengths are in samples at 44.1 kHz; rebuildBuffers scales
// to the current sample rate.
constexpr int kCombLens[8] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
constexpr int kStereoSpread = 23;     // R combs are offset by this many samples
constexpr int kAllpassLens[4] = {
    556, 441, 341, 225
};
constexpr float kFreeverbFs = 44100.0f;

constexpr float kFixedGain = 0.015f;          // input scaling
constexpr float kScaleRoom = 0.28f;
constexpr float kOffsetRoom = 0.7f;
constexpr float kScaleDamp = 0.4f;

inline int scaleLen(int srcLen, double targetFs) {
    return static_cast<int>(srcLen * (targetFs / kFreeverbFs) + 0.5);
}
}

Reverb::Reverb() {
    setSampleRate(48000.0);
    setRoomSize(0.5f);
    setDamping(0.5f);
    setWet(0.0f);
    setWidth(1.0f);
}

void Reverb::setSampleRate(double sr) {
    m_fs = sr > 0 ? sr : 48000.0;
    rebuildBuffers();
    setRoomSize(m_roomSize);
    setDamping(m_damp);
}

void Reverb::rebuildBuffers() {
    for (int i = 0; i < kNumCombs; ++i) {
        m_combL[i].setSize(scaleLen(kCombLens[i], m_fs));
        m_combR[i].setSize(scaleLen(kCombLens[i] + kStereoSpread, m_fs));
    }
    for (int i = 0; i < kNumAllpass; ++i) {
        m_apL[i].setSize(scaleLen(kAllpassLens[i], m_fs));
        m_apR[i].setSize(scaleLen(kAllpassLens[i] + kStereoSpread, m_fs));
        m_apL[i].feedback = 0.5f;
        m_apR[i].feedback = 0.5f;
    }
}

void Reverb::setWet(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_wet = w;
    m_dry = 1.0f - w;
    // Width-aware wet gains (Freeverb's wet1/wet2 channel mix).
    m_wetGainL1 = m_wet * (m_width / 2.0f + 0.5f);
    m_wetGainL2 = m_wet * ((1.0f - m_width) / 2.0f);
    m_wetGainR1 = m_wetGainL1;
    m_wetGainR2 = m_wetGainL2;
}

void Reverb::setRoomSize(float s) {
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    m_roomSize = s;
    float fb = kOffsetRoom + s * kScaleRoom;
    for (int i = 0; i < kNumCombs; ++i) {
        m_combL[i].feedback = fb;
        m_combR[i].feedback = fb;
    }
}

void Reverb::setDamping(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    m_damp = d;
    float damp = d * kScaleDamp;
    for (int i = 0; i < kNumCombs; ++i) {
        m_combL[i].damp = damp;
        m_combR[i].damp = damp;
    }
}

void Reverb::setWidth(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    m_width = w;
    setWet(m_wet);  // refresh wet gains
}

void Reverb::process(float &l, float &r) {
    float in = (l + r) * kFixedGain;

    float outL = 0.0f, outR = 0.0f;
    for (int i = 0; i < kNumCombs; ++i) {
        outL += m_combL[i].process(in);
        outR += m_combR[i].process(in);
    }
    for (int i = 0; i < kNumAllpass; ++i) {
        outL = m_apL[i].process(outL);
        outR = m_apR[i].process(outR);
    }

    float dryL = l * m_dry;
    float dryR = r * m_dry;
    l = dryL + outL * m_wetGainL1 + outR * m_wetGainL2;
    r = dryR + outR * m_wetGainR1 + outL * m_wetGainR2;
}

void Reverb::reset() {
    for (int i = 0; i < kNumCombs; ++i) {
        std::fill(m_combL[i].buf.begin(), m_combL[i].buf.end(), 0.0f);
        std::fill(m_combR[i].buf.begin(), m_combR[i].buf.end(), 0.0f);
        m_combL[i].lpf = 0.0f;
        m_combR[i].lpf = 0.0f;
    }
    for (int i = 0; i < kNumAllpass; ++i) {
        std::fill(m_apL[i].buf.begin(), m_apL[i].buf.end(), 0.0f);
        std::fill(m_apR[i].buf.begin(), m_apR[i].buf.end(), 0.0f);
    }
}
