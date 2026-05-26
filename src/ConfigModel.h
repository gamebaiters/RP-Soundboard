// src/ConfigModel.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#pragma once
#ifndef rpsbsrc__ConfigModel_H__
#define rpsbsrc__ConfigModel_H__

#include <vector>
#include <array>
#include "SoundInfo.h"
#include <QString>
#include <memory>
#include <set>
#include "common.h"

class ConfigModel
{
public:
	enum notifications_e
	{
		NOTIFY_SET_SOUND,
		NOTIFY_SET_ROWS,
		NOTIFY_SET_COLS,
		NOTIFY_SET_VOLUME_LOCAL,
		NOTIFY_SET_VOLUME_REMOTE,
		NOTIFY_SET_PLAYBACK_LOCAL,
		NOTIFY_SET_MUTE_MYSELF_DURING_PB,
		NOTIFY_SET_WINDOW_SIZE,
		NOTIFY_SET_BUBBLE_BUTTONS_BUILD,
		NOTIFY_SET_BUBBLE_STOP_BUILD,
		NOTIFY_SET_BUBBLE_COLS_BUILD,
		NOTIFY_SET_SHOW_HOTKEYS_ON_BUTTONS,
		NOTIFY_SET_HOTKEYS_ENABLED,
		NOTIFY_SET_NEXT_UPDATE_CHECK,
		NOTIFY_SET_LINK_VOLUMES,
		NOTIFY_SET_EARRAPE_PROTECTION,
		NOTIFY_SET_PITCH_SPEED,
		NOTIFY_SET_PITCH,
		NOTIFY_SET_SPEED,
		NOTIFY_SET_SYNC_PITCH_SPEED,
		NOTIFY_SET_REVERB,
		NOTIFY_SET_MULTI_SOUNDBOARD,
		NOTIFY_SET_LOGS_ENABLED,
		NOTIFY_SET_PREVIEW_ONLY,
	};

	class Observer
	{
	public:
        virtual ~Observer() {}
		virtual void notify(ConfigModel &model, notifications_e what, int data) = 0;
	};

public:
	ConfigModel();
	// Clears the static dirty-state pointer if it still references this
	// instance, so a stale flushPendingWrite() after delete cannot reach
	// freed memory. Plugin teardown order is the obvious caller but the
	// guard also covers any future hot-reload / re-init path.
	~ConfigModel();

	void readConfig(const QString &file = QString());
    // Default-path saves are now event-triggered: writeConfig() only
    // marks the model dirty; actual disk I/O fires from event hooks
    // (play start, soundboard window close, server disconnect, plugin
    // shutdown). This avoids the lag-per-slider-tick the old auto-save
    // produced. Specific-path saves still write immediately.
    void writeConfig(const QString &file = QString());
    // Synchronous full-file write. Used by flushPendingWrite() and by
    // callers that need predictable on-disk state (config_io export /
    // import paths, explicit "save" menu).
    void writeConfigImmediate(const QString &file = QString());
    // Persist the default-path ini now if any setter has marked it
    // dirty since the last write. Cheap no-op when clean. Call from
    // play-start, dialog close, server disconnect, plugin shutdown.
    static void flushPendingWrite();
    static bool hasPendingWrite();

	void notifyAllEvents();
	
	static QString GetConfigPath();
	static QString GetFullConfigPath();

	QString getFileName(int itemId) const;
	void setFileName(int itemId, const QString &fn);

	const SoundInfo *getSoundInfo(int itemId) const;
	void setSoundInfo(int itemId, const SoundInfo &info);

	inline int getRows() const { return m_rows[m_activeConfig]; }
	void setRows(int n);

	inline int getCols() const { return m_cols[m_activeConfig]; }
	void setCols(int n);

	inline int getVolumeLocal() const { return m_volumeLocal; }
	void setVolumeLocal(int val);

	inline int getVolumeRemote() const { return m_volumeRemote; }
	void setVolumeRemote(int val);

	inline bool getPlaybackLocal() const { return m_playbackLocal; }
	void setPlaybackLocal(bool val);
	
	inline bool getMuteMyselfDuringPb() const { return m_muteMyselfDuringPb; }
	void setMuteMyselfDuringPb(bool val);

	void getWindowSize(int *width, int *height) const;
	void setWindowSize(int width, int height);

	inline int getBubbleButtonsBuild() const { return m_bubbleButtonsBuild; }
	void setBubbleButtonsBuild(int build);

	inline int getBubbleStopBuild() const { return m_bubbleStopBuild; }
	void setBubbleStopBuild(int build);

	inline int getBubbleColsBuild() const { return m_bubbleColsBuild; }
	void setBubbleColsBuild(int build);

	inline bool getShowHotkeysOnButtons() const { return m_showHotkeysOnButtons; }
	void setShowHotkeysOnButtons(bool show);

	inline bool getHotkeysEnabled() const { return m_hotkeysEnabled; }
	void setHotkeysEnabled(bool enabled);

	inline bool getLinkVolumes() const { return m_linkVolumes; }
	void setLinkVolumes(bool linked);

	inline bool getEarrapeProtection() const { return m_earrapeProtection; }
	void setEarrapeProtection(bool enabled);

	inline int getPitchSpeedValue() const { return m_pitchSpeedValue; }
	void setPitchSpeedValue(int val);

	inline bool getRememberPitchSpeed() const { return m_rememberPitchSpeed; }
	void setRememberPitchSpeed(bool remember);

	inline bool getRestoreSession() const { return m_restoreSession; }
	void setRestoreSession(bool on);

	inline bool getGlobalFxEnabled() const { return m_globalFxEnabled; }
	void setGlobalFxEnabled(bool on);

	inline bool getHideWaveform() const { return m_hideWaveform; }
	void setHideWaveform(bool on);

	inline bool getThemeEnabled() const { return m_themeEnabled; }
	inline QString getThemeAccent() const { return m_themeAccent; }
	inline QString getThemeWaveform() const { return m_themeWaveform; }
	inline QString getThemeBackground() const { return m_themeBackground; }
	inline int getThemeContrast() const { return m_themeContrast; }
	// Empty -> auto-derived (textOn(background)). Any valid hex string
	// overrides text color across the UI.
	inline QString getThemeText() const { return m_themeText; }
	inline QString getThemeButton() const { return m_themeButton; }
	void setTheme(bool enabled, const QString &accent, const QString &waveform, const QString &background, int contrast, const QString &text, const QString &button);

	inline int getPitchValue() const { return m_pitchValue; }
	void setPitchValue(int val);

	inline int getSpeedValue() const { return m_speedValue; }
	void setSpeedValue(int val);

	inline bool getSyncPitchSpeed() const { return m_syncPitchSpeed; }
	void setSyncPitchSpeed(bool sync);

	inline int getReverbValue() const { return m_reverbValue; }
	void setReverbValue(int val);

	inline bool getMultiSoundboard() const { return m_multiSoundboard; }
	void setMultiSoundboard(bool enabled);

	inline bool getLogsEnabled() const { return m_logsEnabled; }
	void setLogsEnabled(bool on);

	inline bool getPreviewOnly() const { return m_previewOnly; }
	void setPreviewOnly(bool on);

	inline bool getAudioSandboxEnabled() const { return m_audioSandboxEnabled; }
	void setAudioSandboxEnabled(bool on);

	inline bool getAudioMeterVisible() const { return m_audioMeterVisible; }
	void setAudioMeterVisible(bool on);

	inline bool getAudioExportEnabled() const { return m_audioExportEnabled; }
	void setAudioExportEnabled(bool on);

	inline bool getAdaptWaveformToFx() const { return m_adaptWaveformToFx; }
	void setAdaptWaveformToFx(bool on);

	inline bool getShowCropMarkers() const { return m_showCropMarkers; }
	void setShowCropMarkers(bool on);

	inline bool getResetChVolume() const { return m_resetChVolume; }
	void setResetChVolume(bool on);
	inline bool getResetChFx() const { return m_resetChFx; }
	void setResetChFx(bool on);
	inline bool getResetChFile() const { return m_resetChFile; }
	void setResetChFile(bool on);
	inline bool getResetChSandbox() const { return m_resetChSandbox; }
	void setResetChSandbox(bool on);
	inline bool getResetAllRemoveExtra() const { return m_resetAllRemoveExtra; }
	void setResetAllRemoveExtra(bool on);
	inline bool getResetAllVolume() const { return m_resetAllVolume; }
	void setResetAllVolume(bool on);
	inline bool getResetAllFx() const { return m_resetAllFx; }
	void setResetAllFx(bool on);
	inline bool getResetAllFiles() const { return m_resetAllFiles; }
	void setResetAllFiles(bool on);
	inline bool getResetAllSandbox() const { return m_resetAllSandbox; }
	void setResetAllSandbox(bool on);

	void addObserver(Observer *obs);
	void remObserver(Observer *obs);

    void setConfiguration(int config);
	int getConfiguration();

	const std::vector<SoundInfo> &sounds() const { return m_sounds[m_activeConfig]; }
    int numSounds() const { return (int)sounds().size(); }

	uint getNextUpdateCheck() const { return m_nextUpdateCheck; }
	void setNextUpdateCheck(uint time);

private:
	std::vector<SoundInfo> &sounds() { return m_sounds[m_activeConfig]; }
	void notify(notifications_e what, int data);
	std::vector<SoundInfo> getInitialSounds();
	std::vector<SoundInfo> readConfiguration(QSettings & settings, const QString &name);
    void writeConfiguration(QSettings & settings, const QString &name, const std::vector<SoundInfo> &sounds);

	std::vector<Observer*> m_obs;
	std::array<std::vector<SoundInfo>, NUM_CONFIGS> m_sounds;
	int m_activeConfig;

    std::array<int, NUM_CONFIGS> m_rows;
	std::array<int, NUM_CONFIGS> m_cols;
	int m_volumeLocal;
	int m_volumeRemote;
	bool m_playbackLocal;
	bool m_muteMyselfDuringPb;
	int m_windowWidth;
	int m_windowHeight;

	int m_bubbleButtonsBuild;
	int m_bubbleStopBuild;
	int m_bubbleColsBuild;

	bool m_showHotkeysOnButtons;
	bool m_hotkeysEnabled;
	bool m_linkVolumes;
	bool m_earrapeProtection;
	int m_pitchSpeedValue;
	bool m_rememberPitchSpeed;
	bool m_restoreSession;
	bool m_globalFxEnabled;
	bool m_hideWaveform;
	bool m_themeEnabled;
	QString m_themeAccent;
	QString m_themeWaveform;
	QString m_themeBackground;
	int m_themeContrast;
	QString m_themeText;
	QString m_themeButton;
	int m_pitchValue;
	int m_speedValue;
	bool m_syncPitchSpeed;
	int m_reverbValue;
	bool m_multiSoundboard;
	bool m_logsEnabled;
	bool m_previewOnly;
	bool m_audioSandboxEnabled;
	bool m_audioMeterVisible;
	bool m_audioExportEnabled = false;
	bool m_adaptWaveformToFx = false;
	bool m_showCropMarkers   = true;

	bool m_resetChVolume     = true;
	bool m_resetChFx         = true;
	bool m_resetChFile       = true;
	bool m_resetChSandbox    = true;
	bool m_resetAllRemoveExtra = true;
	bool m_resetAllVolume    = true;
	bool m_resetAllFx        = true;
	bool m_resetAllFiles     = true;
	bool m_resetAllSandbox   = true;

	uint m_nextUpdateCheck;
};

#endif // rpsbsrc__ConfigModel_H__
