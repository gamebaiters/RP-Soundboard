// src/config_qt.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__config_qt_H__
#define rpsbsrc__config_qt_H__

#include <QDialog>
#include <QWidget>
#include <QGridLayout>
#include <QPushButton>
#include <QBoxLayout>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QList>
#include <QUrl>
#include <QRadioButton>

#include <QSlider>
#include <QLabel>
#include <QCheckBox>

#include "ui_config_qt.h"
#include "ConfigModel.h"

class SpeechBubble;
class ExpandableSection;
class SoundButton;
class SoundView;

struct PlaybackBar
{
	int slot;
	QFrame *frame;
	QPushButton *stopButton;
	QPushButton *pauseButton;
	QLabel *filenameLabel;
	QLabel *timeLabel;
	SoundView *waveformView;
	QSlider *volumeLocalSlider;
	QSlider *volumeRemoteSlider;
	QLabel *volumeLocalLabel;
	QLabel *volumeRemoteLabel;
	QPushButton *linkVolumesButton;
	bool linked;
	// Per-slot FX
	QSlider *pitchSlider;
	QSlider *speedSlider;
	QSlider *combinedSlider;
	QSlider *reverbSlider;
	QLabel *pitchLabel;
	QLabel *speedLabel;
	QLabel *combinedLabel;
	QLabel *reverbLabel;
	QCheckBox *syncCheckbox;
	QPushButton *resetFxButton;
	// Skip buttons
	QPushButton *skipBack10;
	QPushButton *skipBack5;
	QPushButton *skipFwd5;
	QPushButton *skipFwd10;
};

namespace Ui {
	class ConfigQt;
}

class ConfigQt : public QWidget
{
	Q_OBJECT

public:
	explicit ConfigQt(ConfigModel *model, QWidget *parent = 0);

	void createBubbles();

	virtual ~ConfigQt();

	static QString getShortcutString(const char *internalName);
	static QString getShortcutString(size_t buttonId);
    static QString getConfigShortcutString(int cfg);
	static void openHotkeySetDialog(size_t buttonId, QWidget *parent);
	void onHotkeyRecordedEvent(const char *keyword, const char *key);

	// Detach from ConfigModel so this hidden window stops rebuilding its
	// shadow grid on every notification when the new MainPage UI is the
	// visible one. Idempotent.
	void detachFromModel();

    void setConfiguration(int cfg);
    bool hotkeysEnabled();

protected:
	virtual void closeEvent(QCloseEvent * evt) override;
	virtual void showEvent(QShowEvent *evt) override;

private slots:
	void onSkipBack10();
	void onSkipBack5();
	void onSkipFwd5();
	void onSkipFwd10();
	void onProgressSliderMoved(int value);
	void onProgressSliderPressed();
	void onProgressSliderReleased();
	void onLinkVolumesChanged(bool checked);
	void onClickedPlay();
	void onClickedStop();
	void onUpdateVolumeLocal(int val);
	void onUpdateVolumeRemote(int val);
	void onUpdateMuteLocally(bool val);
	void onUpdateCols(int val);
	void onUpdateRows(int val);
	void onUpdateMuteMyself(bool val);
	void showButtonContextMenu(const QPoint &point);
	void onStopBubbleFinished();
	void onButtonBubbleFinished();
	void onColsBubbleFinished();
	void showStopButtonContextMenu(const QPoint &point);
	void showPauseButtonContextMenu(const QPoint &point);
	void onStartPlayingSound(int slot, bool preview, QString filename);
	void onStopPlayingSound(int slot);
	void onPausePlayingSound(int slot);
	void onUnpausePlayingSound(int slot);
	void onPlayingIconTimer();
	void onUpdateShowHotkeysOnButtons(bool val);
	void onUpdateHotkeysDisabled(bool val);
	void onButtonFileDropped(const QList<QUrl> &urls);
	void onButtonPausePressed();
	void onButtonDroppedOnButton(SoundButton *button);
	void onFilterEditTextChanged(const QString &filter);
	void onVolumeSliderContextMenuLocal(const QPoint &point);
	void onVolumeSliderContextMenuRemote(const QPoint &point);
	void onWaveformSeek(double fraction);
	void onEarrapeProtectionChanged(bool checked);
	void onPitchValueChanged(int value);
	void onSpeedValueChanged(int value);
	void onCombinedValueChanged(int value);
	void onSyncToggled(bool checked);
	void onResetFx();
	void onRememberPitchSpeedChanged(bool checked);
	void onReverbValueChanged(int value);

    void onSetConfig();
    void onConfigHotkey();


    void onSaveModel();
    void onLoadModel();

signals:
	void hotkeyRecordedEvent(QString keyword, QString key);

private:
	void showSetHotkeyMenu(const char *hotkeyName, const QPoint &point);
	void setPlayingLabelIcon(int index);
	void playSound(size_t buttonId);
	void chooseFile(size_t buttonId);

	void setButtonFile(size_t buttonId, const QString &fn, bool askForDisablingCrop = true);

	void openAdvanced(size_t buttonId);
	void deleteButton(size_t buttonId);
	void createButtons();
	void createConfigButtons();
	void updateButtonText(int i);
	void openHotkeySetDialog(size_t buttonId);
	void openButtonColorDialog(size_t buttonId);
	QString unescapeCustomText(const QString &text);

	class ModelObserver : public ConfigModel::Observer
	{
	public:
		ModelObserver(ConfigQt &parent) : p(parent) {}
		void notify(ConfigModel &model, ConfigModel::notifications_e what, int data) override;
	private:
		ConfigQt &p;
	};

	Ui::ConfigQt *ui;
	std::vector<SoundButton*> m_buttons;
	ConfigModel *m_model;
	QBoxLayout *m_configArea;
	ModelObserver m_modelObserver;
	QMenu m_buttonContextMenu;
	QPointer<SpeechBubble> m_buttonBubble;
	QAction *actSetHotkey;
    ExpandableSection *settingsSection;
    ExpandableSection *configsSection;
    QTimer *playingIconTimer;
	int playingIconIndex;
	QIcon m_pauseIcon;
	QIcon m_playIcon;
	bool m_sliderPressed = false;
	std::array<QRadioButton*, NUM_CONFIGS> m_configRadioButtons;
	std::array<QPushButton*, NUM_CONFIGS> m_configHotkeyButtons;

	// Multi-soundboard playback bars
	QVBoxLayout *m_multiBarContainer;
	std::vector<PlaybackBar*> m_playbackBars;  // dynamic bars for slots 1-4
	PlaybackBar *createPlaybackBar(int slot, const QString &filename);
	void removePlaybackBar(int slot);
	void updateAllPlaybackBars();
	void onMultiModeChanged(bool enabled);
	void addVolumeSliders(PlaybackBar *bar);

	// Slot-0 volume controls (shown in main bar when multi-mode is on)
	QWidget *m_slot0VolumeWidget;
	QSlider *m_slot0VolumeLocal;
	QSlider *m_slot0VolumeRemote;
	QLabel *m_slot0VolLocalLabel;
	QLabel *m_slot0VolRemoteLabel;

	// Pitch/Speed/Combined sliders
	QSlider *m_pitchSlider;
	QSlider *m_speedSlider;
	QSlider *m_combinedSlider;
	QLabel *m_pitchValueLabel;
	QLabel *m_speedValueLabel;
	QLabel *m_combinedValueLabel;
	QSlider *m_reverbSlider;
	QLabel *m_reverbValueLabel;
	QPushButton *m_syncButton;
	QPushButton *m_resetFxButton;
	// Remembered values when switching between individual/combined mode
	int m_rememberedPitchValue = 0;
	int m_rememberedSpeedValue = 0;
	int m_rememberedCombinedValue = 0;
	void buildPitchSpeedUI();
	void updatePitchSpeedLabels();
};

#endif // rpsbsrc__config_qt_H__
