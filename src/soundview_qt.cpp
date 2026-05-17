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

			// Dim the trimmed-away regions.
			painter.setPen(Qt::NoPen);
			painter.setBrush(QColor(0, 0, 0, 150));
			if (hasStart)
				painter.drawRect(0, 0, startPixel, height() - 1);
			if (hasEnd)
				painter.drawRect(endPixel + 1, 0, w1 - (endPixel + 1), height() - 1);

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

	// Draw position cursor
	if (m_playbackPosition >= 0.0 && m_playbackPosition <= 1.0)
	{
		int posX = (int)(m_playbackPosition * (width() - 1));
		painter.setPen(QPen(QColor(255, 200, 0), 2));
		painter.drawLine(posX, 0, posX, height() - 1);
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
		QString label = progress < 0.95f
			? tr("Applying paulstretch... %1%").arg(static_cast<int>(progress * 100))
			: tr("Paulstretch ready");
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
	m_loadActive = false;
	m_loadTimer->stop();
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
		s.stretchFactor != m_sandbox.stretchFactor ||
		s.stretchWindowMs != m_sandbox.stretchWindowMs ||
		(s.stretchEnabled && !m_sandbox.stretchEnabled));

	bool needRedraw = m_adaptToFx && (
		s.enabled != m_sandbox.enabled ||
		s.stretchEnabled != m_sandbox.stretchEnabled ||
		s.stretchFactor != m_sandbox.stretchFactor ||
		s.stretchWindowMs != m_sandbox.stretchWindowMs ||
		s.eqEnabled != m_sandbox.eqEnabled ||
		s.compEnabled != m_sandbox.compEnabled ||
		s.compThresholdDb != m_sandbox.compThresholdDb ||
		s.compRatio != m_sandbox.compRatio ||
		s.saturatorEnabled != m_sandbox.saturatorEnabled ||
		s.saturatorDrive != m_sandbox.saturatorDrive ||
		s.saturatorMix != m_sandbox.saturatorMix ||
		s.bitcrusherEnabled != m_sandbox.bitcrusherEnabled ||
		s.bitcrusherBitDepth != m_sandbox.bitcrusherBitDepth ||
		s.bitcrusherRate != m_sandbox.bitcrusherRate ||
		s.monoEnabled != m_sandbox.monoEnabled ||
		s.genLossEnabled != m_sandbox.genLossEnabled ||
		s.genLossGenerations != m_sandbox.genLossGenerations ||
		s.chorusEnabled != m_sandbox.chorusEnabled ||
		s.chorusMix != m_sandbox.chorusMix ||
		s.flangerEnabled != m_sandbox.flangerEnabled ||
		s.flangerMix != m_sandbox.flangerMix ||
		s.flangusEnabled != m_sandbox.flangusEnabled ||
		s.flangusMix != m_sandbox.flangusMix ||
		s.phaserEnabled != m_sandbox.phaserEnabled ||
		s.phaserMix != m_sandbox.phaserMix ||
		s.delayEnabled != m_sandbox.delayEnabled ||
		s.delayMix != m_sandbox.delayMix ||
		s.limiterEnabled != m_sandbox.limiterEnabled ||
		s.reverbWet != m_sandbox.reverbWet);

	bool eqChanged = m_adaptToFx && s.eqEnabled;
	if (!needRedraw && eqChanged) {
		for (int i = 0; i < 16; ++i) {
			if (s.eqBandDb[i] != m_sandbox.eqBandDb[i]) { needRedraw = true; break; }
		}
	}

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

	if (needRedraw) {
		m_drawnBins = 0;
		update();
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
				float gain = std::pow(10.0f, gainDb / 20.0f);
				binsL[i] *= gain;
				binsR[i] *= gain;
			}
		}
	}

	// Compressor: roughly reduce peaks above threshold
	if (s.compEnabled && s.compRatio > 1.01f) {
		float threshLin = std::pow(10.0f, s.compThresholdDb / 20.0f);
		float ratio = s.compRatio;
		for (size_t i = 0; i < count; ++i) {
			for (float *ch : {&binsL[i], &binsR[i]}) {
				float a = std::fabs(*ch);
				if (a > threshLin) {
					float over = a - threshLin;
					float compressed = threshLin + over / ratio;
					float makeupLin = std::pow(10.0f, s.compMakeupDb / 20.0f);
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
		float ceil = std::pow(10.0f, s.limiterCeiling / 20.0f);
		for (size_t i = 0; i < count; ++i) {
			if (binsL[i] >  ceil) binsL[i] =  ceil;
			if (binsL[i] < -ceil) binsL[i] = -ceil;
			if (binsR[i] >  ceil) binsR[i] =  ceil;
			if (binsR[i] < -ceil) binsR[i] = -ceil;
		}
	}
}

void SoundView::preparePaths()
{
	SampleVisualizerThread &t = SampleVisualizerThread::GetInstance();
	size_t bins = t.getBinsProcessed();
	if(m_drawnBins < bins)
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
