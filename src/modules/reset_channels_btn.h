// ResetChannelsBtn - dedicated button that resets ONLY the channel state
// (volumes, FX, waveform). Does NOT touch global settings or button
// assignments. Requires a confirmation dialog before firing.

#pragma once

#include <QPushButton>

class ResetChannelsBtn : public QPushButton {
    Q_OBJECT
public:
    explicit ResetChannelsBtn(QWidget *parent = nullptr);

signals:
    void resetRequested();

private slots:
    void onClicked();
};
