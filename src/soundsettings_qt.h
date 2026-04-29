// src/soundsettings_qt.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__soundsettings_qt_H__
#define rpsbsrc__soundsettings_qt_H__


#include <QDialog>
#include <QCloseEvent>
#include <QIcon>
#include <QTimer>
#include <QSlider>
#include <QLabel>
#include <QGroupBox>
#include "SoundInfo.h"

class ConfigModel;
class SoundView;

namespace Ui {
	class SoundSettingsQt;
}


class SoundSettingsQt: public QDialog
{
	Q_OBJECT

public:
	explicit SoundSettingsQt(const SoundInfo &soundInfo, size_t buttonId, QWidget *parent = 0);
	~SoundSettingsQt();
	const SoundInfo &getSoundInfo() const { return m_soundInfo; }

protected:
	void done(int r);

private slots:
	void onVolumeChanged(int value);
	void onBrowsePressed();
	void onPreviewPressed();
	void onTimer();
	void onHotkeyChangePressed();
	void updateHotkeyText();
	void onColorEnabledPressed();
	void onChooseColorPressed();
	void updateSoundView();
	void onFxPitchChanged(int value);
	void onFxSpeedChanged(int value);
	void onFxCombinedChanged(int value);
	void onFxReverbChanged(int value);
	void onFxSyncToggled(bool checked);
	void onFxReset();

private:
	void initGui(const SoundInfo &sound);
	void fillFromGui(SoundInfo &sound);
	void updateFxLabels();
	void applyFxToPreview();
	int getPreviewSlot();

private:
	Ui::SoundSettingsQt *ui;
	SoundInfo m_soundInfo;
	size_t m_buttonId;
	QIcon m_iconPlay;
	QIcon m_iconStop;
	QTimer *m_timer;
	SoundView *m_soundview;
	QColor customColor;

	// Per-song FX (inside a checkable QGroupBox like Crop Sound)
	QGroupBox *m_fxGroup;
	QSlider *m_fxPitchSlider;
	QSlider *m_fxSpeedSlider;
	QSlider *m_fxCombinedSlider;
	QSlider *m_fxReverbSlider;
	QLabel *m_fxPitchLabel;
	QLabel *m_fxSpeedLabel;
	QLabel *m_fxCombinedLabel;
	QLabel *m_fxReverbLabel;
	QPushButton *m_fxSyncButton;
	QPushButton *m_fxResetButton;
};


#endif