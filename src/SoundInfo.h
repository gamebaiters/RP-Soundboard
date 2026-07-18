// src/SoundInfo.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__SoundInfo_H__
#define rpsbsrc__SoundInfo_H__

#include <QSettings>
#include <QColor>
#include <QByteArray>
#include <stdexcept>

class SoundInfo
{
public:
	SoundInfo();
	void readFromConfig(const QSettings &settings);
	void saveToConfig(QSettings &settings) const;
	double getStartTime() const;
	double getPlayTime() const;

	static double getTimeUnitFactor(int unit);
	// A default-constructed QColor is invalid yet reports alpha()==255,
	// so a never-set customColor would read as "enabled". Gate on
	// isValid() first (matches SoundButton's own check).
	bool customColorEnabled() const { return customColor.isValid() && customColor.alpha() != 0; }
	void setCustomColorEnabled(bool enabled) { customColor.setAlpha(enabled ? 255 : 0); }

public:
	QString filename;
	QString customText;
	QColor customColor;
	int volume;
	bool cropEnabled;
	int cropStartValue;
	int cropStartUnit;
	int cropStopAfterAt;
	int cropStopValue;
	int cropStopUnit;
	float fxPitchSpeed;   // legacy (kept for config compat)
	bool fxRemember;
	int fxPitch;          // -100..100 slider value
	int fxSpeed;          // -100..100 slider value
	int fxReverb;         // 0..100 slider value
	bool fxSyncPitchSpeed;

	// Macro: triggering the button restores the JSON ChannelState in
	// macroState instead of playing a sound.
	bool isMacro;
	QByteArray macroState;

	// Optional background image for the button. Painted stretched over
	// the whole button face; the label gets a translucent black backdrop
	// in the center so it stays readable on top of any image.
	QString imagePath;

	// Play the sample back-to-front when ON. Filter chain adds the
	// FFmpeg `areverse` filter, which buffers the whole stream then
	// outputs reversed. Intended for short SFX cells - long files
	// will allocate memory proportional to length.
	bool reverse;

	// EBU R128 loudness normalisation per-cell. When ON the FFmpeg
	// `loudnorm` filter runs in single-pass mode and brings the cell
	// to -16 LUFS integrated with a -1 dBTP peak ceiling. Stops loud
	// memes from drowning out quiet stings without per-cell volume
	// tweaking.
	bool autoNormalize;

	// --- URL / YouTube live-streaming cell (v2.3.1) ---------------------
	// When true, `filename` holds a CANONICAL page URL (youtube.com/watch,
	// youtu.be, or any http[s] media page) rather than a local path. At play
	// time StreamResolver turns it into a fresh direct CDN URL (googlevideo
	// URLs expire ~6 h) which is streamed by FFmpeg — never downloaded whole.
	bool isStreamUrl;
	// When true (and isStreamUrl), `filename` is a PLAYLIST page URL. Triggering
	// the cell opens the whole playlist into a channel (playlist panel + autoplay
	// chaining) instead of playing a single video. Persisted.
	bool isPlaylist = false;
	// Cached human title + total length from the last resolve, so the cell can
	// show a proper label/timeline before a re-resolve completes. Persisted.
	QString streamTitle;
	double  streamDurationSec;

	// Mandatory temporary channel: every trigger of this button spawns a
	// fresh throw-away channel (glowing border) that removes itself the
	// moment its playback finishes or is stopped - unlimited overlapping
	// instances. Persisted.
	bool tempChannel = false;

	// Full per-button Audio Sandbox package. When sandboxRemember is on,
	// sandboxState holds a compact-JSON SandboxState that is pushed onto
	// whatever channel this button plays into (same idea as fxRemember
	// but for the whole 21-stage chain). Persisted.
	bool       sandboxRemember = false;
	QByteArray sandboxState;

	// --- TRANSIENT resolved-play parameters (NOT serialized) ------------
	// The wiring fills these on the SoundInfo COPY it hands to the Sampler for
	// an actual stream play: `filename` is swapped to the resolved direct URL
	// and these carry the CDN request context down to InputFileFFmpeg::open.
	// Empty on ordinary local-file cells.
	QString netUserAgent;
	QString netHeaders;   // CRLF-joined, no User-Agent
	// True when the resolved source is a LIVE stream (is_live): no waveform, no
	// seek/reverse/vinyl/paulstretch, link-only save. Set by the wiring from
	// ResolvedStream::isLive on the play copy. Transient.
	bool    isLive = false;
};

#endif // rpsbsrc__SoundInfo_H__
