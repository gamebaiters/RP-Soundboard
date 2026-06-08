#pragma once
// EqBandWidget - vertical EQ band slider with LED-style colour gradient.
//
// Paint model:
//   * Vertical gradient: blue (bottom) -> green -> yellow -> red (top)
//   * Cells below the slider thumb are LIT (full saturation).
//   * Cells above the slider thumb are DIM (low alpha) so the user sees
//     the cut visually as "leds turned off".
//   * A reactive level overlay (setLevel) paints a brighter column
//     rising from the bottom up to the current audio level, simulating
//     the band's response to the playing audio.
//
// Inherits QSlider for free mouse/keyboard handling; only paint is
// custom.

#include <QSlider>

class EqBandWidget : public QSlider {
    Q_OBJECT
public:
    explicit EqBandWidget(QWidget *parent = nullptr);

    // 0..1 level read from the audio thread (channel peak). Drives the
    // reactive rising-column overlay so the user sees the band react
    // to audio. Smoothed inside the widget to avoid flicker.
    void setLevel(float level01);
    // Number of segmented LED cells (default 16 - matches the EQ band
    // count and gives a clean visual rhythm).
    void setCellCount(int n);

protected:
    void paintEvent(class QPaintEvent *e) override;
    void mousePressEvent(class QMouseEvent *e) override;
    void mouseMoveEvent(class QMouseEvent *e) override;
    void mouseReleaseEvent(class QMouseEvent *e) override;
    void contextMenuEvent(class QContextMenuEvent *e) override;
    void wheelEvent(class QWheelEvent *e) override;

private:
    void updateValueFromY(int y);
    bool m_dragging = false;

private:
    float m_level     = 0.0f;
    float m_levelShow = 0.0f;     // smoothed for display
    int   m_cellCount = 16;
};
