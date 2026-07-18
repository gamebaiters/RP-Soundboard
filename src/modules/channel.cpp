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
#include <QProgressBar>
#include <QToolButton>
#include <QTimer>
#include <QSettings>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QIcon>
#include <QGraphicsDropShadowEffect>

// Indeterminate "loading" bar painted by hand. A QProgressBar in marquee mode
// draws an opaque groove using the native style's palette, which reads as a
// different colour than the themed channel background. This widget paints a
// TRANSPARENT track (nothing) + a single moving azure segment, so the "bar
// background" IS the channel — it can never mismatch. No Q_OBJECT needed
// (no new signals/slots; the QTimer uses a lambda).
class ChannelLoadingBar : public QWidget {
public:
    explicit ChannelLoadingBar(QWidget *parent = nullptr) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedHeight(6);
        setAttribute(Qt::WA_NoSystemBackground);   // never fill a background
        m_timer = new QTimer(this);
        m_timer->setInterval(30);   // ~33 Hz
        QObject::connect(m_timer, &QTimer::timeout, this, [this]{
            m_phase += 0.028; if (m_phase > 1.0) m_phase -= 1.0; update();
        });
    }
protected:
    void showEvent(QShowEvent *) override { m_timer->start(); }
    void hideEvent(QHideEvent *) override { m_timer->stop(); }
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double w = width(), h = height();
        // Paint the EXACT themed channel background as the track first, so the
        // bar can never read as a different colour than the surrounding channel
        // (same principle as the EQ slider groove fix: use the derived theme
        // colour explicitly, never rely on the native style / parent show-through
        // which produced a mismatched dark strip). Clipped to a rounded rect so
        // it tucks under the moving segment cleanly.
        const QColor track = Theme::derivedCached().surface;
        // Flat-fill the ENTIRE rect (incl. corners) with the channel surface so
        // no pixel is ever left unpainted — WA_NoSystemBackground means anything
        // we don't draw keeps stale/dark backing-store content (that was the
        // dark strip). Azure segment then rides on top.
        p.fillRect(QRectF(0.0, 0.0, w, h), track);
        const double segW = w * 0.35;
        const double x = -segW + m_phase * (w + segW);   // slides left->right, loops
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3f, 0xa7, 0xff));            // azure (matches the label)
        p.drawRoundedRect(QRectF(x, 0.0, segW, h), h / 2.0, h / 2.0);
    }
private:
    QTimer *m_timer;
    double  m_phase = 0.0;
};

#include <cmath>
#include <algorithm>

// (The v-next "glow" halo + "mini-EQ bars" stream effects that lived here
// were removed on user feedback — the WEB/LIVE badge on the title plus the
// animated waveform gradient are the stream affordances that stayed.)

// A tiny painted "list" icon (three lines) for the reopen-playlist button —
// drawn instead of a Unicode glyph so it renders identically on every host /
// font (a ☰ glyph showed as tofu / mojibake on the TS3 client font).
static QIcon makeListIcon()
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    QPen pen(QColor(0xc8, 0xc8, 0xc8));
    pen.setWidthF(2.0);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    for (int i = 0; i < 3; ++i) {
        int y = 4 + i * 4;
        p.drawLine(3, y, 13, y);
    }
    p.end();
    return QIcon(pm);
}
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
    m_baseName = m_titleEdit->text();
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
    // Title-row action height: 22 px on Windows, but font-metric-aware
    // so bigger system fonts (macOS) never clip the button text.
    const int actionH = qMax(22, fontMetrics().height() + 6);

    m_sandboxBtn = new QPushButton(this);
    m_sandboxBtn->setIcon(IconFactory::sandbox());
    m_sandboxBtn->setIconSize(QSize(18, 18));
    m_sandboxBtn->setFixedSize(30, actionH);
    m_sandboxBtn->setStyleSheet(
        "QPushButton { padding: 2px; }");
    m_sandboxBtn->setToolTip(tr(
        "Open the Audio Sandbox editor for this channel:\n"
        "  - 16-band ISO graphic EQ\n"
        "  - 3D HRTF spatial audio (manual / orbit / 8D preset)\n"
        "  - Paulstretch and 11 more DSP effects with a\n"
        "    drag-to-reorder pipeline\n"
        "Settings persist per channel and are bundled into macros."));

    // Icon-only (arrow-out-of-tray glyph, same painted set as the mic
    // preset buttons) - the tooltip carries the words. Keeps the title
    // row compact.
    m_exportBtn = new QPushButton(this);
    m_exportBtn->setIcon(IconFactory::exportAudio());
    m_exportBtn->setIconSize(QSize(18, 18));
    m_exportBtn->setFixedSize(30, actionH);
    m_exportBtn->setStyleSheet("QPushButton { padding: 2px; }");
    m_exportBtn->setToolTip(tr("Export this channel's audio with all DSP effects applied (any format)"));
    m_exportBtn->setVisible(false);
    connect(m_exportBtn, &QPushButton::clicked, this, [this]{ emit exportRequested(m_id); });

    // Dedicated "Save audio" button for STREAM channels (green download
    // pill). Separate from "Export audio": saving the source audio of a
    // stream and baking a local file's DSP are different actions, and the
    // DSP export does not apply to streams at all.
    // Green pill kept (it signals "downloadable stream" at a glance)
    // but icon-only now: white arrow-into-tray glyph.
    m_downloadBtn = new QPushButton(this);
    m_downloadBtn->setIcon(IconFactory::download(Qt::white));
    m_downloadBtn->setIconSize(QSize(18, 18));
    m_downloadBtn->setFixedSize(36, actionH);
    m_downloadBtn->setToolTip(tr("Download this video's audio to a file"));
    m_downloadBtn->setCursor(Qt::PointingHandCursor);
    m_downloadBtn->setStyleSheet(
        "QPushButton {"
        "  padding: 2px; border: none; border-radius: 11px;"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "               stop:0 #35c169, stop:1 #1f9a4d);"
        "}"
        "QPushButton:hover {"
        "  background: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
        "               stop:0 #43d179, stop:1 #23ab56); }"
        "QPushButton:pressed { background: #178a41; padding-top: 3px; }");
    m_downloadBtn->setVisible(false);
    connect(m_downloadBtn, &QPushButton::clicked, this, [this]{ emit downloadRequested(m_id); });

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
    // Reopen-playlist button: hidden until a playlist is loaded into this
    // channel; clicking it re-shows the (closed) playlist panel.
    m_playlistBtn = new QToolButton(this);
    m_playlistBtn->setIcon(makeListIcon());     // painted, not a font glyph
    m_playlistBtn->setIconSize(QSize(16, 16));
    // Icon + the word "Playlist" so its purpose is obvious at a glance —
    // the bare ☰ glyph read as a generic menu, not the playlist reopener.
    m_playlistBtn->setText(tr("Playlist"));
    m_playlistBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_playlistBtn->setToolTip(tr("Show playlist"));
    m_playlistBtn->setAutoRaise(true);
    m_playlistBtn->setVisible(false);
    titleRow->addWidget(m_playlistBtn, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_downloadBtn, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_exportBtn, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_sandboxEnableCheck, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_sandboxBtn, 0, Qt::AlignVCenter);

    // Indeterminate marquee + Cancel, shown only while a link is resolving.
    // Custom transparent-track bar so it can never mismatch the channel colour.
    m_loadingBar = new ChannelLoadingBar(m_frame);
    m_loadingBar->setToolTip(tr("Loading link…"));
    m_loadCancelBtn = new QToolButton(m_frame);
    m_loadCancelBtn->setText(QString::fromUtf8("\xC3\x97"));  // × (fromUtf8: MSVC-safe)
    m_loadCancelBtn->setToolTip(tr("Cancel"));
    m_loadCancelBtn->setAutoRaise(true);
    m_loadingRow = new QWidget(m_frame);
    // The row gets the channel surface as its OWN background (themed in
    // refreshTheme). A styled child (the blue label) inside the stylesheet-
    // styled #channelFrame otherwise renders on a default-dark backing, which
    // read as the mismatched dark "loading" strip. Objectname-scoped so the
    // rule never leaks to children.
    m_loadingRow->setObjectName(QStringLiteral("channelLoadingRow"));
    m_loadingRow->setStyleSheet(QString("#channelLoadingRow { background-color: %1; }")
                                .arg(Theme::derivedCached().surface.name()));
    auto *loadRowLay = new QHBoxLayout(m_loadingRow);
    loadRowLay->setContentsMargins(0, 0, 0, 0);
    loadRowLay->setSpacing(6);
    m_loadingText = new QLabel(tr("Loading link…"), m_loadingRow);
    // background: transparent so the label sits on the row surface, not a dark box.
    m_loadingText->setStyleSheet("color: #3fa7ff; font-weight: bold; background: transparent;");
    loadRowLay->addWidget(m_loadingText, 0);
    loadRowLay->addWidget(m_loadingBar, 1);
    loadRowLay->addWidget(m_loadCancelBtn, 0);
    m_loadingRow->setVisible(false);
    connect(m_loadCancelBtn, &QToolButton::clicked, this,
            [this]{ emit streamLoadCancelRequested(m_id); });
    connect(m_playlistBtn, &QToolButton::clicked, this,
            [this]{ emit playlistReopenRequested(m_id); });

    auto *frameLayout = new QVBoxLayout(m_frame);
    frameLayout->setContentsMargins(8,4,8,8);
    frameLayout->setSpacing(4);
    frameLayout->addLayout(titleRow);
    frameLayout->addWidget(m_loadingRow);
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
        // Own top-level window: register it so it carries the
        // soundboard stylesheet itself (host-theme isolation) and gets
        // its bipolar sliders tagged.
        Theme::trackThemedWidget(m_sandboxDialog);
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
    m_meterWanted = on;
    applyCompact();
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
    applyCompact();
    updateMeterWidth();
}

void Channel::applyCompact()
{
    // Width breakpoints for the progressive collapse. Order of
    // sacrifice as the row narrows: meter -> FX panel -> title-row
    // action buttons. Volume sliders, transport and the title always
    // stay. Each element still honours its Settings switch: collapse
    // can only hide, never force-show.
    const int w = width();
    const bool hideMeter  = w < 700;
    const bool hideFx     = w < 560;
    const bool hideExtras = w < 470;

    if (m_meter) m_meter->setVisible(m_meterWanted && !hideMeter);
    const bool fxOn = m_fxWanted && !hideFx;
    if (m_fx)          m_fx->setVisible(fxOn);
    if (m_fxSeparator) m_fxSeparator->setVisible(fxOn);
    if (m_exportBtn)
        m_exportBtn->setVisible(m_exportVisibleSetting && !hideExtras);
    if (m_downloadBtn)
        m_downloadBtn->setVisible(m_exportIsDownload && !hideExtras);
    if (m_playlistBtn)
        m_playlistBtn->setVisible(m_playlistAvailable && !hideExtras);
    const bool sandboxOn = m_sandboxFeatureEnabled && !hideExtras;
    if (m_sandboxBtn)         m_sandboxBtn->setVisible(sandboxOn);
    if (m_sandboxEnableCheck) m_sandboxEnableCheck->setVisible(sandboxOn);
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
    applyCompact();
    if (!on && m_sandboxDialog && m_sandboxDialog->isVisible())
        m_sandboxDialog->close();
}

void Channel::setExportVisible(bool on) {
    m_exportVisibleSetting = on;
    applyCompact();
}

void Channel::setExportIsDownload(bool on) {
    if (!m_exportBtn || !m_downloadBtn) return;
    m_exportIsDownload = on;
    // A VOD stream shows BOTH actions: the green "Save audio" (plain source
    // download) AND "Export audio" (true DSP bake via a temp copy) — they do
    // different things. Export keeps following its global visibility setting.
    applyCompact();
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

void Channel::setTempGlow(bool on) {
    if (m_tempGlow == on) return;
    m_tempGlow = on;
    if (!m_frame) return;
    if (on) {
        // Soft accent halo around the frame + a brighter border (set in
        // refreshTheme). QSS has no box-shadow; the drop-shadow effect
        // with zero offset is the Qt way to get an even glow.
        auto *glow = new QGraphicsDropShadowEffect(m_frame);
        glow->setBlurRadius(22.0);
        glow->setOffset(0.0, 0.0);
        glow->setColor(Theme::derivedCached().accent);
        m_frame->setGraphicsEffect(glow);
    } else {
        m_frame->setGraphicsEffect(nullptr);   // deletes the old effect
    }
    refreshTheme();
}

void Channel::refreshTheme() {
    Theme::Derived d = Theme::derive(Theme::colors());
    if (m_frame) {
        if (m_tempGlow) {
            // Temporary channel: accent border + glow (colour synced
            // here so theme switches recolour the halo too).
            if (auto *glow = qobject_cast<QGraphicsDropShadowEffect *>(
                    m_frame->graphicsEffect()))
                glow->setColor(d.accent);
            m_frame->setStyleSheet(QString(
                "#channelFrame { border: 2px solid %1; border-radius: 6px;"
                " background-color: %2; }")
                .arg(d.accent.name(), d.surface.name()));
        } else {
            m_frame->setStyleSheet(QString(
                "#channelFrame { border: 1px solid %1; border-radius: 6px;"
                " background-color: %2; }").arg(d.border.name(), d.surface.name()));
        }
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
    if (m_loadingRow) {
        // Match the channel background exactly so the loading strip is invisible
        // against the channel (only the moving azure segment + blue label show).
        m_loadingRow->setStyleSheet(QString(
            "#channelLoadingRow { background-color: %1; }").arg(d.surface.name()));
    }
    if (m_fx)     m_fx->refreshTheme();
    if (m_volume) m_volume->refreshTheme();
}

void Channel::setFxVisible(bool on) {
    m_fxWanted = on;
    applyCompact();
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
    // t is always the REAL channel name (persistence / rename). Remember it as
    // the restore target; if a stream link is currently shown, keep showing the
    // green link but update the name we'll fall back to.
    m_baseName = t;
    if (m_streamLinkActive) return;
    // Keep the dedup anchor in lock-step with the displayed text: any later
    // editingFinished (e.g. a focus-out caused by a dialog opening) reads the
    // SAME text and is swallowed, instead of firing a phantom titleChanged that
    // would look like a user rename and unload a just-opened playlist panel.
    m_lastEmittedTitle = t;
    if (m_titleEdit->text() == t) return;
    QSignalBlocker b(m_titleEdit);
    m_titleEdit->setText(t);
}

void Channel::showStreamLink(const QString &link) {
    m_streamLinkActive = true;
    m_lastEmittedTitle = link;   // sync dedup anchor (see setTitle)
    QSignalBlocker b(m_titleEdit);
    m_titleEdit->setText(link);
    // Green + bold so the user sees the link was recognised as a live stream.
    m_titleEdit->setStyleSheet(QString(
        "QLineEdit { background: transparent; color: #3fb950; border: none;"
        " font-weight: bold; padding: 2px 4px; }"));
}

void Channel::restoreName() {
    if (!m_streamLinkActive) return;
    m_streamLinkActive = false;
    // Sync the dedup anchor to the restored name: opening the playlist panel
    // (or any dialog) steals focus from the title edit, firing a phantom
    // editingFinished with this base name. Without this, that phantom looked
    // like a user rename and instantly unloaded the just-opened playlist.
    m_lastEmittedTitle = m_baseName;
    QSignalBlocker b(m_titleEdit);
    m_titleEdit->setText(m_baseName);
    refreshTheme();   // reapply the themed (non-green) title stylesheet
}

void Channel::setStreamLoading(bool on) {
    if (m_loadingRow) m_loadingRow->setVisible(on);
    // Hiding the marquee ends the stage: reset to the generic text so the
    // NEXT load never briefly shows a stale "Buffering…" / "Opening…".
    if (!on && m_loadingText) m_loadingText->setText(tr("Loading link…"));
}

void Channel::setStreamLoadingText(const QString &text) {
    if (m_loadingText)
        m_loadingText->setText(text.isEmpty() ? tr("Loading link…") : text);
}


void Channel::setPlaylistAvailable(bool on) {
    m_playlistAvailable = on;
    applyCompact();
}

void Channel::showDiscoveryBubble(const QString &text) {
    if (m_discoveryBubble) return;   // already shown once
    // SpeechBubble renders as a broken thin line in the TS3 host, so build a
    // plain self-contained card: a rounded, readable panel anchored just below
    // the channel title, dismissed by its "Got it" button or after a while.
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("ytDiscoveryCard"));
    card->setAttribute(Qt::WA_DeleteOnClose);
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(QStringLiteral(
        "#ytDiscoveryCard { background: #fff6d5; border: 1px solid #e6b800;"
        " border-radius: 8px; }"
        "#ytDiscoveryCard QLabel { color: #3a2f00; background: transparent; }"
        "#ytDiscoveryCard QPushButton { background: #e6b800; color: #3a2f00;"
        " border: none; border-radius: 4px; padding: 3px 10px; font-weight: bold; }"
        "#ytDiscoveryCard QPushButton:hover { background: #f0c500; }"));
    auto *lay = new QVBoxLayout(card);
    lay->setContentsMargins(10, 8, 10, 8);
    lay->setSpacing(6);
    auto *lbl = new QLabel(text, card);
    lbl->setWordWrap(true);
    lay->addWidget(lbl);
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto *ok = new QPushButton(tr("Got it"), card);
    btnRow->addWidget(ok);
    lay->addLayout(btnRow);
    connect(ok, &QPushButton::clicked, card, &QWidget::close);

    m_discoveryBubble = card;
    card->setFixedWidth(qMax(240, width() - 20));
    card->adjustSize();
    // Anchor under the title row (top of the channel frame).
    int y = m_frame ? (m_frame->y() + 30) : 30;
    card->move(10, y);
    card->show();
    card->raise();
    // Auto-dismiss after 15 s so it never lingers if the user ignores it.
    QTimer::singleShot(15000, card, &QWidget::close);
}

QString Channel::title() const {
    return m_titleEdit->text();
}

void Channel::onTitleEditFinished() {
    // QLineEdit::editingFinished fires TWICE for one edit (once on Enter, once
    // on the focus-out it triggers) — which double-loaded a pasted link and
    // opened the playlist dialog twice. Only emit when the text actually
    // changed since the last emit.
    const QString t = m_titleEdit->text();
    if (t == m_lastEmittedTitle) return;
    m_lastEmittedTitle = t;
    emit titleChanged(m_id, t);
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
