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
#include "dsp/SlotDsp.h"
#include "dsp/SandboxState.h"

#include <queue>
#include <vector>
#include <cassert>
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
	m_slots[slot].loop = on;
}

void Sampler::setSlotReverse(int slot, bool on)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;

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
		state_e st = s.state.load();
		if (!((st == ePLAYING || st == ePAUSED) && s.lastSoundValid))
			return;

		resumeSec   = s.cachedPositionSec.load(std::memory_order_relaxed);
		lastSound   = s.lastSound;
		pitchBase   = (s.lastSlotPitchFactor > 0.01f)
			? s.lastSlotPitchFactor : m_pitchFactor;
		speedFactor = m_speedFactor;
		reverbMix   = m_reverbMix;

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

	// Phase 2 — wait for the previous worker (if any) to acknowledge
	// the cancel flag and exit. We set it true in Phase 1 above; the
	// decode loop polls every 64 packets, so this typically returns
	// within ~10-100 ms even on multi-minute files. join() (not
	// detach) keeps the Sampler dtor safe — every worker is owned by
	// exactly one std::thread that we can wait on at shutdown.
	PlaybackSlot &s = m_slots[slot];
	if (s.reverseWorker.joinable())
		s.reverseWorker.join();

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
		if (s.dsp) s.dsp->reset();

		s.inputFile = newFile;
		s.cachedPositionSec.store(resumeSec, std::memory_order_relaxed);
		s.posCacheValid = false;
		s.producerThread.setSource(s.inputFile);
	}

	// Close + delete the old file OUTSIDE the audio lock. Closing an
	// FFmpeg decoder context is non-trivial (codec close + format
	// close + filter graph teardown) — keeping it out of m_mutex
	// frees the audio thread the moment the pointer swap is done.
	if (oldFile) {
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
		slot.producerThread.start();
	}
	sdbgLog("Sampler::init() done, %d producer threads started", MAX_SLOTS);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void Sampler::shutdown()
{
	// Flip the global shutdown flag FIRST (no lock). Any reverse
	// worker that is past its cancel check but not yet through the
	// swap section will see this and discard its work instead of
	// touching slot state we're about to tear down.
	m_shuttingDown.store(true, std::memory_order_release);

	// Signal every in-flight worker to cancel its decode loop, then
	// join them OUTSIDE m_mutex. Joining inside the lock would
	// deadlock because the worker grabs m_mutex to perform the swap.
	for (int i = 0; i < MAX_SLOTS; i++) {
		PlaybackSlot &slot = m_slots[i];
		if (slot.reverseWorkerCancel)
			slot.reverseWorkerCancel->store(true, std::memory_order_relaxed);
	}
	for (int i = 0; i < MAX_SLOTS; i++) {
		PlaybackSlot &slot = m_slots[i];
		if (slot.reverseWorker.joinable())
			slot.reverseWorker.join();
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
		slot.producerThread.stop();
	}
}

//---------------------------------------------------------------
// Purpose: Fetch and mix samples from a single buffer
//---------------------------------------------------------------
int Sampler::fetchSamples(SampleBuffer &sb, PeakMeter &pm, short *samples, int count, int channels, bool eraseConsumed, int ciLeft, int ciRight, bool overLeft, bool overRight, float ampThresh, PlaybackSlot *slot)
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
			if (intensity <= 1.01f)
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
			if (intensity <= 1.01f)
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
			? static_cast<float>(std::pow(10.0, duckTargetDb / 20.0))
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
		                    : 20.0 * std::log10(curGain);

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
				// Reverse: decoder runs file backward, so samples still
				// in the playback buffer correspond to forward times
				// HIGHER than decoderPos. Add instead of subtract.
				if (slot.channelReverse) {
					posSec = decoderPos + bufferedSec;
					if (posSec > lenSec) posSec = lenSec;
				} else {
					posSec = decoderPos - bufferedSec;
					if (posSec < 0.0) posSec = 0.0;
				}
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
					if (!slot.channelReverse) {
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
			{
				SampleBuffer::Lock sbl(slot.sbCapture.getMutex());
				canEnd = slot.sbCapture.avail() == 0;
			}
			if (isStretch && canEnd)
				canEnd = slot.dsp->stretchCaptureDone();

			if (canEnd)
			{
				if (slot.loop) {
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
					double loopStartSec = slot.cropStart;
					if (slot.channelReverse) {
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
		                    : 20.0 * std::log10(std::max(curDuck, 1e-4f));
		double localDb = m_multiMode ? slot.slotDbLocal : m_globalDbSettingLocal;
		setVolumeDb(localDb + slot.soundDbSetting + duckActiveDb);

		bool isFirstSlot = (totalWritten == 0);
		int written = fetchSamples(slot.sbPlayback, m_peakMeterPlayback, samples, count, channels, true,
			ciLeft, ciRight,
			isFirstSlot && ((*channelFillMask & bitMaskLeft) == 0),
			isFirstSlot && ((*channelFillMask & bitMaskRight) == 0),
			localThresh, &slot);

		if (written > totalWritten)
			totalWritten = written;

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
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot == -1)
	{
		for (int s = 0; s < MAX_SLOTS; s++)
			stopSlotInternal(s);
	}
	else if (slot >= 0 && slot < MAX_SLOTS)
	{
		stopSlotInternal(slot);
	}
}

#define VOLUMESCALER_EXPONENT 1.0
#define VOLUMESCALER_DB_MIN -28.0
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
	m_slots[slot].lastSlotPitchFactor = factor;
	if (m_slots[slot].inputFile)
		m_slots[slot].inputFile->setPitchFactor(factor);
}


void Sampler::setSlotSpeedFactor(int slot, float factor)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS && m_slots[slot].inputFile)
		m_slots[slot].inputFile->setSpeedFactor(factor);
}


void Sampler::setSlotReverbMix(int slot, float mix)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot < 0 || slot >= MAX_SLOTS) return;
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
	double factor = pow(10.0, decibel/10.0);
	m_volumeFactor = (float)factor;
	m_volumeDivider = (int)(factor * (1 << volumeScaleExp) + 0.5);
}


//---------------------------------------------------------------
// Purpose: Stop a single slot
//---------------------------------------------------------------
void Sampler::stopSlotInternal(int slot)
{
	PlaybackSlot &s = m_slots[slot];
	// Bump the reverse-worker epoch unconditionally so any in-flight
	// async setSlotReverse worker discards its half-built InputFile
	// instead of swapping it into the slot AFTER the stop. Also flip
	// the worker's cancel token so a still-running preDecode pass
	// (multi-minute file) bails out within ~10-50 ms instead of
	// chewing CPU after the user has already moved on.
	++s.reverseEpoch;
	if (s.reverseWorkerCancel)
		s.reverseWorkerCancel->store(true, std::memory_order_relaxed);
	if (s.inputFile)
	{
		s.state = eSILENT;
		s.producerThread.setSource(NULL);
		s.inputFile->close();
		delete s.inputFile;
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

		emit onStopPlaying(slot);
	}
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
	std::lock_guard<std::mutex> Lock(m_mutex);
	// Reset per-slot DSP state on new playback so EQ biquads + HRTF
	// smoothers + paulstretch ring don't carry residual state from
	// the previous sound (which produced a brief robotic transient
	// on start when the sandbox was already enabled).
	if (slot >= 0 && slot < MAX_SLOTS && m_slots[slot].dsp)
		m_slots[slot].dsp->reset();

	SoundInfo sound = soundOrig;

	sdbgLog("playSoundInSlot: slot=%d file='%s' preview=%d startTime=%.2f playTime=%.2f vol=%.1f",
		slot, sound.filename.toUtf8().constData(), preview, sound.getStartTime(), sound.getPlayTime(), (double)sound.volume);

	if (slot == -1)
	{
		// In single mode, stop current sound first
		if (!m_multiMode)
		{
			stopSlotInternal(0);
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

	// Stop this slot if it's already playing
	stopSlotInternal(slot);

	PlaybackSlot &s = m_slots[slot];

	// Per-playback random PITCH-only jitter. Volume + start-offset
	// axes were dropped because they produced glitchy playback
	// (mid-loop volume jumps + cropStart re-seek artefacts). Pitch
	// jitter is applied here for the first fire; loop restarts in
	// fetchInputSamples re-read the LIVE SandboxState so disabling
	// random mid-playback stops the jitter immediately.
	double randPitchMul = 1.0;
	bool   jitterActive = false;
	if (s.dsp) {
		const SandboxState &st = s.dsp->state();
		if (st.enabled && st.randomEnabled && st.randomPitchCents > 0) {
			jitterActive = true;
			auto urand = []{ return (double)rand() / (double)RAND_MAX; };
			double j = (urand() * 2.0 - 1.0) * st.randomPitchCents;
			randPitchMul = std::pow(2.0, j / 1200.0);
		}
	}

	s.inputFile = CreateInputFileFFmpeg();
	sdbgLog("  CreateInputFileFFmpeg returned %p for slot %d", s.inputFile, slot);

	// Reverse playback + loudness normalisation flags MUST be set
	// before open() so the filter graph picks them up on construction.
	// Per-channel reverse override (WaveformPlayer reverse button)
	// wins over the per-cell flag when ON.
	if (sound.reverse || s.channelReverse) s.inputFile->setReverse(true);
	if (sound.autoNormalize)               s.inputFile->setAutoNormalize(true);
	// Pre-set pitch / speed / reverb factors BEFORE open() so the
	// initial filter-graph build (and, in reverse mode, the
	// pre-decode pass) already applies them. Without this the reverse
	// buffer was built at identity and the first ~250 ms of playback
	// rendered without atempo/asetrate.
	{
		float prePitch = (s.lastSlotPitchFactor > 0.01f
		                && std::fabs(s.lastSlotPitchFactor - 1.0f) > 1e-4f)
		                ? s.lastSlotPitchFactor : m_pitchFactor;
		float preFactor = prePitch * static_cast<float>(randPitchMul);
		if (preFactor != 1.0f) s.inputFile->setPitchFactor(preFactor);
		if (m_speedFactor != 1.0f) s.inputFile->setSpeedFactor(m_speedFactor);
		if (m_reverbMix > 0.0f) s.inputFile->setReverbMix(m_reverbMix);
	}

	int openRet = -1;
	try {
		openRet = s.inputFile->open(sound.filename.toUtf8(), sound.getStartTime(), sound.getPlayTime());
	} catch (...) {
		// A malformed / corrupt file can throw deep inside the decoder.
		// Catch it here so the TS3 client never crashes — the slot just
		// reports a clean failure instead.
		openRet = -1;
	}
	sdbgLog("  open() returned %d", openRet);
	if (openRet != 0)
	{
		sdbgLog("  FAILED to open file, deleting inputFile");
		delete s.inputFile;
		s.inputFile = NULL;
		// Tell the UI so it can show a clear error instead of silently
		// doing nothing.
		emit onPlaybackError(slot, sound.filename);
		return false;
	}

	s.soundDbSetting = (double)sound.volume;
	s.stretchBaseTime = 0.0;
	// Remember the trim start so a looping slot restarts inside the crop
	// range. getStartTime() is 0.0 when the cell has no crop configured.
	s.cropStart = sound.getStartTime();
	{
		// getPlayTime() is the crop DURATION (-1 when no end point).
		double pt = sound.getPlayTime();
		s.cropEnd = (pt > 0.0) ? (s.cropStart + pt) : -1.0;
	}
	// Prime the lock-free position cache with sensible initial values
	// so the GUI's first poll (before the audio thread has run a fetch)
	// reads the file length we just opened instead of a stale 0 from
	// a previous slot owner.
	s.cachedPositionSec.store(s.cropStart, std::memory_order_relaxed);
	s.cachedLengthSec.store(s.inputFile->getLength(), std::memory_order_relaxed);
	// Fresh playback: invalidate the rate-limiter anchor so the first
	// audio-thread cache refresh snaps to the actual decoder position
	// instead of being clamped against a stale previous-playback value.
	s.posCacheValid = false;
	s.slotDbLocal = m_globalDbSettingLocal;
	s.slotDbRemote = m_globalDbSettingRemote;
	double localDb = m_multiMode ? s.slotDbLocal : m_globalDbSettingLocal;
	setVolumeDb(localDb + s.soundDbSetting);
	sdbgLog("  volume set: soundDb=%.1f globalLocal=%.1f globalRemote=%.1f", s.soundDbSetting, m_globalDbSettingLocal, m_globalDbSettingRemote);

	// Pitch / speed / reverb were already applied above (BEFORE open
	// so the filter graph / reverse buffer build with them baked in).
	// Reverb mix only needs a refresh because the per-sample freeverb
	// reads the live m_reverbMix every block.
	if (m_reverbMix > 0.0f && std::fabs(m_reverbMix - 0.0f) > 1e-4f)
		s.inputFile->setReverbMix(m_reverbMix);
	sdbgLog("  pitch=%.2f (jitter %.3f) speed=%.2f reverb=%.2f applied to slot %d inputFile",
		m_pitchFactor, randPitchMul, m_speedFactor, m_reverbMix, slot);

	// Fast-path flag so the loop-restart hot loop can skip the
	// SandboxState re-read when random has never been enabled on this
	// slot. The live state is still checked at every loop boundary
	// (see fetchInputSamples) so a user enabling random mid-playback
	// still gets jitter on the next loop iteration.
	s.randomActive = jitterActive;
	// Remember the SoundInfo so setSlotReverse can re-trigger this
	// playback when the channel's reverse button is toggled mid-play.
	s.lastSound       = sound;
	s.lastSoundValid  = true;

	SampleBuffer::Lock sblc(s.sbCapture.getMutex());
	SampleBuffer::Lock sblp(s.sbPlayback.getMutex());

	// Clear buffers
	s.sbCapture.consume(NULL, s.sbCapture.avail());
	s.sbPlayback.consume(NULL, s.sbPlayback.avail());

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

	s.producerThread.setSource(s.inputFile);

	emit onStartPlaying(slot, preview, sound.filename);

	return true;
}


//---------------------------------------------------------------
// Purpose: Pause playback for specific slot or all
//---------------------------------------------------------------
void Sampler::pausePlayback(int slot)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot == -1)
	{
		for (int s = 0; s < MAX_SLOTS; s++)
		{
			if (m_slots[s].state == ePLAYING)
			{
				m_slots[s].state = ePAUSED;
				emit onPausePlaying(s);
			}
		}
	}
	else if (slot >= 0 && slot < MAX_SLOTS)
	{
		if (m_slots[slot].state == ePLAYING)
		{
			m_slots[slot].state = ePAUSED;
			emit onPausePlaying(slot);
		}
	}
}


//---------------------------------------------------------------
// Purpose: Unpause playback for specific slot or all
//---------------------------------------------------------------
void Sampler::unpausePlayback(int slot)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot == -1)
	{
		for (int s = 0; s < MAX_SLOTS; s++)
		{
			if (m_slots[s].state == ePAUSED)
			{
				m_slots[s].state = ePLAYING;
				emit onUnpausePlaying(s);
			}
		}
	}
	else if (slot >= 0 && slot < MAX_SLOTS)
	{
		if (m_slots[slot].state == ePAUSED)
		{
			m_slots[slot].state = ePLAYING;
			emit onUnpausePlaying(slot);
		}
	}
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
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		PlaybackSlot &s = m_slots[slot];
		if (s.inputFile && s.state != eSILENT)
		{
			// Clamp the seek target to the crop range. Without this the
			// user could skip back before the crop start and hear audio
			// outside the trimmed-in region.
			if (s.cropStart > 0.0 && seconds < s.cropStart)
				seconds = s.cropStart;
			if (s.cropEnd > 0.0 && seconds > s.cropEnd)
				seconds = s.cropEnd;
			s.inputFile->seek(seconds);

			SampleBuffer::Lock sblc(s.sbCapture.getMutex());
			SampleBuffer::Lock sblp(s.sbPlayback.getMutex());
			s.sbCapture.clear();
			s.sbPlayback.clear();

			if (s.dsp) s.dsp->reset();
			s.stretchBaseTime = seconds;
			// Explicit seek: snap cache to target + invalidate anchor
			// so the rate-limiter does not reject the user-requested
			// backward jump.
			s.cachedPositionSec.store(seconds, std::memory_order_relaxed);
			s.posCacheValid = false;
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
	std::lock_guard<std::mutex> Lock(m_mutex);
	m_multiMode = enabled;

	// If disabling multi mode, stop all slots except slot 0
	if (!enabled)
	{
		for (int s = 1; s < MAX_SLOTS; s++)
			stopSlotInternal(s);
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
