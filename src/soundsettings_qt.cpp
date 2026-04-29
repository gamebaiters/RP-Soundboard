// src/soundsettings_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------



#include "ui_soundsettings_qt.h"
#include "soundsettings_qt.h"
#include "ConfigModel.h"
#include "main.h"
#include "samples.h"
#include "soundview_qt.h"
#include <QFileDialog>
#include <QPainter>
#include <QFileInfo>
#include <QGridLayout>
#include "config_qt.h"
#include <QColorDialog>
#include <QPushButton>
#include <cmath>
#include "style_helper.h"


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SoundSettingsQt::SoundSettingsQt(const SoundInfo &soundInfo, size_t buttonId, QWidget *parent /*= 0*/) :
	QDialog(parent),
	ui(new Ui::SoundSettingsQt),
	m_soundInfo(soundInfo),
	m_buttonId(buttonId),
	m_iconPlay(":/icon/img/playarrow_32.png"),
	m_iconStop(":/icon/img/stoparrow_32.png")
{
	m_soundview = new SoundView(this);
	m_soundview->setObjectName(QStringLiteral("soundview"));
	m_soundview->setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding));
	m_soundview->setMinimumSize(QSize(0, 30));

	ui->setupUi(this);
	this->setStyleSheet(StyleHelper::loadDarkStyle());
	ui->startSoundUnitCombo->addItem("milliseconds");
	ui->startSoundUnitCombo->addItem("seconds");
	ui->stopSoundUnitCombo->addItem("milliseconds");
	ui->stopSoundUnitCombo->addItem("seconds");
	ui->stopSoundAtAfterCombo->addItem("after");
	ui->stopSoundAtAfterCombo->addItem("at");

	// Per-song FX section — checkable group (like Crop Sound)
	m_fxGroup = new QGroupBox("Custom FX", this);
	m_fxGroup->setCheckable(true);
	m_fxGroup->setChecked(m_soundInfo.fxRemember);
	QHBoxLayout *fxMainRow = new QHBoxLayout(m_fxGroup);
	fxMainRow->setContentsMargins(6, 6, 6, 6);
	fxMainRow->setSpacing(6);

	auto createFxCol = [](const QString &title, QSlider *&slider, QLabel *&label,
						   int minVal, int maxVal, int defVal) -> QVBoxLayout*
	{
		QVBoxLayout *col = new QVBoxLayout();
		col->setSpacing(2);
		col->setAlignment(Qt::AlignHCenter);
		QLabel *t = new QLabel(title);
		t->setAlignment(Qt::AlignHCenter);
		QFont f = t->font(); f.setPointSize(10); t->setFont(f);
		col->addWidget(t, 0, Qt::AlignHCenter);
		slider = new QSlider(Qt::Vertical);
		slider->setRange(minVal, maxVal);
		slider->setValue(defVal);
		slider->setTickPosition(QSlider::TicksBothSides);
		slider->setTickInterval(25);
		slider->setMinimumHeight(70);
		slider->setMaximumHeight(80);
		slider->setFixedWidth(30);
		col->addWidget(slider, 0, Qt::AlignHCenter);
		label = new QLabel("");
		label->setAlignment(Qt::AlignHCenter);
		QFont vf = label->font(); vf.setPointSize(9); label->setFont(vf);
		label->setMinimumWidth(35);
		col->addWidget(label, 0, Qt::AlignHCenter);
		return col;
	};

	fxMainRow->addLayout(createFxCol("Pitch", m_fxPitchSlider, m_fxPitchLabel, -100, 100, m_soundInfo.fxPitch));
	fxMainRow->addLayout(createFxCol("Speed", m_fxSpeedSlider, m_fxSpeedLabel, -100, 100, m_soundInfo.fxSpeed));
	fxMainRow->addLayout(createFxCol("P+S", m_fxCombinedSlider, m_fxCombinedLabel, -100, 100, m_soundInfo.fxPitch));
	fxMainRow->addLayout(createFxCol("Reverb", m_fxReverbSlider, m_fxReverbLabel, 0, 100, m_soundInfo.fxReverb));

	// Sync/Reset column
	QVBoxLayout *fxBtnCol = new QVBoxLayout();
	fxBtnCol->setSpacing(4);
	fxBtnCol->setAlignment(Qt::AlignVCenter);
	m_fxSyncButton = new QPushButton("Sync");
	m_fxSyncButton->setCheckable(true);
	m_fxSyncButton->setChecked(m_soundInfo.fxSyncPitchSpeed);
	m_fxSyncButton->setFixedWidth(50);
	m_fxSyncButton->setToolTip("Link Pitch and Speed to the P+S slider");
	fxBtnCol->addWidget(m_fxSyncButton, 0, Qt::AlignHCenter);
	m_fxResetButton = new QPushButton("Reset");
	m_fxResetButton->setFixedWidth(50);
	m_fxResetButton->setToolTip("Reset all FX to defaults");
	fxBtnCol->addWidget(m_fxResetButton, 0, Qt::AlignHCenter);
	fxMainRow->addLayout(fxBtnCol);

	// Sync state: enable/disable sliders
	bool sync = m_soundInfo.fxSyncPitchSpeed;
	m_fxCombinedSlider->setEnabled(sync);
	m_fxPitchSlider->setEnabled(!sync);
	m_fxSpeedSlider->setEnabled(!sync);
	updateFxLabels();

	connect(m_fxPitchSlider, &QSlider::valueChanged, this, &SoundSettingsQt::onFxPitchChanged);
	connect(m_fxSpeedSlider, &QSlider::valueChanged, this, &SoundSettingsQt::onFxSpeedChanged);
	connect(m_fxCombinedSlider, &QSlider::valueChanged, this, &SoundSettingsQt::onFxCombinedChanged);
	connect(m_fxReverbSlider, &QSlider::valueChanged, this, &SoundSettingsQt::onFxReverbChanged);
	connect(m_fxSyncButton, &QPushButton::toggled, this, &SoundSettingsQt::onFxSyncToggled);
	connect(m_fxResetButton, &QPushButton::clicked, this, &SoundSettingsQt::onFxReset);

	// Insert FX group before the dialog button box
	QVBoxLayout *mainLayout = qobject_cast<QVBoxLayout*>(this->layout());
	if (mainLayout)
		mainLayout->insertWidget(mainLayout->count() - 1, m_fxGroup);
	else
		this->layout()->addWidget(m_fxGroup);

	connect(ui->soundVolumeSlider, SIGNAL(valueChanged(int)), this, SLOT(onVolumeChanged(int)));
	connect(ui->filenameBrowseButton, SIGNAL(released()), this, SLOT(onBrowsePressed()));
	connect(ui->previewSoundButton, SIGNAL(released()), this, SLOT(onPreviewPressed()));
	connect(ui->hotkeyChangeButton, SIGNAL(clicked()), this, SLOT(onHotkeyChangePressed()));
	connect(ui->colorCheckBox, SIGNAL(clicked()), this, SLOT(onColorEnabledPressed()));
	connect(ui->colorButton, SIGNAL(clicked()), this, SLOT(onChooseColorPressed()));
	connect(parent, SIGNAL(hotkeyRecordedEvent(QString,QString)), this, SLOT(updateHotkeyText()));
	connect(ui->startSoundUnitCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSoundView()));
	connect(ui->stopSoundUnitCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSoundView()));
	connect(ui->stopSoundAtAfterCombo, SIGNAL(currentIndexChanged(int)), this, SLOT(updateSoundView()));
	connect(ui->startSoundValueSpin, SIGNAL(valueChanged(int)), this, SLOT(updateSoundView()));
	connect(ui->stopSoundValueSpin, SIGNAL(valueChanged(int)), this, SLOT(updateSoundView()));
	connect(ui->groupCrop, SIGNAL(clicked(bool)), this, SLOT(updateSoundView()));
	initGui(m_soundInfo);

	m_timer = new QTimer(this);
	connect(m_timer, SIGNAL(timeout()), this, SLOT(onTimer()));

	ui->groupCrop->layout()->addWidget(m_soundview);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::initGui(const SoundInfo &sound)
{
	ui->filenameEdit->setText(sound.filename);
	ui->customTextEdit->setPlaceholderText(QFileInfo(sound.filename).baseName());
	ui->customTextEdit->setText(sound.customText);
	ui->soundVolumeSlider->setValue(sound.volume);
	ui->groupCrop->setChecked(sound.cropEnabled);
	ui->startSoundValueSpin->setValue(sound.cropStartValue);
	ui->startSoundUnitCombo->setCurrentIndex(sound.cropStartUnit);
	ui->stopSoundAtAfterCombo->setCurrentIndex(sound.cropStopAfterAt);
	ui->stopSoundValueSpin->setValue(sound.cropStopValue);
	ui->stopSoundUnitCombo->setCurrentIndex(sound.cropStopUnit);
	ui->colorCheckBox->setChecked(sound.customColorEnabled());
	ui->colorButton->setEnabled(sound.customColorEnabled());
	ui->colorButton->setStyleSheet(QString("background-color: %1").arg(sound.customColor.name()));
	this->customColor = sound.customColor;
	
	updateHotkeyText();

	m_soundview->setSound(sound);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::fillFromGui(SoundInfo &sound)
{
	sound.filename = ui->filenameEdit->text();
	sound.customText = ui->customTextEdit->text();
	sound.volume = ui->soundVolumeSlider->value();
	sound.cropEnabled = ui->groupCrop->isChecked();
	sound.cropStartValue = ui->startSoundValueSpin->value();
	sound.cropStartUnit = ui->startSoundUnitCombo->currentIndex();
	sound.cropStopAfterAt = ui->stopSoundAtAfterCombo->currentIndex();
	sound.cropStopValue = ui->stopSoundValueSpin->value();
	sound.cropStopUnit = ui->stopSoundUnitCombo->currentIndex();
	sound.customColor = this->customColor;
	sound.fxPitch = m_fxPitchSlider->value();
	sound.fxSpeed = m_fxSpeedSlider->value();
	sound.fxReverb = m_fxReverbSlider->value();
	sound.fxSyncPitchSpeed = m_fxSyncButton->isChecked();
	sound.fxRemember = m_fxGroup->isChecked();
	// Legacy compat: compute combined factor from pitch
	sound.fxPitchSpeed = (float)pow(3.0, sound.fxPitch / 100.0);
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
SoundSettingsQt::~SoundSettingsQt()
{
	delete ui;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::done( int r )
{
	Sampler *sampler = sb_getSampler();
	if(sampler->getState() == Sampler::ePLAYING_PREVIEW)
		sampler->stopPlayback();
	fillFromGui(m_soundInfo);
	QDialog::done(r);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::onVolumeChanged(int value)
{
	ui->soundVolumeDbLabel->setText(QString("%1%2 dB").arg(value > 0 ? "+" : "", QString::number(value)));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::onBrowsePressed()
{
	QString filePath = ui->filenameEdit->text();
	QString fn = QFileDialog::getOpenFileName(this, tr("Choose File"), filePath, tr("Files (*.*)"));
	if(!fn.isNull())
	{
		ui->filenameEdit->setText(fn);
		ui->customTextEdit->setPlaceholderText(QFileInfo(fn).baseName());

		SoundInfo info;
		fillFromGui(info);
		m_soundview->setSound(info);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::onPreviewPressed()
{
	Sampler *sampler = sb_getSampler();
	if(sampler->getState() != Sampler::ePLAYING_PREVIEW)
	{
		SoundInfo sound;
		fillFromGui(sound);
		if(sampler->playPreview(sound))
		{
			// Apply per-sound FX to the preview slot (always, so user hears what they set)
			applyFxToPreview();
			ui->previewSoundButton->setIcon(m_iconStop);
			m_timer->start(100);
		}
	}
	else
	{
		sampler->stopPlayback();
		m_soundview->setPlaybackPosition(0.0);
		ui->previewSoundButton->setIcon(m_iconPlay);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::onTimer()
{
	Sampler *sampler = sb_getSampler();
	if(sampler->getState() != Sampler::ePLAYING_PREVIEW)
	{
		ui->previewSoundButton->setIcon(m_iconPlay);
		m_soundview->setPlaybackPosition(0.0);
		m_timer->stop();
		return;
	}

	// Update waveform position during preview
	int slot = getPreviewSlot();
	if (slot >= 0)
	{
		double pos = sampler->getPosition(slot);
		double len = sampler->getLength(slot);
		if (len > 0.0)
			m_soundview->setPlaybackPosition(pos / len);
	}
}


int SoundSettingsQt::getPreviewSlot()
{
	Sampler *sampler = sb_getSampler();
	if (!sampler) return -1;
	return sampler->findSlotByState(Sampler::ePLAYING_PREVIEW);
}


void SoundSettingsQt::applyFxToPreview()
{
	Sampler *sampler = sb_getSampler();
	if (!sampler || sampler->getState() != Sampler::ePLAYING_PREVIEW)
		return;
	int slot = getPreviewSlot();
	if (slot < 0) return;

	float pitchFactor = (float)pow(3.0, m_fxPitchSlider->value() / 100.0);
	float speedFactor = (float)pow(3.0, m_fxSpeedSlider->value() / 100.0);
	float reverbMix = m_fxReverbSlider->value() / 100.0f;
	sampler->setSlotPitchFactor(slot, pitchFactor);
	sampler->setSlotSpeedFactor(slot, speedFactor);
	sampler->setSlotReverbMix(slot, reverbMix);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::onHotkeyChangePressed()
{
	ConfigQt::openHotkeySetDialog(m_buttonId, this);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void SoundSettingsQt::updateHotkeyText()
{
	QString hotkeyText = ConfigQt::getShortcutString(m_buttonId);
	ui->hotkeyCurrentLabel->setText(QString("Current hotkey: ") + 
		(hotkeyText.isEmpty() ? QString("None") : hotkeyText));
}


void SoundSettingsQt::onColorEnabledPressed()
{
	customColor.setAlpha(ui->colorCheckBox->isChecked() ? 255 : 0);
	ui->colorButton->setEnabled(ui->colorCheckBox->isChecked());
}


void SoundSettingsQt::onChooseColorPressed()
{
	int alpha = customColor.alpha();
	customColor = QColorDialog::getColor(customColor, this, "Custom button color");
	customColor.setAlpha(alpha);
	ui->colorButton->setStyleSheet(QString("background-color: %1").arg(customColor.name()));
}


void SoundSettingsQt::updateSoundView()
{
	SoundInfo info;
	fillFromGui(info);
	m_soundview->setSound(info);
	m_soundview->update();
}


void SoundSettingsQt::updateFxLabels()
{
	auto toFactor = [](int v) -> float { return (float)pow(3.0, v / 100.0); };
	m_fxPitchLabel->setText(QString("%1x").arg(toFactor(m_fxPitchSlider->value()), 0, 'f', 2));
	m_fxSpeedLabel->setText(QString("%1x").arg(toFactor(m_fxSpeedSlider->value()), 0, 'f', 2));
	m_fxCombinedLabel->setText(QString("%1x").arg(toFactor(m_fxCombinedSlider->value()), 0, 'f', 2));
	m_fxReverbLabel->setText(QString("%1%").arg(m_fxReverbSlider->value()));
}


void SoundSettingsQt::onFxPitchChanged(int value)
{
	if (m_fxSyncButton->isChecked())
	{
		m_fxSpeedSlider->blockSignals(true);
		m_fxSpeedSlider->setValue(value);
		m_fxSpeedSlider->blockSignals(false);
	}
	updateFxLabels();
	applyFxToPreview();
}


void SoundSettingsQt::onFxSpeedChanged(int value)
{
	if (m_fxSyncButton->isChecked())
	{
		m_fxPitchSlider->blockSignals(true);
		m_fxPitchSlider->setValue(value);
		m_fxPitchSlider->blockSignals(false);
	}
	updateFxLabels();
	applyFxToPreview();
}


void SoundSettingsQt::onFxCombinedChanged(int value)
{
	m_fxPitchSlider->blockSignals(true);
	m_fxSpeedSlider->blockSignals(true);
	m_fxPitchSlider->setValue(value);
	m_fxSpeedSlider->setValue(value);
	m_fxPitchSlider->blockSignals(false);
	m_fxSpeedSlider->blockSignals(false);
	updateFxLabels();
	applyFxToPreview();
}


void SoundSettingsQt::onFxReverbChanged(int)
{
	updateFxLabels();
	applyFxToPreview();
}


void SoundSettingsQt::onFxSyncToggled(bool checked)
{
	m_fxCombinedSlider->setEnabled(checked);
	m_fxPitchSlider->setEnabled(!checked);
	m_fxSpeedSlider->setEnabled(!checked);
	if (checked)
	{
		int val = m_fxPitchSlider->value();
		m_fxCombinedSlider->blockSignals(true);
		m_fxCombinedSlider->setValue(val);
		m_fxCombinedSlider->blockSignals(false);
		m_fxSpeedSlider->blockSignals(true);
		m_fxSpeedSlider->setValue(val);
		m_fxSpeedSlider->blockSignals(false);
	}
	updateFxLabels();
}


void SoundSettingsQt::onFxReset()
{
	m_fxPitchSlider->setValue(0);
	m_fxSpeedSlider->setValue(0);
	m_fxCombinedSlider->setValue(0);
	m_fxReverbSlider->setValue(0);
	m_fxSyncButton->setChecked(false);
	updateFxLabels();
}







