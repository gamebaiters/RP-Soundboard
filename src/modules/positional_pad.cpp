#include "positional_pad.h"
#include <QPainter>
#include <QMouseEvent>
#include <QtMath>

PositionalPad::PositionalPad(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 160);
    setMouseTracking(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void PositionalPad::setPosition(float x, float y) {
    if (x < -1) x = -1; if (x > 1) x = 1;
    if (y < -1) y = -1; if (y > 1) y = 1;
    if (x == m_x && y == m_y) return;
    m_x = x; m_y = y;
    update();
}

void PositionalPad::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e));
    p.setRenderHint(QPainter::Antialiasing, true);

    QRectF r = rect().adjusted(8, 8, -8, -8);
    p.setPen(QPen(QColor(0x4a, 0x4a, 0x4a), 1));
    p.drawEllipse(r);

    // Crosshair: head silhouette at center, "front" label towards top.
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));

    p.setPen(QPen(QColor(0x88, 0x88, 0x88)));
    p.drawText(QRectF(r.left(), r.top(), r.width(), 14),
               Qt::AlignHCenter, "front");
    p.drawText(QRectF(r.left(), r.bottom() - 14, r.width(), 14),
               Qt::AlignHCenter, "back");

    // Draw the source dot at (x, y).
    QPointF c = r.center();
    QPointF dot(c.x() + m_x * (r.width()  * 0.5),
                c.y() + m_y * (r.height() * 0.5));
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x4a, 0x90, 0xe2));
    p.drawEllipse(dot, 7, 7);
}

void PositionalPad::mousePressEvent(QMouseEvent *e) {
    updateFromMouse(e->localPos());
}

void PositionalPad::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & Qt::LeftButton) updateFromMouse(e->localPos());
}

void PositionalPad::updateFromMouse(const QPointF &localPos) {
    QRectF r = rect().adjusted(8, 8, -8, -8);
    QPointF c = r.center();
    float x = static_cast<float>((localPos.x() - c.x()) / (r.width()  * 0.5));
    float y = static_cast<float>((localPos.y() - c.y()) / (r.height() * 0.5));
    if (x < -1) x = -1; if (x > 1) x = 1;
    if (y < -1) y = -1; if (y > 1) y = 1;
    m_x = x; m_y = y;
    update();
    emit positionChanged(m_x, m_y);
}
