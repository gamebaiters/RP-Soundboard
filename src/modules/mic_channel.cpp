#include "mic_channel.h"
#include "channel_meter.h"
#include "channel_sandbox_dialog.h"
#include "help_bubble.h"
#include "preset_manager.h"
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
#include <QInputDialog>
#include <QMessageBox>
#include <QApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QIcon>

// Painted preset-action glyphs (floppy = save, trash = delete, share nodes,
// clipboard = paste). Painted rather than Unicode/text so they render
// identically on every host font (a glyph char showed as tofu on the TS3
// client font) and never hit the MSVC narrow-literal mojibake trap.
namespace {
QPixmap micIconCanvas(QPen &pen)
{
    QPixmap pm(18, 18);
    pm.fill(Qt::transparent);
    pen = QPen(QColor(0xdc, 0xdc, 0xdc));
    pen.setWidthF(1.5);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    return pm;
}

QIcon makeSaveIcon()   // floppy disk
{
    QPen pen; QPixmap pm = micIconCanvas(pen);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // Body with a clipped top-right corner (the classic floppy shape).
    QPainterPath body;
    body.moveTo(3, 3);
    body.lineTo(12, 3);
    body.lineTo(15, 6);
    body.lineTo(15, 15);
    body.lineTo(3, 15);
    body.closeSubpath();
    p.drawPath(body);
    // Shutter (top) + label (bottom).
    p.drawRect(QRectF(6, 3, 5, 3));
    p.drawRect(QRectF(5.5, 9.5, 7, 5));
    return QIcon(pm);
}

QIcon makeTrashIcon()   // delete
{
    QPen pen; QPixmap pm = micIconCanvas(pen);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    // Lid + handle.
    p.drawLine(QLineF(3.5, 5, 14.5, 5));
    p.drawLine(QLineF(7, 5, 7.5, 3.3));
    p.drawLine(QLineF(11, 5, 10.5, 3.3));
    p.drawLine(QLineF(7.5, 3.3, 10.5, 3.3));
    // Can body (slight taper).
    p.drawLine(QLineF(5, 6, 6, 15));
    p.drawLine(QLineF(13, 6, 12, 15));
    p.drawLine(QLineF(6, 15, 12, 15));
    // Ribs.
    p.drawLine(QLineF(7.5, 7.5, 7.7, 13.5));
    p.drawLine(QLineF(9, 7.5, 9, 13.5));
    p.drawLine(QLineF(10.5, 7.5, 10.3, 13.5));
    return QIcon(pm);
}

QIcon makeShareIcon()   // three connected nodes
{
    QPen pen; QPixmap pm = micIconCanvas(pen);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    const QPointF a(13, 4), b(5, 9), c(13, 14);   // right-top, left-mid, right-bottom
    p.drawLine(QLineF(b.x() + 1, b.y() - 0.5, a.x() - 1, a.y() + 0.7));
    p.drawLine(QLineF(b.x() + 1, b.y() + 0.5, c.x() - 1, c.y() - 0.7));
    p.setBrush(QColor(0xdc, 0xdc, 0xdc));
    p.drawEllipse(a, 2.0, 2.0);
    p.drawEllipse(b, 2.0, 2.0);
    p.drawEllipse(c, 2.0, 2.0);
    return QIcon(pm);
}

QIcon makePasteIcon()   // clipboard
{
    QPen pen; QPixmap pm = micIconCanvas(pen);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // Board + clip.
    QPainterPath board;
    board.addRoundedRect(QRectF(4, 4, 10, 11), 1.5, 1.5);
    p.drawPath(board);
    p.setBrush(QColor(0xdc, 0xdc, 0xdc));
    p.drawRoundedRect(QRectF(7, 2.3, 4, 3.2), 1.0, 1.0);   // the clip
    p.setBrush(Qt::NoBrush);
    // A couple of "lines of text" on the board.
    p.drawLine(QLineF(6, 9, 12, 9));
    p.drawLine(QLineF(6, 11.5, 11, 11.5));
    return QIcon(pm);
}
} // namespace

MicChannel::MicChannel(QWidget *parent)
    : QWidget(parent)
{
    auto *frame = new QFrame(this);
    frame->setObjectName(QStringLiteral("micChannelFrame"));
    frame->setFrameShape(QFrame::StyledPanel);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(frame);

    // Two clear rows: a header that says WHAT this is + the master toggle +
    // level meter, then a controls row grouped by caption. The old single
    // cramped row read like just another playback channel.
    auto *col = new QVBoxLayout(frame);
    col->setContentsMargins(8, 4, 8, 6);
    col->setSpacing(4);

    // ---- header row ----
    auto *head = new QHBoxLayout;
    head->setSpacing(8);

    auto *micIcon = new QLabel(QString::fromUtf8("\xF0\x9F\x8E\xA4"), frame);
    head->addWidget(micIcon);

    auto *title = new QLabel(tr("<b>Microphone</b> — live voice changer"), frame);
    title->setTextFormat(Qt::RichText);
    head->addWidget(title);

    m_enable = new QCheckBox(tr("Enable"), frame);
    m_enable->setToolTip(tr(
        "Master switch: process YOUR microphone through the effect\n"
        "chain in real time. Everyone on the server hears the result;\n"
        "the soundboard audio stays clean on top. Also bindable to a\n"
        "TS3 hotkey ('Toggle Mic FX')."));
    head->addWidget(m_enable);

    m_liveBadge = new QLabel(tr("LIVE"), frame);
    m_liveBadge->setStyleSheet(
        "color: white; background-color: #c0392b; font-weight: bold;"
        "border-radius: 3px; padding: 1px 6px; font-size: 10px;");
    m_liveBadge->hide();
    head->addWidget(m_liveBadge);

    head->addStretch(1);

    head->addWidget(new QLabel(tr("Level"), frame));
    m_meter = new ChannelMeter(frame);
    m_meter->setMinimumWidth(90);
    m_meter->setMaximumWidth(160);
    head->addWidget(m_meter, 1);

    head->addWidget(new HelpBubble(tr(
        "Real-time voice changer on your OWN microphone.\n"
        "- The toggle (or its TS3 hotkey) turns processing on/off.\n"
        "- LIVE lights up while you are transmitting processed voice.\n"
        "- Pitch is latency-free (1:1); presets configure the whole\n"
        "  chain in one click; Effects... opens the full editor.\n"
        "- A safety limiter always guards the output level."), frame));
    col->addLayout(head);

    // ---- controls row ----
    auto *row = new QHBoxLayout;
    row->setSpacing(8);

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

    row->addWidget(new QLabel(tr("Preset"), frame));
    m_presetBox = new QComboBox(frame);
    m_presetBox->setToolTip(tr(
        "Voice presets. Built-ins + your own saved presets. Picking one\n"
        "REPLACES the current mic chain settings + pitch."));
    row->addWidget(m_presetBox, 1);
    // Icon-only preset actions: a floppy (save), trash (delete), share-nodes
    // (share) and clipboard (paste) so each button's purpose reads at a glance.
    // Tooltips keep the words for discoverability.
    auto makePresetBtn = [frame, row](const QIcon &icon, const QString &tip) {
        auto *b = new QPushButton(frame);
        b->setIcon(icon);
        b->setIconSize(QSize(18, 18));
        b->setFixedSize(30, 26);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };
    m_presetSave   = makePresetBtn(makeSaveIcon(),
        tr("Save the current voice as a custom preset"));
    m_presetDelete = makePresetBtn(makeTrashIcon(),
        tr("Delete the selected custom preset"));
    m_presetShare  = makePresetBtn(makeShareIcon(),
        tr("Copy this voice preset to the clipboard so you can share it"));
    m_presetPaste  = makePresetBtn(makePasteIcon(),
        tr("Apply a shared voice preset from the clipboard"));
    rebuildPresetCombo();

    m_monitor = new QCheckBox(tr("Hear myself"), frame);
    m_monitor->setToolTip(tr(
        "Hear your own processed voice in your headphones while you\n"
        "speak. Useful to calibrate presets; off by default because\n"
        "hearing yourself while talking is distracting."));
    row->addWidget(m_monitor);

    m_fxBtn = new QPushButton(tr("Effects…"), frame);
    m_fxBtn->setToolTip(tr("Open the full effect chain editor for the microphone"));
    row->addWidget(m_fxBtn);
    col->addLayout(row);

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
    connect(m_presetSave,   &QPushButton::clicked, this, &MicChannel::onSavePreset);
    connect(m_presetDelete, &QPushButton::clicked, this, &MicChannel::onDeletePreset);
    connect(m_presetShare,  &QPushButton::clicked, this, &MicChannel::onSharePreset);
    connect(m_presetPaste,  &QPushButton::clicked, this, &MicChannel::onPastePreset);
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
    if (index < 0) return;
    const QVariant data = m_presetBox->itemData(index);
    if (data.type() == QVariant::Int) {
        MicFx::instance().applyPreset(data.toInt());          // built-in
    } else if (data.type() == QVariant::String && !data.toString().isEmpty()) {
        applyMicStateFromData(data.toString());               // user preset
    } else {
        m_presetDelete->setEnabled(false);
        return;   // placeholder row
    }
    // Only user presets can be deleted.
    m_presetDelete->setEnabled(data.type() == QVariant::String);
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

void MicChannel::rebuildPresetCombo()
{
    if (!m_presetBox) return;
    QSignalBlocker bl(m_presetBox);
    m_presetBox->clear();
    m_presetBox->addItem(tr("(choose a voice…)"));          // placeholder, no data
    int i = 0;
    for (const auto &p : MicFx::builtinPresets())
        m_presetBox->addItem(p.name, QVariant(i++));          // int data = builtin idx
    const auto user = PresetManager::loadMicPresets();
    if (!user.isEmpty()) {
        m_presetBox->insertSeparator(m_presetBox->count());
        for (const auto &p : user)
            m_presetBox->addItem(p.name, QVariant(p.data));   // string data = base64
    }
    m_presetBox->setCurrentIndex(0);
    if (m_presetDelete) m_presetDelete->setEnabled(false);
}

QString MicChannel::serialiseMicState() const
{
    QJsonObject o;
    o["pitch"]   = MicFx::instance().pitchSemitones();
    o["sandbox"] = MicFx::instance().sandboxState().toJson();
    const QByteArray json = QJsonDocument(o).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(json.toBase64());
}

void MicChannel::applyMicStateFromData(const QString &base64)
{
    const QByteArray json = QByteArray::fromBase64(base64.toLatin1());
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (!doc.isObject()) return;
    const QJsonObject o = doc.object();
    SandboxState st = SandboxState::fromJson(o.value("sandbox").toObject());
    MicFx::instance().setSandboxState(st);
    MicFx::instance().setPitchSemitones(static_cast<float>(o.value("pitch").toDouble()));
}

void MicChannel::onSavePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save voice preset"),
        tr("Preset name:"), QLineEdit::Normal, QString(), &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    PresetManager::saveMicPreset(name, serialiseMicState());
    rebuildPresetCombo();
    // Select the just-saved preset.
    for (int i = 0; i < m_presetBox->count(); ++i) {
        if (m_presetBox->itemText(i) == name) { m_presetBox->setCurrentIndex(i); break; }
    }
}

void MicChannel::onDeletePreset()
{
    const int idx = m_presetBox->currentIndex();
    const QVariant data = m_presetBox->itemData(idx);
    if (data.type() != QVariant::String) return;   // only user presets
    const QString name = m_presetBox->itemText(idx);
    if (QMessageBox::question(this, tr("Delete preset"),
            tr("Delete the preset \"%1\"?").arg(name)) != QMessageBox::Yes)
        return;
    PresetManager::deleteMicPreset(name);
    rebuildPresetCombo();
}

void MicChannel::onSharePreset()
{
    QApplication::clipboard()->setText("GBSB4-MIC:" + serialiseMicState());
    QMessageBox::information(this, tr("Share voice preset"),
        tr("The voice preset was copied to the clipboard. Share it — the "
           "recipient uses Paste to apply it."));
}

void MicChannel::onPastePreset()
{
    QString s = QApplication::clipboard()->text().trimmed();
    if (!s.startsWith("GBSB4-MIC:")) {
        QMessageBox::warning(this, tr("Paste voice preset"),
            tr("The clipboard does not contain a shared voice preset."));
        return;
    }
    applyMicStateFromData(s.mid(10));
    pullFromMicFx();
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
