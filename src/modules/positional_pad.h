#pragma once

#include <QWidget>

// 2D click+drag pad for picking a virtual sound-source position. Emits
// (x, y) in [-1, +1] where x is left/right and y is front (-1) / back (+1).
class PositionalPad : public QWidget {
    Q_OBJECT
public:
    explicit PositionalPad(QWidget *parent = nullptr);

    QSize sizeHint() const override { return QSize(220, 220); }

    void setPosition(float x, float y);    // programmatic move
    QPointF positionF() const { return QPointF(m_x, m_y); }

signals:
    void positionChanged(float x, float y);

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;

private:
    void updateFromMouse(const QPointF &localPos);

    float m_x = 0.0f;
    float m_y = 0.0f;
};
