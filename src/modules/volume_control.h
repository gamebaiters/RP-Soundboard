// VolumeControl - dual local/remote volume sliders with link toggle.
// Pure UI module: emits value-changed signals, holds no audio state.

#pragma once

#include <QWidget>

class QSlider;
class QLabel;
class QToolButton;

class VolumeControl : public QWidget {
    Q_OBJECT
public:
    explicit VolumeControl(QWidget *parent = nullptr);

    int  local()  const;
    int  remote() const;
    bool linked() const;

public slots:
    void setLocal(int v);
    void setRemote(int v);
    void setLinked(bool on);
    // Re-apply theme-derived link button stylesheet.
    void refreshTheme();

signals:
    void localChanged(int v);
    void remoteChanged(int v);
    void linkedChanged(bool on);

private slots:
    void onLocalSliderMoved(int v);
    void onRemoteSliderMoved(int v);
    void onLinkClicked(bool on);

private:
    void applyLinkDelta(QSlider *driver, QSlider *follower, int newValue);

    void refreshTooltips();

    QSlider     *m_local;
    QSlider     *m_remote;
    QLabel      *m_localLabel;
    QLabel      *m_remoteLabel;
    QLabel      *m_capLocal  = nullptr;   // "Local" caption (hidden in compact)
    QLabel      *m_capRemote = nullptr;   // "Remote" caption
    QWidget     *m_help      = nullptr;   // help bubble (hidden in compact)
    QToolButton *m_link;
    int          m_linkDelta; // remote - local at link time
    bool         m_internalSync; // re-entrancy guard
};
