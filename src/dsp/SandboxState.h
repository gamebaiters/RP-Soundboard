#pragma once

#include <QJsonObject>
#include <QString>

// Per-channel "audio sandbox" configuration. Carries every parameter
// the user can tweak from the channel's Audio Sandbox dialog. Lives on
// disk inside ChannelState so per-channel sandbox settings survive
// session restarts; Sampler reads it when applying state to a slot.
//
// Default-constructed instance = "neutral" / bypass. Slot DSP runs only
// when at least one of {spatialMode != Off, stretchEnabled, EQ touched,
// reverbWet > 0} is true; the bypass flag below short-circuits DSP
// regardless so the user can A/B test cheaply.
struct SandboxState
{
    // Stable enum values - new modes appended at the end so existing
    // INI files keep loading correctly.
    enum SpatialMode {
        Spatial_Off      = 0,
        Spatial_3DManual = 1,
        Spatial_3DRotate = 2,
        Spatial_LRPan    = 3,   // simple equal-power L/R balance
        Spatial_8DPreset = 4    // behaves like Rotate, preset values applied on selection
    };

    // Master per-channel toggle. False = DSP completely bypassed.
    bool  enabled = false;

    // ---- Spatial ----
    int   spatialMode    = Spatial_Off;
    float panValue       = 0.0f;       // L/R Pan mode only (-1..+1)
    float posX           = 0.0f;       // -1..+1 right (centre = 0)
    float posY           = 0.0f;       // -1..+1 back (centre = 0; +y = behind)
    float elev           = 0.0f;       // -1..+1 up (mapped to z)
    float distanceM      = 1.5f;       // 0.5..5
    float stereoWidthDeg = 60.0f;      // 0..120 (60 = ITU-R BS.775)
    bool  headSway       = false;
    // Wet / dry crossfade between the HRTF output and the raw input
    // stereo. 1.0 = full HRTF; 0.0 = bypass. Mid values keep the song
    // "stereo open" while still adding the spatial cue - this is the
    // single biggest reason audio_sandbox standalone sounded better
    // than the soundboard once the chain was migrated over.
    float spatialMix     = 1.0f;       // 0..1

    float rotateRpm      = 15.0f;
    float rotateRadiusM  = 1.5f;
    bool  rotateCcw      = false;
    float rotateElev     = 0.0f;

    // ---- Paulstretch ----
    bool  stretchEnabled = false;
    float stretchFactor  = 4.0f;       // 1..50
    float stretchWindowMs = 180.0f;    // 50..1000

    // ---- 16-band ISO EQ ----
    bool  eqEnabled      = true;       // master EQ bypass toggle
    float eqBandDb[16]   = {0};        // -12..+12

    // Room ambience (Freeverb-style stereo wet send). Renamed from
    // "Room reverb" to avoid confusion with the existing per-button
    // FxPanel reverb (which is a transient sound-mod effect, not a
    // spatialiser). The two complement each other: FxPanel reverb
    // shapes the SOUND, sandbox ambience places it in a SPACE.
    float reverbWet      = 0.0f;       // 0..1 ambience wet mix

    QJsonObject toJson() const;
    static SandboxState fromJson(const QJsonObject &o);
    void resetToDefaults();
    bool isModified() const;           // any param != defaults?
};
