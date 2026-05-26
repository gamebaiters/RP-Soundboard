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
    m_irLeftFreqTarget .assign(N2, 0.0f);
    m_irRightFreqTarget.assign(N2, 0.0f);
    m_irTargetValid = false;

    m_convLeft     .assign(N2, 0.0f);
    m_convRight    .assign(N2, 0.0f);

    m_outLeft      .assign(N, 0.0f);
    m_outRight     .assign(N, 0.0f);

    m_overlapLeft  .assign(N, 0.0f);
    m_overlapRight .assign(N, 0.0f);

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

    // 1. Update HRIR target if direction changed enough to matter.
    //    Wider deadband (3 deg) than the SOFA grid resolution (~5 deg
    //    on MIT KEMAR), well above the 0.32 deg/block 8D rotation
    //    rate at the default 10 RPM. Cuts mysofa_getfilter_float +
    //    IR-FFT cost from every block down to every ~10 blocks on a
    //    moderate rotation - audible CPU savings on the audio thread
    //    that was missing deadlines and producing the residual frying
    //    buzz on complex (broadband, high-peak) material.
    constexpr float kHrirAzDeadbandDeg = 3.0f;
    constexpr float kHrirElDeadbandDeg = 3.0f;
    bool firstLookup = (m_lastAzimuth   <= -9000.0f) ||
                        (m_lastElevation <= -9000.0f);
    bool azMoved = std::fabs(azimuthDeg   - m_lastAzimuth)   >= kHrirAzDeadbandDeg;
    bool elMoved = std::fabs(elevationDeg - m_lastElevation) >= kHrirElDeadbandDeg;
    if (firstLookup || azMoved || elMoved) {
        lookupHRIR(azimuthDeg, elevationDeg);
        m_lastAzimuth   = azimuthDeg;
        m_lastElevation = elevationDeg;
        if (firstLookup) {
            // Snap on the very first lookup so the first block does
            // not produce a half-second silent ramp-up.
            std::memcpy(m_irLeftFreq.data(),  m_irLeftFreqTarget.data(),
                        (m_fftSize + 2) * sizeof(float));
            std::memcpy(m_irRightFreq.data(), m_irRightFreqTarget.data(),
                        (m_fftSize + 2) * sizeof(float));
        }
    }

    // 1b. Smooth the live IR toward the latest target. Single-pole
    //     filter per freq bin: each block the live IR moves a fixed
    //     fraction of the way toward the lookup result. The convolution
    //     uses the smoothed IR, so block-to-block changes are tiny and
    //     the OLA overlap (generated with the live IR's previous state)
    //     adds nearly-coherently with the new block's convolution.
    //
    //     kIrLerp == 0.06 -> ~50 blocks to 95% (~270 ms at 48 kHz /
    //     kBlock 256). Much slower than the previous 0.18 - turns out
    //     the residual frying buzz on complex audio came from the
    //     remaining block-to-block IR delta still being audible. At
    //     0.06 the per-block delta is 3x smaller; combined with the
    //     wider 3 deg lookup deadband the IR is effectively held flat
    //     for slow rotations and creeps smoothly during fast ones -
    //     listener still perceives motion, no click train.
    if (m_irTargetValid) {
        constexpr float kIrLerp = 0.06f;
        const int n = m_fftSize + 2;
        const float *tgtL = m_irLeftFreqTarget.data();
        const float *tgtR = m_irRightFreqTarget.data();
        float *liveL = m_irLeftFreq.data();
        float *liveR = m_irRightFreq.data();
        for (int i = 0; i < n; ++i) {
            liveL[i] += (tgtL[i] - liveL[i]) * kIrLerp;
            liveR[i] += (tgtR[i] - liveR[i]) * kIrLerp;
        }
    }

    // 2. Copy current block into FFT input, zero-pad to fftSize.
    // No analysis window — standard OLA doesn't window the input.
    std::memcpy(m_fftInput.data(), monoIn, frames * sizeof(float));
    std::memset(m_fftInput.data() + frames, 0,
                (m_fftSize - frames) * sizeof(float));

    // 3. Forward FFT the input.
    m_fft->forward(m_fftInput.data(), m_fftFreq.data());

    // 4. Complex multiply: input * HRIR for each ear.
    complexMultiply(m_convLeft.data(),  m_fftFreq.data(), m_irLeftFreq.data(),  m_complexBins);
    complexMultiply(m_convRight.data(), m_fftFreq.data(), m_irRightFreq.data(), m_complexBins);

    // 5. Inverse FFT both results.
    m_fft->inverse(m_convLeft.data(),  m_outLeft.data());
    m_fft->inverse(m_convRight.data(), m_outRight.data());

    // 6. Scale by 1/fftSize (unnormalized IFFT).
    {
        float invN = 1.0f / static_cast<float>(m_fftSize);
        for (int i = 0; i < m_fftSize; ++i) {
            m_outLeft[i]  *= invN;
            m_outRight[i] *= invN;
        }
    }

    // 7. Overlap-add: output first `frames` samples summed with saved overlap.
    for (int i = 0; i < frames; ++i) {
        leftOut[i]  = m_outLeft[i]  + m_overlapLeft[i];
        rightOut[i] = m_outRight[i] + m_overlapRight[i];
    }

    // 8. Update overlap buffer: shift left by `frames`, add current tail.
    int tailLen = m_fftSize - frames;
    if (tailLen > 0) {
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
    if (!m_overlapLeft.empty())
        std::memset(m_overlapLeft.data(), 0, m_overlapLeft.size() * sizeof(float));
    if (!m_overlapRight.empty())
        std::memset(m_overlapRight.data(), 0, m_overlapRight.size() * sizeof(float));

    m_lastAzimuth   = -9999.0f;
    m_lastElevation = -9999.0f;
    // First lookup after reset must snap rather than lerp from the
    // stale target left over from the previous session.
    m_irTargetValid = false;
}
