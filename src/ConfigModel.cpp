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


// Event-triggered save policy.
//
// The original setters called writeConfig() on every change, including
// every slider valueChanged tick - that meant a synchronous full-ini
// write (~50 keys + every sound's metadata) per pixel of slider movement
// and was the dominant source of the GUI lag the user kept reporting.
// A 250 ms debouncer reduced the symptom but still issued a fresh write
// shortly after every drag.
//
// The setting now uses an explicit dirty-flag model: writeConfig() with
// the default path only marks the model dirty; the actual file write is
// triggered from concrete user-facing events that already imply "the
// user is pausing or leaving" - playback start, soundboard window close,
// server disconnect, plugin shutdown. Closing the soundboard window
// without quitting TS3 also flushes, so state is never lost there.
//
// Specific-path writes (export, import, "save as", explicit save menu)
// still go through writeConfigImmediate for predictable semantics.
namespace {
struct DirtyState {
    ConfigModel *model = nullptr;
    bool         dirty = false;
};
DirtyState &dirtyState() { static DirtyState s; return s; }
} // namespace

void ConfigModel::flushPendingWrite()
{
    auto &s = dirtyState();
    if (s.dirty && s.model) {
        s.model->writeConfigImmediate(QString());
        s.dirty = false;
    }
}

bool ConfigModel::hasPendingWrite()
{
    return dirtyState().dirty;
}



ConfigModel::~ConfigModel()
{
	auto &s = dirtyState();
	if (s.model == this) {
		// One last best-effort flush: if a setter has marked the model
		// dirty after the last event-hook flush (e.g. between sb_kill's
		// flushPendingWrite call and the model-delete site), persist
		// the latest state before the static pointer is invalidated.
		if (s.dirty) {
			writeConfigImmediate(QString());
		}
		s.model = nullptr;
		s.dirty = false;
	}
}


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
	m_extremeLogging = false;
	m_rightDragLoopEnabled = false;
	m_replayModeEnabled = true;
	m_previewOnly = false;
	m_audioSandboxEnabled = true;
	m_audioMeterVisible = true;
	m_audioExportEnabled = false;
	m_adaptWaveformToFx = false;
	m_showCropMarkers = true;
	m_multiChannelInfinity = false;
	m_showPauseAllButton = true;
	m_showStopAllButton = true;
	m_showAddChannelButton = true;
	m_showMuteChecks = true;
	m_showProfileButtons = true;
	m_showGridSizeSelectors = true;
	m_verticalMeter = false;
	m_showSkipButtons = true;
	m_spectrogramView = false;
	m_showVinylButton = true;
	m_micFxFeatureEnabled = true;
	m_streamingEnabled = true;
	m_channelNameLinkDetect = true;
	m_streamAutoplay = false;
	m_streamFxGradient = true;
	m_waveAnimSpeed = 50;
	m_waveAnimIntensity = 30;
	m_formatBadgeMode = 3;
	m_showStreamBadge = true;
	m_loudnessNormalize = false;

	m_resetChVolume = true;
	m_resetChFx = true;
	m_resetChFile = true;
	m_resetChSandbox = true;
	m_resetAllRemoveExtra = true;
	m_resetAllVolume = true;
	m_resetAllFx = true;
	m_resetAllFiles = true;
	m_resetAllSandbox = true;

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
	m_extremeLogging = settings.value("extreme_logging", false).toBool();
	g_rpsbExtremeLogging = m_extremeLogging ? 1 : 0;
	m_rightDragLoopEnabled = settings.value("right_drag_loop_enabled", false).toBool();
	m_replayModeEnabled = settings.value("replay_mode_enabled", true).toBool();
	m_previewOnly = settings.value("preview_only", false).toBool();
	m_audioSandboxEnabled = settings.value("audio_sandbox_enabled", true).toBool();
	m_audioMeterVisible   = settings.value("audio_meter_visible", true).toBool();
	m_audioExportEnabled  = settings.value("audio_export_enabled", false).toBool();
	m_adaptWaveformToFx   = settings.value("adapt_waveform_to_fx", false).toBool();
	m_showCropMarkers     = settings.value("show_crop_markers", true).toBool();
	m_multiChannelInfinity= settings.value("multi_channel_infinity", false).toBool();
	m_showPauseAllButton  = settings.value("show_pause_all_button", true).toBool();
	m_showStopAllButton   = settings.value("show_stop_all_button",  true).toBool();
	m_showAddChannelButton  = settings.value("show_add_channel_button",   true).toBool();
	m_showMuteChecks        = settings.value("show_mute_checks",          true).toBool();
	m_showVoiceIndicator    = settings.value("show_voice_indicator",      true).toBool();
	m_showProfileButtons    = settings.value("show_profile_buttons",      true).toBool();
	m_showGridSizeSelectors = settings.value("show_grid_size_selectors",  true).toBool();
	m_verticalMeter       = settings.value("vertical_meter",        false).toBool();
	m_showSkipButtons     = settings.value("show_skip_buttons",     true).toBool();
	m_spectrogramView     = settings.value("spectrogram_view",      false).toBool();
	m_showVinylButton     = settings.value("show_vinyl_button",     true).toBool();
	m_micFxFeatureEnabled = settings.value("micfx_feature_enabled", true).toBool();
	m_tsToolbarButton = settings.value("ts_toolbar_button", true).toBool();
	m_uiFontPt = settings.value("ui_font_pt", 0).toInt();
	m_streamingEnabled      = settings.value("streaming_enabled",         true).toBool();
	m_channelNameLinkDetect = settings.value("channel_name_link_detect",  true).toBool();
	m_streamAutoplay        = settings.value("stream_autoplay",           false).toBool();
	m_streamFxGradient      = settings.value("stream_fx_gradient",        true).toBool();
	m_waveAnimSpeed         = qBound(0, settings.value("wave_anim_speed",     50).toInt(), 100);
	m_waveAnimIntensity     = qBound(0, settings.value("wave_anim_intensity", 30).toInt(), 100);
	// Badge complexity (0 none / 1 format / 2 quality / 3 both). Migrates
	// the short-lived v2.3.3 bool key: an explicit false becomes "none".
	{
		const bool legacyOn = settings.value("show_format_badge", true).toBool();
		m_formatBadgeMode = settings.value("format_badge_mode",
		                                   legacyOn ? 3 : 0).toInt();
		if (m_formatBadgeMode < 0 || m_formatBadgeMode > 3) m_formatBadgeMode = 3;
	}
	m_showStreamBadge       = settings.value("show_stream_badge",         true).toBool();
	m_vadWhilePlaying       = settings.value("vad_while_playing",         false).toBool();
	m_duckWhenTalking       = settings.value("duck_when_talking",         false).toBool();
	m_duckAmountPercent     = settings.value("duck_amount_percent",       40).toInt();
	m_loudnessNormalize   = settings.value("loudness_normalize",    false).toBool();
	m_resetChVolume       = settings.value("reset_ch_volume", true).toBool();
	m_resetChFx           = settings.value("reset_ch_fx", true).toBool();
	m_resetChFile         = settings.value("reset_ch_file", true).toBool();
	m_resetChSandbox      = settings.value("reset_ch_sandbox", true).toBool();
	m_resetAllRemoveExtra = settings.value("reset_all_remove_extra", true).toBool();
	m_resetAllVolume      = settings.value("reset_all_volume", true).toBool();
	m_resetAllFx          = settings.value("reset_all_fx", true).toBool();
	m_resetAllFiles       = settings.value("reset_all_files", true).toBool();
	m_resetAllSandbox     = settings.value("reset_all_sandbox", true).toBool();
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
    // Default-path saves: just mark dirty - real disk write fires from
    // event hooks (play, dialog close, disconnect, shutdown). Avoids
    // the per-slider-tick lag the old auto-write produced.
    if (file.isEmpty()) {
        auto &s = dirtyState();
        s.model = this;
        s.dirty = true;
        return;
    }
    // Caller asked for a specific path (export, import, snapshot) - write
    // immediately so the saved file matches the caller's expectation.
    writeConfigImmediate(file);
}

void ConfigModel::writeConfigImmediate(const QString &file)
{
    QString path;
    if (file.isEmpty()) {
        path = GetFullConfigPath();
        // We just persisted the default-path ini - clear any pending
        // dirty flag so the next flushPendingWrite is a no-op until a
        // setter marks the model dirty again.
        dirtyState().dirty = false;
    } else {
        path = file;
    }

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
	settings.setValue("extreme_logging", m_extremeLogging);
	settings.setValue("right_drag_loop_enabled", m_rightDragLoopEnabled);
	settings.setValue("replay_mode_enabled", m_replayModeEnabled);
	settings.setValue("preview_only", m_previewOnly);
	settings.setValue("audio_sandbox_enabled", m_audioSandboxEnabled);
	settings.setValue("audio_meter_visible", m_audioMeterVisible);
	settings.setValue("audio_export_enabled", m_audioExportEnabled);
	settings.setValue("adapt_waveform_to_fx", m_adaptWaveformToFx);
	settings.setValue("show_crop_markers", m_showCropMarkers);
	settings.setValue("multi_channel_infinity", m_multiChannelInfinity);
	settings.setValue("show_pause_all_button", m_showPauseAllButton);
	settings.setValue("show_stop_all_button",  m_showStopAllButton);
	settings.setValue("show_add_channel_button",  m_showAddChannelButton);
	settings.setValue("show_mute_checks",         m_showMuteChecks);
	settings.setValue("show_voice_indicator",     m_showVoiceIndicator);
	settings.setValue("show_profile_buttons",     m_showProfileButtons);
	settings.setValue("show_grid_size_selectors", m_showGridSizeSelectors);
	settings.setValue("vertical_meter",        m_verticalMeter);
	settings.setValue("show_skip_buttons",     m_showSkipButtons);
	settings.setValue("spectrogram_view",      m_spectrogramView);
	settings.setValue("show_vinyl_button",     m_showVinylButton);
	settings.setValue("micfx_feature_enabled", m_micFxFeatureEnabled);
	settings.setValue("ts_toolbar_button", m_tsToolbarButton);
	settings.setValue("ui_font_pt", m_uiFontPt);
	settings.setValue("streaming_enabled",         m_streamingEnabled);
	settings.setValue("channel_name_link_detect",  m_channelNameLinkDetect);
	settings.setValue("stream_autoplay",           m_streamAutoplay);
	settings.setValue("stream_fx_gradient",        m_streamFxGradient);
	settings.setValue("format_badge_mode",         m_formatBadgeMode);
	settings.setValue("show_stream_badge",         m_showStreamBadge);
	settings.setValue("wave_anim_speed",           m_waveAnimSpeed);
	settings.setValue("wave_anim_intensity",       m_waveAnimIntensity);
	settings.setValue("vad_while_playing",         m_vadWhilePlaying);
	settings.setValue("duck_when_talking",         m_duckWhenTalking);
	settings.setValue("duck_amount_percent",       m_duckAmountPercent);
	settings.setValue("loudness_normalize",    m_loudnessNormalize);
	settings.setValue("reset_ch_volume", m_resetChVolume);
	settings.setValue("reset_ch_fx", m_resetChFx);
	settings.setValue("reset_ch_file", m_resetChFile);
	settings.setValue("reset_ch_sandbox", m_resetChSandbox);
	settings.setValue("reset_all_remove_extra", m_resetAllRemoveExtra);
	settings.setValue("reset_all_volume", m_resetAllVolume);
	settings.setValue("reset_all_fx", m_resetAllFx);
	settings.setValue("reset_all_files", m_resetAllFiles);
	settings.setValue("reset_all_sandbox", m_resetAllSandbox);
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
	// Profile switch = "user is leaving this profile's edit context".
	// Flush any pending dirty changes from the outgoing profile before
	// activating the new one so we never silently drop them if the user
	// crashes / quits TS3 from inside the new profile.
	flushPendingWrite();
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
	// Cell metadata changes (crop markers, loop area, color, filename,
	// macro state) are NEVER slider-rate — they only fire on discrete
	// user actions (right-click menu, advanced panel accept, drop,
	// reorder). The dirty-flag deferral that buys slider perf elsewhere
	// would only put cell edits at risk of being lost across a TS3
	// crash / hard kill / Win+L sleep before any flush event fires.
	// User requirement: "ogni singola volta che un marcatore, un area
	// o altro in memoria viene creata o eliminata, deve essere
	// immediatamente salvato". Persist now.
	writeConfigImmediate(QString());
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

void ConfigModel::setExtremeLogging(bool on)
{
	m_extremeLogging = on;
	g_rpsbExtremeLogging = on ? 1 : 0;
	writeConfig();
}

void ConfigModel::setRightDragLoopEnabled(bool on)
{
	m_rightDragLoopEnabled = on;
	writeConfig();
}

void ConfigModel::setReplayModeEnabled(bool on)
{
	m_replayModeEnabled = on;
	writeConfig();
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

void ConfigModel::setAudioExportEnabled(bool on)
{
	m_audioExportEnabled = on;
	writeConfig();
}

void ConfigModel::setAdaptWaveformToFx(bool on)
{
	m_adaptWaveformToFx = on;
	writeConfig();
}

void ConfigModel::setShowCropMarkers(bool on)
{
	m_showCropMarkers = on;
	writeConfig();
}

void ConfigModel::setMultiChannelInfinity(bool on) { m_multiChannelInfinity = on; writeConfig(); }
void ConfigModel::setShowPauseAllButton(bool on)   { m_showPauseAllButton   = on; writeConfig(); }
void ConfigModel::setShowStopAllButton(bool on)    { m_showStopAllButton    = on; writeConfig(); }
void ConfigModel::setShowAddChannelButton(bool on) { m_showAddChannelButton = on; writeConfig(); }
void ConfigModel::setShowMuteChecks(bool on)       { m_showMuteChecks       = on; writeConfig(); }
void ConfigModel::setShowVoiceIndicator(bool on)   { m_showVoiceIndicator   = on; writeConfig(); }
void ConfigModel::setShowProfileButtons(bool on)   { m_showProfileButtons   = on; writeConfig(); }
void ConfigModel::setShowGridSizeSelectors(bool on){ m_showGridSizeSelectors= on; writeConfig(); }
void ConfigModel::setVerticalMeter(bool on)        { m_verticalMeter        = on; writeConfig(); }
void ConfigModel::setShowSkipButtons(bool on)      { m_showSkipButtons      = on; writeConfig(); }
void ConfigModel::setSpectrogramView(bool on)      { m_spectrogramView      = on; writeConfig(); }
void ConfigModel::setShowVinylButton(bool on)      { m_showVinylButton      = on; writeConfig(); }
void ConfigModel::setMicFxFeatureEnabled(bool on)  { m_micFxFeatureEnabled  = on; writeConfig(); }
void ConfigModel::setTsToolbarButton(bool on)      { m_tsToolbarButton      = on; writeConfig(); }
void ConfigModel::setUiFontPt(int pt)              { m_uiFontPt             = pt; writeConfig(); }
void ConfigModel::setStreamingEnabled(bool on)        { m_streamingEnabled       = on; writeConfig(); }
void ConfigModel::setChannelNameLinkDetect(bool on)   { m_channelNameLinkDetect  = on; writeConfig(); }
void ConfigModel::setStreamAutoplay(bool on)          { m_streamAutoplay         = on; writeConfig(); }
void ConfigModel::setStreamFxGradient(bool on)        { m_streamFxGradient       = on; writeConfig(); }
void ConfigModel::setFormatBadgeMode(int mode)        { m_formatBadgeMode = (mode < 0 || mode > 3) ? 3 : mode; writeConfig(); }
void ConfigModel::setShowStreamBadge(bool on)         { m_showStreamBadge        = on; writeConfig(); }
void ConfigModel::setWaveAnimSpeed(int v)             { m_waveAnimSpeed     = qBound(0, v, 100); writeConfig(); }
void ConfigModel::setWaveAnimIntensity(int v)         { m_waveAnimIntensity = qBound(0, v, 100); writeConfig(); }
void ConfigModel::setVadWhilePlaying(bool on)         { m_vadWhilePlaying        = on; writeConfig(); }
void ConfigModel::setDuckWhenTalking(bool on)         { m_duckWhenTalking        = on; writeConfig(); }
void ConfigModel::setDuckAmountPercent(int pct)       { m_duckAmountPercent      = pct; writeConfig(); }
void ConfigModel::setLoudnessNormalize(bool on)    { m_loudnessNormalize    = on; writeConfig(); }

void ConfigModel::setResetChVolume(bool on) { m_resetChVolume = on; writeConfig(); }
void ConfigModel::setResetChFx(bool on) { m_resetChFx = on; writeConfig(); }
void ConfigModel::setResetChFile(bool on) { m_resetChFile = on; writeConfig(); }
void ConfigModel::setResetChSandbox(bool on) { m_resetChSandbox = on; writeConfig(); }
void ConfigModel::setResetAllRemoveExtra(bool on) { m_resetAllRemoveExtra = on; writeConfig(); }
void ConfigModel::setResetAllVolume(bool on) { m_resetAllVolume = on; writeConfig(); }
void ConfigModel::setResetAllFx(bool on) { m_resetAllFx = on; writeConfig(); }
void ConfigModel::setResetAllFiles(bool on) { m_resetAllFiles = on; writeConfig(); }
void ConfigModel::setResetAllSandbox(bool on) { m_resetAllSandbox = on; writeConfig(); }

