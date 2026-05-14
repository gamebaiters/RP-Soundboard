// src/main.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "common.h"

#include <cstdio>
#include <cmath>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <fstream>
#include <vector>
#include <cstdarg>
#include <map>

#include <QObject>
#include <QMessageBox>
#include <QString>
#include <QCoreApplication>
#include <QEventLoop>
#include <QEvent>
#include <QThread>

#include "main.h"
#include "plugin.h"
#include "ts3log.h"
#include "inputfile.h"
#include "samples.h"
#include "SampleVisualizerThread.h"
#include "config_qt.h"
#include "about_qt.h"
#include "ConfigModel.h"
#include "UpdateChecker.h"
#include "SoundInfo.h"
#include "TalkStateManager.h"
#include "SpeechBubble.h"
#include "modules/main_page.h"
#include "modules/main_page_wiring.h"
#include "modules/theme.h"
#include "modules/button_grid.h"
#include "modules/hotkey_block.h"
#include <QApplication>

extern "C" void rpsb_close_debug_log();

class ModelObserver_Prog : public ConfigModel::Observer
{
public:
	void notify(ConfigModel &model, ConfigModel::notifications_e what, int data) override;
};


static uint64 activeServerId = 1;

ConfigModel *configModel = NULL;
SpeechBubble *notConnectedBubble = NULL;
ConfigQt *configDialog = NULL;          // legacy window, kept for fallback
MainPage *mainPage = NULL;              // active modular UI
AboutQt *aboutDialog = NULL;
Sampler *sampler = NULL;
TalkStateManager *tsMgr = NULL;

bool hotkeysTemporarilyDisabled = false;

ModelObserver_Prog *modelObserver = NULL;
UpdateChecker *updateChecker = NULL;
std::map<uint64, int> connectionStatusMap;
typedef std::lock_guard<std::mutex> Lock;


void ModelObserver_Prog::notify(ConfigModel &model, ConfigModel::notifications_e what, int data)
{
	switch(what)
	{
	case ConfigModel::NOTIFY_SET_VOLUME_LOCAL:
		sampler->setVolumeLocal(data);
		break;
	case ConfigModel::NOTIFY_SET_VOLUME_REMOTE:
		sampler->setVolumeRemote(data);
		break;
	case ConfigModel::NOTIFY_SET_PLAYBACK_LOCAL:
		sampler->setLocalPlayback(model.getPlaybackLocal());
		break;
	case ConfigModel::NOTIFY_SET_MUTE_MYSELF_DURING_PB:
		sampler->setMuteMyself(model.getMuteMyselfDuringPb());
		break;
	case ConfigModel::NOTIFY_SET_EARRAPE_PROTECTION:
		sampler->setEarrapeProtection(model.getEarrapeProtection());
		break;
	case ConfigModel::NOTIFY_SET_PITCH_SPEED:
	{
		float factor = (float)pow(3.0, data / 100.0);
		sampler->setPitchFactor(factor);
		sampler->setSpeedFactor(factor);
		break;
	}
	case ConfigModel::NOTIFY_SET_PITCH:
	{
		float factor = (float)pow(3.0, data / 100.0);
		sampler->setPitchFactor(factor);
		break;
	}
	case ConfigModel::NOTIFY_SET_SPEED:
	{
		float factor = (float)pow(3.0, data / 100.0);
		sampler->setSpeedFactor(factor);
		break;
	}
	case ConfigModel::NOTIFY_SET_REVERB:
	{
		// Slider 0..100 → mix 0.0..1.0
		float mix = (float)data / 100.0f;
		sampler->setReverbMix(mix);
		break;
	}
	case ConfigModel::NOTIFY_SET_MULTI_SOUNDBOARD:
		sampler->setMultiMode(data != 0);
		break;
	default:
		break;
	}
}


CAPI void sb_handlePlaybackData(uint64 serverConnectionHandlerID, short* samples, int sampleCount,
	int channels, const unsigned int *channelSpeakerArray, unsigned int *channelFillMask)
{
	if (serverConnectionHandlerID != activeServerId)
		return; //Ignore other servers

	sampler->fetchOutputSamples(samples, sampleCount, channels, channelSpeakerArray, channelFillMask);
}


CAPI void sb_handleCaptureData(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited)
{
	if (serverConnectionHandlerID != activeServerId)
		return; //Ignore other servers

	if (g_rpsbPreviewOnly)
	{
		// Preview-only: server must hear only the real mic. Earlier we
		// just early-returned, but that left the sampler's per-slot
		// state machine starved (it advances inside fetchInputSamples)
		// so playing slots never transitioned to eSILENT after their
		// file ended, and the internal sbCapture ring filled up. Side
		// effect: TS3 picked up phantom voice activity from soundboard
		// audio that never got consumed and looped on the next pass.
		//
		// Fix: still call fetchInputSamples so the state machine ticks
		// and the ring drains, but route the mix into a discard buffer
		// instead of the real `samples`. The mic buffer the client
		// transmits stays exactly as captured, and we never set *edited
		// so TS3 knows we did not modify the stream.
		static thread_local std::vector<short> previewScratch;
		const size_t needed = static_cast<size_t>(sampleCount) * channels;
		if (previewScratch.size() < needed) previewScratch.assign(needed, 0);
		else std::fill_n(previewScratch.begin(), needed, static_cast<short>(0));
		sampler->fetchInputSamples(previewScratch.data(), sampleCount, channels, NULL);
		return;
	}

	int written = sampler->fetchInputSamples(samples, sampleCount, channels, NULL);
	if(written > 0)
		*edited |= 0x1;
}


int sb_playFile(const SoundInfo &sound)
{
	if (activeServerId == 0)
		return 2;
	return sampler->playFile(sound) ? 0 : 1;
}


Sampler *sb_getSampler()
{
	return sampler;
}

TalkStateManager *sb_getTalkStateManager()
{
	return tsMgr;
}


void sb_enableInterface(bool enabled)
{
	// New modular UI: drive its full-surface overlay + stop any running
	// playback when the user disconnects / gets kicked / loses connection.
	if (mainPage)
	{
		mainPage->setConnected(enabled);
		if (!enabled && sampler)
			sampler->stopPlayback(-1);
	}

	// Legacy bubble path - only relevant if the legacy ConfigQt dialog is
	// the visible window (RPSB_USE_LEGACY_UI=1).
	if (configDialog)
	{
		if (!enabled)
		{
			if (!notConnectedBubble)
			{
				notConnectedBubble = new SpeechBubble(configDialog);
				notConnectedBubble->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
				notConnectedBubble->setFixedSize(350, 80);
				notConnectedBubble->setBackgroundColor(QColor(255, 255, 255));
				notConnectedBubble->setBubbleStyle(false);
				notConnectedBubble->setClosable(false);
				notConnectedBubble->setText("You are not connected to a server.\n"
					"GameBaiters - Soundboard is disabled until you are connected properly.");
				notConnectedBubble->attachTo(configDialog);
				if (configDialog->isVisible())
					notConnectedBubble->show();
			}
		}
		else if (notConnectedBubble)
		{
			delete notConnectedBubble;
			notConnectedBubble = NULL;
		}
		configDialog->setEnabled(enabled);
	}
}

CAPI void sb_init()
{
#ifdef _DEBUG
	QMessageBox::information(NULL, "", "rp soundboard plugin init, attach debugger now");
#endif

	InitFFmpegLibrary();

	QTimer::singleShot(10, []{
		configModel = new ConfigModel();
		configModel->readConfig();
		// Persistent state: hotkey block list survives TS3 restarts so
		// a Reset Hotkeys click is permanent until the user re-arms.
		HotkeyBlock::load();

		/* This if first QObject instantiated, it will load the resources */
		sampler = new Sampler();
		sampler->init();

		tsMgr = new TalkStateManager();
		tsMgr->setSampler(sampler);
		QObject::connect(sampler, &Sampler::onStartPlaying, tsMgr, &TalkStateManager::onStartPlaying, Qt::QueuedConnection);
		QObject::connect(sampler, &Sampler::onStopPlaying, tsMgr, &TalkStateManager::onStopPlaying, Qt::QueuedConnection);
		QObject::connect(sampler, &Sampler::onPausePlaying, tsMgr, &TalkStateManager::onPauseSound, Qt::QueuedConnection);
		QObject::connect(sampler, &Sampler::onUnpausePlaying, tsMgr, &TalkStateManager::onUnpauseSound, Qt::QueuedConnection);

		configDialog = new ConfigQt(configModel);
		// Legacy ConfigQt is kept around for its static helpers + as a
		// fallback UI. By default it's not the visible window. Detach it
		// from the model so the hidden grid does NOT rebuild on every
		// notification - that was the source of the rows/cols-spam lag.
		// (Re-attached only if the user opts into the legacy UI via
		// RPSB_USE_LEGACY_UI=1, see sb_openDialog.)
		const char *legacyEnv = std::getenv("RPSB_USE_LEGACY_UI");
		if (!(legacyEnv && legacyEnv[0] == '1')) {
			configDialog->detachFromModel();
		}
		// MainPage is constructed lazily in sb_openDialog.

		modelObserver = new ModelObserver_Prog();
		configModel->addObserver(modelObserver);

		configModel->notifyAllEvents();

		updateChecker = new UpdateChecker();
		updateChecker->startCheck(false, configModel);
	});
}


CAPI void sb_saveConfig()
{
	configModel->writeConfig();
}


CAPI void sb_kill()
{
	// Stop the singleton visualizer thread FIRST. It's a std::thread that holds
	// references to DLL code; if it survives DLL unload, FreeLibrary fails and
	// TS3's plugin uninstall leaves the file locked.
	SampleVisualizerThread::GetInstance().stop(true);

	if (configModel)
	{
		configModel->remObserver(modelObserver);
	}
	delete modelObserver;
	modelObserver = NULL;

	// Disconnect all signals from sampler before shutdown to prevent
	// callbacks firing into deleted objects during slot cleanup.
	if (sampler)
		QObject::disconnect(sampler, nullptr, nullptr, nullptr);

	if (sampler)
	{
		sampler->shutdown();
		delete sampler;
		sampler = NULL;
	}

	delete tsMgr;
	tsMgr = NULL;

	if (mainPage)
	{
		mainPage->hide();
		mainPage->setParent(nullptr);
		delete mainPage;
		mainPage = NULL;
	}

	if (configDialog)
	{
		configDialog->hide();
		configDialog->setParent(nullptr);
		delete configDialog;
		configDialog = NULL;
	}

	if (configModel)
	{
		configModel->writeConfig();
		delete configModel;
		configModel = NULL;
	}

	if(aboutDialog)
	{
		aboutDialog->hide();
		aboutDialog->setParent(nullptr);
		delete aboutDialog;
		aboutDialog = NULL;
	}

	if (updateChecker)
	{
		delete updateChecker;
		updateChecker = NULL;
	}

	// Drain pending deferred deletes scheduled by Qt during the teardown
	// (QNetworkReply, QTimer one-shots, etc.). Without this, slots may run
	// into already-unloaded DLL code when TS3 calls FreeLibrary.
	if (QCoreApplication::instance())
	{
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
		QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
		QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	}

	// Close debug log file handle so the .log file isn't held open after unload.
	rpsb_close_debug_log();
}


CAPI void sb_onServerChange(uint64 serverID)
{
	if (connectionStatusMap.find(serverID) == connectionStatusMap.end())
		connectionStatusMap[serverID] = STATUS_DISCONNECTED;
	bool connected = connectionStatusMap[serverID] == STATUS_CONNECTION_ESTABLISHED;

	tsMgr->setActiveServerId(serverID);
	activeServerId = serverID;
	logInfo("Server Id: %ull", (unsigned long long)serverID);
	sb_enableInterface(connected);
}


CAPI void sb_openDialog()
{
	// Default visible window is MainPage. Set RPSB_USE_LEGACY_UI=1 to
	// fall back to the legacy ConfigQt window.
	bool useLegacy = false;
	{
		const char *env = std::getenv("RPSB_USE_LEGACY_UI");
		useLegacy = (env && env[0] == '1');
	}

	if (useLegacy) {
		if (!configDialog)
			configDialog = new ConfigQt(configModel);
		configDialog->showNormal();
		configDialog->raise();
		configDialog->activateWindow();
	} else {
		if (!mainPage) {
			Theme::apply(qApp);
			mainPage = new MainPage();
			MainPageWiring::wire(mainPage, configModel, sampler);
		}
		mainPage->showNormal();
		mainPage->raise();
		mainPage->activateWindow();
	}

	sb_enableInterface(connectionStatusMap[activeServerId]);
}


CAPI void sb_stopPlayback()
{
	sampler->stopPlayback();
}


CAPI void sb_pauseSound()
{
	sampler->pausePlayback();
}


CAPI void sb_unpauseSound()
{
	sampler->unpausePlayback();
}


CAPI void sb_pauseButtonPressed()
{
	// Check if any slot is playing or paused
	if (sampler->findSlotByState(Sampler::ePLAYING) >= 0)
		sb_pauseSound();
	else if (sampler->findSlotByState(Sampler::ePAUSED) >= 0)
		sb_unpauseSound();
}

/** play button by name or index(strtol), return 0 on success */
CAPI int sb_playButtonEx(const char* button)
{
	long arg1 = strtol(button, NULL, 10);

	if ((NULL != configDialog) && (configDialog->hotkeysEnabled()))
	{
		if (arg1 <= 0)
		{
			// name-based lookup not implemented
		}
		else
		{
			const SoundInfo *sound = configModel->getSoundInfo(arg1);
			if (sound)
				sb_playFile(*sound);
			else
				return 1;
		}
	}
	return 0;
}

CAPI void sb_playButton(int btn)
{
    // Hotkey blocked at the Reset-hotkey level: short-circuit even if
    // TS3 still holds the binding in its profile.
    if (HotkeyBlock::isBlocked(btn)) return;

    if ((NULL != configDialog) && !configDialog->hotkeysEnabled()) return;

    // Route through the new modular UI's pipeline so hotkey playback uses
    // the same round-robin slot picker, channel-FX integration, error
    // dialog and preview-stop as a real mouse click. Without this routing
    // hotkeys spawn extra slots via Sampler::playFile -> findFreeSlot,
    // which is what made auto-repeat sound like overlapping playbacks.
    if (mainPage)
    {
        QMetaObject::invokeMethod(mainPage, [btn]{
            if (mainPage) mainPage->triggerButton(btn);
        }, Qt::QueuedConnection);
        return;
    }

    // Legacy fallback path (RPSB_USE_LEGACY_UI=1).
    const SoundInfo *sound = configModel->getSoundInfo(btn);
    if (sound)
    {
        if (sound->fxRemember)
        {
            configModel->setPitchValue(sound->fxPitch);
            configModel->setSpeedValue(sound->fxSpeed);
            configModel->setReverbValue(sound->fxReverb);
            configModel->setSyncPitchSpeed(sound->fxSyncPitchSpeed);
        }
        sb_playFile(*sound);
    }
}

CAPI void sb_setConfig(int cfg)
{
    if (configDialog)
        configDialog->setConfiguration(cfg);
}

CAPI void sb_openAbout()
{
	if(!aboutDialog)
		aboutDialog = new AboutQt();
	aboutDialog->show();
}


CAPI void sb_onConnectStatusChange(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) 
{
    Q_UNUSED(errorNumber)

    if(newStatus == STATUS_DISCONNECTED)
		connectionStatusMap.erase(serverConnectionHandlerID);
	else
		connectionStatusMap[serverConnectionHandlerID] = newStatus;

	if (serverConnectionHandlerID == activeServerId)
	{
		if (newStatus == STATUS_DISCONNECTED)
			sb_stopPlayback();
		sb_enableInterface(newStatus == STATUS_CONNECTION_ESTABLISHED);
	}
}


CAPI void sb_getInternalHotkeyName(int buttonId, char *buf)
{
	sprintf(buf, "button_%i", buttonId + 1);
}


CAPI void sb_getInternalConfigHotkeyName(int configId, char *buf)
{
	sprintf(buf, "config_%i", configId);
}


CAPI void sb_onHotkeyRecordedEvent(const char *keyword, const char *key)
{
	if (configDialog)
		configDialog->onHotkeyRecordedEvent(keyword, key);

	// Mirror the binding into the new UI's overlay cache so "show
	// hotkeys on buttons" reflects the hotkey the moment TS3 confirms
	// it, without having to reopen the soundboard window. Recording a
	// fresh hotkey also lifts any previous block on that button.
	if (mainPage && keyword)
	{
		QString kw = QString::fromUtf8(keyword);
		if (kw.startsWith("button_"))
		{
			bool ok = false;
			int btnNum = kw.midRef(7).toInt(&ok);
			if (ok)
			{
				int idx = btnNum - 1; // sb_getInternalHotkeyName uses i+1
				HotkeyBlock::setBlocked(idx, false);
				QString k = key ? QString::fromUtf8(key) : QString();
				QMetaObject::invokeMethod(mainPage->buttonGrid(),
					[idx, k]{
						if (mainPage)
							mainPage->buttonGrid()->setHotkeyOverlay(idx, k);
					}, Qt::QueuedConnection);
			}
		}
	}
}


CAPI void sb_onStopTalking()
{
	tsMgr->onClientStopsTalking();
}

CAPI void sb_onHotkeyPressed(const char * keyword)
{
	if (hotkeysTemporarilyDisabled)
		return;

	int btn = -1;
	if (sscanf(keyword, "button_%i", &btn) > 0)
	{
		sb_playButton(btn - 1);
	}
	else if (sscanf(keyword, "config_%i", &btn) > 0)
	{
		sb_setConfig(btn);
	}
	else if (strcmp(keyword, HOTKEY_STOP_ALL) == 0)
	{
		sb_stopPlayback();
	}
	else if (strcmp(keyword, HOTKEY_PAUSE_ALL) == 0)
	{
		sb_pauseButtonPressed();
	}
	else if (strcmp(keyword, HOTKEY_MUTE_MYSELF) == 0)
	{
		configModel->setMuteMyselfDuringPb(!configModel->getMuteMyselfDuringPb());
	}
	else if (strcmp(keyword, HOTKEY_MUTE_ON_MY_CLIENT) == 0)
	{
		configModel->setPlaybackLocal(!configModel->getPlaybackLocal());
	}
	else if (strcmp(keyword, HOTKEY_VOLUME_INCREASE) == 0)
	{
		configModel->setVolumeRemote(std::min(configModel->getVolumeRemote() + 20, 100));
		configModel->setVolumeLocal(std::min(configModel->getVolumeLocal() + 20, 100));
	}
	else if (strcmp(keyword, HOTKEY_VOLUME_DECREASE) == 0)
	{
		configModel->setVolumeRemote(std::max(configModel->getVolumeRemote() - 20, 0));
		configModel->setVolumeLocal(std::max(configModel->getVolumeLocal() - 20, 0));
	}
}


CAPI void sb_checkForUpdates()
{
	if (!updateChecker)
		updateChecker = new UpdateChecker();
	updateChecker->startCheck(true);
}

/** return 0 if the command was handled, 1 otherwise */
CAPI int sb_parseCommand(char** args, int argc)
{
	if (argc >= 3)
		ts3Functions.printMessageToCurrentTab("Too many arguments");
	else if (argc == 0)
		sb_openDialog();
	else if (argc == 1)
	{
		long arg1 = strtol(args[0], NULL, 10);
		if (strcmp(args[0], "stop")==0)
			sb_stopPlayback();
		else if (strcmp(args[0], "-?") == 0)
			ts3Functions.printMessageToCurrentTab("Arguments: 'stop' to stop playback or '[configuration number] <button number>'");
		else if (sb_playButtonEx(args[0]) != 0)
			ts3Functions.printMessageToCurrentTab("No such button found");
	}
	else if (argc == 2)
	{
		long arg0 = strtol(args[0], NULL, 10);
		int pconfig = configModel->getConfiguration();
		if (arg0 < 1 || arg0 > 4)
			ts3Functions.printMessageToCurrentTab("Invalid configuration number");
		configModel->setConfiguration((int)arg0); //switch to specified configuration
		if (sb_playButtonEx(args[0]) != 0)
			ts3Functions.printMessageToCurrentTab("No such button found");
		configModel->setConfiguration(pconfig); //return to previous configuration

	}
	return 0;
}


CAPI void sb_disableHotkeysTemporarily(bool disable)
{
	hotkeysTemporarilyDisabled = disable;
}
