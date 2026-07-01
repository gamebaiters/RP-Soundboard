#include "eq_band_widget.h"

#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QStyle>
#include <QStyleOptionSlider>
#include <algorithm>
#include <cmath>

namespace {
// Gradient stops used both for the LED column and the level overlay.
// Bottom = cool blue (low energy), top = hot red (peak).
QColor stopColor(float t) {
    t = std::max(0.0f, std::min(1.0f, t));
    // 4-stop interpolation: blue -> cyan -> green -> yellow -> red.
    struct Stop { float at; int r,g,b; };
    static const Stop stops[] = {
        {0.00f,  30, 100, 220},
        {0.25f,  40, 200, 220},
        {0.50f,  70, 200,  70},
        {0.75f, 230, 200,  50},
        {1.00f, 230,  70,  60},
    };
    for (int i = 0; i + 1 < (int)(sizeof(stops)/sizeof(stops[0])); ++i) {
        if (t <= stops[i+1].at) {
            float u = (t - stops[i].at) / (stops[i+1].at - stops[i].at);
            float r = stops[i].r * (1.0f-u) + stops[i+1].r * u;
            float g = stops[i].g * (1.0f-u) + stops[i+1].g * u;
            float b = stops[i].b * (1.0f-u) + stops[i+1].b * u;
            return QColor((int)r, (int)g, (int)b);
        }
    }
    return QColor(stops[4].r, stops[4].g, stops[4].b);
}
} // namespace

EqBandWidget::EqBandWidget(QWidget *parent) : QSlider(Qt::Vertical, parent)
{
    // 0.1 dB granularity so slider drag feels smooth instead of stepping
    // integer decibels. External code multiplies/divides by 10 via the
    // dbToSlider / sliderToDb helpers in the header.
    setRange(kSliderMin, kSliderMax);
    setValue(0);
    setSingleStep(1);      // keyboard arrow = 0.1 dB
    setPageStep(10);       // PgUp/PgDn = 1.0 dB
    setMinimumWidth(22);
    setTracking(true);
}

void EqBandWidget::setLevel(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    m_level = v;
    float prevShow = m_levelShow;
    // Smoothed display value with fast attack, slower release.
    if (v > m_levelShow) m_levelShow = m_levelShow + 0.5f * (v - m_levelShow);
    else                 m_levelShow = m_levelShow + 0.10f * (v - m_levelShow);
    // Snap to floor when decay tail is below the LED quantisation step
    // so the silent-channel drain ticks actually settle (and stop
    // triggering paint events).
    if (m_levelShow < 0.002f) m_levelShow = 0.0f;
    // Change-detect: avoid repaint when both the input level and the
    // smoothed display value are already at rest. Lets the main_page
    // drain loop pump zeros after stop without paying paint cost on
    // every band.
    if (std::fabs(m_levelShow - prevShow) < 0.001f && prevShow == 0.0f && v == 0.0f)
        return;
    update();
}

void EqBandWidget::setCellCount(int n)
{
    // Upper clamp must stay >= the 48-cell default (header) - the old
    // 32 cap would have silently made the default unreachable for any
    // future caller.
    if (n < 4)  n = 4;
    if (n > 64) n = 64;
    m_cellCount = n;
    update();
}

void EqBandWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int W = width();
    const int H = height();
    const int marginX = 2;
    const int trackW = W - marginX * 2;
    const int trackX = marginX;
    const int marginY = 4;
    const int trackH = H - marginY * 2;
    const int trackY = marginY;

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 26));
    p.drawRoundedRect(QRect(trackX - 1, trackY - 1, trackW + 2, trackH + 2),
                      4.0, 4.0);

    int   v   = value();
    int   vmin = minimum();
    int   vmax = maximum();
    float frac = (vmax > vmin) ? (float)(v - vmin) / (float)(vmax - vmin) : 0.5f;
    int   thumbY = trackY + (int)((1.0f - frac) * trackH);

    // LIT factor controls how bright the below-thumb cells get. When
    // nothing is playing the cells are only slightly tinted - "warm
    // outline" colour, not glowing leds. When audio is active they
    // light up properly. lit = smoothed channel level (0..1).
    float lit = m_levelShow;
    if (lit < 0.0f) lit = 0.0f;
    if (lit > 1.0f) lit = 1.0f;

    int   n  = m_cellCount;
    int   gap = 1;
    float cellH = (trackH - (n - 1) * gap) / (float)n;
    for (int i = 0; i < n; ++i) {
        float tCell = (n - 1 - i) / (float)(n - 1);
        int yTop = trackY + (int)(i * (cellH + gap));
        int yMid = yTop + (int)(cellH * 0.5f);
        QColor base = stopColor(tCell);
        bool   below  = (yMid >= thumbY);
        QColor c;
        if (below) {
            // Idle "warm outline": low-saturation tinted dark. Active
            // (audio playing): blend toward full saturation by `lit`.
            QColor idle(base.red()/4 + 35,
                        base.green()/4 + 35,
                        base.blue()/4 + 35);
            int r = (int)(idle.red()   + (base.red()   - idle.red())   * lit);
            int g = (int)(idle.green() + (base.green() - idle.green()) * lit);
            int b = (int)(idle.blue()  + (base.blue()  - idle.blue())  * lit);
            c.setRgb(r, g, b);
            c.setAlpha(180 + (int)(65 * lit));
        } else {
            // Above thumb (cut): same audio-driven level response but at
            // strongly reduced saturation. Idle = dim outline (slightly
            // brighter than the previous flat dim so the cells stay
            // visible at rest). Active = halfway between idle and full
            // base colour, never reaching the brightness of the below-
            // thumb zone. This communicates "audio is here but the band
            // is cut" - the cells are still readable as a spectrum.
            QColor idle(base.red()/4,
                        base.green()/4,
                        base.blue()/4);
            QColor activeDim((base.red()   + idle.red())   / 2,
                             (base.green() + idle.green()) / 2,
                             (base.blue()  + idle.blue())  / 2);
            int r = (int)(idle.red()   + (activeDim.red()   - idle.red())   * lit);
            int g = (int)(idle.green() + (activeDim.green() - idle.green()) * lit);
            int b = (int)(idle.blue()  + (activeDim.blue()  - idle.blue())  * lit);
            c.setRgb(r, g, b);
            c.setAlpha(110 + (int)(60 * lit));
        }
        p.setBrush(c);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(trackX + 1, yTop, trackW - 2, cellH),
                          1.5, 1.5);
        QColor hi = c.lighter(150);
        p.setBrush(hi);
        p.drawRect(QRectF(trackX + 1, yTop, trackW - 2, 1.0));
    }

    // Rising column: brighter overlay capped by smoothed level. Only
    // fires when audio is actually playing (lit > 0.01). Above-thumb
    // cells in the meter range get a DIM ghost overlay so the user can
    // still read the spectrum even on a heavily cut band.
    if (lit > 0.01f) {
        int levelH = (int)(lit * trackH);
        for (int i = 0; i < n; ++i) {
            float tCell = (n - 1 - i) / (float)(n - 1);
            int yTop = trackY + (int)(i * (cellH + gap));
            int yBot = yTop + (int)cellH;
            int yMid = yTop + (int)(cellH * 0.5f);
            int meterTop = trackY + trackH - levelH;
            if (yBot < meterTop) continue;
            const bool above = (yMid < thumbY);
            QColor base = stopColor(tCell);
            QColor hiC;
            if (above) {
                // Above-thumb ghost: darker base, low alpha. Still gives
                // the impression "the cursor is below this cell but the
                // spectrum is visible".
                hiC = base.darker(170);
                hiC.setAlpha(120);
            } else {
                hiC = base.lighter(170);
                hiC.setAlpha(220);
            }
            p.setBrush(hiC);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(trackX + 1, yTop, trackW - 2, cellH),
                              1.5, 1.5);
        }
    }

    // Slider thumb. Enlarged to a chunkier 9-pixel bar (was 4) plus a
    // fine hairline at the exact centre so the user has a bigger target
    // to click / drag without losing sub-cell precision when reading
    // the current value. Widened past the track on both sides so the
    // grab handle protrudes visually like a physical fader cap.
    p.setPen(QPen(QColor(255, 255, 255, 240), 1));
    p.drawLine(trackX, thumbY, trackX + trackW, thumbY);
    p.setBrush(QColor(240, 240, 240, 235));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(trackX - 4, thumbY - 4, trackW + 8, 9),
                      3.0, 3.0);
    // Central hairline for precise readback of the exact set position.
    p.setPen(QPen(QColor(30, 30, 30, 180), 1));
    p.drawLine(trackX - 2, thumbY, trackX + trackW + 2, thumbY);

    int midY = trackY + trackH / 2;
    if (std::abs(midY - thumbY) > 6) {
        p.setPen(QPen(QColor(255, 255, 255, 60), 1, Qt::DashLine));
        p.drawLine(trackX, midY, trackX + trackW, midY);
    }
}

void EqBandWidget::updateValueFromY(int y)
{
    const int marginY = 4;
    const int trackH = height() - marginY * 2;
    const int trackY = marginY;
    if (trackH <= 0) return;
    float frac = 1.0f - (float)(y - trackY) / (float)trackH;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    int newVal = minimum() + (int)std::round(frac * (maximum() - minimum()));
    setValue(newVal);
}

void EqBandWidget::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        m_dragging = true;
        setSliderDown(true);
        updateValueFromY(e->pos().y());
        e->accept();
        return;
    }
    if (e->button() == Qt::MiddleButton) {
        setValue(0);
        e->accept();
        return;
    }
    QSlider::mousePressEvent(e);
}

void EqBandWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging && (e->buttons() & Qt::LeftButton)) {
        updateValueFromY(e->pos().y());
        e->accept();
        return;
    }
    QSlider::mouseMoveEvent(e);
}

void EqBandWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        setSliderDown(false);
        e->accept();
        return;
    }
    QSlider::mouseReleaseEvent(e);
}

void EqBandWidget::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    QAction *reset = menu.addAction(tr("Reset to 0 dB"));
    QAction *boost = menu.addAction(tr("Set to +12 dB"));
    QAction *cut   = menu.addAction(tr("Set to -12 dB"));
    QAction *chosen = menu.exec(e->globalPos());
    if (chosen == reset) setValue(0);
    else if (chosen == boost) setValue(kSliderMax);
    else if (chosen == cut)   setValue(kSliderMin);
}

void EqBandWidget::wheelEvent(QWheelEvent *e)
{
    // Vertical wheel adjusts by 0.5 dB per notch (5 slider ticks at
    // the new 0.1 dB granularity). Shift-wheel = 2 dB per notch. Both
    // feel more natural than the old 1 dB / 3 dB stepping which locked
    // out any decimal readings on the wheel path.
    int delta = e->angleDelta().y();
    if (delta == 0) { QSlider::wheelEvent(e); return; }
    int step = (e->modifiers() & Qt::ShiftModifier) ? 20 : 5;
    setValue(value() + (delta > 0 ? step : -step));
    e->accept();
}
