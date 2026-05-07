#include "settings_window.h"
#include "help_bubble.h"
#include "../style_helper.h"
#include "theme.h"

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
#include "../common.h"

namespace {
// Helper: build a row with [checkbox][?][stretch] so the help bubble sits
// next to the checkbox label instead of below it.
QHBoxLayout *checkRow(QCheckBox *cb, const QString &help, QWidget *owner) {
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    row->addWidget(cb);
    if (!help.isEmpty()) row->addWidget(new HelpBubble(help, owner));
    row->addStretch(1);
    return row;
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
    , m_resetAllSandbox(new QPushButton(tr("Reset all audio sandbox settings"), this))
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
{
    setWindowTitle(tr("Soundboard Settings"));
    setModal(false);
    setProperty("isGBSoundboard", true);
    resize(620, 560);

    m_rows->setRange(1, 50);
    m_cols->setRange(1, 50);
    m_rows->setValue(4);
    m_cols->setValue(8);

    // Tooltip stays for hover; help bubble renders the long-form text.
    m_earrape->setToolTip(tr("Limits local output so a too-loud sample cannot blow your ears."));
    m_linkVolumes->setToolTip(tr(
        "When ON, every new Channel created via \"+ Add channel\" copies "
        "the local/remote volume, pitch, speed and reverb from the first "
        "channel. When OFF, new channels start with neutral defaults."));
    m_rememberFx->setToolTip(tr("Persist pitch/speed/reverb per channel between sessions."));
    m_restoreSession->setToolTip(tr(
        "Remember channel count, what was loaded in each channel and the "
        "pitch / speed / reverb / volume on every channel. The next time "
        "you open the soundboard the same layout is rebuilt and channels "
        "that had a file loaded come back paused."));
    // Mute on my client + Mute myself moved to the main page bottom bar.
    m_muteLocally->setVisible(false);
    m_muteMyself->setVisible(false);
    // Multi-soundboard is implied by the channels mechanism in the new
    // UI - hide its checkbox to remove the duplicate concept.
    m_multi->setVisible(false);

    m_globalFx->setToolTip(tr(
        "Master switch for the pitch / speed / reverb effects.\n"
        "Off = the controls disappear from every channel and from the\n"
        "advanced options dialog. Per-button custom FX are also ignored\n"
        "even if they were previously set up."));

    // Audio group: master DSP switches only - earrape + global FX. Channel-
    // behaviour toggles moved to a dedicated "Channels" group below so each
    // group lines up with one mental model.
    auto *audioBox = new QGroupBox(tr("Audio"), this);
    auto *audioLay = new QVBoxLayout(audioBox);
    audioLay->addLayout(checkRow(m_globalFx, tr(
        "Master switch for the pitch / speed / reverb effects. When OFF\n"
        "every channel hides its FX panel and per-button custom FX are\n"
        "skipped at playback time, even if a button had its custom FX\n"
        "checkbox enabled."), this));
    audioLay->addLayout(checkRow(m_earrape, tr(
        "Limits local audio output so a too-loud sample (or a sample\n"
        "with extreme pitch + speed) cannot deafen you."), this));
    audioLay->addWidget(m_multi);

    // Channels group: how new channels behave + how many you see.
    auto *channelsBox = new QGroupBox(tr("Channels"), this);
    auto *channelsLay = new QVBoxLayout(channelsBox);
    channelsLay->addLayout(checkRow(m_linkVolumes, tr(
        "When ON, each new Channel created with \"+ Add channel\" copies\n"
        "every setting (local + remote volume, pitch, speed, reverb,\n"
        "sync) from the first channel. When OFF, new channels start\n"
        "from neutral defaults."), this));
    channelsLay->addLayout(checkRow(m_rememberFx, tr(
        "When ON, each channel remembers its pitch / speed / reverb\n"
        "between sessions. When OFF, channels reset to zero on next\n"
        "session. Independent of \"Restore last session\"."), this));
    channelsLay->addLayout(checkRow(m_hideWaveform, tr(
        "Compact channel view: removes the waveform display + transport\n"
        "buttons (play / pause / stop / skip) from every channel,\n"
        "keeping only the volume + FX sliders. Useful for tight UIs\n"
        "or when you only need the audio knobs."), this));
    channelsLay->addLayout(checkRow(m_restoreSession, tr(
        "Remember channel count, the file loaded in each channel and\n"
        "every channel's volume / pitch / speed / reverb. On the next\n"
        "soundboard open the same layout is rebuilt and channels that\n"
        "had a file loaded come back paused at the previous position."), this));

    auto *gridBox = new QGroupBox(tr("Button grid"), this);
    auto *gridForm = new QFormLayout(gridBox);
    gridForm->addRow(tr("Rows"), m_rows);
    gridForm->addRow(tr("Columns"), m_cols);

    auto *hotkeyBox = new QGroupBox(tr("Hotkeys"), this);
    auto *hotkeyLay = new QVBoxLayout(hotkeyBox);
    hotkeyLay->addLayout(checkRow(m_showHotkeys, tr(
        "Render the bound hotkey on top of each button so you can see\n"
        "which key triggers what without opening the advanced dialog."), this));
    hotkeyLay->addLayout(checkRow(m_disableHotkeys, tr(
        "Globally suppress the soundboard plugin's hotkey handling. The\n"
        "TeamSpeak bindings stay in your hotkey profile but pressing\n"
        "them does nothing while this is on."), this));
    m_resetHotkeys->setStyleSheet(
        "QPushButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 5px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e04141; }");
    m_resetHotkeys->setToolTip(tr(
        "Clears every saved hotkey from the soundboard so spamming a key\n"
        "no longer triggers a button. To also wipe the binding from\n"
        "TeamSpeak's hotkey profile, open TeamSpeak's hotkey settings."));
    auto *resetRow = new QHBoxLayout;
    resetRow->addWidget(m_resetHotkeys);
    resetRow->addWidget(new HelpBubble(tr(
        "Permanently disables every previously bound hotkey for this\n"
        "soundboard - even after a TS3 client restart. Re-arming a\n"
        "specific button via Set Hotkey lifts the block for that one."), this));
    resetRow->addStretch(1);
    hotkeyLay->addLayout(resetRow);

    // Logging: opt-in. Default off so a fresh install never writes
    // rpsb_debug.log. Toggling here flips the in-process gate immediately
    // (see ConfigModel::setLogsEnabled).
    auto *logBox = new QGroupBox(tr("Logging"), this);
    auto *logLay = new QVBoxLayout(logBox);
    logLay->addLayout(checkRow(m_logsEnabled, tr(
        "Writes a debug log file (rpsb_debug.log) inside your TeamSpeak\n"
        "config folder. Useful when reporting bugs - leave OFF otherwise\n"
        "to save disk space and skip every disk write."), this));

    // Audio sandbox: gates the new per-channel DSP feature plus the
    // dual-channel cyan meter on each Channel widget.
    auto *sandboxBox = new QGroupBox(tr("Audio sandbox"), this);
    auto *sandboxLay = new QVBoxLayout(sandboxBox);
    sandboxLay->addLayout(checkRow(m_sandboxEnabled, tr(
        "Master switch for the per-channel Audio Sandbox button. When OFF\n"
        "the sandbox button is hidden on every channel and the DSP chain\n"
        "is fully bypassed (zero CPU cost). Saved per-channel settings\n"
        "are preserved so you can re-enable later."), this));
    sandboxLay->addLayout(checkRow(m_meterVisible, tr(
        "Render the dual L/R peak meter (cyan; turns red on clipping)\n"
        "directly on each channel. Pure visual - turn off if you don't\n"
        "want the meter eating header space."), this));
    m_resetAllSandbox->setStyleSheet(
        "QPushButton { background-color: #c63131; color: white;"
        " border: 1px solid #7c1c1c; border-radius: 5px; padding: 4px 12px; }"
        "QPushButton:hover { background-color: #e04141; }");
    m_resetAllSandbox->setToolTip(tr(
        "Wipe every per-channel sandbox setting on every profile. The\n"
        "spatial / EQ / reverb dialogs go back to factory defaults."));
    auto *resetSandboxRow = new QHBoxLayout;
    resetSandboxRow->addWidget(m_resetAllSandbox);
    resetSandboxRow->addStretch(1);
    sandboxLay->addLayout(resetSandboxRow);

    // Profiles - 4 separate, switchable button-grid configurations.
    for (int i = 0; i < NUM_CONFIGS; ++i)
        m_profileCombo->addItem(tr("Profile %1").arg(i + 1), i);
    auto *profileBox = new QGroupBox(tr("Profiles"), this);
    auto *profileLay = new QHBoxLayout(profileBox);
    profileLay->addWidget(new QLabel(tr("Active:")));
    profileLay->addWidget(m_profileCombo, 1);
    profileLay->addWidget(m_profileExport);
    profileLay->addWidget(m_profileImport);
    profileLay->addWidget(new HelpBubble(tr(
        "Each soundboard ships 4 independent profiles - separate sets of\n"
        "buttons / hotkeys / grid sizes. Switch via the dropdown.\n"
        "Export Profile saves only the active profile (legacy .ini\n"
        "format compatible with old builds). Import Profile loads such\n"
        "a file into the active slot, leaving the other 3 untouched."), this));

    // Custom theme - checkable group with two color pickers. Toggling
    // the group or picking a color emits themeChanged immediately so the
    // wiring layer can refresh stylesheets across every open dialog.
    m_themeGroup = new QGroupBox(tr("Custom theme"), this);
    m_themeGroup->setCheckable(true);
    m_themeGroup->setChecked(false);
    auto *themeOuter = new QVBoxLayout(m_themeGroup);
    auto *themeLay = new QHBoxLayout;
    themeOuter->addLayout(themeLay);
    themeLay->addWidget(new QLabel(tr("Background:")));
    m_themeBgBtn->setFixedSize(28, 22);
    m_themeBgBtn->setToolTip(tr(
        "Window background. Drives all neutral surfaces (buttons, inputs,\n"
        "borders, hovers) plus auto-contrasted text. Pick any color and\n"
        "the entire UI retints around it in real time."));
    themeLay->addWidget(m_themeBgBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Accent:")));
    m_themeAccentBtn->setFixedSize(28, 22);
    m_themeAccentBtn->setToolTip(tr(
        "Accent color: applied to slider fill, focus rings, button-checked\n"
        "highlights, help-bubble background, etc."));
    themeLay->addWidget(m_themeAccentBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Waveform:")));
    m_themeWaveBtn->setFixedSize(28, 22);
    m_themeWaveBtn->setToolTip(tr(
        "Inner color of the channel waveform paint."));
    themeLay->addWidget(m_themeWaveBtn);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Text:")));
    m_themeTextBtn->setFixedSize(28, 22);
    m_themeTextBtn->setToolTip(tr(
        "Override the text color. By default the soundboard auto-picks\n"
        "light or dark text against your background; pick a color here\n"
        "to force a specific shade. Click Auto to revert to the\n"
        "automatic contrast."));
    themeLay->addWidget(m_themeTextBtn);
    m_themeTextAuto->setToolTip(tr("Revert text color to auto-contrast."));
    m_themeTextAuto->setMaximumWidth(56);
    m_themeTextAuto->setFixedHeight(22);
    themeLay->addWidget(m_themeTextAuto);
    themeLay->addSpacing(12);
    themeLay->addWidget(new QLabel(tr("Buttons:")));
    m_themeButtonBtn->setFixedSize(28, 22);
    m_themeButtonBtn->setToolTip(tr(
        "Override the default button background color (soundboard cells,\n"
        "FX panel buttons, slider grooves). Click Auto to revert to the\n"
        "auto-derived shade."));
    themeLay->addWidget(m_themeButtonBtn);
    m_themeButtonAuto->setToolTip(tr("Revert button color to auto-derived."));
    m_themeButtonAuto->setMaximumWidth(56);
    m_themeButtonAuto->setFixedHeight(22);
    themeLay->addWidget(m_themeButtonAuto);
    themeLay->addStretch(1);
    themeLay->addWidget(new HelpBubble(tr(
        "Override the soundboard's default colors. Background drives all\n"
        "greys (buttons, inputs, borders) and contrast-flips the text\n"
        "automatically. Accent retints sliders / focus rings / checked\n"
        "buttons. Waveform sets the channel waveform fill. Every change\n"
        "applies live across the whole UI - no reopen needed."), this));
    // Contrast slider: scales how strongly the auto-derived surfaces /
    // borders / hovers depart from the background. 0 = barely visible,
    // 100 = maximum separation.
    m_themeContrastSlider->setRange(0, 100);
    m_themeContrastSlider->setValue(50);
    m_themeContrastLabel->setMinimumWidth(40);
    m_themeContrastLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    auto *contrastRow = new QHBoxLayout;
    contrastRow->addWidget(new QLabel(tr("Contrast:")));
    contrastRow->addWidget(m_themeContrastSlider, 1);
    contrastRow->addWidget(m_themeContrastLabel);
    contrastRow->addWidget(new HelpBubble(tr(
        "Scales every auto-derived shade (button bg, hover, borders).\n"
        "Slide left for a subtle look (almost monochrome around your\n"
        "background), slide right for buttons that pop. Saved with\n"
        "the rest of the theme and included in the share string."), this));
    themeOuter->addLayout(contrastRow);
    auto *themeBtnRow = new QHBoxLayout;
    themeBtnRow->addWidget(m_themeResetBtn);
    themeBtnRow->addWidget(m_themeCopyBtn);
    themeBtnRow->addWidget(m_themePasteBtn);
    themeBtnRow->addStretch(1);
    themeBtnRow->addWidget(new HelpBubble(tr(
        "Reset: restore the default dark palette.\n"
        "Copy theme: write the current 3 colors + contrast to the\n"
        "clipboard as a single share string. Send it to a friend -\n"
        "they paste it via Paste theme to load the exact same look."), this));
    m_themeResetBtn->setToolTip(tr("Restore default colors (dark grey + blue accent)."));
    m_themeCopyBtn->setToolTip(tr("Copy a share string with the 3 current colors + contrast."));
    m_themePasteBtn->setToolTip(tr("Load a share string someone sent you and apply its theme."));
    themeOuter->addLayout(themeBtnRow);
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
        // Show the actual auto-derived color in the preview square so
        // the user sees what "Auto" resolved to instead of a misleading
        // grey placeholder.
        QColor preview = m_themeBackground.lightnessF() < 0.5
            ? QColor(0xec, 0xec, 0xec) : QColor(0x10, 0x10, 0x10);
        repaintColorButton(m_themeTextBtn, preview);
        emit themeChanged(m_themeGroup->isChecked(), m_themeAccent, m_themeWaveform, m_themeBackground, m_themeContrast, m_themeText, m_themeButton);
    });
    connect(m_themeButtonAuto, &QPushButton::clicked, this, [this, repaintColorButton]{
        m_themeButton = QColor();
        // Preview the actual auto-derived button colour from the active
        // theme so the swatch reflects what the user will see live.
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

    auto *ioBox = new QGroupBox(tr("Full configuration import / export"), this);
    auto *ioLay = new QHBoxLayout(ioBox);
    ioLay->addWidget(m_export);
    ioLay->addWidget(m_import);
    ioLay->addStretch(1);
    ioLay->addWidget(new HelpBubble(tr(
        "Export the entire configuration (every profile, every macro,\n"
        "every setting). Pick a .json file for the wrapped format or a\n"
        ".ini file for the legacy soundboard layout. Import auto-detects\n"
        "either kind from the file extension."), this));

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_close);

    auto *root = new QVBoxLayout(this);
    root->addWidget(audioBox);
    root->addWidget(channelsBox);
    root->addWidget(gridBox);
    root->addWidget(hotkeyBox);
    root->addWidget(logBox);
    root->addWidget(sandboxBox);
    root->addWidget(profileBox);
    root->addWidget(m_themeGroup);
    root->addWidget(ioBox);
    root->addStretch(1);
    root->addLayout(btnRow);

    connect(m_earrape,        &QCheckBox::toggled, this, &SettingsWindow::earrapeProtectionChanged);
    connect(m_linkVolumes,    &QCheckBox::toggled, this, &SettingsWindow::linkVolumesChanged);
    connect(m_rememberFx,     &QCheckBox::toggled, this, &SettingsWindow::rememberPitchSpeedChanged);
    connect(m_restoreSession, &QCheckBox::toggled, this, &SettingsWindow::restoreSessionChanged);
    connect(m_globalFx,       &QCheckBox::toggled, this, &SettingsWindow::globalFxEnabledChanged);
    connect(m_hideWaveform,   &QCheckBox::toggled, this, &SettingsWindow::hideWaveformChanged);
    connect(m_logsEnabled,    &QCheckBox::toggled, this, &SettingsWindow::logsEnabledChanged);
    connect(m_sandboxEnabled, &QCheckBox::toggled, this, &SettingsWindow::audioSandboxEnabledChanged);
    connect(m_meterVisible,   &QCheckBox::toggled, this, &SettingsWindow::audioMeterVisibleChanged);
    connect(m_resetAllSandbox,&QPushButton::clicked, this, &SettingsWindow::resetAllAudioSandboxRequested);
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
}

bool SettingsWindow::earrapeProtection()      const { return m_earrape->isChecked();        }
bool SettingsWindow::linkVolumes()            const { return m_linkVolumes->isChecked();    }
bool SettingsWindow::rememberPitchSpeed()     const { return m_rememberFx->isChecked();     }
bool SettingsWindow::restoreSession()         const { return m_restoreSession->isChecked(); }
bool SettingsWindow::globalFxEnabled()        const { return m_globalFx->isChecked();        }
bool SettingsWindow::hideWaveform()           const { return m_hideWaveform->isChecked();    }
bool SettingsWindow::logsEnabled()            const { return m_logsEnabled->isChecked();     }
bool SettingsWindow::audioSandboxEnabled()    const { return m_sandboxEnabled->isChecked();   }
bool SettingsWindow::audioMeterVisible()      const { return m_meterVisible->isChecked();     }
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

void SettingsWindow::setEarrapeProtection(bool on)     { QSignalBlocker b(m_earrape);        m_earrape->setChecked(on);        }
void SettingsWindow::setLinkVolumes(bool on)           { QSignalBlocker b(m_linkVolumes);    m_linkVolumes->setChecked(on);    }
void SettingsWindow::setRememberPitchSpeed(bool on)    { QSignalBlocker b(m_rememberFx);     m_rememberFx->setChecked(on);     }
void SettingsWindow::setRestoreSession(bool on)        { QSignalBlocker b(m_restoreSession); m_restoreSession->setChecked(on); }
void SettingsWindow::setGlobalFxEnabled(bool on)       { QSignalBlocker b(m_globalFx);        m_globalFx->setChecked(on);        }
void SettingsWindow::setHideWaveform(bool on)          { QSignalBlocker b(m_hideWaveform);    m_hideWaveform->setChecked(on);    }
void SettingsWindow::setLogsEnabled(bool on)           { QSignalBlocker b(m_logsEnabled);     m_logsEnabled->setChecked(on);     }
void SettingsWindow::setAudioSandboxEnabled(bool on)   { QSignalBlocker b(m_sandboxEnabled);  m_sandboxEnabled->setChecked(on);  }
void SettingsWindow::setAudioMeterVisible(bool on)     { QSignalBlocker b(m_meterVisible);    m_meterVisible->setChecked(on);    }
void SettingsWindow::setActiveProfile(int p)           { QSignalBlocker b(m_profileCombo);    if (p >= 0 && p < m_profileCombo->count()) m_profileCombo->setCurrentIndex(p); }

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
void SettingsWindow::setMultiSoundboard(bool on)       { QSignalBlocker b(m_multi);          m_multi->setChecked(on);          }
void SettingsWindow::setMuteLocally(bool on)           { QSignalBlocker b(m_muteLocally);    m_muteLocally->setChecked(on);    }
void SettingsWindow::setMuteMyself(bool on)            { QSignalBlocker b(m_muteMyself);     m_muteMyself->setChecked(on);     }
void SettingsWindow::setShowHotkeysOnButtons(bool on)  { QSignalBlocker b(m_showHotkeys);    m_showHotkeys->setChecked(on);    }
void SettingsWindow::setDisableHotkeys(bool on)        { QSignalBlocker b(m_disableHotkeys); m_disableHotkeys->setChecked(on); }
void SettingsWindow::setRows(int r)                    { QSignalBlocker b(m_rows);           m_rows->setValue(r);              }
void SettingsWindow::setCols(int c)                    { QSignalBlocker b(m_cols);           m_cols->setValue(c);              }
