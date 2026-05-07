#include "channel_meter.h"
#include <QPainter>
#include <QLinearGradient>
#include <algorithm>
#include <cmath>

namespace {
constexpr float kClipThreshold = 0.99f;
constexpr float kHotThreshold  = 0.85f;
// 96 segments = 4x previous resolution. Each segment = ~0.4 dB worth
// of level so the user can read fine differences in playback level.
constexpr int   kSegmentCount  = 96;
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
    if (l == m_l && r == m_r) return;
    m_l = l;
    m_r = r;
    update();
}

void ChannelMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    QRect bg = rect();
    p.fillRect(bg, QColor(0x14, 0x14, 0x14));
    p.setRenderHint(QPainter::Antialiasing, false);

    // Two stacked LED-style bars. Each bar is a row of small unlit
    // rectangles; lit segments show cyan -> amber -> red as the
    // signal climbs. A 1px outer frame keeps the meter readable on
    // top of the channel theme.
    p.setPen(QColor(0x33, 0x33, 0x33));
    p.drawRect(bg.adjusted(0, 0, -1, -1));

    int padX = 12;
    int barLeft = padX;
    int barRight = bg.width() - 4;
    int barW = barRight - barLeft;
    if (barW <= 0) return;
    // 96 segments need a thinner gap to fit the same widget width.
    // Compute everything off the available pixel budget so the meter
    // scales with the channel's panel size.
    int segGap = (barW > kSegmentCount * 2) ? 1 : 0;
    int segW = std::max(1, (barW - (kSegmentCount - 1) * segGap) / kSegmentCount);
    int actualBarW = segW * kSegmentCount + segGap * (kSegmentCount - 1);
    barLeft = padX + (barW - actualBarW) / 2;

    int barH = std::max(4, (bg.height() - 6) / 2);
    int gapY = 2;
    int top  = (bg.height() - (barH * 2 + gapY)) / 2;
    if (top < 1) top = 1;

    auto colorForSeg = [](float frac) {
        // 0.00..0.70 cyan, 0.70..0.90 amber, 0.90..1.00 red
        if (frac >= 0.90f) return QColor(0xe0, 0x41, 0x41);
        if (frac >= 0.70f) return QColor(0xe0, 0xa0, 0x20);
        return QColor(0x4a, 0xa0, 0xe2);
    };

    auto drawRow = [&](int y, float v, char label) {
        // Channel label (L/R) drawn at the far left.
        p.setPen(QColor(0xaa, 0xaa, 0xaa));
        p.drawText(QRect(2, y, padX - 2, barH),
                   Qt::AlignVCenter | Qt::AlignLeft, QString(QChar(label)));

        bool clipping = v >= kClipThreshold;
        float vClamp = std::min(v, 1.0f);
        int litCount = static_cast<int>(std::round(vClamp * kSegmentCount));
        if (litCount < 0) litCount = 0;
        if (litCount > kSegmentCount) litCount = kSegmentCount;

        // Outline: red flash bar around the meter when clipping.
        if (clipping) {
            p.setPen(QColor(0xe0, 0x41, 0x41));
            p.drawRect(barLeft - 1, y - 1, actualBarW + 1, barH + 1);
        }

        for (int i = 0; i < kSegmentCount; ++i) {
            int x = barLeft + i * (segW + segGap);
            QRect r(x, y, segW, barH);
            float frac = static_cast<float>(i + 1) / kSegmentCount;
            if (i < litCount) {
                p.fillRect(r, colorForSeg(frac));
            } else {
                // unlit segments: faint trace of where the segment will
                // land so the user sees the whole scale even at silence.
                p.fillRect(r, QColor(0x22, 0x2a, 0x32));
            }
        }
    };

    drawRow(top,                  m_l, 'L');
    drawRow(top + barH + gapY,    m_r, 'R');
}
