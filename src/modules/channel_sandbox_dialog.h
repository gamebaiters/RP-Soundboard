#pragma once

#include <QDialog>
#include <QVector>
#include "../dsp/SandboxState.h"

class QComboBox;
class QSlider;
class QLabel;
class QPushButton;
class QCheckBox;
class QGroupBox;
class QScrollArea;
class QVBoxLayout;
class QLineEdit;
class PositionalPad;
class ExpandableSection;

// Cross-channel persistence of the user's preferred sandbox engine
// (Classic / Leia). Written from the sandbox dialog's engine combo and
// read from Channel ctor so a freshly-added channel defaults to the
// engine the user picked most recently rather than always to Classic.
namespace SandboxEnginePref {
    int load();           // returns SandboxState::Engine_* (defaults to Classic)
    void save(int engine);
}

// Per-channel "Audio Sandbox" dialog. Hosts every DSP knob the user can
// tweak for one slot: Spatial (Off / L-R Pan / 3D HRTF / Rotate /
// 8D preset), Paulstretch, 16-band ISO EQ. Master volume, playback
// transport, file loading and the loop toggle live on the channel
// itself, not in here.
class ChannelSandboxDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ChannelSandboxDialog(int channelId, QWidget *parent = nullptr);

    void setChannelTitle(const QString &title);
    void setState(const SandboxState &s);
    SandboxState state() const { return m_state; }

    void setAllControlsEnabled(bool on);

    // Polled from the wiring layer at ~1 Hz: display the DSP CPU% the
    // slot has been spending the last second.
    void setCpuPercent(double pct);
    // Polled from the meter timer (~25 Hz). Pushes the channel's
    // current peak level into the EQ band widgets so they animate.
    void pushAudioLevel(float peakL, float peakR);
    // Per-band spectrum levels from the slot's FFT analyser. Each
    // band's widget gets its own level so the LED column reflects
    // ONLY that frequency's content.
    void pushEqBandLevels(const float bands[16]);

signals:
    void stateChanged(const SandboxState &s);
    void resetRequested(int channelId);

private slots:
    void onEnableToggled(bool on);
    void onModeChanged(int idx);
    void onEngineChanged(int idx);
    void onPanChanged(int v);
    void onPadMoved(float x, float y);
    void onElevChanged(int v);
    void onDistChanged(int v);
    void onWidthChanged(int v);
    void onRpmChanged(int v);
    void onRadiusChanged(int v);
    void onCcwToggled(bool on);
    void onSwayToggled(bool on);
    void onSpatialMixChanged(int v);
    void onAmbienceChanged(int v);
    void onStretchToggled(bool on);
    void onStretchFactorChanged(int v);
    void onStretchWindowChanged(int v);
    void onEqEnabledToggled(bool on);
    void onResetClicked();
    void onCopyEq();
    void onPasteEq();
    void onCopySandbox();
    void onPasteSandbox();
    // Indented plain-JSON dump of the live sandbox state (no marker
    // prefix, no Base64). Aimed at bug reports - the user can paste
    // this into a GitHub issue and a human can read every field.
    void onCopySandboxJsonDebug();

private:
    void buildUi();
    void pushStateToWidgets();
    void pushChange();
    void refreshTitle();
    void applyModeVisibility();
    void load8DPreset();           // populates state for 8D mode
    int  modeForDropdown(int idx) const;     // dropdown index -> SandboxState mode
    int  dropdownForMode(int mode) const;    // SandboxState mode -> dropdown index

    // Re-orders the DSP accordion panels to match m_state.pipelineOrder.
    void applyPipelineOrderToUi();
    // Pipeline block clicked: scroll the matching panel into view + open.
    void onPipelineStageClicked(int stage);
    // Filters the DSP accordion panels by a search substring.
    void filterDspModules(const QString &text);

    int m_channelId;
    QString m_channelTitle;
    SandboxState m_state;
    bool m_loading = false;

    QCheckBox     *m_enable      = nullptr;
    class QLabel  *m_cpuLabel    = nullptr;
    QComboBox     *m_modeBox     = nullptr;
    QPushButton   *m_resetBtn    = nullptr;
    QPushButton   *m_closeBtn    = nullptr;

    // Pan-mode controls
    QGroupBox     *m_panGroup    = nullptr;
    QSlider       *m_pan         = nullptr;
    QLabel        *m_panLabel    = nullptr;

    // 3D HRTF controls (shared by 3D Manual / Rotate / 8D Preset)
    QGroupBox     *m_hrtfGroup   = nullptr;
    PositionalPad *m_pad         = nullptr;
    QLabel        *m_padLabel    = nullptr;
    QSlider       *m_elev        = nullptr;
    QLabel        *m_elevLabel   = nullptr;
    QSlider       *m_dist        = nullptr;
    QLabel        *m_distLabel   = nullptr;
    QSlider       *m_width       = nullptr;
    QLabel        *m_widthLabel  = nullptr;
    QSlider       *m_rpm         = nullptr;
    QLabel        *m_rpmLabel    = nullptr;
    QSlider       *m_radius      = nullptr;
    QLabel        *m_radiusLabel = nullptr;
    QCheckBox     *m_ccw         = nullptr;
    QCheckBox     *m_sway        = nullptr;
    QSlider       *m_spatialMix      = nullptr;
    QLabel        *m_spatialMixLabel = nullptr;
    QSlider       *m_ambience        = nullptr;
    QLabel        *m_ambienceLabel   = nullptr;

    // Spatial engine selector + Leia (measured-HRTF) only controls.
    QComboBox  *m_engineBox      = nullptr;
    QGroupBox  *m_leiaGroup      = nullptr;
    QCheckBox  *m_leiaRefl       = nullptr;
    QSlider    *m_leiaReflLevel  = nullptr; QLabel *m_leiaReflLevelLabel = nullptr;
    QSlider    *m_leiaRoomSize   = nullptr; QLabel *m_leiaRoomSizeLabel  = nullptr;
    QComboBox  *m_leiaRoomType   = nullptr;
    QSlider    *m_leiaClarity    = nullptr; QLabel *m_leiaClarityLabel   = nullptr;
    QSlider    *m_leiaWidth      = nullptr; QLabel *m_leiaWidthLabel     = nullptr;

    // Per-mode rows (whole rows that get hidden together).
    class QWidget *m_padContainer = nullptr;
    class QWidget *m_distRow      = nullptr;
    class QWidget *m_widthRow     = nullptr;
    class QWidget *m_rpmRow       = nullptr;
    class QWidget *m_radiusRow    = nullptr;

    // Paulstretch controls
    QCheckBox     *m_stretchEn   = nullptr;
    QSlider       *m_stretchFac  = nullptr;
    QLabel        *m_stretchFacLabel = nullptr;
    QSlider       *m_stretchWin  = nullptr;
    QLabel        *m_stretchWinLabel = nullptr;

    // EQ
    QCheckBox        *m_eqEnable = nullptr;
    QVector<class EqBandWidget*> m_eqSliders;
    QVector<QLabel*>  m_eqLabels;

    // Compressor
    QCheckBox *m_compEnable = nullptr;
    QSlider *m_compThreshold = nullptr; QLabel *m_compThresholdLabel = nullptr;
    QSlider *m_compRatio     = nullptr; QLabel *m_compRatioLabel     = nullptr;
    QSlider *m_compAttack    = nullptr; QLabel *m_compAttackLabel    = nullptr;
    QSlider *m_compRelease   = nullptr; QLabel *m_compReleaseLabel   = nullptr;
    QSlider *m_compKnee      = nullptr; QLabel *m_compKneeLabel      = nullptr;
    QSlider *m_compMakeup    = nullptr; QLabel *m_compMakeupLabel    = nullptr;

    // Saturator
    QCheckBox *m_satEnable = nullptr;
    QSlider   *m_satDrive = nullptr; QLabel *m_satDriveLabel = nullptr;
    QSlider   *m_satMix   = nullptr; QLabel *m_satMixLabel   = nullptr;
    QSlider   *m_satTone  = nullptr; QLabel *m_satToneLabel  = nullptr;
    QComboBox *m_satMode  = nullptr;

    // Chorus
    QCheckBox *m_chorusEnable = nullptr;
    QSlider *m_chorusRate  = nullptr; QLabel *m_chorusRateLabel  = nullptr;
    QSlider *m_chorusDepth = nullptr; QLabel *m_chorusDepthLabel = nullptr;
    QSlider *m_chorusDelay = nullptr; QLabel *m_chorusDelayLabel = nullptr;
    QSlider *m_chorusVoices= nullptr; QLabel *m_chorusVoicesLabel= nullptr;
    QSlider *m_chorusMix   = nullptr; QLabel *m_chorusMixLabel   = nullptr;

    // Flanger
    QCheckBox *m_flangerEnable = nullptr;
    QSlider *m_flangerRate     = nullptr; QLabel *m_flangerRateLabel     = nullptr;
    QSlider *m_flangerDepth    = nullptr; QLabel *m_flangerDepthLabel    = nullptr;
    QSlider *m_flangerFeedback = nullptr; QLabel *m_flangerFeedbackLabel = nullptr;
    QSlider *m_flangerDelay    = nullptr; QLabel *m_flangerDelayLabel    = nullptr;
    QSlider *m_flangerMix      = nullptr; QLabel *m_flangerMixLabel      = nullptr;

    // Flangus
    QCheckBox *m_flangusEnable = nullptr;
    QSlider *m_flangusRate     = nullptr; QLabel *m_flangusRateLabel     = nullptr;
    QSlider *m_flangusDepth    = nullptr; QLabel *m_flangusDepthLabel    = nullptr;
    QSlider *m_flangusFeedback = nullptr; QLabel *m_flangusFeedbackLabel = nullptr;
    QSlider *m_flangusVoices   = nullptr; QLabel *m_flangusVoicesLabel   = nullptr;
    QSlider *m_flangusSpread   = nullptr; QLabel *m_flangusSpreadLabel   = nullptr;
    QSlider *m_flangusMix      = nullptr; QLabel *m_flangusMixLabel      = nullptr;

    // Phaser
    QCheckBox *m_phaserEnable = nullptr;
    QSlider *m_phaserRate     = nullptr; QLabel *m_phaserRateLabel     = nullptr;
    QSlider *m_phaserDepth    = nullptr; QLabel *m_phaserDepthLabel    = nullptr;
    QSlider *m_phaserFeedback = nullptr; QLabel *m_phaserFeedbackLabel = nullptr;
    QSlider *m_phaserStages   = nullptr; QLabel *m_phaserStagesLabel   = nullptr;
    QSlider *m_phaserMix      = nullptr; QLabel *m_phaserMixLabel      = nullptr;

    // Delay
    QCheckBox *m_delayEnable = nullptr;
    QSlider   *m_delayTime     = nullptr; QLabel *m_delayTimeLabel     = nullptr;
    QSlider   *m_delayFeedback = nullptr; QLabel *m_delayFeedbackLabel = nullptr;
    QSlider   *m_delayMix      = nullptr; QLabel *m_delayMixLabel      = nullptr;
    QSlider   *m_delayDamping  = nullptr; QLabel *m_delayDampingLabel  = nullptr;
    QCheckBox *m_delayPingPong = nullptr;

    // Limiter
    QCheckBox *m_limiterEnable  = nullptr;
    QComboBox *m_limiterModeBox = nullptr;
    QSlider *m_limiterCeiling   = nullptr; QLabel *m_limiterCeilingLabel   = nullptr;
    QSlider *m_limiterLookahead = nullptr; QLabel *m_limiterLookaheadLabel = nullptr;
    QSlider *m_limiterRelease   = nullptr; QLabel *m_limiterReleaseLabel   = nullptr;
    QSlider *m_limiterRatio     = nullptr; QLabel *m_limiterRatioLabel     = nullptr;
    QSlider *m_limiterGate      = nullptr; QLabel *m_limiterGateLabel      = nullptr;

    // Bitcrusher
    QCheckBox *m_bitcrushEnable = nullptr;
    QSlider   *m_bitcrushBits = nullptr; QLabel *m_bitcrushBitsLabel = nullptr;
    QSlider   *m_bitcrushRate = nullptr; QLabel *m_bitcrushRateLabel = nullptr;

    // Mono
    QCheckBox *m_monoEnable = nullptr;

    // Random per-fire PITCH jitter (per-channel sandbox feature).
    // Lives inside the DSP accordion alongside the other modules now,
    // not as its own full-width section, so it shares the visual
    // language of the rest of the chain.
    class QCheckBox *m_randomEnable = nullptr;
    class QSpinBox  *m_randomPitch  = nullptr;

    // Sidechain ducking: this channel ducks every other slot while it
    // plays. Also lives inside the DSP accordion as a compact module.
    class QCheckBox *m_duckEnable     = nullptr;
    class QSlider   *m_duckAmount     = nullptr;
    class QLabel    *m_duckAmountLabel = nullptr;

    // Noise Gate
    QCheckBox *m_gateEnable  = nullptr;
    QSlider   *m_gateThresh  = nullptr; QLabel *m_gateThreshLabel = nullptr;
    QSlider   *m_gateRange   = nullptr; QLabel *m_gateRangeLabel  = nullptr;
    QSlider   *m_gateAttack  = nullptr; QLabel *m_gateAttackLabel = nullptr;
    QSlider   *m_gateHold    = nullptr; QLabel *m_gateHoldLabel   = nullptr;
    QSlider   *m_gateRelease = nullptr; QLabel *m_gateReleaseLabel= nullptr;
    class QSpinBox *m_gateSidechainSlot = nullptr;

    // De-esser
    QCheckBox *m_deEssEnable = nullptr;
    QSlider   *m_deEssFreq   = nullptr; QLabel *m_deEssFreqLabel  = nullptr;
    QSlider   *m_deEssQ      = nullptr; QLabel *m_deEssQLabel     = nullptr;
    QSlider   *m_deEssThresh = nullptr; QLabel *m_deEssThreshLabel= nullptr;
    QSlider   *m_deEssRange  = nullptr; QLabel *m_deEssRangeLabel = nullptr;
    QSlider   *m_deEssAttack = nullptr; QLabel *m_deEssAttackLabel= nullptr;
    QSlider   *m_deEssRelease= nullptr; QLabel *m_deEssReleaseLabel = nullptr;
    class QSpinBox *m_deEssSidechainSlot = nullptr;

    // Transient shaper
    QCheckBox *m_transEnable   = nullptr;
    QSlider   *m_transAttack   = nullptr; QLabel *m_transAttackLabel  = nullptr;
    QSlider   *m_transSustain  = nullptr; QLabel *m_transSustainLabel = nullptr;

    // Dynamic EQ (4 bands)
    QCheckBox *m_dynEqEnable = nullptr;
    struct DynEqBandUi {
        QCheckBox *enable  = nullptr;
        QSlider   *freq    = nullptr; QLabel *freqLabel   = nullptr;
        QSlider   *q       = nullptr; QLabel *qLabel      = nullptr;
        QSlider   *sGain   = nullptr; QLabel *sGainLabel  = nullptr;
        QSlider   *thresh  = nullptr; QLabel *threshLabel = nullptr;
        QSlider   *ratio   = nullptr; QLabel *ratioLabel  = nullptr;
        QSlider   *dGain   = nullptr; QLabel *dGainLabel  = nullptr;
    };
    DynEqBandUi m_dynEqBand[4];

    // Compressor sidechain source (added alongside the existing Comp UI).
    class QSpinBox *m_compSidechainSlot = nullptr;

    // Doppler toggle (Leia group)
    QCheckBox *m_dopplerEnable   = nullptr;
    QSlider   *m_dopplerStrength = nullptr; QLabel *m_dopplerStrengthLabel = nullptr;


    // Generation Loss
    QCheckBox   *m_genLossEnable = nullptr;
    QSlider     *m_genLossGens = nullptr; QLabel *m_genLossGensLabel = nullptr;
    QPushButton *m_resetGenLoss = nullptr;

    // Bitcrusher preset
    QComboBox *m_bitcrushPreset = nullptr;

    // DSP modules container (master gating)
    QGroupBox *m_dspGroup = nullptr;

    // Per-module reset buttons
    QPushButton *m_resetComp    = nullptr;
    QPushButton *m_resetSat     = nullptr;
    QPushButton *m_resetChorus  = nullptr;
    QPushButton *m_resetFlanger = nullptr;
    QPushButton *m_resetFlangus = nullptr;
    QPushButton *m_resetPhaser  = nullptr;
    QPushButton *m_resetDelay   = nullptr;
    QPushButton *m_resetLimiter    = nullptr;
    QPushButton *m_resetBitcrush  = nullptr;

    // Pipeline widget + DSP module list plumbing
    class PipelineWidget *m_pipeline = nullptr;
    QPushButton          *m_resetOrderBtn = nullptr;
    QLineEdit            *m_dspSearch     = nullptr;
    QScrollArea          *m_dspScrollArea = nullptr;
    QVBoxLayout          *m_dspScrollLay  = nullptr;
    QScrollArea          *m_outerScroll   = nullptr;
    QScrollArea          *m_spatialScroll = nullptr;   // fixed-size spatial panel
    QGroupBox            *m_eqBox         = nullptr;

    // DspStage -> its accordion panel. nullptr for stages whose UI lives
    // outside the accordion (EQ, Spatial and Reverb are in the left
    // column). Used for click-to-navigate and order-reflection.
    ExpandableSection *m_stageSection[SandboxState::Stage_COUNT] = {};

    // Preset manager combos
    QComboBox *m_eqPresetBox      = nullptr;
    QComboBox *m_sandboxPresetBox = nullptr;

    // EQ preset state machine.
    //
    // Track how the user got to the current EQ shape so the combo's
    // own displayed text can communicate:
    //   - Predefined + clean: name of the built-in preset ("Smile").
    //   - Predefined + edited: literal "Personalizzato" (user knows any
    //     change to a built-in produces a personalised shape - clicking
    //     the same built-in name in the combo restores it).
    //   - Custom + clean: name of the user-saved preset.
    //   - Custom + edited: "<name>*" (asterisk = unsaved edits, Save
    //     button prompts overwrite / save-as-new).
    //   - None: "(select preset)".
    //
    // Displayed via the combo's own lineEdit (editable + read-only)
    // so the item list stays intact - the shown text is only a view
    // string over the current selection, never a real item.
    enum class EqPresetType { None, Predefined, Custom };
    EqPresetType m_eqPresetType         = EqPresetType::None;
    int          m_eqPresetPredefinedIdx = -1;
    QString      m_eqPresetCustomName;
    bool         m_eqPresetDirty        = false;

    void onEqBandUserEdited();
    void refreshEqPresetComboText();
};
