// src/soundview_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#include <QPainter>
#include <QLinearGradient>
#include "AudioUtils.h"
#include <QTimer>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include "soundview_qt.h"
#include "modules/theme.h"
#include "SampleVisualizerThread.h"

// The waveform preview runs the SAME DSP classes as the audio thread, on a
// decimated copy of the file. No re-implementation, no approximation of the
// maths - only the sample rate differs.
#include "dsp/BiquadPeaking.h"
#include "dsp/EqRack.h"
#include "dsp/Compressor.h"
#include "dsp/Saturator.h"
#include "dsp/NoiseGate.h"
#include "dsp/TransientShaper.h"
#include "dsp/Limiter.h"
#include "dsp/Bitcrusher.h"
#include "dsp/GenerationLoss.h"
#include "dsp/Reverb.h"

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
		QRect tintRect;
		if (m_reverse) {
			int x1 = std::min(width() - 2, cursorMaxX);
			if (x1 > posX)
				tintRect = QRect(posX, 1, x1 - posX, height() - 2);
		} else {
			int x0 = std::max(1, cursorMinX);
			if (posX > x0)
				tintRect = QRect(x0, 1, posX - x0, height() - 2);
		}
		if (!tintRect.isNull()) {
			if (m_streamGradientEnabled) {
				// Animated colour flow on the played portion — ALL
				// playback, not just streams (user request). Phase from
				// the wall clock; the 30 Hz position poll repaints
				// during playback so no extra timer is needed (paused =
				// static gradient, fine).
				//
				// THEME-AWARE: with a custom theme active the gradient
				// runs between the theme's accent and waveform colours
				// (spread apart if the user picked near-identical ones),
				// so it never clashes with the palette. Default theme
				// keeps the azure→violet look. RGB lerp only — no HSL
				// math, so the Qt h=-1 grey trap can't bite.
				if (!m_gradClock.isValid()) m_gradClock.start();
				// 0..100 speed slider -> 0.25x..4x of the base rate (50 = 1x).
				const double rate = 0.0012
					* std::pow(4.0, (m_gradSpeed - 50) / 50.0);
				const double ph = 0.5 + 0.5
					* std::sin((double)m_gradClock.elapsed() * rate);
				// 0..100 intensity slider -> alpha 12..140 (30 = old look).
				const int alpha = 12 + m_gradIntensity * 128 / 100;
				QColor cA(0x3f, 0xa7, 0xff, alpha);   // azure  (default)
				QColor cB(0x8a, 0x5c, 0xf6, alpha);   // violet (default)
				{
					Theme::Colors tc = Theme::colors();
					if (tc.enabled) {
						cA = tc.accent;
						cB = tc.waveform;
					}
					auto dist2 = [](const QColor &x, const QColor &y){
						const int dr = x.red()   - y.red();
						const int dg = x.green() - y.green();
						const int db = x.blue()  - y.blue();
						return dr * dr + dg * dg + db * db;
					};
					// Near-identical stops would make the flow invisible —
					// push the second stop apart (direction chosen so a
					// light pick darkens instead of clipping to white).
					if (dist2(cA, cB) < 48 * 48)
						cB = (cB.lightness() > 150) ? cB.darker(160)
						                            : cB.lighter(160);
					// THE "changed colours broke the animation" bug: a stop
					// too close to the panel background painted the moving
					// tint invisibly (accent ≈ background is a common theme
					// pick). Force contrast against the actual background
					// fill for BOTH stops. RGB only — no HSL h=-1 trap.
					auto contrastFix = [&dist2, &bgFill](QColor c){
						if (dist2(c, bgFill) < 40 * 40)
							return (bgFill.lightness() > 127)
							       ? c.darker(170) : c.lighter(190);
						return c;
					};
					cA = contrastFix(cA);
					cB = contrastFix(cB);
					cA.setAlpha(alpha);
					cB.setAlpha(alpha);
				}
				auto mix = [](const QColor &x, const QColor &y, double t){
					return QColor(
						x.red()   + int((y.red()   - x.red())   * t),
						x.green() + int((y.green() - x.green()) * t),
						x.blue()  + int((y.blue()  - x.blue())  * t),
						x.alpha());
				};
				QLinearGradient g(tintRect.left(), 0, tintRect.right(), 0);
				g.setColorAt(0.0, mix(cA, cB, ph));
				g.setColorAt(1.0, mix(cB, cA, ph));
				painter.setBrush(g);
			} else {
				painter.setBrush(playedTint);
			}
			painter.drawRect(tintRect);
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

	// Draw FX adaptation badge — only when the drawing really IS the
	// processed signal, so the badge never claims an adaptation that the
	// chain did not produce.
	if (m_fxPathsActive) {
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
	// A fresh sound is never live until the wiring flags it (onStartPlaying).
	m_liveStream = false;
	m_networkNotice = false;
	// A network VOD (normal YouTube video) STILL gets a full waveform — only
	// true LIVE streams (setLiveStream) skip it. The analyser decodes the
	// remote file; draw the bins PROGRESSIVELY as they arrive (network is slow).
	const bool isNetwork = sound.isStreamUrl
		|| sound.filename.startsWith("http://")
		|| sound.filename.startsWith("https://");
	m_streamProgressive = isNetwork;
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
		// The cached DSP source belongs to the previous file.
		m_fxSrc.clear();
		m_fxSrc.shrink_to_fit();
		m_fxSrcRate     = 0.0;
		m_fxSrcComplete = false;
		m_fxPathsActive = false;
		m_fxDirty       = true;
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
	// Drop the DSP preview source with the rest of the slot state.
	m_fxSrc.clear();
	m_fxSrc.shrink_to_fit();
	m_fxSrcRate     = 0.0;
	m_fxSrcComplete = false;
	m_fxPathsActive = false;
	m_fxDirty       = true;
	if (m_fxRenderTimer) m_fxRenderTimer->stop();
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

void SoundView::setStreamGradientEnabled(bool on)
{
	if (m_streamGradientEnabled == on)
		return;
	m_streamGradientEnabled = on;
	update();
}

void SoundView::setStreamGradientStyle(int speed, int intensity)
{
	m_gradSpeed = qBound(0, speed, 100);
	m_gradIntensity = qBound(0, intensity, 100);
	update();
}

void SoundView::setLiveStream(bool on)
{
	if (m_liveStream == on)
		return;
	m_liveStream = on;
	if (on) {
		// Endless source: kill the analyser (it would read the live stream
		// forever) and stop the poll timer. Nothing to visualise.
		if (m_vis) m_vis->stop(false);
		if (m_timer) m_timer->stop();
		m_active = true;   // still "loaded" so the notice paints
	}
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

	// LIVE stream ONLY: no waveform (endless, unseekable). Everything else —
	// local files AND normal network VODs — draws a real waveform below.
	if (m_liveStream) {
		painter->save();
		QFont f = painter->font();
		f.setBold(true);
		painter->setFont(f);
		painter->setPen(QColor(0xb0, 0x6f, 0xff));   // purple
		painter->drawText(rect(), Qt::AlignCenter,
			tr("● LIVE — no waveform for live streams"));
		painter->restore();
		return;
	}

	const bool analysing =
		m_vis && (m_vis->isRunning() || !m_analysisReady);

	if (analysing && !m_streamProgressive) {
		// Draw nothing here — the progress bar is painted by the
		// dedicated overlay block in paintEvent (after all other
		// layers) so it sits on top of the dimmed background AND any
		// existing crop / ghost decoration cleanly.
		return;
	}
	// Stream progressive path: fall through and draw whatever bins the
	// analyser has produced so far. preparePaths() lays each processed
	// bin at a fixed 1/1024 x-position, so the waveform grows left→right
	// as decoding proceeds instead of freezing on a blank stripe.

	if (m_displayMode == Mode_Spectrogram && m_vis) {
		// Spectrogram-lite: reuse the analyser's peak min/max bins as a
		// per-column magnitude proxy. Each column is painted top-to-
		// bottom as a heatmap gradient - cool blue at low intensity,
		// hot red at high. Not a TRUE STFT (would need a second bin
		// array of spectral magnitudes) but visually distinct from the
		// waveform view and cheap - reuses the SAME analyser bins.
		volatile const int *bins = m_vis->getBins();
		size_t processed = m_vis->getBinsProcessed();
		if (!bins || processed == 0) return;
		int W = width();
		int H = height();
		if (W <= 0 || H <= 0) return;
		painter->save();
		painter->setPen(Qt::NoPen);
		auto heatColor = [](float t) -> QColor {
			if (t < 0.0f) t = 0.0f;
			if (t > 1.0f) t = 1.0f;
			// Blue -> cyan -> green -> yellow -> red gradient.
			struct S { float at; int r, g, b; };
			static const S stops[] = {
				{0.00f,  20,  40, 120},
				{0.25f,  30, 140, 200},
				{0.50f,  40, 180,  70},
				{0.75f, 230, 200,  50},
				{1.00f, 230,  70,  60},
			};
			for (int i = 0; i + 1 < 5; ++i) {
				if (t <= stops[i + 1].at) {
					float u = (t - stops[i].at) / (stops[i + 1].at - stops[i].at);
					int r = (int)(stops[i].r + (stops[i + 1].r - stops[i].r) * u);
					int g = (int)(stops[i].g + (stops[i + 1].g - stops[i].g) * u);
					int b = (int)(stops[i].b + (stops[i + 1].b - stops[i].b) * u);
					return QColor(r, g, b);
				}
			}
			return QColor(stops[4].r, stops[4].g, stops[4].b);
		};
		for (int x = 0; x < W; ++x) {
			size_t idx = (size_t)((double)x / (double)W * (double)processed);
			if (idx >= processed) idx = processed - 1;
			int mn = bins[idx * 2 + 0];
			int mx = bins[idx * 2 + 1];
			float mag = (float)(std::abs(mn) + std::abs(mx)) / 32768.0f;
			if (mag > 1.0f) mag = 1.0f;
			QColor c = heatColor(mag);
			c.setAlpha(220);
			painter->setBrush(c);
			painter->drawRect(QRect(x, 0, 1, H));
		}
		painter->restore();
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
	m_fxDirty   = true;
	update();
}

void SoundView::scheduleFxRerender()
{
	m_fxDirty = true;
	if (!m_adaptToFx) return;
	// Single-shot, restarted on every push: the render only runs once the
	// user stops moving the slider for ~180 ms. Without this, dragging an EQ
	// band would run a full chain pass over the cached signal per pixel.
	if (!m_fxRenderTimer) {
		m_fxRenderTimer = new QTimer(this);
		m_fxRenderTimer->setSingleShot(true);
		m_fxRenderTimer->setInterval(180);
		connect(m_fxRenderTimer, &QTimer::timeout, this, [this]{ update(); });
	}
	m_fxRenderTimer->start();
}

void SoundView::setDisplayMode(DisplayMode m)
{
	if (m_displayMode == m) return;
	m_displayMode = m;
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
	scheduleFxRerender();
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
	// Only the reverb reaches the drawing (pitch / speed change playback
	// rate, not the file's own shape - see renderFxChain).
	if (reverbChanged) scheduleFxRerender();
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

// ---------------------------------------------------------------
// Real DSP preview
//
// The analyser gives us a MIN/MAX ENVELOPE over time. The old code treated
// the bin index as a FREQUENCY and multiplied bins by interpolated EQ band
// gains, so moving an EQ slider deformed the drawing in a way unrelated to
// what the audio actually does. Everything below runs the effects for real,
// sample by sample, on a decimated mono copy of the decoded file, using the
// exact same DSP classes as the audio thread; only the sample rate differs.
// ---------------------------------------------------------------

bool SoundView::fxViewWouldChangeAudio() const
{
	// Channel FxPanel reverb applies whether or not the sandbox is on.
	if (m_fxReverb > 0) return true;
	const SandboxState &s = m_sandbox;
	if (!s.enabled) return false;
	if (s.eqEnabled) {
		for (int i = 0; i < 16; ++i)
			if (std::fabs(s.eqBandDb[i]) > 0.01f) return true;
	}
	if (s.compEnabled)      return true;
	if (s.saturatorEnabled && s.saturatorMix > 0.001f) return true;
	if (s.gateEnabled)      return true;
	if (s.transEnabled && (std::fabs(s.transAttackDb) > 0.01f ||
	                       std::fabs(s.transSustainDb) > 0.01f)) return true;
	if (s.reverbWet > 0.001f) return true;
	if (s.limiterEnabled)   return true;
	if (s.bitcrusherEnabled && (s.bitcrusherBitDepth < 16 ||
	                            s.bitcrusherRate < 47999.0f)) return true;
	if (s.genLossEnabled && s.genLossGenerations > 1) return true;
	return false;
}

void SoundView::ensureFxSource()
{
	if (!m_vis) return;
	// The analyser publishes the cache progressively and keeps appending, so
	// stop re-pulling only once the decode is over AND we have already taken
	// the final copy. Waiting for completion before the FIRST pull is what
	// left the FX view dead whenever finalizeBins never ran.
	if (m_fxSrcComplete) return;
	std::vector<float> src;
	double rate = 0.0;
	if (!m_vis->getPreviewAudio(src, rate) || src.empty() || rate <= 0.0)
		return;                        // nothing decoded yet
	if (src.size() != m_fxSrc.size() || rate != m_fxSrcRate) {
		m_fxSrc     = std::move(src);
		m_fxSrcRate = rate;
		m_fxDirty   = true;
	}
	// "Done" = the worker has exited and the reveal state machine has seen it.
	if (!m_vis->isRunning() && m_analysisReady)
		m_fxSrcComplete = true;
}

void SoundView::renderFxChain(std::vector<float> &buf) const
{
	const SandboxState &s = m_sandbox;
	const double rate = m_fxSrcRate;
	const size_t n = buf.size();
	if (rate <= 0.0 || n == 0) return;

	// Reverb wet is the SUM of the sandbox room mix and the channel FxPanel
	// reverb - exactly how SlotDsp::refreshReverbWet combines them.
	float reverbWet = (s.enabled ? s.reverbWet : 0.0f)
	                + (float)m_fxReverb / 100.0f;
	if (reverbWet < 0.0f) reverbWet = 0.0f;
	if (reverbWet > 1.0f) reverbWet = 1.0f;

	// Walk the user's pipeline order so the preview reflects the chain the
	// way they arranged it. One full pass per stage: friendlier to the cache
	// than a per-sample switch, and each stage keeps its own state.
	for (int slot = 0; slot < SandboxState::Stage_COUNT; ++slot) {
		const int stage = s.pipelineOrder[slot];
		if (stage < 0 || stage >= SandboxState::Stage_COUNT) continue;

		switch (stage) {
		case SandboxState::Stage_EQ: {
			if (!s.enabled || !s.eqEnabled) break;
			// One biquad per MOVED band, cascaded - identical maths to
			// EqRack, minus the bands the user left flat (a flat peaking
			// section is a no-op, so skipping it changes nothing but the
			// render time).
			const double nyq = rate * 0.5;
			for (int b = 0; b < EqRack::kNumBands; ++b) {
				const double f0 = EqRack::bandFrequency(b);
				if (std::fabs(s.eqBandDb[b]) <= 0.01f) continue;
				// A band above the decimated Nyquist simply is not
				// representable here. Skipping is the honest answer; the
				// alternative (folding it in anyway) is what the old code
				// did wrong.
				if (f0 >= nyq * 0.9) continue;
				BiquadPeaking bq;
				bq.setParams(f0, EqRack::kQ, s.eqBandDb[b], rate);
				for (size_t i = 0; i < n; ++i) buf[i] = bq.process(buf[i]);
			}
			break;
		}
		case SandboxState::Stage_Compressor: {
			if (!s.enabled || !s.compEnabled) break;
			Compressor c;
			c.setSampleRate(rate);
			c.setParams(s.compThresholdDb, s.compRatio, s.compAttackMs,
			            s.compReleaseMs, s.compKneeDb, s.compMakeupDb);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; c.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_Saturator: {
			if (!s.enabled || !s.saturatorEnabled || s.saturatorMix <= 0.001f) break;
			Saturator sat;
			sat.setSampleRate(rate);
			sat.setParams(s.saturatorDrive, s.saturatorMix, s.saturatorTone,
			              (Saturator::Mode)s.saturatorMode);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; sat.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_NoiseGate: {
			if (!s.enabled || !s.gateEnabled) break;
			NoiseGate g;
			g.setSampleRate(rate);
			g.setParams(s.gateThresholdDb, s.gateRangeDb, s.gateAttackMs,
			            s.gateHoldMs, s.gateReleaseMs);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; g.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_TransientShaper: {
			if (!s.enabled || !s.transEnabled) break;
			TransientShaper ts;
			ts.setSampleRate(rate);
			ts.setParams(s.transAttackDb, s.transSustainDb);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; ts.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_Reverb: {
			if (reverbWet <= 0.001f) break;
			Reverb rv;
			rv.setSampleRate(rate);
			rv.setRoomSize(0.5f);      // same fixed recipe as SlotDsp
			rv.setDamping(0.5f);
			rv.setWet(reverbWet);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; rv.process(l, r); buf[i] = 0.5f * (l + r); }
			break;
		}
		case SandboxState::Stage_Limiter: {
			if (!s.enabled || !s.limiterEnabled) break;
			Limiter lim;
			lim.setSampleRate(rate);
			lim.setParams(s.limiterCeiling, s.limiterLookahead, s.limiterRelease,
			              (Limiter::Mode)s.limiterMode, s.limiterRatio,
			              s.limiterGateThresh);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; lim.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_Bitcrusher: {
			if (!s.enabled || !s.bitcrusherEnabled) break;
			Bitcrusher bc;
			bc.setSampleRate(rate);
			// The rate crush is a RATIO of the running rate; scale the target
			// so a "8 kHz out of 48 kHz" setting looks the same here.
			float target = s.bitcrusherRate * (float)(rate / 48000.0);
			bc.setParams(s.bitcrusherBitDepth, target);
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; bc.processStereo(l, r); buf[i] = l; }
			break;
		}
		case SandboxState::Stage_GenLoss: {
			if (!s.enabled || !s.genLossEnabled || s.genLossGenerations <= 1) break;
			GenerationLoss gl;
			gl.setSampleRate(rate);
			gl.setGenerations(s.genLossGenerations);
			gl.reset();
			for (size_t i = 0; i < n; ++i) { float l = buf[i], r = l; gl.processStereo(l, r); buf[i] = l; }
			break;
		}
		default:
			// Paulstretch, Spatial, Delay, Chorus, Flanger, Flangus, Phaser,
			// VoiceFx, DynEq, DeEsser, BassEnh, Binaural: either they change
			// the LENGTH of the signal (so they cannot share this x-axis) or
			// they live in the stereo field / a domain a mono decimated copy
			// cannot represent. They are deliberately left OUT rather than
			// faked - an invented smear is exactly what made the old preview
			// meaningless.
			break;
		}
	}

	// Failsafe brickwall, same place as in the real chain: after everything.
	// With it on, an EQ boost visibly flattens against the ceiling instead of
	// clipping; with it off the drawing runs past the frame, which is what the
	// audio really does.
	if (s.enabled && s.failsafeEnabled) {
		const float ceil = 0.891f;             // -1 dBFS
		for (size_t i = 0; i < n; ++i) {
			if (buf[i] >  ceil) buf[i] =  ceil;
			if (buf[i] < -ceil) buf[i] = -ceil;
		}
	}
}

void SoundView::buildPathsFxRatio(const std::vector<float> &dry,
                                  const std::vector<float> &wet, size_t bins)
{
	const double fhh = (double)height() * 0.5;
	const double fw  = (double)width();
	const double shortScale = 1.0 / ((double)std::numeric_limits<short>::max() * 1.1);
	m_path[0] = QPainterPath(QPointF(0.0, fhh));
	m_path[1] = QPainterPath(QPointF(0.0, fhh));
	if (bins == 0 || dry.empty() || dry.size() != wet.size()) return;

	const size_t n = dry.size();
	volatile const int *raw = m_vis ? m_vis->getBins() : nullptr;
	if (!raw) return;

	// Ceiling on the gain the drawing may show. A near-silent bin divides by a
	// tiny number; without a cap a single denormal would spike the path to the
	// moon. 16x = +24 dB, more than any single stage can add.
	const double kMaxRatio = 16.0;
	// Below this, the dry bin is silence: a ratio is meaningless, so anything
	// the chain produced there (reverb tail, delay, gate release) is drawn on
	// its own instead.
	const double kSilence  = 1.0e-5;

	for (size_t i = 0; i < bins; ++i) {
		size_t a = (size_t)((double)i       / (double)bins * (double)n);
		size_t b = (size_t)((double)(i + 1) / (double)bins * (double)n);
		if (b <= a) b = a + 1;
		if (b > n)  b = n;
		double dpk = 0.0, wpk = 0.0;
		for (size_t k = a; k < b && k < n; ++k) {
			const double da = std::fabs((double)dry[k]);
			const double wa = std::fabs((double)wet[k]);
			if (da > dpk) dpk = da;
			if (wa > wpk) wpk = wa;
		}

		// TRUE envelope for this bin, straight from the full-rate analysis.
		double vmin = (double)raw[i * 2]     * shortScale;
		double vmax = (double)raw[i * 2 + 1] * shortScale;

		if (dpk > kSilence) {
			double ratio = wpk / dpk;
			if (ratio > kMaxRatio) ratio = kMaxRatio;
			vmin *= ratio;
			vmax *= ratio;
		} else {
			// Pure wet content over silence.
			const double v = qMin(wpk, 1.0) / 1.1;
			vmin = -v;
			vmax =  v;
		}

		const double x = (double)i * (1.0 / 1024.0) * fw;
		m_path[0].lineTo(x, (1.0 + vmin) * fhh);
		m_path[1].lineTo(x, (1.0 + vmax) * fhh);
	}
	const double endx = fw * (double)bins * (1.0 / 1024.0);
	m_path[0].lineTo(endx, fhh);
	m_path[1].lineTo(endx, fhh);
	m_path[0].closeSubpath();
	m_path[1].closeSubpath();
}

void SoundView::buildPathsFromBins(size_t bins)
{
	const double fhh = (double)height() * 0.5;
	const double fw  = (double)width();
	const double shortScale = 1.0 / ((double)std::numeric_limits<short>::max() * 1.1);
	m_path[0] = QPainterPath(QPointF(0.0, fhh));
	m_path[1] = QPainterPath(QPointF(0.0, fhh));
	for (size_t i = 0; i < bins; ++i)
	{
		const double v0 = (double)m_vis->getBins()[i * 2]     * shortScale;
		const double v1 = (double)m_vis->getBins()[i * 2 + 1] * shortScale;
		const double x  = (double)i * (1.0 / 1024.0) * fw;
		m_path[0].lineTo(x, (1.0 + v0) * fhh);
		m_path[1].lineTo(x, (1.0 + v1) * fhh);
	}
	const double endx = fw * (double)bins * (1.0 / 1024.0);
	m_path[0].lineTo(endx, fhh);
	m_path[1].lineTo(endx, fhh);
	m_path[0].closeSubpath();
	m_path[1].closeSubpath();
}

void SoundView::preparePaths()
{
	if (!m_vis)
		return;
	// Bins SHRINK on EOF when the visualizer resamples a runaway
	// undershoot (more than numBins source bins generated) back down
	// to numBins. Without the != path, drawnBins would stay at the
	// progressive overshoot value, preparePaths would skip recompute,
	// and the user would keep seeing the pre-finalise paths whose
	// trailing bins live OFF-screen — the silence-at-end stays
	// invisible. Trigger recompute on any count change, not just
	// growth.
	const size_t bins = m_vis->getBinsProcessed();

	// The FX view engages whenever the sandbox is ON (or the channel reverb is
	// dialled in) - NOT only when a stage would actually move the drawing.
	// That keeps the "FX" badge meaning what it always meant ("this waveform
	// is the sandbox's output") and costs nothing: with an inert chain the
	// measured ratio is exactly 1, so the shortcut below draws the raw bins,
	// pixel for pixel.
	if (m_adaptToFx && (m_sandbox.enabled || m_fxReverb > 0)) {
		ensureFxSource();
		// Both halves are required: the decimated copy to MEASURE the effect
		// and the analyser bins to draw it on. Missing either one = raw view,
		// never a blank widget.
		if (!m_fxSrc.empty() && m_vis->getBins() && bins > 0) {
			if (m_fxDirty || !m_fxPathsActive || m_drawnBins != bins) {
				if (fxViewWouldChangeAudio()) {
					std::vector<float> work = m_fxSrc;
					renderFxChain(work);
					buildPathsFxRatio(m_fxSrc, work, bins);
				} else {
					buildPathsFromBins(bins);
				}
				m_drawnBins     = bins;
				m_fxDirty       = false;
				m_fxPathsActive = true;
			}
			return;
		}
		// Preview not available yet (still decoding): fall through and draw
		// the raw waveform rather than nothing.
	}

	if (m_drawnBins != bins || m_fxPathsActive) {
		buildPathsFromBins(bins);
		m_drawnBins     = bins;
		m_fxPathsActive = false;
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
