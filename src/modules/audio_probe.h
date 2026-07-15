// src/modules/audio_probe.h
//----------------------------------
// Cheap header-only probe of a LOCAL audio file: container, sample rate, bit
// depth (lossless) or bitrate (lossy). Drives the format badge the channel
// shows before the filename ("FLAC 16bit/44.1kHz"), the local-file counterpart
// of the WEB badge a stream gets.
//
// Reads only the header (small probesize / analyzeduration) and memoises the
// result per path+size+mtime, so calling it from the GUI thread when a channel
// loads a sound costs microseconds after the first hit. Never probes a URL -
// callers must pass a real file (a network source shows the WEB badge instead).
//----------------------------------

#pragma once
#ifndef rpsbsrc__audio_probe_H__
#define rpsbsrc__audio_probe_H__

#include <QString>
#include <QColor>

struct AudioProbeInfo
{
	bool    valid       = false;
	QString format;              // "FLAC", "MP3", "WAV", ... (uppercase)
	int     sampleRate  = 0;     // Hz
	int     bits        = 0;     // bit depth, >0 only for lossless
	int     channels    = 0;
	int     bitrateKbps = 0;     // >0 for lossy
	bool    lossless    = false;

	// Short human badge: "FLAC 16bit/44.1kHz", "MP3 320kbps/44.1kHz",
	// "OPUS 48kHz". Empty when !valid.
	QString badge() const;

	// The two halves of badge(), for the "format only" / "quality only"
	// badge-complexity settings. formatBadge() == "FLAC"; qualityBadge()
	// == "16bit/44.1kHz" (lossless) / "320kbps/44.1kHz" (lossy) /
	// "48kHz" (rate only). Empty when !valid.
	QString formatBadge() const;
	QString qualityBadge() const;

	// Coarse quality tier, 0 (lowest lossy) .. 4 (hi-res lossless).
	// Drives qualityColor().
	int qualityRank() const;

	// Fixed per-format hue (FLAC green, MP3 orange, OGG purple, ...) so
	// the badge is recognisable at a glance. Unknown formats get slate.
	QColor formatColor() const;
	// Quality-tier colour: gold (hi-res lossless), green (lossless),
	// teal (high lossy), orange (mid lossy), grey-red (low lossy).
	QColor qualityColor() const;
};

namespace AudioProbe {

// Header probe of `path`. Returns an invalid info for a missing file, a URL,
// or anything with no audio stream. Result is cached (path + size + mtime).
AudioProbeInfo probe(const QString &path);

}

#endif // rpsbsrc__audio_probe_H__
