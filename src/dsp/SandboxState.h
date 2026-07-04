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
        // Appended (never renumbered) so pre-existing pipelineOrderV2
        // arrays keep their positions and the new stages are simply
        // added to the tail via the fromJson padding logic.
        Stage_NoiseGate,
        Stage_DeEsser,
        Stage_TransientShaper,
        Stage_DynEq,
        // v2.3 additions - same append-only rule.
        Stage_VoiceFx,      // macro-module: 10 classic voice/creative FX
        Stage_BassEnh,      // psychoacoustic bass enhancer
        Stage_Binaural,     // binaural-beats tone layer
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

    // Which HRTF engine renders the 3D spatial modes.
    //   Engine_Classic - the original parametric Brown-Duda structural
    //                    model (dsp/Positional). Zero dependencies.
    //   Engine_Leia    - measured-HRTF convolution from a bundled SOFA
    //                    dataset plus image-source room reflections
    //                    (dsp/leia). Correct front/back, richer space.
    // Stable values; default 0 keeps every pre-existing INI on Classic.
    enum SpatialEngine {
        Engine_Classic = 0,
        Engine_Leia    = 1
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

    // ---- Spatial engine selection + Leia-only parameters ----
    // spatialEngine applies to every 3D HRTF mode (Manual / Rotate /
    // 8D preset). L/R Pan and Off ignore it. The leia* fields are read
    // only when spatialEngine == Engine_Leia.
    // Default: Leia (measured-HRTF convolution). Classic remains
    // available via Settings → Channels for users who prefer the
    // legacy parametric Brown-Duda engine, but Leia is the user-facing
    // default because it produces dramatically more convincing 3D
    // imaging on every HRTF mode (Manual / Rotate / 8D).
    int   spatialEngine  = Engine_Leia;
    bool  leiaReflEnable = true;        // image-source room reflections
    float leiaReflLevel  = -6.0f;       // dB, -25..20
    float leiaRoomSize   = 12.0f;       // m, 7..50
    int   leiaRoomType   = 1;           // 0..4 Drapes/Studio/Tiles/Concrete/Glass
    float leiaClarity    = 100.0f;      // %, 0..100 direct-path level
    float leiaWidth      = 35.0f;       // %, 0..100 reflection amount

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

    // ---- Random per-fire variation (per channel) ----
    // Opt-in pitch jitter applied to every sound that fires through
    // this channel's slot AND to every loop restart (audio thread
    // re-reads from the live SandboxState every loop, so disabling
    // the feature mid-playback stops the jitter immediately).
    // Volume + start-offset axes were removed - they produced glitchy
    // playback (volume jumps mid-loop, start-offset re-seeks mid-buffer)
    // and the only one the user actually wanted is pitch.
    bool  randomEnabled      = false;
    int   randomPitchCents   = 0;     // 0..200

    // ---- Noise Gate ----
    bool  gateEnabled       = false;
    float gateThresholdDb   = -40.0f;    // -80..0
    float gateRangeDb       = -60.0f;    // -80..0 (deeper = more silent)
    float gateAttackMs      = 2.0f;      // 0.1..50
    float gateHoldMs        = 20.0f;     // 0..500
    float gateReleaseMs     = 150.0f;    // 1..2000
    int   gateSidechainSlot = -1;        // -1 = self; else Sampler slot idx

    // ---- De-esser ----
    bool  deesserEnabled     = false;
    float deesserFreqHz      = 6500.0f;  // 1000..12000
    float deesserQ           = 3.0f;     // 0.5..12
    float deesserThresholdDb = -30.0f;   // -60..0
    float deesserRangeDb     = -10.0f;   // -30..0
    float deesserAttackMs    = 3.0f;
    float deesserReleaseMs   = 80.0f;
    int   deesserSidechainSlot = -1;

    // ---- Transient shaper ----
    bool  transEnabled   = false;
    float transAttackDb  = 0.0f;      // -20..+20
    float transSustainDb = 0.0f;      // -20..+20

    // ---- Dynamic EQ (4 bands) ----
    bool  dyneqEnabled = false;
    struct DynEqBand {
        bool  enabled       = false;
        float freq          = 200.0f;
        float q             = 1.4f;
        float staticGainDb  = 0.0f;
        float thresholdDb   = -30.0f;
        float ratio         = 2.0f;
        float dynamicDb     = -6.0f;
        float attackMs      = 15.0f;
        float releaseMs     = 150.0f;
    };
    DynEqBand dyneqBands[4] = {
        { false,  120.0f, 1.0f, 0.0f, -30.0f, 2.0f, -6.0f, 15.0f, 150.0f },
        { false,  600.0f, 1.2f, 0.0f, -30.0f, 2.0f, -6.0f, 15.0f, 150.0f },
        { false, 3000.0f, 1.4f, 0.0f, -30.0f, 2.0f, -6.0f, 15.0f, 150.0f },
        { false, 8000.0f, 1.4f, 0.0f, -30.0f, 2.0f, -6.0f, 15.0f, 150.0f }
    };

    // ---- Compressor sidechain source ----
    // -1 = self (per-slot input envelope). Otherwise Sampler slot idx
    // to sidechain from - lets a bass track duck under a kick track
    // without any manual routing.
    int   compSidechainSlot = -1;

    // ---- Doppler (Leia 3D rotate / 8D preset only) ----
    // Optional physical Doppler simulation on top of the 8D rotation.
    // dopplerStrength scales the raw physical shift (h*omega/c ~ few
    // cents at typical RPMs) - realistic listeners hear almost nothing,
    // so an exaggeration factor is exposed as strength 0..100 %.
    bool  dopplerEnabled  = false;
    float dopplerStrength = 50.0f;   // 0..100

    // ---- Voice FX macro-module (Stage_VoiceFx) ----
    // 10 classic effects sharing ONE pipeline stage; each sub-effect is
    // individually toggleable. vfxEnabled is the macro master switch.
    bool  vfxEnabled = false;

    // Ring modulator (Dalek voice)
    bool  vfxRingEnabled = false;
    float vfxRingFreq    = 440.0f;   // 20..2000 Hz
    float vfxRingMix     = 1.0f;     // 0..1

    // Tremolo (amplitude LFO)
    bool  vfxTremEnabled = false;
    float vfxTremRate    = 5.0f;     // 0.1..20 Hz
    float vfxTremDepth   = 0.8f;     // 0..1
    int   vfxTremShape   = 0;        // 0=sine 1=square

    // Vibrato (pitch LFO via modulated delay)
    bool  vfxVibEnabled  = false;
    float vfxVibRate     = 5.0f;     // 0.1..14 Hz
    float vfxVibDepth    = 0.5f;     // 0..1 -> 0..8 ms delay swing

    // Auto-wah (envelope-follower bandpass sweep)
    bool  vfxWahEnabled  = false;
    float vfxWahSens     = 0.7f;     // 0..1
    float vfxWahMinHz    = 350.0f;   // 100..1000
    float vfxWahMaxHz    = 2500.0f;  // 1000..6000
    float vfxWahQ        = 4.0f;     // 1..12
    float vfxWahMix      = 1.0f;     // 0..1

    // Exciter (HF harmonic enhancement)
    bool  vfxExcEnabled  = false;
    float vfxExcFreq     = 3000.0f;  // 1000..8000 Hz split
    float vfxExcDrive    = 2.0f;     // 1..10
    float vfxExcMix      = 0.3f;     // 0..1

    // Autotune (pitch correction, T-Pain at strength 1 / speed low)
    bool  vfxTuneEnabled  = false;
    float vfxTuneStrength = 1.0f;    // 0..1
    float vfxTuneSpeedMs  = 20.0f;   // 1..200 (low = hard snap)
    int   vfxTuneScale    = 0;       // 0=Chromatic 1=Major 2=Minor
    int   vfxTuneKey      = 0;       // 0=C .. 11=B

    // Vocoder (internal carrier, input = modulator)
    bool  vfxVocEnabled  = false;
    int   vfxVocCarrier  = 0;        // 0=Saw 1=Noise
    float vfxVocPitchHz  = 110.0f;   // 50..400
    float vfxVocMix      = 1.0f;     // 0..1

    // Formant shifter (spectral envelope warp, pitch preserved)
    bool  vfxFormEnabled = false;
    float vfxFormShift   = 0.0f;     // -12..+12 "semitones" of warp
    float vfxFormMix     = 1.0f;     // 0..1

    // Shimmer reverb (pitch-shifted feedback tail)
    bool  vfxShimEnabled  = false;
    float vfxShimMix      = 0.3f;    // 0..1
    float vfxShimFeedback = 0.5f;    // 0..0.9
    int   vfxShimPitch    = 12;      // +12 or +7 semitones
    float vfxShimDamp     = 0.4f;    // 0..1

    // Reverse delay (grain-reversed echo - live "reverse reverb" feel)
    bool  vfxRevEnabled   = false;
    float vfxRevTimeMs    = 500.0f;  // 100..2000 grain size
    float vfxRevFeedback  = 0.35f;   // 0..0.9
    float vfxRevMix       = 0.4f;    // 0..1

    // ---- Bass Enhancer (Stage_BassEnh) ----
    // Psychoacoustic bass: harmonics generated from the sub band are
    // mixed on top so small speakers/earbuds "hear" the low end.
    bool  bassEnhEnabled = false;
    float bassEnhFreq    = 120.0f;   // 60..300 Hz crossover
    float bassEnhDrive   = 3.0f;     // 1..10
    float bassEnhMix     = 0.4f;     // 0..1 harmonics level

    // ---- Binaural beats (Stage_Binaural) ----
    // Adds a sine pair under the program: baseHz to the left ear,
    // baseHz+beatHz to the right. Level in dB (well under the music).
    bool  binauralEnabled = false;
    float binauralBaseHz  = 200.0f;  // 80..600
    float binauralBeatHz  = 7.0f;    // 0.5..40
    float binauralLevelDb = -24.0f;  // -60..-6

    // ---- LFO modulation matrix (per channel) ----
    // Two free-running LFOs, four routing rows. Target values are the
    // LfoMatrix::Target enum (0 = none). Amount is bipolar -1..+1.
    bool  lfoEnabled[2]     = { false, false };
    float lfoRateHz[2]      = { 1.0f, 0.25f };
    int   lfoShape[2]       = { 0, 0 };     // 0=sine 1=tri 2=square 3=S&H
    int   lfoRouteLfo[4]    = { 0, 0, 1, 1 };
    int   lfoRouteTarget[4] = { 0, 0, 0, 0 };
    float lfoRouteAmount[4] = { 0.5f, 0.5f, 0.5f, 0.5f };

    // ---- Reverb engine mode ----
    // 0 = Algorithmic (Freeverb-style, the historical engine).
    // 1 = Convolution (partitioned FFT convolution against a procedural
    //     or user-loaded impulse response). Wet level reuses reverbWet.
    int     reverbConvMode   = 0;
    int     reverbConvPreset = 0;    // 0=Hall 1=Church 2=Room 3=Spring
    QString reverbConvIrPath;        // empty = use the preset IR

    // ---- Quality switches ----
    // hqOversampling gates the 2x-oversampled shapers in the nonlinear
    // stages (Saturator core is always oversampled; this covers the new
    // Exciter / BassEnhancer shapers). truePeakMode upgrades the Limiter
    // sidechain to 4x interpolated inter-sample peak estimation.
    bool  hqOversampling = true;
    bool  truePeakMode   = true;
    // Failsafe anti-clip: dedicated -1 dBFS true-peak brickwall applied
    // at the very end of the chain (after every stage, mono fold, LFO
    // gains and tape stop). Guarantees no effect combination - EQ
    // boosts included - can ever clip the output. Default ON.
    bool  failsafeEnabled = true;

    // ---- Sidechain ducking (per channel) ----
    // Mark a channel as a DUCK SOURCE - while it is playing, every
    // other slot's output volume drops by duckOthersDb (clamped -30..0).
    // Smoothly attacks / releases so the duck does not click. The
    // typical use is: music on channel 0, mic-style SFX on channel 1
    // with duckSource=true. The SFX plays, music ducks under it,
    // music returns when the SFX ends.
    bool  duckSource         = false;
    float duckOthersDb       = -12.0f; // -30..0; less = deeper duck

    // DSP pipeline order (user-draggable). Default = a musically sane
    // chain: dynamics first, tone shaping, spatial, modulation, the
    // Voice FX macro, time effects, then finalizers + generators.
    int pipelineOrder[Stage_COUNT] = {
        Stage_Paulstretch, Stage_NoiseGate, Stage_EQ, Stage_DynEq,
        Stage_DeEsser, Stage_Compressor, Stage_TransientShaper,
        Stage_Saturator, Stage_BassEnh, Stage_Spatial, Stage_Chorus,
        Stage_Flanger, Stage_Flangus, Stage_Phaser, Stage_VoiceFx,
        Stage_Delay, Stage_Reverb, Stage_Limiter, Stage_Bitcrusher,
        Stage_GenLoss, Stage_Binaural
    };

    QJsonObject toJson() const;
    static SandboxState fromJson(const QJsonObject &o);
    void resetToDefaults();
    bool isModified() const;           // any param != defaults?
};
