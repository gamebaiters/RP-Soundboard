// LeiaEngine.cpp -- Leia spatial audio engine implementation.

#include "LeiaEngine.h"

#include <algorithm>
#include <cstring>

// FTZ/DAZ denormal flushing is an x86 SSE feature. On ARM (Apple Silicon
// macOS, ARM Linux) <xmmintrin.h> does not exist, so guard the whole
// MXCSR path behind an x86 detection macro.
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  #define LEIA_X86 1
  #include <xmmintrin.h>
#else
  #define LEIA_X86 0
#endif

LeiaEngine::LeiaEngine()  = default;
LeiaEngine::~LeiaEngine() = default;

bool LeiaEngine::init(const std::string& sofaPath,
                       float sampleRate,
                       int   blockSize)
{
    m_ready      = false;
    m_sampleRate = sampleRate;
    m_blockSize  = blockSize;

    if (!m_hrtfLeft.init(sofaPath, sampleRate, blockSize))
        return false;
    if (!m_hrtfRight.init(sofaPath, sampleRate, blockSize))
        return false;

    m_room.init(sampleRate, blockSize, sofaPath);

    const size_t N = static_cast<size_t>(blockSize);
    m_monoL     .assign(N, 0.0f);
    m_monoR     .assign(N, 0.0f);
    m_hrtfOutLL .assign(N, 0.0f);
    m_hrtfOutLR .assign(N, 0.0f);
    m_hrtfOutRL .assign(N, 0.0f);
    m_hrtfOutRR .assign(N, 0.0f);
    m_mixL      .assign(N, 0.0f);
    m_mixR      .assign(N, 0.0f);

    m_gainRamp.setTarget(1.0f, 0);

    m_ready = true;
    return true;
}

// Process one chunk of at most m_blockSize frames (internal).
void LeiaEngine::processChunk(const float* stereoIn, float* stereoOut, int frames)
{
    // Deinterleave stereo input into monoL, monoR.
    for (int i = 0; i < frames; ++i) {
        m_monoL[i] = stereoIn[2 * i];
        m_monoR[i] = stereoIn[2 * i + 1];
    }

    float azimuth   = m_azimuth.load(std::memory_order_relaxed);
    float elevation = m_elevation.load(std::memory_order_relaxed);

    // HRTF convolution: left input channel.
    m_hrtfLeft.process(m_monoL.data(),
                       m_hrtfOutLL.data(), m_hrtfOutLR.data(),
                       frames, azimuth, elevation);

    // HRTF convolution: right input channel.
    m_hrtfRight.process(m_monoR.data(),
                        m_hrtfOutRL.data(), m_hrtfOutRR.data(),
                        frames, azimuth, elevation);

    // Sum L/R HRTF outputs per ear, scaled by Clarity.
    float clarity = m_clarity.load(std::memory_order_relaxed) * 0.01f;
    for (int i = 0; i < frames; ++i) {
        m_mixL[i] = (m_hrtfOutLL[i] + m_hrtfOutRL[i]) * clarity;
        m_mixR[i] = (m_hrtfOutLR[i] + m_hrtfOutRR[i]) * clarity;
    }

    // Early reflections, gated and scaled by Width.
    float widthNorm = m_width.load(std::memory_order_relaxed) * 0.01f;
    bool reflOn = m_reflEnable.load(std::memory_order_relaxed);
    if (reflOn && widthNorm > 1e-6f) {
        m_room.setRoomSize(m_roomSize.load(std::memory_order_relaxed));
        m_room.setReflectionLevel(m_reflLevel.load(std::memory_order_relaxed));
        m_room.setRoomType(m_roomType.load(std::memory_order_relaxed));
        m_room.setEnabled(true);

        // ShoeboxRoom::process adds reflections in-place, so run on
        // scratch copies and add back scaled by width.
        std::memcpy(m_hrtfOutLL.data(), m_mixL.data(), frames * sizeof(float));
        std::memcpy(m_hrtfOutLR.data(), m_mixR.data(), frames * sizeof(float));
        m_room.process(m_hrtfOutLL.data(), m_hrtfOutLR.data(), frames,
                       azimuth, elevation);
        for (int i = 0; i < frames; ++i) {
            float reflL = m_hrtfOutLL[i] - m_mixL[i];
            float reflR = m_hrtfOutLR[i] - m_mixR[i];
            m_mixL[i] += reflL * widthNorm;
            m_mixR[i] += reflR * widthNorm;
        }
    }

    // Interleave L/R into stereo output.
    for (int i = 0; i < frames; ++i) {
        stereoOut[2 * i]     = m_mixL[i];
        stereoOut[2 * i + 1] = m_mixR[i];
    }
}

void LeiaEngine::processBlock(const float* stereoIn,
                               float*       stereoOut,
                               int          frames)
{
    if (frames <= 0) return;

    setFTZDAZ();

    int bs = m_blockSize;
    if (bs <= 0 || !m_ready) {
        // Not initialized — passthrough.
        std::memcpy(stereoOut, stereoIn, frames * 2 * sizeof(float));
        restoreFPU();
        return;
    }

    // Process in chunks of at most blockSize to prevent buffer overflow.
    int offset = 0;
    while (offset < frames) {
        int chunk = std::min(frames - offset, bs);
        processChunk(stereoIn + offset * 2, stereoOut + offset * 2, chunk);
        offset += chunk;
    }

    // Master gain ramp spans entire block.
    m_gainRamp.apply(stereoOut, frames, 2);

    // Post-engine soft peak limiter. Slow attack (15 ms) + long
    // release (200 ms) catches sustained overdrive without pumping on
    // transient rotational peaks. Ceiling 1.0 matches outer softLimit.
    {
        constexpr float kCeil = 1.0f;
        float peak = 0.0f;
        for (int i = 0; i < frames * 2; ++i) {
            float a = std::fabs(stereoOut[i]);
            if (a > peak) peak = a;
        }
        float targetGain = 1.0f;
        if (peak * m_postLimGain > kCeil) targetGain = kCeil / peak;
        const float attackCoef  = 1.0f - std::exp(-1.0f / (0.015f * m_sampleRate));
        const float releaseCoef = 1.0f - std::exp(-1.0f / (0.200f * m_sampleRate));
        float g = m_postLimGain;
        for (int i = 0; i < frames; ++i) {
            float coef = (targetGain < g) ? attackCoef : releaseCoef;
            g += coef * (targetGain - g);
            if (g < 0.1f) g = 0.1f;
            if (g > 1.0f) g = 1.0f;
            stereoOut[i * 2 + 0] *= g;
            stereoOut[i * 2 + 1] *= g;
        }
        m_postLimGain = g;
    }

    restoreFPU();
}

void LeiaEngine::setAzimuth(float deg)         { m_azimuth.store(deg, std::memory_order_relaxed); }
void LeiaEngine::setElevation(float deg)       { m_elevation.store(deg, std::memory_order_relaxed); }
void LeiaEngine::setReflectionEnable(bool on)  { m_reflEnable.store(on, std::memory_order_relaxed); }
void LeiaEngine::setReflectionLevel(float dB)  { m_reflLevel.store(dB, std::memory_order_relaxed); }
void LeiaEngine::setRoomSize(float meters)     { m_roomSize.store(meters, std::memory_order_relaxed); }
void LeiaEngine::setRoomType(int type)         { m_roomType.store(type, std::memory_order_relaxed); }
void LeiaEngine::setClarity(float pct)         { m_clarity.store(pct, std::memory_order_relaxed); }
void LeiaEngine::setWidth(float pct)           { m_width.store(pct, std::memory_order_relaxed); }

void LeiaEngine::setGain(float linearGain)
{
    int rampSamples = static_cast<int>(m_sampleRate * 0.005f);
    m_gainRamp.setTarget(linearGain, rampSamples);
}

int         LeiaEngine::roomTypeCount() const     { return ShoeboxRoom::numPresets(); }
const char* LeiaEngine::roomTypeName(int idx) const { return ShoeboxRoom::presetName(idx); }

void LeiaEngine::setSampleRate(double sr) { m_sampleRate = static_cast<float>(sr); }

void LeiaEngine::reset()
{
    m_hrtfLeft.reset();
    m_hrtfRight.reset();
    m_room.reset();

    if (!m_monoL.empty()) {
        std::memset(m_monoL.data(), 0, m_monoL.size() * sizeof(float));
        std::memset(m_monoR.data(), 0, m_monoR.size() * sizeof(float));
        std::memset(m_mixL.data(),  0, m_mixL.size()  * sizeof(float));
        std::memset(m_mixR.data(),  0, m_mixR.size()  * sizeof(float));
    }
    m_gainRamp.setTarget(1.0f, 0);
    m_postLimGain = 1.0f;
}

void LeiaEngine::setFTZDAZ()
{
#if LEIA_X86
    m_savedMXCSR = _mm_getcsr();
    _mm_setcsr(m_savedMXCSR | 0x8040);
#endif
}

void LeiaEngine::restoreFPU()
{
#if LEIA_X86
    _mm_setcsr(m_savedMXCSR);
#endif
}
