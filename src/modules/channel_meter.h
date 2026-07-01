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
    enum Orientation { Horizontal, Vertical };

    explicit ChannelMeter(QWidget *parent = nullptr);

    void setPeak(float l, float r);

    // Switch between horizontal (default: L bar on top, R below, both
    // running left->right) and vertical (L bar on left, R on right,
    // both running bottom->top). Vertical mode narrows the widget
    // footprint to ~40 px so it can drop into the channel row without
    // enlarging the channel height.
    void setOrientation(Orientation o);
    Orientation orientation() const { return m_orient; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    void paintHorizontal(class QPainter &p);
    void paintVertical  (class QPainter &p);

    float m_l = 0.0f;
    float m_r = 0.0f;
    float m_peakHoldL = 0.0f;   // slow-decay peak-hold marker
    float m_peakHoldR = 0.0f;
    Orientation m_orient = Horizontal;
};
