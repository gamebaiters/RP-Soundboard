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
#include <algorithm>
#include <string>
#include <cstring>
#include <cmath>

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
	float getSpeedFactor() const override { return m_speedFactor; }
	void setReverbMix(float mix) override;
	void setMaxPlayTime(double seconds) override;
	void setReverse(bool on) override { m_reverse = on; }
	void setAutoNormalize(bool on) override { m_autoNormalize = on; }
	void setCancelToken(std::atomic<bool> *token) override { m_cancelToken = token; }

private:
	int _close();
	void reset();
	int getAudioStreamNum() const;
	int buildFilterGraph(bool allowPitch = true);
	int _seek(double seconds); // Internal seek without locking (caller must hold m_mutex)
	// Reverse-playback pre-decode: drains the whole filter graph into
	// m_reverseBuf, then reverses the interleaved short buffer in
	// place. Called once from open() when m_reverse is true.
	int preDecodeAndReverse();
	// Re-decode + re-reverse the buffer with the current pitch/speed
	// factors. Called whenever the user moves the FxPanel pitch /
	// speed sliders while a reverse playback is active. Keeps the
	// playhead at the same forward-file position so the cursor does
	// not jump.
	int rebuildReverseBuffer();

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
	double m_filePosition;          // actual position in original file (seconds), written under m_mutex
	float m_pitchFactor;
	float m_speedFactor;
	float m_reverbMix;
	// Reverse playback toggle. Set BEFORE open(); the decoder reads the
	// whole file into m_reverseBuf, reverses it, and readSamples then
	// replays from the buffer. areverse-as-filter does not work with
	// the streaming readSamples loop (areverse needs input EOF).
	bool  m_reverse        = false;
	bool  m_reverseReady   = false;
	std::vector<short> m_reverseBuf;
	int64_t            m_reversePos = 0;   // output sample index
	// Deferred rebuild: setPitch/setSpeed mark dirty + record the
	// current steady-clock tick. readSamples performs the heavy
	// re-decode only after the dirty flag has been quiet for ~250 ms.
	// Coalesces a slider drag at 60 Hz into a single rebuild.
	bool                 m_reverseRebuildPending = false;
	std::chrono::steady_clock::time_point m_reverseRebuildTouch;
	// Speed factor captured at the last rebuild. The forward-time
	// mapping uses this value because the reverse buffer was decoded
	// with atempo applied - output sample count is fileDur/speedF.
	float                m_reverseSpeedAtBuild   = 1.0f;
	// LUFS auto-normalisation toggle. Set BEFORE open(); buildFilterGraph
	// appends a loudnorm filter targeting -16 LUFS integrated.
	bool  m_autoNormalize = false;
	// Cooperative cancel token for the reverse pre-decode pass. Polled
	// inside preDecodeAndReverse's hot loop so a worker spawned by the
	// async setSlotReverse path can be asked to give up without waiting
	// on a multi-minute file. Lifetime owned by caller (Sampler holds a
	// shared_ptr<atomic<bool>> per slot).
	std::atomic<bool> *m_cancelToken = nullptr;
	int m_abufferDeclaredRate;      // rate declared to abuffer (may differ from codec rate for pitch)
	int64_t m_maxConvertedSamples;
	// End-of-playback bound in INPUT-file seconds. m_maxConvertedSamples
	// was the previous mechanism but, being in OUTPUT samples, it became
	// inconsistent under any non-1.0 speedFactor: a 10-second crop with
	// speed=2 stopped the decoder at 20 s of input (because 10*48000
	// output samples * 2 = 20 s of source) instead of at the requested
	// 10 s mark. m_maxFilePosition is compared against m_filePosition
	// (which is tracked in input seconds) so the bound stays correct no
	// matter how pitch / speed are adjusted during playback. 0.0 means
	// "unbounded". When both are set m_maxFilePosition wins.
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
	m_outputChannelLayout(getChannelLayoutFromOptions(options)),
	m_pitchFactor(1.0f),
	m_speedFactor(1.0f),
	m_reverbMix(0.0f)
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
	m_abufferDeclaredRate = 0;
	m_maxConvertedSamples = 0;
	m_maxFilePosition = 0.0;
	m_nextSeekTimestamp = 0;
	m_skipSamples = 0;
	m_freeverb.init(m_outputSamplerate, m_outputChannels);
}


//---------------------------------------------------------------
InputFileFFmpeg::~InputFileFFmpeg()
{
	_close();
}


//---------------------------------------------------------------
int InputFileFFmpeg::buildFilterGraph(bool allowPitch)
{
	double pitch = m_pitchFactor;
	double speed = m_speedFactor;
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
	       allowPitch, pitch, speed, m_reverbMix,
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
		return ret;
	}

	ret = avfilter_graph_create_filter(&m_bufSinkCtx, abuffersink, "out",
										NULL, NULL, m_filterGraph);
	dbgLog("  create_filter(abuffersink) = %d", ret);
	if (ret < 0) {
		char errbuf[128]; av_strerror(ret, errbuf, sizeof(errbuf));
		dbgLog("  FAILED create_filter(abuffersink): %s", errbuf);
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
	if (m_autoNormalize) filters += "loudnorm=I=-16:TP=-1:LRA=11,";

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
	logInfo("Opened file: %s; Codec: %s, Channels: %i, Rate: %i, Format: %s, Timebase: %i/%i, Sample-Estimation: %ll",
		filename, codec->long_name, m_codecCtx->ch_layout.nb_channels, m_codecCtx->sample_rate,
		av_get_sample_fmt_name(m_codecCtx->sample_fmt), m_fmtCtx->streams[m_streamIndex]->time_base.num, m_fmtCtx->streams[m_streamIndex]->time_base.den,
		outputSamplesEstimation());
#else
	logInfo("Opened file: %s; Codec: %s, Channels: %i, Rate: %i, Format: %s, Timebase: %i/%i, Sample-Estimation: %ll",
		filename, codec->long_name, m_codecCtx->channels, m_codecCtx->sample_rate,
		av_get_sample_fmt_name(m_codecCtx->sample_fmt), m_fmtCtx->streams[m_streamIndex]->time_base.num, m_fmtCtx->streams[m_streamIndex]->time_base.den,
		outputSamplesEstimation());
#endif

	m_opened = true;
	dbgLog("  file opened successfully, estimation=%lld samples", (long long)outputSamplesEstimation());

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
		int rv = preDecodeAndReverse();
		if (rv < 0) {
			dbgLog("  preDecodeAndReverse failed");
			_close();
			return -1;
		}
	}

	return 0;
}

int InputFileFFmpeg::preDecodeAndReverse()
{
	AVFrame *frame = av_frame_alloc();
	AVFrame *filt_frame = av_frame_alloc();
	AVPacket *packet = av_packet_alloc();
	if (!frame || !filt_frame || !packet) {
		if (frame) av_frame_free(&frame);
		if (filt_frame) av_frame_free(&filt_frame);
		if (packet) av_packet_free(&packet);
		return -1;
	}

	// Pre-reserve the destination buffer. The decoder appends in small
	// chunks (typical AVFrame nb_samples = 1024) and the default
	// std::vector growth strategy is amortised O(1) but still costs a
	// realloc + memcpy on every doubling. For a 5-minute stereo file
	// that's ~28M shorts (~57 MB) growing through ~24 reallocations.
	// outputSamplesEstimation() gives a tight bound from the container
	// header so one allocation covers the whole decode.
	int channels = m_outputChannels;
	if (channels <= 0) channels = 2;
	int64_t estSamples = outputSamplesEstimation();
	if (estSamples > 0) {
		// outputSamplesEstimation is in the INPUT-time domain; reverse
		// buffer is in the OUTPUT-rate-with-atempo domain. Divide by
		// speedFactor (which atempo applies) to get OUT samples.
		double speedF = (m_speedFactor > 0.0f) ? (double)m_speedFactor : 1.0;
		int64_t expected = (int64_t)((double)estSamples / speedF) + 4096;
		m_reverseBuf.reserve((size_t)(expected * channels));
	}

	bool cancelled = false;
	int packetCounter = 0;
	auto checkCancel = [&]() {
		// Poll the cancel token every 64 packets to keep overhead
		// negligible while still bailing within ~10-50 ms of a request.
		if (!m_cancelToken) return false;
		if ((++packetCounter & 63) != 0) return false;
		return m_cancelToken->load(std::memory_order_relaxed);
	};

	auto drainSink = [&]() {
		while (av_buffersink_get_frame(m_bufSinkCtx, filt_frame) >= 0) {
			int outSamples = filt_frame->nb_samples;
			if (outSamples > 0) {
				short *outPtr = (short*)filt_frame->extended_data[0];
				size_t prev = m_reverseBuf.size();
				m_reverseBuf.resize(prev + outSamples * m_outputChannels);
				std::memcpy(m_reverseBuf.data() + prev, outPtr,
				            outSamples * m_outputChannels * sizeof(short));
			}
			av_frame_unref(filt_frame);
		}
	};

	while (av_read_frame(m_fmtCtx, packet) >= 0) {
		if (packet->stream_index == m_streamIndex) {
			if (avcodec_send_packet(m_codecCtx, packet) == 0) {
				while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
					if (m_abufferDeclaredRate > 0)
						frame->sample_rate = m_abufferDeclaredRate;
					av_buffersrc_add_frame_flags(m_bufSrcCtx, frame,
					                              AV_BUFFERSRC_FLAG_KEEP_REF);
					drainSink();
					av_frame_unref(frame);
				}
			}
		}
		av_packet_unref(packet);
		if (checkCancel()) { cancelled = true; break; }
	}

	if (!cancelled) {
		avcodec_send_packet(m_codecCtx, NULL);
		while (avcodec_receive_frame(m_codecCtx, frame) == 0) {
			av_buffersrc_add_frame_flags(m_bufSrcCtx, frame,
			                              AV_BUFFERSRC_FLAG_KEEP_REF);
			drainSink();
			av_frame_unref(frame);
		}
		av_buffersrc_add_frame_flags(m_bufSrcCtx, NULL, 0);
		drainSink();
	}

	av_frame_free(&frame);
	av_frame_free(&filt_frame);
	av_packet_free(&packet);

	if (cancelled) {
		m_reverseBuf.clear();
		m_reverseBuf.shrink_to_fit();
		dbgLog("  preDecodeAndReverse cancelled");
		return -1;
	}

	int64_t totalSamples = (int64_t)m_reverseBuf.size() / channels;
	// In-place buffer reverse. Stereo fast-path swaps interleaved
	// pairs as one uint32 each, halving the loop body's instruction
	// count vs the per-channel std::swap. Mono / other layouts fall
	// back to the generic path.
	if (channels == 2 && totalSamples > 1) {
		uint32_t *p32 = reinterpret_cast<uint32_t*>(m_reverseBuf.data());
		for (int64_t i = 0, j = totalSamples - 1; i < j; ++i, --j)
			std::swap(p32[i], p32[j]);
	} else {
		for (int64_t i = 0, j = totalSamples - 1; i < j; ++i, --j) {
			for (int c = 0; c < channels; ++c) {
				std::swap(m_reverseBuf[i * channels + c],
				          m_reverseBuf[j * channels + c]);
			}
		}
	}

	m_reversePos    = 0;
	m_reverseReady  = true;
	// Speed factor as it was at decode time. All future forward-time
	// math uses this value, NOT the live m_speedFactor: changing the
	// slider after a rebuild leaves m_speedFactor diverged until the
	// next rebuild settles.
	m_reverseSpeedAtBuild = (m_speedFactor > 0.0f) ? m_speedFactor : 1.0f;
	double speedF = m_reverseSpeedAtBuild;
	// Initial position = cropEnd if set, otherwise file length. Map
	// forward time to output samples via speedF.
	double startFwd = (m_maxFilePosition > 0.0)
		? m_maxFilePosition
		: (double)totalSamples / (double)m_outputSamplerate * speedF;
	int64_t startOutSamples = (int64_t)(
		startFwd * (double)m_outputSamplerate / speedF + 0.5);
	if (startOutSamples < 0) startOutSamples = 0;
	if (startOutSamples > totalSamples) startOutSamples = totalSamples;
	m_reversePos    = totalSamples - startOutSamples;
	m_filePosition  = startFwd;
	m_convertedSamples = 0;
	dbgLog("  preDecodeAndReverse OK: %lld samples buffered",
	       (long long)totalSamples);
	return 0;
}

int InputFileFFmpeg::rebuildReverseBuffer()
{
	// Save user-visible forward position so the cursor does not jump.
	double savedFwd = m_filePosition;
	int channelsOut = m_outputChannels > 0 ? m_outputChannels : 2;
	// Drop existing buffer + re-build the filter graph with current
	// pitch / speed factors via _seek(0). Then drain the whole file
	// through it again.
	m_reverseBuf.clear();
	m_reverseReady = false;
	m_reversePos   = 0;
	int sret = _seek(0.0);
	if (sret < 0) return sret;
	int pret = preDecodeAndReverse();
	if (pret < 0) return pret;
	// Map savedFwd to output samples via the FRESH speed factor
	// (preDecodeAndReverse already cached it into m_reverseSpeedAtBuild).
	int64_t totalSamples = (int64_t)m_reverseBuf.size() / channelsOut;
	double speedF = (m_reverseSpeedAtBuild > 0.0f)
		? (double)m_reverseSpeedAtBuild : 1.0;
	int64_t forwardOutSamples = (int64_t)(
		savedFwd * (double)m_outputSamplerate / speedF + 0.5);
	if (forwardOutSamples < 0) forwardOutSamples = 0;
	if (forwardOutSamples > totalSamples) forwardOutSamples = totalSamples;
	m_reversePos    = totalSamples - forwardOutSamples;
	m_filePosition  = savedFwd;
	m_done          = false;
	return 0;
}


//---------------------------------------------------------------
// Public seek: acquires m_mutex then delegates to _seek.
//---------------------------------------------------------------
int InputFileFFmpeg::seek( double seconds )
{
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

	if (m_reverseReady) {
		// seconds = FORWARD-FILE time. Convert to output sample index
		// via the speed factor captured at buffer build (the buffer
		// has atempo baked in, so 1 forward second = sampleRate /
		// speedAtBuild output samples).
		int channels = m_outputChannels > 0 ? m_outputChannels : 2;
		int64_t totalSamples = (int64_t)m_reverseBuf.size() / channels;
		double speedF = (m_reverseSpeedAtBuild > 0.0f)
			? (double)m_reverseSpeedAtBuild : 1.0;
		int64_t forwardOutSamples = (int64_t)(
			seconds * (double)m_outputSamplerate / speedF + 0.5);
		if (forwardOutSamples < 0) forwardOutSamples = 0;
		if (forwardOutSamples > totalSamples) forwardOutSamples = totalSamples;
		m_reversePos    = totalSamples - forwardOutSamples;
		m_filePosition  = seconds;
		m_done          = (m_reversePos >= totalSamples);
		return 0;
	}

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
	m_freeverb.mute();

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
	// outputSamplesEstimation accesses m_fmtCtx — needs lock for thread safety
	Lock lock(m_mutex);
	return (double)outputSamplesEstimation() / (double)m_outputSamplerate;
}


//---------------------------------------------------------------
int InputFileFFmpeg::close()
{
	Lock lock(m_mutex);
	return _close();
}


//---------------------------------------------------------------
int InputFileFFmpeg::readSamples(SampleProducer *sampleBuffer)
{
	Lock lock(m_mutex);

	if(!m_opened || !m_bufSrcCtx || !m_bufSinkCtx)
	{
		dbgLog("readSamples() called but not ready (opened=%d src=%p sink=%p)", m_opened, m_bufSrcCtx, m_bufSinkCtx);
		return -1;
	}

	if (m_reverseReady) {
		// Deferred rebuild: coalesce a slider drag into a single
		// re-decode 250 ms after the last value change. Cheap dirty
		// check; the expensive work only fires once.
		if (m_reverseRebuildPending) {
			auto now = std::chrono::steady_clock::now();
			auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
			              now - m_reverseRebuildTouch).count();
			if (ms >= 250) {
				m_reverseRebuildPending = false;
				rebuildReverseBuffer();
				if (!m_reverseReady) return 0;
			}
		}
		int channels = m_outputChannels > 0 ? m_outputChannels : 2;
		int64_t totalSamples = (int64_t)m_reverseBuf.size() / channels;
		int64_t avail = totalSamples - m_reversePos;
		if (avail <= 0) {
			m_done = true;
			return 0;
		}
		int toWrite = (avail > 4096) ? 4096 : (int)avail;
		// Forward-time lower bound (cropStart). Output samples per
		// forward second = sampleRate / speedAtBuild because the
		// buffer was decoded with atempo applied.
		double speedF = (m_reverseSpeedAtBuild > 0.0f)
			? (double)m_reverseSpeedAtBuild : 1.0;
		if (m_minFilePosition > 0.0) {
			double curFwd = (double)(totalSamples - m_reversePos)
			              / (double)m_outputSamplerate * speedF;
			double remainingFwd = curFwd - m_minFilePosition;
			if (remainingFwd <= 0.0) {
				m_done = true;
				return 0;
			}
			int maxSamples = (int)(remainingFwd
			              * (double)m_outputSamplerate / speedF);
			if (maxSamples < toWrite) {
				toWrite = maxSamples;
				if (toWrite <= 0) {
					m_done = true;
					return 0;
				}
			}
		}
		short *ptr = m_reverseBuf.data() + m_reversePos * channels;
		m_freeverb.process(ptr, toWrite, m_reverbMix);
		sampleBuffer->produce(ptr, toWrite);
		m_reversePos       += toWrite;
		m_convertedSamples += toWrite;
		m_filePosition = (double)(totalSamples - m_reversePos)
		                / (double)m_outputSamplerate * speedF;
		if (m_reversePos >= totalSamples) m_done = true;
		if (m_filePosition <= m_minFilePosition) m_done = true;
		return toWrite;
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

						if(m_maxConvertedSamples > 0)
						{
							int64_t remaining = m_maxConvertedSamples - m_convertedSamples;
							if (remaining <= 0)
							{
								outSamples = 0;
								m_done = true;
							}
							else if (outSamples > remaining)
							{
								outSamples = (int)remaining;
								m_done = true;
							}
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
								double sf = (m_speedFactor > 0.0f) ? (double)m_speedFactor : 1.0;
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
							m_freeverb.process(outPtr, outSamples, m_reverbMix);
							sampleBuffer->produce(outPtr, outSamples);
							written += outSamples;
							m_convertedSamples += outSamples;
							m_filePosition += (double)outSamples * (double)m_speedFactor / (double)m_outputSamplerate;
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

			if(m_maxConvertedSamples > 0)
			{
				int64_t remaining = m_maxConvertedSamples - m_convertedSamples;
				if (remaining <= 0)
					outSamples = 0;
				else if (outSamples > remaining)
					outSamples = (int)remaining;
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
					double sf = (m_speedFactor > 0.0f) ? (double)m_speedFactor : 1.0;
					double maxOut = remainSec * (double)m_outputSamplerate / sf;
					if ((double)outSamples > maxOut)
						outSamples = (int)(maxOut + 0.5);
				}
			}
			if(outSamples > 0)
			{
				short *outPtr = ((short*)filt_frame->extended_data[0]) + (skippedSamples * m_outputChannels);
				m_freeverb.process(outPtr, outSamples, m_reverbMix);
				sampleBuffer->produce(outPtr, outSamples);
				written += outSamples;
				m_convertedSamples += outSamples;
				m_filePosition += (double)outSamples * (double)m_speedFactor / (double)m_outputSamplerate;
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
	Lock lock(m_mutex);
	if (m_pitchFactor != factor) {
		dbgLog("setPitchFactor(%.4f -> %.4f) pos=%.3f opened=%d", m_pitchFactor, factor, m_filePosition, m_opened);
		m_pitchFactor = factor;
		if (m_opened) {
			if (m_reverseReady) {
				// Defer: a slider drag spams this method 60x/s;
				// rebuilding on every call would melt the GUI.
				m_reverseRebuildPending = true;
				m_reverseRebuildTouch   = std::chrono::steady_clock::now();
			} else {
				int ret = _seek(m_filePosition);
				dbgLog("  setPitchFactor _seek returned %d (src=%p sink=%p)", ret, m_bufSrcCtx, m_bufSinkCtx);
			}
		}
	}
}


void InputFileFFmpeg::setSpeedFactor(float factor)
{
	Lock lock(m_mutex);
	if (m_speedFactor != factor) {
		dbgLog("setSpeedFactor(%.4f -> %.4f) pos=%.3f opened=%d", m_speedFactor, factor, m_filePosition, m_opened);
		m_speedFactor = factor;
		if (m_opened) {
			if (m_reverseReady) {
				m_reverseRebuildPending = true;
				m_reverseRebuildTouch   = std::chrono::steady_clock::now();
			} else {
				int ret = _seek(m_filePosition);
				dbgLog("  setSpeedFactor _seek returned %d (src=%p sink=%p)", ret, m_bufSrcCtx, m_bufSinkCtx);
			}
		}
	}
}


void InputFileFFmpeg::setReverbMix(float mix)
{
	Lock lock(m_mutex);
	m_reverbMix = mix;
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
