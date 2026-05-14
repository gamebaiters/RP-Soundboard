#pragma once

#include <cmath>

class Bitcrusher {
public:
    void setSampleRate(double sr) { m_inputRate = sr; }
    void reset() { m_holdL = 0.0f; m_holdR = 0.0f; m_phase = 0.0f; }

    void setParams(int bitDepth, float targetRate) {
        m_bitDepth = bitDepth;
        m_targetRate = targetRate;
    }

    void processStereo(float &l, float &r) {
        if (m_bitDepth < 16) {
            float levels = std::pow(2.0f, static_cast<float>(m_bitDepth) - 1.0f);
            l = std::round(l * levels) / levels;
            r = std::round(r * levels) / levels;
        }

        if (m_targetRate < static_cast<float>(m_inputRate) - 1.0f) {
            m_phase += m_targetRate / static_cast<float>(m_inputRate);
            if (m_phase >= 1.0f) {
                m_phase -= 1.0f;
                m_holdL = l;
                m_holdR = r;
            }
            l = m_holdL;
            r = m_holdR;
        }
    }

private:
    double m_inputRate = 48000.0;
    float  m_targetRate = 48000.0f;
    int    m_bitDepth = 16;
    float  m_holdL = 0.0f;
    float  m_holdR = 0.0f;
    float  m_phase = 0.0f;
};
