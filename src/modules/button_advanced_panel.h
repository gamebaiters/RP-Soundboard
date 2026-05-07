// ButtonAdvancedPanel - per-button advanced options as a SEPARATE window
// (QDialog). Replaces the inline soundsettings_qt dialog. Reuses FxPanel
// for pitch/speed/reverb so the same module powers both channel + button.
//
// Sections:
//   * File (path + browse + waveform + preview button)
//   * Display (custom text + custom color)
//   * Volume modifier
//   * Crop (enabled + start + stop after/at)
//   * FX (FxPanel) - always applied when this button is clicked
//   * Hotkey
//
// Per-button FX have no "remember" toggle: they ARE the playback effects.
// Macros are NOT a per-button concept here - they're created via the
// button-grid right-click menu (Freeze all channels into macro). The old
// inline macro section was a footgun, removed.

#pragma once

#include <QDialog>
#include "../SoundInfo.h"

class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QTimer;
class QGroupBox;
class FxPanel;
class SoundView;

class ButtonAdvancedPanel : public QDialog {
    Q_OBJECT
public:
    explicit ButtonAdvancedPanel(QWidget *parent = nullptr);

    void                setSoundInfo(const SoundInfo &info);
    SoundInfo           soundInfo() const;
    void                setHotkeyText(const QString &shortcut);
    // Mirror the global "Enable custom FX" switch into this dialog so the
    // FX group disappears when the master toggle is off.
    void                setGlobalFxEnabled(bool on);

signals:
    void hotkeyAssignRequested();                          // opens TS3 hotkey dlg
    void hotkeyResetRequested();                           // clears the binding
    void soundInfoAccepted(const SoundInfo &info);         // OK pressed

private slots:
    void onBrowse();
    void onPickColor();
    void onAccepted();
    void onPreview();
    void onPreviewTimer();

protected:
    void closeEvent(class QCloseEvent *e) override;
    void done(int r) override;

private:
    void writeBackToInfo();
    void refreshSoundView();
    void pushLiveFxToPreview();
    void stopPreview();

    SoundInfo    m_info;

    QLineEdit   *m_filePath;
    QPushButton *m_browse;
    SoundView   *m_soundView;
    QPushButton *m_preview;
    QLabel      *m_previewTimeLabel;
    QTimer      *m_previewTimer;
    int          m_previewSlot = -1;

    QLineEdit   *m_customText;
    QCheckBox   *m_customColorEnabled;
    class QFrame *m_colorSwatch;     // chip showing the currently picked color
    QPushButton *m_pickColor;
    void refreshColorSwatch();
    QLineEdit   *m_imagePath;
    QPushButton *m_imageBrowse;
    QPushButton *m_imageClear;

    QSlider     *m_volume;
    QLabel      *m_volumeLabel;

    QGroupBox   *m_cropGroup;        // checkable - when off, crop is ignored
    QSpinBox    *m_cropStart;
    QComboBox   *m_cropStartUnit;
    QComboBox   *m_cropStopMode;     // "after" / "at"
    QSpinBox    *m_cropStop;
    QComboBox   *m_cropStopUnit;

    QGroupBox   *m_fxGroup;          // checkable - when off, per-button FX
                                     // are ignored at playback / preview
    FxPanel     *m_fx;

    QPushButton *m_hotkeyBtn;
    QPushButton *m_hotkeyReset;

    QPushButton *m_ok;
    QPushButton *m_cancel;
};
