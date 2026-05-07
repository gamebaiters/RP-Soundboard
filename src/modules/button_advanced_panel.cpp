#include "button_advanced_panel.h"
#include "fx_panel.h"
#include "fine_slider.h"
#include "../style_helper.h"
#include "theme.h"
#include "../soundview_qt.h"
#include "../samples.h"
#include "../main.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QFileDialog>
#include <QColorDialog>
#include <QFileInfo>
#include <QFrame>
#include <QTimer>
#include <QCloseEvent>
#include <cmath>

ButtonAdvancedPanel::ButtonAdvancedPanel(QWidget *parent)
    : QDialog(parent)
    , m_filePath(new QLineEdit(this))
    , m_browse(new QPushButton(tr("Browse..."), this))
    , m_soundView(new SoundView(this))
    , m_preview(new QPushButton(tr("Preview"), this))
    , m_previewTimeLabel(new QLabel("0:00 / 0:00", this))
    , m_previewTimer(new QTimer(this))
    , m_customText(new QLineEdit(this))
    , m_customColorEnabled(new QCheckBox(tr("Custom color"), this))
    , m_colorSwatch(new QFrame(this))
    , m_pickColor(new QPushButton(tr("Pick..."), this))
    , m_imagePath(new QLineEdit(this))
    , m_imageBrowse(new QPushButton(tr("Browse..."), this))
    , m_imageClear(new QPushButton(tr("Clear"), this))
    , m_volume(new FineSlider(Qt::Horizontal, this))
    , m_volumeLabel(new QLabel("0 dB", this))
    , m_cropGroup(nullptr)
    , m_cropStart(new QSpinBox(this))
    , m_cropStartUnit(new QComboBox(this))
    , m_cropStopMode(new QComboBox(this))
    , m_cropStop(new QSpinBox(this))
    , m_cropStopUnit(new QComboBox(this))
    , m_fxGroup(nullptr)
    , m_fx(new FxPanel(this))
    , m_hotkeyBtn(new QPushButton(tr("Set hotkey..."), this))
    , m_hotkeyReset(new QPushButton(tr("Reset hotkey"), this))
    , m_ok(new QPushButton(tr("OK"), this))
    , m_cancel(new QPushButton(tr("Cancel"), this))
{
    setWindowTitle(tr("Button Options"));
    setModal(false);
    setProperty("isGBSoundboard", true);
    resize(640, 800);
    setMinimumWidth(560);

    m_volume->setRange(-30, 30);
    m_volume->setSingleStep(1);
    m_volume->setPageStep(5);
    m_volume->setValue(0);

    m_cropStart->setRange(0, 60 * 60 * 24);
    m_cropStop->setRange(0, 60 * 60 * 24);
    m_cropStartUnit->addItems({tr("ms"), tr("s")});
    m_cropStartUnit->setCurrentIndex(1);
    m_cropStopUnit->addItems({tr("ms"), tr("s")});
    m_cropStopUnit->setCurrentIndex(1);
    m_cropStopMode->addItems({tr("after"), tr("at")});

    // File group: path + browse on top, waveform preview below + Preview button
    auto *fileBox = new QGroupBox(tr("Sound file"), this);
    auto *fileLay = new QVBoxLayout(fileBox);
    auto *pathRow = new QHBoxLayout;
    pathRow->addWidget(m_filePath, 1);
    pathRow->addWidget(m_browse);
    fileLay->addLayout(pathRow);
    m_soundView->setMinimumHeight(60);
    m_soundView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    fileLay->addWidget(m_soundView);
    auto *previewRow = new QHBoxLayout;
    m_preview->setIcon(QIcon(":/icon/img/playarrow_32.png"));
    m_preview->setIconSize(QSize(16, 16));
    previewRow->addWidget(m_preview);
    previewRow->addStretch(1);
    m_previewTimeLabel->setMinimumWidth(80);
    m_previewTimeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    previewRow->addWidget(m_previewTimeLabel);
    fileLay->addLayout(previewRow);

    auto *displayBox = new QGroupBox(tr("Display"), this);
    auto *dispForm = new QFormLayout(displayBox);
    dispForm->addRow(tr("Custom text"), m_customText);
    // Swatch sits between the checkbox and Pick button so the user always
    // sees the active color at a glance, even before reopening the dialog.
    m_colorSwatch->setFixedSize(24, 22);
    m_colorSwatch->setFrameShape(QFrame::StyledPanel);
    m_colorSwatch->setToolTip(tr("Currently selected button color."));
    auto *colorRow = new QHBoxLayout;
    colorRow->addWidget(m_customColorEnabled);
    colorRow->addWidget(m_colorSwatch);
    colorRow->addWidget(m_pickColor);
    colorRow->addStretch(1);
    dispForm->addRow(tr("Color"), colorRow);
    refreshColorSwatch();
    connect(m_customColorEnabled, &QCheckBox::toggled, this,
            [this](bool){ refreshColorSwatch(); });
    m_imagePath->setReadOnly(true);
    m_imagePath->setPlaceholderText(tr("(no image)"));
    auto *imgRow = new QHBoxLayout;
    imgRow->addWidget(m_imagePath, 1);
    imgRow->addWidget(m_imageBrowse);
    imgRow->addWidget(m_imageClear);
    dispForm->addRow(tr("Image"), imgRow);

    auto *volBox = new QGroupBox(tr("Volume modifier"), this);
    auto *volLay = new QHBoxLayout(volBox);
    volLay->addWidget(m_volume, 1);
    volLay->addWidget(m_volumeLabel);

    // Crop group is checkable - the title bar's checkbox toggles whether
    // crop applies at playback/preview. Qt auto-disables (grays out) all
    // children when the group is unchecked, so the user gets visual
    // confirmation that moving the spin boxes won't have any effect.
    m_cropGroup = new QGroupBox(tr("Crop"), this);
    m_cropGroup->setCheckable(true);
    m_cropGroup->setChecked(false);
    auto *cropLay = new QFormLayout(m_cropGroup);
    auto *startRow = new QHBoxLayout;
    startRow->addWidget(m_cropStart);
    startRow->addWidget(m_cropStartUnit);
    cropLay->addRow(tr("Start"), startRow);
    auto *stopRow = new QHBoxLayout;
    stopRow->addWidget(m_cropStopMode);
    stopRow->addWidget(m_cropStop);
    stopRow->addWidget(m_cropStopUnit);
    cropLay->addRow(tr("Stop"), stopRow);

    // FX group is checkable - per-button custom FX are written to disk
    // regardless (so the user's last sliders are remembered when they
    // toggle the group back on), but they are ONLY applied to playback /
    // preview when the group is checked. When unchecked, the channel's
    // current FX are used and persist between soundboards.
    m_fxGroup = new QGroupBox(tr("Custom FX (override channel)"), this);
    m_fxGroup->setCheckable(true);
    m_fxGroup->setChecked(false);
    auto *fxLay = new QVBoxLayout(m_fxGroup);
    fxLay->addWidget(m_fx);

    auto *hotkeyBox = new QGroupBox(tr("Hotkey"), this);
    auto *hotkeyLay = new QHBoxLayout(hotkeyBox);
    hotkeyLay->addWidget(m_hotkeyBtn);
    hotkeyLay->addWidget(m_hotkeyReset);
    hotkeyLay->addStretch(1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_ok);
    btnRow->addWidget(m_cancel);

    auto *root = new QVBoxLayout(this);
    root->addWidget(fileBox);
    root->addWidget(displayBox);
    root->addWidget(volBox);
    root->addWidget(m_cropGroup);
    root->addWidget(m_fxGroup);
    root->addWidget(hotkeyBox);
    root->addLayout(btnRow);

    connect(m_browse,    &QPushButton::clicked, this, &ButtonAdvancedPanel::onBrowse);
    connect(m_pickColor, &QPushButton::clicked, this, &ButtonAdvancedPanel::onPickColor);
    connect(m_imageBrowse, &QPushButton::clicked, this, [this]{
        QString p = QFileDialog::getOpenFileName(this, tr("Pick button image"),
            m_imagePath->text(),
            tr("Images (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;All files (*.*)"));
        if (!p.isEmpty()) m_imagePath->setText(p);
    });
    connect(m_imageClear, &QPushButton::clicked, this, [this]{ m_imagePath->clear(); });
    connect(m_volume,    &QSlider::valueChanged, this, [this](int v){ m_volumeLabel->setText(QString("%1 dB").arg(v)); });
    connect(m_hotkeyBtn, &QPushButton::clicked, this, &ButtonAdvancedPanel::hotkeyAssignRequested);
    connect(m_hotkeyReset, &QPushButton::clicked, this, [this]{
        setHotkeyText(QString());
        emit hotkeyResetRequested();
    });
    connect(m_preview,   &QPushButton::clicked, this, &ButtonAdvancedPanel::onPreview);
    // Click on the preview waveform = seek. Clamped to [start, stop] when
    // crop is enabled so the user can never aim past the trimmed range.
    connect(m_soundView, &SoundView::seekRequested, this, [this](double frac){
        Sampler *sampler = sb_getSampler();
        if (!sampler || m_previewSlot < 0) return;
        double len = sampler->getLength(m_previewSlot);
        if (len <= 0.0) return;
        double target = frac * len;
        if (m_cropGroup->isChecked()) {
            SoundInfo s = soundInfo();
            double start = s.getStartTime();
            double playLen = s.getPlayTime();
            double stop = (playLen > 0.0) ? (start + playLen) : len;
            if (start > 0.0 && target < start) target = start;
            if (target > stop) target = stop;
        }
        sampler->seek(target, m_previewSlot);
    });
    connect(m_filePath,  &QLineEdit::textChanged, this, [this](const QString &){ refreshSoundView(); });
    connect(m_cropGroup,      &QGroupBox::toggled, this, [this](bool){ refreshSoundView(); });
    connect(m_cropStart,      QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int){ refreshSoundView(); });
    connect(m_cropStop,       QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int){ refreshSoundView(); });
    connect(m_cropStartUnit,  QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ refreshSoundView(); });
    connect(m_cropStopUnit,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ refreshSoundView(); });
    connect(m_cropStopMode,   QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int){ refreshSoundView(); });
    connect(m_previewTimer,   &QTimer::timeout, this, &ButtonAdvancedPanel::onPreviewTimer);
    connect(m_ok,     &QPushButton::clicked, this, &ButtonAdvancedPanel::onAccepted);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);

    // Live preview update: dragging the FX sliders or the volume slider
    // while a preview is running should be heard immediately instead of
    // forcing the user to restart preview every time.
    connect(m_fx, &FxPanel::pitchChanged,  this, [this](int){ pushLiveFxToPreview(); });
    connect(m_fx, &FxPanel::speedChanged,  this, [this](int){ pushLiveFxToPreview(); });
    connect(m_fx, &FxPanel::reverbChanged, this, [this](int){ pushLiveFxToPreview(); });
    connect(m_volume, &QSlider::valueChanged, this, [this](int){ pushLiveFxToPreview(); });
    // Toggling the Custom FX group during preview is ambiguous (slot
    // already opened with the previous flag) - safest is to stop the
    // preview so the user can restart it under the new FX semantics.
    connect(m_fxGroup,  &QGroupBox::toggled, this, [this](bool){ stopPreview(); });
    connect(m_cropGroup, &QGroupBox::toggled, this, [this](bool){ stopPreview(); });
}

void ButtonAdvancedPanel::setSoundInfo(const SoundInfo &info) {
    m_info = info;
    m_filePath->setText(info.filename);
    m_customText->setText(info.customText);
    m_customColorEnabled->setChecked(info.customColorEnabled());
    refreshColorSwatch();
    m_imagePath->setText(info.imagePath);
    m_volume->setValue(info.volume);
    m_volumeLabel->setText(QString("%1 dB").arg(info.volume));
    m_cropGroup->setChecked(info.cropEnabled);
    m_cropStart->setValue(info.cropStartValue);
    m_cropStartUnit->setCurrentIndex(info.cropStartUnit);
    m_cropStop->setValue(info.cropStopValue);
    m_cropStopUnit->setCurrentIndex(info.cropStopUnit);
    m_cropStopMode->setCurrentIndex(info.cropStopAfterAt);
    m_fxGroup->setChecked(info.fxRemember);
    m_fx->setPitch(info.fxPitch);
    m_fx->setSpeed(info.fxSpeed);
    m_fx->setReverb(info.fxReverb);
    m_fx->setSync(info.fxSyncPitchSpeed);
    refreshSoundView();
}

SoundInfo ButtonAdvancedPanel::soundInfo() const {
    SoundInfo s = m_info;
    s.filename         = m_filePath->text();
    s.customText       = m_customText->text();
    s.setCustomColorEnabled(m_customColorEnabled->isChecked());
    s.volume           = m_volume->value();
    s.cropEnabled      = m_cropGroup->isChecked();
    s.cropStartValue   = m_cropStart->value();
    s.cropStartUnit    = m_cropStartUnit->currentIndex();
    s.cropStopValue    = m_cropStop->value();
    s.cropStopUnit     = m_cropStopUnit->currentIndex();
    s.cropStopAfterAt  = m_cropStopMode->currentIndex();
    s.fxPitch          = m_fx->pitch();
    s.fxSpeed          = m_fx->speed();
    s.fxReverb         = m_fx->reverb();
    s.fxSyncPitchSpeed = m_fx->sync();
    // fxRemember = "use these per-button FX every time this button plays".
    // When false, slider values stay on disk but are ignored at playback.
    s.fxRemember       = m_fxGroup->isChecked();
    s.imagePath        = m_imagePath->text();
    return s;
}

void ButtonAdvancedPanel::setHotkeyText(const QString &shortcut) {
    m_hotkeyBtn->setText(shortcut.isEmpty() ? tr("Set hotkey...") : tr("Hotkey: %1").arg(shortcut));
}

void ButtonAdvancedPanel::setGlobalFxEnabled(bool on) {
    m_fxGroup->setVisible(on);
    if (!on) {
        // Force the per-button FX off so onAccepted writes fxRemember=false
        // - matching the master switch's intent (no FX applied anywhere).
        QSignalBlocker b(m_fxGroup);
        m_fxGroup->setChecked(false);
    }
}

void ButtonAdvancedPanel::onBrowse() {
    QString path = QFileDialog::getOpenFileName(this, tr("Choose sound file"), m_filePath->text(),
        tr("Audio (*.mp3 *.wav *.flac *.ogg *.opus *.aac *.m4a);;All files (*.*)"));
    if (!path.isEmpty()) m_filePath->setText(path);
}

void ButtonAdvancedPanel::onPickColor() {
    QColor seed = m_info.customColor.isValid() && m_info.customColor.alpha() != 0
        ? m_info.customColor : QColor(Qt::white);
    QColor c = QColorDialog::getColor(seed, this, tr("Button color"));
    if (c.isValid()) {
        m_info.customColor = c;
        m_customColorEnabled->setChecked(true);
        refreshColorSwatch();
    }
}

void ButtonAdvancedPanel::refreshColorSwatch() {
    bool on = m_customColorEnabled->isChecked();
    QColor c = m_info.customColor;
    QString rgb = (on && c.isValid())
        ? QString("rgb(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue())
        : QStringLiteral("transparent");
    QString border = on ? QStringLiteral("#888888") : QStringLiteral("#3a3a3a");
    // QFrame doesn't honor border-radius without a border-style set, hence the
    // 1px solid border. Diagonal hatch when disabled gives a clear "no color".
    if (!on) {
        m_colorSwatch->setStyleSheet(QString(
            "QFrame { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,"
            " stop:0 #2a2a2a, stop:0.49 #2a2a2a, stop:0.5 #555555,"
            " stop:0.51 #2a2a2a, stop:1 #2a2a2a); border: 1px solid %1;"
            " border-radius: 3px; }").arg(border));
    } else {
        m_colorSwatch->setStyleSheet(QString(
            "QFrame { background-color: %1; border: 1px solid %2;"
            " border-radius: 3px; }").arg(rgb, border));
    }
}

void ButtonAdvancedPanel::writeBackToInfo() { m_info = soundInfo(); }

void ButtonAdvancedPanel::onAccepted() {
    Sampler *sampler = sb_getSampler();
    if (sampler && m_previewSlot >= 0)
        sampler->stopPlayback(m_previewSlot);
    m_previewSlot = -1;
    m_previewTimer->stop();
    writeBackToInfo();
    emit soundInfoAccepted(m_info);
    accept();
}

static QString fmtTime(double seconds) {
    if (seconds < 0.0 || !std::isfinite(seconds)) seconds = 0.0;
    int s = static_cast<int>(seconds);
    int m = s / 60;
    s = s % 60;
    return QString::asprintf("%d:%02d", m, s);
}

void ButtonAdvancedPanel::onPreview() {
    Sampler *sampler = sb_getSampler();
    if (!sampler) return;
    // Toggle: a previously-started preview from this dialog is stopped.
    if (m_previewSlot >= 0 && sampler->getState(m_previewSlot) == Sampler::ePLAYING_PREVIEW) {
        sampler->stopPlayback(m_previewSlot);
        m_previewSlot = -1;
        m_preview->setText(tr("Preview"));
        m_preview->setIcon(QIcon(":/icon/img/playarrow_32.png"));
        m_previewTimer->stop();
        m_soundView->setPlaybackPosition(0.0);
        m_previewTimeLabel->setText("0:00 / 0:00");
        return;
    }
    SoundInfo s = soundInfo();
    if (s.filename.isEmpty()) return;
    // Reserve the LAST sampler slot for previews so they never collide
    // with channel slots [0..MAX_SLOTS-2]. Stop anything in that slot
    // first so a stale preview can't outlive the dialog.
    const int previewSlot = Sampler::MAX_SLOTS - 1;
    sampler->stopPlayback(previewSlot);
    if (!sampler->playSoundInSlot(previewSlot, s, true)) return;
    m_previewSlot = previewSlot;
    // Preview must NOT inherit the global / channel volume + FX state -
    // it stands alone, only the dialog's own widgets drive it. Force
    // neutral local/remote volume (sound.volume modifier still applies)
    // and let pushLiveFxToPreview write 1.0 / 0.0 factors when the
    // Custom FX group is unchecked.
    sampler->setSlotVolumeLocal (previewSlot, 100);
    sampler->setSlotVolumeRemote(previewSlot, 100);
    pushLiveFxToPreview();
    m_preview->setText(tr("Stop preview"));
    m_preview->setIcon(QIcon(":/icon/img/stoparrow_32.png"));
    m_previewTimer->start(100);
}

void ButtonAdvancedPanel::onPreviewTimer() {
    Sampler *sampler = sb_getSampler();
    if (!sampler || m_previewSlot < 0) return;
    if (sampler->getState(m_previewSlot) != Sampler::ePLAYING_PREVIEW) {
        m_preview->setText(tr("Preview"));
        m_preview->setIcon(QIcon(":/icon/img/playarrow_32.png"));
        m_previewTimer->stop();
        m_soundView->setPlaybackPosition(0.0);
        m_previewTimeLabel->setText("0:00 / 0:00");
        m_previewSlot = -1;
        return;
    }
    double pos = sampler->getPosition(m_previewSlot);
    double len = sampler->getLength(m_previewSlot);
    if (len > 0.0) m_soundView->setPlaybackPosition(pos / len);
    m_previewTimeLabel->setText(fmtTime(pos) + " / " + fmtTime(len));
}

void ButtonAdvancedPanel::closeEvent(QCloseEvent *e) {
    stopPreview();
    QDialog::closeEvent(e);
}

void ButtonAdvancedPanel::done(int r) {
    // Covers reject() / accept() / setResult paths - the X button on the
    // window frame fires closeEvent above, but Cancel only goes through
    // done() so we needed a hook here too.
    stopPreview();
    QDialog::done(r);
}

void ButtonAdvancedPanel::stopPreview() {
    Sampler *sampler = sb_getSampler();
    if (sampler && m_previewSlot >= 0)
        sampler->stopPlayback(m_previewSlot);
    m_previewSlot = -1;
    if (m_previewTimer) m_previewTimer->stop();
    m_preview->setText(tr("Preview"));
    m_preview->setIcon(QIcon(":/icon/img/playarrow_32.png"));
    m_soundView->setPlaybackPosition(0.0);
    m_previewTimeLabel->setText("0:00 / 0:00");
}

void ButtonAdvancedPanel::pushLiveFxToPreview() {
    Sampler *sampler = sb_getSampler();
    if (!sampler || m_previewSlot < 0) return;
    if (sampler->getState(m_previewSlot) != Sampler::ePLAYING_PREVIEW) return;
    // Always update the per-sound dB modifier so the Volume slider takes
    // effect mid-preview without restarting playback.
    sampler->setSlotSoundDb(m_previewSlot, static_cast<double>(m_volume->value()));
    // When the Custom FX group is OFF the preview must mirror the file
    // as-is - so push neutral factors regardless of what the (disabled)
    // sliders happen to show.
    if (m_fxGroup->isChecked()) {
        sampler->setSlotPitchFactor(m_previewSlot, static_cast<float>(std::pow(3.0, m_fx->pitch()  / 100.0)));
        sampler->setSlotSpeedFactor(m_previewSlot, static_cast<float>(std::pow(3.0, m_fx->speed()  / 100.0)));
        sampler->setSlotReverbMix  (m_previewSlot, m_fx->reverb() / 100.0f);
    } else {
        sampler->setSlotPitchFactor(m_previewSlot, 1.0f);
        sampler->setSlotSpeedFactor(m_previewSlot, 1.0f);
        sampler->setSlotReverbMix  (m_previewSlot, 0.0f);
    }
}

void ButtonAdvancedPanel::refreshSoundView() {
    SoundInfo s = soundInfo();
    m_soundView->setSound(s);
    m_soundView->update();
}
