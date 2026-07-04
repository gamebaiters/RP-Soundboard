#include "pipeline_widget.h"

#include <QPainter>
#include <QMouseEvent>
#include <QFontMetrics>
#include <QVector>
#include <algorithm>
#include <cstdlib>

namespace {
// Indexed by DspStage enum value (see SandboxState.h).
const QColor kStageColors[] = {
    QColor(0xD6, 0x4F, 0xB0),  // Paulstretch - magenta
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
    QColor(0x8B, 0x00, 0x00),  // GenLoss - dark red
    QColor(0x34, 0x49, 0x5E),  // Noise Gate - slate
    QColor(0xE8, 0x74, 0x9C),  // De-esser - pink
    QColor(0x1A, 0xBC, 0x9C),  // Transient - aqua
    QColor(0xF3, 0x9C, 0x12),  // Dynamic EQ - amber
    QColor(0x6C, 0x3E, 0xC9),  // Voice FX - violet
    QColor(0xA9, 0x32, 0x26),  // Bass Enh - brick
    QColor(0x5C, 0x6B, 0xC0),  // Binaural - indigo
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

void PipelineWidget::setHiddenStages(quint32 mask)
{
    if (m_hiddenMask == mask) return;
    m_hiddenMask = mask;
    update();
}

QVector<int> PipelineWidget::visibleIndices() const
{
    QVector<int> vis;
    vis.reserve(SandboxState::Stage_COUNT);
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i) {
        int stage = m_order[i];
        if (stage >= 0 && stage < SandboxState::Stage_COUNT &&
            !stageHidden(stage))
            vis.append(i);
    }
    return vis;
}

QRect PipelineWidget::blockRect(int visPos, int visCount) const
{
    if (visCount < 1) visCount = 1;
    int totalGaps = (visCount - 1) * kGap + kMargin * 2;
    int blockW = (width() - totalGaps) / visCount;
    if (blockW < 20) blockW = 20;
    int x = kMargin + visPos * (blockW + kGap);
    return QRect(x, kMargin, blockW, kBlockH);
}

int PipelineWidget::blockAtPos(int x) const
{
    const int visCount = visibleIndices().size();
    for (int k = 0; k < visCount; ++k) {
        QRect r = blockRect(k, visCount);
        if (x >= r.left() && x <= r.right()) return k;
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

    const QVector<int> vis = visibleIndices();
    const int visCount = vis.size();
    if (visCount == 0) return;

    // Visible position of the dragged ORDER index, -1 when not dragging.
    int dragVisPos = -1;
    if (m_dragging && m_dragIndex >= 0)
        dragVisPos = vis.indexOf(m_dragIndex);

    // Arrows between consecutive visible blocks.
    p.setPen(QPen(palette().text().color(), 1));
    for (int k = 0; k < visCount - 1; ++k) {
        if (m_dragging && k == dragVisPos) continue;
        QRect r1 = blockRect(k, visCount);
        QRect r2 = blockRect(k + 1, visCount);
        int y = kMargin + kBlockH / 2;
        int x1 = r1.right() + 1;
        int x2 = r2.left() - 1;
        if (x2 > x1) {
            p.drawLine(x1, y, x2, y);
            p.drawLine(x2 - 3, y - 3, x2, y);
            p.drawLine(x2 - 3, y + 3, x2, y);
        }
    }

    for (int k = 0; k < visCount; ++k) {
        if (m_dragging && k == dragVisPos) continue;
        QRect r = blockRect(k, visCount);
        int stage = m_order[vis[k]];
        QColor c = (stage >= 0 && stage < SandboxState::Stage_COUNT)
                   ? kStageColors[stage] : QColor(128, 128, 128);
        p.setBrush(c);
        p.setPen(c.darker(130));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, SandboxState::stageName(stage));
    }

    // Draw dragged block on top.
    if (m_dragging && dragVisPos >= 0) {
        QRect r = blockRect(dragVisPos, visCount);
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
    const QVector<int> vis = visibleIndices();
    int k = blockAtPos(e->x());
    int idx = (k >= 0 && k < vis.size()) ? vis[k] : -1;
    m_pressIndex = idx;
    m_movedDuringDrag = false;
    m_dragging = false;
    m_dragIndex = -1;
    // Pinned prefix (Paulstretch) cannot be dragged - still clickable.
    if (idx >= kPinnedCount) {
        m_dragIndex = idx;
        m_dragOffsetX = e->x() - blockRect(k, vis.size()).left();
        m_dragCurrentX = e->x();
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        update();
    }
}

void PipelineWidget::mouseMoveEvent(QMouseEvent *e)
{
    const QVector<int> vis = visibleIndices();
    if (!m_dragging) {
        // Hover feedback: pinned blocks read as clickable, not draggable.
        int k = blockAtPos(e->x());
        int idx = (k >= 0 && k < vis.size()) ? vis[k] : -1;
        setCursor(idx >= 0 && idx < kPinnedCount ? Qt::PointingHandCursor
                                                 : Qt::OpenHandCursor);
        return;
    }
    m_dragCurrentX = e->x();

    int kTarget = blockAtPos(e->x());
    if (kTarget >= 0 && kTarget < vis.size()) {
        int targetIdx = vis[kTarget];
        if (targetIdx < kPinnedCount) targetIdx = kPinnedCount;  // never displace the pin
        if (targetIdx != m_dragIndex) {
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
            int newVisPos = visibleIndices().indexOf(targetIdx);
            if (newVisPos >= 0)
                m_dragOffsetX = e->x() - blockRect(newVisPos,
                                                   vis.size()).left();
            m_movedDuringDrag = true;
        }
    }
    update();
}

void PipelineWidget::mouseReleaseEvent(QMouseEvent *e)
{
    bool wasDragging = m_dragging;
    m_dragging = false;
    m_dragIndex = -1;
    setCursor(Qt::OpenHandCursor);
    update();

    // A press+release on the same block with no reorder = a click:
    // jump the parameter panel to that effect instead of reordering.
    const QVector<int> vis = visibleIndices();
    int k = blockAtPos(e->x());
    int releaseIdx = (k >= 0 && k < vis.size()) ? vis[k] : -1;
    if (!m_movedDuringDrag && m_pressIndex >= 0 && releaseIdx == m_pressIndex) {
        emit stageClicked(m_order[m_pressIndex]);
    } else if (wasDragging && m_movedDuringDrag) {
        emit orderChanged();
    }
    m_pressIndex = -1;
    m_movedDuringDrag = false;
}
