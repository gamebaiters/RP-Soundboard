// src/main.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#pragma once
#ifndef rpsbsrc__device_H__
#define rpsbsrc__device_H__

#include "common.h"

#ifdef __cplusplus
class SoundInfo;
int sb_playFile(const SoundInfo &sound);

class Sampler;
Sampler *sb_getSampler();
#endif

CAPI void sb_handleCaptureData(uint64 serverConnectionHandlerID, short* samples,
	int sampleCount, int channels, int* edited);
CAPI void sb_handlePlaybackData(uint64 serverConnectionHandlerID, short* samples, int sampleCount,
	int channels, const unsigned int *channelSpeakerArray, unsigned int *channelFillMask);
CAPI void sb_stopPlayback();
//CAPI void sb_setVolume(int vol);
//CAPI void sb_setLocalPlayback(int enabled);
CAPI void sb_init();
CAPI void sb_kill();
CAPI void sb_onServerChange(uint64 serverID);
CAPI void sb_saveConfig();
CAPI void sb_openDialog();
CAPI int sb_playButtonEx(const char* btn);
CAPI void sb_playButton(int btn);
CAPI void sb_setConfig(int cfg);
CAPI void sb_openAbout();
CAPI void sb_openHowTo();
CAPI void sb_openLogViewer();
CAPI void sb_pauseSound();
CAPI void sb_unpauseSound();
CAPI void sb_pauseButtonPressed();
CAPI void sb_onConnectStatusChange(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber);
CAPI void sb_getInternalHotkeyName(int buttonId, char *buf); // buf should be at sized 16
CAPI void sb_getInternalConfigHotkeyName(int configId, char *buf);
CAPI void sb_onHotkeyRecordedEvent(const char *keyword, const char *key);
CAPI void sb_onStopTalking();
CAPI void sb_onHotkeyPressed(const char *keyword);
CAPI void sb_checkForUpdates();
CAPI int sb_parseCommand(char**, int);
CAPI void sb_disableHotkeysTemporarily(bool disable);
// Voice behaviour while a sound plays: (1) gate the user's mic by VAD so his
// voice is only sent when he talks (soundboard stays continuous); (2) duck the
// soundboard by duckAmount (0..1) whenever he talks. Called by the wiring on
// settings load + change.
CAPI void sb_setVoiceBehaviour(bool vadWhilePlaying, bool duckWhenTalking, float duckAmount);
// TeamSpeak's OWN transmission state for the local client (whatever the user
// configured drives it: voice activation, push-to-talk or continuous). Written
// from ts3plugin_onTalkStatusChangeEvent - a TS3 callback thread, so it is a
// plain atomic and the UI polls it instead of us touching widgets from there.
CAPI void sb_setSelfTalking(bool talking);
CAPI bool sb_isSelfTalking();
// Own-voice indicator. detected = our VAD on the captured mic block fired;
// passing = that voice is really leaving for the server (TS3 transmitting AND
// the soundboard is not muting the mic during playback). Both go false when
// capture callbacks stop arriving. NOT the same as sb_isSelfTalking(), which
// is true for soundboard playback too.
CAPI bool sb_isMicVoiceDetected();
CAPI bool sb_isMicVoicePassing();


#define HOTKEY_STOP_ALL "stop_all"
#define HOTKEY_PAUSE_ALL "pause_all"
#define HOTKEY_MUTE_MYSELF "mute_myself"
#define HOTKEY_MUTE_ON_MY_CLIENT "mute_on_my_client"
#define HOTKEY_VOLUME_INCREASE "volume_increase"
#define HOTKEY_VOLUME_DECREASE "volume_decrease"
#define HOTKEY_MICFX_TOGGLE "micfx_toggle"

#ifdef __cplusplus
class TalkStateManager;
TalkStateManager *sb_getTalkStateManager();
#endif

#endif
