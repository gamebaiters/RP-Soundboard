// Cross-module wiring for MainPage. Kept separate from main_page.cpp so
// the layout file stays free of cross-module logic.

#include "main_page_wiring.h"

#include "main_page.h"
#include "search_bar.h"
#include "button_grid.h"
#include "channel.h"
#include "fx_panel.h"
#include "volume_control.h"
#include "waveform_player.h"
#include "reset_channels_btn.h"
#include "settings_window.h"
#include "button_advanced_panel.h"
#include "config_io.h"
#include "channel_state_persistence.h"
#include "audio_exporter.h"
#include "export_progress_dialog.h"

#include "../ConfigModel.h"
#include "../samples.h"
#include "../SoundInfo.h"
#include "../config_qt.h"
#include "hotkey_block.h"
#include "theme.h"

#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QClipboard>
#include <QApplication>
#include <QButtonGroup>
#include <QToolButton>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QToolButton>
#include <QTimer>
#include <QCheckBox>
#include <QHash>
#include <QDateTime>
#include <cmath>

namespace {

// Observer that re-pushes ConfigModel state into MainPage modules whenever
// the model fires a notification. Without this, the grid stays frozen on
// whatever the model held at wire() time (e.g. empty if readConfig hadn't
// finished yet, or stale after a config switch / import).
class MainPageModelObserver : public ConfigModel::Observer {
public:
    MainPageModelObserver(MainPage *page) : m_page(page), m_dimsDirty(false) {}
    void notify(ConfigModel &model, ConfigModel::notifications_e what, int data) override;
private:
    MainPage *m_page;
    bool      m_dimsDirty;       // pending rows/cols rebuild
};

static MainPageModelObserver *s_observer = nullptr;

static QVector<ChannelState> s_preMacroStates;
static int s_preMacroChannelCount = 0;
static bool s_macroActive = false;

void pushSoundsToGrid(MainPage *page, ConfigModel *model);
void pushSettingsToWindow(MainPage *page, ConfigModel *model);

void MainPageModelObserver::notify(ConfigModel &model,
                                   ConfigModel::notifications_e what, int data) {
    switch (what) {
        case ConfigModel::NOTIFY_SET_SOUND:
            // Update only the affected cell - no full grid rebuild.
            if (auto *info = model.getSoundInfo(data))
                m_page->buttonGrid()->setSoundAt(data, *info);
            break;

        case ConfigModel::NOTIFY_SET_ROWS:
        case ConfigModel::NOTIFY_SET_COLS:
            if (!m_dimsDirty) {
                m_dimsDirty = true;
                MainPage *page = m_page;
                ConfigModel *modelPtr = &model;
                bool *dirtyFlag = &m_dimsDirty;
                QTimer::singleShot(150, page, [page, modelPtr, dirtyFlag]{
                    *dirtyFlag = false;
                    page->buttonGrid()->setRowsCols(modelPtr->getRows(), modelPtr->getCols());
                    pushSoundsToGrid(page, modelPtr);
                    // Re-apply search filter after grid rebuild (bug fix #4)
                    QString currentFilter = page->searchBar()->filter();
                    if (!currentFilter.isEmpty())
                        page->buttonGrid()->setSearchFilter(currentFilter);
                });
            }
            break;

        case ConfigModel::NOTIFY_SET_SHOW_HOTKEYS_ON_BUTTONS:
            m_page->buttonGrid()->setShowHotkeys(model.getShowHotkeysOnButtons());
            break;

        default:
            break;
    }
}

void pushSoundsToGrid(MainPage *page, ConfigModel *model) {
    QList<SoundInfo> list;
    list.reserve(model->numSounds());
    for (int i = 0; i < model->numSounds(); ++i) {
        if (auto *s = model->getSoundInfo(i)) list.append(*s);
        else                                   list.append(SoundInfo{});
    }
    page->buttonGrid()->setSounds(list);
    // Repopulate hotkey overlays after every grid rebuild - they would
    // otherwise be wiped (rebuildLayout resets m_overlays).
    for (int i = 0; i < model->numSounds(); ++i) {
        if (HotkeyBlock::isBlocked(i)) {
            page->buttonGrid()->setHotkeyOverlay(i, QString());
        } else {
            page->buttonGrid()->setHotkeyOverlay(i,
                ConfigQt::getShortcutString(static_cast<size_t>(i)));
        }
    }
}

void pushSettingsToWindow(MainPage *page, ConfigModel *model) {
    auto *w = page->settingsWindow();
    w->setRows(model->getRows());
    w->setCols(model->getCols());
    w->setEarrapeProtection(model->getEarrapeProtection());
    w->setLinkVolumes(model->getLinkVolumes());
    w->setRememberPitchSpeed(model->getRememberPitchSpeed());
    w->setRestoreSession(model->getRestoreSession());
    w->setGlobalFxEnabled(model->getGlobalFxEnabled());
    w->setHideWaveform(model->getHideWaveform());
    w->setLogsEnabled(model->getLogsEnabled());
    w->setActiveProfile(model->getConfiguration());
    w->setTheme(model->getThemeEnabled(),
                QColor(model->getThemeAccent()),
                QColor(model->getThemeWaveform()),
                QColor(model->getThemeBackground()),
                model->getThemeContrast(),
                model->getThemeText().isEmpty()   ? QColor() : QColor(model->getThemeText()),
                model->getThemeButton().isEmpty() ? QColor() : QColor(model->getThemeButton()));
    w->setMultiSoundboard(model->getMultiSoundboard());
    w->setShowHotkeysOnButtons(model->getShowHotkeysOnButtons());
    w->setDisableHotkeys(!model->getHotkeysEnabled());
    // Mute checkboxes live on the main page bottom bar (not in Settings).
    QSignalBlocker bml(page->muteLocallyBox());
    QSignalBlocker bmm(page->muteMyselfBox());
    QSignalBlocker bpo(page->previewOnlyBox());
    page->muteLocallyBox()->setChecked(!model->getPlaybackLocal());
    page->muteMyselfBox()->setChecked(model->getMuteMyselfDuringPb());
    page->previewOnlyBox()->setChecked(model->getPreviewOnly());
    page->buttonGrid()->setRowsCols(model->getRows(), model->getCols());
    page->buttonGrid()->setShowHotkeys(model->getShowHotkeysOnButtons());
}

void connectSettings(MainPage *page, ConfigModel *model, Sampler *sampler) {
    auto *w = page->settingsWindow();

    QObject::connect(w, &SettingsWindow::rowsChanged, [model](int v){ model->setRows(v); });
    QObject::connect(w, &SettingsWindow::colsChanged, [model](int v){ model->setCols(v); });
    QObject::connect(w, &SettingsWindow::earrapeProtectionChanged, [model, sampler](bool v){
        model->setEarrapeProtection(v);
        if (sampler) sampler->setEarrapeProtection(v);
    });
    QObject::connect(w, &SettingsWindow::linkVolumesChanged, [model](bool v){ model->setLinkVolumes(v); });
    QObject::connect(w, &SettingsWindow::rememberPitchSpeedChanged, [model](bool v){
        model->setRememberPitchSpeed(v);
        ChannelStatePersistence::setEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::restoreSessionChanged, [model](bool v){
        model->setRestoreSession(v);
        // Restoring the session needs per-channel state on disk - turn it
        // on implicitly, and persist channel count from now on.
        if (v) {
            ChannelStatePersistence::setEnabled(true);
        }
    });
    QObject::connect(w, &SettingsWindow::globalFxEnabledChanged, [model, page](bool v){
        model->setGlobalFxEnabled(v);
        // Mirror the master switch onto every visible channel so the FX
        // panel disappears / reappears live without rebuilding the UI.
        for (auto *ch : page->channels()) ch->setFxVisible(v);
    });
    QObject::connect(w, &SettingsWindow::hideWaveformChanged, [model, page](bool v){
        model->setHideWaveform(v);
        for (auto *ch : page->channels()) ch->setWaveformVisible(!v);
        // Compact pane preset: drop the channel scroll area's fixed
        // height when waveforms are hidden so we don't leave dead space.
        page->updateChannelsAreaHeight(!v);
    });
    QObject::connect(w, &SettingsWindow::logsEnabledChanged, [model](bool v){
        model->setLogsEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::activeProfileChanged, [model, page, sampler](int idx){
        // Flush every channel's sandbox state to persistence BEFORE the
        // profile switch so it survives any UI rebuild the new config
        // triggers, then re-push the saved state to both widget + sampler
        // afterwards. Without this, switching profiles wiped per-channel
        // EQ / spatial / reverb without warning.
        for (int i = 0; i < page->channels().size(); ++i)
            ChannelStatePersistence::saveState(i, page->channels().at(i)->state());
        model->setConfiguration(idx);
        pushSettingsToWindow(page, model);
        pushSoundsToGrid(page, model);
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ChannelState st;
            if (ChannelStatePersistence::loadState(i, st))
                ch->setSandboxState(st.sandbox);
            if (sampler) {
                if (ch->sandboxState().enabled)
                    sampler->setSlotSandboxState(i, ch->sandboxState());
                else
                    sampler->clearSlotSandbox(i);
            }
        }
    });
    QObject::connect(w, &SettingsWindow::exportProfileRequested, [w, model](int idx){
        QString p = QFileDialog::getSaveFileName(w, QObject::tr("Export profile %1").arg(idx + 1),
            QString(), QObject::tr("Soundboard profile (*.ini);;All files (*.*)"));
        if (p.isEmpty()) return;
        if (!ConfigIO::exportProfileIni(p, *model, idx))
            QMessageBox::warning(w, QObject::tr("Export profile"),
                QObject::tr("Failed to write %1").arg(p));
    });
    QObject::connect(w, &SettingsWindow::importProfileRequested, [w, model, page](int idx){
        QString p = QFileDialog::getOpenFileName(w, QObject::tr("Import profile %1").arg(idx + 1),
            QString(), QObject::tr("Soundboard profile (*.ini);;All files (*.*)"));
        if (p.isEmpty()) return;
        auto r = ConfigIO::importProfileIni(p, *model, idx);
        if (r != ConfigIO::ImportResult::Ok) {
            QMessageBox::warning(w, QObject::tr("Import profile"), ConfigIO::humanError(r));
            return;
        }
        pushSettingsToWindow(page, model);
        pushSoundsToGrid(page, model);
    });
    QObject::connect(w, &SettingsWindow::multiSoundboardChanged, [model, sampler](bool v){
        model->setMultiSoundboard(v);
        if (sampler) sampler->setMultiMode(v);
    });
    // Mute boxes moved to MainPage bottom bar - hook directly there.
    QObject::connect(page->muteLocallyBox(), &QCheckBox::toggled, [model, sampler](bool v){
        model->setPlaybackLocal(!v);
        if (sampler) sampler->setLocalPlayback(!v);
    });
    QObject::connect(page->muteMyselfBox(), &QCheckBox::toggled, [model, sampler](bool v){
        model->setMuteMyselfDuringPb(v);
        if (sampler) sampler->setMuteMyself(v);
    });
    QObject::connect(page->previewOnlyBox(), &QCheckBox::toggled, [model](bool v){
        model->setPreviewOnly(v);
    });
    QObject::connect(w, &SettingsWindow::showHotkeysOnButtonsChanged, [model, page](bool v){
        model->setShowHotkeysOnButtons(v);
        page->buttonGrid()->setShowHotkeys(v);
    });
    QObject::connect(w, &SettingsWindow::disableHotkeysChanged, [model](bool v){
        model->setHotkeysEnabled(!v);
    });
    QObject::connect(w, &SettingsWindow::exportRequested, [w, model]{
        QString p = QFileDialog::getSaveFileName(w, QObject::tr("Export configuration"),
            QString(),
            QObject::tr("Legacy soundboard INI (*.ini);;Wrapped JSON (*.json);;All files (*.*)"));
        if (p.isEmpty()) return;
        bool ok = p.endsWith(".ini", Qt::CaseInsensitive)
            ? ConfigIO::exportIniToFile(p, *model)
            : ConfigIO::exportToFile(p, *model);
        if (!ok) QMessageBox::warning(w, QObject::tr("Export"),
            QObject::tr("Failed to write %1").arg(p));
    });
    QObject::connect(w, &SettingsWindow::importRequested, [w, model, page]{
        QString p = QFileDialog::getOpenFileName(w, QObject::tr("Import configuration"),
            QString(),
            QObject::tr("Soundboard config (*.ini *.json);;Legacy INI (*.ini);;Wrapped JSON (*.json);;All files (*.*)"));
        if (p.isEmpty()) return;
        auto r = p.endsWith(".ini", Qt::CaseInsensitive)
            ? ConfigIO::importIniFromFile(p, *model)
            : ConfigIO::importFromFile(p, *model);
        if (r != ConfigIO::ImportResult::Ok) {
            QMessageBox::warning(w, QObject::tr("Import"), ConfigIO::humanError(r));
            return;
        }
        pushSettingsToWindow(page, model);
        pushSoundsToGrid(page, model);
    });
    QObject::connect(w, &SettingsWindow::themeChanged, [model, page](bool enabled, const QColor &accent, const QColor &waveform, const QColor &background, int contrast, const QColor &text, const QColor &button){
        model->setTheme(enabled, accent.name(), waveform.name(), background.name(), contrast, text.isValid() ? text.name() : QString(), button.isValid() ? button.name() : QString());
        Theme::Colors c;
        c.enabled    = enabled;
        c.accent     = accent;
        c.waveform   = waveform;
        c.background = background;
        c.text       = text;
        c.button     = button;
        c.contrast   = contrast;
        Theme::setColors(c);
        page->refreshTheme();
        for (auto *ch : page->channels()) {
            ch->refreshTheme();
            ch->waveform()->update();
        }
        page->buttonGrid()->refreshAppearance();
    });
    QObject::connect(w, &SettingsWindow::themeResetRequested, [w, model, page]{
        Theme::Colors def = Theme::defaultColors();
        def.enabled = false;
        model->setTheme(false, def.accent.name(), def.waveform.name(), def.background.name(), def.contrast, QString(), QString());
        Theme::setColors(def);
        w->setTheme(false, def.accent, def.waveform, def.background, def.contrast, QColor(), QColor());
        page->refreshTheme();
        for (auto *ch : page->channels()) {
            ch->refreshTheme();
            ch->waveform()->update();
        }
        page->buttonGrid()->refreshAppearance();
    });
    QObject::connect(w, &SettingsWindow::themeCopyRequested, [w, model]{
        // GBSB4#bg#accent#wave#text#button#NN  - 5 hex colors + 2-digit
        // hex contrast. "#------" in text/button == auto (no override).
        // Older GBSB1/2/3 strings still parse on import.
        int contrast = qBound(0, model->getThemeContrast(), 100);
        QString text = model->getThemeText().isEmpty()
            ? QStringLiteral("#------")
            : QColor(model->getThemeText()).name().toUpper();
        QString btn = model->getThemeButton().isEmpty()
            ? QStringLiteral("#------")
            : QColor(model->getThemeButton()).name().toUpper();
        QString s = QString("GBSB4%1%2%3%4%5#%6")
            .arg(QColor(model->getThemeBackground()).name().toUpper())
            .arg(QColor(model->getThemeAccent()).name().toUpper())
            .arg(QColor(model->getThemeWaveform()).name().toUpper())
            .arg(text)
            .arg(btn)
            .arg(contrast, 2, 16, QChar('0')).toUpper();
        QApplication::clipboard()->setText(s);
        QMessageBox::information(w, QObject::tr("Copy theme"),
            QObject::tr("Theme copied to clipboard:\n\n%1\n\n"
                        "Send that string to anyone running this soundboard "
                        "and they can paste it via Paste theme.").arg(s));
    });
    QObject::connect(w, &SettingsWindow::themePasteRequested, [w, model, page]{
        bool ok = false;
        QString seed = QApplication::clipboard()->text().trimmed();
        QString in = QInputDialog::getText(w, QObject::tr("Paste theme"),
            QObject::tr("Theme share string:"), QLineEdit::Normal, seed, &ok);
        if (!ok || in.isEmpty()) return;
        QString s = in.trimmed();
        QRegularExpression rx4("^GBSB4(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f-]{6})(#[0-9A-Fa-f-]{6})#([0-9A-Fa-f]{2})$");
        QRegularExpression rx3("^GBSB3(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f-]{6})#([0-9A-Fa-f]{2})$");
        QRegularExpression rx2("^GBSB2(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})#([0-9A-Fa-f]{2})$");
        QRegularExpression rx1("^GBSB1(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})(#[0-9A-Fa-f]{6})$");
        QColor bg, acc, wave, txt, btn;
        int contrast = 50;
        auto m = rx4.match(s);
        if (m.hasMatch()) {
            bg   = QColor(m.captured(1));
            acc  = QColor(m.captured(2));
            wave = QColor(m.captured(3));
            QString tt = m.captured(4);
            txt  = (tt == "#------") ? QColor() : QColor(tt);
            QString bb = m.captured(5);
            btn  = (bb == "#------") ? QColor() : QColor(bb);
            contrast = qBound(0, m.captured(6).toInt(nullptr, 16), 100);
        } else if ((m = rx3.match(s)).hasMatch()) {
            bg   = QColor(m.captured(1));
            acc  = QColor(m.captured(2));
            wave = QColor(m.captured(3));
            QString tt = m.captured(4);
            txt  = (tt == "#------") ? QColor() : QColor(tt);
            contrast = qBound(0, m.captured(5).toInt(nullptr, 16), 100);
        } else if ((m = rx2.match(s)).hasMatch()) {
            bg   = QColor(m.captured(1));
            acc  = QColor(m.captured(2));
            wave = QColor(m.captured(3));
            contrast = qBound(0, m.captured(4).toInt(nullptr, 16), 100);
        } else if ((m = rx1.match(s)).hasMatch()) {
            bg   = QColor(m.captured(1));
            acc  = QColor(m.captured(2));
            wave = QColor(m.captured(3));
        } else {
            QMessageBox::warning(w, QObject::tr("Paste theme"),
                QObject::tr("That doesn't look like a valid theme string.\n"
                            "Expected format: GBSB4#XXXXXX#XXXXXX#XXXXXX#XXXXXX#XXXXXX#NN"));
            return;
        }
        model->setTheme(true, acc.name(), wave.name(), bg.name(), contrast, txt.isValid() ? txt.name() : QString(), btn.isValid() ? btn.name() : QString());
        Theme::Colors c;
        c.enabled = true; c.accent = acc; c.waveform = wave;
        c.background = bg; c.text = txt; c.button = btn; c.contrast = contrast;
        Theme::setColors(c);
        w->setTheme(true, acc, wave, bg, contrast, txt, btn);
        page->refreshTheme();
        for (auto *ch : page->channels()) {
            ch->refreshTheme();
            ch->waveform()->update();
        }
        page->buttonGrid()->refreshAppearance();
    });
    QObject::connect(w, &SettingsWindow::resetAllHotkeysRequested, [w, model, page]{
        auto choice = QMessageBox::question(w, QObject::tr("Reset all hotkeys"),
            QObject::tr("Stop every saved hotkey from triggering a button?\n\n"
                        "This wipes the soundboard's local list of hotkeys. "
                        "TeamSpeak's hotkey profile still holds the binding "
                        "until you remove it via TeamSpeak's hotkey settings."),
            QMessageBox::Yes | QMessageBox::No);
        if (choice != QMessageBox::Yes) return;
        HotkeyBlock::blockAll(model->numSounds());
        page->buttonGrid()->clearAllHotkeyOverlays();
    });
}

void connectGrid(MainPage *page, ConfigModel *model, Sampler *sampler) {
    auto *grid = page->buttonGrid();

    QObject::connect(grid, &ButtonGrid::buttonTriggered, [model, sampler, page](int idx){
        // 200ms debounce vs TS3 hotkey auto-repeat.
        static QHash<int, qint64> s_lastTriggerMs;
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (s_lastTriggerMs.value(idx, 0) + 200 > now) return;
        s_lastTriggerMs[idx] = now;

        auto *info = model->getSoundInfo(idx);
        if (!info || !sampler) return;
        if (info->filename.isEmpty() && !info->isMacro) return;

        // Stop the running preview - its slot fx fight the channel slot's.
        int prevSlot = sampler->findSlotByState(Sampler::ePLAYING_PREVIEW);
        if (prevSlot >= 0) sampler->stopPlayback(prevSlot);

        // Slot pick: silent channel first, else round-robin oldest.
        const int n = page->channels().size();
        if (n <= 0) return;
        static int s_rr = 0;
        int slot = -1;
        for (int i = 0; i < n; ++i) {
            int s = i;
            if (sampler->getState(s) == Sampler::eSILENT) { slot = s; break; }
        }
        if (slot < 0) { slot = s_rr % n; }
        s_rr = (slot + 1) % n;

        // Push the channel's current slider values into the sampler slot
        // so the slot starts with the visible levels (instead of stale
        // defaults that only refresh on slider tweak).
        auto syncChannelToSlot = [sampler, page](int s){
            if (!sampler || s < 0 || s >= page->channels().size()) return;
            auto *ch = page->channels().at(s);
            sampler->setSlotVolumeLocal (s, ch->volume()->local());
            sampler->setSlotVolumeRemote(s, ch->volume()->remote());
            sampler->setSlotPitchFactor (s, static_cast<float>(std::pow(3.0, ch->fx()->pitch()  / 100.0)));
            sampler->setSlotSpeedFactor (s, static_cast<float>(std::pow(3.0, ch->fx()->speed()  / 100.0)));
            sampler->setSlotReverbMix   (s, ch->fx()->reverb() / 100.0f);
        };

        if (info->isMacro && !info->macroState.isEmpty()) {
            // Save pre-macro state for restore. Snapshot the LIVE playback
            // position from the sampler (Channel::state() always reports
            // 0.0 because the widget doesn't track elapsed time) so the
            // restore can resume each channel at the exact second the user
            // fired the macro.
            s_preMacroStates.clear();
            for (int chi = 0; chi < page->channels().size(); ++chi) {
                auto *chSnap = page->channels().at(chi);
                ChannelState snap = chSnap->state();
                if (sampler) snap.playbackPos = sampler->getPosition(chi);
                s_preMacroStates.append(snap);
            }
            s_preMacroChannelCount = page->channels().size();
            s_macroActive = true;
            page->restoreMacroBtn()->setVisible(true);

            QJsonDocument doc = QJsonDocument::fromJson(info->macroState);
            if (doc.isArray()) {
                QJsonArray arr = doc.array();
                // Make sure we have enough channels visible.
                while (page->channels().size() < arr.size()) page->addChannel();
                for (int i = 0; i < arr.size(); ++i) {
                    auto entry = arr.at(i).toObject();
                    auto stateBytes = QJsonDocument(entry.value("state").toObject()).toJson(QJsonDocument::Compact);
                    ChannelState st;
                    if (!ChannelState::fromJson(stateBytes, st)) continue;
                    auto *ch = page->channels().at(i);
                    if (entry.contains("name"))
                        ch->setTitle(entry.value("name").toString());
                    ch->applyState(st);
                    if (!st.filename.isEmpty()) {
                        SoundInfo macroSound;
                        macroSound.filename = st.filename;
                        sampler->playSoundInSlot(i, macroSound, false);
                        // Slot fx must be pushed AFTER play - slot only has
                        // inputFile once playSoundInSlot returns.
                        sampler->setSlotVolumeLocal (i, st.volumeLocal);
                        sampler->setSlotVolumeRemote(i, st.volumeRemote);
                        sampler->setSlotPitchFactor (i, static_cast<float>(std::pow(3.0, st.pitch / 100.0)));
                        sampler->setSlotSpeedFactor (i, static_cast<float>(std::pow(3.0, st.speed / 100.0)));
                        sampler->setSlotReverbMix   (i, st.reverb / 100.0f);
                        if (st.playbackPos > 0.0)
                            sampler->seek(st.playbackPos, i);
                    }
                }
            }
            return;
        }

        // playSoundInSlot resets per-slot volume to global defaults and
        // only honors pitch/speed/reverb on slots with an inputFile, so
        // push channel/per-button values AFTER play returns.
        if (!sampler->playSoundInSlot(slot, *info, false)) {
            QMessageBox::warning(page, QObject::tr("Playback failed"),
                QObject::tr("Could not play \"%1\".\n\n"
                            "The file may be missing, unreadable, or in an "
                            "unsupported format.").arg(info->filename));
            return;
        }
        auto *ch = page->channels().at(slot);
        sampler->setSlotVolumeLocal (slot, ch->volume()->local());
        sampler->setSlotVolumeRemote(slot, ch->volume()->remote());
        const bool globalFx = model->getGlobalFxEnabled();
        if (globalFx && info->fxRemember) {
            // Per-button FX overrides the channel's current FX.
            sampler->setSlotPitchFactor (slot, static_cast<float>(std::pow(3.0, info->fxPitch  / 100.0)));
            sampler->setSlotSpeedFactor (slot, static_cast<float>(std::pow(3.0, info->fxSpeed  / 100.0)));
            sampler->setSlotReverbMix   (slot, info->fxReverb / 100.0f);
            ch->fx()->setPitch(info->fxPitch);
            ch->fx()->setSpeed(info->fxSpeed);
            ch->fx()->setReverb(info->fxReverb);
            ch->fx()->setSync (info->fxSyncPitchSpeed);
        } else if (globalFx) {
            syncChannelToSlot(slot);
        } else {
            // Master FX off: force neutral so any inherited slot state
            // from playSoundInSlot is wiped.
            sampler->setSlotPitchFactor(slot, 1.0f);
            sampler->setSlotSpeedFactor(slot, 1.0f);
            sampler->setSlotReverbMix  (slot, 0.0f);
        }
    });

    QObject::connect(grid, &ButtonGrid::buttonFileDropped, [model, page](int idx, const QList<QUrl> &urls){
        if (urls.isEmpty()) return;
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        s.filename = urls.first().toLocalFile();
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    // Drag a button onto another = swap their SoundInfo. The actual
    // swap is deferred via QTimer::singleShot(0) because the drop
    // event is still mid-flight when this slot fires - if we rebuild
    // the grid synchronously here, the source SoundButton gets
    // destroyed and the rest of QPushButton::dropEvent dereferences
    // freed memory => crash.
    QObject::connect(grid, &ButtonGrid::buttonReordered, [model, page](int fromIdx, int toIdx){
        if (fromIdx == toIdx) return;
        QTimer::singleShot(0, page, [model, page, fromIdx, toIdx]{
            SoundInfo a, b;
            if (auto *cur = model->getSoundInfo(fromIdx)) a = *cur;
            if (auto *cur = model->getSoundInfo(toIdx))   b = *cur;
            model->setSoundInfo(fromIdx, b);
            model->setSoundInfo(toIdx,   a);
            pushSoundsToGrid(page, model);
        });
    });

    QObject::connect(grid, &ButtonGrid::clearButtonRequested, [model, page](int idx){
        SoundInfo s;
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    QObject::connect(grid, &ButtonGrid::createMacroRequested, [model, page, sampler](int idx){
        if (page->channels().isEmpty()) return;
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        s.isMacro = true;

        // Snapshot every visible channel (name + state + live pos).
        QJsonArray arr;
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ChannelState st = ch->state();
            // Snapshot live playback position from the sampler.
            if (sampler) st.playbackPos = sampler->getPosition(i);
            QJsonObject entry;
            entry["name"]  = ch->title();
            entry["state"] = QJsonDocument::fromJson(st.toJson()).object();
            arr.append(entry);
        }
        s.macroState = QJsonDocument(arr).toJson(QJsonDocument::Compact);
        if (s.customText.isEmpty())
            s.customText = QObject::tr("Macro %1").arg(idx + 1);
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    QObject::connect(grid, &ButtonGrid::chooseFileRequested, [model, page](int idx){
        QString cur;
        if (auto *info = model->getSoundInfo(idx)) cur = info->filename;
        QString p = QFileDialog::getOpenFileName(page,
            QObject::tr("Choose sound file"), cur,
            QObject::tr("Audio (*.mp3 *.wav *.flac *.ogg *.opus *.aac *.m4a);;All files (*.*)"));
        if (p.isEmpty()) return;
        SoundInfo s;
        if (auto *cur2 = model->getSoundInfo(idx)) s = *cur2;
        s.filename = p;
        s.isMacro = false;
        s.macroState.clear();
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    QObject::connect(grid, &ButtonGrid::renameMacroRequested, [model, page](int idx){
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        bool ok = false;
        QString name = QInputDialog::getText(page, QObject::tr("Rename macro"),
            QObject::tr("Macro name:"), QLineEdit::Normal, s.customText, &ok);
        if (!ok) return;
        s.customText = name;
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    QObject::connect(grid, &ButtonGrid::editButtonRequested, [model, page](int idx){
        auto *cur = model->getSoundInfo(idx);
        if (!cur) return;
        auto *dlg = new ButtonAdvancedPanel(page);
        Theme::trackThemedWidget(dlg);
        dlg->setGlobalFxEnabled(model->getGlobalFxEnabled());
        dlg->setSoundInfo(*cur);
        QObject::connect(dlg, &ButtonAdvancedPanel::soundInfoAccepted,
                         [model, page, idx](const SoundInfo &s){
            model->setSoundInfo(idx, s);
            pushSoundsToGrid(page, model);
        });
        QObject::connect(dlg, &ButtonAdvancedPanel::hotkeyAssignRequested,
                         dlg, [page, idx]{
            // Re-arming a hotkey clears any prior block.
            HotkeyBlock::setBlocked(idx, false);
            ConfigQt::openHotkeySetDialog(static_cast<size_t>(idx), page);
        });
        QObject::connect(dlg, &ButtonAdvancedPanel::hotkeyResetRequested,
                         dlg, [page, idx]{
            HotkeyBlock::setBlocked(idx, true);
            page->buttonGrid()->setHotkeyOverlay(idx, QString());
        });
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    QObject::connect(grid, &ButtonGrid::setHotkeyRequested, [page](int idx){
        HotkeyBlock::setBlocked(idx, false);
        ConfigQt::openHotkeySetDialog(static_cast<size_t>(idx), page);
    });

    QObject::connect(page->searchBar(), &SearchBar::filterChanged,
                     grid, &ButtonGrid::setSearchFilter);
}

void connectChannels(MainPage *page, ConfigModel *model, Sampler *sampler) {
    auto wireChannelButtons = [page, sampler, model](Channel *ch) {
        QObject::connect(ch, &Channel::addChannelRequested, page, [page]{
            page->addChannel();
        });
        // Drop a SoundButton on a channel: stop slot, load file, leave
        // paused so the user starts it manually.
        QObject::connect(ch, &Channel::soundDroppedFromButton, page,
                         [page, sampler, model](int channelId, int btnIdx){
            if (!sampler) return;
            auto *info = model->getSoundInfo(btnIdx);
            if (!info || info->filename.isEmpty()) return;
            int slot = -1;
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i)->channelId() == channelId) { slot = i; break; }
            }
            if (slot < 0) return;
            sampler->stopPlayback(slot);
            if (!sampler->playSoundInSlot(slot, *info, false)) {
                QMessageBox::warning(page, QObject::tr("Playback failed"),
                    QObject::tr("Could not load \"%1\" into the channel.")
                        .arg(info->filename));
                return;
            }
            auto *target = page->channels().at(slot);
            sampler->setSlotVolumeLocal (slot, target->volume()->local());
            sampler->setSlotVolumeRemote(slot, target->volume()->remote());
            const bool globalFx = model->getGlobalFxEnabled();
            if (globalFx && info->fxRemember) {
                sampler->setSlotPitchFactor(slot, static_cast<float>(std::pow(3.0, info->fxPitch  / 100.0)));
                sampler->setSlotSpeedFactor(slot, static_cast<float>(std::pow(3.0, info->fxSpeed  / 100.0)));
                sampler->setSlotReverbMix  (slot, info->fxReverb / 100.0f);
                target->fx()->setPitch(info->fxPitch);
                target->fx()->setSpeed(info->fxSpeed);
                target->fx()->setReverb(info->fxReverb);
                target->fx()->setSync (info->fxSyncPitchSpeed);
            } else if (globalFx) {
                sampler->setSlotPitchFactor(slot, static_cast<float>(std::pow(3.0, target->fx()->pitch()  / 100.0)));
                sampler->setSlotSpeedFactor(slot, static_cast<float>(std::pow(3.0, target->fx()->speed()  / 100.0)));
                sampler->setSlotReverbMix  (slot, target->fx()->reverb() / 100.0f);
            } else {
                sampler->setSlotPitchFactor(slot, 1.0f);
                sampler->setSlotSpeedFactor(slot, 1.0f);
                sampler->setSlotReverbMix  (slot, 0.0f);
            }
            sampler->pausePlayback(slot);
        });
        QObject::connect(ch, &Channel::titleChanged, page, [](int id, const QString &t){
            ChannelStatePersistence::saveName(id, t);
        });
        QString savedName = ChannelStatePersistence::loadName(ch->channelId());
        if (!savedName.isEmpty()) ch->setTitle(savedName);
        ch->setFxVisible(model && model->getGlobalFxEnabled());
        ch->setWaveformVisible(!(model && model->getHideWaveform()));
        QObject::connect(ch, &Channel::removeChannelRequested, page, [page, sampler](int id){
            // Stop slot playback BEFORE removing the widget so audio +
            // UI tear down together.
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i)->channelId() == id) {
                    if (sampler) sampler->stopPlayback(i);
                    page->removeChannel(i);
                    break;
                }
            }
            // Channel 0 is the always-present primary.
            for (int i = 0; i < page->channels().size(); ++i) {
                page->channels().at(i)->setRemovable(i > 0);
            }
        });
        int idx = page->channels().indexOf(ch);
        ch->setRemovable(idx > 0);
    };
    QObject::connect(page, &MainPage::channelAdded, page, [page, wireChannelButtons](int idx){
        if (auto *ch = page->channelAt(idx)) wireChannelButtons(ch);
    });
    for (auto *existing : page->channels()) wireChannelButtons(existing);

    // Sampler -> waveform indicator. Slot N drives channel widget N.
    if (sampler) {
        QObject::connect(sampler, &Sampler::onStartPlaying, page,
                         [page](int slot, bool preview, QString filename){
            // Preview never owns a channel widget.
            if (preview) return;
            if (slot < 0 || slot >= page->channels().size()) return;
            auto *ch = page->channels().at(slot);
            SoundInfo info;
            info.filename = filename;
            ch->waveform()->setSound(info);
            ch->waveform()->setPlaying(true);
        }, Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onStopPlaying, page,
                         [page, sampler](int slot){
            if (slot < 0 || slot >= page->channels().size()) return;
            // Preview shares slot indices but doesn't own the UI.
            if (sampler && sampler->getState(slot) == Sampler::ePLAYING_PREVIEW) return;
            auto *wave = page->channels().at(slot)->waveform();
            wave->setPlaying(false);
            wave->setFilename(QString());
            wave->clearPlayback();
        }, Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onPausePlaying, page,
                         [page](int slot){
            if (slot < 0 || slot >= page->channels().size()) return;
            page->channels().at(slot)->waveform()->setPaused(true);
        }, Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onUnpausePlaying, page,
                         [page](int slot){
            if (slot < 0 || slot >= page->channels().size()) return;
            page->channels().at(slot)->waveform()->setPaused(false);
        }, Qt::QueuedConnection);
    }

    auto wire = [model, sampler](Channel *ch) {
        const int slot = ch->channelId();
        // Channel #0 also maps to GLOBAL fx values for legacy callers.
        const bool isPrimary = (slot == 0);

        QObject::connect(ch->volume(), &VolumeControl::localChanged,
                         [model, sampler, slot, isPrimary](int v){
            if (isPrimary) model->setVolumeLocal(v);
            if (sampler) {
                sampler->setSlotVolumeLocal(slot, v);
                if (isPrimary) sampler->setVolumeLocal(v);
            }
        });
        QObject::connect(ch->volume(), &VolumeControl::remoteChanged,
                         [model, sampler, slot, isPrimary](int v){
            if (isPrimary) model->setVolumeRemote(v);
            if (sampler) {
                sampler->setSlotVolumeRemote(slot, v);
                if (isPrimary) sampler->setVolumeRemote(v);
            }
        });
        QObject::connect(ch->fx(), &FxPanel::pitchChanged,
                         [model, sampler, slot, isPrimary](int v){
            if (isPrimary) model->setPitchValue(v);
            if (sampler) {
                // Match legacy ConfigQt scaling: factor = 3^(v/100).
                float factor = static_cast<float>(std::pow(3.0, v / 100.0));
                sampler->setSlotPitchFactor(slot, factor);
                if (isPrimary) sampler->setPitchFactor(factor);
            }
        });
        QObject::connect(ch->fx(), &FxPanel::speedChanged,
                         [model, sampler, slot, isPrimary](int v){
            if (isPrimary) model->setSpeedValue(v);
            if (sampler) {
                float factor = static_cast<float>(std::pow(3.0, v / 100.0));
                sampler->setSlotSpeedFactor(slot, factor);
                if (isPrimary) sampler->setSpeedFactor(factor);
            }
        });
        QObject::connect(ch->fx(), &FxPanel::reverbChanged,
                         [model, sampler, slot, isPrimary](int v){
            if (isPrimary) model->setReverbValue(v);
            if (sampler) sampler->setSlotReverbMix(slot, v / 100.0f);
        });
        QObject::connect(ch->fx(), &FxPanel::syncChanged,
                         [model, isPrimary](bool s){
            if (isPrimary) model->setSyncPitchSpeed(s);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::stopClicked,
                         [sampler, slot]{ if (sampler) sampler->stopPlayback(slot); });
        QObject::connect(ch->waveform(), &WaveformPlayer::playClicked,
                         [sampler, slot]{ if (sampler) sampler->unpausePlayback(slot); });
        QObject::connect(ch->waveform(), &WaveformPlayer::pauseClicked,
                         [sampler, slot]{ if (sampler) sampler->pausePlayback(slot); });
        QObject::connect(ch->waveform(), &WaveformPlayer::skip,
                         [sampler, slot](int sec){
            if (!sampler) return;
            double cur = sampler->getPosition(slot);
            sampler->seek(cur + sec, slot);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::seekRequested,
                         [sampler, slot](double frac){
            if (!sampler) return;
            double len = sampler->getLength(slot);
            if (len > 0.0) sampler->seek(frac * len, slot);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::loopToggled,
                         [sampler, slot](bool on){
            if (sampler) sampler->setSlotLoop(slot, on);
        });
        QObject::connect(ch, &Channel::stateChanged,
                         [ch](int id){
            if (ChannelStatePersistence::isEnabled())
                ChannelStatePersistence::saveState(id, ch->state());
        });
        if (ChannelStatePersistence::isEnabled()) {
            ChannelState st;
            if (ChannelStatePersistence::loadState(ch->channelId(), st)) {
                ch->applyState(st);
                ch->waveform()->setFilename(QString());
                ch->waveform()->clearPlayback();
                ch->waveform()->setPlaying(false);
            }
        }
    };

    // Wire existing channels and any added later.
    for (auto *ch : page->channels()) wire(ch);
    QObject::connect(page, &MainPage::channelAdded, [page, wire](int idx){
        if (auto *ch = page->channelAt(idx)) wire(ch);
    });

    // linkVolumes ON: new channels inherit primary's vol/fx/sync.
    // Runs after wire() so it overrides any persisted-state restore.
    QObject::connect(page, &MainPage::channelAdded, [page, model](int idx){
        if (idx <= 0 || !model->getLinkVolumes()) return;
        auto *primary = page->channelAt(0);
        auto *fresh   = page->channelAt(idx);
        if (!primary || !fresh) return;
        fresh->applyState(primary->state());
    });

    QObject::connect(page->resetButton(), &ResetChannelsBtn::resetRequested,
                     [page, sampler]{
        if (sampler) sampler->stopPlayback(-1);
        while (page->channels().size() > 1)
            page->removeChannel(page->channels().size() - 1);
        if (auto *primary = page->channels().value(0)) {
            primary->setTitle(QObject::tr("Channel 1"));
            primary->applyState(ChannelState{});
            ChannelStatePersistence::saveName(0, primary->title());
        }
    });
}

} // namespace

namespace MainPageWiring {

void wire(MainPage *page, ConfigModel *model, Sampler *sampler) {
    if (!page || !model) return;

    // Track BEFORE setColors so the saved theme actually paints them
    // on first boot (otherwise it stays dormant until a color pick).
    Theme::trackThemedWidget(page);
    Theme::trackThemedWidget(page->settingsWindow());
    {
        Theme::Colors tc;
        tc.enabled    = model->getThemeEnabled();
        tc.accent     = QColor(model->getThemeAccent());
        tc.waveform   = QColor(model->getThemeWaveform());
        tc.background = QColor(model->getThemeBackground());
        tc.contrast   = model->getThemeContrast();
        tc.text       = model->getThemeText().isEmpty() ? QColor() : QColor(model->getThemeText());
        tc.button     = model->getThemeButton().isEmpty() ? QColor() : QColor(model->getThemeButton());
        Theme::setColors(tc);
    }

    pushSettingsToWindow(page, model);
    pushSoundsToGrid(page, model);
    page->refreshTheme();
    for (auto *ch : page->channels()) ch->refreshTheme();
    // Channels added later get the current theme too (Channel ctor reads
    // Theme::colors() but wire ordering can race with restoreSession).
    QObject::connect(page, &MainPage::channelAdded, page, [page](int idx){
        if (auto *ch = page->channelAt(idx)) ch->refreshTheme();
    });

    // Persistence is now ALWAYS on so per-channel link / sync / sandbox
    // state survives a restart. The restoreSession switch only governs
    // whether saved channel COUNT (and the auto-reload of files into
    // those channels) is applied at boot - the underlying state is
    // saved/loaded independently.
    ChannelStatePersistence::setEnabled(true);
    if (model->getRestoreSession()) {
        int saved = ChannelStatePersistence::loadChannelCount();
        const int cap = Sampler::MAX_SLOTS - 1;
        if (saved > cap) saved = cap;
        while (page->channels().size() < saved) page->addChannel();
    }
    if (page->channels().isEmpty()) page->addChannel();

    // Push model state onto sampler so first playback matches UI
    // values without needing a slider tweak.
    if (sampler) {
        sampler->setVolumeLocal(model->getVolumeLocal());
        sampler->setVolumeRemote(model->getVolumeRemote());
        sampler->setLocalPlayback(model->getPlaybackLocal());
        sampler->setMuteMyself(model->getMuteMyselfDuringPb());
        sampler->setEarrapeProtection(model->getEarrapeProtection());
        // Multi-channel always on: one sampler slot per Channel widget.
        sampler->setMultiMode(true);
        model->setMultiSoundboard(true);
        sampler->setPitchFactor(static_cast<float>(std::pow(3.0, model->getPitchValue()  / 100.0)));
        sampler->setSpeedFactor(static_cast<float>(std::pow(3.0, model->getSpeedValue()  / 100.0)));
    }

    // Push ConfigModel vol/FX onto channel 0.
    // Note: linkVolumes is repurposed - it now means "new channels
    // inherit primary's settings", not "local+remote sliders move
    // together".
    if (auto *primary = page->channels().value(0)) {
        primary->volume()->setLocal (model->getVolumeLocal());
        primary->volume()->setRemote(model->getVolumeRemote());
        primary->fx()->setPitch (model->getPitchValue());
        primary->fx()->setSpeed (model->getSpeedValue());
        primary->fx()->setReverb(model->getReverbValue());
        primary->fx()->setSync  (model->getSyncPitchSpeed());
    }

    connectSettings(page, model, sampler);
    connectGrid(page, model, sampler);
    connectChannels(page, model, sampler);

    QObject::connect(page->settingsButton(), &QToolButton::clicked,
                     [page]{ page->settingsWindow()->show(); page->settingsWindow()->raise(); });

    QObject::connect(page->addChannelBtn(), &QPushButton::clicked,
                     page, [page]{
        // Sampler reserves the last slot for previews -> cap MAX_SLOTS-1.
        if (page->channels().size() >= Sampler::MAX_SLOTS - 1) return;
        page->addChannel();
    });

    // Persist channel count for "Restore last session".
    QObject::connect(page, &MainPage::channelAdded, [page, model](int){
        if (model->getRestoreSession())
            ChannelStatePersistence::saveChannelCount(page->channels().size());
        page->updateChannelsAreaHeight(!model->getHideWaveform());
    });
    QObject::connect(page, &MainPage::channelRemoved, [page, model](int){
        if (model->getRestoreSession())
            ChannelStatePersistence::saveChannelCount(page->channels().size());
        page->updateChannelsAreaHeight(!model->getHideWaveform());
    });
    page->updateChannelsAreaHeight(!model->getHideWaveform());

    // Pause / Resume all toggle
    auto *pauseBtn = page->pauseAllBtn();
    pauseBtn->setCheckable(true);
    QObject::connect(pauseBtn, &QPushButton::toggled, pauseBtn,
                     [pauseBtn, sampler](bool checked){
        if (!sampler) return;
        if (checked) {
            sampler->pausePlayback(-1);
            pauseBtn->setText(QObject::tr("Resume all"));
            pauseBtn->setIcon(QIcon(":/icon/img/playarrow_32.png"));
        } else {
            sampler->unpausePlayback(-1);
            pauseBtn->setText(QObject::tr("Pause all"));
            pauseBtn->setIcon(QIcon(":/icon/img/pausebutton_32.png"));
        }
    });
    if (sampler) {
        QObject::connect(sampler, &Sampler::onStartPlaying, pauseBtn,
                         [pauseBtn](int, bool, QString){
            if (pauseBtn->isChecked()) {
                QSignalBlocker b(pauseBtn);
                pauseBtn->setChecked(false);
                pauseBtn->setText(QObject::tr("Pause all"));
                pauseBtn->setIcon(QIcon(":/icon/img/pausebutton_32.png"));
            }
        }, Qt::QueuedConnection);
    }

    QObject::connect(page->stopAllBtn(), &QPushButton::clicked,
                     pauseBtn, [sampler, pauseBtn]{
        if (sampler) sampler->stopPlayback(-1);
        if (pauseBtn->isChecked()) {
            QSignalBlocker b(pauseBtn);
            pauseBtn->setChecked(false);
            pauseBtn->setText(QObject::tr("Pause all"));
            pauseBtn->setIcon(QIcon(":/icon/img/pausebutton_32.png"));
        }
    });

    // Profile switcher wiring
    QObject::connect(page->profileGroup(), qOverload<int>(&QButtonGroup::idClicked),
                     page, [model, page, sampler](int idx){
        // Same save/restore dance as the settings-window profile combo:
        // persist per-channel sandbox state across the switch.
        for (int i = 0; i < page->channels().size(); ++i)
            ChannelStatePersistence::saveState(i, page->channels().at(i)->state());
        model->setConfiguration(idx);
        pushSettingsToWindow(page, model);
        pushSoundsToGrid(page, model);
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ChannelState st;
            if (ChannelStatePersistence::loadState(i, st))
                ch->setSandboxState(st.sandbox);
            if (sampler) {
                if (ch->sandboxState().enabled)
                    sampler->setSlotSandboxState(i, ch->sandboxState());
                else
                    sampler->clearSlotSandbox(i);
            }
        }
        // Re-apply search filter (bug fix #4)
        QString currentFilter = page->searchBar()->filter();
        if (!currentFilter.isEmpty())
            page->buttonGrid()->setSearchFilter(currentFilter);
    });
    // Sync profile buttons when profile changes from any source
    auto syncProfileBtns = [page](int idx) {
        for (int i = 0; i < 4; ++i) {
            auto *btn = page->profileButton(i);
            if (btn) {
                QSignalBlocker b(btn);
                btn->setChecked(i == idx);
            }
        }
    };
    syncProfileBtns(model->getConfiguration());

    QObject::connect(page->restoreMacroBtn(), &QPushButton::clicked, page,
                     [page, sampler]{
        if (!s_macroActive) return;
        // Stop everything the macro started before restoring - otherwise
        // the previous slot keeps playing the macro file while we reload
        // the original.
        if (sampler) sampler->stopPlayback(-1);

        while (page->channels().size() > s_preMacroChannelCount && page->channels().size() > 1)
            page->removeChannel(page->channels().size() - 1);

        for (int i = 0; i < s_preMacroStates.size() && i < page->channels().size(); ++i) {
            const ChannelState &st = s_preMacroStates[i];
            auto *ch = page->channels().at(i);
            ch->applyState(st);
            // Replay the audio that was loaded before the macro fired,
            // seek to the captured live position, then pause so the
            // restore is a quiet undo and the user can resume manually.
            if (!st.filename.isEmpty() && sampler) {
                SoundInfo info;
                info.filename = st.filename;
                if (sampler->playSoundInSlot(i, info, false)) {
                    sampler->setSlotVolumeLocal (i, st.volumeLocal);
                    sampler->setSlotVolumeRemote(i, st.volumeRemote);
                    sampler->setSlotPitchFactor (i, static_cast<float>(std::pow(3.0, st.pitch / 100.0)));
                    sampler->setSlotSpeedFactor (i, static_cast<float>(std::pow(3.0, st.speed / 100.0)));
                    sampler->setSlotReverbMix   (i, st.reverb / 100.0f);
                    sampler->setSlotSandboxState(i, st.sandbox);
                    if (st.playbackPos > 0.0)
                        sampler->seek(st.playbackPos, i);
                    sampler->pausePlayback(i);
                }
            } else if (sampler) {
                sampler->setSlotSandboxState(i, st.sandbox);
            }
        }
        s_macroActive = false;
        page->restoreMacroBtn()->setVisible(false);
        s_preMacroStates.clear();
    });

    // Observe ConfigModel so external state changes (config switch,
    // import, late readConfig) propagate to grid + settings.
    if (!s_observer) {
        s_observer = new MainPageModelObserver(page);
        model->addObserver(s_observer);
    }

    // 10 Hz playback position poll for waveform overlay + time labels.
    if (sampler) {
        auto *posTimer = new QTimer(page);
        posTimer->setInterval(100);
        QObject::connect(posTimer, &QTimer::timeout, page, [page, sampler]{
            for (int i = 0; i < page->channels().size(); ++i) {
                auto *ch = page->channels().at(i);
                if (sampler->getState(i) == Sampler::ePLAYING_PREVIEW) continue;
                double pos = sampler->getPosition(i);
                double len = sampler->getLength(i);
                if (len > 0.0) {
                    ch->waveform()->setPlaybackFraction(pos / len);
                    ch->waveform()->setPosition(pos, len);
                } else {
                    ch->waveform()->clearPlayback();
                }
            }
        });
        posTimer->start();
    }

    // ===== Audio sandbox wiring =====
    auto applyChannelSandboxFlags = [page, model, sampler](){
        bool sandbox = model->getAudioSandboxEnabled();
        bool meter   = model->getAudioMeterVisible();
        bool exprt   = model->getAudioExportEnabled();
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ch->setSandboxFeatureEnabled(sandbox);
            ch->setMeterVisible(meter);
            ch->setExportVisible(exprt);
            // Master switch gates the per-slot DSP. When OFF, drop every
            // slot's DSP so audio fully bypasses the sandbox chain.
            // When ON, only push state for channels whose per-channel
            // FX checkbox is checked - leaving the other slots with no
            // DSP at all (zero CPU + memory cost, matches pre-sandbox
            // playback path exactly).
            if (sampler) {
                if (sandbox && ch->sandboxState().enabled)
                    sampler->setSlotSandboxState(i, ch->sandboxState());
                else
                    sampler->clearSlotSandbox(i);
            }
        }
    };

    // SettingsWindow signals.
    auto *sw = page->settingsWindow();
    QObject::connect(sw, &SettingsWindow::audioSandboxEnabledChanged, [model, applyChannelSandboxFlags](bool v){
        model->setAudioSandboxEnabled(v);
        applyChannelSandboxFlags();
    });
    QObject::connect(sw, &SettingsWindow::audioMeterVisibleChanged, [model, applyChannelSandboxFlags](bool v){
        model->setAudioMeterVisible(v);
        applyChannelSandboxFlags();
    });
    QObject::connect(sw, &SettingsWindow::audioExportEnabledChanged, [model, applyChannelSandboxFlags](bool v){
        model->setAudioExportEnabled(v);
        applyChannelSandboxFlags();
    });
    QObject::connect(sw, &SettingsWindow::resetAllAudioSandboxRequested, [page, sampler](){
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ch->setSandboxState(SandboxState());
            if (sampler) sampler->clearSlotSandbox(i);
        }
    });

    // Per-channel: forward sandbox state changes to Sampler. Wired on
    // every channel-add so dynamically created channels get the same
    // forwarding plumbing.
    auto wireChannelSandbox = [sampler, applyChannelSandboxFlags, model, page](Channel *ch){
        QObject::connect(ch, &Channel::sandboxStateChanged, [sampler](int slot, const SandboxState &s){
            if (sampler) sampler->setSlotSandboxState(slot, s);
        });
        QObject::connect(ch, &Channel::sandboxResetRequested, [sampler](int slot){
            if (sampler) sampler->clearSlotSandbox(slot);
        });
        QObject::connect(ch, &Channel::exportRequested, page, [page, model](int slot){
            // slot here is Channel::m_id which equals the channel's
            // index in MainPage::channels() so long as channels are
            // appended (current behaviour). channelAt(slot) double-
            // guards against the case where the channel was removed
            // between click + slot-dispatch.
            auto *src_ch = page->channelAt(slot);
            if (!src_ch) return;
            QString src = src_ch->waveform()->filename();
            if (src.isEmpty()) {
                QMessageBox::information(page, QObject::tr("Export"),
                    QObject::tr("No audio file loaded on this channel."));
                return;
            }
            if (!QFileInfo::exists(src)) {
                QMessageBox::warning(page, QObject::tr("Export"),
                    QObject::tr("Source file no longer exists:\n%1").arg(src));
                return;
            }
            QString dst = QFileDialog::getSaveFileName(page, QObject::tr("Export audio with DSP"),
                QString(), QObject::tr("WAV files (*.wav)"));
            if (dst.isEmpty()) return;
            if (!dst.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
                dst += QStringLiteral(".wav");
            // Snapshot the channel's LIVE settings - same factor scaling
            // the sampler slot uses (3^(slider/100)) so the exported WAV
            // matches the audible signal. Sandbox state is taken by
            // value so subsequent slider tweaks during the export don't
            // mutate the bake.
            const float pitchFactor  = static_cast<float>(std::pow(3.0, src_ch->fx()->pitch()  / 100.0));
            const float speedFactor  = static_cast<float>(std::pow(3.0, src_ch->fx()->speed()  / 100.0));
            const float reverbMix    = src_ch->fx()->reverb() / 100.0f;
            const bool  sandboxOn    = model->getAudioSandboxEnabled() && src_ch->sandboxState().enabled;
            // No QObject parent: the thread owns its own lifetime and
            // tears down via the QThread::finished -> deleteLater chain.
            // Re-parenting onto `page` risked the parent dying mid-run
            // and tearing the worker down while FFmpeg was still in
            // libavformat.
            auto *exporter = new AudioExporter(src, dst,
                                               pitchFactor, speedFactor, reverbMix,
                                               src_ch->sandboxState(), sandboxOn,
                                               48000.0, nullptr);
            // Floating themed progress card. Lives independently of
            // the exporter (no parent on AudioExporter); the dialog is
            // parented to `page` so it follows the soundboard window.
            auto *progress = new ExportProgressDialog(dst, page);
            progress->setAttribute(Qt::WA_DeleteOnClose);
            QObject::connect(exporter, &AudioExporter::progress,
                             progress, &ExportProgressDialog::setProgress,
                             Qt::QueuedConnection);
            QObject::connect(exporter, &AudioExporter::exportFinished, progress,
                             [progress](bool ok, const QString &err){
                progress->setFinished(ok, err);
            }, Qt::QueuedConnection);
            QObject::connect(progress, &ExportProgressDialog::cancelRequested,
                             exporter, [exporter]{ exporter->requestInterruption(); });
            QObject::connect(exporter, &QThread::finished, exporter, &QObject::deleteLater);
            // If the user closes the dialog mid-encode (Esc / X), make
            // sure the worker is told to stop so it doesn't keep
            // writing to a file the user no longer cares about.
            QObject::connect(progress, &QDialog::rejected, exporter,
                             [exporter]{ exporter->requestInterruption(); });
            progress->show();
            progress->raise();
            progress->activateWindow();
            exporter->start();
        });
        // Apply current global flags so the new channel respects them
        // immediately.
        ch->setSandboxFeatureEnabled(model->getAudioSandboxEnabled());
        ch->setMeterVisible(model->getAudioMeterVisible());
        ch->setExportVisible(model->getAudioExportEnabled());
    };
    for (auto *ch : page->channels()) wireChannelSandbox(ch);
    QObject::connect(page, &MainPage::channelAdded, [page, wireChannelSandbox](int idx){
        if (idx >= 0 && idx < page->channels().size())
            wireChannelSandbox(page->channels().at(idx));
    });

    // Push initial settings + state on startup.
    sw->setAudioSandboxEnabled(model->getAudioSandboxEnabled());
    sw->setAudioMeterVisible(model->getAudioMeterVisible());
    sw->setAudioExportEnabled(model->getAudioExportEnabled());
    applyChannelSandboxFlags();

    // applyChannelSandboxFlags() above already pushed every channel's
    // saved sandbox state into its sampler slot (for channels whose
    // per-channel FX checkbox is checked). connectChannels runs before
    // wireChannelSandbox is hooked, so any applyState() emit during
    // restore is lost - the explicit push here closes the gap and is
    // the fix for "DSP doesn't apply until the user double-toggles
    // the checkbox after a TS3 restart".

    // 25 Hz meter poll: read atomic peak L/R from each slot, push to
    // its channel's ChannelMeter widget.
    if (sampler) {
        auto *meterTimer = new QTimer(page);
        meterTimer->setInterval(40);
        QObject::connect(meterTimer, &QTimer::timeout, page, [page, sampler, model](){
            if (!model->getAudioMeterVisible()) return;
            for (int i = 0; i < page->channels().size(); ++i) {
                float l = 0.0f, r = 0.0f;
                sampler->getSlotPeak(i, l, r);
                page->channels().at(i)->setMeterPeak(l, r);
            }
        });
        meterTimer->start();
    }
}

}
