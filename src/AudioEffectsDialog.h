// src/AudioEffectsDialog.h
//----------------------------------
// RP Soundboard Source Code
// Audio Effects Dialog (Pitch, Speed, Intensity)
//----------------------------------

#pragma once
#ifndef rpsbsrc__AudioEffectsDialog_H__
#define rpsbsrc__AudioEffectsDialog_H__

#include <QDialog>
#include <QSlider>
#include <QLabel>
#include <QCheckBox>
#include <QPushButton>

class Sampler;

class AudioEffectsDialog : public QDialog
{
	Q_OBJECT

public:
	explicit AudioEffectsDialog(Sampler *sampler, QWidget *parent = nullptr);

private slots:
	void onPitchChanged(int value);
	void onSpeedChanged(int value);
	void onCombinedChanged(int value);
	void onIntensityChanged(int value);
	void onLinkToggled(bool checked);
	void onReset();

private:
	void updateLabels();

	Sampler *m_sampler;
	QSlider *m_pitchSlider;
	QSlider *m_speedSlider;
	QSlider *m_combinedSlider;
	QSlider *m_intensitySlider;
	QLabel *m_pitchLabel;
	QLabel *m_speedLabel;
	QLabel *m_combinedLabel;
	QLabel *m_intensityLabel;
	QCheckBox *m_linkCheckbox;
	QPushButton *m_resetButton;
};

#endif // rpsbsrc__AudioEffectsDialog_H__
