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

class SoundView : public QWidget
{
	Q_OBJECT

public:
	SoundView(QWidget *parent = NULL);
	void setSound(const SoundInfo &sound);
	void setPlaybackPosition(double fraction);
	void clearPlayback();
	void setAdaptToFx(bool on);
	void setSandboxState(const SandboxState &s);
	void notifySeek();

	// Crop markers. The wiring feeds the ACTUAL crop applied to the
	// playing slot (seconds). endSeconds < 0 means "no end point set".
	// Both <= 0 means no crop — nothing is drawn. Total length comes
	// from setTotalLength() (the real decoded duration of the slot).
	void setCropRange(double startSeconds, double endSeconds);
	void setTotalLength(double seconds);
	void setShowCropMarkers(bool on);

signals:
	void seekRequested(double fraction);
	// Right-click crop edit. Emitted only when totalLength > 0 (i.e. a
	// sound is playing). Seconds are clamped to [0, totalLength].
	void cropStartRequestedAt(double seconds);
	void cropEndRequestedAt(double seconds);
	void cropClearStartRequested();
	void cropClearEndRequested();
	void cropClearAllRequested();

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
	void startStretchLoadAnimation(int durationMs);

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

	QTimer        *m_loadTimer = nullptr;
	QElapsedTimer  m_loadElapsed;
	int            m_loadDurationMs = 0;
	bool           m_loadActive = false;

	// Crop marker state (fed by the wiring, cleared on stop / sound change).
	double         m_cropStart   = 0.0;
	double         m_cropEnd     = -1.0;
	double         m_totalLength = 0.0;
	bool           m_showCropMarkers = true;
};

#endif // rpsbsrc__soundview_qt_H__
