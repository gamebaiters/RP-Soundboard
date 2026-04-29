// src/SampleProducerThread.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#pragma once
#ifndef rpsbsrc__SampleProducerThread_H__
#define rpsbsrc__SampleProducerThread_H__

#include <thread>
#include <vector>
#include <atomic>

#include "SampleProducer.h"

class SampleBuffer;
class SampleSource;


class SampleProducerThread : public SampleProducer
{
	struct buffer_t
	{
		SampleBuffer *buffer;
		bool enabled;
	};

public:
	SampleProducerThread();
	void addBuffer(SampleBuffer *buffer, bool enableBuffer = true);
	void remBuffer(SampleBuffer *buffer);
	void setBufferEnabled(SampleBuffer *buffer, bool enabled);
	void start();
	void stop(bool wait = true);
	bool isRunning();
	void setSource(SampleSource *source);
	
private:
	void run();
	void threadFunc();
	bool singleBufferFill();
	void produce(const short *samples, int count) override;

	typedef std::lock_guard<std::recursive_mutex> Lock;

	std::thread m_thread;
	SampleSource *m_source;
	std::vector<buffer_t> m_buffers;
	std::atomic<bool> m_running;
	std::atomic<bool> m_stop;
	std::recursive_mutex m_mutex;
};

#endif // rpsbsrc__SampleProducerThread_H__