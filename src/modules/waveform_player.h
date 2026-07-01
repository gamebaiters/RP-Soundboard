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
    bool    isReplayReady() const { return m_replayReady; }

    bool    isLooping()     const;
    bool    isReversed()    const;

    // Current cursor fraction (0..1) on the waveform. Used by the
    // replay path to honour the user-parked position; clamped to the
    // crop range by the wiring layer.
    double  cursorFraction() const;
    double  cropStartFraction() const;   // 0..1 (cropStart / totalLen)
    double  cropEndFraction()   const;   // 0..1 (1.0 if cropEnd<0)
    double  totalLength()       const { return m_totalLen; }

public slots:
    void setSound(const SoundInfo &info);
    void setFilename(const QString &name);
    void setPosition(double seconds, double total);  // updates time label
    // Show / hide the -10s / -5s / +5s / +10s skip buttons for this
    // channel. Driven by the global "Show skip buttons" setting.
    void setSkipButtonsVisible(bool on);
    // Toggle the underlying SoundView between waveform (default) and
    // spectrogram-style heatmap. Driven by the global setting.
    void setSpectrogramView(bool on);
    void setPlaybackFraction(double f);              // 0..1, paints overlay
    void clearPlayback();
    void setPlaying(bool on);
    void setPaused(bool on);
    // Mark the channel as "stopped but a sound is loaded and ready to
    // replay from the cursor". Switches the play/pause button to the
    // reload glyph; click emits replayClicked(). Auto-cleared when
    // setPlaying(true) fires or the channel is wiped (clearRequested).
    void setReplayReady(bool ready);
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
    // Emitted when the play/pause button fires in the replay-ready
    // state (audio finished naturally or was stopped, sound still
    // loaded). Wiring resolves the originating sound + restarts it on
    // this channel, seeking to the user-parked cursor afterwards.
    void replayClicked();
    // The user clicked the red "X" next to the filename label - wipe
    // every trace of the loaded sound on this channel: filename,
    // waveform, replay state, slot bookkeeping.
    void clearRequested();
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
    // Forwarded from SoundView: user right-button-dragged a loop area.
    void loopAreaSelected(double startSec, double endSec);

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
    // Red "X" next to the filename label. Visible only when there is
    // something to clear (a filename loaded or a replay context). Hidden
    // when the channel is truly empty.
    QPushButton *m_clearBtn = nullptr;
    bool         m_playing;
    bool         m_paused;
    // True when the channel is stopped but a sound is still loaded and
    // can be replayed by clicking the (now reload-glyph) play button.
    bool         m_replayReady = false;
    bool         m_looping;
    bool         m_reversed;
    QString      m_fullPath;   // unstripped path, returned by filename()
    // Mirror of the crop range so setPosition() can render the time
    // label in crop-relative form ("X / cropDur") instead of full-file
    // form when a trim is active - matches what the cursor visually
    // expresses now that everything ends at cropEnd consistently.
    double       m_cropStart = 0.0;
    double       m_cropEnd   = -1.0;
    // Total decoded length last reported via setPosition(). Used to
    // map cropStart/End seconds <-> waveform fraction without going
    // back through the audio thread.
    double       m_totalLen  = 0.0;
    // True while the filename label is displaying an error banner.
    // setFilename / setSound clear it; setError sets it. Keeps the
    // styling reset path explicit so the red bold doesn't leak into
    // subsequent successful playbacks.
    bool         m_errorActive = false;

    // Centralised icon + tooltip refresh for the play/pause button.
    // Looks at m_playing / m_paused / m_replayReady and picks the right
    // glyph (pause when playing, play when paused/idle, reload when
    // replay-ready). Called from every state setter so the button never
    // shows a stale glyph.
    void refreshPlayPauseAffordance();
    // Show / hide the clear "X" button to match the channel's loaded
    // state: visible iff filename is set OR replay-ready.
    void refreshClearButton();
};
