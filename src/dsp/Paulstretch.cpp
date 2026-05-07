#include "Paulstretch.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace {
inline int nextPow2(int n) {
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}
}

Paulstretch::Paulstretch()
    : m_rng(0xC0FFEE)
{
    // Pre-allocate every buffer to the maximum we will ever need so
    // setWindowMs() can change the working size without reallocating
    // (which would race with the audio thread and crash the playback).
    m_window.assign(kMaxSize, 0.0f);
    m_specL.assign(kMaxSize, {0.0f, 0.0f});
    m_specR.assign(kMaxSize, {0.0f, 0.0f});
    m_twiddles.assign(kMaxSize / 2, {0.0f, 0.0f});
    m_outL.assign(kMaxSize * 4, 0.0f);
    m_outR.assign(kMaxSize * 4, 0.0f);
    setSampleRate(48000.0);
}

void Paulstretch::setSampleRate(double sr) {
    m_fs = sr > 0 ? sr : 48000.0;
    setWindowMs(180.0f);  // sensible default
}

void Paulstretch::setSource(const float *interleavedStereo, int frameCount) {
    m_src = interleavedStereo;
    m_srcFrames = frameCount;
    reset();
}

void Paulstretch::updateSourceFrames(int frameCount) {
    if (frameCount < 0) frameCount = 0;
    m_srcFrames = frameCount;
}

void Paulstretch::setStretchFactor(float s) {
    if (s < 1.0f) s = 1.0f;
    if (s > 200.0f) s = 200.0f;
    m_stretchFactor.store(s);
}

void Paulstretch::setWindowMs(float ms) {
    if (ms < 30.0f)  ms = 30.0f;
    if (ms > 2000.0f) ms = 2000.0f;
    int target = static_cast<int>(ms * 0.001 * m_fs);
    int pow2 = nextPow2(target);
    if (pow2 < 1024)  pow2 = 1024;
    if (pow2 > kMaxSize) pow2 = kMaxSize;
    if (pow2 == m_size) return;

    // The buffers were sized to kMaxSize in the constructor; we are just
    // changing how many entries we'll use, never the storage layout. The
    // audio thread can safely keep reading - at worst it will read one
    // half-overlap of stale-shape content during the switch.
    m_size = pow2;
    m_hop  = m_size / 2;
    rebuildWindow();
    rebuildTwiddles();

    m_outBufLen = m_size * 4;
    if (m_outBufLen > static_cast<int>(m_outL.size()))
        m_outBufLen = static_cast<int>(m_outL.size());

    // Wipe ring contents so the new window's OLA accumulation starts
    // from zero. Reset write/read indices too.
    std::fill(m_outL.begin(), m_outL.end(), 0.0f);
    std::fill(m_outR.begin(), m_outR.end(), 0.0f);
    m_outWriteIdx = 0;
    m_outReadIdx  = 0;
    m_outFill     = 0;
}

void Paulstretch::rebuildWindow() {
    // In-place: writes only the first m_size entries of a pre-sized
    // buffer. No vector reallocation = audio-thread safe.
    for (int i = 0; i < m_size; ++i) {
        float x = 2.0f * (i / static_cast<float>(m_size - 1)) - 1.0f;
        float w = 1.0f - x * x;
        if (w < 0.0f) w = 0.0f;
        m_window[i] = std::pow(w, 1.25f);
    }
}

void Paulstretch::rebuildTwiddles() {
    // In-place same as rebuildWindow.
    for (int k = 0; k < m_size / 2; ++k) {
        float a = -2.0f * 3.14159265358979323846f * k / m_size;
        m_twiddles[k] = {std::cos(a), std::sin(a)};
    }
}

void Paulstretch::reset() {
    m_inputPos = 0.0;
    std::fill(m_outL.begin(), m_outL.end(), 0.0f);
    std::fill(m_outR.begin(), m_outR.end(), 0.0f);
    m_outWriteIdx = 0;
    m_outReadIdx  = 0;
    m_outFill     = 0;
}

void Paulstretch::seekToFrame(int frame) {
    if (m_srcFrames <= 0) { m_inputPos = 0.0; return; }
    if (frame < 0) frame = 0;
    m_inputPos = frame % m_srcFrames;
    // Ring carries pre-stretched audio from the OLD position - drop it
    // so the user hears the new position right away.
    std::fill(m_outL.begin(), m_outL.end(), 0.0f);
    std::fill(m_outR.begin(), m_outR.end(), 0.0f);
    m_outWriteIdx = 0;
    m_outReadIdx  = 0;
    m_outFill     = 0;
}

int Paulstretch::currentFrame() const {
    if (m_srcFrames <= 0) return 0;
    int f = static_cast<int>(m_inputPos);
    if (f < 0) f = 0;
    return f % m_srcFrames;
}

void Paulstretch::fftBitReverse(std::vector<std::complex<float>> &x) const {
    int N = m_size;
    int j = 0;
    for (int i = 1; i < N; ++i) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
}

void Paulstretch::fftForward(std::vector<std::complex<float>> &x) const {
    int N = m_size;
    fftBitReverse(x);
    for (int len = 2; len <= N; len <<= 1) {
        int half = len >> 1;
        int wstride = N / len;
        for (int k = 0; k < N; k += len) {
            for (int j = 0; j < half; ++j) {
                std::complex<float> t = m_twiddles[j * wstride] * x[k + j + half];
                std::complex<float> u = x[k + j];
                x[k + j]        = u + t;
                x[k + j + half] = u - t;
            }
        }
    }
}

void Paulstretch::fftInverse(std::vector<std::complex<float>> &x) const {
    int N = m_size;
    for (auto &c : x) c = std::conj(c);
    fftForward(x);
    float invN = 1.0f / static_cast<float>(N);
    for (auto &c : x) c = std::conj(c) * invN;
}

void Paulstretch::synthOneWindow() {
    // While the streaming feed is still under one full window's worth
    // of source we can't run a meaningful FFT. Push silence into the
    // output ring instead of returning early - the latter would pin
    // m_outFill at zero, hang fillStereo's while-loop, and freeze the
    // audio thread. Once srcFrames catches up the synth path resumes.
    if (!m_src || m_srcFrames < m_size) {
        for (int i = 0; i < m_hop; ++i) {
            int idx = (m_outWriteIdx + i) % m_outBufLen;
            m_outL[idx] = 0.0f;
            m_outR[idx] = 0.0f;
        }
        m_outWriteIdx = (m_outWriteIdx + m_hop) % m_outBufLen;
        m_outFill += m_hop;
        return;
    }

    // ---- Stage 1: read one window from the source at m_inputPos ----
    // Loop the source so paulstretch can run forever even when the
    // upstream Source isn't in loop mode. The Source itself decides
    // when to stop the playback; if it does, fillStereo will simply
    // return early.
    int srcLen = m_srcFrames;
    int startSample = static_cast<int>(m_inputPos);
    if (startSample < 0) startSample = 0;
    startSample = startSample % srcLen;

    for (int i = 0; i < m_size; ++i) {
        int s = (startSample + i) % srcLen;
        float w = m_window[i];
        m_specL[i] = std::complex<float>(m_src[s * 2 + 0] * w, 0.0f);
        m_specR[i] = std::complex<float>(m_src[s * 2 + 1] * w, 0.0f);
    }

    // ---- Stage 2: FFT both channels ----
    fftForward(m_specL);
    fftForward(m_specR);

    // ---- Stage 3: phase randomization ----
    // Independent per channel -> stereo decorrelation that gives
    // paulstretch its signature diffuse stereo wash.
    std::uniform_real_distribution<float> dist(0.0f,
                                               6.28318530717958647692f);
    for (int k = 0; k < m_size; ++k) {
        float magL = std::abs(m_specL[k]);
        float magR = std::abs(m_specR[k]);
        float phL  = dist(m_rng);
        float phR  = dist(m_rng);
        m_specL[k] = std::complex<float>(magL * std::cos(phL), magL * std::sin(phL));
        m_specR[k] = std::complex<float>(magR * std::cos(phR), magR * std::sin(phR));
    }

    // ---- Stage 4: IFFT ----
    fftInverse(m_specL);
    fftInverse(m_specR);

    // ---- Stage 5: re-window and overlap-add into the output ring ----
    // Trim factor compensates for the COLA gain of the windowed-twice
    // overlap. ~0.7 keeps loudness roughly flat across stretch values.
    constexpr float kOlaTrim = 0.7f;
    for (int i = 0; i < m_size; ++i) {
        int idx = (m_outWriteIdx + i) % m_outBufLen;
        float w = m_window[i] * kOlaTrim;
        m_outL[idx] += m_specL[i].real() * w;
        m_outR[idx] += m_specR[i].real() * w;
    }
    m_outWriteIdx = (m_outWriteIdx + m_hop) % m_outBufLen;
    m_outFill += m_hop;

    // ---- Stage 6: advance input by (hop / stretch) ----
    // Re-read the atomic stretch factor each window so live slider
    // changes take effect within one window (~hop / sampleRate seconds).
    float factor = m_stretchFactor.load();
    double advance = static_cast<double>(m_hop) /
                     static_cast<double>(factor);
    m_inputPos += advance;
    if (m_inputPos > srcLen * 2.0)
        m_inputPos = std::fmod(m_inputPos, static_cast<double>(srcLen));
}

void Paulstretch::fillStereo(float *outL, float *outR, int frames) {
    // Generate windows until the ring has enough samples. The condition
    // `m_outFill < frames + m_hop` keeps a one-hop safety margin.
    while (m_outFill < frames + m_hop) {
        synthOneWindow();
    }

    for (int i = 0; i < frames; ++i) {
        outL[i] = m_outL[m_outReadIdx];
        outR[i] = m_outR[m_outReadIdx];
        m_outL[m_outReadIdx] = 0.0f;        // clear so future OLA accumulates clean
        m_outR[m_outReadIdx] = 0.0f;
        m_outReadIdx = (m_outReadIdx + 1) % m_outBufLen;
        --m_outFill;
    }
}
