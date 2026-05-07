// src/soundview_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#include <QPainter>
#include <QTimer>
#include <QMouseEvent>
#include "soundview_qt.h"
#include "modules/theme.h"
#include "SampleVisualizerThread.h"


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
SoundView::SoundView( QWidget *parent /*= NULL*/ ) :
	QWidget(parent),
	m_timer(new QTimer(this)),
	m_drawnBins(0),
	m_playbackPosition(-1.0),
	m_dragging(false),
	m_active(false)
{
	connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimer()));
	setMinimumHeight(24);
	setCursor(Qt::PointingHandCursor);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::paintEvent(QPaintEvent *evt)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	// Background + frame derive from the active theme so the waveform pane
	// retints with the rest of the UI when the user picks a custom color.
	Theme::Colors tc = Theme::colors();
	QColor bgFill, bgFrame, playedTint;
	if (tc.enabled) {
		Theme::Derived d = Theme::derive(tc);
		bgFill   = d.surfaceAlt;
		bgFrame  = d.border;
		playedTint = QColor(d.accent.red(), d.accent.green(), d.accent.blue(), 50);
	} else {
		bgFill   = QColor(25, 25, 30);
		bgFrame  = QColor(50, 50, 50);
		playedTint = QColor(0, 120, 215, 40);
	}
	painter.setPen(bgFrame);
	painter.setBrush(bgFill);
	painter.drawRect(QRect(0, 0, width() - 1, height() - 1));

	// Draw played portion background
	if (m_playbackPosition > 0.0 && m_playbackPosition <= 1.0)
	{
		int posX = (int)(m_playbackPosition * (width() - 1));
		painter.setPen(Qt::NoPen);
		painter.setBrush(playedTint);
		painter.drawRect(1, 1, posX - 1, height() - 2);
	}

	// Draw waveform - tinted by Theme::colors().waveform when the user's
	// custom theme is enabled. Default falls back to the original cyan.
	{
		Theme::Colors tc = Theme::colors();
		QColor wave = tc.enabled ? tc.waveform : QColor(0, 180, 255);
		QColor fill(wave.red(), wave.green(), wave.blue(), 180);
		painter.setPen(wave);
		painter.setBrush(fill);
	}
	drawWaves(&painter);

	// Draw crop region overlay (from SoundInfo)
	double songLength = SampleVisualizerThread::GetInstance().fileLength();
	if (songLength > 0.0)
	{
		double start = m_soundInfo.getStartTime();
		double playTime = m_soundInfo.getPlayTime();
		double end = (playTime > 0.0) ? (start + playTime) : songLength;
		int startPixel = int(start / songLength * (width() - 1));
		int endPixel = int(end / songLength * (width() - 1));

		// Dim regions outside crop
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0, 0, 0, 150));
		if (start > 0.0)
			painter.drawRect(0, 0, startPixel, height() - 1);
		if (end < songLength)
			painter.drawRect(endPixel + 1, 0, width() - 1 - (endPixel + 1), height() - 1);
	}

	// Draw position cursor
	if (m_playbackPosition >= 0.0 && m_playbackPosition <= 1.0)
	{
		int posX = (int)(m_playbackPosition * (width() - 1));
		painter.setPen(QPen(QColor(255, 200, 0), 2));
		painter.drawLine(posX, 0, posX, height() - 1);
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::resizeEvent(QResizeEvent *evt)
{
	// Invalidate drawings
	m_drawnBins = 0;
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::setSound( const SoundInfo &sound )
{
	bool filenameDiffers = m_soundInfo.filename != sound.filename;
	m_soundInfo = sound;
	m_drawnBins = 0;
	m_playbackPosition = 0.0;
	m_active = true;
	if(filenameDiffers && !sound.filename.isEmpty())
	{
		SampleVisualizerThread::GetInstance().startAnalysis(sound.filename.toUtf8(), 1024);
		m_timer->start(100);
	}
	else
	{
		update();
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::setPlaybackPosition(double fraction)
{
	m_playbackPosition = fraction;
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::clearPlayback()
{
	m_active = false;
	m_playbackPosition = -1.0;
	m_drawnBins = 0;
	m_path[0] = QPainterPath();
	m_path[1] = QPainterPath();
	m_soundInfo = SoundInfo();
	m_timer->stop();
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::onTimer()
{
	SampleVisualizerThread &t = SampleVisualizerThread::GetInstance();
	if (!t.isRunning() && m_drawnBins >= t.getBinsProcessed())
		m_timer->stop();
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::drawWaves(QPainter *painter)
{
	if (!m_active)
		return;

	preparePaths();

	painter->drawPath(m_path[0]);
	painter->drawPath(m_path[1]);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::preparePaths()
{
	SampleVisualizerThread &t = SampleVisualizerThread::GetInstance();
	size_t bins = t.getBinsProcessed();
	if(m_drawnBins < bins)
	{
		double fhh = (double)height() * 0.5;
		double fw = (double)width();
		double fbins = (double)bins;
		double shortScale = 1.0 / ((double)std::numeric_limits<short>::max() * 1.1);
		m_path[0] = QPainterPath(QPointF(0.0, fhh));
		m_path[1] = QPainterPath(QPointF(0.0, fhh));
		for(size_t i = 0; i < bins; ++i)
		{
			double binValue0 = (double)t.getBins()[i * 2] * shortScale;
			double binValue1 = (double)t.getBins()[i * 2 + 1] * shortScale;
			double x = (double)i * (1.0 / 1024.0) * fw;
			m_path[0].lineTo(x, (1.0 + binValue0) * fhh);
			m_path[1].lineTo(x, (1.0 + binValue1) * fhh);
		}
		double endx = fw * fbins * (1.0 / 1024.0);
		m_path[0].lineTo(endx, fhh);
		m_path[1].lineTo(endx, fhh);
		m_path[0].closeSubpath();
		m_path[1].closeSubpath();
		m_drawnBins = bins;
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
double SoundView::fractionFromMouseX(int x) const
{
	if (width() <= 1)
		return 0.0;
	double fraction = (double)x / (double)(width() - 1);
	if (fraction < 0.0) fraction = 0.0;
	if (fraction > 1.0) fraction = 1.0;
	return fraction;
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::mousePressEvent(QMouseEvent *evt)
{
	if (evt->button() == Qt::LeftButton && m_playbackPosition >= 0.0)
	{
		m_dragging = true;
		double frac = fractionFromMouseX(evt->x());
		m_playbackPosition = frac;
		update();
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::mouseMoveEvent(QMouseEvent *evt)
{
	if (m_dragging)
	{
		double frac = fractionFromMouseX(evt->x());
		m_playbackPosition = frac;
		update();
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::mouseReleaseEvent(QMouseEvent *evt)
{
	if (m_dragging)
	{
		m_dragging = false;
		double frac = fractionFromMouseX(evt->x());
		emit seekRequested(frac);
	}
}
