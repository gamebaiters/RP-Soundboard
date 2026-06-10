//----------------------------------
// RP Soundboard Source Code
// GameBaiters fork
//----------------------------------

#include "AudioEffectsDialog.h"
#include "samples.h"
#include "style_helper.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>

static const int SLIDER_MIN = 50;
static const int SLIDER_MAX = 200;
static const int SLIDER_DEFAULT = 100;

static const int INTENSITY_MIN = 100;
static const int INTENSITY_MAX = 2000;
static const int INTENSITY_DEFAULT = 100;


static QSlider *createEffectSlider(int min, int max, int value)
{
	QSlider *slider = new QSlider(Qt::Horizontal);
	slider->setRange(min, max);
	slider->setValue(value);
	slider->setTickPosition(QSlider::TicksBelow);
	slider->setTickInterval((max - min) / 4);
	return slider;
}


AudioEffectsDialog::AudioEffectsDialog(Sampler *sampler, QWidget *parent)
	: QDialog(parent),
	  m_sampler(sampler)
{
	setWindowFlag(Qt::WindowContextHelpButtonHint, false);
	setWindowTitle("Audio Effects");
	setMinimumWidth(350);
	setStyleSheet(StyleHelper::loadDarkStyle());

	QVBoxLayout *mainLayout = new QVBoxLayout(this);

	// --- Pitch ---
	QHBoxLayout *pitchRow = new QHBoxLayout();
	pitchRow->addWidget(new QLabel("Pitch:"));
	m_pitchSlider = createEffectSlider(SLIDER_MIN, SLIDER_MAX, SLIDER_DEFAULT);
	pitchRow->addWidget(m_pitchSlider);
	m_pitchLabel = new QLabel("1.00x");
	m_pitchLabel->setMinimumWidth(45);
	pitchRow->addWidget(m_pitchLabel);
	mainLayout->addLayout(pitchRow);

	// --- Speed ---
	QHBoxLayout *speedRow = new QHBoxLayout();
	speedRow->addWidget(new QLabel("Speed:"));
	m_speedSlider = createEffectSlider(SLIDER_MIN, SLIDER_MAX, SLIDER_DEFAULT);
	speedRow->addWidget(m_speedSlider);
	m_speedLabel = new QLabel("1.00x");
	m_speedLabel->setMinimumWidth(45);
	speedRow->addWidget(m_speedLabel);
	mainLayout->addLayout(speedRow);

	// --- Link checkbox ---
	m_linkCheckbox = new QCheckBox("Link Pitch && Speed");
	mainLayout->addWidget(m_linkCheckbox);

	// --- Combined ---
	QHBoxLayout *combinedRow = new QHBoxLayout();
	combinedRow->addWidget(new QLabel("Combined:"));
	m_combinedSlider = createEffectSlider(SLIDER_MIN, SLIDER_MAX, SLIDER_DEFAULT);
	m_combinedSlider->setEnabled(false);
	combinedRow->addWidget(m_combinedSlider);
	m_combinedLabel = new QLabel("1.00x");
	m_combinedLabel->setMinimumWidth(45);
	combinedRow->addWidget(m_combinedLabel);
	mainLayout->addLayout(combinedRow);

	// --- Intensity ---
	QHBoxLayout *intensityRow = new QHBoxLayout();
	intensityRow->addWidget(new QLabel("Intensity:"));
	m_intensitySlider = createEffectSlider(INTENSITY_MIN, INTENSITY_MAX, INTENSITY_DEFAULT);
	intensityRow->addWidget(m_intensitySlider);
	m_intensityLabel = new QLabel("1.00x");
	m_intensityLabel->setMinimumWidth(45);
	intensityRow->addWidget(m_intensityLabel);
	mainLayout->addLayout(intensityRow);

	m_resetButton = new QPushButton("Reset All");
	mainLayout->addWidget(m_resetButton);

	m_pitchSlider->setValue((int)(sampler->getPitchFactor() * 100));
	m_speedSlider->setValue((int)(sampler->getSpeedFactor() * 100));
	m_intensitySlider->setValue((int)(sampler->getIntensityFactor() * 100));
	updateLabels();

	connect(m_pitchSlider, &QSlider::valueChanged, this, &AudioEffectsDialog::onPitchChanged);
	connect(m_speedSlider, &QSlider::valueChanged, this, &AudioEffectsDialog::onSpeedChanged);
	connect(m_combinedSlider, &QSlider::valueChanged, this, &AudioEffectsDialog::onCombinedChanged);
	connect(m_intensitySlider, &QSlider::valueChanged, this, &AudioEffectsDialog::onIntensityChanged);
	connect(m_linkCheckbox, &QCheckBox::toggled, this, &AudioEffectsDialog::onLinkToggled);
	connect(m_resetButton, &QPushButton::clicked, this, &AudioEffectsDialog::onReset);
}


void AudioEffectsDialog::onPitchChanged(int value)
{
	float factor = value / 100.0f;
	m_sampler->setPitchFactor(factor);
	updateLabels();
}


void AudioEffectsDialog::onSpeedChanged(int value)
{
	float factor = value / 100.0f;
	m_sampler->setSpeedFactor(factor);
	updateLabels();
}


void AudioEffectsDialog::onCombinedChanged(int value)
{
	float factor = value / 100.0f;
	m_sampler->setPitchFactor(factor);
	m_sampler->setSpeedFactor(factor);

	m_pitchSlider->blockSignals(true);
	m_speedSlider->blockSignals(true);
	m_pitchSlider->setValue(value);
	m_speedSlider->setValue(value);
	m_pitchSlider->blockSignals(false);
	m_speedSlider->blockSignals(false);

	updateLabels();
}


void AudioEffectsDialog::onIntensityChanged(int value)
{
	float factor = value / 100.0f;
	m_sampler->setIntensityFactor(factor);
	updateLabels();
}


void AudioEffectsDialog::onLinkToggled(bool checked)
{
	m_combinedSlider->setEnabled(checked);
	m_pitchSlider->setEnabled(!checked);
	m_speedSlider->setEnabled(!checked);

	if (checked)
	{
		int val = m_pitchSlider->value();
		m_combinedSlider->blockSignals(true);
		m_combinedSlider->setValue(val);
		m_combinedSlider->blockSignals(false);

		float factor = val / 100.0f;
		m_sampler->setSpeedFactor(factor);
		m_speedSlider->blockSignals(true);
		m_speedSlider->setValue(val);
		m_speedSlider->blockSignals(false);
		updateLabels();
	}
}


void AudioEffectsDialog::onReset()
{
	m_pitchSlider->setValue(SLIDER_DEFAULT);
	m_speedSlider->setValue(SLIDER_DEFAULT);
	m_combinedSlider->setValue(SLIDER_DEFAULT);
	m_intensitySlider->setValue(INTENSITY_DEFAULT);
	m_linkCheckbox->setChecked(false);

	m_sampler->setPitchFactor(1.0f);
	m_sampler->setSpeedFactor(1.0f);
	m_sampler->setIntensityFactor(1.0f);
	updateLabels();
}


void AudioEffectsDialog::updateLabels()
{
	m_pitchLabel->setText(QString("%1x").arg(m_pitchSlider->value() / 100.0f, 0, 'f', 2));
	m_speedLabel->setText(QString("%1x").arg(m_speedSlider->value() / 100.0f, 0, 'f', 2));
	m_combinedLabel->setText(QString("%1x").arg(m_combinedSlider->value() / 100.0f, 0, 'f', 2));
	m_intensityLabel->setText(QString("%1x").arg(m_intensitySlider->value() / 100.0f, 0, 'f', 2));
}
