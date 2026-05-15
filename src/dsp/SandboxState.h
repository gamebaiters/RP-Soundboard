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
    // Pipeline stages, in canonical/default order. Paulstretch is a
    // represented module: it carries a pipeline slot, a name and a UI
    // panel like every other effect, but its actual DSP runs on a
    // separate streaming feed (see SlotDsp) and is pinned first - its
    // index here is cosmetic. Mono is NOT a stage: it is a plain
    // post-chain checkbox (monoEnabled), applied at a fixed point.
    enum DspStage {
        Stage_Paulstretch = 0,
        Stage_EQ,
        Stage_Compressor,
        Stage_Saturator,
        Stage_Spatial,
        Stage_Chorus,
        Stage_Flanger,
        Stage_Flangus,
        Stage_Phaser,
        Stage_Delay,
        Stage_Reverb,
        Stage_Limiter,
        Stage_Bitcrusher,
        Stage_GenLoss,
        Stage_COUNT
    };

    static const char *stageName(int stage);
    static void defaultPipelineOrder(int out[Stage_COUNT]);

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

    // ---- Compressor ----
    bool  compEnabled     = false;
    float compThresholdDb = -20.0f;
    float compRatio       = 4.0f;
    float compAttackMs    = 10.0f;
    float compReleaseMs   = 100.0f;
    float compKneeDb      = 6.0f;
    float compMakeupDb    = 0.0f;

    // ---- Chorus ----
    bool  chorusEnabled   = false;
    float chorusRate      = 1.0f;
    float chorusDepth     = 3.0f;
    float chorusBaseDelay = 10.0f;
    int   chorusVoices    = 2;
    float chorusMix       = 0.0f;

    // ---- Flanger ----
    bool  flangerEnabled   = false;
    float flangerRate      = 0.5f;
    float flangerDepth     = 0.7f;
    float flangerFeedback  = 0.5f;
    float flangerBaseDelay = 2.0f;
    float flangerMix       = 0.0f;

    // ---- Flangus (Flanger-Chorus hybrid) ----
    bool  flangusEnabled   = false;
    float flangusRate      = 0.8f;
    float flangusDepth     = 0.5f;
    float flangusFeedback  = 0.3f;
    int   flangusVoices    = 3;
    float flangusSpread    = 0.5f;
    float flangusMix       = 0.0f;

    // ---- Phaser ----
    bool  phaserEnabled    = false;
    float phaserRate       = 0.5f;
    float phaserDepth      = 0.7f;
    float phaserFeedback   = 0.3f;
    int   phaserStages     = 6;
    float phaserMix        = 0.0f;

    // ---- Saturator ----
    bool  saturatorEnabled = false;
    float saturatorDrive   = 2.0f;
    float saturatorMix     = 0.0f;
    float saturatorTone    = 8000.0f;
    int   saturatorMode    = 0;        // 0=Soft

    // ---- Delay ----
    bool  delayEnabled     = false;
    float delayTimeMs      = 300.0f;
    float delayFeedback    = 0.4f;
    float delayMix         = 0.0f;
    float delayDamping     = 5000.0f;
    bool  delayPingPong    = false;

    // ---- Limiter ----
    bool  limiterEnabled   = false;
    int   limiterMode      = 0;        // 0=Limiter, 1=Compressor, 2=Gate
    float limiterCeiling   = -0.3f;
    float limiterLookahead = 1.0f;
    float limiterRelease   = 100.0f;
    float limiterRatio     = 4.0f;
    float limiterGateThresh = -60.0f;

    // ---- Bitcrusher (quality degradation) ----
    bool  bitcrusherEnabled   = false;
    int   bitcrusherBitDepth  = 16;    // 1..16 (16 = lossless)
    float bitcrusherRate      = 48000.0f; // 500..48000 (48000 = lossless)

    // ---- Mono ----
    bool  monoEnabled = false;

    // ---- Generation Loss (re-encoding degradation) ----
    bool  genLossEnabled     = false;
    int   genLossGenerations = 1;          // 1..1000

    // DSP pipeline order (user-draggable). Default = canonical enum
    // order, so it matches defaultPipelineOrder() (out[i] = i).
    int pipelineOrder[Stage_COUNT] = {
        Stage_Paulstretch, Stage_EQ, Stage_Compressor, Stage_Saturator,
        Stage_Spatial, Stage_Chorus, Stage_Flanger, Stage_Flangus,
        Stage_Phaser, Stage_Delay, Stage_Reverb, Stage_Limiter,
        Stage_Bitcrusher, Stage_GenLoss
    };

    QJsonObject toJson() const;
    static SandboxState fromJson(const QJsonObject &o);
    void resetToDefaults();
    bool isModified() const;           // any param != defaults?
};
