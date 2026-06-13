
#include "common.h"
#include "TalkStateManager.h"
#include "samples.h"
#include "ts3log.h"
#include "main.h"
#include "plugin.h"
#include <QMetaEnum>

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
#define RETURN_ENUM_CASE(val) case val: return #val
const char * TalkStateManager::toString(talk_state_e ts)
{
	switch (ts)
	{
		RETURN_ENUM_CASE(TS_INVALID);
		RETURN_ENUM_CASE(TS_PTT_WITHOUT_VA);
		RETURN_ENUM_CASE(TS_PTT_WITH_VA);
		RETURN_ENUM_CASE(TS_VOICE_ACTIVATION);
		RETURN_ENUM_CASE(TS_CONT_TRANS);
	default:
		throw std::logic_error("Fucked up toString method");
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
TalkStateManager::TalkStateManager() :
	previousTalkState(TS_INVALID),
	currentTalkState(TS_INVALID),
	lastUserMode(TS_INVALID),
	activeServerId(0),
	playingServerId(0),
	m_sampler(NULL)
{
	// 150 ms watchdog: cheap enough to be invisible, fast enough to
	// catch TS3's PTT-up overwrite of CLIENT_INPUT_DEACTIVATED before
	// the soundboard audibly drops.
	m_contTransWatchdog.setInterval(150);
	m_contTransWatchdog.setSingleShot(false);
	QObject::connect(&m_contTransWatchdog, &QTimer::timeout,
		this, &TalkStateManager::onContTransWatchdog);
	// Post-restore verify timer: setTalkTransMode arms this single-shot
	// timer so any async TS3 flip 50-500 ms after our restore flush is
	// caught and the user's actual target state (PTT / VAD / PTT_WITH_VA)
	// is re-applied. Three fire windows cover every observed delay.
	m_restoreTimer.setSingleShot(true);
	QObject::connect(&m_restoreTimer, &QTimer::timeout,
		this, &TalkStateManager::onRestoreVerify);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
TalkStateManager::~TalkStateManager()
{
	
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void TalkStateManager::onStartPlaying(int slot, bool preview, QString filename)
{
	Q_UNUSED(slot);
	// In preview-only mode the plugin must not touch TS3's transmission
	// state. Forcing CLIENT_INPUT_DEACTIVATED -> ACTIVE (continuous
	// transmission) is what was making the mic light up on the server
	// even though no soundboard audio was being injected. Treat the
	// global preview-only switch as if every sound was a preview.
	if (!preview && !g_rpsbPreviewOnly)
	{
		// A new sound starting cancels any in-flight post-restore
		// verify from a previous sound — playback ownership of the
		// talk state passes to setPlayTransMode now.
		m_restoreTimer.stop();
		m_restoreTarget   = TS_INVALID;
		m_restoreServer   = 0;
		m_restoreAttempts = 0;
		playingServerId = activeServerId;
		setPlayTransMode();
	}
}


//---------------------------------------------------------------
// Purpose: Only restore talk state when ALL slots are done
//---------------------------------------------------------------
void TalkStateManager::onStopPlaying(int slot)
{
	Q_UNUSED(slot);
	if (!anySlotStillPlaying())
		setTalkTransMode();
}


//---------------------------------------------------------------
// Purpose: Only restore talk state when no slot is actively playing
//---------------------------------------------------------------
void TalkStateManager::onPauseSound(int slot)
{
	Q_UNUSED(slot);
	if (!anySlotStillPlaying())
		setTalkTransMode();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void TalkStateManager::onUnpauseSound(int slot)
{
	Q_UNUSED(slot);
	if (g_rpsbPreviewOnly)
		return;
	setPlayTransMode();
}


//---------------------------------------------------------------
// Purpose: Check if any slot is still actively playing (not paused)
//---------------------------------------------------------------
bool TalkStateManager::anySlotStillPlaying() const
{
	if (!m_sampler)
		return false;
	for (int i = 0; i < Sampler::MAX_SLOTS; i++)
	{
		Sampler::state_e st = m_sampler->getState(i);
		if (st == Sampler::ePLAYING)
			return true;
	}
	return false;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void TalkStateManager::setTalkTransMode()
{
	if (previousTalkState == TS_INVALID)
		return;
	talk_state_e ts = previousTalkState;
	previousTalkState = TS_INVALID;
	uint64 srv = playingServerId ? playingServerId : activeServerId;
	// Stop the cont-trans watchdog before we leave TS_CONT_TRANS.
	m_contTransWatchdog.stop();
	// Server already disconnected (onConnectionLost cleared the ids).
	// Skip ts3Functions calls - they'd race with TS3's audio backend
	// teardown and crash the client.
	if (srv == 0)
		return;
	// Always call setTalkState. The previous early-exit on "current==ts"
	// could mistakenly skip the restore when TS3's getTalkState read
	// returned a stale value matching our target - the user's mic would
	// then stay in the override state. Forcing the call is cheap and
	// guarantees the flush actually propagates.
	setTalkState(srv, ts);
	// When restoring a VAD-enabled mode (either pure voice activation
	// or PTT-with-VA), force a VAD preprocessor re-init. The vad=false
	// -> vad=true ping-pong from CONT_TRANS / restore leaves TS3's
	// VAD module in a stuck state where voice no longer triggers
	// transmission until the user mute+unmutes manually.
	if (ts == TS_VOICE_ACTIVATION || ts == TS_PTT_WITH_VA)
		forceVadReinit(srv, ts);
	// Final safety net: re-read CLIENT_INPUT_DEACTIVATED and re-assert
	// if TS3 didn't actually apply our last write. Catches the case
	// where the watchdog re-asserted INPUT_ACTIVE one tick before the
	// restore landed.
	verifyInputDeactivated(srv, ts);
	// Arm a short restore-verify pass: TS3 occasionally flips
	// CLIENT_INPUT_DEACTIVATED back to INPUT_ACTIVE 50-200 ms AFTER our
	// final flush (the PTT-up handler races our restore on PTT
	// channels, side-effect of forceVadReinit on PTT_WITH_VA). The
	// previously observed failure mode was a PTT-required room where
	// the talk mode "decayed" to VAD after sound stop. Re-running the
	// full setTalkState write at 80 / 220 / 500 ms catches every
	// observed TS3 flip window without any noticeable user impact —
	// once the state is sticky the redundant writes are no-ops on
	// the TS3 side.
	m_restoreTarget    = ts;
	m_restoreServer    = srv;
	m_restoreAttempts  = 0;
	m_restoreTimer.start(80);
}

void TalkStateManager::forceVadReinit(uint64 scHandlerID, talk_state_e target)
{
	if (scHandlerID == 0)
		return;
	// Cycle vad off and back on with flushes in between. TS3 tears
	// down + re-creates the VAD module on each "vad" toggle, which is
	// the only programmatic equivalent of the user's manual
	// mute/unmute workaround.
	ts3Functions.setPreProcessorConfigValue(scHandlerID, "vad", "false");
	ts3Functions.flushClientSelfUpdates(scHandlerID, NULL);
	ts3Functions.setPreProcessorConfigValue(scHandlerID, "vad", "true");
	ts3Functions.flushClientSelfUpdates(scHandlerID, NULL);
	// Re-write CLIENT_INPUT_DEACTIVATED to match the TARGET state.
	// The vad-cycle above can cause TS3 to silently flip INPUT to
	// ACTIVE as a side effect of re-creating the VAD module (vad=true
	// is "VAD on" which TS3 conventionally pairs with INPUT_ACTIVE).
	// Without this re-assertion a user who was in TS_PTT_WITH_VA ends
	// up in TS_VOICE_ACTIVATION (vad=true + INPUT_ACTIVE) after the
	// soundboard finishes — the recurring "PTT decays to VAD" bug
	// reported on PTT-only servers.
	int inVal = (target == TS_CONT_TRANS || target == TS_VOICE_ACTIVATION)
		? INPUT_ACTIVE : INPUT_DEACTIVATED;
	ts3Functions.setClientSelfVariableAsInt(scHandlerID,
		CLIENT_INPUT_DEACTIVATED, inVal);
	ts3Functions.flushClientSelfUpdates(scHandlerID, NULL);
}


void TalkStateManager::verifyInputDeactivated(uint64 scHandlerID, talk_state_e ts)
{
	if (scHandlerID == 0)
		return;
	bool wantActive = (ts == TS_CONT_TRANS || ts == TS_VOICE_ACTIVATION);
	int actual = 0;
	if (ts3Functions.getClientSelfVariableAsInt(scHandlerID, CLIENT_INPUT_DEACTIVATED, &actual) != ERROR_ok)
		return;
	bool actualActive = (actual == INPUT_ACTIVE);
	if (wantActive != actualActive) {
		logDebug("TSMGR: post-restore mismatch ts=%s actual=%d, re-asserting",
			toString(ts), actual);
		ts3Functions.setClientSelfVariableAsInt(scHandlerID, CLIENT_INPUT_DEACTIVATED,
			wantActive ? INPUT_ACTIVE : INPUT_DEACTIVATED);
		ts3Functions.flushClientSelfUpdates(scHandlerID, NULL);
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void TalkStateManager::setPlayTransMode()
{
	if (activeServerId == 0)
		return;
	// Only snapshot the user's real talk state the FIRST time we
	// override it. On subsequent calls (re-arm after PTT release,
	// unpause, etc.) we already know the original state.
	if (previousTalkState == TS_INVALID) {
		talk_state_e s = getTalkState(activeServerId);
		if (s == TS_INVALID)
			return;
		// Reject a TS_CONT_TRANS snapshot. setClientSelfVariable +
		// flushClientSelfUpdates is not always immediately visible to
		// the next getClientSelfVariableAsInt - on a fast play/pause/
		// unpause sequence we sometimes read back the value WE just
		// wrote in setContinuousTransmission. Using that as the
		// "original user mode" would make setTalkTransMode restore to
		// TS_CONT_TRANS forever, leaving the mic open after playback.
		// Fall back to the last known user mode if we have one, else
		// default conservatively to TS_PTT_WITHOUT_VA (the safe
		// option for PTT-required channels).
		if (s == TS_CONT_TRANS) {
			if (lastUserMode != TS_INVALID && lastUserMode != TS_CONT_TRANS)
				s = lastUserMode;
			else
				s = TS_PTT_WITHOUT_VA;
		}
		previousTalkState = s;
		lastUserMode = s;
	}
	setContinuousTransmission(activeServerId);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void TalkStateManager::setActiveServerId(uint64 id)
{
	logDebug("TSMGR: Setting active server id: %llu -> %llu",
		(unsigned long long)activeServerId, (unsigned long long)id);
	if (id == activeServerId)
		return;
	talk_state_e oldCurrentTS = currentTalkState;
	if (activeServerId != 0 && previousTalkState != TS_INVALID)
		setTalkState(activeServerId, previousTalkState);
	previousTalkState = TS_INVALID;
	// Different server may have a different user mode - drop the sticky
	// cache so the next setPlayTransMode re-snapshots fresh on the new
	// server.
	lastUserMode = TS_INVALID;
	activeServerId = id;
	if (oldCurrentTS == TS_CONT_TRANS && anySlotStillPlaying())
	{
		setPlayTransMode();
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
TalkStateManager::talk_state_e TalkStateManager::getTalkState(uint64 scHandlerID)
{
	if (scHandlerID == 0)
		return TS_INVALID;
	char *vadStr;
	if (checkError(ts3Functions.getPreProcessorConfigValue(scHandlerID, "vad", &vadStr), "Error retrieving vad setting"))
		return TS_INVALID;
	bool vad = strcmp(vadStr, "true") == 0;
	ts3Functions.freeMemory(vadStr);

	int input;
	if (checkError(ts3Functions.getClientSelfVariableAsInt(scHandlerID, CLIENT_INPUT_DEACTIVATED, &input), "Error retrieving input setting"))
		return TS_INVALID;
	bool ptt = input == INPUT_DEACTIVATED;

	if (ptt)
		return vad ? TS_PTT_WITH_VA : TS_PTT_WITHOUT_VA;
	else
		return vad ? TS_VOICE_ACTIVATION : TS_CONT_TRANS;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------s
bool TalkStateManager::setTalkState(uint64 scHandlerID, talk_state_e state)
{
	logDebug("TSMGR: Setting talk state of %llu to %s, previous was %s",
		(unsigned long long)scHandlerID, toString(state), toString(previousTalkState));
	
	if (scHandlerID == 0 || state == TS_INVALID)
		return false;

	bool va = state == TS_PTT_WITH_VA || state == TS_VOICE_ACTIVATION;
	bool in = state == TS_CONT_TRANS || state == TS_VOICE_ACTIVATION;

	if (checkError(ts3Functions.setPreProcessorConfigValue(
		scHandlerID, "vad", va ? "true" : "false"), "Error toggling vad"))
		return false;

	if (checkError(ts3Functions.setClientSelfVariableAsInt(scHandlerID, CLIENT_INPUT_DEACTIVATED,
		in ? INPUT_ACTIVE : INPUT_DEACTIVATED), "Error toggling input"))
		return false;

	ts3Functions.flushClientSelfUpdates(scHandlerID, NULL);
	currentTalkState = state;
	// Arm the cont-trans watchdog when we enter TS_CONT_TRANS so a TS3
	// PTT-up that overwrites CLIENT_INPUT_DEACTIVATED gets undone fast.
	if (state == TS_CONT_TRANS) {
		if (!m_contTransWatchdog.isActive())
			m_contTransWatchdog.start();
	} else {
		if (m_contTransWatchdog.isActive())
			m_contTransWatchdog.stop();
	}
	return true;
}


//---------------------------------------------------------------
// Purpose: Watchdog tick. Runs while we are holding TS_CONT_TRANS
// for soundboard playback. If TS3 has flipped CLIENT_INPUT_DEACTIVATED
// back to INPUT_DEACTIVATED behind our back (the PTT-key-up handler is
// the usual culprit), force it back to INPUT_ACTIVE.
//---------------------------------------------------------------
void TalkStateManager::onContTransWatchdog()
{
	if (currentTalkState != TS_CONT_TRANS) {
		m_contTransWatchdog.stop();
		return;
	}
	uint64 srv = playingServerId ? playingServerId : activeServerId;
	if (srv == 0)
		return;
	int input = 0;
	if (ts3Functions.getClientSelfVariableAsInt(srv, CLIENT_INPUT_DEACTIVATED, &input) != ERROR_ok)
		return;
	if (input != INPUT_ACTIVE) {
		logDebug("TSMGR: Watchdog re-asserting INPUT_ACTIVE (was %d)", input);
		ts3Functions.setClientSelfVariableAsInt(srv, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE);
		ts3Functions.flushClientSelfUpdates(srv, NULL);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool TalkStateManager::setPushToTalk(uint64 scHandlerID, bool voiceActivation)
{
	return setTalkState(scHandlerID, voiceActivation ? TS_PTT_WITH_VA : TS_PTT_WITHOUT_VA);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool TalkStateManager::setVoiceActivation(uint64 scHandlerID)
{
	return setTalkState(scHandlerID, TS_VOICE_ACTIVATION);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool TalkStateManager::setContinuousTransmission(uint64 scHandlerID)
{
	return setTalkState(scHandlerID, TS_CONT_TRANS);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void TalkStateManager::onClientStopsTalking()
{
	if (g_rpsbPreviewOnly)
		return;
	// If we are in PTT mode and the client lets go of the PTT key while playing a sound, ptt state gets reset to not-talking.
	if (currentTalkState == TS_CONT_TRANS && (previousTalkState == TS_PTT_WITHOUT_VA || previousTalkState == TS_PTT_WITH_VA))
		setPlayTransMode();
}

//---------------------------------------------------------------
// Purpose: Post-restore verify tick. setTalkTransMode arms this at
// 80 ms. We re-read TS3 and if the observed state has drifted from
// the user's target we re-apply it. Re-arms at 220 ms and 500 ms so
// every TS3 async-flip window (PTT-up handler races, deferred VAD
// reactions) gets caught without busy polling.
//---------------------------------------------------------------
void TalkStateManager::onRestoreVerify()
{
	if (m_restoreTarget == TS_INVALID || m_restoreServer == 0)
		return;
	if (currentTalkState == TS_CONT_TRANS)
		return; // playback re-started, abandon the verify
	talk_state_e observed = getTalkState(m_restoreServer);
	if (observed != m_restoreTarget && observed != TS_INVALID) {
		logDebug("TSMGR: restore-verify drift attempt=%d observed=%s target=%s, re-applying",
			m_restoreAttempts, toString(observed), toString(m_restoreTarget));
		setTalkState(m_restoreServer, m_restoreTarget);
		if (m_restoreTarget == TS_VOICE_ACTIVATION
		 || m_restoreTarget == TS_PTT_WITH_VA)
			forceVadReinit(m_restoreServer, m_restoreTarget);
	}
	++m_restoreAttempts;
	if (m_restoreAttempts == 1)
		m_restoreTimer.start(140); // 80 + 140 = 220 ms after restore
	else if (m_restoreAttempts == 2)
		m_restoreTimer.start(280); // 220 + 280 = 500 ms after restore
	else {
		m_restoreTarget   = TS_INVALID;
		m_restoreServer   = 0;
		m_restoreAttempts = 0;
	}
}


void TalkStateManager::onConnectionLost()
{
	// Drop active state without calling ts3Functions - on disconnect /
	// shutdown TS3 has already begun tearing down its audio backend and
	// any setClientSelfVariable / flushClientSelfUpdates call from us
	// will reach freed pointers in directsound_win64.dll / WASAPI.
	m_contTransWatchdog.stop();
	m_restoreTimer.stop();
	m_restoreTarget   = TS_INVALID;
	m_restoreServer   = 0;
	m_restoreAttempts = 0;
	previousTalkState = TS_INVALID;
	currentTalkState = TS_INVALID;
	playingServerId = 0;
	activeServerId = 0;
}

void TalkStateManager::onPreviewOnlyToggled(bool on)
{
	if (on)
	{
		// Switching ON mid-playback: the plugin had previously bumped
		// TS3 to TS_CONT_TRANS so the soundboard could be transmitted.
		// Restore the user's original talk state immediately so the
		// mic stops transmitting now, not when the sound finishes.
		if (currentTalkState == TS_CONT_TRANS && previousTalkState != TS_INVALID)
			setTalkTransMode();
	}
	else
	{
		// Switching OFF mid-playback: if any slot is still active we
		// need to re-arm continuous transmission so the soundboard can
		// reach the server again.
		if (anySlotStillPlaying())
			setPlayTransMode();
	}
}
