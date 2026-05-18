#pragma once
// SimpleFFT.h -- Radix-2 Cooley-Tukey FFT for power-of-2 sizes.
// Single-header implementation.  Interleaved complex format throughout:
//   [re0, im0, re1, im1, ...].
//
// Public interface:
//   forward()  -- N real floats --> (N/2+1) complex bins  (N+2 floats)
//   inverse()  -- (N/2+1) complex bins --> N real floats   (caller scales by 1/N)

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <vector>
#include <cstring>   // memcpy, memset
#include <cassert>

class SimpleFFT {
public:
    // -----------------------------------------------------------------
    // `size` MUST be a power of 2.
    // -----------------------------------------------------------------
    explicit SimpleFFT(int size)
        : m_size(size)
    {
        assert(size > 0 && (size & (size - 1)) == 0);  // power-of-2 check

        // Pre-compute twiddle factors for sizes 2..N.
        // Store as interleaved (cos, -sin) pairs for the forward direction.
        // For a butterfly span of halfN the twiddle for index k is
        //   W_N^k = cos(2*pi*k/N) - j*sin(2*pi*k/N)
        m_twiddle.resize(static_cast<size_t>(size));  // cos,sin pairs = size/2 entries
        for (int k = 0; k < size / 2; ++k) {
            double angle = -2.0 * M_PI * k / size;
            m_twiddle[2 * k]     = static_cast<float>(cos(angle));
            m_twiddle[2 * k + 1] = static_cast<float>(sin(angle));
        }

        // Scratch buffer large enough for the complex FFT (2*N floats).
        m_work.resize(static_cast<size_t>(2 * size), 0.0f);
    }

    // -----------------------------------------------------------------
    // Forward: N real floats --> (N/2+1) complex bins stored as N+2 floats.
    // -----------------------------------------------------------------
    void forward(const float* timeIn, float* freqOut)
    {
        const int N = m_size;

        // Pack real data as complex (zero imaginary).
        for (int i = 0; i < N; ++i) {
            m_work[2 * i]     = timeIn[i];
            m_work[2 * i + 1] = 0.0f;
        }

        complexFFT(m_work.data(), N, /*inverse=*/false);

        // Copy first N/2+1 complex bins to output.
        const int bins = N / 2 + 1;
        std::memcpy(freqOut, m_work.data(), bins * 2 * sizeof(float));
    }

    // -----------------------------------------------------------------
    // Inverse: (N/2+1) complex bins --> N real floats.
    // The caller is responsible for scaling the output by 1/N.
    // -----------------------------------------------------------------
    void inverse(const float* freqIn, float* timeOut)
    {
        const int N    = m_size;
        const int bins = N / 2 + 1;  // = N/2 + 1

        // Reconstruct full N-point complex spectrum from the Hermitian-
        // symmetric half.
        //   bin[0]       = DC
        //   bin[N/2]     = Nyquist
        //   bin[k]       = conj(bin[N-k])  for k = N/2+1 .. N-1
        for (int k = 0; k < bins; ++k) {
            m_work[2 * k]     = freqIn[2 * k];
            m_work[2 * k + 1] = freqIn[2 * k + 1];
        }
        for (int k = bins; k < N; ++k) {
            int mirror = N - k;
            m_work[2 * k]     =  freqIn[2 * mirror];
            m_work[2 * k + 1] = -freqIn[2 * mirror + 1];  // conjugate
        }

        complexFFT(m_work.data(), N, /*inverse=*/true);

        // Extract real part.
        for (int i = 0; i < N; ++i)
            timeOut[i] = m_work[2 * i];
    }

    int size() const { return m_size; }

private:
    int                m_size;
    std::vector<float> m_twiddle;   // pre-computed twiddle factors
    std::vector<float> m_work;      // scratch buffer (2*N floats)

    // -----------------------------------------------------------------
    // Bit-reversal permutation of complex data stored interleaved.
    // -----------------------------------------------------------------
    static void bitReversalPermute(float* data, int n)
    {
        int j = 0;
        for (int i = 0; i < n - 1; ++i) {
            if (i < j) {
                // Swap complex pair i <-> j.
                float tr = data[2 * i];
                float ti = data[2 * i + 1];
                data[2 * i]     = data[2 * j];
                data[2 * i + 1] = data[2 * j + 1];
                data[2 * j]     = tr;
                data[2 * j + 1] = ti;
            }
            int m = n >> 1;
            while (m >= 1 && j >= m) {
                j -= m;
                m >>= 1;
            }
            j += m;
        }
    }

    // -----------------------------------------------------------------
    // In-place radix-2 Cooley-Tukey complex FFT.
    // `data` = interleaved complex, length = 2*n floats.
    // -----------------------------------------------------------------
    void complexFFT(float* data, int n, bool inverse)
    {
        bitReversalPermute(data, n);

        // Butterfly passes: span = 1, 2, 4, ..., n/2.
        for (int span = 1; span < n; span <<= 1) {
            int step = span << 1;                 // distance between butterfly groups
            int twiddleStride = m_size / step;    // step through the pre-computed table

            for (int group = 0; group < n; group += step) {
                for (int k = 0; k < span; ++k) {
                    int twIdx = k * twiddleStride;
                    float wr = m_twiddle[2 * twIdx];
                    float wi = m_twiddle[2 * twIdx + 1];
                    if (inverse) wi = -wi;        // conjugate for inverse

                    int even = group + k;
                    int odd  = even + span;

                    float er = data[2 * even];
                    float ei = data[2 * even + 1];
                    float or_ = data[2 * odd];
                    float oi  = data[2 * odd + 1];

                    // Twiddle multiply: t = W * odd
                    float tr = wr * or_ - wi * oi;
                    float ti = wr * oi  + wi * or_;

                    // Butterfly
                    data[2 * even]     = er + tr;
                    data[2 * even + 1] = ei + ti;
                    data[2 * odd]      = er - tr;
                    data[2 * odd + 1]  = ei - ti;
                }
            }
        }
    }
};
