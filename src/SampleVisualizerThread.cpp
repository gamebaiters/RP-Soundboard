// src/SampleVisualizerThread.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "inputfile.h"
#include "SampleVisualizerThread.h"
#include "SampleBuffer.h"

// Bumped 32 K -> 512 K samples per outer iteration. The old value
// processed ~0.74 s of 44.1 kHz audio per cycle, so a 3-minute file
// needed ~250 outer iterations and at the previous 1 ms inter-cycle
// sleep that meant 250 ms of cumulative sleep on TOP of the decode
// time - the "waveform is too slow to load" complaint. 512 K = ~11.6 s
// per cycle, so the same 3-minute file fits in ~16 iterations end to
// end. Single-cycle peak heap usage is unchanged (the per-bin temp
// buffer scales with samplesPerBin, not the iteration size); only
// the loop overhead shrinks.
#define MIN_SAMPLES_PER_ITERATION (1024 * 512)
#define SAMPLE_RATE 44100;


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SampleVisualizerThread::SampleBufferSynced::SampleBufferSynced(int channels, size_t maxSize) :
	SampleBuffer(channels, maxSize)
{}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::SampleBufferSynced::produce(const short *samples, int count)
{
	SampleBuffer::Lock l(getMutex());
	SampleBuffer::produce(samples, count);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
double SampleVisualizerThread::fileLength() const
{
	uint64_t samples = m_running.load(std::memory_order_acquire)
		? m_numSamplesTotalEst : m_numSamplesProcessed;
	return double(samples) / SAMPLE_RATE;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SampleVisualizerThread::SampleVisualizerThread() :
	m_buffer(1),
	m_numBins(0),
	m_numBinsProcessed(0),
	m_numSamplesProcessed(0),
	m_numSamplesTotalEst(0),
	m_numSamplesProcessedThisBin(0),
	m_file(NULL),
	m_pendingNumBins(0),
	m_running(false),
	m_newFile(false),
	m_stop(false),
	m_previewReady(false)
{

}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SampleVisualizerThread::~SampleVisualizerThread()
{
	stop();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::startAnalysis( const char *filename, size_t numBins )
{
	// Two-step: park the request as "pending" then signal the worker.
	// The brief mutex hold here NEVER overlaps the worker's slow file
	// I/O — the worker drops the mutex around readSamples() so the GUI
	// thread never blocks for longer than a memcpy of the filename and
	// a vector .clear(). Previously the worker held m_mutex through the
	// entire 512 K-sample decode cycle (~hundreds of ms on huge files),
	// so loading a 2nd audio mid-analysis hung the soundboard.
	bool needSpawn = false;
	{
		Lock lock(m_mutex);
		m_pendingFilename = filename;
		m_pendingNumBins  = numBins;
		// Pre-clear & reserve while we hold the mutex so the worker
		// finds a ready buffer. The worker re-validates against
		// m_newFile inside its own brief critical sections, so an
		// in-flight push_back from the old file is impossible the
		// moment m_newFile flips true (worker checks atomically before
		// every push_back).
		m_bins.clear();
		m_bins.reserve(numBins * 32 + 16);
		m_numBinsProcessed.store(0, std::memory_order_release);
		// The DSP preview belongs to the OLD file - drop it now so a GUI
		// re-render between here and the worker's openNewFile can never
		// render the previous sound's audio under the new file's bins.
		m_previewReady.store(false, std::memory_order_release);
		m_preview.clear();
		m_previewAcc  = 0.0;
		m_previewAccN = 0;
		m_newFile.store(true, std::memory_order_release);

		if (!m_running.load(std::memory_order_acquire))
		{
			needSpawn = true;
			m_running.store(true, std::memory_order_release);
			m_stop.store(false, std::memory_order_release);
		}
	}
	if (needSpawn)
	{
		// The worker self-terminates when its file is done. Join the
		// finished thread object before reusing the member.
		if (m_thread.joinable())
			m_thread.join();
		std::thread t(&SampleVisualizerThread::threadFunc, this);
		m_thread = std::move(t);
	}
	// Wake worker if it's sleeping between cycles.
	m_cv.notify_all();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::stop( bool wait /*= true*/ )
{
	m_stop.store(true, std::memory_order_release);
	m_cv.notify_all();
	if(wait && m_thread.joinable())
		m_thread.join();
}


//---------------------------------------------------------------
// Purpose: shutdown-only bounded variant. The thread checks m_stop
// between batches but the in-flight FFmpeg readSamples can take
// 100s of ms on a long file. Cap the wait so DLL unload is not
// held hostage; TerminateThread is the lesser evil compared to a
// zombie process.
//---------------------------------------------------------------
void SampleVisualizerThread::stopBounded(int timeoutMs)
{
	m_stop.store(true, std::memory_order_release);
	m_cv.notify_all();
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
bool SampleVisualizerThread::isRunning() const
{
	return m_running.load(std::memory_order_acquire);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
size_t SampleVisualizerThread::getBinsProcessed() const
{
	return m_numBinsProcessed.load(std::memory_order_acquire);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::run()
{
	while(!m_stop.load(std::memory_order_acquire))
	{
		// Phase 1 — brief mutex acquisition to consume any pending
		// startAnalysis() request and snapshot the worker-owned file.
		// Releasing the mutex BEFORE the slow decode loop is what
		// unblocks GUI swaps mid-analysis.
		InputFile *file = nullptr;
		{
			Lock lock(m_mutex);
			if (m_newFile.load(std::memory_order_acquire))
			{
				// Adopt the GUI's request. openNewFile() reads
				// m_filename / m_numBins (worker-owned snapshots) so
				// copy from the pending fields first.
				m_filename = m_pendingFilename;
				m_numBins  = m_pendingNumBins;
				m_numBinsProcessed.store(0, std::memory_order_release);
				m_numSamplesProcessed = 0;
				m_numSamplesTotalEst = 0;
				m_numSamplesProcessedThisBin = 0;
				{
					// Drain leftover samples from the previous file.
					// SampleBufferSynced::produce locks getMutex(); the
					// consume side is unsynchronised so we take the same
					// lock here to keep the read offset / vector
					// consistent with concurrent produce calls (though
					// the file has been closed by openNewFile below, the
					// lock costs nothing and matches processSamples()).
					SampleBuffer::Lock sbl(m_buffer.getMutex());
					m_buffer.consume(nullptr, m_buffer.avail());
				}
				// startAnalysis() already cleared / reserved m_bins.
				// openNewFile() handles m_file teardown.
				openNewFile();
				m_newFile.store(false, std::memory_order_release);
			}
			file = m_file;
		}

		if (!file)
		{
			// Idle: wait on condvar until startAnalysis() pings us or
			// stop is requested. Self-terminate so an idle view costs
			// no thread.
			std::unique_lock<std::mutex> lk(m_mutex);
			if (!m_newFile.load(std::memory_order_acquire)
			    && !m_stop.load(std::memory_order_acquire))
			{
				m_running.store(false, std::memory_order_release);
				return;
			}
			continue;
		}

		// Phase 2 — decode batch WITHOUT holding m_mutex. Re-check
		// m_newFile between sub-batches so a GUI swap preempts within
		// ~one readSamples() call (~5-50 ms depending on codec).
		int readSamplesThisIt = 0;
		bool justFinished = false;
		bool preempted = false;
		while (readSamplesThisIt < MIN_SAMPLES_PER_ITERATION
		       && !m_stop.load(std::memory_order_acquire))
		{
			// Preempt: GUI requested a new file mid-batch. Drop the
			// in-flight decode and let the outer loop pick up the new
			// request. The brief lock below also closes/destroys the
			// preempted file so no leak.
			if (m_newFile.load(std::memory_order_acquire))
			{
				Lock lock(m_mutex);
				if (m_file == file && m_file)
				{
					m_file->close();
					delete m_file;
					m_file = nullptr;
				}
				preempted = true;
				break;
			}

			int samples = file->readSamples(&m_buffer);

			// Brief lock: produce bins + check if file done. Worker is
			// the only writer to m_bins / m_file so the lock just
			// serialises against the GUI's startAnalysis() clear.
			Lock lock(m_mutex);
			if (m_newFile.load(std::memory_order_acquire))
			{
				if (m_file == file && m_file)
				{
					m_file->close();
					delete m_file;
					m_file = nullptr;
				}
				preempted = true;
				break;
			}
			if (samples > 0)
			{
				readSamplesThisIt += samples;
				processSamples(samples);
			}
			if (samples <= 0 || file->done())
			{
				file->close();
				delete file;
				m_file = nullptr;
				file = nullptr;
				justFinished = true;
				break;
			}
		}

		if (preempted)
			continue;

		// EOF: normalise m_bins to exactly m_numBins. The decoder's
		// actual sample count routinely diverges from
		// outputSamplesEstimation() (VBR MP3 estimation lies, MP3 / AAC
		// encoder padding overshoots, some malformed containers
		// undershoot) - which left the user with two visible bugs:
		//   * overshoot (estimation > actual): fewer bins than 1024
		//     generated → right edge of the waveform was blank dead
		//     space and the silence built into the file end fell
		//     entirely inside the drawn region.
		//   * undershoot (estimation < actual): MORE than 1024 bins
		//     generated → tail bins drawn past the widget's right edge
		//     and clipped off-screen.
		// Resampling here keeps the GUI's hardcoded "i / 1024 * fw"
		// drawing math correct for every file.
		if (justFinished)
		{
			Lock lock(m_mutex);
			finalizeBins();
		}
	}
	m_running.store(false, std::memory_order_release);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SampleVisualizerThread::threadFunc()
{
	run();
	m_running.store(false, std::memory_order_release);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::openNewFile()
{
	// Caller has cleared m_newFile already (after copying pending
	// filename → m_filename); do NOT reset it here, that would race
	// against a pending swap that arrived between the snapshot and the
	// open() call.
	if(m_file)
	{
		m_file->close();
		delete m_file;
		m_file = nullptr;
	}
	InputFileOptions options;
	options.outputChannelLayout = InputFileOptions::MONO;
	options.outputSampleRate = SAMPLE_RATE;
	m_file = CreateInputFileFFmpeg(options);
	if(m_file->open(m_filename.c_str()) == 0)
		m_numSamplesTotalEst = m_file->outputSamplesEstimation();
	else
	{
		delete m_file;
		m_file = NULL;
	}

	// Pick the preview decimation. Target ~11 kHz (decode is mono 44.1 kHz),
	// which keeps EQ bands up to ~5 kHz honest; longer files decimate more so
	// the cache never exceeds kMaxPreviewFrames.
	m_preview.clear();
	m_previewAcc  = 0.0;
	m_previewAccN = 0;
	m_previewDecim = 4;
	if (m_numSamplesTotalEst > 0)
	{
		const int64_t need = (m_numSamplesTotalEst + (int64_t)kMaxPreviewFrames - 1)
		                     / (int64_t)kMaxPreviewFrames;
		if (need > m_previewDecim) m_previewDecim = (int)need;
		m_preview.reserve((size_t)(m_numSamplesTotalEst / m_previewDecim) + 8);
	}
	m_previewRate = 44100.0 / (double)m_previewDecim;
}


//---------------------------------------------------------------
// Purpose: box-average the decoded mono block down into the preview
// cache. A box average (not plain picking) is a crude low-pass, which
// keeps the decimated copy free of the alias garbage that would
// otherwise land right in the band the EQ preview is trying to show.
//---------------------------------------------------------------
void SampleVisualizerThread::accumulatePreview(const short *data, size_t count)
{
	if (m_previewDecim < 1) return;
	for (size_t i = 0; i < count; ++i)
	{
		m_previewAcc += (double)data[i];
		if (++m_previewAccN >= m_previewDecim)
		{
			m_preview.push_back((float)(m_previewAcc / m_previewAccN / 32768.0));
			m_previewAcc  = 0.0;
			m_previewAccN = 0;
			if (m_preview.size() >= kMaxPreviewFrames)
				halvePreview();
		}
	}
	// Publish EARLY and keep publishing: the cache always covers exactly the
	// same prefix of the file as the bins do, so a partial cache maps onto the
	// partial bin range without any x-axis skew. Waiting for EOF meant one
	// missed finalize (preempted analysis, odd container, stopped decode) left
	// the FX view permanently dead.
	if (m_preview.size() >= 256)
		m_previewReady.store(true, std::memory_order_release);
}


//---------------------------------------------------------------
// Purpose: the cache hit its ceiling - halve it in place (average
// adjacent frames) and double the decimation instead of truncating.
// Truncating would have made the cache cover only the FIRST part of the
// file while the bins covered all of it, which skews every x mapping
// built on top. This way the cache ALWAYS spans the same range as the
// bins, at whatever rate fits the budget - and it works even when the
// container reports no duration at all.
//---------------------------------------------------------------
void SampleVisualizerThread::halvePreview()
{
	const size_t n = m_preview.size() / 2;
	for (size_t i = 0; i < n; ++i)
		m_preview[i] = 0.5f * (m_preview[i * 2] + m_preview[i * 2 + 1]);
	// An odd trailing frame is half a decimated frame of audio - dropping it
	// is below the resolution of anything drawn from this cache.
	m_preview.resize(n);
	m_previewDecim *= 2;
	m_previewRate = 44100.0 / (double)m_previewDecim;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SampleVisualizerThread::processSamples(size_t newSamples)
{
	while(true)
	{
		size_t samplesPerBin = (size_t)(m_numSamplesTotalEst / (int64_t)m_numBins);
		if(m_numSamplesProcessedThisBin == 0)
		{
			m_min = std::numeric_limits<short>::max();
			m_max = std::numeric_limits<short>::min();
		}
		SampleBuffer::Lock sbl(m_buffer.getMutex());
		size_t numSamplesThisIt = std::min((size_t)m_buffer.avail(), samplesPerBin - m_numSamplesProcessedThisBin);
		if(numSamplesThisIt == 0)
			break;
		getMinMax(m_buffer.getBufferData(), numSamplesThisIt, m_min, m_max);
		accumulatePreview(m_buffer.getBufferData(), numSamplesThisIt);
		m_buffer.consume(NULL, numSamplesThisIt);
		m_numSamplesProcessedThisBin += numSamplesThisIt;
		if(m_numSamplesProcessedThisBin >= samplesPerBin)
		{
			m_bins.push_back(m_min);
			m_bins.push_back(m_max);
			m_numSamplesProcessedThisBin = 0;
			// release-store: GUI reads getBinsProcessed() lock-free
			// then dereferences getBins() up to that count, so the
			// push_back writes above must be visible before the count
			// is bumped.
			m_numBinsProcessed.store(
				m_numBinsProcessed.load(std::memory_order_relaxed) + 1,
				std::memory_order_release);
		}
	}
}


//---------------------------------------------------------------
// Purpose: after EOF, resize the bin array to exactly m_numBins so
// the GUI's hardcoded "i / numBins * fw" mapping always lands the
// right edge of the waveform at the right edge of the widget. Both
// the over- and undershoot cases are user-visible:
//   * fewer source bins than target → linearly interpolate across
//     groups, then pad any remaining tail with zero bins so the
//     genuine end-of-file silence shows up as a flat line.
//   * more source bins than target → average the per-target source
//     group's [min,max] into one bin per target slot.
// We deliberately avoid changing the buffer's underlying capacity:
// shrinks via resize() never realloc, the GUI's lock-free pointer
// read stays valid.
//---------------------------------------------------------------
void SampleVisualizerThread::finalizeBins()
{
	// Flush the partial preview frame. The cache is published progressively
	// (accumulatePreview) so this only tops it up with the final partial
	// frame; it must run before any early-return below.
	if (m_previewAccN > 0)
		m_preview.push_back((float)(m_previewAcc / m_previewAccN / 32768.0));
	m_previewAcc  = 0.0;
	m_previewAccN = 0;
	if (!m_preview.empty())
		m_previewReady.store(true, std::memory_order_release);

	const size_t target = m_numBins;
	if (target == 0) return;
	const size_t srcBins = m_bins.size() / 2;
	if (srcBins == target) {
		m_numBinsProcessed.store(target, std::memory_order_release);
		return;
	}

	std::vector<int> out;
	out.resize(target * 2);

	if (srcBins == 0) {
		// Whole file silent or decode failed - flat line everywhere.
		for (size_t i = 0; i < target * 2; ++i) out[i] = 0;
	}
	else if (srcBins > target) {
		// Downsample: each target bin absorbs srcBins/target source bins.
		// Use floor() spacing + last-bin catch so no source bin is dropped
		// and the silence-at-end always lands in the right target bin.
		for (size_t i = 0; i < target; ++i) {
			size_t s0 = (size_t)((double)i      / target * srcBins);
			size_t s1 = (size_t)((double)(i + 1) / target * srcBins);
			if (s1 <= s0) s1 = s0 + 1;
			if (s1 > srcBins) s1 = srcBins;
			int mn = m_bins[s0 * 2];
			int mx = m_bins[s0 * 2 + 1];
			for (size_t s = s0 + 1; s < s1; ++s) {
				if (m_bins[s * 2]     < mn) mn = m_bins[s * 2];
				if (m_bins[s * 2 + 1] > mx) mx = m_bins[s * 2 + 1];
			}
			out[i * 2]     = mn;
			out[i * 2 + 1] = mx;
		}
	}
	else {
		// Upsample: estimation overshot the real audio length. Map each
		// target bin to its source bin via nearest-neighbour; trailing
		// target bins past the source range fall to zero (silence) so
		// the cursor's end-of-file region is visually correct.
		for (size_t i = 0; i < target; ++i) {
			double srcFrac = (double)i / target * srcBins;
			size_t s = (size_t)srcFrac;
			if (s >= srcBins) {
				out[i * 2]     = 0;
				out[i * 2 + 1] = 0;
			} else {
				out[i * 2]     = m_bins[s * 2];
				out[i * 2 + 1] = m_bins[s * 2 + 1];
			}
		}
	}

	// In-place swap into the reserved buffer so the underlying pointer
	// stays valid for any concurrent GUI read. resize() to the same
	// capacity is a no-op; assigning element-wise then resize() to
	// target*2 leaves m_bins.data() unchanged because target*2 ≤
	// reserve(numBins * 32 + 16).
	m_bins.resize(target * 2);
	for (size_t i = 0; i < target * 2; ++i)
		m_bins[i] = out[i];
	m_numBinsProcessed.store(target, std::memory_order_release);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SampleVisualizerThread::getMinMax( const short *data, size_t count, int &min, int &max )
{

	for(size_t i = 0; i < count; ++i)
	{
		min = std::min(min, (int)data[i]);
		max = std::max(max, (int)data[i]);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
volatile const int * SampleVisualizerThread::getBins() const
{
	return m_bins.data();
}


//---------------------------------------------------------------
// Purpose: hand the GUI a copy of the decimated decoded audio so it can
// run the real DSP chain over it. Copy (not a pointer) because the
// worker owns the vector and can reallocate it on the next file.
//---------------------------------------------------------------
bool SampleVisualizerThread::getPreviewAudio(std::vector<float> &out, double &sampleRate) const
{
	if (!m_previewReady.load(std::memory_order_acquire))
		return false;
	Lock lock(m_mutex);
	if (m_preview.empty() || m_previewRate <= 0.0)
		return false;
	out = m_preview;
	sampleRate = m_previewRate;
	return true;
}





