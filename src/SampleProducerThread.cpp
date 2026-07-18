// src/SampleProducerThread.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include <thread>
#include <chrono>
#include <algorithm>
#include <cassert>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "SampleBuffer.h"
#include "SampleSource.h"
#include "SampleProducerThread.h"
#include "ThreadQoS.h"



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SampleProducerThread::SampleProducerThread() :
	m_source(nullptr),
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
	m_cv.notify_all();
	if(wait && m_thread.joinable())
		m_thread.join();
}


//---------------------------------------------------------------
// Purpose: join a thread previously stop(false)'d. No effect if
// the thread was never started.
//---------------------------------------------------------------
void SampleProducerThread::joinIfRunning()
{
	if (m_thread.joinable())
		m_thread.join();
}


//---------------------------------------------------------------
// Purpose: shutdown-only timed join. If the worker has not exited
// within timeoutMs we Win32-TerminateThread + detach so the DLL
// unload is not blocked. The cost is a leaked stack page; the
// alternative was the user-reported zombie TS3 process.
//---------------------------------------------------------------
void SampleProducerThread::joinIfRunningBounded(int timeoutMs)
{
	if (!m_thread.joinable()) return;
#ifdef _WIN32
	HANDLE h = (HANDLE)m_thread.native_handle();
	DWORD rc = WaitForSingleObject(h, (DWORD)timeoutMs);
	if (rc == WAIT_OBJECT_0) {
		m_thread.join();
	} else {
		TerminateThread(h, 0);
		m_thread.detach();
	}
#else
	m_thread.join();
#endif
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
	// Lock-free swap: m_source is atomic. Previously this held m_mutex
	// which blocked the GUI thread on the producer's in-flight
	// singleBufferFill — on a 10-hour audio file a single readSamples
	// inside that fill can take tens of ms, so clicking the X to
	// remove the sound (or letting a sound stop mid-playback) stalled
	// the whole UI for a visible spike. The atomic swap is instant;
	// the producer notices on its next batch boundary.
	m_source.store(source, std::memory_order_release);
	wake();
}


//---------------------------------------------------------------
// Purpose: kick the fill loop out of its inter-cycle wait so a fresh
// source / cleared buffer is refilled immediately.
//---------------------------------------------------------------
void SampleProducerThread::wake()
{
	m_wake.store(true, std::memory_order_release);
	m_cv.notify_all();
}


//---------------------------------------------------------------
// Purpose: spin until the producer is not inside a readSamples call.
// See header for the contract. Polling is brief (200 us per tick) so
// the GUI thread cost is negligible in the common case where the
// producer is between batches or sleeping on its condvar.
//---------------------------------------------------------------
void SampleProducerThread::waitForReadDrain(int maxWaitMs)
{
	if (!m_inReadSamples.load(std::memory_order_acquire))
		return;
	auto start = std::chrono::steady_clock::now();
	while (m_inReadSamples.load(std::memory_order_acquire)) {
		auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
		if (elapsed >= maxWaitMs)
			return;
		std::this_thread::sleep_for(std::chrono::microseconds(200));
	}
}

// Producer fill target: how far ahead of the play head the decoder
// keeps the sample buffer. Kept SMALL (0.5 s) so live pitch / speed /
// reverb edits (applied in the decoder's filter graph) are heard almost
// immediately - a larger target means that many seconds of already-
// buffered audio play with the OLD settings before the change lands.
// The vinyl tape decode-ahead window still builds fine from this: the
// producer refills continuously, so the tape can pull ~3x realtime out
// of it and grow its own ring window over a second regardless.
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
			SampleSource *src = m_source.load(std::memory_order_acquire);
			if (src)
				singleBufferFill(src);
		}
		catch (...)
		{
			// A corrupt / malformed file can throw deep inside the
			// decoder. Swallow it here so the producer thread — and the
			// whole TS3 client — does not crash; the slot simply stops
			// receiving samples and ends cleanly.
			m_stop.store(true);
		}

		// Steady-state refill cadence stays 100 ms (the buffers hold
		// >= 0.5 s), but setSource()/wake() interrupts the wait so a
		// freshly clicked sound starts producing samples instantly
		// instead of after a worst-case 100 ms nap.
		std::unique_lock<std::recursive_mutex> lk(m_mutex);
		m_cv.wait_for(lk, std::chrono::milliseconds(100), [this]{
			return m_wake.load(std::memory_order_acquire)
			    || m_stop.load(std::memory_order_relaxed);
		});
		m_wake.store(false, std::memory_order_relaxed);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleProducerThread::threadFunc()
{
	sbPromoteThreadQoS();   // macOS: inherited low QoS throttles network reads
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
bool SampleProducerThread::singleBufferFill(SampleSource *src)
{
	// Snapshot the buffer list under a BRIEF lock — m_mutex is no
	// longer held around the slow readSamples calls below, so addBuffer
	// / remBuffer / setBufferEnabled from any other thread (e.g. the
	// audio thread or GUI) never blocks behind a multi-ms decode.
	std::vector<buffer_t> bufs;
	{
		Lock lock(m_mutex);
		bufs = m_buffers;
	}
	for (const buffer_t &buffer : bufs)
	{
		if (!buffer.enabled)
			continue;
		std::unique_lock<SampleBuffer::Mutex> sbl(buffer.buffer->getMutex());
		while (buffer.buffer->avail() < MIN_BUFFER_SAMPLES)
		{
			assert(buffer.buffer->maxSize() > MIN_BUFFER_SAMPLES && "Buffer too small");
			sbl.unlock();
			// Preempt: stop requested, or the caller swapped the
			// source on us (setSource was called mid-fill). Drop the
			// in-flight batch — readSamples on the now-stale source
			// would be wasted work and could keep this loop running
			// well past the stop/swap intent.
			if (m_stop.load(std::memory_order_relaxed))
				return true;
			if (m_source.load(std::memory_order_acquire) != src)
				return true;
			m_inReadSamples.store(true, std::memory_order_release);
			int samples;
			try {
				samples = src->readSamples(this);
			} catch (...) {
				m_inReadSamples.store(false, std::memory_order_release);
				throw;
			}
			m_inReadSamples.store(false, std::memory_order_release);
			if (samples < 0) // error
				return false;
			if (samples == 0) // file is done
				return true;
			sbl.lock();
		}
	}
	return true;
}
