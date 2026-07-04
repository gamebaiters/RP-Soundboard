#pragma once

#include <cmath>
#include <algorithm>

// Binaural beats tone layer (Stage_Binaural).
//
// Lays a pure sine pair UNDER the program material: baseHz to the left
// ear, baseHz + beatHz to the right. The brain perceives an amplitude
// beat at beatHz (the "binaural beat") that neither ear actually
// receives - only works on headphones, which is exactly what TS3 users
// wear. Level is in dB and deliberately capped low; the tones ride
// under the audio, they do not replace it.
//
// The tone fades in/out over ~50 ms on enable/disable so toggling the
// stage never clicks.
class BinauralBeats {
public:
    void setSampleRate(double sr) {
        m_fs = (sr > 0) ? sr : 48000.0;
        m_fadeStep = static_cast<float>(1.0 / (0.050 * m_fs));
        reset();
    }

    void setParams(float baseHz, float beatHz, float levelDb) {
        m_baseHz = std::min(600.0f, std::max(80.0f, baseHz));
        m_beatHz = std::min(40.0f, std::max(0.5f, beatHz));
        float db = std::min(-6.0f, std::max(-60.0f, levelDb));
        m_level  = std::pow(10.0f, db / 20.0f);
    }

    // `active` = stage currently enabled; drives the anti-click fade.
    void processStereo(float &l, float &r, bool active) {
        float target = active ? 1.0f : 0.0f;
        if (m_fade < target)      m_fade = std::min(target, m_fade + m_fadeStep);
        else if (m_fade > target) m_fade = std::max(target, m_fade - m_fadeStep);
        if (m_fade <= 0.0f) return;

        constexpr double kTwoPi = 6.28318530717958647692;
        m_phL += kTwoPi * m_baseHz / m_fs;
        m_phR += kTwoPi * (m_baseHz + m_beatHz) / m_fs;
        if (m_phL > kTwoPi) m_phL -= kTwoPi;
        if (m_phR > kTwoPi) m_phR -= kTwoPi;

        float g = m_level * m_fade;
        l += g * static_cast<float>(std::sin(m_phL));
        r += g * static_cast<float>(std::sin(m_phR));
    }

    void reset() {
        m_phL = m_phR = 0.0;
        m_fade = 0.0f;
    }

private:
    double m_fs = 48000.0;
    float  m_baseHz = 200.0f;
    float  m_beatHz = 7.0f;
    float  m_level  = 0.063f;   // -24 dB
    double m_phL = 0.0, m_phR = 0.0;
    float  m_fade = 0.0f;
    float  m_fadeStep = 0.0004f;
};
