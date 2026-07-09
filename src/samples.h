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
#include <condition_variable>
#include <limits>
#include <memory>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cmath>

class InputFile;
#include "SoundInfo.h"
class SlotDsp;
struct SandboxState;


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
	// Master duck gain (0..1) applied to the SOUNDBOARD contribution in every
	// fetchSamples mix (capture + playback), leaving mic/host audio untouched.
	// Driven by the "lower soundboard when I talk" voice setting. Atomic; the
	// audio thread reads it lock-free.
	void setMasterDuckGain(float g) { m_masterDuckGain.store(g, std::memory_order_relaxed); }
	// TARGET soundboard gain for ducking (0..1). fetchSamples ramps the actual
	// gain toward this per-sample so talking never produces an abrupt step.
	void setDuckTarget(float t) { m_duckTarget.store(t, std::memory_order_relaxed); }
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

	// Like playSoundInSlot but runs the (potentially blocking) open() on a
	// background worker so a NETWORK stream never freezes the GUI thread. The
	// FX / volume values are captured by the CALLER on the GUI thread and passed
	// in (no widget access on the worker). After a successful open the worker
	// applies them + optionally pauses. Signals still fire (queued) to the GUI.
	// The worker is tracked like the reverse workers (bounded-joined at
	// shutdown), so it can never become a ghost or a use-after-free.
	void   playSoundInSlotAsync(int slot, const SoundInfo &sound,
	                            int volLocal, int volRemote,
	                            float pitchFactor, float speedFactor, float reverbMix,
	                            bool applyFx, bool autoPlay);
	// Crop range (seconds) currently applied to a slot. endSec < 0 means
	// no end point. Both 0 / negative means the slot has no crop.
	void getSlotCrop(int slot, double &startSec, double &endSec) const;
	// True while a network stream in this slot is recovering from a stall
	// (feeding silence). Lock-free read of the cached mirror; drives the GUI
	// "buffering" notice.
	bool getSlotNetBuffering(int slot) const;
	// True (one-shot-ish) when a network stream in this slot declared a stall at
	// the seek target unrecoverable. Lock-free; drives the "network error" toast.
	bool getSlotNetFailed(int slot) const;
	// Live-update the slot's crop range (e.g. from the waveform context
	// menu). Affects loop restart point and the marker overlay; does
	// not retrigger the decoder, so the new end point only takes effect
	// after a re-play. endSec < 0 means "no end point".
	void setSlotCropLive(int slot, double startSec, double endSec);

	// --- Tape stop (vinyl brake, D1) ---
	// tapeStop engages the brake (rate ramps 1 -> 0 over brakeMs); when
	// it reaches zero the audio thread auto-pauses the slot. tapeRelease
	// spins back up (and unpauses when fully stopped). tapeState returns
	// TapeStop::Phase as int (0=Idle 1=Braking 2=Stopped 3=SpinUp
	// 4=CatchUp) for the vinyl popup animation. All GUI-thread.
	void tapeStop(int slot, float brakeMs);
	void tapeRelease(int slot, float spinMs);
	int  tapeState(int slot) const;
	// Popup visibility: while armed the tape ring ingests history and
	// the head tracks live (audibly transparent) so a backward drag
	// right after opening the popup already has material under it.
	void tapeArm(int slot, bool on);
	// Scratch (DJ drag on the vinyl popup): begin on grab (resumes a
	// tape-stopped slot), angular displacement streamed per mouse move
	// as SECONDS of tape (position-locked servo - the audio tracks the
	// disc 1:1, both directions), end = hand off -> spin back up.
	void tapeScratchBegin(int slot);
	// In-ring scratch first; overflow past the ring walls (forward
	// beyond live / backward beyond the history) becomes an async
	// decoder seek + tape scratchRebase() so the drag continues
	// endlessly through the whole file (CDJ-style needle jumps at the
	// walls, true scratch inside them).
	void tapeScratchDelta(int slot, float deltaSeconds);
	void tapeScratchEnd(int slot, float spinMs);

	// Global EBU R128 loudness normalization (Q2). When on, every sound
	// opens with the decoder's loudnorm filter (-16 LUFS single-pass)
	// even if the per-cell "normalize" checkbox is off. Takes effect on
	// the NEXT play of each sound. (Reverse mode still skips loudnorm -
	// the per-chunk graph rebuild would restart its convergence.)
	void setGlobalNormalize(bool on) {
		m_globalNormalize.store(on, std::memory_order_relaxed);
	}

signals:
	void onStartPlaying(int slot, bool preview, QString filename);
	void onStopPlaying(int slot);
	void onPausePlaying(int slot);
	void onUnpausePlaying(int slot);
	// A sound failed to open (missing / unsupported / damaged file). The
	// UI shows a clear message instead of the client silently doing
	// nothing or crashing.
	void onPlaybackError(int slot, QString filename);
	// Async seek finished. Emitted on the worker thread (Qt auto-
	// connects with QueuedConnection across threads); the GUI uses it
	// to release the per-channel "seekPending" poll lock so the
	// cursor stops following the user's click target and resumes
	// tracking the live decoder position.
	void onSeekCommitted(int slot);

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
		// Lock-free mirror of inputFile->isNetBuffering(), refreshed by the
		// audio thread alongside the position cache. GUI poll reads it to show
		// a "buffering" notice while a network stream recovers from a stall.
		std::atomic<bool>   cachedNetBuffering{false};
		// Lock-free mirror of inputFile->netFailed() — GUI shows a transient
		// "network error" toast on the rising edge.
		std::atomic<bool>   cachedNetFailed{false};
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
		// Anti-glitch loop-rate guard. The streaming-reverse decoder can
		// occasionally produce a chunk that covers far less than the
		// requested input range (codec / atempo state confused by rapid
		// pitch / speed dragging on a short audio). Reader plays the
		// tiny chunk, cursor descends to minF immediately, natural-end
		// fires, samples.cpp loops back to cropEnd, the next decode
		// produces another tiny chunk — user hears the last few ms of
		// reverse playback looping at 30-200 Hz, "audio glitches +
		// repeats forever" bug. If loops fire faster than the threshold
		// (= sub-perceptual loop), suppress further loop restarts and
		// let the slot die cleanly so the user hears silence instead of
		// glitch noise. Monotonic ms (steady_clock) so any wall-clock
		// adjustment can't break this.
		int64_t lastLoopMonoMs   = 0;
		int     loopBurstCount   = 0;
		int64_t loopBurstStartMs = 0;
		// Per-channel reverse-playback toggle (set from the WaveformPlayer
		// reverse button via setSlotReverse). When ON, every new play
		// through this slot is forced to reverse mode regardless of
		// SoundInfo.reverse. Toggling mid-play hands the heavy
		// pre-decode + filter-graph build to reverseWorker so the GUI
		// and audio threads never block — the OLD inputFile keeps
		// playing in the original direction until the worker swaps a
		// fully-built new one in under a micro-lock.
		bool channelReverse = false;
		// True whenever audio is currently flowing in reverse, regardless
		// of WHICH source flipped the bit on (per-channel override or
		// per-cell SoundInfo.reverse flag). Drives the reverse cursor
		// branch in fetchInputSamples — without this flag a SoundInfo-
		// flagged reverse cell would render reverse audio but the GUI
		// would walk the forward cursor, locking the cursor at the start.
		bool audioReverse = false;
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

		// Async seek queue. Sampler::seek (GUI thread) writes the
		// latest target here and notifies the dedicated seek worker
		// (one shared worker, sees all slots). The worker performs
		// the slow FFmpeg backward scan OFF the GUI thread so a 10-h
		// MP3 click never freezes the soundboard. NaN = no pending
		// seek. Rapid spam collapses to "last target wins" because
		// the GUI debounce upstream + this atomic both keep only the
		// most recent value.
		std::atomic<double> pendingSeekSec{
			std::numeric_limits<double>::quiet_NaN()};

		// Scratch-seek deadband accumulator (seconds, output-domain).
		// Forward drags at the live edge / backward drags beyond the
		// ring history collect here and fire an async seek only past
		// +/-0.25 s - hand jitter must never trigger a seek (a seek
		// rebases the tape ring). GUI thread only.
		float tapeSeekAccum = 0.0f;
		// Needle-jump pacing: steady-clock ms of the last scrub seek.
		// At most one seek + rebase fires per 120 ms; overflow keeps
		// accumulating in between so fast drags make fewer, bigger
		// hops instead of out-racing the async seek worker.
		int64_t lastScrubMs = 0;
		// Last committed scrub target (input seconds, GUI thread only,
		// NaN = none this gesture). The seek worker NaNs pendingSeekSec
		// when it COLLECTS a batch - during the slow FFmpeg scan a new
		// hop would chain off the stale position cache (audio is still
		// draining pre-seek content) and land BEHIND the in-flight
		// target: a fast forward drag visibly jumped backward. Chaining
		// off max(cache, lastScrubTarget) in the drag direction keeps
		// the hop sequence monotone. Reset by tapeScratchBegin.
		double lastScrubTargetSec = std::numeric_limits<double>::quiet_NaN();
		// Direction (+1/-1, 0 = none) of the last fired hop: opposite-
		// direction hops need 350 ms separation so wiggle strokes
		// cancel in the accumulator instead of seek-storming both ways.
		int lastScrubDir = 0;
		// Natural-end defer deadline (steady ms, 0 = unarmed): a
		// re-timing tape at EOF may play its remaining history for at
		// most lag+1 s before the slot is allowed to end - without the
		// cap a deep-lag CatchUp (drains at 0.3%) became an immortal
		// silence-playing slot.
		int64_t tapeEndDeferMs = 0;
		// Cursor anchor for a scratch gesture: the audible file position
		// at the instant the disc was grabbed. During the scratch the
		// cursor = this + tapeHeadDisplacementSeconds()*speed, which
		// follows the head's true file position even as backfill slides
		// the ring across the whole file (the head-to-frontier lag stays
		// pinned by the frontier trim and can't drive the cursor). NaN
		// when no scratch is in progress. GUI/audio-thread read.
		double scratchStartCursorSec = std::numeric_limits<double>::quiet_NaN();
		// Release re-home ("resta indietro"): on a big BACKWARD scratch the
		// frontier trim decouples the main decoder from the head (the
		// decoder stays parked at the pre-scratch spot while the ring slides
		// across the file via backfill). Releasing must resume forward from
		// the NEEDLE, not snap back to the stranded decoder. tapeScratchEnd
		// spins the flywheel up on the ring and fires an async prime-seek
		// that re-homes the decoder to the ring's frontier file position.
		// While this flag is set: (1) the cursor stays pinned to the head
		// (scratchStartCursorSec + headDisplacement) instead of the stranded
		// decoder formula; (2) the tape ingest is HELD so the stale
		// pre-seek buffer is never spliced into the ring. Cleared by the
		// seek worker at prime-commit (buffer refilled from the needle).
		std::atomic<bool> tapePrimeSeek{false};

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

		// ---- Backward-infinite vinyl backfill ----
		// A dedicated decoder + a single-chunk handoff feed OLDER audio
		// into the playback tape ring's oldest end, so backward scratch
		// is unbounded while the ring stays a fixed 10 s. All decode work
		// runs on m_backfillWorker; the audio thread only drains a ready
		// chunk (try_lock, never blocks). See Sampler::backfillWorkerProc.
		InputFile*          backfillFile = nullptr;   // worker-owned, lazy
		QString             backfillPath;             // desired file (set by play)
		QString             backfillOpenPath;         // worker-only: file open on
		std::mutex          backfillMutex;            // guards the chunk
		std::vector<float>  backfillChunkL, backfillChunkR;
		std::atomic<int>    backfillChunkN { 0 };     // ready samples (0=none)
		std::atomic<double> backfillNextFileSec { 0.0 }; // file pos of tape oldest
		std::atomic<float>  backfillSpeed { 1.0f };
		// Mirror the live FxPanel pitch + reverb onto the backfill decoder
		// so backward-scratched history sounds like the forward stream. The
		// backfill decoder is a SEPARATE InputFile; without these its output
		// was dry / un-pitched, so scratching back into backfilled audio cut
		// the effects out ("gli effetti si attivano in ritardo"). Speed is
		// already matched via backfillSpeed. (The per-sample sandbox chain
		// is not re-applied to backfill - only the decoder-graph FX.)
		std::atomic<float>  backfillPitch  { 1.0f };
		std::atomic<float>  backfillReverb { 0.0f };
		std::atomic<bool>   backfillActive { false }; // scratch in progress
		std::atomic<bool>   backfillAtStart { false };// reached file start

		PlaybackSlot();
		~PlaybackSlot();
	};

	// Tear down a slot's playback state under m_mutex, but DEFER the two
	// operations that must not run inside the audio lock: the FFmpeg
	// context teardown (returned InputFile* - caller closes + deletes it
	// after releasing m_mutex) and the onStopPlaying emit (emitStop -
	// emitting Qt signals while holding m_mutex is a latent deadlock for
	// any direct-connection slot that calls back into the Sampler).
	InputFile *stopSlotInternal(int slot, bool &emitStop);
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
	int fetchSamples(SampleBuffer &sb, PeakMeter &pm, short *samples, int count, int channels, bool eraseConsumed, int ciLeft, int ciRight, bool overLeft, bool overRight, float ampThresh = 0.0f, PlaybackSlot *slot = nullptr, bool forceLimit = false);
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
	std::atomic<bool> m_globalNormalize{false};
	std::atomic<float> m_masterDuckGain{1.0f};   // current (smoothed) duck gain
	std::atomic<float> m_duckTarget{1.0f};       // target duck gain (ramped to)
	float m_pitchFactor;
	float m_speedFactor;
	float m_intensityFactor;
	float m_reverbMix;
	bool m_multiMode;
	// Set by shutdown() so any in-flight reverse worker about to swap
	// can see "Sampler is dying" and quietly discard its work instead
	// of touching the slot's m_mutex / inputFile after teardown.
	std::atomic<bool> m_shuttingDown{false};
	// Retired reverse workers. setSlotReverse used to join() the previous
	// worker on the GUI thread (10-100 ms freeze on rapid toggles); now
	// the old thread is parked here and joined opportunistically (list
	// overflow) or at shutdown - each worker self-terminates within
	// ~100 ms of its cancel token flipping, so joins here are short.
	std::mutex m_retiredMutex;
	std::vector<std::thread> m_retiredWorkers;

	// Dedicated seek worker. One std::thread shared across all slots
	// processes pendingSeekSec values async — Sampler::seek (GUI
	// thread) just sets the atomic + notifies; the worker does the
	// hundreds-of-ms FFmpeg backward-scan and then briefly takes
	// m_mutex to commit the buffer-clear + cache-update + DSP reset.
	// Started lazily on first seek() call; joined in shutdown().
	std::thread             m_seekWorker;
	std::mutex              m_seekMutex;
	std::condition_variable m_seekCv;
	std::atomic<bool>       m_seekStop{false};
	std::atomic<bool>       m_seekWorkerStarted{false};
	void seekWorkerProc();
	void startSeekWorker();

	// ---- Backward-infinite vinyl backfill worker ----
	// Decodes OLDER chunks off-thread for the actively-scratching slot
	// and hands them to the audio thread (see PlaybackSlot backfill
	// members). Started lazily on the first scratch; joined in shutdown.
	std::thread             m_backfillWorker;
	std::mutex              m_backfillMutex;
	std::condition_variable m_backfillCv;
	std::atomic<bool>       m_backfillStop{false};
	std::atomic<bool>       m_backfillStarted{false};
	std::atomic<int>        m_backfillSlot{-1};   // slot being scratched, -1=none
	void backfillWorkerProc();
	void startBackfillWorker();
	// GUI-thread: begin / end a backfill session for a scratching slot.
	void backfillBegin(int slot);
	void backfillEnd(int slot);
	// Re-home the main decoder to `frontierSec` (input seconds) so forward
	// playback continues from where a backward scratch left the needle.
	// Unlike seek() this never snap-resets the tape (the flywheel keeps
	// spinning on the ring) and the commit preserves the tape/cursor state
	// - it only repositions the decoder that feeds the ring frontier.
	void primeSeekDecoder(int slot, double frontierSec);
};


#endif // rpsbsrc__samples_H__
