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
    transport->addSpacing(8);
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
        m_reversed = on; emit reverseToggled(on);
    });
    connect(m_stop,      &QPushButton::clicked, this, &WaveformPlayer::onStop);
    connect(m_playPause, &QPushButton::clicked, this, &WaveformPlayer::onPlayPause);
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
    QSignalBlocker b(m_reverse);
    m_reverse->setChecked(on);
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
    m_filenameLabel->setText(display.isEmpty() ? tr("(no file)") : display);
    m_filenameLabel->setToolTip(name);
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
    // Feed the real decoded duration to the waveform so crop-marker
    // fractions are computed against an accurate, per-channel length.
    m_wave->setTotalLength(total);
}

void WaveformPlayer::setPlaybackFraction(double f) {
    m_wave->setPlaybackPosition(f);
}

void WaveformPlayer::clearPlayback() {
    m_wave->clearPlayback();
    m_timeLabel->setText("0:00 / 0:00");
}

void WaveformPlayer::setPlaying(bool on) {
    m_playing = on;
    if (on) m_paused = false;
    bool isPlayingNow = on && !m_paused;
    m_playPause->setIcon(isPlayingNow ? IconFactory::pause()
                                      : IconFactory::play());
    m_playPause->setToolTip(isPlayingNow ? tr("Pause") : tr("Play"));
}

void WaveformPlayer::setPaused(bool on) {
    m_paused = on;
    bool isPlayingNow = m_playing && !on;
    m_playPause->setIcon(isPlayingNow ? IconFactory::pause()
                                      : IconFactory::play());
    m_playPause->setToolTip(isPlayingNow ? tr("Pause") : tr("Play"));
}

void WaveformPlayer::onPlayPause() {
    if (m_playing && !m_paused) emit pauseClicked();
    else                        emit playClicked();
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
