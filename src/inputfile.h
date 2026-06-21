// src/inputfile.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__inputfile_H__
#define rpsbsrc__inputfile_H__

#include <stdint.h>
#include <atomic>
#include "SampleSource.h"

class SampleBuffer;

struct InputFileOptions
{
	enum channel_layout_e
	{
		MONO = 0,
		STEREO,
	};

	channel_layout_e outputChannelLayout;
	int outputSampleRate;

	InputFileOptions() :
		outputChannelLayout(STEREO),
		outputSampleRate(48000)
	{}

	inline int getNumChannels() const
	{
		switch(outputChannelLayout)
		{
		case MONO:   return 1;
		case STEREO: return 2;
		default: return 0;
		}
	}
};


class InputFile : public SampleSource
{
public:
	virtual ~InputFile() {};
	virtual int open(const char *filename, double startPosSeconds = 0.0, double playTimeSeconds = -1.0) = 0;
	virtual int close() = 0;
	virtual bool done() const = 0;
	virtual int seek(double seconds) = 0;
	virtual double getPosition() const = 0;
	virtual double getLength() const = 0;
	virtual int64_t outputSamplesEstimation() const = 0;
	virtual void setPitchFactor(float factor) { (void)factor; }
	virtual void setSpeedFactor(float factor) { (void)factor; }
	virtual float getSpeedFactor() const { return 1.0f; }
	// In streaming-reverse mode the LIVE m_speedFactor (set by the
	// last setSpeedFactor call) lags the audio actually being heard
	// by one chunk worth, because the chunk currently feeding the
	// reader was decoded with the speed value that was set BEFORE the
	// most recent change. The Sampler position cache uses this for
	// posSec descent so heavy speed dragging does not make the
	// reverse cursor race ahead of or fall behind the audible head.
	// Forward path / non-reverse files: returns getSpeedFactor().
	virtual float getCurrentReverseChunkSpeed() const { return getSpeedFactor(); }
	virtual void setReverbMix(float mix) { (void)mix; }
	// Live-update the decoder's end-of-playback bound. <0 = unlimited.
	// Lets the waveform right-click crop editor truncate the active
	// playback without restarting the slot.
	virtual void setMaxPlayTime(double seconds) { (void)seconds; }
	// Set BEFORE open(): when true the FFmpeg filter graph adds an
	// `areverse` filter so the file plays back-to-front. Memory cost
	// scales with file length (areverse buffers everything) so this
	// is intended for short SFX cells, not multi-minute music.
	virtual void setReverse(bool on) { (void)on; }
	// Cooperative cancel token for the reverse pre-decode pass.
	// preDecodeAndReverse polls *token every N decoded packets; if
	// true it bails out early and open() returns -1 so the worker can
	// discard the half-built buffer without waiting on a multi-minute
	// file. nullptr (default) = no cancellation. Lifetime of the
	// pointed-to atomic must outlive open().
	virtual void setCancelToken(std::atomic<bool> *token) { (void)token; }
	// Streaming-reverse interrogation: returns true once the chunk
	// worker has buffered at least one decoded chunk so the caller
	// can swap the new InputFile in without leaving the audio thread
	// reading from an empty queue. Returns true immediately for input
	// files that are not in streaming-reverse mode.
	virtual bool isReverseFirstChunkReady() const { return true; }
	// Set BEFORE open(): when true the FFmpeg filter graph adds a
	// `loudnorm` filter targeting -16 LUFS integrated / -1 dBTP peak
	// so loud cells don't drown quiet ones. EBU R128 single-pass mode
	// (good enough for live playback; not the offline two-pass quality).
	virtual void setAutoNormalize(bool on) { (void)on; }
};

extern InputFile *CreateInputFileFFmpeg(InputFileOptions options = InputFileOptions());
extern void InitFFmpegLibrary();


#endif // rpsbsrc__inputfile_H__
