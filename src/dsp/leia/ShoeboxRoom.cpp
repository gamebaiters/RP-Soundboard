// ShoeboxRoom.cpp -- Image-source early reflections in a rectangular room.
// Uses simple stereo panning for reflections (not per-wall HRTF, which would
// need separate convolver state per wall).

#include "ShoeboxRoom.h"

#include <algorithm>
#include <cstring>

const ShoeboxRoom::RoomPreset ShoeboxRoom::kPresets[] = {
    { "Drapes",   { 0.80f, 0.80f, 0.80f, 0.80f, 0.70f, 0.60f } },
    { "Studio",   { 0.50f, 0.50f, 0.50f, 0.50f, 0.45f, 0.40f } },
    { "Tiles",    { 0.20f, 0.20f, 0.20f, 0.20f, 0.25f, 0.15f } },
    { "Concrete", { 0.08f, 0.08f, 0.08f, 0.08f, 0.10f, 0.05f } },
    { "Glass",    { 0.05f, 0.05f, 0.05f, 0.05f, 0.10f, 0.05f } },
};

const int ShoeboxRoom::kNumPresets =
    static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

ShoeboxRoom::ShoeboxRoom()  = default;
ShoeboxRoom::~ShoeboxRoom() = default;

void ShoeboxRoom::init(float sampleRate, int blockSize,
                        const std::string& sofaPath)
{
    (void)sofaPath;
    m_sampleRate = sampleRate;
    m_blockSize  = blockSize;

    const float kSmoothTime = 0.2f;
    m_smoothWidth .init(sampleRate, kSmoothTime, m_width);
    m_smoothHeight.init(sampleRate, kSmoothTime, m_height);
    m_smoothDepth .init(sampleRate, kSmoothTime, m_depth);

    const int kRingCap = 12000;
    for (int w = 0; w < kNumWalls; ++w)
        m_taps[w].buffer.init(kRingCap);

    m_monoScratch .assign(static_cast<size_t>(blockSize), 0.0f);
    m_leftScratch .assign(static_cast<size_t>(blockSize), 0.0f);
    m_rightScratch.assign(static_cast<size_t>(blockSize), 0.0f);
}

void ShoeboxRoom::setRoomSize(float meters)
{
    meters = std::max(7.0f, std::min(meters, 50.0f));
    m_width  = meters;
    m_depth  = meters;
    m_height = std::max(2.5f, meters * 0.30f);

    m_smoothWidth .setTarget(m_width);
    m_smoothHeight.setTarget(m_height);
    m_smoothDepth .setTarget(m_depth);
}

void ShoeboxRoom::setReflectionLevel(float dB)
{
    dB = std::max(-25.0f, std::min(dB, 20.0f));
    m_reflLevel = powf(10.0f, dB / 20.0f);
}

void ShoeboxRoom::setRoomType(int type)
{
    if (type < 0 || type >= kNumPresets) return;
    std::memcpy(m_absorption, kPresets[type].absorption, sizeof(m_absorption));
}

void ShoeboxRoom::setEnabled(bool on) { m_enabled = on; }

int         ShoeboxRoom::numPresets()        { return kNumPresets; }
const char* ShoeboxRoom::presetName(int idx)
{
    if (idx < 0 || idx >= kNumPresets) return "Unknown";
    return kPresets[idx].name;
}

void ShoeboxRoom::process(float* leftIO, float* rightIO, int frames,
                           float sourceAzimuthDeg, float sourceElevationDeg)
{
    if (!m_enabled || frames <= 0) return;
    if (frames > m_blockSize) frames = m_blockSize;

    m_smoothWidth .process();
    m_smoothHeight.process();
    m_smoothDepth .process();

    computeReflections(sourceAzimuthDeg, sourceElevationDeg);

    // Build mono input from average of L+R.
    for (int i = 0; i < frames; ++i)
        m_monoScratch[i] = (leftIO[i] + rightIO[i]) * 0.5f;

    // Feed every wall's delay line with mono signal.
    for (int w = 0; w < kNumWalls; ++w)
        m_taps[w].buffer.write(m_monoScratch.data(), frames);

    // For each reflection: read delayed mono, apply gain, pan to stereo, sum in.
    for (int w = 0; w < kNumWalls; ++w) {
        const ReflectionTap& tap = m_taps[w];
        if (tap.gain < 1e-6f) continue;

        tap.buffer.readFixed(tap.delaySamples, m_monoScratch.data(), frames);

        float g = tap.gain * m_reflLevel;

        // Pan from sin(azimuth), not (az+180)/360. The linear mapping
        // jumped L<->R at the -180/+180 wrap when a rotating source
        // crossed straight-behind; sin() is continuous and collapses
        // front/rear to centre, matching the median-plane cue.
        float azRad = tap.azimuthDeg * static_cast<float>(M_PI / 180.0);
        float lateral = std::sin(azRad);
        if (lateral < -1.0f) lateral = -1.0f;
        if (lateral >  1.0f) lateral =  1.0f;
        float azNorm = (lateral + 1.0f) * 0.5f;
        float panR = azNorm;
        float panL = 1.0f - azNorm;
        float gainL = g * std::sqrt(panL);
        float gainR = g * std::sqrt(panR);

        for (int i = 0; i < frames; ++i) {
            leftIO[i]  += m_monoScratch[i] * gainL;
            rightIO[i] += m_monoScratch[i] * gainR;
        }
    }
}

void ShoeboxRoom::computeReflections(float srcAz, float srcEl)
{
    for (int w = 0; w < kNumWalls; ++w) {
        float az = 0.0f, el = 0.0f, dist = 0.0f;
        computeImageSource(w, srcAz, srcEl, az, el, dist);

        m_taps[w].azimuthDeg   = az;
        m_taps[w].elevationDeg = el;

        static constexpr float kSpeedOfSound = 343.0f;
        int delaySamples = static_cast<int>(dist / kSpeedOfSound * m_sampleRate + 0.5f);

        int maxDelay = m_taps[w].buffer.capacity - m_blockSize - 1;
        if (maxDelay < 0) maxDelay = 0;
        m_taps[w].delaySamples = std::min(delaySamples, maxDelay);

        float absCoef = m_absorption[w];
        m_taps[w].gain = (1.0f - absCoef) / std::max(dist, 0.1f);
    }
}

void ShoeboxRoom::computeImageSource(int   wallIdx,
                                      float srcAz,
                                      float srcEl,
                                      float& outAz,
                                      float& outEl,
                                      float& outDist)
{
    float azRad = srcAz * static_cast<float>(M_PI / 180.0);
    float elRad = srcEl * static_cast<float>(M_PI / 180.0);

    float cosEl = cosf(elRad);
    float sx = cosEl * cosf(azRad);
    float sy = cosEl * sinf(azRad);
    float sz = sinf(elRad);

    float curW = m_smoothWidth .current();
    float curH = m_smoothHeight.current();
    float curD = m_smoothDepth .current();

    const float kSrcDist = 1.0f;
    float px = sx * kSrcDist;
    float py = sy * kSrcDist;
    float pz = sz * kSrcDist;

    float ix = px, iy = py, iz = pz;

    float halfW = curW * 0.5f;
    float halfD = curD * 0.5f;
    float halfH = curH * 0.5f;

    switch (wallIdx) {
        case 0: ix =  2.0f * halfW - px;  break;
        case 1: ix = -2.0f * halfW - px;  break;
        case 2: iy =  2.0f * halfD - py;  break;
        case 3: iy = -2.0f * halfD - py;  break;
        case 4: iz =  2.0f * halfH - pz;  break;
        case 5: iz = -2.0f * halfH - pz;  break;
        default: break;
    }

    outDist = sqrtf(ix * ix + iy * iy + iz * iz);
    if (outDist < 1e-6f) outDist = 1e-6f;

    outAz = atan2f(iy, ix) * static_cast<float>(180.0 / M_PI);
    outEl = asinf(std::max(-1.0f, std::min(1.0f, iz / outDist)))
          * static_cast<float>(180.0 / M_PI);
}

void ShoeboxRoom::reset()
{
    for (int w = 0; w < kNumWalls; ++w)
        m_taps[w].buffer.clear();
}
