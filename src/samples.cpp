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
	if (!g_rpsbLogsEnabled) return;
	sdbgOpen();
	if (!g_dbgSamples) return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(g_dbgSamples, "[samples] ");
	vfprintf(g_dbgSamples, fmt, ap);
	va_end(ap);
	fprintf(g_dbgSamples, "\n");
	fflush(g_dbgSamples);
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

void Sampler::setSlotSandboxState(int slot, const SandboxState &s)
{
	if (slot < 0 || slot >= MAX_SLOTS) return;
	std::lock_guard<std::mutex> Lock(m_mutex);
	PlaybackSlot &sl = m_slots[slot];
	bool wasStretchOn = sl.dsp ? sl.dsp->state().stretchEnabled : false;
	bool wasDspMissing = (sl.dsp == nullptr);
	if (!sl.dsp) sl.dsp = std::make_unique<SlotDsp>();
	sl.dsp->setSampleRate(48000.0);
	sl.dsp->applyState(s);
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
	std::lock_guard<std::mutex> Lock(m_mutex);

	int totalWritten = 0;

	for (int s = 0; s < MAX_SLOTS; s++)
	{
		PlaybackSlot &slot = m_slots[s];
		state_e st = slot.state.load();
		if (st != ePLAYING && st != ePAUSED)
			continue;

		if (st == ePAUSED)
			continue;

		// Set volume for this slot: per-slot in multi-mode, global otherwise
		double remoteDb = m_multiMode ? slot.slotDbRemote : m_globalDbSettingRemote;
		setVolumeDb(remoteDb + slot.soundDbSetting);

		bool muteCapture = m_muteMyself.load(std::memory_order_relaxed);
		bool isFirstSlot = (totalWritten == 0);
		int written = fetchSamples(slot.sbCapture, m_peakMeterCapture, samples, count, channels, true,
			0, 1, muteCapture && isFirstSlot, muteCapture && isFirstSlot, 0.0f, &slot);
		if (written > totalWritten)
			totalWritten = written;

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
					slot.inputFile->seek(slot.cropStart);
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

		// Set volume for this slot: per-slot in multi-mode, global otherwise
		double localDb = m_multiMode ? slot.slotDbLocal : m_globalDbSettingLocal;
		setVolumeDb(localDb + slot.soundDbSetting);

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
	if (slot >= 0 && slot < MAX_SLOTS && m_slots[slot].inputFile)
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
bool Sampler::playSoundInSlot(int slot, const SoundInfo &sound, bool preview)
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

	s.inputFile = CreateInputFileFFmpeg();
	sdbgLog("  CreateInputFileFFmpeg returned %p for slot %d", s.inputFile, slot);

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
	s.slotDbLocal = m_globalDbSettingLocal;
	s.slotDbRemote = m_globalDbSettingRemote;
	double localDb = m_multiMode ? s.slotDbLocal : m_globalDbSettingLocal;
	setVolumeDb(localDb + s.soundDbSetting);
	sdbgLog("  volume set: soundDb=%.1f globalLocal=%.1f globalRemote=%.1f", s.soundDbSetting, m_globalDbSettingLocal, m_globalDbSettingRemote);

	// Apply current pitch/speed factors to the new input file
	if (m_pitchFactor != 1.0f)
		s.inputFile->setPitchFactor(m_pitchFactor);
	if (m_speedFactor != 1.0f)
		s.inputFile->setSpeedFactor(m_speedFactor);
	if (m_reverbMix > 0.0f)
		s.inputFile->setReverbMix(m_reverbMix);
	sdbgLog("  pitch=%.2f speed=%.2f reverb=%.2f applied to slot %d inputFile", m_pitchFactor, m_speedFactor, m_reverbMix, slot);

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
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		PlaybackSlot &s = m_slots[slot];
		if (s.inputFile && s.state != eSILENT)
		{
			if (s.dsp && s.dsp->isStretchEnabled()) {
				double pos = s.stretchBaseTime + s.dsp->stretchPlaybackPosition();
				double len = s.inputFile->getLength();
				if (len > 0.0 && pos > len) pos = std::fmod(pos, len);
				return pos < 0.0 ? 0.0 : pos;
			}
			double decoderPos = s.inputFile->getPosition();
			// avail() counts post-effect OUTPUT samples (48 kHz). The
			// decoder position is in INPUT-file time, advanced by
			// speedFactor. Converting the buffered span back to input
			// time needs the same speedFactor — without it the cursor
			// jumped erratically the moment a slowdown/effect was applied.
			double sf = (m_speedFactor > 0.0f) ? (double)m_speedFactor : 1.0;
			double bufferedSec = s.sbPlayback.avail() / 48000.0 * sf;
			double audible = decoderPos - bufferedSec;
			return audible < 0.0 ? 0.0 : audible;
		}
	}
	return 0.0;
}


//---------------------------------------------------------------
// Purpose: Get total length for a specific slot
//---------------------------------------------------------------
double Sampler::getLength(int slot)
{
	std::lock_guard<std::mutex> Lock(m_mutex);
	if (slot >= 0 && slot < MAX_SLOTS)
	{
		PlaybackSlot &s = m_slots[slot];
		if (s.inputFile && s.state != eSILENT)
			return s.inputFile->getLength();
	}
	return 0.0;
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
