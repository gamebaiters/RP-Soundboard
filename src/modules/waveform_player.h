// WaveformPlayer - waveform display + transport controls (play/pause, stop,
// skip +/-5, +/-10, filename label, time readout).
// Uses the existing SoundView widget for the waveform paint + click-to-seek.

#pragma once

#include <QWidget>
#include "../SoundInfo.h"
#include "../dsp/SandboxState.h"

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

    bool    isLooping()     const;
    bool    isReversed()    const;

public slots:
    void setSound(const SoundInfo &info);
    void setFilename(const QString &name);
    void setPosition(double seconds, double total);  // updates time label
    void setPlaybackFraction(double f);              // 0..1, paints overlay
    void clearPlayback();
    void setPlaying(bool on);
    void setPaused(bool on);
    void setLooping(bool on);
    void setReversed(bool on);
    // Toggle ONLY the waveform visualisation (the SoundView). Filename,
    // time label and transport buttons stay so the user can still
    // play / pause / seek even in the compact "no waveform" mode.
    void setWavePaintVisible(bool on);
    void setAdaptToFx(bool on);
    void setSandboxState(const SandboxState &s);
    // Per-channel FxPanel state (simple pitch/speed/reverb) layered on
    // top of the sandbox-driven waveform visualisation. Slider units.
    void setLiveFx(int pitch, int speed, int reverb);
    void notifySeek();
    // Crop markers: range = actual crop applied to the slot (seconds,
    // endSeconds < 0 = open end); toggle = global "show crop markers".
    void setCropRange(double startSeconds, double endSeconds);
    void setShowCropMarkers(bool on);
    // Show an in-line playback error in place of the filename label
    // (red, bold, prefixed with a warning glyph). Auto-clears on the
    // next successful setFilename / setSound (i.e. when something else
    // is played in this channel). No popup, no modal interruption.
    void setError(const QString &message);

signals:
    void playClicked();
    void pauseClicked();
    void stopClicked();
    void skip(int seconds);              // signed: -10, -5, +5, +10
    void seekRequested(double fraction); // 0..1 from waveform click
    void loopToggled(bool on);
    void reverseToggled(bool on);
    // Right-click crop editor (forwarded from SoundView). Seconds are
    // already clamped to the slot's decoded length.
    void cropStartRequestedAt(double seconds);
    void cropEndRequestedAt(double seconds);
    void cropClearStartRequested();
    void cropClearEndRequested();
    void cropClearAllRequested();

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
    QPushButton *m_loop;
    QPushButton *m_reverse;
    QPushButton *m_fwd10;
    bool         m_playing;
    bool         m_paused;
    bool         m_looping;
    bool         m_reversed;
    QString      m_fullPath;   // unstripped path, returned by filename()
    // Mirror of the crop range so setPosition() can render the time
    // label in crop-relative form ("X / cropDur") instead of full-file
    // form when a trim is active - matches what the cursor visually
    // expresses now that everything ends at cropEnd consistently.
    double       m_cropStart = 0.0;
    double       m_cropEnd   = -1.0;
    // True while the filename label is displaying an error banner.
    // setFilename / setSound clear it; setError sets it. Keeps the
    // styling reset path explicit so the red bold doesn't leak into
    // subsequent successful playbacks.
    bool         m_errorActive = false;
};
