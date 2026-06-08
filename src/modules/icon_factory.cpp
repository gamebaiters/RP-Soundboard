#include "icon_factory.h"

#include <QPixmap>
#include <QPainter>
#include <QPolygonF>
#include <QPainterPath>
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
