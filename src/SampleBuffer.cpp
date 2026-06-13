// src/SampleBuffer.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#include <algorithm>
#include <cstring>
#include "SampleBuffer.h"


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
SampleBuffer::SampleBuffer( int channels, size_t maxSize /*= 0*/ ) :
	m_channels(channels),
	m_maxSize(maxSize),
	m_cbProd(NULL),
	m_cbCons(NULL)
{

}


//---------------------------------------------------------------
// Purpose: Reclaim the consumed head region. Runs on the PRODUCER
// side (never on the audio thread) and only when the dead head has
// grown past the live payload, so the memmove cost is amortised to
// O(1) per sample over time.
//---------------------------------------------------------------
void SampleBuffer::compactIfNeeded()
{
	if (m_readPos == 0)
		return;
	size_t live = m_buf.size() - m_readPos;
	if (m_readPos < live && m_readPos < 4096)
		return;
	if (live > 0)
		std::memmove(m_buf.data(), m_buf.data() + m_readPos,
		             live * sizeof(short));
	m_buf.resize(live);
	m_readPos = 0;
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SampleBuffer::produce( const short *samples, int count )
{
	if (count <= 0)
		return;
	// Clamp to the configured cap instead of letting a whole batch
	// overshoot it (the old check passed as long as avail() was below
	// max BEFORE the insert, so the buffer could exceed maxSize by up
	// to one full produce batch).
	if (m_maxSize != 0)
	{
		int room = (int)m_maxSize - avail();
		if (room <= 0)
			return;
		if (count > room)
			count = room;
	}
	compactIfNeeded();
	m_buf.insert(m_buf.end(), samples, samples + ((size_t)count * m_channels));
	if(m_cbProd)
		m_cbProd->onProduceSamples(samples, count, this);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
int SampleBuffer::consume( short *samples, int maxCount, bool eraseConsumed )
{
	int count = std::min(avail(), maxCount);
	size_t shorts = (size_t)count * m_channels;
	if(samples)
		memcpy(samples, m_buf.data() + m_readPos, shorts * sizeof(short));
	if(eraseConsumed)
	{
		// O(1): advance the read offset. The producer compacts later.
		m_readPos += shorts;
		if (m_readPos >= m_buf.size())
		{
			m_buf.clear();
			m_readPos = 0;
		}
	}
	if(m_cbCons)
		m_cbCons->onConsumeSamples(samples, count, this);
	return count;
}
