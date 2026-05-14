
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
	activeServerId(0),
	playingServerId(0),
	m_sampler(NULL)
{

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
	// Skip the TS3 API call if the client is already in the target
	// state. Avoids an unnecessary flushClientSelfUpdates round-trip
	// that can briefly glitch VAD / continuous-transmission.
	uint64 srv = playingServerId ? playingServerId : activeServerId;
	talk_state_e current = getTalkState(srv);
	if (current != TS_INVALID && current == ts) {
		currentTalkState = ts;
		return;
	}
	setTalkState(srv, ts);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void TalkStateManager::setPlayTransMode()
{
	// Only snapshot the user's real talk state the FIRST time we
	// override it. On subsequent calls (re-arm after PTT release,
	// unpause, etc.) we already know the original state.
	if (previousTalkState == TS_INVALID) {
		talk_state_e s = getTalkState(activeServerId);
		if (s == TS_INVALID)
			return;
		previousTalkState = s;
	}
	setContinuousTransmission(activeServerId);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void TalkStateManager::setActiveServerId(uint64 id)
{
	logDebug("TSMGR: Setting active server id: %i -> %i", (int)activeServerId, (int)id);
	if (id == activeServerId)
		return;
	talk_state_e oldCurrentTS = currentTalkState;
	if (activeServerId != 0 && previousTalkState != TS_INVALID)
		setTalkState(activeServerId, previousTalkState);
	previousTalkState = TS_INVALID;
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
	logDebug("TSMGR: Setting talk state of %ull to %s, previous was %s",
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
	return true;
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
