// src/ConfigModel.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "common.h"

#include <QSettings>

#include "ConfigModel.h"
#include "main.h"
#include "TalkStateManager.h"
#include "buildinfo.h"
#include "plugin.h"



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
ConfigModel::ConfigModel()
{
	m_rows.fill(2);
	m_cols.fill(5);
	m_volumeLocal = 80;
	m_volumeRemote = 80;
	m_playbackLocal = true;
	m_muteMyselfDuringPb = false;
	m_windowWidth = 600;
	m_windowHeight = 240;

	m_bubbleButtonsBuild = 0;
	m_bubbleStopBuild = 0;
	m_bubbleColsBuild = 0;

	m_showHotkeysOnButtons = false;
	m_hotkeysEnabled = true;
	m_linkVolumes = false;
	m_earrapeProtection = false;
	m_pitchSpeedValue = 100;
	m_rememberPitchSpeed = false;
	m_restoreSession = false;
	m_globalFxEnabled = true;
	m_hideWaveform = false;
	m_themeEnabled = false;
	m_themeAccent = QStringLiteral("#4a90e2");
	m_themeWaveform = QStringLiteral("#4a90e2");
	m_themeBackground = QStringLiteral("#2b2b2b");
	m_themeContrast = 50;
	m_themeText = QString();
	m_themeButton = QString();
	m_pitchValue = 0;
	m_speedValue = 0;
	m_syncPitchSpeed = false;
	m_reverbValue = 0;
	m_multiSoundboard = false;
	m_logsEnabled = false;
	m_previewOnly = false;
	m_audioSandboxEnabled = true;
	m_audioMeterVisible = true;

    m_activeConfig = 0;
	m_nextUpdateCheck = 0;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::readConfig(const QString &file)
{
    QString path;
    if (file.isEmpty())
        path = GetFullConfigPath();
    else
        path = file;

    QSettings settings(path, QSettings::IniFormat);

	for (int i = 0; i < NUM_CONFIGS; i++)
		m_sounds[i] = readConfiguration(settings, i == 0 ? QString("files") : QString("files%1").arg(i+1));

	for (int i = 0; i < NUM_CONFIGS; i++)
	{
		m_rows[i] = settings.value(i == 0 ? QString("num_rows") : QString("num_rows%1").arg(i + 1), i == 0 ? 2 : m_rows[0]).toInt();
		m_cols[i] = settings.value(i == 0 ? QString("num_cols") : QString("num_cols%1").arg(i + 1), i == 0 ? 5 : m_cols[0]).toInt();
	}
	int volume_old = settings.value("volume", 50).toInt();
	m_volumeLocal = settings.value("volumeLocal", volume_old).toInt();
	m_volumeRemote = settings.value("volumeRemote", volume_old).toInt();
	m_playbackLocal = settings.value("playback_local", true).toBool();
	m_muteMyselfDuringPb = settings.value("mute_myself_during_pb", false).toBool();
	m_windowWidth = settings.value("window_width", 600).toInt();
	m_windowHeight = settings.value("window_height", 240).toInt();
	m_bubbleButtonsBuild = settings.value("bubble_buttons_build", 0).toInt();
	m_bubbleStopBuild = settings.value("bubble_stop_build", 0).toInt();
	m_bubbleColsBuild = settings.value("bubble_cols_build", 0).toInt();
	m_showHotkeysOnButtons = settings.value("show_hotkeys_on_buttons", false).toBool();
	m_hotkeysEnabled = settings.value("hotkeys_enabled", true).toBool();
	m_linkVolumes = settings.value("link_volumes", false).toBool();
	m_earrapeProtection = settings.value("earrape_protection", false).toBool();
	m_rememberPitchSpeed = settings.value("remember_pitch_speed", false).toBool();
	m_restoreSession = settings.value("restore_session", false).toBool();
	m_globalFxEnabled = settings.value("global_custom_fx", true).toBool();
	m_hideWaveform    = settings.value("hide_waveform", false).toBool();
	m_themeEnabled    = settings.value("theme_enabled", false).toBool();
	m_themeAccent     = settings.value("theme_accent", "#4a90e2").toString();
	m_themeWaveform   = settings.value("theme_waveform", "#4a90e2").toString();
	m_themeBackground = settings.value("theme_background", "#2b2b2b").toString();
	m_themeContrast   = settings.value("theme_contrast", 50).toInt();
	m_themeText       = settings.value("theme_text", "").toString();
	m_themeButton     = settings.value("theme_button", "").toString();
	m_pitchSpeedValue = m_rememberPitchSpeed ? settings.value("pitch_speed_value", 100).toInt() : 100;
	m_pitchValue = m_rememberPitchSpeed ? settings.value("pitch_value", 0).toInt() : 0;
	m_speedValue = m_rememberPitchSpeed ? settings.value("speed_value", 0).toInt() : 0;
	m_syncPitchSpeed = settings.value("sync_pitch_speed", false).toBool();
	m_reverbValue = m_rememberPitchSpeed ? settings.value("reverb_value", 0).toInt() : 0;
	m_multiSoundboard = settings.value("multi_soundboard", false).toBool();
	m_logsEnabled = settings.value("logs_enabled", false).toBool();
	m_previewOnly = settings.value("preview_only", false).toBool();
	m_audioSandboxEnabled = settings.value("audio_sandbox_enabled", true).toBool();
	m_audioMeterVisible   = settings.value("audio_meter_visible", true).toBool();
	m_nextUpdateCheck = settings.value("next_update_check", 0).toUInt();

	// Propagate the logging gate to the C-land writers immediately so
	// subsequent rpsb_debug.log calls honor the saved preference even
	// before the user opens Settings this session.
	g_rpsbLogsEnabled = m_logsEnabled ? 1 : 0;
	g_rpsbPreviewOnly = m_previewOnly ? 1 : 0;

	notifyAllEvents();
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::writeConfig(const QString &file)
{
    QString path;
    if (file.isEmpty())
        path = GetFullConfigPath();
    else
        path = file;

    QSettings settings(path, QSettings::IniFormat);

    settings.setValue("config_build", buildinfo_getBuildNumber());
    settings.setValue("volumeLocal", m_volumeLocal);
    settings.setValue("volumeRemote", m_volumeRemote);
    settings.setValue("playback_local", m_playbackLocal);
    settings.setValue("mute_myself_during_pb", m_muteMyselfDuringPb);
    settings.setValue("window_width", m_windowWidth);
    settings.setValue("window_height", m_windowHeight);
    settings.setValue("bubble_buttons_build", m_bubbleButtonsBuild);
    settings.setValue("bubble_stop_build", m_bubbleStopBuild);
    settings.setValue("bubble_cols_build", m_bubbleColsBuild);
    settings.setValue("show_hotkeys_on_buttons", m_showHotkeysOnButtons);
	settings.setValue("hotkeys_enabled", m_hotkeysEnabled);
	settings.setValue("link_volumes", m_linkVolumes);
	settings.setValue("earrape_protection", m_earrapeProtection);
	settings.setValue("remember_pitch_speed", m_rememberPitchSpeed);
	settings.setValue("restore_session", m_restoreSession);
	settings.setValue("global_custom_fx", m_globalFxEnabled);
	settings.setValue("hide_waveform", m_hideWaveform);
	settings.setValue("theme_enabled", m_themeEnabled);
	settings.setValue("theme_accent", m_themeAccent);
	settings.setValue("theme_waveform", m_themeWaveform);
	settings.setValue("theme_background", m_themeBackground);
	settings.setValue("theme_contrast", m_themeContrast);
	settings.setValue("theme_text", m_themeText);
	settings.setValue("theme_button", m_themeButton);
	settings.setValue("pitch_speed_value", m_pitchSpeedValue);
	settings.setValue("pitch_value", m_pitchValue);
	settings.setValue("speed_value", m_speedValue);
	settings.setValue("sync_pitch_speed", m_syncPitchSpeed);
	settings.setValue("reverb_value", m_reverbValue);
	settings.setValue("multi_soundboard", m_multiSoundboard);
	settings.setValue("logs_enabled", m_logsEnabled);
	settings.setValue("preview_only", m_previewOnly);
	settings.setValue("audio_sandbox_enabled", m_audioSandboxEnabled);
	settings.setValue("audio_meter_visible", m_audioMeterVisible);
	settings.setValue("next_update_check", m_nextUpdateCheck);

	for (int i = 0; i < NUM_CONFIGS; i++)
		writeConfiguration(settings, i == 0 ? QString("files") : QString("files%1").arg(i + 1), m_sounds[i]);
	for (int i = 0; i < NUM_CONFIGS; i++)
	{
		settings.setValue(i == 0 ? QString("num_rows") : QString("num_rows%1").arg(i + 1), m_rows[i]);
		settings.setValue(i == 0 ? QString("num_cols") : QString("num_cols%1").arg(i + 1), m_cols[i]);
	}
}


std::vector<SoundInfo> ConfigModel::readConfiguration(QSettings &settings, const QString &name)
{
	std::vector<SoundInfo> sounds;
    int size = settings.beginReadArray(name);
    if (size == 0)
        sounds = getInitialSounds();
    else
    {
        sounds.resize(size);
        for (int i = 0; i < size; i++)
        {
            settings.setArrayIndex(i);
            sounds[i].readFromConfig(settings);
        }
    }
    settings.endArray();
	return sounds;
}


void ConfigModel::writeConfiguration(QSettings & settings, const QString &name, const std::vector<SoundInfo> &sounds)
{
    settings.beginWriteArray(name);
    for (int i = 0; i < (int)sounds.size(); i++)
    {
        settings.setArrayIndex(i);
        sounds[i].saveToConfig(settings);
    }
    settings.endArray();
}


void ConfigModel::setConfiguration(int config)
{
	m_activeConfig = config;

    /* Tell observers that our data changed */
    notifyAllEvents();
}

int ConfigModel::getConfiguration()
{
	return m_activeConfig;
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigModel::getFileName( int itemId ) const
{
    if(itemId >= 0 && itemId < numSounds())
		return sounds()[itemId].filename;
	return QString();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setFileName( int itemId, const QString &fn )
{
	if(itemId >= 0)
	{
        if(itemId < 1000 && itemId >= numSounds())
			sounds().resize(itemId + 1);
		sounds()[itemId].filename = fn;
		writeConfig();
		notify(NOTIFY_SET_SOUND, itemId);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
const SoundInfo *ConfigModel::getSoundInfo(int itemId) const
{
    if(itemId >= 0 && itemId < numSounds())
		return &sounds()[itemId];
	return NULL;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setSoundInfo( int itemId, const SoundInfo &info )
{
    if(itemId < 1000 && itemId >= numSounds())
		sounds().resize(itemId + 1);
	sounds()[itemId] = info;
	writeConfig();
	notify(NOTIFY_SET_SOUND, itemId);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigModel::GetConfigPath()
{
	// Find config path for config class
	char* configPath = (char*)malloc(PATH_BUFSIZE);
	ts3Functions.getConfigPath(configPath, PATH_BUFSIZE);
	return QString::fromUtf8(configPath);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigModel::GetFullConfigPath()
{
	QString fullPath = GetConfigPath();
	QChar last = fullPath[fullPath.count() - 1];
	if (last != '/' && last != '\\')
		fullPath.append('/');
	fullPath.append("rp_soundboard.ini");
	return fullPath;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setRows( int n )
{
	m_rows[m_activeConfig] = n;
	writeConfig();
	notify(NOTIFY_SET_ROWS, n);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setCols( int n )
{
    //if (n > m_cols)
    //{
    //    for (int i = m_rows - 1; i >= 1; i--)
    //        for (int k = m_cols - 1; k >= 0; k--)
    //            if (m_sounds->size() > (i * m_cols + k))
    //            {
    //                int index = i * n + k;
    //                if (m_sounds->size() <= index)
    //                    m_sounds->resize(index + 1);
    //                (*m_sounds)[index] = (*m_sounds)[i * m_cols + k];
    //            }
    //}
    //else if (n < m_cols)
    //{
    //    for (int i = 1; i < m_rows; i++)
    //        for (int k = 0; k < m_cols; k++)
    //            if (m_sounds->size() > (i * m_cols + k))
    //                (*m_sounds)[i * n + k] = (*m_sounds)[i * m_cols + k];
    //}

	m_cols[m_activeConfig] = n;
	writeConfig();
	notify(NOTIFY_SET_COLS, n);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setVolumeLocal( int val )
{
	m_volumeLocal = val;
	notify(NOTIFY_SET_VOLUME_LOCAL, val);
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setVolumeRemote( int val )
{
	m_volumeRemote = val;
	notify(NOTIFY_SET_VOLUME_REMOTE, val);
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setPlaybackLocal( bool val )
{
	m_playbackLocal = val;
	writeConfig();
	notify(NOTIFY_SET_PLAYBACK_LOCAL, val ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setMuteMyselfDuringPb(bool val)
{
	m_muteMyselfDuringPb = val;
	writeConfig();
	notify(NOTIFY_SET_MUTE_MYSELF_DURING_PB, val ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::getWindowSize(int *width, int *height) const
{
	if(width)
		*width = m_windowWidth;
	if(height)
		*height = m_windowHeight;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setWindowSize(int width, int height)
{
	m_windowWidth = width;
	m_windowHeight = height;
	notify(NOTIFY_SET_WINDOW_SIZE, 0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setNextUpdateCheck(uint time)
{
	m_nextUpdateCheck = time;
	notify(NOTIFY_SET_NEXT_UPDATE_CHECK, (int)time);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::notify(notifications_e what, int data)
{
	for(Observer *obs : m_obs)
		obs->notify(*this, what, data);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setHotkeysEnabled(bool enabled)
{
	m_hotkeysEnabled = enabled;
	writeConfig();
	notify(NOTIFY_SET_HOTKEYS_ENABLED, enabled ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::addObserver(Observer *obs)
{
	m_obs.push_back(obs);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::remObserver(Observer *obs)
{
	m_obs.erase(std::remove(m_obs.begin(), m_obs.end(), obs), m_obs.end());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setBubbleButtonsBuild(int build)
{
	m_bubbleButtonsBuild = build;
	writeConfig();
	notify(NOTIFY_SET_BUBBLE_BUTTONS_BUILD, build);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setBubbleStopBuild(int build)
{
	m_bubbleStopBuild = build;
	writeConfig();
	notify(NOTIFY_SET_BUBBLE_STOP_BUILD, build);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setBubbleColsBuild(int build)
{
	m_bubbleColsBuild = build;
	writeConfig();
	notify(NOTIFY_SET_BUBBLE_COLS_BUILD, build);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
std::vector<SoundInfo> ConfigModel::getInitialSounds()
{
	char* pluginPath = (char*)malloc(PATH_BUFSIZE);
	ts3Functions.getPluginPath(pluginPath, PATH_BUFSIZE, getPluginID());
	QString fullPath = QString::fromUtf8(pluginPath);
	QChar last = fullPath[fullPath.count() - 1];
	if (last != '/' && last != '\\')
		fullPath.append('/');
	fullPath.append("rp_soundboard/");

	static const char* files[] = {
		"Airhorn Sonata.mp3",
		"Airhorn.mp3",
		"Airporn.mp3",
		"Peter Griffin Laugh.mp3",
		"Spooky.mp3",
		NULL,
	};

	std::vector<SoundInfo> sounds;
	for (int i = 0; files[i] != NULL; i++)
	{
		SoundInfo info;
		info.filename = fullPath + files[i];
		sounds.push_back(info);
	}
	return sounds;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::notifyAllEvents()
{
	//Notify all changes
    for(int i = 0; i < numSounds(); i++)
		notify(NOTIFY_SET_SOUND, i);
	notify(NOTIFY_SET_COLS, getCols());
	notify(NOTIFY_SET_ROWS, getRows());
	notify(NOTIFY_SET_VOLUME_LOCAL, m_volumeLocal);
	notify(NOTIFY_SET_VOLUME_REMOTE, m_volumeRemote);
	notify(NOTIFY_SET_PLAYBACK_LOCAL, m_playbackLocal);
	notify(NOTIFY_SET_MUTE_MYSELF_DURING_PB, m_muteMyselfDuringPb);
	notify(NOTIFY_SET_WINDOW_SIZE, 0);
	notify(NOTIFY_SET_BUBBLE_BUTTONS_BUILD, m_bubbleButtonsBuild);
	notify(NOTIFY_SET_BUBBLE_STOP_BUILD, m_bubbleStopBuild);
	notify(NOTIFY_SET_BUBBLE_COLS_BUILD, m_bubbleColsBuild);
	notify(NOTIFY_SET_SHOW_HOTKEYS_ON_BUTTONS, m_showHotkeysOnButtons);
	notify(NOTIFY_SET_HOTKEYS_ENABLED, m_hotkeysEnabled);
	notify(NOTIFY_SET_LINK_VOLUMES, m_linkVolumes);
	notify(NOTIFY_SET_EARRAPE_PROTECTION, m_earrapeProtection);
	notify(NOTIFY_SET_PITCH_SPEED, m_pitchSpeedValue);
	notify(NOTIFY_SET_PITCH, m_pitchValue);
	notify(NOTIFY_SET_SPEED, m_speedValue);
	notify(NOTIFY_SET_SYNC_PITCH_SPEED, m_syncPitchSpeed ? 1 : 0);
	notify(NOTIFY_SET_REVERB, m_reverbValue);
	notify(NOTIFY_SET_MULTI_SOUNDBOARD, m_multiSoundboard ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigModel::setShowHotkeysOnButtons(bool show)
{
	m_showHotkeysOnButtons = show;
	writeConfig();
	notify(NOTIFY_SET_SHOW_HOTKEYS_ON_BUTTONS, show ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigModel::setLinkVolumes(bool linked)
{
	m_linkVolumes = linked;
	writeConfig();
	notify(NOTIFY_SET_LINK_VOLUMES, linked ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigModel::setEarrapeProtection(bool enabled)
{
	m_earrapeProtection = enabled;
	writeConfig();
	notify(NOTIFY_SET_EARRAPE_PROTECTION, enabled ? 1 : 0);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigModel::setPitchSpeedValue(int val)
{
	m_pitchSpeedValue = val;
	if (m_rememberPitchSpeed)
		writeConfig();
	notify(NOTIFY_SET_PITCH_SPEED, val);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigModel::setRestoreSession(bool on)
{
	m_restoreSession = on;
	writeConfig();
}


void ConfigModel::setGlobalFxEnabled(bool on)
{
	m_globalFxEnabled = on;
	writeConfig();
}


void ConfigModel::setHideWaveform(bool on)
{
	m_hideWaveform = on;
	writeConfig();
}


void ConfigModel::setTheme(bool enabled, const QString &accent, const QString &waveform, const QString &background, int contrast, const QString &text, const QString &button)
{
	m_themeEnabled = enabled;
	m_themeAccent = accent;
	m_themeWaveform = waveform;
	m_themeBackground = background;
	m_themeContrast = contrast;
	m_themeText = text;
	m_themeButton = button;
	writeConfig();
}


void ConfigModel::setRememberPitchSpeed(bool remember)
{
	m_rememberPitchSpeed = remember;
	writeConfig();
}


void ConfigModel::setPitchValue(int val)
{
	m_pitchValue = val;
	if (m_rememberPitchSpeed)
		writeConfig();
	notify(NOTIFY_SET_PITCH, val);
}


void ConfigModel::setSpeedValue(int val)
{
	m_speedValue = val;
	if (m_rememberPitchSpeed)
		writeConfig();
	notify(NOTIFY_SET_SPEED, val);
}


void ConfigModel::setSyncPitchSpeed(bool sync)
{
	m_syncPitchSpeed = sync;
	writeConfig();
	notify(NOTIFY_SET_SYNC_PITCH_SPEED, sync ? 1 : 0);
}


void ConfigModel::setReverbValue(int val)
{
	m_reverbValue = val;
	if (m_rememberPitchSpeed)
		writeConfig();
	notify(NOTIFY_SET_REVERB, val);
}


void ConfigModel::setMultiSoundboard(bool enabled)
{
	m_multiSoundboard = enabled;
	writeConfig();
	notify(NOTIFY_SET_MULTI_SOUNDBOARD, enabled ? 1 : 0);
}

void ConfigModel::setLogsEnabled(bool on)
{
	m_logsEnabled = on;
	g_rpsbLogsEnabled = on ? 1 : 0;
	writeConfig();
	notify(NOTIFY_SET_LOGS_ENABLED, on ? 1 : 0);
}

void ConfigModel::setPreviewOnly(bool on)
{
	m_previewOnly = on;
	g_rpsbPreviewOnly = on ? 1 : 0;
	// React to mid-playback toggles immediately: the plugin needs to
	// pull TS3's mic out of forced CONT_TRANS the moment the user
	// flips the switch.
	if (auto *ts = sb_getTalkStateManager()) ts->onPreviewOnlyToggled(on);
	writeConfig();
	notify(NOTIFY_SET_PREVIEW_ONLY, on ? 1 : 0);
}

void ConfigModel::setAudioSandboxEnabled(bool on)
{
	m_audioSandboxEnabled = on;
	writeConfig();
}

void ConfigModel::setAudioMeterVisible(bool on)
{
	m_audioMeterVisible = on;
	writeConfig();
}

