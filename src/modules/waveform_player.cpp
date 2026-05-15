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

void WaveformPlayer::notifySeek() {
    m_wave->notifySeek();
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
    , m_fwd10(new QPushButton("+10s", this))
    , m_playing(false)
    , m_paused(false)
    , m_looping(false)
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
    connect(m_stop,      &QPushButton::clicked, this, &WaveformPlayer::onStop);
    connect(m_playPause, &QPushButton::clicked, this, &WaveformPlayer::onPlayPause);
    connect(m_wave,      &SoundView::seekRequested, this, &WaveformPlayer::onWaveSeek);
}

QString WaveformPlayer::filename() const  { return m_fullPath; }
bool    WaveformPlayer::isPlaying() const { return m_playing; }
bool    WaveformPlayer::isPaused()  const { return m_paused;  }
bool    WaveformPlayer::isLooping() const { return m_looping; }

void WaveformPlayer::setLooping(bool on) {
    if (m_looping == on) return;
    m_looping = on;
    QSignalBlocker b(m_loop);
    m_loop->setChecked(on);
}

void WaveformPlayer::setSound(const SoundInfo &info) {
    m_wave->setSound(info);
    setFilename(info.filename);
}

void WaveformPlayer::setFilename(const QString &name) {
    m_fullPath = name;
    // Display only the basename so the channel header doesn't get cluttered
    // with C:/Users/.../foo.mp3 style absolute paths.
    QString display = name;
    int slash = qMax(name.lastIndexOf('/'), name.lastIndexOf('\\'));
    if (slash >= 0) display = name.mid(slash + 1);
    m_filenameLabel->setText(display.isEmpty() ? tr("(no file)") : display);
    m_filenameLabel->setToolTip(name);
}

void WaveformPlayer::setPosition(double seconds, double total) {
    m_timeLabel->setText(fmtTime(seconds) + " / " + fmtTime(total));
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
