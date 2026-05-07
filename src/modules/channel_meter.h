#pragma once

#include <QWidget>

// Two-channel peak meter (L on top, R below) drawn directly with QPainter.
// Cyan when the signal stays in headroom, flashes red when the absolute
// peak crosses the clipping threshold (~0.99 of full scale). The widget
// is dumb on its own - the host channel pushes peak values via setPeak()
// from a polling QTimer driven by the audio thread's atomic peak slots.
class ChannelMeter : public QWidget
{
    Q_OBJECT
public:
    explicit ChannelMeter(QWidget *parent = nullptr);

    void setPeak(float l, float r);

    QSize sizeHint() const override { return QSize(220, 28); }
    QSize minimumSizeHint() const override { return QSize(120, 24); }

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    float m_l = 0.0f;
    float m_r = 0.0f;
};
