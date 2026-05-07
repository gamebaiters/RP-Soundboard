#include "Positional.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {

constexpr float kDeg2Rad = static_cast<float>(M_PI / 180.0);

// ---- Brown-Duda pinna model coefficients (Table I, 1998 paper). ----
// Times in microseconds; converted to samples per sample rate at runtime.
// Event 1 is the direct path (handled separately as the tap-0 unit gain).
//
// Scaling: literal Brown-Duda rho values colour the signal heavily on
// dense music material. A blanket 0.5 multiplier keeps the elevation
// notch sweep audible while preserving timbral integrity. The k=3 event
// at full -1.0 was the worst offender (full inversion = comb filter,
// 6 dB peaks every 5 kHz) - now -0.5 = ~3 dB peaks.
struct PinnaEvent { float A_us, B_us, D, rho; };
// Soundboard build: very conservative pinna FIR. The user kept
// reporting that the sandbox compressed the audio "like an
// over-compressed YouTube video" - the pinna reflections were the
// main culprit. 0.3 keeps a hint of front/back colour without
// making the source feel processed.
constexpr float kPinnaScale = 0.3f;
constexpr PinnaEvent kPinnaEvents[5] = {
    { 1.0f,  2.0f, 1.0f, +0.50f * kPinnaScale },   // k=2
    { 5.0f,  4.0f, 0.5f, -1.00f * kPinnaScale },   // k=3
    { 5.0f,  7.0f, 0.5f, +0.50f * kPinnaScale },   // k=4
    { 5.0f, 11.0f, 0.5f, -0.25f * kPinnaScale },   // k=5
    { 5.0f, 13.0f, 0.5f, +0.25f * kPinnaScale },   // k=6
};

// Read a fractional-delay sample (linear interp) from a circular buffer.
// `delayInSamples` is positive: delayed past the *previous* write.
inline float readFrac(const std::vector<float> &buf, int writeIdx,
                      float delayInSamples) {
    int n = static_cast<int>(buf.size());
    if (delayInSamples < 0.0f) delayInSamples = 0.0f;
    int  d0 = static_cast<int>(delayInSamples);
    float frac = delayInSamples - d0;
    int idxA = (writeIdx - d0 - 1 + n) % n;
    int idxB = (idxA - 1 + n) % n;
    return buf[idxA] * (1.0f - frac) + buf[idxB] * frac;
}

inline float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

Positional::Positional()
    : m_delayBufL(kMaxItdSamples * 4, 0.0f)
    , m_delayBufR(kMaxItdSamples * 4, 0.0f)
    , m_pinnaBufL(kPinnaBufLen, 0.0f)
    , m_pinnaBufR(kPinnaBufLen, 0.0f)
    , m_tapBufL(kMaxTapBuf, 0.0f)
    , m_tapBufR(kMaxTapBuf, 0.0f)
{
    for (int i = 0; i < kCombCount; ++i)
        m_combBuf[i].assign(m_combLen[i] + 4, 0.0f);
    setSampleRate(48000.0);
}

void Positional::setSampleRate(double sr) {
    m_fs = sr > 0 ? sr : 48000.0;
    // Smoother time constant ~20 ms for position; ITD/shadow handled by
    // chasing the smoothed position, so a single smoother is fine.
    double tau = 0.020;
    m_smoothA = static_cast<float>(std::exp(-1.0 / (tau * m_fs)));
    recomputeCoeffs();
}

void Positional::setPosition(float x, float y, float z) {
    m_xT = x; m_yT = y; m_zT = z;
}

void Positional::setHeadSwayAmount(float deg) {
    if (deg < 0.0f) deg = 0.0f;
    if (deg > 5.0f) deg = 5.0f;
    m_swayAmountDeg = deg;
}

void Positional::reset() {
    std::fill(m_delayBufL.begin(), m_delayBufL.end(), 0.0f);
    std::fill(m_delayBufR.begin(), m_delayBufR.end(), 0.0f);
    std::fill(m_pinnaBufL.begin(), m_pinnaBufL.end(), 0.0f);
    std::fill(m_pinnaBufR.begin(), m_pinnaBufR.end(), 0.0f);
    std::fill(m_tapBufL.begin(),   m_tapBufL.end(),   0.0f);
    std::fill(m_tapBufR.begin(),   m_tapBufR.end(),   0.0f);
    for (int i = 0; i < kCombCount; ++i)
        std::fill(m_combBuf[i].begin(), m_combBuf[i].end(), 0.0f);
    m_shXL = m_shYL = m_shXR = m_shYR = 0.0f;
    m_fbXL1 = m_fbXL2 = m_fbYL1 = m_fbYL2 = 0.0f;
    m_fbXR1 = m_fbXR2 = m_fbYR1 = m_fbYR2 = 0.0f;
    m_chXL1 = m_chXL2 = m_chYL1 = m_chYL2 = 0.0f;
    m_chXR1 = m_chXR2 = m_chYR1 = m_chYR2 = 0.0f;
    m_airLpL = m_airLpR = 0.0f;
    m_delayWriteL = m_delayWriteR = 0;
    m_pinnaWriteL = m_pinnaWriteR = 0;
    m_tapWriteL = m_tapWriteR = 0;
    // Snap the position smoother to its target. Without this the
    // smoother ramps from the previous mode's position to the new
    // one over ~20 ms - the ITD/ILD trajectory during that ramp
    // sounds exactly like a pitch-shifting Doppler artefact ("robot"
    // sound) every time the user activated 3D.
    m_x = m_xT;
    m_y = m_yT;
    m_z = m_zT;
    // Force a coefficient recompute on the next process() so the new
    // position takes effect immediately rather than at the next
    // control-rate boundary.
    m_ctrlCounter = 0;
    recomputeCoeffs();
}

void Positional::smoothTick() {
    // One-pole smoother on the target position. Pulled into the audio
    // thread because position changes happen at GUI rate (~60 Hz) but we
    // want sample-rate granularity to avoid stair-step artifacts.
    float b = 1.0f - m_smoothA;
    m_x += b * (m_xT - m_x);
    m_y += b * (m_yT - m_y);
    m_z += b * (m_zT - m_z);
}

void Positional::recomputeCoeffs() {
    float x = m_x, y = m_y, z = m_z;

    // Head-sway LFO: advance phase, rotate (x, y) by a small azimuth
    // wobble. Phase advances on each control-rate update so the wobble
    // frequency is independent of sample rate.
    if (m_swayAmountDeg > 1e-3f) {
        constexpr double kSwayHz = 0.30;
        double dPhase = 2.0 * M_PI * kSwayHz * (kCtrlBlock / m_fs);
        m_swayPhase += dPhase;
        if (m_swayPhase > 2.0 * M_PI) m_swayPhase -= 2.0 * M_PI;
        float swayRad = m_swayAmountDeg * kDeg2Rad *
                         static_cast<float>(std::sin(m_swayPhase));
        float cs = std::cos(swayRad), sn = std::sin(swayRad);
        float rx =  x * cs + y * sn;
        float ry = -x * sn + y * cs;
        x = rx; y = ry;
    }

    float r = std::sqrt(x*x + y*y + z*z);
    if (r < 0.05f) r = 0.05f;
    float invR = 1.0f / r;
    float ux = x * invR, uy = y * invR, uz = z * invR;

    // ===== Stage 1: distance gain + air absorption =====
    // Tapered 1/r model. For the soundboard build the slope is gentle
    // (0.15 instead of 0.5) so a default 1.5 m position only loses
    // about -1.5 dB instead of -5 dB - the user expected the audio
    // sandbox to spatialise without "compressing" the source.
    m_gDist = 1.0f / (1.0f + r * 0.15f);

    // Air absorption: ~-0.6 dB/m at 10 kHz. Implement as a one-pole LP
    // mixed back with the dry signal so the shelf knee sits at 10 kHz.
    {
        float dbAt10k = -0.6f * r;
        float gHF = std::pow(10.0f, dbAt10k / 20.0f);
        if (gHF > 1.0f) gHF = 1.0f;
        if (gHF < 0.05f) gHF = 0.05f;
        m_airMix = gHF;                 // dry gain
        float fc = 10000.0f;
        float w  = 2.0f * static_cast<float>(M_PI) * fc / static_cast<float>(m_fs);
        m_airAlpha = std::exp(-w);      // LP feedback coefficient
    }

    // ===== Stage 2: ITD via Woodworth =====
    // Lateral angle theta in [-pi/2, +pi/2]: lateral component of source
    // direction projected onto x axis. Cones of constant lateral angle
    // share ITD, so we don't need the full az/el split here.
    float lateral = ux;        // positive = right side
    if (lateral > 1.0f)  lateral = 1.0f;
    if (lateral < -1.0f) lateral = -1.0f;
    float theta = std::asin(lateral);
    float itdSec = (kHeadRadius / kSpeedSound) * (std::abs(theta) + std::sin(std::abs(theta)));
    float itdSamples = itdSec * static_cast<float>(m_fs);
    if (itdSamples > kMaxItdSamples - 4) itdSamples = kMaxItdSamples - 4;
    if (lateral >= 0) {
        // Source on right -> left ear sees the longer path.
        m_itdSamplesR = 0.0f;
        m_itdSamplesL = itdSamples;
    } else {
        m_itdSamplesL = 0.0f;
        m_itdSamplesR = itdSamples;
    }

    // ===== Stage 3: head-shadow filter per ear =====
    // alpha(theta_inc) = 1 + (source_dir . ear_dir).
    // Ears at +/- (90 - 10) deg = ±80 deg from front, in the horizontal plane.
    {
        float earXR = std::cos(kEarOffsetDeg * kDeg2Rad);
        float earYR = std::sin(kEarOffsetDeg * kDeg2Rad);
        float earXL = -earXR;
        float earYL =  earYR;
        float dotR = ux * earXR + uy * earYR;
        float dotL = ux * earXL + uy * earYL;
        float alphaR = clamp(1.0f + dotR, 0.05f, 2.0f);
        float alphaL = clamp(1.0f + dotL, 0.05f, 2.0f);

        // Brown-Duda 1p/1z, beta = 2c/a ~ 7840 rad/s, T = 1/fs.
        float beta = 2.0f * kSpeedSound / kHeadRadius;
        float T = 1.0f / static_cast<float>(m_fs);
        float bd2 = beta * 0.5f;
        float invT = 1.0f / T;
        auto computeBiquad = [&](float alpha, float &b0, float &b1, float &a1) {
            float den = bd2 + invT;
            b0 = (bd2 + alpha * invT) / den;
            b1 = (bd2 - alpha * invT) / den;
            a1 = (bd2 - invT) / den;
        };
        computeBiquad(alphaL, m_shB0L, m_shB1L, m_shA1L);
        computeBiquad(alphaR, m_shB0R, m_shB1R, m_shA1R);
    }

    // ===== Stage 4: pinna sparse-FIR delays per ear =====
    // theta_pinna = arccos(-uy) = angle from front (front is uy=-1 -> 0 deg).
    // phi_deg     = elevation derived from uz.
    {
        float thetaDeg = std::acos(clamp(-uy, -1.0f, 1.0f)) / kDeg2Rad;  // 0..180
        float phiDeg   = std::asin(clamp(uz, -1.0f, 1.0f))  / kDeg2Rad;  // -90..+90
        float thetaSign = (ux >= 0) ? +1.0f : -1.0f;        // ±side bias

        float cosHalfTheta = std::cos((thetaDeg * kDeg2Rad) * 0.5f);
        if (cosHalfTheta < 0.0f) cosHalfTheta = 0.0f;

        for (int k = 0; k < kPinnaEvents; ++k) {
            const auto &E = ::kPinnaEvents[k];
            float tauUs = E.A_us * cosHalfTheta *
                          std::sin(E.D * (90.0f - phiDeg) * kDeg2Rad) + E.B_us;
            float tauSamples = tauUs * 1e-6f * static_cast<float>(m_fs);
            if (tauSamples < 0.0f) tauSamples = 0.0f;
            if (tauSamples > kPinnaBufLen - 4) tauSamples = kPinnaBufLen - 4;
            // Ears differ by mirroring the lateral side of the pinna:
            // shorten R-ear delays slightly when source is on the right
            // (concha less obstructed) and vice versa for left ear.
            float lateralBias = 0.85f + 0.15f * thetaSign;     // 0.7..1.0
            m_pinnaTapR[k] = tauSamples * (2.0f - lateralBias);
            m_pinnaTapL[k] = tauSamples * lateralBias;
            m_pinnaGainL[k] = E.rho;
            m_pinnaGainR[k] = E.rho;
        }
    }

    // ===== Stage 5: shoulder + torso reflections =====
    {
        float shoulderSec = 0.00035f;     // 0.35 ms
        float torsoSec    = 0.00100f;     // 1.0 ms
        float shoulderSamples = shoulderSec * static_cast<float>(m_fs);
        float torsoSamples    = torsoSec * static_cast<float>(m_fs);
        if (shoulderSamples > kMaxTapBuf - 4) shoulderSamples = kMaxTapBuf - 4;
        if (torsoSamples    > kMaxTapBuf - 4) torsoSamples    = kMaxTapBuf - 4;

        float cosPhiSqr = 1.0f - uz * uz;             // ~ cos^2(elevation)
        float shoulderGain = 0.20f * cosPhiSqr;
        // Torso reflection grows for sources below the horizon.
        float torsoGain = 0.10f * std::max(0.0f, -uz);

        m_shoulderTapL = shoulderSamples;
        m_shoulderTapR = shoulderSamples;
        m_shoulderGainL = shoulderGain;
        m_shoulderGainR = shoulderGain;
        m_torsoTapL = torsoSamples;
        m_torsoTapR = torsoSamples;
        m_torsoGainL = torsoGain;
        m_torsoGainR = torsoGain;
    }

    // ===== Stage 6: front/back peaking notch =====
    {
        // Front-of-head gets a wide +3 dB shelf around 5 kHz (concha
        // resonance brightening). Back-of-head gets a narrow, very deep
        // notch around 7.5 kHz - this is the dominant monaural cue our
        // brain uses to localise sources behind us. The previous -10 dB
        // wasn't strong enough; a real head's pinna can produce a
        // 15..25 dB notch in this band when the source is behind, so
        // -18 dB / Q=4 lands in the right perceptual ballpark.
        float front = clamp(-uy, -1.0f, 1.0f);    // 1 = front, -1 = back
        float fc, gainDb, Q;
        if (front >= 0) {
            fc = 5000.0f;
            gainDb = +3.0f * front;
            Q = 0.7f;                              // wide shelf-like boost
        } else {
            fc = 7500.0f;
            gainDb = -18.0f * (-front);
            Q = 4.0f;                              // narrow deep cut
        }
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * static_cast<float>(M_PI) * fc / static_cast<float>(m_fs);
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);
        float bb0 = 1.0f + alpha * A;
        float bb1 = -2.0f * cosw0;
        float bb2 = 1.0f - alpha * A;
        float aa0 = 1.0f + alpha / A;
        float aa1 = -2.0f * cosw0;
        float aa2 = 1.0f - alpha / A;
        m_fbB0L = bb0 / aa0; m_fbB1L = bb1 / aa0; m_fbB2L = bb2 / aa0;
        m_fbA1L = aa1 / aa0; m_fbA2L = aa2 / aa0;
        m_fbB0R = m_fbB0L; m_fbB1R = m_fbB1L; m_fbB2R = m_fbB2L;
        m_fbA1R = m_fbA1L; m_fbA2R = m_fbA2L;
    }

    // ===== Stage 6b: concha boost peaking biquad =====
    // 3 kHz +/-4 dB driven by source front-ness. Both ears share the
    // same coefficients because the front/back spectral cue is monaural
    // (Blauert 1969 - listeners discriminate front/back from spectrum
    // shape at one ear, not from interaural differences).
    {
        float front = clamp(-uy, -1.0f, 1.0f);    // 1 = front, -1 = back
        float gainDb = 4.0f * front;              // +4 front, -4 back
        float fc = 3000.0f;
        float Q  = 1.0f;
        float A  = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * static_cast<float>(M_PI) * fc / static_cast<float>(m_fs);
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);
        float bb0 = 1.0f + alpha * A;
        float bb1 = -2.0f * cosw0;
        float bb2 = 1.0f - alpha * A;
        float aa0 = 1.0f + alpha / A;
        float aa1 = -2.0f * cosw0;
        float aa2 = 1.0f - alpha / A;
        m_chB0 = bb0 / aa0; m_chB1 = bb1 / aa0; m_chB2 = bb2 / aa0;
        m_chA1 = aa1 / aa0; m_chA2 = aa2 / aa0;
    }

    // ===== Stage 7: back tail mix =====
    // Disabled in the soundboard build. The Schroeder comb network
    // created audible ringing / pumping when two channels played
    // similar material; the other front/back cues (pinna, notch,
    // concha, head sway) are enough to pin the source perceptually.
    (void)uy;
    m_tailMix = 0.0f;
}

void Positional::process(float mono, float &outL, float &outR) {
    smoothTick();
    // Recompute at control rate (every kCtrlBlock samples). Per-sample
    // recomputation was swapping biquad coefficients on a stateful filter
    // every 21 us - guaranteed clicks and audible ringing on transients.
    // The position smoother already gives us a smooth trajectory between
    // recomputes, so 32-sample granularity (~0.67 ms) is inaudible.
    if (m_ctrlCounter <= 0) {
        recomputeCoeffs();
        m_ctrlCounter = kCtrlBlock;
    }
    --m_ctrlCounter;

    // Stage 1: distance gain + air absorption shelf (mono first; saves work).
    float dry = mono * m_gDist;
    // Per-channel one-pole LP for air absorption.
    m_airLpL = m_airLpL * m_airAlpha + dry * (1.0f - m_airAlpha);
    m_airLpR = m_airLpR * m_airAlpha + dry * (1.0f - m_airAlpha);
    float airL = m_airMix * dry + (1.0f - m_airMix) * m_airLpL;
    float airR = m_airMix * dry + (1.0f - m_airMix) * m_airLpR;

    // Stage 2: ITD fractional delay (push to ring, read back delayed).
    int dN = static_cast<int>(m_delayBufL.size());
    m_delayBufL[m_delayWriteL] = airL;
    m_delayBufR[m_delayWriteR] = airR;
    m_delayWriteL = (m_delayWriteL + 1) % dN;
    m_delayWriteR = (m_delayWriteR + 1) % dN;
    float dlyL = readFrac(m_delayBufL, m_delayWriteL, m_itdSamplesL);
    float dlyR = readFrac(m_delayBufR, m_delayWriteR, m_itdSamplesR);

    // Stage 3: head-shadow biquad (1 pole / 1 zero, normalized).
    float shL = m_shB0L * dlyL + m_shB1L * m_shXL - m_shA1L * m_shYL;
    m_shXL = dlyL;
    m_shYL = shL;
    float shR = m_shB0R * dlyR + m_shB1R * m_shXR - m_shA1R * m_shYR;
    m_shXR = dlyR;
    m_shYR = shR;

    // Stage 4: pinna sparse FIR (5 events) per ear.
    int pN = static_cast<int>(m_pinnaBufL.size());
    m_pinnaBufL[m_pinnaWriteL] = shL;
    m_pinnaBufR[m_pinnaWriteR] = shR;
    m_pinnaWriteL = (m_pinnaWriteL + 1) % pN;
    m_pinnaWriteR = (m_pinnaWriteR + 1) % pN;
    float pinnaL = shL;     // direct path, rho_1 = +1
    float pinnaR = shR;
    for (int k = 0; k < kPinnaEvents; ++k) {
        pinnaL += m_pinnaGainL[k] * readFrac(m_pinnaBufL, m_pinnaWriteL, m_pinnaTapL[k]);
        pinnaR += m_pinnaGainR[k] * readFrac(m_pinnaBufR, m_pinnaWriteR, m_pinnaTapR[k]);
    }
    // Trim slightly - direct path + halved reflections still sums hot.
    // Soundboard build: keep the pinna direct path nearly at unity so
    // the source doesn't feel attenuated. Reflections are already at
    // 0.3 scale (above) so they can't push past full scale here.
    pinnaL *= 0.9f;
    pinnaR *= 0.9f;

    // Stage 5: shoulder + torso single-tap echoes.
    int tN = static_cast<int>(m_tapBufL.size());
    m_tapBufL[m_tapWriteL] = pinnaL;
    m_tapBufR[m_tapWriteR] = pinnaR;
    m_tapWriteL = (m_tapWriteL + 1) % tN;
    m_tapWriteR = (m_tapWriteR + 1) % tN;
    pinnaL += m_shoulderGainL * readFrac(m_tapBufL, m_tapWriteL, m_shoulderTapL);
    pinnaR += m_shoulderGainR * readFrac(m_tapBufR, m_tapWriteR, m_shoulderTapR);
    pinnaL += m_torsoGainL * readFrac(m_tapBufL, m_tapWriteL, m_torsoTapL);
    pinnaR += m_torsoGainR * readFrac(m_tapBufR, m_tapWriteR, m_torsoTapR);

    // Stage 6: front/back peaking notch.
    float fbL = m_fbB0L * pinnaL + m_fbB1L * m_fbXL1 + m_fbB2L * m_fbXL2
              - m_fbA1L * m_fbYL1 - m_fbA2L * m_fbYL2;
    m_fbXL2 = m_fbXL1; m_fbXL1 = pinnaL;
    m_fbYL2 = m_fbYL1; m_fbYL1 = fbL;
    float fbR = m_fbB0R * pinnaR + m_fbB1R * m_fbXR1 + m_fbB2R * m_fbXR2
              - m_fbA1R * m_fbYR1 - m_fbA2R * m_fbYR2;
    m_fbXR2 = m_fbXR1; m_fbXR1 = pinnaR;
    m_fbYR2 = m_fbYR1; m_fbYR1 = fbR;

    // Stage 6b: concha boost (front/back monaural spectral cue).
    {
        float yL = m_chB0 * fbL + m_chB1 * m_chXL1 + m_chB2 * m_chXL2
                 - m_chA1 * m_chYL1 - m_chA2 * m_chYL2;
        m_chXL2 = m_chXL1; m_chXL1 = fbL;
        m_chYL2 = m_chYL1; m_chYL1 = yL;
        fbL = yL;
        float yR = m_chB0 * fbR + m_chB1 * m_chXR1 + m_chB2 * m_chXR2
                 - m_chA1 * m_chYR1 - m_chA2 * m_chYR2;
        m_chXR2 = m_chXR1; m_chXR1 = fbR;
        m_chYR2 = m_chYR1; m_chYR1 = yR;
        fbR = yR;
    }

    // Stage 7: back-tail Schroeder comb mix (only when source is behind).
    float wet = 0.0f;
    if (m_tailMix > 1e-4f) {
        float feed = 0.5f * (fbL + fbR);
        float tail = 0.0f;
        for (int i = 0; i < kCombCount; ++i) {
            int len = m_combLen[i];
            int idx = m_combIdx[i];
            float cur = m_combBuf[i][idx];
            tail += cur;
            // Comb: y[n] = x[n] + fb * y[n - len]
            m_combBuf[i][idx] = feed + cur * m_combFb[i];
            m_combIdx[i] = (idx + 1) % len;
        }
        wet = (tail * 0.25f) * m_tailMix;
    }

    outL = fbL + wet;
    outR = fbR + wet;
}
