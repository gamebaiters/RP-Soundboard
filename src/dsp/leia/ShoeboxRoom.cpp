// ShoeboxRoom.cpp -- Image-source early reflections in a rectangular room.
// Uses simple stereo panning for reflections (not per-wall HRTF, which would
// need separate convolver state per wall).

#include "ShoeboxRoom.h"

#include <algorithm>
#include <cstring>

// Stable index ordering - new entries always appended so saved INIs
// keep loading correctly. Each preset carries: absorption[6] +
// lateFeedback + lateDamp + lateMix + tapLpHz + erDelayScale, so a
// preset can be characterised by far more than its absorption alone.
const ShoeboxRoom::RoomPreset ShoeboxRoom::kPresets[] = {
    // name           absorption[6]                                                fb     damp    mix    lpHz       erScale
    { "Drapes",       { 0.80f, 0.80f, 0.80f, 0.80f, 0.70f, 0.60f },              0.40f, 0.70f,  0.18f, 2400.0f,   1.0f },
    { "Studio",       { 0.50f, 0.50f, 0.50f, 0.50f, 0.45f, 0.40f },              0.55f, 0.45f,  0.28f, 7000.0f,   1.0f },
    { "Tiles",        { 0.20f, 0.20f, 0.20f, 0.20f, 0.25f, 0.15f },              0.78f, 0.18f,  0.45f, 12000.0f,  1.0f },
    { "Concrete",     { 0.08f, 0.08f, 0.08f, 0.08f, 0.10f, 0.05f },              0.86f, 0.12f,  0.55f, 14000.0f,  1.0f },
    { "Glass",        { 0.05f, 0.05f, 0.05f, 0.05f, 0.10f, 0.05f },              0.88f, 0.08f,  0.60f, 16000.0f,  1.0f },
    { "Living room",  { 0.55f, 0.55f, 0.60f, 0.60f, 0.75f, 0.45f },              0.50f, 0.55f,  0.22f, 5500.0f,   1.0f },
    { "Wood cabin",   { 0.40f, 0.40f, 0.40f, 0.40f, 0.50f, 0.45f },              0.58f, 0.42f,  0.30f, 6500.0f,   1.0f },
    { "Hall",         { 0.18f, 0.18f, 0.18f, 0.18f, 0.55f, 0.35f },              0.86f, 0.22f,  0.62f, 11000.0f,  1.3f },
    { "Cathedral",    { 0.06f, 0.06f, 0.06f, 0.06f, 0.10f, 0.08f },              0.92f, 0.18f,  0.85f, 9000.0f,   1.6f },
    { "Bathroom",     { 0.08f, 0.08f, 0.08f, 0.08f, 0.08f, 0.10f },              0.82f, 0.05f,  0.55f, 18000.0f,  0.7f },
    { "Car",          { 0.70f, 0.70f, 0.75f, 0.75f, 0.65f, 0.70f },              0.30f, 0.65f,  0.15f, 3500.0f,   0.6f },
    { "Outdoor",      { 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f },              0.00f, 0.85f,  0.00f, 18000.0f,  1.0f },
    { "Underwater",   { 0.45f, 0.45f, 0.45f, 0.45f, 0.30f, 0.40f },              0.65f, 0.80f,  0.55f, 600.0f,    0.9f },
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
    m_delayRamp   .assign(static_cast<size_t>(blockSize), 0.0f);

    // Schroeder diffuse tail initialisation. Comb delays + allpass
    // delays from Freeverb (Jezar) scaled to the active sample rate.
    // Delays are coprime-ish to break the periodicity of the feedback
    // loops, which is what gives a smooth "wash" instead of a metallic
    // ring. Right channel uses a small offset so the stereo image is
    // mildly decorrelated.
    const float scale = sampleRate / 44100.0f;
    const int combDelays[kNumCombs] = {
        int(1116 * scale), int(1188 * scale),
        int(1277 * scale), int(1356 * scale)
    };
    const int combStereoSpread = int(23 * scale);
    for (int c = 0; c < kNumCombs; ++c) {
        int dL = combDelays[c];
        int dR = combDelays[c] + combStereoSpread;
        m_combs[c].bufL.assign(static_cast<size_t>(dL), 0.0f);
        m_combs[c].bufR.assign(static_cast<size_t>(dR), 0.0f);
        m_combs[c].idx = 0;
        m_combs[c].dampL = 0.0f;
        m_combs[c].dampR = 0.0f;
        m_combs[c].feedback = 0.84f;
        m_combs[c].damp     = 0.20f;
    }
    const int allpassDelays[kNumAllpass] = {
        int(556 * scale), int(441 * scale)
    };
    for (int a = 0; a < kNumAllpass; ++a) {
        int d = allpassDelays[a];
        m_allpass[a].bufL.assign(static_cast<size_t>(d), 0.0f);
        m_allpass[a].bufR.assign(static_cast<size_t>(d + combStereoSpread), 0.0f);
        m_allpass[a].idx = 0;
        m_allpass[a].feedback = 0.5f;
    }
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
    const RoomPreset &p = kPresets[type];
    std::memcpy(m_absorption, p.absorption, sizeof(m_absorption));
    // Clamp aggressive Cathedral-class feedback. 0.92 + signal already
    // EQ-boosted into the Spatial input could drive the comb network
    // to converge above unity, which then fed the post-engine limiter
    // continuously and produced rotation-rate AM = frying. 0.88 keeps
    // the perceived decay tail close to the original while bounding
    // the loop gain comfortably below the limiter wakeup point.
    float feedbackClamped = p.lateFeedback;
    if (feedbackClamped > 0.88f) feedbackClamped = 0.88f;
    m_presetLateFeedback.store(feedbackClamped,    std::memory_order_relaxed);
    m_presetLateDamp    .store(p.lateDamp,         std::memory_order_relaxed);
    m_presetLateMix     .store(p.lateMix,          std::memory_order_relaxed);
    m_presetTapLpHz     .store(p.tapLpHz,          std::memory_order_relaxed);
    m_presetErDelayScale.store(p.erDelayScale,     std::memory_order_relaxed);
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

    // Accumulate per-tap wet into scratch buffers - then mix BOTH the
    // direct early-reflection wet AND a Schroeder-derived diffuse late
    // tail into the output. The tail is the difference between this
    // ShoeboxRoom's output and the old "discrete echoes only" code: it
    // smooths the decay so the room sounds like a real space instead of
    // 6 ping-pong echoes.
    std::fill_n(m_leftScratch .begin(), frames, 0.0f);
    std::fill_n(m_rightScratch.begin(), frames, 0.0f);

    for (int w = 0; w < kNumWalls; ++w) {
        ReflectionTap& tap = m_taps[w];
        if (tap.gain < 1e-6f) continue;

        // Crackle fix: per-sample fractional read across the block so
        // a rotating source does not jump delay every block boundary.
        float prev = tap.prevDelaySamples;
        if (prev < 0.0f) prev = tap.delaySamples;
        float target = tap.delaySamples;
        float dD = (frames > 0) ? (target - prev) / static_cast<float>(frames)
                                : 0.0f;
        for (int i = 0; i < frames; ++i)
            m_delayRamp[i] = prev + dD * static_cast<float>(i);
        tap.buffer.readFractional(m_delayRamp.data(),
                                   m_monoScratch.data(), frames);
        tap.prevDelaySamples = target;

        float g = tap.gain * m_reflLevel;

        // sin(az) panning - continuous through the -180/+180 wrap.
        float azRad = tap.azimuthDeg * static_cast<float>(M_PI / 180.0);
        float lateral = std::sin(azRad);
        if (lateral < -1.0f) lateral = -1.0f;
        if (lateral >  1.0f) lateral =  1.0f;
        float azNorm = (lateral + 1.0f) * 0.5f;
        float panR = azNorm;
        float panL = 1.0f - azNorm;
        float gainLTarget = g * std::sqrt(panL);
        float gainRTarget = g * std::sqrt(panR);

        // Per-sample lerp of pan gain across the block. The previous
        // implementation locked gain at the block boundary which produced
        // a step every ~5 ms on a rotating source - audible as a fast
        // crackle / fry riding the reflection. Slewing per sample
        // eliminates the boundary, the lerp time constant matches the
        // rotation slew already applied to the delay line.
        float prevGL = tap.prevGainL;
        float prevGR = tap.prevGainR;
        if (prevGL == 0.0f && prevGR == 0.0f) {
            prevGL = gainLTarget;
            prevGR = gainRTarget;
        }
        float dGL = (gainLTarget - prevGL) / static_cast<float>(frames);
        float dGR = (gainRTarget - prevGR) / static_cast<float>(frames);
        for (int i = 0; i < frames; ++i) {
            float gl = prevGL + dGL * static_cast<float>(i);
            float gr = prevGR + dGR * static_cast<float>(i);
            float x = m_monoScratch[i];
            tap.lpStateL += (1.0f - tap.lpCoef) * (x * gl - tap.lpStateL);
            tap.lpStateR += (1.0f - tap.lpCoef) * (x * gr - tap.lpStateR);
            m_leftScratch [i] += tap.lpStateL;
            m_rightScratch[i] += tap.lpStateR;
        }
        tap.prevGainL = gainLTarget;
        tap.prevGainR = gainRTarget;
    }

    // ---- Schroeder diffuse late tail ------------------------------------
    // Feed the DRY signal (leftIO + rightIO mono mix) into the comb
    // network so the tail produces a real audible wash. The previous
    // build fed the early-reflection wet into the combs, but those wets
    // were already ~30 dB below source so even Cathedral feedback of
    // 0.92 produced an inaudible tail. Drying the input lets each
    // preset show its actual character (Underwater LP-heavy mud,
    // Cathedral long wash, Outdoor stays silent because lateLevel=0).
    //
    // 0.5x input attenuation: combined with the feedback clamp in
    // setRoomType this puts the loop gain comfortably below the post-
    // engine limiter wakeup ceiling even when the upstream chain
    // (EQ + Saturator + Compressor) has already pushed peaks toward 1.0.
    float lateMix = m_lateLevel * m_reflLevel;
    if (lateMix > 1e-6f) {
        for (int i = 0; i < frames; ++i) {
            float dryMono = (leftIO[i] + rightIO[i]) * 0.5f * 0.5f;
            float inL = dryMono;
            float inR = dryMono;
            float combOutL = 0.0f, combOutR = 0.0f;
            for (int c = 0; c < kNumCombs; ++c) {
                CombFilter &cf = m_combs[c];
                int szL = static_cast<int>(cf.bufL.size());
                int szR = static_cast<int>(cf.bufR.size());
                int idxL = cf.idx % szL;
                int idxR = cf.idx % szR;
                float yL = cf.bufL[idxL];
                float yR = cf.bufR[idxR];
                // One-pole HF damping inside the loop. Smoother decay
                // tail; less metallic ring.
                cf.dampL = yL * (1.0f - cf.damp) + cf.dampL * cf.damp;
                cf.dampR = yR * (1.0f - cf.damp) + cf.dampR * cf.damp;
                cf.bufL[idxL] = inL + cf.dampL * cf.feedback;
                cf.bufR[idxR] = inR + cf.dampR * cf.feedback;
                combOutL += yL;
                combOutR += yR;
            }
            for (int c = 0; c < kNumCombs; ++c) m_combs[c].idx++;

            // Series allpasses for diffusion.
            float yL = combOutL;
            float yR = combOutR;
            for (int a = 0; a < kNumAllpass; ++a) {
                AllPass &ap = m_allpass[a];
                int szL = static_cast<int>(ap.bufL.size());
                int szR = static_cast<int>(ap.bufR.size());
                int idxL = ap.idx % szL;
                int idxR = ap.idx % szR;
                float bL = ap.bufL[idxL];
                float bR = ap.bufR[idxR];
                float outL = -yL + bL;
                float outR = -yR + bR;
                ap.bufL[idxL] = yL + bL * ap.feedback;
                ap.bufR[idxR] = yR + bR * ap.feedback;
                yL = outL;
                yR = outR;
                ap.idx++;
            }

            // DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]. R = 0.995
            // gives -3dB at ~38 Hz at 48 kHz - kills the slow DC
            // accumulation in the comb loops without touching musical
            // content. Stops Cathedral / Concrete preset from biasing
            // the post-engine limiter into permanent compression.
            constexpr float kR = 0.995f;
            float xL = yL;
            float xR = yR;
            yL = xL - m_dcLastInL + kR * m_dcLastOutL;
            yR = xR - m_dcLastInR + kR * m_dcLastOutR;
            m_dcLastInL  = xL;
            m_dcLastInR  = xR;
            m_dcLastOutL = yL;
            m_dcLastOutR = yR;

            m_leftScratch [i] += yL * lateMix;
            m_rightScratch[i] += yR * lateMix;
        }
    }

    // Sum the combined early + late wet into the output buffers.
    for (int i = 0; i < frames; ++i) {
        leftIO [i] += m_leftScratch [i];
        rightIO[i] += m_rightScratch[i];
    }
}

void ShoeboxRoom::computeReflections(float srcAz, float srcEl)
{
    // Snapshot preset atomics ONCE per block so every wall + every
    // comb sees the same value even if GUI fires setRoomType mid-call.
    const float erDelayScale = m_presetErDelayScale.load(std::memory_order_relaxed);
    const float tapLpHz      = m_presetTapLpHz    .load(std::memory_order_relaxed);
    const float lateFeedback = m_presetLateFeedback.load(std::memory_order_relaxed);
    const float lateDamp     = m_presetLateDamp   .load(std::memory_order_relaxed);
    const float lateMix      = m_presetLateMix    .load(std::memory_order_relaxed);

    for (int w = 0; w < kNumWalls; ++w) {
        float az = 0.0f, el = 0.0f, dist = 0.0f;
        computeImageSource(w, srcAz, srcEl, az, el, dist);

        m_taps[w].azimuthDeg   = az;
        m_taps[w].elevationDeg = el;

        static constexpr float kSpeedOfSound = 343.0f;
        float delaySamplesF = (dist * erDelayScale) / kSpeedOfSound * m_sampleRate;

        float maxDelay = static_cast<float>(m_taps[w].buffer.capacity - m_blockSize - 2);
        if (maxDelay < 0.0f) maxDelay = 0.0f;
        if (delaySamplesF < 0.0f)        delaySamplesF = 0.0f;
        if (delaySamplesF > maxDelay)    delaySamplesF = maxDelay;
        m_taps[w].delaySamples = delaySamplesF;

        float absCoef = m_absorption[w];
        m_taps[w].gain = (1.0f - absCoef) / std::max(dist, 0.1f);

        // Frequency-dependent damping. Per-preset tapLpHz scales the
        // per-wall absorption curve - Bathroom keeps 18 kHz, Underwater
        // forces a hard 600 Hz LP no matter the surface absorption.
        float cutoffHz = 200.0f + (1.0f - absCoef) * (tapLpHz - 200.0f);
        if (cutoffHz < 100.0f)  cutoffHz = 100.0f;
        if (cutoffHz > 18000.0f) cutoffHz = 18000.0f;
        float a = std::exp(-2.0f * static_cast<float>(M_PI) * cutoffHz / m_sampleRate);
        if (a < 0.0f)   a = 0.0f;
        if (a > 0.999f) a = 0.999f;
        m_taps[w].lpCoef = a;
    }

    // Late-tail character is driven by the active preset, not derived
    // from absorption alone. Outdoor explicitly sends 0 wet so it stays
    // dry. Underwater has heavy damping. Cathedral feedback near 0.92
    // (already clamped to 0.88 in setRoomType).
    for (int c = 0; c < kNumCombs; ++c) {
        m_combs[c].feedback = lateFeedback;
        m_combs[c].damp     = lateDamp;
    }
    m_lateLevel = lateMix;
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
    for (int w = 0; w < kNumWalls; ++w) {
        m_taps[w].buffer.clear();
        m_taps[w].prevDelaySamples = -1.0f;
        m_taps[w].lpStateL = 0.0f;
        m_taps[w].lpStateR = 0.0f;
        m_taps[w].prevGainL = 0.0f;
        m_taps[w].prevGainR = 0.0f;
    }
    // Drain the Schroeder network so a stop/resume does not start the
    // next playback with a tail from the previous source.
    for (int c = 0; c < kNumCombs; ++c) {
        std::fill(m_combs[c].bufL.begin(), m_combs[c].bufL.end(), 0.0f);
        std::fill(m_combs[c].bufR.begin(), m_combs[c].bufR.end(), 0.0f);
        m_combs[c].dampL = 0.0f;
        m_combs[c].dampR = 0.0f;
        m_combs[c].idx = 0;
    }
    for (int a = 0; a < kNumAllpass; ++a) {
        std::fill(m_allpass[a].bufL.begin(), m_allpass[a].bufL.end(), 0.0f);
        std::fill(m_allpass[a].bufR.begin(), m_allpass[a].bufR.end(), 0.0f);
        m_allpass[a].idx = 0;
    }
    // DC-blocker state.
    m_dcLastInL  = 0.0f;
    m_dcLastInR  = 0.0f;
    m_dcLastOutL = 0.0f;
    m_dcLastOutR = 0.0f;
    // m_lateLevel / m_reflLevel are intentionally NOT zeroed here -
    // they get re-applied by setRoomType + setReflectionLevel on the
    // next applyState() call, and zeroing them between would create
    // a brief silent-reflection window on slot reuse.
}
