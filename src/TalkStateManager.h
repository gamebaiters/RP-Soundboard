#pragma once
#include <QObject>
#include <QTimer>
#include <stdexcept>
#include "common.h"

class Sampler;

class TalkStateManager : public QObject
{
	Q_OBJECT

public:
	enum talk_state_e
	{
		TS_INVALID,
		TS_PTT_WITHOUT_VA,
		TS_PTT_WITH_VA,
		TS_VOICE_ACTIVATION,
		TS_CONT_TRANS,
	};
	static const char *toString(talk_state_e ts);

public:
	TalkStateManager();
	~TalkStateManager();
	void setSampler(Sampler *s) { m_sampler = s; }

public slots:
	void onStartPlaying(int slot, bool preview, QString filename);
	void onStopPlaying(int slot);
	void onPauseSound(int slot);
	void onUnpauseSound(int slot);

public:
	void setActiveServerId(uint64 id);
	talk_state_e getTalkState(uint64 scHandlerID);
	bool setTalkState(uint64 scHandlerID, talk_state_e state);
	bool setPushToTalk(uint64 scHandlerID, bool voiceActivation);
	bool setVoiceActivation(uint64 scHandlerID);
	bool setContinuousTransmission(uint64 scHandlerID);
	void onClientStopsTalking();
	// Called when the global preview-only flag is toggled mid-playback
	// so TS3's transmission state can react immediately instead of
	// waiting for the next sound to start.
	void onPreviewOnlyToggled(bool on);
	// Called from sb_onConnectStatusChange on STATUS_DISCONNECTED and
	// from sb_kill on plugin teardown. Stops the watchdog timer and
	// drops the active server id so any subsequent queued setTalkTransMode
	// invocation skips its ts3Functions calls - those calls would hit
	// directsound_win64.dll / WASAPI cleanup paths that are already
	// torn down by TS3 at this point, crashing the client on close.
	void onConnectionLost();

private slots:
	// Re-asserts CLIENT_INPUT_DEACTIVATED=INPUT_ACTIVE while we're holding
	// TS_CONT_TRANS for soundboard playback. Defeats TS3's PTT-up handler,
	// which can flip CLIENT_INPUT_DEACTIVATED back to INPUT_DEACTIVATED
	// when the user releases the PTT key mid-playback.
	void onContTransWatchdog();

private:
	bool anySlotStillPlaying() const;
	void setTalkTransMode();
	void setPlayTransMode();
	// Force a VAD preprocessor re-init via a vad=false -> vad=true cycle.
	// TS3's VAD module can get stuck after a vad=false/vad=true ping-pong
	// (the side effect of forcing CONT_TRANS then restoring), so cycle it
	// explicitly when we restore a VAD-enabled mode.
	void forceVadReinit(uint64 scHandlerID);
	// After restoring talk state, verify CLIENT_INPUT_DEACTIVATED matches
	// the target and re-assert if TS3 hasn't actually propagated our
	// last flush. Without this, a stale read on the next setPlayTransMode
	// captures TS_CONT_TRANS as the "user mode", which then restores
	// itself forever and leaves the mic open after playback ends.
	void verifyInputDeactivated(uint64 scHandlerID, talk_state_e ts);
	talk_state_e previousTalkState;
	talk_state_e currentTalkState;
	// Sticky last-known user mode. Set when we first snapshot and reused
	// when a fresh snapshot returns an unreliable TS_CONT_TRANS (almost
	// always our own override leaking back). Cleared only on server
	// change so a different server can capture a fresh real user mode.
	talk_state_e lastUserMode;
	uint64 activeServerId;
	uint64 playingServerId;
	Sampler *m_sampler;
	QTimer m_contTransWatchdog;

};
