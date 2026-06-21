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
#include <mutex>
#include <condition_variable>

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
	// Join an already-stopped thread without re-signalling. Pair with
	// stop(false) for parallel-shutdown patterns where ALL producers
	// get the stop flag first (so they wake up simultaneously) then
	// the caller harvests them. No-op if the thread was never started
	// or has already been joined.
	void joinIfRunning();
	// Bounded variant: wait up to timeoutMs for the worker, then
	// force-terminate + detach if it has not exited yet. Use ONLY
	// during shutdown — TerminateThread leaks the worker's stack +
	// resources, but the alternative is keeping the DLL mapped (=
	// zombie TS3.exe blocking the next plugin install / update).
	void joinIfRunningBounded(int timeoutMs);
	bool isRunning();
	void setSource(SampleSource *source);
	// Wake the fill loop immediately (e.g. after a seek cleared the
	// buffers) instead of waiting for the next 100 ms refill tick.
	void wake();
	// Spin-wait until the producer is not currently inside a
	// readSamples() call on the previous source. Use after
	// setSource(nullptr) but BEFORE deleting the old InputFile, since
	// setSource is now lock-free and the producer might still be
	// finishing one last readSamples on the detached pointer. Spins
	// briefly (sub-millisecond in the common case, capped at
	// `maxWaitMs` for pathological cases — e.g. a 10-hour file
	// blocking on a slow network share). Safe to call without holding
	// any lock; never touches m_mutex so the audio path is undisturbed.
	void waitForReadDrain(int maxWaitMs = 250);

private:
	void run();
	void threadFunc();
	// Pre-snapshotted source pointer so the slow readSamples loop
	// does not need to re-load m_source on every iteration. Caller
	// (run()) snapshots once per outer batch.
	bool singleBufferFill(SampleSource *src);
	void produce(const short *samples, int count) override;

	typedef std::lock_guard<std::recursive_mutex> Lock;

	std::thread m_thread;
	// Atomic: setSource() must not block on the producer's m_mutex
	// while a slow readSamples() is in flight. On long audio a single
	// readSamples can take tens of ms and we want a click on the X
	// (or a stop-slot during playback teardown) to be instant. The
	// caller's contract is unchanged: the source must remain valid
	// until setSource(nullptr) followed by either stop()+join or
	// setSource(other) — i.e., the previous source can only be
	// destroyed AFTER the producer has rotated past it.
	std::atomic<SampleSource*> m_source;
	std::vector<buffer_t> m_buffers;
	std::atomic<bool> m_running;
	std::atomic<bool> m_stop;
	// Wake flag + condvar replace the old fixed 100 ms sleep: a fresh
	// setSource (= user clicked play) wakes the fill loop instantly, so
	// playback no longer starts up to 100 ms late.
	std::atomic<bool> m_wake{false};
	// "Currently inside src->readSamples()" — set immediately before
	// the call, cleared after. waitForReadDrain() polls this so the
	// caller can know when it is safe to delete the previous source.
	std::atomic<bool> m_inReadSamples{false};
	std::condition_variable_any m_cv;
	std::recursive_mutex m_mutex;
};

#endif // rpsbsrc__SampleProducerThread_H__