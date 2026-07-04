#include "mic_channel.h"
#include "channel_meter.h"
#include "channel_sandbox_dialog.h"
#include "help_bubble.h"
#include "../MicFx.h"

#include <cmath>

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QFrame>

MicChannel::MicChannel(QWidget *parent)
    : QWidget(parent)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("micChannelFrame"));
    frame->setFrameShape(QFrame::StyledPanel);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);

    auto *row = new QHBoxLayout(frame);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(8);

    auto *micIcon = new QLabel(QString::fromUtf8("\xF0\x9F\x8E\xA4"), frame);
    row->addWidget(micIcon);

    m_enable = new QCheckBox(tr("Mic FX"), frame);
    m_enable->setToolTip(tr(
        "Master switch: process YOUR microphone through the effect\n"
        "chain in real time. Everyone on the server hears the result;\n"
        "the soundboard audio stays clean on top. Also bindable to a\n"
        "TS3 hotkey ('Toggle Mic FX')."));
    row->addWidget(m_enable);

    m_liveBadge = new QLabel(tr("LIVE"), frame);
    m_liveBadge->setStyleSheet(
        "color: white; background-color: #c0392b; font-weight: bold;"
        "border-radius: 3px; padding: 1px 6px; font-size: 10px;");
    m_liveBadge->hide();
    row->addWidget(m_liveBadge);

    m_meter = new ChannelMeter(frame);
    m_meter->setMinimumWidth(90);
    m_meter->setMaximumWidth(140);
    row->addWidget(m_meter, 1);

    row->addWidget(new QLabel(tr("Pitch"), frame));
    m_pitch = new QSlider(Qt::Horizontal, frame);
    // Tenths of a semitone: fine vocal tuning needs sub-semitone
    // resolution (slider value = semitones * 10).
    m_pitch->setRange(-120, 120);
    m_pitch->setValue(0);
    m_pitch->setSingleStep(1);    // 0.1 st per arrow key / wheel notch
    m_pitch->setPageStep(10);     // 1 st per page
    m_pitch->setMinimumWidth(90);
    m_pitch->setMaximumWidth(160);
    m_pitch->setToolTip(tr(
        "Live pitch shift in semitones (0.1 st steps). 1:1 - the voice\n"
        "changes pitch but stays perfectly in sync (no delay build-up)."));
    row->addWidget(m_pitch, 1);
    m_pitchLabel = new QLabel(QStringLiteral("0.0 st"), frame);
    m_pitchLabel->setMinimumWidth(46);
    row->addWidget(m_pitchLabel);

    // Reverb wet: drives the sandbox ambience reverb (SandboxState::
    // reverbWet) - the mic has no decoder, so the channels' FFmpeg
    // reverb path does not exist here; the DSP-chain reverb is the
    // mic's reverb. Same 0..100 feel as the channel FxPanel slider.
    row->addWidget(new QLabel(tr("Reverb"), frame));
    m_reverb = new QSlider(Qt::Horizontal, frame);
    m_reverb->setRange(0, 100);
    m_reverb->setValue(0);
    m_reverb->setMinimumWidth(70);
    m_reverb->setMaximumWidth(140);
    m_reverb->setToolTip(tr(
        "Live reverb on your voice (the sandbox ambience reverb).\n"
        "0 = dry. The reverb engine (algorithmic / convolution) is\n"
        "picked inside Effects..."));
    row->addWidget(m_reverb, 1);
    m_reverbLabel = new QLabel(QStringLiteral("0"), frame);
    m_reverbLabel->setMinimumWidth(24);
    row->addWidget(m_reverbLabel);

    m_presetBox = new QComboBox(frame);
    m_presetBox->addItem(tr("(voice preset)"));
    for (const auto &p : MicFx::builtinPresets())
        m_presetBox->addItem(p.name);
    m_presetBox->setToolTip(tr(
        "One-click voice presets. Picking one REPLACES the current mic\n"
        "chain settings + pitch. Open Effects... to fine-tune after."));
    row->addWidget(m_presetBox, 1);

    m_monitor = new QCheckBox(tr("Monitor"), frame);
    m_monitor->setToolTip(tr(
        "Hear your own processed voice in your headphones while you\n"
        "speak. Useful to calibrate presets; off by default because\n"
        "hearing yourself while talking is distracting."));
    row->addWidget(m_monitor);

    m_fxBtn = new QPushButton(tr("Effects..."), frame);
    m_fxBtn->setToolTip(tr("Open the full effect chain editor for the microphone"));
    row->addWidget(m_fxBtn);

    row->addWidget(new HelpBubble(tr(
        "Real-time voice changer on your OWN microphone.\n"
        "- The toggle (or its TS3 hotkey) turns processing on/off.\n"
        "- LIVE lights up while you are transmitting processed voice.\n"
        "- Pitch is latency-free (1:1); presets configure the whole\n"
        "  chain in one click; Effects... opens the full editor.\n"
        "- A safety limiter always guards the output level."), frame));

    connect(m_enable, &QCheckBox::toggled, this, &MicChannel::onEnableToggled);
    connect(m_pitch, &QSlider::valueChanged, this, [this](int v){
        m_pitchLabel->setText(QString::number(v / 10.0, 'f', 1) + " st");
        if (m_loading) return;
        MicFx::instance().setPitchSemitones(v / 10.0f);
    });
    connect(m_reverb, &QSlider::valueChanged, this, [this](int v){
        m_reverbLabel->setText(QString::number(v));
        if (m_loading) return;
        SandboxState s = MicFx::instance().sandboxState();
        s.reverbWet = v / 100.0f;
        MicFx::instance().setSandboxState(s);
        // Mirror into the open editor so its ambience knob follows.
        if (m_dialog && m_dialog->isVisible())
            m_dialog->setState(MicFx::instance().sandboxState());
    });
    connect(m_presetBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &MicChannel::onPresetPicked);
    connect(m_monitor, &QCheckBox::toggled, this, [this](bool on){
        if (m_loading) return;
        MicFx::instance().setMonitor(on);
    });
    connect(m_fxBtn, &QPushButton::clicked, this, &MicChannel::onOpenEffects);

    // Hotkey / external toggles (incl. Mic FX macros) reflect back into
    // the row: full re-pull so pitch + monitor mirror the applied
    // package, not just the master checkbox.
    connect(&MicFx::instance(), &MicFx::enabledChanged, this, [this](bool){
        pullFromMicFx();
        if (m_dialog && m_dialog->isVisible())
            m_dialog->setState(MicFx::instance().sandboxState());
    });

    m_meterTimer = new QTimer(this);
    m_meterTimer->setInterval(40);   // 25 Hz
    connect(m_meterTimer, &QTimer::timeout, this, &MicChannel::onMeterTick);
    m_meterTimer->start();

    pullFromMicFx();
}

void MicChannel::refreshTheme()
{
    // Frame follows the app palette automatically; nothing custom yet.
}

void MicChannel::pullFromMicFx()
{
    m_loading = true;
    MicFx &fx = MicFx::instance();
    m_enable->setChecked(fx.enabled());
    int pv = static_cast<int>(std::lround(fx.pitchSemitones() * 10.0f));
    m_pitch->setValue(qBound(-120, pv, 120));
    m_pitchLabel->setText(
        QString::number(m_pitch->value() / 10.0, 'f', 1) + " st");
    int rv = static_cast<int>(std::lround(fx.sandboxState().reverbWet * 100.0f));
    m_reverb->setValue(qBound(0, rv, 100));
    m_reverbLabel->setText(QString::number(m_reverb->value()));
    m_monitor->setChecked(fx.monitor());
    m_loading = false;
    updateLiveBadge();
}

void MicChannel::updateLiveBadge()
{
    m_liveBadge->setVisible(MicFx::instance().enabled());
}

void MicChannel::onEnableToggled(bool on)
{
    if (m_loading) return;
    MicFx::instance().setEnabled(on);
    updateLiveBadge();
}

void MicChannel::onPresetPicked(int index)
{
    if (m_loading) return;
    if (index <= 0) return;   // placeholder row
    MicFx::instance().applyPreset(index - 1);
    // Preset changed pitch + reverb too - re-sync the sliders.
    m_loading = true;
    int pv = static_cast<int>(std::lround(
        MicFx::instance().pitchSemitones() * 10.0f));
    m_pitch->setValue(qBound(-120, pv, 120));
    m_pitchLabel->setText(
        QString::number(m_pitch->value() / 10.0, 'f', 1) + " st");
    int rv = static_cast<int>(std::lround(
        MicFx::instance().sandboxState().reverbWet * 100.0f));
    m_reverb->setValue(qBound(0, rv, 100));
    m_reverbLabel->setText(QString::number(m_reverb->value()));
    m_loading = false;
    // Refresh the dialog if it is open so knobs mirror the preset.
    if (m_dialog && m_dialog->isVisible())
        m_dialog->setState(MicFx::instance().sandboxState());
}

void MicChannel::onOpenEffects()
{
    if (!m_dialog) {
        m_dialog = new ChannelSandboxDialog(0, this, /*micMode=*/true);
        m_dialog->setState(MicFx::instance().sandboxState());
        connect(m_dialog, &ChannelSandboxDialog::stateChanged, this,
                [this](const SandboxState &s){
            MicFx::instance().setSandboxState(s);
            // The editor's ambience knob edits reverbWet too - keep
            // the row slider mirrored without re-entering the setter.
            m_loading = true;
            int rv = static_cast<int>(std::lround(s.reverbWet * 100.0f));
            m_reverb->setValue(qBound(0, rv, 100));
            m_reverbLabel->setText(QString::number(m_reverb->value()));
            m_loading = false;
        });
        connect(m_dialog, &ChannelSandboxDialog::resetRequested, this,
                [this](int){
            SandboxState fresh;
            fresh.enabled = true;
            MicFx::instance().setSandboxState(fresh);
            m_dialog->setState(MicFx::instance().sandboxState());
        });
    } else {
        m_dialog->setState(MicFx::instance().sandboxState());
    }
    m_dialog->show();
    m_dialog->raise();
    m_dialog->activateWindow();
}

void MicChannel::onMeterTick()
{
    if (!isVisible()) return;
    MicFx &fx = MicFx::instance();
    if (fx.enabled())
        m_meter->setPeak(fx.levelOut(), fx.levelOut());
    else
        m_meter->setPeak(0.0f, 0.0f);
}
