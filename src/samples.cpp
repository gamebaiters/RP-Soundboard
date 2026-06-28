// src/samples.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "common.h"

#include "inputfile.h"
#include "samples.h"
#include "SoundInfo.h"
#include "ts3log.h"
#include "HighResClock.h"
#include "AudioUtils.h"
#include "dsp/SlotDsp.h"
#include "dsp/SandboxState.h"

#include <queue>
#include <vector>
#include <cassert>
#include <chrono>
#include <math.h>

// ===== FILE DEBUG LOGGING =====
// Compile-time gate. Always 1 in shipping builds; the runtime
// checkbox flips g_rpsbLogsEnabled which is what gates each write.
#define RPSB_FILE_DEBUG 1
#if RPSB_FILE_DEBUG
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdlib>
#include <cstring>
#include <limits.h>
#endif
#include <cstdio>
#include <cstdarg>
#include <ctime>
static FILE *g_dbgSamples = nullptr;
#include "plugin.h"
#include "ts3log.h"
static void sdbgOpen()
{
	if (!g_rpsbLogsEnabled) return;
	if (!g_dbgSamples)
	{
		const char *cfgDir = getTs3ConfigPath();
		if (cfgDir && cfgDir[0])
		{
			char path[PATH_BUFSIZE + 64];
			snprintf(path, sizeof(path), "%srpsb_debug.log", cfgDir);
			g_dbgSamples = fopen(path, "a");
		}
	}
}
static void sdbgLog(const char *fmt, ...)
{
	// Format into a stack buffer once, then send the same line to
	// both the file and the in-memory ring used by the in-app log
	// viewer. Mirrors the policy in inputfileffmpeg.cpp's dbgLog.
	if (!g_rpsbLogsEnabled) return;
	char buf[1024];
	int written = 0;
	written = snprintf(buf, sizeof(buf), "[samples] ");
	if (written < 0) written = 0;
	if (written > static_cast<int>(sizeof(buf)) - 1)
		written = static_cast<int>(sizeof(buf)) - 1;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf + written, sizeof(buf) - written, fmt, ap);
	va_end(ap);
	sdbgOpen();
	if (g_dbgSamples) {
		fputs(buf, g_dbgSamples);
		fputc('\n', g_dbgSamples);
		fflush(g_dbgSamples);
	}
	rpsbDebugRingPush(buf);
}

extern "C" void rpsb_close_debug_log()
{
	if (g_dbgSamples)
	{
		fclose(g_dbgSamples);
		g_dbgSamples = nullptr;
	}
}
#else
#define sdbgLog(...) ((void)0)
extern "C" void rpsb_close_debug_log() {}
#endif

using std::vector;
using std::queue;

static_assert(sizeof(short) == 2, "Short is weird size");

#if defined(_MSC_VER)
#define ALIGNED_(x) __declspec(align(x))
#elif defined(__GNUC__)
#define ALIGNED_(x) __attribute__ ((aligned(x)))
#else
#error Unknown compiler
#endif

#define ALIGNED_STACK_ARRAY(name, size, alignment) name[size] ALIGNED_(alignment)

#define MAX_SAMPLEBUFFER_SIZE (48000 * 5)
#define AMP_THRESH (SHRT_MAX / 2)
#define AMP_THRESH_EARRAPE (SHRT_MAX / 6)


//---------------------------------------------------------------
// PlaybackSlot constructor
//---------------------------------------------------------------
Sampler::PlaybackSlot::PlaybackSlot() :
	sbCapture(2, MAX_SAMPLEBUFFER_SIZE),
	sbPlayback(2, MAX_SAMPLEBUFFER_SIZE),
	producerThread(),
	inputFile(NULL),
	state(eSILENT),
	soundDbSetting(0.0),
	slotDbLocal(-1.0),
	slotDbRemote(-1.0)
{
}

// Out-of-line so unique_ptr<SlotDsp> sees the full type for its destructor.
Sampler::PlaybackSlot::~PlaybackSlot() = default;

void Sampler::setSlotLoop(int slot, bool on)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> Lock(m_mutex);
	extremeLog("Sampler::setSlotLoop slot=%d on=%d", slot, on ? 1 : 0);
	m_slots[slot].loop = on;
}

void Sampler::setSlotReverse(int slot, bool on)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	extremeLog("Sampler::setSlotReverse slot=%d on=%d", slot, on ? 1 : 0);

	// Phase 1 — snapshot the parameters the worker needs under a
	// micro-lock. NO heavy work (no close, no open, no FFmpeg) happens
	// here. The OLD inputFile keeps streaming through the producer
	// thread completely undisturbed, so audio continues without a
	// gap even for multi-minute files.
	SoundInfo lastSound;
	double    resumeSec   = 0.0;
	float     pitchBase   = 1.0f;
	float     speedFactor = 1.0f;
	float     reverbMix   = 0.0f;
	uint64_t  epoch       = 0;
	std::shared_ptr<std::atomic<bool>> newCancel;
	std::shared_ptr<std::atomic<bool>> oldCancel;

	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		PlaybackSlot &s = m_slots[slot];
		if (s.channelReverse == on) return;
		s.channelReverse = on;
		// Mirror onto audioReverse so the reverse cursor branch picks up
		// the new direction at the very next audio cycle, even before
		// the new InputFile swap completes. Without this flip the cursor
		// branch would still treat the slot as forward for the ~tens of
		// ms between the click and the swap, drifting upward at exactly
		// the moment the user expects "engage reverse here".
		s.audioReverse = on;
		state_e st = s.state.load();
		if (!((st == ePLAYING || st == ePAUSED) && s.lastSoundValid))
			return;

		resumeSec   = s.cachedPositionSec.load(std::memory_order_relaxed);
		lastSound   = s.lastSound;
		pitchBase   = (s.lastSlotPitchFactor > 0.01f)
			? s.lastSlotPitchFactor : m_pitchFactor;
		// Per-slot snapshot of speed / reverb. The previous version
		// read m_speedFactor / m_reverbMix (the GLOBAL factors), which
		// reset to 1.0 / 0.0 the moment the user touched any other
		// channel's FX sliders — so toggling reverse off "stole" the
		// channel's speed and dropped it to identity even though the
		// FxPanel slider still showed the user's chosen value. Read
		// from the live InputFile (and the per-slot fxReverbWet field)
		// so the new file inherits the slot's CURRENT effect chain.
		if (s.inputFile) {
			speedFactor = s.inputFile->getSpeedFactor();
			if (speedFactor <= 0.0f) speedFactor = 1.0f;
		} else {
			speedFactor = m_speedFactor;
		}
		reverbMix = (s.fxReverbWet > 0.0f) ? s.fxReverbWet : m_reverbMix;

		// Tell any in-flight worker to give up — it shares the OLD
		// cancel token, will see the flag flip on its next 64-packet
		// poll inside preDecodeAndReverse and bail out within ~10-50
		// ms. Mismatched epoch is the second-line discard for the
		// case where the worker has already passed the cancel poll
		// and is about to swap.
		oldCancel = s.reverseWorkerCancel;
		if (oldCancel) oldCancel->store(true, std::memory_order_relaxed);
		newCancel = std::make_shared<std::atomic<bool>>(false);
		s.reverseWorkerCancel = newCancel;
		epoch = ++s.reverseEpoch;
	}

	// Phase 2 — retire the previous worker (if any) WITHOUT joining it
	// on this (GUI) thread. The old join() blocked the click handler
	// for the cancel-acknowledge latency (~10-100 ms) on every rapid
	// toggle. The retired thread self-terminates shortly after seeing
	// its cancel flag; it is joined opportunistically when the retire
	// list overflows, and unconditionally at shutdown / destruction.
	PlaybackSlot &s = m_slots[slot];
	if (s.reverseWorker.joinable()) {
		std::lock_guard<std::mutex> rl(m_retiredMutex);
		m_retiredWorkers.push_back(std::move(s.reverseWorker));
		if (m_retiredWorkers.size() > 8) {
			// All of these have long since seen their cancel token (we
			// only get here after 8 further toggles) - joins are instant.
			for (auto &t : m_retiredWorkers)
				if (t.joinable()) t.join();
			m_retiredWorkers.clear();
		}
	}

	// Phase 3 — spawn the new worker. All heavy work (CreateInputFile,
	// open + preDecodeAndReverse for the reverse case, or just open()
	// for the forward case) runs OUTSIDE m_mutex on this thread. GUI
	// + audio both return / continue immediately.
	s.reverseWorker = std::thread(
		&Sampler::reverseWorkerProc, this,
		slot, epoch, on, lastSound, resumeSec,
		pitchBase, speedFactor, reverbMix, newCancel);
}


void Sampler::reverseWorkerProc(int slot, uint64_t epoch, bool wantReverse,
                                 SoundInfo sound, double resumeSec,
                                 float pitchBase, float speedFactor,
                                 float reverbMix,
                                 std::shared_ptr<std::atomic<bool>> cancel)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	PlaybackSlot &s = m_slots[slot];

	// Build the new InputFile completely outside any lock. open() is
	// the multi-second operation when reverse is on (full file decode
	// + atempo + reverse-in-place); doing it here lets the audio
	// thread keep mixing the OLD file in the meantime.
	InputFile *newFile = CreateInputFileFFmpeg();
	if (!newFile) return;

	newFile->setCancelToken(cancel.get());
	if (sound.reverse || wantReverse) newFile->setReverse(true);
	if (sound.autoNormalize)           newFile->setAutoNormalize(true);
	if (pitchBase   != 1.0f) newFile->setPitchFactor(pitchBase);
	if (speedFactor != 1.0f) newFile->setSpeedFactor(speedFactor);
	if (reverbMix   >  0.0f) newFile->setReverbMix(reverbMix);

	int rc = -1;
	try {
		rc = newFile->open(sound.filename.toUtf8(),
		                   sound.getStartTime(),
		                   sound.getPlayTime());
	} catch (...) { rc = -1; }

	// Bail if the decode was cancelled mid-way (user rapid-clicked or
	// stopped playback) or the file failed to open.
	if (rc != 0 || (cancel && cancel->load(std::memory_order_relaxed))
	            || m_shuttingDown.load(std::memory_order_relaxed)
	            || s.reverseEpoch.load(std::memory_order_relaxed) != epoch) {
		newFile->close();
		delete newFile;
		return;
	}

	// Seek the new file to the live cursor BEFORE we go under the
	// audio lock — keeps the lock-hold time microscopic.
	newFile->seek(resumeSec);
	// Clear the cancel token: from here on the new file is the
	// canonical inputFile and any future cancel must come through
	// setSlotReverse re-spawning.
	newFile->setCancelToken(nullptr);

	// Wait for the streaming-reverse worker to populate the first
	// chunk before swapping. Without this the audio thread sees an
	// empty queue right after the swap and m_filePosition (the GUI
	// cursor source) stays anchored at the file end / seek target
	// while the producer thread spins on a 100 ms cycle waiting for
	// data. The user-visible effect on long files was a cursor that
	// "jumped seconds backward" the moment reverse engaged — the
	// real cursor + the displayed cursor briefly diverged because
	// the producer/audio pipeline had nothing to anchor against. A
	// short bounded wait (~300 ms cap) is enough to cover the
	// CHUNK_SEC decode budget on every container we ship; if the
	// worker can't produce a chunk in that window (very slow disk,
	// huge file, malformed container) we swap anyway and accept the
	// brief silence — never block the user click indefinitely.
	for (int waitMs = 0; waitMs < 300; waitMs += 5) {
		if (newFile->isReverseFirstChunkReady()) break;
		if (cancel && cancel->load(std::memory_order_relaxed)) break;
		if (m_shuttingDown.load(std::memory_order_relaxed)) break;
		if (s.reverseEpoch.load(std::memory_order_relaxed) != epoch) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}

	// Phase 4 — disconnect the producer thread from the OLD inputFile
	// OUTSIDE m_mutex. setSource(NULL) only contends on the producer
	// thread's own recursive mutex; meanwhile the audio thread is
	// free to keep draining sbPlayback, so playback continues
	// uninterrupted while we wait for the producer's current
	// readSamples cycle to finish (typically ~1-50 ms on long
	// streaming files).
	s.producerThread.setSource(NULL);

	// Phase 5 — micro-lock swap. m_mutex is held only for the pointer
	// shuffle + buffer drain. The audio thread is briefly blocked
	// here, but the inner work is microseconds (free, reset, assign)
	// and the producer is already disconnected so no readSamples can
	// race us on the old / new file pointer.
	InputFile *oldFile = nullptr;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);

		// Final epoch re-check inside the lock — guards against the
		// race where the user clicked reverse again (or stopped) in
		// the window between our last check and the lock.
		if (s.reverseEpoch.load(std::memory_order_relaxed) != epoch ||
		    m_shuttingDown.load(std::memory_order_relaxed)) {
			// Restore the old source so the slot keeps playing in its
			// pre-click direction. Bail out cleanly.
			if (s.inputFile)
				s.producerThread.setSource(s.inputFile);
			newFile->close();
			delete newFile;
			return;
		}

		state_e st2 = s.state.load();
		if (!((st2 == ePLAYING || st2 == ePAUSED) && s.lastSoundValid)) {
			// Slot was stopped while we were decoding. Drop the file
			// (the slot has already been torn down by stopSlotInternal,
			// so do NOT re-attach anything).
			newFile->close();
			delete newFile;
			return;
		}

		oldFile = s.inputFile;
		{
			SampleBuffer::Lock sblc(s.sbCapture.getMutex());
			SampleBuffer::Lock sblp(s.sbPlayback.getMutex());
			s.sbCapture.consume(NULL, s.sbCapture.avail());
			s.sbPlayback.consume(NULL, s.sbPlayback.avail());
		}
		// Reset filter delay lines + ring buffers, but PRESERVE the 8D
		// rotation phase. Otherwise toggling reverse mid-orbit snaps
		// the source back to the front-azimuth start position, which
		// is jarring when the user is mid-listening to a 3D Rotate /
		// 8D source and momentarily flips the direction.
		if (s.dsp) s.dsp->resetPreservingRotation();

		s.inputFile = newFile;
		s.cachedPositionSec.store(resumeSec, std::memory_order_relaxed);
		s.posCacheValid = false;
		s.producerThread.setSource(s.inputFile);
	}

	// Close + delete the old file OUTSIDE the audio lock. Closing an
	// FFmpeg decoder context is non-trivial (codec close + format
	// close + filter graph teardown) — keeping it out of m_mutex
	// frees the audio thread the moment the pointer swap is done.
	// Drain producer first: setSource(nullptr) at line ~305 is now
	// lock-free, so the producer may still be inside one last
	// readSamples on oldFile. UAF avoided.
	if (oldFile) {
		s.producerThread.waitForReadDrain();
		oldFile->close();
		delete oldFile;
	}
}

void Sampler::setSlotSandboxState(int slot, const SandboxState &s)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	PlaybackSlot &sl = m_slots[slot];

	// Phase 1 - ensure the SlotDsp object exists. The dsp pointer is
	// only mutated from the GUI thread (here and clearSlotSandbox), so
	// once we have created it under m_mutex we can call into it from
	// outside the lock; the audio thread will see a fully-constructed
	// object because make_unique returns before we publish the pointer.
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		if (!sl.dsp) sl.dsp = std::make_unique<SlotDsp>();
		sl.dsp->setSampleRate(48000.0);
	}

	// Phase 2 - heavy Leia bring-up runs OUTSIDE m_mutex. The first
	// time the user picks the Leia 3D engine the SOFA file load + FFT
	// plan + noise-probe calibration can burn ~1 second. With the lock
	// held that 1 second freezes BOTH the audio callback (m_mutex
	// blocks fetchSamples) and the GUI thread (the signal that
	// triggered us blocks behind the audio callback). Moving init out
	// of the lock makes engine swap responsive: the audio thread keeps
	// running through the Classic fallback (leia.ready() stays false)
	// until the init finishes, then quietly flips to Leia next block.
	bool wantsLeia3D = (s.spatialEngine == SandboxState::Engine_Leia) &&
	                   (s.spatialMode == SandboxState::Spatial_3DManual ||
	                    s.spatialMode == SandboxState::Spatial_3DRotate ||
	                    s.spatialMode == SandboxState::Spatial_8DPreset);
	if (wantsLeia3D)
		sl.dsp->prepareLeia(48000.0);

	// Phase 3 - apply the rest of the state under the audio lock. With
	// Leia already initialised, applyState's own ensureInit call is a
	// cached early-return, so total lock-hold time is microseconds even
	// on engine swap.
	std::lock_guard<std::mutex> Lock(m_mutex);
	bool wasStretchOn = sl.dsp ? sl.dsp->state().stretchEnabled : false;
	bool wasDspMissing = false;  // dsp was ensured under Phase 1's lock
	sl.dsp->applyState(s);
	// Mirror the ducking flags onto the slot so the audio thread can
	// read them lock-free in fetchInputSamples. Master sandbox switch
	// off => duck is OFF too, regardless of the per-feature checkbox.
	sl.duckSource   = s.enabled && s.duckSource;
	sl.duckOthersDb = s.duckOthersDb;
	// Re-route any prior FxPanel reverb into the dsp's end-stage and
	// disable the libavfilter pre-reverb. Without this, enabling the
	// sandbox after a reverb was already set would leave the legacy
	// decoder reverb running while the user expected it at the end.
	if (wasDspMissing) {
		sl.dsp->setFxReverbWet(sl.fxReverbWet);
		if (sl.inputFile) sl.inputFile->setReverbMix(0.0f);
	} else {
		sl.dsp->setFxReverbWet(sl.fxReverbWet);
	}

	// On stretch toggle ON, drain whatever the decoder has already
	// queued in sbPlayback into paulstretch's feed buffer so it has
	// at least one full window of source ready immediately. Without
	// this, ~1-2 seconds of silence leaked through (the streaming
	// buffer warmup) before stretched output came out. Drain
	// sbCapture too to avoid backlog while the capture path
	// consumes slowly.
	if (s.stretchEnabled && !wasStretchOn) {
		// Toggle ON => reset stretch state first so the new session
		// starts cleanly. Without this, the second time the user
		// enables paulstretch the inputPos resumes mid-buffer (left
		// over from the previous session) and the output runs
		// seconds behind the live audio.
		sl.dsp->reset();
		// Pre-fill BOTH stretch state buffers so paulstretch on
		// capture and playback both have at least one window of
		// source ready immediately - skips the 1-2 s warm-up silence
		// that otherwise made stretch toggle look broken.
		{
			SampleBuffer::Lock sbl(sl.sbPlayback.getMutex());
			int avail = sl.sbPlayback.avail();
			if (avail > 0) {
				int prefill = std::min(avail, 96000); // up to 2 sec @48k
				sl.dsp->feedStretchShort(sl.sbPlayback.getBufferData(), prefill, /*isCapture=*/false);
				sl.sbPlayback.consume(NULL, prefill);
			}
		}
		{
			SampleBuffer::Lock sblc(sl.sbCapture.getMutex());
			int availC = sl.sbCapture.avail();
			if (availC > 0) {
				int prefillC = std::min(availC, 96000);
				sl.dsp->feedStretchShort(sl.sbCapture.getBufferData(), prefillC, /*isCapture=*/true);
				sl.sbCapture.consume(NULL, prefillC);
			}
		}
	}
}

void Sampler::clearSlotSandbox(int slot)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_slots[slot].dsp.reset();
	m_slots[slot].peakL.store(0.0f);
	m_slots[slot].peakR.store(0.0f);
}

void Sampler::getSlotPeak(int slot, float &peakL, float &peakR) const
{
	if (slot < 0 || slot >= MAX_SLOTS) {
		peakL = peakR = 0.0f;
		return;
	}
	peakL = m_slots[slot].peakL.load();
	peakR = m_slots[slot].peakR.load();
}

double Sampler::getSlotCpuPercent(int slot)
{
	if (slot < 0 || slot >= MAX_SLOTS) return 0.0;
	SlotDsp *d = m_slots[slot].dsp.get();
	return d ? d->cpuPercent() : 0.0;
}

void Sampler::getSlotEqBandLevels(int slot, float out[16]) const
{
	for (int i = 0; i < 16; ++i) out[i] = 0.0f;
	if (slot < 0 || slot >= MAX_SLOTS) return;
	// Levels only meaningful while audio is actively being rendered.
	// Pause / stop -> push zeros so the widgets smoothly decay to off
	// instead of freezing at the last frame.
	state_e st = m_slots[slot].state.load(std::memory_order_relaxed);
	if (st != ePLAYING && st != ePLAYING_PREVIEW) return;
	SlotDsp *d = m_slots[slot].dsp.get();
	if (d) d->getEqBandLevels(out);
}

bool Sampler::anyPlaying() const
{
	for (int s = 0; s < MAX_SLOTS; ++s) {
		state_e st = m_slots[s].state.load(std::memory_order_relaxed);
		if (st == ePLAYING || st == ePAUSED || st == ePLAYING_PREVIEW)
			return true;
	}
	return false;
}

//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
Sampler::Sampler() :
	m_peakMeterCapture(0.01f, 0.00005f, 24000),
	m_peakMeterPlayback(0.01f, 0.00005f, 24000),
	m_volumeDivider(1),
	m_volumeFactor(1.0f),
	m_globalDbSettingLocal(-1.0),
	m_globalDbSettingRemote(-1.0),
	m_localPlayback(true),
	m_muteMyself(false),
	m_earrapeProtection(false),
	m_pitchFactor(1.0f),
	m_speedFactor(1.0f),
	m_intensityFactor(1.0f),
	m_reverbMix(0.0f),
	m_multiMode(false)
{
	/* Ensure resources are loaded */
	Q_INIT_RESOURCE(qtres);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
Sampler::~Sampler()
{
	// Belt-and-braces: if shutdown() was not invoked the std::thread
	// member of each PlaybackSlot would still be joinable, and its
	// destructor would call std::terminate(). Signal cancel + join
	// here so the plugin shuts down cleanly even on the error path.
	m_shuttingDown.store(true, std::memory_order_release);
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (m_slots[i].reverseWorkerCancel)
			m_slots[i].reverseWorkerCancel->store(true, std::memory_order_relaxed);
	}
	for (int i = 0; i < MAX_SLOTS; i++) {
		if (m_slots[i].reverseWorker.joinable())
			m_slots[i].reverseWorker.join();
	}
	{
		std::lock_guard<std::mutex> rl(m_retiredMutex);
		for (auto &t : m_retiredWorkers)
			if (t.joinable()) t.join();
		m_retiredWorkers.clear();
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::init()
{
	sdbgLog("Sampler::init() called");
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		PlaybackSlot &slot = m_slots[i];
		slot.producerThread.addBuffer(&slot.sbCapture);
		slot.producerThread.addBuffer(&slot.sbPlayback, m_localPlayback);
		// Producer threads start LAZILY on the slot's first playback
		// (playSoundInSlot). Starting all MAX_SLOTS threads here kept
		// 32 idle threads waking 10x/s for users with 1-2 channels.
	}
	sdbgLog("Sampler::init() done, producer threads start lazily");
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
// Bounded thread join. The cooperative cancel paths above are
// best-effort: if a worker is stuck deep inside FFmpeg (open / seek
// scan / decode of a malformed packet), no flag can interrupt that
// call before it returns. Letting std::thread::join wait
// indefinitely is what produced the zombie TS3.exe after window
// close — the DLL unload sat behind the worker, the user force-
// killed TS3, and Windows kept the process around as half-dead
// (blocking the next TS3 install / update because the executable
// stayed locked). Bound the wait; on timeout, TerminateThread the
// worker and detach the std::thread so DLL unload can proceed.
// TerminateThread is hostile (leaks the worker's stack + resources)
// but the alternative is keeping the process alive forever, which
// is worse — and we're seconds away from FreeLibrary anyway.
static void joinThreadBounded(std::thread &t, int timeoutMs)
{
	if (!t.joinable()) return;
#ifdef _WIN32
	HANDLE h = (HANDLE)t.native_handle();
	DWORD rc = WaitForSingleObject(h, (DWORD)timeoutMs);
	if (rc == WAIT_OBJECT_0) {
		t.join();
	} else {
		TerminateThread(h, 0);
		t.detach();
	}
#else
	// POSIX: no portable timed-join in std::thread. Best effort is to
	// just join — the cooperative cancel flags above should make this
	// quick on every supported platform anyway.
	t.join();
#endif
}

void Sampler::shutdown()
{
	// Flip the global shutdown flag FIRST (no lock). Any reverse
	// worker that is past its cancel check but not yet through the
	// swap section will see this and discard its work instead of
	// touching slot state we're about to tear down.
	m_shuttingDown.store(true, std::memory_order_release);

	// Stop the dedicated seek worker. cv.notify_all + atomic stop flag
	// breaks out of its wait predicate; the worker exits its outer
	// loop without committing any in-flight scan result (the recheck
	// inside the per-Work loop honours m_shuttingDown). Join before
	// closing input files so the worker can never race the InputFile
	// teardown below.
	m_seekStop.store(true, std::memory_order_release);
	m_seekCv.notify_all();
	joinThreadBounded(m_seekWorker, 500);

	// Signal every in-flight worker to cancel its decode loop, then
	// join them OUTSIDE m_mutex. Joining inside the lock would
	// deadlock because the worker grabs m_mutex to perform the swap.
	for (int i = 0; i < MAX_SLOTS; i++) {
		PlaybackSlot &slot = m_slots[i];
		if (slot.reverseWorkerCancel)
			slot.reverseWorkerCancel->store(true, std::memory_order_relaxed);
	}
	// Bounded waits for the reverse-toggle workers. These can sit deep
	// inside FFmpeg open()/seek() (no cancel hook there) — without a
	// timeout, sb_kill blocks the DLL unload until each open returns,
	// which on a long audio file is multiple seconds. Multiply by the
	// retired-workers list and TS3 sits zombied long enough that the
	// user closes the client, force-kills the orphan process, and
	// Windows then blocks the next plugin install / update because
	// the executable is still locked.
	for (int i = 0; i < MAX_SLOTS; i++) {
		PlaybackSlot &slot = m_slots[i];
		joinThreadBounded(slot.reverseWorker, 300);
	}
	{
		std::lock_guard<std::mutex> rl(m_retiredMutex);
		for (auto &t : m_retiredWorkers)
			joinThreadBounded(t, 200);
		m_retiredWorkers.clear();
	}

	// Stop every producer FIRST, then close + delete InputFiles. The
	// old order (close + delete, then stop) created a use-after-free
	// window: producer threads still pointed at the just-deleted
	// InputFile and were free to dereference it before they noticed
	// m_stop. Symptom in the wild was a leftover TS3.exe in task
	// manager — the producer hung on a dead pointer, its std::thread
	// dtor called std::terminate, and Windows kept the process around
	// in a half-dead state long enough that the user force-killed it
	// and TS3 popped a crash-on-exit dialog.
	//
	// PARALLEL stop: previously the loop did stop(true) per slot which
	// serialised the join — slot 0 had to finish decoding its current
	// batch (could be hundreds of ms on a very long file) BEFORE slot
	// 1 even got the stop signal. With 32 slots and a long audio in
	// each, sb_kill could take multiple seconds; TS3's plugin
	// watchdog then declared us hung and showed the crash dialog
	// "soundboard crashed" even though we were just shutting down
	// cleanly. Now: signal ALL producers first (atomic flag + cv
	// notify), THEN join them all. Total stop time = max single
	// in-flight readSamples, not sum.
	for (int i = 0; i < MAX_SLOTS; i++) {
		PlaybackSlot &slot = m_slots[i];
		slot.producerThread.setSource(NULL);  // wakes cv
		slot.producerThread.stop(false);      // sets m_stop, NO join
	}
	// Bounded producer joins. singleBufferFill checks m_stop between
	// readSamples calls so the producer usually exits within one
	// FFmpeg packet decode (~5-50 ms). Pathological cases (codec
	// frozen on a malformed packet) are bounded at 250 ms per slot —
	// the parallel signal above means total wait is max(slot), not
	// sum, so 250 ms ceiling holds regardless of slot count.
	for (int i = 0; i < MAX_SLOTS; i++) {
		m_slots[i].producerThread.joinIfRunningBounded(250);
	}

	std::lock_guard<std::mutex> Lock(m_mutex);

	for (int i = 0; i < MAX_SLOTS; i++)
	{
		PlaybackSlot &slot = m_slots[i];
		if (slot.inputFile)
		{
			slot.inputFile->close();
			delete slot.inputFile;
			slot.inputFile = NULL;
		}
	}
}

//---------------------------------------------------------------
// Purpose: Fetch and mix samples from a single buffer
//---------------------------------------------------------------
int Sampler::fetchSamples(SampleBuffer &sb, PeakMeter &pm, short *samples, int count, int channels, bool eraseConsumed, int ciLeft, int ciRight, bool overLeft, bool overRight, float ampThresh, PlaybackSlot *slot, bool forceLimit)
{
	float thresh = (ampThresh > 0.0f) ? ampThresh : (float)AMP_THRESH;

	SampleBuffer::Lock sbl(sb.getMutex());

	const bool isStretch = slot && slot->dsp && slot->dsp->isStretchEnabled();

	if(sb.avail() == 0 && !isStretch)
		return 0;

	if(overLeft)
	{
		if(channels == 1)
			memset(samples, 0, count * sizeof(short));
		else
			for(int i = 0; i < count; i++)
                samples[i*channels+ciLeft] = 0;
	}

	if(overRight && channels > 1)
		for(int i = 0; i < count; i++)
			samples[i*channels+ciRight] = 0;

	const float volGain = m_volumeFactor;
	const float intensity = m_intensityFactor;
	const int avail = sb.avail();
	const short* in = sb.getBufferData();
	short* const out = samples;
	int write = 0;
	int consumed = 0;

	write = isStretch ? count : std::min(count, avail);

	static thread_local std::vector<short> dspTemp;
	int  stretchConsumed = -1;     // -1 = not on stretch path
	bool isCapturePath = slot && (&sb == &slot->sbCapture);
	// Push the output-domain gain to the DSP block so its EQ analyser
	// scales POST-chain samples by what the listener actually hears
	// (post-chain spectrum * volume * intensity). Without this the LEDs
	// would not respond to the slot's volume slider on the bypass path.
	if (slot && slot->dsp)
		slot->dsp->setOutputGain(volGain * intensity);
	if (isStretch)
	{
		int needIn = slot->dsp->inputFramesNeededFor(write);
		if (needIn > avail) needIn = avail;
		if (needIn > 0) slot->dsp->feedStretchShort(in, needIn, isCapturePath);
		if ((int)dspTemp.size() < write * 2) dspTemp.resize(write * 2);
		float pL = 0.0f, pR = 0.0f;
		slot->dsp->produceStretchedShort(dspTemp.data(), write, 2, pL, pR, isCapturePath);
		(void)pL; (void)pR;
		in = dspTemp.data();
		stretchConsumed = needIn;
	}
	else if (slot && slot->dsp)
	{
		if ((int)dspTemp.size() < write * 2) dspTemp.resize(write * 2);
		std::memcpy(dspTemp.data(), in, sizeof(short) * write * 2);
		float pL = 0.0f, pR = 0.0f;
		slot->dsp->process(dspTemp.data(), write, 2, pL, pR, isCapturePath);
		(void)pL; (void)pR;
		in = dspTemp.data();
	}
	float slotMaxL = 0.0f, slotMaxR = 0.0f;
	if (channels == 1)
	{
		for (int i = 0; i < write; i++)
		{
			float sbSample = volGain * intensity * (float(in[i * 2]) + float(in[i * 2 + 1])) * 0.5f;
			float a = std::fabs(sbSample) * (1.0f / 32768.0f);
			if (a > slotMaxL) slotMaxL = a;
			if (a > slotMaxR) slotMaxR = a;
			float mixed = (float)out[i] + sbSample;
			// Intensity > 1 historically skipped the limiter (the boost
			// is MEANT to clip), but forceLimit (earrape protection
			// checkbox) overrides that: protection must hold exactly
			// when the user cranks intensity.
			if (intensity <= 1.01f || forceLimit)
			{
				pm.process(mixed);
				mixed = (float)pm.limit(mixed, thresh);
			}
			else
			{
				pm.process(mixed);
			}
			if (mixed > 32767.0f) mixed = 32767.0f;
			else if (mixed < -32768.0f) mixed = -32768.0f;
			out[i] = (short)mixed;
		}
	}
	else
	{
		for (int i = 0; i < write; i++)
		{
			float sbL = volGain * intensity * float(in[i * 2]);
			float sbR = volGain * intensity * float(in[i * 2 + 1]);
			float aL = std::fabs(sbL) * (1.0f / 32768.0f);
			float aR = std::fabs(sbR) * (1.0f / 32768.0f);
			if (aL > slotMaxL) slotMaxL = aL;
			if (aR > slotMaxR) slotMaxR = aR;
			float tsL = (float)out[i * channels + ciLeft];
			float tsR = (float)out[i * channels + ciRight];
			float mixL = tsL + sbL;
			float mixR = tsR + sbR;
			float resL, resR;
			// See mono branch: forceLimit = earrape protection stays
			// active even when intensity > 1 disables normal limiting.
			if (intensity <= 1.01f || forceLimit)
			{
				pm.process(fabs(mixL) > fabs(mixR) ? mixL : mixR);
				resL = (float)pm.limit(mixL, thresh);
				resR = (float)pm.limit(mixR, thresh);
			}
			else
			{
				pm.process(fabs(mixL) > fabs(mixR) ? mixL : mixR);
				resL = mixL;
				resR = mixR;
			}
			if (resL > 32767.0f) resL = 32767.0f; else if (resL < -32768.0f) resL = -32768.0f;
			if (resR > 32767.0f) resR = 32767.0f; else if (resR < -32768.0f) resR = -32768.0f;
			out[i * channels + ciLeft] = (short)resL;
			out[i * channels + ciRight] = (short)resR;
		}
	}

	// Capture-path peak: reflects remote volume, not local slider.
	if (isCapturePath && slot) {
		float prevL = slot->peakL.load();
		float prevR = slot->peakR.load();
		slot->peakL.store(std::max(slotMaxL, prevL * 0.95f));
		slot->peakR.store(std::max(slotMaxR, prevR * 0.95f));
	}
	consumed = write;
	if (stretchConsumed >= 0) consumed = stretchConsumed;

	sb.consume(NULL, consumed, true);
	return write;
}


int Sampler::findChannelId(unsigned int channel, const unsigned int *channelSpeakerArray, int count)
{
	for(int i = 0; i < count; i++)
		if(channelSpeakerArray[i] & channel)
			return i;
	return 0;
}


//---------------------------------------------------------------
// Purpose: Fetch input (capture) samples - mix all active slots
//---------------------------------------------------------------
int Sampler::fetchInputSamples(short *samples, int count, int channels, bool *finished)
{
	// Lock-free fast path: at idle (no slot in PLAYING / PAUSED) the
	// audio thread used to acquire + release m_mutex every callback
	// (50 Hz from TS3), and any GUI thread waiting on m_mutex got
	// shoved to the back of the lock queue on each cycle. Atomic state
	// loads let the audio callback bail in nanoseconds when nothing is
	// playing, so GUI slider / scroll events see m_mutex completely
	// uncontended.
	{
		bool any = false;
		for (int s = 0; s < MAX_SLOTS; s++) {
			state_e st = m_slots[s].state.load(std::memory_order_relaxed);
			if (st == ePLAYING || st == ePAUSED) { any = true; break; }
		}
		if (!any) {
			if (finished) *finished = true;
			return 0;
		}
	}

	std::lock_guard<std::mutex> Lock(m_mutex);

	int totalWritten = 0;

	// Sidechain ducking: find the deepest (most negative dB) active
	// duck source. Every non-source playing slot will smoothly attack
	// toward that attenuation; everyone else releases back to unity.
	// Source slots themselves never duck their own output (otherwise
	// the duck depth depends on whether a single channel is its own
	// source).
	double duckTargetDb       = 0.0;
	int    duckSourceSlotIdx  = -1;
	for (int s = 0; s < MAX_SLOTS; s++) {
		const PlaybackSlot &slot = m_slots[s];
		state_e st2 = slot.state.load();
		if (st2 != ePLAYING) continue;
		if (!slot.duckSource) continue;
		if (slot.duckOthersDb < duckTargetDb) {
			duckTargetDb      = slot.duckOthersDb;
			duckSourceSlotIdx = s;
		} else if (duckSourceSlotIdx < 0) {
			duckSourceSlotIdx = s;
		}
	}

	for (int s = 0; s < MAX_SLOTS; s++)
	{
		PlaybackSlot &slot = m_slots[s];
		state_e st = slot.state.load();
		if (st != ePLAYING && st != ePAUSED)
			continue;

		if (st == ePAUSED)
			continue;

		// Smoothly attack / release the ducking gain. duckGain is the
		// LINEAR multiplier currently applied; convert to dB and add
		// to setVolumeDb for this block. Source slots and lone-channel
		// playback see target = 1.0 (no attenuation).
		bool isDuckee = (duckSourceSlotIdx >= 0) && (s != duckSourceSlotIdx)
		             && !slot.duckSource;
		float targetGain = isDuckee
			? static_cast<float>(AudioUtils::dbToLinear(duckTargetDb))
			: 1.0f;
		float curGain = slot.duckGain.load(std::memory_order_relaxed);
		// Attack faster than release so a SFX riding music gets the
		// duck instantly but the music returns gracefully when the
		// SFX ends. Per-block coefficients (~20ms blocks).
		float alpha = (targetGain < curGain) ? 0.55f : 0.10f;
		curGain += alpha * (targetGain - curGain);
		if (curGain < 0.0001f) curGain = 0.0001f;
		if (curGain > 1.0f)    curGain = 1.0f;
		slot.duckGain.store(curGain, std::memory_order_relaxed);
		double duckActiveDb = (curGain >= 0.9999f) ? 0.0
		                    : AudioUtils::linearToDb((double)curGain);

		// Set volume for this slot: per-slot in multi-mode, global otherwise
		double remoteDb = m_multiMode ? slot.slotDbRemote : m_globalDbSettingRemote;
		setVolumeDb(remoteDb + slot.soundDbSetting + duckActiveDb);

		bool muteCapture = m_muteMyself.load(std::memory_order_relaxed);
		bool isFirstSlot = (totalWritten == 0);
		int written = fetchSamples(slot.sbCapture, m_peakMeterCapture, samples, count, channels, true,
			0, 1, muteCapture && isFirstSlot, muteCapture && isFirstSlot, 0.0f, &slot);
		if (written > totalWritten)
			totalWritten = written;

		// Refresh the GUI's lock-free position cache. Computing it here
		// (audio thread) instead of on demand under m_mutex from the GUI
		// timer eliminates a 30 Hz x N-channel mutex contention that
		// stalled scroll repaints and slider drags - especially under
		// the Leia engine, which holds m_mutex inside its block-FFT.
		if (slot.inputFile) {
			double posSec = 0.0;
			double lenSec = slot.inputFile->getLength();
			if (slot.dsp && slot.dsp->isStretchEnabled()) {
				posSec = slot.stretchBaseTime + slot.dsp->stretchPlaybackPosition();
				if (lenSec > 0.0 && posSec > lenSec) posSec = std::fmod(posSec, lenSec);
				if (posSec < 0.0) posSec = 0.0;
			} else if (slot.audioReverse) {
				// Reverse cursor: track via samples actually consumed
				// by this audio cycle (written), descending at exactly
				// the playback rate. The chunked-streaming decoder's
				// decoderPos + bufferedSec formula is brittle (every
				// chunk transition + every codec seek-overshoot adds
				// drift on the position read, and the monotonic rate
				// limiter locks each tiny error in as a permanent
				// downward step), so a few seconds into reverse the
				// displayed cursor races SECONDS below the actual
				// audible playback head. The audio itself plays
				// correctly — only the cursor mapping is broken.
				//
				// Tracking by "written" makes the cursor descend at
				// playback rate by definition: each output frame at
				// 48 kHz corresponds to 1 / 48 000 output seconds,
				// scaled by speedFactor to input-time. Cursor never
				// drifts because the source of truth is "how much
				// audio was just sent out", not a decoder-internal
				// timestamp that can be off-by-batch.
				//
				// IMPORTANT: in streaming-reverse mode the LIVE
				// m_speedFactor lags the audio actually being heard
				// by one chunk worth — the chunk currently feeding
				// the reader was decoded at the speed value set
				// BEFORE the most recent slider tick. Reading the
				// LIVE speed (via getSpeedFactor) made the cursor
				// race ahead of audio during heavy drag (post-drag
				// the drift was capped permanently by the monotonic
				// rate limit below). getCurrentReverseChunkSpeed
				// returns the speedAtBuild of the chunk currently
				// being drained so descent rate matches what the
				// user is hearing.
				double sf = (double)slot.inputFile->getCurrentReverseChunkSpeed();
				if (sf <= 0.0) sf = 1.0;
				if (slot.posCacheValid) {
					double prev = slot.cachedPositionSec.load(
					                std::memory_order_relaxed);
					double consumedSec =
					    (double)written / 48000.0 * sf;
					posSec = prev - consumedSec;
				} else {
					// First cycle after seek / swap / play start —
					// use the seeded cachedPositionSec (set by
					// playSoundInSlot to cropStart, by seek to the
					// target, by the reverse swap to resumeSec, by
					// loop wrap to loopStartSec). NOT the decoder's
					// getPosition(): in streaming reverse the decoder
					// scans FORWARD ahead of the playback head to
					// emit reversed chunks, so getPosition() returns
					// a value seconds away from where the audible
					// cursor actually is. Reading it on the first
					// cycle after a mid-playback reverse toggle was
					// the "cursor jumps several seconds back" bug.
					posSec = slot.cachedPositionSec.load(
					           std::memory_order_relaxed);
				}
				double minBound = (slot.cropStart > 0.0) ? slot.cropStart : 0.0;
				if (posSec < minBound) posSec = minBound;
				if (posSec > lenSec)   posSec = lenSec;
			} else {
				double decoderPos = slot.inputFile->getPosition();
				double sf = (double)slot.inputFile->getSpeedFactor();
				if (sf <= 0.0) sf = 1.0;
				int availSamples = 0;
				{
					SampleBuffer::Lock sblp(slot.sbPlayback.getMutex());
					availSamples = slot.sbPlayback.avail();
				}
				double bufferedSec = availSamples / 48000.0 * sf;
				posSec = decoderPos - bufferedSec;
				if (posSec < 0.0) posSec = 0.0;
			}
			// Rate-limit + monotonic guard. Direction depends on the
			// slot's reverse flag - forward play is monotonic up,
			// reverse play is monotonic down. The per-cycle advance is
			// clamped so a slider seek can never jump several seconds.
			{
				double prev = slot.cachedPositionSec.load(std::memory_order_relaxed);
				double accepted = posSec;
				if (slot.posCacheValid) {
					constexpr double kMaxPerCycle = 0.20;
					if (!slot.audioReverse) {
						if (accepted < prev) accepted = prev;
						else if (accepted > prev + kMaxPerCycle)
							accepted = prev + kMaxPerCycle;
					} else {
						if (accepted > prev) accepted = prev;
						else if (accepted < prev - kMaxPerCycle)
							accepted = prev - kMaxPerCycle;
					}
				} else {
					slot.posCacheValid = true;
				}
				slot.cachedPositionSec.store(accepted, std::memory_order_relaxed);
			}
			slot.cachedLengthSec.store(lenSec, std::memory_order_relaxed);
		}

		// Check if this slot's file is done
		if (st == ePLAYING && slot.inputFile && slot.inputFile->done())
		{
			bool isStretch = slot.dsp && slot.dsp->isStretchEnabled();
			bool canEnd = false;
			int  captureAvail = 0;
			{
				SampleBuffer::Lock sbl(slot.sbCapture.getMutex());
				captureAvail = slot.sbCapture.avail();
				canEnd = captureAvail == 0;
			}
			if (isStretch && canEnd)
				canEnd = slot.dsp->stretchCaptureDone();

			if (canEnd)
			{
				sdbgLog("[loopBranch] slot=%d done=1 canEnd=1 loop=%d "
				        "reverse=%d capAvail=%d",
				        s, slot.loop ? 1 : 0,
				        slot.audioReverse ? 1 : 0, captureAvail);
				if (slot.loop) {
					// Anti-glitch loop-rate guard. Streaming-reverse +
					// rapid pitch / speed dragging on a short audio can
					// leave the codec / atempo chain in a state where
					// every fresh decode produces a chunk that covers
					// FAR less than the requested input range. Each
					// tiny chunk drains in a few ms, natural-end fires,
					// loop restarts, the next decode produces another
					// tiny chunk — the user hears the last fragment of
					// reverse playback (= start of file in forward time)
					// looping at 30-200 Hz, "audio si glitcha + ripete
					// gli ultimi tot millisecondi all'infinito" bug.
					// Detect by measuring loop-restart cadence: more
					// than kMaxLoopsPerBurst loop fires inside a
					// kBurstWindowMs window = sub-perceptual loop = kill
					// the slot cleanly so the user gets silence (and a
					// reload glyph in replay mode) instead of glitch
					// noise. Normal loop usage of a 200 ms file at
					// neutral speed = 5 loops / s ≪ threshold; even an
					// extreme but legit case (100 ms file at speed=3)
					// = 30 loops / s, also below threshold.
					constexpr int kBurstWindowMs   = 1000;
					constexpr int kMaxLoopsPerBurst = 35;
					auto now = std::chrono::steady_clock::now();
					int64_t nowMs = std::chrono::duration_cast<
						std::chrono::milliseconds>(
							now.time_since_epoch()).count();
					if (slot.loopBurstStartMs == 0
					    || nowMs - slot.loopBurstStartMs > kBurstWindowMs) {
						slot.loopBurstStartMs = nowMs;
						slot.loopBurstCount   = 0;
					}
					++slot.loopBurstCount;
					if (slot.loopBurstCount > kMaxLoopsPerBurst) {
						extremeLog("Sampler::loopRateLimit slot=%d "
						           "burst=%d window=%lld ms — killing slot "
						           "(glitch loop detected)",
						           s, slot.loopBurstCount,
						           (long long)(nowMs - slot.loopBurstStartMs));
						slot.state = eSILENT;
						slot.peakL.store(0.0f);
						slot.peakR.store(0.0f);
						emit onStopPlaying(s);
						continue;
					}
					slot.lastLoopMonoMs = nowMs;
					extremeLog("Sampler::loopRestart slot=%d reverse=%d cropStart=%.3f cropEnd=%.3f burst=%d/%d",
					           s, slot.audioReverse ? 1 : 0,
					           slot.cropStart, slot.cropEnd,
					           slot.loopBurstCount, kMaxLoopsPerBurst);
					// Per-playback random pitch jitter. Reads the LIVE
					// SandboxState + gates on the sandbox master switch
					// so toggling master off stops the jitter on the
					// next loop boundary. Applies the jitter multiplier
					// ON TOP of the slot's intended pitch (last value
					// pushed via setSlotPitchFactor by the FxPanel
					// wiring), so the channel's pitch slider is never
					// clobbered by global pitch on loop restart.
					//
					// Reverse mode: loop restart point is the FORWARD
					// file end (cropEnd if set, otherwise full file
					// length). seek(end) drops the reverse buffer back
					// to its index-0 position so playback restarts at
					// the right edge of the waveform and walks left.
					// IMPORTANT: gate on audioReverse, NOT channelReverse.
					// A sound played reversed via the cell-level
					// SoundInfo::reverse flag (cell context menu) has
					// audioReverse=true but channelReverse=false; the old
					// channelReverse check sent the loop restart to
					// cropStart=0, which is ALREADY at minF for the
					// reverse worker, so done() flipped back to true
					// immediately and the slot ping-ponged between
					// loop restart and natural-end forever — the
					// "reverse + pitch/speed mods loop forever" bug.
					double loopStartSec = slot.cropStart;
					if (slot.audioReverse) {
						loopStartSec = (slot.cropEnd > 0.0)
							? slot.cropEnd
							: slot.inputFile->getLength();
					}
					float  slotBasePitch = slot.lastSlotPitchFactor;
					if (slotBasePitch < 0.01f) slotBasePitch = m_pitchFactor;
					if (slot.dsp && slot.inputFile) {
						const SandboxState &st = slot.dsp->state();
						if (st.enabled
						 && st.randomEnabled
						 && st.randomPitchCents > 0)
						{
							auto urand = []{ return (double)rand()
							                       / (double)RAND_MAX; };
							double jc = (urand() * 2.0 - 1.0) * st.randomPitchCents;
							double mul = std::pow(2.0, jc / 1200.0);
							slot.inputFile->setPitchFactor(
								slotBasePitch * static_cast<float>(mul));
						} else if (slot.randomActive) {
							// Random was active at fire but is now off -
							// restore the slot's intended pitch (FxPanel
							// channel slider value).
							slot.inputFile->setPitchFactor(slotBasePitch);
						}
					}
					slot.inputFile->seek(loopStartSec);
					{
						SampleBuffer::Lock sblp(slot.sbPlayback.getMutex());
						slot.sbPlayback.clear();
					}
					{
						SampleBuffer::Lock sblc(slot.sbCapture.getMutex());
						slot.sbCapture.clear();
					}
					if (slot.dsp) slot.dsp->reset();
					slot.stretchBaseTime = 0.0;
					// Loop restart: drop the rate-limiter anchor + snap
					// the cache to cropStart so the cursor jumps back to
					// the loop point instead of being held forward by
					// the monotonic guard.
					slot.cachedPositionSec.store(loopStartSec, std::memory_order_relaxed);
					slot.posCacheValid = false;
				} else {
					sdbgLog("[loopBranch] slot=%d -> eSILENT "
					        "(natural end, loop off)", s);
					slot.state = eSILENT;
					slot.peakL.store(0.0f);
					slot.peakR.store(0.0f);
					emit onStopPlaying(s);
				}
			}
		}
	}

	if (finished && totalWritten == 0)
	{
		// Check if any slot is still playing
		bool anyPlaying = false;
		for (int s = 0; s < MAX_SLOTS; s++)
		{
			state_e st = m_slots[s].state.load();
			if (st == ePLAYING || st == ePAUSED)
			{
				anyPlaying = true;
				break;
			}
		}
		if (!anyPlaying)
			*finished = true;
	}

	return totalWritten;
}


//---------------------------------------------------------------
// Purpose: Fetch output (playback) samples - mix all active slots
//---------------------------------------------------------------
int Sampler::fetchOutputSamples(short *samples, int count, int channels, const unsigned int *channelSpeakerArray, unsigned int *channelFillMask)
{
	// Lock-free fast path (see fetchInputSamples for rationale).
	{
		bool any = false;
		for (int s = 0; s < MAX_SLOTS; s++) {
			state_e st = m_slots[s].state.load(std::memory_order_relaxed);
			if (st == ePLAYING || st == ePAUSED || st == ePLAYING_PREVIEW) {
				any = true; break;
			}
		}
		if (!any) return 0;
	}

	std::lock_guard<std::mutex> Lock(m_mutex);

	const unsigned int bitMaskLeft = SPEAKER_FRONT_LEFT | SPEAKER_HEADPHONES_LEFT;
	const unsigned int bitMaskRight = SPEAKER_FRONT_RIGHT | SPEAKER_HEADPHONES_RIGHT;
	int ciLeft = findChannelId(bitMaskLeft, channelSpeakerArray, channels);
	int ciRight = findChannelId(bitMaskRight, channelSpeakerArray, channels);
	float localThresh = m_earrapeProtection.load(std::memory_order_relaxed) ? (float)AMP_THRESH_EARRAPE : (float)AMP_THRESH;

	int totalWritten = 0;

	for (int s = 0; s < MAX_SLOTS; s++)
	{
		PlaybackSlot &slot = m_slots[s];
		state_e st = slot.state.load();

		// Both ePLAYING and ePLAYING_PREVIEW output locally via sbPlayback
		if (st != ePLAYING_PREVIEW && st != ePLAYING && st != ePAUSED)
			continue;

		if (st == ePAUSED)
			continue;

		// Set volume for this slot: per-slot in multi-mode, global otherwise.
		// Local playback honours the same duck gain the capture path
		// applied this block - reading the already-updated value keeps
		// the two paths phase-locked.
		float curDuck = slot.duckGain.load(std::memory_order_relaxed);
		double duckActiveDb = (curDuck >= 0.9999f) ? 0.0
		                    : AudioUtils::linearToDb((double)std::max(curDuck, 1e-4f));
		double localDb = m_multiMode ? slot.slotDbLocal : m_globalDbSettingLocal;
		setVolumeDb(localDb + slot.soundDbSetting + duckActiveDb);

		bool isFirstSlot = (totalWritten == 0);
		int written = fetchSamples(slot.sbPlayback, m_peakMeterPlayback, samples, count, channels, true,
			ciLeft, ciRight,
			isFirstSlot && ((*channelFillMask & bitMaskLeft) == 0),
			isFirstSlot && ((*channelFillMask & bitMaskRight) == 0),
			localThresh, &slot,
			/*forceLimit=*/m_earrapeProtection.load(std::memory_order_relaxed));

		if (written > totalWritten)
			totalWritten = written;

		// Preview slots never see fetchInputSamples (it gates on ePLAYING
		// / ePAUSED only), so the GUI position cache would never refresh
		// for previews -> cursor frozen at 0. Refresh it here for the
		// preview case so the ButtonAdvancedPanel preview cursor walks
		// the waveform in lock-step with audio. Same rate-limit /
		// monotonic guard as fetchInputSamples.
		if (st == ePLAYING_PREVIEW && slot.inputFile) {
			double lenSec = slot.inputFile->getLength();
			double decoderPos = slot.inputFile->getPosition();
			double sf = (double)slot.inputFile->getSpeedFactor();
			if (sf <= 0.0) sf = 1.0;
			int availSamples = 0;
			{
				SampleBuffer::Lock sblp(slot.sbPlayback.getMutex());
				availSamples = slot.sbPlayback.avail();
			}
			double bufferedSec = availSamples / 48000.0 * sf;
			double posSec = decoderPos - bufferedSec;
			if (posSec < 0.0) posSec = 0.0;
			double prev = slot.cachedPositionSec.load(std::memory_order_relaxed);
			double accepted = posSec;
			if (slot.posCacheValid) {
				constexpr double kMaxPerCycle = 0.20;
				if (accepted < prev) accepted = prev;
				else if (accepted > prev + kMaxPerCycle)
					accepted = prev + kMaxPerCycle;
			} else {
				slot.posCacheValid = true;
			}
			slot.cachedPositionSec.store(accepted, std::memory_order_relaxed);
			slot.cachedLengthSec.store(lenSec, std::memory_order_relaxed);
		}

		// Check if this preview slot's file is done
		if (st == ePLAYING_PREVIEW && slot.inputFile && slot.inputFile->done())
		{
			SampleBuffer::Lock sbl(slot.sbPlayback.getMutex());
			if (slot.sbPlayback.avail() == 0)
			{
				slot.state = eSILENT;
				slot.peakL.store(0.0f);
				slot.peakR.store(0.0f);
				emit onStopPlaying(s);
			}
		}
	}

	if (totalWritten > 0)
		*channelFillMask |= (bitMaskLeft | bitMaskRight);

	return totalWritten;
}


//---------------------------------------------------------------
// Purpose: Also mix regular playback slots into local output
//---------------------------------------------------------------
bool Sampler::playFile(const SoundInfo &sound)
{
	return playSoundInSlot(-1, sound, false);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
bool Sampler::playPreview(const SoundInfo &sound)
{
	return playSoundInSlot(-1, sound, true);
}


//---------------------------------------------------------------
// Purpose: Stop playback for specific slot or all slots
//---------------------------------------------------------------
void Sampler::stopPlayback(int slot)
{
	// Collect deferred work under the lock; perform it after release:
	// FFmpeg teardown is slow (audio thread must not wait on it) and
	// signal emission under m_mutex is a latent deadlock.
	struct Stopped { InputFile *file; int slot; bool emitStop; };
	std::vector<Stopped> stopped;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		if (slot == -1)
		{
			for (int s = 0; s < MAX_SLOTS; s++)
			{
				bool e = false;
				InputFile *f = stopSlotInternal(s, e);
				if (f || e) stopped.push_back({f, s, e});
			}
		}
		else if (slot >= 0 && slot < MAX_SLOTS)
		{
			bool e = false;
			InputFile *f = stopSlotInternal(slot, e);
			if (f || e) stopped.push_back({f, slot, e});
		}
	}
	// Drain phase OUTSIDE m_mutex: stopSlotInternal calls
	// producer.setSource(nullptr) which is now lock-free (so it does
	// not stall the audio thread on long-audio decode). The producer
	// may still be inside one last readSamples on the detached file —
	// wait briefly per affected slot so the close+delete below cannot
	// race a still-running decode (UAF in FFmpeg context teardown).
	for (auto &st : stopped)
	{
		if (st.file && st.slot >= 0 && st.slot < MAX_SLOTS)
			m_slots[st.slot].producerThread.waitForReadDrain();
	}
	for (auto &st : stopped)
	{
		if (st.file) { st.file->close(); delete st.file; }
		if (st.emitStop) emit onStopPlaying(st.slot);
	}
}

// Slider->dB mapping. DB_MIN doubled from -28 to -56 when setVolumeDb
// switched from the (wrong) power convention 10^(dB/10) to the correct
// amplitude convention 10^(dB/20): with /10 every "dB" was applied at
// twice its value, so -28 dB of slider range actually attenuated by
// -56 dB of amplitude. Doubling DB_MIN keeps every existing slider
// position sounding exactly like before while the rest of the engine
// (ducking, per-sound dB) now applies true decibels.
#define VOLUMESCALER_EXPONENT 1.0
#define VOLUMESCALER_DB_MIN -56.0
//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setVolumeRemote( int vol )
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	double v = (double)vol / 100.0;
	double db = pow(1.0 - v, VOLUMESCALER_EXPONENT) * VOLUMESCALER_DB_MIN;
	m_globalDbSettingRemote = db;
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setVolumeLocal( int vol )
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	double v = (double)vol / 100.0;
	double db = pow(1.0 - v, VOLUMESCALER_EXPONENT) * VOLUMESCALER_DB_MIN;
	m_globalDbSettingLocal = db;
}


//---------------------------------------------------------------
// Purpose: Set per-slot local volume (for multi-mode)
//---------------------------------------------------------------
void Sampler::setSlotVolumeLocal(int slot, int vol)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		double v = (double)vol / 100.0;
		double db = pow(1.0 - v, VOLUMESCALER_EXPONENT) * VOLUMESCALER_DB_MIN;
		m_slots[slot].slotDbLocal = db;
	}
}


//---------------------------------------------------------------
// Purpose: Set per-slot remote volume (for multi-mode)
//---------------------------------------------------------------
void Sampler::setSlotVolumeRemote(int slot, int vol)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		double v = (double)vol / 100.0;
		double db = pow(1.0 - v, VOLUMESCALER_EXPONENT) * VOLUMESCALER_DB_MIN;
		m_slots[slot].slotDbRemote = db;
	}
}


void Sampler::setSlotPitchFactor(int slot, float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot < 0 || slot >= MAX_SLOTS) return;
	extremeLog("Sampler::setSlotPitchFactor slot=%d factor=%.4f", slot, factor);
	m_slots[slot].lastSlotPitchFactor = factor;
	if (m_slots[slot].inputFile)
		m_slots[slot].inputFile->setPitchFactor(factor);
}


void Sampler::setSlotSpeedFactor(int slot, float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	extremeLog("Sampler::setSlotSpeedFactor slot=%d factor=%.4f", slot, factor);
	if (slot >= 0 && slot < MAX_SLOTS && m_slots[slot].inputFile)
		m_slots[slot].inputFile->setSpeedFactor(factor);
}


void Sampler::setSlotReverbMix(int slot, float mix)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot < 0 || slot >= MAX_SLOTS) return;
	extremeLog("Sampler::setSlotReverbMix slot=%d mix=%.4f", slot, mix);
	auto &s = m_slots[slot];
	s.fxReverbWet = mix;
	if (s.dsp) {
		// Sandbox active on this slot - route the FxPanel reverb to
		// the END of the DSP pipeline (sandbox Reverb stage). User
		// requested: "the reverb must be the absolute final part of
		// the pipeline". Also clear the libavfilter pre-reverb so it
		// doesn't double-apply at decode time.
		s.dsp->setFxReverbWet(mix);
		if (s.inputFile) s.inputFile->setReverbMix(0.0f);
	} else if (s.inputFile) {
		// No sandbox -> existing legacy path (libavfilter at decode).
		s.inputFile->setReverbMix(mix);
	}
}


void Sampler::setSlotSoundDb(int slot, double db)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
		m_slots[slot].soundDbSetting = db;
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setLocalPlayback( bool enabled )
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_localPlayback.store(enabled, std::memory_order_relaxed);
	for (int i = 0; i < MAX_SLOTS; i++)
		m_slots[i].producerThread.setBufferEnabled(&m_slots[i].sbPlayback, enabled);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setMuteMyself(bool enabled)
{
	m_muteMyself.store(enabled, std::memory_order_relaxed);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setEarrapeProtection(bool enabled)
{
	m_earrapeProtection.store(enabled, std::memory_order_relaxed);
}


//---------------------------------------------------------------
// Purpose: Set pitch factor for all active slots
//---------------------------------------------------------------
void Sampler::setPitchFactor(float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_pitchFactor = factor;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		PlaybackSlot &slot = m_slots[i];
		if (slot.inputFile)
			slot.inputFile->setPitchFactor(factor);
	}
}


//---------------------------------------------------------------
// Purpose: Set speed factor for all active slots
//---------------------------------------------------------------
void Sampler::setSpeedFactor(float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_speedFactor = factor;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		PlaybackSlot &slot = m_slots[i];
		if (slot.inputFile)
			slot.inputFile->setSpeedFactor(factor);
	}
}


//---------------------------------------------------------------
// Purpose: Set intensity (gain) multiplier
//---------------------------------------------------------------
void Sampler::setIntensityFactor(float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_intensityFactor = factor;
}


//---------------------------------------------------------------
// Purpose: Set reverb mix for all active slots (0.0=dry, 1.0=full reverb)
//---------------------------------------------------------------
void Sampler::setReverbMix(float mix)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_reverbMix = mix;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		PlaybackSlot &slot = m_slots[i];
		if (slot.inputFile)
			slot.inputFile->setReverbMix(mix);
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::setVolumeDb( double decibel )
{
	// Amplitude convention (10^(dB/20)). The previous 10^(dB/10) was the
	// POWER convention and made every dB count double - notably the
	// sidechain duck depth (a requested -12 dB duck attenuated by -24)
	// and the per-sound volume. Slider ranges were recalibrated via
	// VOLUMESCALER_DB_MIN so perceived slider loudness is unchanged.
	double factor = AudioUtils::dbToLinear(decibel);
	m_volumeFactor = (float)factor;
	m_volumeDivider = (int)(factor * (1 << volumeScaleExp) + 0.5);
}


//---------------------------------------------------------------
// Purpose: Stop a single slot
//---------------------------------------------------------------
InputFile *Sampler::stopSlotInternal(int slot, bool &emitStop)
{
	emitStop = false;
	PlaybackSlot &s = m_slots[slot];
	// Bump the reverse-worker epoch unconditionally so any in-flight
	// async setSlotReverse worker discards its half-built InputFile
	// instead of swapping it into the slot AFTER the stop. Also flip
	// the worker's cancel token so a still-running decode pass
	// (multi-minute file) bails out within ~10-50 ms instead of
	// chewing CPU after the user has already moved on.
	++s.reverseEpoch;
	if (s.reverseWorkerCancel)
		s.reverseWorkerCancel->store(true, std::memory_order_relaxed);
	InputFile *detached = nullptr;
	if (s.inputFile)
	{
		s.state = eSILENT;
		s.producerThread.setSource(NULL);
		// DETACH the file instead of closing it here: FFmpeg context
		// teardown (codec close + format close + filter graph free) is
		// slow and the caller holds m_mutex - the audio thread would
		// stall behind it. The caller closes the returned pointer after
		// releasing the lock.
		detached = s.inputFile;
		s.inputFile = NULL;

		// Reset DSP so the next play starts paulstretch from a clean
		// state - otherwise the stretched output keeps playing the
		// previous sound's content for several seconds after stop.
		if (s.dsp) s.dsp->reset();

		// Clear buffers
		SampleBuffer::Lock sblc(s.sbCapture.getMutex());
		SampleBuffer::Lock sblp(s.sbPlayback.getMutex());
		s.sbCapture.consume(NULL, s.sbCapture.avail());
		s.sbPlayback.consume(NULL, s.sbPlayback.avail());

		// Reset per-slot volume to default
		s.slotDbLocal = -1.0;
		s.slotDbRemote = -1.0;

		// Reset peak meter so the channel widget snaps back to zero
		// instead of freezing at the last per-frame peak.
		s.peakL.store(0.0f);
		s.peakR.store(0.0f);

		// Stop also resets the rate-limiter anchor so the next play
		// starts the cursor from a clean state.
		s.cachedPositionSec.store(0.0, std::memory_order_relaxed);
		s.cachedLengthSec.store(0.0, std::memory_order_relaxed);
		s.posCacheValid = false;
		// Drop audioReverse: it tracks the active stream's direction.
		// Leaving it set after stop would pin the next play's first
		// cursor cycle into the reverse rate-limiter branch even when
		// the new sound plays forward.
		s.audioReverse = false;

		// Drop the per-playback random-variation footprint so the next
		// play starts with a fresh roll (the audio thread reads these
		// at loop boundaries; stale values from a previous file would
		// otherwise still drive a brand-new playback for one block).
		s.randomActive = false;
		// Release the sidechain duck gain so a slot that was being
		// ducked snaps back to unity on its next play. Otherwise a
		// stale 0.x value would attenuate the next sound until the
		// smoother walked back to 1.
		s.duckGain.store(1.0f, std::memory_order_relaxed);

		// The onStopPlaying emit is DEFERRED to the caller (after it
		// releases m_mutex). Emitting here, under the lock, was a
		// latent deadlock: a direct-connection slot calling back into
		// any locking Sampler method would self-deadlock the client.
		emitStop = true;
	}
	return detached;
}


//---------------------------------------------------------------
// Purpose: Find a free slot, -1 if none available
//---------------------------------------------------------------
int Sampler::findFreeSlot() const
{
	int maxSlots = m_multiMode ? MAX_SLOTS : 1;
	for (int i = 0; i < maxSlots; i++)
	{
		if (m_slots[i].state.load() == eSILENT)
			return i;
	}
	return -1;
}


//---------------------------------------------------------------
// Purpose: Play a sound in a specific slot (or find one if slot=-1)
//---------------------------------------------------------------
bool Sampler::playSoundInSlot(int slot, const SoundInfo &soundOrig, bool preview)
{
	SoundInfo sound = soundOrig;

	// ===== Phase 1: slot resolution + teardown of the previous sound,
	// under m_mutex. NO FFmpeg work happens in this phase - open() of
	// the new file (potentially hundreds of ms of disk + probe work)
	// used to run with the audio lock held, which stalled fetchSamples
	// and produced audible dropouts on every OTHER playing slot in
	// multi-mode. The old file is detached here and closed in Phase 2.
	InputFile *oldFile     = nullptr;
	bool       oldEmitStop = false;
	int        oldEmitSlot = -1;
	uint64_t   epoch       = 0;
	bool       chanReverse = false;
	float      prePitch    = 1.0f;
	double     randPitchMul = 1.0;
	bool       jitterActive = false;
	float      speedFactor  = 1.0f;
	float      reverbMix    = 0.0f;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);

		// Reset per-slot DSP state on new playback so EQ biquads + HRTF
		// smoothers + paulstretch ring don't carry residual state from
		// the previous sound (which produced a brief robotic transient
		// on start when the sandbox was already enabled).
		if (slot >= 0 && slot < MAX_SLOTS && m_slots[slot].dsp)
			m_slots[slot].dsp->reset();

		sdbgLog("playSoundInSlot: slot=%d file='%s' preview=%d startTime=%.2f playTime=%.2f vol=%.1f",
			slot, sound.filename.toUtf8().constData(), preview, sound.getStartTime(), sound.getPlayTime(), (double)sound.volume);

		if (slot == -1)
		{
			// In single mode, stop current sound first
			if (!m_multiMode)
			{
				slot = 0;
			}
			else
			{
				slot = findFreeSlot();
				if (slot == -1)
				{
					sdbgLog("  No free slot available (max %d)", m_multiMode ? MAX_SLOTS : 1);
					return false;
				}
			}
		}

		if (slot < 0 || slot >= MAX_SLOTS)
			return false;

		// Stop this slot if it's already playing (detach - close happens
		// outside the lock in Phase 2).
		oldFile = stopSlotInternal(slot, oldEmitStop);
		oldEmitSlot = slot;

		PlaybackSlot &s = m_slots[slot];

		// Claim the slot for THIS playback. Any stop / replay / reverse
		// toggle that lands while we are opening the file bumps the
		// epoch again, and Phase 3's re-check discards our half-built
		// InputFile instead of stomping the newer state.
		epoch = ++s.reverseEpoch;

		// Per-playback random PITCH-only jitter. Volume + start-offset
		// axes were dropped because they produced glitchy playback
		// (mid-loop volume jumps + cropStart re-seek artefacts). Pitch
		// jitter is applied here for the first fire; loop restarts in
		// fetchInputSamples re-read the LIVE SandboxState so disabling
		// random mid-playback stops the jitter immediately.
		if (s.dsp) {
			const SandboxState &st = s.dsp->state();
			if (st.enabled && st.randomEnabled && st.randomPitchCents > 0) {
				jitterActive = true;
				auto urand = []{ return (double)rand() / (double)RAND_MAX; };
				double j = (urand() * 2.0 - 1.0) * st.randomPitchCents;
				randPitchMul = std::pow(2.0, j / 1200.0);
			}
		}

		chanReverse = s.channelReverse;
		prePitch = (s.lastSlotPitchFactor > 0.01f
		          && std::fabs(s.lastSlotPitchFactor - 1.0f) > 1e-4f)
		          ? s.lastSlotPitchFactor : m_pitchFactor;
		speedFactor = m_speedFactor;
		reverbMix   = m_reverbMix;
	}

	// ===== Phase 2: heavy work OUTSIDE the audio lock. Close the old
	// FFmpeg context, deliver the deferred stop signal, then build +
	// open the new file. The audio thread keeps mixing the other slots
	// undisturbed for the whole duration.
	if (oldFile) {
		// setSource(nullptr) inside stopSlotInternal is lock-free, so
		// drain the producer before deleting the detached file to
		// avoid UAF if a readSamples is still in flight on it.
		if (oldEmitSlot >= 0 && oldEmitSlot < MAX_SLOTS)
			m_slots[oldEmitSlot].producerThread.waitForReadDrain();
		oldFile->close();
		delete oldFile;
	}
	if (oldEmitStop) emit onStopPlaying(oldEmitSlot);

	InputFile *newFile = CreateInputFileFFmpeg();
	sdbgLog("  CreateInputFileFFmpeg returned %p for slot %d", newFile, slot);

	// Reverse playback + loudness normalisation flags MUST be set
	// before open() so the filter graph picks them up on construction.
	// Per-channel reverse override (WaveformPlayer reverse button)
	// wins over the per-cell flag when ON.
	const bool isReverse = (sound.reverse || chanReverse);
	if (isReverse)           newFile->setReverse(true);
	if (sound.autoNormalize) newFile->setAutoNormalize(true);
	// Pre-set pitch / speed / reverb factors BEFORE open() so the
	// initial filter-graph build already applies them.
	{
		float preFactor = prePitch * static_cast<float>(randPitchMul);
		if (preFactor != 1.0f)   newFile->setPitchFactor(preFactor);
		if (speedFactor != 1.0f) newFile->setSpeedFactor(speedFactor);
		if (reverbMix > 0.0f)    newFile->setReverbMix(reverbMix);
	}

	int openRet = -1;
	try {
		openRet = newFile->open(sound.filename.toUtf8(), sound.getStartTime(), sound.getPlayTime());
	} catch (...) {
		// A malformed / corrupt file can throw deep inside the decoder.
		// Catch it here so the TS3 client never crashes - the slot just
		// reports a clean failure instead.
		openRet = -1;
	}
	sdbgLog("  open() returned %d", openRet);
	if (openRet != 0)
	{
		sdbgLog("  FAILED to open file, deleting inputFile");
		newFile->close();
		delete newFile;
		// Tell the UI so it can show a clear error instead of silently
		// doing nothing.
		emit onPlaybackError(slot, sound.filename);
		return false;
	}

	// ===== Phase 3: install under a brief lock. If the slot's epoch
	// moved while we were opening (user stopped / replayed / toggled
	// reverse), discard our file instead of clobbering the newer state.
	bool discarded = false;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		PlaybackSlot &s = m_slots[slot];
		if (s.reverseEpoch.load(std::memory_order_relaxed) != epoch ||
		    m_shuttingDown.load(std::memory_order_relaxed))
		{
			discarded = true;
		}
		else
		{
			s.inputFile = newFile;
			s.soundDbSetting = (double)sound.volume;
			s.stretchBaseTime = 0.0;
			// Remember the trim start so a looping slot restarts inside the
			// crop range. getStartTime() is 0.0 when the cell has no crop.
			s.cropStart = sound.getStartTime();
			{
				// getPlayTime() is the crop DURATION (-1 when no end point).
				double pt = sound.getPlayTime();
				s.cropEnd = (pt > 0.0) ? (s.cropStart + pt) : -1.0;
			}
			// Prime the lock-free position cache. Reverse playback starts
			// at the upper bound (cropEnd or file length) and walks down;
			// forward playback starts at cropStart.
			s.audioReverse = isReverse;
			double initialPos = s.cropStart;
			if (isReverse) {
				initialPos = s.inputFile->getPosition();
				if (initialPos <= s.cropStart) {
					initialPos = (s.cropEnd > 0.0)
						? s.cropEnd : s.inputFile->getLength();
				}
			}
			s.cachedPositionSec.store(initialPos, std::memory_order_relaxed);
			s.cachedLengthSec.store(s.inputFile->getLength(), std::memory_order_relaxed);
			// Fresh playback: invalidate the rate-limiter anchor so the
			// first audio-thread cache refresh snaps to the decoder
			// position instead of being clamped against a stale value.
			s.posCacheValid = false;
			// Fresh playback: clear the anti-glitch loop-rate guard so
			// the first loop boundary is timed cleanly (otherwise a
			// previous-session high-rate burst could carry over and
			// kill this playback before it loops once).
			s.lastLoopMonoMs   = 0;
			s.loopBurstCount   = 0;
			s.loopBurstStartMs = 0;
			s.slotDbLocal = m_globalDbSettingLocal;
			s.slotDbRemote = m_globalDbSettingRemote;
			double localDb = m_multiMode ? s.slotDbLocal : m_globalDbSettingLocal;
			setVolumeDb(localDb + s.soundDbSetting);
			sdbgLog("  volume set: soundDb=%.1f globalLocal=%.1f globalRemote=%.1f", s.soundDbSetting, m_globalDbSettingLocal, m_globalDbSettingRemote);
			sdbgLog("  pitch=%.2f (jitter %.3f) speed=%.2f reverb=%.2f applied to slot %d inputFile",
				prePitch, randPitchMul, speedFactor, reverbMix, slot);

			// Fast-path flag so the loop-restart hot loop can skip the
			// SandboxState re-read when random has never been enabled on
			// this slot.
			s.randomActive = jitterActive;
			// Remember the SoundInfo so setSlotReverse can re-trigger this
			// playback when the channel's reverse button is toggled mid-play.
			s.lastSound       = sound;
			s.lastSoundValid  = true;

			{
				SampleBuffer::Lock sblc(s.sbCapture.getMutex());
				SampleBuffer::Lock sblp(s.sbPlayback.getMutex());
				s.sbCapture.clear();
				s.sbPlayback.clear();
			}

			if (preview)
			{
				s.state = ePLAYING_PREVIEW;
				s.producerThread.setBufferEnabled(&s.sbCapture, false);
				s.producerThread.setBufferEnabled(&s.sbPlayback, true);
			}
			else
			{
				s.state = ePLAYING;
				s.producerThread.setBufferEnabled(&s.sbCapture, true);
				s.producerThread.setBufferEnabled(&s.sbPlayback, m_localPlayback.load(std::memory_order_relaxed));
			}

			// Lazy producer-thread start (idempotent after the first
			// play on this slot) - threads are no longer pre-spawned
			// for all 32 slots at init.
			s.producerThread.start();
			s.producerThread.setSource(s.inputFile);
		}
	}

	if (discarded)
	{
		sdbgLog("  slot epoch moved during open - discarding playback");
		newFile->close();
		delete newFile;
		return false;
	}

	emit onStartPlaying(slot, preview, sound.filename);

	return true;
}


//---------------------------------------------------------------
// Purpose: Pause playback for specific slot or all
//---------------------------------------------------------------
void Sampler::pausePlayback(int slot)
{
	// Emit after releasing m_mutex (see stopSlotInternal rationale).
	std::vector<int> paused;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		if (slot == -1)
		{
			for (int s = 0; s < MAX_SLOTS; s++)
			{
				if (m_slots[s].state == ePLAYING)
				{
					m_slots[s].state = ePAUSED;
					paused.push_back(s);
				}
			}
		}
		else if (slot >= 0 && slot < MAX_SLOTS)
		{
			if (m_slots[slot].state == ePLAYING)
			{
				m_slots[slot].state = ePAUSED;
				paused.push_back(slot);
			}
		}
	}
	for (int s : paused)
		emit onPausePlaying(s);
}


//---------------------------------------------------------------
// Purpose: Unpause playback for specific slot or all
//---------------------------------------------------------------
void Sampler::unpausePlayback(int slot)
{
	// Emit after releasing m_mutex (see stopSlotInternal rationale).
	std::vector<int> resumed;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		if (slot == -1)
		{
			for (int s = 0; s < MAX_SLOTS; s++)
			{
				if (m_slots[s].state == ePAUSED)
				{
					m_slots[s].state = ePLAYING;
					resumed.push_back(s);
				}
			}
		}
		else if (slot >= 0 && slot < MAX_SLOTS)
		{
			if (m_slots[slot].state == ePAUSED)
			{
				m_slots[slot].state = ePLAYING;
				resumed.push_back(slot);
			}
		}
	}
	for (int s : resumed)
		emit onUnpausePlaying(s);
}


//---------------------------------------------------------------
// Purpose: Get playback position for a specific slot
//---------------------------------------------------------------
double Sampler::getPosition(int slot)
{
	// Lock-free: the audio thread refreshes cachedPositionSec inside
	// fetchInputSamples once per cycle (~50 Hz). The GUI's 30 Hz timer
	// reads this atomic directly so it never contends with the audio
	// mutex - previously a system-wide lag source under the Leia engine.
	if (slot < 0 || slot >= MAX_SLOTS) return 0.0;
	PlaybackSlot &s = m_slots[slot];
	if (s.state.load(std::memory_order_relaxed) == eSILENT) return 0.0;
	return s.cachedPositionSec.load(std::memory_order_relaxed);
}


//---------------------------------------------------------------
// Purpose: Get total length for a specific slot
//---------------------------------------------------------------
double Sampler::getLength(int slot)
{
	// Lock-free, same rationale as getPosition above.
	if (slot < 0 || slot >= MAX_SLOTS) return 0.0;
	PlaybackSlot &s = m_slots[slot];
	if (s.state.load(std::memory_order_relaxed) == eSILENT) return 0.0;
	return s.cachedLengthSec.load(std::memory_order_relaxed);
}


//---------------------------------------------------------------
// Purpose: Get the crop range (seconds) applied to a slot
//---------------------------------------------------------------
void Sampler::getSlotCrop(int slot, double &startSec, double &endSec) const
{
	startSec = 0.0;
	endSec   = -1.0;
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		const PlaybackSlot &s = m_slots[slot];
		if (s.state != eSILENT)
		{
			startSec = s.cropStart;
			endSec   = s.cropEnd;
		}
	}
}


//---------------------------------------------------------------
// Purpose: Live-update the slot's crop range. Updates loop restart
// point, seek() clamp, marker overlay, and pushes the new upper
// bound into the decoder so current playback truncates immediately.
//---------------------------------------------------------------
void Sampler::setSlotCropLive(int slot, double startSec, double endSec)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot < 0 || slot >= MAX_SLOTS) return;
	if (startSec < 0.0) startSec = 0.0;
	if (endSec >= 0.0 && endSec <= startSec) endSec = -1.0;
	m_slots[slot].cropStart = startSec;
	m_slots[slot].cropEnd   = endSec;
	if (m_slots[slot].inputFile)
		m_slots[slot].inputFile->setMaxPlayTime(endSec);
}


//---------------------------------------------------------------
// Purpose: Seek to position for a specific slot
//---------------------------------------------------------------
void Sampler::seek(double seconds, int slot)
{
	if (slot < 0 || slot >= MAX_SLOTS)
		return;
	PlaybackSlot &s = m_slots[slot];
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		if (s.state == eSILENT) return;
		if (!s.inputFile)        return;
		// Clamp the seek target to the crop range. Without this the
		// user could skip back before the crop start and hear audio
		// outside the trimmed-in region.
		if (s.cropStart > 0.0 && seconds < s.cropStart)
			seconds = s.cropStart;
		if (s.cropEnd > 0.0 && seconds > s.cropEnd)
			seconds = s.cropEnd;
	}
	// Lock-free atomic enqueue + wake the dedicated seek worker. The
	// GUI thread RETURNS INSTANTLY — the slow FFmpeg backward scan
	// (300–500 ms on a 10-hour MP3) runs entirely on the worker, so
	// rapid waveform clicks never freeze the soundboard. Last-wins:
	// every call overwrites pendingSeekSec; whatever value the worker
	// reads when it next iterates is the committed target.
	s.pendingSeekSec.store(seconds, std::memory_order_release);
	extremeLog("Sampler::seek slot=%d target=%.3fs cropStart=%.3f cropEnd=%.3f",
	           slot, seconds, s.cropStart, s.cropEnd);
	startSeekWorker();
	m_seekCv.notify_all();
}


//---------------------------------------------------------------
// Lazy start of the seek worker thread on the first seek request.
// Idle-spawn keeps init() flat — most sessions never call seek().
//---------------------------------------------------------------
void Sampler::startSeekWorker()
{
	bool expected = false;
	if (!m_seekWorkerStarted.compare_exchange_strong(
			expected, true, std::memory_order_acq_rel))
		return;
	m_seekWorker = std::thread(&Sampler::seekWorkerProc, this);
}


//---------------------------------------------------------------
// Seek worker body. Waits on m_seekCv; when notified, scans every
// slot for a pendingSeekSec and processes them one by one. The
// FFmpeg seek runs OUTSIDE m_mutex (so the audio path is unblocked
// for the full scan duration); a brief m_mutex re-lock after the
// scan commits the buffer flush + cache update. Slot ownership of
// the InputFile is rechecked at commit time so a stop/swap that
// landed mid-scan is honoured (the worker silently discards its
// post-scan work in that case — the next seek (if any) will re-do
// it on the new file).
//---------------------------------------------------------------
void Sampler::seekWorkerProc()
{
	while (!m_seekStop.load(std::memory_order_acquire))
	{
		// Wait for pending work. We snapshot the per-slot atomics in
		// a small array so the slow FFmpeg scan below runs without
		// any held lock.
		struct Work { int slot; double target; InputFile *file; };
		std::vector<Work> batch;
		{
			std::unique_lock<std::mutex> lk(m_seekMutex);
			m_seekCv.wait(lk, [this]{
				if (m_seekStop.load(std::memory_order_acquire))
					return true;
				for (int i = 0; i < MAX_SLOTS; ++i) {
					double v = m_slots[i].pendingSeekSec.load(
						std::memory_order_acquire);
					if (!std::isnan(v)) return true;
				}
				return false;
			});
			if (m_seekStop.load(std::memory_order_acquire))
				return;
		}
		// Collect work under m_mutex (briefly): snapshot file
		// pointer + exchange the atomic. Multiple clicks during the
		// scan keep overwriting pendingSeekSec; the LAST value lands
		// here and earlier targets are silently discarded.
		{
			std::lock_guard<std::mutex> Lock(m_mutex);
			for (int i = 0; i < MAX_SLOTS; ++i) {
				double v = m_slots[i].pendingSeekSec.exchange(
					std::numeric_limits<double>::quiet_NaN(),
					std::memory_order_acq_rel);
				if (std::isnan(v)) continue;
				PlaybackSlot &s = m_slots[i];
				if (s.state == eSILENT || !s.inputFile) continue;
				batch.push_back({i, v, s.inputFile});
			}
		}
		for (const Work &w : batch) {
			if (m_seekStop.load(std::memory_order_acquire))
				return;
			// THE expensive part — runs without holding any sampler
			// lock so the audio thread keeps mixing pre-seek samples
			// from the existing buffer for the full scan duration.
			w.file->seek(w.target);
			// Re-lock briefly to commit. Skip the commit if the slot
			// state has rotated under us (stopSlot, playSoundInSlot,
			// reverse swap, shutdown) so we never stomp on a fresh
			// state with the result of a now-stale scan.
			std::lock_guard<std::mutex> Lock(m_mutex);
			PlaybackSlot &s = m_slots[w.slot];
			if (m_shuttingDown.load(std::memory_order_relaxed)) return;
			if (s.state == eSILENT || s.inputFile != w.file) continue;
			{
				SampleBuffer::Lock sblc(s.sbCapture.getMutex());
				SampleBuffer::Lock sblp(s.sbPlayback.getMutex());
				s.sbCapture.consume(NULL, s.sbCapture.avail());
				s.sbPlayback.consume(NULL, s.sbPlayback.avail());
			}
			if (s.dsp) s.dsp->reset();
			s.stretchBaseTime = w.target;
			s.cachedPositionSec.store(w.target,
				std::memory_order_relaxed);
			s.posCacheValid = false;
			s.producerThread.wake();
			// Notify GUI on the audio thread's queued path so the
			// per-channel cursor lock can release. Emitting under
			// m_mutex would deadlock any direct-connected slot; Qt's
			// default (auto) connection is queued across threads, so
			// this is safe — the slot fires on the GUI thread later.
			emit onSeekCommitted(w.slot);
		}
	}
}


//---------------------------------------------------------------
// Purpose: Get state of a specific slot
//---------------------------------------------------------------
Sampler::state_e Sampler::getState(int slot) const
{
	if (slot >= 0 && slot < MAX_SLOTS)
		return m_slots[slot].state.load();
	return eSILENT;
}


//---------------------------------------------------------------
// Purpose: Enable/disable multi-soundboard mode
//---------------------------------------------------------------
void Sampler::setMultiMode(bool enabled)
{
	struct Stopped { InputFile *file; int slot; bool emitStop; };
	std::vector<Stopped> stopped;
	{
		std::lock_guard<std::mutex> Lock(m_mutex);
		m_multiMode = enabled;

		// If disabling multi mode, stop all slots except slot 0
		if (!enabled)
		{
			for (int s = 1; s < MAX_SLOTS; s++)
			{
				bool e = false;
				InputFile *f = stopSlotInternal(s, e);
				if (f || e) stopped.push_back({f, s, e});
			}
		}
	}
	// Drain producers BEFORE closing: setSource(nullptr) inside
	// stopSlotInternal is lock-free now, so a producer may still be
	// in its final readSamples on a detached file. See stopPlayback
	// for the same pattern.
	for (auto &st : stopped)
	{
		if (st.file && st.slot >= 0 && st.slot < MAX_SLOTS)
			m_slots[st.slot].producerThread.waitForReadDrain();
	}
	for (auto &st : stopped)
	{
		if (st.file) { st.file->close(); delete st.file; }
		if (st.emitStop) emit onStopPlaying(st.slot);
	}
}


//---------------------------------------------------------------
// Purpose: Count active (non-silent) slots
//---------------------------------------------------------------
int Sampler::getActiveSlotCount() const
{
	int count = 0;
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		state_e st = m_slots[i].state.load();
		if (st != eSILENT)
			count++;
	}
	return count;
}


//---------------------------------------------------------------
// Purpose: Find first slot with the given state
//---------------------------------------------------------------
int Sampler::findSlotByState(state_e state) const
{
	for (int i = 0; i < MAX_SLOTS; i++)
	{
		if (m_slots[i].state.load() == state)
			return i;
	}
	return -1;
}
