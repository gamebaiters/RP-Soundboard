// WaveformPlayer - waveform display + transport controls (play/pause, stop,
// skip +/-5, +/-10, filename label, time readout).
// Uses the existing SoundView widget for the waveform paint + click-to-seek.

#pragma once

#include <QWidget>
#include "../SoundInfo.h"

class QLabel;
class QPushButton;
class SoundView;

class WaveformPlayer : public QWidget {
    Q_OBJECT
public:
    explicit WaveformPlayer(QWidget *parent = nullptr);

    QString filename()      const;
    bool    isPlaying()     const;
    bool    isPaused()      const;

public slots:
    void setSound(const SoundInfo &info);
    void setFilename(const QString &name);
    void setPosition(double seconds, double total);  // updates time label
    void setPlaybackFraction(double f);              // 0..1, paints overlay
    void clearPlayback();
    void setPlaying(bool on);
    void setPaused(bool on);
    // Toggle ONLY the waveform visualisation (the SoundView). Filename,
    // time label and transport buttons stay so the user can still
    // play / pause / seek even in the compact "no waveform" mode.
    void setWavePaintVisible(bool on);

signals:
    void playClicked();
    void pauseClicked();
    void stopClicked();
    void skip(int seconds);              // signed: -10, -5, +5, +10
    void seekRequested(double fraction); // 0..1 from waveform click

private slots:
    void onPlayPause();
    void onStop();
    void onWaveSeek(double frac);

private:
    static QString fmtTime(double sec);

    SoundView   *m_wave;
    QLabel      *m_filenameLabel;
    QLabel      *m_timeLabel;
    QPushButton *m_back10;
    QPushButton *m_back5;
    QPushButton *m_stop;
    QPushButton *m_playPause;
    QPushButton *m_fwd5;
    QPushButton *m_fwd10;
    bool         m_playing;
    bool         m_paused;
    QString      m_fullPath;   // unstripped path, returned by filename()
};
