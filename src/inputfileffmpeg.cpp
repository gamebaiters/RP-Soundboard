// src/inputfileffmpeg.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#include "common.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <vector>
#include <deque>
#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <thread>

#include "ts3log.h"
#include "inputfile.h"
#include "SampleBuffer.h"
#include "SampleSource.h"
#include "main.h"
#include <mutex>
#include <atomic>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
}

#include <chrono>


#define OUTPUT_BUFFER_COUNT 32768
#define OUTPUT_FORMAT AV_SAMPLE_FMT_S16

// ===== FILE DEBUG LOGGING =====
// Compile-time gate. Always 1 in shipping builds so the runtime
// checkbox (model->setLogsEnabled -> g_rpsbLogsEnabled) actually
// controls whether bytes hit the disk. With this at 0 every dbgLog
// would compile to a no-op and the user's "Write debug log file"
// checkbox would have nothing to switch on.
#define RPSB_FILE_DEBUG 1

#if RPSB_FILE_DEBUG
#include <cstdio>
#include <cstdarg>
#include <ctime>
#ifndef _WIN32
#include <cstdlib>
#include <limits.h>
#endif
#include "plugin.h"
#include "ts3log.h"
static FILE *g_debugFile = nullptr;
static void dbgOpen()
{
	if (!g_rpsbLogsEnabled) return;
	if (!g_debugFile)
	{
		const char *cfgDir = getTs3ConfigPath();
		if (cfgDir && cfgDir[0])
		{
			char path[PATH_BUFSIZE + 64];
			snprintf(path, sizeof(path), "%srpsb_debug.log", cfgDir);
			g_debugFile = fopen(path, "a");
		}
		if (g_debugFile)
		{
			time_t t = time(NULL);
			struct tm *tm = localtime(&t);
			char ts[64];
			strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
			fprintf(g_debugFile, "\n=== RPSB Debug Session %s ===\n", ts);
			fflush(g_debugFile);
		}
	}
}
static void dbgLog(const char *fmt, ...)
{
	// Format once into a stack buffer so we can dispatch the same
	// rendered line to BOTH the on-disk rpsb_debug.log and the
	// in-memory ring read by the in-app log viewer - the user's
	// expectation is that the viewer mirrors exactly what the file
	// gets when "Write debug log file" is on.
	if (!g_rpsbLogsEnabled) return;
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	dbgOpen();
	if (g_debugFile) {
		fputs(buf, g_debugFile);
		fputc('\n', g_debugFile);
		fflush(g_debugFile);
	}
	rpsbDebugRingPush(buf);
}
#else
#define dbgLog(...) ((void)0)
#endif
// ===== END FILE DEBUG LOGGING =====

void InitFFmpegLibrary()
{
	dbgLog("InitFFmpegLibrary called");
}


int LogFFmpegError(int code, const char *msg = NULL)
{
	if(code < 0)
	{
		char buf[256];
		if(av_strerror(code, buf, sizeof buf) < 0)
			strcpy(buf, "Unknown Error");
		if(msg)
			logError("%s. FFmpeg Error: %s", msg, buf);
		else
			logError("FFmpeg Error: %s", buf);
	}

	return code;
}


// =====================================================================
// Freeverb implementation (Jezar at Dreampoint)
// 8 parallel LBCF (Lowpass-feedback Comb Filters) + 4 series All-Pass
// Tuning constants from the original Freeverb public domain source.
// =====================================================================

static const float kFixedGain   = 0.012f;
static const float kScaleWet    = 3.0f;
static const float kScaleDry    = 2.0f;
static const float kScaleDamp   = 0.4f;
static const float kScaleRoom   = 0.28f;
static const float kOffsetRoom  = 0.7f;

// Comb filter delay lengths for 44100 Hz (Jezar original tuning)
static const int kCombTuning[8] = {
	1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617
};
// All-pass delay lengths for 44100 Hz
static const int kAllPassTuning[4] = {
	225, 341, 441, 556
};
// Stereo spread (added to right-channel delay lines)
static const int kStereoSpread = 23;

static inline int scaleDelay(int baseSamples, int targetRate)
{
	return (int)((double)baseSamples * (double)targetRate / 44100.0 + 0.5);
}

// --- Comb Filter (Lowpass-Feedback) ---
class CombFilter
{
public:
	CombFilter() : m_buf(NULL), m_bufSize(0), m_idx(0), m_filterStore(0.0f),
	               m_feedback(0.0f), m_damp1(0.0f), m_damp2(1.0f) {}
	~CombFilter() { delete[] m_buf; }

	void init(int size)
	{
		delete[] m_buf;
		m_bufSize = size;
		m_buf = new float[size];
		memset(m_buf, 0, size * sizeof(float));
		m_idx = 0;
		m_filterStore = 0.0f;
	}

	void mute()
	{
		if (m_buf) memset(m_buf, 0, m_bufSize * sizeof(float));
		m_filterStore = 0.0f;
	}

	void setDamp(float val)   { m_damp1 = val; m_damp2 = 1.0f - val; }
	void setFeedback(float f) { m_feedback = f; }

	inline float process(float input)
	{
		float output = m_buf[m_idx];
		// Lowpass filter inside the feedback loop (Jezar's undenormalise omitted,
		// we add a tiny DC offset instead)
		m_filterStore = output * m_damp2 + m_filterStore * m_damp1 + 1e-20f;
		m_buf[m_idx] = input + m_filterStore * m_feedback;
		if (++m_idx >= m_bufSize) m_idx = 0;
		return output;
	}

private:
	float *m_buf;
	int    m_bufSize;
	int    m_idx;
	float  m_filterStore;
	float  m_feedback;
	float  m_damp1;
	float  m_damp2;
};

// --- All-Pass Filter ---
class AllPassFilter
{
public:
	AllPassFilter() : m_buf(NULL), m_bufSize(0), m_idx(0) {}
	~AllPassFilter() { delete[] m_buf; }

	void init(int size)
	{
		delete[] m_buf;
		m_bufSize = size;
		m_buf = new float[size];
		memset(m_buf, 0, size * sizeof(float));
		m_idx = 0;
	}

	void mute()
	{
		if (m_buf) memset(m_buf, 0, m_bufSize * sizeof(float));
	}

	inline float process(float input)
	{
		float bufOut = m_buf[m_idx];
		m_buf[m_idx] = input + bufOut * 0.5f;
		if (++m_idx >= m_bufSize) m_idx = 0;
		return bufOut - input;
	}

private:
	float *m_buf;
	int    m_bufSize;
	int    m_idx;
};

// --- Freeverb model ---
class Freeverb
{
public:
	Freeverb() : m_roomSize(0.0f), m_damp(0.0f),
	             m_sampleRate(48000), m_numChannels(1) {}

	void init(int sampleRate, int numChannels)
	{
		m_sampleRate = sampleRate;
		m_numChannels = (numChannels >= 2) ? 2 : 1;

		for (int i = 0; i < 8; i++)
		{
			int len = scaleDelay(kCombTuning[i], sampleRate);
			m_combL[i].init(len);
			if (m_numChannels == 2)
				m_combR[i].init(len + scaleDelay(kStereoSpread, sampleRate));
		}
		for (int i = 0; i < 4; i++)
		{
			int len = scaleDelay(kAllPassTuning[i], sampleRate);
			m_allPassL[i].init(len);
			if (m_numChannels == 2)
				m_allPassR[i].init(len + scaleDelay(kStereoSpread, sampleRate));
		}

		// Slowed+reverb defaults: large room, heavy damping for dark/warm tail
		setRoomSize(0.85f);
		setDamp(0.7f);
	}

	void mute()
	{
		for (int i = 0; i < 8; i++) { m_combL[i].mute(); m_combR[i].mute(); }
		for (int i = 0; i < 4; i++) { m_allPassL[i].mute(); m_allPassR[i].mute(); }
	}

	void setRoomSize(float v) {
		m_roomSize = v;
		float fb = v * kScaleRoom + kOffsetRoom;
		for (int i = 0; i < 8; i++) { m_combL[i].setFeedback(fb); m_combR[i].setFeedback(fb); }
	}

	void setDamp(float v) {
		m_damp = v;
		float d = v * kScaleDamp;
		for (int i = 0; i < 8; i++) { m_combL[i].setDamp(d); m_combR[i].setDamp(d); }
	}

	// Process interleaved int16 samples in-place.
	// mix (slider 0.0–1.0): linear crossfade between dry and wet.
	// Final = dry * (1 - mix) + wet * mix   →  unity gain guaranteed.
	void process(short *samples, int numFrames, float mix)
	{
		if (mix < 0.001f) return;
		if (mix > 1.0f) mix = 1.0f;

		// Scale room/damp with the slider for the slowed+reverb vibe
		float room = 0.70f + 0.28f * mix;
		float damp = 0.30f + 0.55f * mix;
		setRoomSize(room);
		setDamp(damp);

		// Linear crossfade coefficients (never exceed unity)
		float dryCoeff = 1.0f - mix;
		float wetCoeff = mix;

		if (m_numChannels == 2)
		{
			for (int f = 0; f < numFrames; f++)
			{
				// Normalize to [-1, 1]
				float inL = (float)samples[f * 2]     * (1.0f / 32768.0f);
				float inR = (float)samples[f * 2 + 1] * (1.0f / 32768.0f);

				// Input scaling: attenuate before the 8 parallel combs (Jezar's 0.015)
				float input = (inL + inR) * kFixedGain;

				// 8 comb filters in parallel
				float outL = 0.0f, outR = 0.0f;
				for (int c = 0; c < 8; c++) { outL += m_combL[c].process(input); outR += m_combR[c].process(input); }
				// 4 all-pass filters in series
				for (int a = 0; a < 4; a++) { outL = m_allPassL[a].process(outL); outR = m_allPassR[a].process(outR); }

				// Linear crossfade: final = dry*(1-mix) + wet*mix
				float finalL = inL * dryCoeff + outL * wetCoeff;
				float finalR = inR * dryCoeff + outR * wetCoeff;

				// Hard clamp to [-1, 1] then back to int16
				if (finalL >  1.0f) finalL =  1.0f; if (finalL < -1.0f) finalL = -1.0f;
				if (finalR >  1.0f) finalR =  1.0f; if (finalR < -1.0f) finalR = -1.0f;
				samples[f * 2]     = (short)(finalL * 32767.0f);
				samples[f * 2 + 1] = (short)(finalR * 32767.0f);
			}
		}
		else // mono
		{
			for (int f = 0; f < numFrames; f++)
			{
				float in = (float)samples[f] * (1.0f / 32768.0f);
				float input = in * kFixedGain;

				float out = 0.0f;
				for (int c = 0; c < 8; c++) out += m_combL[c].process(input);
				for (int a = 0; a < 4; a++) out = m_allPassL[a].process(out);

				float final_ = in * dryCoeff + out * wetCoeff;
				if (final_ >  1.0f) final_ =  1.0f; if (final_ < -1.0f) final_ = -1.0f;
				samples[f] = (short)(final_ * 32767.0f);
			}
		}
	}

private:
	CombFilter    m_combL[8], m_combR[8];
	AllPassFilter m_allPassL[4], m_allPassR[4];
	float m_roomSize, m_damp;
	int   m_sampleRate, m_numChannels;
};


class InputFileFFmpeg : public InputFile
{
public:
	InputFileFFmpeg(const InputFileOptions &options);
	~InputFileFFmpeg();
	int open(const char *filename, double startPosSeconds = 0.0, double playTimeSeconds = -1.0) override;
	int close() override;

	int readSamples(SampleProducer *sampleBuffer) override;
	bool done() const override;
	int seek(double seconds) override;
	double getPosition() const override;
	double getLength() const override;
	int64_t outputSamplesEstimation() const override;
	void setPitchFactor(float factor) override;
	void setSpeedFactor(float factor) override;
	float getSpeedFactor() const override { return m_speedFactor.load(std::memory_order_relaxed); }
	// Streaming-reverse: speed of the chunk currently being drained by
	// the reader (= the speed of the audio about to leave sbPlayback).
	// Sampler::fetchInputSamples uses this to descend the position
	// cursor at the rate of the audio that is ACTUALLY playing rather
	// than the rate the user picked one slider tick ago. Forward path
	// (no chunked decoder) returns the live m_speedFactor unchanged.
	float getCurrentReverseChunkSpeed() const override {
		if (!m_streamingReverse)
			return m_speedFactor.load(std::memory_order_relaxed);
		float v = m_currentReverseChunkSpeed.load(std::memory_order_relaxed);
		return (v > 0.0f) ? v : m_speedFactor.load(std::memory_order_relaxed);
	}
	void setReverbMix(float mix) override;
	void setMaxPlayTime(double seconds) override;
	void setReverse(bool on) override { m_reverse = on; }
	void setAutoNormalize(bool on) override { m_autoNormalize = on; }
	void setCancelToken(std::atomic<bool> *token) override { m_cancelToken = token; }
	bool isReverseFirstChunkReady() const override {
		if (!m_streamingReverse) return true;
		std::lock_guard<std::mutex> lg(m_chunkMutex);
		return !m_chunkQueue.empty()
		    || m_chunkWorkerDone.load(std::memory_order_acquire);
	}

private:
	int _close();
	void reset();
	int getAudioStreamNum() const;
	int buildFilterGraph(bool allowPitch = true);
	int _seek(double seconds); // Internal seek without locking (caller must hold m_mutex)

	typedef std::lock_guard<std::mutex> Lock;
private:
	const InputFileOptions m_inputFileOptions;
	const int m_outputChannels;
	const int m_outputSamplerate;      // always 48000
	const int64_t m_outputChannelLayout;

	AVFormatContext *m_fmtCtx;
	AVCodecContext *m_codecCtx;
	AVFilterGraph *m_filterGraph;
	AVFilterContext *m_bufSrcCtx;
	AVFilterContext *m_bufSinkCtx;

	int m_streamIndex;
	bool m_opened;
	std::atomic<bool> m_done;
	mutable std::mutex m_mutex;
	int64_t m_decodedSamples;
	int64_t m_convertedSamples;
	// Actual position in the original file (seconds). Written under
	// m_mutex; read lock-free by getPosition() from the audio thread -
	// atomic so the unsynchronised read is well-defined.
	std::atomic<double> m_filePosition{0.0};
	// File length cached once at the end of open() so getLength() never
	// needs m_mutex. The chunked-reverse worker holds m_mutex for tens
	// of ms per decode; the audio thread polls getLength() every cycle,
	// so a mutex there stalls the TS3 callback (reverse-mode stutter).
	std::atomic<double> m_cachedLengthSec{-1.0};
	// Atomic so setPitchFactor / setSpeedFactor can run on the GUI
	// thread WITHOUT acquiring m_mutex in streaming-reverse mode. The
	// chunk worker holds m_mutex for ~150 ms per decode; a blocking
	// slider drag on the GUI thread froze the UI behind the worker
	// (the user-reported "modifico velocemente lo slider e si blocca"
	// bug). The forward path still rebuilds the filter graph under
	// m_mutex via _seek; the streaming-reverse path just stamps the
	// new value here and lets the worker re-read it on its next chunk.
	std::atomic<float> m_pitchFactor{1.0f};
	std::atomic<float> m_speedFactor{1.0f};
	// Streaming-reverse: speed-factor the chunk currently being drained
	// was decoded with. Updated by readSamples whenever a new chunk
	// becomes the queue head. Sampler::fetchInputSamples reads this for
	// the reverse-cursor descent rate so it stays phase-locked to the
	// audible head even when m_speedFactor was just changed via the
	// FxPanel slider and no new chunk has been built yet. 0.0 = no
	// chunk has been read yet (worker still spinning up); callers
	// should fall back to m_speedFactor in that case.
	std::atomic<float> m_currentReverseChunkSpeed{0.0f};
	// Atomic so the reverse reader can pass it to freeverb.process()
	// without taking m_mutex - the previous design forced the reader to
	// acquire m_mutex on every audio cycle just to read this float,
	// which blocked behind the chunk worker's decodeChunkForward
	// (~100-200 ms mutex hold) and showed up as the "mini lag" the user
	// reported in reverse mode.
	std::atomic<float> m_reverbMix{0.0f};
	// Dedicated lock for the freeverb in-place processor + its mute()
	// flush. Keeps the reader's freeverb call off m_mutex (which is
	// held by the chunk worker for the full decode budget). Worker's
	// _seek path takes this briefly for mute().
	mutable std::mutex m_freeverbMutex;
	// Reverse playback toggle. Set BEFORE open(); when true open()
	// spawns the chunked streaming-reverse worker (see below). The old
	// whole-file pre-decode path was removed once streaming reverse
	// became the only mode.
	bool  m_reverse        = false;
	// LUFS auto-normalisation toggle. Set BEFORE open(); buildFilterGraph
	// appends a loudnorm filter targeting -16 LUFS integrated.
	bool  m_autoNormalize = false;
	// Cooperative cancel token set by the async setSlotReverse worker so
	// an in-flight open() can be asked to give up early. Lifetime owned
	// by caller (Sampler holds a shared_ptr<atomic<bool>> per slot).
	std::atomic<bool> *m_cancelToken = nullptr;

	// ===== STREAMING REVERSE (chunked seek-and-decode) =====
	// When set, open() spawns m_chunkWorker which decodes the file
	// backwards in small chunks starting AT the user's current play
	// position. First chunk is ready in ~50 ms regardless of file
	// length, so reverse playback starts immediately — no whole-file
	// pre-decode, no cursor drift, no client freeze.
	bool m_streamingReverse = false;
	struct ReverseChunk {
		std::vector<short> samples;   // already reversed; interleaved S16
		double startForward = 0.0;    // input-file time of FIRST sample (after reverse, this is the EARLIER end)
		double endForward   = 0.0;    // input-file time of LAST sample (after reverse, this is the LATER end)
		size_t readPos      = 0;      // shorts already consumed by readSamples
		float  speedAtBuild = 1.0f;   // atempo factor baked into the chunk
	};
	std::deque<ReverseChunk> m_chunkQueue;
	mutable std::mutex       m_chunkMutex;
	std::condition_variable  m_chunkCv;
	std::thread              m_chunkWorker;
	std::atomic<bool>        m_chunkWorkerStop{false};
	std::atomic<bool>        m_chunkWorkerDone{false};
	// True ONLY when the worker has reached the file's forward-time
	// lower bound (= reverse playback genuinely finished). The old
	// m_chunkWorkerDone flag was overloaded: it doubled as "worker
	// parked for any reason", including transient empty-queue parks
	// during a rapid pitch / speed slider drag. done() then latched
	// the slot to eSILENT the moment the audio thread observed the
	// transient state between two GUI flag updates. This dedicated
	// flag only flips at the legitimate file-start cursor, so a
	// drag-induced park never reads "playback finished" to done().
	std::atomic<bool>        m_chunkFileEnded{false};
	// Forward-time cursor the worker walks DOWN towards
	// m_streamingMinForward. Each chunk decoded covers
	// [cursor - CHUNK_SEC, cursor]; on success cursor is moved down.
	double m_chunkCursorForward   = 0.0;
	double m_streamingMinForward  = 0.0; // hard lower bound (cropStart)
	double m_streamingMaxForward  = 0.0; // ceiling for clamping seeks back up
	// Restart latch: setSpeed / setPitch / external seek flip this so
	// the worker discards the queue and resumes decoding chunks around
	// the new cursor with the current pitch / speed factors.
	std::atomic<bool> m_chunkRestartPending{false};
	// True → restart processor drops the ENTIRE queue and reseeks to
	// m_chunkRestartTarget (used by seek() — a real jump). False →
	// processor keeps queue.front() so the reader stays fed during
	// the rebuild, and anchors the cursor at front.startForward
	// (used by setPitchFactor / setSpeedFactor — same play position,
	// new factors).
	std::atomic<bool> m_chunkRestartFullWipe{false};
	double            m_chunkRestartTarget = 0.0;
	// Helpers (defined further down in this file).
	// Returns 0 on success, -1 on hard error. On return, `out` holds
	// the decoded interleaved S16 stereo samples and
	// `actualFirstInputSec` is the input-file time of the first
	// emitted sample (≠ startSec when avformat_seek_file lands
	// slightly off the requested keyframe boundary). The caller
	// re-anchors the chunk's claimed input-time range against this
	// value so reverse-playback cursor mapping stays bit-accurate to
	// the actual decoded audio.
	int  decodeChunkForward(double startSec, double endSec,
	                        std::vector<short> &out,
	                        double &actualFirstInputSec,
	                        float  &usedSpeedFactor);
	void chunkWorkerLoop();
	int  startStreamingReverse(double cursorForward,
	                           double minForward,
	                           double maxForward);
	void stopStreamingReverse();
	int m_abufferDeclaredRate;      // rate declared to abuffer (may differ from codec rate for pitch)
	// End-of-playback bound in INPUT-file seconds, compared against
	// m_filePosition (also input-time) so the bound stays correct no
	// matter how pitch / speed are adjusted during playback. 0.0 means
	// "unbounded". (The old OUTPUT-samples bound m_maxConvertedSamples
	// was removed: it desynced under any non-1.0 speedFactor.)
	double  m_maxFilePosition;
	// Forward-time lower bound. Reverse playback stops when
	// m_filePosition crosses below this; forward seeking past it does
	// not happen in practice (caller calls _seek with values >= this).
	double  m_minFilePosition = 0.0;
	int64_t m_nextSeekTimestamp;
	int64_t m_skipSamples;

	// Freeverb (Jezar) — processed in-place on output samples
	Freeverb m_freeverb;
};

//---------------------------------------------------------------
inline int64_t getChannelLayoutFromOptions(const InputFileOptions &options)
{
	switch(options.outputChannelLayout)
	{
	case InputFileOptions::MONO:
		return AV_CH_LAYOUT_MONO;
	case InputFileOptions::STEREO:
		return AV_CH_LAYOUT_STEREO;
	default:
		return AV_CH_LAYOUT_STEREO;
	}
}


//---------------------------------------------------------------
InputFileFFmpeg::InputFileFFmpeg(const InputFileOptions &options) :
	m_inputFileOptions(options),
	m_outputChannels(options.getNumChannels()),
	m_outputSamplerate(options.outputSampleRate),
	m_outputChannelLayout(getChannelLayoutFromOptions(options))
{
	reset();
}


//---------------------------------------------------------------
void InputFileFFmpeg::reset()
{
	m_fmtCtx = NULL;
	m_codecCtx = NULL;
	m_filterGraph = NULL;
	m_bufSrcCtx = NULL;
	m_bufSinkCtx = NULL;
	m_streamIndex = 0;
	m_opened = false;
	m_done = false;
	m_decodedSamples = 0;
	m_convertedSamples = 0;
	m_filePosition = 0.0;
	m_cachedLengthSec = -1.0;
	m_abufferDeclaredRate = 0;
	m_maxFilePosition = 0.0;
	m_nextSeekTimestamp = 0;
	m_skipSamples = 0;
	m_freeverb.init(m_outputSamplerate, m_outputChannels);
}


//---------------------------------------------------------------
InputFileFFmpeg::~InputFileFFmpeg()
{
	// Tear down the streaming-reverse worker BEFORE _close() so the
	// worker is fully joined before the FFmpeg context goes away
	// underneath it. _close() runs without the worker so it never has
	// to think about the chunk path.
	if (m_streamingReverse || m_chunkWorker.joinable())
		stopStreamingReverse();
	_close();
}


//---------------------------------------------------------------
int InputFileFFmpeg::buildFilterGraph(bool allowPitch)
{
	double pitch = m_pitchFactor.load(std::memory_order_relaxed);
	double speed = m_speedFactor.load(std::memory_order_relaxed);
	if (pitch < 0.01) pitch = 1.0;
	if (speed < 0.01) speed = 1.0;

	// Source of truth for the source PCM parameters is AVCodecParameters
	// on the stream, NOT m_codecCtx. For containers where the container
	// holds the parameters (M4A / MP4 / AAC) `avcodec_parameters_to_context`
	// has been observed to leave the destination codec context with
	// sample_rate=1 and an uninitialised ch_layout. The codecpar struct
	// is populated by avformat_find_stream_info and is correct - read
	// from there.
	AVCodecParameters *par = m_fmtCtx ? m_fmtCtx->streams[m_streamIndex]->codecpar : nullptr;
	int srcSampleRate = par ? par->sample_rate : 0;
	if (srcSampleRate <= 0 && m_codecCtx) srcSampleRate = m_codecCtx->sample_rate;
	enum AVSampleFormat srcFmt = (par && par->format != AV_SAMPLE_FMT_NONE)
	                           ? (enum AVSampleFormat)par->format
	                           : (m_codecCtx ? m_codecCtx->sample_fmt : AV_SAMPLE_FMT_S16);

	dbgLog("buildFilterGraph(allowPitch=%d) pitch=%.4f speed=%.4f reverb=%.3f codecSR=%d (par=%d) outSR=%d",
	       allowPitch, pitch, speed, m_reverbMix.load(std::memory_order_relaxed),
	       m_codecCtx ? m_codecCtx->sample_rate : 0,
	       par ? par->sample_rate : 0, m_outputSamplerate);

	if (m_filterGraph)
		avfilter_graph_free(&m_filterGraph);

	m_filterGraph = avfilter_graph_alloc();
	if (!m_filterGraph) { dbgLog("  FAILED: avfilter_graph_alloc returned NULL"); return -1; }

	char args[512];
	const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	dbgLog("  abuffersrc=%p abuffersink=%p", abuffersrc, abuffersink);
	if (!abuffersrc || !abuffersink) { dbgLog("  FAILED: filter not found!"); return -1; }

	const AVFilter *asetrate_check = avfilter_get_by_name("asetrate");
	const AVFilter *aresample_check = avfilter_get_by_name("aresample");
	dbgLog("  asetrate=%p aresample=%p", asetrate_check, aresample_check);

	bool usePitch = (pitch != 1.0) && allowPitch && aresample_check;
	bool useAsetrate = usePitch && asetrate_check;

	// Pitch via abuffer rate lie: declare the input sample rate as
	// codecRate*pitch so the downstream aresample resamples from the
	// "wrong" rate back to the real rate — identical to asetrate but
	// works even when asetrate is not compiled in.
	int declaredSR = srcSampleRate;
	if (usePitch && !useAsetrate) {
		declaredSR = (int)(srcSampleRate * pitch);
		dbgLog("  pitch via abuffer rate lie: declared=%d real=%d", declaredSR, srcSampleRate);
	}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    char ch_layout_str[128];
    // Prefer codecpar's ch_layout - it's authoritative. Fall back to
    // codec context only if codecpar's layout is uninitialised.
    AVChannelLayout *srcLayout = nullptr;
    if (par && par->ch_layout.nb_channels > 0 && par->ch_layout.nb_channels <= 64)
        srcLayout = &par->ch_layout;
    else if (m_codecCtx && m_codecCtx->ch_layout.nb_channels > 0 && m_codecCtx->ch_layout.nb_channels <= 64)
        srcLayout = &m_codecCtx->ch_layout;

    // Normalize AV_CHANNEL_ORDER_UNSPEC into a named layout. PCM/WAV
    // containers populate codecpar with nb_channels=2 but leave the
    // layout order UNSPEC; av_channel_layout_describe then emits
    // "2 channels" which abuffer accepts but downstream filters like
    // aformat=channel_layouts=stereo cannot match - the graph builds
    // fine and then drops every frame at runtime, so playback never
    // produces audible output. Resolve to a default mask layout based
    // on nb_channels so the rest of the chain sees a real label.
    AVChannelLayout normalized;
    bool useNormalized = false;
    if (srcLayout && srcLayout->order == AV_CHANNEL_ORDER_UNSPEC && srcLayout->nb_channels > 0) {
        av_channel_layout_default(&normalized, srcLayout->nb_channels);
        useNormalized = true;
        dbgLog("  normalized UNSPEC layout (nb=%d) to default mask", srcLayout->nb_channels);
    }
    AVChannelLayout *describeLayout = useNormalized ? &normalized : srcLayout;
    if (describeLayout) {
        av_channel_layout_describe(describeLayout, ch_layout_str, sizeof(ch_layout_str));
    } else {
        // Last resort: pretend stereo. The aformat filter later in the
        // chain forces stereo so the WRONG label here gets corrected.
        snprintf(ch_layout_str, sizeof(ch_layout_str), "stereo");
        dbgLog("  WARNING: no usable channel layout, defaulting to stereo");
    }
    snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
			m_fmtCtx->streams[m_streamIndex]->time_base.num,
			m_fmtCtx->streams[m_streamIndex]->time_base.den,
			declaredSR,
			av_get_sample_fmt_name(srcFmt),
			ch_layout_str);
#else
	uint64_t srcMask = (m_codecCtx && m_codecCtx->channel_layout)
	                 ? m_codecCtx->channel_layout
	                 : (uint64_t)AV_CH_LAYOUT_STEREO;
	snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
			m_fmtCtx->streams[m_streamIndex]->time_base.num,
			m_fmtCtx->streams[m_streamIndex]->time_base.den,
			declaredSR,
			av_get_sample_fmt_name(srcFmt),
			srcMask);
#endif

	m_abufferDeclaredRate = declaredSR;
	dbgLog("  abuffer args: %s", args);
	int ret = avfilter_graph_create_filter(&m_bufSrcCtx, abuffersrc, "in",
										args, NULL, m_filterGraph);
	dbgLog("  create_filter(abuffersrc) = %d", ret);
	if (ret < 0) {
		char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  FAILED create_filter(abuffersrc): %s", errbuf);
		// Old code returned ret without freeing the graph, leaking it
		// and leaving m_bufSrcCtx / m_bufSinkCtx in whatever state the
		// failed create call left them. After many heavy pitch/speed
		// rebuilds those leaks accumulated and made subsequent rebuild
		// attempts unreliable — the decoder thread's _seek then hit
		// repeated -1 returns and the worker hit its retry cap. Always
		// free + null the graph + contexts on the way out so the next
		// rebuild starts clean.
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;
		return ret;
	}

	ret = avfilter_graph_create_filter(&m_bufSinkCtx, abuffersink, "out",
										NULL, NULL, m_filterGraph);
	dbgLog("  create_filter(abuffersink) = %d", ret);
	if (ret < 0) {
		char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  FAILED create_filter(abuffersink): %s", errbuf);
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;
		return ret;
	}

	auto setBestEffort = [&](const char *strName, const char *strVal,
	                         const char *binName, const void *binVal, int binSz) {
		int r = -1;
		if (strName && strVal) {
			r = av_opt_set(m_bufSinkCtx, strName, strVal, AV_OPT_SEARCH_CHILDREN);
			dbgLog("  set %s (str) = %d", strName, r);
			if (r >= 0) return;
		}
		if (binName && binVal) {
			r = av_opt_set_bin(m_bufSinkCtx, binName, (const uint8_t*)binVal,
			                   binSz, AV_OPT_SEARCH_CHILDREN);
			dbgLog("  set %s (bin) = %d", binName, r);
		}
	};

	const enum AVSampleFormat out_fmts[] = { (enum AVSampleFormat)OUTPUT_FORMAT,
	                                          AV_SAMPLE_FMT_NONE };
	setBestEffort("sample_formats", "s16",
	              "sample_fmts", out_fmts, sizeof(out_fmts));

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
	AVChannelLayout out_ch_layout;
	av_channel_layout_from_mask(&out_ch_layout, m_outputChannelLayout);
	char out_ch_layout_str[128];
	av_channel_layout_describe(&out_ch_layout, out_ch_layout_str, sizeof(out_ch_layout_str));
	dbgLog("  out_ch_layout_str='%s' m_outputChannelLayout=0x%llx",
	       out_ch_layout_str, (long long)m_outputChannelLayout);
	int r_chl = av_opt_set(m_bufSinkCtx, "ch_layouts", out_ch_layout_str,
	                        AV_OPT_SEARCH_CHILDREN);
	dbgLog("  set ch_layouts = %d", r_chl);
	av_channel_layout_uninit(&out_ch_layout);
#else
	const int64_t out_layouts[] = { (int64_t)m_outputChannelLayout, -1 };
	setBestEffort(nullptr, nullptr,
	              "channel_layouts", out_layouts, sizeof(out_layouts));
#endif

	const int out_rates[] = { m_outputSamplerate, -1 };
	char rates_str[16];
	snprintf(rates_str, sizeof(rates_str), "%d", m_outputSamplerate);
	setBestEffort("sample_rates", rates_str,
	              "sample_rates", out_rates, sizeof(out_rates));

	auto fmtDbl = [](double v) -> std::string {
		char b[32];
		snprintf(b, sizeof(b), "%.4f", v);
		for (char *c = b; *c; c++) if (*c == ',') *c = '.';
		return b;
	};

	std::string filters;

	if (useAsetrate) {
		int newRate = (int)(srcSampleRate * pitch);
		dbgLog("  pitch via asetrate=%d, aresample=%d", newRate, srcSampleRate);
		filters += "asetrate=" + std::to_string(newRate) + ",aresample=" + std::to_string(srcSampleRate) + ",";
	} else if (usePitch) {
		dbgLog("  pitch via abuffer rate lie + aresample=%d", srcSampleRate);
		filters += "aresample=" + std::to_string(srcSampleRate) + ",";
	}

	// FFmpeg's atempo filter only accepts a factor in 0.5..2.0. Anything
	// outside must be decomposed into a chain of in-range atempo filters.
	// The old code clamped the upper bound at 100.0 (itself invalid), so
	// e.g. max speed (3.0) + min pitch (0.333) produced a single
	// atempo=9.0 — rejected by avfilter_graph_config(), crashing the plugin.
	double t = usePitch ? (speed / pitch) : speed;
	if (!(t > 0.0)) t = 1.0;   // guard against a degenerate / NaN factor
	while (t < 0.5) { filters += "atempo=0.5,"; t /= 0.5; }
	while (t > 2.0) { filters += "atempo=2.0,"; t /= 2.0; }
	if (t != 1.0) { filters += "atempo=" + fmtDbl(t) + ","; }

	// Reverse playback handled OUTSIDE the filter graph: areverse needs
	// EOF on its input which the streaming readSamples loop never
	// pushes (it reads one packet at a time and yields). Reverse mode
	// pre-decodes the whole file in open(), reverses the buffer, and
	// has readSamples replay from the buffer instead of the decoder.

	// LUFS auto-normalisation via EBU R128 loudnorm filter. Targets
	// -16 LUFS integrated, -1 dBTP true peak, 11 LU range. Single-pass
	// mode (slower convergence but no two-pass cost) - good enough for
	// SFX/short content, the dominant soundboard use-case.
	// SKIPPED in reverse mode: the streaming-reverse worker rebuilds the
	// filter graph for every 3 s chunk, so single-pass loudnorm restarts
	// its convergence at every chunk boundary - audible as loudness
	// pumping at the chunk cadence. Reverse playback therefore runs
	// without auto-normalisation.
	if (m_autoNormalize && !m_reverse) filters += "loudnorm=I=-16:TP=-1:LRA=11,";

	filters += "aformat=sample_fmts=s16:sample_rates=48000";

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    filters += ":channel_layouts=" + std::string(out_ch_layout_str);
#else
	char layout_hex[64];
	snprintf(layout_hex, sizeof(layout_hex), "0x%" PRIx64, m_outputChannelLayout);
	filters += ":channel_layouts=" + std::string(layout_hex);
#endif

	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = m_bufSrcCtx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = m_bufSinkCtx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	dbgLog("  filter chain: %s", filters.c_str());

	ret = avfilter_graph_parse_ptr(m_filterGraph, filters.c_str(),
									&inputs, &outputs, NULL);
	if (ret < 0) {
		char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  avfilter_graph_parse_ptr FAILED: %d (%s)", ret, errbuf);
	} else {
		dbgLog("  avfilter_graph_parse_ptr OK: %d", ret);
	}
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (ret < 0) {
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;

		if (allowPitch && pitch != 1.0) {
			dbgLog("  >>> RETRYING without asetrate (fallback to speed-only)");
			return buildFilterGraph(false);
		}
		return ret;
	}

	ret = avfilter_graph_config(m_filterGraph, NULL);
	if (ret < 0) {
		char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  avfilter_graph_config FAILED: %d (%s)", ret, errbuf);
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;

		if (allowPitch && pitch != 1.0) {
			dbgLog("  >>> RETRYING without asetrate (fallback to speed-only)");
			return buildFilterGraph(false);
		}
	} else {
		dbgLog("  avfilter_graph_config OK: %d", ret);
	}
	return ret;
}


//---------------------------------------------------------------
int InputFileFFmpeg::open(const char *filename, double startPosSeconds /*= 0.0*/, double playTimeSeconds /*= -1.0*/)
{
	Lock lock(m_mutex);

	dbgLog("open() called: file='%s' start=%.2f playTime=%.2f", filename, startPosSeconds, playTimeSeconds);

	if(m_opened)
	{
		dbgLog("  already opened, closing first");
		_close();
		reset();
	}

	{
		// Pre-flight diagnostics: existence + readability + size on disk.
		// Helps tell apart "file missing on this machine / VM" from
		// "FFmpeg internal failure" when avformat_open_input returns -2.
#ifdef _WIN32
		int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, NULL, 0);
		std::wstring wpath(wlen > 0 ? wlen - 1 : 0, L'\0');
		if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, filename, -1, &wpath[0], wlen);
		DWORD attrs = GetFileAttributesW(wpath.c_str());
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			dbgLog("  preflight: GetFileAttributesW FAILED, GetLastError=%lu", GetLastError());
		} else {
			HANDLE h = CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
			                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if (h == INVALID_HANDLE_VALUE) {
				dbgLog("  preflight: CreateFileW FAILED, GetLastError=%lu", GetLastError());
			} else {
				LARGE_INTEGER sz; sz.QuadPart = 0;
				GetFileSizeEx(h, &sz);
				dbgLog("  preflight: file exists, %lld bytes, attrs=0x%08lx",
				       (long long)sz.QuadPart, (unsigned long)attrs);
				CloseHandle(h);
			}
		}
#endif
	}

	int ret = avformat_open_input(&m_fmtCtx, filename, NULL, NULL);
	{
		char errbuf[256] = {0};
		av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  avformat_open_input returned %d (%s)", ret, errbuf);
	}
	if(LogFFmpegError(ret, "Cannot open file") != 0)
	{
		dbgLog("  FAILED to open file");
		return -1;
	}

	ret = avformat_find_stream_info(m_fmtCtx, NULL);
	dbgLog("  avformat_find_stream_info returned %d", ret);
	if(LogFFmpegError(ret, "Cannot find stream info") < 0)
	{
		dbgLog("  FAILED to find stream info");
		_close();
		return -1;
	}

	m_streamIndex = getAudioStreamNum();
	dbgLog("  audio stream index = %d", m_streamIndex);
	if(m_streamIndex < 0)
	{
		logError("Cannot find a suitable stream");
		dbgLog("  FAILED: no suitable audio stream");
		_close();
		return -1;
	}

	int codec_id = (int)m_fmtCtx->streams[m_streamIndex]->codecpar->codec_id;
	dbgLog("  codec_id = %d", codec_id);
	const AVCodec *codec = avcodec_find_decoder(m_fmtCtx->streams[m_streamIndex]->codecpar->codec_id);
	if(!codec)
	{
		logError("Unsupported codec");
		dbgLog("  FAILED: avcodec_find_decoder returned NULL for codec_id=%d", codec_id);
		_close();
		return -1;
	}
	dbgLog("  codec found: %s (%s)", codec->name, codec->long_name ? codec->long_name : "?");

	m_codecCtx = avcodec_alloc_context3(codec);
	if (!m_codecCtx) {
		logError("Cannot allocate codec context");
		dbgLog("  FAILED: avcodec_alloc_context3 returned NULL");
		_close();
		return -1;
	}
	{
		int parRet = avcodec_parameters_to_context(m_codecCtx, m_fmtCtx->streams[m_streamIndex]->codecpar);
		dbgLog("  avcodec_parameters_to_context returned %d", parRet);
		if (parRet < 0) {
			char errbuf[128]; av_strerror(parRet, errbuf, sizeof(errbuf));
			logError("avcodec_parameters_to_context: %s", errbuf);
			_close();
			return -1;
		}
	}

	// Pull authoritative source PCM parameters from codecpar (populated
	// by avformat_find_stream_info) rather than relying on m_codecCtx -
	// for M4A/AAC inputs we have observed parameters_to_context leaving
	// m_codecCtx->sample_rate / ch_layout uninitialised even though
	// codecpar holds the correct values. Force-copy back into m_codecCtx
	// before avcodec_open2 so the decoder sees sane state and reject the
	// file cleanly if BOTH sides are garbage (prevents downstream
	// avcodec_free_context segfaults inside the AAC decoder teardown).
	{
		AVCodecParameters *par = m_fmtCtx->streams[m_streamIndex]->codecpar;
		int parRate = par ? par->sample_rate : 0;
		if (parRate >= 1000 && parRate <= 384000)
			m_codecCtx->sample_rate = parRate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
		if (par && par->ch_layout.nb_channels >= 1 && par->ch_layout.nb_channels <= 64) {
			av_channel_layout_uninit(&m_codecCtx->ch_layout);
			av_channel_layout_copy(&m_codecCtx->ch_layout, &par->ch_layout);
		}
		int srcChannels = m_codecCtx->ch_layout.nb_channels;
#else
		if (par && par->channels >= 1 && par->channels <= 64)
			m_codecCtx->channels = par->channels;
		int srcChannels = m_codecCtx->channels;
#endif
		int srcRate = m_codecCtx->sample_rate;
		if (srcRate < 1000 || srcRate > 384000 || srcChannels < 1 || srcChannels > 64) {
			logError("Invalid codec parameters: rate=%d channels=%d", srcRate, srcChannels);
			dbgLog("  FAILED: bogus codec parameters rate=%d channels=%d (parRate=%d parCh=%d)",
			       srcRate, srcChannels, par ? par->sample_rate : 0,
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
			       par ? par->ch_layout.nb_channels : 0
#else
			       par ? par->channels : 0
#endif
			);
			_close();
			return -1;
		}
	}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    av_channel_layout_default(&m_codecCtx->ch_layout, m_codecCtx->ch_layout.nb_channels);
    dbgLog("  codec: channels=%d rate=%d fmt=%s", m_codecCtx->ch_layout.nb_channels, m_codecCtx->sample_rate, av_get_sample_fmt_name(m_codecCtx->sample_fmt));
#else
	m_codecCtx->channel_layout = av_get_default_channel_layout(m_codecCtx->channels);
	dbgLog("  codec: channels=%d rate=%d fmt=%s", m_codecCtx->channels, m_codecCtx->sample_rate, av_get_sample_fmt_name(m_codecCtx->sample_fmt));
#endif

	ret = avcodec_open2(m_codecCtx, codec, NULL);
	dbgLog("  avcodec_open2 returned %d", ret);
	if(LogFFmpegError(ret, "Cannot open codec") < 0)
	{
		dbgLog("  FAILED to open codec");
		_close();
		return -1;
	}

	// Post-open2 validation. open2 may pull parameters out of the codec's
	// extradata so values that were garbage in codecpar can become correct
	// here; conversely, if they STILL look wrong the decoder will fail
	// catastrophically deeper in the pipeline. Bail cleanly so the host
	// (TS3) never sees a libavcodec abort.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
	int postOpenChannels = m_codecCtx->ch_layout.nb_channels;
#else
	int postOpenChannels = m_codecCtx->channels;
#endif
	int postOpenRate = m_codecCtx->sample_rate;
	if (postOpenRate < 1000 || postOpenRate > 384000 ||
	    postOpenChannels < 1 || postOpenChannels > 64) {
		logError("Codec produced invalid parameters: rate=%d channels=%d",
		         postOpenRate, postOpenChannels);
		dbgLog("  FAILED: post-open2 codec parameters still invalid (rate=%d channels=%d) - aborting before filter graph",
		       postOpenRate, postOpenChannels);
		_close();
		return -1;
	}

	ret = buildFilterGraph();
	dbgLog("  buildFilterGraph returned %d", ret);
	if(LogFFmpegError(ret, "Cannot build filter graph") < 0)
	{
		dbgLog("  FAILED to build filter graph");
		_close();
		return -1;
	}

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
	logInfo("Opened file: %s; Codec: %s, Channels: %i, Rate: %i, Format: %s, Timebase: %i/%i, Sample-Estimation: %lld",
		filename, codec->long_name, m_codecCtx->ch_layout.nb_channels, m_codecCtx->sample_rate,
		av_get_sample_fmt_name(m_codecCtx->sample_fmt), m_fmtCtx->streams[m_streamIndex]->time_base.num, m_fmtCtx->streams[m_streamIndex]->time_base.den,
		(long long)outputSamplesEstimation());
#else
	logInfo("Opened file: %s; Codec: %s, Channels: %i, Rate: %i, Format: %s, Timebase: %i/%i, Sample-Estimation: %lld",
		filename, codec->long_name, m_codecCtx->channels, m_codecCtx->sample_rate,
		av_get_sample_fmt_name(m_codecCtx->sample_fmt), m_fmtCtx->streams[m_streamIndex]->time_base.num, m_fmtCtx->streams[m_streamIndex]->time_base.den,
		(long long)outputSamplesEstimation());
#endif

	m_opened = true;
	dbgLog("  file opened successfully, estimation=%lld samples", (long long)outputSamplesEstimation());

	// Cache the file length once so getLength() is lock-free from now
	// on (the audio thread polls it every cycle).
	m_cachedLengthSec.store(
		(double)outputSamplesEstimation() / (double)m_outputSamplerate,
		std::memory_order_relaxed);

	// Use internal _seek (no lock) — we already hold m_mutex from open()
	if(startPosSeconds > 0.0 && !m_reverse)
		_seek(startPosSeconds);

	if(playTimeSeconds > 0.0)
		m_maxFilePosition = startPosSeconds + playTimeSeconds;
	// Track the FORWARD-time lower bound. In forward mode it is the
	// cropStart (decoder seeks past anything below it). In reverse
	// mode it is the value m_filePosition must NOT cross going down -
	// reverse playback ends when we reach the cropStart (or 0).
	m_minFilePosition = startPosSeconds;

	if (m_reverse) {
		// Streaming reverse: spawn a worker that decodes BACKWARDS in
		// CHUNK_SEC chunks anchored at the current cursor. Returns
		// within ms (just sets up state + starts the thread) so the
		// caller is never blocked on a whole-file pre-decode.
		double cursorForward = (m_maxFilePosition > 0.0)
			? m_maxFilePosition
			: (double)outputSamplesEstimation() / (double)m_outputSamplerate;
		int sret = startStreamingReverse(cursorForward, m_minFilePosition,
		                                  cursorForward);
		if (sret < 0) {
			dbgLog("  startStreamingReverse failed");
			_close();
			return -1;
		}
		m_filePosition = cursorForward;
	}

	return 0;
}


//---------------------------------------------------------------
// Streaming reverse: chunked seek-and-decode.
//
// The worker walks DOWN m_chunkCursorForward by CHUNK_SEC at a time:
// each iteration seeks to [cursor - CHUNK_SEC, cursor], decodes those
// CHUNK_SEC seconds of input forward through the filter graph,
// reverses the resulting buffer in place, and pushes it onto
// m_chunkQueue. readSamples drains the queue head-first so reverse
// playback begins instantly at the user's click position and
// continues seamlessly while the worker fills earlier chunks.
//---------------------------------------------------------------
namespace {
// 3-second chunks balance latency and seam quality. Each chunk
// requires a backward FFmpeg seek + filter-graph rebuild + atempo
// state reset — at the previous 0.5 s budget that meant a rebuild
// every 500 ms of playback, and atempo's resampler restart left an
// audible click + brief silence at every seam. 3 s reduces seams
// to one per ~3 s of playback and ensures the worker's decode time
// (~80-200 ms per chunk) is comfortably below the chunk's own
// runtime, so the queue never drains mid-playback. First-chunk
// latency rises from ~50 ms to ~150 ms but the swap path already
// runs async + waits on isReverseFirstChunkReady() so the user
// click still feels instant.
constexpr double CHUNK_SEC          = 3.0;
// Cap how far ahead the worker decodes before parking. Bumped 3 → 5
// after user reports of an occasional ~100 ms silence during reverse
// playback. The old 3-chunk ceiling left the queue at 2/3 capacity
// during every backwards seek, and any extra CPU contention (heavy
// DSP slots, slow disk, m_mutex held by the reader for freeverb)
// pushed chunk decode past the reader's wait window — the reader
// then returned 0, the producer thread treated it as EOF, and
// cycled with a 100 ms nap before retrying. 5 chunks = 15 s of
// buffered reverse audio, peak heap per slot ~2.9 MB, and the
// worker still parks well below the moment the reader could
// observe an empty queue under normal load.
constexpr size_t MAX_QUEUE_CHUNKS   = 5;
}

// Decode the input-time range [startSec, endSec] FORWARD through the
// existing filter graph and append the resulting interleaved S16
// stereo samples to `out`. Caller holds m_mutex. Returns 0 on
// success, -1 on hard failure (seek / decoder error).
int InputFileFFmpeg::decodeChunkForward(double startSec, double endSec,
                                         std::vector<short> &out,
                                         double &actualFirstInputSec,
                                         float  &usedSpeedFactor)
{
	actualFirstInputSec = startSec; // sane default if the codec gives us no PTS
	usedSpeedFactor = 1.0f;
	// Only m_opened + m_fmtCtx are TRUE prerequisites. m_bufSrcCtx /
	// m_bufSinkCtx can legitimately be NULL on entry — buildFilterGraph
	// (called inside _seek a few lines below) NULLs them on its error
	// path, and a previous chunk hit during heavy pitch/speed drag is
	// allowed to leave us in that state. Gating decode on those pointers
	// here meant every subsequent chunk returned -1 immediately without
	// even trying _seek, accumulating consecutiveFail to the cap = slot
	// ended early as if the file had finished. _seek is the recovery
	// path; let it run.
	if (!m_opened || !m_fmtCtx) return -1;
	if (endSec <= startSec) return 0;

	// Internal _seek rebuilds the filter graph + flushes codec — same
	// path used by the existing live-seek code, so all filter chain
	// invariants (atempo, asetrate, aformat, loudnorm) stay in sync
	// with the user's current pitch / speed. _seek's avformat_seek_file
	// call lands on the nearest sync header / keyframe AT OR BEFORE
	// startSec for compressed containers (MP3, AAC, OGG); the actual
	// first decoded sample is therefore BEFORE our requested startSec
	// by a container-dependent amount. We track first-frame PTS below
	// and discard the pre-startSec head of the decoded stream so the
	// chunk's audio matches its claimed [startSec, endSec] range.
	if (_seek(startSec) < 0) return -1;
	AVRational tb = m_fmtCtx->streams[m_streamIndex]->time_base;
	int64_t targetTs = (int64_t)(startSec / av_q2d(tb));

	AVFrame *frame      = av_frame_alloc();
	AVFrame *filt_frame = av_frame_alloc();
	AVPacket *packet    = av_packet_alloc();
	if (!frame || !filt_frame || !packet) {
		if (frame)      av_frame_free(&frame);
		if (filt_frame) av_frame_free(&filt_frame);
		if (packet)     av_packet_free(&packet);
		return -1;
	}

	// Target sample count for the chunk:
	//   (endSec - startSec) seconds of INPUT * sampleRate / speedFactor
	// (atempo packs `speedFactor` seconds of input into 1 second of
	// output samples, so we get fewer output samples than the input
	// duration at speed > 1).
	float speedRaw = m_speedFactor.load(std::memory_order_relaxed);
	double speedF = (speedRaw > 0.0f) ? (double)speedRaw : 1.0;
	// Hand the actual speed-factor that drove the atempo filter back to
	// the caller. The worker previously captured speedAtBuild OUTSIDE
	// the lock before _seek, so a slider tick between that read and
	// THIS read raced — the chunk's claimed input-time range was
	// computed against the OLD factor while the audio was produced with
	// the NEW factor, breaking reverse-cursor math and pushing the
	// natural-end check to fire incorrectly when speed was decreased
	// many times. Stamping the in-decode value at the source removes
	// the race entirely.
	usedSpeedFactor = (float)speedF;
	int64_t targetOutSamples =
		(int64_t)((endSec - startSec) *
		          (double)m_outputSamplerate / speedF + 0.5);
	int channels = m_outputChannels > 0 ? m_outputChannels : 2;
	out.reserve(out.size() + (size_t)(targetOutSamples * channels));

	// First-frame PTS skip: the existing readSamples path uses
	// m_skipSamples / m_nextSeekTimestamp for this; we keep that
	// machinery for the forward-streaming branch but compute our own
	// skip locally so the chunk worker stays self-contained and
	// thread-isolated from the forward path.
	int64_t skipOutSamples = -1; // -1 = haven't computed yet
	int64_t collected = 0;
	// Input-time of the first sample we will actually emit. Set on
	// the first sample produced from a frame: either AT startSec
	// (when the codec landed before target and skip+collect lined up),
	// or AT first-frame PTS converted to seconds (when the codec
	// landed AFTER target — no skip possible, audio is from later in
	// the file). This is what chunkWorkerLoop uses to re-anchor the
	// chunk so cursor mapping reflects what was actually decoded.
	int64_t firstEmittedFramePts = AV_NOPTS_VALUE;
	int64_t firstEmittedFrameSkip = 0;
	bool    firstEmittedDone      = false;
	int64_t currentFramePts       = AV_NOPTS_VALUE;
	bool inputDone = false;
	auto drainSink = [&]() {
		while (av_buffersink_get_frame(m_bufSinkCtx, filt_frame) >= 0) {
			int outSamples = filt_frame->nb_samples;
			int origOutSamples = outSamples;
			short *outPtr  = (short*)filt_frame->extended_data[0];
			int skippedThisFrame = 0;
			if (skipOutSamples > 0) {
				skippedThisFrame = (int)std::min((int64_t)outSamples,
				                                  skipOutSamples);
				skipOutSamples -= skippedThisFrame;
				outSamples     -= skippedThisFrame;
				outPtr         += skippedThisFrame * channels;
			}
			if (outSamples > 0 && collected < targetOutSamples) {
				int64_t room = targetOutSamples - collected;
				if (outSamples > room) outSamples = (int)room;
				if (!firstEmittedDone) {
					// Record what frame and what offset produced the
					// FIRST emitted sample. We resolve the input-time
					// from frame PTS plus the in-frame OUTPUT offset
					// scaled back into input-time via speedF.
					firstEmittedFramePts  = currentFramePts;
					firstEmittedFrameSkip = skippedThisFrame;
					firstEmittedDone      = true;
				}
				size_t prev = out.size();
				out.resize(prev + outSamples * channels);
				std::memcpy(out.data() + prev, outPtr,
				            outSamples * channels * sizeof(short));
				collected += outSamples;
			}
			(void)origOutSamples;
			av_frame_unref(filt_frame);
		}
	};

	while (collected < targetOutSamples && !inputDone) {
		int rret = av_read_frame(m_fmtCtx, packet);
		if (rret < 0) { inputDone = true; break; }
		if (packet->stream_index == m_streamIndex) {
			if (avcodec_send_packet(m_codecCtx, packet) == 0) {
				while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
					int64_t curTs = frame->best_effort_timestamp;
					if (curTs == AV_NOPTS_VALUE) curTs = frame->pts;
					currentFramePts = curTs;
					if (skipOutSamples < 0) {
						// First decoded frame after seek: figure
						// out how much the seek overshot backward
						// (curTs < targetTs) and discard that much
						// output. tsToSkip is in input-time-base
						// units; output-domain samples to drop is
						// `seconds * sampleRate / speedF` because
						// atempo packs speedF input-seconds into 1
						// output-second.
						int64_t tsToSkip = (curTs != AV_NOPTS_VALUE)
							? targetTs - curTs : 0;
						if (tsToSkip > 0) {
							double secondsToSkip = tsToSkip * av_q2d(tb);
							skipOutSamples = (int64_t)(
								secondsToSkip *
								(double)m_outputSamplerate / speedF + 0.5);
						} else {
							skipOutSamples = 0;
						}
					}
					if (m_abufferDeclaredRate > 0)
						frame->sample_rate = m_abufferDeclaredRate;
					av_buffersrc_add_frame_flags(m_bufSrcCtx, frame,
					                              AV_BUFFERSRC_FLAG_KEEP_REF);
					drainSink();
					av_frame_unref(frame);
					if (collected >= targetOutSamples) break;
				}
			}
		}
		av_packet_unref(packet);
	}
	// Flush whatever is still pending in the filter graph if the
	// reader hit EOF before we collected enough samples (tail chunk
	// near file end).
	if (collected < targetOutSamples) {
		avcodec_send_packet(m_codecCtx, NULL);
		while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
			av_buffersrc_add_frame_flags(m_bufSrcCtx, frame,
			                              AV_BUFFERSRC_FLAG_KEEP_REF);
			drainSink();
			av_frame_unref(frame);
			if (collected >= targetOutSamples) break;
		}
		av_buffersrc_add_frame_flags(m_bufSrcCtx, NULL, 0);
		drainSink();
	}

	// Resolve the input-time of the first emitted sample.
	// frame-PTS resolution gives us the input-time of the head of the
	// frame; we then add the in-frame OUTPUT-domain skip offset back
	// in seconds via skip * speedF / sampleRate (atempo packs speedF
	// seconds of input into 1 second of output). When the codec gave
	// no PTS at all (rare), fall back to startSec — caller's
	// re-anchor against actualInputSec then still keeps the chunk
	// internally consistent.
	if (firstEmittedDone && firstEmittedFramePts != AV_NOPTS_VALUE) {
		actualFirstInputSec = firstEmittedFramePts * av_q2d(tb)
		    + (double)firstEmittedFrameSkip * speedF
		      / (double)m_outputSamplerate;
	}

	av_frame_free(&frame);
	av_frame_free(&filt_frame);
	av_packet_free(&packet);
	return 0;
}

// Set up worker state and spawn the decoder thread.
int InputFileFFmpeg::startStreamingReverse(double cursorForward,
                                            double minForward,
                                            double maxForward)
{
	if (cursorForward <= minForward) {
		// Cursor already at the lower bound — nothing to reverse.
		// Set BOTH the parked flag AND the natural-end flag so done()
		// reports true to the audio thread without ambiguity.
		m_streamingReverse     = true;
		m_chunkFileEnded.store(true);
		m_chunkWorkerDone.store(true);
		return 0;
	}
	m_streamingReverse        = true;
	m_chunkCursorForward      = cursorForward;
	m_streamingMinForward     = minForward;
	m_streamingMaxForward     = (maxForward > 0.0) ? maxForward : cursorForward;
	m_chunkWorkerStop.store(false);
	m_chunkWorkerDone.store(false);
	// Reset the chunk-speed cache so Sampler::fetchInputSamples falls
	// back to m_speedFactor until the worker has actually published the
	// speedAtBuild of the first chunk it pushes.
	m_currentReverseChunkSpeed.store(0.0f, std::memory_order_relaxed);
	// Clear any latched end / restart flags from a previous session.
	// stopStreamingReverse leaves m_chunkFileEnded / m_chunkRestartPending
	// at their final values; a subsequent fresh start without explicit
	// reset would carry m_chunkFileEnded=true into the new session and
	// make done() report "finished" before the first chunk lands.
	m_chunkFileEnded.store(false);
	m_chunkRestartPending.store(false);
	m_chunkRestartFullWipe.store(false);
	m_chunkWorker = std::thread(&InputFileFFmpeg::chunkWorkerLoop, this);
	return 0;
}

// Tear down the worker. Caller MUST NOT hold m_mutex (worker grabs
// it itself between iterations). Safe to call repeatedly.
void InputFileFFmpeg::stopStreamingReverse()
{
	m_chunkWorkerStop.store(true);
	{
		std::lock_guard<std::mutex> lg(m_chunkMutex);
		m_chunkCv.notify_all();
	}
	if (m_chunkWorker.joinable())
		m_chunkWorker.join();
	{
		std::lock_guard<std::mutex> lg(m_chunkMutex);
		m_chunkQueue.clear();
	}
	m_chunkWorkerDone.store(true);
	m_streamingReverse = false;
}

void InputFileFFmpeg::chunkWorkerLoop()
{
	int consecutiveEmpty = 0;
	// Hard-failure counter: bumped on every decode error / empty
	// chunk that is NOT followed by a successful decode. Used to cap
	// retry attempts when a truly broken file produces nothing back-
	// to-back. Reset to 0 on every successful chunk push.
	int consecutiveFail  = 0;
	// Cap: 60 retries * ~150 ms retry budget = ~9 s of automatic
	// retry before we PARK the worker (no longer latches done = true).
	// Heavy pitch/speed dragging can leave the codec in a flaky state
	// for several seconds after the drag ends, before buildFilterGraph
	// + avformat_seek_file recover. The OLD cap of 20 = 3 s, combined
	// with the LATCHED m_chunkFileEnded = true on cap reach, was the
	// long-standing "reverse mode acts as if audio ended after lots of
	// speed/pitch changes" bug: the codec needed a beat to settle, the
	// cap fired first, m_chunkFileEnded latched, and the slot was
	// permanently terminated. Park-on-cap (see kMaxConsecutiveFail
	// handler below) keeps the worker alive so a subsequent restart
	// (the user nudges any control, or simply playback's next loop
	// boundary) revives it. Real broken files still terminate cleanly
	// via the natural-end branch (cursor <= minF).
	constexpr int kMaxConsecutiveFail = 60;
	// Track first-chunk-in-session for the leading-silence trim. The
	// FIRST chunk after worker spawn / restart is the one anchored at
	// the user's "start of reverse playback" cursor. For containers
	// with encoder padding at file end (MP3 / AAC) or content that
	// fades to silence, those trailing forward samples become LEADING
	// silence after reverse — the user-reported "first seconds of
	// reverse playback are silent" bug. We trim them only on the
	// first chunk so middle-of-file silences stay intact in reverse.
	bool firstChunkInSession = true;
	while (!m_chunkWorkerStop.load(std::memory_order_relaxed)) {
		// Restart latch: setPitch / setSpeed / external seek flip
		// m_chunkRestartPending so the queue is cleared and the
		// cursor is moved to the new target before the next
		// iteration. Lets pitch / speed live-tuning take effect
		// without losing the current play position.
		if (m_chunkRestartPending.exchange(false,
		                                    std::memory_order_relaxed)) {
			std::lock_guard<std::mutex> lg(m_chunkMutex);
			// Two restart sources:
			//   1) seek() — sets m_chunkRestartFullWipe = true so the
			//      ENTIRE queue is dropped and the cursor jumps to the
			//      user-requested input-time (m_chunkRestartTarget).
			//   2) setPitchFactor / setSpeedFactor — leaves the wipe
			//      flag false. We KEEP queue.front() (the chunk the
			//      reader is currently draining) so audio stays
			//      continuous while we rebuild, and anchor the cursor
			//      at the EARLIER edge of that chunk so the next chunk
			//      decoded picks up exactly where the front chunk ends
			//      (= no gap, no overlap). If the queue happens to be
			//      empty we fall back to the live m_filePosition.
			//
			// Old design cleared the queue from the GUI thread on
			// every setPitch / setSpeed call. A 30-events-per-second
			// drag burst then wiped the queue faster than the worker's
			// ~150 ms decode could refill, starving the reader and
			// freezing audio for the duration of the drag. Pushing the
			// clear into the worker + retaining the front chunk costs
			// at most ~1.5 s of "old pitch" audio but guarantees the
			// reader always has something to feed sbPlayback.
			bool fullWipe = m_chunkRestartFullWipe.exchange(false,
			                  std::memory_order_relaxed);
			if (fullWipe) {
				m_chunkQueue.clear();
				m_chunkCursorForward = m_chunkRestartTarget;
			} else {
				if (m_chunkQueue.size() > 1) {
					// Drop everything past the front. Reader holds
					// m_chunkMutex while it touches queue.front(),
					// so by the time we get here the reader is
					// either between calls or waiting on m_chunkCv.
					m_chunkQueue.resize(1);
				}
				if (!m_chunkQueue.empty()) {
					// Latency control: the previous design kept the
					// ENTIRE front chunk so audio stayed continuous,
					// but at CHUNK_SEC=3 s the user heard up to ~3 s
					// of old-factor audio before the new factors
					// landed. Truncate the front chunk to a small
					// safety window past readPos (~150 ms of output
					// time) so the new factors take effect almost
					// immediately while the reader still has enough
					// to drink while the worker rebuilds.
					ReverseChunk &front = m_chunkQueue.front();
					int channels = m_outputChannels > 0
					    ? m_outputChannels : 2;
					constexpr double kLookAheadSec = 0.15;
					size_t lookAheadShorts =
					    (size_t)(kLookAheadSec
					             * (double)m_outputSamplerate
					             * (double)channels);
					size_t maxSize = front.readPos + lookAheadShorts;
					if (front.samples.size() > maxSize
					    && maxSize > front.readPos) {
						// Re-anchor startForward to the input-time
						// of the new last playable sample. Mapping
						// (see readSamples): forward time at index i
						// = endForward - range * (i / oldSize). New
						// final sample is at index maxSize-1, so
						// new startForward = endForward - range *
						// (maxSize / oldSize).
						double range = front.endForward
						             - front.startForward;
						double oldSize = (double)front.samples.size();
						double frac = (double)maxSize / oldSize;
						if (frac > 1.0) frac = 1.0;
						front.startForward =
						    front.endForward - range * frac;
						front.samples.resize(maxSize);
					}
					m_chunkCursorForward = front.startForward;
				} else {
					m_chunkCursorForward =
						m_filePosition.load(
							std::memory_order_relaxed);
				}
			}
			m_chunkCv.notify_all();
			// Restart is treated as a fresh session for the trim: a
			// seek to file end / cropEnd should also benefit from
			// trailing-silence trimming, otherwise the same silent
			// intro happens on loop restart and on every reverse
			// engagement.
			firstChunkInSession = true;
			// CRITICAL: reset retry counters on every restart. A
			// continuous pitch / speed slider drag fires a restart
			// per event (10-30 per second). If the codec produces a
			// partial / empty chunk during ANY of those rebuilds the
			// previous policy incremented consecutiveFail forever
			// across restarts and hit the 20-failure cap mid-drag,
			// latching done = true and ending playback as if the
			// file finished. Each user-driven restart is the
			// equivalent of a fresh playback attempt, so the
			// counters belong to that attempt only.
			consecutiveFail  = 0;
			consecutiveEmpty = 0;
		}

		double cursor;
		double minF;
		{
			std::lock_guard<std::mutex> lg(m_chunkMutex);
			cursor = m_chunkCursorForward;
			minF   = m_streamingMinForward;
		}
		if (cursor <= minF) {
			// Reached file start in input-time. LEGITIMATE end of
			// reverse playback: flip the natural-end flag so done()
			// returns true once the reader drains the remaining
			// queue. Then park so a subsequent restart (loop / seek)
			// can revive the worker.
			m_chunkFileEnded.store(true, std::memory_order_release);
			m_chunkWorkerDone.store(true, std::memory_order_release);
			std::unique_lock<std::mutex> lk(m_chunkMutex);
			m_chunkCv.notify_all();
			m_chunkCv.wait(lk, [&] {
				return m_chunkWorkerStop.load(std::memory_order_relaxed)
				    || m_chunkRestartPending.load(std::memory_order_relaxed);
			});
			if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
			m_chunkWorkerDone.store(false, std::memory_order_release);
			m_chunkFileEnded.store(false, std::memory_order_release);
			consecutiveEmpty = 0;
			consecutiveFail  = 0;
			continue;
		}

		// Backpressure: park until the reader has consumed enough
		// chunks to make room, or until a cancel / restart fires.
		{
			std::unique_lock<std::mutex> lk(m_chunkMutex);
			m_chunkCv.wait(lk, [&] {
				return m_chunkQueue.size() < MAX_QUEUE_CHUNKS
				    || m_chunkWorkerStop.load(std::memory_order_relaxed)
				    || m_chunkRestartPending.load(std::memory_order_relaxed);
			});
			if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
			if (m_chunkRestartPending.load(std::memory_order_relaxed))
				continue;
		}

		double chunkEnd   = cursor;
		double chunkStart = (chunkEnd - CHUNK_SEC > minF)
			? chunkEnd - CHUNK_SEC : minF;

		// Decode the chunk under m_mutex (FFmpeg context is not
		// thread-safe). The lock-hold is bounded by CHUNK_SEC of
		// audio decode (~10-50 ms on typical hardware) so GUI
		// thread setters (setPitch / setSpeed / setReverbMix)
		// observe at most one chunk's worth of latency.
		ReverseChunk chk;
		chk.startForward = chunkStart;
		chk.endForward   = chunkEnd;
		// speedAtBuild is the speed factor the atempo filter was driven
		// with INSIDE decodeChunkForward — captured at the same lock-free
		// load that targetOutSamples is computed from, NOT a pre-decode
		// read out here that could race with the user's slider tick.
		// Old design read m_speedFactor here, then again inside decode,
		// and a slider change between the two reads produced a chunk
		// labeled with one speed but whose audio was decoded at another.
		// Cursor / position math then drifted by the inter-read speed
		// delta — magnified at extreme slow speeds where the delta is
		// large fraction of the factor.
		chk.speedAtBuild = 1.0f;
		float usedSpeed   = 1.0f;
		double actualFirstInputSec = chunkStart;
		int dret = -1;
		{
			Lock lock(m_mutex);
			if (m_chunkWorkerStop.load(std::memory_order_relaxed))
				break;
			dret = decodeChunkForward(chunkStart, chunkEnd, chk.samples,
			                          actualFirstInputSec, usedSpeed);
		}
		chk.speedAtBuild = usedSpeed;
		if (dret < 0) {
			// Hard decoder error. Most commonly transient: a rapid
			// pitch / speed slider drag rebuilds the filter graph
			// repeatedly and the codec sometimes returns -1 from
			// _seek for the first chunk decoded with the new factors.
			// Retry with a short timed wait, NOT a done-latch park.
			// Latching m_chunkFileEnded = true here would prematurely
			// end the slot. Only after kMaxConsecutiveFail back-to-
			// back failures with NO intervening restart do we accept
			// the file as truly unplayable (the restart branch resets
			// consecutiveFail so a user-driven drag never triggers
			// the unplayable cap).
			++consecutiveFail;
			if (consecutiveFail >= kMaxConsecutiveFail) {
				// THROTTLED auto-retry. Codec / filter graph likely
				// flaky after a long pitch / speed drag — settle for
				// 1 second then try again. Do NOT latch m_chunkFileEnded
				// (the file is not over), and do NOT set m_chunkWorkerDone
				// (the worker IS still active, just paced down). An
				// earlier "park forever until external restart" design
				// left the slot silent after a heavy drag because no
				// further user input was guaranteed to fire and revive
				// the worker — audio just died, indistinguishable from
				// a true end. Auto-retry guarantees recovery within
				// ~1 second of the codec settling, with no help needed
				// from the user.
				std::unique_lock<std::mutex> lk(m_chunkMutex);
				m_chunkCv.wait_for(lk, std::chrono::milliseconds(1000),
				                    [&] {
					return m_chunkWorkerStop.load(std::memory_order_relaxed)
					    || m_chunkRestartPending.load(std::memory_order_relaxed);
				});
				if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
				consecutiveFail  = 0;
				consecutiveEmpty = 0;
				continue;
			}
			std::unique_lock<std::mutex> lk(m_chunkMutex);
			m_chunkCv.wait_for(lk, std::chrono::milliseconds(150), [&] {
				return m_chunkWorkerStop.load(std::memory_order_relaxed)
				    || m_chunkRestartPending.load(std::memory_order_relaxed);
			});
			if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
			continue;
		}

		// Skip empty chunks — happens when seek lands past EOF, or
		// the requested range is shorter than the codec frame size.
		// We advance the cursor to chunkStart so the next iteration
		// tries earlier ground; but if we keep coming back empty
		// (broken codec state, malformed container, perpetual seek
		// failure) advancing the cursor unchecked would zoom it
		// SECONDS down the file in a few iterations and the GUI
		// would visualise a seconds-backward cursor skip. Cap the
		// consecutive empty count at 3 (1.5 s of attempted span);
		// past that, mark the worker done so the reader cleanly
		// terminates playback instead of silently chewing through
		// the file.
		if (chk.samples.empty()) {
			++consecutiveEmpty;
			++consecutiveFail;
			std::unique_lock<std::mutex> lk(m_chunkMutex);
			// chunkStart <= minF = genuinely reached file start = end
			// of reverse playback. Park with naturalEnd (legitimate
			// end of stream).
			if (chunkStart <= minF) {
				m_chunkFileEnded.store(true, std::memory_order_release);
				m_chunkWorkerDone.store(true, std::memory_order_release);
				m_chunkCv.notify_all();
				m_chunkCv.wait(lk, [&] {
					return m_chunkWorkerStop.load(std::memory_order_relaxed)
					    || m_chunkRestartPending.load(std::memory_order_relaxed);
				});
				if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
				m_chunkWorkerDone.store(false, std::memory_order_release);
				m_chunkFileEnded.store(false, std::memory_order_release);
				consecutiveEmpty = 0;
				consecutiveFail  = 0;
				continue;
			}
			// Mid-file empty chunk = codec / seek hiccup. Transient,
			// DON'T flip naturalEnd. Retry until kMaxConsecutiveFail
			// without intervening restart (the restart branch resets
			// the counter, so a user-driven slider drag never hits
			// the cap). On cap reach, throttled retry (see dret < 0
			// branch above for full rationale).
			if (consecutiveFail >= kMaxConsecutiveFail) {
				m_chunkCv.wait_for(lk, std::chrono::milliseconds(1000),
				                    [&] {
					return m_chunkWorkerStop.load(std::memory_order_relaxed)
					    || m_chunkRestartPending.load(std::memory_order_relaxed);
				});
				if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
				consecutiveFail  = 0;
				consecutiveEmpty = 0;
				continue;
			}
			m_chunkCursorForward = chunkStart;
			m_chunkCv.wait_for(lk, std::chrono::milliseconds(50), [&] {
				return m_chunkWorkerStop.load(std::memory_order_relaxed)
				    || m_chunkRestartPending.load(std::memory_order_relaxed);
			});
			if (m_chunkWorkerStop.load(std::memory_order_relaxed)) break;
			continue;
		}
		consecutiveEmpty = 0;
		consecutiveFail  = 0;

		// Re-anchor the chunk's claimed input-time range against the
		// ACTUAL decoded audio. Two things can drift from the
		// requested range:
		//   1. avformat_seek_file landed slightly off the requested
		//      keyframe → first emitted sample's input-time is
		//      actualFirstInputSec, NOT chunkStart.
		//   2. Decoder produced fewer samples than expected (EOF,
		//      short tail chunk) → actualInputSec < requested.
		// Both make the linear position mapping in readSamples
		// (chunk.endForward → chunk.startForward across readPos /
		// samples.size()) misreport the time-per-sample. The result
		// the user sees is the cursor + audio skipping whole seconds
		// backward at every chunk boundary because the next chunk's
		// claimed endForward sits well below where the previous
		// chunk's startForward actually played out.
		int channels = m_outputChannels > 0 ? m_outputChannels : 2;
		int64_t totalSamples = (int64_t)chk.samples.size() / channels;
		double actualInputSec =
			(double)totalSamples * chk.speedAtBuild
			/ (double)m_outputSamplerate;
		chk.startForward = actualFirstInputSec;
		chk.endForward   = actualFirstInputSec + actualInputSec;

		// First-chunk leading-silence trim (reverse playback). The
		// forward-time TAIL of this chunk is what reverse playback
		// emits FIRST. Encoder padding (MP3 / AAC) or content fade-
		// outs land there as near-zero samples → silent intro for
		// reverse. Trim trailing near-silent samples FORWARD so the
		// reversed chunk's first samples are real audio.
		//
		// Only applies to the first chunk in this streaming session
		// (open / seek / loop restart) to avoid clipping audible-but-
		// quiet sections mid-file. Capped at 1.5 s to bound the worst
		// case (a song with a very long fade tail) — we'd rather
		// accept some leading silence there than risk eating actual
		// audible content.
		if (firstChunkInSession && totalSamples > 0) {
			constexpr int16_t kSilenceFloor = 32;          // ~-60 dBFS
			constexpr double  kMaxTrimSec   = 1.5;
			int64_t minKeep = totalSamples
				- (int64_t)(kMaxTrimSec
				            * (double)m_outputSamplerate
				            / std::max(0.5, (double)chk.speedAtBuild));
			if (minKeep < 0) minKeep = 0;
			int64_t lastAudible = totalSamples - 1;
			while (lastAudible >= minKeep) {
				bool silent = true;
				for (int c = 0; c < channels; ++c) {
					if (std::abs(chk.samples[lastAudible * channels + c])
					    > kSilenceFloor) {
						silent = false;
						break;
					}
				}
				if (!silent) break;
				--lastAudible;
			}
			int64_t newTotal = lastAudible + 1;
			if (newTotal > 0 && newTotal < totalSamples) {
				chk.samples.resize((size_t)(newTotal * channels));
				double newSec = (double)newTotal * chk.speedAtBuild
				              / (double)m_outputSamplerate;
				chk.endForward = chk.startForward + newSec;
				totalSamples = newTotal;
			}
			firstChunkInSession = false;
		}

		// In-place reverse. Stereo fast path = 32-bit pair swap.
		if (channels == 2 && totalSamples > 1) {
			uint32_t *p32 = reinterpret_cast<uint32_t*>(chk.samples.data());
			for (int64_t i = 0, j = totalSamples - 1; i < j; ++i, --j)
				std::swap(p32[i], p32[j]);
		} else {
			for (int64_t i = 0, j = totalSamples - 1; i < j; ++i, --j) {
				for (int c = 0; c < channels; ++c) {
					std::swap(chk.samples[i * channels + c],
					          chk.samples[j * channels + c]);
				}
			}
		}

		{
			std::lock_guard<std::mutex> lg(m_chunkMutex);
			// Drop the chunk on the floor if a restart fired during
			// decode — its samples were built with stale speed /
			// pitch factors and the queue is about to be wiped.
			if (m_chunkRestartPending.load(std::memory_order_relaxed))
				continue;
			m_chunkQueue.push_back(std::move(chk));
			// Advance the worker cursor to the chunk's ACTUAL lower
			// bound so the NEXT chunk picks up exactly where this
			// one ends — no overlap (= same audio twice = audible
			// stutter and apparent cursor jump backward) and no gap
			// (= missing audio between chunks).
			m_chunkCursorForward = m_chunkQueue.back().startForward;
			m_chunkCv.notify_one();
		}
	}
	m_chunkWorkerDone.store(true, std::memory_order_release);
	std::lock_guard<std::mutex> lg(m_chunkMutex);
	m_chunkCv.notify_all();
}


//---------------------------------------------------------------
// Public seek: acquires m_mutex then delegates to _seek.
//---------------------------------------------------------------
int InputFileFFmpeg::seek( double seconds )
{
	if (m_streamingReverse) {
		// Lock-free seek: NO m_mutex acquire from the GUI thread. The
		// worker handles the actual decoder reseek on its next chunk
		// (it picks up m_chunkRestartTarget at the top of its loop).
		// Acquiring m_mutex here would block the GUI for the worker's
		// in-flight decode (~150 ms) and make a waveform click feel
		// laggy.
		//
		// A real seek IS a wipe — user wants to jump to a new spot,
		// not continue from where the head was. Set the full-wipe
		// flag so the worker's restart branch drops the queue and
		// repositions cursor to `seconds`.
		//
		// Loop+reverse infinite-spin fix: also clear m_chunkFileEnded.
		// done() reads that flag directly — if it stays latched from
		// the natural-end branch, the GUI / sampler observe done()=true
		// for the few ms between this seek call and the worker actually
		// reaching its restart branch (where the flag gets cleared).
		// In that window Sampler::fetchInputSamples's loop branch fires
		// AGAIN, requests another seek, and the slot enters a tight
		// CPU loop until the worker finally races ahead.
		m_filePosition.store(seconds, std::memory_order_relaxed);
		m_done.store(false, std::memory_order_release);
		m_chunkFileEnded.store(false, std::memory_order_release);
		{
			std::lock_guard<std::mutex> lg(m_chunkMutex);
			m_chunkRestartTarget = seconds;
			m_chunkRestartFullWipe.store(true,
			                              std::memory_order_release);
			m_chunkRestartPending.store(true,
			                              std::memory_order_release);
		}
		m_chunkCv.notify_all();
		return 0;
	}
	Lock lock(m_mutex);
	return _seek(seconds);
}


//---------------------------------------------------------------
// Internal seek: caller MUST already hold m_mutex.
// Separated from seek() to avoid deadlock when called from open(),
// which also holds m_mutex.
//---------------------------------------------------------------
int InputFileFFmpeg::_seek( double seconds )
{
	if (!m_opened || !m_fmtCtx)
		return -1;

	AVRational time_base = m_fmtCtx->streams[m_streamIndex]->time_base;
	int64_t ts = (int64_t)(seconds / time_base.num * time_base.den);
	if(LogFFmpegError(avformat_seek_file(m_fmtCtx, m_streamIndex, INT64_MIN, ts, ts, 0), "Seeking failed") < 0)
		return -1;
	avcodec_flush_buffers(m_codecCtx);

	// Rebuild graph to flush filter buffers
	int graphRet = buildFilterGraph();
	dbgLog("  _seek: buildFilterGraph returned %d (src=%p sink=%p)", graphRet, m_bufSrcCtx, m_bufSinkCtx);
	if (graphRet < 0) {
		dbgLog("  _seek: CRITICAL - filter graph rebuild failed, audio will stall!");
		return -1;
	}
	{
		std::lock_guard<std::mutex> fl(m_freeverbMutex);
		m_freeverb.mute();
	}

	m_nextSeekTimestamp = ts;
	m_skipSamples = 0;
	m_convertedSamples = (int64_t)(seconds * (double)m_outputSamplerate);
	m_filePosition = seconds;
	m_done = false;
	return 0;
}


//---------------------------------------------------------------
double InputFileFFmpeg::getPosition() const
{
	return m_filePosition;
}


//---------------------------------------------------------------
double InputFileFFmpeg::getLength() const
{
	// Lock-free fast path: open() caches the length once (it never
	// changes for an opened file). Taking m_mutex here stalled the
	// audio thread behind the chunked-reverse worker's per-chunk
	// decode (which holds m_mutex for tens of ms).
	double cached = m_cachedLengthSec.load(std::memory_order_relaxed);
	if (cached >= 0.0)
		return cached;
	// Pre-open fallback (rare): compute under the lock.
	Lock lock(m_mutex);
	return (double)outputSamplesEstimation() / (double)m_outputSamplerate;
}


//---------------------------------------------------------------
int InputFileFFmpeg::close()
{
	// Stop the streaming-reverse worker OUTSIDE m_mutex: the worker
	// grabs m_mutex itself for each chunk decode, so joining it
	// while holding m_mutex would deadlock.
	if (m_streamingReverse || m_chunkWorker.joinable())
		stopStreamingReverse();
	Lock lock(m_mutex);
	return _close();
}


//---------------------------------------------------------------
int InputFileFFmpeg::readSamples(SampleProducer *sampleBuffer)
{
	if (m_streamingReverse) {
		// Drain the chunk queue head-first. Front chunk holds the
		// LATEST forward-time samples (the ones we want to play
		// first when reversing). When a chunk is exhausted, pop it
		// so the next iteration starts on the next-earlier chunk.
		std::unique_lock<std::mutex> lk(m_chunkMutex);
		// Wait window before reporting back. Old value (40 ms) was
		// shorter than the worker's typical chunk decode (~80-150 ms
		// with a filter graph rebuild) so any chunk-transition slip
		// returned 0 immediately, the producer treated it as EOF
		// and took a 100 ms nap before retrying - audible as a
		// short silence. 250 ms covers the worker's worst case on
		// the platforms we ship while still leaving plenty of
		// playback budget in the queued chunks (the user can still
		// click stop / pause through the GUI; this lock yields the
		// instant a chunk arrives).
		m_chunkCv.wait_for(lk, std::chrono::milliseconds(250), [&] {
			return !m_chunkQueue.empty()
			    || m_chunkWorkerDone.load(std::memory_order_acquire)
			    || m_chunkWorkerStop.load(std::memory_order_relaxed);
		});
		if (m_chunkQueue.empty()) {
			// Don't latch m_done here: the live done() accessor reads
			// queue + worker state directly, so a transient empty
			// queue while the worker decodes the next chunk reports
			// "not done" without risking the pitch / speed restart
			// race that the old latch caused. Just yield: the
			// producer thread cycles every 100 ms.
			return 0;
		}
		ReverseChunk &chk = m_chunkQueue.front();
		// Publish the head chunk's speedAtBuild so the audio thread's
		// reverse-cursor descent uses the SAME speed factor the audio
		// in this chunk was decoded at. Without this, a fast slider
		// drag has the cursor descend at the post-drag speed while the
		// audio still playing is from chunks decoded at the pre-drag
		// speed — cursor races ahead (or lags behind) the audible
		// head and the rate-limit clamp at the bottom of the position
		// refresh holds the drift in place. Updated every read so a
		// chunk transition (different speedAtBuild) phases-in cleanly.
		m_currentReverseChunkSpeed.store(chk.speedAtBuild,
		                                  std::memory_order_relaxed);
		int channels = m_outputChannels > 0 ? m_outputChannels : 2;
		size_t availShorts = chk.samples.size() - chk.readPos;
		size_t takeShorts  = std::min(availShorts,
		                              (size_t)(4096 * channels));
		short *ptr   = chk.samples.data() + chk.readPos;
		int toWrite  = (int)(takeShorts / channels);

		// Freeverb is on its own mutex now: the chunk worker holds
		// m_mutex for the full decode budget (~150 ms / chunk), so
		// taking m_mutex here just to run freeverb would stall the
		// reader every chunk boundary and underflow sbPlayback - the
		// user-reported "mini lag" in reverse playback. m_freeverb
		// is touched ONLY here in the reverse reader and from _seek's
		// mute() in the worker thread, so the dedicated lock is the
		// only contention in the freeverb critical section.
		{
			std::lock_guard<std::mutex> lock(m_freeverbMutex);
			m_freeverb.process(ptr, toWrite,
			                   m_reverbMix.load(std::memory_order_relaxed));
		}
		sampleBuffer->produce(ptr, toWrite);
		chk.readPos += takeShorts;

		// Update m_filePosition AFTER the produce + readPos
		// increment. m_filePosition must reflect the input-time of
		// the NEXT sample about to be sent to producer (= the input-
		// time of the TAIL of the playback buffer once these freshly
		// produced samples settle in). Sampler's reverse-mode
		// formula posSec = decoderPos + bufferedSec then resolves
		// correctly to the input-time of the HEAD of the buffer (=
		// next sample to play) regardless of how many chunks are
		// stacked in sbPlayback.
		//
		// Computing it BEFORE produce — as we did previously — set
		// m_filePosition to the input-time of the FIRST sample of
		// the just-produced batch, which is ALWAYS one batch worth
		// HIGHER than the buffer tail. That off-by-one batch error
		// accumulates across every chunk transition (chunk1 fully
		// consumed leaves m_filePosition at chunk1.endForward minus
		// only N-1 batches worth instead of N), so the GUI cursor
		// races ahead of audio by a growing margin and the rate-
		// limiter's monotonic clamp locks the lag in: cursor drifts
		// backward by entire seconds within a few chunks. Fixing
		// this single formula closes the seconds-backward-skip.
		// m_filePosition is std::atomic<double> so the unlocked
		// write is well-defined; we never need m_mutex here.
		if (chk.readPos < chk.samples.size()) {
			double frac = (double)chk.readPos
			            / (double)chk.samples.size();
			m_filePosition.store(
				chk.endForward - (chk.endForward - chk.startForward) * frac,
				std::memory_order_relaxed);
		} else {
			m_filePosition.store(chk.startForward,
			                     std::memory_order_relaxed);
		}

		if (chk.readPos >= chk.samples.size())
			m_chunkQueue.pop_front();
		// Wake worker if we made room.
		m_chunkCv.notify_one();
		// Done state is computed live by done() (queue empty + worker
		// done + no restart pending) - no latch here so the audio
		// thread never races the GUI thread's pitch / speed change.
		return toWrite;
	}

	Lock lock(m_mutex);

	if(!m_opened || !m_bufSrcCtx || !m_bufSinkCtx)
	{
		dbgLog("readSamples() called but not ready (opened=%d src=%p sink=%p)", m_opened, m_bufSrcCtx, m_bufSinkCtx);
		return -1;
	}

	AVFrame *frame = av_frame_alloc();
	AVFrame *filt_frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	int written = 0; //samples read

	int properFrames = 0;
	while(properFrames == 0 && av_read_frame(m_fmtCtx, packet) == 0)
	{
		if(packet->stream_index == m_streamIndex)
		{
			if (avcodec_send_packet(m_codecCtx, packet) == 0)
			{
				while (avcodec_receive_frame(m_codecCtx, frame) == 0)
				{
					if (m_nextSeekTimestamp > 0)
					{
						int64_t curTs = frame->best_effort_timestamp;
						int64_t tsToSkip = m_nextSeekTimestamp - curTs;
						if (tsToSkip > 0)
						{
							double timeBase = av_q2d(m_fmtCtx->streams[m_streamIndex]->time_base);
							double secondsToSkip = tsToSkip * timeBase;
							m_skipSamples = (int)(secondsToSkip * m_outputSamplerate);
						}
						m_nextSeekTimestamp = 0;
					}

					if (m_abufferDeclaredRate > 0)
						frame->sample_rate = m_abufferDeclaredRate;

					if (av_buffersrc_add_frame_flags(m_bufSrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0)
					{
						logError("Error while feeding the audio filtergraph");
						break;
					}

					while (1)
					{
						int ret = av_buffersink_get_frame(m_bufSinkCtx, filt_frame);
						if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
							break;
						if (ret < 0)
							break;

						int outSamples = filt_frame->nb_samples;
						int skippedSamples = 0;
						if (m_skipSamples > 0)
						{
							skippedSamples = std::min((int64_t)outSamples, m_skipSamples);
							m_skipSamples -= skippedSamples;
							outSamples -= skippedSamples;
						}

						// Input-time cap (the one the crop UI actually feeds).
						// Computed against m_filePosition so live speed / pitch
						// changes never desync the end of playback from the
						// requested input second mark.
						if (outSamples > 0 && m_maxFilePosition > 0.0)
						{
							double remainSec = m_maxFilePosition - m_filePosition;
							if (remainSec <= 0.0)
							{
								outSamples = 0;
								m_done = true;
							}
							else
							{
								float sRaw = m_speedFactor.load(std::memory_order_relaxed);
								double sf = (sRaw > 0.0f) ? (double)sRaw : 1.0;
								double maxOut = remainSec * (double)m_outputSamplerate / sf;
								if ((double)outSamples > maxOut)
								{
									outSamples = (int)(maxOut + 0.5);
									m_done = true;
								}
							}
						}

						if(outSamples > 0)
						{
							short *outPtr = ((short*)filt_frame->extended_data[0]) + (skippedSamples * m_outputChannels);
							{
								std::lock_guard<std::mutex> fl(m_freeverbMutex);
								m_freeverb.process(outPtr, outSamples,
								                   m_reverbMix.load(std::memory_order_relaxed));
							}
							sampleBuffer->produce(outPtr, outSamples);
							written += outSamples;
							m_convertedSamples += outSamples;
							m_filePosition = m_filePosition + (double)outSamples * (double)m_speedFactor.load(std::memory_order_relaxed) / (double)m_outputSamplerate;
							properFrames++;
						}

						av_frame_unref(filt_frame);
					}
					av_frame_unref(frame);
				}
			}
		}

		av_packet_unref(packet);
	}

	if(properFrames == 0)
	{
		// EOF handling
		avcodec_send_packet(m_codecCtx, NULL);
		while (avcodec_receive_frame(m_codecCtx, frame) == 0)
		{
			av_buffersrc_add_frame_flags(m_bufSrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
			av_frame_unref(frame);
		}
		
		if (m_bufSrcCtx) av_buffersrc_add_frame_flags(m_bufSrcCtx, NULL, 0);

		while (1)
		{
			int ret = av_buffersink_get_frame(m_bufSinkCtx, filt_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0)
				break;

			int outSamples = filt_frame->nb_samples;
			int skippedSamples = 0;
			if (m_skipSamples > 0)
			{
				skippedSamples = std::min((int64_t)outSamples, m_skipSamples);
				m_skipSamples -= skippedSamples;
				outSamples -= skippedSamples;
			}

			// Input-time cap (see twin block above).
			if (outSamples > 0 && m_maxFilePosition > 0.0)
			{
				double remainSec = m_maxFilePosition - m_filePosition;
				if (remainSec <= 0.0)
				{
					outSamples = 0;
					m_done = true;
				}
				else
				{
					float sRaw = m_speedFactor.load(std::memory_order_relaxed);
					double sf = (sRaw > 0.0f) ? (double)sRaw : 1.0;
					double maxOut = remainSec * (double)m_outputSamplerate / sf;
					if ((double)outSamples > maxOut)
						outSamples = (int)(maxOut + 0.5);
				}
			}
			if(outSamples > 0)
			{
				short *outPtr = ((short*)filt_frame->extended_data[0]) + (skippedSamples * m_outputChannels);
				{
					std::lock_guard<std::mutex> fl(m_freeverbMutex);
					m_freeverb.process(outPtr, outSamples,
					                   m_reverbMix.load(std::memory_order_relaxed));
				}
				sampleBuffer->produce(outPtr, outSamples);
				written += outSamples;
				m_convertedSamples += outSamples;
				m_filePosition = m_filePosition + (double)outSamples * (double)m_speedFactor.load(std::memory_order_relaxed) / (double)m_outputSamplerate;
			}
			av_frame_unref(filt_frame);
		}
		m_done = true;
	}

	av_packet_free(&packet);
	av_frame_free(&frame);
	av_frame_free(&filt_frame);

	return written;
}


//---------------------------------------------------------------
bool InputFileFFmpeg::done() const
{
	if (m_streamingReverse) {
		// done() returns true ONLY for a TRUE end of stream:
		//   - file ended naturally (worker reached cursor <= minF),
		//   - OR the worker hit a hard unrecoverable error.
		// m_chunkFileEnded is set ONLY by those two paths. Empty
		// queue + worker parked transiently (e.g. mid-pitch-drag
		// rebuild) no longer satisfies done() — that path was the
		// race that ended reverse playback prematurely while the
		// user dragged the speed slider. The queue check still keeps
		// us "not done" while the reader has buffered audio to
		// drain after a legitimate end was signalled.
		if (m_chunkFileEnded.load(std::memory_order_acquire)) {
			std::lock_guard<std::mutex> lg(m_chunkMutex);
			return m_chunkQueue.empty();
		}
		return false;
	}
	return m_done;
}


//---------------------------------------------------------------
int InputFileFFmpeg::getAudioStreamNum() const
{
	return av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
}


//---------------------------------------------------------------
int InputFileFFmpeg::_close()
{
	if(m_filterGraph)
	{
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;
	}

	if(m_codecCtx)
	{
		avcodec_free_context(&m_codecCtx);
		m_codecCtx = NULL;
	}

	if(m_fmtCtx)
	{
		avformat_close_input(&m_fmtCtx);
		m_codecCtx = NULL;
	}

	m_opened = false;

	return 0;
}


//---------------------------------------------------------------
int64_t InputFileFFmpeg::outputSamplesEstimation() const
{
	if (!m_fmtCtx || m_streamIndex < 0 || m_streamIndex >= (int)m_fmtCtx->nb_streams)
		return 0;
	AVStream *stream = m_fmtCtx->streams[m_streamIndex];
	if (stream->duration > 0)
		return stream->duration * (int64_t)stream->time_base.num *
			(int64_t)m_outputSamplerate / (int64_t)stream->time_base.den;
	else
		return m_fmtCtx->duration * m_outputSamplerate / AV_TIME_BASE;
}


//---------------------------------------------------------------
void InputFileFFmpeg::setPitchFactor(float factor)
{
	// Streaming reverse: NO m_mutex acquire from the GUI thread. The
	// chunk worker holds m_mutex for the full per-chunk decode budget
	// (~150 ms); the previous design blocked the GUI thread on the
	// slider drag handler behind that hold and the user saw the UI
	// freeze. Atomic store + restart signal lets the worker re-read
	// the new factor on its next chunk without any GUI-side wait.
	//
	// Continuity: do NOT clear the chunk queue here. A rapid slider
	// drag (~30 events/s) was wiping the queue faster than the worker
	// could refill it (~150 ms per decode), starving the reader and
	// freezing audio mid-stream. The worker now coalesces restart
	// signals and keeps queue.front() while it rebuilds — the reader
	// keeps draining the in-flight chunk, so the user hears at most
	// one chunk (~1.5 s) of stale pitch instead of a stall.
	if (m_streamingReverse) {
		float old = m_pitchFactor.exchange(factor, std::memory_order_relaxed);
		if (old == factor) return;
		m_chunkRestartPending.store(true, std::memory_order_release);
		m_chunkCv.notify_all();
		return;
	}
	// Forward path: filter graph rebuild needs m_mutex.
	Lock lock(m_mutex);
	float old = m_pitchFactor.load(std::memory_order_relaxed);
	if (old != factor) {
		dbgLog("setPitchFactor(%.4f -> %.4f) pos=%.3f opened=%d", old, factor, m_filePosition.load(), m_opened);
		m_pitchFactor.store(factor, std::memory_order_relaxed);
		if (m_opened) {
			int ret = _seek(m_filePosition);
			dbgLog("  setPitchFactor _seek returned %d (src=%p sink=%p)", ret, m_bufSrcCtx, m_bufSinkCtx);
		}
	}
}


void InputFileFFmpeg::setSpeedFactor(float factor)
{
	// See setPitchFactor: lock-free + queue retained + worker-side
	// restart coalesce. Speed drag was the worst offender for the
	// stall — it shares the rebuild path with pitch (same atempo /
	// asetrate filter graph) so the worker takes ~150 ms per chunk,
	// long enough for a fast drag to wipe the queue 5x before any
	// new chunk lands.
	if (m_streamingReverse) {
		float old = m_speedFactor.exchange(factor, std::memory_order_relaxed);
		if (old == factor) return;
		m_chunkRestartPending.store(true, std::memory_order_release);
		m_chunkCv.notify_all();
		return;
	}
	Lock lock(m_mutex);
	float old = m_speedFactor.load(std::memory_order_relaxed);
	if (old != factor) {
		dbgLog("setSpeedFactor(%.4f -> %.4f) pos=%.3f opened=%d", old, factor, m_filePosition.load(), m_opened);
		m_speedFactor.store(factor, std::memory_order_relaxed);
		if (m_opened) {
			int ret = _seek(m_filePosition);
			dbgLog("  setSpeedFactor _seek returned %d (src=%p sink=%p)", ret, m_bufSrcCtx, m_bufSinkCtx);
		}
	}
}


void InputFileFFmpeg::setReverbMix(float mix)
{
	// m_reverbMix is std::atomic<float> - no lock needed. Decouples
	// the GUI thread's reverb slider from the chunk-worker's m_mutex
	// hold, so dragging the reverb during reverse playback no longer
	// stalls on the decoder.
	m_reverbMix.store(mix, std::memory_order_relaxed);
}


//---------------------------------------------------------------
// Purpose: live-update the absolute end-sample bound so the
// waveform right-click "Set end" truncates active playback at the
// new point. seconds <= 0 -> clear bound (unlimited).
//---------------------------------------------------------------
void InputFileFFmpeg::setMaxPlayTime(double seconds)
{
	Lock lock(m_mutex);
	// Stored as input-time so the bound is invariant under live speed
	// or pitch changes. Comparison happens against m_filePosition in
	// readSamples (which is also input-time).
	m_maxFilePosition = (seconds > 0.0) ? seconds : 0.0;
}


//---------------------------------------------------------------
InputFile *CreateInputFileFFmpeg(InputFileOptions options /*= InputFileOptions()*/)
{
	return new InputFileFFmpeg(options);
}
