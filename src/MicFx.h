#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "dsp/SandboxState.h"
#include "dsp/SlotDsp.h"
#include "dsp/PitchShiftGrain.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// Real-time microphone FX (V1/V2).
//
// Processes the user's OWN outgoing mic stream inside TS3's capture
// callback, BEFORE the soundboard mix-in. The chain is the same
// SlotDsp sandbox used by the channels, with a curated sanitization:
// Spatial, Delay, Paulstretch and Binaural are forced off (they make
// no sense / accumulate latency on a live stream). A dedicated live
// pitch shifter (granular, 1:1 - pitch changes, tempo does not) runs
// in front of the chain.
//
// Threading model mirrors the Sampler: GUI thread mutates state under
// m_dspMutex; the TS3 capture thread try-locks per callback - if a
// state push holds the lock, the mic passes through CLEAN for that
// block. Any DSP exception also falls back to clean pass-through:
// the mic can never go silent because of an effect.
//
// Master enable is persisted (user decision: remember last state) in
// QSettings("GameBaiters","Soundboard") under micfx/, together with
// pitch, monitor flag and the serialized sandbox state.
class MicFx : public QObject
{
    Q_OBJECT

public:
    static MicFx &instance();

    // ---- GUI-thread API ----
    void setEnabled(bool on);
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    void toggle() { setEnabled(!enabled()); }

    // Live 1:1 pitch in semitones (-12..+12).
    void  setPitchSemitones(float st);
    float pitchSemitones() const { return m_pitchSemitones; }

    // Mic input gain boost in dB (-20..+20, 0 = unity). Applied to the
    // outgoing capture buffer BEFORE the pitch shifter and DSP chain,
    // so it is the effective microphone volume.
    void  setGainDb(float db);
    float gainDb() const { return m_gainDb; }

    // Hear my own processed voice (mixed into local playback).
    void setMonitor(bool on) { m_monitor.store(on, std::memory_order_relaxed); saveSettings(); }
    bool monitor() const { return m_monitor.load(std::memory_order_relaxed); }

    // Full sandbox state for the mic chain (sanitized internally).
    void setSandboxState(const SandboxState &s);
    const SandboxState &sandboxState() const { return m_state; }

    // ---- Background ambience (V3) ----
    // A procedurally generated environment loop (washing machine,
    // drill, rain, ...) mixed into the outgoing mic stream while the
    // master toggle is on - "you are talking from inside the scene".
    // id < 0 = none. Ids/names come from MicAmbience. id == -2 means a
    // user-picked custom file (see setAmbienceFile).
    void  setAmbience(int id);
    int   ambienceId() const { return m_ambienceId; }
    // Custom user file as the ambience loop (any format FFmpeg opens).
    // Returns false when the file could not be decoded (state untouched).
    bool  setAmbienceFile(const QString &path);
    QString ambienceFile() const { return m_ambCustomPath; }
    static constexpr int kAmbienceCustom = -2;
    // Ambience mix volume, 0..1 (applied post-DSP so voice effects
    // never distort the scene bed).
    void  setAmbienceVolume(float v);
    float ambienceVolume() const { return m_ambGain.load(std::memory_order_relaxed); }

    // Feature kill switch (Settings). When off the whole feature is
    // inert AND every related UI element hides.
    void setFeatureEnabled(bool on);
    bool featureEnabled() const { return m_featureEnabled.load(std::memory_order_relaxed); }

    // Latest peak levels (0..1) for the mic channel meter.
    float levelIn()  const { return m_levelIn.load(std::memory_order_relaxed); }
    float levelOut() const { return m_levelOut.load(std::memory_order_relaxed); }

    // ---- Voice presets (V2) ----
    struct Preset {
        QString      name;
        SandboxState state;
        float        pitchSemitones;
    };
    static QVector<Preset> builtinPresets();
    // Applies preset i (state + pitch) in one shot.
    void applyPreset(int index);

    // ---- TS3 audio-thread API ----
    // In-place processing of the outgoing mic buffer. Returns true when
    // the buffer was modified (caller sets *edited). channels is 1 or 2.
    bool processCapture(short *samples, int sampleCount, int channels);
    // Mixes the monitor ring into the local playback buffer. ciLeft /
    // ciRight are the interleaved channel indices of the front/head
    // L-R speakers (resolved by the caller from the TS3 speaker
    // array); overwriteL/R mirror the channelFillMask semantics - when
    // a channel is not yet filled its content is undefined and must be
    // overwritten rather than summed. Returns true when samples were
    // written (caller updates the fill mask).
    bool mixMonitor(short *samples, int sampleCount, int channels,
                    int ciLeft, int ciRight,
                    bool overwriteL, bool overwriteR);

    // Load persisted state. Called once at plugin init (after Qt is up).
    void loadSettings();

signals:
    // Emitted on every master-toggle change (GUI or hotkey). Queued
    // across threads - safe to connect UI directly.
    void enabledChanged(bool on);

private:
    MicFx();
    ~MicFx() override = default;
    Q_DISABLE_COPY(MicFx)

    void saveSettings();
    // Forces the disallowed stages off; keeps everything else.
    static SandboxState sanitize(const SandboxState &s);

    std::atomic<bool> m_enabled{false};
    std::atomic<bool> m_featureEnabled{true};
    std::atomic<bool> m_monitor{false};
    float m_pitchSemitones = 0.0f;
    std::atomic<float> m_pitchRatio{1.0f};
    float m_gainDb = 0.0f;
    std::atomic<float> m_gainLin{1.0f};
    SandboxState m_state;

    // DSP core - guarded by m_dspMutex (GUI holds on state push,
    // capture thread try-locks per callback).
    std::mutex      m_dspMutex;
    SlotDsp         m_dsp;
    PitchShiftGrain m_pitchL, m_pitchR;

    std::atomic<float> m_levelIn{0.0f};
    std::atomic<float> m_levelOut{0.0f};

    // Ambience loop: generated on the GUI thread, swapped in under
    // m_dspMutex; the capture thread (which already holds the try-lock
    // while processing) only reads it. m_ambPos is audio-thread-only.
    std::shared_ptr<const std::vector<float>> m_ambLoop;
    size_t m_ambPos = 0;
    int    m_ambienceId = -1;
    QString m_ambCustomPath;
    std::atomic<float> m_ambGain{0.35f};

    // Monitor ring: SPSC (capture thread produces, playback thread
    // consumes), interleaved stereo shorts.
    static constexpr int kMonFrames = 16384;
    short m_monRing[kMonFrames * 2] = {};
    std::atomic<int> m_monW{0};
    std::atomic<int> m_monR{0};

    bool m_settingsLoaded = false;
};
