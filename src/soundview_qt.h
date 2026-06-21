// src/soundview_qt.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__soundview_qt_H__
#define rpsbsrc__soundview_qt_H__

#include <QWidget>
#include <QPainterPath>
#include <QElapsedTimer>
#include <memory>
#include <vector>

#include "SoundInfo.h"
#include "dsp/SandboxState.h"

class QTimer;
class SampleVisualizerThread;

class SoundView : public QWidget
{
	Q_OBJECT

public:
	SoundView(QWidget *parent = NULL);
	~SoundView();
	void setSound(const SoundInfo &sound);
	void setPlaybackPosition(double fraction);
	// Last set playback cursor fraction (0..1). -1.0 if no cursor.
	// Used by the replay path to seek to wherever the user parked the
	// cursor while the channel was stopped.
	double currentPosition() const { return m_playbackPosition; }
	void clearPlayback();
	// Reverse-direction hint for the "played portion" tint: in reverse
	// the already-played region is to the RIGHT of the cursor.
	void setReverse(bool on);
	void setAdaptToFx(bool on);
	void setSandboxState(const SandboxState &s);
	// Live FxPanel state (per-channel simple FX outside the sandbox).
	// pitch/speed in slider units (-100..100, pow(3.0, v/100) factor),
	// reverb 0..100. Fed by the wiring layer when the channel's
	// FxPanel changes so the waveform mirrors what the user just
	// applied without waiting for a fresh sandbox state.
	void setLiveFx(int pitch, int speed, int reverb);
	void notifySeek();

	// Crop markers. The wiring feeds the ACTUAL crop applied to the
	// playing slot (seconds). endSeconds < 0 means "no end point set".
	// Both <= 0 means no crop — nothing is drawn. Total length comes
	// from setTotalLength() (the real decoded duration of the slot).
	void setCropRange(double startSeconds, double endSeconds);
	void setTotalLength(double seconds);
	void setShowCropMarkers(bool on);
	// "Loaded but not playing" affordance: when true, the waveform is
	// painted with reduced alpha + desaturated tint to read as
	// "stopped, ready to replay". PAUSED state is considered PLAYING
	// (cursor frozen mid-stream, audio is loaded into the player) — the
	// wiring only flips this on a true stop, not on pause.
	void setGhosted(bool on);
	bool isGhosted() const { return m_ghosted; }

signals:
	void seekRequested(double fraction);
	// Right-click crop edit. Emitted only when totalLength > 0 (i.e. a
	// sound is playing). Seconds are clamped to [0, totalLength].
	void cropStartRequestedAt(double seconds);
	void cropEndRequestedAt(double seconds);
	void cropClearStartRequested();
	void cropClearEndRequested();
	void cropClearAllRequested();
	// Right-button drag selected a loop area. startSec / endSec are
	// already clamped to [0, totalLength] and ordered (start < end).
	// Receiver sets the crop markers AND turns loop ON.
	void loopAreaSelected(double startSec, double endSec);

protected:
	void paintEvent(QPaintEvent *evt);
	void resizeEvent(QResizeEvent *evt);
	void mousePressEvent(QMouseEvent *evt);
	void mouseMoveEvent(QMouseEvent *evt);
	void mouseReleaseEvent(QMouseEvent *evt);
	void contextMenuEvent(QContextMenuEvent *evt);

private slots:
	void onTimer();
	void onLoadTick();

private:
	void drawWaves(QPainter *painter);
	void preparePaths();
	void applyFxToBins(std::vector<float> &binsL, std::vector<float> &binsR, size_t count) const;
	double fractionFromMouseX(int x) const;
	double clampFractionToCrop(double fraction) const;
	void startStretchLoadAnimation(int durationMs);
	// Reverse-mode pitch/speed/reverb feedback overlay. Same visual
	// machinery as paulstretch but with caller-supplied label template
	// so the user gets "Applying pitch...", "Applying speed..." etc.
	// activeTemplate must contain "%1" — paint substitutes the percent
	// progress. doneText shown for the final 5 % of the animation.
	void startFxLoadAnimation(int durationMs,
	                          const QString &activeTemplate,
	                          const QString &doneText);

private:
	SoundInfo m_soundInfo;
	QTimer *m_timer;
	size_t m_drawnBins;
	QPainterPath m_path[2];
	double m_playbackPosition;
	bool m_dragging;
	bool m_active;
	bool m_adaptToFx = false;
	SandboxState m_sandbox;
	// Live FxPanel state. Defaults = neutral (pitch=0, speed=0,
	// reverb=0). When m_adaptToFx is on these get layered on top of
	// the sandbox visualisation: pitch shifts bin colour cast, speed
	// compresses the rendered x-axis, reverb stretches a fading tail
	// past the end of the played portion.
	int  m_fxPitch  = 0;
	int  m_fxSpeed  = 0;
	int  m_fxReverb = 0;

	QTimer        *m_loadTimer = nullptr;
	QElapsedTimer  m_loadElapsed;
	int            m_loadDurationMs = 0;
	bool           m_loadActive = false;
	// Labels for the loading overlay. m_loadLabelActive must contain
	// "%1" — paintEvent fills in the percent progress. m_loadLabelDone
	// is shown for the final 5 % of the animation. Decoupling the
	// label from paintEvent lets paulstretch AND the reverse-mode
	// FX overlay share the same alpha + progress-bar code path.
	QString        m_loadLabelActive;
	QString        m_loadLabelDone;

	// Crop marker state (fed by the wiring, cleared on stop / sound change).
	double         m_cropStart   = 0.0;
	double         m_cropEnd     = -1.0;
	double         m_totalLength = 0.0;
	bool           m_showCropMarkers = true;
	// Reverse playback direction (affects only the played-portion tint).
	bool           m_reverse = false;
	// "Loaded but stopped" affordance — see setGhosted.
	bool           m_ghosted = false;

	// Per-view waveform analyser. Was a process-wide singleton: two
	// channels loading different files fought over the same bin array
	// and one channel ended up painting the other's waveform.
	std::unique_ptr<SampleVisualizerThread> m_vis;

	// Seamless load: hide the progressive path while the analyser is
	// still appending bins (was making the waveform visibly grow
	// horizontally and then "resize" again when finalizeBins() snapped
	// the bin count to the 1024 target). m_analysisReady flips true on
	// the first onTimer tick after !isRunning(); we then start a short
	// alpha fade-in so the full-resolution waveform appears smoothly.
	bool           m_analysisReady = false;
	bool           m_revealActive  = false;
	QTimer        *m_revealTimer   = nullptr;
	QElapsedTimer  m_revealElapsed;
	static constexpr int kRevealMs = 250;
	// Target bin count we asked the analyser for (matches what we pass
	// to startAnalysis). Used by paintEvent to draw a per-channel
	// loading progress bar (binsProcessed / m_analysisBins). Keeping
	// it here avoids exposing m_numBins from SampleVisualizerThread.
	int            m_analysisBins  = 1024;

	// Right-click crop marker preview. While the context menu is open
	// the click position is rendered as a pulsing semi-transparent
	// vertical line so the user gets immediate spatial confirmation
	// of where the marker is about to land. -1.0 = inactive.
	double         m_ghostMarkerSec   = -1.0;
	bool           m_ghostBlinkVisible = true;
	QTimer        *m_ghostTimer       = nullptr;

	// Drag-preview cursor. While the user holds the mouse down on the
	// waveform the LIVE playback cursor must keep tracking audio (so
	// the user sees where playback actually is), and a SEPARATE
	// translucent cursor follows the finger to show where the seek
	// will land on release. Previously the drag overwrote
	// m_playbackPosition every move while the position-poll restored
	// it 60 times/second — the cursor visibly flickered between the
	// finger and the audio position. -1.0 = inactive.
	double         m_dragPreview = -1.0;

	// Right-button drag for loop-area selection. m_rightDragging is true
	// while the right mouse button is held down. m_rightDragStart and
	// m_rightDragEnd are fractions (0..1). Drag ≥ kRightDragMinPx pixels
	// converts the release into a loop-area emit; below threshold it
	// falls through to the normal contextMenu (existing crop editor).
	bool           m_rightDragging          = false;
	double         m_rightDragStart         = -1.0;
	double         m_rightDragEnd           = -1.0;
	int            m_rightDragStartPx       = 0;
	bool           m_suppressNextContextMenu = false;
	static constexpr int kRightDragMinPx = 6;
};

#endif // rpsbsrc__soundview_qt_H__
