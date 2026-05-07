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
};
