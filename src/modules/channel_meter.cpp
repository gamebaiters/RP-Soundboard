#include "channel_meter.h"
#include "../AudioUtils.h"
#include "theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace {
constexpr float kFloorDb   = -60.0f;
// Peak-hold marker jumps up instantly, decays slowly so the user can
// read transient peaks after they have passed.
constexpr float kPeakDecay = 0.93f;
// Stop-slide decay rate. User reported the previous fast 0.45 value
// looked inconsistent with the "natural" release rate the meter uses
// during playback (the bar suddenly accelerated as soon as playback
// ended). Matching kPeakDecay keeps the visual rhythm continuous:
// the marker drains at the same dB/sec on stop as it does on a
// transient release mid-playback, so the user just sees the same
// decay curve carry through past the end of the audio.
constexpr float kStopDecay = kPeakDecay;

// Level (0..1.5 linear) -> 0..1 normalised position on a -60..0 dB scale.
float toNorm(float v) {
    float db = (v > 1e-6f) ? AudioUtils::linearToDb(v) : kFloorDb;
    if (db < kFloorDb) db = kFloorDb;
    if (db > 0.0f)     db = 0.0f;
    return (db - kFloorDb) / (0.0f - kFloorDb);
}
}

ChannelMeter::ChannelMeter(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    // Stay opaque (no Qt clear before paintEvent → no flicker), but
    // paint the 4 corner regions with the THEMED channel surface
    // colour instead of the previous hardcoded grey. See paintEvent
    // for the fill source.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

QSize ChannelMeter::sizeHint() const {
    return (m_orient == Horizontal) ? QSize(220, 28) : QSize(42, 72);
}

QSize ChannelMeter::minimumSizeHint() const {
    return (m_orient == Horizontal) ? QSize(120, 24) : QSize(36, 48);
}

void ChannelMeter::setOrientation(Orientation o) {
    if (o == m_orient) return;
    m_orient = o;
    // Push a fresh sizeHint through Qt's layout system - the surrounding
    // Channel row swaps horizontal-heavy for vertical-narrow footprint,
    // so parent layouts must recompute or the meter's old cell size
    // sticks around and either clips the new orientation or leaves an
    // empty gap.
    if (o == Horizontal) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    } else {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    }
    updateGeometry();
    update();
}

void ChannelMeter::setPeak(float l, float r)
{
    if (l < 0.0f) l = 0.0f;
    if (l > 1.5f) l = 1.5f;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.5f) r = 1.5f;
    // Silent-slot signal from the wiring is EXACTLY (0, 0) — real
    // audio essentially never produces a literal 0.0f peak on both
    // channels at once, so this branch reliably means "playback
    // ended". Switch the decay rate to a much faster value so the
    // marker SLIDES smoothly down to the left edge in ~480 ms, then
    // hits the floor snap and vanishes. User wanted "cursor goes
    // all the way to the start, then disappears" — not the previous
    // 4–5 s lazy decay, not the instant snap either.
    const float decay = (l == 0.0f && r == 0.0f) ? kStopDecay
                                                 : kPeakDecay;
    float newHoldL = (l > m_peakHoldL) ? l : std::max(l, m_peakHoldL * decay);
    float newHoldR = (r > m_peakHoldR) ? r : std::max(r, m_peakHoldR * decay);
    // Snap to floor so the multiplicative decay actually reaches zero
    // (without this, float -> denormal infinite-tail land and the
    // change-detect below keeps pumping micro-paints forever).
    constexpr float kHoldFloor = 1e-4f;   // -80 dB - well below the meter's -60 dB floor
    if (newHoldL < kHoldFloor) newHoldL = 0.0f;
    if (newHoldR < kHoldFloor) newHoldR = 0.0f;
    // Change-detect: once everything decays to zero the paint loop
    // stops, but only AFTER the marker has finished animating back
    // down. Previously the meter wiring sent a single drain tick on
    // playback-stop and never followed up - the marker froze where it
    // landed instead of completing the fall-off animation, the user
    // regression reported. The meter timer now pumps every tick and
    // this guard kills paints only at true rest (0,0,0,0).
    bool changed = (l != m_l) || (r != m_r)
                || (newHoldL != m_peakHoldL) || (newHoldR != m_peakHoldR);
    m_peakHoldL = newHoldL;
    m_peakHoldR = newHoldR;
    m_l = l;
    m_r = r;
    if (changed) update();
}

void ChannelMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect bg = rect();

    // Widget is opaque so EVERY pixel must be painted. Fill the 4
    // corners with the themed channel surface so the rounded panel
    // sits cleanly on the parent Channel widget's background.
    p.fillRect(bg, Theme::derivedCached().surface);

    // Rounded recessed LED panel.
    const qreal radius = 6.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x15, 0x17, 0x1a));
    p.drawRoundedRect(bg, radius, radius);
    p.setPen(QColor(0x3c, 0x3e, 0x42));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(bg).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    if (m_orient == Horizontal) {
        paintHorizontal(p);
    } else {
        paintVertical(p);
    }
}

void ChannelMeter::paintHorizontal(QPainter &p)
{
    const QRect bg = rect();
    const int padL = 13;   // room for the L / R label
    const int padR = 6;
    int barW = bg.width() - padL - padR;
    if (barW < 6) return;

    int barH = std::max(4, (bg.height() - 8) / 2);
    const int gapY = 3;
    int top = (bg.height() - (barH * 2 + gapY)) / 2;
    if (top < 2) top = 2;
    int barLeft = padL;

    // Continuous VU-style render. Replaces the old discrete-LED grid
    // (was ~6 px per segment → ~33 segments at typical channel width)
    // with a per-pixel gradient fill, so meter resolution equals widget
    // width: ~200 px instead of ~33 steps = ~6x more precision. The
    // colour zones (cyan / green / amber / red) become smooth gradient
    // transitions at the same dB landmarks the LED grid used, so the
    // visual "where am I on the scale" cue is preserved.

    // Bright (lit) gradient: cyan up to -12 dB, green to -6 dB, amber
    // to -3 dB, red beyond. Stops match the previous LED zone fracs
    // (0.55 / 0.78 / 0.90 of the -60..0 dB range).
    auto buildGrad = [](int x0, int x1, int alpha) {
        QLinearGradient g(x0, 0, x1, 0);
        QColor cyan (0x3f, 0xb0, 0xe0, alpha);
        QColor green(0x49, 0xc0, 0x55, alpha);
        QColor amber(0xe0, 0xa0, 0x22, alpha);
        QColor red  (0xe2, 0x4b, 0x4b, alpha);
        g.setColorAt(0.00, cyan);
        g.setColorAt(0.54, cyan);
        g.setColorAt(0.60, green);
        g.setColorAt(0.76, green);
        g.setColorAt(0.80, amber);
        g.setColorAt(0.88, amber);
        g.setColorAt(0.92, red);
        g.setColorAt(1.00, red);
        return g;
    };

    QFont lf = p.font();
    lf.setPixelSize(9);
    p.setFont(lf);

    auto drawRow = [&](int y, float v, float hold, char label) {
        p.setPen(QColor(0x9a, 0x9a, 0x9a));
        p.drawText(QRect(2, y, padL - 4, barH),
                   Qt::AlignVCenter | Qt::AlignLeft, QString(QChar(label)));

        QRectF track(barLeft, y, barW, barH);
        const qreal r = std::min<qreal>(barH * 0.45, 2.5);

        // Dim track: faint full-width gradient so the scale stays
        // readable when the signal is below the floor.
        p.setPen(Qt::NoPen);
        p.setBrush(buildGrad(barLeft, barLeft + barW, 38));
        p.drawRoundedRect(track, r, r);

        // Live level fill, clipped to [0..fillW]. Pixel-precise: a
        // 0.4 dB delta at -20 dB moves the fill ~1 px on a 200 px bar.
        float norm   = toNorm(v);
        float fillWf = norm * static_cast<float>(barW);
        int   fillW  = static_cast<int>(std::round(fillWf));
        if (fillW > 0) {
            p.save();
            QPainterPath clip;
            // Round corners only on the LEFT — the right edge of a
            // partial fill is a hard vertical so the level reads as a
            // crisp position, not a soft blob.
            clip.addRoundedRect(track, r, r);
            p.setClipPath(clip);
            p.setBrush(buildGrad(barLeft, barLeft + barW, 255));
            p.drawRect(QRectF(barLeft, y, fillW, barH));
            p.restore();
        }

        // Tick marks at the zone boundaries (~-12 / -6 / -3 dB) so
        // the eye still has reference points without an LED grid.
        p.setPen(QColor(0xff, 0xff, 0xff, 36));
        const float tickFracs[] = {0.55f, 0.78f, 0.90f};
        for (float tf : tickFracs) {
            int tx = barLeft + static_cast<int>(std::round(tf * barW));
            p.drawLine(tx, y + 1, tx, y + barH - 1);
        }

        // Peak-hold marker: rounded vertical bar at the held peak so
        // transients stay visible after the live level falls. Drawn
        // only while hold is above the floor — when setPeak receives
        // (0, 0) the floor snap forces hold to 0 so the marker
        // vanishes immediately (no slow tail). Rounded via a tiny
        // QPainterPath rounded rect: sharp 2 px line looked
        // out-of-place against the rounded track.
        if (hold > 1e-4f) {
            float peakNorm = toNorm(hold);
            int peakX = barLeft + static_cast<int>(
                std::round(peakNorm * barW));
            if (peakX >= barLeft && peakX <= barLeft + barW) {
                QColor pc = (peakNorm >= 0.90f) ? QColor(0xff, 0xc8, 0xc8)
                          : QColor(0xff, 0xff, 0xff);
                qreal mw = 2.6;          // bar width — same visual weight as the old 2 px line
                QRectF markerRect(
                    static_cast<qreal>(peakX) - mw * 0.5,
                    static_cast<qreal>(y) + 0.5,
                    mw,
                    static_cast<qreal>(barH) - 1.0);
                qreal mr = std::min<qreal>(mw * 0.5, barH * 0.35);
                p.setPen(Qt::NoPen);
                p.setBrush(pc);
                p.drawRoundedRect(markerRect, mr, mr);
            }
        }
    };

    drawRow(top,                m_l, m_peakHoldL, 'L');
    drawRow(top + barH + gapY,  m_r, m_peakHoldR, 'R');
}

void ChannelMeter::paintVertical(QPainter &p)
{
    const QRect bg = rect();

    // Vertical layout: two thin bars filling bottom -> top, tiny L / R
    // caption at the top of each. Same colour zones + peak-hold semantics
    // as the horizontal path, only the axes are swapped. Keeps the whole
    // widget inside the channel row's existing height so switching to
    // vertical never enlarges the channel.
    const int padTop = 12;   // room for the L / R caption
    const int padBot = 4;
    int barH = bg.height() - padTop - padBot;
    if (barH < 8) return;

    int barW = std::max(6, (bg.width() - 10) / 2);
    const int gapX = 4;
    int total = barW * 2 + gapX;
    int left = (bg.width() - total) / 2;
    if (left < 2) left = 2;
    int barTop = padTop;

    auto buildGrad = [](int y0, int y1, int alpha) {
        QLinearGradient g(0, y1, 0, y0);   // colour rises with level
        QColor cyan (0x3f, 0xb0, 0xe0, alpha);
        QColor green(0x49, 0xc0, 0x55, alpha);
        QColor amber(0xe0, 0xa0, 0x22, alpha);
        QColor red  (0xe2, 0x4b, 0x4b, alpha);
        g.setColorAt(0.00, cyan);
        g.setColorAt(0.54, cyan);
        g.setColorAt(0.60, green);
        g.setColorAt(0.76, green);
        g.setColorAt(0.80, amber);
        g.setColorAt(0.88, amber);
        g.setColorAt(0.92, red);
        g.setColorAt(1.00, red);
        return g;
    };

    QFont lf = p.font();
    lf.setPixelSize(9);
    p.setFont(lf);

    auto drawCol = [&](int x, float v, float hold, char label) {
        p.setPen(QColor(0x9a, 0x9a, 0x9a));
        p.drawText(QRect(x, 1, barW, padTop - 2),
                   Qt::AlignHCenter | Qt::AlignVCenter, QString(QChar(label)));

        QRectF track(x, barTop, barW, barH);
        const qreal r = std::min<qreal>(barW * 0.45, 2.5);

        // Dim track: faint full-height gradient.
        p.setPen(Qt::NoPen);
        p.setBrush(buildGrad(barTop, barTop + barH, 38));
        p.drawRoundedRect(track, r, r);

        // Live level fill.
        float norm   = toNorm(v);
        float fillH  = norm * static_cast<float>(barH);
        int   fillHi = static_cast<int>(std::round(fillH));
        if (fillHi > 0) {
            p.save();
            QPainterPath clip;
            clip.addRoundedRect(track, r, r);
            p.setClipPath(clip);
            p.setBrush(buildGrad(barTop, barTop + barH, 255));
            p.drawRect(QRectF(x, barTop + (barH - fillHi), barW, fillHi));
            p.restore();
        }

        // Tick marks at zone boundaries (~-12 / -6 / -3 dB).
        p.setPen(QColor(0xff, 0xff, 0xff, 36));
        const float tickFracs[] = {0.55f, 0.78f, 0.90f};
        for (float tf : tickFracs) {
            int ty = barTop + barH - static_cast<int>(std::round(tf * barH));
            p.drawLine(x + 1, ty, x + barW - 1, ty);
        }

        // Peak-hold marker.
        if (hold > 1e-4f) {
            float peakNorm = toNorm(hold);
            int peakY = barTop + barH - static_cast<int>(std::round(peakNorm * barH));
            if (peakY >= barTop && peakY <= barTop + barH) {
                QColor pc = (peakNorm >= 0.90f) ? QColor(0xff, 0xc8, 0xc8)
                          : QColor(0xff, 0xff, 0xff);
                qreal mh = 2.6;
                QRectF markerRect(
                    static_cast<qreal>(x) + 0.5,
                    static_cast<qreal>(peakY) - mh * 0.5,
                    static_cast<qreal>(barW) - 1.0,
                    mh);
                qreal mr = std::min<qreal>(mh * 0.5, barW * 0.35);
                p.setPen(Qt::NoPen);
                p.setBrush(pc);
                p.drawRoundedRect(markerRect, mr, mr);
            }
        }
    };

    drawCol(left,                    m_l, m_peakHoldL, 'L');
    drawCol(left + barW + gapX,      m_r, m_peakHoldR, 'R');
}
