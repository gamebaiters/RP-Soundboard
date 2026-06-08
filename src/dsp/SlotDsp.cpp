#include "SlotDsp.h"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace {
// Tiny RAII timer that adds elapsed ns to a pair of atomic counters at
// destruction. Used to wrap SlotDsp::process and produceStretchedShort
// without sprinkling timer logic through both functions.
struct ScopedCpuTimer {
    std::atomic<int64_t> &ns;
    std::atomic<int64_t> &frames;
    int  framesProcessed;
    std::chrono::steady_clock::time_point start;
    ScopedCpuTimer(std::atomic<int64_t> &nsOut,
                   std::atomic<int64_t> &framesOut,
                   int framesIn)
        : ns(nsOut), frames(framesOut), framesProcessed(framesIn),
          start(std::chrono::steady_clock::now()) {}
    ~ScopedCpuTimer() {
        auto end = std::chrono::steady_clock::now();
        int64_t took = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           end - start).count();
        ns.fetch_add(took, std::memory_order_relaxed);
        frames.fetch_add(framesProcessed, std::memory_order_relaxed);
    }
};
} // namespace

void SlotDsp::getEqBandLevels(float out[16]) const
{
    for (int i = 0; i < 16; ++i)
        out[i] = m_play.eq.bandLevel(i);
}

double SlotDsp::cpuPercent()
{
    int64_t ns     = m_cpuNs    .exchange(0, std::memory_order_relaxed);
    int64_t frames = m_cpuFrames.exchange(0, std::memory_order_relaxed);
    if (frames <= 0 || m_fs <= 0.0) return 0.0;
    double realTimeNs = (double)frames / m_fs * 1e9;
    if (realTimeNs <= 0.0) return 0.0;
    return 100.0 * (double)ns / realTimeNs;
}

namespace {
inline short clampToShort(float x) {
    if (x > 32767.0f)  return 32767;
    if (x < -32768.0f) return -32768;
    return static_cast<short>(x);
}
// Anti-denormal injection. IIR cascades (EQ biquads + Reverb combs +
// Positional shadow filters) decay state values toward zero on silent
// input. Once values drop below ~1.18e-38 they hit the float subnormal
// range: MSVC falls back to a microcoded path 100x slower and the
// values pollute next-sample math with quantisation noise that sounds
// like robotic hiss/buzz under near-silent material. A tiny alternating
// bias keeps every state above the denormal floor; the bias rounds to
// zero in the int16 output so it never reaches the listener.
inline float antiDenormDither(int n) {
    constexpr float kEps = 1e-20f;
    return (n & 1) ? kEps : -kEps;
}
// Soft limiter with strict bound to [-1, +1]. T=0.95 -> only the last
// 0.5 dB of headroom triggers compression; normal program material
// passes through linearly.
inline float softLimit(float x) {
    constexpr float T = 0.95f;
    constexpr float A = 0.05f;
    float ax = std::fabs(x);
    if (ax <= T) return x;
    float over = ax - T;
    float compressed = T + A * over / (over + A);
    return (x < 0) ? -compressed : compressed;
}
}

SlotDsp::SlotDsp() {
    setSampleRate(48000.0);
}

void SlotDsp::setSampleRate(double sr) {
    double newFs = sr > 0 ? sr : 48000.0;
    // Skip work when nothing changed. setSlotSandboxState calls this
    // every time the user moves a slider; without the guard each call
    // would hammer reverb buffer reallocation and wipe the comb /
    // allpass tails -> audible click on every parameter change.
    if (std::abs(newFs - m_fs) < 0.5) return;
    m_fs = newFs;
    auto initPath = [this](PathState &p){
        p.eq.setSampleRate(m_fs);
        p.comp.setSampleRate(m_fs);
        p.sat.setSampleRate(m_fs);
        p.posL.setSampleRate(m_fs);
        p.posR.setSampleRate(m_fs);
        p.leia.setSampleRate(m_fs);
        p.chorus.setSampleRate(m_fs);
        p.flanger.setSampleRate(m_fs);
        p.flangus.setSampleRate(m_fs);
        p.phaser.setSampleRate(m_fs);
        p.delay.setSampleRate(m_fs);
        p.reverb.setSampleRate(m_fs);
        p.limiter.setSampleRate(m_fs);
        p.bitcrusher.setSampleRate(m_fs);
        p.genLoss.setSampleRate(m_fs);
    };
    initPath(m_play);
    initPath(m_cap);
    m_stretchPlay.ps.setSampleRate(m_fs);
    m_stretchCap.ps.setSampleRate(m_fs);
    double tau = 0.200;
    m_peakDecay = static_cast<float>(std::exp(-1.0 / (tau * m_fs)));
}

void SlotDsp::reset() {
    auto resetPath = [](PathState &p){
        p.eq.reset();
        p.comp.reset();
        p.sat.reset();
        p.posL.reset();
        p.posR.reset();
        p.leia.reset();
        p.chorus.reset();
        p.flanger.reset();
        p.flangus.reset();
        p.phaser.reset();
        p.delay.reset();
        p.reverb.reset();
        p.limiter.reset();
        p.bitcrusher.reset();
        p.genLoss.reset();
        p.rotPhase = 0.0;
        p.rotBlockCounter = 0;
    };
    resetPath(m_play);
    resetPath(m_cap);
    auto resetStretch = [](StretchState &s) {
        s.ps.reset();
        if (!s.buf.empty()) {
            std::fill(s.buf.begin(), s.buf.end(), 0.0f);
            s.ps.updateSourceFrames(0);
        }
        s.write = 0;
    };
    resetStretch(m_stretchPlay);
    resetStretch(m_stretchCap);
    m_peakL = m_peakR = 0.0f;
}

void SlotDsp::resetPeak() {
    m_peakL = m_peakR = 0.0f;
}

void SlotDsp::setFxReverbWet(float wet) {
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 1.0f) wet = 1.0f;
    m_fxReverbWet = wet;
    refreshReverbWet();
    recomputeActive();
}

void SlotDsp::refreshReverbWet() {
    float combined = m_state.reverbWet + m_fxReverbWet;
    if (combined < 0.0f) combined = 0.0f;
    if (combined > 1.0f) combined = 1.0f;
    m_play.reverb.setWet(combined);
    m_cap.reverb.setWet(combined);
}

void SlotDsp::recomputeActive() {
    const auto &s = m_state;
    bool spatialActive =
        s.spatialMode == SandboxState::Spatial_3DManual ||
        s.spatialMode == SandboxState::Spatial_3DRotate ||
        s.spatialMode == SandboxState::Spatial_8DPreset ||
        (s.spatialMode == SandboxState::Spatial_LRPan && std::abs(s.panValue) > 0.001f);
    bool eqActive = s.eqEnabled && std::any_of(std::begin(s.eqBandDb), std::end(s.eqBandDb),
                                                 [](float v){ return std::abs(v) > 0.001f; });
    bool reverbActive = (s.reverbWet + m_fxReverbWet) > 0.001f;
    bool newFxActive = s.compEnabled ||
                       (s.chorusEnabled && s.chorusMix > 0.001f) ||
                       (s.flangerEnabled && s.flangerMix > 0.001f) ||
                       (s.flangusEnabled && s.flangusMix > 0.001f) ||
                       (s.phaserEnabled && s.phaserMix > 0.001f) ||
                       (s.saturatorEnabled && s.saturatorMix > 0.001f) ||
                       (s.delayEnabled && s.delayMix > 0.001f) ||
                       s.limiterEnabled ||
                       s.bitcrusherEnabled ||
                       s.monoEnabled ||
                       s.genLossEnabled;
    bool sandboxActive = s.enabled && (spatialActive || eqActive || reverbActive ||
                                        s.headSway || s.stretchEnabled || newFxActive);
    m_active = sandboxActive || m_fxReverbWet > 0.001f;
}

int SlotDsp::inputFramesNeededFor(int outputFrames) const {
    if (!m_state.stretchEnabled) return outputFrames;
    float f = m_state.stretchFactor;
    if (f < 1.0f) f = 1.0f;
    int n = static_cast<int>(std::ceil(outputFrames / f));
    n = std::max(1, n);
    if (m_stretchPlay.inited && m_stretchPlay.ps.isPriming())
        n = std::max(n, outputFrames);
    return n;
}

double SlotDsp::stretchPlaybackPosition() const {
    if (!m_stretchPlay.inited || m_fs < 1.0) return 0.0;
    return m_stretchPlay.ps.currentFrame() / m_fs;
}

bool SlotDsp::stretchCaptureDone() const {
    if (!m_stretchCap.inited) return true;
    return m_stretchCap.ps.hasProcessedAllSource();
}

void SlotDsp::initStretchStateIfNeeded(StretchState &s) {
    if (s.inited) return;
    constexpr double kStretchSeconds = 30.0;
    int cap = static_cast<int>(kStretchSeconds * m_fs);
    if (cap < 1024) cap = 1024;
    s.buf.assign(static_cast<size_t>(cap) * 2, 0.0f);
    s.write = 0;
    s.inited = true;
    s.ps.setSampleRate(m_fs);
    s.ps.setSource(s.buf.data(), 0);
    s.ps.setEnabled(true);
}

void SlotDsp::feedStretchShort(const short *interleaved, int frames, bool isCapture) {
    StretchState &s = isCapture ? m_stretchCap : m_stretchPlay;
    initStretchStateIfNeeded(s);
    int cap = static_cast<int>(s.buf.size() / 2);
    constexpr float kInv = 1.0f / 32768.0f;
    for (int i = 0; i < frames; ++i) {
        int idx = s.write % cap;
        s.buf[idx * 2 + 0] = interleaved[i * 2 + 0] * kInv;
        s.buf[idx * 2 + 1] = interleaved[i * 2 + 1] * kInv;
        ++s.write;
    }
    int avail = std::min(s.write, cap);
    s.ps.updateSourceFrames(avail);
    s.ps.setStretchFactor(m_state.stretchFactor);
    s.ps.setWindowMs(m_state.stretchWindowMs);
}

void SlotDsp::prepareLeia(double fs) {
    if (fs <= 0.0) fs = m_fs > 0.0 ? m_fs : 48000.0;
    // ensureInit is idempotent after the first successful call (atomic
    // ready flag short-circuits). The two-path init runs only the FIRST
    // time Leia is selected; subsequent calls are a few atomic loads.
    m_play.leia.ensureInit(fs);
    m_cap.leia.ensureInit(fs);
}

void SlotDsp::applyState(const SandboxState &s) {
    int oldMode = m_state.spatialMode;
    int oldEngine = m_state.spatialEngine;
    bool oldStretch = m_state.stretchEnabled;
    m_state = s;

    // Mode transitions = reset filter state on both paths. Without
    // this, switching from Off / Pan into a 3D mode left EQ biquad
    // states + reverb tails from the prior mode in the chain - the
    // HRTF stage suddenly fed those into its own filters and produced
    // a sustained robotic transient until everything settled. Same
    // protection on stretch toggle so re-enabling never inherits a
    // half-flushed filter.
    bool modeChanged = (oldMode != s.spatialMode) ||
                       (oldEngine != s.spatialEngine) ||
                       (oldStretch != s.stretchEnabled);
    if (modeChanged) {
        auto resetFull = [](PathState &p){
            p.eq.reset();
            p.comp.reset();
            p.sat.reset();
            p.posL.reset();
            p.posR.reset();
            p.chorus.reset();
            p.flanger.reset();
            p.flangus.reset();
            p.phaser.reset();
            p.delay.reset();
            p.reverb.reset();
            p.limiter.reset();
            p.bitcrusher.reset();
            p.rotPhase = 0.0;
            p.rotBlockCounter = 0;
        };
        resetFull(m_play);
        resetFull(m_cap);
    }

    recomputeActive();

    auto applyToPath = [this, &s](PathState &p){
        p.reverb.setRoomSize(0.5f);
        p.reverb.setDamping(0.5f);
        for (int i = 0; i < 16; ++i)
            p.eq.setBandGainDb(i, s.eqBandDb[i]);
        float swayDeg = s.headSway ? 1.5f : 0.0f;
        p.posL.setHeadSwayAmount(swayDeg);
        p.posR.setHeadSwayAmount(swayDeg);

        p.comp.setParams(s.compThresholdDb, s.compRatio, s.compAttackMs,
                         s.compReleaseMs, s.compKneeDb, s.compMakeupDb);
        p.sat.setParams(s.saturatorDrive, s.saturatorMix, s.saturatorTone,
                        static_cast<Saturator::Mode>(s.saturatorMode));
        p.chorus.setParams(s.chorusRate, s.chorusDepth, s.chorusBaseDelay,
                           s.chorusVoices, s.chorusMix);
        p.flanger.setParams(s.flangerRate, s.flangerDepth, s.flangerFeedback,
                            s.flangerBaseDelay, s.flangerMix);
        p.flangus.setParams(s.flangusRate, s.flangusDepth, s.flangusFeedback,
                            s.flangusVoices, s.flangusSpread, s.flangusMix);
        p.phaser.setParams(s.phaserRate, s.phaserDepth, s.phaserFeedback,
                           s.phaserStages, s.phaserMix);
        p.delay.setParams(s.delayTimeMs, s.delayFeedback, s.delayMix,
                          s.delayDamping, s.delayPingPong);
        p.limiter.setParams(s.limiterCeiling, s.limiterLookahead, s.limiterRelease,
                            static_cast<Limiter::Mode>(s.limiterMode),
                            s.limiterRatio, s.limiterGateThresh);
        p.bitcrusher.setParams(s.bitcrusherBitDepth, s.bitcrusherRate);
        p.genLoss.setGenerations(s.genLossGenerations);

        // Leia (measured-HRTF) engine. The heavy SOFA load happens
        // lazily here on the GUI thread, and only when the user has
        // actually picked Leia for a 3D mode - otherwise the instance
        // stays a zero-cost no-op. If init fails (missing/bad SOFA),
        // leia.ready() stays false and applyStage falls back to the
        // Classic engine, so audio is never broken.
        bool leia3D = (s.spatialEngine == SandboxState::Engine_Leia) &&
                      (s.spatialMode == SandboxState::Spatial_3DManual ||
                       s.spatialMode == SandboxState::Spatial_3DRotate ||
                       s.spatialMode == SandboxState::Spatial_8DPreset);
        if (leia3D)
            p.leia.ensureInit(m_fs);
        p.leia.setMix(s.spatialMix);
        p.leia.setReflections(s.leiaReflEnable, s.leiaReflLevel,
                              s.leiaRoomSize, s.leiaRoomType);
        p.leia.setClarity(s.leiaClarity);
        p.leia.setWidth(s.leiaWidth);
    };
    applyToPath(m_play);
    applyToPath(m_cap);
    refreshReverbWet();

    auto placeStatic = [this, &s](PathState &p){
        if (s.spatialMode == SandboxState::Spatial_3DManual) {
            float r = s.distanceM;
            float ux = s.posX, uy = s.posY, uz = s.elev;
            float n = std::sqrt(ux*ux + uy*uy + uz*uz);
            if (n < 1e-3f) { uy = -1.0f; n = 1.0f; }
            ux /= n; uy /= n; uz /= n;
            pushSpeakerPair(p, ux * r, uy * r, uz * r);
        } else {
            pushSpeakerPair(p, 0.0f, -1.0f, 0.0f);
        }
    };
    placeStatic(m_play);
    placeStatic(m_cap);
}

void SlotDsp::pushSpeakerPair(PathState &p, float cx, float cy, float cz) {
    float r2d = std::sqrt(cx * cx + cy * cy);
    if (r2d < 1e-3f) { cx = 0.0f; cy = -1.0f; r2d = 1.0f; }
    float ux = cx / r2d, uy = cy / r2d;

    float halfDeg = m_state.stereoWidthDeg * 0.5f;
    float ang = halfDeg * 3.14159265358979323846f / 180.0f;
    float cd = std::cos(ang), sd = std::sin(ang);
    float Lux =  ux * cd + uy * sd;
    float Luy = -ux * sd + uy * cd;
    float Rux =  ux * cd - uy * sd;
    float Ruy =  ux * sd + uy * cd;
    p.posL.setPosition(Lux * r2d, Luy * r2d, cz);
    p.posR.setPosition(Rux * r2d, Ruy * r2d, cz);
}

void SlotDsp::advanceRotationIfNeeded(PathState &p) {
    if (m_state.spatialMode != SandboxState::Spatial_3DRotate &&
        m_state.spatialMode != SandboxState::Spatial_8DPreset) return;
    if (p.rotBlockCounter > 0) {
        --p.rotBlockCounter;
        return;
    }
    int dir = m_state.rotateCcw ? -1 : +1;
    double dPhase = 2.0 * 3.14159265358979323846 * m_state.rotateRpm /
                    60.0 * (kRotateUpdateBlock / m_fs);
    p.rotPhase += dir * dPhase;
    if (p.rotPhase >  6.28318530717958647692) p.rotPhase -= 6.28318530717958647692;
    if (p.rotPhase < -6.28318530717958647692) p.rotPhase += 6.28318530717958647692;
    float ph = static_cast<float>(p.rotPhase);
    float x =  std::sin(ph) * m_state.rotateRadiusM;
    float y = -std::cos(ph) * m_state.rotateRadiusM;
    pushSpeakerPair(p, x, y, m_state.rotateElev);
    p.rotBlockCounter = kRotateUpdateBlock;
}

void SlotDsp::updateLeiaDirection(PathState &p) {
    // Control-rate az/el feed for the Leia engine, mirroring the cadence
    // and orbit direction of advanceRotationIfNeeded so 8D presets sound
    // the same regardless of engine.
    if (p.rotBlockCounter > 0) { --p.rotBlockCounter; return; }
    p.rotBlockCounter = kRotateUpdateBlock;

    constexpr double kPi      = 3.14159265358979323846;
    constexpr float  kRad2Deg = static_cast<float>(180.0 / 3.14159265358979323846);
    float az = 0.0f, el = 0.0f;

    if (m_state.spatialMode == SandboxState::Spatial_3DManual) {
        // x = right, y = back, z = up. Front = (0,-1,0).
        float ux = m_state.posX, uy = m_state.posY, uz = m_state.elev;
        float n = std::sqrt(ux*ux + uy*uy + uz*uz);
        if (n < 1e-3f) { ux = 0.0f; uy = -1.0f; uz = 0.0f; n = 1.0f; }
        ux /= n; uy /= n; uz /= n;
        az = std::atan2(ux, -uy) * kRad2Deg;
        el = std::atan2(uz, std::sqrt(ux*ux + uy*uy)) * kRad2Deg;
    } else {
        // Rotate / 8D preset: advance the orbit phase.
        int dir = m_state.rotateCcw ? -1 : +1;
        double dPhase = 2.0 * kPi * m_state.rotateRpm / 60.0 *
                        (kRotateUpdateBlock / m_fs);
        p.rotPhase += dir * dPhase;
        if (p.rotPhase >  2.0 * kPi) p.rotPhase -= 2.0 * kPi;
        if (p.rotPhase < -2.0 * kPi) p.rotPhase += 2.0 * kPi;
        az = static_cast<float>(p.rotPhase) * kRad2Deg;
        float radius = m_state.rotateRadiusM;
        if (radius < 0.05f) radius = 0.05f;
        el = std::atan2(m_state.rotateElev, radius) * kRad2Deg;
    }
    p.leia.setDirection(az, el);
}

void SlotDsp::applyStage(int stage, PathState &p, float &l, float &r) {
    switch (stage) {
    case SandboxState::Stage_Paulstretch:
        // Represented module only - the time-stretch runs on a separate
        // streaming feed (feedStretchShort / produceStretchedShort)
        // before this chain. Nothing to do per-sample here.
        break;
    case SandboxState::Stage_EQ:
        if (m_state.eqEnabled) p.eq.processStereo(l, r);
        break;
    case SandboxState::Stage_Compressor:
        if (m_state.compEnabled) p.comp.processStereo(l, r);
        break;
    case SandboxState::Stage_Saturator:
        if (m_state.saturatorEnabled && m_state.saturatorMix > 0.001f) p.sat.processStereo(l, r);
        break;
    case SandboxState::Stage_Spatial:
        switch (m_state.spatialMode) {
            case SandboxState::Spatial_LRPan: {
                float pan = m_state.panValue;
                if (pan < -1.0f) pan = -1.0f;
                if (pan >  1.0f) pan =  1.0f;
                float theta = (pan + 1.0f) * 0.7853981634f;
                float gL = std::cos(theta), gR = std::sin(theta);
                l *= gL; r *= gR;
                break;
            }
            case SandboxState::Spatial_3DManual:
            case SandboxState::Spatial_3DRotate:
            case SandboxState::Spatial_8DPreset: {
                if (m_state.spatialEngine == SandboxState::Engine_Leia &&
                    p.leia.ready()) {
                    // Leia (measured HRTF). The wrapper does its own
                    // block buffering + wet/dry crossfade internally.
                    updateLeiaDirection(p);
                    p.leia.process(l, r);
                } else {
                    // Classic parametric HRTF (also the fallback path
                    // when a Leia init failed).
                    advanceRotationIfNeeded(p);
                    float ll, lr, rl, rr;
                    p.posL.process(l, ll, lr);
                    p.posR.process(r, rl, rr);
                    float wetL = ll + rl, wetR = lr + rr;
                    // Width-dependent dual-speaker sum normalisation.
                    //
                    // In 3DManual the two virtual speakers are stationary,
                    // so when they sit at the same point (width 0) summing
                    // both ears doubles mono content - the "boombox" bass
                    // bump fix shipped earlier. In Spatial_3DRotate /
                    // Spatial_8DPreset the speakers orbit the head, so the
                    // perceived loudness comes from the rotation envelope,
                    // not from speaker separation. The user reported the
                    // 8D effect (rotateRpm + width 0) felt anaemic after
                    // the earlier fix; halving the rotating sum was the
                    // reason. Keep the narrow-width attenuation on the
                    // static modes only; rotating modes get full strength
                    // with a small overdrive guard (1.4x at width 0,
                    // tapering to unity at >=90 deg) to actually exceed
                    // the original pre-fix loudness without the bass bump
                    // returning (rotation modulates spectrum away from
                    // sustained mono LF content, so the doubling fix is
                    // not needed there).
                    bool rotating = (m_state.spatialMode == SandboxState::Spatial_3DRotate ||
                                      m_state.spatialMode == SandboxState::Spatial_8DPreset);
                    float wScale;
                    if (rotating) {
                        float spread = std::min(1.0f, m_state.stereoWidthDeg / 90.0f);
                        wScale = 1.4f - 0.4f * spread;  // 1.4 .. 1.0
                    } else {
                        wScale = 0.5f + 0.5f * std::min(1.0f,
                            m_state.stereoWidthDeg / 90.0f);
                    }
                    wetL *= wScale; wetR *= wScale;
                    float w = m_state.spatialMix;
                    if (w < 0.0f) w = 0.0f; else if (w > 1.0f) w = 1.0f;
                    float d = 1.0f - w;
                    l = w * wetL + d * l;
                    r = w * wetR + d * r;
                }
                break;
            }
            default: break;
        }
        break;
    case SandboxState::Stage_Chorus:
        if (m_state.chorusEnabled && m_state.chorusMix > 0.001f) p.chorus.processStereo(l, r);
        break;
    case SandboxState::Stage_Flanger:
        if (m_state.flangerEnabled && m_state.flangerMix > 0.001f) p.flanger.processStereo(l, r);
        break;
    case SandboxState::Stage_Flangus:
        if (m_state.flangusEnabled && m_state.flangusMix > 0.001f) p.flangus.processStereo(l, r);
        break;
    case SandboxState::Stage_Phaser:
        if (m_state.phaserEnabled && m_state.phaserMix > 0.001f) p.phaser.processStereo(l, r);
        break;
    case SandboxState::Stage_Delay:
        if (m_state.delayEnabled && m_state.delayMix > 0.001f) p.delay.processStereo(l, r);
        break;
    case SandboxState::Stage_Reverb:
        if ((m_state.reverbWet + m_fxReverbWet) > 0.001f) p.reverb.process(l, r);
        break;
    case SandboxState::Stage_Limiter:
        if (m_state.limiterEnabled) p.limiter.processStereo(l, r);
        break;
    case SandboxState::Stage_Bitcrusher:
        if (m_state.bitcrusherEnabled) p.bitcrusher.processStereo(l, r);
        break;
    case SandboxState::Stage_GenLoss:
        if (m_state.genLossEnabled) p.genLoss.processStereo(l, r);
        break;
    }
}

void SlotDsp::process(short *interleaved, int frames, int channels,
                      float &peakL, float &peakR, bool isCapture) {
    ScopedCpuTimer t(m_cpuNs, m_cpuFrames, frames);
    PathState &p = isCapture ? m_cap : m_play;
    constexpr float kInv = 1.0f / 32768.0f;

    if (!m_active) {
        for (int i = 0; i < frames; ++i) {
            float l = (channels >= 2)
                          ? interleaved[i * channels + 0] * kInv
                          : interleaved[i] * kInv;
            float r = (channels >= 2)
                          ? interleaved[i * channels + 1] * kInv
                          : l;
            float al = std::fabs(l);
            float ar = std::fabs(r);
            m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
            m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;
        }
        peakL = m_peakL;
        peakR = m_peakR;
        return;
    }

    for (int i = 0; i < frames; ++i) {
        float l = (channels >= 2)
                      ? interleaved[i * channels + 0] * kInv
                      : interleaved[i] * kInv;
        float r = (channels >= 2)
                      ? interleaved[i * channels + 1] * kInv
                      : l;

        // Inject anti-denormal bias before any stateful filter.
        float dither = antiDenormDither(i);
        l += dither; r -= dither;

        // Always feed the EQ analyser regardless of the eqEnabled
        // gate so the per-band LED widgets keep reflecting the input
        // spectrum even when the EQ stage is bypassed.
        p.eq.feedAnalysis(l, r);

        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            applyStage(stage, p, l, r);
        }

        // Mono fold-down: post-chain checkbox, not a pipeline stage.
        if (m_state.enabled && m_state.monoEnabled) {
            float m = (l + r) * 0.5f; l = m; r = m;
        }

        l = softLimit(l);
        r = softLimit(r);

        float al = std::fabs(l);
        float ar = std::fabs(r);
        m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
        m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;

        constexpr float kHeadroom = 0.95f;
        if (channels >= 2) {
            interleaved[i * channels + 0] = clampToShort(l * kHeadroom * 32767.0f);
            interleaved[i * channels + 1] = clampToShort(r * kHeadroom * 32767.0f);
        } else {
            interleaved[i] = clampToShort((l + r) * 0.5f * kHeadroom * 32767.0f);
        }
    }

    peakL = m_peakL;
    peakR = m_peakR;
}

void SlotDsp::produceStretchedShort(short *out, int frames, int channels,
                                     float &peakL, float &peakR, bool isCapture) {
    ScopedCpuTimer t(m_cpuNs, m_cpuFrames, frames);
    StretchState &s = isCapture ? m_stretchCap : m_stretchPlay;
    PathState &p = isCapture ? m_cap : m_play;
    static thread_local std::vector<float> tmpL, tmpR;
    if (static_cast<int>(tmpL.size()) < frames) {
        tmpL.assign(frames, 0.0f);
        tmpR.assign(frames, 0.0f);
    } else {
        std::fill_n(tmpL.begin(), frames, 0.0f);
        std::fill_n(tmpR.begin(), frames, 0.0f);
    }

    if (s.inited) {
        s.ps.fillStereo(tmpL.data(), tmpR.data(), frames);
    }

    for (int i = 0; i < frames; ++i) {
        float l = tmpL[i];
        float r = tmpR[i];

        // Anti-denormal bias for the stretched path too.
        float dither = antiDenormDither(i);
        l += dither; r -= dither;

        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            applyStage(stage, p, l, r);
        }

        // Mono fold-down: post-chain checkbox, not a pipeline stage.
        if (m_state.enabled && m_state.monoEnabled) {
            float m = (l + r) * 0.5f; l = m; r = m;
        }

        l = softLimit(l);
        r = softLimit(r);

        float al = std::fabs(l), ar = std::fabs(r);
        m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
        m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;

        constexpr float kHeadroom = 0.95f;
        if (channels >= 2) {
            out[i * channels + 0] = clampToShort(l * kHeadroom * 32767.0f);
            out[i * channels + 1] = clampToShort(r * kHeadroom * 32767.0f);
        } else {
            out[i] = clampToShort((l + r) * 0.5f * kHeadroom * 32767.0f);
        }
    }

    peakL = m_peakL;
    peakR = m_peakR;
}
