// src/SampleVisualizerThread.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__SampleVisualizerThread_H__
#define rpsbsrc__SampleVisualizerThread_H__

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <string>

#include "SampleBuffer.h"


class InputFile;
class SampleVisualizerThread
{
public:
	class SampleBufferSynced : public SampleBuffer
	{
	public:
		SampleBufferSynced(int channels, size_t maxSize = 0);
		virtual void produce(const short *samples, int count) override;
	};

public:
	SampleVisualizerThread();
	~SampleVisualizerThread();
	void startAnalysis(const char *filename, size_t numBins);
	void stop(bool wait = true);
	// Shutdown-only bounded join. Signals stop + waits up to
	// timeoutMs; force-terminates + detaches if the worker hasn't
	// exited (e.g. stuck deep inside a long FFmpeg decode). Used by
	// ~SoundView during sb_kill — prevents the per-channel visualiser
	// from blocking DLL unload and leaving a zombie TS3.exe behind.
	void stopBounded(int timeoutMs);
	bool isRunning() const;
	size_t getBinsProcessed() const;
	
	// Array with numBins * 2 values (min and max value)
	volatile const int *getBins() const;
	
	inline int64_t getTotalSamplesEst() const {
		return m_numSamplesTotalEst;
	}

	// Get file length in seconds, might be an estimation when processing isn't finished yet
	double fileLength() const;

	// Decimated MONO copy of the decoded signal, kept alongside the bins so
	// the GUI can re-render the waveform through the REAL DSP chain instead
	// of guessing what an effect does to an envelope.
	//
	// Published PROGRESSIVELY: the cache always spans exactly the same prefix
	// of the file as the bins do, so a partial cache maps onto the partial bin
	// range with no x-axis skew, and the caller can re-pull as it grows. It is
	// self-limiting (halvePreview) rather than truncating, so it stays valid
	// even for a container that reports no duration at all. Returns false only
	// while there is genuinely nothing decoded yet.
	bool getPreviewAudio(std::vector<float> &out, double &sampleRate) const;

	// Hard ceiling on the cached preview (floats). 1.2 M = 4.8 MB per view and
	// ~110 s at the 11 kHz target rate; longer files decimate further rather
	// than allocating more. The cap is also the render budget: the GUI thread
	// walks this array once per stage on every re-render.
	static const size_t kMaxPreviewFrames = 1200000;

	// NOTE: no singleton anymore. Each SoundView owns its own instance
	// so concurrent channels can analyse different files without
	// overwriting each other's bins. The worker thread self-terminates
	// once its file is fully processed, so idle views cost no thread.

private:
	void run();
	void threadFunc();
	void openNewFile();
	void processSamples(size_t newSamples);
	void finalizeBins();
	static void getMinMax(const short *data, size_t count, int &min, int &max);
	// Box-average `count` decoded samples down into the preview cache.
	void accumulatePreview(const short *data, size_t count);
	// Cache hit its ceiling: average adjacent frames in place and double the
	// decimation, so the cache keeps covering the WHOLE decoded range.
	void halvePreview();

	typedef std::lock_guard<std::mutex> Lock;

private:
	SampleBufferSynced m_buffer;
	size_t m_numBins;
	std::atomic<size_t> m_numBinsProcessed;
	int64_t m_numSamplesProcessed;
	int64_t m_numSamplesTotalEst;
	size_t m_numSamplesProcessedThisBin;
	int m_min;
	int m_max;
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::vector<int> m_bins;
	InputFile *m_file;
	std::thread m_thread;
	// Filename the GUI requested; consumed by worker at next iteration.
	std::string m_pendingFilename;
	size_t      m_pendingNumBins;
	// Filename + numBins currently being processed (worker-owned).
	std::string m_filename;
	std::atomic<bool> m_running;
	std::atomic<bool> m_newFile;
	std::atomic<bool> m_stop;

	// ---- DSP-preview cache (guarded by m_mutex like m_bins) ----
	std::vector<float> m_preview;
	std::atomic<bool>  m_previewReady;
	int    m_previewDecim = 4;
	double m_previewRate  = 0.0;
	double m_previewAcc   = 0.0;         // running box-average accumulator
	int    m_previewAccN  = 0;
};


#endif // rpsbsrc__SampleVisualizerThread_H__
