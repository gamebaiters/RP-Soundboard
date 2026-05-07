// FineSlider - QSlider that always advances by exactly 1 step per mouse-wheel
// notch (regardless of QApplication::wheelScrollLines). Lets users dial in
// pitch / speed / reverb / volume one tick at a time.

#pragma once

#include <QSlider>

class FineSlider : public QSlider {
    Q_OBJECT
public:
    explicit FineSlider(Qt::Orientation orientation, QWidget *parent = nullptr);
    explicit FineSlider(QWidget *parent = nullptr);

protected:
    void wheelEvent(QWheelEvent *event) override;
    // Left-click on the track jumps the slider directly to the clicked
    // position (default Qt behavior is page-step, which feels sticky).
    void mousePressEvent(class QMouseEvent *event) override;
};
