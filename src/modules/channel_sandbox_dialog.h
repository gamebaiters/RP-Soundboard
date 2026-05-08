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
class PositionalPad;

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

signals:
    void stateChanged(const SandboxState &s);
    void resetRequested(int channelId);

private slots:
    void onEnableToggled(bool on);
    void onModeChanged(int idx);
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

private:
    void buildUi();
    void pushStateToWidgets();
    void pushChange();
    void refreshTitle();
    void applyModeVisibility();
    void load8DPreset();           // populates state for 8D mode
    int  modeForDropdown(int idx) const;     // dropdown index -> SandboxState mode
    int  dropdownForMode(int mode) const;    // SandboxState mode -> dropdown index

    int m_channelId;
    QString m_channelTitle;
    SandboxState m_state;
    bool m_loading = false;

    QCheckBox     *m_enable      = nullptr;
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

    // Per-mode rows (whole rows that get hidden together).
    class QWidget *m_padContainer = nullptr;
    class QWidget *m_distRow      = nullptr;
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
    QVector<QSlider*> m_eqSliders;
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
    QPushButton *m_resetLimiter = nullptr;

    // Pipeline widget
    class PipelineWidget *m_pipeline = nullptr;

    // Preset manager combos
    QComboBox *m_eqPresetBox      = nullptr;
    QComboBox *m_sandboxPresetBox = nullptr;
};
