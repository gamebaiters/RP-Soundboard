#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class ChannelMeter;
class ChannelSandboxDialog;

// Pinned "Microphone" channel (V1/V2).
//
// A reduced channel row that edits the LIVE mic chain instead of a
// playback slot: master toggle (with LIVE affordance), in/out level
// meter, live pitch slider, one-click voice preset combo, monitor
// checkbox and an "Effects..." button that opens the sandbox dialog
// in micMode. Fully self-contained - it talks directly to the MicFx
// singleton, so mounting it requires zero wiring.
//
// Visibility is driven two ways:
//   - the mic button next to "+ Channel" toggles the row (persisted
//     under micfx/panel_visible);
//   - the Settings "Enable Mic FX feature" master switch hides BOTH
//     the row and the toggle button (handled by MainPage).
class MicChannel : public QWidget
{
    Q_OBJECT
public:
    explicit MicChannel(QWidget *parent = nullptr);

    // Re-apply theme-derived inline styles (parallels Channel::refreshTheme).
    void refreshTheme();

private slots:
    void onEnableToggled(bool on);
    void onPresetPicked(int index);
    void onOpenEffects();
    void onMeterTick();

private:
    void pullFromMicFx();     // sync widgets <- MicFx state
    void updateLiveBadge();
    // Custom voice presets (save / delete / share) — mirrors the EQ preset
    // controls in the audio sandbox. A mic preset = the mic sandbox state +
    // live pitch, serialised to Base64.
    void rebuildPresetCombo();          // built-ins + saved user presets
    QString serialiseMicState() const;  // Base64 of {pitch, sandbox}
    void applyMicStateFromData(const QString &base64);
    void onSavePreset();
    void onDeletePreset();
    void onSharePreset();               // copy "GBSB4-MIC:<b64>" to clipboard
    void onPastePreset();               // apply from clipboard

    QCheckBox   *m_enable      = nullptr;
    QLabel      *m_liveBadge   = nullptr;
    ChannelMeter *m_meter      = nullptr;
    QSlider     *m_pitch       = nullptr;
    QLabel      *m_pitchLabel  = nullptr;
    QSlider     *m_reverb      = nullptr;
    QLabel      *m_reverbLabel = nullptr;
    QComboBox   *m_presetBox   = nullptr;
    QPushButton *m_presetSave  = nullptr;
    QPushButton *m_presetDelete= nullptr;
    QPushButton *m_presetShare = nullptr;
    QPushButton *m_presetPaste = nullptr;
    QCheckBox   *m_monitor     = nullptr;
    QPushButton *m_fxBtn       = nullptr;
    QTimer      *m_meterTimer  = nullptr;
    ChannelSandboxDialog *m_dialog = nullptr;
    bool m_loading = false;
};
