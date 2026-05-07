#include "channel_sandbox_dialog.h"
#include "positional_pad.h"
#include "help_bubble.h"
#include "../dsp/EqRack.h"

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
#include <cmath>

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
    setProperty("isGBSoundboard", true);
    setModal(false);
    refreshTitle();
    resize(960, 600);
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

    // ===== Top row: enable + mode + close =====
    auto *topRow = new QHBoxLayout;
    m_enable = new QCheckBox(tr("Enable audio sandbox on this channel"), this);
    m_enable->setToolTip(tr(
        "Master per-channel switch. When OFF, the entire DSP chain is\n"
        "bypassed (zero CPU cost) but every value below is preserved\n"
        "so re-enabling brings everything back exactly as set."));
    topRow->addWidget(m_enable);
    topRow->addWidget(new HelpBubble(tr(
        "Master switch. When OFF the entire DSP chain is bypassed and\n"
        "consumes zero CPU. Every parameter below is preserved on disk\n"
        "so you can re-enable later and pick up exactly where you left."), this));
    topRow->addSpacing(20);
    topRow->addWidget(new QLabel(tr("Mode:"), this));
    m_modeBox = new QComboBox(this);
    for (const auto &m : kModes) m_modeBox->addItem(QObject::tr(m.label));
    m_modeBox->setMinimumWidth(220);
    topRow->addWidget(m_modeBox);
    topRow->addWidget(new HelpBubble(tr(
        "Spatial mode:\n"
        "  - Off: pure passthrough.\n"
        "  - L/R Pan: equal-power balance (cheap, no HRTF).\n"
        "  - 3D Manual: place a virtual source on the pad and slide\n"
        "    elevation / distance / stereo width.\n"
        "  - 3D Rotate: source orbits around your head at the chosen\n"
        "    RPM and radius.\n"
        "  - 8D preset: orbit + head sway + tight width, the classic\n"
        "    'YouTube 8D remix' recipe."), this));
    topRow->addStretch(1);
    root->addLayout(topRow);

    // ===== Mid row: Spatial column | EQ column =====
    auto *midRow = new QHBoxLayout;

    // ---- Spatial column ----
    auto *spatialCol = new QVBoxLayout;

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

        row = buildSliderRow(m_hrtfGroup, tr("Stereo width"),
                              0, 120, 60, " deg", s, lbl);
        m_width = s; m_widthLabel = lbl;
        l->addWidget(row);

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

    midRow->addLayout(spatialCol, 2);

    // ---- EQ + Paulstretch column ----
    auto *fxCol = new QVBoxLayout;

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
    eqHeader->addStretch(1);
    eqOuter->addLayout(eqHeader);

    auto *eqGrid = new QWidget(eqBox);
    auto *eqLay = new QGridLayout(eqGrid);
    eqLay->setHorizontalSpacing(2);
    eqLay->setVerticalSpacing(3);
    eqOuter->addWidget(eqGrid, 1);
    for (int i = 0; i < EqRack::kNumBands; ++i) {
        auto *s = new QSlider(Qt::Vertical, eqGrid);
        s->setRange(-12, 12);
        s->setValue(0);
        s->setMinimumHeight(80);
        s->setMaximumWidth(20);
        m_eqSliders.append(s);
        auto *freq = new QLabel(fmtFreq(EqRack::bandFrequency(i)), eqGrid);
        freq->setAlignment(Qt::AlignHCenter);
        freq->setStyleSheet("font-size: 10px;");
        auto *val = new QLabel("0", eqGrid);
        val->setAlignment(Qt::AlignHCenter);
        val->setStyleSheet("font-size: 10px;");
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
    fxCol->addWidget(eqBox, 1);

    auto *stretchBox = new QGroupBox(tr("Paulstretch (extreme time-stretch)"), this);
    auto *stretchLay = new QFormLayout(stretchBox);
    m_stretchEn = new QCheckBox(tr("Enable"), stretchBox);
    m_stretchEn->setToolTip(tr(
        "Paul Nasca's phase-randomisation time-stretch. Source plays back\n"
        "much slower (1x..50x) while keeping pitch. Stereo channels get\n"
        "independent random phases for a wide diffuse texture."));
    auto *stretchHeaderRow = new QHBoxLayout;
    stretchHeaderRow->addWidget(m_stretchEn);
    stretchHeaderRow->addWidget(new HelpBubble(tr(
        "Paulstretch is a LOCAL effect: only you hear the stretched\n"
        "audio - the server gets a clean mic stream while the effect\n"
        "is on, exactly like preview-only mode. Streams up to ~30 s\n"
        "of source material so very long clips wrap around."), stretchBox));
    stretchHeaderRow->addStretch(1);
    stretchLay->addRow(stretchHeaderRow);
    {
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *row = buildSliderRow(stretchBox, tr("Factor"),
                                   10, 500, 40, "", s, lbl);
        m_stretchFac = s; m_stretchFacLabel = lbl;
        m_stretchFacLabel->setText("4.0x");
        stretchLay->addRow(row);
    }
    {
        QSlider *s = nullptr; QLabel *lbl = nullptr;
        auto *row = buildSliderRow(stretchBox, tr("Window"),
                                   50, 1000, 180, " ms", s, lbl);
        m_stretchWin = s; m_stretchWinLabel = lbl;
        m_stretchWin->setToolTip(tr(
            "FFT analysis window in milliseconds. Bigger = smoother /\n"
            "more 'frozen-in-amber' drone, blurred high frequencies.\n"
            "Smaller = grittier with more transient detail."));
        stretchLay->addRow(row);
    }
    fxCol->addWidget(stretchBox);

    midRow->addLayout(fxCol, 3);
    root->addLayout(midRow, 1);

    // ===== Bottom: reset + close =====
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
    connect(m_stretchEn,  &QCheckBox::toggled,    this, &ChannelSandboxDialog::onStretchToggled);
    connect(m_stretchFac, &QSlider::valueChanged, this, &ChannelSandboxDialog::onStretchFactorChanged);
    connect(m_stretchWin, &QSlider::valueChanged, this, &ChannelSandboxDialog::onStretchWindowChanged);
    connect(m_eqEnable,&QCheckBox::toggled,    this, &ChannelSandboxDialog::onEqEnabledToggled);
    connect(m_resetBtn,&QPushButton::clicked,  this, &ChannelSandboxDialog::onResetClicked);
    connect(m_closeBtn,&QPushButton::clicked,  this, &QDialog::accept);
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
    m_elev->setValue(static_cast<int>(m_state.elev * 100.0f));
    m_elevLabel->setText(fmtElev(m_elev->value()));
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
    // Apply gating: master switch first, then sub-checkboxes.
    bool master = m_state.enabled;
    if (m_modeBox)     m_modeBox->setEnabled(master);
    if (m_panGroup)    m_panGroup->setEnabled(master);
    if (m_hrtfGroup)   m_hrtfGroup->setEnabled(master);
    if (m_eqEnable)    m_eqEnable->setEnabled(master);
    for (auto *s : m_eqSliders) if (s) s->setEnabled(master && m_state.eqEnabled);
    if (m_stretchEn)   m_stretchEn->setEnabled(master);
    if (m_stretchFac)  m_stretchFac->setEnabled(master && m_state.stretchEnabled);
    if (m_stretchWin)  m_stretchWin->setEnabled(master && m_state.stretchEnabled);
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

    m_panGroup->setVisible(isPan);
    m_hrtfGroup->setVisible(any3D);

    // Inside the 3D group: pad + dist visible only in Manual; rpm +
    // radius + ccw visible in Rotate / 8D. Elev / Width / Sway always
    // visible in any 3D mode.
    if (m_padContainer) m_padContainer->setVisible(isManual);
    if (m_distRow)      m_distRow->setVisible(isManual);
    if (m_rpmRow)       m_rpmRow->setVisible(isRotate || is8D);
    if (m_radiusRow)    m_radiusRow->setVisible(isRotate || is8D);
    if (m_ccw)          m_ccw->setVisible(isRotate || is8D);
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
    if (m_eqEnable)    m_eqEnable->setEnabled(on);
    for (auto *s : m_eqSliders) if (s) s->setEnabled(on && m_state.eqEnabled);
    if (m_stretchEn)   m_stretchEn->setEnabled(on);
    if (m_stretchFac)  m_stretchFac->setEnabled(on && m_state.stretchEnabled);
    if (m_stretchWin)  m_stretchWin->setEnabled(on && m_state.stretchEnabled);
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
    m_state.elev = v / 100.0f;
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
