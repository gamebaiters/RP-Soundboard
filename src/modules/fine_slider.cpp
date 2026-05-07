#include "fine_slider.h"

#include <QWheelEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionSlider>

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
