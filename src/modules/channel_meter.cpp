#include "channel_meter.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr float kFloorDb   = -60.0f;
// Peak-hold marker jumps up instantly, decays slowly so the user can
// read transient peaks after they have passed.
constexpr float kPeakDecay = 0.93f;

// Level (0..1.5 linear) -> 0..1 normalised position on a -60..0 dB scale.
float toNorm(float v) {
    float db = (v > 1e-6f) ? 20.0f * std::log10(v) : kFloorDb;
    if (db < kFloorDb) db = kFloorDb;
    if (db > 0.0f)     db = 0.0f;
    return (db - kFloorDb) / (0.0f - kFloorDb);
}
}

ChannelMeter::ChannelMeter(QWidget *parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void ChannelMeter::setPeak(float l, float r)
{
    if (l < 0.0f) l = 0.0f;
    if (l > 1.5f) l = 1.5f;
    if (r < 0.0f) r = 0.0f;
    if (r > 1.5f) r = 1.5f;
    // Peak-hold tracks the maximum then decays toward the live level.
    m_peakHoldL = (l > m_peakHoldL) ? l : std::max(l, m_peakHoldL * kPeakDecay);
    m_peakHoldR = (r > m_peakHoldR) ? r : std::max(r, m_peakHoldR * kPeakDecay);
    m_l = l;
    m_r = r;
    update();
}

void ChannelMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect bg = rect();

    // The widget is opaque (WA_OpaquePaintEvent) so EVERY pixel must be
    // painted. Fill the whole rect with a channel-grey first: the four
    // corners left outside the rounded panel keep this color, so the
    // recessed panel reads as resting on the channel - never as garbage
    // and never as a detached box.
    p.fillRect(bg, QColor(0x33, 0x34, 0x37));

    // Rounded recessed LED panel.
    const qreal radius = 6.0;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x15, 0x17, 0x1a));
    p.drawRoundedRect(bg, radius, radius);
    p.setPen(QColor(0x3c, 0x3e, 0x42));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(bg).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    const int padL = 13;   // room for the L / R label
    const int padR = 6;
    int barW = bg.width() - padL - padR;
    if (barW < 6) return;

    int barH = std::max(4, (bg.height() - 8) / 2);
    const int gapY = 3;
    int top = (bg.height() - (barH * 2 + gapY)) / 2;
    if (top < 2) top = 2;

    // Adaptive segment count: ~6 px per segment, clamped so the meter
    // still reads as discrete LEDs at any channel width.
    int segCount = barW / 6;
    if (segCount < 6)  segCount = 6;
    if (segCount > 46) segCount = 46;
    int segGap = (barW > segCount * 3) ? 1 : 0;
    int segW   = (barW - (segCount - 1) * segGap) / segCount;
    if (segW < 1) segW = 1;
    int usedW   = segW * segCount + segGap * (segCount - 1);
    int barLeft = padL + (barW - usedW) / 2;   // always >= padL

    auto segColor = [](float frac, bool lit) -> QColor {
        QColor c;
        if      (frac >= 0.90f) c = QColor(0xe2, 0x4b, 0x4b);   // red
        else if (frac >= 0.78f) c = QColor(0xe0, 0xa0, 0x22);   // amber
        else if (frac >= 0.55f) c = QColor(0x49, 0xc0, 0x55);   // green
        else                    c = QColor(0x3f, 0xb0, 0xe0);   // cyan
        if (lit) return c;
        // Unlit: a faint trace of the zone color so the scale is visible.
        return QColor(c.red() / 6 + 0x18, c.green() / 6 + 0x1a,
                      c.blue() / 6 + 0x1e);
    };

    QFont lf = p.font();
    lf.setPixelSize(9);
    p.setFont(lf);

    auto drawRow = [&](int y, float v, float hold, char label) {
        p.setPen(QColor(0x9a, 0x9a, 0x9a));
        p.drawText(QRect(2, y, padL - 4, barH),
                   Qt::AlignVCenter | Qt::AlignLeft, QString(QChar(label)));

        int lit  = static_cast<int>(std::round(toNorm(v)    * segCount));
        int peak = static_cast<int>(std::round(toNorm(hold) * segCount));
        lit  = std::min(std::max(lit, 0), segCount);
        peak = std::min(std::max(peak, 0), segCount);

        p.setPen(Qt::NoPen);
        for (int i = 0; i < segCount; ++i) {
            int x = barLeft + i * (segW + segGap);
            float frac = static_cast<float>(i + 1) / segCount;
            // A segment is on if filled by the level OR it is the
            // held-peak segment (which stays lit after the peak passes).
            bool on = (i < lit) || (peak > 0 && i == peak - 1);
            p.setBrush(segColor(frac, on));
            p.drawRoundedRect(QRectF(x, y, segW, barH), 1.3, 1.3);
        }
    };

    drawRow(top,                m_l, m_peakHoldL, 'L');
    drawRow(top + barH + gapY,  m_r, m_peakHoldR, 'R');
}
