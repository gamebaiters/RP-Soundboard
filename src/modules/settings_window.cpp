#include "settings_window.h"
#include "help_bubble.h"
#include "../style_helper.h"
#include "../ExpandableSection.h"
#include "theme.h"
#include "channel_sandbox_dialog.h"   // SandboxEnginePref
#include "../dsp/SandboxState.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QColorDialog>
#include <QSlider>
#include <QScrollArea>
#include <QSettings>
#include <QFileDialog>
#include <QMessageBox>
#include "../common.h"

namespace {
QHBoxLayout *checkRow(QCheckBox *cb, const QString &help, QWidget *owner) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addWidget(cb);
    if (!help.isEmpty()) row->addWidget(new HelpBubble(help, owner));
    row->addStretch(1);
    return row;
}

ExpandableSection *makeSection(const QString &title, QLayout *content, QWidget *parent,
                               const QString &key, bool expanded = true) {
    auto *sec = new ExpandableSection(title, 200, parent);
    sec->setContentLayout(*content);
    sec->setExpanded(expanded);
    // key is a stable, language-independent id (the visible title is
    // translated, so it can't be used) — remembers open/collapsed state.
    sec->setPersistenceKey(QStringLiteral("settings_") + key);
    return sec;
}
}

SettingsWindow::SettingsWindow(QWidget *parent)
    : QDialog(parent)
    , m_earrape(new QCheckBox(tr("Earrape protection"), this))
    , m_linkVolumes(new QCheckBox(tr("New channels inherit settings from the first channel"), this))
    , m_rememberFx(new QCheckBox(tr("Remember pitch / speed / reverb per channel"), this))
    , m_restoreSession(new QCheckBox(tr("Restore last session on startup"), this))
    , m_globalFx(new QCheckBox(tr("Enable custom FX (pitch / speed / reverb)"), this))
    , m_hideWaveform(new QCheckBox(tr("Hide waveform in channels (compact view)"), this))
    , m_logsEnabled(new QCheckBox(tr("Write debug log file"), this))
    , m_sandboxEnabled(new QCheckBox(tr("Enable audio sandbox (per-channel HRTF / EQ / reverb)"), this))
    , m_meterVisible(new QCheckBox(tr("Show audio meter on each channel"), this))
    , m_exportEnabled(new QCheckBox(tr("Show export button on each channel"), this))
    , m_resetAllSandboxBtn(new QPushButton(tr("Reset all audio sandbox settings"), this))
    , m_profileCombo(new QComboBox(this))
    , m_profileExport(new QPushButton(tr("Export profile..."), this))
    , m_profileImport(new QPushButton(tr("Import profile..."), this))
    , m_themeGroup(nullptr)
    , m_themeAccentBtn(new QPushButton(this))
    , m_themeWaveBtn(new QPushButton(this))
    , m_themeBgBtn(new QPushButton(this))
    , m_themeTextBtn(new QPushButton(this))
    , m_themeTextAuto(new QPushButton(tr("Auto"), this))
    , m_themeButtonBtn(new QPushButton(this))
    , m_themeButtonAuto(new QPushButton(tr("Auto"), this))
    , m_themeResetBtn(new QPushButton(tr("Reset"), this))
    , m_themeCopyBtn(new QPushButton(tr("Copy theme"), this))
    , m_themePasteBtn(new QPushButton(tr("Paste theme..."), this))
    , m_themeAccent(0x4a, 0x90, 0xe2)
    , m_themeWaveform(0x4a, 0x90, 0xe2)
    , m_themeBackground(0x2b, 0x2b, 0x2b)
    , m_themeText()
    , m_themeButton()
    , m_themeContrast(50)
    , m_themeContrastSlider(new QSlider(Qt::Horizontal, this))
    , m_themeContrastLabel(new QLabel("50%", this))
    , m_multi(new QCheckBox(tr("Multi soundboard (parallel channels)"), this))
    , m_muteLocally(new QCheckBox(tr("Mute on my client"), this))
    , m_muteMyself(new QCheckBox(tr("Mute myself during playback"), this))
    , m_showHotkeys(new QCheckBox(tr("Show hotkeys on buttons"), this))
    , m_disableHotkeys(new QCheckBox(tr("Disable hotkeys"), this))
    , m_rows(new QSpinBox(this))
    , m_cols(new QSpinBox(this))
    , m_export(new QPushButton(tr("Export configuration..."), this))
    , m_import(new QPushButton(tr("Import configuration..."), this))
    , m_resetHotkeys(new QPushButton(tr("Reset all hotkeys"), this))
    , m_close(new QPushButton(tr("Close"), this))
    , m_adaptWaveform(new QCheckBox(tr("Adapt waveform display to audio effects"), this))
    , m_cropMarkers(new QCheckBox(tr("Show crop start/end markers on the waveform"), this))
    , m_resetChVolume(new QCheckBox(tr("Volume"), this))
    , m_resetChFx(new QCheckBox(tr("Pitch / speed / reverb"), this))
    , m_resetChFile(new QCheckBox(tr("Loaded file / playback position"), this))
    , m_resetChSandbox(new QCheckBox(tr("Audio sandbox settings"), this))
    , m_resetAllRemoveExtra(new QCheckBox(tr("Remove extra channels"), this))
    , m_resetAllVolume(new QCheckBox(tr("Volume"), this))
    , m_resetAllFx(new QCheckBox(tr("Pitch / speed / reverb"), this))
    , m_resetAllFiles(new QCheckBox(tr("Loaded files"), this))
    , m_resetAllSandbox(new QCheckBox(tr("Audio sandbox settings"), this))
{
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowTitle(tr("Soundboard Settings"));
    setModal(false);
    setProperty("isGBSoundboard", true);
    resize(620, 600);

    m_rows->setRange(1, 50);
    m_cols->setRange(1, 50);
    m_rows->setValue(4);
    m_cols->setValue(8);

    m_earrape->setToolTip(tr("Limits local output so a too-loud sample cannot blow your ears."));
    m_linkVolumes->setToolTip(tr(
        "When ON, every new Channel created via \"+ Add channel\" copies "
        "the local/remote volume, pitch, speed and reverb from the first "
        "channel. When OFF, new channels start with neutral defaults."));
    m_rememberFx->setToolTip(tr("Persist pitch/speed/reverb per channel between sessions."));
    m_restoreSession->setToolTip(tr(
        "Remember channel count, what was loaded in each channel and the "
        "pitch / speed / reverb / volume on every channel."));
    m_muteLocally->setVisible(false);
    m_muteMyself->setVisible(false);
    m_multi->setVisible(false);

    m_globalFx->setToolTip(tr(
        "Master switch for the pitch / speed / reverb effects.\n"
        "Off = the controls disappear from every channel."));

    // Reset behaviour defaults
    m_resetChVolume->setChecked(true);
    m_resetChFx->setChecked(true);
    m_resetChFile->setChecked(true);
    m_resetChSandbox->setChecked(true);
    m_resetAllRemoveExtra->setChecked(true);
    m_resetAllVolume->setChecked(true);
    m_resetAllFx->setChecked(true);
    m_resetAllFiles->setChecked(true);
    m_resetAllSandbox->setChecked(true);

    // ============== Audio section ==============
    auto *audioLay = new QVBoxLayout;
    audioLay->addLayout(checkRow(m_globalFx, tr(
        "Master switch for the pitch / speed / reverb effects. When OFF\n"
        "every channel hides its FX panel and per-button custom FX are\n"
        "skipped at playback time."), this));
    audioLay->addLayout(checkRow(m_earrape, tr(
        "Limits local audio output so a too-loud sample cannot deafen you."), this));
    audioLay->addWidget(m_multi);

    // ============== Channels section ==============
    auto *channelsLay = new QVBoxLayout;
    channelsLay->addLayout(checkRow(m_linkVolumes, tr(
        "When ON, each new Channel copies settings from the first channel."), this));
    channelsLay->addLayout(checkRow(m_rememberFx, tr(
        "When ON, each channel remembers its pitch / speed / reverb between sessions."), this));
    channelsLay->addLayout(checkRow(m_hideWaveform, tr(
        "Compact channel view: removes the waveform display from every channel."), this));
    channelsLay->addLayout(checkRow(m_restoreSession, tr(
        "Remember channel count, loaded files and all settings on the next open."), this));
    channelsLay->addLayout(checkRow(m_adaptWaveform, tr(
        "When ON, the waveform display adapts to show the visual effect of\n"
        "active audio sandbox effects (especially Paulstretch stretching).\n"
        "When OFF, the raw audio waveform is always shown."), this));
    channelsLay->addLayout(checkRow(m_cropMarkers, tr(
        "When ON, a sound that has a per-cell crop start and/or end point\n"
        "shows coloured markers on the waveform at those positions.\n"
        "Only the points that are actually set are drawn."), this));

    // ---- 3D HRTF engine (default for new channels) ----
    // The Classic parametric engine is deprecated but kept available
    // for users who prefer its lightweight character. Default is
    // Leia for every new channel; selection here writes the
    // SandboxEnginePref QSettings key which Channel ctor reads on
    // creation. Existing channels with a per-cell saved engine
    // restore their saved value regardless of this default.
    {
        auto *engRow = new QHBoxLayout;
        engRow->setContentsMargins(0, 0, 0, 0);
        engRow->setSpacing(6);
        engRow->addWidget(new QLabel(tr("Default 3D HRTF engine for new channels:"), this));
        auto *engBox = new QComboBox(this);
        engBox->addItem(tr("Leia (measured HRTF) - recommended"));
        engBox->addItem(tr("Classic (parametric, deprecated)"));
        const int saved = SandboxEnginePref::load();
        engBox->setCurrentIndex(saved == SandboxState::Engine_Classic ? 1 : 0);
        connect(engBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [](int idx) {
            SandboxEnginePref::save(idx == 1
                ? SandboxState::Engine_Classic
                : SandboxState::Engine_Leia);
        });
        engRow->addWidget(engBox, 1);
        engRow->addWidget(new HelpBubble(tr(
            "Leia uses measured-HRTF convolution with image-source room\n"
            "reflections - correct front/back localisation and a far more\n"
            "convincing sense of space. Classic is the older parametric\n"
            "Brown-Duda engine; kept for users who prefer its lighter\n"
            "character. The choice applies to new channels; existing\n"
            "channels keep their per-cell saved engine."), this));
        channelsLay->addLayout(engRow);
    }

    // ============== Button grid section ==============
    auto *gridLay = new QFormLayout;
    gridLay->addRow(tr("Rows"), m_rows);
    gridLay->addRow(tr("Columns"), m_cols);

    // ============== Hotkeys section ==============
    auto *hotkeyLay = new QVBoxLayout;
    hotkeyLay->addLayout(checkRow(m_showHotkeys, tr(
        "Render the bound hotkey on top of each button."), this));
    hotkeyLay->addLayout(checkRow(m_disableHotkeys, tr(
        "Globally suppress hotkey handling."), this));
    m_resetHotkeys->setStyleSheet(
        "QPushButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 5px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e04141; }");
    auto *resetHkRow = new QHBoxLayout;
    resetHkRow->addWidget(m_resetHotkeys);
    resetHkRow->addStretch(1);
    hotkeyLay->addLayout(resetHkRow);

    // ============== Custom Leia SOFA dataset section ==============
    auto *sofaLay = new QVBoxLayout;
    auto *sofaRow = new QHBoxLayout;
    auto *sofaPathLbl = new QLabel(tr("(using bundled default)"), this);
    auto *sofaPickBtn = new QPushButton(tr("Pick custom SOFA..."), this);
    auto *sofaClearBtn = new QPushButton(tr("Use default"), this);
    sofaPathLbl->setStyleSheet("color: #aaa;");
    sofaPathLbl->setWordWrap(true);
    sofaRow->addWidget(sofaPickBtn);
    sofaRow->addWidget(sofaClearBtn);
    sofaRow->addStretch(1);
    sofaLay->addLayout(sofaRow);
    sofaLay->addWidget(sofaPathLbl);
    auto loadSofaPath = [sofaPathLbl, this]{
        QSettings sset(QStringLiteral("GameBaiters"),
                       QStringLiteral("Soundboard"));
        QString p = sset.value(QStringLiteral("leia/custom_sofa_path"))
                       .toString();
        sofaPathLbl->setText(p.isEmpty()
            ? tr("(using bundled default)")
            : tr("Custom: %1").arg(p));
    };
    loadSofaPath();
    connect(sofaPickBtn, &QPushButton::clicked, this, [this, loadSofaPath]{
        QString p = QFileDialog::getOpenFileName(
            this, tr("Pick a SOFA HRTF dataset"), QString(),
            tr("SOFA datasets (*.sofa);;All files (*.*)"));
        if (p.isEmpty()) return;
        QSettings sset(QStringLiteral("GameBaiters"),
                       QStringLiteral("Soundboard"));
        sset.setValue(QStringLiteral("leia/custom_sofa_path"), p);
        loadSofaPath();
        QMessageBox::information(this, tr("Custom HRTF"),
            tr("Reopen the soundboard to apply the new dataset."));
    });
    connect(sofaClearBtn, &QPushButton::clicked, this, [this, loadSofaPath]{
        QSettings sset(QStringLiteral("GameBaiters"),
                       QStringLiteral("Soundboard"));
        sset.remove(QStringLiteral("leia/custom_sofa_path"));
        loadSofaPath();
    });

    // ============== Logging section ==============
    auto *logLay = new QVBoxLayout;
    logLay->addLayout(checkRow(m_logsEnabled, tr(
        "Writes a debug log file (rpsb_debug.log) inside your TeamSpeak config folder."), this));
    // Hidden-by-design real-time log viewer button. Plain link-style so
    // it does not draw a casual user's eye - this is an advanced
    // diagnostic surface. When the user enables "Write debug log file"
    // above, the viewer mirrors EXACTLY the same lines the file gets.
    m_logViewerBtn = new QPushButton(tr("Show real-time log..."), this);
    m_logViewerBtn->setFlat(true);
    m_logViewerBtn->setCursor(Qt::PointingHandCursor);
    m_logViewerBtn->setStyleSheet(
        "QPushButton { color: #8aa6c0; text-align: left;"
        " border: none; padding: 0px; font-size: 10px;"
        " text-decoration: underline; }"
        "QPushButton:hover { color: #b6cee4; }");
    auto *copyDebugBtn = new QPushButton(tr("Copy sandbox debug snapshot"), this);
    copyDebugBtn->setFlat(true);
    copyDebugBtn->setCursor(Qt::PointingHandCursor);
    copyDebugBtn->setStyleSheet(m_logViewerBtn->styleSheet());
    auto *logBtnRow = new QHBoxLayout;
    logBtnRow->addWidget(m_logViewerBtn);
    logBtnRow->addSpacing(12);
    logBtnRow->addWidget(copyDebugBtn);
    logBtnRow->addStretch(1);
    logLay->addLayout(logBtnRow);
    connect(m_logViewerBtn, &QPushButton::clicked, this,
            &SettingsWindow::showLogViewerRequested);
    connect(copyDebugBtn, &QPushButton::clicked, this,
            &SettingsWindow::copySandboxDebugRequested);

    // ============== Audio sandbox section ==============
    auto *sandboxLay = new QVBoxLayout;
    sandboxLay->addLayout(checkRow(m_sandboxEnabled, tr(
        "Master switch for the per-channel Audio Sandbox button."), this));
    sandboxLay->addLayout(checkRow(m_meterVisible, tr(
        "Render the dual L/R peak meter on each channel."), this));
    sandboxLay->addLayout(checkRow(m_exportEnabled, tr(
        "Show an Export button on each channel."), this));
    m_resetAllSandboxBtn->setStyleSheet(
        "QPushButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 5px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e04141; }");
    auto *resetSbRow = new QHBoxLayout;
    resetSbRow->addWidget(m_resetAllSandboxBtn);
    resetSbRow->addStretch(1);
    sandboxLay->addLayout(resetSbRow);

    // ============== Profiles section ==============
    for (int i = 0; i < NUM_CONFIGS; ++i)
        m_profileCombo->addItem(tr("Profile %1").arg(i + 1), i);
    auto *profileLay = new QHBoxLayout;
    profileLay->addWidget(new QLabel(tr("Active:")));
    profileLay->addWidget(m_profileCombo, 1);
    profileLay->addWidget(m_profileExport);
    profileLay->addWidget(m_profileImport);

    // ============== Custom theme section ==============
    // Theme remains a QGroupBox (checkable) wrapped inside an ExpandableSection.
    m_themeGroup = new QGroupBox(tr("Enable custom theme"), this);
    m_themeGroup->setCheckable(true);
    m_themeGroup->setChecked(false);
    auto *themeOuter = new QVBoxLayout(m_themeGroup);
    auto *themeLay = new QHBoxLayout;
    themeOuter->addLayout(themeLay);
    themeLay->addWidget(new QLabel(tr("Background:")));
    m_themeBgBtn->setFixedSize(28, 22);
    themeLay->addWidget(m_themeBgBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Accent:")));
    m_themeAccentBtn->setFixedSize(28, 22);
    themeLay->addWidget(m_themeAccentBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Waveform:")));
    m_themeWaveBtn->setFixedSize(28, 22);
    themeLay->addWidget(m_themeWaveBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Text:")));
    m_themeTextBtn->setFixedSize(28, 22);
    themeLay->addWidget(m_themeTextBtn);
    m_themeTextAuto->setMaximumWidth(56);
    m_themeTextAuto->setFixedHeight(22);
    themeLay->addWidget(m_themeTextAuto);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Buttons:")));
    m_themeButtonBtn->setFixedSize(28, 22);
    themeLay->addWidget(m_themeButtonBtn);
    m_themeButtonAuto->setMaximumWidth(56);
    m_themeButtonAuto->setFixedHeight(22);
    themeLay->addWidget(m_themeButtonAuto);
    themeLay->addStretch(1);
    m_themeContrastSlider->setRange(0, 100);
    m_themeContrastSlider->setValue(50);
    m_themeContrastLabel->setMinimumWidth(40);
    m_themeContrastLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *contrastRow = new QHBoxLayout;
    contrastRow->addWidget(new QLabel(tr("Contrast:")));
    contrastRow->addWidget(m_themeContrastSlider, 1);
    contrastRow->addWidget(m_themeContrastLabel);
    themeOuter->addLayout(contrastRow);
    auto *themeBtnRow = new QHBoxLayout;
    themeBtnRow->addWidget(m_themeResetBtn);
    themeBtnRow->addWidget(m_themeCopyBtn);
    themeBtnRow->addWidget(m_themePasteBtn);
    themeBtnRow->addStretch(1);
    themeOuter->addLayout(themeBtnRow);
    auto *themeWrapLay = new QVBoxLayout;
    themeWrapLay->addWidget(m_themeGroup);

    // Theme signal connections
    connect(m_themeContrastSlider, &QSlider::valueChanged, this, [this](int v){
        m_themeContrast = v;
        m_themeContrastLabel->setText(QString::number(v) + "%");
        emit themeChanged(m_themeGroup->isChecked(), m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton);
    });
    auto repaintColorButton = [](QPushButton *btn, const QColor &c){
        btn->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(c.name()));
    };
    repaintColorButton(m_themeAccentBtn, m_themeAccent);
    repaintColorButton(m_themeWaveBtn,   m_themeWaveform);
    repaintColorButton(m_themeBgBtn,     m_themeBackground);
    repaintColorButton(m_themeTextBtn,   m_themeText.isValid() ? m_themeText : QColor("#dcdcdc"));
    repaintColorButton(m_themeButtonBtn, m_themeButton.isValid() ? m_themeButton : QColor("#3a3a3a"));
    auto pickColor = [this, repaintColorButton](QColor &target, QPushButton *btn){
        QColor seed = target.isValid() ? target : QColor("#dcdcdc");
        QColor c = QColorDialog::getColor(seed, this, tr("Pick a color"));
        if (!c.isValid()) return;
        target = c;
        repaintColorButton(btn, c);
        emit themeChanged(m_themeGroup->isChecked(), m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton);
    };
    connect(m_themeAccentBtn, &QPushButton::clicked, this,
            [this, pickColor]{ auto fn = pickColor; fn(m_themeAccent,     m_themeAccentBtn); });
    connect(m_themeWaveBtn,   &QPushButton::clicked, this,
            [this, pickColor]{ auto fn = pickColor; fn(m_themeWaveform,   m_themeWaveBtn);   });
    connect(m_themeBgBtn,     &QPushButton::clicked, this,
            [this, pickColor]{ auto fn = pickColor; fn(m_themeBackground, m_themeBgBtn);     });
    connect(m_themeTextBtn,   &QPushButton::clicked, this,
            [this, pickColor]{ auto fn = pickColor; fn(m_themeText,       m_themeTextBtn);   });
    connect(m_themeButtonBtn, &QPushButton::clicked, this,
            [this, pickColor]{ auto fn = pickColor; fn(m_themeButton,     m_themeButtonBtn); });
    connect(m_themeTextAuto,  &QPushButton::clicked, this, [this, repaintColorButton]{
        m_themeText = QColor();
        QColor preview = m_themeBackground.lightnessF() < 0.5
            ? QColor(0xec, 0xec, 0xec) : QColor(0x10, 0x10, 0x10);
        repaintColorButton(m_themeTextBtn, preview);
        emit themeChanged(m_themeGroup->isChecked(), m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton);
    });
    connect(m_themeButtonAuto, &QPushButton::clicked, this, [this, repaintColorButton]{
        m_themeButton = QColor();
        Theme::Colors c = Theme::colors();
        c.background = m_themeBackground;
        c.contrast = m_themeContrast;
        c.enabled = true;
        QColor preview = Theme::derive(c).button;
        repaintColorButton(m_themeButtonBtn, preview);
        emit themeChanged(m_themeGroup->isChecked(), m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton);
    });
    connect(m_themeGroup,     &QGroupBox::toggled, this,
            [this](bool on){ emit themeChanged(on, m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton); });
    connect(m_themeResetBtn,  &QPushButton::clicked, this, &SettingsWindow::themeResetRequested);
    connect(m_themeCopyBtn,   &QPushButton::clicked, this, &SettingsWindow::themeCopyRequested);
    connect(m_themePasteBtn,  &QPushButton::clicked, this, &SettingsWindow::themePasteRequested);

    // ============== Reset behaviour section ==============
    auto *resetBehLay = new QVBoxLayout;
    resetBehLay->addWidget(new QLabel(tr("Per-channel reset button resets:")));
    resetBehLay->addWidget(m_resetChVolume);
    resetBehLay->addWidget(m_resetChFx);
    resetBehLay->addWidget(m_resetChFile);
    resetBehLay->addWidget(m_resetChSandbox);
    resetBehLay->addSpacing(8);
    resetBehLay->addWidget(new QLabel(tr("\"Reset channels\" button resets:")));
    resetBehLay->addWidget(m_resetAllRemoveExtra);
    resetBehLay->addWidget(m_resetAllVolume);
    resetBehLay->addWidget(m_resetAllFx);
    resetBehLay->addWidget(m_resetAllFiles);
    resetBehLay->addWidget(m_resetAllSandbox);

    // ============== Import/Export section ==============
    auto *ioLay = new QHBoxLayout;
    ioLay->addWidget(m_export);
    ioLay->addWidget(m_import);
    ioLay->addStretch(1);

    // ============== Close button ==============
    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_close);

    // ============== Language section ==============
    // Self-contained: persisted directly to QSettings and read back at
    // plugin init. Applied on the next plugin load (no live switch).
    auto *langLay = new QVBoxLayout;
    {
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(tr("Interface language:")));
        auto *langCombo = new QComboBox(this);
        langCombo->addItem(tr("Automatic (system language)"), QStringLiteral("auto"));
        langCombo->addItem(QStringLiteral("English"),  QStringLiteral("en"));
        langCombo->addItem(QStringLiteral("Italiano"), QStringLiteral("it"));
        QSettings ls(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        int ci = langCombo->findData(ls.value(QStringLiteral("language"),
                                              QStringLiteral("auto")).toString());
        langCombo->setCurrentIndex(ci >= 0 ? ci : 0);
        row->addWidget(langCombo, 1);
        langLay->addLayout(row);
        auto *note = new QLabel(tr(
            "Italian is selected automatically when the system language "
            "is Italian. A change here is applied the next time the "
            "plugin loads (reload the plugin or restart TeamSpeak)."), this);
        note->setWordWrap(true);
        note->setStyleSheet("color: #999; font-size: 11px;");
        langLay->addWidget(note);
        connect(langCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, [langCombo](int){
            QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
            s.setValue(QStringLiteral("language"), langCombo->currentData().toString());
            s.sync();
        });
    }

    // Build the scrollable body with ExpandableSection for each category
    auto *body = new QVBoxLayout;
    body->setSpacing(2);
    body->addWidget(makeSection(tr("Language"),       langLay,     this, "language"));
    body->addWidget(makeSection(tr("Audio"),          audioLay,    this, "audio"));
    body->addWidget(makeSection(tr("Channels"),       channelsLay, this, "channels"));
    body->addWidget(makeSection(tr("Button grid"),    gridLay,     this, "grid"));
    body->addWidget(makeSection(tr("Hotkeys"),        hotkeyLay,   this, "hotkeys"));
    body->addWidget(makeSection(tr("Logging"),        logLay,      this, "logging"));
    body->addWidget(makeSection(tr("Audio sandbox"),  sandboxLay,  this, "sandbox"));
    body->addWidget(makeSection(tr("HRTF dataset (Leia)"), sofaLay, this, "sofa", false));
    body->addWidget(makeSection(tr("Profiles"),       profileLay,  this, "profiles"));
    body->addWidget(makeSection(tr("Custom theme"),   themeWrapLay,this, "theme"));
    body->addWidget(makeSection(tr("Reset behaviour"),resetBehLay, this, "reset", false));
    body->addWidget(makeSection(tr("Import / Export"),ioLay,       this, "io",    false));
    body->addStretch(1);

    auto *scrollWidget = new QWidget(this);
    scrollWidget->setLayout(body);
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(scrollWidget);
    scroll->setFrameShape(QFrame::NoFrame);

    auto *root = new QVBoxLayout(this);
    root->addWidget(scroll, 1);
    root->addLayout(btnRow);

    // Signal connections
    connect(m_earrape,        &QCheckBox::toggled, this, &SettingsWindow::earrapeProtectionChanged);
    connect(m_linkVolumes,    &QCheckBox::toggled, this, &SettingsWindow::linkVolumesChanged);
    connect(m_rememberFx,     &QCheckBox::toggled, this, &SettingsWindow::rememberPitchSpeedChanged);
    connect(m_restoreSession, &QCheckBox::toggled, this, &SettingsWindow::restoreSessionChanged);
    connect(m_globalFx,       &QCheckBox::toggled, this, &SettingsWindow::globalFxEnabledChanged);
    connect(m_hideWaveform,   &QCheckBox::toggled, this, &SettingsWindow::hideWaveformChanged);
    connect(m_logsEnabled,    &QCheckBox::toggled, this, &SettingsWindow::logsEnabledChanged);
    connect(m_sandboxEnabled, &QCheckBox::toggled, this, &SettingsWindow::audioSandboxEnabledChanged);
    connect(m_meterVisible,   &QCheckBox::toggled, this, &SettingsWindow::audioMeterVisibleChanged);
    connect(m_exportEnabled,  &QCheckBox::toggled, this, &SettingsWindow::audioExportEnabledChanged);
    connect(m_resetAllSandboxBtn,&QPushButton::clicked, this, &SettingsWindow::resetAllAudioSandboxRequested);
    connect(m_profileCombo,   QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx){ emit activeProfileChanged(idx); });
    connect(m_profileExport,  &QPushButton::clicked, this, [this]{
        emit exportProfileRequested(m_profileCombo->currentIndex());
    });
    connect(m_profileImport,  &QPushButton::clicked, this, [this]{
        emit importProfileRequested(m_profileCombo->currentIndex());
    });
    connect(m_multi,          &QCheckBox::toggled, this, &SettingsWindow::multiSoundboardChanged);
    connect(m_muteLocally,    &QCheckBox::toggled, this, &SettingsWindow::muteLocallyChanged);
    connect(m_muteMyself,     &QCheckBox::toggled, this, &SettingsWindow::muteMyselfChanged);
    connect(m_showHotkeys,    &QCheckBox::toggled, this, &SettingsWindow::showHotkeysOnButtonsChanged);
    connect(m_disableHotkeys, &QCheckBox::toggled, this, &SettingsWindow::disableHotkeysChanged);
    connect(m_rows, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsWindow::rowsChanged);
    connect(m_cols, QOverload<int>::of(&QSpinBox::valueChanged), this, &SettingsWindow::colsChanged);
    connect(m_export, &QPushButton::clicked, this, &SettingsWindow::exportRequested);
    connect(m_import, &QPushButton::clicked, this, &SettingsWindow::importRequested);
    connect(m_resetHotkeys, &QPushButton::clicked, this, &SettingsWindow::resetAllHotkeysRequested);
    connect(m_close,  &QPushButton::clicked, this, &QDialog::accept);
    connect(m_adaptWaveform, &QCheckBox::toggled, this, &SettingsWindow::adaptWaveformToFxChanged);
    connect(m_cropMarkers,   &QCheckBox::toggled, this, &SettingsWindow::showCropMarkersChanged);

    connect(m_resetChVolume,      &QCheckBox::toggled, this, &SettingsWindow::resetChVolumeChanged);
    connect(m_resetChFx,          &QCheckBox::toggled, this, &SettingsWindow::resetChFxChanged);
    connect(m_resetChFile,        &QCheckBox::toggled, this, &SettingsWindow::resetChFileChanged);
    connect(m_resetChSandbox,     &QCheckBox::toggled, this, &SettingsWindow::resetChSandboxChanged);
    connect(m_resetAllRemoveExtra,&QCheckBox::toggled, this, &SettingsWindow::resetAllRemoveExtraChanged);
    connect(m_resetAllVolume,     &QCheckBox::toggled, this, &SettingsWindow::resetAllVolumeChanged);
    connect(m_resetAllFx,         &QCheckBox::toggled, this, &SettingsWindow::resetAllFxChanged);
    connect(m_resetAllFiles,      &QCheckBox::toggled, this, &SettingsWindow::resetAllFilesChanged);
    connect(m_resetAllSandbox,    &QCheckBox::toggled, this, &SettingsWindow::resetAllSandboxChanged);
}

// ---- Getters ----
bool SettingsWindow::earrapeProtection()      const { return m_earrape->isChecked();        }
bool SettingsWindow::linkVolumes()            const { return m_linkVolumes->isChecked();    }
bool SettingsWindow::rememberPitchSpeed()     const { return m_rememberFx->isChecked();     }
bool SettingsWindow::restoreSession()         const { return m_restoreSession->isChecked(); }
bool SettingsWindow::globalFxEnabled()        const { return m_globalFx->isChecked();        }
bool SettingsWindow::hideWaveform()           const { return m_hideWaveform->isChecked();    }
bool SettingsWindow::logsEnabled()            const { return m_logsEnabled->isChecked();     }
bool SettingsWindow::audioSandboxEnabled()    const { return m_sandboxEnabled->isChecked();   }
bool SettingsWindow::audioMeterVisible()      const { return m_meterVisible->isChecked();     }
bool SettingsWindow::audioExportEnabled()     const { return m_exportEnabled->isChecked();    }
int  SettingsWindow::activeProfile()          const { return m_profileCombo->currentIndex(); }
bool   SettingsWindow::themeEnabled()         const { return m_themeGroup && m_themeGroup->isChecked(); }
QColor SettingsWindow::themeAccent()          const { return m_themeAccent; }
QColor SettingsWindow::themeWaveform()        const { return m_themeWaveform; }
QColor SettingsWindow::themeBackground()      const { return m_themeBackground; }
int    SettingsWindow::themeContrast()        const { return m_themeContrast; }
QColor SettingsWindow::themeText()            const { return m_themeText; }
QColor SettingsWindow::themeButton()          const { return m_themeButton; }
bool SettingsWindow::multiSoundboard()        const { return m_multi->isChecked();          }
bool SettingsWindow::muteLocally()            const { return m_muteLocally->isChecked();    }
bool SettingsWindow::muteMyself()             const { return m_muteMyself->isChecked();     }
bool SettingsWindow::showHotkeysOnButtons()   const { return m_showHotkeys->isChecked();    }
bool SettingsWindow::disableHotkeys()         const { return m_disableHotkeys->isChecked(); }
int  SettingsWindow::rows()                   const { return m_rows->value();               }
int  SettingsWindow::cols()                   const { return m_cols->value();               }
bool SettingsWindow::adaptWaveformToFx()      const { return m_adaptWaveform->isChecked();  }
bool SettingsWindow::showCropMarkers()        const { return m_cropMarkers->isChecked();    }

bool SettingsWindow::resetChVolume()          const { return m_resetChVolume->isChecked();      }
bool SettingsWindow::resetChFx()              const { return m_resetChFx->isChecked();          }
bool SettingsWindow::resetChFile()            const { return m_resetChFile->isChecked();         }
bool SettingsWindow::resetChSandbox()         const { return m_resetChSandbox->isChecked();     }
bool SettingsWindow::resetAllRemoveExtra()    const { return m_resetAllRemoveExtra->isChecked(); }
bool SettingsWindow::resetAllVolume()         const { return m_resetAllVolume->isChecked();      }
bool SettingsWindow::resetAllFx()             const { return m_resetAllFx->isChecked();          }
bool SettingsWindow::resetAllFiles()          const { return m_resetAllFiles->isChecked();       }
bool SettingsWindow::resetAllSandbox()        const { return m_resetAllSandbox->isChecked();     }

// ---- Setters ----
void SettingsWindow::setEarrapeProtection(bool on)     { QSignalBlocker b(m_earrape);        m_earrape->setChecked(on);        }
void SettingsWindow::setLinkVolumes(bool on)           { QSignalBlocker b(m_linkVolumes);    m_linkVolumes->setChecked(on);    }
void SettingsWindow::setRememberPitchSpeed(bool on)    { QSignalBlocker b(m_rememberFx);     m_rememberFx->setChecked(on);     }
void SettingsWindow::setRestoreSession(bool on)        { QSignalBlocker b(m_restoreSession); m_restoreSession->setChecked(on); }
void SettingsWindow::setGlobalFxEnabled(bool on)       { QSignalBlocker b(m_globalFx);        m_globalFx->setChecked(on);        }
void SettingsWindow::setHideWaveform(bool on)          { QSignalBlocker b(m_hideWaveform);    m_hideWaveform->setChecked(on);    }
void SettingsWindow::setLogsEnabled(bool on)           { QSignalBlocker b(m_logsEnabled);     m_logsEnabled->setChecked(on);     }
void SettingsWindow::setAudioSandboxEnabled(bool on)   { QSignalBlocker b(m_sandboxEnabled);  m_sandboxEnabled->setChecked(on);  }
void SettingsWindow::setAudioMeterVisible(bool on)     { QSignalBlocker b(m_meterVisible);    m_meterVisible->setChecked(on);    }
void SettingsWindow::setAudioExportEnabled(bool on)    { QSignalBlocker b(m_exportEnabled);   m_exportEnabled->setChecked(on);   }
void SettingsWindow::setActiveProfile(int p)           { QSignalBlocker b(m_profileCombo);    if (p >= 0 && p < m_profileCombo->count()) m_profileCombo->setCurrentIndex(p); }
void SettingsWindow::setMultiSoundboard(bool on)       { QSignalBlocker b(m_multi);          m_multi->setChecked(on);          }
void SettingsWindow::setMuteLocally(bool on)           { QSignalBlocker b(m_muteLocally);    m_muteLocally->setChecked(on);    }
void SettingsWindow::setMuteMyself(bool on)            { QSignalBlocker b(m_muteMyself);     m_muteMyself->setChecked(on);     }
void SettingsWindow::setShowHotkeysOnButtons(bool on)  { QSignalBlocker b(m_showHotkeys);    m_showHotkeys->setChecked(on);    }
void SettingsWindow::setDisableHotkeys(bool on)        { QSignalBlocker b(m_disableHotkeys); m_disableHotkeys->setChecked(on); }
void SettingsWindow::setRows(int r)                    { QSignalBlocker b(m_rows);           m_rows->setValue(r);              }
void SettingsWindow::setCols(int c)                    { QSignalBlocker b(m_cols);           m_cols->setValue(c);              }
void SettingsWindow::setAdaptWaveformToFx(bool on)     { QSignalBlocker b(m_adaptWaveform);  m_adaptWaveform->setChecked(on);  }
void SettingsWindow::setShowCropMarkers(bool on)       { QSignalBlocker b(m_cropMarkers);    m_cropMarkers->setChecked(on);    }

void SettingsWindow::setResetChVolume(bool on)         { QSignalBlocker b(m_resetChVolume);      m_resetChVolume->setChecked(on);      }
void SettingsWindow::setResetChFx(bool on)             { QSignalBlocker b(m_resetChFx);          m_resetChFx->setChecked(on);          }
void SettingsWindow::setResetChFile(bool on)           { QSignalBlocker b(m_resetChFile);        m_resetChFile->setChecked(on);        }
void SettingsWindow::setResetChSandbox(bool on)        { QSignalBlocker b(m_resetChSandbox);     m_resetChSandbox->setChecked(on);     }
void SettingsWindow::setResetAllRemoveExtra(bool on)   { QSignalBlocker b(m_resetAllRemoveExtra);m_resetAllRemoveExtra->setChecked(on); }
void SettingsWindow::setResetAllVolume(bool on)        { QSignalBlocker b(m_resetAllVolume);     m_resetAllVolume->setChecked(on);      }
void SettingsWindow::setResetAllFx(bool on)            { QSignalBlocker b(m_resetAllFx);         m_resetAllFx->setChecked(on);          }
void SettingsWindow::setResetAllFiles(bool on)         { QSignalBlocker b(m_resetAllFiles);      m_resetAllFiles->setChecked(on);       }
void SettingsWindow::setResetAllSandbox(bool on)       { QSignalBlocker b(m_resetAllSandbox);    m_resetAllSandbox->setChecked(on);     }

void SettingsWindow::setTheme(bool enabled, const QColor &accent, const QColor &waveform, const QColor &background, int contrast, const QColor &text, const QColor &button)
{
    {
        QSignalBlocker b(m_themeGroup);
        m_themeGroup->setChecked(enabled);
    }
    m_themeAccent     = accent;
    m_themeWaveform   = waveform;
    m_themeBackground = background;
    m_themeText       = text;
    m_themeButton     = button;
    m_themeContrast   = qBound(0, contrast, 100);
    {
        QSignalBlocker b(m_themeContrastSlider);
        m_themeContrastSlider->setValue(m_themeContrast);
    }
    m_themeContrastLabel->setText(QString::number(m_themeContrast) + "%");
    m_themeAccentBtn->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(accent.name()));
    m_themeWaveBtn  ->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(waveform.name()));
    m_themeBgBtn    ->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(background.name()));
    QColor textPreview = text.isValid() ? text
        : (background.lightnessF() < 0.5 ? QColor(0xec, 0xec, 0xec) : QColor(0x10, 0x10, 0x10));
    m_themeTextBtn  ->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(textPreview.name()));
    QColor buttonPreview;
    if (button.isValid()) {
        buttonPreview = button;
    } else {
        Theme::Colors c = Theme::colors();
        c.background = background;
        c.contrast = m_themeContrast;
        c.enabled = true;
        buttonPreview = Theme::derive(c).button;
    }
    m_themeButtonBtn->setStyleSheet(QString("QPushButton { background-color: %1; border: 1px solid #555; }").arg(buttonPreview.name()));
}
