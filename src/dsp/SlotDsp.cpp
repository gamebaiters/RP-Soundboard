#include "SlotDsp.h"
#include <cmath>
#include <algorithm>

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

void SlotDsp::applyState(const SandboxState &s) {
    int oldMode = m_state.spatialMode;
    bool oldStretch = m_state.stretchEnabled;
    m_state = s;

    // Mode transitions = reset filter state on both paths. Without
    // this, switching from Off / Pan into a 3D mode left EQ biquad
    // states + reverb tails from the prior mode in the chain - the
    // HRTF stage suddenly fed those into its own filters and produced
    // a sustained robotic transient until everything settled. Same
    // protection on stretch toggle so re-enabling never inherits a
    // half-flushed filter.
    bool modeChanged = (oldMode != s.spatialMode) || (oldStretch != s.stretchEnabled);
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

    auto applyToPath = [&s](PathState &p){
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

void SlotDsp::applyStage(int stage, PathState &p, float &l, float &r) {
    switch (stage) {
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
                advanceRotationIfNeeded(p);
                float ll, lr, rl, rr;
                p.posL.process(l, ll, lr);
                p.posR.process(r, rl, rr);
                float wetL = ll + rl, wetR = lr + rr;
                float w = m_state.spatialMix;
                if (w < 0.0f) w = 0.0f; else if (w > 1.0f) w = 1.0f;
                float d = 1.0f - w;
                l = w * wetL + d * l;
                r = w * wetR + d * r;
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
    case SandboxState::Stage_Mono:
        if (m_state.monoEnabled) { float m = (l + r) * 0.5f; l = m; r = m; }
        break;
    case SandboxState::Stage_GenLoss:
        if (m_state.genLossEnabled) p.genLoss.processStereo(l, r);
        break;
    }
}

void SlotDsp::process(short *interleaved, int frames, int channels,
                      float &peakL, float &peakR, bool isCapture) {
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

        for (int si = 0; si < SandboxState::Stage_COUNT; ++si) {
            int stage = m_state.pipelineOrder[si];
            if (!m_state.enabled && stage != SandboxState::Stage_Reverb) continue;
            applyStage(stage, p, l, r);
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
