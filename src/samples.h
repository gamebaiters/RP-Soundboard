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
#include <memory>
#include <thread>
#include <vector>
#include <cstring>
#include <cmath>

class InputFile;
#include "SoundInfo.h"
class SlotDsp;
struct SandboxState;


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

	// One slot per visible channel + 1 reserved for the preview dialog.
	// 32 keeps the playback array tiny (only allocated when init() runs)
	// while giving the UI an effectively-unlimited channel cap.
	static const int MAX_SLOTS = 32;

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
	// Replace the per-sound dB modifier mid-playback (used by the preview
	// dialog so the volume slider takes effect immediately).
	void setSlotSoundDb(int slot, double db);
	int getActiveSlotCount() const;
	int findSlotByState(state_e state) const; // find first slot with given state

	// --- Audio-sandbox per-slot DSP API ---
	// Apply / replace the slot's sandbox state. Lazily allocates the DSP
	// block the first time a non-default state hits a slot.
	void setSlotLoop(int slot, bool on);
	// Per-channel reverse-playback override. Toggling mid-playback
	// stops the slot and re-triggers the last sound with the new
	// reverse flag so the user gets immediate audible feedback.
	void setSlotReverse(int slot, bool on);
	void setSlotSandboxState(int slot, const SandboxState &s);
	// Drop the slot's DSP block (frees ~few hundred KB). Used by the
	// "Reset all sandbox" command in Settings.
	void clearSlotSandbox(int slot);
	// Latest peak |L|, |R| (0..1) measured at the slot's DSP output. The
	// channel meter widget polls these via QTimer at ~25 Hz.
	void getSlotPeak(int slot, float &peakL, float &peakR) const;
	// Rolling CPU% spent in the slot's DSP chain over the last poll
	// window. 0 when the slot has no DSP block. Self-resets the counter
	// at read, so consecutive calls report consecutive windows.
	double getSlotCpuPercent(int slot);
	// Read this slot's playback-path EQ band levels (16 floats, 0..1).
	// Out array zeroed when the slot has no DSP block yet.
	void   getSlotEqBandLevels(int slot, float out[16]) const;
	// True when ANY slot is in ePLAYING or ePAUSED. Cheap atomic-only
	// scan, no mutex. GUI timers gate their per-channel iteration on
	// this so they idle to zero when nothing is playing.
	bool   anyPlaying() const;
	// Crop range (seconds) currently applied to a slot. endSec < 0 means
	// no end point. Both 0 / negative means the slot has no crop.
	void getSlotCrop(int slot, double &startSec, double &endSec) const;
	// Live-update the slot's crop range (e.g. from the waveform context
	// menu). Affects loop restart point and the marker overlay; does
	// not retrigger the decoder, so the new end point only takes effect
	// after a re-play. endSec < 0 means "no end point".
	void setSlotCropLive(int slot, double startSec, double endSec);

signals:
	void onStartPlaying(int slot, bool preview, QString filename);
	void onStopPlaying(int slot);
	void onPausePlaying(int slot);
	void onUnpausePlaying(int slot);
	// A sound failed to open (missing / unsupported / damaged file). The
	// UI shows a clear message instead of the client silently doing
	// nothing or crashing.
	void onPlaybackError(int slot, QString filename);

public:
	// Direct slot-targeted play - the new modular UI uses this to do its
	// own slot picking (round-robin across visible channels) instead of
	// relying on the sampler's free-slot heuristic.
	bool playSoundInSlot(int slot, const SoundInfo &sound, bool preview);

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

		// Lazy-allocated audio-sandbox DSP block. Null when the channel
		// has never had its sandbox toggled on - which keeps memory
		// flat for users who don't use the feature.
		std::unique_ptr<SlotDsp> dsp;
		// Latest peak as observed by the meter (atomic for lock-free
		// read from the GUI thread).
		std::atomic<float> peakL{0.0f};
		std::atomic<float> peakR{0.0f};
		// Lock-free position / length cache. Updated by the audio
		// thread inside fetchInputSamples once per cycle; read by the
		// GUI position-poll timer at ~30 Hz. Without this the GUI
		// polled Sampler::getPosition / getLength under m_mutex 30
		// times a second per channel, contending against the audio
		// thread's mutex hold inside fetchSamples - measurable as a
		// system-wide lag on the GUI thread (~30 ms slider stalls)
		// once the Leia engine started running its 5 ms convolution
		// blocks inside that same lock. Atomic double is lock-free on
		// every platform we ship (x86-64, ARM64).
		std::atomic<double> cachedPositionSec{0.0};
		std::atomic<double> cachedLengthSec{0.0};
		// Anchor flag for the cursor rate-limiter inside fetchInputSamples.
		// False after a play / seek / loop-restart so the first cycle
		// snaps the cache to the fresh truth; true thereafter so jumps
		// from live speed/pitch changes are clamped against the prior
		// frame. Owned by the audio thread (set/cleared under m_mutex).
		bool posCacheValid = false;
		// Latest FxPanel reverb value (0..1). Stored on the slot so
		// the routing decision (decoder vs end-of-DSP reverb) can be
		// re-applied whenever the slot's dsp is created/cleared.
		float fxReverbWet = 0.0f;
		bool loop = false;
		// Per-channel reverse-playback toggle (set from the WaveformPlayer
		// reverse button via setSlotReverse). When ON, every new play
		// through this slot is forced to reverse mode regardless of
		// SoundInfo.reverse. Toggling mid-play hands the heavy
		// pre-decode + filter-graph build to reverseWorker so the GUI
		// and audio threads never block — the OLD inputFile keeps
		// playing in the original direction until the worker swaps a
		// fully-built new one in under a micro-lock.
		bool channelReverse = false;
		// Background worker that builds the new (reverse / forward)
		// InputFile off the audio path. Replaced on every rapid click;
		// the prior worker is signalled via reverseWorkerCancel and
		// detached — when it finishes, it sees its cancel flag (or the
		// epoch mismatch) and discards its half-built buffer.
		std::thread                        reverseWorker;
		std::shared_ptr<std::atomic<bool>> reverseWorkerCancel;
		// Monotonic counter bumped on every setSlotReverse /
		// stopSlotInternal / playSoundInSlot. The async worker captures
		// the value at spawn time and verifies it matches under the
		// final swap lock; mismatch = "user moved on" so the worker
		// throws away its new InputFile instead of stomping the slot.
		std::atomic<uint64_t> reverseEpoch{0};
		SoundInfo lastSound;
		bool      lastSoundValid = false;
		// Latest pitch factor pushed via setSlotPitchFactor by the
		// wiring layer (FxPanel slider). Stored so the loop-restart
		// jitter code can apply random multipliers ON TOP of the
		// channel's intended pitch instead of clobbering it with
		// the global m_pitchFactor (which defaults to identity).
		float  lastSlotPitchFactor = 1.0f;
		double stretchBaseTime = 0.0;
		// Trim start (seconds) of the currently loaded sound. A looping
		// slot must restart from here, not from the file start, otherwise
		// the loop ignores the per-cell crop range.
		double cropStart = 0.0;
		// Trim end (seconds), or < 0 when the cell has no end point set.
		// Used only to drive the waveform crop markers.
		double cropEnd = -1.0;

		// Per-playback random pitch jitter feature. The audio thread
		// re-reads the live SandboxState (slot.dsp->state()) at every
		// loop restart, so when the user disables random mid-playback
		// the next loop iteration drops the jitter immediately. The
		// `randomActive` flag is just a fast-path skip for the
		// common no-random case.
		bool   randomActive    = false;

		// Sidechain ducking. duckSource: when this slot is playing it
		// attenuates every OTHER slot's output by duckOthersDb. duckGain:
		// the smoothly attacked / released gain currently APPLIED to
		// this slot's output (so a slot can simultaneously be a source
		// and be ducked by another source). Both copies are pushed
		// from the sandbox state at applyState / playSoundInSlot time
		// so the audio thread reads them lock-free.
		bool                duckSource       = false;
		float               duckOthersDb     = -12.0f;
		std::atomic<float>  duckGain         { 1.0f };

		PlaybackSlot();
		~PlaybackSlot();
	};

	void stopSlotInternal(int slot);
	// Async reverse-toggle worker body. Runs on a detachable std::thread
	// spawned by setSlotReverse. Builds a fresh InputFile (heavy
	// pre-decode for reverse, plain open for forward), then takes
	// m_mutex briefly to swap. Self-discards on epoch mismatch,
	// m_shuttingDown true, or cancel-token true.
	void reverseWorkerProc(int slot, uint64_t epoch, bool wantReverse,
	                       SoundInfo sound, double resumeSec,
	                       float pitchBase, float speedFactor,
	                       float reverbMix,
	                       std::shared_ptr<std::atomic<bool>> cancel);
	int findFreeSlot() const;
	void setVolumeDb(double decibel);
	int fetchSamples(SampleBuffer &sb, PeakMeter &pm, short *samples, int count, int channels, bool eraseConsumed, int ciLeft, int ciRight, bool overLeft, bool overRight, float ampThresh = 0.0f, PlaybackSlot *slot = nullptr);
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
	// Set by shutdown() so any in-flight reverse worker about to swap
	// can see "Sampler is dying" and quietly discard its work instead
	// of touching the slot's m_mutex / inputFile after teardown.
	std::atomic<bool> m_shuttingDown{false};
};


#endif // rpsbsrc__samples_H__
