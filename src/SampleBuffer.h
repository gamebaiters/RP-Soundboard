// src/SampleBuffer.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#pragma once
#ifndef rpsbsrc__SampleBuffer_H__
#define rpsbsrc__SampleBuffer_H__

#include <vector>
#include <mutex>
#include <cassert>


#include "SampleProducer.h"

// FIFO sample buffer with an amortised-O(1) consume.
//
// Storage is a flat vector plus a read offset: consume() only advances
// m_readPos (no erase, no memmove on the audio thread); produce()
// appends, and compacts the dead head region once it grows past half
// the live data. The previous implementation erased from the front of
// the vector on EVERY consume, which moved up to ~1 MB of samples per
// audio callback per slot - measurable as audio-thread CPU burn.
//
// Locking contract unchanged: callers hold getMutex() around every
// accessor (the audio path takes SampleBuffer::Lock explicitly).
class SampleBuffer : public SampleProducer
{
public:
	typedef std::mutex Mutex;
	typedef std::lock_guard<Mutex> Lock;

	class ProduceCallback
	{
	public:
		virtual void onProduceSamples(const short *samples, int count, SampleBuffer *caller) = 0;
	};

	class ConsumeCallback
	{
	public:
		virtual void onConsumeSamples(const short *samples, int count, SampleBuffer *caller) = 0;
	};

public:
	SampleBuffer(int channels, size_t maxSize = 0);

	//Set the callback that is called when samples are placed into the buffer (produced)
	inline void setOnProduce(ProduceCallback *cb) {
		m_cbProd = cb;
	}

	//Set the callback that is called when samples are read from the buffer (consumed)
	inline void setOnConsume(ConsumeCallback *cb) {
		m_cbCons = cb;
	}

	//Get the callback that is called when samples are read from the buffer (consumed)
	inline ConsumeCallback *getOnConsume() const {
		return m_cbCons;
	}

	//Get the number of available samples
	//One sample is (2 * channels) bytes in size
	inline int avail() const {
		return (int)((m_buf.size() - m_readPos) / m_channels);
	}

	//Return the number of channels this buffer was initialized with
	inline int channels() const {
		return m_channels;
	}

	//Return max size as set
	inline size_t maxSize() const {
		return m_maxSize;
	}

	//Place some samples into the buffer
	//samples: The sample buffer
	//count: Number of samples in buffer
	//One sample is (2 * channels) bytes in size
	virtual void produce(const short *samples, int count) override;

	//Consume some samples from the buffer
	//samples: The sample buffer
	//count: Size of buffer measured in Samples
	//eraseConsumed: Erase the consumed samples from the buffer
	//One sample is (2 * channels) bytes in size
	int consume(short *samples, int maxCount, bool eraseConsumed = true);

	//Get size of a sample in bytes
	inline int sampleSize() const {
		return (int)sizeof(short) * m_channels;
	}

	//Directly return bare memory address of the FIRST unconsumed sample.
	//Be careful! Pointer is invalidated by produce() (compaction /
	//reallocation) - only valid while the caller holds the mutex and
	//performs no produce in between.
	inline short *getBufferData() {
		return m_buf.data() + m_readPos;
	}

	inline const std::mutex &getMutex() const {
		return m_mutex;
	}

	inline std::mutex &getMutex() {
		return m_mutex;
	}

	//Clear the buffer
	inline void clear() {
		m_buf.clear();
		m_readPos = 0;
	}

private:
	void compactIfNeeded();

	const int m_channels;
	const size_t m_maxSize;
	std::vector<short> m_buf;
	size_t m_readPos = 0;   // index (in shorts) of first unconsumed sample
	mutable std::mutex m_mutex;
	ProduceCallback *m_cbProd;
	ConsumeCallback *m_cbCons;
};

#endif // rpsbsrc__SampleBuffer_H__
