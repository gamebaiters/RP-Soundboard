// src/config_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#include "common.h"

#include <cmath>
#include <QFileDialog>
#include <QFileInfo>
#include <QResizeEvent>
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QColorDialog>

#include "config_qt.h"
#include "ConfigModel.h"
#include "main.h"
#include "soundsettings_qt.h"
#include "ts3log.h"
#include "SpeechBubble.h"
#include "buildinfo.h"
#include "plugin.h"
#include "style_helper.h"
#include "ExpandableSection.h"
#include "samples.h"
#include "SoundButton.h"
#include "soundview_qt.h"

#ifdef _WIN32
#include "windows.h"
#endif

enum button_choices_e {
	BC_CHOOSE = 0,
	BC_ADVANCED,
	BC_SET_HOTKEY,
	BC_SET_COLOR,
	BC_DELETE,
};


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
ConfigQt::ConfigQt( ConfigModel *model, QWidget *parent /*= 0*/ ) :
	QWidget(parent),
	ui(new Ui::ConfigQt),
	m_model(model),
	m_modelObserver(*this),
	m_buttonBubble(nullptr)
{
    /* Ensure resources are loaded */
    Q_INIT_RESOURCE(qtres);

    m_pauseIcon = QIcon(":/icon/img/pausebutton_32.png");
    m_playIcon = QIcon(":/icon/img/playarrow_32.png");

    ui->setupUi(this);
    this->setStyleSheet(StyleHelper::loadDarkStyle());
    //setAttribute(Qt::WA_DeleteOnClose);

	createConfigButtons();

    settingsSection = new ExpandableSection("Settings", 200, this);
    settingsSection->setContentLayout(*ui->settingsWidget->layout());
    layout()->addWidget(settingsSection);

    configsSection = new ExpandableSection("Configurations", 200, this);
    configsSection->setContentLayout(*ui->configsWidget->layout());
    layout()->addWidget(configsSection);

    QAction *actChooseFile = new QAction("Choose File", this);
	actChooseFile->setData((int)BC_CHOOSE);
	m_buttonContextMenu.addAction(actChooseFile);

	QAction *actAdvancedOpts = new QAction("Advanced Options", this);
	actAdvancedOpts->setData((int)BC_ADVANCED);
	m_buttonContextMenu.addAction(actAdvancedOpts);

	actSetHotkey = new QAction("Set hotkey", this);
	actSetHotkey->setData((int)BC_SET_HOTKEY);
	m_buttonContextMenu.addAction(actSetHotkey);

	QAction *actSetColor = new QAction("Set color", this);
	actSetColor->setData((int)BC_SET_COLOR);
	m_buttonContextMenu.addAction(actSetColor);

	QAction *actDeleteButton = new QAction("Make button great again (delete)", this);
	actDeleteButton->setData((int)BC_DELETE);
	m_buttonContextMenu.addAction(actDeleteButton);

	createButtons();

	ui->b_stop->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->b_pause->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->cb_mute_locally->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->cb_mute_myself->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->sl_volumeLocal->setContextMenuPolicy(Qt::CustomContextMenu);
	ui->sl_volumeRemote->setContextMenuPolicy(Qt::CustomContextMenu);
	
	connect(ui->b_stop, SIGNAL(clicked()), this, SLOT(onClickedStop()));
	connect(ui->b_stop, SIGNAL(customContextMenuRequested(const QPoint&)), this,
		SLOT(showStopButtonContextMenu(const QPoint&)));
	connect(ui->b_skip_back_10, SIGNAL(clicked()), this, SLOT(onSkipBack10()));
	connect(ui->b_skip_back_5, SIGNAL(clicked()), this, SLOT(onSkipBack5()));
	connect(ui->b_skip_fwd_5, SIGNAL(clicked()), this, SLOT(onSkipFwd5()));
	connect(ui->b_skip_fwd_10, SIGNAL(clicked()), this, SLOT(onSkipFwd10()));
	connect(ui->waveformView, &SoundView::seekRequested, this, &ConfigQt::onWaveformSeek);
	connect(ui->b_pause, SIGNAL(clicked()), this, SLOT(onButtonPausePressed()));
	connect(ui->b_pause, SIGNAL(customContextMenuRequested(const QPoint&)), this,
		SLOT(showPauseButtonContextMenu(const QPoint&)));
	connect(ui->sl_volumeLocal, SIGNAL(valueChanged(int)), this, SLOT(onUpdateVolumeLocal(int)));
	connect(ui->sl_volumeRemote, SIGNAL(valueChanged(int)), this, SLOT(onUpdateVolumeRemote(int)));
	connect(ui->cb_mute_locally, SIGNAL(clicked(bool)), this, SLOT(onUpdateMuteLocally(bool)));
	connect(ui->sb_rows, SIGNAL(valueChanged(int)), this, SLOT(onUpdateRows(int)));
	connect(ui->sb_cols, SIGNAL(valueChanged(int)), this, SLOT(onUpdateCols(int)));
	connect(ui->cb_mute_myself, SIGNAL(clicked(bool)), this, SLOT(onUpdateMuteMyself(bool)));
	connect(ui->cb_show_hotkeys_on_buttons, SIGNAL(clicked(bool)), this, SLOT(onUpdateShowHotkeysOnButtons(bool)));
	connect(ui->cb_disable_hotkeys, SIGNAL(clicked(bool)), this, SLOT(onUpdateHotkeysDisabled(bool)));
	connect(ui->filterEdit, SIGNAL(textChanged(const QString&)), this, SLOT(onFilterEditTextChanged(const QString&)));
	connect(ui->cb_mute_locally, &QCheckBox::customContextMenuRequested,
		[this](const QPoint &point) {this->showSetHotkeyMenu(HOTKEY_MUTE_ON_MY_CLIENT, ui->cb_mute_locally->mapToGlobal(point));});
	connect(ui->cb_mute_myself, &QCheckBox::customContextMenuRequested,
		[this](const QPoint &point) {this->showSetHotkeyMenu(HOTKEY_MUTE_MYSELF, ui->cb_mute_myself->mapToGlobal(point));});
	connect(ui->sl_volumeLocal, &QSlider::customContextMenuRequested, this, &ConfigQt::onVolumeSliderContextMenuLocal);
	connect(ui->sl_volumeRemote, &QSlider::customContextMenuRequested, this, &ConfigQt::onVolumeSliderContextMenuRemote);
	connect(ui->cb_link_volumes, SIGNAL(toggled(bool)), this, SLOT(onLinkVolumesChanged(bool)));
	connect(ui->cb_earrape_protection, SIGNAL(toggled(bool)), this, SLOT(onEarrapeProtectionChanged(bool)));
	connect(ui->cb_remember_pitch_speed, SIGNAL(toggled(bool)), this, SLOT(onRememberPitchSpeedChanged(bool)));
	connect(ui->cb_multi_soundboard, &QCheckBox::toggled, [this](bool checked) {
		m_model->setMultiSoundboard(checked);
		onMultiModeChanged(checked);
	});

	m_multiBarContainer = ui->multiBarLayout;
	m_slot0VolumeWidget = nullptr;
	m_slot0VolumeLocal = nullptr;
	m_slot0VolumeRemote = nullptr;
	m_slot0VolLocalLabel = nullptr;
	m_slot0VolRemoteLabel = nullptr;

	buildPitchSpeedUI();

    /* Load/Save Model */
    connect(ui->pushLoad, SIGNAL(released()), this, SLOT(onLoadModel()));
    connect(ui->pushSave, SIGNAL(released()), this, SLOT(onSaveModel()));

    ui->playingIconLabel->hide();
	ui->playingLabel->setText("");
	playingIconTimer = new QTimer(this);
	playingIconTimer->setInterval(150);
	connect(playingIconTimer, SIGNAL(timeout()), this, SLOT(onPlayingIconTimer()));

	Sampler *sampler = sb_getSampler();
	connect(sampler, SIGNAL(onStartPlaying(int, bool, QString)), this, SLOT(onStartPlayingSound(int, bool, QString)), Qt::QueuedConnection);
	connect(sampler, SIGNAL(onStopPlaying(int)), this, SLOT(onStopPlayingSound(int)), Qt::QueuedConnection);
	connect(sampler, SIGNAL(onPausePlaying(int)), this, SLOT(onPausePlayingSound(int))); // No queued connection since signal is emitted from GUI Thread
	connect(sampler, SIGNAL(onUnpausePlaying(int)), this, SLOT(onUnpausePlayingSound(int))); // No queued connection since signal is emitted from GUI Thread

	createBubbles();

	m_model->addObserver(&m_modelObserver);

	/* Force configuration 0 */
	setConfiguration(0);
}

void ConfigQt::setConfiguration(int cfg)
{
	if (cfg < 0 || cfg >= NUM_CONFIGS)
	{
		logError("Invalid config id: %i", cfg);
		return;
	}

	m_configRadioButtons[cfg]->setChecked(true);
	m_model->setConfiguration(cfg);
	ui->labelStatus->setText(QString("Configuration %1").arg(cfg + 1));
}

void ConfigQt::onSetConfig()
{
	QRadioButton *button = qobject_cast<QRadioButton*>(sender());
	int configId = button->property("configId").toInt();
	setConfiguration(configId);
}

void ConfigQt::onConfigHotkey()
{
	QPushButton *button = qobject_cast<QPushButton*>(sender());
	int configId = button->property("configId").toInt();

	char buf[16];
	sb_getInternalConfigHotkeyName(configId, buf);
    ts3Functions.requestHotkeyInputDialog(getPluginID(), buf, configId, this);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
ConfigQt::~ConfigQt()
{
	if (m_model)
		m_model->remObserver(&m_modelObserver);  // safe even if already detached
	delete ui;
}

void ConfigQt::detachFromModel()
{
	if (!m_model) return;
	m_model->remObserver(&m_modelObserver);
	// Keep m_model pointer valid (other code paths read from it). Just
	// stop receiving notifications so the hidden grid stops rebuilding.
}

void ConfigQt::onSaveModel()
{
    QString fn = QFileDialog::getSaveFileName(this, tr("Choose File to Save"), QString(), tr("Ini Files (*.ini)"));
    if (fn.isNull())
        return;
    m_model->writeConfig(fn);
}

void ConfigQt::onLoadModel()
{
    QString fn = QFileDialog::getOpenFileName(this, tr("Choose File to Load"), QString(), tr("Ini Files (*.ini)"));
    if (fn.isNull())
        return;
    m_model->readConfig(fn);
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::closeEvent(QCloseEvent *)
{
	m_model->setWindowSize(size().width(), size().height());
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onClickedPlay()
{
	QPushButton *button = dynamic_cast<QPushButton*>(sender());
	size_t buttonId = std::find_if(m_buttons.begin(), m_buttons.end(), [button](SoundButton *b){return b == button;}) - m_buttons.begin();

	playSound(buttonId);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onClickedStop()
{
	sb_stopPlayback();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateVolumeLocal(int val)
{
	m_model->setVolumeLocal(val);
	if (m_model->getLinkVolumes())
	{
		ui->sl_volumeRemote->blockSignals(true);
		ui->sl_volumeRemote->setValue(val);
		ui->sl_volumeRemote->blockSignals(false);
		m_model->setVolumeRemote(val);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateVolumeRemote(int val)
{
	m_model->setVolumeRemote(val);
	if (m_model->getLinkVolumes())
	{
		ui->sl_volumeLocal->blockSignals(true);
		ui->sl_volumeLocal->setValue(val);
		ui->sl_volumeLocal->blockSignals(false);
		m_model->setVolumeLocal(val);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateMuteLocally(bool val)
{
	m_model->setPlaybackLocal(!val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateCols(int val)
{
	m_model->setCols(val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateRows(int val)
{
	m_model->setRows(val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateMuteMyself(bool val)
{
	m_model->setMuteMyselfDuringPb(val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::createButtons()
{
	for(SoundButton *button : m_buttons)
		delete button;
	m_buttons.clear();

	int numRows = m_model->getRows();
	int numCols = m_model->getCols();

	for(int i = 0; i < numRows; i++)
	{
		for(int j = 0; j < numCols; j++)
		{
			SoundButton *elem = new SoundButton(this);
			elem->setProperty("buttonId", (int)m_buttons.size());
			elem->setText("(no file)");
			elem->setEnabled(true);
			QSizePolicy policy(QSizePolicy::Ignored, QSizePolicy::Expanding);
			policy.setRetainSizeWhenHidden(true);
			elem->setSizePolicy(policy);
			ui->gridLayout->addWidget(elem, i, j);
			connect(elem, SIGNAL(clicked()), this, SLOT(onClickedPlay()));
			elem->setContextMenuPolicy(Qt::CustomContextMenu);
			connect(elem, SIGNAL(customContextMenuRequested(const QPoint&)), this,
				SLOT(showButtonContextMenu(const QPoint&)));
			connect(elem, SIGNAL(fileDropped(QList<QUrl>)), this, SLOT(onButtonFileDropped(QList<QUrl>)));
			connect(elem, SIGNAL(buttonDropped(SoundButton*)), this, SLOT(onButtonDroppedOnButton(SoundButton*)));

			elem->updateGeometry();
			m_buttons.push_back(elem);
		}
	}

    for(int i = 0; i < (int)m_buttons.size(); i++)
		updateButtonText(i);

	if(m_buttonBubble)
		m_buttonBubble->attachTo(m_buttons[0]);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::createConfigButtons()
{
	for (int i = 0; i < NUM_CONFIGS; i++)
	{
		m_configRadioButtons[i] = new QRadioButton(this);
		m_configRadioButtons[i]->setText(QString("Config %1").arg(i + 1));
		m_configRadioButtons[i]->setProperty("configId", i);
		m_configRadioButtons[i]->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
		ui->configsGrid->addWidget(m_configRadioButtons[i], 0, i, Qt::AlignCenter);
		connect(m_configRadioButtons[i], SIGNAL(clicked()), this, SLOT(onSetConfig()));

		m_configHotkeyButtons[i] = new QPushButton(this);
		m_configHotkeyButtons[i]->setText("Set hotkey");
		m_configHotkeyButtons[i]->setProperty("configId", i);
		m_configHotkeyButtons[i]->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		m_configHotkeyButtons[i]->setToolTipDuration(0);
		m_configHotkeyButtons[i]->setToolTip(getConfigShortcutString(i));
		ui->configsGrid->addWidget(m_configHotkeyButtons[i], 1, i, Qt::AlignCenter);
		connect(m_configHotkeyButtons[i], SIGNAL(clicked()), this, SLOT(onConfigHotkey()));

		ui->configsWidget->updateGeometry();
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool ConfigQt::hotkeysEnabled()
{
	return m_model->getHotkeysEnabled();
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::updateButtonText(int i)
{
    if(i >= (int)m_buttons.size())
		return;

	QString text;
	const SoundInfo *info = m_model->getSoundInfo(i);
	if (info && !info->filename.isEmpty())
	{
		if (!info->customText.isEmpty())
			text = unescapeCustomText(info->customText);
		else
			text = QFileInfo(info->filename).baseName();
	}
	else
		text = "(no file)";

	if (m_model->getShowHotkeysOnButtons())
	{
		QString shortcut = getShortcutString(i);
		if (shortcut.length() > 0)
			text = text + "\n" + shortcut;
	}
	m_buttons[i]->setText(text);
	m_buttons[i]->setBackgroundColor(info ? info->customColor : QColor(0, 0, 0, 0));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::showButtonContextMenu( const QPoint &point )
{
	QPushButton *button = dynamic_cast<QPushButton*>(sender());
	size_t buttonId = std::find_if(m_buttons.begin(), m_buttons.end(), [button](SoundButton *b){return b == button;}) - m_buttons.begin();

	QString shortcutName = getShortcutString(buttonId);
	QString hotkeyText = "Set hotkey (Current: " +
		(shortcutName.isEmpty() ? QString("None") : shortcutName) + ")";
	actSetHotkey->setText(hotkeyText);

	QPoint globalPos = m_buttons[buttonId]->mapToGlobal(point);
	QAction *action = m_buttonContextMenu.exec(globalPos);
	if(action)
	{
		bool ok = false;
		int choice = action->data().toInt(&ok);
		if(ok)
		{
			switch(choice)
			{
			case BC_CHOOSE: 
				chooseFile(buttonId); 
				break;
			case BC_ADVANCED: 
				openAdvanced(buttonId); 
				break;
			case BC_SET_HOTKEY:
				openHotkeySetDialog(buttonId);
				break;
			case BC_SET_COLOR:
				openButtonColorDialog(buttonId);
				break;
			case BC_DELETE:
				deleteButton(buttonId);
				break;
			default: break;
			}
		}
		else
			logError("Invalid user data in context menu");
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::setPlayingLabelIcon(int index)
{
	ui->playingIconLabel->setPixmap(QPixmap(QString(":/icon/img/speaker_icon_%1_64.png").arg(index)));
}



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::playSound( size_t buttonId )
{
	const SoundInfo *info = m_model->getSoundInfo(buttonId);
	if(info)
	{
		// Apply per-song Custom FX to global sliders.
		// Always apply when fxRemember is on — including zeros, so that
		// resetting FX to defaults actually takes effect instead of
		// leaving stale values from the previous playback.
		if (info->fxRemember)
		{
			m_model->setPitchValue(info->fxPitch);
			m_model->setSpeedValue(info->fxSpeed);
			m_model->setReverbValue(info->fxReverb);
			m_model->setSyncPitchSpeed(info->fxSyncPitchSpeed);
		}

		int result = sb_playFile(*info);
		if (result == 2)
			ui->labelStatus->setText("Not connected to a server");
		else if (result == 1)
			ui->labelStatus->setText(QString("Failed to open: %1").arg(QFileInfo(info->filename).fileName()));
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::chooseFile( size_t buttonId )
{
	QString filePath = m_model->getFileName(buttonId);
	QString fn = QFileDialog::getOpenFileName(this, tr("Choose File"), filePath, tr("Files (*.*)"));
	if (fn.isNull())
		return;
	setButtonFile(buttonId, fn);

}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::openAdvanced( size_t buttonId )
{
	const SoundInfo *buttonInfo = m_model->getSoundInfo(buttonId);
	SoundInfo defaultInfo;
	const SoundInfo &info = buttonInfo ? *buttonInfo : defaultInfo;
	SoundSettingsQt dlg(info, buttonId, this);
	dlg.setWindowTitle(QString("Sound %1 Settings").arg(QString::number(buttonId + 1)));
	if(dlg.exec() == QDialog::Accepted)
		m_model->setSoundInfo(buttonId, dlg.getSoundInfo());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::deleteButton(size_t buttonId)
{
	const SoundInfo *info = m_model->getSoundInfo(buttonId);
	if (info)
		m_model->setSoundInfo(buttonId, SoundInfo());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::createBubbles()
{
	if(m_model->getBubbleButtonsBuild() == 0)
	{
		m_buttonBubble = new SpeechBubble(this);
		m_buttonBubble->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
		m_buttonBubble->setFixedSize(180, 80);
		m_buttonBubble->setText("Right click to choose sound file\nor open advanced options.");
		m_buttonBubble->attachTo(m_buttons[0]);
		connect(m_buttonBubble, SIGNAL(closePressed()), this, SLOT(onButtonBubbleFinished()));
	}

	if(m_model->getBubbleStopBuild() == 0)
	{
		SpeechBubble *stopBubble = new SpeechBubble(this);
		stopBubble->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
		stopBubble->setFixedSize(180, 60);
		stopBubble->setText("Stop the currently playing sound.");
		stopBubble->attachTo(ui->b_stop);
		connect(stopBubble, SIGNAL(closePressed()), this, SLOT(onStopBubbleFinished()));
	}

	if(m_model->getBubbleColsBuild() == 0)
	{
		settingsSection->setExpanded(true);
		SpeechBubble *colsBubble = new SpeechBubble(this);
		colsBubble->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
		colsBubble->setFixedSize(180, 80);
		colsBubble->setText("Change the number of buttons\non the soundboard.");
		colsBubble->attachTo(ui->sb_cols);
		connect(colsBubble, SIGNAL(closePressed()), this, SLOT(onColsBubbleFinished()));
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onStopBubbleFinished()
{
	m_model->setBubbleStopBuild(buildinfo_getBuildNumber());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onButtonBubbleFinished()
{
	m_model->setBubbleButtonsBuild(buildinfo_getBuildNumber());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onColsBubbleFinished()
{
	m_model->setBubbleColsBuild(buildinfo_getBuildNumber());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::showStopButtonContextMenu(const QPoint &point)
{
	showSetHotkeyMenu(HOTKEY_STOP_ALL, ui->b_stop->mapToGlobal(point));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::showPauseButtonContextMenu(const QPoint &point)
{
	showSetHotkeyMenu(HOTKEY_PAUSE_ALL, ui->b_pause->mapToGlobal(point));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::showSetHotkeyMenu(const char *hotkeyName, const QPoint &point)
{
	QString hotkeyString = getShortcutString(hotkeyName);
	QString hotkeyText = "Set hotkey (Current: " +
		(hotkeyString.isEmpty() ? QString("None") : hotkeyString) + ")";

	QMenu menu;
	menu.addAction(hotkeyText);
	QAction *action = menu.exec(point);
	if (action)
		ts3Functions.requestHotkeyInputDialog(getPluginID(), hotkeyName, 0, this);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onStartPlayingSound(int slot, bool preview, QString filename)
{
	// Don't update main playback UI for previews (they play in the advanced options dialog)
	if (preview)
		return;

	if (slot == 0)
	{
		// Slot 0 uses the built-in UI bar
		QFileInfo info(filename);
		ui->playingLabel->setText(info.fileName());
		setPlayingLabelIcon(0);
		ui->playingIconLabel->show();
		playingIconIndex = 1;
		playingIconTimer->start();
		ui->b_stop->setEnabled(true);
		ui->b_pause->setEnabled(true);
		ui->b_pause->setIcon(m_pauseIcon);
		SoundInfo sound;
		sound.filename = filename;
		ui->waveformView->setSound(sound);
		ui->lb_playback_time->setText("0:00 / 0:00");
	}
	else
	{
		// Slots 1-4: create a dynamic playback bar
		createPlaybackBar(slot, filename);
		// Ensure the icon timer is running for updating all bars
		if (!playingIconTimer->isActive())
		{
			playingIconIndex = 1;
			playingIconTimer->start();
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onStopPlayingSound(int slot)
{
	if (slot == 0)
	{
		// Reset the built-in bar for slot 0
		ui->playingLabel->setText("");
		ui->playingIconLabel->hide();
		ui->b_pause->setIcon(m_pauseIcon);
		ui->waveformView->clearPlayback();
		ui->lb_playback_time->setText("0:00 / 0:00");

		// Reset slot-0 per-slot volume sliders to default
		if (m_slot0VolumeLocal)
		{
			m_slot0VolumeLocal->blockSignals(true);
			m_slot0VolumeLocal->setValue(m_model->getVolumeLocal());
			m_slot0VolumeLocal->blockSignals(false);
		}
		if (m_slot0VolumeRemote)
		{
			m_slot0VolumeRemote->blockSignals(true);
			m_slot0VolumeRemote->setValue(m_model->getVolumeRemote());
			m_slot0VolumeRemote->blockSignals(false);
		}
	}
	else
	{
		// Remove the dynamic bar for this slot (volume sliders are destroyed with it)
		removePlaybackBar(slot);
	}

	Sampler *sampler = sb_getSampler();
	bool anyStillPlaying = sampler && sampler->getActiveSlotCount() > 0;

	if (!anyStillPlaying)
	{
		playingIconTimer->stop();

		// Reset pitch/speed/combined sliders unless "Remember Pitch/Speed" is checked
		if (!ui->cb_remember_pitch_speed->isChecked())
		{
			onResetFx();
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onPausePlayingSound(int slot)
{
	if (slot == 0)
	{
		ui->b_pause->setIcon(m_playIcon);
	}
	else
	{
		for (PlaybackBar *bar : m_playbackBars)
		{
			if (bar->slot == slot)
			{
				bar->pauseButton->setIcon(m_playIcon);
				break;
			}
		}
	}

	// If all slots are paused, stop the icon timer
	Sampler *sampler = sb_getSampler();
	if (sampler && sampler->findSlotByState(Sampler::ePLAYING) < 0)
		playingIconTimer->stop();
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigQt::onUnpausePlayingSound(int slot)
{
	if (slot == 0)
	{
		ui->b_pause->setIcon(m_pauseIcon);
	}
	else
	{
		for (PlaybackBar *bar : m_playbackBars)
		{
			if (bar->slot == slot)
			{
				bar->pauseButton->setIcon(m_pauseIcon);
				break;
			}
		}
	}

	playingIconTimer->start();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onPlayingIconTimer()
{
	setPlayingLabelIcon(playingIconIndex);
	++playingIconIndex %= 4;

	Sampler *sampler = sb_getSampler();
	if (sampler)
	{
		// Update slot 0 (built-in bar)
		if (sampler->getState(0) == Sampler::ePLAYING || sampler->getState(0) == Sampler::ePAUSED)
		{
			double pos = sampler->getPosition(0);
			double len = sampler->getLength(0);
			if (len > 0.0)
			{
				ui->waveformView->setPlaybackPosition(pos / len);

				int posMin = (int)pos / 60;
				int posSec = (int)pos % 60;
				int lenMin = (int)len / 60;
				int lenSec = (int)len % 60;
				ui->lb_playback_time->setText(
					QString("%1:%2 / %3:%4")
						.arg(posMin)
						.arg(posSec, 2, 10, QChar('0'))
						.arg(lenMin)
						.arg(lenSec, 2, 10, QChar('0'))
				);
			}
		}

		// Update all dynamic playback bars
		updateAllPlaybackBars();
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onSkipBack10()
{
	Sampler *sampler = sb_getSampler();
	if (sampler) sampler->seek((std::max)(0.0, sampler->getPosition() - 10.0));
}

void ConfigQt::onSkipBack5()
{
	Sampler *sampler = sb_getSampler();
	if (sampler) sampler->seek((std::max)(0.0, sampler->getPosition() - 5.0));
}

void ConfigQt::onSkipFwd5()
{
	Sampler *sampler = sb_getSampler();
	if (sampler) sampler->seek((std::min)(sampler->getLength(), sampler->getPosition() + 5.0));
}

void ConfigQt::onSkipFwd10()
{
	Sampler *sampler = sb_getSampler();
	if (sampler) sampler->seek((std::min)(sampler->getLength(), sampler->getPosition() + 10.0));
}

void ConfigQt::onProgressSliderMoved(int value)
{
}

void ConfigQt::onProgressSliderPressed()
{
	m_sliderPressed = true;
}

void ConfigQt::onProgressSliderReleased()
{
	m_sliderPressed = false;
}

void ConfigQt::onWaveformSeek(double fraction)
{
	Sampler *sampler = sb_getSampler();
	if (sampler)
	{
		double len = sampler->getLength();
		if (len > 0.0)
			sampler->seek(fraction * len);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onLinkVolumesChanged(bool checked)
{
	m_model->setLinkVolumes(checked);
	if (checked)
	{
		// Sync remote to local when enabling
		int val = ui->sl_volumeLocal->value();
		ui->sl_volumeRemote->blockSignals(true);
		ui->sl_volumeRemote->setValue(val);
		ui->sl_volumeRemote->blockSignals(false);
		m_model->setVolumeRemote(val);
	}
}

void ConfigQt::onEarrapeProtectionChanged(bool checked)
{
	m_model->setEarrapeProtection(checked);
}


void ConfigQt::onPitchValueChanged(int value)
{
	m_model->setPitchValue(value);
	updatePitchSpeedLabels();
}

void ConfigQt::onSpeedValueChanged(int value)
{
	m_model->setSpeedValue(value);
	updatePitchSpeedLabels();
}

void ConfigQt::onCombinedValueChanged(int value)
{
	// Combined slider sets both pitch and speed
	m_pitchSlider->blockSignals(true);
	m_speedSlider->blockSignals(true);
	m_pitchSlider->setValue(value);
	m_speedSlider->setValue(value);
	m_pitchSlider->blockSignals(false);
	m_speedSlider->blockSignals(false);

	m_model->setPitchValue(value);
	m_model->setSpeedValue(value);
	updatePitchSpeedLabels();
}

void ConfigQt::onSyncToggled(bool checked)
{
	m_model->setSyncPitchSpeed(checked);
	m_combinedSlider->setEnabled(checked);
	m_pitchSlider->setEnabled(!checked);
	m_speedSlider->setEnabled(!checked);

	if (checked)
	{
		// Switching to combined mode: save individual values
		m_rememberedPitchValue = m_pitchSlider->value();
		m_rememberedSpeedValue = m_speedSlider->value();

		// Restore remembered combined value
		m_combinedSlider->blockSignals(true);
		m_combinedSlider->setValue(m_rememberedCombinedValue);
		m_combinedSlider->blockSignals(false);

		// Apply combined value to both pitch and speed
		m_pitchSlider->blockSignals(true);
		m_speedSlider->blockSignals(true);
		m_pitchSlider->setValue(m_rememberedCombinedValue);
		m_speedSlider->setValue(m_rememberedCombinedValue);
		m_pitchSlider->blockSignals(false);
		m_speedSlider->blockSignals(false);
		m_model->setPitchValue(m_rememberedCombinedValue);
		m_model->setSpeedValue(m_rememberedCombinedValue);
	}
	else
	{
		// Switching to individual mode: save combined value
		m_rememberedCombinedValue = m_combinedSlider->value();

		// Restore remembered individual values
		m_pitchSlider->blockSignals(true);
		m_speedSlider->blockSignals(true);
		m_pitchSlider->setValue(m_rememberedPitchValue);
		m_speedSlider->setValue(m_rememberedSpeedValue);
		m_pitchSlider->blockSignals(false);
		m_speedSlider->blockSignals(false);

		// Apply individual values
		m_model->setPitchValue(m_rememberedPitchValue);
		m_model->setSpeedValue(m_rememberedSpeedValue);
	}
	updatePitchSpeedLabels();
}


//---------------------------------------------------------------
// Purpose: Create a dynamic playback bar for an additional slot
//---------------------------------------------------------------
PlaybackBar *ConfigQt::createPlaybackBar(int slot, const QString &filename)
{
	PlaybackBar *bar = new PlaybackBar();
	bar->slot = slot;

	bar->frame = new QFrame(this);
	bar->frame->setFrameShape(QFrame::StyledPanel);
	QVBoxLayout *outerLayout = new QVBoxLayout(bar->frame);
	outerLayout->setSpacing(3);
	outerLayout->setContentsMargins(6, 4, 6, 4);

	// ---- Row 1: transport + filename + time + waveform + volumes ----
	QHBoxLayout *topRow = new QHBoxLayout();
	topRow->setSpacing(4);

	bar->stopButton = new QPushButton(bar->frame);
	bar->stopButton->setIcon(QIcon(":/icon/img/stoparrow_32.png"));
	bar->stopButton->setMaximumWidth(30);
	connect(bar->stopButton, &QPushButton::clicked, [this, slot]() {
		Sampler *sampler = sb_getSampler();
		if (sampler) sampler->stopPlayback(slot);
	});
	topRow->addWidget(bar->stopButton);

	bar->pauseButton = new QPushButton(bar->frame);
	bar->pauseButton->setIcon(m_pauseIcon);
	bar->pauseButton->setMaximumWidth(30);
	connect(bar->pauseButton, &QPushButton::clicked, [this, slot]() {
		Sampler *sampler = sb_getSampler();
		if (!sampler) return;
		if (sampler->getState(slot) == Sampler::ePLAYING)
			sampler->pausePlayback(slot);
		else if (sampler->getState(slot) == Sampler::ePAUSED)
			sampler->unpausePlayback(slot);
	});
	topRow->addWidget(bar->pauseButton);

	// Skip buttons
	auto makeSkipBtn = [&](const QString &text) -> QPushButton* {
		QPushButton *b = new QPushButton(text, bar->frame);
		b->setMinimumWidth(44);
		b->setMaximumWidth(56);
		QFont f = b->font(); f.setPointSize(9); b->setFont(f);
		return b;
	};
	bar->skipBack10 = makeSkipBtn("-10s");
	bar->skipBack5  = makeSkipBtn("-5s");
	bar->skipFwd5   = makeSkipBtn("+5s");
	bar->skipFwd10  = makeSkipBtn("+10s");
	connect(bar->skipBack10, &QPushButton::clicked, [this, slot]() {
		Sampler *s = sb_getSampler();
		if (s) s->seek((std::max)(0.0, s->getPosition(slot) - 10.0), slot);
	});
	connect(bar->skipBack5, &QPushButton::clicked, [this, slot]() {
		Sampler *s = sb_getSampler();
		if (s) s->seek((std::max)(0.0, s->getPosition(slot) - 5.0), slot);
	});
	connect(bar->skipFwd5, &QPushButton::clicked, [this, slot]() {
		Sampler *s = sb_getSampler();
		if (s) s->seek((std::min)(s->getLength(slot), s->getPosition(slot) + 5.0), slot);
	});
	connect(bar->skipFwd10, &QPushButton::clicked, [this, slot]() {
		Sampler *s = sb_getSampler();
		if (s) s->seek((std::min)(s->getLength(slot), s->getPosition(slot) + 10.0), slot);
	});
	topRow->addWidget(bar->skipBack10);
	topRow->addWidget(bar->skipBack5);
	topRow->addWidget(bar->skipFwd5);
	topRow->addWidget(bar->skipFwd10);

	bar->filenameLabel = new QLabel(QFileInfo(filename).fileName(), bar->frame);
	bar->filenameLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
	topRow->addWidget(bar->filenameLabel);

	bar->timeLabel = new QLabel("0:00 / 0:00", bar->frame);
	bar->timeLabel->setMinimumWidth(80);
	topRow->addWidget(bar->timeLabel);

	bar->waveformView = new SoundView(bar->frame);
	bar->waveformView->setMinimumSize(100, 28);
	bar->waveformView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	SoundInfo sound;
	sound.filename = filename;
	bar->waveformView->setSound(sound);
	connect(bar->waveformView, &SoundView::seekRequested, [this, slot](double fraction) {
		Sampler *sampler = sb_getSampler();
		if (sampler) {
			double len = sampler->getLength(slot);
			if (len > 0.0) sampler->seek(fraction * len, slot);
		}
	});
	topRow->addWidget(bar->waveformView);

	outerLayout->addLayout(topRow);

	// ---- Row 2: FX sliders (Pitch / Speed / P+S / Reverb) + Sync/Reset ----
	QHBoxLayout *fxRow = new QHBoxLayout();
	fxRow->setSpacing(4);

	// Volume controls inline
	QLabel *remLabel = new QLabel("Rem", bar->frame);
	remLabel->setFixedWidth(22);
	fxRow->addWidget(remLabel);
	bar->volumeRemoteSlider = new QSlider(Qt::Horizontal, bar->frame);
	bar->volumeRemoteSlider->setRange(0, 100);
	bar->volumeRemoteSlider->setValue(m_model->getVolumeRemote());
	bar->volumeRemoteSlider->setFixedWidth(60);
	fxRow->addWidget(bar->volumeRemoteSlider);

	bar->linked = m_model->getLinkVolumes();
	bar->linkVolumesButton = new QPushButton(bar->linked ? "=" : "!=", bar->frame);
	bar->linkVolumesButton->setCheckable(true);
	bar->linkVolumesButton->setChecked(bar->linked);
	bar->linkVolumesButton->setFixedWidth(22);
	fxRow->addWidget(bar->linkVolumesButton);

	QLabel *locLabel = new QLabel("Loc", bar->frame);
	locLabel->setFixedWidth(20);
	fxRow->addWidget(locLabel);
	bar->volumeLocalSlider = new QSlider(Qt::Horizontal, bar->frame);
	bar->volumeLocalSlider->setRange(0, 100);
	bar->volumeLocalSlider->setValue(m_model->getVolumeLocal());
	bar->volumeLocalSlider->setFixedWidth(60);
	fxRow->addWidget(bar->volumeLocalSlider);
	bar->volumeLocalLabel = locLabel;
	bar->volumeRemoteLabel = remLabel;

	// Volume connections
	connect(bar->volumeRemoteSlider, &QSlider::valueChanged, [this, bar, slot](int val) {
		Sampler *sampler = sb_getSampler();
		if (sampler) sampler->setSlotVolumeRemote(slot, val);
		if (bar->linked) {
			bar->volumeLocalSlider->blockSignals(true);
			bar->volumeLocalSlider->setValue(val);
			bar->volumeLocalSlider->blockSignals(false);
			if (sampler) sampler->setSlotVolumeLocal(slot, val);
		}
	});
	connect(bar->volumeLocalSlider, &QSlider::valueChanged, [this, bar, slot](int val) {
		Sampler *sampler = sb_getSampler();
		if (sampler) sampler->setSlotVolumeLocal(slot, val);
		if (bar->linked) {
			bar->volumeRemoteSlider->blockSignals(true);
			bar->volumeRemoteSlider->setValue(val);
			bar->volumeRemoteSlider->blockSignals(false);
			if (sampler) sampler->setSlotVolumeRemote(slot, val);
		}
	});
	connect(bar->linkVolumesButton, &QPushButton::toggled, [bar](bool checked) {
		bar->linked = checked;
		bar->linkVolumesButton->setText(checked ? "=" : "!=");
		if (checked) bar->volumeLocalSlider->setValue(bar->volumeRemoteSlider->value());
	});

	// Separator
	QFrame *sep = new QFrame(bar->frame);
	sep->setFrameShape(QFrame::VLine);
	sep->setFrameShadow(QFrame::Sunken);
	fxRow->addWidget(sep);

	// Per-slot FX sliders (horizontal, compact)
	auto addFxSlider = [&](const QString &label, int minV, int maxV, int defV,
						   QSlider *&slider, QLabel *&valLabel) {
		QLabel *lbl = new QLabel(label, bar->frame);
		QFont f = lbl->font(); f.setPointSize(9); lbl->setFont(f);
		lbl->setFixedWidth(36);
		fxRow->addWidget(lbl);
		slider = new QSlider(Qt::Horizontal, bar->frame);
		slider->setRange(minV, maxV);
		slider->setValue(defV);
		slider->setFixedWidth(55);
		fxRow->addWidget(slider);
		valLabel = new QLabel("", bar->frame);
		QFont vf = valLabel->font(); vf.setPointSize(9); valLabel->setFont(vf);
		valLabel->setFixedWidth(40);
		fxRow->addWidget(valLabel);
	};

	addFxSlider("P:", -100, 100, 0, bar->pitchSlider, bar->pitchLabel);
	addFxSlider("S:", -100, 100, 0, bar->speedSlider, bar->speedLabel);
	addFxSlider("P+S:", -100, 100, 0, bar->combinedSlider, bar->combinedLabel);
	addFxSlider("Rev:", 0, 100, 0, bar->reverbSlider, bar->reverbLabel);

	// Sync checkbox + Reset
	bar->syncCheckbox = new QCheckBox("Sync", bar->frame);
	bar->syncCheckbox->setChecked(false);
	QFont sf = bar->syncCheckbox->font(); sf.setPointSize(9); bar->syncCheckbox->setFont(sf);
	fxRow->addWidget(bar->syncCheckbox);

	bar->resetFxButton = new QPushButton("Reset", bar->frame);
	bar->resetFxButton->setMinimumWidth(44);
	QFont rf = bar->resetFxButton->font(); rf.setPointSize(9); bar->resetFxButton->setFont(rf);
	fxRow->addWidget(bar->resetFxButton);

	// Initial sync state
	bar->combinedSlider->setEnabled(false);

	// Lambda to update FX labels
	auto updateBarFxLabels = [bar]() {
		auto toF = [](int v) -> float { return (float)pow(3.0, v / 100.0); };
		bar->pitchLabel->setText(QString("%1x").arg(toF(bar->pitchSlider->value()), 0, 'f', 2));
		bar->speedLabel->setText(QString("%1x").arg(toF(bar->speedSlider->value()), 0, 'f', 2));
		bar->combinedLabel->setText(QString("%1x").arg(toF(bar->combinedSlider->value()), 0, 'f', 2));
		bar->reverbLabel->setText(QString("%1%").arg(bar->reverbSlider->value()));
	};
	updateBarFxLabels();

	// FX connections
	connect(bar->pitchSlider, &QSlider::valueChanged, [this, bar, slot, updateBarFxLabels](int val) {
		Sampler *s = sb_getSampler();
		if (s) s->setSlotPitchFactor(slot, (float)pow(3.0, val / 100.0));
		if (bar->syncCheckbox->isChecked()) {
			bar->speedSlider->blockSignals(true);
			bar->speedSlider->setValue(val);
			bar->speedSlider->blockSignals(false);
			if (s) s->setSlotSpeedFactor(slot, (float)pow(3.0, val / 100.0));
		}
		updateBarFxLabels();
	});
	connect(bar->speedSlider, &QSlider::valueChanged, [this, bar, slot, updateBarFxLabels](int val) {
		Sampler *s = sb_getSampler();
		if (s) s->setSlotSpeedFactor(slot, (float)pow(3.0, val / 100.0));
		if (bar->syncCheckbox->isChecked()) {
			bar->pitchSlider->blockSignals(true);
			bar->pitchSlider->setValue(val);
			bar->pitchSlider->blockSignals(false);
			if (s) s->setSlotPitchFactor(slot, (float)pow(3.0, val / 100.0));
		}
		updateBarFxLabels();
	});
	connect(bar->combinedSlider, &QSlider::valueChanged, [this, bar, slot, updateBarFxLabels](int val) {
		Sampler *s = sb_getSampler();
		float f = (float)pow(3.0, val / 100.0);
		bar->pitchSlider->blockSignals(true);
		bar->speedSlider->blockSignals(true);
		bar->pitchSlider->setValue(val);
		bar->speedSlider->setValue(val);
		bar->pitchSlider->blockSignals(false);
		bar->speedSlider->blockSignals(false);
		if (s) { s->setSlotPitchFactor(slot, f); s->setSlotSpeedFactor(slot, f); }
		updateBarFxLabels();
	});
	connect(bar->reverbSlider, &QSlider::valueChanged, [this, bar, slot, updateBarFxLabels](int val) {
		Sampler *s = sb_getSampler();
		if (s) s->setSlotReverbMix(slot, val / 100.0f);
		updateBarFxLabels();
	});
	connect(bar->syncCheckbox, &QCheckBox::toggled, [bar, updateBarFxLabels](bool checked) {
		bar->combinedSlider->setEnabled(checked);
		bar->pitchSlider->setEnabled(!checked);
		bar->speedSlider->setEnabled(!checked);
		if (checked) {
			int val = bar->pitchSlider->value();
			bar->combinedSlider->blockSignals(true);
			bar->combinedSlider->setValue(val);
			bar->combinedSlider->blockSignals(false);
			bar->speedSlider->blockSignals(true);
			bar->speedSlider->setValue(val);
			bar->speedSlider->blockSignals(false);
		}
		updateBarFxLabels();
	});
	connect(bar->resetFxButton, &QPushButton::clicked, [bar, slot, updateBarFxLabels, this]() {
		Sampler *s = sb_getSampler();
		bar->pitchSlider->setValue(0);
		bar->speedSlider->setValue(0);
		bar->combinedSlider->setValue(0);
		bar->reverbSlider->setValue(0);
		bar->syncCheckbox->setChecked(false);
		if (s) { s->setSlotPitchFactor(slot, 1.0f); s->setSlotSpeedFactor(slot, 1.0f); s->setSlotReverbMix(slot, 0.0f); }
		updateBarFxLabels();
	});

	outerLayout->addLayout(fxRow);

	ui->multiBarLayout->addWidget(bar->frame);
	m_playbackBars.push_back(bar);
	return bar;
}


//---------------------------------------------------------------
// Purpose: Add per-slot volume sliders (Remote + Link + Local) to a bar.
//         Inserted BEFORE the waveform in the layout.
//---------------------------------------------------------------
void ConfigQt::addVolumeSliders(PlaybackBar *bar)
{
	QHBoxLayout *layout = qobject_cast<QHBoxLayout*>(bar->frame->layout());
	int slot = bar->slot;

	// Find waveform widget index to insert before it
	int waveIdx = layout->indexOf(bar->waveformView);

	// Remote volume slider
	bar->volumeRemoteLabel = new QLabel("Rem", bar->frame);
	bar->volumeRemoteLabel->setToolTip("Volume Remote (what others hear)");
	bar->volumeRemoteLabel->setFixedWidth(22);
	layout->insertWidget(waveIdx++, bar->volumeRemoteLabel);

	bar->volumeRemoteSlider = new QSlider(Qt::Horizontal, bar->frame);
	bar->volumeRemoteSlider->setRange(0, 100);
	bar->volumeRemoteSlider->setValue(m_model->getVolumeRemote());
	bar->volumeRemoteSlider->setFixedWidth(70);
	bar->volumeRemoteSlider->setToolTip("Volume Remote");
	layout->insertWidget(waveIdx++, bar->volumeRemoteSlider);

	// Link button
	bar->linked = m_model->getLinkVolumes();
	bar->linkVolumesButton = new QPushButton(bar->linked ? "=" : "!=", bar->frame);
	bar->linkVolumesButton->setCheckable(true);
	bar->linkVolumesButton->setChecked(bar->linked);
	bar->linkVolumesButton->setFixedWidth(22);
	bar->linkVolumesButton->setToolTip("Link Local/Remote volumes");
	layout->insertWidget(waveIdx++, bar->linkVolumesButton);

	// Local volume slider
	bar->volumeLocalLabel = new QLabel("Loc", bar->frame);
	bar->volumeLocalLabel->setToolTip("Volume Local (what you hear)");
	bar->volumeLocalLabel->setFixedWidth(20);
	layout->insertWidget(waveIdx++, bar->volumeLocalLabel);

	bar->volumeLocalSlider = new QSlider(Qt::Horizontal, bar->frame);
	bar->volumeLocalSlider->setRange(0, 100);
	bar->volumeLocalSlider->setValue(m_model->getVolumeLocal());
	bar->volumeLocalSlider->setFixedWidth(70);
	bar->volumeLocalSlider->setToolTip("Volume Local");
	layout->insertWidget(waveIdx++, bar->volumeLocalSlider);

	// Separator before waveform
	QFrame *sep = new QFrame(bar->frame);
	sep->setFrameShape(QFrame::VLine);
	sep->setFrameShadow(QFrame::Sunken);
	layout->insertWidget(waveIdx++, sep);

	// If linked, sync local to remote's value
	if (bar->linked)
		bar->volumeLocalSlider->setValue(bar->volumeRemoteSlider->value());

	// Connect remote slider
	connect(bar->volumeRemoteSlider, &QSlider::valueChanged, [this, bar, slot](int val) {
		Sampler *sampler = sb_getSampler();
		if (sampler) sampler->setSlotVolumeRemote(slot, val);
		if (bar->linked)
		{
			bar->volumeLocalSlider->blockSignals(true);
			bar->volumeLocalSlider->setValue(val);
			bar->volumeLocalSlider->blockSignals(false);
			if (sampler) sampler->setSlotVolumeLocal(slot, val);
		}
	});

	// Connect local slider
	connect(bar->volumeLocalSlider, &QSlider::valueChanged, [this, bar, slot](int val) {
		Sampler *sampler = sb_getSampler();
		if (sampler) sampler->setSlotVolumeLocal(slot, val);
		if (bar->linked)
		{
			bar->volumeRemoteSlider->blockSignals(true);
			bar->volumeRemoteSlider->setValue(val);
			bar->volumeRemoteSlider->blockSignals(false);
			if (sampler) sampler->setSlotVolumeRemote(slot, val);
		}
	});

	// Connect link button
	connect(bar->linkVolumesButton, &QPushButton::toggled, [bar](bool checked) {
		bar->linked = checked;
		bar->linkVolumesButton->setText(checked ? "=" : "!=");
		if (checked)
		{
			// Sync local to remote
			bar->volumeLocalSlider->setValue(bar->volumeRemoteSlider->value());
		}
	});
}


//---------------------------------------------------------------
// Purpose: Handle multi-mode toggle - grey out global sliders
//---------------------------------------------------------------
void ConfigQt::onMultiModeChanged(bool enabled)
{
	// Grey out global volume sliders (don't reset their values)
	ui->sl_volumeLocal->setEnabled(!enabled);
	ui->sl_volumeRemote->setEnabled(!enabled);
	ui->cb_link_volumes->setEnabled(!enabled);

	if (enabled)
	{
		// Create slot-0 volume widget if needed
		if (!m_slot0VolumeWidget)
		{
			// Build a minimal PlaybackBar-like widget for slot 0 volumes
			m_slot0VolumeWidget = new QWidget(this);
			QHBoxLayout *sl0Layout = new QHBoxLayout(m_slot0VolumeWidget);
			sl0Layout->setSpacing(4);
			sl0Layout->setContentsMargins(6, 2, 6, 2);

			// Remote
			m_slot0VolRemoteLabel = new QLabel("Rem", m_slot0VolumeWidget);
			m_slot0VolRemoteLabel->setFixedWidth(22);
			sl0Layout->addWidget(m_slot0VolRemoteLabel);

			m_slot0VolumeRemote = new QSlider(Qt::Horizontal, m_slot0VolumeWidget);
			m_slot0VolumeRemote->setRange(0, 100);
			m_slot0VolumeRemote->setValue(m_model->getVolumeRemote());
			m_slot0VolumeRemote->setToolTip("Volume Remote - Main Slot");
			sl0Layout->addWidget(m_slot0VolumeRemote);

			// Link
			QPushButton *sl0Link = new QPushButton(m_model->getLinkVolumes() ? "=" : "!=", m_slot0VolumeWidget);
			sl0Link->setCheckable(true);
			sl0Link->setChecked(m_model->getLinkVolumes());
			sl0Link->setFixedWidth(22);
			sl0Link->setToolTip("Link Local/Remote volumes");
			sl0Layout->addWidget(sl0Link);

			// Local
			m_slot0VolLocalLabel = new QLabel("Loc", m_slot0VolumeWidget);
			m_slot0VolLocalLabel->setFixedWidth(20);
			sl0Layout->addWidget(m_slot0VolLocalLabel);

			m_slot0VolumeLocal = new QSlider(Qt::Horizontal, m_slot0VolumeWidget);
			m_slot0VolumeLocal->setRange(0, 100);
			m_slot0VolumeLocal->setValue(m_model->getVolumeLocal());
			m_slot0VolumeLocal->setToolTip("Volume Local - Main Slot");
			sl0Layout->addWidget(m_slot0VolumeLocal);

			sl0Layout->addStretch();

			// Connections
			bool *sl0linked = new bool(m_model->getLinkVolumes());

			connect(m_slot0VolumeRemote, &QSlider::valueChanged, [this, sl0linked](int val) {
				Sampler *sampler = sb_getSampler();
				if (sampler) sampler->setSlotVolumeRemote(0, val);
				if (*sl0linked)
				{
					m_slot0VolumeLocal->blockSignals(true);
					m_slot0VolumeLocal->setValue(val);
					m_slot0VolumeLocal->blockSignals(false);
					if (sampler) sampler->setSlotVolumeLocal(0, val);
				}
			});
			connect(m_slot0VolumeLocal, &QSlider::valueChanged, [this, sl0linked](int val) {
				Sampler *sampler = sb_getSampler();
				if (sampler) sampler->setSlotVolumeLocal(0, val);
				if (*sl0linked)
				{
					m_slot0VolumeRemote->blockSignals(true);
					m_slot0VolumeRemote->setValue(val);
					m_slot0VolumeRemote->blockSignals(false);
					if (sampler) sampler->setSlotVolumeRemote(0, val);
				}
			});
			connect(sl0Link, &QPushButton::toggled, [sl0linked, sl0Link, this](bool checked) {
				*sl0linked = checked;
				sl0Link->setText(checked ? "=" : "!=");
				if (checked)
					m_slot0VolumeLocal->setValue(m_slot0VolumeRemote->value());
			});

			ui->multiBarLayout->insertWidget(0, m_slot0VolumeWidget);
		}
		// Reset slot-0 sliders to current global values
		m_slot0VolumeRemote->setValue(m_model->getVolumeRemote());
		m_slot0VolumeLocal->setValue(m_model->getVolumeLocal());
		m_slot0VolumeWidget->show();
	}
	else
	{
		if (m_slot0VolumeWidget)
			m_slot0VolumeWidget->hide();
	}
}


//---------------------------------------------------------------
// Purpose: Remove a dynamic playback bar for a slot
//---------------------------------------------------------------
void ConfigQt::removePlaybackBar(int slot)
{
	for (auto it = m_playbackBars.begin(); it != m_playbackBars.end(); ++it)
	{
		if ((*it)->slot == slot)
		{
			ui->multiBarLayout->removeWidget((*it)->frame);
			delete (*it)->frame;
			delete *it;
			m_playbackBars.erase(it);
			return;
		}
	}
}


//---------------------------------------------------------------
// Purpose: Update all dynamic playback bars (called from timer)
//---------------------------------------------------------------
void ConfigQt::updateAllPlaybackBars()
{
	Sampler *sampler = sb_getSampler();
	if (!sampler) return;

	for (PlaybackBar *bar : m_playbackBars)
	{
		double pos = sampler->getPosition(bar->slot);
		double len = sampler->getLength(bar->slot);

		if (len > 0.0)
		{
			bar->waveformView->setPlaybackPosition(pos / len);
			int posMin = (int)pos / 60;
			int posSec = (int)pos % 60;
			int lenMin = (int)len / 60;
			int lenSec = (int)len % 60;
			bar->timeLabel->setText(
				QString("%1:%2 / %3:%4")
					.arg(posMin)
					.arg(posSec, 2, 10, QChar('0'))
					.arg(lenMin)
					.arg(lenSec, 2, 10, QChar('0'))
			);
		}
	}
}


void ConfigQt::onResetFx()
{
	m_pitchSlider->blockSignals(true);
	m_speedSlider->blockSignals(true);
	m_combinedSlider->blockSignals(true);
	m_reverbSlider->blockSignals(true);
	m_pitchSlider->setValue(0);
	m_speedSlider->setValue(0);
	m_combinedSlider->setValue(0);
	m_reverbSlider->setValue(0);
	m_pitchSlider->blockSignals(false);
	m_speedSlider->blockSignals(false);
	m_combinedSlider->blockSignals(false);
	m_reverbSlider->blockSignals(false);
	m_syncButton->setChecked(false);
	m_model->setPitchValue(0);
	m_model->setSpeedValue(0);
	m_model->setReverbValue(0);
	m_model->setSyncPitchSpeed(false);
	m_rememberedPitchValue = 0;
	m_rememberedSpeedValue = 0;
	m_rememberedCombinedValue = 0;
	updatePitchSpeedLabels();
}


void ConfigQt::onRememberPitchSpeedChanged(bool checked)
{
	m_model->setRememberPitchSpeed(checked);
}


void ConfigQt::onReverbValueChanged(int value)
{
	m_model->setReverbValue(value);
	updatePitchSpeedLabels();
}


void ConfigQt::buildPitchSpeedUI()
{
	// Build 3 vertical sliders side by side + sync button + reset button
	// inside the pitchSpeedContainer from the .ui file
	QHBoxLayout *mainRow = new QHBoxLayout(ui->pitchSpeedContainer);
	mainRow->setContentsMargins(0, 0, 0, 0);
	mainRow->setSpacing(6);

	auto createSliderColumn = [](const QString &title, QSlider *&slider, QLabel *&valueLabel) -> QVBoxLayout*
	{
		QVBoxLayout *col = new QVBoxLayout();
		col->setSpacing(2);
		col->setAlignment(Qt::AlignHCenter);

		QLabel *titleLabel = new QLabel(title);
		titleLabel->setAlignment(Qt::AlignHCenter);
		QFont f = titleLabel->font();
		f.setPointSize(10);
		titleLabel->setFont(f);
		col->addWidget(titleLabel, 0, Qt::AlignHCenter);

		slider = new QSlider(Qt::Vertical);
		slider->setRange(-100, 100);
		slider->setValue(0);
		slider->setTickPosition(QSlider::TicksBothSides);
		slider->setTickInterval(25);
		slider->setMinimumHeight(80);
		slider->setMaximumHeight(90);
		slider->setFixedWidth(30);
		col->addWidget(slider, 0, Qt::AlignHCenter);

		valueLabel = new QLabel("1.00x");
		valueLabel->setAlignment(Qt::AlignHCenter);
		QFont vf = valueLabel->font();
		vf.setPointSize(9);
		valueLabel->setFont(vf);
		valueLabel->setMinimumWidth(35);
		col->addWidget(valueLabel, 0, Qt::AlignHCenter);

		return col;
	};

	// Pitch column
	QVBoxLayout *pitchCol = createSliderColumn("Pitch", m_pitchSlider, m_pitchValueLabel);
	mainRow->addLayout(pitchCol);

	// Speed column
	QVBoxLayout *speedCol = createSliderColumn("Speed", m_speedSlider, m_speedValueLabel);
	mainRow->addLayout(speedCol);

	// Combined column
	QVBoxLayout *combinedCol = createSliderColumn("P+S", m_combinedSlider, m_combinedValueLabel);
	m_combinedSlider->setEnabled(false);
	mainRow->addLayout(combinedCol);

	// Reverb column — range 0..100 (percentage), not the same log mapping as pitch/speed
	{
		QVBoxLayout *reverbCol = new QVBoxLayout();
		reverbCol->setSpacing(2);
		reverbCol->setAlignment(Qt::AlignHCenter);

		QLabel *titleLabel = new QLabel("Reverb");
		titleLabel->setAlignment(Qt::AlignHCenter);
		QFont f = titleLabel->font();
		f.setPointSize(10);
		titleLabel->setFont(f);
		reverbCol->addWidget(titleLabel, 0, Qt::AlignHCenter);

		m_reverbSlider = new QSlider(Qt::Vertical);
		m_reverbSlider->setRange(0, 100);
		m_reverbSlider->setValue(0);
		m_reverbSlider->setTickPosition(QSlider::TicksBothSides);
		m_reverbSlider->setTickInterval(25);
		m_reverbSlider->setMinimumHeight(80);
		m_reverbSlider->setMaximumHeight(90);
		m_reverbSlider->setFixedWidth(30);
		reverbCol->addWidget(m_reverbSlider, 0, Qt::AlignHCenter);

		m_reverbValueLabel = new QLabel("0%");
		m_reverbValueLabel->setAlignment(Qt::AlignHCenter);
		QFont vf = m_reverbValueLabel->font();
		vf.setPointSize(9);
		m_reverbValueLabel->setFont(vf);
		m_reverbValueLabel->setMinimumWidth(35);
		reverbCol->addWidget(m_reverbValueLabel, 0, Qt::AlignHCenter);

		mainRow->addLayout(reverbCol);
	}

	// Buttons column (Sync + Reset)
	QVBoxLayout *btnCol = new QVBoxLayout();
	btnCol->setSpacing(6);
	btnCol->setAlignment(Qt::AlignVCenter);

	m_syncButton = new QPushButton("Sync");
	m_syncButton->setCheckable(true);
	m_syncButton->setChecked(false);
	m_syncButton->setFixedWidth(50);
	m_syncButton->setToolTip("Link Pitch and Speed to the Combined slider");
	btnCol->addWidget(m_syncButton, 0, Qt::AlignHCenter);

	m_resetFxButton = new QPushButton("Reset");
	m_resetFxButton->setFixedWidth(50);
	m_resetFxButton->setToolTip("Reset all effects to 1.00x");
	btnCol->addWidget(m_resetFxButton, 0, Qt::AlignHCenter);

	mainRow->addLayout(btnCol);

	// Connect signals
	connect(m_pitchSlider, &QSlider::valueChanged, this, &ConfigQt::onPitchValueChanged);
	connect(m_speedSlider, &QSlider::valueChanged, this, &ConfigQt::onSpeedValueChanged);
	connect(m_combinedSlider, &QSlider::valueChanged, this, &ConfigQt::onCombinedValueChanged);
	connect(m_reverbSlider, &QSlider::valueChanged, this, &ConfigQt::onReverbValueChanged);
	connect(m_syncButton, &QPushButton::toggled, this, &ConfigQt::onSyncToggled);
	connect(m_resetFxButton, &QPushButton::clicked, this, &ConfigQt::onResetFx);

	// Initialize from model
	m_reverbSlider->setValue(m_model->getReverbValue());
	m_pitchSlider->setValue(m_model->getPitchValue());
	m_speedSlider->setValue(m_model->getSpeedValue());
	m_combinedSlider->setValue(m_model->getPitchValue());
	bool sync = m_model->getSyncPitchSpeed();
	m_syncButton->setChecked(sync);
	m_combinedSlider->setEnabled(sync);
	m_pitchSlider->setEnabled(!sync);
	m_speedSlider->setEnabled(!sync);
	updatePitchSpeedLabels();
}


void ConfigQt::updatePitchSpeedLabels()
{
	// Logarithmic mapping: factor = 3^(value/100), so 0=1.00x, 100=3.00x, -100=0.33x
	auto sliderToFactor = [](int value) -> float { return (float)pow(3.0, value / 100.0); };
	m_pitchValueLabel->setText(QString("%1x").arg(sliderToFactor(m_pitchSlider->value()), 0, 'f', 2));
	m_speedValueLabel->setText(QString("%1x").arg(sliderToFactor(m_speedSlider->value()), 0, 'f', 2));
	m_combinedValueLabel->setText(QString("%1x").arg(sliderToFactor(m_combinedSlider->value()), 0, 'f', 2));
	m_reverbValueLabel->setText(QString("%1%").arg(m_reverbSlider->value()));
}


//---------------------------------------------------------------
// Purpose:
//---------------------------------------------------------------
void ConfigQt::openHotkeySetDialog(size_t buttonId)
{
	openHotkeySetDialog(buttonId, this);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::openButtonColorDialog(size_t buttonId)
{
	QColorDialog dialog;
	const SoundInfo *pinfo = m_model->getSoundInfo(buttonId);
	SoundInfo info = pinfo ? *pinfo : SoundInfo();
	dialog.setCurrentColor(info.customColor);
	dialog.setWindowTitle("Choose button color");
	if (dialog.exec())
	{
		info.customColor = dialog.currentColor();
		m_model->setSoundInfo(buttonId, info);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigQt::unescapeCustomText(const QString &text)
{
	QString cpy = text;
	return cpy.replace("\\n", "\n");
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::openHotkeySetDialog( size_t buttonId, QWidget *parent )
{
	char intName[16];
	sb_getInternalHotkeyName((int)buttonId, intName);
	ts3Functions.requestHotkeyInputDialog(getPluginID(), intName, 0, parent);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigQt::getShortcutString(const char *internalName)
{
	std::vector<char> name(128, 0);
	char *namePtr = name.data();
	unsigned int res = ts3Functions.getHotkeyFromKeyword(
		getPluginID(), &internalName, &namePtr, 1, 128);
	QString str = res == 0 ? QString(name.data()) : QString();
	return str;
}



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QString ConfigQt::getShortcutString(size_t buttonId)
{
	char intName[16];
	sb_getInternalHotkeyName((int)buttonId, intName);
	return getShortcutString(intName);
}

QString ConfigQt::getConfigShortcutString(int cfg)
{
	char buf[16];
	sb_getInternalConfigHotkeyName(cfg, buf);
    QString shortcut = getShortcutString(buf);
    if (!shortcut.isEmpty())
        return shortcut;

    return QString("no hotkey");
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onHotkeyRecordedEvent(const char *keyword, const char *key)
{
    QString sKey = key;

	int configId = -1;
	if (sscanf(keyword, "config_%i", &configId) == 1)
	{
		if (configId >= 0 && configId < NUM_CONFIGS)
			m_configHotkeyButtons[configId]->setToolTip(sKey);
		else
			logError("Invalid hotkey keyword: %s", keyword);
	}
    else
    {
        QString sKeyword = keyword;

        emit hotkeyRecordedEvent(sKey, sKeyword);

        if (m_model->getShowHotkeysOnButtons())
            for (size_t i = 0; i < m_buttons.size(); i++)
                updateButtonText(i);
    }
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateShowHotkeysOnButtons(bool val)
{
	m_model->setShowHotkeysOnButtons(val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onUpdateHotkeysDisabled(bool val)
{
	m_model->setHotkeysEnabled(!val);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::showEvent(QShowEvent *evt)
{
	QWidget::showEvent(evt);

	for(size_t i = 0; i < m_buttons.size(); i++)
		updateButtonText(i);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onButtonFileDropped(const QList<QUrl> &urls)
{
	int buttonId = sender()->property("buttonId").toInt();
	int buttonNr = 0;

	for(int i = 0; i < urls.size(); i++)
	{
		if (buttonNr == 1)
		{
			QMessageBox msgbox(QMessageBox::Icon::Question, "Fill buttons?",
				"You dropped multiple files. Consecutively apply them to the buttons following the one you dropped your files on?",
				QMessageBox::Yes | QMessageBox::No, this);
			if (msgbox.exec() == QMessageBox::No)
				break;
		}

		if (urls[i].isLocalFile())
		{
			QFileInfo info(urls[i].toLocalFile());
			if (info.isFile())
			{
				setButtonFile(buttonId + buttonNr, urls[i].toLocalFile(), buttonNr == 0);
				++buttonNr;
			}
			else if (buttonNr == 0)
			{
				QMessageBox msgBox(QMessageBox::Icon::Critical, "Unsupported drop type", "Some things could not be dropped here :(", QMessageBox::Ok, this);
				msgBox.exec();
			}
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::setButtonFile(size_t buttonId, const QString &fn, bool askForDisablingCrop)
{
	const SoundInfo *info = m_model->getSoundInfo(buttonId);
	if (askForDisablingCrop && info && info->cropEnabled && info->filename != fn)
	{
		QMessageBox mb(QMessageBox::Question, "Keep crop settings?",
			"You selected a new file for a button that has 'crop sound' enabled.", QMessageBox::NoButton, this);
		QPushButton *btnDisable = mb.addButton("Disable cropping (recommended)", QMessageBox::YesRole);
		QPushButton *btnKeep = mb.addButton("Keep old crop settings", QMessageBox::NoRole);
		mb.setDefaultButton(btnDisable);
		mb.exec();
		if (mb.clickedButton() != btnKeep)
		{
			SoundInfo newInfo(*info);
			newInfo.cropEnabled = false;
			m_model->setSoundInfo(buttonId, newInfo);
		}
	}
	m_model->setFileName(buttonId, fn);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onButtonPausePressed()
{
	sb_pauseButtonPressed();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onButtonDroppedOnButton(SoundButton *button)
{
	SoundButton *btn0 = button;
	SoundButton *btn1 = qobject_cast<SoundButton*>(sender());
	int bid0 = btn0->property("buttonId").toInt();
	int bid1 = btn1->property("buttonId").toInt();
	const SoundInfo *info0 = m_model->getSoundInfo(bid0);
	const SoundInfo *info1 = m_model->getSoundInfo(bid1);

	// Copy sound info
	SoundInfo infoCopy0;
	SoundInfo infoCopy1;
	if (info0)
		infoCopy0 = *info0;
	if (info1)
		infoCopy1 = *info1;

	// And switch em
	m_model->setSoundInfo(bid0, infoCopy1);
	m_model->setSoundInfo(bid1, infoCopy0);

	// Switch button position and then animate the buttons to slide into place
	const int animDuration = 300;
	QPropertyAnimation *anim0 = new QPropertyAnimation(btn0, "pos");
	anim0->setStartValue(btn1->pos());
	anim0->setEndValue(btn0->pos());
	anim0->setDuration(animDuration);

	QPropertyAnimation *anim1 = new QPropertyAnimation(btn1, "pos");
	anim1->setStartValue(btn0->pos());
	anim1->setEndValue(btn1->pos());
	anim1->setDuration(animDuration);

	anim0->start();
	anim1->start();
	btn0->raise();
	btn1->raise();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onFilterEditTextChanged(const QString &text)
{
	QString filter = text.trimmed();
	for (auto &button : m_buttons)
	{
		int buttonId = button->property("buttonId").toInt();
		const SoundInfo *info = m_model->getSoundInfo(buttonId);
		bool hasFile = info && !info->filename.isEmpty();
		QString buttonText = button->text();
		bool pass = filter.length() == 0 || (hasFile && buttonText.contains(filter, Qt::CaseInsensitive));
		button->setVisible(pass);
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onVolumeSliderContextMenuLocal(const QPoint &point)
{
	QString hotkeyStringIncr = getShortcutString(HOTKEY_VOLUME_INCREASE);
	QString hotkeyTextIncr = "Set 'increase 20%' hotkey (Current: " +
		(hotkeyStringIncr.isEmpty() ? QString("None") : hotkeyStringIncr) + ")";

	QString hotkeyStringDecr = getShortcutString(HOTKEY_VOLUME_DECREASE);
	QString hotkeyTextDecr = "Set 'decrease 20%' hotkey (Current: " +
		(hotkeyStringDecr.isEmpty() ? QString("None") : hotkeyStringDecr) + ")";

	QMenu menu;
	QAction *actIncr = menu.addAction(hotkeyTextIncr);
	QAction *actDecr = menu.addAction(hotkeyTextDecr);
	QAction *action = menu.exec(ui->sl_volumeLocal->mapToGlobal(point));
	if (action == actIncr)
		ts3Functions.requestHotkeyInputDialog(getPluginID(), HOTKEY_VOLUME_INCREASE, 0, this);
	else if (action == actDecr)
		ts3Functions.requestHotkeyInputDialog(getPluginID(), HOTKEY_VOLUME_DECREASE, 0, this);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::onVolumeSliderContextMenuRemote(const QPoint &point)
{
	QString hotkeyStringIncr = getShortcutString(HOTKEY_VOLUME_INCREASE);
	QString hotkeyTextIncr = "Set 'increase 20%' hotkey (Current: " +
		(hotkeyStringIncr.isEmpty() ? QString("None") : hotkeyStringIncr) + ")";

	QString hotkeyStringDecr = getShortcutString(HOTKEY_VOLUME_DECREASE);
	QString hotkeyTextDecr = "Set 'decrease 20%' hotkey (Current: " +
		(hotkeyStringDecr.isEmpty() ? QString("None") : hotkeyStringDecr) + ")";

	QMenu menu;
	QAction *actIncr = menu.addAction(hotkeyTextIncr);
	QAction *actDecr = menu.addAction(hotkeyTextDecr);
	QAction *action = menu.exec(ui->sl_volumeRemote->mapToGlobal(point));
	if (action == actIncr)
		ts3Functions.requestHotkeyInputDialog(getPluginID(), HOTKEY_VOLUME_INCREASE, 0, this);
	else if (action == actDecr)
		ts3Functions.requestHotkeyInputDialog(getPluginID(), HOTKEY_VOLUME_DECREASE, 0, this);
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void ConfigQt::ModelObserver::notify(ConfigModel &model, ConfigModel::notifications_e what, int data)
{
    //p.ui->labelStatus->setText(QString("Notify Code: %1").arg((int)what));

	switch(what)
	{
	case ConfigModel::NOTIFY_SET_ROWS:
		p.ui->sb_rows->setValue(model.getRows());
		p.createButtons();
		break;
	case ConfigModel::NOTIFY_SET_COLS:
		p.ui->sb_cols->setValue(model.getCols());
		p.createButtons();
		break;
	case ConfigModel::NOTIFY_SET_VOLUME_LOCAL:
		if (p.ui->sl_volumeLocal->value() != model.getVolumeLocal())
			p.ui->sl_volumeLocal->setValue(model.getVolumeLocal());
		break;
	case ConfigModel::NOTIFY_SET_VOLUME_REMOTE:
		if (p.ui->sl_volumeRemote->value() != model.getVolumeRemote())
			p.ui->sl_volumeRemote->setValue(model.getVolumeRemote());
		break;
	case ConfigModel::NOTIFY_SET_PLAYBACK_LOCAL:
		if (p.ui->cb_mute_locally->isChecked() != !model.getPlaybackLocal())
			p.ui->cb_mute_locally->setChecked(!model.getPlaybackLocal());
		break;
	case ConfigModel::NOTIFY_SET_SOUND:
		p.updateButtonText(data);
		break;
	case ConfigModel::NOTIFY_SET_MUTE_MYSELF_DURING_PB:
		if (p.ui->cb_mute_myself->isChecked() != model.getMuteMyselfDuringPb())
			p.ui->cb_mute_myself->setChecked(model.getMuteMyselfDuringPb());
		break;
	case ConfigModel::NOTIFY_SET_WINDOW_SIZE:
		{
			QSize s = p.size();
			int w = 0, h = 0;
			model.getWindowSize(&w, &h);
			if(s.width() != w || s.height() != h)
				p.resize(w, h);
		}
		break;
	case ConfigModel::NOTIFY_SET_SHOW_HOTKEYS_ON_BUTTONS:
		if (p.ui->cb_show_hotkeys_on_buttons->isChecked() != model.getShowHotkeysOnButtons())
			p.ui->cb_show_hotkeys_on_buttons->setChecked(model.getShowHotkeysOnButtons());
		for(size_t i = 0; i < p.m_buttons.size(); i++)
			p.updateButtonText(i);
		break;
	case ConfigModel::NOTIFY_SET_HOTKEYS_ENABLED:
		if (p.ui->cb_disable_hotkeys->isChecked() == model.getHotkeysEnabled())
			p.ui->cb_disable_hotkeys->setChecked(!model.getHotkeysEnabled());
		break;
	case ConfigModel::NOTIFY_SET_LINK_VOLUMES:
		if (p.ui->cb_link_volumes->isChecked() != model.getLinkVolumes())
			p.ui->cb_link_volumes->setChecked(model.getLinkVolumes());
		break;
	case ConfigModel::NOTIFY_SET_EARRAPE_PROTECTION:
		if (p.ui->cb_earrape_protection->isChecked() != model.getEarrapeProtection())
			p.ui->cb_earrape_protection->setChecked(model.getEarrapeProtection());
		break;
	case ConfigModel::NOTIFY_SET_PITCH_SPEED:
	{
		// Legacy: keep remember checkbox in sync
		if (p.ui->cb_remember_pitch_speed->isChecked() != model.getRememberPitchSpeed())
			p.ui->cb_remember_pitch_speed->setChecked(model.getRememberPitchSpeed());
		break;
	}
	case ConfigModel::NOTIFY_SET_PITCH:
	{
		int val = model.getPitchValue();
		if (p.m_pitchSlider->value() != val)
		{
			p.m_pitchSlider->blockSignals(true);
			p.m_pitchSlider->setValue(val);
			p.m_pitchSlider->blockSignals(false);
		}
		if (model.getSyncPitchSpeed())
		{
			p.m_combinedSlider->blockSignals(true);
			p.m_combinedSlider->setValue(val);
			p.m_combinedSlider->blockSignals(false);
		}
		p.updatePitchSpeedLabels();
		break;
	}
	case ConfigModel::NOTIFY_SET_SPEED:
	{
		int val = model.getSpeedValue();
		if (p.m_speedSlider->value() != val)
		{
			p.m_speedSlider->blockSignals(true);
			p.m_speedSlider->setValue(val);
			p.m_speedSlider->blockSignals(false);
		}
		p.updatePitchSpeedLabels();
		break;
	}
	case ConfigModel::NOTIFY_SET_SYNC_PITCH_SPEED:
	{
		bool sync = model.getSyncPitchSpeed();
		p.m_syncButton->blockSignals(true);
		p.m_syncButton->setChecked(sync);
		p.m_syncButton->blockSignals(false);
		p.m_combinedSlider->setEnabled(sync);
		p.m_pitchSlider->setEnabled(!sync);
		p.m_speedSlider->setEnabled(!sync);
		p.updatePitchSpeedLabels();
		break;
	}
	case ConfigModel::NOTIFY_SET_REVERB:
	{
		int val = model.getReverbValue();
		if (p.m_reverbSlider->value() != val)
		{
			p.m_reverbSlider->blockSignals(true);
			p.m_reverbSlider->setValue(val);
			p.m_reverbSlider->blockSignals(false);
		}
		p.updatePitchSpeedLabels();
		break;
	}
	case ConfigModel::NOTIFY_SET_MULTI_SOUNDBOARD:
		if (p.ui->cb_multi_soundboard->isChecked() != model.getMultiSoundboard())
			p.ui->cb_multi_soundboard->setChecked(model.getMultiSoundboard());
		p.onMultiModeChanged(model.getMultiSoundboard());
		break;
	default:
		break;
	}
}
