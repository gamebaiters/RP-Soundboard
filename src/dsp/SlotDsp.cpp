#include "SlotDsp.h"
#include <cmath>
#include <algorithm>
#include <chrono>

// All 21 stages enabled by default; Settings > "sandbox modules"
// clears bits to hide + bypass entire modules process-wide.
std::atomic<uint32_t> SlotDsp::s_globalStageMask{0xFFFFFFFFu};

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
    for (int i = 0; i < EqRack::kNumBands; ++i)
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
// TPDF quantization dither (Q5): ±1 LSB triangular noise added right
// before the float -> int16 conversion. Decorrelates the truncation
// error so quiet reverb/shimmer tails and fade-outs quantize to a
// benign noise floor instead of harmonic grit. This is the ONLY
// float->16-bit point in the whole pipeline (the exporter consumes
// the same s16 stream), so chain-level dither covers live playback
// AND file export identically. Two LCG steps per sample - negligible.
inline float tpdfDither(uint32_t &rng) {
    rng = rng * 1664525u + 1013904223u;
    float a = static_cast<float>(rng >> 17) * (1.0f / 32768.0f);
    rng = rng * 1664525u + 1013904223u;
    float b = static_cast<float>(rng >> 17) * (1.0f / 32768.0f);
    return a - b;
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
// Soft limiter with strict bound to [-1, +1]. T=0.98 -> only the last
// 0.2 dB of headroom triggers compression; normal program material
// passes through linearly. Softer knee (A=0.12 up from 0.05) so hot
// bass content is bent into place gently instead of hitting a tight
// crossover-distortion elbow - the tight elbow was audible as the
// residual "physical-limiter-inhibits-playback" symptom the user
// reported on sustained bass through the Leia path.
inline float softLimit(float x) {
    constexpr float T = 0.98f;
    constexpr float A = 0.12f;
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
        p.deesser.setSampleRate(m_fs);
        p.gate.setSampleRate(m_fs);
        p.trans.setSampleRate(m_fs);
        p.dyneq.setSampleRate(m_fs);
        p.voicefx.setSampleRate(m_fs);
        p.bassEnh.setSampleRate(m_fs);
        p.binaural.setSampleRate(m_fs);
        p.convRev.setSampleRate(m_fs);
        p.tape.setSampleRate(m_fs);
        p.lfo.setSampleRate(m_fs);
        p.failsafe.setSampleRate(m_fs);
        // Fixed brickwall config: -1 dBFS true-peak, 1 ms lookahead,
        // fast-ish release. Engaged per-sample only when the state's
        // failsafeEnabled flag is on.
        p.failsafe.setParams(-1.0f, 1.0f, 80.0f,
                             Limiter::LimiterMode, 100.0f, -120.0f);
        p.failsafe.setTruePeak(true);
    };
    initPath(m_play);
    initPath(m_cap);
    m_stretchPlay.ps.setSampleRate(m_fs);
    m_stretchCap.ps.setSampleRate(m_fs);
    double tau = 0.200;
    m_peakDecay = static_cast<float>(std::exp(-1.0 / (tau * m_fs)));
}

void SlotDsp::reset(bool keepTape) {
    auto resetPath = [keepTape](PathState &p){
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
        p.deesser.reset();
        p.gate.reset();
        p.trans.reset();
        p.dyneq.reset();
        p.voicefx.reset();
        p.bassEnh.reset();
        p.binaural.reset();
        p.convRev.reset();
        // keepTape: a scrub-seek commit lands MID-GESTURE - resetting
        // the tape here kicked it out of Scratch (sticky-arm snapReset
        // -> Armed), the pending hand motion was silently ignored and
        // the drag degraded to bare skips / a full lock-up. The caller
        // rebases the tape onto the post-seek stream instead.
        if (!keepTape) p.tape.reset();
        p.lfo.reset();
        p.failsafe.reset();
        p.lfoGainL = p.lfoGainR = 1.0f;
        p.lfoForce = 0;
        p.rotPhase = 0.0;
        p.rotBlockCounter = 0;
        std::fill(std::begin(p.dopplerBufL), std::end(p.dopplerBufL), 0.0f);
        std::fill(std::begin(p.dopplerBufR), std::end(p.dopplerBufR), 0.0f);
        p.dopplerWrite = 0;
        p.dopplerDelayL = p.dopplerDelayR = 0.0f;
        p.dopplerPrevAz = 0.0f;
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

void SlotDsp::resetPreservingRotation() {
    double savedPhasePlay = m_play.rotPhase;
    int    savedBlockPlay = m_play.rotBlockCounter;
    double savedPhaseCap  = m_cap.rotPhase;
    int    savedBlockCap  = m_cap.rotBlockCounter;
    reset();
    m_play.rotPhase        = savedPhasePlay;
    m_play.rotBlockCounter = savedBlockPlay;
    m_cap.rotPhase         = savedPhaseCap;
    m_cap.rotBlockCounter  = savedBlockCap;
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
    m_play.convRev.setWet(combined);
    m_cap.convRev.setWet(combined);
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
                       s.genLossEnabled ||
                       s.gateEnabled ||
                       s.deesserEnabled ||
                       s.transEnabled ||
                       s.dyneqEnabled ||
                       (s.vfxEnabled) ||
                       s.bassEnhEnabled ||
                       s.binauralEnabled ||
                       s.lfoEnabled[0] || s.lfoEnabled[1];
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

void SlotDsp::prepareConvReverb(const SandboxState &s) {
    if (s.reverbConvMode != 1) return;
    // prepare() no-ops when the requested IR is already loaded, so
    // calling this on every state push is cheap after the first build.
    m_play.convRev.prepare(s.reverbConvPreset, s.reverbConvIrPath);
    m_cap.convRev.prepare(s.reverbConvPreset, s.reverbConvIrPath);
}

void SlotDsp::tapeTrigger(float brakeMs) {
    m_play.tape.trigger(brakeMs);
    m_cap.tape.trigger(brakeMs);
}

void SlotDsp::tapeRelease(float spinMs) {
    m_play.tape.release(spinMs);
    m_cap.tape.release(spinMs);
}

void SlotDsp::tapeSnapReset() {
    m_play.tape.snapReset();
    m_cap.tape.snapReset();
}

void SlotDsp::tapeArm(bool on) {
    m_play.tape.arm(on);
    m_cap.tape.arm(on);
}

void SlotDsp::tapeScratchBegin() {
    m_play.tape.scratchBegin();
    m_cap.tape.scratchBegin();
}

void SlotDsp::tapeScratchDelta(float deltaSeconds) {
    m_play.tape.scratchDelta(deltaSeconds);
    m_cap.tape.scratchDelta(deltaSeconds);
}

void SlotDsp::tapeScratchRebase() {
    m_play.tape.scratchRebase();
    m_cap.tape.scratchRebase();
}

void SlotDsp::tapeScratchEnd(float spinMs) {
    m_play.tape.scratchEnd(spinMs);
    m_cap.tape.scratchEnd(spinMs);
}

void SlotDsp::applyLfoRoutes(PathState &p, int frames) {
    p.lfo.advance(frames);

    // Baselines every block; routes stack on top. Pan/Volume default
    // to unity when no route targets them this block.
    float gainL = 1.0f, gainR = 1.0f;
    uint32_t force = 0;
    const SandboxState &s = m_state;

    for (int i = 0; i < 4; ++i) {
        int target = s.lfoRouteTarget[i];
        if (target == LfoMatrix::Target_None) continue;
        int li = (s.lfoRouteLfo[i] != 0) ? 1 : 0;
        if (!s.lfoEnabled[li]) continue;
        float v = p.lfo.value(li) * s.lfoRouteAmount[i];

        switch (target) {
        case LfoMatrix::Target_RingFreq:
            p.voicefx.modRingFreq(std::min(2000.0f, std::max(20.0f,
                s.vfxRingFreq * std::pow(2.0f, v))));
            break;
        case LfoMatrix::Target_WahSweep:
            p.voicefx.modWahBias(v);
            break;
        case LfoMatrix::Target_TremRate:
            p.voicefx.modTremRate(std::min(20.0f, std::max(0.05f,
                s.vfxTremRate * std::pow(2.0f, v))));
            break;
        case LfoMatrix::Target_VibDepth:
            p.voicefx.modVibDepth(std::min(1.0f, std::max(0.0f,
                s.vfxVibDepth + v)));
            break;
        case LfoMatrix::Target_ChorusMix:
            p.chorus.setParams(s.chorusRate, s.chorusDepth, s.chorusBaseDelay,
                               s.chorusVoices,
                               std::min(1.0f, std::max(0.0f, s.chorusMix + v)));
            force |= 1u << SandboxState::Stage_Chorus;
            break;
        case LfoMatrix::Target_ReverbWet: {
            float w = s.reverbWet + m_fxReverbWet + v;
            if (w < 0.0f) w = 0.0f;
            if (w > 1.0f) w = 1.0f;
            p.reverb.setWet(w);
            p.convRev.setWet(w);
            force |= 1u << SandboxState::Stage_Reverb;
            break;
        }
        case LfoMatrix::Target_SatDrive:
            p.sat.setParams(std::min(10.0f, std::max(1.0f,
                                s.saturatorDrive * std::pow(2.0f, v))),
                            std::max(0.35f, s.saturatorMix),
                            s.saturatorTone,
                            static_cast<Saturator::Mode>(s.saturatorMode));
            force |= 1u << SandboxState::Stage_Saturator;
            break;
        case LfoMatrix::Target_CrushRate:
            p.bitcrusher.setParams(s.bitcrusherBitDepth,
                std::min(48000.0f, std::max(500.0f,
                    s.bitcrusherRate * std::pow(4.0f, v))));
            break;
        case LfoMatrix::Target_Pan: {
            // Balance-style pan: attenuate the far side only, unity at
            // centre - never boosts, so it can't clip the chain.
            float pan = std::min(1.0f, std::max(-1.0f, v));
            if (pan > 0.0f) gainL *= 1.0f - pan;
            else            gainR *= 1.0f + pan;
            break;
        }
        case LfoMatrix::Target_Volume: {
            float g = std::min(1.0f, std::max(0.0f, 1.0f + v));
            gainL *= g;
            gainR *= g;
            break;
        }
        case LfoMatrix::Target_DelayFeedback:
            p.delay.setParams(s.delayTimeMs,
                std::min(0.95f, std::max(0.0f, s.delayFeedback + 0.5f * v)),
                s.delayMix, s.delayDamping, s.delayPingPong);
            break;
        default:
            break;
        }
    }

    // Smooth block-rate gain moves a touch to avoid zipper (~1 block).
    p.lfoGainL += 0.5f * (gainL - p.lfoGainL);
    p.lfoGainR += 0.5f * (gainR - p.lfoGainR);
    p.lfoForce = force;
}

void SlotDsp::clearLfoRoutes(PathState &p) {
    // LFO matrix just went inactive: restore the base parameters the
    // routes were riding on so the chain snaps back to slider truth.
    if (p.lfoForce == 0 && p.lfoGainL == 1.0f && p.lfoGainR == 1.0f)
        return;
    const SandboxState &s = m_state;
    p.chorus.setParams(s.chorusRate, s.chorusDepth, s.chorusBaseDelay,
                       s.chorusVoices, s.chorusMix);
    p.sat.setParams(s.saturatorDrive, s.saturatorMix, s.saturatorTone,
                    static_cast<Saturator::Mode>(s.saturatorMode));
    p.bitcrusher.setParams(s.bitcrusherBitDepth, s.bitcrusherRate);
    p.delay.setParams(s.delayTimeMs, s.delayFeedback, s.delayMix,
                      s.delayDamping, s.delayPingPong);
    float w = s.reverbWet + m_fxReverbWet;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    p.reverb.setWet(w);
    p.convRev.setWet(w);
    p.voicefx.setParams(s);
    p.lfoGainL = p.lfoGainR = 1.0f;
    p.lfoForce = 0;
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
        // Throttle the EQ analyser FFT when the EQ stage is bypassed -
        // LEDs still animate, just at a coarser rate, freeing audio-
        // thread CPU for the heavier Spatial path.
        bool eqBandActive = s.eqEnabled && std::any_of(
            std::begin(s.eqBandDb), std::end(s.eqBandDb),
            [](float v){ return std::abs(v) > 0.001f; });
        p.eq.setStageActive(eqBandActive);
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
        p.limiter.setTruePeak(s.truePeakMode);
        p.bitcrusher.setParams(s.bitcrusherBitDepth, s.bitcrusherRate);
        p.voicefx.setParams(s);
        p.bassEnh.setParams(s.bassEnhFreq, s.bassEnhDrive, s.bassEnhMix,
                            s.hqOversampling);
        p.binaural.setParams(s.binauralBaseHz, s.binauralBeatHz,
                             s.binauralLevelDb);
        p.lfo.setParams(s);
        if (!(s.lfoEnabled[0] || s.lfoEnabled[1]))
            p.lfoGainL = p.lfoGainR = 1.0f;
        p.genLoss.setGenerations(s.genLossGenerations);
        p.deesser.setParams(s.deesserFreqHz, s.deesserQ, s.deesserThresholdDb,
                            s.deesserRangeDb, s.deesserAttackMs, s.deesserReleaseMs);
        p.gate.setParams(s.gateThresholdDb, s.gateRangeDb,
                         s.gateAttackMs, s.gateHoldMs, s.gateReleaseMs);
        p.trans.setParams(s.transAttackDb, s.transSustainDb);
        for (int b = 0; b < DynEq::kNumBands; ++b) {
            DynEq::BandConfig cfg;
            cfg.enabled      = s.dyneqBands[b].enabled;
            cfg.freq         = s.dyneqBands[b].freq;
            cfg.q            = s.dyneqBands[b].q;
            cfg.staticGainDb = s.dyneqBands[b].staticGainDb;
            cfg.thresholdDb  = s.dyneqBands[b].thresholdDb;
            cfg.ratio        = s.dyneqBands[b].ratio;
            cfg.dynamicDb    = s.dyneqBands[b].dynamicDb;
            cfg.attackMs     = s.dyneqBands[b].attackMs;
            cfg.releaseMs    = s.dyneqBands[b].releaseMs;
            p.dyneq.setBand(b, cfg);
        }

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
    // Push the speaker pair at the CURRENT phase FIRST, then advance
    // for the next block. Without this the first non-trivial call
    // emits at +dPhase (already to the right of front by one tick)
    // and the listener never hears the "starts at front" cue — the
    // orbit appears to spawn already off-axis. Pre-emit fixes the
    // perception "8D starts from the side".
    float ph0 = static_cast<float>(p.rotPhase);
    float x0 =  std::sin(ph0) * m_state.rotateRadiusM;
    float y0 = -std::cos(ph0) * m_state.rotateRadiusM;
    pushSpeakerPair(p, x0, y0, m_state.rotateElev);

    int dir = m_state.rotateCcw ? -1 : +1;
    double dPhase = 2.0 * 3.14159265358979323846 * m_state.rotateRpm /
                    60.0 * (kRotateUpdateBlock / m_fs);
    p.rotPhase += dir * dPhase;
    if (p.rotPhase >  6.28318530717958647692) p.rotPhase -= 6.28318530717958647692;
    if (p.rotPhase < -6.28318530717958647692) p.rotPhase += 6.28318530717958647692;
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
        // Rotate / 8D preset: emit the CURRENT phase to the Leia
        // direction setter, THEN advance for the next block. Without
        // the pre-emit the first non-trivial block already sat at
        // +dPhase, so the orbit "starts from the side" instead of
        // visibly from front.
        az = static_cast<float>(p.rotPhase) * kRad2Deg;
        float radius = m_state.rotateRadiusM;
        if (radius < 0.05f) radius = 0.05f;
        el = std::atan2(m_state.rotateElev, radius) * kRad2Deg;
        int dir = m_state.rotateCcw ? -1 : +1;
        double dPhase = 2.0 * kPi * m_state.rotateRpm / 60.0 *
                        (kRotateUpdateBlock / m_fs);
        p.rotPhase += dir * dPhase;
        if (p.rotPhase >  2.0 * kPi) p.rotPhase -= 2.0 * kPi;
        if (p.rotPhase < -2.0 * kPi) p.rotPhase += 2.0 * kPi;
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
        if (m_state.compEnabled) {
            if (m_state.compSidechainSlot >= 0)
                p.comp.feedSidechain(m_extCompEnv.load(std::memory_order_relaxed));
            else
                p.comp.feedSidechain(0.0f);
            p.comp.processStereo(l, r);
        }
        break;
    case SandboxState::Stage_Saturator:
        // lfoForce: an LFO route is riding this stage's mix/drive - the
        // base slider may be at 0 but the modulation must be audible.
        if (m_state.saturatorEnabled &&
            (m_state.saturatorMix > 0.001f ||
             ((p.lfoForce >> SandboxState::Stage_Saturator) & 1u)))
            p.sat.processStereo(l, r);
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
                // Pre-attenuate the Spatial input by the EQ's positive
                // sum, then post-amplify by the inverse so loudness is
                // preserved. Without this, EQ-boosted material fed
                // straight into the Spatial stage drove peaks above the
                // post-engine soft-saturator's knee and produced the
                // frying the user reported when modifying the EQ while
                // Leia was active. Capped at 6 dB of pull-down so the
                // post-mul never has to amplify by more than 2x and the
                // spatial cue stays audible.
                float positiveSumDb = m_state.eqEnabled
                    ? p.eq.positiveSumDb() : 0.0f;
                if (positiveSumDb > 6.0f) positiveSumDb = 6.0f;
                const float preAtt = (positiveSumDb > 0.001f)
                    ? std::pow(10.0f, -positiveSumDb / 20.0f) : 1.0f;
                const float postAmp = (preAtt > 0.0f) ? 1.0f / preAtt : 1.0f;
                if (preAtt < 1.0f) {
                    l *= preAtt;
                    r *= preAtt;
                }
                if (m_state.spatialEngine == SandboxState::Engine_Leia &&
                    p.leia.ready()) {
                    // Leia (measured HRTF). The wrapper does its own
                    // block buffering + wet/dry crossfade internally.
                    updateLeiaDirection(p);
                    p.leia.process(l, r);
                    // Optional Doppler on the Leia output. Only meaningful
                    // in rotate / 8D modes where the source azimuth
                    // actually moves.
                    if (m_state.dopplerEnabled &&
                        (m_state.spatialMode == SandboxState::Spatial_3DRotate ||
                         m_state.spatialMode == SandboxState::Spatial_8DPreset)) {
                        constexpr float kHeadRadiusM = 0.09f;
                        constexpr float kSpeedOfSoundMps = 343.0f;
                        const float strength = 1.0f
                            + 49.0f * (m_state.dopplerStrength * 0.01f); // 1..50x
                        const float az = static_cast<float>(p.rotPhase); // rad
                        const float dAz = az - p.dopplerPrevAz;
                        p.dopplerPrevAz = az;
                        // Sample-rate to convert rad/sample to m/s:
                        // radial velocity per ear = h * cos(az) * dAz/dt
                        // dAz here is per-block-boundary but we approximate
                        // per-sample delta by dAz/kRotateUpdateBlock.
                        const float perSampleDaz = dAz / static_cast<float>(kRotateUpdateBlock);
                        const float velR =  kHeadRadiusM * std::cos(az) * perSampleDaz
                                            * static_cast<float>(m_fs);
                        const float velL = -velR;
                        // Doppler delay accumulates: per-sample rate of
                        // change matches velocity / speed-of-sound.
                        p.dopplerDelayR += velR / kSpeedOfSoundMps * strength;
                        p.dopplerDelayL += velL / kSpeedOfSoundMps * strength;
                        // Clamp to buffer capacity - 2 samples of headroom.
                        auto clampDelay = [](float d) {
                            const float lim = static_cast<float>(PathState::kDopplerMax - 2);
                            if (d >  lim) d =  lim;
                            if (d < -lim) d = -lim;
                            return d;
                        };
                        p.dopplerDelayL = clampDelay(p.dopplerDelayL);
                        p.dopplerDelayR = clampDelay(p.dopplerDelayR);
                        // Write current sample, then read fractional at
                        // (write - delay) in a ring buffer.
                        int w = p.dopplerWrite;
                        p.dopplerBufL[w] = l;
                        p.dopplerBufR[w] = r;
                        auto fracRead = [](const float *buf, int w, float delay) -> float {
                            constexpr int N = PathState::kDopplerMax;
                            float target = w - delay;
                            while (target < 0.0f) target += N;
                            while (target >= N) target -= N;
                            int i0 = static_cast<int>(target);
                            int i1 = (i0 + 1) % N;
                            float f = target - static_cast<float>(i0);
                            return buf[i0] * (1.0f - f) + buf[i1] * f;
                        };
                        l = fracRead(p.dopplerBufL, w, p.dopplerDelayL);
                        r = fracRead(p.dopplerBufR, w, p.dopplerDelayR);
                        p.dopplerWrite = (w + 1) % PathState::kDopplerMax;
                    }
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
                // Post-amplify by the inverse pre-att so output level
                // matches the un-attenuated path.
                if (postAmp > 1.0f) {
                    l *= postAmp;
                    r *= postAmp;
                }
                break;
            }
            default: break;
        }
        break;
    case SandboxState::Stage_Chorus:
        if (m_state.chorusEnabled &&
            (m_state.chorusMix > 0.001f ||
             ((p.lfoForce >> SandboxState::Stage_Chorus) & 1u)))
            p.chorus.processStereo(l, r);
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
        if ((m_state.reverbWet + m_fxReverbWet) > 0.001f ||
            ((p.lfoForce >> SandboxState::Stage_Reverb) & 1u)) {
            // Convolution engine when selected AND its IR is built;
            // otherwise the algorithmic Freeverb path (also the
            // fallback while an IR is still loading).
            if (m_state.reverbConvMode == 1 && p.convRev.ready())
                p.convRev.process(l, r);
            else
                p.reverb.process(l, r);
        }
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
    case SandboxState::Stage_NoiseGate:
        if (m_state.gateEnabled) {
            if (m_state.gateSidechainSlot >= 0)
                p.gate.feedSidechain(m_extGateEnv.load(std::memory_order_relaxed));
            else
                p.gate.feedSidechain(0.0f);
            p.gate.processStereo(l, r);
        }
        break;
    case SandboxState::Stage_DeEsser:
        if (m_state.deesserEnabled) {
            if (m_state.deesserSidechainSlot >= 0)
                p.deesser.feedSidechain(m_extDeesserEnv.load(std::memory_order_relaxed));
            else
                p.deesser.feedSidechain(0.0f);
            p.deesser.processStereo(l, r);
        }
        break;
    case SandboxState::Stage_TransientShaper:
        if (m_state.transEnabled) p.trans.processStereo(l, r);
        break;
    case SandboxState::Stage_DynEq:
        if (m_state.dyneqEnabled) p.dyneq.processStereo(l, r);
        break;
    case SandboxState::Stage_VoiceFx:
        if (m_state.vfxEnabled) p.voicefx.processStereo(l, r);
        break;
    case SandboxState::Stage_BassEnh:
        if (m_state.bassEnhEnabled) p.bassEnh.processStereo(l, r);
        break;
    case SandboxState::Stage_Binaural:
        // The generator handles its own enable fade internally so
        // toggling never clicks; call gated on the enable to keep the
        // disabled cost at a single branch.
        if (m_state.binauralEnabled) p.binaural.processStereo(l, r, true);
        break;
    }
}

void SlotDsp::process(short *interleaved, int frames, int channels,
                      float &peakL, float &peakR, bool isCapture) {
    ScopedCpuTimer t(m_cpuNs, m_cpuFrames, frames);
    PathState &p = isCapture ? m_cap : m_play;
    constexpr float kInv = 1.0f / 32768.0f;

    // Tape stop is a transport effect: it must run even when the
    // sandbox chain is otherwise bypassed. A RE-TIMING phase (brake /
    // scratch / spin-up) forces the full path; the transparent Armed
    // state (history ingest only - now active for the whole playback
    // once the vinyl feature arms the slot) stays on the cheap bypass
    // path and just feeds the ring inline.
    const bool tapeOn = p.tape.active();
    const bool tapeArmedOnly = tapeOn && (p.tape.phase() == TapeStop::Armed);

    if (!m_active && (!tapeOn || tapeArmedOnly)) {
        const float outGain = m_outputGain.load(std::memory_order_relaxed);
        for (int i = 0; i < frames; ++i) {
            float l = (channels >= 2)
                          ? interleaved[i * channels + 0] * kInv
                          : interleaved[i] * kInv;
            float r = (channels >= 2)
                          ? interleaved[i * channels + 1] * kInv
                          : l;
            if (tapeOn) {
                p.tape.processStereo(l, r);
                // Armed is transparent (no write-back needed), but if
                // the phase flips mid-block (brake / scratch just
                // engaged from the GUI) the samples ARE re-timed -
                // write them back so the engage is sample-accurate
                // instead of snapping at the next block boundary.
                if (p.tape.phase() != TapeStop::Armed) {
                    if (channels >= 2) {
                        interleaved[i * channels + 0] =
                            clampToShort(l * 32767.0f);
                        interleaved[i * channels + 1] =
                            clampToShort(r * 32767.0f);
                    } else {
                        interleaved[i] =
                            clampToShort((l + r) * 0.5f * 32767.0f);
                    }
                }
            }
            // Feed analyser with the OUTPUT-domain signal: bypass path
            // emits (l, r) unchanged, then Sampler scales by volume *
            // intensity before mixing into the TS3 buffer. Multiplying
            // here by the same gain makes the LEDs match what the
            // listener actually hears - the slider truly lowers them.
            // Only the playback path drives the LEDs (getEqBandLevels
            // reads m_play exclusively) so the capture path skips the
            // FFT.
            if (!isCapture)
                p.eq.feedAnalysis(l * outGain, r * outGain);
            float al = std::fabs(l);
            float ar = std::fabs(r);
            m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
            m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;
        }
        peakL = m_peakL;
        peakR = m_peakR;
        return;
    }

    const float outGain = m_outputGain.load(std::memory_order_relaxed);
    const uint32_t stageMask = s_globalStageMask.load(std::memory_order_relaxed);

    // LFO matrix: one block tick, cheap live-setter pushes.
    if (m_state.enabled &&
        (m_state.lfoEnabled[0] || m_state.lfoEnabled[1]))
        applyLfoRoutes(p, frames);
    else
        clearLfoRoutes(p);

    const bool lfoGainOn = (p.lfoGainL != 1.0f || p.lfoGainR != 1.0f);
    const bool failsafeOn = m_state.enabled && m_state.failsafeEnabled;

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

        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            if (!((stageMask >> stage) & 1u)) continue;   // globally disabled
            applyStage(stage, p, l, r);
        }

        // Mono fold-down: post-chain checkbox, not a pipeline stage.
        if (m_state.enabled && m_state.monoEnabled) {
            float m = (l + r) * 0.5f; l = m; r = m;
        }

        // LFO-matrix Pan / Volume routes.
        if (lfoGainOn) {
            l *= p.lfoGainL;
            r *= p.lfoGainR;
        }

        // Tape stop (vinyl brake / scratch) - post-chain transport.
        if (tapeOn) p.tape.processStereo(l, r);

        // Failsafe anti-clip: true-peak brickwall at -1 dBFS. Catches
        // EQ boosts, resonant filters, stacked effects - guarantees
        // the signal reaches the output stage without ever clipping
        // (softLimit below then only shaves what physics still slips
        // through the 1 ms lookahead).
        if (failsafeOn) p.failsafe.processStereo(l, r);

        l = softLimit(l);
        r = softLimit(r);

        // Feed EQ analyser POST-chain + scaled by the output gain so the
        // per-band LEDs reflect what the listener actually hears: EQ
        // shaping, every effect, mono fold, soft-limit, AND the slot's
        // volume / intensity. Playback path only - getEqBandLevels reads
        // m_play exclusively, so running the FFT on the capture path was
        // pure wasted CPU.
        if (!isCapture)
            p.eq.feedAnalysis(l * outGain, r * outGain);

        float al = std::fabs(l);
        float ar = std::fabs(r);
        m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
        m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;
        // Sidechain-visible envelope: post-DSP peak, published lock-free
        // for cross-slot sidechain sources. Slow release so the envelope
        // reads as an amplitude, not a transient-noisy sample stream.
        if (!isCapture) {
            float peak = (al > ar) ? al : ar;
            float cur = m_sidechainEnv.load(std::memory_order_relaxed);
            float ncur = (peak > cur) ? peak : cur * 0.9995f;
            m_sidechainEnv.store(ncur, std::memory_order_relaxed);
        }

        // No extra headroom scaling: softLimit already bounds the
        // signal strictly inside [-1, +1], so the old 0.95 factor was
        // just an unconditional -0.45 dB level drop whenever the chain
        // was active - audible as "enabling the sandbox makes it
        // quieter" even with neutral settings.
        static thread_local uint32_t s_ditherRng = 0x9E3779B9u;
        if (channels >= 2) {
            interleaved[i * channels + 0] =
                clampToShort(l * 32767.0f + tpdfDither(s_ditherRng));
            interleaved[i * channels + 1] =
                clampToShort(r * 32767.0f + tpdfDither(s_ditherRng));
        } else {
            interleaved[i] =
                clampToShort((l + r) * 0.5f * 32767.0f + tpdfDither(s_ditherRng));
        }
    }

    peakL = m_peakL;
    peakR = m_peakR;
}

int SlotDsp::processTapeBlock(const short *in, int inFrames,
                              short *out, int outFrames, int channels,
                              float &peakL, float &peakR, bool isCapture) {
    ScopedCpuTimer t(m_cpuNs, m_cpuFrames, outFrames);
    PathState &p = isCapture ? m_cap : m_play;
    constexpr float kInv = 1.0f / 32768.0f;
    const float outGain = m_outputGain.load(std::memory_order_relaxed);
    const uint32_t stageMask = s_globalStageMask.load(std::memory_order_relaxed);

    // LFO matrix once per output block.
    if (m_state.enabled &&
        (m_state.lfoEnabled[0] || m_state.lfoEnabled[1]))
        applyLfoRoutes(p, outFrames);
    else
        clearLfoRoutes(p);
    const bool lfoGainOn = (p.lfoGainL != 1.0f || p.lfoGainR != 1.0f);
    const bool failsafeOn = m_state.enabled && m_state.failsafeEnabled;

    // ---- INGEST: run the pre-tape chain per input frame, feed the ring
    // AHEAD of the head. Bounded by the tape's own ahead target and a
    // 3x-block CPU cap so building the window never overruns the audio
    // callback (the window then fills over ~1 s of playback).
    int ingest = p.tape.ingestBudget();
    if (ingest > inFrames)      ingest = inFrames;
    // CPU cap: normally 3x the block so building the window never overruns
    // the callback. But the ARMED decode-ahead is kept tiny (low FX
    // latency), so a fast FORWARD scratch at the start of a gesture had no
    // runway and seek-stormed (choppy fast-forward). While actively
    // scratching AND the forward runway is still short, allow a much bigger
    // ingest burst so the decode-ahead rebuilds within ~150 ms - fast
    // enough to outrun a forward stroke. Reverts to 3x once the runway is
    // built (bounds the DSP burst to the ramp-up only). Backward scratch is
    // unaffected (it consumes history behind the head, not this ahead).
    int cap = outFrames * 3;
    if (p.tape.phase() == TapeStop::Scratch && p.tape.lagSeconds() < 0.6f)
        cap = outFrames * 8;
    if (ingest > cap) ingest = cap;
    if (ingest < 0)   ingest = 0;
    for (int i = 0; i < ingest; ++i) {
        float l = (channels >= 2) ? in[i * channels + 0] * kInv : in[i] * kInv;
        float r = (channels >= 2) ? in[i * channels + 1] * kInv : l;
        float dither = antiDenormDither(i);
        l += dither; r -= dither;
        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            if (!((stageMask >> stage) & 1u)) continue;
            applyStage(stage, p, l, r);
        }
        if (m_state.enabled && m_state.monoEnabled) {
            float m = (l + r) * 0.5f; l = m; r = m;
        }
        if (lfoGainOn) { l *= p.lfoGainL; r *= p.lfoGainR; }
        p.tape.ingest(l, r);
    }

    // ---- PRODUCE: read the head at the tape rate, apply output-domain
    // limiter, meter + convert. Failsafe + softLimit sit here (post-
    // tape) exactly as in process(): they bound what the listener
    // actually hears, including scratched / braked audio.
    static thread_local uint32_t s_ditherRng = 0x51E3A7B9u;
    for (int i = 0; i < outFrames; ++i) {
        float l = 0.0f, r = 0.0f;
        p.tape.produce(l, r);

        if (failsafeOn) p.failsafe.processStereo(l, r);
        l = softLimit(l);
        r = softLimit(r);

        if (!isCapture)
            p.eq.feedAnalysis(l * outGain, r * outGain);

        float al = std::fabs(l), ar = std::fabs(r);
        m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
        m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;
        if (!isCapture) {
            float peak = (al > ar) ? al : ar;
            float cur = m_sidechainEnv.load(std::memory_order_relaxed);
            float ncur = (peak > cur) ? peak : cur * 0.9995f;
            m_sidechainEnv.store(ncur, std::memory_order_relaxed);
        }

        if (channels >= 2) {
            out[i * channels + 0] =
                clampToShort(l * 32767.0f + tpdfDither(s_ditherRng));
            out[i * channels + 1] =
                clampToShort(r * 32767.0f + tpdfDither(s_ditherRng));
        } else {
            out[i] =
                clampToShort((l + r) * 0.5f * 32767.0f + tpdfDither(s_ditherRng));
        }
    }

    peakL = m_peakL;
    peakR = m_peakR;
    return ingest;
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

    const float outGain = m_outputGain.load(std::memory_order_relaxed);
    const uint32_t stageMask = s_globalStageMask.load(std::memory_order_relaxed);
    const bool tapeOn = p.tape.active();

    if (m_state.enabled &&
        (m_state.lfoEnabled[0] || m_state.lfoEnabled[1]))
        applyLfoRoutes(p, frames);
    else
        clearLfoRoutes(p);

    const bool lfoGainOn = (p.lfoGainL != 1.0f || p.lfoGainR != 1.0f);
    const bool failsafeOn = m_state.enabled && m_state.failsafeEnabled;

    for (int i = 0; i < frames; ++i) {
        float l = tmpL[i];
        float r = tmpR[i];

        // Anti-denormal bias for the stretched path too.
        float dither = antiDenormDither(i);
        l += dither; r -= dither;

        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            if (!((stageMask >> stage) & 1u)) continue;   // globally disabled
            applyStage(stage, p, l, r);
        }

        // Mono fold-down: post-chain checkbox, not a pipeline stage.
        if (m_state.enabled && m_state.monoEnabled) {
            float m = (l + r) * 0.5f; l = m; r = m;
        }

        if (lfoGainOn) {
            l *= p.lfoGainL;
            r *= p.lfoGainR;
        }

        if (tapeOn) p.tape.processStereo(l, r);

        if (failsafeOn) p.failsafe.processStereo(l, r);

        l = softLimit(l);
        r = softLimit(r);

        // Feed EQ analyser POST-chain + scaled by the output gain - see
        // process() for the rationale. Without this the per-band LEDs
        // froze whenever paulstretch was enabled.
        if (!isCapture)
            p.eq.feedAnalysis(l * outGain, r * outGain);

        float al = std::fabs(l), ar = std::fabs(r);
        m_peakL = (m_peakL > al) ? m_peakL * m_peakDecay : al;
        m_peakR = (m_peakR > ar) ? m_peakR * m_peakDecay : ar;

        // See process(): softLimit already bounds to [-1, +1], no
        // extra headroom scaling needed. Same TPDF dither as process().
        static thread_local uint32_t s_ditherRng = 0x7F4A7C15u;
        if (channels >= 2) {
            out[i * channels + 0] =
                clampToShort(l * 32767.0f + tpdfDither(s_ditherRng));
            out[i * channels + 1] =
                clampToShort(r * 32767.0f + tpdfDither(s_ditherRng));
        } else {
            out[i] =
                clampToShort((l + r) * 0.5f * 32767.0f + tpdfDither(s_ditherRng));
        }
    }

    peakL = m_peakL;
    peakR = m_peakR;
}
