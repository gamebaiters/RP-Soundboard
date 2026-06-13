// src/SoundInfo.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "SoundInfo.h"
#include "ColorUtils.h"

#define NAME_PATH "path"
#define NAME_CUSTOM_TEXT "customText"
#define NAME_CUSTOM_COLOR "customColor"
#define NAME_VOLUME "volume"
#define NAME_CROP_ENABLED "cropEnabled"
#define NAME_CROP_START_VALUE "cropStartValue"
#define NAME_CROP_START_UNIT "cropStartUnit"
#define NAME_CROP_STOP_AFTER_AT "cropStopAfterAt"
#define NAME_CROP_STOP_VALUE "cropStopValue"
#define NAME_CROP_STOP_UNIT "cropStopUnit"
#define NAME_FX_PITCH_SPEED "fxPitchSpeed"
#define NAME_FX_REMEMBER "fxRemember"
#define NAME_FX_PITCH "fxPitch"
#define NAME_FX_SPEED "fxSpeed"
#define NAME_FX_REVERB "fxReverb"
#define NAME_FX_SYNC_PITCH_SPEED "fxSyncPitchSpeed"
#define NAME_IS_MACRO "isMacro"
#define NAME_MACRO_STATE "macroState"
#define NAME_IMAGE_PATH "imagePath"
#define NAME_REVERSE "reverse"
#define NAME_AUTO_NORM "autoNormalize"

#define DEFAULT_PATH ""
#define DEFAULT_CUSTOM_TEXT ""
#define DEFAULT_CUSTOM_COLOR "00FFFFFF"
#define DEFAULT_VOLUME 0
#define DEFAULT_CROP_ENABLED false
#define DEFAULT_CROP_START_VALUE 1
#define DEFAULT_CROP_START_UNIT 1
#define DEFAULT_CROP_STOP_AFTER_AT 0
#define DEFAULT_CROP_STOP_VALUE 0
#define DEFAULT_CROP_STOP_UNIT 1
#define DEFAULT_FX_PITCH_SPEED 1.0f
#define DEFAULT_FX_REMEMBER false
#define DEFAULT_FX_PITCH 0
#define DEFAULT_FX_SPEED 0
#define DEFAULT_FX_REVERB 0
#define DEFAULT_FX_SYNC_PITCH_SPEED false
#define DEFAULT_IS_MACRO false


// stringToColor / colorToString live in ColorUtils.h (shared header).


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SoundInfo::SoundInfo() :
	filename(DEFAULT_PATH),
	customText(DEFAULT_CUSTOM_TEXT),
	customColor(stringToColor(DEFAULT_CUSTOM_COLOR)),
	volume(DEFAULT_VOLUME),
	cropEnabled(DEFAULT_CROP_ENABLED),
	cropStartValue(DEFAULT_CROP_START_VALUE),
	cropStartUnit(DEFAULT_CROP_START_UNIT),
	cropStopAfterAt(DEFAULT_CROP_STOP_AFTER_AT),
	cropStopValue(DEFAULT_CROP_STOP_VALUE),
	cropStopUnit(DEFAULT_CROP_STOP_UNIT),
	fxPitchSpeed(DEFAULT_FX_PITCH_SPEED),
	fxRemember(DEFAULT_FX_REMEMBER),
	fxPitch(DEFAULT_FX_PITCH),
	fxSpeed(DEFAULT_FX_SPEED),
	fxReverb(DEFAULT_FX_REVERB),
	fxSyncPitchSpeed(DEFAULT_FX_SYNC_PITCH_SPEED),
	isMacro(DEFAULT_IS_MACRO),
	macroState(),
	imagePath(),
	reverse(false),
	autoNormalize(false)
{

}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundInfo::readFromConfig( const QSettings &settings )
{
	filename = settings.value(NAME_PATH, DEFAULT_PATH).toString();
	customText = settings.value(NAME_CUSTOM_TEXT, DEFAULT_CUSTOM_TEXT).toString();
	customColor = stringToColor(settings.value(NAME_CUSTOM_COLOR, DEFAULT_CUSTOM_COLOR).toString());
	volume = settings.value(NAME_VOLUME, DEFAULT_VOLUME).toInt();
	cropEnabled = settings.value(NAME_CROP_ENABLED, DEFAULT_CROP_ENABLED).toBool();
	cropStartValue = settings.value(NAME_CROP_START_VALUE, DEFAULT_CROP_START_VALUE).toInt();
	cropStartUnit = settings.value(NAME_CROP_START_UNIT, DEFAULT_CROP_START_UNIT).toInt();
	cropStopAfterAt = settings.value(NAME_CROP_STOP_AFTER_AT, DEFAULT_CROP_STOP_AFTER_AT).toInt();
	cropStopValue = settings.value(NAME_CROP_STOP_VALUE, DEFAULT_CROP_STOP_VALUE).toInt();
	cropStopUnit = settings.value(NAME_CROP_STOP_UNIT, DEFAULT_CROP_STOP_UNIT).toInt();
	fxPitchSpeed = settings.value(NAME_FX_PITCH_SPEED, DEFAULT_FX_PITCH_SPEED).toFloat();
	fxRemember = settings.value(NAME_FX_REMEMBER, DEFAULT_FX_REMEMBER).toBool();
	fxPitch = settings.value(NAME_FX_PITCH, DEFAULT_FX_PITCH).toInt();
	fxSpeed = settings.value(NAME_FX_SPEED, DEFAULT_FX_SPEED).toInt();
	fxReverb = settings.value(NAME_FX_REVERB, DEFAULT_FX_REVERB).toInt();
	fxSyncPitchSpeed = settings.value(NAME_FX_SYNC_PITCH_SPEED, DEFAULT_FX_SYNC_PITCH_SPEED).toBool();
	isMacro = settings.value(NAME_IS_MACRO, DEFAULT_IS_MACRO).toBool();
	macroState = settings.value(NAME_MACRO_STATE, QByteArray()).toByteArray();
	imagePath = settings.value(NAME_IMAGE_PATH, QString()).toString();
	reverse       = settings.value(NAME_REVERSE, false).toBool();
	autoNormalize = settings.value(NAME_AUTO_NORM, false).toBool();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundInfo::saveToConfig( QSettings &settings ) const
{
	settings.setValue(NAME_PATH, filename);
	settings.setValue(NAME_CUSTOM_TEXT, customText);
	settings.setValue(NAME_CUSTOM_COLOR, colorToString(customColor));
	settings.setValue(NAME_VOLUME, volume);
	settings.setValue(NAME_CROP_ENABLED, cropEnabled);
	settings.setValue(NAME_CROP_START_VALUE, cropStartValue);
	settings.setValue(NAME_CROP_START_UNIT, cropStartUnit);
	settings.setValue(NAME_CROP_STOP_AFTER_AT, cropStopAfterAt);
	settings.setValue(NAME_CROP_STOP_VALUE, cropStopValue);
	settings.setValue(NAME_CROP_STOP_UNIT, cropStopUnit);
	settings.setValue(NAME_FX_PITCH_SPEED, fxPitchSpeed);
	settings.setValue(NAME_FX_REMEMBER, fxRemember);
	settings.setValue(NAME_FX_PITCH, fxPitch);
	settings.setValue(NAME_FX_SPEED, fxSpeed);
	settings.setValue(NAME_FX_REVERB, fxReverb);
	settings.setValue(NAME_FX_SYNC_PITCH_SPEED, fxSyncPitchSpeed);
	settings.setValue(NAME_IS_MACRO, isMacro);
	settings.setValue(NAME_MACRO_STATE, macroState);
	settings.setValue(NAME_IMAGE_PATH, imagePath);
	settings.setValue(NAME_REVERSE, reverse);
	settings.setValue(NAME_AUTO_NORM, autoNormalize);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
double SoundInfo::getStartTime() const
{
	if(!cropEnabled)
		return 0.0;
	return (double)cropStartValue * getTimeUnitFactor(cropStartUnit);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
double SoundInfo::getPlayTime() const
{
	if(!cropEnabled)
		return -1.0;
	double t = (double)cropStopValue * getTimeUnitFactor(cropStopUnit);
	if(cropStopAfterAt == 1) //stop AT x seconds instead of AFTER?
		t -= getStartTime();
	return std::max(t, 0.0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
double SoundInfo::getTimeUnitFactor(int unit)
{
	switch(unit)
	{
	case 0: return 0.001;
	case 1: return 1.0;
	default:
		// Unknown unit index (corrupt config) — fall back to seconds
		// instead of throwing, which would crash the plugin from the
		// audio path with no recoverable error.
		return 1.0;
	}
}
