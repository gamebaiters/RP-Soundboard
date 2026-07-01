#include "channel.h"
#include "../SoundButton.h"
#include "theme.h"
#include "channel_meter.h"
#include "channel_sandbox_dialog.h"   // SandboxEnginePref
#include "icon_factory.h"

// Defined in SoundButton.cpp (not exposed via header).
extern const QString &getButtonMime();

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QFrame>
#include <QLabel>
#include <QResizeEvent>
#include <QLineEdit>
#include <QStyle>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCheckBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>

QByteArray ChannelState::toJson() const {
    QJsonObject o;
    o["volumeLocal"]   = volumeLocal;
    o["volumeRemote"]  = volumeRemote;
    o["volumesLinked"] = volumesLinked;
    o["pitch"]         = pitch;
    o["speed"]         = speed;
    o["reverb"]        = reverb;
    o["fxSync"]        = fxSync;
    o["filename"]      = filename;
    o["playbackPos"]   = playbackPos;
    o["sandbox"]       = sandbox.toJson();
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

bool ChannelState::fromJson(const QByteArray &data, ChannelState &out) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    auto o = doc.object();
    out.volumeLocal   = o.value("volumeLocal").toInt(100);
    out.volumeRemote  = o.value("volumeRemote").toInt(100);
    out.volumesLinked = o.value("volumesLinked").toBool(false);
    out.pitch         = o.value("pitch").toInt(0);
    out.speed         = o.value("speed").toInt(0);
    out.reverb        = o.value("reverb").toInt(0);
    out.fxSync        = o.value("fxSync").toBool(false);
    out.filename      = o.value("filename").toString();
    out.playbackPos   = o.value("playbackPos").toDouble(0.0);
    out.sandbox       = SandboxState::fromJson(o.value("sandbox").toObject());
    return true;
}

Channel::Channel(int channelId, QWidget *parent)
    : QWidget(parent)
    , m_id(channelId)
    , m_volume(new VolumeControl(this))
    , m_fx(new FxPanel(this))
    , m_wave(new WaveformPlayer(this))
    , m_addBtn(new QPushButton(tr("+ Add channel"), this))
    , m_removeBtn(new QPushButton(this))
    , m_titleEdit(new QLineEdit(this))
{
    // Seed the per-channel sandbox state with the user's most recently
    // chosen engine (Classic / Leia). Per-channel persistence, when the
    // user has it enabled, overrides this on restore via applyState();
    // without persistence this is what survives sessions.
    m_sandbox.spatialEngine = SandboxEnginePref::load();

    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    setAcceptDrops(true);

    m_removeBtn->setText(QString::fromUtf8("\xE2\x9C\x95")); // ✕
    m_removeBtn->setVisible(false);
    m_removeBtn->setToolTip(tr("Remove this channel (stops its playback)"));
    m_removeBtn->setFixedSize(22, 22);
    m_removeBtn->setCursor(Qt::PointingHandCursor);
    m_removeBtn->setFlat(true);

    m_titleEdit->setText(tr("Channel %1").arg(channelId + 1));
    m_titleEdit->setFrame(false);
    m_titleEdit->setMinimumWidth(120);

    m_frame = new QFrame(this);
    m_frame->setObjectName("channelFrame");
    m_frame->setFrameShape(QFrame::StyledPanel);
    m_frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    refreshTheme();

    // Per-channel meter sits VERTICALLY CENTERED between the volume
    // controls and the FX panel - that's where the audio "leaves" the
    // channel so it's the natural visual home. When the FX panel is
    // hidden it slides next to the volume controls instead.
    m_meter = new ChannelMeter(this);
    m_meter->setMinimumWidth(180);
    m_meter->setMaximumWidth(260);
    m_meter->setFixedHeight(36);
    m_meter->setToolTip(tr(
        "Per-channel L/R peak meter (post-DSP, post-Remote-volume).\n"
        "Always reflects what the SERVER hears, regardless of how the\n"
        "Local volume slider is set. Cyan = headroom, amber = warning,\n"
        "red = clipping. Hide via Settings > Audio sandbox."));

    auto *controls = new QHBoxLayout;
    controls->setContentsMargins(0,0,0,0);
    controls->setSpacing(8);
    controls->addWidget(m_volume, 1);
    controls->addWidget(m_meter, 0, Qt::AlignVCenter);
    m_fxSeparator = new QFrame(this);
    m_fxSeparator->setFrameShape(QFrame::VLine);
    controls->addWidget(m_fxSeparator);
    controls->addWidget(m_fx, 1);

    // Per-channel DSP entry point. Compact icon button (mixer-faders
    // glyph) - the "Audio Sandbox" name is carried by the enable
    // checkbox right next to it, so the row stays short.
    m_sandboxBtn = new QPushButton(this);
    m_sandboxBtn->setIcon(IconFactory::sandbox());
    m_sandboxBtn->setIconSize(QSize(18, 18));
    m_sandboxBtn->setFixedSize(30, 22);
    m_sandboxBtn->setStyleSheet(
        "QPushButton { padding: 2px; }");
    m_sandboxBtn->setToolTip(tr(
        "Open the Audio Sandbox editor for this channel:\n"
        "  - 16-band ISO graphic EQ\n"
        "  - 3D HRTF spatial audio (manual / orbit / 8D preset)\n"
        "  - Paulstretch and 11 more DSP effects with a\n"
        "    drag-to-reorder pipeline\n"
        "Settings persist per channel and are bundled into macros."));

    // Text-only button (the previous stoparrow icon was the wrong art and
    // forced the channel row taller). Same fixed height as the sandbox
    // button so the title row stays compact.
    m_exportBtn = new QPushButton(tr("Export audio"), this);
    m_exportBtn->setFixedHeight(22);
    m_exportBtn->setMinimumWidth(96);
    m_exportBtn->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    m_exportBtn->setStyleSheet("QPushButton { padding: 2px 10px; }");
    m_exportBtn->setToolTip(tr("Export this channel's audio with all DSP effects applied to a WAV file"));
    m_exportBtn->setVisible(false);
    connect(m_exportBtn, &QPushButton::clicked, this, [this]{ emit exportRequested(m_id); });

    m_sandboxEnableCheck = new QCheckBox(tr("Audio Sandbox"), this);
    m_sandboxEnableCheck->setToolTip(tr(
        "Enable the Audio Sandbox on this channel (EQ, spatial audio, "
        "and the DSP effect chain). Open the editor with the button next "
        "to this checkbox."));
    m_sandboxEnableCheck->setChecked(m_sandbox.enabled);
    connect(m_sandboxEnableCheck, &QCheckBox::toggled, this, [this](bool on){
        m_sandbox.enabled = on;
        m_sandboxBtn->setEnabled(on);
        if (m_sandboxDialog) {
            m_sandboxDialog->setState(m_sandbox);
        }
        emit sandboxStateChanged(m_id, m_sandbox);
        onAnyChange();
    });

    // Layout: [X][title.....][FX check][sandboxBtn]
    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0,0,0,0);
    titleRow->setSpacing(4);
    titleRow->addWidget(m_removeBtn, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_titleEdit, 1);
    titleRow->addWidget(m_exportBtn, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_sandboxEnableCheck, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_sandboxBtn, 0, Qt::AlignVCenter);

    auto *frameLayout = new QVBoxLayout(m_frame);
    frameLayout->setContentsMargins(8,4,8,8);
    frameLayout->setSpacing(4);
    frameLayout->addLayout(titleRow);
    frameLayout->addWidget(m_wave);
    frameLayout->addLayout(controls);

    m_addBtn->setVisible(false);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0,0,0,0);
    root->setSpacing(0);
    root->addWidget(m_frame);

    auto onChange = [this]{ onAnyChange(); };
    connect(m_volume, &VolumeControl::localChanged,   this, onChange);
    connect(m_volume, &VolumeControl::remoteChanged,  this, onChange);
    connect(m_volume, &VolumeControl::linkedChanged,  this, onChange);
    connect(m_fx,     &FxPanel::pitchChanged,         this, onChange);
    connect(m_fx,     &FxPanel::speedChanged,         this, onChange);
    connect(m_fx,     &FxPanel::reverbChanged,        this, onChange);
    connect(m_fx,     &FxPanel::syncChanged,          this, onChange);

    connect(m_addBtn,    &QPushButton::clicked, this, [this]{ emit addChannelRequested(m_id); });
    connect(m_removeBtn, &QPushButton::clicked, this, [this]{ emit removeChannelRequested(m_id); });
    connect(m_titleEdit, &QLineEdit::editingFinished, this, &Channel::onTitleEditFinished);
    connect(m_sandboxBtn,&QPushButton::clicked, this, [this]{ openSandboxDialog(); });
}

void Channel::setChannelId(int newId)
{
    if (m_id == newId) return;
    m_id = newId;
    if (m_sandboxDialog) {
        // The dialog's title shows "Channel N - Audio Sandbox" derived
        // from its own m_channelId. Push the new id + title so a moved
        // channel does not lie about which row it is.
        m_sandboxDialog->setChannelTitle(title());
    }
}

void Channel::setSandboxState(const SandboxState &s) {
    m_sandbox = s;
    if (m_sandboxEnableCheck) {
        QSignalBlocker b(m_sandboxEnableCheck);
        m_sandboxEnableCheck->setChecked(s.enabled);
    }
    if (m_sandboxBtn) m_sandboxBtn->setEnabled(s.enabled);
    if (m_sandboxDialog) m_sandboxDialog->setState(m_sandbox);
}

void Channel::openSandboxDialog() {
    if (!m_sandboxFeatureEnabled) return;
    if (!m_sandboxDialog) {
        m_sandboxDialog = new ChannelSandboxDialog(m_id, this);
        m_sandboxDialog->setProperty("isGBSoundboard", true);
        connect(m_sandboxDialog, &ChannelSandboxDialog::stateChanged,
                this, [this](const SandboxState &s){
            m_sandbox = s;
            if (m_sandboxEnableCheck && m_sandboxEnableCheck->isChecked() != s.enabled) {
                QSignalBlocker b(m_sandboxEnableCheck);
                m_sandboxEnableCheck->setChecked(s.enabled);
                m_sandboxBtn->setEnabled(s.enabled);
            }
            emit sandboxStateChanged(m_id, s);
            onAnyChange();
        });
        connect(m_sandboxDialog, &ChannelSandboxDialog::resetRequested,
                this, [this](int id){ emit sandboxResetRequested(id); });
    }
    m_sandboxDialog->setChannelTitle(title());
    m_sandboxDialog->setState(m_sandbox);
    m_sandboxDialog->show();
    m_sandboxDialog->raise();
    m_sandboxDialog->activateWindow();
}

void Channel::setMeterPeak(float l, float r) {
    if (m_meter) m_meter->setPeak(l, r);
}

void Channel::setMeterVisible(bool on) {
    if (m_meter) m_meter->setVisible(on);
}

void Channel::setMeterVertical(bool on) {
    if (!m_meter) return;
    if (on) {
        // Vertical: narrow strip (~40 px) that stretches to the
        // channel row height. Remove the fixed height so it can grow
        // to fill the controls row; parent layout re-flows.
        m_meter->setOrientation(ChannelMeter::Vertical);
        m_meter->setMinimumWidth(40);
        m_meter->setMaximumWidth(48);
        m_meter->setMinimumHeight(48);
        m_meter->setMaximumHeight(QWIDGETSIZE_MAX);
    } else {
        // Horizontal (default): wide low-height meter matching original layout.
        m_meter->setOrientation(ChannelMeter::Horizontal);
        m_meter->setMinimumWidth(120);
        m_meter->setMaximumWidth(260);
        m_meter->setFixedHeight(36);
    }
    updateMeterWidth();
    if (auto *p = parentWidget()) p->updateGeometry();
    updateGeometry();
}

void Channel::setSkipButtonsVisible(bool on) {
    if (m_wave) m_wave->setSkipButtonsVisible(on);
}

void Channel::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    updateMeterWidth();
}

void Channel::updateMeterWidth()
{
    if (!m_meter || !m_volume || !m_fx) return;
    // Vertical meter has a fixed narrow footprint so setMeterVertical
    // handles its sizing directly. Only the horizontal path needs the
    // "share row width with the surrounding controls" logic below.
    if (m_meter->orientation() == ChannelMeter::Vertical) return;
    // Width the controls row needs for EVERYTHING except the meter.
    int reserved = m_volume->sizeHint().width()
                 + m_fx->sizeHint().width()
                 + (m_fxSeparator ? m_fxSeparator->sizeHint().width() : 2)
                 + 8 * 3;                 // controls layout spacing, 3 gaps
    int row = width() - 16;               // frame left + right margins
    int w = row - reserved;               // leftover space -> the meter
    // The meter yields space FIRST and EAGERLY: a low ceiling keeps it
    // compact even on a wide window (so the sliders already have room),
    // and a low floor lets it keep shrinking. Only once it bottoms out
    // do volume / FX start to shrink.
    if (w < 56)  w = 56;
    if (w > 150) w = 150;
    if (m_meter->maximumWidth() != w)     // skip redundant relayouts
        m_meter->setFixedWidth(w);
}

void Channel::setSandboxFeatureEnabled(bool on) {
    m_sandboxFeatureEnabled = on;
    // Master switch hides BOTH the per-channel FX checkbox and the
    // sandbox button so the entire sandbox UI disappears from the row.
    // The persisted m_sandbox value is left untouched so a later
    // re-enable restores whatever each channel had before.
    if (m_sandboxBtn) m_sandboxBtn->setVisible(on);
    if (m_sandboxEnableCheck) m_sandboxEnableCheck->setVisible(on);
    if (!on && m_sandboxDialog && m_sandboxDialog->isVisible())
        m_sandboxDialog->close();
}

void Channel::setExportVisible(bool on) {
    if (m_exportBtn) m_exportBtn->setVisible(on);
}

void Channel::pushTitleToSandboxDialog() {
    if (m_sandboxDialog) m_sandboxDialog->setChannelTitle(title());
}

void Channel::pushSandboxCpu(double pct) {
    if (m_sandboxDialog && m_sandboxDialog->isVisible())
        m_sandboxDialog->setCpuPercent(pct);
}

void Channel::pushSandboxLevel(float l, float r) {
    if (m_sandboxDialog && m_sandboxDialog->isVisible())
        m_sandboxDialog->pushAudioLevel(l, r);
}

void Channel::pushSandboxEqLevels(const float bands[16]) {
    if (m_sandboxDialog && m_sandboxDialog->isVisible())
        m_sandboxDialog->pushEqBandLevels(bands);
}

void Channel::setRemovable(bool on) {
    m_removeBtn->setVisible(on);
}

void Channel::refreshTheme() {
    Theme::Derived d = Theme::derive(Theme::colors());
    if (m_frame) {
        m_frame->setStyleSheet(QString(
            "#channelFrame { border: 1px solid %1; border-radius: 6px;"
            " background-color: %2; }").arg(d.border.name(), d.surface.name()));
    }
    if (m_removeBtn) {
        m_removeBtn->setStyleSheet(QString(
            "QPushButton { background: transparent; color: #c64545;"
            " border: none; border-radius: 4px; font-weight: bold;"
            " font-size: 13px; padding: 0; }"
            "QPushButton:hover { background-color: #c63131; color: white; }"
            "QPushButton:pressed { background-color: #a32626; color: white; }"));
    }
    if (m_titleEdit) {
        m_titleEdit->setStyleSheet(QString(
            "QLineEdit { background: transparent; color: %1; border: none;"
            " font-weight: bold; padding: 2px 4px; }"
            "QLineEdit:focus { background: %2; border: 1px solid %3;"
            " border-radius: 3px; }")
            .arg(d.text.name(), d.surfaceAlt.name(), d.borderStrong.name()));
    }
    if (m_fx)     m_fx->refreshTheme();
    if (m_volume) m_volume->refreshTheme();
}

void Channel::setFxVisible(bool on) {
    m_fx->setVisible(on);
    if (m_fxSeparator) m_fxSeparator->setVisible(on);
    if (auto *p = parentWidget()) p->updateGeometry();
    updateGeometry();
}

void Channel::setWaveformVisible(bool on) {
    // Hides waveform paint only; transport + filename + time stay visible.
    m_wave->setWavePaintVisible(on);
    if (auto *p = parentWidget()) p->updateGeometry();
    updateGeometry();
}

void Channel::setTitle(const QString &t) {
    if (m_titleEdit->text() == t) return;
    QSignalBlocker b(m_titleEdit);
    m_titleEdit->setText(t);
}

QString Channel::title() const {
    return m_titleEdit->text();
}

void Channel::onTitleEditFinished() {
    emit titleChanged(m_id, m_titleEdit->text());
}

ChannelState Channel::state() const {
    ChannelState s;
    s.volumeLocal   = m_volume->local();
    s.volumeRemote  = m_volume->remote();
    s.volumesLinked = m_volume->linked();
    s.pitch         = m_fx->pitch();
    s.speed         = m_fx->speed();
    s.reverb        = m_fx->reverb();
    s.fxSync        = m_fx->sync();
    s.filename      = m_wave->filename();
    s.playbackPos   = 0.0;
    s.sandbox       = m_sandbox;
    return s;
}

void Channel::applyState(const ChannelState &s) {
    m_volume->setLocal(s.volumeLocal);
    m_volume->setRemote(s.volumeRemote);
    m_volume->setLinked(s.volumesLinked);
    m_fx->setPitch(s.pitch);
    m_fx->setSpeed(s.speed);
    m_fx->setReverb(s.reverb);
    m_fx->setSync(s.fxSync);
    m_wave->setFilename(s.filename);
    setSandboxState(s.sandbox);
    // Macro restore + session restore both go through applyState. Both
    // need the wiring layer to push the sandbox state back to the
    // sampler's slot DSP, otherwise the saved spatial / EQ / stretch
    // settings load into the dialog but never reach the audio engine.
    emit sandboxStateChanged(m_id, m_sandbox);
}

void Channel::onAnyChange() {
    emit stateChanged(m_id);
}

void Channel::dragEnterEvent(QDragEnterEvent *e) {
    if (e->mimeData() && e->mimeData()->hasFormat(getButtonMime()))
        e->acceptProposedAction();
}

void Channel::dragMoveEvent(QDragMoveEvent *e) {
    if (e->mimeData() && e->mimeData()->hasFormat(getButtonMime()))
        e->acceptProposedAction();
}

void Channel::dropEvent(QDropEvent *e) {
    if (!e->mimeData() || !e->mimeData()->hasFormat(getButtonMime())) return;
    QObject *src = e->mimeData()->property("sourceButton").value<QObject *>();
    if (!src) return;
    int idx = src->property("buttonIndex").toInt();
    emit soundDroppedFromButton(m_id, idx);
    e->acceptProposedAction();
}
