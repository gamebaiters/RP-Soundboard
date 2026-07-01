// HrtfProcessor.cpp -- HRTF convolution via overlap-add.
// Uses mysofa_open_no_norm to preserve natural HRIR measurement levels
// (no loudness normalization, no minphase conversion).
// ITD is naturally encoded in the HRIR phase.

#include "HrtfProcessor.h"

#include <algorithm>
#include <cstring>
#include <QtGlobal>

HrtfProcessor::HrtfProcessor() = default;

HrtfProcessor::~HrtfProcessor()
{
    if (m_sofa) {
        mysofa_close(m_sofa);
        m_sofa = nullptr;
    }
}

bool HrtfProcessor::init(const std::string& sofaPath,
                          float sampleRate,
                          int   blockSize)
{
    if (m_sofa) {
        mysofa_close(m_sofa);
        m_sofa = nullptr;
    }

    m_sampleRate = sampleRate;
    m_blockSize  = blockSize;

    int filterLen = 0;
    int err       = 0;

    // Use no_norm variant: skips mysofa_loudness (peak normalization)
    // and mysofa_minphase. HRIRs stay at natural measurement levels
    // and retain original phase (which encodes ITD naturally).
    m_sofa = mysofa_open_no_norm(sofaPath.c_str(), sampleRate, &filterLen, &err);
    if (!m_sofa || err != MYSOFA_OK) {
        qWarning("[Leia] mysofa_open_no_norm failed (err %d)", err);
        m_sofa = nullptr;
        return false;
    }
    m_filterLength = filterLen;

    m_fftSize     = nextPow2(blockSize + filterLen);
    m_complexBins = m_fftSize / 2 + 1;

    m_fft = std::unique_ptr<SimpleFFT>(new SimpleFFT(m_fftSize));

    const size_t N   = static_cast<size_t>(m_fftSize);
    const size_t N2  = static_cast<size_t>(m_fftSize + 2);
    const size_t FL  = static_cast<size_t>(m_filterLength);

    m_fftInput     .assign(N,  0.0f);
    m_fftFreq      .assign(N2, 0.0f);

    m_irLeft       .assign(FL, 0.0f);
    m_irRight      .assign(FL, 0.0f);
    m_irLeftFreq   .assign(N2, 0.0f);
    m_irRightFreq  .assign(N2, 0.0f);
    m_irLeftFreqPending .assign(N2, 0.0f);
    m_irRightFreqPending.assign(N2, 0.0f);
    m_irLeftFreqTarget .assign(N2, 0.0f);
    m_irRightFreqTarget.assign(N2, 0.0f);
    m_irTargetValid = false;

    m_convLeft     .assign(N2, 0.0f);
    m_convRight    .assign(N2, 0.0f);
    m_convLeftPending .assign(N2, 0.0f);
    m_convRightPending.assign(N2, 0.0f);

    m_outLeft      .assign(N, 0.0f);
    m_outRight     .assign(N, 0.0f);
    m_outLeftPending .assign(N, 0.0f);
    m_outRightPending.assign(N, 0.0f);

    m_overlapLeft  .assign(N, 0.0f);
    m_overlapRight .assign(N, 0.0f);
    m_overlapLeftPending .assign(N, 0.0f);
    m_overlapRightPending.assign(N, 0.0f);

    // Output crossfade: 30 ms at the configured sample rate. Longer
    // fade == smoother crossfade == less audible discontinuity when
    // rotation triggers repeated HRIR updates on high-magnitude
    // signal. The user reported that with rotation + many DSP
    // stages boosting the input, residual frying persisted even after
    // the earlier 5 -> 15 ms bump; going to 30 ms halves the per-sample
    // slope of the crossfade envelope and lets the pending chain settle
    // more of its tail into the output while the fade is still in
    // progress. 30 ms is still short enough that positional lag under
    // rotation stays subjectively "on the source".
    const float kFadeSeconds = 0.030f;
    int fadeSamples = static_cast<int>(sampleRate * kFadeSeconds);
    if (fadeSamples < 32) fadeSamples = 32;
    m_fadeAlphaInc = 1.0f / static_cast<float>(fadeSamples);
    m_fadeAlpha    = 1.0f;
    m_fadePending  = false;

    m_lastAzimuth   = -9999.0f;
    m_lastElevation = -9999.0f;

    return true;
}

void HrtfProcessor::process(const float* monoIn,
                             float* leftOut,
                             float* rightOut,
                             int    frames,
                             float  azimuthDeg,
                             float  elevationDeg)
{
    if (!m_sofa || frames <= 0) {
        if (leftOut)  std::memset(leftOut,  0, frames * sizeof(float));
        if (rightOut) std::memset(rightOut, 0, frames * sizeof(float));
        return;
    }
    if (frames > m_blockSize) frames = m_blockSize;

    // 1. Update HRIR target if direction changed enough to matter and
    //    no fade is already in progress. The deadband suppresses tiny
    //    moves; the fade gate serialises updates so a rapid rotation
    //    cannot stack up overlapping fades and lose IR continuity.
    constexpr float kHrirAzDeadbandDeg = 3.0f;
    constexpr float kHrirElDeadbandDeg = 3.0f;
    bool firstLookup = (m_lastAzimuth   <= -9000.0f) ||
                        (m_lastElevation <= -9000.0f);
    bool azMoved = std::fabs(azimuthDeg   - m_lastAzimuth)   >= kHrirAzDeadbandDeg;
    bool elMoved = std::fabs(elevationDeg - m_lastElevation) >= kHrirElDeadbandDeg;
    bool canStartFade = !m_fadePending;

    if ((firstLookup || azMoved || elMoved) && canStartFade) {
        lookupHRIR(azimuthDeg, elevationDeg);
        m_lastAzimuth   = azimuthDeg;
        m_lastElevation = elevationDeg;
        if (firstLookup) {
            // First-time snap: load target into BOTH live and pending
            // and skip the fade.
            const int n2 = m_fftSize + 2;
            std::memcpy(m_irLeftFreq.data(),  m_irLeftFreqTarget.data(),  n2 * sizeof(float));
            std::memcpy(m_irRightFreq.data(), m_irRightFreqTarget.data(), n2 * sizeof(float));
            std::memcpy(m_irLeftFreqPending.data(),  m_irLeftFreqTarget.data(),  n2 * sizeof(float));
            std::memcpy(m_irRightFreqPending.data(), m_irRightFreqTarget.data(), n2 * sizeof(float));
            m_fadePending = false;
            m_fadeAlpha   = 1.0f;
        } else {
            // Start crossfade: load new IR into pending chain (snap,
            // not lerp - the pending chain runs its own OLA with this
            // IR fixed for the fade duration, so the overlap will be
            // self-consistent).
            //
            // Seed pending OLA overlap with a COPY of the active
            // overlap instead of zeroing. Rationale:
            //
            // Zero-init made the pending chain start with NO tail. The
            // active chain, meanwhile, had a fully-built tail from the
            // previous block. During the fade, blending (active + tail)
            // with (pending + zero) added a per-sample residual = the
            // full active tail scaled by (1 - alpha). That residual
            // decayed sample-by-sample - exactly the impulse-like
            // artefact that read as "frying" on rotation-heavy content
            // when the input was already boosted by upstream DSP.
            //
            // The old and new IR are similar for small azimuth deltas
            // (deadband is 3 deg, HRIRs are smooth in the SOFA grid),
            // so seeding with the previous tail is a good approximation
            // - the crossfade replaces the copied tail with the new
            // IR's own tail over the fade window instead of ADDING one
            // tail to zero. Discontinuity at fade start is near zero.
            const int n2 = m_fftSize + 2;
            const int n  = m_fftSize;
            std::memcpy(m_irLeftFreqPending.data(),  m_irLeftFreqTarget.data(),  n2 * sizeof(float));
            std::memcpy(m_irRightFreqPending.data(), m_irRightFreqTarget.data(), n2 * sizeof(float));
            std::memcpy(m_overlapLeftPending.data(),  m_overlapLeft.data(),  n * sizeof(float));
            std::memcpy(m_overlapRightPending.data(), m_overlapRight.data(), n * sizeof(float));
            m_fadePending = true;
            m_fadeAlpha   = 0.0f;
        }
    }

    // 2. Copy current block into FFT input, zero-pad to fftSize.
    // No analysis window — standard OLA doesn't window the input.
    std::memcpy(m_fftInput.data(), monoIn, frames * sizeof(float));
    std::memset(m_fftInput.data() + frames, 0,
                (m_fftSize - frames) * sizeof(float));

    // 3. Forward FFT the input. Shared across both chains.
    m_fft->forward(m_fftInput.data(), m_fftFreq.data());

    const float invN = 1.0f / static_cast<float>(m_fftSize);

    // 4. Active chain: complex multiply + inverse FFT + scale.
    complexMultiply(m_convLeft.data(),  m_fftFreq.data(), m_irLeftFreq.data(),  m_complexBins);
    complexMultiply(m_convRight.data(), m_fftFreq.data(), m_irRightFreq.data(), m_complexBins);
    m_fft->inverse(m_convLeft.data(),  m_outLeft.data());
    m_fft->inverse(m_convRight.data(), m_outRight.data());
    for (int i = 0; i < m_fftSize; ++i) {
        m_outLeft[i]  *= invN;
        m_outRight[i] *= invN;
    }

    // 5. Pending chain: only when a fade is in progress.
    if (m_fadePending) {
        complexMultiply(m_convLeftPending.data(),  m_fftFreq.data(),
                        m_irLeftFreqPending.data(),  m_complexBins);
        complexMultiply(m_convRightPending.data(), m_fftFreq.data(),
                        m_irRightFreqPending.data(), m_complexBins);
        m_fft->inverse(m_convLeftPending.data(),  m_outLeftPending.data());
        m_fft->inverse(m_convRightPending.data(), m_outRightPending.data());
        for (int i = 0; i < m_fftSize; ++i) {
            m_outLeftPending[i]  *= invN;
            m_outRightPending[i] *= invN;
        }
    }

    // 6. OLA + per-sample output crossfade.
    if (m_fadePending) {
        float a = m_fadeAlpha;
        for (int i = 0; i < frames; ++i) {
            float aL = m_outLeft[i]         + m_overlapLeft[i];
            float aR = m_outRight[i]        + m_overlapRight[i];
            float pL = m_outLeftPending[i]  + m_overlapLeftPending[i];
            float pR = m_outRightPending[i] + m_overlapRightPending[i];
            float w  = a;
            if (w < 0.0f) w = 0.0f; else if (w > 1.0f) w = 1.0f;
            leftOut[i]  = (1.0f - w) * aL + w * pL;
            rightOut[i] = (1.0f - w) * aR + w * pR;
            a += m_fadeAlphaInc;
        }
        m_fadeAlpha = a;
    } else {
        for (int i = 0; i < frames; ++i) {
            leftOut[i]  = m_outLeft[i]  + m_overlapLeft[i];
            rightOut[i] = m_outRight[i] + m_overlapRight[i];
        }
    }

    // 7. Update overlap buffers for both chains. Each chain MUST keep
    // its own OLA self-consistent so the tail it adds next block was
    // generated against the same IR currently in its slot.
    int tailLen = m_fftSize - frames;
    if (tailLen > 0) {
        // Active.
        std::memmove(m_overlapLeft.data(),
                     m_overlapLeft.data() + frames,
                     tailLen * sizeof(float));
        std::memmove(m_overlapRight.data(),
                     m_overlapRight.data() + frames,
                     tailLen * sizeof(float));
        std::memset(m_overlapLeft.data()  + tailLen, 0, frames * sizeof(float));
        std::memset(m_overlapRight.data() + tailLen, 0, frames * sizeof(float));
        for (int i = 0; i < tailLen; ++i) {
            m_overlapLeft[i]  += m_outLeft[frames + i];
            m_overlapRight[i] += m_outRight[frames + i];
        }
        if (m_fadePending) {
            std::memmove(m_overlapLeftPending.data(),
                         m_overlapLeftPending.data() + frames,
                         tailLen * sizeof(float));
            std::memmove(m_overlapRightPending.data(),
                         m_overlapRightPending.data() + frames,
                         tailLen * sizeof(float));
            std::memset(m_overlapLeftPending.data()  + tailLen, 0, frames * sizeof(float));
            std::memset(m_overlapRightPending.data() + tailLen, 0, frames * sizeof(float));
            for (int i = 0; i < tailLen; ++i) {
                m_overlapLeftPending[i]  += m_outLeftPending[frames + i];
                m_overlapRightPending[i] += m_outRightPending[frames + i];
            }
        }
    }

    // 8. Fade complete - promote pending to active.
    if (m_fadePending && m_fadeAlpha >= 1.0f) {
        const int n2 = m_fftSize + 2;
        const int n  = m_fftSize;
        std::memcpy(m_irLeftFreq.data(),  m_irLeftFreqPending.data(),  n2 * sizeof(float));
        std::memcpy(m_irRightFreq.data(), m_irRightFreqPending.data(), n2 * sizeof(float));
        std::memcpy(m_overlapLeft.data(),  m_overlapLeftPending.data(),  n * sizeof(float));
        std::memcpy(m_overlapRight.data(), m_overlapRightPending.data(), n * sizeof(float));
        m_fadePending = false;
        m_fadeAlpha   = 1.0f;
    }
}

void HrtfProcessor::lookupHRIR(float azDeg, float elDeg)
{
    if (!m_sofa) return;

    // Negate azimuth: our convention is CW-positive (+90=right),
    // SOFA is CCW-positive (+y=left). Negating maps correctly.
    float azRad = -azDeg * static_cast<float>(M_PI / 180.0);
    float elRad =  elDeg * static_cast<float>(M_PI / 180.0);

    float cosEl = cosf(elRad);
    float x = cosEl * cosf(azRad);
    float y = cosEl * sinf(azRad);
    float z = sinf(elRad);

    float delayL = 0.0f, delayR = 0.0f;
    mysofa_getfilter_float(m_sofa, x, y, z,
                           m_irLeft.data(), m_irRight.data(),
                           &delayL, &delayR);

    // FFT into the TARGET buffers; process() lerps the live IR toward
    // these each block so direction changes are smoothed across multiple
    // blocks instead of taking effect instantly (which produced an
    // OLA-overlap discontinuity = audible click train under rotation).
    std::memset(m_fftInput.data(), 0, m_fftSize * sizeof(float));
    std::memcpy(m_fftInput.data(), m_irLeft.data(),
                m_filterLength * sizeof(float));
    m_fft->forward(m_fftInput.data(), m_irLeftFreqTarget.data());

    std::memset(m_fftInput.data(), 0, m_fftSize * sizeof(float));
    std::memcpy(m_fftInput.data(), m_irRight.data(),
                m_filterLength * sizeof(float));
    m_fft->forward(m_fftInput.data(), m_irRightFreqTarget.data());

    m_irTargetValid = true;
}

void HrtfProcessor::complexMultiply(float*       dst,
                                     const float* a,
                                     const float* b,
                                     int          complexBins)
{
    for (int i = 0; i < complexBins; ++i) {
        float ar = a[2 * i],     ai = a[2 * i + 1];
        float br = b[2 * i],     bi = b[2 * i + 1];
        dst[2 * i]     = ar * br - ai * bi;
        dst[2 * i + 1] = ar * bi + ai * br;
    }
}

int HrtfProcessor::nextPow2(int n)
{
    if (n <= 1) return 1;
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

void HrtfProcessor::reset()
{
    auto zero = [](std::vector<float> &v) {
        if (!v.empty()) std::memset(v.data(), 0, v.size() * sizeof(float));
    };

    zero(m_overlapLeft);  zero(m_overlapRight);
    zero(m_overlapLeftPending); zero(m_overlapRightPending);

    // Zero the live + pending + target frequency-domain IRs. Without
    // this a resumed slot briefly convolves with the previous session's
    // IR (firstLookup snaps target -> live but the convolution that
    // produces the overlap-add tail can fire on the first sample of
    // the new session BEFORE the snap, leaving an audible transient).
    zero(m_irLeftFreq);  zero(m_irRightFreq);
    zero(m_irLeftFreqPending);  zero(m_irRightFreqPending);
    zero(m_irLeftFreqTarget);   zero(m_irRightFreqTarget);

    m_fadePending = false;
    m_fadeAlpha   = 1.0f;

    m_lastAzimuth   = -9999.0f;
    m_lastElevation = -9999.0f;
    // First lookup after reset must snap rather than lerp from the
    // stale target left over from the previous session.
    m_irTargetValid = false;
}
