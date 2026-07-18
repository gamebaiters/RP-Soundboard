#include "fine_slider.h"
#include "theme.h"

#include <QWheelEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QPainter>
#include <QPaintEvent>

FineSlider::FineSlider(Qt::Orientation orientation, QWidget *parent)
    : QSlider(orientation, parent) {}

FineSlider::FineSlider(QWidget *parent)
    : QSlider(parent) {}

void FineSlider::wheelEvent(QWheelEvent *event) {
    int delta = event->angleDelta().y();
    if (delta == 0) delta = event->angleDelta().x();
    if (delta == 0) { event->ignore(); return; }
    int step = (delta > 0) ? 1 : -1;
    if (invertedControls()) step = -step;
    int newVal = value() + step * singleStep();
    setValue(qBound(minimum(), newVal, maximum()));
    event->accept();
}

void FineSlider::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        QStyleOptionSlider opt;
        initStyleOption(&opt);
        QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt,
                                               QStyle::SC_SliderHandle, this);
        // Click on the handle = drag mode (default Qt). Click on the track
        // = jump-to-click. Without this, Qt does a page-step which feels
        // sticky for fine-control sliders.
        if (!handle.contains(event->pos())) {
            QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                  QStyle::SC_SliderGroove, this);
            int sliderMin, sliderMax, pos;
            if (orientation() == Qt::Horizontal) {
                sliderMin = groove.x();
                sliderMax = groove.right() - handle.width() + 1;
                pos = event->pos().x() - handle.width() / 2;
            } else {
                sliderMin = groove.y();
                sliderMax = groove.bottom() - handle.height() + 1;
                pos = event->pos().y() - handle.height() / 2;
            }
            int newVal = QStyle::sliderValueFromPosition(minimum(), maximum(),
                                                         pos - sliderMin,
                                                         sliderMax - sliderMin,
                                                         opt.upsideDown);
            setSliderPosition(newVal);
            triggerAction(SliderMove);
            setRepeatAction(SliderNoAction);
            event->accept();
            return;
        }
    }
    QSlider::mousePressEvent(event);
}

void FineSlider::paintEvent(QPaintEvent *event) {
    // A slider is BIPOLAR when its range straddles zero (pitch, speed,
    // mic gain) or when the owner tagged it explicitly. Qt's sub-page
    // always fills from the left edge, so a neutral bipolar slider read
    // as "half set" - here the track is repainted with the accent bar
    // growing OUT of the centre instead.
    const bool bipolar = (minimum() < 0 && maximum() > 0)
                      || property("bipolarFill").toBool();
    QSlider::paintEvent(event);
    if (!bipolar) return;

    // Geometry computed directly from the widget rect. The previous
    // version asked the style for the groove/handle rects and bailed
    // out when they came back empty - which is exactly what happens
    // with some stylesheet styles, leaving the default left-anchored
    // fill on screen (the bug the user kept seeing).
    const int hw = 14;                       // handle width (stylesheet)
    const Theme::Derived &d = Theme::derivedCached();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);

    if (orientation() == Qt::Horizontal) {
        const int span = qMax(1, width() - hw);
        const int pos  = QStyle::sliderPositionFromValue(
                             minimum(), maximum(), value(), span,
                             invertedAppearance()) + hw / 2;
        const int h  = 6;
        const int y  = (height() - h) / 2;
        const QRect track(0, y, width(), h);
        p.setBrush(d.surfaceAlt);
        p.drawRoundedRect(track, h / 2.0, h / 2.0);

        const int centre = width() / 2;
        const int x1 = qMin(centre, pos), x2 = qMax(centre, pos);
        if (x2 - x1 >= 2) {
            p.setBrush(d.accent);
            p.drawRoundedRect(QRect(x1, y, x2 - x1, h), h / 2.0, h / 2.0);
        }
    } else {
        const int span = qMax(1, height() - hw);
        const int pos  = QStyle::sliderPositionFromValue(
                             minimum(), maximum(), value(), span,
                             !invertedAppearance()) + hw / 2;
        const int w  = 6;
        const int x  = (width() - w) / 2;
        const QRect track(x, 0, w, height());
        p.setBrush(d.surfaceAlt);
        p.drawRoundedRect(track, w / 2.0, w / 2.0);

        const int centre = height() / 2;
        const int y1 = qMin(centre, pos), y2 = qMax(centre, pos);
        if (y2 - y1 >= 2) {
            p.setBrush(d.accent);
            p.drawRoundedRect(QRect(x, y1, w, y2 - y1), w / 2.0, w / 2.0);
        }
    }

    // Handle painted back on top of the repainted track.
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    opt.subControls = QStyle::SC_SliderHandle;
    style()->drawComplexControl(QStyle::CC_Slider, &opt, &p, this);
}
