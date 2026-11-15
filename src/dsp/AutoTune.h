#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Real-time monophonic Auto-Tune (YIN pitch detector + phase-locked TD-PSOLA).
// Header-only C++14, zero Qt dependencies, zero allocations/locks/I/O on the
// audio thread. Constant input-to-output latency (kLatency = 1792 samples @ 48 kHz).
class AutoTune {
public:
    static constexpr int kLatency = 1792; // 37.33 ms @ 48 kHz (fits 70 Hz full 2*T0 grain + lookahead)

    enum Scale {
        Scale_Chromatic      = 0,
        Scale_Major          = 1,
        Scale_Minor          = 2,
        Scale_MajorPent      = 3,
        Scale_MinorPent      = 4,
        Scale_Blues          = 5,
        Scale_HarmonicMinor  = 6,
        Scale_Dorian         = 7,
        Scale_Mixolydian     = 8,
        Scale_COUNT          = 9
    };

    AutoTune() {
        setSampleRate(48000.0);
    }

    void setSampleRate(double sr) {
        m_fs = (sr > 1000.0) ? sr : 48000.0;
        const double ratio = m_fs / 48000.0;

        m_latency = clampInt(static_cast<int>(std::lround(kLatency * ratio)), 512, kMaxLatency);
        m_maxGrainHalf = clampInt(static_cast<int>(std::lround(720.0 * ratio)), 256, kMaxGrainHalfCap);
        m_decim = clampInt(static_cast<int>(std::lround(m_fs / 12000.0)), 1, 16);
        m_dfs = m_fs / static_cast<double>(m_decim);

        // Make hop an exact multiple of m_decim so YIN runs cleanly on decimated frames.
        int rawHop = clampInt(static_cast<int>(std::lround(192.0 * ratio)), m_decim, 512);
        m_hopSamples = std::max(m_decim, (rawHop / m_decim) * m_decim);

        m_unvoicedStep = clampInt(static_cast<int>(std::lround(240.0 * ratio)), 64, 512);
        m_holdMaxSamples = static_cast<int>(std::lround(0.150 * m_fs));

        // ~45 Hz DC / rumble blocker for pitch detection only (audio path stays untouched).
        m_detHpCoeff = static_cast<float>(std::exp(-kTwoPi * 45.0 / m_fs));
        // ~1 kHz 1-pole LP for clean glottal peak picking on voiced onset.
        m_lpCoeff = static_cast<float>(1.0 - std::exp(-kTwoPi * 1000.0 / m_fs));

        reset();
    }

    void setParams(float strength, float retuneMs, int scale, int key) {
        m_strength = std::max(0.0f, std::min(1.0f, strength));
        m_retuneMs = std::max(0.0f, std::min(200.0f, retuneMs));
        m_scale    = clampInt(scale, 0, Scale_COUNT - 1);
        m_key      = ((key % 12) + 12) % 12;
    }

    void reset() {
        std::memset(m_inRing, 0, sizeof(m_inRing));
        std::memset(m_lpRing, 0, sizeof(m_lpRing));
        std::memset(m_outRing, 0, sizeof(m_outRing));
        std::memset(m_normRing, 0, sizeof(m_normRing));
        std::memset(m_decimRing, 0, sizeof(m_decimRing));
        for (int i = 0; i < kEstRing; ++i) {
            m_estRing[i] = PitchEst();
        }

        m_n = 0;
        m_detHpX1 = 0.0f;
        m_detHpY1 = 0.0f;
        m_lpState = 0.0f;
        m_decimAcc = 0.0f;
        m_decimCount = 0;
        m_decimW = 0;
        m_hopCounter = 0;

        m_yinVoiced = false;
        m_yinEnterCount = 0;
        m_yinExitCount = 0;
        m_yinPrevRawMidi = -1.0f;
        m_yinPrevHz = -1.0f;
        m_estWrite = 0;

        for (int i = 0; i < kMaxMarks; ++i) {
            m_marks[i] = { 0.0, static_cast<float>(m_unvoicedStep), false };
        }
        m_markWrite = 0;
        m_markCount = 0;
        m_lastMarkPos = 0.0;
        m_lastMarkPeriod = static_cast<float>(m_unvoicedStep);
        m_lastMarkVoiced = false;

        // Seed initial unvoiced marker at 0 so synthesis starts locked to integer grid.
        pushMark(0.0, static_cast<float>(m_unvoicedStep), false);
        m_nextSynthPos = 0.0;

        m_heldMidi = -1.0f;
        m_corrSt = 0.0f;
        m_unvoicedGapSamples = m_holdMaxSamples + 1;
        m_wasContinuouslyVoiced = false;
        m_prevSynthMidiIn = -1.0f;

        m_diagVoiced = false;
        m_diagDetectedHz = 0.0f;
        m_diagTargetHz = 0.0f;
        m_diagNoteMidi = -1.0f;
        m_diagClarity = 0.0f;
    }

    float process(float in) {
        const uint64_t n = m_n;
        const uint32_t idx = static_cast<uint32_t>(n) & kRingMask;

        // 1. Store raw input + LP-filtered signal for glottal onset peak search.
        m_inRing[idx] = in;
        m_lpState += m_lpCoeff * (in - m_lpState);
        m_lpState = flushDenorm(m_lpState);
        m_lpRing[idx] = m_lpState;

        // 2. Feed detection branch (45 Hz HPF -> decimation boxcar -> YIN every hop).
        float hp = m_detHpCoeff * (m_detHpY1 + in - m_detHpX1);
        hp = flushDenorm(hp);
        m_detHpX1 = in;
        m_detHpY1 = hp;

        m_decimAcc += hp;
        if (++m_decimCount >= m_decim) {
            m_decimRing[m_decimW] = m_decimAcc / static_cast<float>(m_decim);
            m_decimW = (m_decimW + 1) & kYinMask;
            m_decimAcc = 0.0f;
            m_decimCount = 0;
        }

        if (++m_hopCounter >= m_hopSamples) {
            m_hopCounter = 0;
            runYin(n);
        }

        // 3. Advance incremental analysis markers up to safe input causal horizon.
        //    Worst-case 70 Hz period: 1.15*T0 + halfCorr <= ~950 samples (< 1072).
        const double placeLimit = static_cast<double>(n) - static_cast<double>(m_maxPlaceAhead());
        while (m_lastMarkPos < placeLimit) {
            placeNextAnalysisMark(n);
        }

        // 4. Advance TD-PSOLA synthesis markers up to t_emit + m_maxGrainHalf.
        const int64_t tEmit = static_cast<int64_t>(n) - static_cast<int64_t>(m_latency);
        const double synthHorizon = static_cast<double>(tEmit + m_maxGrainHalf);
        while (m_nextSynthPos <= synthHorizon) {
            synthesizeNextGrain(n);
        }

        // 5. Read delayed output (zero before m_latency samples have entered).
        float out = 0.0f;
        if (tEmit >= 0) {
            const uint32_t rIdx = static_cast<uint32_t>(tEmit) & kRingMask;
            const float norm = m_normRing[rIdx];
            const float sum  = m_outRing[rIdx];
            m_outRing[rIdx]  = 0.0f;
            m_normRing[rIdx] = 0.0f;
            if (norm > 0.20f) {
                out = sum / norm;
            } else if (norm > 1e-5f) {
                // Smoothly crossfade towards aligned dry if window coverage ever dips very low.
                float dry = m_inRing[rIdx];
                float blend = norm / 0.20f;
                out = blend * (sum / norm) + (1.0f - blend) * dry;
            } else {
                out = m_inRing[rIdx];
            }
        }

        ++m_n;
        return out;
    }

    int latencySamples() const { return m_latency; }

    // Cheap diagnostic getters (updated at synthesis markers).
    bool  isVoiced() const { return m_diagVoiced; }
    float detectedHz() const { return m_diagDetectedHz; }
    float targetHz() const { return m_diagTargetHz; }
    float currentNoteMidi() const { return m_diagNoteMidi; }
    float clarity() const { return m_diagClarity; }

private:
    static constexpr double kTwoPi = 6.28318530717958647692;
    static constexpr int kRingSize = 8192;
    static constexpr uint32_t kRingMask = kRingSize - 1;
    static constexpr int kMaxLatency = 3584;
    static constexpr int kMaxGrainHalfCap = 1440;

    static constexpr int kYinBuf  = 512;
    static constexpr int kYinMask = kYinBuf - 1;
    static constexpr int kYinW    = 256;

    static constexpr int kEstRing = 32;
    static constexpr int kMaxMarks = 64;

    struct PitchEst {
        double centerSample = 0.0;
        float  hz = -1.0f;
        float  midi = -1.0f;
        float  clarity = 0.0f;
        bool   voiced = false;
    };

    struct AnalysisMark {
        double pos = 0.0;
        float  period = 240.0f;
        bool   voiced = false;
    };

    static inline int clampInt(int v, int lo, int hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static inline float flushDenorm(float v) {
        return (v < 1e-15f && v > -1e-15f) ? 0.0f : v;
    }

    static inline float hzToMidi(float hz) {
        return 69.0f + 12.0f * std::log2(hz / 440.0f);
    }

    static inline float midiToHz(float midi) {
        return 440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
    }

    static inline int scaleMask(int scale) {
        static const int kMasks[Scale_COUNT] = {
            0xFFF, // 0: Chromatic
            0xAB5, // 1: Major
            0x5AD, // 2: Minor (natural)
            0x295, // 3: Major pentatonic
            0x4A9, // 4: Minor pentatonic
            0x4E9, // 5: Blues
            0x9AD, // 6: Harmonic minor
            0x6AD, // 7: Dorian
            0x6B5  // 8: Mixolydian
        };
        return kMasks[clampInt(scale, 0, Scale_COUNT - 1)];
    }

    static inline bool isInScale(int midiNote, int scale, int key) {
        int pc = ((midiNote - key) % 12 + 12) % 12;
        return (scaleMask(scale) & (1 << pc)) != 0;
    }

    static inline float nearestScaleMidi(float midi, int scale, int key) {
        const int mask = scaleMask(scale);
        const int centre = static_cast<int>(std::lround(midi));
        int bestNote = centre;
        float bestDist = 1e9f;
        for (int n = centre - 6; n <= centre + 6; ++n) {
            int pc = ((n - key) % 12 + 12) % 12;
            if ((mask & (1 << pc)) == 0) continue;
            float d = std::fabs(midi - static_cast<float>(n));
            if (d < bestDist) {
                bestDist = d;
                bestNote = n;
            }
        }
        return static_cast<float>(bestNote);
    }

    inline float readInHermite(double pos) const {
        const int64_t i0 = static_cast<int64_t>(std::floor(pos));
        const float f = static_cast<float>(pos - static_cast<double>(i0));
        if (f == 0.0f) {
            return (i0 >= 0) ? m_inRing[static_cast<uint32_t>(i0) & kRingMask] : 0.0f;
        }
        auto at = [&](int64_t idx) -> float {
            return (idx >= 0) ? m_inRing[static_cast<uint32_t>(idx) & kRingMask] : 0.0f;
        };
        const float ym1 = at(i0 - 1);
        const float y0  = at(i0);
        const float y1  = at(i0 + 1);
        const float y2  = at(i0 + 2);
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * f + c2) * f + c1) * f + y0;
    }

    inline int m_maxPlaceAhead() const {
        // Max search horizon ahead of m_lastMarkPos needed for 70 Hz (1.15*T0 + halfCorr).
        return static_cast<int>(std::lround(960.0 * (m_fs / 48000.0)));
    }

    void runYin(uint64_t n) {
        const double windowCenter = static_cast<double>(n) - 0.5 * static_cast<double>(m_decim * kYinW);

        // Gather chronological decimated buffer where x[kYinBuf - 1] is the most recent sample.
        float x[kYinBuf];
        for (int i = 0; i < kYinBuf; ++i) {
            x[i] = m_decimRing[(m_decimW + i) & kYinMask];
        }

        // Energy gate on the most recent kYinW decimated samples.
        double energySum = 0.0;
        for (int i = kYinBuf - kYinW; i < kYinBuf; ++i) {
            energySum += static_cast<double>(x[i]) * x[i];
        }
        const float meanSq = static_cast<float>(energySum / static_cast<double>(kYinW));

        float detHz = -1.0f;
        float detMidi = -1.0f;
        float detClarity = 0.0f;
        bool  candidateValid = false;

        if (meanSq >= 1e-7f) {
            const int tauMin = std::max(2, static_cast<int>(m_dfs / 1050.0));
            const int tauMax = std::min(kYinBuf - kYinW, static_cast<int>(m_dfs / 65.0));

            if (tauMax > tauMin + 2) {
                float dp[kYinBuf];
                std::fill(dp, dp + tauMax + 1, 1.0f);
                double cum = 0.0;

                // YIN CMNDF on the MOST RECENT kYinW samples ending at x[kYinBuf - 1].
                for (int tau = 1; tau <= tauMax; ++tau) {
                    double s = 0.0;
                    for (int i = 0; i < kYinW; ++i) {
                        const float d = x[kYinBuf - 1 - i] - x[kYinBuf - 1 - i - tau];
                        s += static_cast<double>(d) * d;
                    }
                    cum += s;
                    dp[tau] = (cum > 1e-12) ? static_cast<float>(s * tau / cum) : 1.0f;
                }

                // 1. Find first valley below threshold 0.24.
                int tauFirst = -1;
                for (int tau = tauMin; tau <= tauMax; ++tau) {
                    if (dp[tau] < 0.24f) {
                        while (tau + 1 <= tauMax && dp[tau + 1] < dp[tau]) ++tau;
                        tauFirst = tau;
                        break;
                    }
                }

                // 2. Find global minimum.
                int tauGlobal = tauMin;
                for (int tau = tauMin + 1; tau <= tauMax; ++tau) {
                    if (dp[tau] < dp[tauGlobal]) tauGlobal = tau;
                }

                int tauPick = (tauFirst >= 0) ? tauFirst : tauGlobal;

                // 3. 2nd/3rd harmonic octave-trap guard: if tauFirst was a harmonic (e.g. 2*f0
                //    +12 dB stronger than f0), 2*tauFirst or 3*tauFirst will have a much deeper CMNDF valley.
                if (tauFirst >= 0) {
                    for (int mult = 2; mult <= 3; ++mult) {
                        int center = tauPick * mult;
                        if (center > tauMax) break;
                        int lo = std::max(tauMin, static_cast<int>(std::lround(center * 0.92)));
                        int hi = std::min(tauMax, static_cast<int>(std::lround(center * 1.08)));
                        int bestM = lo;
                        for (int t = lo + 1; t <= hi; ++t) {
                            if (dp[t] < dp[bestM]) bestM = t;
                        }
                        if (dp[bestM] < 0.65f * dp[tauPick] && dp[bestM] < 0.16f) {
                            tauPick = bestM;
                            break;
                        }
                    }
                }

                // 4. Octave-continuity guard against previous voiced frequency.
                if (m_yinVoiced && m_yinPrevHz > 65.0f) {
                    const double expTau = m_dfs / static_cast<double>(m_yinPrevHz);
                    const double ratio = static_cast<double>(tauPick) / expTau;
                    if (ratio < 0.65 || ratio > 1.55) {
                        int lo = std::max(tauMin, static_cast<int>(std::lround(expTau * 0.85)));
                        int hi = std::min(tauMax, static_cast<int>(std::lround(expTau * 1.15)));
                        if (hi >= lo) {
                            int bestC = lo;
                            for (int t = lo + 1; t <= hi; ++t) {
                                if (dp[t] < dp[bestC]) bestC = t;
                            }
                            if (dp[bestC] < 0.24f && dp[bestC] <= dp[tauPick] + 0.02f) {
                                tauPick = bestC;
                            }
                        }
                    }
                }

                // 5. Parabolic interpolation at decimated rate, then full-rate lag refinement.
                double tauF = static_cast<double>(tauPick);
                if (tauPick > tauMin && tauPick < tauMax) {
                    const float a = dp[tauPick - 1], b = dp[tauPick], c = dp[tauPick + 1];
                    const float den = a - 2.0f * b + c;
                    if (std::fabs(den) > 1e-9f) {
                        tauF += std::max(-0.5, std::min(0.5, 0.5 * static_cast<double>(a - c) / static_cast<double>(den)));
                    }
                }

                double refinedLag = tauF * static_cast<double>(m_decim);
                const int centerLag = static_cast<int>(std::lround(refinedLag));
                const int fullW = std::min(512, m_decim * kYinW);
                if (centerLag >= 20 && n >= static_cast<uint64_t>(centerLag + fullW + 4)) {
                    float errs[5] = {};
                    int bestOff = 0;
                    float bestErr = 1e30f;
                    for (int k = -2; k <= 2; ++k) {
                        const int lag = centerLag + k;
                        double acc = 0.0;
                        for (int i = 0; i < fullW; ++i) {
                            const uint32_t iA = static_cast<uint32_t>(n - static_cast<uint64_t>(i)) & kRingMask;
                            const uint32_t iB = static_cast<uint32_t>(n - static_cast<uint64_t>(i + lag)) & kRingMask;
                            const float d = m_inRing[iA] - m_inRing[iB];
                            acc += static_cast<double>(d) * d;
                        }
                        errs[k + 2] = static_cast<float>(acc);
                        if (errs[k + 2] < bestErr) {
                            bestErr = errs[k + 2];
                            bestOff = k;
                        }
                    }
                    const int idxBest = bestOff + 2;
                    double fineLag = static_cast<double>(centerLag + bestOff);
                    if (idxBest > 0 && idxBest < 4) {
                        const float a = errs[idxBest - 1], b = errs[idxBest], c = errs[idxBest + 1];
                        const float den = a - 2.0f * b + c;
                        if (den > 1e-9f && b <= a && b <= c) {
                            fineLag += std::max(-0.5, std::min(0.5, 0.5 * static_cast<double>(a - c) / static_cast<double>(den)));
                        }
                    }
                    refinedLag = fineLag;
                }

                const float hz = static_cast<float>(m_fs / refinedLag);
                if (hz >= 65.0f && hz <= 1020.0f) {
                    detHz = hz;
                    detMidi = hzToMidi(hz);
                    detClarity = std::max(0.0f, std::min(1.0f, 1.0f - dp[tauPick]));
                    candidateValid = true;
                }
            }
        }

        // Clarity hysteresis: enter voiced after 2 coherent hops (>0.73), exit after 2 invalid hops (<0.66).
        if (candidateValid) {
            if (!m_yinVoiced) {
                if (detClarity > 0.73f) {
                    if (m_yinEnterCount > 0 && std::fabs(detMidi - m_yinPrevRawMidi) < 3.0f) {
                        m_yinVoiced = true;
                        m_yinEnterCount = 0;
                        m_yinExitCount = 0;
                    } else {
                        m_yinEnterCount = 1;
                    }
                    m_yinPrevRawMidi = detMidi;
                } else {
                    m_yinEnterCount = 0;
                }
            } else {
                if (detClarity > 0.66f) {
                    m_yinExitCount = 0;
                    m_yinPrevRawMidi = detMidi;
                } else if (++m_yinExitCount >= 2) {
                    m_yinVoiced = false;
                    m_yinEnterCount = 0;
                }
            }
        } else {
            m_yinEnterCount = 0;
            if (m_yinVoiced && ++m_yinExitCount >= 2) {
                m_yinVoiced = false;
            }
        }

        if (m_yinVoiced && candidateValid) {
            m_yinPrevHz = detHz;
        } else if (!m_yinVoiced) {
            m_yinPrevHz = -1.0f;
        }

        PitchEst &slot = m_estRing[m_estWrite & (kEstRing - 1)];
        slot.centerSample = windowCenter;
        slot.hz = detHz;
        slot.midi = detMidi;
        slot.clarity = detClarity;
        slot.voiced = m_yinVoiced && candidateValid;
        ++m_estWrite;
    }

    PitchEst lookupEstimate(double sampleTime) const {
        const uint32_t count = std::min<uint32_t>(m_estWrite, kEstRing);
        if (count == 0) return {};

        int bestIdx = 0;
        double bestDist = 1e30;
        for (uint32_t k = 0; k < count; ++k) {
            uint32_t r = (m_estWrite - 1 - k) & (kEstRing - 1);
            double d = std::fabs(m_estRing[r].centerSample - sampleTime);
            if (d < bestDist) {
                bestDist = d;
                bestIdx = static_cast<int>(r);
            }
        }

        PitchEst cur = m_estRing[bestIdx];
        if (count >= 3) {
            const PitchEst &prev = m_estRing[(bestIdx - 1) & (kEstRing - 1)];
            const PitchEst &next = m_estRing[(bestIdx + 1) & (kEstRing - 1)];

            // 3-point outlier median for isolated 1-hop pitch spikes (> 3 st).
            if (prev.voiced && cur.voiced && next.voiced) {
                if (std::fabs(cur.midi - prev.midi) > 3.0f &&
                    std::fabs(cur.midi - next.midi) > 3.0f &&
                    std::fabs(next.midi - prev.midi) < 2.5f) {
                    float a = prev.midi, b = cur.midi, c = next.midi;
                    if (a > b) std::swap(a, b);
                    if (b > c) std::swap(b, c);
                    if (a > b) std::swap(a, b);
                    cur.midi = b;
                    cur.hz = midiToHz(b);
                }
            } else if (!cur.voiced && prev.voiced && next.voiced &&
                       std::fabs(next.midi - prev.midi) < 2.0f) {
                // Bridge single-hop dropout inside steady vowel without delaying real transitions.
                cur.voiced = true;
                cur.midi = 0.5f * (prev.midi + next.midi);
                cur.hz = midiToHz(cur.midi);
                cur.clarity = 0.5f * (prev.clarity + next.clarity);
            }
        }
        return cur;
    }

    void pushMark(double pos, float period, bool voiced) {
        m_marks[m_markWrite] = { pos, period, voiced };
        m_markWrite = (m_markWrite + 1) % kMaxMarks;
        if (m_markCount < kMaxMarks) ++m_markCount;
        m_lastMarkPos = pos;
        m_lastMarkPeriod = period;
        m_lastMarkVoiced = voiced;
    }

    void placeNextAnalysisMark(uint64_t n) {
        const double nominal = m_lastMarkPos + (m_lastMarkVoiced ? static_cast<double>(m_lastMarkPeriod) : static_cast<double>(m_unvoicedStep));
        const PitchEst est = lookupEstimate(nominal);

        if (!est.voiced || m_strength < 0.005f || est.hz <= 0.0f) {
            // Exact integer step for bit-transparent unvoiced/bypass OLA.
            const double nextPos = std::round(m_lastMarkPos) + static_cast<double>(m_unvoicedStep);
            pushMark(nextPos, static_cast<float>(m_unvoicedStep), false);
            return;
        }

        const float T0 = std::max(45.0f, std::min(740.0f, static_cast<float>(m_fs / static_cast<double>(est.hz))));
        if (!m_lastMarkVoiced) {
            // First voiced marker: locate glottal energy peak (positive max of LP signal) near nominal.
            const int64_t center = static_cast<int64_t>(std::lround(m_lastMarkPos + 0.5 * T0));
            const int halfSearch = std::max(12, static_cast<int>(std::lround(0.5 * T0)));
            int64_t lo = std::max<int64_t>(static_cast<int64_t>(std::floor(m_lastMarkPos)) + 16, center - halfSearch);
            int64_t hi = std::min<int64_t>(static_cast<int64_t>(n) - 16, center + halfSearch);
            if (hi < lo) lo = hi = center;

            int64_t bestPos = center;
            float bestVal = -1e30f;
            for (int64_t p = lo; p <= hi; ++p) {
                float v = (p >= 0) ? m_lpRing[static_cast<uint32_t>(p) & kRingMask] : 0.0f;
                if (v > bestVal) {
                    bestVal = v;
                    bestPos = p;
                }
            }
            pushMark(static_cast<double>(bestPos), T0, true);
            return;
        }

        // Incremental pitch-synchronous marker: refine m_{k-1} + T0 via normalized cross-correlation.
        const double nomPos = m_lastMarkPos + static_cast<double>(T0);
        const int64_t prevInt = static_cast<int64_t>(std::lround(m_lastMarkPos));
        const int64_t nomInt  = static_cast<int64_t>(std::lround(nomPos));
        const int halfCorr = clampInt(static_cast<int>(std::lround(0.38f * T0)), 16, 140);
        const int stride   = (T0 > 180.0f) ? 2 : 1;
        const int searchR  = std::max(4, static_cast<int>(std::lround(0.14f * T0)));

        const int64_t minCand = std::max(prevInt + std::max(24, static_cast<int>(0.55f * T0)), nomInt - searchR);
        const int64_t maxCand = std::min(static_cast<int64_t>(n) - halfCorr - 4, nomInt + searchR);

        if (maxCand <= minCand) {
            pushMark(nomPos, T0, true);
            return;
        }

        double refEnergy = 1e-12;
        for (int j = -halfCorr; j <= halfCorr; j += stride) {
            int64_t p = prevInt + j;
            float v = (p >= 0) ? m_inRing[static_cast<uint32_t>(p) & kRingMask] : 0.0f;
            refEnergy += static_cast<double>(v) * v;
        }

        // Coarse search at stride, store scores for parabolic refinement around peak.
        int64_t bestC = nomInt;
        double bestScore = -1e30;
        double prevScore = 0.0, midScore = 0.0, nextScore = 0.0;

        auto evalNcc = [&](int64_t c) -> double {
            double num = 0.0, candE = 1e-12;
            for (int j = -halfCorr; j <= halfCorr; j += stride) {
                int64_t pA = prevInt + j;
                int64_t pB = c + j;
                float a = (pA >= 0) ? m_inRing[static_cast<uint32_t>(pA) & kRingMask] : 0.0f;
                float b = (pB >= 0) ? m_inRing[static_cast<uint32_t>(pB) & kRingMask] : 0.0f;
                num += static_cast<double>(a) * b;
                candE += static_cast<double>(b) * b;
            }
            double distNorm = static_cast<double>(c - nomInt) / static_cast<double>(T0);
            double bias = 1.0 - 0.002 * distNorm * distNorm;
            return (num / std::sqrt(refEnergy * candE)) * bias;
        };

        for (int64_t c = minCand; c <= maxCand; ++c) {
            double sc = evalNcc(c);
            if (sc > bestScore) {
                bestScore = sc;
                bestC = c;
            }
        }

        double refinedPos = static_cast<double>(bestC);
        if (bestC > minCand && bestC < maxCand) {
            prevScore = evalNcc(bestC - 1);
            midScore  = bestScore;
            nextScore = evalNcc(bestC + 1);
            double den = prevScore - 2.0 * midScore + nextScore;
            if (den < -1e-9 && midScore >= prevScore && midScore >= nextScore) {
                double delta = 0.5 * (prevScore - nextScore) / den;
                refinedPos += std::max(-0.5, std::min(0.5, delta));
            }
        }

        pushMark(refinedPos, T0, true);
    }

    AnalysisMark closestAnalysisMark(double pos) const {
        AnalysisMark best = m_marks[0];
        double bestDist = 1e30;
        for (int i = 0; i < m_markCount; ++i) {
            double d = std::fabs(m_marks[i].pos - pos);
            if (d < bestDist) {
                bestDist = d;
                best = m_marks[i];
            }
        }
        return best;
    }

    double nextUnvoicedMarkAfter(double pos) const {
        double best = pos + static_cast<double>(m_unvoicedStep);
        double bestDist = 1e30;
        for (int i = 0; i < m_markCount; ++i) {
            if (!m_marks[i].voiced && m_marks[i].pos > pos + 0.5) {
                double d = m_marks[i].pos - pos;
                if (d < bestDist) {
                    bestDist = d;
                    best = m_marks[i].pos;
                }
            }
        }
        return best;
    }

    void synthesizeNextGrain(uint64_t n) {
        const double ts = m_nextSynthPos;
        const AnalysisMark mark = closestAnalysisMark(ts);
        const PitchEst est = lookupEstimate(ts);

        const bool activeVoiced = est.voiced && mark.voiced && (m_strength >= 0.005f) && (est.hz > 0.0f);

        if (!activeVoiced) {
            // Unvoiced / silence / bypass: snap synthesis grid cleanly onto exact integer analysis markers.
            m_unvoicedGapSamples += m_unvoicedStep;
            if (m_unvoicedGapSamples > m_holdMaxSamples) {
                m_heldMidi = -1.0f;
                m_wasContinuouslyVoiced = false;
                m_corrSt = 0.0f;
            }
            m_diagVoiced = false;
            m_diagDetectedHz = 0.0f;
            m_diagTargetHz = 0.0f;
            m_diagNoteMidi = m_heldMidi;
            m_diagClarity = est.clarity;

            const int W = m_unvoicedStep;
            const int64_t tEmitMin = std::max<int64_t>(0, static_cast<int64_t>(n) - static_cast<int64_t>(m_latency));
            const double fracDiff = std::fabs(ts - mark.pos);
            if (fracDiff < 1e-4 && !mark.voiced) {
                // Exact integer unvoiced COLA grain: out == in(t - latency) within ~1e-7.
                const int64_t center = static_cast<int64_t>(std::llround(mark.pos));
                const float invTwoW = 1.0f / static_cast<float>(2 * W);
                for (int j = -W; j < W; ++j) {
                    const int64_t p = center + j;
                    if (p < tEmitMin) continue;
                    const float x = static_cast<float>(j + W) * invTwoW;
                    const float w = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * x));
                    const float s = m_inRing[static_cast<uint32_t>(p) & kRingMask];
                    const uint32_t dst = static_cast<uint32_t>(p) & kRingMask;
                    m_outRing[dst]  += s * w;
                    m_normRing[dst] += w;
                }
            } else {
                // Transition grain from voiced -> unvoiced: write Hann-windowed grain at fractional ts.
                const int64_t tFloor = static_cast<int64_t>(std::floor(ts));
                const float invTwoW = 1.0f / static_cast<float>(2 * W);
                for (int j = -W; j <= W; ++j) {
                    const int64_t tOut = tFloor + j;
                    if (tOut < tEmitMin) continue;
                    const double delta = static_cast<double>(tOut) - ts;
                    if (std::fabs(delta) >= static_cast<double>(W)) continue;
                    const float u = static_cast<float>(delta + static_cast<double>(W)) * invTwoW;
                    const float w = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * u));
                    const float s = readInHermite(mark.pos + delta);
                    const uint32_t dst = static_cast<uint32_t>(tOut) & kRingMask;
                    m_outRing[dst]  += s * w;
                    m_normRing[dst] += w;
                }
            }

            // Lock next synthesis marker onto the upcoming integer unvoiced marker.
            const double nextUv = nextUnvoicedMarkAfter(ts);
            if (std::fabs(nextUv - ts) <= 1.55 * static_cast<double>(m_unvoicedStep)) {
                m_nextSynthPos = nextUv;
            } else {
                m_nextSynthPos = std::round(ts) + static_cast<double>(m_unvoicedStep);
            }
            return;
        }

        // ---- Voiced synthesis ----
        m_unvoicedGapSamples = 0;
        const float midiIn = est.midi;
        const float nearest = nearestScaleMidi(midiIn, m_scale, m_key);

        bool noteChanged = false;
        if (m_heldMidi < 0.0f || !isInScale(static_cast<int>(std::lround(m_heldMidi)), m_scale, m_key)) {
            m_heldMidi = nearest;
            noteChanged = true;
        } else if (nearest != m_heldMidi) {
            const float dHeld = std::fabs(midiIn - m_heldMidi);
            const float dNew  = std::fabs(midiIn - nearest);
            // ~0.30 semitone hysteresis past midpoint prevents boundary flicker and preserves +-60c vibrato.
            if (dNew + 0.60f < dHeld) {
                m_heldMidi = nearest;
                noteChanged = true;
            }
        }

        float desiredCorr = (m_heldMidi - midiIn) * m_strength;
        desiredCorr = std::max(-12.0f, std::min(12.0f, desiredCorr));

        if (m_retuneMs <= 0.05f) {
            m_corrSt = desiredCorr;
        } else {
            // Preserve output pitch continuity across large discontinuous note steps when retuneMs > 0,
            // while still letting slow retune preserve vocal vibrato inside a held note.
            if (noteChanged && m_wasContinuouslyVoiced && m_prevSynthMidiIn > 0.0f) {
                float prevOutMidi = m_prevSynthMidiIn + m_corrSt;
                m_corrSt = std::max(-12.0f, std::min(12.0f, prevOutMidi - midiIn));
            }
            const double approxTs = m_fs / static_cast<double>(midiToHz(midiIn + m_corrSt));
            const double dtSec = std::max(0.0005, approxTs / m_fs);
            const float alpha = static_cast<float>(1.0 - std::exp(-dtSec / (static_cast<double>(m_retuneMs) * 0.001)));
            m_corrSt += (desiredCorr - m_corrSt) * alpha;
            m_corrSt = flushDenorm(std::max(-12.0f, std::min(12.0f, m_corrSt)));
        }

        m_wasContinuouslyVoiced = true;
        m_prevSynthMidiIn = midiIn;

        const float hzOut = std::max(40.0f, std::min(1800.0f, est.hz * std::pow(2.0f, m_corrSt / 12.0f)));
        const double Ts = std::max(24.0, std::min(1200.0, m_fs / static_cast<double>(hzOut)));

        m_diagVoiced = true;
        m_diagDetectedHz = est.hz;
        m_diagTargetHz = hzOut;
        m_diagNoteMidi = m_heldMidi;
        m_diagClarity = est.clarity;

        // Grain half-width W = T0 (widened smoothly up to 1.44*T0 for deep downward shifts so COLA stays > 0.25).
        const float T0 = std::max(45.0f, std::min(740.0f, mark.period));
        const int W = clampInt(
            static_cast<int>(std::lround(std::max(static_cast<double>(T0), 0.72 * Ts))),
            24,
            std::min(m_maxGrainHalf, static_cast<int>(std::lround(1.45f * T0)))
        );

        const int64_t tFloor = static_cast<int64_t>(std::floor(ts));
        const int64_t tEmitMin = std::max<int64_t>(0, static_cast<int64_t>(n) - static_cast<int64_t>(m_latency));
        const float invTwoW = 1.0f / static_cast<float>(2 * W);

        for (int j = -W; j <= W + 1; ++j) {
            const int64_t tOut = tFloor + j;
            if (tOut < tEmitMin) continue;
            const double delta = static_cast<double>(tOut) - ts;
            if (delta <= -static_cast<double>(W) || delta >= static_cast<double>(W)) continue;

            const float u = static_cast<float>(delta + static_cast<double>(W)) * invTwoW;
            const float w = 0.5f * (1.0f - std::cos(static_cast<float>(kTwoPi) * u));
            const float s = readInHermite(mark.pos + delta);

            const uint32_t dst = static_cast<uint32_t>(tOut) & kRingMask;
            m_outRing[dst]  += s * w;
            m_normRing[dst] += w;
        }

        m_nextSynthPos = ts + Ts;
    }

    double m_fs = 48000.0;
    double m_dfs = 12000.0;
    int    m_latency = kLatency;
    int    m_maxGrainHalf = 720;
    int    m_decim = 4;
    int    m_hopSamples = 192;
    int    m_unvoicedStep = 240;
    int    m_holdMaxSamples = 7200;

    float m_strength = 1.0f;
    float m_retuneMs = 0.0f;
    int   m_scale = Scale_Chromatic;
    int   m_key = 0;

    float m_detHpCoeff = 0.994f;
    float m_lpCoeff = 0.12f;

    // Input / LP / Output OLA rings (power-of-2 size for bitmask indexing).
    float m_inRing[kRingSize]   = {};
    float m_lpRing[kRingSize]   = {};
    float m_outRing[kRingSize]  = {};
    float m_normRing[kRingSize] = {};

    // Pitch detection state.
    float    m_decimRing[kYinBuf] = {};
    float    m_detHpX1 = 0.0f;
    float    m_detHpY1 = 0.0f;
    float    m_lpState = 0.0f;
    float    m_decimAcc = 0.0f;
    int      m_decimCount = 0;
    int      m_decimW = 0;
    int      m_hopCounter = 0;

    bool     m_yinVoiced = false;
    int      m_yinEnterCount = 0;
    int      m_yinExitCount = 0;
    float    m_yinPrevRawMidi = -1.0f;
    float    m_yinPrevHz = -1.0f;
    PitchEst m_estRing[kEstRing] = {};
    uint32_t m_estWrite = 0;

    // Analysis marks and synthesis scheduler.
    AnalysisMark m_marks[kMaxMarks] = {};
    int    m_markWrite = 0;
    int    m_markCount = 0;
    double m_lastMarkPos = 0.0;
    float  m_lastMarkPeriod = 240.0f;
    bool   m_lastMarkVoiced = false;
    double m_nextSynthPos = 0.0;

    // Note hold & correction dynamics.
    float  m_heldMidi = -1.0f;
    float  m_corrSt = 0.0f;
    int    m_unvoicedGapSamples = 0;
    bool   m_wasContinuouslyVoiced = false;
    float  m_prevSynthMidiIn = -1.0f;

    uint64_t m_n = 0;

    // Diagnostic getters.
    bool  m_diagVoiced = false;
    float m_diagDetectedHz = 0.0f;
    float m_diagTargetHz = 0.0f;
    float m_diagNoteMidi = -1.0f;
    float m_diagClarity = 0.0f;
};
