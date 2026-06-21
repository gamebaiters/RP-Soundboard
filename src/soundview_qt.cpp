// src/soundview_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#include <QPainter>
#include "AudioUtils.h"
#include <QTimer>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include "soundview_qt.h"
#include "modules/theme.h"
#include "SampleVisualizerThread.h"

#include <cmath>
#include <algorithm>


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
	m_loadTimer = new QTimer(this);
	m_loadTimer->setInterval(40);
	connect(m_loadTimer, &QTimer::timeout, this, &SoundView::onLoadTick);
	setMinimumHeight(24);
	setCursor(Qt::PointingHandCursor);
}


SoundView::~SoundView()
{
	if (m_vis)
		m_vis->stopBounded(200);
}


void SoundView::setReverse(bool on)
{
	if (m_reverse == on) return;
	m_reverse = on;
	update();
}


void SoundView::setGhosted(bool on)
{
	if (m_ghosted == on) return;
	m_ghosted = on;
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::paintEvent(QPaintEvent *evt)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, false);

	Theme::Colors tc = Theme::colors();
	QColor bgFill, bgFrame, playedTint;
	if (tc.enabled) {
		// Memoised - paintEvent fires at up to 30 Hz per channel and
		// derive() is a few dozen HSL conversions.
		const Theme::Derived &d = Theme::derivedCached();
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

	// Cursor / played-tint clamp range. Without this the cursor visually
	// escapes past the start/end crop markers when speed or pitch is
	// modified - the position fraction is computed against the full file
	// length (so the cursor maps 0..fullLen onto 0..widget-width), but
	// the crop overlay only allows the active region to be visible. The
	// cursor must not paint outside [cropStart, cropEnd]. Compute once
	// here and reuse for both the played-tint and the cursor line.
	int cursorMinX = 0;
	int cursorMaxX = width() - 1;
	if (m_showCropMarkers && m_totalLength > 0.0)
	{
		const int w1 = width() - 1;
		double clipStart = (m_cropStart > 0.0) ? m_cropStart : 0.0;
		double clipEnd   = (m_cropEnd   > 0.0) ? m_cropEnd   : m_totalLength;
		if (clipStart > m_totalLength) clipStart = m_totalLength;
		if (clipEnd   > m_totalLength) clipEnd   = m_totalLength;
		cursorMinX = int(clipStart / m_totalLength * w1);
		cursorMaxX = int(clipEnd   / m_totalLength * w1);
		if (cursorMinX < 0) cursorMinX = 0;
		if (cursorMaxX > w1) cursorMaxX = w1;
	}

	// Draw played portion background. Forward playback: tint from the
	// left edge (crop start) up to the cursor. Reverse playback: the
	// already-played region is to the RIGHT of the cursor (the cursor
	// walks right-to-left), so tint cursor -> crop end instead.
	if (m_playbackPosition > 0.0 && m_playbackPosition <= 1.0)
	{
		int posX = (int)(m_playbackPosition * (width() - 1));
		if (posX > cursorMaxX) posX = cursorMaxX;
		if (posX < cursorMinX) posX = cursorMinX;
		painter.setPen(Qt::NoPen);
		painter.setBrush(playedTint);
		if (m_reverse) {
			int x1 = std::min(width() - 2, cursorMaxX);
			if (x1 > posX)
				painter.drawRect(posX, 1, x1 - posX, height() - 2);
		} else {
			int x0 = std::max(1, cursorMinX);
			if (posX > x0)
				painter.drawRect(x0, 1, posX - x0, height() - 2);
		}
	}

	// Draw waveform
	{
		Theme::Colors tc = Theme::colors();
		QColor wave = tc.enabled ? tc.waveform : QColor(0, 180, 255);
		QColor fill(wave.red(), wave.green(), wave.blue(), 180);
		painter.setPen(wave);
		painter.setBrush(fill);
	}
	drawWaves(&painter);

	// Draw crop region overlay + start/end markers. Fed by the wiring
	// with the ACTUAL crop applied to the slot, so it survives looping
	// and is cleared automatically on stop / sound change.
	if (m_showCropMarkers && m_totalLength > 0.0)
	{
		double start = (m_cropStart > 0.0) ? m_cropStart : 0.0;
		double end   = (m_cropEnd   > 0.0) ? m_cropEnd   : m_totalLength;
		if (start > m_totalLength) start = m_totalLength;
		if (end   > m_totalLength) end   = m_totalLength;
		bool hasStart = m_cropStart > 0.0 && start < m_totalLength;
		bool hasEnd   = m_cropEnd   > 0.0 && end   < m_totalLength - 1e-3;

		if (hasStart || hasEnd)
		{
			const int w1 = width() - 1;
			int startPixel = int(start / m_totalLength * w1);
			int endPixel   = int(end   / m_totalLength * w1);

			// Dim the trimmed-away regions. drawRect(x,y,w,h) covers
			// pixel rows [y .. y+h-1], so passing height()-1 leaves the
			// last visible row uncovered - the user reported a couple
			// of unobscured pixels at the bottom of the dimmed band.
			// Use the full height() so the overlay reaches the bottom
			// edge of the widget cleanly.
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(0, 0, 0, 150));
			if (hasStart)
				painter.drawRect(0, 0, startPixel, height());
			if (hasEnd)
				painter.drawRect(endPixel + 1, 0, w1 - (endPixel + 1), height());

			// Explicit marker line + flag label at each defined point.
			painter.setRenderHint(QPainter::Antialiasing, true);
			QFont mf = font();
			mf.setPixelSize(8);
			mf.setBold(true);
			painter.setFont(mf);
			QFontMetrics fm(mf);
			auto drawMarker = [&](int x, const QColor &c, const QString &label,
			                      bool labelLeft) {
				painter.setPen(QPen(c, 2));
				painter.drawLine(x, 0, x, height() - 1);
				int tw = fm.horizontalAdvance(label) + 6;
				int th = fm.height();
				int fx = labelLeft ? (x - tw) : x;
				if (fx < 0) fx = 0;
				if (fx + tw > width()) fx = width() - tw;
				QRect fr(fx, 0, tw, th);
				painter.setPen(Qt::NoPen);
				painter.setBrush(c);
				painter.drawRect(fr);
				painter.setPen(Qt::black);
				painter.drawText(fr, Qt::AlignCenter, label);
			};
			if (hasStart)
				drawMarker(startPixel, QColor(0x3f, 0xb0, 0xe0), tr("Start"), false);
			if (hasEnd)
				drawMarker(endPixel, QColor(0xe0, 0xa0, 0x22), tr("End"), true);
			painter.setRenderHint(QPainter::Antialiasing, false);
		}
	}

	// Right-click crop ghost marker. Pulses while the context menu is
	// open so the user sees where the start / end is going to land.
	// Drawn BEFORE the playback cursor so the live cursor stays on
	// top — ghost is "future intent", cursor is "current state".
	if (m_ghostMarkerSec >= 0.0 && m_totalLength > 0.0
	    && m_ghostBlinkVisible)
	{
		double frac = m_ghostMarkerSec / m_totalLength;
		if (frac < 0.0) frac = 0.0;
		if (frac > 1.0) frac = 1.0;
		int gx = static_cast<int>(frac * (width() - 1) + 0.5);
		painter.setRenderHint(QPainter::Antialiasing, true);
		// Faint glow halo to make the line readable against any
		// waveform colour.
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(255, 255, 255, 28));
		painter.drawRect(gx - 3, 0, 7, height());
		// Dashed core line — distinguishes ghost from solid markers /
		// solid playback cursor.
		QPen ghostPen(QColor(255, 240, 200, 220), 2);
		ghostPen.setStyle(Qt::DashLine);
		ghostPen.setDashPattern({3.0, 3.0});
		painter.setPen(ghostPen);
		painter.drawLine(gx, 0, gx, height() - 1);
		painter.setRenderHint(QPainter::Antialiasing, false);
	}

	// Draw position cursor
	if (m_playbackPosition >= 0.0 && m_playbackPosition <= 1.0)
	{
		int posX = (int)(m_playbackPosition * (width() - 1));
		if (posX < cursorMinX) posX = cursorMinX;
		if (posX > cursorMaxX) posX = cursorMaxX;
		painter.setPen(QPen(QColor(255, 200, 0), 2));
		painter.drawLine(posX, 0, posX, height() - 1);
	}

	// Drag-preview cursor — finger position while the user holds the
	// mouse over the waveform. Painted on top of the live cursor in a
	// distinct translucent style so the user sees BOTH "where audio
	// is now" and "where I am about to seek to". On release the
	// release handler clears m_dragPreview and snaps m_playbackPosition
	// to the released spot — no flicker, no race with the position
	// poll.
	if (m_dragging && m_dragPreview >= 0.0 && m_dragPreview <= 1.0)
	{
		int dx = (int)(m_dragPreview * (width() - 1));
		if (dx < cursorMinX) dx = cursorMinX;
		if (dx > cursorMaxX) dx = cursorMaxX;
		painter.setRenderHint(QPainter::Antialiasing, true);
		// Faint glow halo so the preview is visible against any
		// waveform colour.
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0xfd, 0xe0, 0x70, 60));
		painter.drawRect(dx - 4, 0, 9, height());
		// Dashed core — visually distinct from the solid live cursor
		// (yellow, 2 px). User can tell at a glance which is which.
		QPen dragPen(QColor(0xff, 0xf2, 0xa8, 230), 2);
		dragPen.setStyle(Qt::DashLine);
		dragPen.setDashPattern({4.0, 3.0});
		painter.setPen(dragPen);
		painter.drawLine(dx, 0, dx, height() - 1);

		// Drag tooltip — small dark pill above the cursor showing the
		// precise time at the drag target. Format: M:SS.mmm so the user
		// sees sub-second precision while scrubbing.
		if (m_totalLength > 0.0) {
			double sec = m_dragPreview * m_totalLength;
			if (sec < 0.0) sec = 0.0;
			if (sec > m_totalLength) sec = m_totalLength;
			int minutes = static_cast<int>(sec) / 60;
			int seconds = static_cast<int>(sec) % 60;
			int millis  = static_cast<int>((sec - std::floor(sec)) * 1000.0);
			if (millis < 0) millis = 0;
			if (millis > 999) millis = 999;
			QString lbl = QString("%1:%2.%3")
				.arg(minutes)
				.arg(seconds, 2, 10, QChar('0'))
				.arg(millis,  3, 10, QChar('0'));
			QFont tf = font();
			tf.setPixelSize(10);
			tf.setBold(true);
			painter.setFont(tf);
			QFontMetrics fm(tf);
			int padX = 5, padY = 2;
			int tw = fm.horizontalAdvance(lbl) + padX * 2;
			int th = fm.height() + padY * 2;
			int tx = dx - tw / 2;
			if (tx < 0) tx = 0;
			if (tx + tw > width()) tx = width() - tw;
			int ty = 1;
			if (ty + th > height() - 1) ty = height() - th - 1;
			QRect bg(tx, ty, tw, th);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(20, 20, 20, 220));
			painter.drawRoundedRect(bg, 3, 3);
			painter.setPen(QColor(255, 240, 168, 250));
			painter.drawText(bg, Qt::AlignCenter, lbl);
		}
		painter.setRenderHint(QPainter::Antialiasing, false);
	}

	// Right-button drag selection (loop area). Painted as a translucent
	// accent band between the two anchors so the user previews the
	// region. On release the wiring turns this into Start/End markers +
	// loop ON. Kept distinct from the playback cursor preview (yellow).
	if (m_rightDragging
	    && m_rightDragStart >= 0.0 && m_rightDragStart <= 1.0
	    && m_rightDragEnd   >= 0.0 && m_rightDragEnd   <= 1.0)
	{
		double a = std::min(m_rightDragStart, m_rightDragEnd);
		double b = std::max(m_rightDragStart, m_rightDragEnd);
		int ax = (int)(a * (width() - 1));
		int bx = (int)(b * (width() - 1));
		if (ax < cursorMinX) ax = cursorMinX;
		if (bx > cursorMaxX) bx = cursorMaxX;
		if (bx < ax + 1) bx = ax + 1;
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0x3c, 0x8c, 0x3c, 90));
		painter.drawRect(ax, 0, bx - ax, height());
		// Edge bars — same green tint as the loop button.
		painter.setBrush(QColor(0x3c, 0x8c, 0x3c, 220));
		painter.drawRect(ax, 0, 2, height());
		painter.drawRect(bx - 1, 0, 2, height());
		// Duration label centered on the band.
		if (m_totalLength > 0.0) {
			double sa = a * m_totalLength;
			double sb = b * m_totalLength;
			QString lbl = QString("%1\xE2\x86\x92%2 (%3 s)")
				.arg(sa, 0, 'f', 2)
				.arg(sb, 0, 'f', 2)
				.arg(sb - sa, 0, 'f', 2);
			QFont tf = font();
			tf.setPixelSize(10);
			tf.setBold(true);
			painter.setFont(tf);
			QFontMetrics fm(tf);
			int padX = 5, padY = 2;
			int tw = fm.horizontalAdvance(lbl) + padX * 2;
			int th = fm.height() + padY * 2;
			int tx = (ax + bx) / 2 - tw / 2;
			if (tx < 0) tx = 0;
			if (tx + tw > width()) tx = width() - tw;
			int ty = 1;
			QRect bg(tx, ty, tw, th);
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(20, 20, 20, 230));
			painter.drawRoundedRect(bg, 3, 3);
			painter.setPen(QColor(0xc8, 0xff, 0xc8, 255));
			painter.drawText(bg, Qt::AlignCenter, lbl);
		}
		painter.setRenderHint(QPainter::Antialiasing, false);
	}

	// Draw FX adaptation badge
	if (m_adaptToFx && m_sandbox.enabled) {
		painter.setRenderHint(QPainter::Antialiasing, true);
		QFont f = font();
		f.setPixelSize(9);
		painter.setFont(f);
		QString badge = "FX";
		QFontMetrics fm(f);
		int tw = fm.horizontalAdvance(badge) + 6;
		int th = fm.height() + 2;
		QRect br(width() - tw - 3, 2, tw, th);
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(0, 180, 255, 140));
		painter.drawRoundedRect(br, 3, 3);
		painter.setPen(Qt::white);
		painter.drawText(br, Qt::AlignCenter, badge);
	}

	// Waveform-analysis progress bar. Same visual language as the
	// paulstretch/FX overlay (paulstretch one paints below if
	// m_loadActive is also true — they share the bottom strip but
	// analysis takes precedence visually as it's the more impactful
	// loading state). Driven by m_vis->getBinsProcessed() against the
	// target m_analysisBins. Stays visible until the reveal animation
	// kicks in so the user always sees a continuous progress narrative.
	{
		const bool analysing =
			m_active && m_vis &&
			(m_vis->isRunning() || !m_analysisReady);
		if (analysing) {
			const int target = m_analysisBins > 0 ? m_analysisBins : 1024;
			int processed = static_cast<int>(m_vis->getBinsProcessed());
			if (processed < 0) processed = 0;
			if (processed > target) processed = target;
			float progress = static_cast<float>(processed)
			                 / static_cast<float>(target);
			if (progress > 1.0f) progress = 1.0f;

			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(Qt::NoPen);
			// Slight darken so the bar reads against any background
			// while still letting the dimmed area underneath show.
			painter.setBrush(QColor(0, 0, 0, 55));
			painter.drawRect(0, 0, width(), height());

			int barH = 4;
			int barY = height() - barH - 2;
			int barW = width() - 8;
			int barX = 4;

			painter.setBrush(QColor(40, 40, 50));
			painter.drawRoundedRect(barX, barY, barW, barH, 2, 2);

			int fillW = static_cast<int>(barW * progress);
			if (fillW > 0) {
				Theme::Colors tc = Theme::colors();
				QColor accent = tc.enabled
					? Theme::derivedCached().accent
					: QColor(0, 180, 255);
				accent.setAlpha(220);
				painter.setBrush(accent);
				painter.drawRoundedRect(barX, barY, fillW, barH, 2, 2);
			}

			QFont f = font();
			f.setPixelSize(10);
			painter.setFont(f);
			painter.setPen(QColor(255, 255, 255, 210));
			QString label = (progress < 0.99f)
				? tr("Analysing waveform... %1%").arg(
					static_cast<int>(progress * 100))
				: tr("Waveform ready");
			painter.drawText(QRect(0, 0, width(), height() - barH - 4),
			                 Qt::AlignCenter, label);
			painter.setRenderHint(QPainter::Antialiasing, false);
		}
	}

	// Draw paulstretch loading overlay (timer-based animation)
	if (m_loadActive && m_loadDurationMs > 0) {
		qint64 elapsed = m_loadElapsed.elapsed();
		float progress = static_cast<float>(elapsed) / m_loadDurationMs;
		if (progress > 1.0f) progress = 1.0f;

		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(Qt::NoPen);
		int alpha = static_cast<int>(100 * (1.0f - progress * 0.5f));
		painter.setBrush(QColor(0, 0, 0, alpha));
		painter.drawRect(0, 0, width(), height());

		int barH = 4;
		int barY = height() - barH - 2;
		int barW = width() - 8;
		int barX = 4;

		painter.setBrush(QColor(40, 40, 50));
		painter.drawRoundedRect(barX, barY, barW, barH, 2, 2);

		int fillW = static_cast<int>(barW * progress);
		if (fillW > 0) {
			painter.setBrush(QColor(0, 180, 255, 200));
			painter.drawRoundedRect(barX, barY, fillW, barH, 2, 2);
		}

		QFont f = font();
		f.setPixelSize(10);
		painter.setFont(f);
		painter.setPen(QColor(255, 255, 255, static_cast<int>(200 * (1.0f - progress * 0.7f))));
		QString label;
		if (progress < 0.95f) {
			// Dynamic template — paulstretch and reverse-mode FX
			// overlays share this paint code via m_loadLabelActive.
			label = m_loadLabelActive.contains(QLatin1String("%1"))
				? m_loadLabelActive.arg(static_cast<int>(progress * 100))
				: m_loadLabelActive;
		} else {
			label = m_loadLabelDone;
		}
		painter.drawText(QRect(0, 0, width(), height() - barH - 4),
		                 Qt::AlignCenter, label);
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::resizeEvent(QResizeEvent *evt)
{
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
		// Per-view analyser (lazy): each SoundView owns its own thread
		// so concurrent channels never overwrite each other's bins.
		if (!m_vis)
			m_vis.reset(new SampleVisualizerThread());
		// Reset reveal state for the new file so the loading indicator
		// shows and the fade-in fires at the next completion.
		m_analysisReady = false;
		m_revealActive  = false;
		if (m_revealTimer) m_revealTimer->stop();
		// A fresh file load starts unghosted — playback is about to
		// begin. setPlaying(true) from the wiring would clear this
		// anyway, but explicit reset prevents a one-frame ghost
		// flash between setSound and setPlaying.
		m_ghosted = false;
		m_analysisBins = 1024;
		m_vis->startAnalysis(sound.filename.toUtf8(), m_analysisBins);
		// 50 ms poll — fast enough that the progress bar animates
		// smoothly even on small files (a 3-min audio finishes in ~16
		// outer cycles so we still want sub-second update rate).
		m_timer->start(50);
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
	// Change-detect: the position poll fires at ~30 Hz for every active
	// slot; on widgets whose width is small (~200 px) two consecutive
	// fractions land on the same cursor pixel, so an unconditional
	// update() forwarded a redundant paint event ~half the time.
	if (m_playbackPosition == fraction) return;
	m_playbackPosition = fraction;
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::clearPlayback()
{
	// Fast path when the view is already cleared - the position poll
	// used to call this every tick on every silent channel, which
	// queued a forced update() per idle row and stalled scroll repaints.
	if (!m_active && m_playbackPosition < 0.0 && m_drawnBins == 0
		&& m_cropStart == 0.0 && m_cropEnd < 0.0 && m_totalLength == 0.0)
		return;
	m_active = false;
	m_playbackPosition = -1.0;
	m_drawnBins = 0;
	m_path[0] = QPainterPath();
	m_path[1] = QPainterPath();
	m_soundInfo = SoundInfo();
	m_timer->stop();
	m_loadActive = false;
	m_loadTimer->stop();
	m_analysisReady = false;
	m_revealActive  = false;
	if (m_revealTimer) m_revealTimer->stop();
	// Ghost flag belongs to the "loaded but stopped" state — a true
	// clear wipes the slot so the next setSound starts unghosted.
	m_ghosted = false;
	// Crop markers must not survive a stop / sound change.
	m_cropStart = 0.0;
	m_cropEnd = -1.0;
	m_totalLength = 0.0;
	update();
}


//---------------------------------------------------------------
// Purpose: crop marker feed (see header)
//---------------------------------------------------------------
void SoundView::setCropRange(double startSeconds, double endSeconds)
{
	m_cropStart = startSeconds;
	m_cropEnd   = endSeconds;
	update();
}

void SoundView::setTotalLength(double seconds)
{
	if (seconds == m_totalLength)
		return;
	m_totalLength = seconds;
	update();
}

void SoundView::setShowCropMarkers(bool on)
{
	if (m_showCropMarkers == on)
		return;
	m_showCropMarkers = on;
	update();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::onTimer()
{
	if (!m_vis) { m_timer->stop(); return; }
	// Only repaint while ANALYSIS-still-running OR a final ready
	// transition is pending. While loading we draw a loading bar
	// (drawWaves bails out — no progressive path growth so the user
	// never sees a "resize at end of load"). At completion ONE
	// repaint fades the full-resolution path in.
	bool finished = !m_vis->isRunning();
	bool repaintFinalFrame = finished && !m_analysisReady;
	if (finished && m_drawnBins >= m_vis->getBinsProcessed()
	             && m_analysisReady) {
		m_timer->stop();
	}
	if (repaintFinalFrame) {
		m_analysisReady = true;
		// Fade-in: short alpha ramp so the transition from loading
		// indicator to full waveform is smooth instead of a hard pop.
		m_revealElapsed.start();
		m_revealActive = true;
		if (!m_revealTimer) {
			m_revealTimer = new QTimer(this);
			m_revealTimer->setInterval(16); // ~60 Hz
			connect(m_revealTimer, &QTimer::timeout,
			        this, [this]{
				if (m_revealElapsed.elapsed() >= kRevealMs) {
					m_revealActive = false;
					m_revealTimer->stop();
				}
				update();
			});
		}
		m_revealTimer->start();
	}
	update();
}


//---------------------------------------------------------------
// Purpose: paint the per-channel waveform. While the visualizer is
// still analysing we draw NOTHING — only a thin loading indicator
// stripe so the user has feedback without ever seeing the path
// progressively extend / re-resize at end of load. The full
// waveform appears in one go with a short alpha fade-in.
//---------------------------------------------------------------
void SoundView::drawWaves(QPainter *painter)
{
	if (!m_active)
		return;

	const bool analysing =
		m_vis && (m_vis->isRunning() || !m_analysisReady);

	if (analysing) {
		// Draw nothing here — the progress bar is painted by the
		// dedicated overlay block in paintEvent (after all other
		// layers) so it sits on top of the dimmed background AND any
		// existing crop / ghost decoration cleanly.
		return;
	}

	preparePaths();

	// Reveal fade-in: render the full path at increasing alpha for
	// kRevealMs after analysis completion. After the fade the
	// painter state is restored to whatever the caller set up so
	// the rest of paintEvent draws normally.
	int alpha = 255;
	if (m_revealActive) {
		qint64 e = m_revealElapsed.elapsed();
		double t = (double)e / (double)kRevealMs;
		if (t < 0.0) t = 0.0;
		if (t > 1.0) t = 1.0;
		alpha = (int)(t * 255.0);
	}
	// Ghosted (loaded but not playing): cap alpha so the waveform
	// reads as "stopped, ready to replay". Pause does NOT flip this —
	// only the wiring's true-stop edge does. Used together with the
	// reveal fade so a freshly loaded sound still fades in before
	// settling at the ghost alpha.
	if (m_ghosted) {
		int ghostAlpha = 95;
		if (alpha > ghostAlpha) alpha = ghostAlpha;
	}
	if (alpha < 255) {
		QColor pen  = painter->pen().color();
		QColor brsh = painter->brush().color();
		QPen   p    = painter->pen();
		QBrush b    = painter->brush();
		QColor penA(pen.red(),  pen.green(),  pen.blue(),  alpha);
		QColor brA (brsh.red(), brsh.green(), brsh.blue(),
		            std::min(180, alpha));
		p.setColor(penA);
		b.setColor(brA);
		painter->setPen(p);
		painter->setBrush(b);
	}

	painter->drawPath(m_path[0]);
	painter->drawPath(m_path[1]);
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::setAdaptToFx(bool on)
{
	if (m_adaptToFx == on) return;
	m_adaptToFx = on;
	m_drawnBins = 0;
	update();
}

void SoundView::setSandboxState(const SandboxState &s)
{
	bool stretchParamChanged = s.enabled && s.stretchEnabled && (
		s.stretchFactor   != m_sandbox.stretchFactor ||
		s.stretchWindowMs != m_sandbox.stretchWindowMs ||
		(s.stretchEnabled && !m_sandbox.stretchEnabled));

	m_sandbox = s;

	if (stretchParamChanged) {
		int durationMs = static_cast<int>(s.stretchWindowMs * 1.5f);
		if (durationMs < 300) durationMs = 300;
		if (durationMs > 3000) durationMs = 3000;
		startStretchLoadAnimation(durationMs);
	}

	if (!s.stretchEnabled && m_loadActive) {
		m_loadActive = false;
		m_loadTimer->stop();
	}

	// Earlier this method had a long list of per-field diffs that
	// gated needRedraw. The list missed every continuous slider that
	// did not also flip a bool (e.g. compThresholdDb without
	// compEnabled toggling), so dragging compressor ratio left the
	// waveform stale. Cheaper and more correct: when the adaptive
	// view is on, redraw whenever ANY sandbox state lands. The bin
	// FX pass is cheap (1024 bins) and only happens after a state
	// arrives, so this does not spin the GUI.
	if (m_adaptToFx) {
		m_drawnBins = 0;
		update();
	}
}

void SoundView::setLiveFx(int pitch, int speed, int reverb)
{
	if (pitch == m_fxPitch && speed == m_fxSpeed && reverb == m_fxReverb)
		return;
	bool pitchChanged  = (pitch  != m_fxPitch);
	bool speedChanged  = (speed  != m_fxSpeed);
	bool reverbChanged = (reverb != m_fxReverb);
	m_fxPitch  = pitch;
	m_fxSpeed  = speed;
	m_fxReverb = reverb;
	if (m_adaptToFx) {
		m_drawnBins = 0;
		update();
	}
	// Reverse-mode feedback overlay. Pitch/speed in reverse trigger
	// a chunk-worker rebuild (~150 ms decode + queue refill) so the
	// user gets ~400 ms of visible progress. Reverb is instant but
	// still flashes a short 250 ms acknowledgement so the slider
	// drag feels responsive. Same overlay machinery as paulstretch.
	if (m_active && m_reverse) {
		if (pitchChanged || speedChanged) {
			QString label = (pitchChanged && speedChanged)
				? tr("Applying pitch + speed... %1%")
				: (pitchChanged ? tr("Applying pitch... %1%")
				                : tr("Applying speed... %1%"));
			QString done = tr("FX ready");
			startFxLoadAnimation(400, label, done);
		} else if (reverbChanged) {
			startFxLoadAnimation(250,
			                     tr("Applying reverb... %1%"),
			                     tr("Reverb ready"));
		}
	}
}

void SoundView::notifySeek()
{
	if (m_sandbox.enabled && m_sandbox.stretchEnabled) {
		int durationMs = static_cast<int>(m_sandbox.stretchWindowMs);
		if (durationMs < 200) durationMs = 200;
		if (durationMs > 2000) durationMs = 2000;
		startStretchLoadAnimation(durationMs);
	}
}

void SoundView::startStretchLoadAnimation(int durationMs)
{
	m_loadDurationMs = durationMs;
	m_loadElapsed.start();
	m_loadActive = true;
	m_loadLabelActive = tr("Applying paulstretch... %1%");
	m_loadLabelDone   = tr("Paulstretch ready");
	m_loadTimer->start();
	update();
}

void SoundView::startFxLoadAnimation(int durationMs,
                                     const QString &activeTemplate,
                                     const QString &doneText)
{
	m_loadDurationMs = durationMs;
	m_loadElapsed.start();
	m_loadActive = true;
	m_loadLabelActive = activeTemplate;
	m_loadLabelDone   = doneText;
	m_loadTimer->start();
	update();
}

void SoundView::onLoadTick()
{
	if (!m_loadActive) { m_loadTimer->stop(); return; }
	qint64 elapsed = m_loadElapsed.elapsed();
	if (elapsed >= m_loadDurationMs + 300) {
		m_loadActive = false;
		m_loadTimer->stop();
	}
	update();
}

void SoundView::applyFxToBins(std::vector<float> &binsL, std::vector<float> &binsR, size_t count) const
{
	const auto &s = m_sandbox;
	if (!s.enabled) return;

	// EQ: approximate visual gain per bin based on band gains.
	// Each bin maps to a frequency; we compute a weighted sum from the
	// nearest EQ bands. This is a rough visual hint, not a real IIR.
	if (s.eqEnabled) {
		static const float kBandFreqs[16] = {
			20, 25, 40, 63, 100, 160, 250, 400,
			630, 1000, 1600, 2500, 4000, 6300, 10000, 16000
		};
		bool anyNonZero = false;
		for (int i = 0; i < 16; ++i)
			if (std::fabs(s.eqBandDb[i]) > 0.01f) { anyNonZero = true; break; }
		if (anyNonZero) {
			float sampleRate = 48000.0f;
			for (size_t i = 0; i < count; ++i) {
				float freq = (static_cast<float>(i) / 1024.0f) * sampleRate * 0.5f;
				float gainDb = 0.0f;
				float totalWeight = 0.0f;
				for (int b = 0; b < 16; ++b) {
					if (std::fabs(s.eqBandDb[b]) < 0.01f) continue;
					float bFreq = kBandFreqs[b];
					float dist = std::fabs(std::log2(std::max(freq, 1.0f) / bFreq));
					float w = 1.0f / (1.0f + dist * dist * 4.0f);
					gainDb += s.eqBandDb[b] * w;
					totalWeight += w;
				}
				if (totalWeight > 0.0f) gainDb /= totalWeight;
				float gain = AudioUtils::dbToLinear(gainDb);
				binsL[i] *= gain;
				binsR[i] *= gain;
			}
		}
	}

	// Compressor: roughly reduce peaks above threshold
	if (s.compEnabled && s.compRatio > 1.01f) {
		float threshLin = AudioUtils::dbToLinear(s.compThresholdDb);
		float ratio = s.compRatio;
		for (size_t i = 0; i < count; ++i) {
			for (float *ch : {&binsL[i], &binsR[i]}) {
				float a = std::fabs(*ch);
				if (a > threshLin) {
					float over = a - threshLin;
					float compressed = threshLin + over / ratio;
					float makeupLin = AudioUtils::dbToLinear(s.compMakeupDb);
					*ch = (*ch < 0 ? -compressed : compressed) * makeupLin;
				}
			}
		}
	}

	// Saturator: tanh-style soft clip visual
	if (s.saturatorEnabled && s.saturatorMix > 0.001f) {
		float drive = s.saturatorDrive;
		float mix = s.saturatorMix;
		for (size_t i = 0; i < count; ++i) {
			float dryL = binsL[i], dryR = binsR[i];
			float wetL = std::tanh(dryL * drive);
			float wetR = std::tanh(dryR * drive);
			binsL[i] = dryL * (1.0f - mix) + wetL * mix;
			binsR[i] = dryR * (1.0f - mix) + wetR * mix;
		}
	}

	// Bitcrusher: quantize + decimate
	if (s.bitcrusherEnabled) {
		if (s.bitcrusherBitDepth < 16) {
			float levels = std::pow(2.0f, static_cast<float>(s.bitcrusherBitDepth) - 1.0f);
			for (size_t i = 0; i < count; ++i) {
				binsL[i] = std::round(binsL[i] * levels) / levels;
				binsR[i] = std::round(binsR[i] * levels) / levels;
			}
		}
		if (s.bitcrusherRate < 47999.0f) {
			float step = 48000.0f / s.bitcrusherRate;
			float holdL = 0.0f, holdR = 0.0f;
			float phase = 0.0f;
			for (size_t i = 0; i < count; ++i) {
				phase += 1.0f;
				if (phase >= step) {
					phase -= step;
					holdL = binsL[i];
					holdR = binsR[i];
				}
				binsL[i] = holdL;
				binsR[i] = holdR;
			}
		}
	}

	// Mono
	if (s.monoEnabled) {
		for (size_t i = 0; i < count; ++i) {
			float m = (binsL[i] + binsR[i]) * 0.5f;
			binsL[i] = m;
			binsR[i] = m;
		}
	}

	// Generation loss: cascaded LPF + quantization + saturation
	if (s.genLossEnabled && s.genLossGenerations > 1) {
		float g = static_cast<float>(s.genLossGenerations);
		float bits = 16.f * std::pow(0.997f, g);
		if (bits < 1.5f) bits = 1.5f;
		float quantLevels = std::pow(2.f, bits);
		float drive = 1.f + g * 0.004f;
		if (drive > 6.f) drive = 6.f;
		float driveNorm = 1.f / std::tanh(drive);
		int decimFactor = 1 + static_cast<int>(g / 80.f);
		if (decimFactor > 48) decimFactor = 48;
		for (size_t i = 0; i < count; ++i) {
			if (decimFactor > 1 && (i % decimFactor) != 0) {
				size_t prev = (i / decimFactor) * decimFactor;
				binsL[i] = binsL[prev];
				binsR[i] = binsR[prev];
			}
			if (quantLevels < 32768.f) {
				binsL[i] = std::round(binsL[i] * quantLevels) / quantLevels;
				binsR[i] = std::round(binsR[i] * quantLevels) / quantLevels;
			}
			if (drive > 1.001f) {
				binsL[i] = std::tanh(binsL[i] * drive) * driveNorm;
				binsR[i] = std::tanh(binsR[i] * drive) * driveNorm;
			}
		}
	}

	// Paulstretch smoothing: moving average to simulate the frequency smearing
	if (s.stretchEnabled && s.stretchFactor > 1.01f) {
		int smoothW = static_cast<int>(s.stretchFactor * 2.0f);
		if (smoothW < 2) smoothW = 2;
		if (smoothW > static_cast<int>(count / 2)) smoothW = static_cast<int>(count / 2);
		if (smoothW > 1) {
			std::vector<float> tmpL(count), tmpR(count);
			int half = smoothW / 2;
			for (size_t i = 0; i < count; ++i) {
				float sumL = 0.0f, sumR = 0.0f;
				int n = 0;
				for (int j = -half; j <= half; ++j) {
					int idx = static_cast<int>(i) + j;
					if (idx < 0 || idx >= static_cast<int>(count)) continue;
					sumL += binsL[idx];
					sumR += binsR[idx];
					++n;
				}
				tmpL[i] = sumL / n;
				tmpR[i] = sumR / n;
			}
			binsL = tmpL;
			binsR = tmpR;
		}
	}

	// Chorus/Flanger/Flangus/Phaser/Delay/Reverb: add subtle visual
	// thickening proportional to the combined wet mix. These time-domain
	// effects are hard to visualise precisely so we just broaden the
	// waveform slightly.
	float wetSum = 0.0f;
	if (s.chorusEnabled) wetSum += s.chorusMix;
	if (s.flangerEnabled) wetSum += s.flangerMix;
	if (s.flangusEnabled) wetSum += s.flangusMix;
	if (s.phaserEnabled) wetSum += s.phaserMix;
	if (s.delayEnabled) wetSum += s.delayMix * 0.5f;
	wetSum += s.reverbWet * 0.3f;
	if (wetSum > 0.01f) {
		float spread = 1.0f + wetSum * 0.15f;
		for (size_t i = 0; i < count; ++i) {
			binsL[i] *= spread;
			binsR[i] *= spread;
		}
	}

	// Limiter: clamp peaks
	if (s.limiterEnabled) {
		float ceil = AudioUtils::dbToLinear(s.limiterCeiling);
		for (size_t i = 0; i < count; ++i) {
			if (binsL[i] >  ceil) binsL[i] =  ceil;
			if (binsL[i] < -ceil) binsL[i] = -ceil;
			if (binsR[i] >  ceil) binsR[i] =  ceil;
			if (binsR[i] < -ceil) binsR[i] = -ceil;
		}
	}

	// FxPanel pitch / speed do not actually transform the waveform
	// (the file content does not change - only playback rate). Earlier
	// builds resampled the bins on the x-axis to "visualise" speed,
	// but that squashed the wave toward x=0 and left the cursor
	// floating over empty pixels in the right half. Pitch gain biasing
	// was also misleading because the file amplitude does not depend
	// on pitch slider. Both are removed. Only reverb is hinted at via
	// a tail since reverb truly adds extra wet on top of the dry.
	if (m_fxReverb > 0) {
		float wet = m_fxReverb / 100.0f;
		int tailLen = 64;
		std::vector<float> tailL(count, 0.0f), tailR(count, 0.0f);
		float decay = 0.93f;
		for (size_t i = 0; i < count; ++i) {
			float al = binsL[i], ar = binsR[i];
			float g = wet * 0.5f;
			for (int k = 1; k < tailLen && i + k < count; ++k) {
				tailL[i + k] += al * g;
				tailR[i + k] += ar * g;
				g *= decay;
			}
		}
		for (size_t i = 0; i < count; ++i) {
			binsL[i] += tailL[i];
			binsR[i] += tailR[i];
		}
	}
}

void SoundView::preparePaths()
{
	if (!m_vis)
		return;
	SampleVisualizerThread &t = *m_vis;
	size_t bins = t.getBinsProcessed();
	// Bins SHRINK on EOF when the visualizer resamples a runaway
	// undershoot (more than numBins source bins generated) back down
	// to numBins. Without the != path, drawnBins would stay at the
	// progressive overshoot value, preparePaths would skip recompute,
	// and the user would keep seeing the pre-finalise paths whose
	// trailing bins live OFF-screen — the silence-at-end stays
	// invisible. Trigger recompute on any count change, not just
	// growth.
	if (m_drawnBins != bins)
	{
		double fhh = (double)height() * 0.5;
		double fw = (double)width();
		double shortScale = 1.0 / ((double)std::numeric_limits<short>::max() * 1.1);

		if (m_adaptToFx && m_sandbox.enabled) {
			std::vector<float> binsL(bins), binsR(bins);
			for (size_t i = 0; i < bins; ++i) {
				binsL[i] = static_cast<float>(t.getBins()[i * 2]) * static_cast<float>(shortScale);
				binsR[i] = static_cast<float>(t.getBins()[i * 2 + 1]) * static_cast<float>(shortScale);
			}

			applyFxToBins(binsL, binsR, bins);

			m_path[0] = QPainterPath(QPointF(0.0, fhh));
			m_path[1] = QPainterPath(QPointF(0.0, fhh));
			for (size_t i = 0; i < bins; ++i) {
				double x = (double)i * (1.0 / 1024.0) * fw;
				m_path[0].lineTo(x, (1.0 + static_cast<double>(binsL[i])) * fhh);
				m_path[1].lineTo(x, (1.0 + static_cast<double>(binsR[i])) * fhh);
			}
			double endx = fw * (double)bins * (1.0 / 1024.0);
			m_path[0].lineTo(endx, fhh);
			m_path[1].lineTo(endx, fhh);
			m_path[0].closeSubpath();
			m_path[1].closeSubpath();
		} else {
			m_path[0] = QPainterPath(QPointF(0.0, fhh));
			m_path[1] = QPainterPath(QPointF(0.0, fhh));
			for(size_t i = 0; i < bins; ++i)
			{
				double v0 = (double)t.getBins()[i * 2] * shortScale;
				double v1 = (double)t.getBins()[i * 2 + 1] * shortScale;
				double x = (double)i * (1.0 / 1024.0) * fw;
				m_path[0].lineTo(x, (1.0 + v0) * fhh);
				m_path[1].lineTo(x, (1.0 + v1) * fhh);
			}
			double endx = fw * (double)bins * (1.0 / 1024.0);
			m_path[0].lineTo(endx, fhh);
			m_path[1].lineTo(endx, fhh);
			m_path[0].closeSubpath();
			m_path[1].closeSubpath();
		}
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
// Purpose: clamp a seek fraction inside the active crop range so a
// click on the dimmed (trimmed-away) regions snaps the visual cursor
// to the crop edge immediately instead of painting it in dead space
// for one frame before Sampler::seek clamps the audio anyway. NOT
// applied to the right-click crop editor - setting a marker inside the
// dimmed region (e.g. moving the start earlier) must stay possible.
//---------------------------------------------------------------
double SoundView::clampFractionToCrop(double fraction) const
{
	if (m_showCropMarkers && m_totalLength > 0.0)
	{
		double lo = (m_cropStart > 0.0) ? m_cropStart / m_totalLength : 0.0;
		double hi = (m_cropEnd   > 0.0) ? m_cropEnd   / m_totalLength : 1.0;
		if (hi > 1.0) hi = 1.0;
		if (fraction < lo) fraction = lo;
		if (fraction > hi) fraction = hi;
	}
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
		double frac = clampFractionToCrop(fractionFromMouseX(evt->x()));
		// Do NOT overwrite m_playbackPosition: the live cursor must
		// keep tracking audio while the user previews the seek with
		// the finger. Only the drag-preview overlay moves.
		m_dragPreview = frac;
		update();
	}
	else if (evt->button() == Qt::RightButton && m_totalLength > 0.0)
	{
		// Begin right-drag for loop area selection. The actual loop
		// emit only fires if the user moves > kRightDragMinPx before
		// release; below threshold the contextMenu (existing crop
		// editor) still fires through contextMenuEvent.
		m_rightDragging  = true;
		double frac = fractionFromMouseX(evt->x());
		m_rightDragStart   = frac;
		m_rightDragEnd     = frac;
		m_rightDragStartPx = evt->x();
		m_suppressNextContextMenu = false;
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
		double frac = clampFractionToCrop(fractionFromMouseX(evt->x()));
		m_dragPreview = frac;
		update();
	}
	if (m_rightDragging)
	{
		m_rightDragEnd = fractionFromMouseX(evt->x());
		update();
	}
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void SoundView::mouseReleaseEvent(QMouseEvent *evt)
{
	if (m_dragging && evt->button() == Qt::LeftButton)
	{
		m_dragging = false;
		double frac = clampFractionToCrop(fractionFromMouseX(evt->x()));
		// Commit: snap the LIVE cursor to the release point so the
		// next position poll (~16 ms later) does not paint the
		// cursor drifting from the previous audio position to here.
		m_playbackPosition = frac;
		m_dragPreview      = -1.0;
		update();
		emit seekRequested(frac);
	}
	else if (m_rightDragging && evt->button() == Qt::RightButton)
	{
		bool wasDrag = std::abs(evt->x() - m_rightDragStartPx) >= kRightDragMinPx;
		double a = std::min(m_rightDragStart, m_rightDragEnd);
		double b = std::max(m_rightDragStart, m_rightDragEnd);
		m_rightDragging = false;
		m_rightDragStart = -1.0;
		m_rightDragEnd   = -1.0;
		update();
		if (wasDrag && m_totalLength > 0.0) {
			double sa = a * m_totalLength;
			double sb = b * m_totalLength;
			if (sa < 0.0) sa = 0.0;
			if (sb > m_totalLength) sb = m_totalLength;
			if (sb - sa < 0.05) {
				// Drag too narrow once mapped to seconds — fall through
				// to context menu instead so the user gets the crop
				// editor (right-click at this point) rather than a
				// 50 ms loop they didn't want.
				return;
			}
			// Suppress the contextMenuEvent that Qt fires AFTER
			// release — without this the menu pops on top of the
			// freshly placed loop range.
			m_suppressNextContextMenu = true;
			emit loopAreaSelected(sa, sb);
		}
	}
}


// Right-click crop editor: hit-test the click against existing Start
// / End flags (Remove) and otherwise offer Set start / Set end. Needs
// a total-length feed to map click x -> seconds.
void SoundView::contextMenuEvent(QContextMenuEvent *evt)
{
	// Right-button drag committed a loop-area selection in
	// mouseReleaseEvent. The contextMenuEvent that Qt fires AFTER the
	// release would otherwise pop the crop-editor menu on top of the
	// just-placed loop range — swallow it here once.
	if (m_suppressNextContextMenu) {
		m_suppressNextContextMenu = false;
		evt->accept();
		return;
	}
	if (m_totalLength <= 0.0) {
		QWidget::contextMenuEvent(evt);
		return;
	}

	const int w1 = width() - 1;
	const double frac = fractionFromMouseX(evt->x());
	const double clickSec = frac * m_totalLength;
	const bool hasStart = m_cropStart > 0.0 && m_cropStart < m_totalLength;
	const bool hasEnd   = m_cropEnd   > 0.0 && m_cropEnd   < m_totalLength - 1e-3;

	auto pixelOf = [this, w1](double sec) {
		if (m_totalLength <= 0.0) return 0;
		return int(sec / m_totalLength * w1 + 0.5);
	};
	const int kHitPx = 10;
	bool onStartFlag = hasStart && std::abs(evt->x() - pixelOf(m_cropStart)) <= kHitPx;
	bool onEndFlag   = hasEnd   && std::abs(evt->x() - pixelOf(m_cropEnd))   <= kHitPx;

	QMenu menu(this);
	if (onStartFlag) {
		menu.addAction(tr("Remove start marker"), this, [this]{
			emit cropClearStartRequested();
		});
	} else if (onEndFlag) {
		menu.addAction(tr("Remove end marker"), this, [this]{
			emit cropClearEndRequested();
		});
	} else {
		double startSec = hasStart ? m_cropStart : 0.0;
		double endSec   = hasEnd   ? m_cropEnd   : m_totalLength;
		QAction *startAct = menu.addAction(tr("Set start here (%1 s)")
			.arg(clickSec, 0, 'f', 2),
			this, [this, clickSec]{ emit cropStartRequestedAt(clickSec); });
		// Enforce start < end. If clicking past the current end, the
		// "set start" option would invert the range - disable it.
		if (hasEnd && clickSec >= endSec - 0.01)
			startAct->setEnabled(false);

		QAction *endAct = menu.addAction(tr("Set end here (%1 s)")
			.arg(clickSec, 0, 'f', 2),
			this, [this, clickSec]{ emit cropEndRequestedAt(clickSec); });
		if (hasStart && clickSec <= startSec + 0.01)
			endAct->setEnabled(false);

		if (hasStart || hasEnd) {
			menu.addSeparator();
			if (hasStart)
				menu.addAction(tr("Remove start marker"), this, [this]{
					emit cropClearStartRequested();
				});
			if (hasEnd)
				menu.addAction(tr("Remove end marker"), this, [this]{
					emit cropClearEndRequested();
				});
			menu.addAction(tr("Remove both markers"), this, [this]{
				emit cropClearAllRequested();
			});
		}
	}
	// Ghost marker: pulsing semi-transparent line at the click
	// position so the user sees exactly where the marker is about to
	// land while the menu is open. Cleared after menu.exec returns
	// regardless of which action was picked (or cancel).
	m_ghostMarkerSec    = clickSec;
	m_ghostBlinkVisible = true;
	if (!m_ghostTimer) {
		m_ghostTimer = new QTimer(this);
		m_ghostTimer->setInterval(220);   // ~2.3 Hz pulse
		connect(m_ghostTimer, &QTimer::timeout, this, [this]{
			m_ghostBlinkVisible = !m_ghostBlinkVisible;
			update();
		});
	}
	m_ghostTimer->start();
	update();

	menu.exec(evt->globalPos());

	if (m_ghostTimer) m_ghostTimer->stop();
	m_ghostMarkerSec    = -1.0;
	m_ghostBlinkVisible = true;
	update();
	evt->accept();
}
