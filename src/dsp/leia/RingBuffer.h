#pragma once
// RingBuffer.h -- Simple circular float buffer for delay lines and OLA.
// Single-header implementation, no .cpp needed.

#include <vector>
#include <cstring>  // memcpy, memset
#include <cmath>    // floorf

struct RingBuffer {
    std::vector<float> data;
    int capacity  = 0;
    int writePos  = 0;

    // -----------------------------------------------------------------------
    // Allocate storage and zero everything.
    // -----------------------------------------------------------------------
    void init(int cap)
    {
        capacity = cap;
        data.assign(static_cast<size_t>(cap), 0.0f);
        writePos = 0;
    }

    // -----------------------------------------------------------------------
    // Write `count` samples into the ring, advancing writePos.
    // Classic two-part wrap-around copy.
    // -----------------------------------------------------------------------
    void write(const float* src, int count)
    {
        if (capacity == 0 || count <= 0) return;

        // Clamp: if someone writes more than a full ring, keep only the tail.
        if (count > capacity) {
            src   += (count - capacity);
            count  = capacity;
        }

        int first = capacity - writePos;          // space until physical end
        if (first >= count) {
            std::memcpy(&data[writePos], src, count * sizeof(float));
        } else {
            std::memcpy(&data[writePos], src,         first          * sizeof(float));
            std::memcpy(&data[0],        src + first, (count - first) * sizeof(float));
        }
        writePos = (writePos + count) % capacity;
    }

    // -----------------------------------------------------------------------
    // Read `count` samples at a fixed integer delay behind writePos.
    // delay = number of samples between the *end* of the read region and
    //         writePos.  So the first sample read was written
    //         (delay + count - 1) samples ago.
    // -----------------------------------------------------------------------
    void readFixed(int delay, float* dst, int count) const
    {
        if (capacity == 0 || count <= 0) return;

        int startIdx = writePos - delay - count;
        // Positive modulo -- works even for very negative values.
        startIdx = ((startIdx % capacity) + capacity) % capacity;

        int first = capacity - startIdx;
        if (first >= count) {
            std::memcpy(dst, &data[startIdx], count * sizeof(float));
        } else {
            std::memcpy(dst,         &data[startIdx], first          * sizeof(float));
            std::memcpy(dst + first, &data[0],        (count - first) * sizeof(float));
        }
    }

    // -----------------------------------------------------------------------
    // Per-sample fractional delay read with linear interpolation.
    // delays[i] is the *fractional* delay for output sample i.
    // The reference point moves forward with each sample:
    //   readPos_i = (writePos - count + i) - delays[i]
    // -----------------------------------------------------------------------
    void readFractional(const float* delays, float* dst, int count) const
    {
        if (capacity == 0 || count <= 0) return;

        for (int i = 0; i < count; ++i) {
            float readPos = static_cast<float>(writePos - count + i) - delays[i];

            // Floor index (always positive modulo).
            int   idx0 = static_cast<int>(floorf(readPos));
            float frac = readPos - static_cast<float>(idx0);

            idx0 = ((idx0 % capacity) + capacity) % capacity;
            int idx1 = (idx0 + 1) % capacity;

            dst[i] = data[idx0] + frac * (data[idx1] - data[idx0]);
        }
    }

    // -----------------------------------------------------------------------
    // Zero the buffer and reset the write head.
    // -----------------------------------------------------------------------
    void clear()
    {
        if (!data.empty()) {
            std::memset(data.data(), 0, data.size() * sizeof(float));
        }
        writePos = 0;
    }
};
