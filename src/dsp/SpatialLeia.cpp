#include "SpatialLeia.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QString>
#include <QtGlobal>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  #define SPATIALLEIA_X86 1
  #include <xmmintrin.h>
#else
  #define SPATIALLEIA_X86 0
#endif

namespace {
struct ScopedFtzDaz {
#if SPATIALLEIA_X86
    unsigned int saved;
    ScopedFtzDaz() { saved = _mm_getcsr(); _mm_setcsr(saved | 0x8040); }
    ~ScopedFtzDaz() { _mm_setcsr(saved); }
#else
    ScopedFtzDaz() {}
    ~ScopedFtzDaz() {}
#endif
};
}

namespace {

// The Leia HRTF dataset is shipped inside the plugin as a Qt resource
// (qtres.qrc, prefix "leia"). libmysofa needs a real file path, so the
// resource is extracted once to a per-user cache directory. Returns an
// empty string on failure.
QString resolveSofaPath()
{
    static QMutex mutex;
    static QString cached;
    QMutexLocker lock(&mutex);

    // Custom-SOFA override: user can point the Leia engine at any
    // personalised HRTF dataset via QSettings("GameBaiters","Soundboard")
    // key "leia/custom_sofa_path". When set and the file exists, it
    // wins over the bundled default. Re-read on every resolve (the
    // call only happens inside ensureInit, i.e. on the GUI thread at
    // engine bring-up, so the QSettings I/O is off the audio path) -
    // the previous once-per-process cache meant changing the key
    // silently required a full TS3 restart.
    {
        QSettings sset(QStringLiteral("GameBaiters"),
                       QStringLiteral("Soundboard"));
        QString custom = sset.value(QStringLiteral("leia/custom_sofa_path"))
                             .toString();
        if (!custom.isEmpty() && QFile::exists(custom))
            return custom;
    }

    if (!cached.isEmpty())
        return cached;

    QString cacheDir = QStandardPaths::writableLocation(
        QStandardPaths::CacheLocation);
    if (cacheDir.isEmpty())
        cacheDir = QDir::tempPath();
    QDir().mkpath(cacheDir + "/leia");

    // Version the filename so a future dataset swap forces a re-extract.
    const QString target = cacheDir + "/leia/default_v1.sofa";

    QFile res(":/leia/default.sofa");
    if (!res.exists()) {
        qWarning("[Leia] SOFA resource missing");
        return QString();
    }

    QFileInfo ti(target);
    if (ti.exists() && ti.size() == res.size()) {
        cached = target;
        return cached;
    }

    if (!res.open(QIODevice::ReadOnly)) {
        qWarning("[Leia] cannot open SOFA resource");
        return QString();
    }
    QByteArray data = res.readAll();
    res.close();

    QFile out(target);
    if (!out.open(QIODevice::WriteOnly)) {
        qWarning("[Leia] cannot write SOFA cache file");
        return QString();
    }
    out.write(data);
    out.close();

    cached = target;
    return cached;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace

SpatialLeia::SpatialLeia() = default;

bool SpatialLeia::ensureInit(double sampleRate)
{
    if (sampleRate <= 0.0) sampleRate = 48000.0;

    if (m_ready && std::abs(sampleRate - m_fs) < 0.5)
        return true;

    m_ready = false;
    m_fs    = sampleRate;

    QString sofa = resolveSofaPath();
    if (sofa.isEmpty())
        return false;

    bool ok = false;
    try {
        // QFile gives a UTF-8 path; libmysofa fopen()s it. Non-ASCII
        // install paths may fail here - that is fine, we fall back to
        // the Classic engine.
        ok = m_engine.init(sofa.toUtf8().constData(),
                           static_cast<float>(sampleRate), kBlock);
    } catch (...) {
        ok = false;
    }
    if (!ok)
        return false;

    m_inBuf .assign(static_cast<size_t>(kBlock) * 2, 0.0f);
    m_outBuf.assign(static_cast<size_t>(kBlock) * 2, 0.0f);
    m_dryBuf.assign(static_cast<size_t>(kBlock) * 2, 0.0f);
    m_inFill   = 0;
    m_outPos   = 0;
    m_outValid = false;
    m_dryPos   = 0;
    m_engage   = 0.0f;
    // Engage ramp raised from 50 ms to 120 ms so the initial dry->wet
    // handoff is smoother on percussive / bass-heavy transients (the
    // audible 5-10 ms jump at 50 ms was reading as a light thump the
    // user characterised as "compressed bass start-up").
    m_engageInc = 1.0f / static_cast<float>(std::max(1.0, sampleRate * 0.12));

    // Auto makeup gain so Leia's measured HRIRs play at sane loudness.
    float makeup = 1.0f;
    try {
        makeup = calibrateMakeup();
    } catch (...) {
        makeup = 1.0f;
    }
    m_engine.reset();                 // wipe calibration state
    m_engine.setGain(makeup);

    // Release store: every buffer above is now allocated, so the audio
    // thread that observes ready()==true also sees a fully built state.
    m_ready.store(true, std::memory_order_release);
    return true;
}

float SpatialLeia::calibrateMakeup()
{
    // FTZ/DAZ to keep probe out of denormal land - subnormals get
    // microcoded to ~0 by MSVC, inflating the inRms/outRms ratio
    // and producing a too-large makeup gain that later clips Leia.
    ScopedFtzDaz ftz;

    // Direct path only, source straight ahead.
    m_engine.setReflectionEnable(false);
    m_engine.setWidth(0.0f);
    m_engine.setClarity(100.0f);
    m_engine.setAzimuth(0.0f);
    m_engine.setElevation(0.0f);
    m_engine.setGain(1.0f);

    const int kProbeBlocks = 14;
    const int kSkipBlocks  = 4;       // discard FFT/HRIR warm-up
    std::vector<float> in(static_cast<size_t>(kBlock) * 2, 0.0f);
    std::vector<float> out(static_cast<size_t>(kBlock) * 2, 0.0f);

    // Deterministic pseudo-random probe (correlated L/R = centred source).
    unsigned int rng = 0x9E3779B9u;
    auto nextNoise = [&rng]() -> float {
        rng = rng * 1664525u + 1013904223u;
        return (static_cast<float>(rng >> 8) / 8388608.0f - 1.0f) * 0.25f;
    };

    double inSq = 0.0, outSq = 0.0;
    long   count = 0;
    for (int b = 0; b < kProbeBlocks; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            float n = nextNoise();
            in[i * 2 + 0] = n;
            in[i * 2 + 1] = n;
        }
        m_engine.processBlock(in.data(), out.data(), kBlock);
        if (b < kSkipBlocks) continue;
        for (int i = 0; i < kBlock; ++i) {
            float di = 0.5f * (in[i * 2 + 0]  + in[i * 2 + 1]);
            float dO = 0.5f * (out[i * 2 + 0] + out[i * 2 + 1]);
            inSq  += static_cast<double>(di) * di;
            outSq += static_cast<double>(dO) * dO;
            ++count;
        }
    }
    if (count <= 0 || outSq < 1e-20) return 1.0f;

    double inRms  = std::sqrt(inSq  / count);
    double outRms = std::sqrt(outSq / count);
    if (outRms < 1e-12) return 1.0f;

    float makeup = static_cast<float>(inRms / outRms);
    // Headroom factor. The post-engine memoryless saturator now sits at
    // T=1.8 (up from 1.2), so the makeup no longer needs the aggressive
    // 0.7 pull-down that was in place when the saturator engaged at
    // T=1.2. 0.85x keeps ~1.4 dB of margin against the probe worst-case
    // while giving back ~1.7 dB of overall loudness the previous factor
    // was taking away. Under normal listening conditions the saturator
    // stays entirely bypassed with this pairing.
    makeup *= 0.85f;
    return clampf(makeup, 0.2f, 50.0f);
}

void SpatialLeia::setSampleRate(double sr)
{
    if (sr <= 0.0) return;
    if (m_ready && std::abs(sr - m_fs) >= 0.5) {
        // Force a re-init on the next ensureInit() call (GUI thread).
        m_ready = false;
    }
    m_fs = sr;
}

void SpatialLeia::reset()
{
    m_engine.reset();
    if (!m_inBuf.empty())  std::fill(m_inBuf.begin(),  m_inBuf.end(),  0.0f);
    if (!m_outBuf.empty()) std::fill(m_outBuf.begin(), m_outBuf.end(), 0.0f);
    if (!m_dryBuf.empty()) std::fill(m_dryBuf.begin(), m_dryBuf.end(), 0.0f);
    m_inFill   = 0;
    m_outPos   = 0;
    m_outValid = false;
    m_dryPos   = 0;
    m_engage   = 0.0f;
}

void SpatialLeia::setDirection(float azimuthDeg, float elevationDeg)
{
    // Wrap azimuth into -180..180, clamp elevation.
    while (azimuthDeg >  180.0f) azimuthDeg -= 360.0f;
    while (azimuthDeg < -180.0f) azimuthDeg += 360.0f;
    m_engine.setAzimuth(azimuthDeg);
    m_engine.setElevation(clampf(elevationDeg, -90.0f, 90.0f));
}

void SpatialLeia::setReflections(bool on, float levelDb, float roomSizeM,
                                  int roomType)
{
    m_engine.setReflectionEnable(on);
    m_engine.setReflectionLevel(clampf(levelDb, -25.0f, 20.0f));
    m_engine.setRoomSize(clampf(roomSizeM, 7.0f, 50.0f));
    m_engine.setRoomType(roomType);
}

void SpatialLeia::setClarity(float pct) { m_engine.setClarity(clampf(pct, 0.0f, 100.0f)); }
void SpatialLeia::setWidth(float pct)   { m_engine.setWidth(clampf(pct, 0.0f, 100.0f)); }
void SpatialLeia::setMix(float mix)     { m_mix = clampf(mix, 0.0f, 1.0f); }

void SpatialLeia::process(float &l, float &r)
{
    if (!m_ready) return;   // passthrough - caller handles fallback

    // FTZ/DAZ for the per-sample wet/dry crossfade path. Without this
    // a denormal sneaks into the dry delay ring or the m_engage ramp
    // multiplier and the audio thread spikes CPU.
    ScopedFtzDaz ftz;

    // Dry delay ring: read the kBlock-old sample, then overwrite with
    // the current input. Keeps the dry path aligned with the wet path.
    float dryL = m_dryBuf[m_dryPos * 2 + 0];
    float dryR = m_dryBuf[m_dryPos * 2 + 1];
    m_dryBuf[m_dryPos * 2 + 0] = l;
    m_dryBuf[m_dryPos * 2 + 1] = r;
    m_dryPos = (m_dryPos + 1) % kBlock;

    // Process block, then read, then write input. The earlier order
    // (write, then process, then read) silently dropped outBuf[kBlock-1]
    // every block - audible as a ~187 Hz frying buzz at 48 kHz.
    if (m_inFill >= kBlock) {
        m_engine.processBlock(m_inBuf.data(), m_outBuf.data(), kBlock);
        m_inFill   = 0;
        m_outPos   = 0;
        m_outValid = true;
    }

    if (!m_outValid) {
        // Pre-roll: first kBlock samples have no processed output yet.
        // Stash the input and pass the (delayed) dry through so
        // playback never starts with a gap. Also tick the engage ramp
        // here - without it, the ramp sat at 0 throughout pre-roll
        // then suddenly jumped to its first non-zero increment when the
        // very first wet block arrived, producing a click on hot input.
        // Ramping during pre-roll means by the time wet arrives the ramp
        // has already covered ~5 ms of its 50 ms span, and the next
        // crossfade is smooth.
        m_inBuf[m_inFill * 2 + 0] = l;
        m_inBuf[m_inFill * 2 + 1] = r;
        ++m_inFill;
        if (m_engage < 1.0f) {
            m_engage += m_engageInc;
            if (m_engage > 1.0f) m_engage = 1.0f;
        }
        l = dryL;
        r = dryR;
        return;
    }

    float wetL = m_outBuf[m_outPos * 2 + 0];
    float wetR = m_outBuf[m_outPos * 2 + 1];
    m_outPos = (m_outPos + 1) % kBlock;

    // Accumulate the current input for the NEXT block.
    m_inBuf[m_inFill * 2 + 0] = l;
    m_inBuf[m_inFill * 2 + 1] = r;
    ++m_inFill;

    // Click-free engagement ramp from full dry to the wet/dry mix.
    if (m_engage < 1.0f) {
        m_engage += m_engageInc;
        if (m_engage > 1.0f) m_engage = 1.0f;
    }

    float mixedL = m_mix * wetL + (1.0f - m_mix) * dryL;
    float mixedR = m_mix * wetR + (1.0f - m_mix) * dryR;

    l = m_engage * mixedL + (1.0f - m_engage) * dryL;
    r = m_engage * mixedR + (1.0f - m_engage) * dryR;
}
