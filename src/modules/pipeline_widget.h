#pragma once

#include <QWidget>
#include "../dsp/SandboxState.h"

class PipelineWidget : public QWidget
{
    Q_OBJECT
public:
    explicit PipelineWidget(QWidget *parent = nullptr);

    void setOrder(const int order[SandboxState::Stage_COUNT]);
    void getOrder(int out[SandboxState::Stage_COUNT]) const;

signals:
    void orderChanged();

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
    int m_dragOffsetX = 0;
    int m_dragCurrentX = 0;
    bool m_dragging = false;

    static constexpr int kBlockH = 32;
    static constexpr int kGap = 3;
    static constexpr int kMargin = 4;
};
