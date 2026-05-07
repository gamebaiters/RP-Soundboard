#pragma once
#include <QObject>
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

private:
	bool anySlotStillPlaying() const;
	void setTalkTransMode();
	void setPlayTransMode();
	talk_state_e previousTalkState;
	talk_state_e defaultTalkState;
	talk_state_e currentTalkState;
	uint64 activeServerId;
	uint64 playingServerId;
	Sampler *m_sampler;

};
