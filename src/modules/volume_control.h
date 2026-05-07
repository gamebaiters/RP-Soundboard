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

    QSlider     *m_local;
    QSlider     *m_remote;
    QLabel      *m_localLabel;
    QLabel      *m_remoteLabel;
    QToolButton *m_link;
    int          m_linkDelta; // remote - local at link time
    bool         m_internalSync; // re-entrancy guard
};
