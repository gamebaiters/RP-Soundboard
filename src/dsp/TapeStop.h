#pragma once

#include <vector>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <algorithm>

// Tape-stop / vinyl-brake / scratch effect (per slot, post-chain).
//
// ARCHITECTURE (v3 - sliding decode-ahead window with backward backfill).
// The ring is a FIXED-SIZE (~10 s) cache that SLIDES over the file, like
// the in-memory buffer of DJ software:
//
//     [ oldest ........ HEAD ........ frontier ]
//       backfilled       |       decode-ahead (~1 s)
//       history          |
//
// Positions are ABSOLUTE int64 sample indices (no modular-wrap
// ambiguity, which in v2 slammed the head backward on fast forward
// scratches). The valid region is [m_oldest, m_frontier); the head
// m_headAbs sits inside it; ring storage index = absolute % N.
//
//   - Forward ingest() appends at the frontier (decode-ahead). When the
//     window is full it slides forward (drops the oldest).
//   - PRODUCE reads the head at the play rate.
//   - Forward scratch has the ~1 s decode-ahead budget; forward beyond
//     that is a decoder seek (the Sampler router).
//   - Backward scratch consumes history behind the head. When that runs
//     low the Sampler decodes the PREVIOUS chunk off-thread and pushes
//     it via feedBackfill(), which PREPENDS it at the oldest end and
//     trims the now-unneeded forward frontier - so the window slides
//     BACKWARD with the head. Backward is therefore effectively
//     infinite while memory stays a fixed 10 s ring (backfillWant()
//     tells the Sampler how much older audio to fetch, and
//     backfillFilePosHint()/setBackfill FilePos anchor the file mapping).
//
// SCRATCH is a critically damped position servo (platter inertia + hand
// grip); reads are 4-point Catmull-Rom. No CatchUp phase: after a brake
// or scratch the needle plays on from where it is.
//
// Threading: ingest()/produce()/feedBackfill()/ingestBudget() run on the
// AUDIO thread only. GUI commands use atomics; m_restart resets the
// window indices on the audio thread (never memset the 4 MB ring).
class TapeStop {
public:
    enum Phase { Idle = 0, Braking, Stopped, SpinUp, CatchUp, Scratch, Armed };

    void setSampleRate(double sr) {
        m_fs = (sr > 0) ? sr : 48000.0;
        // Hand-grip spring: critically damped, fc ~22 Hz.
        m_servoW = 6.283185307179586 * 22.0 / m_fs;
        m_gainCoef = static_cast<float>(1.0 - std::exp(-1.0 / (0.005 * m_fs)));
    }

    // ---- GUI-thread commands ----

    void arm(bool on) {
        m_armed.store(on, std::memory_order_relaxed);
        if (on) {
            ensureRing();
            int ph = m_phase.load(std::memory_order_relaxed);
            if (ph == Idle) {
                m_restart.store(true, std::memory_order_relaxed);
                m_phase.store(Armed, std::memory_order_release);
            }
        } else {
            int expected = Armed;
            m_phase.compare_exchange_strong(expected, Idle,
                                            std::memory_order_release,
                                            std::memory_order_relaxed);
        }
    }

    void trigger(float brakeMs) {
        ensureRing();
        float ms = std::min(4000.0f, std::max(80.0f, brakeMs));
        m_step = static_cast<float>(1.0 / (ms * 0.001 * m_fs));
        int ph = m_phase.load(std::memory_order_relaxed);
        if (ph == Idle) { m_restart.store(true, std::memory_order_relaxed); m_rate = 1.0f; }
        m_phase.store(Braking, std::memory_order_release);
    }

    void release(float spinMs) {
        int ph = m_phase.load(std::memory_order_relaxed);
        if (ph == Idle || ph == Armed) return;
        float ms = std::min(4000.0f, std::max(80.0f, spinMs));
        m_step = static_cast<float>(1.0 / (ms * 0.001 * m_fs));
        m_phase.store(SpinUp, std::memory_order_release);
    }

    // ---- Scratch ----
    void scratchBegin() {
        ensureRing();
        m_scratchPendingFP.store(0, std::memory_order_relaxed);
        int ph = m_phase.load(std::memory_order_relaxed);
        if (ph == Idle) { m_restart.store(true, std::memory_order_relaxed); m_rate = 1.0f; }
        m_scratchResync.store(true, std::memory_order_relaxed);
        // Anchor the head-displacement accumulator on the next produce
        // (the audio thread owns m_headAbs) so the cursor tracks the
        // head's absolute file motion for this gesture.
        m_captureHeadStart.store(true, std::memory_order_relaxed);
        m_headDispSec.store(0.0f, std::memory_order_relaxed);
        m_phase.store(Scratch, std::memory_order_release);
    }

    void scratchDelta(float deltaSeconds) {
        if (deltaSeconds > 2.0f)  deltaSeconds = 2.0f;
        if (deltaSeconds < -2.0f) deltaSeconds = -2.0f;
        int64_t fp = static_cast<int64_t>(
            std::llround(static_cast<double>(deltaSeconds) * m_fs * 256.0));
        m_scratchPendingFP.fetch_add(fp, std::memory_order_relaxed);
    }

    // Needle jump (forward seek): the decoder moved, window is obsolete.
    void scratchRebase() {
        if (m_phase.load(std::memory_order_relaxed) != Scratch) return;
        m_restart.store(true, std::memory_order_relaxed);
        // The restart resets the head coordinate (headAbs -> 0) on the next
        // produce. Re-arm the head-start capture so headStartAbs re-syncs to
        // that new origin; without this headDisplacement = (0 - stale
        // headStartAbs) reads hugely NEGATIVE and snaps the cursor (and the
        // next scrub-seek's base, via the position cache) backward on a
        // forward needle-jump = "fast forward jumps back". Zero the
        // published displacement now (GUI thread) so the cursor reads the
        // seek target immediately; the Sampler re-anchors scratchStartCursor
        // to that target, so cursor = target + 0.
        m_captureHeadStart.store(true, std::memory_order_relaxed);
        m_headDispSec.store(0.0f, std::memory_order_relaxed);
    }

    void scratchEnd(float spinMs) {
        if (m_phase.load(std::memory_order_relaxed) != Scratch) return;
        float ms = std::min(4000.0f, std::max(80.0f, spinMs));
        float dist = std::fabs(1.0f - m_rate);
        if (dist < 1.0f) dist = 1.0f;
        m_step = static_cast<float>(dist / (ms * 0.001 * m_fs));
        m_phase.store(SpinUp, std::memory_order_release);
    }

    void snapReset() {
        m_rate = 1.0f;
        m_restart.store(true, std::memory_order_relaxed);
        m_lagSec.store(0.0f, std::memory_order_relaxed);
        bool rearm = m_armed.load(std::memory_order_relaxed) && !m_ringL.empty();
        m_phase.store(rearm ? Armed : Idle, std::memory_order_release);
    }
    void reset() { snapReset(); }

    Phase phase() const { return static_cast<Phase>(m_phase.load(std::memory_order_acquire)); }
    bool fullyStopped() const { return phase() == Stopped; }
    bool active() const { return phase() != Idle; }
    bool retiming() const { Phase p = phase(); return p != Idle && p != Armed; }

    // Forward decode-ahead room (head -> frontier), output seconds.
    float lagSeconds() const { return m_lagSec.load(std::memory_order_relaxed); }
    // Net head displacement since scratchBegin, in OUTPUT seconds
    // (negative = moved backward). The cursor uses THIS during a scratch
    // instead of head-to-frontier lag: the frontier trim keeps the lag
    // pinned ~1.5 s while the head roams the whole file via backfill, so
    // the lag no longer reflects the head's absolute position - it was
    // the "cursor stays at 27 s while the audio scratches back" bug.
    float headDisplacementSeconds() const {
        return m_headDispSec.load(std::memory_order_relaxed);
    }
    // Backward room currently in the ring (head -> oldest), output secs.
    float backwardRoomSeconds() const {
        return m_backSec.load(std::memory_order_relaxed);
    }
    // Kept for API compat: total valid ring seconds.
    float availableHistorySeconds() const {
        return backwardRoomSeconds() + lagSeconds();
    }

    // ---- Backward backfill (Sampler orchestration) ----
    // How many OLDER output samples the tape wants prepended to keep the
    // backward window near its target. > 0 only while there is a real
    // risk of the backward scratch running out of history. The Sampler
    // decodes that many samples ending just before backfillFileSec() and
    // pushes them via feedBackfill().
    int backfillWantSamples() const {
        if (m_phase.load(std::memory_order_relaxed) != Scratch) return 0;
        double back = m_headAbs - static_cast<double>(m_oldest);
        int target = static_cast<int>(kBackTargetSec * m_fs);
        int want = target - static_cast<int>(back);
        if (want < 0) want = 0;
        // Never ask for more than we can store.
        int cap = N() - static_cast<int>(kMinAheadSamples) - 16;
        if (want > cap) want = cap;
        return want;
    }
    // The file position (input seconds) the oldest ring sample maps to.
    // The Sampler seeds it at play/seek/rebase and the tape advances it
    // as the window slides; backfill decodes the chunk ending here.
    double oldestFileSec() const { return m_oldestFileSec.load(std::memory_order_relaxed); }
    void   setOldestFileSec(double s) { m_oldestFileSec.store(s, std::memory_order_relaxed); }

    // Prepend `n` OLDER output samples (audio thread). Extends history
    // backward and trims the forward frontier if the ring is full - the
    // window slides backward with the head. `olderFirst`==true means
    // L[0]/R[0] is the OLDEST of the batch (natural decode order); the
    // samples are laid out so that reading forward through them plays in
    // file order. Advances oldestFileSec back by n/fs*speed via
    // setOldestFileSec done by the caller.
    void feedBackfill(const float* L, const float* R, int n) {
        applyRestart();
        const int nN = N();
        if (nN == 0 || n <= 0) return;
        for (int i = 0; i < n; ++i) {
            // Need room at the oldest end: total span must stay <= N. If
            // full, drop one frontier sample (stale decode-ahead, not
            // needed while scratching backward) provided the head keeps
            // its guard.
            if (m_frontier - m_oldest >= nN) {
                double aheadRoom = static_cast<double>(m_frontier) - m_headAbs;
                if (aheadRoom <= kMinAheadSamples + 1.0)
                    break;                 // no frontier to sacrifice
                --m_frontier;              // discard newest (stale ahead)
            }
            --m_oldest;
            int idx = ringIndex(m_oldest);
            // L is oldest-first: sample for absolute index m_oldest (the
            // new oldest) is the FIRST not-yet-written one from the tail.
            m_ringL[idx] = L[n - 1 - i];
            m_ringR[idx] = R[n - 1 - i];
        }
        publishRoom();
    }

    // ---- Audio thread: forward ingest / produce ----

    int ingestBudget() const {
        const int nN = N();
        if (nN == 0) return 0;
        int ph = m_phase.load(std::memory_order_relaxed);
        if (ph == Idle) return 0;
        double lag = static_cast<double>(m_frontier) - m_headAbs;
        // The head-to-frontier lag IS the added FX latency: pitch / speed /
        // reverb are applied at the frontier (decoder) and only reach the
        // audible head `lag` seconds later. In the transparent ARMED state
        // (popup open, not scratching) keep that tiny so live FX stays
        // real-time; the full forward-scratch runway is only built once a
        // gesture actually starts (Scratch). Backward history accumulates
        // BEHIND the head regardless of this target, so a backward scratch
        // right after opening the popup still has ~10 s under it.
        double aheadSec = (ph == Scratch) ? kAheadTargetSec : kArmedAheadSec;
        int target = static_cast<int>(aheadSec * m_fs);
        int cap    = nN - kGuard;
        int want   = std::min(target, cap) - static_cast<int>(lag);
        return (want > 0) ? want : 0;
    }

    void ingest(float l, float r) {
        applyRestart();
        const int nN = N();
        if (nN == 0) return;
        double lag = static_cast<double>(m_frontier) - m_headAbs;
        if (lag >= static_cast<double>(nN - kGuard)) return;
        m_ringL[ringIndex(m_frontier)] = l;
        m_ringR[ringIndex(m_frontier)] = r;
        ++m_frontier;
        if (m_frontier - m_oldest > nN) m_oldest = m_frontier - nN;  // slide fwd
        publishRoom();
    }

    void produce(float &l, float &r) {
        applyRestart();
        Phase ph = phase();
        const int nN = N();
        if (ph == Idle || nN == 0) { l = 0.0f; r = 0.0f; return; }

        switch (ph) {
        case Braking:
            m_rate -= m_step;
            if (m_rate <= 0.0f) { m_rate = 0.0f; ph = Stopped;
                m_phase.store(Stopped, std::memory_order_release); }
            break;
        case SpinUp:
            if (m_rate < 1.0f) { m_rate += m_step; if (m_rate > 1.0f) m_rate = 1.0f; }
            else               { m_rate -= m_step; if (m_rate < 1.0f) m_rate = 1.0f; }
            if (m_rate == 1.0f) {
                bool rearm = m_armed.load(std::memory_order_relaxed);
                ph = rearm ? Armed : Idle;
                m_phase.store(ph, std::memory_order_release);
            }
            break;
        case Scratch: {
            if (m_captureHeadStart.exchange(false, std::memory_order_relaxed))
                m_headStartAbs = m_headAbs;
            int64_t pend = m_scratchPendingFP.exchange(0, std::memory_order_relaxed);
            if (m_scratchResync.exchange(false, std::memory_order_relaxed)) {
                m_scratchTargetPos = 0.0;
                m_scratchServoPos  = 0.0;
                m_scratchVel = static_cast<double>(m_rate);
            }
            m_scratchTargetPos += static_cast<double>(pend) / 256.0;
            const double maxLead = 0.25 * m_fs;
            if (m_scratchTargetPos > m_scratchServoPos + maxLead)
                m_scratchTargetPos = m_scratchServoPos + maxLead;
            else if (m_scratchTargetPos < m_scratchServoPos - maxLead)
                m_scratchTargetPos = m_scratchServoPos - maxLead;
            double err = m_scratchTargetPos - m_scratchServoPos;
            m_scratchVel += m_servoW * (m_servoW * err - 2.0 * m_scratchVel);
            if (m_scratchVel >  12.0) m_scratchVel =  12.0;
            if (m_scratchVel < -12.0) m_scratchVel = -12.0;
            m_scratchServoPos += m_scratchVel;
            m_rate = static_cast<float>(m_scratchVel);
            break;
        }
        case Armed: m_rate = 1.0f; break;
        default: break;
        }

        if (ph == Stopped) { l = 0.0f; r = 0.0f; publishRoom(); return; }

        const double minLag = (ph == Scratch) ? 4.0 : 1.0;
        double newHead = m_headAbs + static_cast<double>(m_rate);

        // Clamp to the valid window [oldest+minGuard, frontier-minLag].
        double hiCap = static_cast<double>(m_frontier) - minLag;
        double loCap = static_cast<double>(m_oldest) + minLag;
        if (loCap > hiCap) loCap = hiCap;
        bool starvedFwd = false, starvedBwd = false;
        if (newHead > hiCap) {
            newHead = hiCap; starvedFwd = (m_rate > 0.0f);
            if (ph == Scratch && m_scratchVel > 0.0) {
                m_scratchVel = 0.0; m_scratchTargetPos = m_scratchServoPos;
            }
        } else if (newHead < loCap) {
            newHead = loCap; starvedBwd = (m_rate < 0.0f);
            if (ph == Scratch && m_scratchVel < 0.0) {
                m_scratchVel = 0.0; m_scratchTargetPos = m_scratchServoPos;
            }
        }
        m_headAbs = newHead;

        // Frontier trim during a BACKWARD scratch: the head is pulling
        // away from the frozen frontier, so the ahead region balloons
        // with stale decode-ahead and starves the backfill of ring
        // space. Cap the ahead window so the ring stays mostly history.
        if (ph == Scratch) {
            double ahead = static_cast<double>(m_frontier) - m_headAbs;
            double maxAhead = kAheadTargetSec * m_fs + 0.5 * m_fs;
            if (ahead > maxAhead)
                m_frontier = static_cast<int64_t>(
                    std::llround(m_headAbs + maxAhead));
        }

        if (starvedFwd) {
            l = 0.0f; r = 0.0f; m_scratchGain = 0.0f; publishRoom(); return;
        }
        (void)starvedBwd;

        // Read the head. 4-point Catmull-Rom; near an edge or thin window
        // fall back to nearest sample (neighbours may be invalid).
        {
            int i1 = ringIndex(static_cast<int64_t>(std::floor(m_headAbs)));
            double span = static_cast<double>(m_frontier - m_oldest);
            double distHi = static_cast<double>(m_frontier) - m_headAbs;
            double distLo = m_headAbs - static_cast<double>(m_oldest);
            if (span < 8.0 || distHi < 2.0 || distLo < 2.0) {
                l = m_ringL[i1]; r = m_ringR[i1];
            } else {
                int64_t f = static_cast<int64_t>(std::floor(m_headAbs));
                float t = static_cast<float>(m_headAbs - static_cast<double>(f));
                int i0 = ringIndex(f - 1), i2 = ringIndex(f + 1), i3 = ringIndex(f + 2);
                auto cm = [t](float p0, float p1, float p2, float p3) {
                    float a = 0.5f * (3.0f * (p1 - p2) - p0 + p3);
                    float b = p0 + 2.0f * p2 - 0.5f * (5.0f * p1 + p3);
                    float c = 0.5f * (p2 - p0);
                    return ((a * t + b) * t + c) * t + p1;
                };
                l = cm(m_ringL[i0], m_ringL[i1], m_ringL[i2], m_ringL[i3]);
                r = cm(m_ringR[i0], m_ringR[i1], m_ringR[i2], m_ringR[i3]);
            }
        }

        if (ph == Scratch) {
            float g = std::fabs(m_rate) * 50.0f;
            if (g > 1.0f) g = 1.0f;
            m_scratchGain += (g - m_scratchGain) * m_gainCoef;
            l *= m_scratchGain; r *= m_scratchGain;
        } else {
            m_scratchGain = 1.0f;
        }
        publishRoom();
    }

    // Paulstretch adapter: pass-through (see v2 note - the 1:1 feed can
    // never build the decode-ahead window, so scratching a time-stretch
    // is a graceful no-op rather than silence).
    void processStereo(float &l, float &r) { (void)l; (void)r; }

private:
    static constexpr double kAheadTargetSec = 1.0;   // forward window (scratching)
    // Transparent-ARMED decode-ahead: tiny so live pitch/speed/reverb stay
    // ~real-time while the vinyl popup is open but no scratch is active.
    static constexpr double kArmedAheadSec  = 0.20;  // ~200 ms latency armed
    static constexpr double kBackTargetSec  = 4.0;   // backfill keeps this behind
    static constexpr int    kGuard = 4800;
    static constexpr double kMinAheadSamples = 8.0;

    int N() const { return static_cast<int>(m_ringL.size()); }
    int ringIndex(int64_t abs) const {
        int nN = N();
        int64_t m = abs % nN;
        if (m < 0) m += nN;
        return static_cast<int>(m);
    }
    void publishRoom() {
        double lag  = static_cast<double>(m_frontier) - m_headAbs;
        double back = m_headAbs - static_cast<double>(m_oldest);
        if (lag < 0)  lag = 0;
        if (back < 0) back = 0;
        m_lagSec.store(static_cast<float>(lag / m_fs), std::memory_order_relaxed);
        m_backSec.store(static_cast<float>(back / m_fs), std::memory_order_relaxed);
        m_headDispSec.store(
            static_cast<float>((m_headAbs - m_headStartAbs) / m_fs),
            std::memory_order_relaxed);
    }

    void ensureRing() {
        if (!m_ringL.empty()) return;
        int n = static_cast<int>(10.0 * m_fs);
        m_ringL.assign(n, 0.0f);
        m_ringR.assign(n, 0.0f);
        m_frontier = m_oldest = 0;
        m_headAbs = 0.0;
    }

    void applyRestart() {
        if (!m_restart.exchange(false, std::memory_order_relaxed)) return;
        m_frontier = m_oldest = 0;
        m_headAbs = 0.0;
        m_scratchTargetPos = m_scratchServoPos = m_scratchVel = 0.0;
        m_scratchPendingFP.store(0, std::memory_order_relaxed);
        m_lagSec.store(0.0f, std::memory_order_relaxed);
        m_backSec.store(0.0f, std::memory_order_relaxed);
    }

    std::vector<float> m_ringL, m_ringR;
    double m_fs = 48000.0;
    double m_servoW = 6.283185307179586 * 22.0 / 48000.0;
    std::atomic<int>     m_phase{Idle};
    std::atomic<bool>    m_armed{false};
    std::atomic<bool>    m_restart{false};
    std::atomic<int64_t> m_scratchPendingFP{0};
    std::atomic<bool>    m_scratchResync{false};
    std::atomic<float>   m_lagSec{0.0f};
    std::atomic<float>   m_backSec{0.0f};
    std::atomic<float>   m_headDispSec{0.0f};      // head motion since grab
    std::atomic<bool>    m_captureHeadStart{false};
    std::atomic<double>  m_oldestFileSec{0.0};
    double  m_scratchTargetPos = 0.0;
    double  m_scratchServoPos  = 0.0;
    double  m_scratchVel       = 0.0;
    float   m_rate = 1.0f;
    float   m_step = 0.0f;
    float   m_scratchGain = 1.0f;
    float   m_gainCoef = 0.004f;
    int64_t m_frontier = 0;   // absolute index of next forward write
    int64_t m_oldest   = 0;   // absolute index of oldest valid sample
    double  m_headAbs  = 0.0;  // absolute head position (fractional)
    double  m_headStartAbs = 0.0;  // head position at scratchBegin
};
