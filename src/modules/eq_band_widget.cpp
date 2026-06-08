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
    setRange(-12, 12);
    setValue(0);
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
    if (n < 4) n = 4;
    if (n > 32) n = 32;
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
            // Above thumb (cut): very dim regardless of audio.
            c.setRgb(base.red()/4, base.green()/4, base.blue()/4);
            c.setAlpha(95);
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
    // fires when audio is actually playing (lit > 0.01).
    if (lit > 0.01f) {
        int levelH = (int)(lit * trackH);
        for (int i = 0; i < n; ++i) {
            float tCell = (n - 1 - i) / (float)(n - 1);
            int yTop = trackY + (int)(i * (cellH + gap));
            int yBot = yTop + (int)cellH;
            int yMid = yTop + (int)(cellH * 0.5f);
            int meterTop = trackY + trackH - levelH;
            if (yBot < meterTop) continue;
            // Only paint the column INSIDE the lit (below-thumb) zone -
            // a column rising above the cut threshold would contradict
            // the visual statement of the slider.
            if (yMid < thumbY) continue;
            QColor base = stopColor(tCell);
            QColor hiC = base.lighter(170);
            hiC.setAlpha(220);
            p.setBrush(hiC);
            p.setPen(Qt::NoPen);
            p.drawRoundedRect(QRectF(trackX + 1, yTop, trackW - 2, cellH),
                              1.5, 1.5);
        }
    }

    // Slider thumb.
    p.setPen(QPen(QColor(245, 245, 245, 230), 2));
    p.drawLine(trackX, thumbY, trackX + trackW, thumbY);
    p.setBrush(QColor(245, 245, 245, 220));
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(trackX - 2, thumbY - 2, trackW + 4, 4),
                      2.0, 2.0);

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
    else if (chosen == boost) setValue(12);
    else if (chosen == cut)   setValue(-12);
}

void EqBandWidget::wheelEvent(QWheelEvent *e)
{
    // Vertical wheel adjusts by 1 dB per notch. Easier than dragging
    // the thin track. Shift = 3 dB.
    int delta = e->angleDelta().y();
    if (delta == 0) { QSlider::wheelEvent(e); return; }
    int step = (e->modifiers() & Qt::ShiftModifier) ? 3 : 1;
    setValue(value() + (delta > 0 ? step : -step));
    e->accept();
}
