// src/samples.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__samples_H__
#define rpsbsrc__samples_H__

#include <QObject>

#include "SampleBuffer.h"
#include "SampleProducerThread.h"
#include "peakmeter.h"

#include <mutex>
#include <atomic>
#include <vector>
#include <cstring>
#include <cmath>

class InputFile;
class SoundInfo;


struct OlaState
{
	static const int WIN = 1024;       // ~21ms window at 48kHz
	static const int HOP = 128;        // analysis hop = WIN/8
	static const int ACC_SIZE = 8192;  // persistent output accumulator

	float window[WIN];                 // Hann window (precomputed)
	float accL[ACC_SIZE];              // output accumulator (left)
	float accR[ACC_SIZE];              // output accumulator (right)
	int accValid;                      // synthesis write head (fully-accumulated count)
	int accRead;                       // output read head
	double bufPos;                     // fractional read position in sample buffer
	bool inited;

	OlaState() : accValid(0), accRead(0), bufPos(0.0), inited(false) {}
	void init();
	void reset();
	void shift();  // shift accumulator to reclaim space
};


class Sampler : public QObject
{
	Q_OBJECT

public:
	enum state_e
	{
		eSILENT = 0,
		ePLAYING,
		ePAUSED,
		ePLAYING_PREVIEW,
	};

	static const int MAX_SLOTS = 5;

public:
	Sampler();
	~Sampler();
	void init();
	void shutdown();
	int fetchInputSamples(short *samples, int count, int channels, bool *finished);
	int fetchOutputSamples(short *samples, int count, int channels, const unsigned int *channelSpeakerArray, unsigned int *channelFillMask);
	bool playFile(const SoundInfo &sound);
	bool playPreview(const SoundInfo &sound);
	void stopPlayback(int slot = -1);  // -1 = stop all
	void setVolumeLocal(int vol);
	void setVolumeRemote(int vol);
	void setLocalPlayback(bool enabled);
	void setMuteMyself(bool enabled);
	void setEarrapeProtection(bool enabled);
	void setPitchFactor(float factor);
	void setSpeedFactor(float factor);
	void setIntensityFactor(float factor);
	void setReverbMix(float mix);
	float getPitchFactor() const { return m_pitchFactor; }
	float getSpeedFactor() const { return m_speedFactor; }
	float getIntensityFactor() const { return m_intensityFactor; }
	float getReverbMix() const { return m_reverbMix; }
	void pausePlayback(int slot = -1);
	void unpausePlayback(int slot = -1);
	double getPosition(int slot = 0);
	double getLength(int slot = 0);
	void seek(double seconds, int slot = 0);
	state_e getState(int slot = 0) const;
	void setMultiMode(bool enabled);
	bool isMultiMode() const { return m_multiMode; }
	void setSlotVolumeLocal(int slot, int vol);
	void setSlotVolumeRemote(int slot, int vol);
	void setSlotPitchFactor(int slot, float factor);
	void setSlotSpeedFactor(int slot, float factor);
	void setSlotReverbMix(int slot, float mix);
	int getActiveSlotCount() const;
	int findSlotByState(state_e state) const; // find first slot with given state

signals:
	void onStartPlaying(int slot, bool preview, QString filename);
	void onStopPlaying(int slot);
	void onPausePlaying(int slot);
	void onUnpausePlaying(int slot);

private:
	struct PlaybackSlot
	{
		SampleBuffer sbCapture;
		SampleBuffer sbPlayback;
		SampleProducerThread producerThread;
		InputFile *inputFile;
		std::atomic<state_e> state;
		double soundDbSetting;
		double slotDbLocal;    // per-slot local volume (multi-mode)
		double slotDbRemote;   // per-slot remote volume (multi-mode)

		PlaybackSlot();
	};

	void stopSlotInternal(int slot);
	bool playSoundInSlot(int slot, const SoundInfo &sound, bool preview);
	int findFreeSlot() const;
	void setVolumeDb(double decibel);
	int fetchSamples(SampleBuffer &sb, PeakMeter &pm, short *samples, int count, int channels, bool eraseConsumed, int ciLeft, int ciRight, bool overLeft, bool overRight, float ampThresh = 0.0f);
	int findChannelId(unsigned int channel, const unsigned int *channelSpeakerArray, int count);

private:
	PlaybackSlot m_slots[MAX_SLOTS];
	PeakMeter m_peakMeterCapture;
	PeakMeter m_peakMeterPlayback;
	int m_volumeDivider;
	float m_volumeFactor;
	static const int volumeScaleExp = 12;
	double m_globalDbSettingLocal;
	double m_globalDbSettingRemote;
	std::mutex m_mutex;
	std::atomic<bool> m_localPlayback;
	std::atomic<bool> m_muteMyself;
	std::atomic<bool> m_earrapeProtection;
	float m_pitchFactor;
	float m_speedFactor;
	float m_intensityFactor;
	float m_reverbMix;
	bool m_multiMode;
};


#endif // rpsbsrc__samples_H__
