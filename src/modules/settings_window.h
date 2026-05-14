// SettingsWindow - separate top-level window for global options. Each category
// is wrapped in an ExpandableSection that collapses with an arrow click.

#pragma once

#include <QDialog>
#include <QColor>

class QCheckBox;
class QSpinBox;
class QPushButton;
class QGroupBox;
class ExpandableSection;

class SettingsWindow : public QDialog {
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

    // current values (read these on show, set them on apply)
    bool earrapeProtection() const;
    bool linkVolumes() const;
    bool rememberPitchSpeed() const;
    bool restoreSession() const;
    bool globalFxEnabled() const;
    bool hideWaveform() const;
    bool logsEnabled() const;
    bool audioSandboxEnabled() const;
    bool audioMeterVisible() const;
    bool audioExportEnabled() const;
    int  activeProfile() const;
    bool themeEnabled() const;
    QColor themeAccent() const;
    QColor themeWaveform() const;
    QColor themeBackground() const;
    int    themeContrast() const;
    QColor themeText() const;
    QColor themeButton() const;
    bool multiSoundboard() const;
    bool muteLocally() const;
    bool muteMyself() const;
    bool showHotkeysOnButtons() const;
    bool disableHotkeys() const;
    int  rows() const;
    int  cols() const;
    bool adaptWaveformToFx() const;

    // Reset behaviour flags
    bool resetChVolume() const;
    bool resetChFx() const;
    bool resetChFile() const;
    bool resetChSandbox() const;
    bool resetAllRemoveExtra() const;
    bool resetAllVolume() const;
    bool resetAllFx() const;
    bool resetAllFiles() const;
    bool resetAllSandbox() const;

public slots:
    void setEarrapeProtection(bool on);
    void setLinkVolumes(bool on);
    void setRememberPitchSpeed(bool on);
    void setRestoreSession(bool on);
    void setGlobalFxEnabled(bool on);
    void setHideWaveform(bool on);
    void setLogsEnabled(bool on);
    void setAudioSandboxEnabled(bool on);
    void setAudioMeterVisible(bool on);
    void setAudioExportEnabled(bool on);
    void setActiveProfile(int profile);
    void setTheme(bool enabled, const QColor &accent, const QColor &waveform, const QColor &background, int contrast, const QColor &text, const QColor &button);
    void setMultiSoundboard(bool on);
    void setMuteLocally(bool on);
    void setMuteMyself(bool on);
    void setShowHotkeysOnButtons(bool on);
    void setDisableHotkeys(bool on);
    void setRows(int r);
    void setCols(int c);
    void setAdaptWaveformToFx(bool on);

    void setResetChVolume(bool on);
    void setResetChFx(bool on);
    void setResetChFile(bool on);
    void setResetChSandbox(bool on);
    void setResetAllRemoveExtra(bool on);
    void setResetAllVolume(bool on);
    void setResetAllFx(bool on);
    void setResetAllFiles(bool on);
    void setResetAllSandbox(bool on);

signals:
    void earrapeProtectionChanged(bool);
    void linkVolumesChanged(bool);
    void rememberPitchSpeedChanged(bool);
    void restoreSessionChanged(bool);
    void globalFxEnabledChanged(bool);
    void hideWaveformChanged(bool);
    void logsEnabledChanged(bool);
    void audioSandboxEnabledChanged(bool);
    void audioMeterVisibleChanged(bool);
    void audioExportEnabledChanged(bool);
    void resetAllAudioSandboxRequested();
    void activeProfileChanged(int profile);
    void exportProfileRequested(int profile);
    void importProfileRequested(int profile);
    void themeChanged(bool enabled, const QColor &accent, const QColor &waveform, const QColor &background, int contrast, const QColor &text, const QColor &button);
    void themeResetRequested();
    void themeCopyRequested();
    void themePasteRequested();
    void multiSoundboardChanged(bool);
    void muteLocallyChanged(bool);
    void muteMyselfChanged(bool);
    void showHotkeysOnButtonsChanged(bool);
    void disableHotkeysChanged(bool);
    void rowsChanged(int);
    void colsChanged(int);
    void exportRequested();
    void importRequested();
    void resetAllHotkeysRequested();
    void adaptWaveformToFxChanged(bool);

    void resetChVolumeChanged(bool);
    void resetChFxChanged(bool);
    void resetChFileChanged(bool);
    void resetChSandboxChanged(bool);
    void resetAllRemoveExtraChanged(bool);
    void resetAllVolumeChanged(bool);
    void resetAllFxChanged(bool);
    void resetAllFilesChanged(bool);
    void resetAllSandboxChanged(bool);

private:
    QCheckBox   *m_earrape;
    QCheckBox   *m_linkVolumes;
    QCheckBox   *m_rememberFx;
    QCheckBox   *m_restoreSession;
    QCheckBox   *m_globalFx;
    QCheckBox   *m_hideWaveform;
    QCheckBox   *m_logsEnabled;
    QCheckBox   *m_sandboxEnabled = nullptr;
    QCheckBox   *m_meterVisible   = nullptr;
    QCheckBox   *m_exportEnabled  = nullptr;
    QPushButton *m_resetAllSandboxBtn = nullptr;
    class QComboBox *m_profileCombo;
    QPushButton *m_profileExport;
    QPushButton *m_profileImport;
    class QGroupBox *m_themeGroup;
    QPushButton *m_themeAccentBtn;
    QPushButton *m_themeWaveBtn;
    QPushButton *m_themeBgBtn;
    QPushButton *m_themeTextBtn;
    QPushButton *m_themeTextAuto;
    QPushButton *m_themeButtonBtn;
    QPushButton *m_themeButtonAuto;
    QPushButton *m_themeResetBtn;
    QPushButton *m_themeCopyBtn;
    QPushButton *m_themePasteBtn;
    QColor       m_themeAccent;
    QColor       m_themeWaveform;
    QColor       m_themeBackground;
    QColor       m_themeText;       // invalid -> auto
    QColor       m_themeButton;     // invalid -> auto
    int          m_themeContrast;
    class QSlider *m_themeContrastSlider;
    class QLabel  *m_themeContrastLabel;
    QCheckBox   *m_multi;
    QCheckBox   *m_muteLocally;
    QCheckBox   *m_muteMyself;
    QCheckBox   *m_showHotkeys;
    QCheckBox   *m_disableHotkeys;
    QSpinBox    *m_rows;
    QSpinBox    *m_cols;
    QPushButton *m_export;
    QPushButton *m_import;
    QPushButton *m_resetHotkeys;
    QPushButton *m_close;
    QCheckBox   *m_adaptWaveform = nullptr;

    // Reset behaviour
    QCheckBox   *m_resetChVolume     = nullptr;
    QCheckBox   *m_resetChFx         = nullptr;
    QCheckBox   *m_resetChFile       = nullptr;
    QCheckBox   *m_resetChSandbox    = nullptr;
    QCheckBox   *m_resetAllRemoveExtra = nullptr;
    QCheckBox   *m_resetAllVolume    = nullptr;
    QCheckBox   *m_resetAllFx        = nullptr;
    QCheckBox   *m_resetAllFiles     = nullptr;
    QCheckBox   *m_resetAllSandbox   = nullptr;
};
