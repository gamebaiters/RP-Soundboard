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
};


#endif // rpsbsrc__SampleVisualizerThread_H__
