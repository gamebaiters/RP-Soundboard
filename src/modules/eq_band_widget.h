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
#include <cmath>

// Slider range is stored in TENTHS of a decibel so the user can dial
// in a smooth 0.1 dB granularity: internal QSlider value in
// [-120, 120] maps to [-12.0, +12.0] dB. All external code that used
// to treat the value as integer dB must now divide by 10 before
// writing to SandboxState::eqBandDb[i], and multiply by 10 before
// pushing state back into the slider. Two static helpers below cover
// the conversions so callers do not have to bake the factor into a
// dozen call sites.
class EqBandWidget : public QSlider {
    Q_OBJECT
public:
    static constexpr int kSliderMin = -120;   // -12.0 dB
    static constexpr int kSliderMax =  120;   // +12.0 dB
    static constexpr int kSliderPerDb = 10;   // one dB = ten slider ticks

    static inline float sliderToDb(int v) { return static_cast<float>(v) / kSliderPerDb; }
    static inline int   dbToSlider(float dB) {
        int v = static_cast<int>(std::round(dB * kSliderPerDb));
        if (v < kSliderMin) v = kSliderMin;
        if (v > kSliderMax) v = kSliderMax;
        return v;
    }

    explicit EqBandWidget(QWidget *parent = nullptr);

    // 0..1 level read from the audio thread (channel peak). Drives the
    // reactive rising-column overlay so the user sees the band react
    // to audio. Smoothed inside the widget to avoid flicker.
    void setLevel(float level01);
    // Number of segmented LED cells (default 48 - tripled from the
    // original 16 so every frequency band, including high-end ones
    // like 16 kHz that rarely peak, has fine-grained visual
    // resolution and the listener can see the spectrum response of
    // bands that would otherwise barely register on a coarse meter).
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
    int   m_cellCount = 48;
};
