#include "waveform_player.h"
#include "help_bubble.h"
#include "icon_factory.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QIcon>
#include <QSignalBlocker>

#include "../soundview_qt.h"

void WaveformPlayer::setWavePaintVisible(bool on) {
    m_wave->setVisible(on);
    if (auto *p = parentWidget()) p->updateGeometry();
    updateGeometry();
}

void WaveformPlayer::setAdaptToFx(bool on) {
    m_wave->setAdaptToFx(on);
}

void WaveformPlayer::setSandboxState(const SandboxState &s) {
    m_wave->setSandboxState(s);
}

void WaveformPlayer::setLiveFx(int pitch, int speed, int reverb) {
    m_wave->setLiveFx(pitch, speed, reverb);
}

void WaveformPlayer::notifySeek() {
    m_wave->notifySeek();
}

void WaveformPlayer::setCropRange(double startSeconds, double endSeconds) {
    m_cropStart = startSeconds;
    m_cropEnd   = endSeconds;
    m_wave->setCropRange(startSeconds, endSeconds);
}

void WaveformPlayer::setShowCropMarkers(bool on) {
    m_wave->setShowCropMarkers(on);
}

void WaveformPlayer::setSkipButtonsVisible(bool on) {
    if (m_back10) m_back10->setVisible(on);
    if (m_back5)  m_back5->setVisible(on);
    if (m_fwd5)   m_fwd5->setVisible(on);
    if (m_fwd10)  m_fwd10->setVisible(on);
}

void WaveformPlayer::setVinylButtonVisible(bool on) {
    if (m_vinylBtn) m_vinylBtn->setVisible(on);
}

QPoint WaveformPlayer::vinylButtonGlobalPos() const {
    if (!m_vinylBtn) return QPoint();
    return m_vinylBtn->mapToGlobal(QPoint(0, m_vinylBtn->height()));
}

void WaveformPlayer::setSpectrogramView(bool on) {
    if (m_wave) m_wave->setDisplayMode(on ? SoundView::Mode_Spectrogram
                                          : SoundView::Mode_Waveform);
}


WaveformPlayer::WaveformPlayer(QWidget *parent)
    : QWidget(parent)
    , m_wave(new SoundView(this))
    , m_filenameLabel(new QLabel(this))
    , m_timeLabel(new QLabel(this))
    , m_back10(new QPushButton("-10s", this))
    , m_back5(new QPushButton("-5s", this))
    , m_stop(new QPushButton(this))
    , m_playPause(new QPushButton(this))
    , m_fwd5(new QPushButton("+5s", this))
    , m_loop(new QPushButton(tr("Loop"), this))
    , m_reverse(new QPushButton(this))
    , m_fwd10(new QPushButton("+10s", this))
    , m_playing(false)
    , m_paused(false)
    , m_looping(false)
    , m_reversed(false)
{
    m_filenameLabel->setText(tr("(no file)"));
    m_filenameLabel->setMinimumWidth(80);
    // Clear button - red disc with white X. Sits to the LEFT of the
    // filename label and is only visible when there is actually a
    // loaded sound to wipe. setObjectName scopes the QSS so it doesn't
    // leak into TS3's qApp.
    m_clearBtn = new QPushButton(this);
    m_clearBtn->setObjectName("GBClearFileBtn");
    m_clearBtn->setIcon(IconFactory::clear());
    // Larger icon + bigger button so the "remove sound" affordance
    // reads as a real destructive action, not a tiny garnish next to
    // the filename. Previous 20x20 with a 16 px icon disappeared into
    // the strip; 26x26 with a 20 px icon is unmistakable. Background
    // tint is visible AT REST (not only on hover) so the user spots
    // the X before they have to fish for it.
    // Matches the other transport buttons in height + adds breathing
    // room around the X glyph so it lines up visually with the back/fwd
    // buttons instead of looking pinched against the filename label.
    m_clearBtn->setIconSize(QSize(18, 18));
    m_clearBtn->setFlat(true);
    m_clearBtn->setCursor(Qt::PointingHandCursor);
    m_clearBtn->setFixedSize(28, 28);
    m_clearBtn->setMinimumHeight(28);
    // NoFocus: clicking the X must not steal keyboard focus from the
    // surrounding row. Without this the button took focus on press;
    // when it then hid itself a frame later (no filename + no replay
    // = nothing to clear), Qt's focus chain handed focus to the next
    // focusable widget - the channel title QLineEdit - which selectAll-
    // s its contents on tab-focus and visually highlighted the channel
    // name as if the user had just clicked it.
    m_clearBtn->setFocusPolicy(Qt::NoFocus);
    m_clearBtn->setToolTip(tr("Remove the loaded sound from this channel"));
    m_clearBtn->setStyleSheet(
        // At-rest: soft red tint + rounded shape so the user sees the
        // remove affordance even without hovering. Hover bumps the
        // saturation; pressed deepens it.
        //
        // Padding 3 px on all sides centres the 18 px icon inside the
        // 28 px button consistently with the surrounding transport
        // buttons (which inherit Qt's default 4 px padding). The
        // earlier 0 padding made the icon kiss the border and the
        // overall control read as "pinched" next to the filename
        // label.
        "QPushButton#GBClearFileBtn {"
        "  background: rgba(220, 60, 60, 70);"
        "  border: 1px solid rgba(255, 110, 110, 130);"
        "  border-radius: 14px;"
        "  padding: 3px; margin: 0px;"
        "}"
        "QPushButton#GBClearFileBtn:hover {"
        "  background: rgba(255, 80, 80, 150);"
        "  border-color: rgba(255, 200, 200, 200);"
        "}"
        "QPushButton#GBClearFileBtn:pressed {"
        "  background: rgba(190, 40, 40, 220);"
        "}");
    m_clearBtn->hide();
    m_timeLabel->setText("0:00 / 0:00");
    m_timeLabel->setMinimumWidth(80);

    m_back10->setMinimumWidth(40);
    m_back5->setMinimumWidth(36);
    m_fwd5->setMinimumWidth(36);
    m_loop->setMinimumWidth(40);
    m_loop->setCheckable(true);
    m_loop->setToolTip(tr("Loop: repeat the sound endlessly until deactivated"));
    m_loop->setMinimumHeight(28);
    m_reverse->setCheckable(true);
    m_reverse->setIcon(IconFactory::reverse());
    m_reverse->setIconSize(QSize(20, 20));
    m_reverse->setMinimumWidth(36);
    m_reverse->setMinimumHeight(28);
    m_reverse->setToolTip(tr("Reverse playback: play the sound from end to start"));
    m_reverse->setStyleSheet(
        "QPushButton:checked { background-color: #c6691c; color: white;"
        " border: 1px solid #8a4a14; border-radius: 3px; }");
    m_fwd10->setMinimumWidth(40);

    m_stop->setIcon(IconFactory::stop());
    m_stop->setIconSize(QSize(20, 20));
    m_playPause->setIcon(IconFactory::play());
    m_playPause->setIconSize(QSize(20, 20));
    m_stop->setMinimumWidth(36);
    m_playPause->setMinimumWidth(36);

    m_back10->setToolTip(tr("Skip back 10 seconds"));
    m_back5->setToolTip(tr("Skip back 5 seconds"));
    m_stop->setToolTip(tr("Stop playback"));
    m_playPause->setToolTip(tr("Play / pause the current sound"));
    m_fwd5->setToolTip(tr("Skip forward 5 seconds"));
    m_loop->setStyleSheet(
        "QPushButton:checked { background-color: #3c8c3c; color: white;"
        " border: 1px solid #2a6e2a; border-radius: 3px; }");
    m_fwd10->setToolTip(tr("Skip forward 10 seconds"));

    m_wave->setMinimumHeight(28);
    m_wave->setMaximumHeight(40);
    m_wave->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *transport = new QHBoxLayout;
    transport->setContentsMargins(0,0,0,0);
    transport->setSpacing(2);
    transport->addWidget(m_back10);
    transport->addWidget(m_back5);
    transport->addWidget(m_stop);
    transport->addWidget(m_playPause);
    transport->addWidget(m_fwd5);
    transport->addWidget(m_fwd10);
    transport->addSpacing(4);
    transport->addWidget(m_loop);
    transport->addWidget(m_reverse);
    m_vinylBtn = new QPushButton(this);
    m_vinylBtn->setIcon(IconFactory::vinyl());
    m_vinylBtn->setIconSize(QSize(20, 20));
    m_vinylBtn->setMinimumWidth(32);
    m_vinylBtn->setMinimumHeight(28);
    m_vinylBtn->setToolTip(tr(
        "Vinyl deck: opens the turntable popup.\n"
        "Drag the disc to scratch, click it for a one-shot full stop."));
    transport->addWidget(m_vinylBtn);
    connect(m_vinylBtn, &QPushButton::clicked, this,
            [this]{ emit vinylClicked(); });
    transport->addSpacing(8);
    transport->addWidget(m_clearBtn);
    transport->addSpacing(6);   // breathing room before the filename label
    transport->addWidget(m_filenameLabel, 1);
    transport->addWidget(m_timeLabel);
    transport->addWidget(new HelpBubble(tr(
        "Transport for the currently loaded sound.\n"
        "Click anywhere on the waveform to seek to that point.\n"
        "+/- 5s and +/- 10s buttons jump by that many seconds.\n"
        "The right-side label shows: elapsed / total."), this));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->setSpacing(2);
    root->addLayout(transport);
    root->addWidget(m_wave);

    connect(m_back10,    &QPushButton::clicked, this, [this]{ emit skip(-10); });
    connect(m_back5,     &QPushButton::clicked, this, [this]{ emit skip( -5); });
    connect(m_fwd5,      &QPushButton::clicked, this, [this]{ emit skip( +5); });
    connect(m_fwd10,     &QPushButton::clicked, this, [this]{ emit skip(+10); });
    connect(m_loop,      &QPushButton::toggled, this, [this](bool on){
        m_looping = on; emit loopToggled(on);
    });
    connect(m_reverse,   &QPushButton::toggled, this, [this](bool on){
        m_reversed = on;
        m_wave->setReverse(on);   // played-tint flips side in reverse
        emit reverseToggled(on);
    });
    connect(m_stop,      &QPushButton::clicked, this, &WaveformPlayer::onStop);
    connect(m_playPause, &QPushButton::clicked, this, &WaveformPlayer::onPlayPause);
    connect(m_clearBtn,  &QPushButton::clicked, this, [this]{ emit clearRequested(); });
    connect(m_wave,      &SoundView::seekRequested, this, &WaveformPlayer::onWaveSeek);
    connect(m_wave, &SoundView::cropStartRequestedAt,
            this,   &WaveformPlayer::cropStartRequestedAt);
    connect(m_wave, &SoundView::cropEndRequestedAt,
            this,   &WaveformPlayer::cropEndRequestedAt);
    connect(m_wave, &SoundView::cropClearStartRequested,
            this,   &WaveformPlayer::cropClearStartRequested);
    connect(m_wave, &SoundView::cropClearEndRequested,
            this,   &WaveformPlayer::cropClearEndRequested);
    connect(m_wave, &SoundView::cropClearAllRequested,
            this,   &WaveformPlayer::cropClearAllRequested);
    connect(m_wave, &SoundView::loopAreaSelected,
            this,   &WaveformPlayer::loopAreaSelected);
}

QString WaveformPlayer::filename() const  { return m_fullPath; }
bool    WaveformPlayer::isPlaying() const { return m_playing; }
bool    WaveformPlayer::isPaused()  const { return m_paused;  }
bool    WaveformPlayer::isLooping() const { return m_looping; }
bool    WaveformPlayer::isReversed() const { return m_reversed; }

void WaveformPlayer::setLooping(bool on) {
    if (m_looping == on) return;
    m_looping = on;
    QSignalBlocker b(m_loop);
    m_loop->setChecked(on);
}

void WaveformPlayer::setReversed(bool on) {
    if (m_reversed == on) return;
    m_reversed = on;
    m_wave->setReverse(on);
    QSignalBlocker b(m_reverse);
    m_reverse->setChecked(on);
}

void WaveformPlayer::setStreamMode(bool on) {
    // Reverse + vinyl ARE available on streams: the tape ring keeps the recent
    // seconds in RAM (smooth within the window) and the streaming-reverse /
    // backfill paths seek the direct URL over HTTP range for anything older
    // (may stutter - fine for a stream). So this is intentionally a no-op; the
    // controls stay enabled. Kept as a hook in case a future build wants to
    // surface a "stream" affordance on the transport.
    Q_UNUSED(on);
}

void WaveformPlayer::setLiveStream(bool on) {
    m_liveStream = on;
    m_wave->setLiveStream(on);
    // A live stream can't be reversed, scratched or seeked. Force those off and
    // grey them out; loop stays (it just re-opens the URL). VOD videos never
    // reach here so their transport is untouched.
    if (on && m_reversed) {
        m_reversed = false;
        m_wave->setReverse(false);
        QSignalBlocker b(m_reverse);
        m_reverse->setChecked(false);
    }
    if (m_reverse)  m_reverse->setEnabled(!on);
    if (m_vinylBtn) m_vinylBtn->setEnabled(!on);
    if (m_back10)   m_back10->setEnabled(!on);
    if (m_back5)    m_back5->setEnabled(!on);
    if (m_fwd5)     m_fwd5->setEnabled(!on);
    if (m_fwd10)    m_fwd10->setEnabled(!on);
}

void WaveformPlayer::setSound(const SoundInfo &info) {
    m_wave->setSound(info);
    setFilename(info.filename);
}

void WaveformPlayer::setFilename(const QString &name) {
    // Any new filename (including the empty "(no file)" reset that
    // fires on stop) must wipe the error banner so the red bold from
    // a prior failure does not leak into the next playback.
    if (m_errorActive) {
        m_errorActive = false;
        m_filenameLabel->setStyleSheet(QString());
    }
    m_fullPath = name;
    // Display only the basename so the channel header doesn't get cluttered
    // with C:/Users/.../foo.mp3 style absolute paths.
    QString display = name;
    int slash = qMax(name.lastIndexOf('/'), name.lastIndexOf('\\'));
    if (slash >= 0) display = name.mid(slash + 1);
    m_filenameLabel->setTextFormat(Qt::PlainText);
    m_filenameLabel->setText(display.isEmpty() ? tr("(no file)") : display);
    m_filenameLabel->setToolTip(name);
    refreshClearButton();
}

void WaveformPlayer::setStreamLabel(const QString &title) {
    if (m_errorActive) {
        m_errorActive = false;
        m_filenameLabel->setStyleSheet(QString());
    }
    m_fullPath = title;
    // Blue globe prefix (azure) + the escaped title. Rich text so the prefix
    // can be coloured independently of the theme text colour. fromUtf8 for the
    // emoji avoids the MSVC narrow-literal encoding trap.
    const QString globe = QString::fromUtf8("\xF0\x9F\x8C\x90");   // 🌐
    m_filenameLabel->setTextFormat(Qt::RichText);
    m_filenameLabel->setText(
        "<span style='color:#3fa7ff; font-weight:bold;'>" + globe + "</span> "
        + title.toHtmlEscaped());
    m_filenameLabel->setToolTip(tr("Internet stream") + ": " + title);
    refreshClearButton();
}

void WaveformPlayer::setError(const QString &message) {
    // Replace the filename label with an in-line red banner. Scoped
    // QSS on the QLabel only -> no qApp / TS3 host leak (CLAUDE.md).
    // Hardcoded hex is deliberate: HSL math would have to dodge the
    // h=-1 grey trap on invalid QColors, and red is theme-agnostic
    // anyway.
    m_errorActive = true;
    m_fullPath.clear();
    const QString prefixed = QString::fromUtf8("\xE2\x9A\xA0 ") + message;
    m_filenameLabel->setText(prefixed);
    m_filenameLabel->setToolTip(message);
    m_filenameLabel->setStyleSheet(
        "QLabel { color: #ff5555; font-weight: bold;"
        " background-color: rgba(255, 80, 80, 32);"
        " border: 1px solid #ff5555; border-radius: 3px;"
        " padding: 0px 4px; }");
    // Time readout becomes meaningless once playback failed; reset it
    // so the user does not see a leftover "1:23 / 0:00" next to the
    // error.
    m_timeLabel->setText("0:00 / 0:00");
}

void WaveformPlayer::setPosition(double seconds, double total) {
    // Express progress against the ACTIVE region. With a crop active
    // the label reads "(seconds - cropStart) / (cropEnd - cropStart)"
    // so it lines up with what the cursor shows and what the audio
    // actually plays; with no crop it falls back to the full file
    // length. Without this the user saw the elapsed seconds run past
    // the visible end of the bar and the "total" number was the file
    // length, not the played duration.
    const bool cropActive = (m_cropStart > 0.0)
                         || (m_cropEnd   > 0.0 && m_cropEnd < total);
    double labelPos = seconds;
    double labelTot = total;
    if (cropActive && total > 0.0) {
        const double start = (m_cropStart > 0.0) ? m_cropStart : 0.0;
        const double end   = (m_cropEnd   > 0.0) ? m_cropEnd   : total;
        labelPos = seconds - start;
        if (labelPos < 0.0)    labelPos = 0.0;
        labelTot = end - start;
        if (labelTot < 0.0)    labelTot = 0.0;
        if (labelPos > labelTot) labelPos = labelTot;
    }
    m_timeLabel->setText(fmtTime(labelPos) + " / " + fmtTime(labelTot));
    m_totalLen = total;
    // Feed the real decoded duration to the waveform so crop-marker
    // fractions are computed against an accurate, per-channel length.
    m_wave->setTotalLength(total);
}

double WaveformPlayer::cropStartFraction() const {
    if (m_totalLen <= 0.0) return 0.0;
    if (m_cropStart <= 0.0) return 0.0;
    double f = m_cropStart / m_totalLen;
    return (f < 0.0) ? 0.0 : ((f > 1.0) ? 1.0 : f);
}

double WaveformPlayer::cropEndFraction() const {
    if (m_totalLen <= 0.0)       return 1.0;
    if (m_cropEnd  <= 0.0)       return 1.0;
    double f = m_cropEnd / m_totalLen;
    return (f < 0.0) ? 0.0 : ((f > 1.0) ? 1.0 : f);
}

void WaveformPlayer::setPlaybackFraction(double f) {
    m_wave->setPlaybackPosition(f);
}

void WaveformPlayer::clearPlayback() {
    m_wave->clearPlayback();
    m_timeLabel->setText("0:00 / 0:00");
    m_totalLen = 0.0;
    m_cropStart = 0.0;
    m_cropEnd   = -1.0;
}

void WaveformPlayer::setPlaying(bool on) {
    m_playing = on;
    if (on) {
        m_paused = false;
        // Active playback never sits on top of "replay-ready" - it's the
        // opposite state.
        m_replayReady = false;
    }
    // Loaded-but-stopped visual: ghost the waveform so the user can
    // tell at a glance the file is loaded but not actively playing.
    // Pause does NOT flip this (setPaused leaves m_playing true so
    // this stays unghosted) — matches the user's mental model where
    // paused = "playing, just frozen".
    if (m_wave) m_wave->setGhosted(!on);
    refreshPlayPauseAffordance();
    refreshClearButton();
}

void WaveformPlayer::setPaused(bool on) {
    m_paused = on;
    refreshPlayPauseAffordance();
}

void WaveformPlayer::setReplayReady(bool ready) {
    if (m_replayReady == ready) return;
    m_replayReady = ready;
    refreshPlayPauseAffordance();
    refreshClearButton();
}

void WaveformPlayer::refreshPlayPauseAffordance() {
    // State machine:
    //   playing && !paused  -> pause glyph, click pauses
    //   playing &&  paused  -> play glyph, click resumes
    //  !playing &&  replay  -> reload glyph, click restarts from cursor
    //  !playing && !replay  -> play glyph (idle / no sound loaded)
    if (m_playing && !m_paused) {
        m_playPause->setIcon(IconFactory::pause());
        m_playPause->setToolTip(tr("Pause"));
    } else if (!m_playing && m_replayReady) {
        m_playPause->setIcon(IconFactory::reload());
        m_playPause->setToolTip(tr("Replay from cursor"));
    } else {
        m_playPause->setIcon(IconFactory::play());
        m_playPause->setToolTip(tr("Play"));
    }
}

void WaveformPlayer::refreshClearButton() {
    if (!m_clearBtn) return;
    const bool hasContent = !m_fullPath.isEmpty() || m_replayReady;
    m_clearBtn->setVisible(hasContent);
}

double WaveformPlayer::cursorFraction() const {
    return m_wave ? m_wave->currentPosition() : -1.0;
}

void WaveformPlayer::onPlayPause() {
    if (m_playing && !m_paused) {
        emit pauseClicked();
    } else if (m_playing && m_paused) {
        emit playClicked();
    } else if (m_replayReady) {
        emit replayClicked();
    } else {
        // Idle with no loaded sound: nothing to play. Falling through to
        // playClicked() would emit unpausePlayback() on an empty slot -
        // harmless but pointless. Drop the click.
    }
}

void WaveformPlayer::onStop() {
    emit stopClicked();
}

void WaveformPlayer::onWaveSeek(double frac) {
    emit seekRequested(frac);
}

QString WaveformPlayer::fmtTime(double sec) {
    if (sec < 0) sec = 0;
    int s = static_cast<int>(sec + 0.5);
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
