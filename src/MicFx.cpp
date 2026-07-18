#include "MicFx.h"
#include "MicAmbience.h"

#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include <algorithm>

namespace {
constexpr const char *kOrg = "GameBaiters";
constexpr const char *kApp = "Soundboard";

inline short clampShort(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<short>(v);
}
} // namespace

MicFx &MicFx::instance()
{
    static MicFx s_instance;
    return s_instance;
}

MicFx::MicFx()
{
    m_dsp.setSampleRate(48000.0);
    m_pitchL.setSampleRate(48000.0);
    m_pitchR.setSampleRate(48000.0);
    m_pitchL.setWindowMs(45.0f);
    m_pitchR.setWindowMs(45.0f);
}

SandboxState MicFx::sanitize(const SandboxState &in)
{
    SandboxState s = in;
    // The mic chain runs on a LIVE stream. Spatial (including the Leia
    // 8D engine) IS allowed - your voice orbits the listeners; the
    // HRTF convolution adds only ~5 ms, well inside the voice budget.
    // Still curated out: Delay (long echoes on live speech), Paul-
    // stretch (unbounded latency accumulation), Binaural (a tone
    // generator, not a voice effect).
    s.delayEnabled    = false;
    s.stretchEnabled  = false;
    s.binauralEnabled = false;
    // Sidechain routing references Sampler slots - meaningless here.
    s.compSidechainSlot    = -1;
    s.gateSidechainSlot    = -1;
    s.deesserSidechainSlot = -1;
    s.duckSource           = false;
    // The chain master must be on for the stages to run.
    s.enabled = true;
    return s;
}

void MicFx::setEnabled(bool on)
{
    if (!m_featureEnabled.load(std::memory_order_relaxed))
        on = false;
    bool prev = m_enabled.exchange(on, std::memory_order_relaxed);
    if (prev == on) return;
    if (on) {
        // Fresh engage: clear stale filter tails + monitor backlog.
        std::lock_guard<std::mutex> g(m_dspMutex);
        m_dsp.reset();
        m_pitchL.reset();
        m_pitchR.reset();
        m_monR.store(m_monW.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    } else {
        m_levelIn.store(0.0f, std::memory_order_relaxed);
        m_levelOut.store(0.0f, std::memory_order_relaxed);
    }
    saveSettings();
    emit enabledChanged(on);
}

void MicFx::setFeatureEnabled(bool on)
{
    m_featureEnabled.store(on, std::memory_order_relaxed);
    if (!on && enabled())
        setEnabled(false);
}

void MicFx::setPitchSemitones(float st)
{
    st = std::min(12.0f, std::max(-12.0f, st));
    m_pitchSemitones = st;
    m_pitchRatio.store(std::pow(2.0f, st / 12.0f), std::memory_order_relaxed);
    saveSettings();
}

bool MicFx::setAmbienceFile(const QString &path)
{
    auto buf = std::make_shared<std::vector<float>>(
        MicAmbience::loadCustomFile(path, 48000));
    if (buf->empty()) return false;
    m_ambienceId = kAmbienceCustom;
    m_ambCustomPath = path;
    {
        std::lock_guard<std::mutex> g(m_dspMutex);
        m_ambLoop = buf;
        m_ambPos = 0;
    }
    saveSettings();
    return true;
}

void MicFx::setAmbience(int id)
{
    if (id >= MicAmbience::count()) id = -1;
    if (id < 0) id = -1;
    m_ambienceId = id;
    m_ambCustomPath.clear();
    // Generation happens OUTSIDE the mutex (GUI thread, a few ms); the
    // capture thread keeps passing the mic through while we build.
    std::shared_ptr<const std::vector<float>> loop;
    if (id >= 0) {
        auto buf = std::make_shared<std::vector<float>>(
            MicAmbience::generate(id, 48000));
        if (!buf->empty()) loop = buf;
    }
    {
        std::lock_guard<std::mutex> g(m_dspMutex);
        m_ambLoop = loop;
        m_ambPos = 0;
    }
    saveSettings();
}

void MicFx::setAmbienceVolume(float v)
{
    m_ambGain.store(std::min(1.0f, std::max(0.0f, v)),
                    std::memory_order_relaxed);
    saveSettings();
}

void MicFx::setGainDb(float db)
{
    db = std::min(20.0f, std::max(-20.0f, db));
    m_gainDb = db;
    m_gainLin.store(std::pow(10.0f, db / 20.0f), std::memory_order_relaxed);
    saveSettings();
}

void MicFx::setSandboxState(const SandboxState &s)
{
    SandboxState clean = sanitize(s);
    m_state = clean;

    // Heavy Leia bring-up (SOFA load + FFT plan + calibration, ~1 s the
    // first time) runs OUTSIDE m_dspMutex - ensureInit is idempotent
    // and designed to run on the GUI thread while the audio thread
    // keeps processing (same contract as Sampler::setSlotSandboxState
    // phase 2). While it initializes, the Spatial stage silently uses
    // the Classic fallback, then flips to Leia on the next block.
    bool wantsLeia3D = (clean.spatialEngine == SandboxState::Engine_Leia) &&
                       (clean.spatialMode == SandboxState::Spatial_3DManual ||
                        clean.spatialMode == SandboxState::Spatial_3DRotate ||
                        clean.spatialMode == SandboxState::Spatial_8DPreset);
    if (wantsLeia3D)
        m_dsp.prepareLeia(48000.0);

    {
        std::lock_guard<std::mutex> g(m_dspMutex);
        // Conv-reverb IR build still happens under the mutex; the
        // capture thread try-locks, so while we hold it the mic passes
        // through clean - never blocks, never glitches.
        m_dsp.prepareConvReverb(clean);
        m_dsp.applyState(clean);
    }
    saveSettings();
}

bool MicFx::processCapture(short *samples, int sampleCount, int channels)
{
    if (!m_enabled.load(std::memory_order_relaxed)) return false;
    if (!m_featureEnabled.load(std::memory_order_relaxed)) return false;
    if (sampleCount <= 0 || channels < 1) return false;

    // Never block TS3's capture thread: state push in progress = this
    // block passes through clean.
    std::unique_lock<std::mutex> g(m_dspMutex, std::try_to_lock);
    if (!g.owns_lock()) return false;

    bool modified = false;
    try {
        constexpr float kInv = 1.0f / 32768.0f;

        // Mic gain boost FIRST - it is the effective microphone volume,
        // so everything downstream (meter, pitch, chain) sees it.
        {
            const float g = m_gainLin.load(std::memory_order_relaxed);
            if (std::fabs(g - 1.0f) > 0.001f) {
                for (int i = 0; i < sampleCount * channels; ++i)
                    samples[i] = clampShort(
                        static_cast<int>(samples[i] * g));
                modified = true;
            }
        }

        // Input level (pre-FX) for the meter.
        float peakIn = 0.0f;
        for (int i = 0; i < sampleCount * channels; ++i) {
            float a = std::fabs(samples[i] * kInv);
            if (a > peakIn) peakIn = a;
        }
        m_levelIn.store(peakIn, std::memory_order_relaxed);

        // Live 1:1 pitch shift in front of the chain.
        float ratio = m_pitchRatio.load(std::memory_order_relaxed);
        if (std::fabs(ratio - 1.0f) > 0.0005f) {
            m_pitchL.setRatio(ratio);
            m_pitchR.setRatio(ratio);
            for (int i = 0; i < sampleCount; ++i) {
                if (channels >= 2) {
                    float l = samples[i * channels + 0] * kInv;
                    float r = samples[i * channels + 1] * kInv;
                    l = m_pitchL.process(l);
                    r = m_pitchR.process(r);
                    samples[i * channels + 0] =
                        clampShort(static_cast<int>(l * 32767.0f));
                    samples[i * channels + 1] =
                        clampShort(static_cast<int>(r * 32767.0f));
                } else {
                    float m = samples[i] * kInv;
                    m = m_pitchL.process(m);
                    samples[i] = clampShort(static_cast<int>(m * 32767.0f));
                }
            }
            modified = true;
        }

        // Sandbox chain (capture path - the server-bound stream).
        if (m_dsp.isActive()) {
            float pl = 0.0f, pr = 0.0f;
            m_dsp.process(samples, sampleCount, channels, pl, pr,
                          /*isCapture=*/true);
            m_levelOut.store(std::max(pl, pr), std::memory_order_relaxed);
            modified = true;
        } else {
            m_levelOut.store(peakIn, std::memory_order_relaxed);
        }

        // Background ambience: mixed AFTER the chain so voice effects
        // never distort the scene bed. Mixing marks the buffer edited,
        // so the ambience transmits whenever the mic transmits.
        if (m_ambLoop && !m_ambLoop->empty()) {
            const float g = m_ambGain.load(std::memory_order_relaxed);
            if (g > 0.001f) {
                const std::vector<float> &loop = *m_ambLoop;
                const size_t n = loop.size();
                for (int i = 0; i < sampleCount; ++i) {
                    const int a = static_cast<int>(
                        loop[m_ambPos] * g * 32767.0f);
                    if (++m_ambPos >= n) m_ambPos = 0;
                    for (int c = 0; c < channels; ++c) {
                        short *dst = &samples[i * channels + c];
                        *dst = clampShort(int(*dst) + a);
                    }
                }
                modified = true;
            }
        }

        // Monitor feed: push the PROCESSED stream into the SPSC ring.
        if (modified && m_monitor.load(std::memory_order_relaxed)) {
            int w = m_monW.load(std::memory_order_relaxed);
            for (int i = 0; i < sampleCount; ++i) {
                short l = (channels >= 2) ? samples[i * channels + 0]
                                          : samples[i];
                short r = (channels >= 2) ? samples[i * channels + 1]
                                          : samples[i];
                m_monRing[(w % kMonFrames) * 2 + 0] = l;
                m_monRing[(w % kMonFrames) * 2 + 1] = r;
                ++w;
            }
            m_monW.store(w, std::memory_order_release);
        }
    } catch (...) {
        // Auto-bypass: a DSP fault must never mute or garble the mic.
        // The buffer may be partially processed - acceptable for one
        // 20 ms block; the flag below keeps TS3 consistent.
        return modified;
    }
    return modified;
}

bool MicFx::mixMonitor(short *samples, int sampleCount, int channels,
                       int ciLeft, int ciRight,
                       bool overwriteL, bool overwriteR)
{
    if (!m_monitor.load(std::memory_order_relaxed)) return false;
    if (!m_enabled.load(std::memory_order_relaxed)) return false;
    if (channels < 1) return false;
    if (ciLeft < 0 || ciLeft >= channels) ciLeft = 0;
    if (ciRight < 0 || ciRight >= channels) ciRight = channels >= 2 ? 1 : 0;

    int r = m_monR.load(std::memory_order_relaxed);
    int w = m_monW.load(std::memory_order_acquire);
    int avail = w - r;
    if (avail <= 0) return false;
    // Drop backlog beyond ~100 ms so the monitor stays near-live even
    // if the playback callback stalls for a moment.
    if (avail > 4800) { r = w - 4800; avail = 4800; }

    int frames = std::min(avail, sampleCount);
    for (int i = 0; i < frames; ++i) {
        short l  = m_monRing[(r % kMonFrames) * 2 + 0];
        short rr = m_monRing[(r % kMonFrames) * 2 + 1];
        ++r;
        short *dstL = &samples[i * channels + ciLeft];
        short *dstR = &samples[i * channels + ciRight];
        *dstL = overwriteL ? l  : clampShort(*dstL + l);
        if (ciRight != ciLeft)
            *dstR = overwriteR ? rr : clampShort(*dstR + rr);
    }
    // Channels marked "overwrite" must be defined for the WHOLE block.
    for (int i = frames; i < sampleCount; ++i) {
        if (overwriteL) samples[i * channels + ciLeft] = 0;
        if (overwriteR && ciRight != ciLeft) samples[i * channels + ciRight] = 0;
    }
    m_monR.store(r, std::memory_order_release);
    return true;
}

void MicFx::loadSettings()
{
    QSettings st(kOrg, kApp);
    m_settingsLoaded = false;   // suppress saves while loading

    float pitch = st.value("micfx/pitch", 0.0f).toFloat();
    m_pitchSemitones = std::min(12.0f, std::max(-12.0f, pitch));
    m_pitchRatio.store(std::pow(2.0f, m_pitchSemitones / 12.0f),
                       std::memory_order_relaxed);
    m_gainDb = std::min(20.0f, std::max(-20.0f,
        st.value("micfx/gain_db", 0.0f).toFloat()));
    m_gainLin.store(std::pow(10.0f, m_gainDb / 20.0f),
                    std::memory_order_relaxed);
    m_monitor.store(st.value("micfx/monitor", false).toBool(),
                    std::memory_order_relaxed);
    m_ambGain.store(qBound(0.0f,
        st.value("micfx/ambience_vol", 0.35f).toFloat(), 1.0f),
        std::memory_order_relaxed);
    {
        int ambId = st.value("micfx/ambience", -1).toInt();
        if (ambId >= MicAmbience::count()) ambId = -1;
        QString ambFile = st.value("micfx/ambience_file").toString();
        std::shared_ptr<const std::vector<float>> loop;
        if (ambId == kAmbienceCustom && !ambFile.isEmpty()) {
            auto buf = std::make_shared<std::vector<float>>(
                MicAmbience::loadCustomFile(ambFile, 48000));
            if (!buf->empty()) {
                loop = buf;
                m_ambCustomPath = ambFile;
            } else {
                ambId = -1;   // file gone / undecodable -> none
            }
        } else if (ambId == kAmbienceCustom) {
            ambId = -1;
        } else if (ambId >= 0) {
            auto buf = std::make_shared<std::vector<float>>(
                MicAmbience::generate(ambId, 48000));
            if (!buf->empty()) loop = buf;
        }
        m_ambienceId = ambId;
        std::lock_guard<std::mutex> g(m_dspMutex);
        m_ambLoop = loop;
        m_ambPos = 0;
    }

    QByteArray json = QByteArray::fromBase64(
        st.value("micfx/state").toByteArray());
    if (!json.isEmpty()) {
        QJsonDocument doc = QJsonDocument::fromJson(json);
        if (doc.isObject())
            m_state = sanitize(SandboxState::fromJson(doc.object()));
    } else {
        m_state = sanitize(SandboxState());
    }
    // Same outside-the-mutex Leia bring-up as setSandboxState, for a
    // restored 8D/3D mic chain.
    if ((m_state.spatialEngine == SandboxState::Engine_Leia) &&
        (m_state.spatialMode == SandboxState::Spatial_3DManual ||
         m_state.spatialMode == SandboxState::Spatial_3DRotate ||
         m_state.spatialMode == SandboxState::Spatial_8DPreset))
        m_dsp.prepareLeia(48000.0);
    {
        std::lock_guard<std::mutex> g(m_dspMutex);
        m_dsp.prepareConvReverb(m_state);
        m_dsp.applyState(m_state);
    }

    m_settingsLoaded = true;

    // Master toggle LAST (user decision: remember last state). Feature
    // gate is pushed by the settings wiring before this runs at init.
    bool wasOn = st.value("micfx/enabled", false).toBool();
    if (wasOn) setEnabled(true);
}

void MicFx::saveSettings()
{
    if (!m_settingsLoaded) return;
    QSettings st(kOrg, kApp);
    st.setValue("micfx/enabled", enabled());
    st.setValue("micfx/pitch", m_pitchSemitones);
    st.setValue("micfx/gain_db", m_gainDb);
    st.setValue("micfx/monitor", monitor());
    st.setValue("micfx/ambience", m_ambienceId);
    st.setValue("micfx/ambience_file", m_ambCustomPath);
    st.setValue("micfx/ambience_vol",
                m_ambGain.load(std::memory_order_relaxed));
    QJsonDocument doc(m_state.toJson());
    st.setValue("micfx/state",
                doc.toJson(QJsonDocument::Compact).toBase64());
}

void MicFx::applyPreset(int index)
{
    QVector<Preset> presets = builtinPresets();
    if (index < 0 || index >= presets.size()) return;
    const Preset &p = presets[index];
    setSandboxState(p.state);
    setPitchSemitones(p.pitchSemitones);
}

QVector<MicFx::Preset> MicFx::builtinPresets()
{
    QVector<Preset> out;
    auto base = []() {
        SandboxState s;
        s.enabled = true;
        return s;
    };

    {   // Predefinito: clean voice - every effect off, pitch 0. One
        // click back to normal after any preset without touching the
        // master toggle.
        Preset p; p.name = QStringLiteral("Predefinito"); p.pitchSemitones = 0.0f;
        p.state = base(); out.push_back(p);
    }

    // ---- Pack classico ----
    {   // Robot: ring mod + bitcrush
        Preset p; p.name = QStringLiteral("Robot"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxRingEnabled = true; s.vfxRingFreq = 42.0f; s.vfxRingMix = 0.85f;
        s.bitcrusherEnabled = true; s.bitcrusherBitDepth = 7;
        s.bitcrusherRate = 14000.0f;
        p.state = s; out.push_back(p);
    }
    {   // Chipmunk: pitch up
        Preset p; p.name = QStringLiteral("Chipmunk"); p.pitchSemitones = 7.0f;
        p.state = base(); out.push_back(p);
    }
    {   // Demone: pitch down + dark ambience + weight
        Preset p; p.name = QStringLiteral("Demone"); p.pitchSemitones = -6.0f;
        SandboxState s = base();
        s.reverbWet = 0.22f;
        s.bassEnhEnabled = true; s.bassEnhFreq = 150.0f;
        s.bassEnhDrive = 4.0f; s.bassEnhMix = 0.5f;
        p.state = s; out.push_back(p);
    }
    {   // Telefono: band-limit + saturation
        Preset p; p.name = QStringLiteral("Telefono"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.eqEnabled = true;
        static const float kTel[16] = { -12, -12, -12, -12, -8, -3, 0, 2,
                                        3, 2, -2, -8, -12, -12, -12, -12 };
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kTel[i];
        s.saturatorEnabled = true; s.saturatorDrive = 3.0f;
        s.saturatorMix = 0.45f; s.saturatorTone = 4000.0f;
        p.state = s; out.push_back(p);
    }
    {   // Radio militare: telefono + gate + crunch
        Preset p; p.name = QStringLiteral("Radio militare"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.eqEnabled = true;
        static const float kRad[16] = { -12, -12, -12, -12, -10, -5, 0, 3,
                                        4, 3, -1, -6, -12, -12, -12, -12 };
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kRad[i];
        s.saturatorEnabled = true; s.saturatorDrive = 5.0f;
        s.saturatorMix = 0.65f; s.saturatorTone = 3200.0f;
        s.saturatorMode = 3;
        s.gateEnabled = true; s.gateThresholdDb = -34.0f;
        s.gateRangeDb = -70.0f; s.gateReleaseMs = 90.0f;
        s.bitcrusherEnabled = true; s.bitcrusherBitDepth = 10;
        s.bitcrusherRate = 16000.0f;
        p.state = s; out.push_back(p);
    }

    // ---- Pack creativo ----
    {   // Alieno: hard autotune + formant warp + vibrato
        Preset p; p.name = QStringLiteral("Alieno"); p.pitchSemitones = 2.0f;
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxTuneEnabled = true; s.vfxTuneStrength = 1.0f;
        s.vfxTuneSpeedMs = 1.0f; s.vfxTuneScale = 0;
        s.vfxFormEnabled = true; s.vfxFormShift = 4.0f; s.vfxFormMix = 1.0f;
        s.vfxVibEnabled = true; s.vfxVibRate = 6.5f; s.vfxVibDepth = 0.35f;
        p.state = s; out.push_back(p);
    }
    {   // Caverna: big dark reverb
        Preset p; p.name = QStringLiteral("Caverna"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.reverbWet = 0.5f;
        p.state = s; out.push_back(p);
    }
    {   // Subacqueo: heavy high cut + slow chorus wobble
        Preset p; p.name = QStringLiteral("Subacqueo"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.eqEnabled = true;
        static const float kSub[16] = { 2, 2, 1, 0, 0, -2, -5, -8,
                                        -11, -12, -12, -12, -12, -12, -12, -12 };
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kSub[i];
        s.chorusEnabled = true; s.chorusRate = 0.35f; s.chorusDepth = 6.0f;
        s.chorusBaseDelay = 14.0f; s.chorusVoices = 2; s.chorusMix = 0.55f;
        p.state = s; out.push_back(p);
    }
    {   // Megafono: crunchy compressed presence
        Preset p; p.name = QStringLiteral("Megafono"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.eqEnabled = true;
        static const float kMeg[16] = { -10, -10, -8, -6, -3, 0, 2, 4,
                                        5, 5, 4, 2, -2, -6, -10, -12 };
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kMeg[i];
        s.compEnabled = true; s.compThresholdDb = -28.0f; s.compRatio = 8.0f;
        s.compAttackMs = 2.0f; s.compReleaseMs = 80.0f; s.compMakeupDb = 6.0f;
        s.saturatorEnabled = true; s.saturatorDrive = 6.0f;
        s.saturatorMix = 0.7f; s.saturatorTone = 5500.0f;
        s.vfxEnabled = true;
        s.vfxExcEnabled = true; s.vfxExcFreq = 2500.0f;
        s.vfxExcDrive = 4.0f; s.vfxExcMix = 0.4f;
        p.state = s; out.push_back(p);
    }

    // ---- Pack assurdo ----
    {   // Elio: formants up, pitch untouched (helium physics)
        Preset p; p.name = QStringLiteral("Elio"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxFormEnabled = true; s.vfxFormShift = 7.0f; s.vfxFormMix = 1.0f;
        p.state = s; out.push_back(p);
    }
    {   // Gigante: formants down + slight pitch down
        Preset p; p.name = QStringLiteral("Gigante"); p.pitchSemitones = -3.0f;
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxFormEnabled = true; s.vfxFormShift = -6.0f; s.vfxFormMix = 1.0f;
        p.state = s; out.push_back(p);
    }
    {   // Fantasma: shimmer tail + gate + subtle wobble
        Preset p; p.name = QStringLiteral("Fantasma"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.vfxEnabled = true;
        s.vfxShimEnabled = true; s.vfxShimMix = 0.45f;
        s.vfxShimFeedback = 0.6f; s.vfxShimPitch = 12; s.vfxShimDamp = 0.35f;
        s.vfxVibEnabled = true; s.vfxVibRate = 0.8f; s.vfxVibDepth = 0.25f;
        s.reverbWet = 0.35f;
        s.gateEnabled = true; s.gateThresholdDb = -38.0f;
        p.state = s; out.push_back(p);
    }
    {   // Vecchia radio AM: generation loss + slow tremolo + bandlimit
        Preset p; p.name = QStringLiteral("Vecchia radio AM"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.genLossEnabled = true; s.genLossGenerations = 180;
        s.vfxEnabled = true;
        s.vfxTremEnabled = true; s.vfxTremRate = 5.5f;
        s.vfxTremDepth = 0.3f; s.vfxTremShape = 0;
        s.eqEnabled = true;
        static const float kAm[16] = { -12, -12, -10, -6, -2, 0, 1, 1,
                                       0, -2, -6, -10, -12, -12, -12, -12 };
        for (int i = 0; i < 16; ++i) s.eqBandDb[i] = kAm[i];
        p.state = s; out.push_back(p);
    }
    {   // Voce 8D: Leia measured-HRTF orbit - your voice circles the
        // listeners' heads. Mirrors the channel-side 8D recipe.
        Preset p; p.name = QStringLiteral("Voce 8D"); p.pitchSemitones = 0.0f;
        SandboxState s = base();
        s.spatialMode    = SandboxState::Spatial_8DPreset;
        s.spatialEngine  = SandboxState::Engine_Leia;
        s.rotateRpm      = 10.0f;
        s.rotateRadiusM  = 1.5f;
        s.rotateElev     = 0.0f;
        s.stereoWidthDeg = 0.0f;
        s.spatialMix     = 0.9f;
        s.headSway       = true;
        s.leiaReflEnable = true;
        s.leiaReflLevel  = -8.0f;
        s.leiaRoomSize   = 10.0f;
        s.leiaRoomType   = 1;
        s.leiaClarity    = 100.0f;
        s.leiaWidth      = 35.0f;
        s.reverbWet      = 0.12f;
        p.state = s; out.push_back(p);
    }

    return out;
}
