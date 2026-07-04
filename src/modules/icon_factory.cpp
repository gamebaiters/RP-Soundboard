#include "icon_factory.h"

#include <QPixmap>
#include <QPainter>
#include <QPolygonF>
#include <QPainterPath>
#include <QRadialGradient>
#include <cmath>

namespace {
// All glyphs are drawn on this canvas then handed to QIcon, which
// downscales smoothly. A generous inset keeps every glyph optically
// the same weight.
constexpr int kSize   = 64;
constexpr qreal kInset = 13.0;

QPixmap canvas() {
    QPixmap pm(kSize, kSize);
    pm.fill(Qt::transparent);
    return pm;
}

QIcon finish(const QPixmap &pm) {
    return QIcon(pm);
}
}

namespace IconFactory {

QIcon play(const QColor &c)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    // Right-pointing triangle, slightly inset on the right so it reads
    // as centred.
    QPolygonF tri;
    tri << QPointF(kInset + 2.0, kInset)
        << QPointF(kInset + 2.0, kSize - kInset)
        << QPointF(kSize - kInset, kSize / 2.0);
    QPainterPath path;
    path.addPolygon(tri);
    path.closeSubpath();
    p.drawPath(path);
    return finish(pm);
}

QIcon reverse(const QColor &c)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    // Two stacked left-pointing triangles (universal "fast rewind"
    // glyph) - reads as "go backward / from end to start".
    const qreal w = (kSize - kInset * 2.0) * 0.5;
    const qreal h = kSize - kInset * 2.0;
    qreal x1 = kInset;
    qreal x2 = kInset + w;
    auto leftTri = [&](qreal x){
        QPolygonF tri;
        tri << QPointF(x,         kSize / 2.0)
            << QPointF(x + w - 1, kInset)
            << QPointF(x + w - 1, kSize - kInset);
        QPainterPath path;
        path.addPolygon(tri);
        path.closeSubpath();
        p.drawPath(path);
    };
    leftTri(x1);
    leftTri(x2);
    (void)h;
    return finish(pm);
}

QIcon pause(const QColor &c)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const qreal barW = 12.0;
    const qreal gap  = 10.0;
    const qreal h    = kSize - kInset * 2.0;
    qreal x1 = kSize / 2.0 - gap / 2.0 - barW;
    qreal x2 = kSize / 2.0 + gap / 2.0;
    p.drawRoundedRect(QRectF(x1, kInset, barW, h), 2.5, 2.5);
    p.drawRoundedRect(QRectF(x2, kInset, barW, h), 2.5, 2.5);
    return finish(pm);
}

QIcon stop(const QColor &c)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const qreal s = kSize - kInset * 2.0;
    p.drawRoundedRect(QRectF(kInset, kInset, s, s), 3.5, 3.5);
    return finish(pm);
}

QIcon reload(const QColor &c)
{
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal pen = 7.0;
    QPen arcPen(c, pen, Qt::SolidLine, Qt::RoundCap);
    p.setPen(arcPen);
    p.setBrush(Qt::NoBrush);
    // ~290 degree arc, leaving a gap at the top-right for the arrowhead.
    QRectF arcRect(kInset, kInset,
                   kSize - kInset * 2.0, kSize - kInset * 2.0);
    const int startDeg = 70;
    const int spanDeg  = 290;
    p.drawArc(arcRect, startDeg * 16, spanDeg * 16);
    // Arrowhead at the arc's end (start + span), tangent to the circle.
    qreal cx = arcRect.center().x();
    qreal cy = arcRect.center().y();
    qreal r  = arcRect.width() / 2.0;
    qreal endRad = -(startDeg + spanDeg) * 3.14159265358979 / 180.0;
    QPointF tip(cx + r * std::cos(endRad), cy + r * std::sin(endRad));
    const qreal a = 11.0;
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    QPolygonF head;
    head << QPointF(tip.x() - a, tip.y() - a * 0.2)
         << QPointF(tip.x() + a * 0.4, tip.y() - a)
         << QPointF(tip.x() + a * 0.4, tip.y() + a * 0.9);
    p.drawPolygon(head);
    return finish(pm);
}

QIcon clear(const QColor &c)
{
    // Filled red disc with a centered white X. Same canvas + inset as
    // the rest of the set so it reads as the toolbar's "clear / unset"
    // affordance. Used next to the waveform filename label.
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const qreal s = kSize - kInset * 2.0;
    p.drawEllipse(QRectF(kInset, kInset, s, s));
    // White X: two diagonal strokes inset from the disc so the arms
    // don't visually clip the rim.
    const qreal armInset = kInset + 11.0;
    QPen xPen(Qt::white, 8.0, Qt::SolidLine, Qt::RoundCap);
    p.setPen(xPen);
    p.drawLine(QPointF(armInset,       armInset),
               QPointF(kSize - armInset, kSize - armInset));
    p.drawLine(QPointF(kSize - armInset, armInset),
               QPointF(armInset,       kSize - armInset));
    return finish(pm);
}

QIcon vinyl(const QColor &labelColor)
{
    // Vinyl record built to survive 16-20 px downscale AND look good
    // at 24-32 px: bold light rim (separates from dark buttons), a
    // subtly graded body, two groove rings, a specular sheen wedge,
    // and a large colored label with an inner ring + spindle dot.
    // Everything >= 3 px on the 64 px canvas so nothing turns to mush.
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = 5.0;
    const qreal s = kSize - inset * 2.0;
    const QRectF disc(inset, inset, s, s);
    const QPointF c = disc.center();

    // Body: gentle radial grade (lighter toward the rim) reads as a
    // curved lacquer surface even when tiny.
    QRadialGradient body(c, s * 0.5);
    body.setColorAt(0.0, QColor(0x26, 0x27, 0x2C));
    body.setColorAt(0.75, QColor(0x2E, 0x30, 0x36));
    body.setColorAt(1.0, QColor(0x3C, 0x3F, 0x47));
    p.setPen(QPen(QColor(0xC9, 0xCE, 0xD6), 3.5));   // bold light rim
    p.setBrush(body);
    p.drawEllipse(disc.adjusted(1.8, 1.8, -1.8, -1.8));

    // Two groove rings, two greys - the classic pressed-vinyl texture.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(0x60, 0x64, 0x6D), 2.6));
    qreal g1 = s * 0.145;
    p.drawEllipse(disc.adjusted(g1, g1, -g1, -g1));
    p.setPen(QPen(QColor(0x50, 0x53, 0x5B), 2.2));
    qreal g2 = s * 0.235;
    p.drawEllipse(disc.adjusted(g2, g2, -g2, -g2));

    // Specular sheen: a soft light arc across the upper-left grooves,
    // clipped between the rim and the label so it reads as reflection.
    {
        p.save();
        QPainterPath clip;
        clip.addEllipse(disc.adjusted(4.0, 4.0, -4.0, -4.0));
        QPainterPath hole;
        hole.addEllipse(c, s * 0.30, s * 0.30);
        p.setClipPath(clip.subtracted(hole));
        p.setPen(QPen(QColor(255, 255, 255, 46), 7.0, Qt::SolidLine,
                      Qt::RoundCap));
        QRectF arcRect = disc.adjusted(7.0, 7.0, -7.0, -7.0);
        p.drawArc(arcRect, 100 * 16, 55 * 16);
        p.drawArc(arcRect, 280 * 16, 55 * 16);
        p.restore();
    }

    // Label: large colored disc with a darker inner ring (record-label
    // print) - stays a clear colored dot at 16 px.
    const qreal lr = s * 0.26;
    p.setPen(Qt::NoPen);
    QRadialGradient lab(c, lr);
    lab.setColorAt(0.0, labelColor.lighter(118));
    lab.setColorAt(1.0, labelColor.darker(112));
    p.setBrush(lab);
    p.drawEllipse(c, lr, lr);
    p.setPen(QPen(labelColor.darker(135), 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, lr * 0.62, lr * 0.62);

    // Spindle dot.
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xF2, 0xEE, 0xE4));
    p.drawEllipse(c, 4.0, 4.0);
    return finish(pm);
}

QIcon sandbox(const QColor &c)
{
    // Three mixer faders: the universal "audio effects rack" glyph.
    QPixmap pm = canvas();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal top   = kInset;
    const qreal bot   = kSize - kInset;
    const qreal trkW  = 4.5;
    const qreal knobR = 7.0;
    const qreal xs[3]   = { kSize * 0.27, kSize * 0.50, kSize * 0.73 };
    const qreal knobY[3]= { kSize * 0.63, kSize * 0.34, kSize * 0.54 };
    QColor track = c;
    track.setAlpha(150);
    for (int i = 0; i < 3; ++i) {
        p.setPen(Qt::NoPen);
        p.setBrush(track);
        p.drawRoundedRect(QRectF(xs[i] - trkW / 2.0, top, trkW, bot - top),
                          2.0, 2.0);
        p.setBrush(c);
        p.drawEllipse(QPointF(xs[i], knobY[i]), knobR, knobR);
    }
    return finish(pm);
}

}
