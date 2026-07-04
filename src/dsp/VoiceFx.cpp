#include "VoiceFx.h"
#include "leia/SimpleFFT.h"

#include <cmath>
#include <cstring>
#include <algorithm>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr double kTwoPi = 6.28318530717958647692;

inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// One-pole coefficient for a time constant in ms.
inline float onePoleCoeff(double ms, double fs) {
    if (ms <= 0.0) return 1.0f;
    return static_cast<float>(1.0 - std::exp(-1.0 / (ms * 0.001 * fs)));
}
} // namespace

VoiceFx::VoiceFx() {
    setSampleRate(48000.0);
}

VoiceFx::~VoiceFx() = default;

void VoiceFx::setSampleRate(double sr) {
    m_fs = (sr > 0) ? sr : 48000.0;

    m_tuneShift.setSampleRate(m_fs);
    m_tuneShift.setWindowMs(35.0f);
    m_shimShiftL.setSampleRate(m_fs);
    m_shimShiftR.setSampleRate(m_fs);
    m_shimShiftL.setWindowMs(60.0f);
    m_shimShiftR.setWindowMs(60.0f);

    if (!m_formFft) m_formFft.reset(new SimpleFFT(kFormN));
    m_formIn.assign(kFormN, 0.0f);
    m_formOut.assign(kFormN * 2, 0.0f);
    m_formWin.resize(kFormN);
    for (int i = 0; i < kFormN; ++i)
        m_formWin[i] = 0.5f - 0.5f * std::cos(kTwoPi * i / kFormN);
    m_formFreq.assign(kFormN + 2, 0.0f);
    m_formEnv.assign(kFormN / 2 + 1, 0.0f);
    m_formEnvW.assign(kFormN / 2 + 1, 0.0f);
    m_formTime.assign(kFormN, 0.0f);
    m_formFill = 0;
    m_formReadIdx = 0;
    m_formPrimed = false;

    // Shimmer loop delays: prime-ish lengths ~90 / ~100 ms.
    int dl = static_cast<int>(0.090 * m_fs) | 1;
    int dr = static_cast<int>(0.101 * m_fs) | 1;
    m_shimDlyL.assign(dl, 0.0f);
    m_shimDlyR.assign(dr, 0.0f);
    m_shimApL.assign(137, 0.0f);
    m_shimApR.assign(149, 0.0f);
    m_shimWL = m_shimWR = m_shimApWL = m_shimApWR = 0;
    m_shimLpL = m_shimLpR = 0.0f;

    m_vocLastPitch = -1.0f;   // force vocoder rebuild
    m_revLastTimeMs = -1.0f;  // force reverse-delay rebuild
    setParams(m_p);
    reset();
}

void VoiceFx::setParams(const SandboxState &s) {
    m_p = s;

    // Live-mod baselines.
    m_liveRingFreq = s.vfxRingFreq;
    m_liveTremRate = s.vfxTremRate;
    m_liveVibDepth = s.vfxVibDepth;
    m_liveWahBias  = 0.0f;

    // Wah envelope follower.
    m_wahEnvAtk = onePoleCoeff(5.0, m_fs);
    m_wahEnvRel = onePoleCoeff(120.0, m_fs);

    // Exciter split HP.
    float f = std::min(8000.0f, std::max(1000.0f, s.vfxExcFreq));
    m_excHpCoeff = static_cast<float>(
        1.0 - std::exp(-kTwoPi * f / m_fs));

    // Autotune retune glide (per sample).
    m_tuneSmCoeff = onePoleCoeff(std::max(1.0f, s.vfxTuneSpeedMs), m_fs);

    // Vocoder band rebuild only when the carrier actually changed.
    if (std::fabs(s.vfxVocPitchHz - m_vocLastPitch) > 0.01f) {
        vocRebuild();
        m_vocLastPitch = s.vfxVocPitchHz;
    }
    m_vocAtk = onePoleCoeff(3.0, m_fs);
    m_vocRel = onePoleCoeff(60.0, m_fs);

    // Reverse-delay grain resize on time change.
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
    std::memset(m_tuneRing, 0, sizeof(m_tuneRing));
    m_tuneW = 0;
    m_tuneHopCounter = 0;
    m_tuneRatioTarget = 1.0f;
    m_tuneRatioSm = 1.0f;
    m_tuneShift.reset();
    std::fill(m_formIn.begin(), m_formIn.end(), 0.0f);
    std::fill(m_formOut.begin(), m_formOut.end(), 0.0f);
    m_formFill = 0;
    m_formInW = 0;
    m_formReadIdx = 0;
    m_formPrimed = false;
    for (int i = 0; i < kVocBands; ++i) {
        m_voc[i].mx1 = m_voc[i].mx2 = m_voc[i].my1 = m_voc[i].my2 = 0.0f;
        m_voc[i].cx1 = m_voc[i].cx2 = m_voc[i].cy1 = m_voc[i].cy2 = 0.0f;
        m_voc[i].env = 0.0f;
    }
    m_vocSawPhase = 0.0;
    std::fill(m_revBufL.begin(), m_revBufL.end(), 0.0f);
    std::fill(m_revBufR.begin(), m_revBufR.end(), 0.0f);
    m_revIdx = 0;
    std::fill(m_shimDlyL.begin(), m_shimDlyL.end(), 0.0f);
    std::fill(m_shimDlyR.begin(), m_shimDlyR.end(), 0.0f);
    std::fill(m_shimApL.begin(), m_shimApL.end(), 0.0f);
    std::fill(m_shimApR.begin(), m_shimApR.end(), 0.0f);
    m_shimWL = m_shimWR = m_shimApWL = m_shimApWR = 0;
    m_shimLpL = m_shimLpR = 0.0f;
    m_shimShiftL.reset();
    m_shimShiftR.reset();
}

// --------------------------------------------------------------------
// Main entry
// --------------------------------------------------------------------

void VoiceFx::processStereo(float &l, float &r) {
    const SandboxState &s = m_p;

    // Voice-centric mono block: autotune -> formant -> vocoder.
    if (s.vfxTuneEnabled || s.vfxFormEnabled || s.vfxVocEnabled) {
        float m = 0.5f * (l + r);
        if (s.vfxTuneEnabled) processTune(m);
        if (s.vfxFormEnabled) processFormantSample(m);
        if (s.vfxVocEnabled)  processVocoder(m);
        l = r = m;
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
// Autotune
// --------------------------------------------------------------------

float VoiceFx::detectPitch() {
    // Autocorrelation over the freshest 1024 samples of the ring.
    constexpr int W = 1024;
    float frame[W];
    for (int i = 0; i < W; ++i)
        frame[i] = m_tuneRing[(m_tuneW - W + i + kTuneBuf) % kTuneBuf];

    float energy = 0.0f;
    for (int i = 0; i < W; ++i) energy += frame[i] * frame[i];
    if (energy < 1e-4f) return -1.0f;    // silence

    const int minLag = static_cast<int>(m_fs / 500.0);   // <= 500 Hz
    const int maxLag = std::min(W / 2, static_cast<int>(m_fs / 70.0)); // >= 70 Hz
    if (maxLag <= minLag) return -1.0f;

    float bestCorr = 0.0f;
    int   bestLag = -1;
    for (int lag = minLag; lag <= maxLag; ++lag) {
        float sum = 0.0f, norm = 0.0f;
        for (int i = 0; i + lag < W; i += 2) {   // stride 2: half cost, ample data
            sum  += frame[i] * frame[i + lag];
            norm += frame[i] * frame[i] + frame[i + lag] * frame[i + lag];
        }
        float c = (norm > 1e-9f) ? (2.0f * sum / norm) : 0.0f;
        if (c > bestCorr) { bestCorr = c; bestLag = lag; }
    }
    if (bestLag < 0 || bestCorr < 0.45f) return -1.0f;   // unvoiced
    return static_cast<float>(m_fs / bestLag);
}

float VoiceFx::nearestScaleFreq(float hz) const {
    static const int kMajor[7] = { 0, 2, 4, 5, 7, 9, 11 };
    static const int kMinor[7] = { 0, 2, 3, 5, 7, 8, 10 };

    float midi = 69.0f + 12.0f * std::log2(hz / 440.0f);
    int centre = static_cast<int>(std::lround(midi));

    int bestNote = centre;
    float bestDist = 1e9f;
    for (int n = centre - 6; n <= centre + 6; ++n) {
        int pc = ((n - m_p.vfxTuneKey) % 12 + 12) % 12;
        bool inScale = true;
        if (m_p.vfxTuneScale == 1) {
            inScale = false;
            for (int j = 0; j < 7; ++j) if (kMajor[j] == pc) { inScale = true; break; }
        } else if (m_p.vfxTuneScale == 2) {
            inScale = false;
            for (int j = 0; j < 7; ++j) if (kMinor[j] == pc) { inScale = true; break; }
        }
        if (!inScale) continue;
        float d = std::fabs(midi - n);
        if (d < bestDist) { bestDist = d; bestNote = n; }
    }
    return 440.0f * std::pow(2.0f, (bestNote - 69) / 12.0f);
}

void VoiceFx::processTune(float &m) {
    m_tuneRing[m_tuneW] = m;
    m_tuneW = (m_tuneW + 1) % kTuneBuf;

    if (++m_tuneHopCounter >= 256) {
        m_tuneHopCounter = 0;
        float hz = detectPitch();
        if (hz > 0.0f) {
            float target = nearestScaleFreq(hz);
            float ratio = target / hz;
            if (ratio < 0.5f) ratio = 0.5f;
            if (ratio > 2.0f) ratio = 2.0f;
            // Strength warps the correction toward/away from full snap.
            float st = clamp01(m_p.vfxTuneStrength);
            m_tuneRatioTarget = std::exp(std::log(ratio) * st);
        } else {
            m_tuneRatioTarget = 1.0f;   // unvoiced: glide back to unity
        }
    }

    m_tuneRatioSm += (m_tuneRatioTarget - m_tuneRatioSm) * m_tuneSmCoeff;
    m_tuneShift.setRatio(m_tuneRatioSm);
    m = m_tuneShift.process(m);
}

// --------------------------------------------------------------------
// Formant shifter (FFT OLA envelope warp)
// --------------------------------------------------------------------

void VoiceFx::formantFrame() {
    const int bins = kFormN / 2 + 1;

    // Analysis window + FFT. m_formIn is a ring; m_formInW is the next
    // write slot, i.e. the OLDEST sample - unroll chronologically.
    for (int i = 0; i < kFormN; ++i) {
        int idx = (m_formInW + i) % kFormN;   // oldest -> newest
        m_formTime[i] = m_formIn[idx] * m_formWin[i];
    }
    m_formFft->forward(m_formTime.data(), m_formFreq.data());

    // Magnitude spectrum -> smoothed spectral envelope (two box passes
    // = triangular smoothing, width ~17 bins ≈ 800 Hz at 48 kHz).
    for (int k = 0; k < bins; ++k) {
        float re = m_formFreq[2 * k], im = m_formFreq[2 * k + 1];
        m_formEnv[k] = std::sqrt(re * re + im * im);
    }
    auto boxSmooth = [bins](std::vector<float> &v) {
        constexpr int R = 8;
        static thread_local std::vector<float> tmp;
        tmp.assign(v.begin(), v.begin() + bins);
        for (int k = 0; k < bins; ++k) {
            float acc = 0.0f;
            int lo = std::max(0, k - R), hi = std::min(bins - 1, k + R);
            for (int j = lo; j <= hi; ++j) acc += tmp[j];
            v[k] = acc / static_cast<float>(hi - lo + 1);
        }
    };
    boxSmooth(m_formEnv);
    boxSmooth(m_formEnv);

    // Warped envelope: envW(k) = env(k / ratio).
    float ratio = std::pow(2.0f, m_p.vfxFormShift / 12.0f);
    for (int k = 0; k < bins; ++k) {
        float src = static_cast<float>(k) / ratio;
        int i0 = static_cast<int>(src);
        if (i0 >= bins - 1) { m_formEnvW[k] = m_formEnv[bins - 1]; continue; }
        float f = src - i0;
        m_formEnvW[k] = m_formEnv[i0] * (1.0f - f) + m_formEnv[i0 + 1] * f;
    }

    // Per-bin correction gain, clamped so envelope nulls can't explode.
    constexpr float kEps = 1e-6f;
    for (int k = 0; k < bins; ++k) {
        float g = (m_formEnvW[k] + kEps) / (m_formEnv[k] + kEps);
        if (g > 12.0f) g = 12.0f;
        if (g < 0.08f) g = 0.08f;
        m_formFreq[2 * k]     *= g;
        m_formFreq[2 * k + 1] *= g;
    }

    // Inverse FFT + synthesis window + OLA.
    m_formFft->inverse(m_formFreq.data(), m_formTime.data());
    const float invN = 1.0f / static_cast<float>(kFormN);
    // Hann^2 at 75% overlap sums to 1.5 - fold that into the scale.
    const float olaNorm = invN / 1.5f;
    for (int i = 0; i < kFormN; ++i)
        m_formOut[i] += m_formTime[i] * m_formWin[i] * olaNorm;
}

void VoiceFx::processFormantSample(float &m) {
    // m_formIn is a RING holding the freshest kFormN samples (m_formInW
    // = next write position = oldest sample). Every kFormHop samples:
    // shift the OLA accumulator left by one hop and add a freshly
    // processed frame; the first hop of the accumulator is then fully
    // summed and gets streamed out over the next kFormHop samples.
    // Constant kFormN-sample latency.
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

    float wet = 0.0f;
    if (m_formPrimed && m_formReadIdx < kFormHop)
        wet = m_formOut[m_formReadIdx++];

    float mix = clamp01(m_p.vfxFormMix);
    m = (1.0f - mix) * m + mix * wet;
}

// --------------------------------------------------------------------
// Vocoder
// --------------------------------------------------------------------

void VoiceFx::vocRebuild() {
    // Log-spaced band centres 100 Hz .. 7.5 kHz, Q ~ matched to spacing.
    for (int i = 0; i < kVocBands; ++i) {
        double t = static_cast<double>(i) / (kVocBands - 1);
        double fc = 100.0 * std::pow(75.0, t);
        double q  = 4.5;
        double w0 = kTwoPi * fc / m_fs;
        double alpha = std::sin(w0) / (2.0 * q);
        double a0 = 1.0 + alpha;
        m_voc[i].b0 = static_cast<float>(alpha / a0);
        m_voc[i].b1 = 0.0f;
        m_voc[i].b2 = static_cast<float>(-alpha / a0);
        m_voc[i].a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
        m_voc[i].a2 = static_cast<float>((1.0 - alpha) / a0);
    }
}

void VoiceFx::processVocoder(float &m) {
    // Carrier sample.
    float car;
    if (m_p.vfxVocCarrier == 1) {
        m_vocNoise = m_vocNoise * 1664525u + 1013904223u;
        car = (static_cast<float>(m_vocNoise >> 9) / 4194304.0f) - 1.0f;
    } else {
        double inc = m_p.vfxVocPitchHz / m_fs;
        m_vocSawPhase += inc;
        if (m_vocSawPhase >= 1.0) m_vocSawPhase -= 1.0;
        car = static_cast<float>(2.0 * m_vocSawPhase - 1.0);
    }

    float sum = 0.0f;
    for (int i = 0; i < kVocBands; ++i) {
        VocBand &b = m_voc[i];
        // Modulator band.
        float my = b.b0 * m + b.b2 * b.mx2 - b.a1 * b.my1 - b.a2 * b.my2;
        b.mx2 = b.mx1; b.mx1 = m;
        b.my2 = b.my1; b.my1 = my;
        // Carrier band.
        float cy = b.b0 * car + b.b2 * b.cx2 - b.a1 * b.cy1 - b.a2 * b.cy2;
        b.cx2 = b.cx1; b.cx1 = car;
        b.cy2 = b.cy1; b.cy1 = cy;
        // Envelope of the modulator band drives the carrier band.
        float rect = std::fabs(my);
        b.env += (rect > b.env ? m_vocAtk : m_vocRel) * (rect - b.env);
        sum += cy * b.env;
    }

    float wet = sum * 4.0f;   // makeup for narrow-band energy loss
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
    // ~1 ms smoothing de-clicks the square shape.
    m_tremGainSm += (target - m_tremGainSm) * 0.02f;
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
    // 1..~8.5 ms swing.
    float delay = 48.0f + depth * static_cast<float>(0.0075 * m_fs)
                          * (0.5f + 0.5f * std::sin(static_cast<float>(m_vibPhase)));
    if (delay > kVibMax - 2) delay = kVibMax - 2;

    auto readFrac = [](const float *buf, int w, float d) -> float {
        float pos = static_cast<float>(w) - d;
        while (pos < 0.0f) pos += kVibMax;
        int i0 = static_cast<int>(pos);
        int i1 = (i0 + 1) % kVibMax;
        float f = pos - i0;
        return buf[i0] * (1.0f - f) + buf[i1] * f;
    };
    l = readFrac(m_vibBufL, m_vibW, delay);
    r = readFrac(m_vibBufR, m_vibW, delay);
    m_vibW = (m_vibW + 1) % kVibMax;
}

void VoiceFx::wahRecalc(float centerHz) {
    float q = std::min(12.0f, std::max(1.0f, m_p.vfxWahQ));
    double w0 = kTwoPi * centerHz / m_fs;
    if (w0 > 3.0) w0 = 3.0;
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

    if (--m_wahCoeffCountdown <= 0) {
        m_wahCoeffCountdown = 16;
        float sens = clamp01(m_p.vfxWahSens);
        float sweep = clamp01(m_wahEnv * sens * 8.0f + 0.5f * m_liveWahBias);
        float fmin = std::max(80.0f,  m_p.vfxWahMinHz);
        float fmax = std::min(8000.0f, std::max(fmin + 50.0f, m_p.vfxWahMaxHz));
        float center = fmin * std::pow(fmax / fmin, sweep);
        wahRecalc(center);
    }

    float yl = m_wahB0 * l + m_wahB2 * m_wahX2L - m_wahA1 * m_wahY1L - m_wahA2 * m_wahY2L;
    m_wahX2L = m_wahX1L; m_wahX1L = l;
    m_wahY2L = m_wahY1L; m_wahY1L = yl;
    float yr = m_wahB0 * r + m_wahB2 * m_wahX2R - m_wahA1 * m_wahY1R - m_wahA2 * m_wahY2R;
    m_wahX2R = m_wahX1R; m_wahX1R = r;
    m_wahY2R = m_wahY1R; m_wahY1R = yr;

    float mix = clamp01(m_p.vfxWahMix);
    // The BPF strips a lot of energy; 2x makeup keeps wah roughly
    // level-matched with the dry path.
    l = (1.0f - mix) * l + mix * (2.0f * yl);
    r = (1.0f - mix) * r + mix * (2.0f * yr);
}

void VoiceFx::processExciter(float &l, float &r) {
    float drive = std::min(10.0f, std::max(1.0f, m_p.vfxExcDrive));
    float mix   = clamp01(m_p.vfxExcMix);

    auto shape = [drive](float x) { return std::tanh(drive * x); };

    // 1-pole highpass extracts the band to excite.
    m_excHpL += m_excHpCoeff * (l - m_excHpL);
    m_excHpR += m_excHpCoeff * (r - m_excHpR);
    float hpL = l - m_excHpL;
    float hpR = r - m_excHpR;

    float satL, satR;
    if (m_p.hqOversampling) {
        // 2x oversampled shaper: shape the midpoint too, average both.
        // Crude halfband, halves the alias energy of the tanh.
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

    l += mix * satL;
    r += mix * satR;
}

// --------------------------------------------------------------------
// Reverse delay (grain-reversed echo)
// --------------------------------------------------------------------

void VoiceFx::revRebuild() {
    float ms = std::min(2000.0f, std::max(100.0f, m_p.vfxRevTimeMs));
    int n = static_cast<int>(ms * 0.001 * m_fs);
    if (n < 256) n = 256;
    m_revN = n;
    // Two grains per channel: [0..N) = capture, [N..2N) = playback.
    m_revBufL.assign(static_cast<size_t>(n) * 2, 0.0f);
    m_revBufR.assign(static_cast<size_t>(n) * 2, 0.0f);
    m_revIdx = 0;
}

void VoiceFx::processRevDelay(float &l, float &r) {
    if (m_revN <= 0) return;
    float *capL = m_revBufL.data();
    float *capR = m_revBufR.data();
    float *plyL = m_revBufL.data() + m_revN;
    float *plyR = m_revBufR.data() + m_revN;

    // Play the PREVIOUS grain backwards while capturing the current.
    float wetL = plyL[m_revN - 1 - m_revIdx];
    float wetR = plyR[m_revN - 1 - m_revIdx];

    float fb = std::min(0.9f, std::max(0.0f, m_p.vfxRevFeedback));
    capL[m_revIdx] = l + fb * wetL;
    capR[m_revIdx] = r + fb * wetR;

    if (++m_revIdx >= m_revN) {
        m_revIdx = 0;
        // Finished grain becomes the next playback grain.
        std::memcpy(plyL, capL, m_revN * sizeof(float));
        std::memcpy(plyR, capR, m_revN * sizeof(float));
    }

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
        float out = -x + v;
        buf[w] = x + v * g;
        if (++w >= static_cast<int>(buf.size())) w = 0;
        return out;
    };

    float fb   = std::min(0.9f, std::max(0.0f, m_p.vfxShimFeedback));
    float damp = clamp01(m_p.vfxShimDamp);
    float mix  = clamp01(m_p.vfxShimMix);

    // Read loop tails.
    float tailL = m_shimDlyL[m_shimWL];
    float tailR = m_shimDlyR[m_shimWR];

    // Damping LPF inside the loop keeps repeated shifts from screeching.
    m_shimLpL += (1.0f - damp) * (tailL - m_shimLpL);
    m_shimLpR += (1.0f - damp) * (tailR - m_shimLpR);

    // Feedback path is pitch-shifted: each pass rises by shimPitch.
    float upL = m_shimShiftL.process(m_shimLpL);
    float upR = m_shimShiftR.process(m_shimLpR);

    // Slight cross-mix widens the tail.
    float inL = allpass(m_shimApL, m_shimApWL, l + fb * (0.8f * upL + 0.2f * upR));
    float inR = allpass(m_shimApR, m_shimApWR, r + fb * (0.8f * upR + 0.2f * upL));

    m_shimDlyL[m_shimWL] = inL;
    m_shimDlyR[m_shimWR] = inR;
    if (++m_shimWL >= static_cast<int>(m_shimDlyL.size())) m_shimWL = 0;
    if (++m_shimWR >= static_cast<int>(m_shimDlyR.size())) m_shimWR = 0;

    l += mix * tailL;
    r += mix * tailR;
}
