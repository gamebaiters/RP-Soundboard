#include "channel_sandbox_dialog.h"
#include "positional_pad.h"
#include "help_bubble.h"
#include "preset_manager.h"
#include "pipeline_widget.h"
#include "icon_factory.h"
#include "../ExpandableSection.h"
#include "../dsp/EqRack.h"
#include "eq_band_widget.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QSlider>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QScrollArea>
#include <QStyle>
#include <QApplication>
#include <QClipboard>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QJsonDocument>
#include <QScreen>
#include <QGuiApplication>
#include <QSettings>
#include <cmath>

namespace {
// Global preferred sandbox engine. Written by the sandbox dialog when
// the user picks Classic / Leia from the combo, read at Channel ctor
// time so a freshly-added channel inherits the last choice instead of
// always falling back to the SandboxState default (Classic). Per-channel
// persistence (when enabled) overrides this on restore.
constexpr const char *kPreferredEngineKey = "sandbox/preferred_engine";
} // namespace

namespace SandboxEnginePref {
int load() {
    QSettings s("GameBaiters", "Soundboard");
    return s.value(kPreferredEngineKey, SandboxState::Engine_Leia).toInt();
}
void save(int engine) {
    QSettings s("GameBaiters", "Soundboard");
    s.setValue(kPreferredEngineKey, engine);
}
} // namespace SandboxEnginePref

namespace {
QString fmtFreq(double hz) {
    if (hz >= 1000.0) {
        if (std::fmod(hz, 1000.0) == 0.0)
            return QString::number(static_cast<int>(hz / 1000)) + "k";
        return QString::number(hz / 1000.0, 'f', 1) + "k";
    }
    return QString::number(hz, 'f', 0);
}
QString fmtElev(int v) {
    if (v == 0) return ChannelSandboxDialog::tr("level");
    return v < 0 ? ChannelSandboxDialog::tr("down %1").arg(-v)
                 : ChannelSandboxDialog::tr("up %1").arg(v);
}
QString fmtPan(int v) {
    if (v == 0) return ChannelSandboxDialog::tr("center");
    return v < 0 ? ChannelSandboxDialog::tr("L %1").arg(-v)
                 : ChannelSandboxDialog::tr("R %1").arg(v);
}
QString genLossDesc(int v) {
    if (v <= 1)   return QStringLiteral("1 (pristine)");
    if (v <= 5)   return QString::number(v) + " (barely touched)";
    if (v <= 20)  return QString::number(v) + " (YouTube reupload)";
    if (v <= 50)  return QString::number(v) + " (VHS copy)";
    if (v <= 100) return QString::number(v) + " (deep fried)";
    if (v <= 200) return QString::number(v) + " (underwater)";
    if (v <= 500) return QString::number(v) + " (corrupted)";
    return QString::number(v) + " (destroyed)";
}

// Built-in EQ preset shapes shared by the preset dropdown lambda
// captures. Kept at namespace scope so lambdas don't have to capture
// the array.
struct EqPresetEntry { const char *name; float v[16]; };
static const EqPresetEntry kEqBuiltIn[] = {
    {"Flat",     {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
    {"Low cut",  {-12,-12,-10,-7,-4,-1,0,0,0,0,0,0,0,0,0,0}},
    {"High cut", {0,0,0,0,0,0,0,0,0,0,0,-2,-5,-8,-10,-12}},
    {"Smile",    {4,5,5,4,2,0,-2,-3,-3,-2,0,2,4,5,5,4}},
    {"Voice",    {-4,-3,-2,-1,0,1,2,3,4,5,5,4,2,0,-2,-4}},
};
static constexpr int kEqBuiltInCount =
    (int)(sizeof(kEqBuiltIn) / sizeof(kEqBuiltIn[0]));

// Map dropdown index <-> SandboxState::SpatialMode. The dropdown order
// is the natural reading order; the enum values are stable so saved
// INI files keep loading correctly.
struct ModeMap { int dropdownIdx; int enumValue; const char *label; };
const ModeMap kModes[] = {
    { 0, SandboxState::Spatial_Off,      "Off (no spatial)" },
    { 1, SandboxState::Spatial_LRPan,    "L/R Pan (simple balance)" },
    { 2, SandboxState::Spatial_3DManual, "3D HRTF - manual position" },
    { 3, SandboxState::Spatial_3DRotate, "3D HRTF - auto-orbit (rotate)" },
    { 4, SandboxState::Spatial_8DPreset, "3D HRTF - 8D preset" },
};
constexpr int kModeCount = sizeof(kModes) / sizeof(kModes[0]);
}

ChannelSandboxDialog::ChannelSandboxDialog(int channelId, QWidget *parent)
    : QDialog(parent)
    , m_channelId(channelId)
{
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setProperty("isGBSoundboard", true);
    setModal(false);
    refreshTitle();
    // Open at a size that fits the user's screen. The whole dialog is
    // scroll-backed, so a smaller window scrolls instead of clipping -
    // never force a window bigger than the desktop.
    {
        QSize avail(1280, 800);
        if (QScreen *scr = QGuiApplication::primaryScreen())
            avail = scr->availableGeometry().size();
        resize(qMin(1360, avail.width()  - 60),
               qMin(1000, avail.height() - 40));
    }
    setMinimumSize(640, 460);
    buildUi();
    pushStateToWidgets();
    applyModeVisibility();
}

int ChannelSandboxDialog::modeForDropdown(int idx) const {
    if (idx < 0 || idx >= kModeCount) return SandboxState::Spatial_Off;
    return kModes[idx].enumValue;
}

int ChannelSandboxDialog::dropdownForMode(int mode) const {
    for (int i = 0; i < kModeCount; ++i)
        if (kModes[i].enumValue == mode) return i;
    return 0;
}

void ChannelSandboxDialog::setChannelTitle(const QString &title)
{
    m_channelTitle = title;
    refreshTitle();
}

void ChannelSandboxDialog::refreshTitle()
{
    QString base = tr("Channel %1").arg(m_channelId + 1);
    if (!m_channelTitle.isEmpty() && m_channelTitle != base)
        base = QString("%1 (%2)").arg(m_channelTitle, base);
    setWindowTitle(tr("%1 - Audio Sandbox").arg(base));
}

void ChannelSandboxDialog::setState(const SandboxState &s)
{
    m_state = s;
    pushStateToWidgets();
    applyModeVisibility();
}

void ChannelSandboxDialog::pushChange()
{
    if (m_loading) return;
    emit stateChanged(m_state);
}

namespace {
// Helper: build a [label][slider][value] row, returns the wrapper widget
// + writes the slider/label out-pointers so the caller can wire them.
QWidget *buildSliderRow(QWidget *parent, const QString &label,
                       int min, int max, int defVal, const QString &suffix,
                       QSlider *&outSlider, QLabel *&outLabel) {
    auto *w = new QWidget(parent);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(new QLabel(label, w));
    outSlider = new QSlider(Qt::Horizontal, w);
    outSlider->setRange(min, max);
    outSlider->setValue(defVal);
    outLabel = new QLabel(QString::number(defVal) + suffix, w);
    outLabel->setMinimumWidth(60);
    outLabel->setAlignment(Qt::AlignRight);
    h->addWidget(outSlider, 1);
    h->addWidget(outLabel);
    return w;
}
}

void ChannelSandboxDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);

    // ===== Top: enable checkbox =====
    auto *topRow = new QHBoxLayout;
    m_enable = new QCheckBox(tr("Enable the Audio Sandbox on this channel"), this);
    m_enable->setToolTip(tr(
        "Master per-channel switch. When OFF, the entire DSP chain is\n"
        "bypassed (zero CPU cost) but every value below is preserved\n"
        "so re-enabling brings everything back exactly as set."));
    topRow->addWidget(m_enable);
    topRow->addWidget(new HelpBubble(tr(
        "Master switch. When OFF the entire DSP chain is bypassed and\n"
        "consumes zero CPU. Every parameter below is preserved on disk\n"
        "so you can re-enable later and pick up exactly where you left."), this));
    topRow->addStretch(1);
    m_cpuLabel = new QLabel(tr("CPU --"), this);
    m_cpuLabel->setStyleSheet("color: #8aa6c0; font-size: 10px;");
    m_cpuLabel->setToolTip(tr(
        "DSP CPU used by this channel over the last second.\n"
        "High values mean a heavy effect is engaged."));
    topRow->addWidget(m_cpuLabel);
    root->addLayout(topRow);

    // ===== Spatial mode selector =====
    // Vertical so the caption (which wraps) sits above the combo - in a
    // horizontal row a long non-wrapping caption forces the whole
    // spatial panel wider than its viewport and clips every control.
    auto *modeSection = new QVBoxLayout;
    auto *modeHeader = new QLabel(tr(
        "Spatial mode — choose how the sound is positioned in 3D space:"), this);
    modeHeader->setStyleSheet("font-weight: bold; margin-top: 2px;");
    modeHeader->setWordWrap(true);
    modeSection->addWidget(modeHeader);
    auto *modeRow = new QHBoxLayout;
    m_modeBox = new QComboBox(this);
    for (const auto &m : kModes) m_modeBox->addItem(QObject::tr(m.label));
    // No hard minimum width - the combo must be free to shrink with the
    // panel; the drop-down still shows each item in full.
    m_modeBox->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modeBox->setMinimumContentsLength(6);
    modeRow->addWidget(m_modeBox, 1);
    modeRow->addWidget(new HelpBubble(tr(
        "Spatial mode:\n"
        "  - Off: pure passthrough.\n"
        "  - L/R Pan: equal-power balance (cheap, no HRTF).\n"
        "  - 3D Manual: place a virtual source on the pad and slide\n"
        "    elevation / distance / stereo width.\n"
        "  - 3D Rotate: source orbits around your head at the chosen\n"
        "    RPM and radius.\n"
        "  - 8D preset: orbit + head sway + tight width, the classic\n"
        "    'YouTube 8D remix' recipe."), this));
    modeSection->addLayout(modeRow);
    // The mode selector is packed into the fixed-size spatial panel
    // below so that every spatial control lives in one self-contained,
    // independently scrollable box.
    auto *modeWidget = new QWidget(this);
    modeWidget->setLayout(modeSection);

    // ===== Mid row: Spatial column | EQ column =====
    auto *midRow = new QHBoxLayout;

    // ---- Spatial column: collected here, then wrapped in a fixed-size
    //      scroll area further down. ----
    auto *spatialCol = new QVBoxLayout;
    spatialCol->addWidget(modeWidget);

    // Pan group (visible only in L/R Pan mode)
    m_panGroup = new QGroupBox(tr("L/R Pan"), this);
    {
        auto *l = new QFormLayout(m_panGroup);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *row = buildSliderRow(m_panGroup, tr("Balance"),
                                   -100, 100, 0, "", s, lbl);
        m_pan = s; m_panLabel = lbl;
        m_panLabel->setText(fmtPan(0));
        l->addRow(row);
    }
    spatialCol->addWidget(m_panGroup);

    // 3D HRTF group (visible in any 3D mode; child widgets selectively
    // hidden depending on which 3D mode is active).
    m_hrtfGroup = new QGroupBox(tr("3D HRTF"), this);
    {
        auto *l = new QVBoxLayout(m_hrtfGroup);

        // HRTF engine selector - applies to every 3D mode.
        // HRTF engine selector lives in Settings → Channels now
        // (Leia is the new default for every cell). A hidden combo
        // box stays in the layout but is parented out + invisible so
        // every existing connect()/signal path keeps compiling
        // without #ifdef noise. The Settings window writes
        // SandboxEnginePref::save() which Channel ctor pulls in on
        // every new sound, and pushStateToWidgets() still mirrors
        // m_state.spatialEngine onto m_engineBox so existing widget
        // wiring (load8DPreset, pad updates) keeps functioning.
        m_engineBox = new QComboBox(this);
        m_engineBox->addItem(tr("Classic (parametric)"));
        m_engineBox->addItem(tr("Leia (measured HRTF)"));
        m_engineBox->hide();

        m_padContainer = new QWidget(m_hrtfGroup);
        auto *padLay = new QVBoxLayout(m_padContainer);
        padLay->setContentsMargins(0, 0, 0, 0);
        m_pad = new PositionalPad(m_padContainer);
        m_pad->setMinimumSize(180, 180);
        padLay->addWidget(m_pad, 1);
        m_padLabel = new QLabel(tr("x=0.00 y=0.00"), m_padContainer);
        m_padLabel->setAlignment(Qt::AlignCenter);
        padLay->addWidget(m_padLabel);
        l->addWidget(m_padContainer);

        QWidget *row = nullptr;
        QSlider *s = nullptr; QLabel *lbl = nullptr;

        row = buildSliderRow(m_hrtfGroup, tr("Elevation"),
                              -100, 100, 0, "", s, lbl);
        m_elev = s; m_elevLabel = lbl; m_elevLabel->setText(fmtElev(0));
        l->addWidget(row);

        m_distRow = buildSliderRow(m_hrtfGroup, tr("Distance"),
                                    50, 500, 150, " cm", s, lbl);
        m_dist = s; m_distLabel = lbl;
        l->addWidget(m_distRow);

        m_widthRow = buildSliderRow(m_hrtfGroup, tr("Stereo width"),
                              0, 120, 60, " deg", s, lbl);
        m_width = s; m_widthLabel = lbl;
        l->addWidget(m_widthRow);

        m_rpmRow = buildSliderRow(m_hrtfGroup, tr("RPM"),
                                   0, 120, 15, " rpm", s, lbl);
        m_rpm = s; m_rpmLabel = lbl;
        l->addWidget(m_rpmRow);

        m_radiusRow = buildSliderRow(m_hrtfGroup, tr("Orbit radius"),
                                      50, 500, 150, " cm", s, lbl);
        m_radius = s; m_radiusLabel = lbl;
        l->addWidget(m_radiusRow);

        m_ccw = new QCheckBox(tr("Counter-clockwise (left first)"), m_hrtfGroup);
        l->addWidget(m_ccw);
        m_sway = new QCheckBox(tr("Head sway (cone-of-confusion break)"), m_hrtfGroup);
        m_sway->setToolTip(tr(
            "Adds a slow 0.3 Hz wobble to the source position simulating\n"
            "the tiny head movements humans make. Strongly improves the\n"
            "'sound is OUT of my head' feel on static positions."));
        l->addWidget(m_sway);

        QWidget *r2 = nullptr;
        QSlider *s2 = nullptr; QLabel *lbl2 = nullptr;
        r2 = buildSliderRow(m_hrtfGroup, tr("Spatial mix"),
                              0, 100, 100, "%", s2, lbl2);
        m_spatialMix = s2; m_spatialMixLabel = lbl2;
        m_spatialMix->setToolTip(tr(
            "Wet/dry crossfade between the HRTF processed signal and\n"
            "the raw stereo input. 100 = full HRTF placement, 0 =\n"
            "bypass (raw stereo). Pull back to ~70 if the spatialised\n"
            "audio feels mono/filtered - this brings the original\n"
            "stereo content back without losing the position cue."));
        l->addWidget(r2);
        r2 = buildSliderRow(m_hrtfGroup, tr("Ambience"),
                              0, 100, 0, "%", s2, lbl2);
        m_ambience = s2; m_ambienceLabel = lbl2;
        m_ambience->setToolTip(tr(
            "Stereo room ambience reverb (Freeverb-style). Pushes the\n"
            "HRTF placement OUT of the head by adding subtle room\n"
            "reflections. Independent from the per-button FxPanel\n"
            "reverb (which is a sound-shaping wet, not a spatialiser)."));
        l->addWidget(r2);
    }
    spatialCol->addWidget(m_hrtfGroup, 1);

    // Leia engine room / tone controls (visible only when the Leia
    // engine is selected for a 3D mode).
    m_leiaGroup = new QGroupBox(tr("Leia engine - room & tone"), this);
    {
        auto *l = new QVBoxLayout(m_leiaGroup);
        QSlider *s = nullptr; QLabel *lbl = nullptr;

        m_leiaRefl = new QCheckBox(tr("Early reflections (room)"), m_leiaGroup);
        m_leiaRefl->setToolTip(tr(
            "Image-source early reflections. They place the sound in a\n"
            "real room - the single strongest 'out of the head' cue.\n"
            "Turn off for a dry, anechoic placement."));
        l->addWidget(m_leiaRefl);

        QWidget *row = buildSliderRow(m_leiaGroup, tr("Reflection level"),
                                      -25, 20, -6, " dB", s, lbl);
        m_leiaReflLevel = s; m_leiaReflLevelLabel = lbl;
        l->addWidget(row);

        row = buildSliderRow(m_leiaGroup, tr("Room size"),
                             7, 50, 12, " m", s, lbl);
        m_leiaRoomSize = s; m_leiaRoomSizeLabel = lbl;
        l->addWidget(row);

        {
            auto *rtRow = new QWidget(m_leiaGroup);
            auto *rh = new QHBoxLayout(rtRow);
            rh->setContentsMargins(0, 0, 0, 0);
            rh->addWidget(new QLabel(tr("Room type"), rtRow));
            m_leiaRoomType = new QComboBox(rtRow);
            m_leiaRoomType->addItem(tr("Drapes (most absorbent)"));
            m_leiaRoomType->addItem(tr("Studio"));
            m_leiaRoomType->addItem(tr("Tiles"));
            m_leiaRoomType->addItem(tr("Concrete"));
            m_leiaRoomType->addItem(tr("Glass (most reflective)"));
            m_leiaRoomType->addItem(tr("Living room"));
            m_leiaRoomType->addItem(tr("Wood cabin"));
            m_leiaRoomType->addItem(tr("Hall"));
            m_leiaRoomType->addItem(tr("Cathedral"));
            m_leiaRoomType->addItem(tr("Bathroom"));
            m_leiaRoomType->addItem(tr("Car"));
            m_leiaRoomType->addItem(tr("Outdoor (anechoic)"));
            m_leiaRoomType->addItem(tr("Underwater"));
            m_leiaRoomType->setSizeAdjustPolicy(
                QComboBox::AdjustToMinimumContentsLengthWithIcon);
            m_leiaRoomType->setMinimumContentsLength(6);
            rh->addWidget(m_leiaRoomType, 1);
            l->addWidget(rtRow);
        }

        row = buildSliderRow(m_leiaGroup, tr("Clarity"),
                             0, 100, 100, "%", s, lbl);
        m_leiaClarity = s; m_leiaClarityLabel = lbl;
        m_leiaClarity->setToolTip(tr(
            "Level of the direct (dry HRTF) path. 100 = full direct\n"
            "sound; lower values lean on the room reflections."));
        l->addWidget(row);

        row = buildSliderRow(m_leiaGroup, tr("Width"),
                             0, 100, 35, "%", s, lbl);
        m_leiaWidth = s; m_leiaWidthLabel = lbl;
        m_leiaWidth->setToolTip(tr(
            "Amount of room reflection blended in. 0 = pure direct,\n"
            "higher = a wider, more enveloping space."));
        l->addWidget(row);
    }
    spatialCol->addWidget(m_leiaGroup, 1);

    // ---- EQ (belongs with spatial on the left) ----

    auto *eqBox = new QGroupBox(tr("16-band EQ (-12..+12 dB)"), this);
    auto *eqOuter = new QVBoxLayout(eqBox);
    auto *eqHeader = new QHBoxLayout;
    m_eqEnable = new QCheckBox(tr("Enable EQ"), eqBox);
    m_eqEnable->setToolTip(tr(
        "Master bypass for the 16-band graphic EQ. Off = the entire\n"
        "EQ chain is skipped (zero CPU cost, signal passes untouched).\n"
        "Slider values are kept on disk so you can A/B with one click."));
    eqHeader->addWidget(m_eqEnable);
    eqHeader->addWidget(new HelpBubble(tr(
        "ISO-aligned 2/3-octave centres at 20, 25, 40, 63, 100, 160,\n"
        "250, 400, 630, 1k, 1.6k, 2.5k, 4k, 6.3k, 10k, 16k Hz with\n"
        "matched Q (= 2.145) so adjacent bands sum to about +/-12 dB\n"
        "at the seam without exploding into earrape. Big jumps reset\n"
        "the band's IIR state to kill cascading transients."), eqBox));
    auto *copyEqBtn = new QPushButton(tr("Copy EQ"), eqBox);
    auto *pasteEqBtn = new QPushButton(tr("Paste EQ"), eqBox);
    eqHeader->addWidget(copyEqBtn);
    eqHeader->addWidget(pasteEqBtn);
    connect(copyEqBtn, &QPushButton::clicked, this, &ChannelSandboxDialog::onCopyEq);
    connect(pasteEqBtn, &QPushButton::clicked, this, &ChannelSandboxDialog::onPasteEq);
    // EQ preset picker - small dropdown matching the sandbox preset
    // strip below. Built-in presets are listed first (locked, no
    // delete) followed by the user's own saved curves. Save dumps the
    // current 16 bands into a named preset; Delete only fires on user
    // presets.
    auto applyShape = [this](const float *shape){
        m_loading = true;
        for (int b = 0; b < 16 && b < m_eqSliders.size(); ++b) {
            int v = (int)std::round(shape[b]);
            if (v < -12) v = -12;
            if (v >  12) v =  12;
            m_state.eqBandDb[b] = (float)v;
            if (m_eqSliders[b]) m_eqSliders[b]->setValue(v);
        }
        m_loading = false;
        pushChange();
    };
    eqHeader->addSpacing(6);
    m_eqPresetBox = new QComboBox(eqBox);
    m_eqPresetBox->setMinimumContentsLength(10);
    auto *eqPresetSaveBtn   = new QPushButton(tr("Save..."), eqBox);
    auto *eqPresetDeleteBtn = new QPushButton(tr("Delete"),  eqBox);
    eqPresetSaveBtn->setMaximumWidth(64);
    eqPresetDeleteBtn->setMaximumWidth(64);
    eqHeader->addWidget(m_eqPresetBox);
    eqHeader->addWidget(eqPresetSaveBtn);
    eqHeader->addWidget(eqPresetDeleteBtn);

    auto refreshEqCombo = [this]{
        QSignalBlocker bl(m_eqPresetBox);
        m_eqPresetBox->clear();
        m_eqPresetBox->addItem(tr("(select preset)"), -1);
        for (int i = 0; i < kEqBuiltInCount; ++i)
            m_eqPresetBox->addItem(QString::fromLatin1(kEqBuiltIn[i].name), i);
        const auto userPresets = PresetManager::loadEqPresets();
        if (!userPresets.isEmpty())
            m_eqPresetBox->insertSeparator(m_eqPresetBox->count());
        for (const auto &p : userPresets)
            m_eqPresetBox->addItem(p.name, 1000 + m_eqPresetBox->count());
    };
    refreshEqCombo();

    auto updateDeleteEnabled = [this, eqPresetDeleteBtn]{
        int idx = m_eqPresetBox->currentIndex();
        QVariant tag = m_eqPresetBox->itemData(idx);
        bool isUser = idx >= 0 && tag.isValid() && tag.toInt() >= 1000;
        eqPresetDeleteBtn->setEnabled(isUser);
    };
    updateDeleteEnabled();

    connect(m_eqPresetBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this, applyShape, updateDeleteEnabled](int idx){
        QVariant tag = m_eqPresetBox->itemData(idx);
        if (tag.isValid()) {
            int t = tag.toInt();
            if (t >= 0 && t < kEqBuiltInCount) {
                applyShape(kEqBuiltIn[t].v);
            } else if (t >= 1000) {
                QString name = m_eqPresetBox->itemText(idx);
                const auto presets = PresetManager::loadEqPresets();
                for (const auto &p : presets) {
                    if (p.name == name) {
                        QStringList parts = p.data.split('#', Qt::SkipEmptyParts);
                        float buf[16] = {0};
                        for (int i = 0; i < 16 && i < parts.size(); ++i)
                            buf[i] = parts[i].toFloat();
                        applyShape(buf);
                        break;
                    }
                }
            }
        }
        updateDeleteEnabled();
    });

    connect(eqPresetSaveBtn, &QPushButton::clicked, this,
            [this, refreshEqCombo]{
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Save EQ preset"),
            tr("Preset name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        name = name.trimmed();
        // Refuse a name that collides with a built-in - those are
        // immutable. The user can always pick a different name.
        for (int i = 0; i < kEqBuiltInCount; ++i) {
            if (name.compare(QString::fromLatin1(kEqBuiltIn[i].name),
                              Qt::CaseInsensitive) == 0) {
                QMessageBox::warning(this, tr("Save EQ preset"),
                    tr("\"%1\" is a built-in preset; pick another name.").arg(name));
                return;
            }
        }
        QString data;
        for (int i = 0; i < 16; ++i)
            data += "#" + QString::number(m_state.eqBandDb[i]);
        PresetManager::saveEqPreset(name, data);
        refreshEqCombo();
        for (int i = 0; i < m_eqPresetBox->count(); ++i) {
            if (m_eqPresetBox->itemText(i) == name) {
                QSignalBlocker bl(m_eqPresetBox);
                m_eqPresetBox->setCurrentIndex(i);
                break;
            }
        }
    });

    connect(eqPresetDeleteBtn, &QPushButton::clicked, this,
            [this, refreshEqCombo]{
        int idx = m_eqPresetBox->currentIndex();
        QVariant tag = m_eqPresetBox->itemData(idx);
        if (!tag.isValid() || tag.toInt() < 1000) return;
        QString name = m_eqPresetBox->itemText(idx);
        auto choice = QMessageBox::question(this, tr("Delete EQ preset"),
            tr("Delete preset \"%1\"?").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        PresetManager::deleteEqPreset(name);
        refreshEqCombo();
    });
    eqHeader->addStretch(1);
    eqOuter->addLayout(eqHeader);

    auto *eqGrid = new QWidget(eqBox);
    auto *eqLay = new QGridLayout(eqGrid);
    eqLay->setHorizontalSpacing(0);
    eqLay->setVerticalSpacing(2);
    eqLay->setContentsMargins(0, 0, 0, 0);
    eqOuter->addWidget(eqGrid, 1);
    for (int i = 0; i < EqRack::kNumBands; ++i) {
        auto *s = new EqBandWidget(eqGrid);
        s->setMinimumHeight(110);
        s->setMaximumWidth(14);
        s->setMinimumWidth(14);
        m_eqSliders.append(s);
        auto *freq = new QLabel(fmtFreq(EqRack::bandFrequency(i)), eqGrid);
        freq->setAlignment(Qt::AlignHCenter);
        freq->setStyleSheet("font-size: 9px;");
        freq->setMaximumWidth(30);
        auto *val = new QLabel("0", eqGrid);
        val->setAlignment(Qt::AlignHCenter);
        val->setStyleSheet("font-size: 9px;");
        val->setMaximumWidth(30);
        m_eqLabels.append(val);
        eqLay->addWidget(s,    0, i, Qt::AlignHCenter);
        eqLay->addWidget(freq, 1, i, Qt::AlignHCenter);
        eqLay->addWidget(val,  2, i, Qt::AlignHCenter);

        connect(s, &QSlider::valueChanged, this, [this, i, val](int v){
            val->setText(QString::number(v));
            if (m_loading) return;
            m_state.eqBandDb[i] = static_cast<float>(v);
            pushChange();
        });
    }
    m_eqBox = eqBox;

    // Wrap every spatial control (mode selector, Pan, 3D HRTF, Leia
    // room/tone) into a fixed-height, independently scrollable panel.
    // This is the whole point: the user can reach any spatial option
    // by scrolling this box alone, never the entire dialog.
    spatialCol->addStretch(1);
    auto *spatialInner = new QWidget(this);
    spatialInner->setLayout(spatialCol);
    m_spatialScroll = new QScrollArea(this);
    m_spatialScroll->setWidget(spatialInner);
    m_spatialScroll->setWidgetResizable(true);
    // As-needed (not Off): if the panel is ever narrower than the
    // controls' hard minimum, scroll rather than clip them invisibly.
    m_spatialScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_spatialScroll->setFixedHeight(480);
    m_spatialScroll->setMinimumWidth(320);

    auto *leftCol = new QVBoxLayout;
    leftCol->addWidget(m_spatialScroll);
    leftCol->addWidget(eqBox, 1);

    midRow->addLayout(leftCol, 2);

    // ---- DSP modules column (right) ----
    auto *fxCol = new QVBoxLayout;

    // ===== DSP modules in a scrollable area =====
    m_dspGroup = new QGroupBox(tr("DSP Modules"), this);
    auto *dspGroupLay = new QVBoxLayout(m_dspGroup);
    dspGroupLay->setContentsMargins(4, 8, 4, 4);

    // Pipeline order bar + reset-order button.
    auto *pipeRow = new QHBoxLayout;
    auto *pipeLabel = new QLabel(tr(
        "Pipeline order — drag to reorder, click a block to open it:"), m_dspGroup);
    pipeLabel->setStyleSheet("font-weight: bold; font-size: 11px;");
    pipeRow->addWidget(pipeLabel, 1);
    m_resetOrderBtn = new QPushButton(tr("Reset order"), m_dspGroup);
    m_resetOrderBtn->setToolTip(tr(
        "Restore the default DSP processing order. Effect parameters\n"
        "are left untouched — only the chain order is reset."));
    pipeRow->addWidget(m_resetOrderBtn);
    dspGroupLay->addLayout(pipeRow);

    m_pipeline = new PipelineWidget(m_dspGroup);
    dspGroupLay->addWidget(m_pipeline);

    // Mono fold-down: a plain post-chain checkbox, NOT a pipeline module.
    auto *monoRow = new QHBoxLayout;
    m_monoEnable = new QCheckBox(tr("Fold output to mono"), m_dspGroup);
    m_monoEnable->setToolTip(tr(
        "Sum left + right into a single mono signal sent equally to\n"
        "both ears. Applied at the very end of the chain — it is not\n"
        "a reorderable module."));
    monoRow->addWidget(m_monoEnable);
    monoRow->addWidget(new HelpBubble(tr(
        "Mono fold-down collapses the stereo image to one centred\n"
        "signal. Handy for compatibility checks or to feed the spatial\n"
        "stage from a clean mono source."), m_dspGroup));
    monoRow->addStretch(1);
    dspGroupLay->addLayout(monoRow);

    // Search bar over the module list.
    m_dspSearch = new QLineEdit(m_dspGroup);
    m_dspSearch->setPlaceholderText(tr("Search DSP modules..."));
    m_dspSearch->setClearButtonEnabled(true);
    dspGroupLay->addWidget(m_dspSearch);

    m_dspScrollArea = new QScrollArea(m_dspGroup);
    m_dspScrollArea->setWidgetResizable(true);
    m_dspScrollArea->setFrameShape(QFrame::StyledPanel);
    auto *dspScrollArea = m_dspScrollArea;            // local alias
    auto *dspScrollContent = new QWidget(dspScrollArea);
    m_dspScrollLay = new QVBoxLayout(dspScrollContent);
    auto *dspScrollLay = m_dspScrollLay;              // local alias

    auto makeResetBtn = [](QWidget *parent) -> QPushButton* {
        auto *btn = new QPushButton(parent);
        btn->setIcon(IconFactory::reload());
        btn->setIconSize(QSize(14, 14));
        btn->setToolTip(tr("Reset this module to defaults"));
        btn->setFixedSize(22, 22);
        btn->setFlat(true);
        return btn;
    };

    // ---- Paulstretch (represented module; runs on a separate feed) ----
    auto *stretchSection = new ExpandableSection(tr("Paulstretch"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_stretchEn = new QCheckBox(tr("Enable Paulstretch"));
        m_stretchEn->setToolTip(tr(
            "Paul Nasca's phase-randomisation time-stretch. Source plays back\n"
            "much slower (1x..50x) while keeping pitch. Stereo channels get\n"
            "independent random phases for a wide diffuse texture."));
        hdr->addWidget(m_stretchEn);
        hdr->addWidget(new HelpBubble(tr(
            "Paulstretch is a LOCAL effect: only you hear the stretched\n"
            "audio — the server gets a clean mic stream while it is on,\n"
            "exactly like preview-only mode. Its pipeline slot is fixed\n"
            "first: it runs on a separate streaming feed before the\n"
            "rest of the chain."), nullptr));
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *r1 = buildSliderRow(nullptr, tr("Factor"), 10, 500, 40, "", s, lbl);
        m_stretchFac = s; m_stretchFacLabel = lbl; m_stretchFacLabel->setText("4.0x");
        lay->addWidget(r1);
        auto *r2 = buildSliderRow(nullptr, tr("Window"), 50, 1000, 180, " ms", s, lbl);
        m_stretchWin = s; m_stretchWinLabel = lbl;
        m_stretchWin->setToolTip(tr(
            "FFT analysis window in milliseconds. Bigger = smoother /\n"
            "more 'frozen-in-amber' drone, blurred high frequencies.\n"
            "Smaller = grittier with more transient detail."));
        lay->addWidget(r2);
        stretchSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(stretchSection);

    // ---- Compressor ----
    auto *compSection = new ExpandableSection(tr("Compressor"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_compEnable = new QCheckBox(tr("Enable Compressor"));
        hdr->addWidget(m_compEnable);
        m_resetComp = makeResetBtn(compSection);
        hdr->addWidget(m_resetComp);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *r1 = buildSliderRow(nullptr, tr("Threshold"), -600, 0, -200, "", s, lbl);
        m_compThreshold = s; m_compThresholdLabel = lbl; m_compThresholdLabel->setText("-20.0 dB");
        lay->addWidget(r1);
        auto *r2 = buildSliderRow(nullptr, tr("Ratio"), 10, 200, 40, "", s, lbl);
        m_compRatio = s; m_compRatioLabel = lbl; m_compRatioLabel->setText("4.0:1");
        lay->addWidget(r2);
        auto *r3 = buildSliderRow(nullptr, tr("Attack"), 1, 1000, 100, "", s, lbl);
        m_compAttack = s; m_compAttackLabel = lbl; m_compAttackLabel->setText("10.0 ms");
        lay->addWidget(r3);
        auto *r4 = buildSliderRow(nullptr, tr("Release"), 10, 1000, 100, " ms", s, lbl);
        m_compRelease = s; m_compReleaseLabel = lbl;
        lay->addWidget(r4);
        auto *r5 = buildSliderRow(nullptr, tr("Knee"), 0, 200, 60, "", s, lbl);
        m_compKnee = s; m_compKneeLabel = lbl; m_compKneeLabel->setText("6.0 dB");
        lay->addWidget(r5);
        auto *r6 = buildSliderRow(nullptr, tr("Makeup"), 0, 300, 0, "", s, lbl);
        m_compMakeup = s; m_compMakeupLabel = lbl; m_compMakeupLabel->setText("0.0 dB");
        lay->addWidget(r6);
        compSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(compSection);

    // ---- Saturator ----
    auto *satSection = new ExpandableSection(tr("Saturator"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_satEnable = new QCheckBox(tr("Enable Saturator"));
        hdr->addWidget(m_satEnable);
        m_resetSat = makeResetBtn(satSection);
        hdr->addWidget(m_resetSat);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        auto *modeRow = new QHBoxLayout;
        modeRow->addWidget(new QLabel(tr("Mode:")));
        m_satMode = new QComboBox;
        m_satMode->addItems({tr("Soft (tanh)"), tr("Tube"), tr("Tape"), tr("Hard clip")});
        modeRow->addWidget(m_satMode);
        modeRow->addStretch(1);
        lay->addLayout(modeRow);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *r1 = buildSliderRow(nullptr, tr("Drive"), 10, 200, 20, "", s, lbl);
        m_satDrive = s; m_satDriveLabel = lbl; m_satDriveLabel->setText("2.0x");
        lay->addWidget(r1);
        auto *r2 = buildSliderRow(nullptr, tr("Tone"), 1000, 20000, 8000, " Hz", s, lbl);
        m_satTone = s; m_satToneLabel = lbl;
        lay->addWidget(r2);
        auto *r3 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_satMix = s; m_satMixLabel = lbl;
        lay->addWidget(r3);
        satSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(satSection);

    // ---- Chorus ----
    auto *chorusSection = new ExpandableSection(tr("Chorus"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_chorusEnable = new QCheckBox(tr("Enable Chorus"));
        hdr->addWidget(m_chorusEnable);
        m_resetChorus = makeResetBtn(chorusSection);
        hdr->addWidget(m_resetChorus);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *c1 = buildSliderRow(nullptr, tr("Rate"), 1, 50, 10, "", s, lbl);
        m_chorusRate = s; m_chorusRateLabel = lbl; m_chorusRateLabel->setText("1.0 Hz");
        lay->addWidget(c1);
        auto *c2 = buildSliderRow(nullptr, tr("Depth"), 0, 100, 30, "", s, lbl);
        m_chorusDepth = s; m_chorusDepthLabel = lbl; m_chorusDepthLabel->setText("3.0 ms");
        lay->addWidget(c2);
        auto *c3 = buildSliderRow(nullptr, tr("Delay"), 5, 30, 10, " ms", s, lbl);
        m_chorusDelay = s; m_chorusDelayLabel = lbl;
        lay->addWidget(c3);
        auto *c4 = buildSliderRow(nullptr, tr("Voices"), 1, 4, 2, "", s, lbl);
        m_chorusVoices = s; m_chorusVoicesLabel = lbl;
        lay->addWidget(c4);
        auto *c5 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_chorusMix = s; m_chorusMixLabel = lbl;
        lay->addWidget(c5);
        chorusSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(chorusSection);

    // ---- Flanger ----
    auto *flangerSection = new ExpandableSection(tr("Flanger"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_flangerEnable = new QCheckBox(tr("Enable Flanger"));
        hdr->addWidget(m_flangerEnable);
        m_resetFlanger = makeResetBtn(flangerSection);
        hdr->addWidget(m_resetFlanger);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *f1 = buildSliderRow(nullptr, tr("Rate"), 5, 1000, 50, "", s, lbl);
        m_flangerRate = s; m_flangerRateLabel = lbl; m_flangerRateLabel->setText("0.50 Hz");
        lay->addWidget(f1);
        auto *f2 = buildSliderRow(nullptr, tr("Depth"), 0, 100, 70, "%", s, lbl);
        m_flangerDepth = s; m_flangerDepthLabel = lbl;
        lay->addWidget(f2);
        auto *f3 = buildSliderRow(nullptr, tr("Feedback"), -95, 95, 50, "%", s, lbl);
        m_flangerFeedback = s; m_flangerFeedbackLabel = lbl;
        lay->addWidget(f3);
        auto *f4 = buildSliderRow(nullptr, tr("Delay"), 5, 50, 20, "", s, lbl);
        m_flangerDelay = s; m_flangerDelayLabel = lbl; m_flangerDelayLabel->setText("2.0 ms");
        lay->addWidget(f4);
        auto *f5 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_flangerMix = s; m_flangerMixLabel = lbl;
        lay->addWidget(f5);
        flangerSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(flangerSection);

    // ---- Flangus ----
    auto *flangusSection = new ExpandableSection(tr("Flangus"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_flangusEnable = new QCheckBox(tr("Enable Flangus"));
        hdr->addWidget(m_flangusEnable);
        m_resetFlangus = makeResetBtn(flangusSection);
        hdr->addWidget(m_resetFlangus);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *g1 = buildSliderRow(nullptr, tr("Rate"), 1, 50, 8, "", s, lbl);
        m_flangusRate = s; m_flangusRateLabel = lbl; m_flangusRateLabel->setText("0.8 Hz");
        lay->addWidget(g1);
        auto *g2 = buildSliderRow(nullptr, tr("Depth"), 0, 100, 50, "%", s, lbl);
        m_flangusDepth = s; m_flangusDepthLabel = lbl;
        lay->addWidget(g2);
        auto *g3 = buildSliderRow(nullptr, tr("Feedback"), -95, 95, 30, "%", s, lbl);
        m_flangusFeedback = s; m_flangusFeedbackLabel = lbl;
        lay->addWidget(g3);
        auto *g4 = buildSliderRow(nullptr, tr("Voices"), 1, 4, 3, "", s, lbl);
        m_flangusVoices = s; m_flangusVoicesLabel = lbl;
        lay->addWidget(g4);
        auto *g5 = buildSliderRow(nullptr, tr("Spread"), 0, 100, 50, "%", s, lbl);
        m_flangusSpread = s; m_flangusSpreadLabel = lbl;
        lay->addWidget(g5);
        auto *g6 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_flangusMix = s; m_flangusMixLabel = lbl;
        lay->addWidget(g6);
        flangusSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(flangusSection);

    // ---- Phaser ----
    auto *phaserSection = new ExpandableSection(tr("Phaser"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_phaserEnable = new QCheckBox(tr("Enable Phaser"));
        hdr->addWidget(m_phaserEnable);
        m_resetPhaser = makeResetBtn(phaserSection);
        hdr->addWidget(m_resetPhaser);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *p1 = buildSliderRow(nullptr, tr("Rate"), 5, 500, 50, "", s, lbl);
        m_phaserRate = s; m_phaserRateLabel = lbl; m_phaserRateLabel->setText("0.50 Hz");
        lay->addWidget(p1);
        auto *p2 = buildSliderRow(nullptr, tr("Depth"), 0, 100, 70, "%", s, lbl);
        m_phaserDepth = s; m_phaserDepthLabel = lbl;
        lay->addWidget(p2);
        auto *p3 = buildSliderRow(nullptr, tr("Feedback"), 0, 95, 30, "%", s, lbl);
        m_phaserFeedback = s; m_phaserFeedbackLabel = lbl;
        lay->addWidget(p3);
        auto *p4 = buildSliderRow(nullptr, tr("Stages"), 1, 6, 3, "", s, lbl);
        m_phaserStages = s; m_phaserStagesLabel = lbl;
        lay->addWidget(p4);
        auto *p5 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_phaserMix = s; m_phaserMixLabel = lbl;
        lay->addWidget(p5);
        phaserSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(phaserSection);

    // ---- Delay / Echo ----
    auto *delaySection = new ExpandableSection(tr("Delay / Echo"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_delayEnable = new QCheckBox(tr("Enable Delay"));
        hdr->addWidget(m_delayEnable);
        m_resetDelay = makeResetBtn(delaySection);
        hdr->addWidget(m_resetDelay);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *d1 = buildSliderRow(nullptr, tr("Time"), 10, 3000, 300, " ms", s, lbl);
        m_delayTime = s; m_delayTimeLabel = lbl;
        lay->addWidget(d1);
        auto *d2 = buildSliderRow(nullptr, tr("Feedback"), 0, 95, 40, "%", s, lbl);
        m_delayFeedback = s; m_delayFeedbackLabel = lbl;
        lay->addWidget(d2);
        auto *d3 = buildSliderRow(nullptr, tr("Damping"), 1000, 20000, 5000, " Hz", s, lbl);
        m_delayDamping = s; m_delayDampingLabel = lbl;
        lay->addWidget(d3);
        m_delayPingPong = new QCheckBox(tr("Ping-pong (L/R bounce)"));
        lay->addWidget(m_delayPingPong);
        auto *d4 = buildSliderRow(nullptr, tr("Mix"), 0, 100, 0, "%", s, lbl);
        m_delayMix = s; m_delayMixLabel = lbl;
        lay->addWidget(d4);
        delaySection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(delaySection);

    // ---- Limiter ----
    auto *limiterSection = new ExpandableSection(tr("Limiter"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_limiterEnable = new QCheckBox(tr("Enable Limiter"));
        hdr->addWidget(m_limiterEnable);
        m_resetLimiter = makeResetBtn(limiterSection);
        hdr->addWidget(m_resetLimiter);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        auto *lmRow = new QHBoxLayout;
        lmRow->addWidget(new QLabel(tr("Mode:")));
        m_limiterModeBox = new QComboBox;
        m_limiterModeBox->addItems({tr("Limiter"), tr("Compressor"), tr("Gate")});
        lmRow->addWidget(m_limiterModeBox);
        lmRow->addStretch(1);
        lay->addLayout(lmRow);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *lr1 = buildSliderRow(nullptr, tr("Ceiling"), -60, 0, -3, "", s, lbl);
        m_limiterCeiling = s; m_limiterCeilingLabel = lbl; m_limiterCeilingLabel->setText("-0.3 dB");
        lay->addWidget(lr1);
        auto *lr2 = buildSliderRow(nullptr, tr("Lookahead"), 5, 100, 10, "", s, lbl);
        m_limiterLookahead = s; m_limiterLookaheadLabel = lbl; m_limiterLookaheadLabel->setText("1.0 ms");
        lay->addWidget(lr2);
        auto *lr3 = buildSliderRow(nullptr, tr("Release"), 10, 500, 100, " ms", s, lbl);
        m_limiterRelease = s; m_limiterReleaseLabel = lbl;
        lay->addWidget(lr3);
        auto *lr4 = buildSliderRow(nullptr, tr("Ratio"), 10, 200, 40, "", s, lbl);
        m_limiterRatio = s; m_limiterRatioLabel = lbl; m_limiterRatioLabel->setText("4.0:1");
        lay->addWidget(lr4);
        auto *lr5 = buildSliderRow(nullptr, tr("Gate Thresh"), -800, -200, -600, "", s, lbl);
        m_limiterGate = s; m_limiterGateLabel = lbl; m_limiterGateLabel->setText("-60.0 dB");
        lay->addWidget(lr5);
        limiterSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(limiterSection);

    // ---- Bitcrusher ----
    auto *bitcrushSection = new ExpandableSection(tr("Bitcrusher (quality degradation)"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_bitcrushEnable = new QCheckBox(tr("Enable Bitcrusher"));
        m_bitcrushEnable->setToolTip(tr(
            "Degrade audio quality by reducing bit depth and/or effective\n"
            "sample rate. At maximum settings (16 bit, 48 kHz) the signal\n"
            "is untouched. Lower values add grit and lo-fi character."));
        hdr->addWidget(m_bitcrushEnable);
        m_resetBitcrush = makeResetBtn(bitcrushSection);
        hdr->addWidget(m_resetBitcrush);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        m_bitcrushPreset = new QComboBox;
        m_bitcrushPreset->addItems({tr("Custom"), tr("HiFi (CD quality)"), tr("Radio"), tr("Telephone"), tr("Retro (8-bit)"), tr("Garbage")});
        m_bitcrushPreset->setToolTip(tr("Quick quality presets - select one to set bit depth and sample rate automatically"));
        lay->addWidget(m_bitcrushPreset);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *b1 = buildSliderRow(nullptr, tr("Bit Depth"), 1, 16, 16, "", s, lbl);
        m_bitcrushBits = s; m_bitcrushBitsLabel = lbl; m_bitcrushBitsLabel->setText("16 bit");
        m_bitcrushBits->setToolTip(tr(
            "Bit depth controls how many amplitude levels are available.\n"
            "16 = CD quality, 12 = radio, 8 = telephone, 4 = retro, 1 = extreme."));
        lay->addWidget(b1);
        auto *b2 = buildSliderRow(nullptr, tr("Sample Rate"), 500, 48000, 48000, "", s, lbl);
        m_bitcrushRate = s; m_bitcrushRateLabel = lbl; m_bitcrushRateLabel->setText("48000 Hz");
        lay->addWidget(b2);
        bitcrushSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(bitcrushSection);

    // ---- Generation Loss ----
    auto *genLossSection = new ExpandableSection(tr("Generation Loss"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_genLossEnable = new QCheckBox(tr("Enable Generation Loss"));
        m_genLossEnable->setToolTip(tr(
            "Simulates the quality loss of re-encoding audio multiple\n"
            "times (like a video re-uploaded to YouTube 20 times).\n"
            "Adds cumulative low-pass filtering, quantization noise,\n"
            "random noise injection, and soft compression."));
        hdr->addWidget(m_genLossEnable);
        m_resetGenLoss = makeResetBtn(genLossSection);
        hdr->addWidget(m_resetGenLoss);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *g1 = buildSliderRow(nullptr, tr("Generations"), 1, 1000, 1, "", s, lbl);
        m_genLossGens = s; m_genLossGensLabel = lbl; m_genLossGensLabel->setText("1 (pristine)");
        m_genLossGens->setToolTip(tr(
            "Number of simulated re-encoding passes.\n"
            "Like re-rendering a video over and over:\n"
            "1 = clean, 20 = YouTube reupload, 100 = deep fried,\n"
            "500 = corrupted, 1000 = completely destroyed."));
        lay->addWidget(g1);
        genLossSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(genLossSection);

    // ---- Random per-fire pitch jitter ----
    auto *randomSection = new ExpandableSection(
        tr("Random pitch per fire"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_randomEnable = new QCheckBox(tr("Enable random pitch jitter"));
        m_randomEnable->setToolTip(tr(
            "Roll a fresh random pitch offset every fire and on every\n"
            "loop. Keeps repeated triggers from sounding mechanical."));
        hdr->addWidget(m_randomEnable);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        QSlider *rs = nullptr; QLabel *rlbl = nullptr;
        auto *r1 = buildSliderRow(nullptr, tr("Pitch range +/-"),
                                  0, 200, 0, tr(" cents"), rs, rlbl);
        m_randomPitch = nullptr; // we keep QSpinBox below, slider unused
        // Replace with QSpinBox for explicit numeric entry (cents are
        // small enough that the slider precision was awkward).
        delete r1;
        m_randomPitch = new QSpinBox(nullptr);
        m_randomPitch->setRange(0, 200);
        m_randomPitch->setSuffix(tr(" cents"));
        auto *rrow = new QHBoxLayout;
        rrow->addWidget(new QLabel(tr("Pitch range +/-")));
        rrow->addWidget(m_randomPitch, 1);
        lay->addLayout(rrow);
        randomSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(randomSection);

    // ---- Sidechain ducking ----
    auto *duckSection = new ExpandableSection(
        tr("Sidechain ducking"), 200, dspScrollContent);
    {
        auto *lay = new QVBoxLayout;
        auto *hdr = new QHBoxLayout;
        m_duckEnable = new QCheckBox(tr("Duck other channels while I play"));
        m_duckEnable->setToolTip(tr(
            "While this channel produces audio every other slot's\n"
            "output is attenuated by the amount below. Smooth attack /\n"
            "release. Music channel + SFX channel pair: music dips\n"
            "under the SFX and returns when it ends."));
        hdr->addWidget(m_duckEnable);
        hdr->addStretch(1);
        lay->addLayout(hdr);
        m_duckAmount = new QSlider(Qt::Horizontal);
        m_duckAmount->setRange(-30, 0);
        m_duckAmount->setValue(-12);
        m_duckAmountLabel = new QLabel("-12 dB");
        m_duckAmountLabel->setMinimumWidth(56);
        m_duckAmountLabel->setAlignment(Qt::AlignRight);
        auto *drow = new QHBoxLayout;
        drow->addWidget(new QLabel(tr("Amount:")));
        drow->addWidget(m_duckAmount, 1);
        drow->addWidget(m_duckAmountLabel);
        lay->addLayout(drow);
        duckSection->setContentLayout(*lay);
    }
    dspScrollLay->addWidget(duckSection);

    // Map each DspStage to its accordion panel. EQ, Spatial and Reverb
    // stay nullptr - their controls live in the left column.
    m_stageSection[SandboxState::Stage_Paulstretch] = stretchSection;
    m_stageSection[SandboxState::Stage_Compressor]  = compSection;
    m_stageSection[SandboxState::Stage_Saturator]   = satSection;
    m_stageSection[SandboxState::Stage_Chorus]      = chorusSection;
    m_stageSection[SandboxState::Stage_Flanger]     = flangerSection;
    m_stageSection[SandboxState::Stage_Flangus]     = flangusSection;
    m_stageSection[SandboxState::Stage_Phaser]      = phaserSection;
    m_stageSection[SandboxState::Stage_Delay]       = delaySection;
    m_stageSection[SandboxState::Stage_Limiter]     = limiterSection;
    m_stageSection[SandboxState::Stage_Bitcrusher]  = bitcrushSection;
    m_stageSection[SandboxState::Stage_GenLoss]     = genLossSection;

    // Remember which DSP panels the user left open/closed so re-opening
    // the sandbox does not force them to re-collapse everything.
    for (int st = 0; st < SandboxState::Stage_COUNT; ++st)
        if (m_stageSection[st])
            m_stageSection[st]->setPersistenceKey(
                QStringLiteral("sandbox_stage_") + QString::number(st));

    dspScrollLay->addStretch(1);
    dspScrollArea->setWidget(dspScrollContent);
    dspGroupLay->addWidget(dspScrollArea, 1);
    fxCol->addWidget(m_dspGroup, 1);

    midRow->addLayout(fxCol, 3);

    // Wrap midRow in scroll area
    auto *scrollContent = new QWidget(this);
    auto *scrollLayout = new QVBoxLayout(scrollContent);
    scrollLayout->setContentsMargins(0,0,0,0);
    scrollLayout->addLayout(midRow, 1);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidget(scrollContent);
    root->addWidget(scrollArea, 1);
    m_outerScroll = scrollArea;

    // ===== Preset row =====
    auto *presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel(tr("Preset:"), this));
    m_sandboxPresetBox = new QComboBox(this);
    m_sandboxPresetBox->setMinimumWidth(180);
    m_sandboxPresetBox->setToolTip(tr("Load a saved sandbox preset"));
    presetRow->addWidget(m_sandboxPresetBox, 1);
    auto *presetSaveBtn = new QPushButton(tr("Save"), this);
    presetSaveBtn->setToolTip(tr("Save current sandbox settings as a named preset"));
    auto *presetDeleteBtn = new QPushButton(tr("Delete"), this);
    presetDeleteBtn->setToolTip(tr("Delete the selected preset"));
    presetRow->addWidget(presetSaveBtn);
    presetRow->addWidget(presetDeleteBtn);
    root->addLayout(presetRow);

    auto refreshPresetCombo = [this]{
        QSignalBlocker blk(m_sandboxPresetBox);
        m_sandboxPresetBox->clear();
        m_sandboxPresetBox->addItem(tr("(none)"));
        auto presets = PresetManager::loadSandboxPresets();
        for (const auto &p : presets)
            m_sandboxPresetBox->addItem(p.name, p.data);
    };
    refreshPresetCombo();

    connect(m_sandboxPresetBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx){
        if (idx <= 0 || m_loading) return;
        QString data = m_sandboxPresetBox->itemData(idx).toString();
        QByteArray decoded = QByteArray::fromBase64(data.toUtf8());
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(decoded, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
        m_state = SandboxState::fromJson(doc.object());
        pushStateToWidgets();
        applyModeVisibility();
        pushChange();
    });

    connect(presetSaveBtn, &QPushButton::clicked, this, [this, refreshPresetCombo]{
        bool ok = false;
        QString name = QInputDialog::getText(this, tr("Save Sandbox Preset"),
            tr("Preset name:"), QLineEdit::Normal, QString(), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        name = name.trimmed();
        QJsonDocument doc(m_state.toJson());
        QString encoded = QString::fromUtf8(doc.toJson(QJsonDocument::Compact).toBase64());
        PresetManager::saveSandboxPreset(name, encoded);
        refreshPresetCombo();
        for (int i = 0; i < m_sandboxPresetBox->count(); ++i) {
            if (m_sandboxPresetBox->itemText(i) == name) {
                m_sandboxPresetBox->setCurrentIndex(i);
                break;
            }
        }
    });

    connect(presetDeleteBtn, &QPushButton::clicked, this, [this, refreshPresetCombo]{
        int idx = m_sandboxPresetBox->currentIndex();
        if (idx <= 0) return;
        QString name = m_sandboxPresetBox->itemText(idx);
        auto choice = QMessageBox::question(this, tr("Delete Preset"),
            tr("Delete preset \"%1\"?").arg(name),
            QMessageBox::Yes | QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        PresetManager::deleteSandboxPreset(name);
        refreshPresetCombo();
    });

    // Random per-fire pitch jitter + Sidechain ducking live inside the
    // DSP accordion above (alongside Compressor, Saturator, etc.) so
    // they share the visual language of the rest of the chain instead
    // of stealing a whole row of the dialog.

    // ===== Bottom: reset + copy/paste sandbox + close =====
    auto *btnRow = new QHBoxLayout;
    m_resetBtn = new QPushButton(tr("Reset audio sandbox for this channel"), this);
    m_resetBtn->setStyleSheet(
        "QPushButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 4px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e04141; }");
    m_resetBtn->setToolTip(tr(
        "Reset only this channel's sandbox settings to defaults.\n"
        "Volume, FX panel, file and the channel's other state are\n"
        "left untouched."));
    btnRow->addWidget(m_resetBtn);
    auto *copySbxBtn = new QPushButton(tr("Copy Sandbox"), this);
    auto *pasteSbxBtn = new QPushButton(tr("Paste Sandbox"), this);
    btnRow->addWidget(copySbxBtn);
    btnRow->addWidget(pasteSbxBtn);
    connect(copySbxBtn, &QPushButton::clicked, this, &ChannelSandboxDialog::onCopySandbox);
    connect(pasteSbxBtn, &QPushButton::clicked, this, &ChannelSandboxDialog::onPasteSandbox);
    btnRow->addStretch(1);
    m_closeBtn = new QPushButton(tr("Close"), this);
    btnRow->addWidget(m_closeBtn);
    root->addLayout(btnRow);

    // ===== connections =====
    connect(m_enable,  &QCheckBox::toggled,    this, &ChannelSandboxDialog::onEnableToggled);
    connect(m_modeBox, qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ChannelSandboxDialog::onModeChanged);
    connect(m_pan,     &QSlider::valueChanged, this, &ChannelSandboxDialog::onPanChanged);
    connect(m_pad,     &PositionalPad::positionChanged,
            this, &ChannelSandboxDialog::onPadMoved);
    connect(m_elev,    &QSlider::valueChanged, this, &ChannelSandboxDialog::onElevChanged);
    connect(m_dist,    &QSlider::valueChanged, this, &ChannelSandboxDialog::onDistChanged);
    connect(m_width,   &QSlider::valueChanged, this, &ChannelSandboxDialog::onWidthChanged);
    connect(m_rpm,     &QSlider::valueChanged, this, &ChannelSandboxDialog::onRpmChanged);
    connect(m_radius,  &QSlider::valueChanged, this, &ChannelSandboxDialog::onRadiusChanged);
    connect(m_ccw,     &QCheckBox::toggled,    this, &ChannelSandboxDialog::onCcwToggled);
    connect(m_sway,    &QCheckBox::toggled,    this, &ChannelSandboxDialog::onSwayToggled);
    connect(m_spatialMix, &QSlider::valueChanged, this, &ChannelSandboxDialog::onSpatialMixChanged);
    connect(m_ambience,   &QSlider::valueChanged, this, &ChannelSandboxDialog::onAmbienceChanged);
    connect(m_engineBox,  qOverload<int>(&QComboBox::currentIndexChanged),
            this, &ChannelSandboxDialog::onEngineChanged);
    connect(m_leiaRefl, &QCheckBox::toggled, this, [this](bool on){
        m_state.leiaReflEnable = on; pushChange();
    });
    connect(m_leiaReflLevel, &QSlider::valueChanged, this, [this](int v){
        m_state.leiaReflLevel = static_cast<float>(v);
        m_leiaReflLevelLabel->setText(QString::number(v) + " dB");
        pushChange();
    });
    connect(m_leiaRoomSize, &QSlider::valueChanged, this, [this](int v){
        m_state.leiaRoomSize = static_cast<float>(v);
        m_leiaRoomSizeLabel->setText(QString::number(v) + " m");
        pushChange();
    });
    connect(m_leiaRoomType, qOverload<int>(&QComboBox::currentIndexChanged),
            this, [this](int idx){
        // Each preset is a BUNDLE: absorption (handled by the engine
        // from leiaRoomType) + a recommended room size + reflection
        // level + width slider. Without bundling, picking "Cathedral"
        // on a 10 m room at -25 dB reflLevel sounded identical to
        // "Drapes" - because the reflection signal was already drowned
        // by absorption-independent gating. The bundle below
        // guarantees an audible difference on every preset.
        //
        // size in metres / reflLvl in dB / width % / enableRefl
        struct PresetBundle { float size; int reflDb; int width; bool enable; };
        static const PresetBundle kBundle[] = {
            { 10.0f,  -12,  70, true  }, // 0  Drapes
            { 10.0f,   -9,  60, true  }, // 1  Studio
            { 10.0f,   -3,  80, true  }, // 2  Tiles
            { 15.0f,   -3,  85, true  }, // 3  Concrete
            { 15.0f,   -3,  90, true  }, // 4  Glass
            { 12.0f,   -6,  70, true  }, // 5  Living room
            { 10.0f,   -6,  65, true  }, // 6  Wood cabin
            { 35.0f,   -3,  90, true  }, // 7  Hall
            { 50.0f,   -1,  95, true  }, // 8  Cathedral
            {  7.0f,    0,  85, true  }, // 9  Bathroom
            {  7.0f,   -9,  60, true  }, // 10 Car
            { 50.0f,  -25,  10, false }, // 11 Outdoor (no reflections)
            { 18.0f,   -6,  60, true  }, // 12 Underwater
        };
        const int n = static_cast<int>(sizeof(kBundle) / sizeof(kBundle[0]));
        if (idx < 0 || idx >= n) {
            m_state.leiaRoomType = idx;
            pushChange();
            return;
        }
        const PresetBundle &b = kBundle[idx];
        m_state.leiaRoomType   = idx;
        m_state.leiaRoomSize   = b.size;
        m_state.leiaReflLevel  = static_cast<float>(b.reflDb);
        m_state.leiaWidth      = static_cast<float>(b.width);
        m_state.leiaReflEnable = b.enable;
        // Guarantee audibility: the room type ONLY produces sound when
        // the Leia engine is rendering a 3D mode. Picking a preset
        // while the channel sits on Classic / Off / LR Pan would do
        // absolutely nothing audible - "I picked Cathedral and heard
        // no change" was exactly that. Force the engine to Leia and
        // bump the spatial mode to 3D Manual when needed, plus
        // re-enable the master sandbox if the user had it off.
        bool needsModeBump = (m_state.spatialMode != SandboxState::Spatial_3DManual
                          &&  m_state.spatialMode != SandboxState::Spatial_3DRotate
                          &&  m_state.spatialMode != SandboxState::Spatial_8DPreset);
        if (!m_state.enabled)        m_state.enabled        = true;
        if (m_state.spatialEngine != SandboxState::Engine_Leia)
            m_state.spatialEngine = SandboxState::Engine_Leia;
        if (needsModeBump)
            m_state.spatialMode = SandboxState::Spatial_3DManual;
        // Make sure the spatial wet/dry isn't zero either - a user with
        // the slider at 0 would still hear nothing. Nudge to at least
        // 60% but never lower the user's setting.
        if (m_state.spatialMix < 0.6f) m_state.spatialMix = 0.6f;
        // Push the new bundle values into the widgets too so the user
        // sees what just happened. m_loading-style blocker not needed
        // here - the per-widget connect lambdas all early-return on
        // m_loading, and we set it before / after the setters.
        m_loading = true;
        if (m_leiaRoomSize) {
            m_leiaRoomSize->setValue(static_cast<int>(b.size));
            m_leiaRoomSizeLabel->setText(QString::number(static_cast<int>(b.size)) + " m");
        }
        if (m_leiaReflLevel) {
            m_leiaReflLevel->setValue(b.reflDb);
            m_leiaReflLevelLabel->setText(QString::number(b.reflDb) + " dB");
        }
        if (m_leiaWidth) {
            m_leiaWidth->setValue(b.width);
            m_leiaWidthLabel->setText(QString::number(b.width) + "%");
        }
        if (m_leiaRefl) m_leiaRefl->setChecked(b.enable);
        if (m_engineBox && m_engineBox->currentIndex() != 1)
            m_engineBox->setCurrentIndex(1); // Leia
        if (needsModeBump && m_modeBox)
            m_modeBox->setCurrentIndex(dropdownForMode(SandboxState::Spatial_3DManual));
        if (m_enable && !m_enable->isChecked()) m_enable->setChecked(true);
        if (m_spatialMix) m_spatialMix->setValue(
            std::max(m_spatialMix->value(), 60));
        m_loading = false;
        applyModeVisibility();
        pushChange();
    });
    connect(m_leiaClarity, &QSlider::valueChanged, this, [this](int v){
        m_state.leiaClarity = static_cast<float>(v);
        m_leiaClarityLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_leiaWidth, &QSlider::valueChanged, this, [this](int v){
        m_state.leiaWidth = static_cast<float>(v);
        m_leiaWidthLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_stretchEn,  &QCheckBox::toggled,    this, &ChannelSandboxDialog::onStretchToggled);
    connect(m_stretchFac, &QSlider::valueChanged, this, &ChannelSandboxDialog::onStretchFactorChanged);
    connect(m_stretchWin, &QSlider::valueChanged, this, &ChannelSandboxDialog::onStretchWindowChanged);
    connect(m_eqEnable,&QCheckBox::toggled,    this, &ChannelSandboxDialog::onEqEnabledToggled);
    connect(m_resetBtn,&QPushButton::clicked,  this, &ChannelSandboxDialog::onResetClicked);
    connect(m_closeBtn,&QPushButton::clicked,  this, &QDialog::accept);

    // Compressor
    connect(m_compEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.compEnabled = on; pushChange();
    });
    connect(m_compThreshold, &QSlider::valueChanged, this, [this](int v){
        m_state.compThresholdDb = v / 10.0f;
        m_compThresholdLabel->setText(QString::number(v / 10.0, 'f', 1) + " dB");
        pushChange();
    });
    connect(m_compRatio, &QSlider::valueChanged, this, [this](int v){
        m_state.compRatio = v / 10.0f;
        m_compRatioLabel->setText(QString::number(v / 10.0, 'f', 1) + ":1");
        pushChange();
    });
    connect(m_compAttack, &QSlider::valueChanged, this, [this](int v){
        m_state.compAttackMs = v / 10.0f;
        m_compAttackLabel->setText(QString::number(v / 10.0, 'f', 1) + " ms");
        pushChange();
    });
    connect(m_compRelease, &QSlider::valueChanged, this, [this](int v){
        m_state.compReleaseMs = static_cast<float>(v);
        m_compReleaseLabel->setText(QString::number(v) + " ms");
        pushChange();
    });
    connect(m_compKnee, &QSlider::valueChanged, this, [this](int v){
        m_state.compKneeDb = v / 10.0f;
        m_compKneeLabel->setText(QString::number(v / 10.0, 'f', 1) + " dB");
        pushChange();
    });
    connect(m_compMakeup, &QSlider::valueChanged, this, [this](int v){
        m_state.compMakeupDb = v / 10.0f;
        m_compMakeupLabel->setText(QString::number(v / 10.0, 'f', 1) + " dB");
        pushChange();
    });

    // Saturator
    connect(m_satEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.saturatorEnabled = on;
        if (on && m_state.saturatorMix < 0.01f) {
            m_state.saturatorMix = 0.5f;
            m_satMix->setValue(50);
        }
        pushChange();
    });
    connect(m_satMode, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx){
        m_state.saturatorMode = idx; pushChange();
    });
    connect(m_satDrive, &QSlider::valueChanged, this, [this](int v){
        m_state.saturatorDrive = v / 10.0f;
        m_satDriveLabel->setText(QString::number(v / 10.0, 'f', 1) + "x");
        pushChange();
    });
    connect(m_satTone, &QSlider::valueChanged, this, [this](int v){
        m_state.saturatorTone = static_cast<float>(v);
        m_satToneLabel->setText(QString::number(v) + " Hz");
        pushChange();
    });
    connect(m_satMix, &QSlider::valueChanged, this, [this](int v){
        m_state.saturatorMix = v / 100.0f;
        m_satMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Chorus
    connect(m_chorusEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.chorusEnabled = on;
        if (on && m_state.chorusMix < 0.01f) {
            m_state.chorusMix = 0.5f;
            m_chorusMix->setValue(50);
        }
        pushChange();
    });
    connect(m_chorusRate, &QSlider::valueChanged, this, [this](int v){
        m_state.chorusRate = v / 10.0f;
        m_chorusRateLabel->setText(QString::number(v / 10.0, 'f', 1) + " Hz");
        pushChange();
    });
    connect(m_chorusDepth, &QSlider::valueChanged, this, [this](int v){
        m_state.chorusDepth = v / 10.0f;
        m_chorusDepthLabel->setText(QString::number(v / 10.0, 'f', 1) + " ms");
        pushChange();
    });
    connect(m_chorusDelay, &QSlider::valueChanged, this, [this](int v){
        m_state.chorusBaseDelay = static_cast<float>(v);
        m_chorusDelayLabel->setText(QString::number(v) + " ms");
        pushChange();
    });
    connect(m_chorusVoices, &QSlider::valueChanged, this, [this](int v){
        m_state.chorusVoices = v;
        m_chorusVoicesLabel->setText(QString::number(v));
        pushChange();
    });
    connect(m_chorusMix, &QSlider::valueChanged, this, [this](int v){
        m_state.chorusMix = v / 100.0f;
        m_chorusMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Flanger
    connect(m_flangerEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.flangerEnabled = on;
        if (on && m_state.flangerMix < 0.01f) {
            m_state.flangerMix = 0.5f;
            m_flangerMix->setValue(50);
        }
        pushChange();
    });
    connect(m_flangerRate, &QSlider::valueChanged, this, [this](int v){
        m_state.flangerRate = v / 100.0f;
        m_flangerRateLabel->setText(QString::number(v / 100.0, 'f', 2) + " Hz");
        pushChange();
    });
    connect(m_flangerDepth, &QSlider::valueChanged, this, [this](int v){
        m_state.flangerDepth = v / 100.0f;
        m_flangerDepthLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_flangerFeedback, &QSlider::valueChanged, this, [this](int v){
        m_state.flangerFeedback = v / 100.0f;
        m_flangerFeedbackLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_flangerDelay, &QSlider::valueChanged, this, [this](int v){
        m_state.flangerBaseDelay = v / 10.0f;
        m_flangerDelayLabel->setText(QString::number(v / 10.0, 'f', 1) + " ms");
        pushChange();
    });
    connect(m_flangerMix, &QSlider::valueChanged, this, [this](int v){
        m_state.flangerMix = v / 100.0f;
        m_flangerMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Flangus
    connect(m_flangusEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.flangusEnabled = on;
        if (on && m_state.flangusMix < 0.01f) {
            m_state.flangusMix = 0.5f;
            m_flangusMix->setValue(50);
        }
        pushChange();
    });
    connect(m_flangusRate, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusRate = v / 10.0f;
        m_flangusRateLabel->setText(QString::number(v / 10.0, 'f', 1) + " Hz");
        pushChange();
    });
    connect(m_flangusDepth, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusDepth = v / 100.0f;
        m_flangusDepthLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_flangusFeedback, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusFeedback = v / 100.0f;
        m_flangusFeedbackLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_flangusVoices, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusVoices = v;
        m_flangusVoicesLabel->setText(QString::number(v));
        pushChange();
    });
    connect(m_flangusSpread, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusSpread = v / 100.0f;
        m_flangusSpreadLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_flangusMix, &QSlider::valueChanged, this, [this](int v){
        m_state.flangusMix = v / 100.0f;
        m_flangusMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Phaser
    connect(m_phaserEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.phaserEnabled = on;
        if (on && m_state.phaserMix < 0.01f) {
            m_state.phaserMix = 0.5f;
            m_phaserMix->setValue(50);
        }
        pushChange();
    });
    connect(m_phaserRate, &QSlider::valueChanged, this, [this](int v){
        m_state.phaserRate = v / 100.0f;
        m_phaserRateLabel->setText(QString::number(v / 100.0, 'f', 2) + " Hz");
        pushChange();
    });
    connect(m_phaserDepth, &QSlider::valueChanged, this, [this](int v){
        m_state.phaserDepth = v / 100.0f;
        m_phaserDepthLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_phaserFeedback, &QSlider::valueChanged, this, [this](int v){
        m_state.phaserFeedback = v / 100.0f;
        m_phaserFeedbackLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_phaserStages, &QSlider::valueChanged, this, [this](int v){
        m_state.phaserStages = v * 2;
        m_phaserStagesLabel->setText(QString::number(v * 2));
        pushChange();
    });
    connect(m_phaserMix, &QSlider::valueChanged, this, [this](int v){
        m_state.phaserMix = v / 100.0f;
        m_phaserMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Delay
    connect(m_delayEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.delayEnabled = on;
        if (on && m_state.delayMix < 0.01f) {
            m_state.delayMix = 0.5f;
            m_delayMix->setValue(50);
        }
        pushChange();
    });
    connect(m_delayTime, &QSlider::valueChanged, this, [this](int v){
        m_state.delayTimeMs = static_cast<float>(v);
        m_delayTimeLabel->setText(QString::number(v) + " ms");
        pushChange();
    });
    connect(m_delayFeedback, &QSlider::valueChanged, this, [this](int v){
        m_state.delayFeedback = v / 100.0f;
        m_delayFeedbackLabel->setText(QString::number(v) + "%");
        pushChange();
    });
    connect(m_delayDamping, &QSlider::valueChanged, this, [this](int v){
        m_state.delayDamping = static_cast<float>(v);
        m_delayDampingLabel->setText(QString::number(v) + " Hz");
        pushChange();
    });
    connect(m_delayPingPong, &QCheckBox::toggled, this, [this](bool on){
        m_state.delayPingPong = on; pushChange();
    });
    connect(m_delayMix, &QSlider::valueChanged, this, [this](int v){
        m_state.delayMix = v / 100.0f;
        m_delayMixLabel->setText(QString::number(v) + "%");
        pushChange();
    });

    // Limiter
    connect(m_limiterEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.limiterEnabled = on; pushChange();
    });
    connect(m_limiterModeBox, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx){
        m_state.limiterMode = idx; pushChange();
    });
    connect(m_limiterCeiling, &QSlider::valueChanged, this, [this](int v){
        m_state.limiterCeiling = v / 10.0f;
        m_limiterCeilingLabel->setText(QString::number(v / 10.0, 'f', 1) + " dB");
        pushChange();
    });
    connect(m_limiterLookahead, &QSlider::valueChanged, this, [this](int v){
        m_state.limiterLookahead = v / 10.0f;
        m_limiterLookaheadLabel->setText(QString::number(v / 10.0, 'f', 1) + " ms");
        pushChange();
    });
    connect(m_limiterRelease, &QSlider::valueChanged, this, [this](int v){
        m_state.limiterRelease = static_cast<float>(v);
        m_limiterReleaseLabel->setText(QString::number(v) + " ms");
        pushChange();
    });
    connect(m_limiterRatio, &QSlider::valueChanged, this, [this](int v){
        m_state.limiterRatio = v / 10.0f;
        m_limiterRatioLabel->setText(QString::number(v / 10.0, 'f', 1) + ":1");
        pushChange();
    });
    connect(m_limiterGate, &QSlider::valueChanged, this, [this](int v){
        m_state.limiterGateThresh = v / 10.0f;
        m_limiterGateLabel->setText(QString::number(v / 10.0, 'f', 1) + " dB");
        pushChange();
    });

    // Bitcrusher
    connect(m_bitcrushEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.bitcrusherEnabled = on; pushChange();
    });
    connect(m_bitcrushBits, &QSlider::valueChanged, this, [this](int v){
        m_state.bitcrusherBitDepth = v;
        m_bitcrushBitsLabel->setText(QString::number(v) + " bit");
        pushChange();
    });
    connect(m_bitcrushRate, &QSlider::valueChanged, this, [this](int v){
        m_state.bitcrusherRate = static_cast<float>(v);
        m_bitcrushRateLabel->setText(QString::number(v) + " Hz");
        pushChange();
    });
    connect(m_bitcrushPreset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx){
        if (m_loading) return;
        struct Preset { int bits; int rate; };
        const Preset presets[] = {
            {16, 48000},  // Custom (no change)
            {16, 48000},  // HiFi
            {12, 22050},  // Radio
            {8,  8000},   // Telephone
            {4,  11025},  // Retro
            {1,  500},    // Garbage
        };
        if (idx <= 0 || idx >= 6) return;
        m_state.bitcrusherBitDepth = presets[idx].bits;
        m_state.bitcrusherRate = static_cast<float>(presets[idx].rate);
        m_state.bitcrusherEnabled = true;
        pushStateToWidgets();
        pushChange();
    });

    // Mono
    connect(m_monoEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.monoEnabled = on; pushChange();
    });

    // Generation Loss
    if (m_randomEnable)
        connect(m_randomEnable, &QCheckBox::toggled, this, [this](bool on){
            m_state.randomEnabled = on; pushChange();
        });
    if (m_randomPitch)
        connect(m_randomPitch, qOverload<int>(&QSpinBox::valueChanged),
                this, [this](int v){ m_state.randomPitchCents = v; pushChange(); });
    if (m_duckEnable)
        connect(m_duckEnable, &QCheckBox::toggled, this, [this](bool on){
            m_state.duckSource = on; pushChange();
        });
    if (m_duckAmount)
        connect(m_duckAmount, &QSlider::valueChanged, this, [this](int v){
            m_state.duckOthersDb = static_cast<float>(v);
            if (m_duckAmountLabel)
                m_duckAmountLabel->setText(QString::number(v) + " dB");
            pushChange();
        });

    connect(m_genLossEnable, &QCheckBox::toggled, this, [this](bool on){
        m_state.genLossEnabled = on; pushChange();
    });
    connect(m_genLossGens, &QSlider::valueChanged, this, [this](int v){
        m_state.genLossGenerations = v;
        m_genLossGensLabel->setText(genLossDesc(v));
        pushChange();
    });

    // Per-module reset buttons
    connect(m_resetComp, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.compEnabled = d.compEnabled; m_state.compThresholdDb = d.compThresholdDb;
        m_state.compRatio = d.compRatio; m_state.compAttackMs = d.compAttackMs;
        m_state.compReleaseMs = d.compReleaseMs; m_state.compKneeDb = d.compKneeDb;
        m_state.compMakeupDb = d.compMakeupDb;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetSat, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.saturatorEnabled = d.saturatorEnabled; m_state.saturatorDrive = d.saturatorDrive;
        m_state.saturatorMix = d.saturatorMix; m_state.saturatorTone = d.saturatorTone;
        m_state.saturatorMode = d.saturatorMode;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetChorus, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.chorusEnabled = d.chorusEnabled; m_state.chorusRate = d.chorusRate;
        m_state.chorusDepth = d.chorusDepth; m_state.chorusBaseDelay = d.chorusBaseDelay;
        m_state.chorusVoices = d.chorusVoices; m_state.chorusMix = d.chorusMix;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetFlanger, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.flangerEnabled = d.flangerEnabled; m_state.flangerRate = d.flangerRate;
        m_state.flangerDepth = d.flangerDepth; m_state.flangerFeedback = d.flangerFeedback;
        m_state.flangerBaseDelay = d.flangerBaseDelay; m_state.flangerMix = d.flangerMix;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetFlangus, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.flangusEnabled = d.flangusEnabled; m_state.flangusRate = d.flangusRate;
        m_state.flangusDepth = d.flangusDepth; m_state.flangusFeedback = d.flangusFeedback;
        m_state.flangusVoices = d.flangusVoices; m_state.flangusSpread = d.flangusSpread;
        m_state.flangusMix = d.flangusMix;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetPhaser, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.phaserEnabled = d.phaserEnabled; m_state.phaserRate = d.phaserRate;
        m_state.phaserDepth = d.phaserDepth; m_state.phaserFeedback = d.phaserFeedback;
        m_state.phaserStages = d.phaserStages; m_state.phaserMix = d.phaserMix;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetDelay, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.delayEnabled = d.delayEnabled; m_state.delayTimeMs = d.delayTimeMs;
        m_state.delayFeedback = d.delayFeedback; m_state.delayMix = d.delayMix;
        m_state.delayDamping = d.delayDamping; m_state.delayPingPong = d.delayPingPong;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetLimiter, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.limiterEnabled = d.limiterEnabled; m_state.limiterMode = d.limiterMode;
        m_state.limiterCeiling = d.limiterCeiling; m_state.limiterLookahead = d.limiterLookahead;
        m_state.limiterRelease = d.limiterRelease; m_state.limiterRatio = d.limiterRatio;
        m_state.limiterGateThresh = d.limiterGateThresh;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetBitcrush, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.bitcrusherEnabled = d.bitcrusherEnabled;
        m_state.bitcrusherBitDepth = d.bitcrusherBitDepth;
        m_state.bitcrusherRate = d.bitcrusherRate;
        pushStateToWidgets(); pushChange();
    });
    connect(m_resetGenLoss, &QPushButton::clicked, this, [this]{
        SandboxState d;
        m_state.genLossEnabled = d.genLossEnabled;
        m_state.genLossGenerations = d.genLossGenerations;
        pushStateToWidgets(); pushChange();
    });

    // Pipeline order
    connect(m_pipeline, &PipelineWidget::orderChanged, this, [this]{
        if (m_loading) return;
        m_pipeline->getOrder(m_state.pipelineOrder);
        applyPipelineOrderToUi();
        pushChange();
    });
    connect(m_pipeline, &PipelineWidget::stageClicked, this,
            &ChannelSandboxDialog::onPipelineStageClicked);
    connect(m_resetOrderBtn, &QPushButton::clicked, this, [this]{
        SandboxState::defaultPipelineOrder(m_state.pipelineOrder);
        m_pipeline->setOrder(m_state.pipelineOrder);
        applyPipelineOrderToUi();
        pushChange();
    });
    connect(m_dspSearch, &QLineEdit::textChanged, this,
            &ChannelSandboxDialog::filterDspModules);
}

void ChannelSandboxDialog::pushStateToWidgets()
{
    m_loading = true;
    m_enable->setChecked(m_state.enabled);
    m_modeBox->setCurrentIndex(dropdownForMode(m_state.spatialMode));
    m_pan->setValue(static_cast<int>(m_state.panValue * 100.0f));
    m_panLabel->setText(fmtPan(m_pan->value()));
    m_pad->setPosition(m_state.posX, m_state.posY);
    m_padLabel->setText(QString("x=%1 y=%2")
                            .arg(m_state.posX, 0, 'f', 2)
                            .arg(m_state.posY, 0, 'f', 2));
    // Restore from whichever field the active mode reads; on legacy
    // INIs where only one was ever written, prefer the non-zero side.
    {
        float ev = (m_state.spatialMode == SandboxState::Spatial_3DRotate ||
                    m_state.spatialMode == SandboxState::Spatial_8DPreset)
                       ? m_state.rotateElev
                       : m_state.elev;
        if (std::fabs(ev) < 1e-4f &&
            std::fabs(m_state.elev) > std::fabs(m_state.rotateElev))
            ev = m_state.elev;
        m_elev->setValue(static_cast<int>(ev * 100.0f));
        m_elevLabel->setText(fmtElev(m_elev->value()));
    }
    m_dist->setValue(static_cast<int>(m_state.distanceM * 100.0f));
    m_distLabel->setText(QString::number(m_dist->value()) + " cm");
    m_width->setValue(static_cast<int>(m_state.stereoWidthDeg));
    m_widthLabel->setText(QString::number(m_width->value()) + " deg");
    m_rpm->setValue(static_cast<int>(m_state.rotateRpm));
    m_rpmLabel->setText(QString::number(m_rpm->value()) + " rpm");
    m_radius->setValue(static_cast<int>(m_state.rotateRadiusM * 100.0f));
    m_radiusLabel->setText(QString::number(m_radius->value()) + " cm");
    m_ccw->setChecked(m_state.rotateCcw);
    m_sway->setChecked(m_state.headSway);
    m_spatialMix->setValue(static_cast<int>(m_state.spatialMix * 100.0f));
    m_spatialMixLabel->setText(QString::number(m_spatialMix->value()) + "%");
    m_ambience->setValue(static_cast<int>(m_state.reverbWet * 100.0f));
    m_ambienceLabel->setText(QString::number(m_ambience->value()) + "%");
    if (m_engineBox) {
        // The combo is hidden — the user can only change the engine
        // default from Settings → Channels. Block signals so the index
        // sync below does not call back into onEngineChanged() and
        // accidentally overwrite the QSettings preferred-engine seed
        // with the per-cell engine of the channel being opened.
        QSignalBlocker blocker(m_engineBox);
        m_engineBox->setCurrentIndex(
            m_state.spatialEngine == SandboxState::Engine_Leia ? 1 : 0);
    }
    if (m_leiaRefl) m_leiaRefl->setChecked(m_state.leiaReflEnable);
    if (m_leiaReflLevel) {
        m_leiaReflLevel->setValue(static_cast<int>(m_state.leiaReflLevel));
        m_leiaReflLevelLabel->setText(
            QString::number(m_leiaReflLevel->value()) + " dB");
    }
    if (m_leiaRoomSize) {
        m_leiaRoomSize->setValue(static_cast<int>(m_state.leiaRoomSize));
        m_leiaRoomSizeLabel->setText(
            QString::number(m_leiaRoomSize->value()) + " m");
    }
    if (m_leiaRoomType) m_leiaRoomType->setCurrentIndex(m_state.leiaRoomType);
    if (m_leiaClarity) {
        m_leiaClarity->setValue(static_cast<int>(m_state.leiaClarity));
        m_leiaClarityLabel->setText(
            QString::number(m_leiaClarity->value()) + "%");
    }
    if (m_leiaWidth) {
        m_leiaWidth->setValue(static_cast<int>(m_state.leiaWidth));
        m_leiaWidthLabel->setText(QString::number(m_leiaWidth->value()) + "%");
    }
    m_stretchEn->setChecked(m_state.stretchEnabled);
    m_stretchFac->setValue(static_cast<int>(m_state.stretchFactor * 10.0f));
    m_stretchFacLabel->setText(QString::number(m_state.stretchFactor, 'f', 1) + "x");
    m_stretchWin->setValue(static_cast<int>(m_state.stretchWindowMs));
    m_stretchWinLabel->setText(QString::number(m_stretchWin->value()) + " ms");
    m_eqEnable->setChecked(m_state.eqEnabled);
    for (int i = 0; i < 16 && i < m_eqSliders.size(); ++i) {
        m_eqSliders[i]->setValue(static_cast<int>(m_state.eqBandDb[i]));
        m_eqLabels[i]->setText(QString::number(static_cast<int>(m_state.eqBandDb[i])));
    }
    // New effects
    if (m_compEnable) m_compEnable->setChecked(m_state.compEnabled);
    if (m_compThreshold) { m_compThreshold->setValue(static_cast<int>(m_state.compThresholdDb * 10)); m_compThresholdLabel->setText(QString::number(m_state.compThresholdDb, 'f', 1) + " dB"); }
    if (m_compRatio) { m_compRatio->setValue(static_cast<int>(m_state.compRatio * 10)); m_compRatioLabel->setText(QString::number(m_state.compRatio, 'f', 1) + ":1"); }
    if (m_compAttack) { m_compAttack->setValue(static_cast<int>(m_state.compAttackMs * 10)); m_compAttackLabel->setText(QString::number(m_state.compAttackMs, 'f', 1) + " ms"); }
    if (m_compRelease) { m_compRelease->setValue(static_cast<int>(m_state.compReleaseMs)); m_compReleaseLabel->setText(QString::number(static_cast<int>(m_state.compReleaseMs)) + " ms"); }
    if (m_compKnee) { m_compKnee->setValue(static_cast<int>(m_state.compKneeDb * 10)); m_compKneeLabel->setText(QString::number(m_state.compKneeDb, 'f', 1) + " dB"); }
    if (m_compMakeup) { m_compMakeup->setValue(static_cast<int>(m_state.compMakeupDb * 10)); m_compMakeupLabel->setText(QString::number(m_state.compMakeupDb, 'f', 1) + " dB"); }

    if (m_satEnable) m_satEnable->setChecked(m_state.saturatorEnabled);
    if (m_satMode) m_satMode->setCurrentIndex(m_state.saturatorMode);
    if (m_satDrive) { m_satDrive->setValue(static_cast<int>(m_state.saturatorDrive * 10)); m_satDriveLabel->setText(QString::number(m_state.saturatorDrive, 'f', 1) + "x"); }
    if (m_satTone) { m_satTone->setValue(static_cast<int>(m_state.saturatorTone)); m_satToneLabel->setText(QString::number(static_cast<int>(m_state.saturatorTone)) + " Hz"); }
    if (m_satMix) { m_satMix->setValue(static_cast<int>(m_state.saturatorMix * 100)); m_satMixLabel->setText(QString::number(static_cast<int>(m_state.saturatorMix * 100)) + "%"); }

    if (m_chorusEnable) m_chorusEnable->setChecked(m_state.chorusEnabled);
    if (m_chorusRate) { m_chorusRate->setValue(static_cast<int>(m_state.chorusRate * 10)); m_chorusRateLabel->setText(QString::number(m_state.chorusRate, 'f', 1) + " Hz"); }
    if (m_chorusDepth) { m_chorusDepth->setValue(static_cast<int>(m_state.chorusDepth * 10)); m_chorusDepthLabel->setText(QString::number(m_state.chorusDepth, 'f', 1) + " ms"); }
    if (m_chorusDelay) { m_chorusDelay->setValue(static_cast<int>(m_state.chorusBaseDelay)); m_chorusDelayLabel->setText(QString::number(static_cast<int>(m_state.chorusBaseDelay)) + " ms"); }
    if (m_chorusVoices) { m_chorusVoices->setValue(m_state.chorusVoices); m_chorusVoicesLabel->setText(QString::number(m_state.chorusVoices)); }
    if (m_chorusMix) { m_chorusMix->setValue(static_cast<int>(m_state.chorusMix * 100)); m_chorusMixLabel->setText(QString::number(static_cast<int>(m_state.chorusMix * 100)) + "%"); }

    if (m_flangerEnable) m_flangerEnable->setChecked(m_state.flangerEnabled);
    if (m_flangerRate) { m_flangerRate->setValue(static_cast<int>(m_state.flangerRate * 100)); m_flangerRateLabel->setText(QString::number(m_state.flangerRate, 'f', 2) + " Hz"); }
    if (m_flangerDepth) { m_flangerDepth->setValue(static_cast<int>(m_state.flangerDepth * 100)); m_flangerDepthLabel->setText(QString::number(static_cast<int>(m_state.flangerDepth * 100)) + "%"); }
    if (m_flangerFeedback) { m_flangerFeedback->setValue(static_cast<int>(m_state.flangerFeedback * 100)); m_flangerFeedbackLabel->setText(QString::number(static_cast<int>(m_state.flangerFeedback * 100)) + "%"); }
    if (m_flangerDelay) { m_flangerDelay->setValue(static_cast<int>(m_state.flangerBaseDelay * 10)); m_flangerDelayLabel->setText(QString::number(m_state.flangerBaseDelay, 'f', 1) + " ms"); }
    if (m_flangerMix) { m_flangerMix->setValue(static_cast<int>(m_state.flangerMix * 100)); m_flangerMixLabel->setText(QString::number(static_cast<int>(m_state.flangerMix * 100)) + "%"); }

    if (m_flangusEnable) m_flangusEnable->setChecked(m_state.flangusEnabled);
    if (m_flangusRate) { m_flangusRate->setValue(static_cast<int>(m_state.flangusRate * 10)); m_flangusRateLabel->setText(QString::number(m_state.flangusRate, 'f', 1) + " Hz"); }
    if (m_flangusDepth) { m_flangusDepth->setValue(static_cast<int>(m_state.flangusDepth * 100)); m_flangusDepthLabel->setText(QString::number(static_cast<int>(m_state.flangusDepth * 100)) + "%"); }
    if (m_flangusFeedback) { m_flangusFeedback->setValue(static_cast<int>(m_state.flangusFeedback * 100)); m_flangusFeedbackLabel->setText(QString::number(static_cast<int>(m_state.flangusFeedback * 100)) + "%"); }
    if (m_flangusVoices) { m_flangusVoices->setValue(m_state.flangusVoices); m_flangusVoicesLabel->setText(QString::number(m_state.flangusVoices)); }
    if (m_flangusSpread) { m_flangusSpread->setValue(static_cast<int>(m_state.flangusSpread * 100)); m_flangusSpreadLabel->setText(QString::number(static_cast<int>(m_state.flangusSpread * 100)) + "%"); }
    if (m_flangusMix) { m_flangusMix->setValue(static_cast<int>(m_state.flangusMix * 100)); m_flangusMixLabel->setText(QString::number(static_cast<int>(m_state.flangusMix * 100)) + "%"); }

    if (m_phaserEnable) m_phaserEnable->setChecked(m_state.phaserEnabled);
    if (m_phaserRate) { m_phaserRate->setValue(static_cast<int>(m_state.phaserRate * 100)); m_phaserRateLabel->setText(QString::number(m_state.phaserRate, 'f', 2) + " Hz"); }
    if (m_phaserDepth) { m_phaserDepth->setValue(static_cast<int>(m_state.phaserDepth * 100)); m_phaserDepthLabel->setText(QString::number(static_cast<int>(m_state.phaserDepth * 100)) + "%"); }
    if (m_phaserFeedback) { m_phaserFeedback->setValue(static_cast<int>(m_state.phaserFeedback * 100)); m_phaserFeedbackLabel->setText(QString::number(static_cast<int>(m_state.phaserFeedback * 100)) + "%"); }
    if (m_phaserStages) { m_phaserStages->setValue(m_state.phaserStages / 2); m_phaserStagesLabel->setText(QString::number(m_state.phaserStages)); }
    if (m_phaserMix) { m_phaserMix->setValue(static_cast<int>(m_state.phaserMix * 100)); m_phaserMixLabel->setText(QString::number(static_cast<int>(m_state.phaserMix * 100)) + "%"); }

    if (m_delayEnable) m_delayEnable->setChecked(m_state.delayEnabled);
    if (m_delayTime) { m_delayTime->setValue(static_cast<int>(m_state.delayTimeMs)); m_delayTimeLabel->setText(QString::number(static_cast<int>(m_state.delayTimeMs)) + " ms"); }
    if (m_delayFeedback) { m_delayFeedback->setValue(static_cast<int>(m_state.delayFeedback * 100)); m_delayFeedbackLabel->setText(QString::number(static_cast<int>(m_state.delayFeedback * 100)) + "%"); }
    if (m_delayDamping) { m_delayDamping->setValue(static_cast<int>(m_state.delayDamping)); m_delayDampingLabel->setText(QString::number(static_cast<int>(m_state.delayDamping)) + " Hz"); }
    if (m_delayPingPong) m_delayPingPong->setChecked(m_state.delayPingPong);
    if (m_delayMix) { m_delayMix->setValue(static_cast<int>(m_state.delayMix * 100)); m_delayMixLabel->setText(QString::number(static_cast<int>(m_state.delayMix * 100)) + "%"); }

    if (m_limiterEnable) m_limiterEnable->setChecked(m_state.limiterEnabled);
    if (m_limiterModeBox) m_limiterModeBox->setCurrentIndex(m_state.limiterMode);
    if (m_limiterCeiling) { m_limiterCeiling->setValue(static_cast<int>(m_state.limiterCeiling * 10)); m_limiterCeilingLabel->setText(QString::number(m_state.limiterCeiling, 'f', 1) + " dB"); }
    if (m_limiterLookahead) { m_limiterLookahead->setValue(static_cast<int>(m_state.limiterLookahead * 10)); m_limiterLookaheadLabel->setText(QString::number(m_state.limiterLookahead, 'f', 1) + " ms"); }
    if (m_limiterRelease) { m_limiterRelease->setValue(static_cast<int>(m_state.limiterRelease)); m_limiterReleaseLabel->setText(QString::number(static_cast<int>(m_state.limiterRelease)) + " ms"); }
    if (m_limiterRatio) { m_limiterRatio->setValue(static_cast<int>(m_state.limiterRatio * 10)); m_limiterRatioLabel->setText(QString::number(m_state.limiterRatio, 'f', 1) + ":1"); }
    if (m_limiterGate) { m_limiterGate->setValue(static_cast<int>(m_state.limiterGateThresh * 10)); m_limiterGateLabel->setText(QString::number(m_state.limiterGateThresh, 'f', 1) + " dB"); }

    if (m_bitcrushEnable) m_bitcrushEnable->setChecked(m_state.bitcrusherEnabled);
    if (m_bitcrushBits) { m_bitcrushBits->setValue(m_state.bitcrusherBitDepth); m_bitcrushBitsLabel->setText(QString::number(m_state.bitcrusherBitDepth) + " bit"); }
    if (m_bitcrushRate) { m_bitcrushRate->setValue(static_cast<int>(m_state.bitcrusherRate)); m_bitcrushRateLabel->setText(QString::number(static_cast<int>(m_state.bitcrusherRate)) + " Hz"); }

    if (m_monoEnable) m_monoEnable->setChecked(m_state.monoEnabled);

    if (m_genLossEnable) m_genLossEnable->setChecked(m_state.genLossEnabled);
    if (m_randomEnable) m_randomEnable->setChecked(m_state.randomEnabled);
    if (m_randomPitch)  m_randomPitch->setValue(m_state.randomPitchCents);
    if (m_duckEnable)   m_duckEnable->setChecked(m_state.duckSource);
    if (m_duckAmount)   m_duckAmount->setValue(static_cast<int>(m_state.duckOthersDb));
    if (m_duckAmountLabel)
        m_duckAmountLabel->setText(QString::number(static_cast<int>(m_state.duckOthersDb)) + " dB");
    if (m_genLossGens) {
        m_genLossGens->setValue(m_state.genLossGenerations);
        m_genLossGensLabel->setText(genLossDesc(m_state.genLossGenerations));
    }

    if (m_bitcrushPreset) { QSignalBlocker blk(m_bitcrushPreset); m_bitcrushPreset->setCurrentIndex(0); }

    if (m_pipeline) m_pipeline->setOrder(m_state.pipelineOrder);
    applyPipelineOrderToUi();

    // Apply gating: master switch first, then sub-checkboxes.
    bool master = m_state.enabled;
    if (m_modeBox)     m_modeBox->setEnabled(master);
    if (m_panGroup)    m_panGroup->setEnabled(master);
    if (m_hrtfGroup)   m_hrtfGroup->setEnabled(master);
    if (m_leiaGroup)   m_leiaGroup->setEnabled(master);
    if (m_eqEnable)    m_eqEnable->setEnabled(master);
    for (auto *s : m_eqSliders) if (s) s->setEnabled(master && m_state.eqEnabled);
    if (m_stretchEn)   m_stretchEn->setEnabled(master);
    if (m_stretchFac)  m_stretchFac->setEnabled(master && m_state.stretchEnabled);
    if (m_stretchWin)  m_stretchWin->setEnabled(master && m_state.stretchEnabled);
    if (m_dspGroup)    m_dspGroup->setEnabled(master);
    m_loading = false;
}

void ChannelSandboxDialog::applyModeVisibility()
{
    int m = m_state.spatialMode;
    bool isPan    = (m == SandboxState::Spatial_LRPan);
    bool isManual = (m == SandboxState::Spatial_3DManual);
    bool isRotate = (m == SandboxState::Spatial_3DRotate);
    bool is8D     = (m == SandboxState::Spatial_8DPreset);
    bool any3D    = isManual || isRotate || is8D;

    // Leia is a measured-HRTF engine: it has no virtual-speaker pair
    // (no stereo-width angle) and resolves front/back from the data
    // itself (no head-sway trick), so those Classic-only rows are
    // hidden when Leia is the active engine. Its room / tone controls
    // appear in their own group instead.
    bool leia = (m_state.spatialEngine == SandboxState::Engine_Leia);

    m_panGroup->setVisible(isPan);
    m_hrtfGroup->setVisible(any3D);
    if (m_leiaGroup) m_leiaGroup->setVisible(any3D && leia);

    // Inside the 3D group: pad + dist visible only in Manual; rpm +
    // radius + ccw visible in Rotate / 8D. Elevation / Spatial mix /
    // Ambience always visible in any 3D mode.
    if (m_padContainer) m_padContainer->setVisible(isManual);
    if (m_distRow)      m_distRow->setVisible(isManual && !leia);
    if (m_widthRow)     m_widthRow->setVisible(any3D && !leia);
    if (m_sway)         m_sway->setVisible(any3D && !leia);
    if (m_rpmRow)       m_rpmRow->setVisible(isRotate || is8D);
    if (m_radiusRow)    m_radiusRow->setVisible(isRotate || is8D);
    if (m_ccw)          m_ccw->setVisible(isRotate || is8D);
}

void ChannelSandboxDialog::onEngineChanged(int idx)
{
    int engine = (idx == 1) ? SandboxState::Engine_Leia
                            : SandboxState::Engine_Classic;
    m_state.spatialEngine = engine;
    // Remember the choice so the next channel the user creates picks
    // it up by default. Per-channel persistence (when enabled) still
    // overrides this on restore - the QSettings key is only the seed
    // for fresh sandboxes that have no saved state of their own.
    SandboxEnginePref::save(engine);
    applyModeVisibility();
    pushChange();
}

void ChannelSandboxDialog::setCpuPercent(double pct)
{
    if (!m_cpuLabel) return;
    if (pct < 0.05) {
        m_cpuLabel->setText(tr("CPU <0.1%"));
    } else {
        m_cpuLabel->setText(tr("CPU %1%").arg(pct, 0, 'f', 1));
    }
}

void ChannelSandboxDialog::pushAudioLevel(float peakL, float peakR)
{
    // Broadband fallback. The proper per-band animation now comes from
    // pushEqBandLevels (FFT-driven); this stays as a safety net for
    // slots whose DSP block has not been built yet.
    (void)peakL; (void)peakR;
}

void ChannelSandboxDialog::pushEqBandLevels(const float bands[16])
{
    if (!isVisible()) return;
    for (int i = 0; i < 16 && i < m_eqSliders.size(); ++i)
        if (m_eqSliders[i]) m_eqSliders[i]->setLevel(bands[i]);
}

void ChannelSandboxDialog::onCopySandboxJsonDebug()
{
    QJsonDocument doc(m_state.toJson());
    QApplication::clipboard()->setText(
        QString::fromUtf8(doc.toJson(QJsonDocument::Indented)));
}

void ChannelSandboxDialog::onEnableToggled(bool on)
{
    m_state.enabled = on;
    // Master switch first, sub-toggles after. When master is OFF every
    // child group + checkbox + slider goes disabled so the user can't
    // touch knobs that have no effect, and the gating order is visible.
    if (m_modeBox)     m_modeBox->setEnabled(on);
    if (m_panGroup)    m_panGroup->setEnabled(on);
    if (m_hrtfGroup)   m_hrtfGroup->setEnabled(on);
    if (m_leiaGroup)   m_leiaGroup->setEnabled(on);
    if (m_eqEnable)    m_eqEnable->setEnabled(on);
    for (auto *s : m_eqSliders) if (s) s->setEnabled(on && m_state.eqEnabled);
    if (m_stretchEn)   m_stretchEn->setEnabled(on);
    if (m_stretchFac)  m_stretchFac->setEnabled(on && m_state.stretchEnabled);
    if (m_stretchWin)  m_stretchWin->setEnabled(on && m_state.stretchEnabled);
    if (m_dspGroup)    m_dspGroup->setEnabled(on);
    pushChange();
}

void ChannelSandboxDialog::onModeChanged(int idx)
{
    int newMode = modeForDropdown(idx);
    bool entering8D = (newMode == SandboxState::Spatial_8DPreset &&
                       m_state.spatialMode != SandboxState::Spatial_8DPreset);
    m_state.spatialMode = newMode;
    if (entering8D) {
        load8DPreset();
        pushStateToWidgets();
    }
    applyModeVisibility();
    pushChange();
}

void ChannelSandboxDialog::load8DPreset()
{
    // Classic YouTube 8D recipe: orbit a single tighter source around
    // the head with the head-sway cue on. Reverb stays out of the
    // sandbox - the existing FxPanel reverb covers that role.
    m_state.enabled        = true;
    m_state.rotateRpm      = 10.0f;
    m_state.rotateRadiusM  = 2.0f;
    m_state.rotateCcw      = false;
    m_state.elev           = 0.0f;
    m_state.headSway       = true;
    m_state.stereoWidthDeg = 0.0f;
    // Reverb wet 12% mirrors audio_sandbox standalone's 8D preset.
    // Without this, the rotating source stays "in the head" - the
    // listener hears the orbit but not the room. Adding the ambience
    // is what closes the gap to the standalone reference.
    m_state.reverbWet      = 0.12f;
    // Spatial mix slightly under 100 keeps original stereo content
    // bleeding through, again matching standalone behaviour.
    m_state.spatialMix     = 0.85f;

    // Leia-engine 8D recipe: image-source reflections give the orbit a
    // believable room so the preset sounds right whichever engine the
    // user has selected. The engine choice itself is left untouched.
    m_state.leiaReflEnable = true;
    m_state.leiaRoomType   = 1;       // Studio
    m_state.leiaRoomSize   = 14.0f;
    m_state.leiaReflLevel  = -4.0f;
    m_state.leiaClarity    = 100.0f;
    m_state.leiaWidth      = 45.0f;
}

void ChannelSandboxDialog::onPanChanged(int v) {
    m_state.panValue = v / 100.0f;
    m_panLabel->setText(fmtPan(v));
    pushChange();
}
void ChannelSandboxDialog::onPadMoved(float x, float y) {
    m_state.posX = x;
    m_state.posY = y;
    m_padLabel->setText(QString("x=%1 y=%2")
                            .arg(x, 0, 'f', 2).arg(y, 0, 'f', 2));
    pushChange();
}
void ChannelSandboxDialog::onElevChanged(int v) {
    // One slider drives every 3D mode: 3DManual reads m_state.elev
    // (posZ), 3DRotate / 8D read m_state.rotateElev (orbit tilt).
    float f = v / 100.0f;
    m_state.elev = f;
    m_state.rotateElev = f;
    m_elevLabel->setText(fmtElev(v));
    pushChange();
}
void ChannelSandboxDialog::onDistChanged(int v) {
    m_state.distanceM = v / 100.0f;
    m_distLabel->setText(QString::number(v) + " cm");
    pushChange();
}
void ChannelSandboxDialog::onWidthChanged(int v) {
    m_state.stereoWidthDeg = static_cast<float>(v);
    m_widthLabel->setText(QString::number(v) + " deg");
    pushChange();
}
void ChannelSandboxDialog::onRpmChanged(int v) {
    m_state.rotateRpm = static_cast<float>(v);
    m_rpmLabel->setText(QString::number(v) + " rpm");
    pushChange();
}
void ChannelSandboxDialog::onRadiusChanged(int v) {
    m_state.rotateRadiusM = v / 100.0f;
    m_radiusLabel->setText(QString::number(v) + " cm");
    pushChange();
}
void ChannelSandboxDialog::onCcwToggled(bool on) {
    m_state.rotateCcw = on;
    pushChange();
}
void ChannelSandboxDialog::onSwayToggled(bool on) {
    m_state.headSway = on;
    pushChange();
}
void ChannelSandboxDialog::onSpatialMixChanged(int v) {
    m_state.spatialMix = v / 100.0f;
    m_spatialMixLabel->setText(QString::number(v) + "%");
    pushChange();
}
void ChannelSandboxDialog::onAmbienceChanged(int v) {
    m_state.reverbWet = v / 100.0f;
    m_ambienceLabel->setText(QString::number(v) + "%");
    pushChange();
}
void ChannelSandboxDialog::onStretchToggled(bool on) {
    m_state.stretchEnabled = on;
    // Stretch checkbox = secondary gate. Factor + Window sliders only
    // respond when master + this checkbox both on.
    bool useable = m_state.enabled && on;
    if (m_stretchFac) m_stretchFac->setEnabled(useable);
    if (m_stretchWin) m_stretchWin->setEnabled(useable);
    pushChange();
}
void ChannelSandboxDialog::onStretchFactorChanged(int v) {
    m_state.stretchFactor = v / 10.0f;
    m_stretchFacLabel->setText(QString::number(m_state.stretchFactor, 'f', 1) + "x");
    pushChange();
}
void ChannelSandboxDialog::onStretchWindowChanged(int v) {
    m_state.stretchWindowMs = static_cast<float>(v);
    m_stretchWinLabel->setText(QString::number(v) + " ms");
    pushChange();
}

void ChannelSandboxDialog::onEqEnabledToggled(bool on) {
    m_state.eqEnabled = on;
    // EQ checkbox = secondary gate. Sliders only respond when both
    // master "Enable audio sandbox" AND this checkbox are on.
    bool useable = m_state.enabled && on;
    for (auto *s : m_eqSliders) if (s) s->setEnabled(useable);
    pushChange();
}
void ChannelSandboxDialog::onResetClicked()
{
    m_state = SandboxState();
    pushStateToWidgets();
    applyModeVisibility();
    emit resetRequested(m_channelId);
    pushChange();
}

void ChannelSandboxDialog::onCopyEq()
{
    QString s = "GBSB4-EQ";
    for (int i = 0; i < 16; ++i)
        s += "#" + QString::number(static_cast<double>(m_state.eqBandDb[i]), 'f', 1);
    QApplication::clipboard()->setText(s);
}

void ChannelSandboxDialog::onPasteEq()
{
    QString s = QApplication::clipboard()->text().trimmed();
    if (!s.startsWith("GBSB4-EQ#")) {
        QMessageBox::warning(this, tr("Paste EQ"), tr("Invalid EQ preset string."));
        return;
    }
    QStringList parts = s.mid(9).split('#');
    if (parts.size() < 16) {
        QMessageBox::warning(this, tr("Paste EQ"), tr("EQ string must have 16 values."));
        return;
    }
    for (int i = 0; i < 16; ++i)
        m_state.eqBandDb[i] = parts[i].toFloat();
    pushStateToWidgets();
    pushChange();
}

void ChannelSandboxDialog::onCopySandbox()
{
    QJsonDocument doc(m_state.toJson());
    QString encoded = doc.toJson(QJsonDocument::Compact).toBase64();
    QApplication::clipboard()->setText("GBSB4-SBX:" + encoded);
}

void ChannelSandboxDialog::onPasteSandbox()
{
    QString s = QApplication::clipboard()->text().trimmed();
    if (!s.startsWith("GBSB4-SBX:")) {
        QMessageBox::warning(this, tr("Paste Sandbox"), tr("Invalid sandbox preset string."));
        return;
    }
    QByteArray decoded = QByteArray::fromBase64(s.mid(10).toUtf8());
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(decoded, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        QMessageBox::warning(this, tr("Paste Sandbox"), tr("Failed to parse sandbox preset."));
        return;
    }
    m_state = SandboxState::fromJson(doc.object());
    pushStateToWidgets();
    applyModeVisibility();
    pushChange();
}

void ChannelSandboxDialog::setAllControlsEnabled(bool on)
{
    QList<QWidget*> kids = findChildren<QWidget*>();
    for (auto *w : kids) {
        if (w != m_enable) w->setEnabled(on);
    }
}

void ChannelSandboxDialog::applyPipelineOrderToUi()
{
    if (!m_dspScrollLay) return;
    // Pull every managed panel out of the layout (without deleting it),
    // then re-insert in pipeline order. Stages with no panel (EQ,
    // Spatial, Reverb) are skipped. The trailing stretch item stays last.
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i)
        if (m_stageSection[i]) m_dspScrollLay->removeWidget(m_stageSection[i]);
    int pos = 0;
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i) {
        int stage = m_state.pipelineOrder[i];
        if (stage < 0 || stage >= SandboxState::Stage_COUNT) continue;
        if (m_stageSection[stage])
            m_dspScrollLay->insertWidget(pos++, m_stageSection[stage]);
    }
}

void ChannelSandboxDialog::onPipelineStageClicked(int stage)
{
    if (stage < 0 || stage >= SandboxState::Stage_COUNT) return;
    if (ExpandableSection *sec = m_stageSection[stage]) {
        sec->setVisible(true);          // un-hide if a search filter hid it
        sec->setExpanded(true);
        if (m_dspScrollArea) m_dspScrollArea->ensureWidgetVisible(sec);
        return;
    }
    // EQ / Spatial / Reverb live in the left column of the outer scroll.
    QWidget *target = (stage == SandboxState::Stage_EQ) ? m_eqBox
                                                        : m_hrtfGroup;
    if (!target) return;
    if (m_outerScroll) m_outerScroll->ensureWidgetVisible(target);
    // The 3D HRTF controls sit inside their own fixed-size spatial
    // panel - scroll that one too so the target actually shows.
    if (target == m_hrtfGroup && m_spatialScroll)
        m_spatialScroll->ensureWidgetVisible(target);
}

void ChannelSandboxDialog::filterDspModules(const QString &text)
{
    QString needle = text.trimmed();
    for (int i = 0; i < SandboxState::Stage_COUNT; ++i) {
        ExpandableSection *sec = m_stageSection[i];
        if (!sec) continue;
        bool match = needle.isEmpty() ||
            QString::fromLatin1(SandboxState::stageName(i))
                .contains(needle, Qt::CaseInsensitive);
        sec->setVisible(match);
    }
}
