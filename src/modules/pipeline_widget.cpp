#include "pipeline_widget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include <algorithm>

namespace {
const QColor kStageColors[] = {
    QColor(0x5B, 0x9B, 0xD5),  // EQ - blue
    QColor(0xE0, 0x73, 0x35),  // Compressor - orange
    QColor(0xC0, 0x39, 0x2B),  // Saturator - red
    QColor(0x27, 0xAE, 0x60),  // Spatial - green
    QColor(0x8E, 0x44, 0xAD),  // Chorus - purple
    QColor(0x2C, 0x80, 0x8F),  // Flanger - teal
    QColor(0x16, 0xA0, 0x85),  // Flangus - dark teal
    QColor(0xD4, 0xAC, 0x0D),  // Phaser - gold
    QColor(0x7F, 0x8C, 0x8D),  // Delay - grey
    QColor(0x29, 0x80, 0xB9),  // Reverb - dark blue
    QColor(0x95, 0x5B, 0xA5),  // Limiter - mauve
    QColor(0xD3, 0x54, 0x00),  // Bitcrusher - burnt orange
    QColor(0x56, 0x6D, 0x7E),  // Mono - steel blue
    QColor(0x8B, 0x00, 0x00),  // GenLoss - dark red
};
}

PipelineWidget::PipelineWidget(QWidget *parent) : QWidget(parent)
{
    SandboxState::defaultPipelineOrder(m_order);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setMinimumHeight(kBlockH + kMargin * 2);
    setMaximumHeight(kBlockH + kMargin * 2);
}

void PipelineWidget::setOrder(const int order[SandboxState::Stage_COUNT])
{
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i)
        m_order[i] = order[i];
    update();
}

void PipelineWidget::getOrder(int out[SandboxState::Stage_COUNT]) const
{
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i)
        out[i] = m_order[i];
}

QRect PipelineWidget::blockRect(int index) const
{
    int totalGaps = (SandboxState::Stage_COUNT - 1) * kGap + kMargin * 2;
    int blockW = (width() - totalGaps) / SandboxState::Stage_COUNT;
    if (blockW < 20) blockW = 20;
    int x = kMargin + index * (blockW + kGap);
    return QRect(x, kMargin, blockW, kBlockH);
}

int PipelineWidget::blockAtPos(int x) const
{
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i) {
        QRect r = blockRect(i);
        if (x >= r.left() && x <= r.right()) return i;
    }
    return -1;
}

void PipelineWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);

    // Draw arrows between blocks
    p.setPen(QPen(palette().text().color(), 1));
    for (int i = 0; i < SandboxState::Stage_COUNT - 1; ++i) {
        if (m_dragging && i == m_dragIndex) continue;
        QRect r1 = blockRect(i);
        QRect r2 = blockRect(i + 1);
        int y = kMargin + kBlockH / 2;
        int x1 = r1.right() + 1;
        int x2 = r2.left() - 1;
        if (x2 > x1) {
            p.drawLine(x1, y, x2, y);
            p.drawLine(x2 - 3, y - 3, x2, y);
            p.drawLine(x2 - 3, y + 3, x2, y);
        }
    }

    for (int i = 0; i < SandboxState::Stage_COUNT; ++i) {
        if (m_dragging && i == m_dragIndex) continue;
        QRect r = blockRect(i);
        int stage = m_order[i];
        QColor c = (stage >= 0 && stage < SandboxState::Stage_COUNT)
                   ? kStageColors[stage] : QColor(128, 128, 128);
        p.setBrush(c);
        p.setPen(c.darker(130));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, SandboxState::stageName(stage));
    }

    // Draw dragged block on top
    if (m_dragging && m_dragIndex >= 0) {
        QRect r = blockRect(m_dragIndex);
        int dx = m_dragCurrentX - (r.left() + m_dragOffsetX);
        r.translate(dx, 0);
        int stage = m_order[m_dragIndex];
        QColor c = (stage >= 0 && stage < SandboxState::Stage_COUNT)
                   ? kStageColors[stage] : QColor(128, 128, 128);
        p.setOpacity(0.85);
        p.setBrush(c);
        p.setPen(c.darker(150));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, SandboxState::stageName(stage));
        p.setOpacity(1.0);
    }
}

void PipelineWidget::mousePressEvent(QMouseEvent *e)
{
    int idx = blockAtPos(e->x());
    if (idx >= 0) {
        m_dragIndex = idx;
        m_dragOffsetX = e->x() - blockRect(idx).left();
        m_dragCurrentX = e->x();
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        update();
    }
}

void PipelineWidget::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_dragging) return;
    m_dragCurrentX = e->x();

    int targetIdx = blockAtPos(e->x());
    if (targetIdx >= 0 && targetIdx != m_dragIndex) {
        int stage = m_order[m_dragIndex];
        if (targetIdx < m_dragIndex) {
            for (int i = m_dragIndex; i > targetIdx; --i)
                m_order[i] = m_order[i - 1];
        } else {
            for (int i = m_dragIndex; i < targetIdx; ++i)
                m_order[i] = m_order[i + 1];
        }
        m_order[targetIdx] = stage;
        m_dragIndex = targetIdx;
        m_dragOffsetX = e->x() - blockRect(targetIdx).left();
    }
    update();
}

void PipelineWidget::mouseReleaseEvent(QMouseEvent *)
{
    if (m_dragging) {
        m_dragging = false;
        m_dragIndex = -1;
        setCursor(Qt::OpenHandCursor);
        update();
        emit orderChanged();
    }
}
