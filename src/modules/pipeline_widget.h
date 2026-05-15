#pragma once

#include <QWidget>
#include "../dsp/SandboxState.h"

// Horizontal bar of colored blocks, one per DSP stage, showing the
// pipeline order. Blocks past the pinned prefix can be drag-reordered.
// A plain click (no drag) emits stageClicked() so the host can jump
// the parameter panel to that effect.
class PipelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PipelineWidget(QWidget *parent = nullptr);

    void setOrder(const int order[SandboxState::Stage_COUNT]);
    void getOrder(int out[SandboxState::Stage_COUNT]) const;

signals:
    void orderChanged();
    void stageClicked(int stage);   // DspStage value of the clicked block

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;

private:
    int blockAtPos(int x) const;
    QRect blockRect(int index) const;

    int m_order[SandboxState::Stage_COUNT];
    int m_dragIndex = -1;
    int m_pressIndex = -1;
    int m_dragOffsetX = 0;
    int m_dragCurrentX = 0;
    bool m_dragging = false;
    bool m_movedDuringDrag = false;

    // Paulstretch (index 0) is a fixed-position module - it runs on a
    // separate streaming feed, so its slot is pinned and not draggable.
    static constexpr int kPinnedCount = 1;

    static constexpr int kBlockH = 32;
    static constexpr int kGap = 3;
    static constexpr int kMargin = 4;
};
