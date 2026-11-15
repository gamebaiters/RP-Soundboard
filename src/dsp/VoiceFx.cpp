#include "VoiceFx.h"
#include "leia/SimpleFFT.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
constexpr float  kPi    = 3.14159265358979323846f;
constexpr double kTwoPi = 6.28318530717958647692;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// Anti-denormal: recursive states that decay towards 0 (silence) are flushed to exactly 0 once
// they are below -300 dBFS, so they never sit in the (very slow) denormal range.
inline float flushDenorm(float v) { return (v < 1e-15f && v > -1e-15f) ? 0.0f : v; }

// Transparent below 0.9, smoothly saturating towards 1.0 above (exciter output ceiling).
inline float softLimit(float x) {
    constexpr float knee = 0.9f;
    float a = std::fabs(x);
    if (a <= knee) return x;
    float y = knee + (1.0f - knee) * std::tanh((a - knee) / (1.0f - knee));
    return x < 0.0f ? -y : y;
}

inline float onePoleCoeff(double ms, double fs) {
    if (ms <= 0.0 || fs <= 0.0) return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (ms * 0.001 * fs)));
}

// Band-limited PolyBLEP residual for alias-free sawtooth carrier.
inline float polyBlep(double t, double dt) {
    if (t < dt) {
        double x = t / dt;
        return static_cast<float>(x + x - x * x - 1.0);
    }
    if (t > 1.0 - dt) {
        double x = (t - 1.0) / dt;
        return static_cast<float>(x * x + x + x + 1.0);
    }
    return 0.0f;
}

inline float readHermiteRing(const float *buf, int bufSize, int writePos, float delaySamples) {
    int i0 = static_cast<int>(delaySamples);
    float f = delaySamples - static_cast<float>(i0);
    auto at = [&](int lag) -> float {
        int idx = writePos - lag;
        idx %= bufSize;
        if (idx < 0) idx += bufSize;
        return buf[idx];
    };
    float ym1 = at(i0 - 1);
    float y0  = at(i0);
    float y1  = at(i0 + 1);
    float y2  = at(i0 + 2);
    float c1 = 0.5f * (y1 - ym1);
    float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
    return ((c3 * f + c2) * f + c1) * f + y0;
}
} // namespace

VoiceFx::VoiceFx() {
    setSampleRate(48000.0);
}

VoiceFx::~VoiceFx() = default;

void VoiceFx::setSampleRate(double sr) {
    m_fs = (sr > 0) ? sr : 48000.0;

    m_shimShiftL.setSampleRate(m_fs);
    m_shimShiftR.setSampleRate(m_fs);
    m_shimShiftL.setWindowMs(60.0f);
    m_shimShiftR.setWindowMs(60.0f);
    m_autoTune.setSampleRate(m_fs);

    if (!m_formFft) m_formFft.reset(new SimpleFFT(kFormN));
    m_formIn.assign(kFormN, 0.0f);
    m_formOut.assign(kFormN * 2, 0.0f);
    m_formWin.resize(kFormN);
    for (int i = 0; i < kFormN; ++i)
        m_formWin[i] = 0.5f - 0.5f * std::cos(kTwoPi * i / kFormN);
    m_formFreq.assign(kFormN + 2, 0.0f);
    m_formCepFreq.assign(kFormN + 2, 0.0f);
    m_formEnv.assign(kFormN / 2 + 1, 0.0f);
    m_formEnvW.assign(kFormN / 2 + 1, 0.0f);
    m_formTime.assign(kFormN, 0.0f);
    m_formDryRing.assign(kFormN, 0.0f);
    m_formDryIdx = 0;
    m_formFill = 0;
    m_formInW = 0;
    m_formReadIdx = 0;
    m_formPrimed = false;

    int dl = static_cast<int>(0.089 * m_fs) | 1;
    int dr = static_cast<int>(0.103 * m_fs) | 1;
    m_shimDlyL.assign(dl, 0.0f);
    m_shimDlyR.assign(dr, 0.0f);
    m_shimApL.assign(137, 0.0f);
    m_shimApR.assign(149, 0.0f);
    m_shimWL = m_shimWR = m_shimApWL = m_shimApWR = 0;
    m_shimLpL = m_shimLpR = 0.0f;

    // Reverse delay: allocate once at the maximum slider range (2000 ms); only the active length changes.
    int revMax = static_cast<int>(2.0 * m_fs) + 2;
    if (revMax & 1) ++revMax;
    m_revMax = revMax;
    m_revRing = 2 * revMax;
    m_revCapL.assign(m_revRing, 0.0f);
    m_revCapR.assign(m_revRing, 0.0f);
    m_revN = 0;
    m_revW = 0;

    m_vocLastPitch = -1.0f;
    m_revLastTimeMs = -1.0f;
    setParams(m_p);
    reset();
}

void VoiceFx::setParams(const SandboxState &s) {
    m_p = s;

    m_liveRingFreq = s.vfxRingFreq;
    m_liveTremRate = s.vfxTremRate;
    m_liveVibDepth = s.vfxVibDepth;
    m_liveWahBias  = 0.0f;

    m_wahEnvAtk = onePoleCoeff(4.0, m_fs);
    m_wahEnvRel = onePoleCoeff(110.0, m_fs);

    m_tremSmCoeff = onePoleCoeff(2.0, m_fs);

    float f = std::min(8000.0f, std::max(1000.0f, s.vfxExcFreq));
    m_excHpCoeff = static_cast<float>(1.0 - std::exp(-kTwoPi * f / m_fs));

    m_autoTune.setParams(s.vfxTuneStrength, s.vfxTuneSpeedMs, s.vfxTuneScale, s.vfxTuneKey);

    if (std::fabs(s.vfxVocPitchHz - m_vocLastPitch) > 0.01f) {
        vocRebuild();
        m_vocLastPitch = s.vfxVocPitchHz;
    }
    m_vocAtk = onePoleCoeff(3.0, m_fs);
    m_vocRel = onePoleCoeff(45.0, m_fs);
    m_vocRmsAtk = onePoleCoeff(5.0, m_fs);
    m_vocRmsRel = onePoleCoeff(180.0, m_fs);
    m_vocLfC = static_cast<float>(1.0 - std::exp(-kTwoPi * 1000.0 / m_fs));
    m_vocHfC = static_cast<float>(1.0 - std::exp(-kTwoPi * 3200.0 / m_fs));

    if (std::fabs(s.vfxRevTimeMs - m_revLastTimeMs) > 0.5f) {
        revRebuild();
        m_revLastTimeMs = s.vfxRevTimeMs;
    }

    float shimRatio = std::pow(2.0f, static_cast<float>(m_p.vfxShimPitch) / 12.0f);
    m_shimShiftL.setRatio(shimRatio);
    m_shimShiftR.setRatio(shimRatio);
}

bool VoiceFx::anySubEnabled() const {
    const SandboxState &s = m_p;
    return s.vfxRingEnabled || s.vfxTremEnabled || s.vfxVibEnabled ||
           s.vfxWahEnabled || s.vfxExcEnabled || s.vfxTuneEnabled ||
           s.vfxVocEnabled || s.vfxFormEnabled || s.vfxShimEnabled ||
           s.vfxRevEnabled;
}

void VoiceFx::reset() {
    m_ringPhase = 0.0;
    m_tremPhase = 0.0;
    m_tremGainSm = 1.0f;
    std::memset(m_vibBufL, 0, sizeof(m_vibBufL));
    std::memset(m_vibBufR, 0, sizeof(m_vibBufR));
    m_vibW = 0;
    m_vibPhase = 0.0;
    m_wahEnv = 0.0f;
    m_wahX1L = m_wahX2L = m_wahY1L = m_wahY2L = 0.0f;
    m_wahX1R = m_wahX2R = m_wahY1R = m_wahY2R = 0.0f;
    m_wahCoeffCountdown = 0;
    m_excHpL = m_excHpR = 0.0f;
    m_excPrevL = m_excPrevR = 0.0f;
    std::memset(m_sideRing, 0, sizeof(m_sideRing));
    m_sideW = 0;

    m_autoTune.reset();

    std::fill(m_formIn.begin(), m_formIn.end(), 0.0f);
    std::fill(m_formOut.begin(), m_formOut.end(), 0.0f);
    std::fill(m_formDryRing.begin(), m_formDryRing.end(), 0.0f);
    m_formDryIdx = 0;
    m_formFill = 0;
    m_formInW = 0;
    m_formReadIdx = 0;
    m_formPrimed = false;

    for (int i = 0; i < kVocBands; ++i) {
        m_voc[i].mod1.reset();
        m_voc[i].mod2.reset();
        m_voc[i].car1.reset();
        m_voc[i].car2.reset();
        m_voc[i].env = 0.0f;
    }
    m_vocSawPhase = 0.0;
    m_vocSubPhase = 0.0;
    m_vocModRms = 0.1f;
    m_vocHfEnv = m_vocLfEnv = 0.0f;
    m_vocHpState = m_vocLpState = m_vocSibHpState = m_vocOutLp = 0.0f;

    std::fill(m_revCapL.begin(), m_revCapL.end(), 0.0f);
    std::fill(m_revCapR.begin(), m_revCapR.end(), 0.0f);
    for (int g = 0; g < kRevGrains; ++g) m_revG[g] = RevGrain();
    m_revW = 0;
    m_revLast = -1;

    std::fill(m_shimDlyL.begin(), m_shimDlyL.end(), 0.0f);
    std::fill(m_shimDlyR.begin(), m_shimDlyR.end(), 0.0f);
    std::fill(m_shimApL.begin(), m_shimApL.end(), 0.0f);
    std::fill(m_shimApR.begin(), m_shimApR.end(), 0.0f);
    m_shimWL = m_shimWR = m_shimApWL = m_shimApWR = 0;
    m_shimLpL = m_shimLpR = 0.0f;
    m_shimShiftL.reset();
    m_shimShiftR.reset();
}

void VoiceFx::processStereo(float &l, float &r) {
    const SandboxState &s = m_p;

    if (s.vfxTuneEnabled || s.vfxFormEnabled || s.vfxVocEnabled) {
        // Mid is processed; the side (stereo image) is re-added delayed by the same latency
        // (Tune: m_autoTune.latencySamples(), Formant: kFormN, Vocoder: 0) so it stays time-aligned with the mid.
        float m = 0.5f * (l + r);
        float side = 0.5f * (l - r);
        if (s.vfxTuneEnabled) m = m_autoTune.process(m);
        if (s.vfxFormEnabled) processFormantSample(m);
        if (s.vfxVocEnabled)  processVocoder(m);

        const int latency = (s.vfxTuneEnabled ? m_autoTune.latencySamples() : 0) + (s.vfxFormEnabled ? static_cast<int>(kFormN) : 0);
        m_sideRing[m_sideW & kSideRingMask] = side;
        float sideOut = m_sideRing[(m_sideW - static_cast<uint32_t>(latency)) & kSideRingMask];
        ++m_sideW;
        // The vocoder replaces the timbre: stereo side fades out in proportion to its mix.
        if (s.vfxVocEnabled) sideOut *= (1.0f - clamp01(s.vfxVocMix));
        l = m + sideOut;
        r = m - sideOut;
    }

    if (s.vfxRingEnabled) processRing(l, r);
    if (s.vfxWahEnabled)  processWah(l, r);
    if (s.vfxVibEnabled)  processVib(l, r);
    if (s.vfxTremEnabled) processTrem(l, r);
    if (s.vfxExcEnabled)  processExciter(l, r);
    if (s.vfxRevEnabled)  processRevDelay(l, r);
    if (s.vfxShimEnabled) processShimmer(l, r);
}

// --------------------------------------------------------------------
// Formant shifter (Cepstral liftered spectral envelope + dry delay match)
// --------------------------------------------------------------------

void VoiceFx::formantFrame() {
    const int bins = kFormN / 2 + 1;

    for (int i = 0; i < kFormN; ++i) {
        int idx = (m_formInW + i) % kFormN;
        m_formTime[i] = m_formIn[idx] * m_formWin[i];
    }
    m_formFft->forward(m_formTime.data(), m_formFreq.data());

    // Cepstral liftering extracts vocal-tract spectral envelope without harmonic rippling.
    for (int k = 0; k < bins; ++k) {
        float re = m_formFreq[2 * k], im = m_formFreq[2 * k + 1];
        float mag = std::sqrt(re * re + im * im);
        m_formCepFreq[2 * k]     = std::log(mag + 1e-4f);
        m_formCepFreq[2 * k + 1] = 0.0f;
    }
    m_formFft->inverse(m_formCepFreq.data(), m_formTime.data());

    constexpr int qPass = 30;
    constexpr int qStop = 56;
    const float invN = 1.0f / static_cast<float>(kFormN);
    for (int i = 0; i < kFormN; ++i) {
        int q = (i <= kFormN / 2) ? i : (kFormN - i);
        float w = 0.0f;
        if (q <= qPass) {
            w = 1.0f;
        } else if (q < qStop) {
            float t = static_cast<float>(q - qPass) / static_cast<float>(qStop - qPass);
            w = 0.5f * (1.0f + std::cos(kPi * t));
        }
        m_formTime[i] *= w * invN;
    }
    m_formFft->forward(m_formTime.data(), m_formCepFreq.data());

    for (int k = 0; k < bins; ++k) {
        float e = std::max(-12.0f, std::min(12.0f, m_formCepFreq[2 * k]));
        m_formEnv[k] = std::exp(e);
    }

    float ratio = std::pow(2.0f, m_p.vfxFormShift / 12.0f);
    for (int k = 0; k < bins; ++k) {
        float src = static_cast<float>(k) / ratio;
        int i0 = static_cast<int>(src);
        if (i0 >= bins - 1) {
            m_formEnvW[k] = m_formEnv[bins - 1] * 0.25f; // smoothly taper shifted HF tail
            continue;
        }
        float f = src - i0;
        m_formEnvW[k] = m_formEnv[i0] * (1.0f - f) + m_formEnv[i0 + 1] * f;
    }

    constexpr float kEps = 1e-5f;
    for (int k = 0; k < bins; ++k) {
        float g = (m_formEnvW[k] + kEps) / (m_formEnv[k] + kEps);
        if (g > 10.0f) g = 10.0f;
        if (g < 0.06f) g = 0.06f;
        m_formFreq[2 * k]     *= g;
        m_formFreq[2 * k + 1] *= g;
    }

    m_formFft->inverse(m_formFreq.data(), m_formTime.data());
    const float olaNorm = invN / 1.5f;
    for (int i = 0; i < kFormN; ++i)
        m_formOut[i] += m_formTime[i] * m_formWin[i] * olaNorm;
}

void VoiceFx::processFormantSample(float &m) {
    // Match kFormN latency for dry signal so partial mixes never comb-filter.
    float dryAligned = m_formDryRing[m_formDryIdx];
    m_formDryRing[m_formDryIdx] = m;
    m_formDryIdx = (m_formDryIdx + 1) % kFormN;

    m_formIn[m_formInW] = m;
    m_formInW = (m_formInW + 1) % kFormN;

    if (++m_formFill >= kFormHop) {
        m_formFill = 0;
        if (m_formPrimed) {
            std::memmove(m_formOut.data(), m_formOut.data() + kFormHop,
                         (m_formOut.size() - kFormHop) * sizeof(float));
            std::fill(m_formOut.end() - kFormHop, m_formOut.end(), 0.0f);
        }
        formantFrame();
        m_formPrimed = true;
        m_formReadIdx = 0;
    }

    float wet = dryAligned;
    if (m_formPrimed && m_formReadIdx < kFormHop)
        wet = m_formOut[m_formReadIdx++];

    float mix = clamp01(m_p.vfxFormMix);
    m = (1.0f - mix) * dryAligned + mix * wet;
}

// --------------------------------------------------------------------
// Vocoder (20-band 4th-order BPF + AGC + Unvoiced Sibilance + PolyBLEP)
// --------------------------------------------------------------------

void VoiceFx::vocRebuild() {
    // Log-spaced centres 95 Hz .. 8.5 kHz; cascaded 2nd-order BPF per band (4th-order total)
    // eliminates adjacent-band phase cancellation and boosts stopband separation cleanly.
    const double fMin = 95.0;
    const double fMax = std::min(8500.0, 0.40 * m_fs);
    const double ratio = fMax / fMin;
    for (int i = 0; i < kVocBands; ++i) {
        double t = static_cast<double>(i) / (kVocBands - 1);
        double fc = fMin * std::pow(ratio, t);
        double q  = 2.8;
        double w0 = kTwoPi * fc / m_fs;
        double alpha = std::sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        m_voc[i].b0 = static_cast<float>(alpha / a0);
        m_voc[i].b1 = 0.0f;
        m_voc[i].b2 = static_cast<float>(-alpha / a0);
        m_voc[i].a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
        m_voc[i].a2 = static_cast<float>((1.0 - alpha) / a0);
        // Gentle presence tilt helps intelligible mid/upper speech formants shine through.
        m_voc[i].bandGain = static_cast<float>(0.85 + 0.55 * t);
    }
}

void VoiceFx::processVocoder(float &m) {
    // Modulator auto-leveling (AGC + noise gate): speech from -28 dBFS to -3 dBFS
    // drives the vocoder cleanly without dropping 30 dB or gating into silence.
    float absM = std::fabs(m);
    float rmsC = (absM > m_vocModRms) ? m_vocRmsAtk : m_vocRmsRel;
    m_vocModRms += rmsC * (absM - m_vocModRms);
    m_vocModRms = flushDenorm(m_vocModRms);
    float agcGain = (m_vocModRms > 0.0012f)
        ? std::min(16.0f, std::max(0.5f, 0.25f / m_vocModRms))
        : 0.0f;
    float modNorm = m * agcGain;

    // Extract voiced/unvoiced consonant balance (HF > 3.2 kHz vs LF < 1.0 kHz) on raw modulator.
    const float lfC = m_vocLfC;
    const float hfC = m_vocHfC;
    m_vocLpState = flushDenorm(m_vocLpState + lfC * (modNorm - m_vocLpState));
    m_vocHpState = flushDenorm(m_vocHpState + hfC * (modNorm - m_vocHpState));
    float hfSig = modNorm - m_vocHpState;
    m_vocLfEnv = flushDenorm(m_vocLfEnv + 0.005f * (std::fabs(m_vocLpState) - m_vocLfEnv));
    m_vocHfEnv = flushDenorm(m_vocHfEnv + 0.01f  * (std::fabs(hfSig) - m_vocHfEnv));
    float unvoicedAmt = (m_vocHfEnv > m_vocLfEnv * 0.65f + 0.01f)
        ? std::min(0.65f, (m_vocHfEnv - m_vocLfEnv * 0.65f) * 2.5f)
        : 0.0f;

    // White noise sample (reused for whisper mode & consonant sibilance burst).
    m_vocNoise = m_vocNoise * 1664525u + 1013904223u;
    float noise = (static_cast<float>(m_vocNoise >> 9) / 4194304.0f) - 1.0f;

    // High-pass filtered noise for sibilants ("s", "t", "sh", "k").
    m_vocSibHpState += hfC * (noise - m_vocSibHpState);
    float sibNoise = noise - m_vocSibHpState;

    // Carrier synthesis.
    float car;
    if (m_p.vfxVocCarrier == 1) {
        car = noise * 0.85f;
    } else {
        double dt = std::max(20.0f, m_p.vfxVocPitchHz) / m_fs;
        m_vocSawPhase += dt;
        if (m_vocSawPhase >= 1.0) m_vocSawPhase -= 1.0;
        float saw = static_cast<float>(2.0 * m_vocSawPhase - 1.0) - polyBlep(m_vocSawPhase, dt);

        // Rich second harmonic/pulse layer fills spectral gaps cleanly across bands.
        double dtSub = dt * 0.5;
        m_vocSubPhase += dtSub;
        if (m_vocSubPhase >= 1.0) m_vocSubPhase -= 1.0;
        float sub = static_cast<float>(2.0 * m_vocSubPhase - 1.0) - polyBlep(m_vocSubPhase, dtSub);
        float pulse = (m_vocSawPhase < 0.35) ? 0.5f : -0.5f;

        car = 0.62f * saw + 0.25f * sub + 0.22f * pulse + 0.06f * noise;
    }

    float sum = 0.0f;
    for (int i = 0; i < kVocBands; ++i) {
        VocBand &b = m_voc[i];
        float m1 = b.mod1.process(modNorm, b.b0, b.b1, b.b2, b.a1, b.a2);
        float m2 = b.mod2.process(m1,      b.b0, b.b1, b.b2, b.a1, b.a2);

        float c1 = b.car1.process(car,     b.b0, b.b1, b.b2, b.a1, b.a2);
        float c2 = b.car2.process(c1,      b.b0, b.b1, b.b2, b.a1, b.a2);

        float rect = std::fabs(m2);
        b.env += (rect > b.env ? m_vocAtk : m_vocRel) * (rect - b.env);
        b.env = flushDenorm(b.env);
        sum += c2 * b.env * b.bandGain;
    }

    // Scale summed bands clearly into natural speech range (-28..-3 dBFS voices stay loud & clear).
    float levelTarget = (m_vocModRms > 0.0012f)
        ? std::max(0.28f, std::min(1.35f, m_vocModRms * 8.0f))
        : 0.0f;
    float wetRaw = (sum * 28.0f + sibNoise * unvoicedAmt * 0.45f) * levelTarget;
    float wet = std::tanh(wetRaw);

    float mix = clamp01(m_p.vfxVocMix);
    m = (1.0f - mix) * m + mix * wet;
}

// --------------------------------------------------------------------
// Cheap per-sample subs
// --------------------------------------------------------------------

void VoiceFx::processRing(float &l, float &r) {
    m_ringPhase += kTwoPi * m_liveRingFreq / m_fs;
    if (m_ringPhase > kTwoPi) m_ringPhase -= kTwoPi;
    float s = std::sin(static_cast<float>(m_ringPhase));
    float mix = clamp01(m_p.vfxRingMix);
    l = (1.0f - mix) * l + mix * (l * s);
    r = (1.0f - mix) * r + mix * (r * s);
}

void VoiceFx::processTrem(float &l, float &r) {
    float rate = std::min(20.0f, std::max(0.05f, m_liveTremRate));
    m_tremPhase += kTwoPi * rate / m_fs;
    if (m_tremPhase > kTwoPi) m_tremPhase -= kTwoPi;
    float s = std::sin(static_cast<float>(m_tremPhase));
    float lfo = (m_p.vfxTremShape == 1) ? (s > 0.0f ? 1.0f : 0.0f)
                                        : (0.5f + 0.5f * s);
    float depth = clamp01(m_p.vfxTremDepth);
    float target = 1.0f - depth * (1.0f - lfo);
    m_tremGainSm += (target - m_tremGainSm) * m_tremSmCoeff;
    m_tremGainSm = flushDenorm(m_tremGainSm); // depth 100% drives the gain towards 0
    l *= m_tremGainSm;
    r *= m_tremGainSm;
}

void VoiceFx::processVib(float &l, float &r) {
    m_vibBufL[m_vibW] = l;
    m_vibBufR[m_vibW] = r;

    float rate = std::min(14.0f, std::max(0.05f, m_p.vfxVibRate));
    m_vibPhase += kTwoPi * rate / m_fs;
    if (m_vibPhase > kTwoPi) m_vibPhase -= kTwoPi;
    float depth = clamp01(m_liveVibDepth);

    // Hermite cubic interpolation eliminates HF dulling; slight stereo offset preserves air.
    float baseSamp = static_cast<float>(0.004 * m_fs);
    float swing    = depth * static_cast<float>(0.0038 * m_fs);
    float dL = std::max(1.0f, std::min(static_cast<float>(kVibMax - 4),
        baseSamp + swing * std::sin(static_cast<float>(m_vibPhase))));
    float dR = std::max(1.0f, std::min(static_cast<float>(kVibMax - 4),
        baseSamp + swing * std::sin(static_cast<float>(m_vibPhase) + 0.35f)));

    l = readHermiteRing(m_vibBufL, kVibMax, m_vibW, dL);
    r = readHermiteRing(m_vibBufR, kVibMax, m_vibW, dR);
    m_vibW = (m_vibW + 1) % kVibMax;
}

void VoiceFx::wahRecalc(float centerHz) {
    float q = std::min(12.0f, std::max(1.0f, m_p.vfxWahQ));
    double w0 = kTwoPi * centerHz / m_fs;
    if (w0 > 2.95) w0 = 2.95;
    double alpha = std::sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    m_wahB0 = static_cast<float>(alpha / a0);
    m_wahB1 = 0.0f;
    m_wahB2 = static_cast<float>(-alpha / a0);
    m_wahA1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
    m_wahA2 = static_cast<float>((1.0 - alpha) / a0);
}

void VoiceFx::processWah(float &l, float &r) {
    float rect = std::max(std::fabs(l), std::fabs(r));
    m_wahEnv += (rect > m_wahEnv ? m_wahEnvAtk : m_wahEnvRel) * (rect - m_wahEnv);
    m_wahEnv = flushDenorm(m_wahEnv);

    if (--m_wahCoeffCountdown <= 0) {
        m_wahCoeffCountdown = 16;
        float sens = clamp01(m_p.vfxWahSens);
        // Log-dB envelope mapping (-48..-4 dBFS -> 0..1) sweeps across both quiet speech & loud audio.
        float envDb = 20.0f * std::log10(std::max(1e-5f, m_wahEnv));
        float normDb = clamp01((envDb + 46.0f) / 34.0f);
        float sweep = clamp01(normDb * (0.20f + 0.90f * sens) + 0.5f * m_liveWahBias);
        float fmin = std::max(80.0f,  m_p.vfxWahMinHz);
        float fmax = std::min(8000.0f, std::max(fmin + 50.0f, m_p.vfxWahMaxHz));
        float center = fmin * std::pow(fmax / fmin, sweep);
        wahRecalc(center);
    }

    float yl = flushDenorm(m_wahB0 * l + m_wahB2 * m_wahX2L - m_wahA1 * m_wahY1L - m_wahA2 * m_wahY2L);
    m_wahX2L = m_wahX1L; m_wahX1L = l;
    m_wahY2L = m_wahY1L; m_wahY1L = yl;
    float yr = flushDenorm(m_wahB0 * r + m_wahB2 * m_wahX2R - m_wahA1 * m_wahY1R - m_wahA2 * m_wahY2R);
    m_wahX2R = m_wahX1R; m_wahX1R = r;
    m_wahY2R = m_wahY1R; m_wahY1R = yr;

    float mix = clamp01(m_p.vfxWahMix);
    float wetWahL = 2.6f * yl;
    float wetWahR = 2.6f * yr;
    l = (1.0f - mix) * l + mix * wetWahL;
    r = (1.0f - mix) * r + mix * wetWahR;
}

void VoiceFx::processExciter(float &l, float &r) {
    float drive = std::min(10.0f, std::max(1.0f, m_p.vfxExcDrive));
    float mix   = clamp01(m_p.vfxExcMix);

    auto shape = [drive](float x) { return std::tanh(drive * x); };

    m_excHpL = flushDenorm(m_excHpL + m_excHpCoeff * (l - m_excHpL));
    m_excHpR = flushDenorm(m_excHpR + m_excHpCoeff * (r - m_excHpR));
    float hpL = l - m_excHpL;
    float hpR = r - m_excHpR;

    float satL, satR;
    if (m_p.hqOversampling) {
        float midL = 0.5f * (m_excPrevL + hpL);
        float midR = 0.5f * (m_excPrevR + hpR);
        satL = 0.5f * (shape(midL) + shape(hpL));
        satR = 0.5f * (shape(midR) + shape(hpR));
    } else {
        satL = shape(hpL);
        satR = shape(hpR);
    }
    m_excPrevL = hpL;
    m_excPrevR = hpR;

    // Added harmonics can sum to +-1 on top of a full-scale dry: soft-limit the result (ceiling 1.0).
    if (mix > 0.0f) {
        l = softLimit(l + mix * satL);
        r = softLimit(r + mix * satR);
    }
}

// --------------------------------------------------------------------
// Reverse delay (Dual 50% overlapping Hann-windowed reversed grains)
// --------------------------------------------------------------------

void VoiceFx::revRebuild() {
    // Only updates the active grain length (buffers were allocated once in setSampleRate).
    float ms = std::min(2000.0f, std::max(100.0f, m_p.vfxRevTimeMs));
    int n = static_cast<int>(ms * 0.001 * m_fs);
    if (n < 256) n = 256;
    if (n & 1) ++n; // even length for exact N/2 overlap
    if (n > m_revMax) n = m_revMax;
    if (n < 2) return; // ring not allocated yet (setSampleRate not called)
    m_revN = n;
}

void VoiceFx::processRevDelay(float &l, float &r) {
    if (m_revN <= 0 || m_revRing <= 0) return;
    const int M = m_revRing;

    // Start a new grain when the latest one is half way through (50% overlap, also while the length
    // changes): it reads the N samples before "now" backwards.
    bool startNew = true;
    if (m_revLast >= 0) {
        const RevGrain &lg = m_revG[m_revLast];
        startNew = !lg.active || lg.pos >= std::min(lg.len, m_revN) / 2;
    }
    if (startNew) {
        for (int g = 0; g < kRevGrains; ++g) {
            if (!m_revG[g].active) {
                m_revG[g].start = m_revW;
                m_revG[g].len = m_revN;
                m_revG[g].pos = 0;
                m_revG[g].active = true;
                m_revLast = g;
                break;
            }
        }
    }

    float wetL = 0.0f, wetR = 0.0f;
    for (int g = 0; g < kRevGrains; ++g) {
        RevGrain &gr = m_revG[g];
        if (!gr.active) continue;
        float w = 0.5f * (1.0f - std::cos(2.0f * kPi * static_cast<float>(gr.pos) / static_cast<float>(gr.len)));
        int idx = gr.start - 1 - gr.pos;
        if (idx < 0) idx += M;
        wetL += m_revCapL[idx] * w;
        wetR += m_revCapR[idx] * w;
        if (++gr.pos >= gr.len) gr.active = false;
    }

    float fb = std::min(0.9f, std::max(0.0f, m_p.vfxRevFeedback));
    m_revCapL[m_revW] = flushDenorm(l + fb * std::tanh(wetL));
    m_revCapR[m_revW] = flushDenorm(r + fb * std::tanh(wetR));
    if (++m_revW >= M) m_revW = 0;

    float mix = clamp01(m_p.vfxRevMix);
    l = (1.0f - mix) * l + mix * wetL;
    r = (1.0f - mix) * r + mix * wetR;
}

// --------------------------------------------------------------------
// Shimmer (pitch-shifted feedback reverb tail)
// --------------------------------------------------------------------

void VoiceFx::processShimmer(float &l, float &r) {
    auto allpass = [](std::vector<float> &buf, int &w, float x) -> float {
        constexpr float g = 0.55f;
        float v = buf[w];
        // Schroeder allpass (unity gain): w = x + g*v; out = v - g*w; store w.
        float wv = x + g * v;
        float out = v - g * wv;
        buf[w] = flushDenorm(wv);
        if (++w >= static_cast<int>(buf.size())) w = 0;
        return out;
    };

    float fb   = std::min(0.9f, std::max(0.0f, m_p.vfxShimFeedback));
    float damp = clamp01(m_p.vfxShimDamp);
    float mix  = clamp01(m_p.vfxShimMix);

    float tailL = m_shimDlyL[m_shimWL];
    float tailR = m_shimDlyR[m_shimWR];

    m_shimLpL = flushDenorm(m_shimLpL + (1.0f - damp) * (tailL - m_shimLpL));
    m_shimLpR = flushDenorm(m_shimLpR + (1.0f - damp) * (tailR - m_shimLpR));

    float upL = m_shimShiftL.process(m_shimLpL);
    float upR = m_shimShiftR.process(m_shimLpR);

    float inL = allpass(m_shimApL, m_shimApWL, l + fb * std::tanh(0.8f * upL + 0.2f * upR));
    float inR = allpass(m_shimApR, m_shimApWR, r + fb * std::tanh(0.8f * upR + 0.2f * upL));

    m_shimDlyL[m_shimWL] = flushDenorm(inL);
    m_shimDlyR[m_shimWR] = flushDenorm(inR);
    if (++m_shimWL >= static_cast<int>(m_shimDlyL.size())) m_shimWL = 0;
    if (++m_shimWR >= static_cast<int>(m_shimDlyR.size())) m_shimWR = 0;

    l += mix * tailL;
    r += mix * tailR;
}
