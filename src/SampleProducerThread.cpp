// src/SampleProducerThread.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include <thread>
#include <algorithm>
#include <cassert>

#include "SampleBuffer.h"
#include "SampleSource.h"
#include "SampleProducerThread.h"



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SampleProducerThread::SampleProducerThread() :
	m_source(NULL),
	m_running(false),
	m_stop(false)
{

}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::start()
{
	if(m_running.load())
		return;

	m_running.store(true);
	m_stop.store(false);
	std::thread t(&SampleProducerThread::threadFunc, this);
	m_thread = std::move(t);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::stop(bool wait)
{
	m_stop.store(true);
	if(wait && m_thread.joinable())
		m_thread.join();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool SampleProducerThread::isRunning()
{
	return m_running.load();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::setSource( SampleSource *source )
{
	Lock lock(m_mutex);
	m_source = source;
}

#define MIN_BUFFER_SAMPLES (48000 / 2)
//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::run()
{
	while(!m_stop.load(std::memory_order_relaxed))
	{
		try
		{
			Lock lock(m_mutex);
			if (m_source)
				singleBufferFill();
		}
		catch (...)
		{
			// A corrupt / malformed file can throw deep inside the
			// decoder. Swallow it here so the producer thread — and the
			// whole TS3 client — does not crash; the slot simply stops
			// receiving samples and ends cleanly.
			m_stop.store(true);
		}

		// We now have half a second of samples available and have done
		// so much work that we deserve a little rest
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::threadFunc()
{
	run();
	m_running.store(false);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::addBuffer( SampleBuffer *buffer, bool enableBuffer /*= true*/ )
{
	Lock lock(m_mutex);
	if(std::find_if(m_buffers.begin(), m_buffers.end(), [buffer](const buffer_t &b) {return b.buffer == buffer;}) == m_buffers.end())
	{
		buffer_t b = {buffer, enableBuffer};
		m_buffers.push_back(b);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::remBuffer( SampleBuffer *buffer )
{
	Lock lock(m_mutex);
	auto it = std::find_if(m_buffers.begin(), m_buffers.end(), [buffer](const buffer_t &b) {return b.buffer == buffer;});
	if(it != m_buffers.end())
		m_buffers.erase(it);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::setBufferEnabled( SampleBuffer *buffer, bool enabled )
{
	Lock lock(m_mutex);
	auto it = std::find_if(m_buffers.begin(), m_buffers.end(), [buffer](const buffer_t &b) {return b.buffer == buffer;});
	if(it != m_buffers.end())
		it->enabled = enabled;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::produce( const short *samples, int count )
{
	Lock lock(m_mutex);
	for(const buffer_t &buffer : m_buffers)
	{
		if(buffer.enabled)
		{
			SampleBuffer::Lock sbl(buffer.buffer->getMutex());
			buffer.buffer->produce(samples, count);
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool SampleProducerThread::singleBufferFill()
{
	for(const buffer_t &buffer : m_buffers)
	{
		if (buffer.enabled)
		{
			std::unique_lock<SampleBuffer::Mutex> sbl(buffer.buffer->getMutex());
			while (buffer.buffer->avail() < MIN_BUFFER_SAMPLES)
			{
				assert(buffer.buffer->maxSize() > MIN_BUFFER_SAMPLES && "Buffer too small");
				sbl.unlock();
				int samples = m_source->readSamples(this);
				if (samples < 0) // error
					return false;
				if (samples == 0) // file is done
					return true;
				sbl.lock();
			}
		}
	}
	return true;
}
