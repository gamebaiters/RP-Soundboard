#include "fx_panel.h"
#include "../AudioUtils.h"
#include "help_bubble.h"
#include "fine_slider.h"
#include "theme.h"

#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVariant>
#include <cmath>

// Pitch/Speed sliders feed factor = 3^(v/100). Display the factor.
static QString fmtFactor(int v) {
    double factor = AudioUtils::sliderToPitchFactor(v);
    return QString::number(factor, 'f', 2) + "x";
}
static QString fmtPlain(int v)  { return QString::number(v); }

FxPanel::FxPanel(QWidget *parent)
    : QWidget(parent)
    , m_pitch(new FineSlider(Qt::Horizontal, this))
    , m_speed(new FineSlider(Qt::Horizontal, this))
    , m_reverb(new FineSlider(Qt::Horizontal, this))
    , m_pitchLabel(new QLabel(this))
    , m_speedLabel(new QLabel(this))
    , m_reverbLabel(new QLabel(this))
    , m_sync(new QToolButton(this))
    , m_reset(new QToolButton(this))
    , m_internalSync(false)
{
    m_pitch->setRange(kPitchMin, kPitchMax);
    m_speed->setRange(kSpeedMin, kSpeedMax);
    m_reverb->setRange(kReverbMin, kReverbMax);
    for (auto *s : {m_pitch, m_speed, m_reverb}) {
        s->setSingleStep(1);
        s->setPageStep(10);
    }
    m_pitch->setValue(0);
    m_speed->setValue(0);
    m_reverb->setValue(0);
    for (auto *s : {m_pitch, m_speed, m_reverb})
        s->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    m_pitchLabel->setText(fmtFactor(0));
    m_speedLabel->setText(fmtFactor(0));
    m_reverbLabel->setText(fmtPlain(0));
    m_pitchLabel->setMinimumWidth(46);
    m_speedLabel->setMinimumWidth(46);
    m_reverbLabel->setMinimumWidth(46);

    m_sync->setText(QString::fromUtf8("\xF0\x9F\x94\x97"));
    m_sync->setCheckable(true);
    m_sync->setToolTip(tr("Link pitch and speed"));
    m_sync->setProperty("syncRole", QVariant(QString("link")));
    connect(m_sync, &QToolButton::toggled, this, [this](bool){ refreshTheme(); });

    m_reset->setText(tr("Reset"));
    m_reset->setToolTip(tr("Reset pitch / speed / reverb to zero"));
    refreshTheme();

    auto *capPitch  = new QLabel(tr("Pitch"),  this);
    auto *capSpeed  = new QLabel(tr("Speed"),  this);
    auto *capReverb = new QLabel(tr("Reverb"), this);
    for (auto *c : {capPitch, capSpeed, capReverb}) {
        c->setMinimumWidth(48);
        c->setMaximumWidth(60);
    }

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0,0,0,0);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);
    grid->setColumnStretch(0, 0);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 0);
    auto *help = new HelpBubble(tr(
        "Pitch  = changes the perceived note (higher / lower).\n"
        "Speed  = changes how fast the audio plays back.\n"
        "Reverb = adds a room/space effect (0..100).\n"
        "Click the chain to lock pitch and speed together so they\n"
        "move 1:1. Reverb is always independent. Reset puts pitch /\n"
        "speed / reverb back to zero."), this);

    grid->addWidget(capPitch,      0, 0);
    grid->addWidget(m_pitch,       0, 1);
    grid->addWidget(m_pitchLabel,  0, 2);
    grid->addWidget(m_sync,        0, 3, 2, 1, Qt::AlignVCenter);
    grid->addWidget(help,          0, 4, 3, 1, Qt::AlignVCenter);
    grid->addWidget(capSpeed,      1, 0);
    grid->addWidget(m_speed,       1, 1);
    grid->addWidget(m_speedLabel,  1, 2);
    grid->addWidget(capReverb,     2, 0);
    grid->addWidget(m_reverb,      2, 1);
    grid->addWidget(m_reverbLabel, 2, 2);
    grid->addWidget(m_reset,       2, 3);

    connect(m_pitch,  &QSlider::valueChanged,  this, &FxPanel::onPitchMoved);
    connect(m_speed,  &QSlider::valueChanged,  this, &FxPanel::onSpeedMoved);
    connect(m_reverb, &QSlider::valueChanged,  this, &FxPanel::onReverbMoved);
    connect(m_sync,   &QToolButton::toggled,   this, &FxPanel::onSyncToggled);
    connect(m_reset,  &QToolButton::clicked,   this, [this]{ resetAll(); emit resetClicked(); });
}

int  FxPanel::pitch()  const { return m_pitch->value(); }
int  FxPanel::speed()  const { return m_speed->value(); }
int  FxPanel::reverb() const { return m_reverb->value(); }
bool FxPanel::sync()   const { return m_sync->isChecked(); }

void FxPanel::setPitch(int v)  { QSignalBlocker b(m_pitch);  m_pitch->setValue(v);  m_pitchLabel->setText(fmtFactor(v)); }
void FxPanel::setSpeed(int v)  { QSignalBlocker b(m_speed);  m_speed->setValue(v);  m_speedLabel->setText(fmtFactor(v)); }
void FxPanel::setReverb(int v) { QSignalBlocker b(m_reverb); m_reverb->setValue(v); m_reverbLabel->setText(fmtPlain(v));  }

void FxPanel::setSync(bool on) {
    bool wasOn = m_sync->isChecked();
    {
        QSignalBlocker b(m_sync);
        m_sync->setChecked(on);
    }
    // Always refresh - same Reset Channels race as VolumeControl: the
    // canonical state may already match the button, but we must still
    // run the theme path so the green-checked stylesheet drops back to
    // grey-unchecked when the channel is reset programmatically.
    refreshTheme();
    if (wasOn != on) emit syncChanged(on);
}

void FxPanel::resetAll() {
    setPitch(0); setSpeed(0); setReverb(0);
    emit pitchChanged(0); emit speedChanged(0); emit reverbChanged(0);
}

void FxPanel::onPitchMoved(int v) {
    m_pitchLabel->setText(fmtFactor(v));
    if (m_sync->isChecked() && !m_internalSync) {
        m_internalSync = true;
        QSignalBlocker b(m_speed);
        m_speed->setValue(v);
        m_speedLabel->setText(fmtFactor(v));
        m_internalSync = false;
        emit speedChanged(v);
    }
    emit pitchChanged(v);
}

void FxPanel::onSpeedMoved(int v) {
    m_speedLabel->setText(fmtFactor(v));
    if (m_sync->isChecked() && !m_internalSync) {
        m_internalSync = true;
        QSignalBlocker b(m_pitch);
        m_pitch->setValue(v);
        m_pitchLabel->setText(fmtFactor(v));
        m_internalSync = false;
        emit pitchChanged(v);
    }
    emit speedChanged(v);
}

void FxPanel::onReverbMoved(int v) {
    m_reverbLabel->setText(fmtPlain(v));
    emit reverbChanged(v);
}

void FxPanel::refreshTheme() {
    Theme::Derived d = Theme::derive(Theme::colors());
    bool linkOn = m_sync->isChecked();
    // Per-widget slider QSS so all three sliders match VolumeControl
    // even when Qt's cascade misses this deep in the tree.
    // Custom theme uses d.button so the track stands out from surface;
    // default palette uses #1e1e1e (darker than #3a3a3a channel frame).
    const QString grooveBg = Theme::colors().enabled
        ? d.button.name() : QStringLiteral("#1e1e1e");
    QString sliderCss = QStringLiteral(
        "QSlider { background: transparent; }"
        "QSlider::groove:horizontal { background: ") + grooveBg +
        QStringLiteral("; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: ") + d.slider.name() +
        QStringLiteral("; width: 14px; margin: -5px 0; border-radius: 7px;"
        " border: 1px solid ") + d.border.name() + QStringLiteral("; }"
        "QSlider::handle:horizontal:hover { background: ") + d.accent.name() +
        QStringLiteral("; }"
        "QSlider::sub-page:horizontal { background: ") + d.accent.name() +
        QStringLiteral("; border-radius: 3px; }");
    m_pitch ->setStyleSheet(sliderCss);
    m_speed ->setStyleSheet(sliderCss);
    m_reverb->setStyleSheet(sliderCss);
    // Sync stays green when checked (semantic), themed when off.
    QString syncBase = linkOn
        ? QString("QToolButton { background-color: #2e7d32; border: 1px solid #66bb6a;"
                  " border-radius: 5px; color: white; padding: 2px 4px; }")
        : QString("QToolButton { background-color: %1; border: 1px solid %2;"
                  " border-radius: 5px; color: %3; padding: 2px 4px; }")
              .arg(d.surface.name(), d.border.name(), d.textMuted.name());
    QString syncDisabled = QString(
        "QToolButton:disabled { background-color: %1; border: 1px solid %2;"
        " color: %3; }").arg(d.disabledSurface.name(), d.disabledBorder.name(),
                             d.textMuted.name());
    m_sync->setStyleSheet(syncBase + syncDisabled);
    // Reset stays red (semantic); disabled state pulls from theme.
    m_reset->setStyleSheet(QString(
        "QToolButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 5px; padding: 2px 8px; }"
        "QToolButton:hover { background-color: #e04141; }"
        "QToolButton:disabled { background-color: %1; color: %2;"
        " border: 1px solid %3; }")
        .arg(d.disabledSurface.name(), d.textMuted.name(), d.disabledBorder.name()));
}

void FxPanel::onSyncToggled(bool on) {
    if (on) {
        m_internalSync = true;
        QSignalBlocker b(m_speed);
        m_speed->setValue(m_pitch->value());
        m_speedLabel->setText(fmtFactor(m_pitch->value()));
        m_internalSync = false;
        emit speedChanged(m_pitch->value());
    }
    emit syncChanged(on);
}
