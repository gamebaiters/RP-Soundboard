// src/main.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "common.h"
#include "AudioUtils.h"

#include <cstdio>
#include <cmath>
#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <fstream>
#include <vector>
#include <cstdarg>
#include <map>
#include <atomic>
#include <chrono>
#include <thread>

#include <QObject>
#include <QMessageBox>
#include <QString>
#include <QCoreApplication>
#include <QEventLoop>
#include <QEvent>
#include <QThread>
#include <QTranslator>
#include <QLocale>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "main.h"
#include "plugin.h"
#include "ts3log.h"
#include "inputfile.h"
#include "samples.h"
#include "SampleVisualizerThread.h"
#include "config_qt.h"
#include "about_qt.h"
#include "howto_qt.h"
#include "ConfigModel.h"
#include "UpdateChecker.h"
#include "SoundInfo.h"
#include "TalkStateManager.h"
#include "SpeechBubble.h"
#include "MicFx.h"
#include "PlatformStyle.h"
#include "TsToolbarButton.h"
#include "modules/main_page.h"
#include "modules/main_page_wiring.h"
#include "modules/theme.h"
#include "modules/button_grid.h"
#include "modules/hotkey_block.h"
#include "modules/whats_new_dialog.h"
#include "modules/log_viewer_dialog.h"
#include "modules/audio_exporter.h"
#include "modules/stream_resolver.h"
#include <QApplication>

extern "C" void rpsb_close_debug_log();

// File-scope storage for the lazily-created log viewer instance.
// sb_kill (further up the TU) references it to delete the top-level
// QWidget before TS3 unloads the plugin DLL; without that step the
// freed-DLL vtable pattern crashes the client on close.
LogViewerDialog *logViewerDialog = nullptr;

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
HowToDialog *howToDialog = NULL;
// Atomic pointer so the audio thread's null-check + dereference races
// safely against sb_kill nulling the pointer. The old plain pointer
// allowed a window where TS3's audio thread had passed the null guard
// in sb_handlePlaybackData / sb_handleCaptureData but had not yet
// dereferenced — and sb_kill was free to delete the object in that
// window, crashing the audio thread (TS3 then popped its own
// "soundboard plugin crashed" dialog on exit and left a zombie TS3.exe
// in task manager). sb_kill now: store null + fence + brief sleep so
// any in-flight callback drains, THEN shutdown + delete.
std::atomic<Sampler*> g_samplerAtomic{nullptr};
Sampler *sampler = NULL;
TalkStateManager *tsMgr = NULL;

// --- Voice behaviour while a sound plays (v2.3.1) ------------------------
// (1) vadWhilePlaying: gate the USER'S mic by voice activity so his voice is
//     only sent when he actually talks, while the soundboard is transmitted
//     continuously on top. (2) duckWhenTalking: lower the soundboard by
//     g_duckAmount (0..1) whenever the user talks, so his voice stays audible.
static std::atomic<bool>  g_vadWhilePlaying{false};
static std::atomic<bool>  g_duckWhenTalking{false};
static std::atomic<float> g_duckAmount{0.4f};

void sb_setVoiceBehaviour(bool vadWhilePlaying, bool duckWhenTalking, float duckAmount)
{
	g_vadWhilePlaying.store(vadWhilePlaying);
	g_duckWhenTalking.store(duckWhenTalking);
	if (duckAmount < 0.0f) duckAmount = 0.0f;
	if (duckAmount > 0.95f) duckAmount = 0.95f;
	g_duckAmount.store(duckAmount);
	// When ducking is turned off, release the soundboard to unity (both the
	// target and the current smoothed gain) so it never stays dipped.
	if (!duckWhenTalking && sampler) {
		sampler->setDuckTarget(1.0f);
		sampler->setMasterDuckGain(1.0f);
	}
}

bool hotkeysTemporarilyDisabled = false;

ModelObserver_Prog *modelObserver = NULL;
UpdateChecker *updateChecker = NULL;
std::map<uint64, int> connectionStatusMap;
typedef std::lock_guard<std::mutex> Lock;

// Owns every DEFERRED (QTimer::singleShot / queued) callback the plugin posts
// to the shared qApp event loop. TS3 hosts the plugin inside its own Qt app, so
// a context-less singleShot creates a Qt5Core-owned timer that holds a functor
// compiled into THIS DLL. On quit TS3 can call ts3plugin_shutdown + FreeLibrary
// before that timer fires; Qt then dispatches it into freed DLL code and the
// client crashes on exit (Qt5Core event-dispatch frame, no plugin frame on the
// stack — the vtable/functor is already gone). Routing every deferred call
// through this plugin-owned context makes Qt CANCEL the pending call the moment
// the context is destroyed. sb_kill deletes it first thing, so no plugin
// callback can survive the DLL. NEVER post a context-less singleShot with a
// plugin lambda — always pass g_deferCtx (or another plugin QObject) as context.
static QObject *g_deferCtx = NULL;
// Set at the very top of sb_kill so late TS3 callbacks (a disconnect racing the
// unload) stop posting NEW deferred work into the dying event loop.
static std::atomic<bool> g_pluginShuttingDown{false};


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
		float factor = AudioUtils::sliderToPitchFactor(data);
		sampler->setPitchFactor(factor);
		sampler->setSpeedFactor(factor);
		break;
	}
	case ConfigModel::NOTIFY_SET_PITCH:
	{
		float factor = AudioUtils::sliderToPitchFactor(data);
		sampler->setPitchFactor(factor);
		break;
	}
	case ConfigModel::NOTIFY_SET_SPEED:
	{
		float factor = AudioUtils::sliderToPitchFactor(data);
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

	// Atomic snapshot. After sb_kill stores nullptr the next call
	// returns immediately; in-flight calls that already passed the
	// null check still hold the previously-loaded pointer — sb_kill
	// then sleeps briefly before delete so those drain.
	Sampler *s = g_samplerAtomic.load(std::memory_order_acquire);
	if (!s) return;

	s->fetchOutputSamples(samples, sampleCount, channels, channelSpeakerArray, channelFillMask);

	// Mic FX monitor ("hear my own processed voice"): mix of the
	// processed capture stream into the local playback buffer. Resolve
	// the L/R channel indices from the speaker array and honour the
	// fill-mask semantics (unfilled channels must be overwritten).
	{
		MicFx &mic = MicFx::instance();
		if (mic.enabled() && mic.monitor())
		{
			const unsigned int bmL = SPEAKER_FRONT_LEFT | SPEAKER_HEADPHONES_LEFT;
			const unsigned int bmR = SPEAKER_FRONT_RIGHT | SPEAKER_HEADPHONES_RIGHT;
			int ciLeft = 0, ciRight = (channels >= 2) ? 1 : 0;
			if (channelSpeakerArray)
			{
				for (int i = 0; i < channels; ++i)
					if (channelSpeakerArray[i] & bmL) { ciLeft = i; break; }
				for (int i = 0; i < channels; ++i)
					if (channelSpeakerArray[i] & bmR) { ciRight = i; break; }
			}
			bool overL = (*channelFillMask & bmL) == 0;
			bool overR = (*channelFillMask & bmR) == 0;
			if (mic.mixMonitor(samples, sampleCount, channels,
			                   ciLeft, ciRight, overL, overR))
				*channelFillMask |= (bmL | bmR);
		}
	}
}


CAPI void sb_handleCaptureData(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited)
{
	if (serverConnectionHandlerID != activeServerId)
		return; //Ignore other servers

	// Atomic snapshot — see sb_handlePlaybackData.
	Sampler *s = g_samplerAtomic.load(std::memory_order_acquire);
	if (!s) return;

	// Mic FX (V1): process the user's OWN voice BEFORE the soundboard
	// mix-in, so listeners hear voice-through-effects + clean soundboard
	// audio on top. try-lock design inside: never blocks this thread.
	if (MicFx::instance().processCapture(samples, sampleCount, channels))
		*edited |= 0x1;

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
		s->fetchInputSamples(previewScratch.data(), sampleCount, channels, NULL);
		return;
	}

	// Voice behaviour: mic VAD gating + soundboard ducking. `samples` here is
	// the user's mic (post Mic-FX) BEFORE the soundboard is mixed in below, so
	// we can measure the mic level and gate/duck accordingly.
	{
		const bool vadOpt  = g_vadWhilePlaying.load(std::memory_order_relaxed);
		const bool duckOpt = g_duckWhenTalking.load(std::memory_order_relaxed);
		if ((vadOpt || duckOpt) && s->anyPlaying() && sampleCount > 0)
		{
			// Block RMS of the first channel (mic is mono into TS3 anyway).
			double sumsq = 0.0;
			for (int i = 0; i < sampleCount; ++i) {
				const double v = samples[i * channels];
				sumsq += v * v;
			}
			const double rms = std::sqrt(sumsq / (double)sampleCount);

			// Envelope + hold so the gate/duck doesn't chatter on word gaps.
			static double s_env = 0.0;
			static int    s_hold = 0;
			s_env = (rms > s_env) ? rms : (s_env * 0.90 + rms * 0.10);
			const double kTalkThresh = 500.0;   // ~ -36 dBFS on int16
			const int    kHoldBlocks = 12;       // ~240 ms at 20 ms/block
			bool talking = s_env > kTalkThresh;
			if (talking) s_hold = kHoldBlocks;
			else if (s_hold > 0) { --s_hold; talking = true; }

			if (duckOpt) {
				// Only set the TARGET — fetchSamples ramps the actual gain
				// toward it per-sample, so the dip/recovery is smooth (no step).
				const float target = talking ? (1.0f - g_duckAmount.load(std::memory_order_relaxed)) : 1.0f;
				s->setDuckTarget(target);
			}

			if (vadOpt && !talking) {
				// User is silent: drop the mic so ONLY the soundboard is sent
				// (soundboard is mixed in by fetchInputSamples right after).
				std::fill_n(samples, (size_t)sampleCount * channels, (short)0);
				*edited |= 0x1;
			}
		}
	}

	int written = s->fetchInputSamples(samples, sampleCount, channels, NULL);
	if(written > 0)
		*edited |= 0x1;
}


int sb_playFile(const SoundInfo &sound)
{
	if (activeServerId == 0)
		return 2;
	// Persist any pending model changes (volume, pitch, theme...)
	// before starting playback. The save model is event-triggered now:
	// every "user does something concrete" hook flushes the dirty
	// model, so users don't have to wait for an automatic timer.
	ConfigModel::flushPendingWrite();
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

// UI translation. Installed on qApp at plugin init - QTranslator
// lookups are context-scoped, so it only ever supplies strings for the
// soundboard's own classes and never re-translates the TS3 host UI.
static QTranslator *uiTranslator = NULL;

static void sb_installTranslation()
{
	QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
	QString lang = s.value(QStringLiteral("language"), QStringLiteral("auto")).toString();
	if (lang == QLatin1String("auto"))
		lang = (QLocale::system().language() == QLocale::Italian)
		           ? QStringLiteral("it") : QStringLiteral("en");
	if (lang == QLatin1String("en"))
		return;   // source strings are already English

	QTranslator *t = new QTranslator();
	if (t->load(QStringLiteral(":/i18n/soundboard_") + lang + QStringLiteral(".qm"))) {
		qApp->installTranslator(t);
		uiTranslator = t;
	} else {
		delete t;
	}
}

CAPI void sb_init()
{
#ifdef _DEBUG
	QMessageBox::information(NULL, "", "rp soundboard plugin init, attach debugger now");
#endif

	InitFFmpegLibrary();

	// Disarm the global network-I/O abort: TS3 can disable + re-enable the
	// plugin in the same process, and sb_kill leaves the flag armed. Without
	// this, every network open/read after a plugin re-enable would abort
	// instantly and streaming would be silently dead until a client restart.
	InputFileNet::setShutdownAbort(false);

	// Re-enable deferred callbacks (a prior disable + re-enable in the same
	// process left this set) and create the context that owns every deferred
	// plugin callback — must exist BEFORE the bootstrap singleShot below uses
	// it. Parentless + qApp-thread: destroyed explicitly in sb_kill.
	g_pluginShuttingDown.store(false, std::memory_order_release);
	if (!g_deferCtx) g_deferCtx = new QObject();

	// Wipe any leftover stream scratch files from a previous session BEFORE
	// anything runs — a 10-hour video must never accumulate on disk.
	StreamResolver::cleanTempDir();

	// Context-bound: if the plugin is disabled during these 10 ms, sb_kill
	// deletes g_deferCtx and Qt cancels this bootstrap instead of half-
	// initialising into a DLL that is about to unload.
	QTimer::singleShot(10, g_deferCtx, []{
		configModel = new ConfigModel();
		configModel->readConfig();
		// Install the UI translation before any soundboard window is
		// built - tr() resolves at widget-construction time.
		sb_installTranslation();
		// Persistent state: hotkey block list survives TS3 restarts so
		// a Reset Hotkeys click is permanent until the user re-arms.
		HotkeyBlock::load();

		/* This if first QObject instantiated, it will load the resources */
		sampler = new Sampler();
		sampler->init();
		// Publish AFTER init so the audio thread can never see a half-
		// constructed Sampler. Release fence pairs with the audio
		// thread's acquire load in sb_handlePlaybackData /
		// sb_handleCaptureData.
		g_samplerAtomic.store(sampler, std::memory_order_release);

		// Mic FX: restore persisted state (chain, pitch, monitor and -
		// per user decision - the master toggle itself). Must run after
		// the Sampler publish so a restored "enabled" flows into a fully
		// live audio path. Feature gate FIRST so a disabled feature
		// blocks the restored master toggle.
		MicFx::instance().setFeatureEnabled(configModel->getMicFxFeatureEnabled());
		MicFx::instance().loadSettings();
		// Global loudness normalization (Q2) applies from the first play.
		sampler->setGlobalNormalize(configModel->getLoudnessNormalize());

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

		// Pre-warm the streaming engine (background, off the GUI thread) so the
		// OS caches the binary and the FIRST real link resolve doesn't pay the
		// cold self-extraction / disk-read start cost, and run the silent engine
		// self-update — but at most once a day, and never while a link the user
		// pasted is waiting on it (warmUp owns both rules; it used to update on
		// every single startup, which on macOS could leave `-U` rewriting the
		// engine binary at the exact moment a resolve tried to spawn it).
		StreamResolver::instance().warmUp();

		// Native host-toolbar button (unofficial, defensive): a checkable
		// soundboard toggle next to the client's own mute/away buttons.
		// No-ops silently if the client toolbar cannot be found.
		// Settings-gated ("Show soundboard button in the TeamSpeak
		// toolbar").
		if (configModel->getTsToolbarButton())
			TsToolbarButton::install();
	});
}


CAPI void sb_saveConfig()
{
	// Public save entry: bypass the debouncer so the caller gets an
	// on-disk file before this returns.
	configModel->writeConfigImmediate();
}


CAPI void sb_kill()
{
	// Mark teardown so any late TS3 callback (a disconnect racing the unload)
	// stops posting NEW deferred work into the event loop we are draining.
	g_pluginShuttingDown.store(true, std::memory_order_release);

	// Pull our button OUT of the host toolbar first: it is a plugin-owned
	// widget parented into the client's UI - it must not survive the DLL.
	TsToolbarButton::remove();

	// Cancel EVERY pending deferred plugin callback FIRST. Destroying the
	// context QObject makes Qt drop the single-shot timers bound to it (the
	// disconnect flush, the init bootstrap, …) so none of them can fire into
	// this DLL after it unloads — the definitive fix for the "TeamSpeak
	// crashed on close" dialog (a Qt5Core-owned timer holding a functor
	// compiled into the plugin, dispatched after FreeLibrary). The state those
	// callbacks would have written is still saved by the flush just below.
	if (g_deferCtx)
	{
		delete g_deferCtx;
		g_deferCtx = NULL;
	}

	// Arm the global FFmpeg network-I/O abort FIRST: any producer / seek /
	// export worker blocked inside a network open/read (rw_timeout is 15 s)
	// returns within milliseconds, so every bounded thread join below
	// actually succeeds instead of escalating to TerminateThread — which
	// could kill a worker mid-heap-alloc / mid-SSL-handshake and produce
	// the intermittent "TeamSpeak crashed" dialog on a normal close.
	InputFileNet::setShutdownAbort(true);

	// Flush any debounced config write so the last slider position the
	// user set in the seconds before quit is persisted. writeConfig()
	// schedules on a 250 ms timer; without this flush, fast-close TS3
	// drops anything still pending in that window.
	ConfigModel::flushPendingWrite();

	// Cancel + join every still-running AudioExporter. Exporters live
	// without a Qt parent (so the export survives a closed soundboard
	// window), which means sb_kill had no other way to reach them.
	// A live QThread at DLL unload kept TS3.exe in task manager as a
	// zombie - the user-reported "soundboard process stays open and
	// TS3 pops a crash dialog when I force-kill it" bug.
	AudioExporter::cancelAllAndWait(500);

	// Kill any running yt-dlp child (a live resolve or an in-flight self-update)
	// BEFORE Qt/plugin teardown so it can never become a ghost process or hang
	// the unload. Resolves are quick (yt-dlp only prints the URL, it never
	// downloads the video), but a slow network / stuck update must not linger.
	StreamResolver::instance().shutdown();

	// Tear down the TalkStateManager active server FIRST so the watchdog
	// timer stops and any in-flight queued setTalkTransMode calls skip
	// their ts3Functions invocations. By the time TS3 calls
	// ts3plugin_shutdown its audio backend (directsound_win64.dll /
	// WASAPI) is already in tear-down and any further setClientSelfVariable
	// / flushClientSelfUpdates from us hits freed pointers in that DLL.
	if (tsMgr) tsMgr->onConnectionLost();

	// Visualizer threads are now per-SoundView instances; each widget's
	// destructor stops + joins its own thread when the windows are torn
	// down below, so no global visualizer stop is needed here.

	if (uiTranslator)
	{
		qApp->removeTranslator(uiTranslator);
		delete uiTranslator;
		uiTranslator = NULL;
	}

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
		// STEP 1: null the atomic pointer the audio thread reads from
		// — any subsequent sb_handlePlaybackData / sb_handleCaptureData
		// call returns immediately at the null check.
		g_samplerAtomic.store(nullptr, std::memory_order_release);
		// STEP 2: brief drain window so any in-flight audio callback
		// that already passed the null check completes its current
		// fetchOutputSamples / fetchInputSamples and returns BEFORE
		// we touch the sampler. TS3 ticks audio at 20 ms intervals
		// (960 frames @ 48 kHz); 50 ms is two full ticks, well past
		// the worst case. Without this drain the audio thread could
		// be mid-deref while sampler->shutdown() ran below and the
		// teardown joined producer threads from under its feet —
		// crash on close + zombie TS3.exe in task manager.
		//
		// PUMP while draining instead of a dead sleep: sb_kill runs on
		// the client's GUI/STA thread, and TS3's DirectSound worker
		// makes COM calls that marshal through this thread during audio
		// teardown. A blocked STA here starved those calls and produced
		// the directsound_win64.dll+0xD527 NULL-deref half of the
		// crash-on-close dumps (same mechanism as the ghost-user bug,
		// this time inside our own shutdown). processEvents runs the
		// Windows message pump, so the COM proxies keep completing.
		{
			QElapsedTimer drain;
			drain.start();
			while (drain.elapsed() < 50) {
				QCoreApplication::processEvents(
					QEventLoop::ExcludeUserInputEvents, 10);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}
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
		// flushPendingWrite() at the top of sb_kill already drained the
		// debouncer; bypass it here as a belt-and-braces final save so
		// the on-disk state matches the in-memory model before deletion.
		configModel->writeConfigImmediate();
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

	// Must be destroyed before TS3 unloads the plugin DLL: a leaked
	// top-level QWidget keeps a vtable into freed DLL code and crashes
	// the client on exit.
	if(howToDialog)
	{
		howToDialog->hide();
		howToDialog->setParent(nullptr);
		delete howToDialog;
		howToDialog = NULL;
	}

	// Same rule for the in-app log viewer. Without this, opening the
	// viewer once then quitting TS3 left a top-level QWidget pointing
	// into freed plugin DLL code, causing the intermittent client
	// crash on close the user reported.
	if (logViewerDialog)
	{
		logViewerDialog->hide();
		logViewerDialog->setParent(nullptr);
		delete logViewerDialog;
		logViewerDialog = nullptr;
	}

	if (updateChecker)
	{
		delete updateChecker;
		updateChecker = NULL;
	}

	// ---- Font-cache purge (crash-on-close fix, dump-proven) ----
	// Qt keeps a GLOBAL per-thread QFontCache keyed by QFontDef, whose
	// family QStrings can share data owned by THIS DLL (any font the
	// soundboard widgets requested). The host clears that cache only in
	// ~QGuiApplication - AFTER FreeLibrary - so a surviving entry means
	// a QString destructor reading freed plugin memory: exactly the
	// Qt5Core+0x15D3 / QFontCache::clear signature in the client crash
	// dumps. QFontCache is private API but its symbols are EXPORTED by
	// Qt5Gui; resolve them dynamically and empty the cache now, while
	// our strings can still be destructed safely. Fully defensive: if
	// the symbols are missing, we simply skip.
	{
		typedef void *(*FcInstanceFn)();
		typedef void  (*FcClearFn)(void *);
		FcInstanceFn fcInstance = nullptr;
		FcClearFn    fcClear    = nullptr;
#ifdef _WIN32
		if (HMODULE qtgui = GetModuleHandleW(L"Qt5Gui.dll")) {
			fcInstance = reinterpret_cast<FcInstanceFn>(
				GetProcAddress(qtgui, "?instance@QFontCache@@SAPEAV1@XZ"));
			fcClear = reinterpret_cast<FcClearFn>(
				GetProcAddress(qtgui, "?clear@QFontCache@@QEAAXXZ"));
		}
#else
		fcInstance = reinterpret_cast<FcInstanceFn>(
			dlsym(RTLD_DEFAULT, "_ZN10QFontCache8instanceEv"));
		fcClear = reinterpret_cast<FcClearFn>(
			dlsym(RTLD_DEFAULT, "_ZN10QFontCache5clearEv"));
#endif
		if (fcInstance && fcClear) {
			if (void *fc = fcInstance())
				fcClear(fc);
		}
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
		TsToolbarButton::watchWindow(configDialog);
		configDialog->showNormal();
		configDialog->raise();
		configDialog->activateWindow();
	} else {
		if (!mainPage) {
			Theme::apply(qApp);
			mainPage = new MainPage();
			MainPageWiring::wire(mainPage, configModel, sampler);
		}
		TsToolbarButton::watchWindow(mainPage);
		mainPage->showNormal();
		mainPage->raise();
		mainPage->activateWindow();
	}

	sb_enableInterface(connectionStatusMap[activeServerId]);

	// One-shot upgrade notice. Deferred to dialog-open (instead of
	// sb_init) so the user actually has a parent window to anchor the
	// modal to, and we never pop release-notes over the TS3 main window
	// while the soundboard is still hidden. The static guard inside the
	// helper makes it self-debouncing if openDialog is called repeatedly.
	WhatsNewDialog::showIfUpdated(mainPage ? static_cast<QWidget*>(mainPage)
	                                       : static_cast<QWidget*>(configDialog));
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
	if(!aboutDialog) {
		aboutDialog = new AboutQt();
		PlatformStyle::apply(aboutDialog);
	}
	aboutDialog->show();
	aboutDialog->raise();
	aboutDialog->activateWindow();
}

CAPI void sb_openHowTo()
{
	if(!howToDialog) {
		howToDialog = new HowToDialog();
		PlatformStyle::apply(howToDialog);
	}
	howToDialog->show();
	howToDialog->raise();
	howToDialog->activateWindow();
}

// `logViewerDialog` is declared at file-scope at the top of the TU so
// sb_kill (which lives further up) can null-check + delete it without
// hitting LNK2019. The forward decl provides the storage; this comment
// just marks where the symbol's lifetime actually starts in code.
CAPI void sb_openLogViewer()
{
	if (!logViewerDialog)
		logViewerDialog = new LogViewerDialog();
	logViewerDialog->show();
	logViewerDialog->raise();
	logViewerDialog->activateWindow();
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
		{
			// Invalidate the talk state BEFORE stopping playback. The
			// stop emits queued onStopPlaying signals; without the
			// invalidation those would run setTalkTransMode -> ts3Functions
			// on the just-disconnected handler, racing with TS3's
			// directsound / WASAPI teardown and crashing the client on
			// close.
			if (tsMgr) tsMgr->onConnectionLost();

			// Closing a NETWORK stream can block in avformat teardown
			// (rw_timeout is 15 s) — never inside this TS3 callback.
			// Arm the process-wide abort CB for the duration of the
			// stop so blocked I/O returns in ms, then DISARM so
			// streaming keeps working after a reconnect.
			InputFileNet::setShutdownAbort(true);
			sb_stopPlayback();
			InputFileNet::setShutdownAbort(false);

			// Persist any dirty model state (event-triggered save
			// policy) — but NOT synchronously inside this TS3 callback.
			// A full-ini QSettings write can stall the GUI thread for
			// hundreds of ms; TS3's disconnect teardown meanwhile
			// continues on its own threads, and the DirectSound backend
			// worker sits in a COM call that needs this (blocked) STA
			// thread. That starvation crashed the client on disconnect
			// (minidump 2026-07-15: NULL read in directsound_win64.dll
			// +0xD527, GUI thread inside writeConfigImmediate) — the
			// crash left a zombie session on the server: the "ghost
			// user" with the user's own identity that kept timing out.
			// Deferring one event-loop turn runs the write after TS3's
			// callback returns; a client quit is still covered by the
			// sb_kill top-of-function flush.
			//
			// Context-bound to g_deferCtx: on a QUIT (disconnect immediately
			// followed by ts3plugin_shutdown) sb_kill deletes g_deferCtx, so
			// Qt CANCELS this pending call instead of firing the plugin lambda
			// after the DLL has unloaded — the "TeamSpeak crashed on close"
			// dialog (Qt5Core event-dispatch crash, no directsound involved).
			// State is still saved: sb_kill flushes at its top. If we are
			// already tearing down, don't post at all.
			if (g_deferCtx && !g_pluginShuttingDown.load(std::memory_order_acquire))
				QTimer::singleShot(0, g_deferCtx, []() { ConfigModel::flushPendingWrite(); });
		}
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
	else if (strcmp(keyword, HOTKEY_MICFX_TOGGLE) == 0)
	{
		// Mic FX master toggle (V1). The MicFx singleton is thread-safe
		// and no-ops when the feature is disabled in Settings.
		MicFx::instance().toggle();
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
