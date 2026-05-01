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


#define OUTPUT_BUFFER_COUNT 32768
#define OUTPUT_FORMAT AV_SAMPLE_FMT_S16

// ===== FILE DEBUG LOGGING =====
// Set to 1 to enable debug log file, 0 to disable
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
static FILE *g_debugFile = nullptr;
static void dbgOpen()
{
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
	dbgOpen();
	if (!g_debugFile) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_debugFile, fmt, ap);
	va_end(ap);
	fprintf(g_debugFile, "\n");
	fflush(g_debugFile);
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
	void setReverbMix(float mix) override;

private:
	int _close();
	void reset();
	int getAudioStreamNum() const;
	int buildFilterGraph();
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
	double m_filePosition;          // actual position in original file (seconds), written under m_mutex
	float m_pitchFactor;
	float m_speedFactor;
	float m_reverbMix;
	int64_t m_maxConvertedSamples;
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
	m_maxConvertedSamples = 0;
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
int InputFileFFmpeg::buildFilterGraph()
{
	dbgLog("buildFilterGraph() pitch=%.3f speed=%.3f reverb=%.3f", m_pitchFactor, m_speedFactor, m_reverbMix);

	if (m_filterGraph)
		avfilter_graph_free(&m_filterGraph);

	m_filterGraph = avfilter_graph_alloc();
	if (!m_filterGraph) { dbgLog("  FAILED: avfilter_graph_alloc returned NULL"); return -1; }

	char args[512];
	const AVFilter *abuffersrc = avfilter_get_by_name("abuffer");
	const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
	dbgLog("  abuffersrc=%p abuffersink=%p", abuffersrc, abuffersink);
	if (!abuffersrc || !abuffersink) { dbgLog("  FAILED: filter not found!"); return -1; }

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
    char ch_layout_str[128];
    av_channel_layout_describe(&m_codecCtx->ch_layout, ch_layout_str, sizeof(ch_layout_str));
    snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
			m_fmtCtx->streams[m_streamIndex]->time_base.num,
			m_fmtCtx->streams[m_streamIndex]->time_base.den,
			m_codecCtx->sample_rate,
			av_get_sample_fmt_name(m_codecCtx->sample_fmt),
			ch_layout_str);
#else
	snprintf(args, sizeof(args),
			"time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
			m_fmtCtx->streams[m_streamIndex]->time_base.num,
			m_fmtCtx->streams[m_streamIndex]->time_base.den,
			m_codecCtx->sample_rate,
			av_get_sample_fmt_name(m_codecCtx->sample_fmt),
			m_codecCtx->channel_layout);
#endif

	dbgLog("  abuffer args: %s", args);
	int ret = avfilter_graph_create_filter(&m_bufSrcCtx, abuffersrc, "in",
										args, NULL, m_filterGraph);
	dbgLog("  create_filter(abuffersrc) = %d", ret);
	if (ret < 0) return ret;

	ret = avfilter_graph_create_filter(&m_bufSinkCtx, abuffersink, "out",
										NULL, NULL, m_filterGraph);
	dbgLog("  create_filter(abuffersink) = %d", ret);
	if (ret < 0) return ret;

	// abuffersink format constraints have changed names/types across FFmpeg
	// versions:
	//   FFmpeg 6: sample_fmts (BINARY, int array)
	//   FFmpeg 7: sample_fmts (BINARY, must be sentinel-terminated)
	//   FFmpeg 8: sample_formats (STRING, '|'-separated list - sample_fmts
	//             returns EINVAL because the option's type changed)
	// We don't actually NEED to set these on the sink because the aformat
	// filter at the end of the chain (added below) already forces
	// s16/48000/stereo before the sink. Sink with no explicit constraints
	// accepts whatever aformat produces. Best-effort try both names so the
	// sink also has the constraint baked in where supported, but never bail
	// on failure.
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

	// sample format: try FFmpeg 8 STRING name first, fall back to FFmpeg 6/7
	// BINARY (sentinel-terminated)
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
	// ch_layouts has been STRING since FFmpeg 5+; same name in FFmpeg 6/7/8
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

	// Locale-safe double formatter (Italian locale uses comma, which breaks FFmpeg)
	auto fmtDbl = [](double v) -> std::string {
		char b[32];
		snprintf(b, sizeof(b), "%.4f", v);
		for (char *c = b; *c; c++) if (*c == ',') *c = '.';
		return b;
	};

	std::string filters;
	double pitch = m_pitchFactor;
	double speed = m_speedFactor;
	if (pitch < 0.01) pitch = 1.0;
	if (speed < 0.01) speed = 1.0;

	if (pitch != 1.0) {
		filters += "asetrate=" + std::to_string((int)(m_codecCtx->sample_rate * pitch)) + ",aresample=" + std::to_string(m_codecCtx->sample_rate) + ",";
	}

	double t = speed / pitch;
	while (t < 0.5) { filters += "atempo=0.5,"; t /= 0.5; }
	while (t > 100.0) { filters += "atempo=100.0,"; t /= 100.0; }
	if (t != 1.0) { filters += "atempo=" + fmtDbl(t) + ","; }

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
	dbgLog("  avfilter_graph_parse_ptr returned %d", ret);
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	if (ret < 0) {
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;
		return ret;
	}

	ret = avfilter_graph_config(m_filterGraph, NULL);
	dbgLog("  avfilter_graph_config returned %d", ret);
	if (ret < 0) {
		avfilter_graph_free(&m_filterGraph);
		m_filterGraph = NULL;
		m_bufSrcCtx = NULL;
		m_bufSinkCtx = NULL;
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

	int ret = avformat_open_input(&m_fmtCtx, filename, NULL, NULL);
	dbgLog("  avformat_open_input returned %d", ret);
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
	avcodec_parameters_to_context(m_codecCtx, m_fmtCtx->streams[m_streamIndex]->codecpar);

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
	if(startPosSeconds > 0.0)
		_seek(startPosSeconds);

	if(playTimeSeconds > 0.0)
		m_maxConvertedSamples = uint64_t(playTimeSeconds * (double)m_outputSamplerate + 0.5);

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

	AVRational time_base = m_fmtCtx->streams[m_streamIndex]->time_base;
	int64_t ts = (int64_t)(seconds / time_base.num * time_base.den);
	if(LogFFmpegError(avformat_seek_file(m_fmtCtx, m_streamIndex, INT64_MIN, ts, ts, 0), "Seeking failed") < 0)
		return -1;
	avcodec_flush_buffers(m_codecCtx);

	// Rebuild graph to flush filter buffers
	buildFilterGraph();
	m_freeverb.mute();

	m_nextSeekTimestamp = ts;
	m_skipSamples = 0;
	m_convertedSamples = (int64_t)(seconds * (double)m_outputSamplerate);
	m_filePosition = seconds;
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
		m_pitchFactor = factor;
		if (m_opened) buildFilterGraph();
	}
}


void InputFileFFmpeg::setSpeedFactor(float factor)
{
	Lock lock(m_mutex);
	if (m_speedFactor != factor) {
		m_speedFactor = factor;
		if (m_opened) buildFilterGraph();
	}
}


void InputFileFFmpeg::setReverbMix(float mix)
{
	Lock lock(m_mutex);
	m_reverbMix = mix;
}


//---------------------------------------------------------------
InputFile *CreateInputFileFFmpeg(InputFileOptions options /*= InputFileOptions()*/)
{
	return new InputFileFFmpeg(options);
}
