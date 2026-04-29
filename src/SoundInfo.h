// src/SoundInfo.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__SoundInfo_H__
#define rpsbsrc__SoundInfo_H__

#include <QSettings>
#include <QColor>
#include <stdexcept>

class SoundInfo
{
public:
	SoundInfo();
	void readFromConfig(const QSettings &settings);
	void saveToConfig(QSettings &settings) const;
	double getStartTime() const;
	double getPlayTime() const;

	static double getTimeUnitFactor(int unit);
	bool customColorEnabled() const { return customColor.alpha() != 0; }
	void setCustomColorEnabled(bool enabled) { customColor.setAlpha(enabled ? 255 : 0); }

public:
	QString filename;
	QString customText;
	QColor customColor;
	int volume;
	bool cropEnabled;
	int cropStartValue;
	int cropStartUnit;
	int cropStopAfterAt;
	int cropStopValue;
	int cropStopUnit;
	float fxPitchSpeed;   // legacy (kept for config compat)
	bool fxRemember;
	int fxPitch;          // -100..100 slider value
	int fxSpeed;          // -100..100 slider value
	int fxReverb;         // 0..100 slider value
	bool fxSyncPitchSpeed;
};

#endif // rpsbsrc__SoundInfo_H__
