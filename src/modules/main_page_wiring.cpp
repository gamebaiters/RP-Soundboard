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
#include "vinyl_popup.h"
#include "stream_resolver.h"

#include "../ConfigModel.h"
#include "../samples.h"
#include "../SoundInfo.h"
#include "../MicFx.h"
#include "../dsp/SlotDsp.h"
#include "../config_qt.h"
#include "../main.h"
#include "../AudioUtils.h"
#include "../ts3log.h"
#include "hotkey_block.h"
#include "theme.h"
#include "icon_factory.h"

#include <memory>

#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QDialog>
#include <QComboBox>
#include <QLineEdit>
#include <QClipboard>
#include <QApplication>
#include <QButtonGroup>
#include <QToolButton>
#include <QSet>
#include <QSpinBox>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QToolButton>
#include <QTimer>
#include <QCheckBox>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSettings>
#include <QProgressDialog>
#include <QDir>
#include <memory>
#include <QPointer>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScreen>
#include <QDateTime>
#include <QFile>
#include <cmath>
#include <memory>
#include <functional>

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

// Map slot -> grid button index, so the waveform right-click crop
// editor knows which SoundInfo cell to write back to. Missing entry
// (macro restore, drag-from-file) = live-only edit, no persistence.
static QHash<int, int> s_slotToBtnIdx;

// Map slot -> last-played grid button index. UNLIKE s_slotToBtnIdx
// this survives the slot's stop event so the replay-from-cursor path
// can rebuild the playback context. Cleared by:
//   - clearRequested() from the waveform (the red X next to filename),
//   - removeChannelRequested shift handler,
//   - replay click that mismatches the cell's current filename
//     (cell was emptied or rebound to another file).
struct LastPlayedCtx {
    int     btnIdx   = -1;
    QString filename;          // expected source file (mismatch invalidates)
};
static QHash<int, LastPlayedCtx> s_lastPlayedCtx;

// --- URL / YouTube live-stream per-slot state (v2.3.1) --------------------
// slot -> canonical page URL currently loaded as a live stream in that slot.
static QHash<int, QString> s_slotStreamUrl;
// slot -> video title (drives the waveform file-label + is the "this slot is a
// stream" flag onStartPlaying reads to apply the stream-mode transport).
static QHash<int, QString> s_slotStreamTitle;
// slots mid-load of a stream: guards onStopPlaying from restoring the channel
// name when the stop is only the OLD content being replaced by a new stream.
static QSet<int>           s_slotStreamLoading;
// slots whose current stream resolved as LIVE (no waveform/seek/reverse/vinyl).
static QSet<int>           s_slotStreamLive;
// slot -> the QObject scope of the in-flight resolve for that slot. A NEW
// resolve on the same slot deletes the old scope first, so a stale resolve can
// never fire its resolved() lambda after the user typed a different link
// (seamless replace) or cleared the link (abort).
static QHash<int, QPointer<QObject>> s_slotResolveCtx;
// slot -> canonical URL currently being resolved (for the Cancel button).
static QHash<int, QString> s_slotPendingUrl;
// slot -> its open playlist panel (kept alive, hidden on close so the channel's
// ☰ button can re-show it). One panel per channel = per-channel playlists.
static QHash<int, QPointer<QDialog>> s_slotPlaylistPanel;
// Playlist page URL + title currently loaded in a slot (whole-playlist mode), so
// the right-click menu can offer "save the whole playlist into a button".
static QHash<int, QString> s_slotPlaylistUrl;
static QHash<int, QString> s_slotPlaylistTitle;
// "Assign a just-downloaded audio to a button" mode: after the user saves a
// video's audio and answers "yes, also to a button", the NEXT grid cell click
// binds this file instead of playing. Empty = not in assign mode.
static QString s_pendingAssignFile;
static QString s_pendingAssignTitle;
// slot -> wall-clock (ms) of the last AUTOMATIC stream reconnect. When a
// stream's direct URL dies mid-play (expiry / CDN throttle), the poll re-
// resolves the page URL through yt-dlp and resumes at the last position
// instead of showing an error — the error toast is reserved for the case
// where even the fresh resolve fails. Budget: one auto reconnect per minute
// per slot, so a genuinely broken stream still errors out instead of
// resolve-looping forever. Cleared on every MANUAL load of the slot.
static QHash<int, qint64> s_slotNetRetryMs;

// Clear a slot's live-stream state and put the channel's real name back
// (restoreName is a no-op if the channel isn't showing a green link) and drop
// the stream-mode transport lock. Safe on any slot, stream or not.
static void clearSlotStream(MainPage *page, int slot) {
    s_slotStreamUrl.remove(slot);
    s_slotStreamTitle.remove(slot);
    s_slotStreamLoading.remove(slot);
    s_slotStreamLive.remove(slot);
    if (page && slot >= 0 && slot < page->channels().size()) {
        if (auto *ch = page->channels().at(slot)) {
            ch->restoreName();
            ch->setStreamLoading(false);
            ch->setExportIsDownload(false);
            ch->waveform()->setLiveStream(false);
        }
    }
}

// Fully UNLOAD a channel's playlist (close + forget its panel, hide the ☰).
// Called ONLY on an explicit "do something else" (load a normal sound, paste a
// different link, click another button) — never on a natural track-end, so
// autoplay keeps working.
static void unloadPlaylistPanel(MainPage *page, int slot) {
    if (auto panel = s_slotPlaylistPanel.value(slot)) { panel->close(); panel->deleteLater(); }
    s_slotPlaylistPanel.remove(slot);
    s_slotPlaylistUrl.remove(slot);
    s_slotPlaylistTitle.remove(slot);
    if (page && slot >= 0 && slot < page->channels().size())
        if (auto *ch = page->channels().at(slot)) ch->setPlaylistAvailable(false);
}

// Abort any in-flight resolve for `slot` and return a FRESH one-shot scope for a
// new resolve. Shows the channel's loading marquee. Deleting the previous scope
// disconnects its resolved/failed lambdas so they no longer fire.
static QObject *beginSlotResolve(MainPage *page, int slot,
                                 const QString &stageText = QString()) {
    if (auto old = s_slotResolveCtx.value(slot)) old->deleteLater();
    QObject *ctx = new QObject(page);
    s_slotResolveCtx[slot] = ctx;
    if (page && slot >= 0 && slot < page->channels().size())
        if (auto *ch = page->channels().at(slot)) {
            // Stage-by-stage loading text so the user sees WHAT is happening
            // (resolve vs open vs buffering), not just a generic marquee.
            if (!stageText.isEmpty()) ch->setStreamLoadingText(stageText);
            ch->setStreamLoading(true);
        }
    return ctx;
}

// End the resolve scope for a slot (hide marquee, forget the scope pointer).
static void endSlotResolve(MainPage *page, int slot, QObject *ctx) {
    if (s_slotResolveCtx.value(slot) == ctx) s_slotResolveCtx.remove(slot);
    s_slotPendingUrl.remove(slot);
    if (page && slot >= 0 && slot < page->channels().size())
        if (auto *ch = page->channels().at(slot)) ch->setStreamLoading(false);
    if (ctx) ctx->deleteLater();
}

// Cancel whatever network operation is in flight for `slot`: abort the resolve
// scope (its result is ignored), kill the yt-dlp process, cancel any download,
// hide the marquee and restore the channel. Everything is per-slot so cancelling
// one channel never touches another.
static void cancelSlotStreamLoad(MainPage *page, int slot) {
    if (auto ctx = s_slotResolveCtx.value(slot)) { ctx->deleteLater(); s_slotResolveCtx.remove(slot); }
    const QString url = s_slotPendingUrl.value(slot);
    if (!url.isEmpty()) StreamResolver::instance().cancelResolve(url);
    s_slotPendingUrl.remove(slot);
    StreamResolver::instance().cancelDownload();
    clearSlotStream(page, slot);   // hides marquee + restores name
}

// Transient auto-dismiss toast over a channel widget: shows a stream error +
// probable reason, fades itself out after a few seconds. No modal interruption.
static void showStreamErrorBubble(QWidget *anchor, const QString &msg) {
    if (!anchor) return;
    auto *toast = new QLabel(anchor->window());
    toast->setText(msg);
    toast->setWordWrap(true);
    toast->setMaximumWidth(320);
    toast->setAttribute(Qt::WA_DeleteOnClose);
    toast->setStyleSheet(
        "QLabel { background: #5a1f24; color: #ffd7da; border: 1px solid #b0434c;"
        " border-radius: 6px; padding: 8px 10px; font-weight: bold; }");
    toast->adjustSize();
    QPoint tl = anchor->mapTo(anchor->window(),
                             QPoint((anchor->width() - toast->width()) / 2, 4));
    if (tl.x() < 4) tl.setX(4);
    toast->move(tl);
    toast->show();
    toast->raise();
    QTimer::singleShot(5000, toast, &QWidget::close);
}

// Neutral (azure) transient toast over the window — used for informational
// hints like "click a cell to save the audio there".
static void showInfoToast(QWidget *anchor, const QString &msg, int ms = 6000) {
    if (!anchor) return;
    auto *toast = new QLabel(anchor->window());
    toast->setText(msg);
    toast->setWordWrap(true);
    toast->setMaximumWidth(360);
    toast->setAttribute(Qt::WA_DeleteOnClose);
    toast->setStyleSheet(
        "QLabel { background: #143a52; color: #d7ecff; border: 1px solid #3fa7ff;"
        " border-radius: 6px; padding: 8px 10px; font-weight: bold; }");
    toast->adjustSize();
    QWidget *win = anchor->window();
    toast->move(qMax(8, (win->width() - toast->width()) / 2), 8);
    toast->show();
    toast->raise();
    QTimer::singleShot(ms, toast, &QWidget::close);
}

// Resolve `pageUrl` and load it as a live/VOD stream into `slot`, seamlessly
// replacing whatever that slot was resolving/playing. `greenChannelName` shows
// the pasted link IN GREEN as the channel name (channel-name-paste path);
// playlist / button paths pass false and keep the real name. `autoPlay` leaves
// the slot playing (playlist autoplay); otherwise it loads paused + ready.
// Central point for: per-slot abort/replace, live detection, loading marquee,
// error toast. Used by titleChanged AND the playlist panel.
static void loadStreamIntoSlot(MainPage *page, Sampler *sampler, ConfigModel *model,
                               int slot, const QString &pageUrl,
                               bool greenChannelName, bool autoPlay,
                               bool keepPlaylist = false,
                               bool isAutoRetry = false)
{
    if (!page || !sampler || slot < 0 || slot >= page->channels().size()) return;
    auto *ch0 = page->channels().at(slot);
    if (!ch0) return;
    // A fresh link that is NOT a playlist track = the user chose something else:
    // unload any playlist previously loaded in this channel.
    if (!keepPlaylist) unloadPlaylistPanel(page, slot);
    if (greenChannelName) ch0->showStreamLink(pageUrl);
    // A MANUAL load resets the auto-reconnect budget for this slot; the auto
    // retry itself must not, or a broken stream would reconnect-loop forever.
    if (!isAutoRetry) s_slotNetRetryMs.remove(slot);

    StreamResolver &R = StreamResolver::instance();
    QObject *ctx = beginSlotResolve(page, slot,    // aborts any prior resolve
        isAutoRetry ? QObject::tr("Reconnecting to the stream…")
                    : QObject::tr("Fetching video info…"));
    s_slotPendingUrl[slot] = pageUrl;              // for the Cancel button

    // Fine-grained stage narration from the resolver (engine spawn, site
    // contact, extraction, queued-behind-update). ctx-scoped: dies with the
    // resolve, so a stale stage can never overwrite a newer load's strip.
    QObject::connect(&R, &StreamResolver::resolveProgress, ctx,
        [page, slot, pageUrl](const QString &u, const QString &stage){
            if (u != pageUrl) return;
            if (slot < page->channels().size())
                if (auto *ch = page->channels().at(slot))
                    ch->setStreamLoadingText(stage);
        });
    QObject::connect(&R, &StreamResolver::resolved, ctx,
        [page, sampler, model, slot, pageUrl, ctx, greenChannelName, autoPlay](
            const QString &u, const ResolvedStream &s){
            if (u != pageUrl) return;
            endSlotResolve(page, slot, ctx);
            if (slot >= page->channels().size()) return;
            auto *ch = page->channels().at(slot);
            if (!ch) return;
            // Resolve done — the next (visible) stage is FFmpeg connecting to
            // the CDN, which can take a second or two. Keep the marquee up
            // with the new stage text (with the found title, so the user sees
            // WHAT was found); onStartPlaying hides it.
            QString found = s.title;
            if (found.size() > 34) found = found.left(32) + QStringLiteral("…");
            if (s.isLive)
                ch->setStreamLoadingText(found.isEmpty()
                    ? QObject::tr("Connecting to the live stream…")
                    : QObject::tr("Connecting to live: %1").arg(found));
            else
                ch->setStreamLoadingText(found.isEmpty()
                    ? QObject::tr("Opening audio stream…")
                    : QObject::tr("Opening: %1").arg(found));
            ch->setStreamLoading(true);
            SoundInfo snd;
            snd.filename          = s.directUrl;
            snd.isStreamUrl       = true;
            snd.isLive            = s.isLive;
            snd.netUserAgent      = s.userAgent;
            snd.netHeaders        = s.headers;
            snd.streamTitle       = s.title;
            snd.streamDurationSec = s.durationSec;
            s_slotStreamLoading.insert(slot);
            s_slotStreamTitle[slot] = s.title.isEmpty() ? pageUrl : s.title;
            s_slotStreamUrl[slot]   = pageUrl;
            if (s.isLive) s_slotStreamLive.insert(slot);
            else          s_slotStreamLive.remove(slot);
            // Open the network stream OFF the GUI thread so nothing freezes.
            // Capture the FX / volume values here (GUI) and hand them off; a
            // failed open surfaces via onPlaybackError (which clears the state).
            const bool globalFx = model && model->getGlobalFxEnabled();
            sampler->playSoundInSlotAsync(slot, snd,
                ch->volume()->local(), ch->volume()->remote(),
                AudioUtils::sliderToPitchFactor(ch->fx()->pitch()),
                AudioUtils::sliderToPitchFactor(ch->fx()->speed()),
                ch->fx()->reverb() / 100.0f,
                globalFx, autoPlay);
        });
    QObject::connect(&R, &StreamResolver::failed, ctx,
        [page, slot, pageUrl, ctx, greenChannelName](const QString &u, const QString &err){
            if (u != pageUrl) return;
            endSlotResolve(page, slot, ctx);
            if (slot < page->channels().size())
                if (auto *ch = page->channels().at(slot))
                    showStreamErrorBubble(ch, QObject::tr("Couldn't load the link.\n%1").arg(err));
            clearSlotStream(page, slot);   // restore name / drop green
            Q_UNUSED(greenChannelName);
        });
    R.resolve(pageUrl);
}

// Open the non-modal playlist panel for a resolved playlist: header (reference
// channel + playlist name), a list of every entry, an Autoplay toggle. Clicking
// a row loads that video into the reference channel; with autoplay on, the next
// entry auto-loads when the current one ends. Reuses loadStreamIntoSlot so every
// per-entry behaviour (live handling, error toast, seamless replace) is shared.
static void openPlaylistPanel(MainPage *page, Sampler *sampler, ConfigModel *model,
                              int slot, const QString &playlistTitle,
                              const QVector<PlaylistEntry> &entries)
{
    // One panel per slot: if this channel already has a playlist panel open (or
    // hidden), reuse it instead of stacking a second one (fixes the double-open
    // and lets the channel's ☰ button re-show a closed panel).
    if (auto existing = s_slotPlaylistPanel.value(slot)) {
        existing->show();
        existing->raise();
        existing->activateWindow();
        return;
    }
    auto *dlg = new QDialog(page);
    // NOT delete-on-close: closing hides it so the ☰ reopen button works.
    dlg->setWindowTitle(QObject::tr("Playlist"));
    dlg->resize(420, 460);
    s_slotPlaylistPanel[slot] = dlg;
    s_slotPlaylistTitle[slot] = playlistTitle;
    if (slot >= 0 && slot < page->channels().size())
        if (auto *c = page->channels().at(slot)) c->setPlaylistAvailable(true);

    QString chName = (slot >= 0 && slot < page->channels().size() && page->channels().at(slot))
                        ? page->channels().at(slot)->title() : QObject::tr("Channel %1").arg(slot + 1);

    auto *lay = new QVBoxLayout(dlg);
    auto *header = new QLabel(QObject::tr("<b>%1</b><br>Channel: %2")
                             .arg(playlistTitle.isEmpty() ? QObject::tr("Playlist") : playlistTitle.toHtmlEscaped())
                             .arg(chName.toHtmlEscaped()), dlg);
    header->setTextFormat(Qt::RichText);
    lay->addWidget(header);

    auto *list = new QListWidget(dlg);
    for (const PlaylistEntry &e : entries) {
        auto *item = new QListWidgetItem(e.title.isEmpty() ? e.pageUrl : e.title, list);
        item->setData(Qt::UserRole, e.pageUrl);
    }
    lay->addWidget(list, 1);

    auto *autoplay = new QCheckBox(QObject::tr("Autoplay (advance to the next when one ends)"), dlg);
    lay->addWidget(autoplay);

    auto *hint = new QLabel(QObject::tr("Click a track to load it into the channel."), dlg);
    hint->setStyleSheet("color: palette(mid);");
    lay->addWidget(hint);

    // Shared cursor into the list, so autoplay chaining can advance it.
    auto curIdx = std::make_shared<int>(-1);
    // "armed" gates the autoplay advance to ONE per track that ACTUALLY started
    // playing. Loading a track stops whatever the slot held first, so
    // onStopPlaying fires spuriously during EVERY load — without this guard,
    // clicking track 1 cascaded (load 1 -> stop -> advance to 2 -> stop ->
    // advance to 3...) and jumped straight to the third track. Any load (user
    // click OR autoplay) clears it; only a real onStartPlaying re-arms it, and
    // an advance consumes it — so a load-induced stop can never advance.
    auto armed = std::make_shared<bool>(false);

    auto loadRow = [page, sampler, model, slot, curIdx, list, armed](int row, bool play){
        if (row < 0 || row >= list->count()) return;
        *armed = false;   // a fresh load: the imminent stop-of-old must NOT advance
        *curIdx = row;
        list->setCurrentRow(row);
        const QString url = list->item(row)->data(Qt::UserRole).toString();
        loadStreamIntoSlot(page, sampler, model, slot, url,
                           /*greenChannelName*/false, /*autoPlay*/play,
                           /*keepPlaylist*/true);   // stay loaded across tracks
    };

    QObject::connect(list, &QListWidget::itemClicked, dlg, [loadRow, list, autoplay](QListWidgetItem *it){
        loadRow(list->row(it), autoplay->isChecked());
    });

    // Autoplay chaining: when the reference slot goes idle and autoplay is on,
    // load the next entry. Scoped to `dlg` so it dies with the panel.
    if (sampler) {
        QObject::connect(sampler, &Sampler::onStartPlaying, dlg,
            [slot, armed](int startedSlot, bool, QString){
                if (startedSlot == slot) *armed = true;
            });
        QObject::connect(sampler, &Sampler::onStopPlaying, dlg,
            [dlg, slot, curIdx, list, autoplay, loadRow, armed](int stoppedSlot){
                if (stoppedSlot != slot) return;
                if (!autoplay->isChecked()) return;
                if (!*armed) return;            // stop was a load, not a track end
                *armed = false;                 // consume: exactly one advance
                int next = *curIdx + 1;
                if (next < list->count())
                    QTimer::singleShot(150, dlg, [loadRow, next]{ loadRow(next, true); });
            }, Qt::QueuedConnection);
    }

    dlg->show();
    dlg->raise();
}

// Resolve a playlist page URL and open its (non-modal) panel against `slot` as
// the reference channel. Shared by the paste-as-channel-name "Whole playlist"
// choice AND a button that has a whole playlist saved on it. Records the
// playlist URL/title per slot so the right-click menu can re-save it.
static void resolveAndOpenPlaylist(MainPage *page, Sampler *sampler, ConfigModel *model,
                                   int slot, const QString &pageUrl)
{
    if (!page || slot < 0 || slot >= page->channels().size()) return;
    // If a DIFFERENT playlist is already loaded in this slot, tear its panel
    // down FIRST — otherwise openPlaylistPanel's reuse-by-slot early-return
    // would just re-show the OLD (stale) panel instead of the new playlist.
    // (Same URL = a deliberate reopen; keep the existing panel.)
    if (s_slotPlaylistUrl.value(slot) != pageUrl)
        unloadPlaylistPanel(page, slot);
    s_slotPlaylistUrl[slot] = pageUrl;
    StreamResolver &R = StreamResolver::instance();
    QObject *ctx = beginSlotResolve(page, slot,    // marquee + cancellable
        QObject::tr("Fetching playlist tracks…"));
    s_slotPendingUrl[slot] = pageUrl;
    QObject::connect(&R, &StreamResolver::playlistResolved, ctx,
        [page, sampler, model, slot, pageUrl, ctx](const QString &u, const QString &title,
                                                   const QVector<PlaylistEntry> &entries){
            if (u != pageUrl) return;
            endSlotResolve(page, slot, ctx);
            openPlaylistPanel(page, sampler, model, slot, title, entries);
        });
    QObject::connect(&R, &StreamResolver::failed, ctx,
        [page, slot, pageUrl, ctx](const QString &u, const QString &err){
            if (u != pageUrl) return;
            endSlotResolve(page, slot, ctx);
            if (slot < page->channels().size())
                if (auto *c = page->channels().at(slot))
                    showStreamErrorBubble(c, QObject::tr("Couldn't load the playlist.\n%1").arg(err));
        });
    R.resolvePlaylist(pageUrl);
}

// Playlist link pasted as a channel name: ask whole-playlist vs single video.
static void openPlaylistFlow(MainPage *page, Sampler *sampler, ConfigModel *model,
                             int slot, const QString &pageUrl)
{
    if (slot < 0 || slot >= page->channels().size()) return;
    auto *ch = page->channels().at(slot);
    if (!ch) return;
    ch->showStreamLink(pageUrl);   // green while the user decides

    QMessageBox box(page);
    box.setWindowTitle(QObject::tr("Playlist detected"));
    box.setText(QObject::tr("This link is a playlist. What would you like to load?"));
    QPushButton *whole  = box.addButton(QObject::tr("Whole playlist"), QMessageBox::AcceptRole);
    QPushButton *single = box.addButton(QObject::tr("Just this video"), QMessageBox::YesRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();

    if (box.clickedButton() == single) {
        loadStreamIntoSlot(page, sampler, model, slot, pageUrl,
                           /*greenChannelName*/true,
                           /*autoPlay*/model->getStreamAutoplay());
        return;
    }
    if (box.clickedButton() == whole) {
        ch->restoreName();          // channel keeps its real name; panel is the ref
        resolveAndOpenPlaylist(page, sampler, model, slot, pageUrl);
        return;
    }
    // Cancel: drop the green link, restore the channel's real name.
    ch->restoreName();
}

// Sanitise a video title into a safe filename stem.
static QString safeFileStem(const QString &title) {
    QString s = title;
    s.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    s = s.trimmed();
    if (s.isEmpty()) s = "youtube_audio";
    return s.left(120);
}

void pushSoundsToGrid(MainPage *page, ConfigModel *model);   // fwd (defined below)

// Bind button `idx` to a WHOLE-PLAYLIST cell (isStreamUrl + isPlaylist). Clicking
// it later opens the entire playlist. The playlist name is resolved async for a
// nicer label; until then the cell shows "Playlist".
static void saveLinkAsPlaylistButton(ConfigModel *model, MainPage *page,
                                     int idx, const QString &url) {
    if (!model) return;
    SoundInfo s;
    if (auto *cur = model->getSoundInfo(idx)) s = *cur;
    s.filename    = url;
    s.isStreamUrl = true;
    s.isPlaylist  = true;
    s.streamTitle = QObject::tr("Playlist");
    if (s.customText.isEmpty()) s.customText = QObject::tr("Playlist");
    model->setSoundInfo(idx, s);
    pushSoundsToGrid(page, model);
    StreamResolver &R = StreamResolver::instance();
    QObject *ctx = new QObject(page);
    QObject::connect(&R, &StreamResolver::playlistResolved, ctx,
        [model, page, idx, url, ctx](const QString &u, const QString &title,
                                     const QVector<PlaylistEntry> &){
            if (u != url) return;
            if (auto *cur = model->getSoundInfo(idx)) {
                if (cur->filename == url && cur->isPlaylist) {
                    SoundInfo up = *cur;
                    if (!title.isEmpty()) { up.streamTitle = title; up.customText = title; }
                    model->setSoundInfo(idx, up);
                    pushSoundsToGrid(page, model);
                }
            }
            ctx->deleteLater();
        });
    QObject::connect(&R, &StreamResolver::failed, ctx,
        [ctx](const QString &, const QString &){ ctx->deleteLater(); });
    R.resolvePlaylist(url);
}

// Ask "whole playlist vs just this video" for a playlist URL being saved to a
// button. Returns 1 = whole, 0 = single, -1 = cancel.
static int askPlaylistSaveChoice(MainPage *page) {
    QMessageBox box(page);
    box.setWindowTitle(QObject::tr("Playlist link"));
    box.setText(QObject::tr("This link is a playlist. Save the whole playlist to the button, or just this video?"));
    QPushButton *whole  = box.addButton(QObject::tr("Whole playlist"), QMessageBox::AcceptRole);
    QPushButton *single = box.addButton(QObject::tr("Just this video"), QMessageBox::YesRole);
    box.addButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() == whole)  return 1;
    if (box.clickedButton() == single) return 0;
    return -1;
}

// Download the audio of pageUrl to destFile with the themed, custom-painted
// progress card (the same one the DSP export uses — a plain QProgressDialog
// rendered broken on the native Windows style). onOk is invoked with destFile
// on success (e.g. to bind a button to the new file).
static void runStreamDownload(MainPage *page, const QString &pageUrl,
                              const QString &destFile,
                              std::function<void(const QString &)> onOk)
{
    auto *prog = new ExportProgressDialog(destFile, page);
    prog->setAttribute(Qt::WA_DeleteOnClose);
    prog->show();
    prog->raise();

    StreamResolver &R = StreamResolver::instance();
    QObject *ctx = new QObject(prog);   // dies with the dialog

    QObject::connect(&R, &StreamResolver::downloadProgress, ctx,
        [prog](const QString &, int pct){ if (pct >= 0) prog->setProgress(pct); });
    QObject::connect(prog, &ExportProgressDialog::cancelRequested, ctx, []{
        StreamResolver::instance().cancelDownload();
    });
    QObject::connect(&R, &StreamResolver::downloadFinished, ctx,
        [prog, onOk](bool ok, const QString &msg, const QString &dest){
            prog->setFinished(ok, ok ? QString() : msg);
            if (ok && onOk) onOk(dest);
        });
    R.downloadAudio(pageUrl, destFile);
}

// "Save audio" flow for a STREAM slot: download the source audio at full
// quality via the engine, then optionally bind the saved file to a grid
// button. This is a separate action from "Export audio" (the DSP bake of a
// local file), which does not apply to streams — the channel shows a
// dedicated green download button instead of repurposing the export one.
static void startStreamDownloadFlow(MainPage *page, int slot)
{
    if (!page || !s_slotStreamUrl.contains(slot)) return;
    if (s_slotStreamLive.contains(slot)) {
        QMessageBox::information(page, QObject::tr("Save audio"),
            QObject::tr("This is a live stream — it can only be saved as a link, not to a file."));
        return;
    }
    const QString pageUrl = s_slotStreamUrl.value(slot);
    const QString title   = s_slotStreamTitle.value(slot, QObject::tr("youtube_audio"));
    QString dir = QFileDialog::getExistingDirectory(page,
        QObject::tr("Choose destination folder"));
    if (dir.isEmpty()) return;
    const QString dest = QDir(dir).filePath(safeFileStem(title) + ".m4a");
    runStreamDownload(page, pageUrl, dest, [page, title](const QString &file){
        // Offer to also bind the saved file to a soundboard button.
        if (QMessageBox::question(page, QObject::tr("Save to a button"),
                QObject::tr("Audio saved.\n\nAlso assign it to a soundboard button? "
                            "Click Yes, then click the cell where you want it."))
            == QMessageBox::Yes) {
            s_pendingAssignFile  = file;
            s_pendingAssignTitle = title;
            showInfoToast(page, QObject::tr("Click a soundboard cell to save the audio there."));
        }
    });
}

// "Export audio" (true DSP bake) for a STREAM slot. The AudioExporter needs a
// local file — and the direct CDN URL may demand rotating headers — so the
// flow is: engine-download the source audio into the contained scratch dir,
// then run the NORMAL AudioExporter over that temp copy with the channel's
// live fx + sandbox state (snapshotted at click), then delete the temp. Two
// visible phases: download progress, then the usual encode progress card.
static void startStreamExportFlow(MainPage *page, ConfigModel *model, int slot)
{
    if (!page || !model || !s_slotStreamUrl.contains(slot)) return;
    if (s_slotStreamLive.contains(slot)) {
        QMessageBox::information(page, QObject::tr("Export audio"),
            QObject::tr("This is a live stream — it has no end, so it cannot be exported to a file."));
        return;
    }
    auto *src_ch = page->channelAt(slot);
    if (!src_ch) return;

    // Destination + format (same dialog as the local-file export).
    QString selectedFilter;
    const QString filters =
        QObject::tr("WAV (PCM 16-bit) (*.wav);;FLAC (lossless) (*.flac);;"
                    "OGG Vorbis (*.ogg);;AAC / M4A (*.m4a);;All files (*.*)");
    QString dst = QFileDialog::getSaveFileName(page,
        QObject::tr("Export audio with DSP"),
        QString(), filters, &selectedFilter);
    if (dst.isEmpty()) return;
    auto endsWithI = [&](const QString &s, const char *ext) {
        return s.endsWith(QString::fromLatin1(ext), Qt::CaseInsensitive);
    };
    if (!(endsWithI(dst, ".wav") || endsWithI(dst, ".flac") ||
          endsWithI(dst, ".ogg") || endsWithI(dst, ".oga") ||
          endsWithI(dst, ".m4a") || endsWithI(dst, ".mp4") ||
          endsWithI(dst, ".aac")))
    {
        if      (selectedFilter.contains(".flac")) dst += QStringLiteral(".flac");
        else if (selectedFilter.contains(".ogg"))  dst += QStringLiteral(".ogg");
        else if (selectedFilter.contains(".m4a"))  dst += QStringLiteral(".m4a");
        else                                       dst += QStringLiteral(".wav");
    }

    // Snapshot the channel's LIVE settings NOW (click time), not when the
    // download lands — matches the local export's semantics.
    const float pitchFactor = AudioUtils::sliderToPitchFactor(src_ch->fx()->pitch());
    const float speedFactor = AudioUtils::sliderToPitchFactor(src_ch->fx()->speed());
    const float reverbMix   = src_ch->fx()->reverb() / 100.0f;
    const bool  sandboxOn   = model->getAudioSandboxEnabled() && src_ch->sandboxState().enabled;
    const SandboxState sbState = src_ch->sandboxState();

    // Phase 1: download the source audio into the scratch dir (auto-wiped at
    // start/shutdown, so a failed run never leaves junk on disk).
    const QString pageUrl = s_slotStreamUrl.value(slot);
    const QString title   = s_slotStreamTitle.value(slot, QObject::tr("youtube_audio"));
    const QString tmp     = StreamResolver::workDir() + "/"
                          + safeFileStem(title) + "_export.m4a";
    runStreamDownload(page, pageUrl, tmp,
        [page, dst, pitchFactor, speedFactor, reverbMix, sbState, sandboxOn](const QString &file){
        // Phase 2: the normal DSP bake over the temp copy.
        auto *exporter = new AudioExporter(file, dst,
                                           pitchFactor, speedFactor, reverbMix,
                                           sbState, sandboxOn,
                                           48000.0, nullptr);
        auto *progress = new ExportProgressDialog(dst, page);
        progress->setAttribute(Qt::WA_DeleteOnClose);
        QObject::connect(exporter, &AudioExporter::progress,
                         progress, &ExportProgressDialog::setProgress,
                         Qt::QueuedConnection);
        QObject::connect(exporter, &AudioExporter::exportFinished, progress,
                         [progress](bool ok, const QString &err){
            progress->setFinished(ok, err);
        }, Qt::QueuedConnection);
        // Temp source is deleted whatever the outcome.
        QObject::connect(exporter, &AudioExporter::exportFinished, exporter,
                         [file](bool, const QString &){ QFile::remove(file); },
                         Qt::QueuedConnection);
        QObject::connect(progress, &ExportProgressDialog::cancelRequested,
                         exporter, [exporter]{ exporter->requestInterruption(); });
        QObject::connect(exporter, &QThread::finished, exporter, &QObject::deleteLater);
        QObject::connect(progress, &QDialog::rejected, exporter,
                         [exporter]{ exporter->requestInterruption(); });
        progress->show();
        progress->raise();
        progress->activateWindow();
        exporter->start();
    });
}

// Slots flagged for a HARD clear on the next onStopPlaying tick. Set
// by clearRequested handlers (the red X button next to the filename,
// also future drag-out paths) BEFORE they call sampler->stopPlayback,
// so the queued onStopPlaying handler knows it must wipe the channel
// instead of transitioning into the soft replay-ready state. Without
// this flag the cleanup we did synchronously in clearRequested was
// silently undone a few ms later when the Sampler signal arrived.
static QSet<int> s_pendingHardClear;

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
    // Mirror onto the main-page spinners too — settings + main page
    // both observe the same model, but profile-switch / import only
    // calls this function so the main-page widgets needed a hook.
    if (auto *rs = page->rowsSpin()) {
        QSignalBlocker b(rs);
        rs->setValue(model->getRows());
    }
    if (auto *cs = page->colsSpin()) {
        QSignalBlocker b(cs);
        cs->setValue(model->getCols());
    }
    w->setEarrapeProtection(model->getEarrapeProtection());
    w->setLinkVolumes(model->getLinkVolumes());
    w->setRememberPitchSpeed(model->getRememberPitchSpeed());
    w->setRestoreSession(model->getRestoreSession());
    w->setGlobalFxEnabled(model->getGlobalFxEnabled());
    w->setHideWaveform(model->getHideWaveform());
    w->setLogsEnabled(model->getLogsEnabled());
    w->setExtremeLogging(model->getExtremeLogging());
    w->setRightDragLoopEnabled(model->getRightDragLoopEnabled());
    w->setReplayModeEnabled(model->getReplayModeEnabled());
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
    w->setAdaptWaveformToFx(model->getAdaptWaveformToFx());
    w->setShowCropMarkers(model->getShowCropMarkers());
    w->setMultiChannelInfinity(model->getMultiChannelInfinity());
    w->setShowPauseAllButton  (model->getShowPauseAllButton());
    w->setShowStopAllButton   (model->getShowStopAllButton());
    w->setShowAddChannelButton(model->getShowAddChannelButton());
    w->setShowMuteChecks      (model->getShowMuteChecks());
    w->setShowProfileButtons  (model->getShowProfileButtons());
    w->setShowGridSizeSelectors(model->getShowGridSizeSelectors());
    w->setVerticalMeter       (model->getVerticalMeter());
    w->setShowSkipButtons     (model->getShowSkipButtons());
    w->setSpectrogramView     (model->getSpectrogramView());
    w->setShowVinylButton     (model->getShowVinylButton());
    w->setMicFxFeatureEnabled (model->getMicFxFeatureEnabled());
    w->setLoudnessNormalize   (model->getLoudnessNormalize());
    w->setStreamingEnabled    (model->getStreamingEnabled());
    w->setChannelNameLinkDetect(model->getChannelNameLinkDetect());
    w->setStreamAutoplay      (model->getStreamAutoplay());
    w->setStreamQuality       (StreamResolver::preferredQuality());
    w->setStreamFxGradient    (model->getStreamFxGradient());
    w->setWaveAnimStyle       (QColor(model->getWaveAnimColorA()),
                               QColor(model->getWaveAnimColorB()),
                               model->getWaveAnimSpeed(),
                               model->getWaveAnimIntensity());
    w->setFormatBadgeMode     (model->getFormatBadgeMode());
    w->setShowStreamBadge     (model->getShowStreamBadge());
    w->setVadWhilePlaying     (model->getVadWhilePlaying());
    w->setDuckWhenTalking     (model->getDuckWhenTalking());
    w->setDuckAmount          (model->getDuckAmountPercent());
    // Push the persisted voice behaviour into the audio path at load.
    sb_setVoiceBehaviour(model->getVadWhilePlaying(), model->getDuckWhenTalking(),
                         model->getDuckAmountPercent() / 100.0f);
    // Sandbox module kill switch: restore the persisted mask into the
    // static SlotDsp mask (audio side) + the Settings checkboxes.
    {
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        quint32 mask = st.value(QStringLiteral("sandbox_modules/mask"),
                                0xFFFFFFFFu).toUInt();
        // VERSIONED mask: a mask saved before newer DspStages were appended
        // has ZERO bits for them, which silently hard-disabled every new
        // module (VoiceFx/autotune, Gate, DynEq, ...) no matter what the
        // sandbox UI said — THE "the new effects do nothing" bug. Bits for
        // stages that did not exist when the mask was written default to ON.
        // mask_stages records the Stage_COUNT at write time; legacy masks
        // (no key) are assumed to predate the first append wave (14 stages).
        int maskStages = st.value(QStringLiteral("sandbox_modules/mask_stages"),
                                  14).toInt();
        if (maskStages < 1) maskStages = 14;
        for (int stg = maskStages; stg < SandboxState::Stage_COUNT; ++stg)
            mask |= (1u << stg);
        mask |= 1u;   // Paulstretch's pipeline slot is structural - keep it on
        for (int stg = 0; stg < SandboxState::Stage_COUNT; ++stg)
            SlotDsp::setGlobalStageEnabled(stg, (mask >> stg) & 1u);
        w->setSandboxModuleMask(SlotDsp::globalStageMask());
    }
    // Push initial toolbar / channel visibility so the page reflects
    // saved settings right after wiring (no need for user to re-toggle).
    if (page->pauseAllBtn())   page->pauseAllBtn()->setVisible(model->getShowPauseAllButton());
    if (page->stopAllBtn())    page->stopAllBtn ()->setVisible(model->getShowStopAllButton());
    if (page->addChannelBtn()) page->addChannelBtn()->setVisible(
        model->getShowAddChannelButton() && !model->getMultiChannelInfinity());
    page->setMuteChecksVisible(model->getShowMuteChecks());
    page->setProfileButtonsVisible(model->getShowProfileButtons());
    page->setGridSizeVisible(model->getShowGridSizeSelectors());
    page->setMicFxFeatureVisible(model->getMicFxFeatureEnabled());
    MicFx::instance().setFeatureEnabled(model->getMicFxFeatureEnabled());
    if (sb_getSampler())
        sb_getSampler()->setGlobalNormalize(model->getLoudnessNormalize());
    for (auto *ch : page->channels()) {
        ch->setMeterVertical(model->getVerticalMeter());
        ch->setSkipButtonsVisible(model->getShowSkipButtons());
        ch->waveform()->setSpectrogramView(model->getSpectrogramView());
        ch->setVinylButtonVisible(model->getShowVinylButton());
        ch->waveform()->setStreamGradientEnabled(model->getStreamFxGradient());
        ch->waveform()->setStreamGradientStyle(QColor(model->getWaveAnimColorA()),
                                               QColor(model->getWaveAnimColorB()),
                                               model->getWaveAnimSpeed(),
                                               model->getWaveAnimIntensity());
        ch->waveform()->setFormatBadgeMode(model->getFormatBadgeMode());
        ch->waveform()->setStreamBadgeEnabled(model->getShowStreamBadge());
    }
    w->setResetChVolume(model->getResetChVolume());
    w->setResetChFx(model->getResetChFx());
    w->setResetChFile(model->getResetChFile());
    w->setResetChSandbox(model->getResetChSandbox());
    w->setResetAllRemoveExtra(model->getResetAllRemoveExtra());
    w->setResetAllVolume(model->getResetAllVolume());
    w->setResetAllFx(model->getResetAllFx());
    w->setResetAllFiles(model->getResetAllFiles());
    w->setResetAllSandbox(model->getResetAllSandbox());
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
    QObject::connect(w, &SettingsWindow::extremeLoggingChanged, [model](bool v){
        model->setExtremeLogging(v);
    });
    QObject::connect(w, &SettingsWindow::rightDragLoopEnabledChanged, [model](bool v){
        model->setRightDragLoopEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::replayModeEnabledChanged, [model](bool v){
        model->setReplayModeEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::showLogViewerRequested, []{
        sb_openLogViewer();
    });
    QObject::connect(w, &SettingsWindow::copySandboxDebugRequested, [page]{
        QJsonArray arr;
        for (int i = 0; i < page->channels().size(); ++i) {
            QJsonObject ch;
            ch["channelId"] = i;
            ch["title"]     = page->channels().at(i)->title();
            ch["sandbox"]   = page->channels().at(i)->sandboxState().toJson();
            arr.append(ch);
        }
        QJsonObject root;
        root["channels"] = arr;
        QApplication::clipboard()->setText(
            QString::fromUtf8(QJsonDocument(root)
                .toJson(QJsonDocument::Indented)));
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
        // Sync the P1..P4 buttons on the main panel so the change in
        // the settings combo is reflected everywhere. QSignalBlocker
        // prevents the button-click handler from re-firing the same
        // profile switch.
        for (int i = 0; i < 4; ++i) {
            auto *btn = page->profileButton(i);
            if (btn) {
                QSignalBlocker b(btn);
                btn->setChecked(i == idx);
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
            // Rich-text badges are not QSS-styled, so refreshTheme() cannot
            // retint them: re-render the file label against the new accent.
            ch->waveform()->setFormatBadgeMode(model->getFormatBadgeMode());
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
            // Rich-text badges are not QSS-styled, so refreshTheme() cannot
            // retint them: re-render the file label against the new accent.
            ch->waveform()->setFormatBadgeMode(model->getFormatBadgeMode());
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
            // Rich-text badges are not QSS-styled, so refreshTheme() cannot
            // retint them: re-render the file label against the new accent.
            ch->waveform()->setFormatBadgeMode(model->getFormatBadgeMode());
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

    QObject::connect(w, &SettingsWindow::adaptWaveformToFxChanged, [model, page](bool v){
        model->setAdaptWaveformToFx(v);
        for (auto *ch : page->channels()) ch->waveform()->setAdaptToFx(v);
    });
    QObject::connect(w, &SettingsWindow::showCropMarkersChanged, [model, page](bool v){
        model->setShowCropMarkers(v);
        for (auto *ch : page->channels()) ch->waveform()->setShowCropMarkers(v);
    });
    // Animated played-waveform gradient (all playback, theme-aware).
    QObject::connect(w, &SettingsWindow::streamFxGradientChanged, [model, page](bool v){
        model->setStreamFxGradient(v);
        for (auto *ch : page->channels()) ch->waveform()->setStreamGradientEnabled(v);
    });
    // Waveform animation style (colors / speed / intensity). Invalid
    // colors persist as empty strings = auto (follow the theme).
    QObject::connect(w, &SettingsWindow::waveAnimStyleChanged,
                     [model, page](const QColor &a, const QColor &b, int speed, int intensity){
        model->setWaveAnimColorA(a.isValid() ? a.name() : QString());
        model->setWaveAnimColorB(b.isValid() ? b.name() : QString());
        model->setWaveAnimSpeed(speed);
        model->setWaveAnimIntensity(intensity);
        for (auto *ch : page->channels())
            ch->waveform()->setStreamGradientStyle(a, b, speed, intensity);
    });
    // Format / quality badge before a local file's name (theme-aware).
    QObject::connect(w, &SettingsWindow::formatBadgeModeChanged, [model, page](int mode){
        model->setFormatBadgeMode(mode);
        for (auto *ch : page->channels()) ch->waveform()->setFormatBadgeMode(mode);
    });
    // WEB / LIVE pill on stream titles.
    QObject::connect(w, &SettingsWindow::showStreamBadgeChanged, [model, page](bool v){
        model->setShowStreamBadge(v);
        for (auto *ch : page->channels()) ch->waveform()->setStreamBadgeEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::multiChannelInfinityChanged, [model, page](bool v){
        model->setMultiChannelInfinity(v);
        // "+ Add channel" disappears when infinity mode is on - a
        // manual add is redundant, temp channels spawn automatically.
        // (Also gated on its own toolbar-visibility setting.)
        if (page->addChannelBtn()) page->addChannelBtn()->setVisible(
            model->getShowAddChannelButton() && !v);
    });
    // Main-toolbar group visibility.
    QObject::connect(w, &SettingsWindow::showAddChannelButtonChanged, [model, page](bool v){
        model->setShowAddChannelButton(v);
        if (page->addChannelBtn()) page->addChannelBtn()->setVisible(
            v && !model->getMultiChannelInfinity());
    });
    QObject::connect(w, &SettingsWindow::showMuteChecksChanged, [model, page](bool v){
        model->setShowMuteChecks(v);
        page->setMuteChecksVisible(v);
    });
    QObject::connect(w, &SettingsWindow::showProfileButtonsChanged, [model, page](bool v){
        model->setShowProfileButtons(v);
        page->setProfileButtonsVisible(v);
    });
    QObject::connect(w, &SettingsWindow::showGridSizeSelectorsChanged, [model, page](bool v){
        model->setShowGridSizeSelectors(v);
        page->setGridSizeVisible(v);
    });
    QObject::connect(w, &SettingsWindow::showPauseAllButtonChanged, [model, page](bool v){
        model->setShowPauseAllButton(v);
        if (page->pauseAllBtn()) page->pauseAllBtn()->setVisible(v);
    });
    QObject::connect(w, &SettingsWindow::showStopAllButtonChanged, [model, page](bool v){
        model->setShowStopAllButton(v);
        if (page->stopAllBtn()) page->stopAllBtn()->setVisible(v);
    });
    QObject::connect(w, &SettingsWindow::verticalMeterChanged, [model, page](bool v){
        model->setVerticalMeter(v);
        for (auto *ch : page->channels()) ch->setMeterVertical(v);
    });
    QObject::connect(w, &SettingsWindow::showSkipButtonsChanged, [model, page](bool v){
        model->setShowSkipButtons(v);
        for (auto *ch : page->channels()) ch->setSkipButtonsVisible(v);
    });
    QObject::connect(w, &SettingsWindow::spectrogramViewChanged, [model, page](bool v){
        model->setSpectrogramView(v);
        for (auto *ch : page->channels()) ch->waveform()->setSpectrogramView(v);
    });
    QObject::connect(w, &SettingsWindow::showVinylButtonChanged, [model, page](bool v){
        model->setShowVinylButton(v);
        for (auto *ch : page->channels()) ch->setVinylButtonVisible(v);
    });
    QObject::connect(w, &SettingsWindow::micFxFeatureEnabledChanged, [model, page](bool v){
        model->setMicFxFeatureEnabled(v);
        // Feature OFF = every related surface disappears AND processing
        // stops (MicFx forces its master toggle off).
        page->setMicFxFeatureVisible(v);
        MicFx::instance().setFeatureEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::loudnessNormalizeChanged, [model](bool v){
        model->setLoudnessNormalize(v);
        if (Sampler *smp = sb_getSampler()) smp->setGlobalNormalize(v);
    });
    QObject::connect(w, &SettingsWindow::streamingEnabledChanged, [model](bool v){
        model->setStreamingEnabled(v);
    });
    QObject::connect(w, &SettingsWindow::channelNameLinkDetectChanged, [model](bool v){
        model->setChannelNameLinkDetect(v);
    });
    QObject::connect(w, &SettingsWindow::streamAutoplayChanged, [model](bool v){
        model->setStreamAutoplay(v);
    });
    QObject::connect(w, &SettingsWindow::streamQualityChanged, [](const QString &q){
        StreamResolver::setPreferredQuality(q);
    });
    QObject::connect(w, &SettingsWindow::vadWhilePlayingChanged, [model](bool v){
        model->setVadWhilePlaying(v);
        sb_setVoiceBehaviour(v, model->getDuckWhenTalking(), model->getDuckAmountPercent() / 100.0f);
    });
    QObject::connect(w, &SettingsWindow::duckWhenTalkingChanged, [model](bool v){
        model->setDuckWhenTalking(v);
        sb_setVoiceBehaviour(model->getVadWhilePlaying(), v, model->getDuckAmountPercent() / 100.0f);
    });
    QObject::connect(w, &SettingsWindow::duckAmountChanged, [model](int v){
        model->setDuckAmountPercent(v);
        sb_setVoiceBehaviour(model->getVadWhilePlaying(), model->getDuckWhenTalking(), v / 100.0f);
    });
    QObject::connect(w, &SettingsWindow::sandboxModuleToggled, [](int stage, bool on){
        // Paulstretch's slot is structural (pinned pipeline index 0);
        // it can be disabled like the rest but never breaks anything.
        SlotDsp::setGlobalStageEnabled(stage, on);
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        st.setValue(QStringLiteral("sandbox_modules/mask"),
                    SlotDsp::globalStageMask());
        // Stamp the stage count so a future stage append can tell which
        // bits this mask actually covers (see the versioned read at init).
        st.setValue(QStringLiteral("sandbox_modules/mask_stages"),
                    (int)SandboxState::Stage_COUNT);
        // Open sandbox dialogs pick the change up on their next show
        // (ChannelSandboxDialog::showEvent -> refreshModuleVisibility);
        // the audio-side bypass is instant via the static mask.
    });
    QObject::connect(w, &SettingsWindow::resetChVolumeChanged, [model](bool v){ model->setResetChVolume(v); });
    QObject::connect(w, &SettingsWindow::resetChFxChanged, [model](bool v){ model->setResetChFx(v); });
    QObject::connect(w, &SettingsWindow::resetChFileChanged, [model](bool v){ model->setResetChFile(v); });
    QObject::connect(w, &SettingsWindow::resetChSandboxChanged, [model](bool v){ model->setResetChSandbox(v); });
    QObject::connect(w, &SettingsWindow::resetAllRemoveExtraChanged, [model](bool v){ model->setResetAllRemoveExtra(v); });
    QObject::connect(w, &SettingsWindow::resetAllVolumeChanged, [model](bool v){ model->setResetAllVolume(v); });
    QObject::connect(w, &SettingsWindow::resetAllFxChanged, [model](bool v){ model->setResetAllFx(v); });
    QObject::connect(w, &SettingsWindow::resetAllFilesChanged, [model](bool v){ model->setResetAllFiles(v); });
    QObject::connect(w, &SettingsWindow::resetAllSandboxChanged, [model](bool v){ model->setResetAllSandbox(v); });
}

void connectGrid(MainPage *page, ConfigModel *model, Sampler *sampler) {
    auto *grid = page->buttonGrid();

    QObject::connect(grid, &ButtonGrid::buttonTriggered, [model, sampler, page](int idx){
        // "Assign a just-downloaded audio to a button" mode: the next cell
        // click BINDS the file instead of playing anything.
        if (!s_pendingAssignFile.isEmpty()) {
            SoundInfo s;
            if (auto *cur = model->getSoundInfo(idx)) s = *cur;
            s.filename    = s_pendingAssignFile;
            s.isStreamUrl = false;
            s.streamTitle.clear();
            s.customText  = s_pendingAssignTitle;
            model->setSoundInfo(idx, s);
            pushSoundsToGrid(page, model);
            showInfoToast(page, QObject::tr("Saved to the button."), 3000);
            s_pendingAssignFile.clear();
            s_pendingAssignTitle.clear();
            return;
        }

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

        // Slot pick.
        //
        // Multi-channel infinity mode: if slot 0 is silent, play there
        // (that is the "default" channel every subsequent click bounces
        // off of). Otherwise spawn a fresh TEMPORARY channel and play in
        // it. Auto-created channels get an "infinityAuto" property so
        // onStopPlaying can remove them once playback ends (unless the
        // user has loop / reverse on, in which case removal is deferred
        // until an explicit stop button click).
        //
        // Standard mode: silent channel first, else round-robin oldest.
        const int n0 = page->channels().size();
        if (n0 <= 0) return;
        int slot = -1;
        const bool infinity = model && model->getMultiChannelInfinity();
        if (infinity) {
            if (sampler->getState(0) == Sampler::eSILENT) {
                slot = 0;
            } else {
                Channel *newCh = page->addChannel();
                slot = page->channels().size() - 1;
                if (newCh) {
                    newCh->setProperty("infinityAuto", true);
                    // Push global UI toggles onto the fresh widget so it
                    // matches the rest of the row (skip buttons, meter
                    // orientation, waveform visibility, etc.). Without
                    // this the newly-spawned channel would render with
                    // hardcoded defaults regardless of the user's saved
                    // settings.
                    newCh->setSkipButtonsVisible(model->getShowSkipButtons());
                    newCh->setMeterVertical(model->getVerticalMeter());
                }
            }
        } else {
            static int s_rr = 0;
            for (int i = 0; i < n0; ++i) {
                int s = i;
                if (sampler->getState(s) == Sampler::eSILENT) { slot = s; break; }
            }
            if (slot < 0) { slot = s_rr % n0; }
            s_rr = (slot + 1) % n0;
        }
        const int n = page->channels().size();
        (void)n;

        // Push the channel's current slider values into the sampler slot
        // so the slot starts with the visible levels (instead of stale
        // defaults that only refresh on slider tweak).
        auto syncChannelToSlot = [sampler, page](int s){
            if (!sampler || s < 0 || s >= page->channels().size()) return;
            auto *ch = page->channels().at(s);
            sampler->setSlotVolumeLocal (s, ch->volume()->local());
            sampler->setSlotVolumeRemote(s, ch->volume()->remote());
            sampler->setSlotPitchFactor (s, AudioUtils::sliderToPitchFactor(ch->fx()->pitch() ));
            sampler->setSlotSpeedFactor (s, AudioUtils::sliderToPitchFactor(ch->fx()->speed() ));
            sampler->setSlotReverbMix   (s, ch->fx()->reverb() / 100.0f);
        };

        if (info->isMacro && !info->macroState.isEmpty()) {
            // Mic FX macro: the payload is a JSON OBJECT tagged
            // type=micfx (channel macros are a JSON ARRAY). Applies the
            // frozen mic package - chain state + live pitch - and turns
            // Mic FX on. Firing it again with the same package already
            // active toggles the mic OFF (one button = on/off).
            {
                QJsonDocument mdoc = QJsonDocument::fromJson(info->macroState);
                if (mdoc.isObject()) {
                    QJsonObject mo = mdoc.object();
                    if (mo.value("type").toString() == QLatin1String("micfx")) {
                        MicFx &mic = MicFx::instance();
                        if (!mic.featureEnabled()) return;
                        SandboxState st = SandboxState::fromJson(
                            mo.value("state").toObject());
                        float pitch = static_cast<float>(
                            mo.value("pitch").toDouble(0.0));
                        // Toggle-off path: same package, mic already on.
                        QJsonDocument cur(mic.sandboxState().toJson());
                        bool samePkg = mic.enabled() &&
                            qAbs(mic.pitchSemitones() - pitch) < 0.01f &&
                            cur.object() == mo.value("state").toObject();
                        if (samePkg) {
                            mic.setEnabled(false);
                        } else {
                            mic.setSandboxState(st);
                            mic.setPitchSemitones(pitch);
                            mic.setEnabled(true);
                        }
                        return;
                    }
                }
            }
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
                    // Stream channel saved in the macro: st.filename is the
                    // long-expired direct CDN URL. Re-resolve the PAGE url
                    // through the engine instead and resume at the saved
                    // position once playback actually starts.
                    const QString mStreamUrl   = entry.value("streamUrl").toString();
                    const QString mPlaylistUrl = entry.value("playlistUrl").toString();
                    if (!mPlaylistUrl.isEmpty())
                        resolveAndOpenPlaylist(page, sampler, model, i, mPlaylistUrl);
                    if (!mStreamUrl.isEmpty()) {
                        const double resumeAt = st.playbackPos;
                        if (sampler && resumeAt > 1.0) {
                            auto *once = new QObject(page);
                            QObject::connect(sampler, &Sampler::onStartPlaying, once,
                                [sampler, i, resumeAt, once](int startedSlot, bool preview, QString){
                                    if (preview || startedSlot != i) return;
                                    sampler->seek(resumeAt, i);
                                    once->deleteLater();
                                });
                            QTimer::singleShot(30000, once, &QObject::deleteLater);
                        }
                        loadStreamIntoSlot(page, sampler, model, i, mStreamUrl,
                                           entry.value("streamGreen").toBool(),
                                           /*autoPlay*/true,
                                           /*keepPlaylist*/!mPlaylistUrl.isEmpty());
                        continue;
                    }
                    if (!st.filename.isEmpty()) {
                        SoundInfo macroSound;
                        macroSound.filename = st.filename;
                        sampler->playSoundInSlot(i, macroSound, false);
                        // Slot fx must be pushed AFTER play - slot only has
                        // inputFile once playSoundInSlot returns.
                        sampler->setSlotVolumeLocal (i, st.volumeLocal);
                        sampler->setSlotVolumeRemote(i, st.volumeRemote);
                        sampler->setSlotPitchFactor (i, AudioUtils::sliderToPitchFactor(st.pitch));
                        sampler->setSlotSpeedFactor (i, AudioUtils::sliderToPitchFactor(st.speed));
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
        // push channel/per-button values AFTER play returns. Error
        // dialog comes from the onPlaybackError handler.
        //
        // Factored into a local so the normal path and the async stream-resolve
        // callback below run the IDENTICAL post-play FX setup.
        auto startPlayback = [model, sampler, page, syncChannelToSlot, idx](const SoundInfo &snd, int slot) {
            // An explicit button play into this slot = the user chose something
            // else, so any playlist loaded here is unloaded (panel never drives
            // startPlayback — it uses loadStreamIntoSlot with keepPlaylist).
            unloadPlaylistPanel(page, slot);
            // Loading a NON-stream sound into a slot that was streaming restores
            // the channel's real name + unlocks reverse/vinyl (synchronous, so
            // no flicker). Stream loads keep their state (set by the caller).
            if (!snd.isStreamUrl) clearSlotStream(page, slot);
            s_slotToBtnIdx[slot] = idx;
            auto *ch = page->channels().at(slot);
            const bool globalFx = model->getGlobalFxEnabled();
            // Compute the FX factors to apply after open — on the GUI thread so
            // the widget reads are safe even when the open runs on a worker.
            float pf = 1.0f, sf = 1.0f, rv = 0.0f;
            bool  applyFx = false;
            if (globalFx && snd.fxRemember) {
                pf = AudioUtils::sliderToPitchFactor(snd.fxPitch);
                sf = AudioUtils::sliderToPitchFactor(snd.fxSpeed);
                rv = snd.fxReverb / 100.0f;
                applyFx = true;
                ch->fx()->setPitch(snd.fxPitch);
                ch->fx()->setSpeed(snd.fxSpeed);
                ch->fx()->setReverb(snd.fxReverb);
                ch->fx()->setSync (snd.fxSyncPitchSpeed);
            } else if (globalFx) {
                pf = AudioUtils::sliderToPitchFactor(ch->fx()->pitch());
                sf = AudioUtils::sliderToPitchFactor(ch->fx()->speed());
                rv = ch->fx()->reverb() / 100.0f;
                applyFx = true;
            }
            // Network stream: open OFF the GUI thread (no freeze). A button
            // click plays immediately (autoPlay), unlike the channel-name paste
            // which loads paused.
            if (snd.isStreamUrl) {
                sampler->playSoundInSlotAsync(slot, snd,
                    ch->volume()->local(), ch->volume()->remote(),
                    pf, sf, rv, applyFx, /*autoPlay*/true);
                return;
            }
            if (!sampler->playSoundInSlot(slot, snd, false))
                return;
            sampler->setSlotVolumeLocal (slot, ch->volume()->local());
            sampler->setSlotVolumeRemote(slot, ch->volume()->remote());
            if (applyFx) {
                sampler->setSlotPitchFactor(slot, pf);
                sampler->setSlotSpeedFactor(slot, sf);
                sampler->setSlotReverbMix  (slot, rv);
            } else {
                // Master FX off: force neutral so any inherited slot state
                // from playSoundInSlot is wiped.
                sampler->setSlotPitchFactor(slot, 1.0f);
                sampler->setSlotSpeedFactor(slot, 1.0f);
                sampler->setSlotReverbMix  (slot, 0.0f);
            }
            (void)syncChannelToSlot;
        };

        if (info->isStreamUrl && info->isPlaylist) {
            // Whole-playlist cell: open the playlist into the picked channel
            // (panel + click-to-load + autoplay chaining) instead of playing a
            // single video. Nothing is played until the user picks a track.
            if (!model || !model->getStreamingEnabled()) return;
            resolveAndOpenPlaylist(page, sampler, model, slot, info->filename);
            return;
        }

        if (info->isStreamUrl) {
            // Master streaming switch OFF -> a saved-link cell is inert.
            if (!model || !model->getStreamingEnabled()) return;
            // Live URL / YouTube cell: resolve the canonical page URL to a fresh
            // direct CDN URL (async, off the GUI thread), then play. A fresh
            // cache hit returns on the next event-loop turn, so the flow is
            // uniform. FFmpeg streams the result - nothing is downloaded whole.
            const QString pageUrl = info->filename;
            const SoundInfo base  = *info;  // carry FX / crop / volume forward
            StreamResolver &R = StreamResolver::instance();
            QObject *ctx = beginSlotResolve(page, slot);  // marquee + abort prior
            s_slotPendingUrl[slot] = pageUrl;             // for the Cancel button
            QObject::connect(&R, &StreamResolver::resolved, ctx,
                [startPlayback, base, pageUrl, slot, ctx, page](const QString &u, const ResolvedStream &s) {
                    if (u != pageUrl) return;
                    endSlotResolve(page, slot, ctx);
                    SoundInfo play = base;
                    play.filename     = s.directUrl;
                    play.netUserAgent = s.userAgent;
                    play.netHeaders   = s.headers;
                    play.isLive       = s.isLive;
                    if (s.durationSec > 0.0) play.streamDurationSec = s.durationSec;
                    // Mark the slot as a live stream so onStartPlaying shows the
                    // video title in the file-label + locks reverse/vinyl. (A
                    // button-cell stream has no green channel name - that is only
                    // for the paste-link-as-channel-name entry.)
                    s_slotStreamLoading.insert(slot);
                    s_slotStreamTitle[slot] = s.title.isEmpty() ? pageUrl : s.title;
                    s_slotStreamUrl[slot]   = pageUrl;
                    if (s.isLive) s_slotStreamLive.insert(slot);
                    else          s_slotStreamLive.remove(slot);
                    startPlayback(play, slot);
                });
            QObject::connect(&R, &StreamResolver::failed, ctx,
                [page, pageUrl, slot, ctx](const QString &u, const QString &err) {
                    if (u != pageUrl) return;
                    endSlotResolve(page, slot, ctx);
                    if (slot < page->channels().size())
                        if (auto *ch = page->channels().at(slot))
                            showStreamErrorBubble(ch, QObject::tr("Couldn't load the link.\n%1").arg(err));
                });
            R.resolve(pageUrl);
            return;
        }

        startPlayback(*info, slot);
    });

    QObject::connect(grid, &ButtonGrid::buttonFileDropped, [model, page](int idx, const QList<QUrl> &urls){
        if (urls.isEmpty()) return;
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        const QUrl u = urls.first();
        const QString scheme = u.scheme().toLower();
        if ((scheme == "http" || scheme == "https") && model && model->getStreamingEnabled()) {
            // A playlist link -> ask whole-playlist vs single video.
            if (StreamResolver::looksLikePlaylist(u.toString())) {
                const int choice = askPlaylistSaveChoice(page);
                if (choice < 0) return;                 // cancel
                if (choice == 1) { saveLinkAsPlaylistButton(model, page, idx, u.toString()); return; }
                // choice == 0 -> fall through, save as a single-video stream cell.
            }
            // Dropped a web link -> live stream cell (v2.3.1). Store the
            // CANONICAL page URL; resolve-on-play fetches a fresh direct URL
            // each play (googlevideo URLs expire). Kick a resolve now so the
            // cell shows a real title before the user ever clicks it.
            s.filename    = u.toString();
            s.isStreamUrl = true;
            if (s.customText.isEmpty())
                s.customText = QObject::tr("Loading…");
            model->setSoundInfo(idx, s);
            pushSoundsToGrid(page, model);

            StreamResolver &R = StreamResolver::instance();
            const QString pageUrl = s.filename;
            QObject *ctx = new QObject(page);
            QObject::connect(&R, &StreamResolver::resolved, ctx,
                [model, page, idx, pageUrl, ctx](const QString &ru, const ResolvedStream &rs){
                    if (ru != pageUrl) return;
                    if (auto *cur = model->getSoundInfo(idx)) {
                        if (cur->filename == pageUrl && cur->isStreamUrl) {
                            SoundInfo up = *cur;
                            if (!rs.title.isEmpty()) { up.streamTitle = rs.title; up.customText = rs.title; }
                            up.streamDurationSec = rs.durationSec;
                            model->setSoundInfo(idx, up);
                            pushSoundsToGrid(page, model);
                        }
                    }
                    ctx->deleteLater();
                });
            QObject::connect(&R, &StreamResolver::failed, ctx,
                [ctx](const QString &, const QString &){ ctx->deleteLater(); });
            R.resolve(pageUrl);
            return;
        }
        s.filename    = u.toLocalFile();
        s.isStreamUrl = false;
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    // Right-click "Save link...": paste a media/YouTube link into a QInputDialog
    // and store it as a live-stream cell on this button. Clicking the button
    // then resolves + streams it (buttonTriggered's isStreamUrl path). The title
    // is resolved now so the cell shows the video name instead of the URL.
    QObject::connect(grid, &ButtonGrid::saveLinkRequested, [model, page](int idx){
        if (!model || !model->getStreamingEnabled()) {
            QMessageBox::information(page, QObject::tr("Save link"),
                QObject::tr("URL / YouTube streaming is disabled in Settings."));
            return;
        }
        bool ok = false;
        const QString link = QInputDialog::getText(page, QObject::tr("Save link"),
            QObject::tr("Paste a video / audio link (YouTube, etc.):"),
            QLineEdit::Normal, QString(), &ok).trimmed();
        if (!ok || link.isEmpty()) return;
        if (!StreamResolver::looksLikeUrl(link)) {
            QMessageBox::warning(page, QObject::tr("Save link"),
                QObject::tr("That does not look like a valid link."));
            return;
        }
        // A playlist link -> ask whole-playlist vs single video.
        if (StreamResolver::looksLikePlaylist(link)) {
            const int choice = askPlaylistSaveChoice(page);
            if (choice < 0) return;                 // cancel
            if (choice == 1) { saveLinkAsPlaylistButton(model, page, idx, link); return; }
            // choice == 0 -> fall through, save as a single-video stream cell.
        }
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        s.filename    = link;
        s.isStreamUrl = true;
        if (s.customText.isEmpty()) s.customText = QObject::tr("Loading...");
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);

        StreamResolver &R = StreamResolver::instance();
        const QString pageUrl = link;
        QObject *ctx = new QObject(page);
        QObject::connect(&R, &StreamResolver::resolved, ctx,
            [model, page, idx, pageUrl, ctx](const QString &ru, const ResolvedStream &rs){
                if (ru != pageUrl) return;
                if (auto *cur = model->getSoundInfo(idx)) {
                    if (cur->filename == pageUrl && cur->isStreamUrl) {
                        SoundInfo up = *cur;
                        if (!rs.title.isEmpty()) { up.streamTitle = rs.title; up.customText = rs.title; }
                        up.streamDurationSec = rs.durationSec;
                        model->setSoundInfo(idx, up);
                        pushSoundsToGrid(page, model);
                    }
                }
                ctx->deleteLater();
            });
        QObject::connect(&R, &StreamResolver::failed, ctx,
            [ctx](const QString &, const QString &){ ctx->deleteLater(); });
        R.resolve(pageUrl);
    });

    // Provider: which channels currently hold a resolved YouTube video (so the
    // cell right-click menu can offer save-link / download-audio per channel).
    grid->setStreamChannelsProvider([page]() -> QVector<StreamChannelInfo> {
        QVector<StreamChannelInfo> out;
        // Candidate slots = those holding a single video OR a whole playlist.
        // (NB: 'slots' is a Qt keyword/macro — must not be used as an identifier.)
        QSet<int> slotSet;
        for (auto it = s_slotStreamUrl.constBegin(); it != s_slotStreamUrl.constEnd(); ++it)
            slotSet.insert(it.key());
        for (auto it = s_slotPlaylistUrl.constBegin(); it != s_slotPlaylistUrl.constEnd(); ++it)
            slotSet.insert(it.key());
        for (int slot : slotSet) {
            StreamChannelInfo sc;
            sc.slot          = slot;
            sc.pageUrl       = s_slotStreamUrl.value(slot);   // may be empty (playlist only)
            sc.title         = s_slotStreamTitle.value(slot, sc.pageUrl);
            sc.isLive        = s_slotStreamLive.contains(slot);
            sc.isPlaylist    = s_slotPlaylistUrl.contains(slot);
            sc.playlistUrl   = s_slotPlaylistUrl.value(slot);
            sc.playlistTitle = s_slotPlaylistTitle.value(slot, QObject::tr("Playlist"));
            if (slot >= 0 && slot < page->channels().size())
                if (auto *c = page->channels().at(slot)) {
                    // A channel-name-paste stream shows the green URL as its
                    // name — use the video title as the label instead.
                    sc.channelName = c->showingStreamLink() ? sc.title : c->title();
                }
            out.push_back(sc);
        }
        return out;
    });

    // Right-click a cell -> "Save <channel>'s link here": bind this button to
    // the channel's page URL as a live-stream cell (works for live too).
    QObject::connect(grid, &ButtonGrid::saveStreamLinkToButton,
        [model, page](int idx, const QString &pageUrl, const QString &title){
            if (!model) return;
            SoundInfo s;
            if (auto *cur = model->getSoundInfo(idx)) s = *cur;
            s.filename    = pageUrl;
            s.isStreamUrl = true;
            s.streamTitle = title;
            s.customText  = title.isEmpty() ? QObject::tr("(link)") : title;
            model->setSoundInfo(idx, s);
            pushSoundsToGrid(page, model);
        });

    // Right-click a cell -> "Save <channel>'s whole playlist here": bind this
    // button to the PLAYLIST URL. Triggering it later opens the whole playlist.
    QObject::connect(grid, &ButtonGrid::savePlaylistToButton,
        [model, page](int idx, const QString &playlistUrl, const QString &title){
            if (!model || playlistUrl.isEmpty()) return;
            SoundInfo s;
            if (auto *cur = model->getSoundInfo(idx)) s = *cur;
            s.filename    = playlistUrl;
            s.isStreamUrl = true;
            s.isPlaylist  = true;
            s.streamTitle = title;
            s.customText  = title.isEmpty() ? QObject::tr("Playlist") : title;
            model->setSoundInfo(idx, s);
            pushSoundsToGrid(page, model);
            showInfoToast(page, QObject::tr("Playlist saved to the button."), 3000);
        });

    // Right-click a cell -> "Download <channel>'s audio here…": pick a folder,
    // download the audio, then bind this button to the resulting local file.
    QObject::connect(grid, &ButtonGrid::downloadStreamToButton,
        [model, page](int idx, const QString &pageUrl, const QString &title){
            if (!model || !model->getStreamingEnabled()) return;
            QString dir = QFileDialog::getExistingDirectory(page,
                QObject::tr("Choose destination folder"));
            if (dir.isEmpty()) return;
            const QString dest = QDir(dir).filePath(safeFileStem(title) + ".m4a");
            runStreamDownload(page, pageUrl, dest, [model, page, idx, title](const QString &file){
                SoundInfo s;
                if (auto *cur = model->getSoundInfo(idx)) s = *cur;
                s.filename    = file;
                s.isStreamUrl = false;
                s.streamTitle.clear();
                s.customText  = title;
                model->setSoundInfo(idx, s);
                pushSoundsToGrid(page, model);
            });
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
            // Live network stream: the state's filename is the DIRECT CDN
            // URL, which expires within hours and carries no HTTP headers —
            // replaying it as a plain file (the old behaviour) broke the
            // macro. Persist the PAGE url + title and re-resolve at fire.
            if (s_slotStreamUrl.contains(i)) {
                entry["streamUrl"]   = s_slotStreamUrl.value(i);
                entry["streamTitle"] = s_slotStreamTitle.value(i);
                entry["streamGreen"] = ch->showingStreamLink();
            }
            if (s_slotPlaylistUrl.contains(i))
                entry["playlistUrl"] = s_slotPlaylistUrl.value(i);
            arr.append(entry);
        }
        s.macroState = QJsonDocument(arr).toJson(QJsonDocument::Compact);
        if (s.customText.isEmpty())
            s.customText = QObject::tr("Macro %1").arg(idx + 1);
        model->setSoundInfo(idx, s);
        pushSoundsToGrid(page, model);
    });

    // Mic FX macro: freeze the CURRENT microphone package (full chain
    // state + live pitch) into the button. Triggering applies it and
    // turns Mic FX on; triggering again with the same package active
    // toggles the mic off.
    QObject::connect(grid, &ButtonGrid::createMicMacroRequested, [model, page](int idx){
        MicFx &mic = MicFx::instance();
        SoundInfo s;
        if (auto *cur = model->getSoundInfo(idx)) s = *cur;
        s.isMacro = true;
        s.filename.clear();
        QJsonObject mo;
        mo["type"]  = QStringLiteral("micfx");
        mo["state"] = mic.sandboxState().toJson();
        mo["pitch"] = static_cast<double>(mic.pitchSemitones());
        s.macroState = QJsonDocument(mo).toJson(QJsonDocument::Compact);
        if (s.customText.isEmpty())
            s.customText = QObject::tr("Mic FX");
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
            // Dragging a saved-link (stream) button onto a channel: resolve the
            // page URL to a fresh direct URL, then load (play->pause). Mirrors
            // the buttonTriggered stream path so effects/label/stream-mode all
            // apply. FFmpeg streams it live.
            if (info->isStreamUrl) {
                if (!model || !model->getStreamingEnabled()) return;
                const QString pageUrl = info->filename;
                const SoundInfo base  = *info;
                StreamResolver &R = StreamResolver::instance();
                QObject *ctx = new QObject(page);
                QObject::connect(&R, &StreamResolver::resolved, ctx,
                    [page, sampler, slot, btnIdx, base, pageUrl, ctx](const QString &u, const ResolvedStream &s){
                        if (u != pageUrl) return;
                        ctx->deleteLater();
                        if (slot >= page->channels().size()) return;
                        auto *tch = page->channels().at(slot);
                        if (!tch) return;
                        SoundInfo play = base;
                        play.filename          = s.directUrl;
                        play.netUserAgent      = s.userAgent;
                        play.netHeaders        = s.headers;
                        play.streamTitle       = s.title;
                        if (s.durationSec > 0.0) play.streamDurationSec = s.durationSec;
                        s_slotStreamLoading.insert(slot);
                        s_slotStreamTitle[slot] = s.title.isEmpty() ? pageUrl : s.title;
                        s_slotStreamUrl[slot]   = pageUrl;
                        if (sampler->playSoundInSlot(slot, play, false)) {
                            s_slotToBtnIdx[slot] = btnIdx;
                            sampler->setSlotVolumeLocal (slot, tch->volume()->local());
                            sampler->setSlotVolumeRemote(slot, tch->volume()->remote());
                            sampler->pausePlayback(slot);
                        } else {
                            clearSlotStream(page, slot);
                        }
                    });
                QObject::connect(&R, &StreamResolver::failed, ctx,
                    [page, slot, pageUrl, ctx](const QString &u, const QString &){
                        if (u != pageUrl) return;
                        ctx->deleteLater();
                        clearSlotStream(page, slot);
                    });
                R.resolve(pageUrl);
                return;
            }
            // Non-stream drop restores the channel name if the slot was a stream.
            clearSlotStream(page, slot);
            sampler->stopPlayback(slot);
            // Error dialog comes from onPlaybackError handler.
            if (!sampler->playSoundInSlot(slot, *info, false))
                return;
            s_slotToBtnIdx[slot] = btnIdx;
            auto *target = page->channels().at(slot);
            sampler->setSlotVolumeLocal (slot, target->volume()->local());
            sampler->setSlotVolumeRemote(slot, target->volume()->remote());
            const bool globalFx = model->getGlobalFxEnabled();
            if (globalFx && info->fxRemember) {
                sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(info->fxPitch ));
                sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(info->fxSpeed ));
                sampler->setSlotReverbMix  (slot, info->fxReverb / 100.0f);
                target->fx()->setPitch(info->fxPitch);
                target->fx()->setSpeed(info->fxSpeed);
                target->fx()->setReverb(info->fxReverb);
                target->fx()->setSync (info->fxSyncPitchSpeed);
            } else if (globalFx) {
                sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(target->fx()->pitch() ));
                sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(target->fx()->speed() ));
                sampler->setSlotReverbMix  (slot, target->fx()->reverb() / 100.0f);
            } else {
                sampler->setSlotPitchFactor(slot, 1.0f);
                sampler->setSlotSpeedFactor(slot, 1.0f);
                sampler->setSlotReverbMix  (slot, 0.0f);
            }
            sampler->pausePlayback(slot);
        });
        QObject::connect(ch, &Channel::titleChanged, page, [page, sampler, model](int id, const QString &t){
            int slot = -1;
            for (int i = 0; i < page->channels().size(); ++i)
                if (page->channels().at(i)->channelId() == id) { slot = i; break; }

            // Paste a media / YouTube link AS THE CHANNEL NAME: the instant it
            // looks like a URL (synchronous check) the channel name (= the link)
            // turns GREEN and the video loads as a live stream in THIS channel.
            // The video TITLE goes to the waveform file-label (onStartPlaying),
            // NOT the channel name - the name stays the green link until the
            // stream stops or another sound is loaded, then the real name is
            // restored. The link is never persisted, so a restart shows the name.
            if (sampler && slot >= 0 && model && model->getStreamingEnabled()
                && model->getChannelNameLinkDetect() && StreamResolver::looksLikeUrl(t)) {
                auto *ch2 = page->channels().at(slot);
                const QString pageUrl = t.trimmed();
                // Already loaded this exact link (focus-out with the green link
                // still shown and the slot already holds this URL) -> no reload.
                if (ch2 && ch2->showingStreamLink() && ch2->title() == pageUrl
                    && s_slotStreamUrl.value(slot) == pageUrl)
                    return;
                // Playlist link -> ask whole-playlist vs single video.
                if (StreamResolver::looksLikePlaylist(pageUrl)) {
                    openPlaylistFlow(page, sampler, model, slot, pageUrl);
                    return;
                }
                // Single video: resolve + load (seamlessly replacing whatever
                // was loading/playing here — loadStreamIntoSlot aborts the prior
                // resolve, shows the loading marquee, live-detects, error-toasts).
                // Auto-play is opt-in via the "Auto-play files loaded from a
                // link" setting; default loads paused/ready.
                loadStreamIntoSlot(page, sampler, model, slot, pageUrl,
                                   /*greenChannelName*/true,
                                   /*autoPlay*/model->getStreamAutoplay());
                return;
            }

            // A real (non-URL) name: abort any in-flight resolve for this slot
            // (user cleared/changed the link mid-load) and, if the channel was
            // showing a green stream link, exit that display; adopt the name.
            // Also unload any playlist loaded here — the user chose to rename.
            if (slot >= 0) {
                if (auto old = s_slotResolveCtx.value(slot)) { old->deleteLater(); s_slotResolveCtx.remove(slot); }
                if (auto *ch2 = page->channels().at(slot)) ch2->setStreamLoading(false);
                unloadPlaylistPanel(page, slot);
            }
            if (slot >= 0)
                if (auto *ch2 = page->channels().at(slot))
                    if (ch2->showingStreamLink()) ch2->restoreName();
            // Cleared to empty → fall back to the default "Channel N" name rather
            // than leaving a blank title.
            const QString finalName = t.trimmed().isEmpty()
                ? QObject::tr("Channel %1").arg(id + 1) : t;
            if (slot >= 0)
                if (auto *ch2 = page->channels().at(slot))
                    ch2->setTitle(finalName);
            ChannelStatePersistence::saveName(id, finalName);
        });
        QString savedName = ChannelStatePersistence::loadName(ch->channelId());
        if (!savedName.isEmpty()) ch->setTitle(savedName);
        ch->setFxVisible(model && model->getGlobalFxEnabled());
        ch->setWaveformVisible(!(model && model->getHideWaveform()));
        // Cancel (✕) on the resolving marquee: abort this channel's load.
        QObject::connect(ch, &Channel::streamLoadCancelRequested, page, [page](int id){
            for (int i = 0; i < page->channels().size(); ++i)
                if (page->channels().at(i)->channelId() == id) { cancelSlotStreamLoad(page, i); break; }
        });
        // ☰ reopen: re-show this channel's (hidden) playlist panel.
        QObject::connect(ch, &Channel::playlistReopenRequested, page, [page](int id){
            for (int i = 0; i < page->channels().size(); ++i)
                if (page->channels().at(i)->channelId() == id) {
                    if (auto p = s_slotPlaylistPanel.value(i)) { p->show(); p->raise(); p->activateWindow(); }
                    break;
                }
        });
        QObject::connect(ch, &Channel::removeChannelRequested, page, [page, sampler, model](int id){
            int idx = -1;
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i)->channelId() == id) { idx = i; break; }
            }
            if (idx < 0) return;
            const int oldCount = page->channels().size();
            // Sampler slots are POSITIONAL: removing a middle channel
            // shifts every following channel widget down by one, but
            // audio playing on slot j > idx would stay on slot j and
            // end up controlled by the wrong widget. Stop the removed
            // slot AND every slot above it, then re-push each shifted
            // channel's per-slot state to its new index below. (This
            // closes the long-standing "middle channel removal desyncs
            // audio routing" bug.)
            if (sampler) {
                for (int s = idx; s < oldCount; ++s)
                    sampler->stopPlayback(s);
            }
            // Crop-edit slot->button mappings for the stopped slots are
            // stale now (onStopPlaying also clears them, but only via a
            // queued connection - drop them synchronously before the
            // shift re-uses those indices). Same applies to the replay
            // context map: the slot indices about to be shifted must
            // not carry stale last-played records to a different widget.
            for (int s = idx; s < oldCount; ++s) {
                s_slotToBtnIdx.remove(s);
                s_lastPlayedCtx.remove(s);
            }

            page->removeChannel(idx);

            if (sampler) {
                const bool sandboxOn = model && model->getAudioSandboxEnabled();
                for (int s = idx; s < page->channels().size(); ++s) {
                    auto *c = page->channels().at(s);
                    if (sandboxOn && c->sandboxState().enabled)
                        sampler->setSlotSandboxState(s, c->sandboxState());
                    else
                        sampler->clearSlotSandbox(s);
                    sampler->setSlotLoop(s, c->waveform()->isLooping());
                    sampler->setSlotReverse(s, c->waveform()->isReversed());
                    sampler->setSlotPitchFactor(s,
                        AudioUtils::sliderToPitchFactor(c->fx()->pitch()));
                    sampler->setSlotSpeedFactor(s,
                        AudioUtils::sliderToPitchFactor(c->fx()->speed()));
                    sampler->setSlotReverbMix(s, c->fx()->reverb() / 100.0f);
                    ChannelState st = c->state();
                    sampler->setSlotVolumeLocal(s, st.volumeLocal);
                    sampler->setSlotVolumeRemote(s, st.volumeRemote);
                }
                // The previously-highest slot index is unused now - drop
                // its leftover per-slot state.
                const int freed = page->channels().size();
                sampler->clearSlotSandbox(freed);
                sampler->setSlotLoop(freed, false);
                sampler->setSlotReverse(freed, false);
            }
            // Channel 0 is the always-present primary.
            for (int i = 0; i < page->channels().size(); ++i) {
                page->channels().at(i)->setRemovable(i > 0);
            }
        });
        int idx = page->channels().indexOf(ch);
        ch->setRemovable(idx > 0);

        // Waveform right-click crop editor: persist edit back to the
        // originating cell via the slot->btn map, then live-update the
        // active playback so the marker + decoder bound + loop agree.
        //
        // The function is fully EXPLICIT — callers compute the desired
        // (newStart, newEnd) absolute values themselves. The legacy
        // mutator-lambda API was replaced because it conflated three
        // things: (1) the *current* state (seed), (2) the user's *intent*
        // (mutate), and (3) the *value to save* (= post-mutate state).
        // When the seed was wrong (live slot fell out of sync, or
        // getSlotCrop returned defaults for a stopped slot, or anything
        // else), the resulting save-to-model wrote a CORRECT-FOR-WRONG-
        // SEED tuple to disk, silently corrupting the persisted crop.
        // The user-reported "remove start/end/both does not persist on
        // re-press" was this class of bug.
        //
        // Now: each caller decides "I want start=X and end=Y." This
        // function ONLY persists + propagates. No seeding, no mutating.
        //
        // Convention: start <= 0 = no start marker; end < 0 = no end
        // marker. (start=0 is intentionally treated as "no start" since
        // a crop that begins at 0 s carries no information beyond cropEnabled.)
        auto applyCropEdit = [page, sampler, model, ch](double newStart, double newEnd) {
            int slot = -1;
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i) == ch) { slot = i; break; }
            }
            if (slot < 0) return;
            // Normalise: an out-of-order pair collapses to "no end".
            // (Callers should already do the right thing; this is a
            // belt-and-braces guard.)
            if (newStart < 0.0) newStart = 0.0;
            if (newEnd >= 0.0 && newEnd <= newStart + 0.001) newEnd = -1.0;

            int btn = s_slotToBtnIdx.value(slot, -1);
            if (btn >= 0) {
                if (auto *cur = model->getSoundInfo(btn)) {
                    SoundInfo si = *cur;
                    bool anyCrop = newStart > 0.0 || newEnd > 0.0;
                    si.cropEnabled     = anyCrop;
                    si.cropStartUnit   = 0;  // ms
                    si.cropStartValue  = (newStart > 0.0) ? int(newStart * 1000.0 + 0.5) : 0;
                    si.cropStopAfterAt = 1;  // "stop AT X" (absolute, not duration)
                    si.cropStopUnit    = 0;  // ms
                    si.cropStopValue   = (newEnd   > 0.0) ? int(newEnd   * 1000.0 + 0.5) : 0;
                    extremeLog("cropEdit slot=%d btn=%d newS=%.3f newE=%.3f -> cropEn=%d sVal=%d eVal=%d",
                               slot, btn, newStart, newEnd,
                               si.cropEnabled ? 1 : 0,
                               si.cropStartValue, si.cropStopValue);
                    model->setSoundInfo(btn, si);
                    // Push the edit to disk immediately. Default ConfigModel
                    // policy only marks dirty + waits for the next flush
                    // event (play / disconnect / window close). A crop edit
                    // does not normally trigger any of those — the user
                    // right-clicks the waveform, sees the change, and may
                    // never re-play or close until next session restart,
                    // by which point the dirty flag was either flushed
                    // with a stale value (rare) or lost (TS3 crash, hard
                    // exit). Persisting now matches user expectation:
                    // "the markers I set should still be there next time".
                    model->writeConfigImmediate(QString());
                }
            }
            if (sampler) sampler->setSlotCropLive(slot, newStart, newEnd);
            ch->waveform()->setCropRange(newStart, newEnd);
            // Track the new range for the cursor-snap logic below.
            const double sCur = newStart, eCur = newEnd;
            // Cursor-snap when loop is currently ON: shifting the crop
            // edges effectively shifts the loop window. If the live
            // cursor falls outside the new window the channel would
            // play silently (or escape the crop overlay) until the
            // next loop wrap; jump to the relevant edge so audio
            // continuity is preserved. No-op when the cursor is
            // already inside or the channel is not playing.
            if (sampler && ch->waveform()->isLooping()) {
                auto st = sampler->getState(slot);
                if (st == Sampler::ePLAYING || st == Sampler::ePAUSED) {
                    double pos = sampler->getPosition(slot);
                    double lo = (sCur > 0.0) ? sCur : 0.0;
                    double hi = (eCur > 0.0) ? eCur : ch->waveform()->totalLength();
                    if (hi <= 0.0) hi = lo + 1.0;
                    bool inside = (pos >= lo - 1e-3) && (pos <= hi + 1e-3);
                    if (!inside) {
                        bool reverse = ch->waveform()->isReversed();
                        sampler->seek(reverse ? hi : lo, slot);
                        ch->waveform()->notifySeek();
                    }
                }
            }
        };
        // Helper: read the CURRENT crop range for this channel.
        // Priority: STORED SoundInfo (disk truth) when the slot has a
        // cell mapping, otherwise live slot. Returns absolute seconds:
        // start = 0.0 + end = -1.0 means "no crop set".
        //
        // Stored SoundInfo wins over live slot because: (a) live slot
        // is wiped to defaults when the slot is eSILENT, which would
        // make a "remove start" on an idle channel see "0/-1" as the
        // current state and then save "0/-1" back — wiping the
        // untouched end too; (b) live slot can drift behind disk if
        // a sandbox/macro path mutated the model without going through
        // setSlotCropLive.
        auto currentCrop = [page, sampler, model, ch](double &start, double &end) {
            start = 0.0; end = -1.0;
            int slot = -1;
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i) == ch) { slot = i; break; }
            }
            if (slot < 0) return;
            int btn = s_slotToBtnIdx.value(slot, -1);
            if (btn >= 0) {
                if (const auto *cur = model->getSoundInfo(btn)) {
                    start = cur->getStartTime();
                    const double dur = cur->getPlayTime();
                    end = (dur > 0.0) ? (start + dur) : -1.0;
                    return;
                }
            }
            if (sampler) sampler->getSlotCrop(slot, start, end);
        };
        QObject::connect(ch->waveform(), &WaveformPlayer::cropStartRequestedAt,
                         page, [applyCropEdit, currentCrop](double seconds){
            double s, e; currentCrop(s, e);
            double newS = seconds;
            if (e > 0.0 && newS >= e - 0.001) newS = qMax(0.0, e - 0.1);
            applyCropEdit(newS, e);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::cropEndRequestedAt,
                         page, [applyCropEdit, currentCrop](double seconds){
            double s, e; currentCrop(s, e);
            double newE = seconds;
            if (newE <= s + 0.001) newE = s + 0.1;
            applyCropEdit(s, newE);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::cropClearStartRequested,
                         page, [applyCropEdit, currentCrop]{
            double s, e; currentCrop(s, e);
            applyCropEdit(0.0, e);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::cropClearEndRequested,
                         page, [applyCropEdit, currentCrop]{
            double s, e; currentCrop(s, e);
            applyCropEdit(s, -1.0);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::cropClearAllRequested,
                         page, [applyCropEdit]{
            applyCropEdit(0.0, -1.0);
        });
        // Right-button drag on the waveform proposes a loop area. Two
        // user-driven gates: (1) the feature itself is OFF by default
        // and only fires when "Right-drag loop selection" is enabled
        // in Settings; (2) even when enabled, the commit is gated by
        // an inline confirm bubble floating ABOVE the waveform cursor
        // — no modal dialog interrupts mouse / keyboard flow. Yes =
        // write Start/End markers + flip Loop ON; No = bubble closes
        // silently.
        QObject::connect(ch->waveform(), &WaveformPlayer::loopAreaSelected,
                         page, [applyCropEdit, ch, sampler, page, model](double sa, double sb){
            if (!model->getRightDragLoopEnabled()) return;
            // Snapshot the cell index AT BUBBLE CREATION. If the user
            // clicks the red X (clearRequested) while the bubble is
            // open and THEN clicks Yes, s_slotToBtnIdx will have been
            // wiped by the X handler, so a fresh lookup at Yes-time
            // returns -1 and the persist is silently skipped — exact
            // shape of the user-reported "right-drag loop area
            // sometimes doesn't persist" regression. Capture it here
            // and re-inject below.
            int snapshotSlot = -1;
            for (int i = 0; i < page->channels().size(); ++i) {
                if (page->channels().at(i) == ch) { snapshotSlot = i; break; }
            }
            int snapshotBtn = (snapshotSlot >= 0)
                ? s_slotToBtnIdx.value(snapshotSlot, -1) : -1;
            // Fall back to s_lastPlayedCtx in case the slot just finished
            // playing (replay-ready) and s_slotToBtnIdx was never set
            // for it (unusual but possible during a transient state).
            if (snapshotBtn < 0 && snapshotSlot >= 0) {
                auto it = s_lastPlayedCtx.find(snapshotSlot);
                if (it != s_lastPlayedCtx.end()) snapshotBtn = it->btnIdx;
            }
            auto fmtSec = [](double v){
                int m  = static_cast<int>(v) / 60;
                int s  = static_cast<int>(v) % 60;
                int ms = static_cast<int>((v - std::floor(v)) * 1000.0);
                return QString("%1:%2.%3")
                    .arg(m).arg(s, 2, 10, QChar('0')).arg(ms, 3, 10, QChar('0'));
            };

            // Reuse one bubble per channel so a rapid second drag
            // replaces the previous prompt instead of stacking.
            QPointer<QFrame> existing =
                ch->findChild<QFrame*>(QStringLiteral("loopConfirmBubble"),
                                       Qt::FindDirectChildrenOnly);
            if (existing) existing->deleteLater();

            auto *bubble = new QFrame(ch);
            bubble->setObjectName(QStringLiteral("loopConfirmBubble"));
            bubble->setAttribute(Qt::WA_DeleteOnClose);
            bubble->setFrameShape(QFrame::StyledPanel);
            bubble->setStyleSheet(
                "#loopConfirmBubble { background: rgba(20,22,26,235);"
                " border: 1px solid rgba(120,200,120,200);"
                " border-radius: 6px; }"
                "#loopConfirmBubble QLabel { color: #e8f5e8; }"
                "#loopConfirmBubble QPushButton {"
                " background: #3c8c3c; color: white;"
                " border: 1px solid #2a6e2a; border-radius: 3px;"
                " padding: 3px 12px; min-width: 56px; }"
                "#loopConfirmBubble QPushButton#noBtn {"
                " background: #6a6a6a; border-color: #4a4a4a; }"
                "#loopConfirmBubble QPushButton:hover { background: #4eaf4e; }"
                "#loopConfirmBubble QPushButton#noBtn:hover { background: #888; }");

            auto *lbl = new QLabel(
                QObject::tr("Create loop area %1 → %2 (%3 s)?")
                    .arg(fmtSec(sa)).arg(fmtSec(sb))
                    .arg(sb - sa, 0, 'f', 2),
                bubble);
            lbl->setAlignment(Qt::AlignCenter);
            auto *yes = new QPushButton(QObject::tr("Yes"), bubble);
            auto *no  = new QPushButton(QObject::tr("No"),  bubble);
            no->setObjectName(QStringLiteral("noBtn"));

            auto *col = new QVBoxLayout(bubble);
            col->setContentsMargins(8, 6, 8, 6);
            col->setSpacing(4);
            col->addWidget(lbl);
            auto *row = new QHBoxLayout;
            row->setSpacing(6);
            row->addStretch(1);
            row->addWidget(yes);
            row->addWidget(no);
            row->addStretch(1);
            col->addLayout(row);

            // Auto-close on no, or after 8 s of inactivity so a stray
            // bubble doesn't sit forever if the user wanders off.
            auto *timeout = new QTimer(bubble);
            timeout->setSingleShot(true);
            QObject::connect(timeout, &QTimer::timeout, bubble, &QFrame::close);
            timeout->start(8000);

            QObject::connect(no, &QPushButton::clicked, bubble, &QFrame::close);
            QObject::connect(yes, &QPushButton::clicked, bubble,
                             [ch, sampler, model, sa, sb, bubble,
                              snapshotSlot, snapshotBtn]{
                // Persist crop to the snapshotted cell directly. Bypasses
                // applyCropEdit's s_slotToBtnIdx lookup because the user
                // may have clicked X between bubble-open and Yes-click,
                // wiping the live mapping. Mirrors applyCropEdit's
                // persist block 1:1.
                if (snapshotBtn >= 0) {
                    if (const auto *cur = model->getSoundInfo(snapshotBtn)) {
                        SoundInfo si = *cur;
                        si.cropEnabled     = (sa > 0.0) || (sb > 0.0);
                        si.cropStartUnit   = 0;
                        si.cropStartValue  = (sa > 0.0) ? int(sa * 1000.0 + 0.5) : 0;
                        si.cropStopAfterAt = 1;
                        si.cropStopUnit    = 0;
                        si.cropStopValue   = (sb > 0.0) ? int(sb * 1000.0 + 0.5) : 0;
                        extremeLog("loopBubble.Yes slot=%d btn=%d sa=%.3f sb=%.3f -> cropEn=%d sVal=%d eVal=%d",
                                   snapshotSlot, snapshotBtn, sa, sb,
                                   si.cropEnabled ? 1 : 0,
                                   si.cropStartValue, si.cropStopValue);
                        model->setSoundInfo(snapshotBtn, si);
                    }
                }
                if (sampler && snapshotSlot >= 0)
                    sampler->setSlotCropLive(snapshotSlot, sa, sb);
                ch->waveform()->setCropRange(sa, sb);
                ch->waveform()->setLooping(true);
                int slotIdx = ch->channelId();
                if (sampler && slotIdx >= 0) {
                    sampler->setSlotLoop(slotIdx, true);
                    // Cursor-snap: if the channel is currently playing
                    // and the live cursor sits OUTSIDE the freshly
                    // committed loop window, jump it to the appropriate
                    // edge so the next sample heard is inside the loop.
                    //   - Forward play  → seek to start (sa)
                    //   - Reverse play  → seek to end (sb), the natural
                    //     entry point of a reverse pass.
                    // If the cursor is already inside [sa, sb] do
                    // nothing — let the audio play out to the boundary
                    // and the existing loop branch take it from there.
                    auto st = sampler->getState(slotIdx);
                    if (st == Sampler::ePLAYING || st == Sampler::ePAUSED) {
                        double pos = sampler->getPosition(slotIdx);
                        bool inside = (pos >= sa - 1e-3) && (pos <= sb + 1e-3);
                        if (!inside) {
                            bool reverse = ch->waveform()->isReversed();
                            sampler->seek(reverse ? sb : sa, slotIdx);
                            ch->waveform()->notifySeek();
                        }
                    }
                }
                bubble->close();
            });

            // Position: centred on the drag midpoint, ABOVE the
            // SoundView. mapToParent puts coords in the channel's
            // coordinate space so the bubble floats over the channel
            // strip without leaking into other channels.
            bubble->adjustSize();
            auto *wave = ch->waveform();
            QWidget *soundView = wave->findChild<QWidget*>(
                QString(), Qt::FindChildrenRecursively);
            QRect waveRect = wave->geometry();
            QPoint topLeftInCh = wave->mapToParent(QPoint(0, 0));
            int midFracPx = static_cast<int>(
                ((sa + sb) * 0.5)
                / qMax(0.001, ch->waveform()->totalLength())
                * waveRect.width());
            int x = topLeftInCh.x() + midFracPx - bubble->width() / 2;
            int y = topLeftInCh.y() - bubble->height() - 4;
            if (x < 4) x = 4;
            if (x + bubble->width() > ch->width() - 4)
                x = ch->width() - bubble->width() - 4;
            if (y < 4) y = topLeftInCh.y() + waveRect.height() + 4;
            bubble->move(x, y);
            bubble->show();
            bubble->raise();
            (void)soundView;
        });
    };
    QObject::connect(page, &MainPage::channelAdded, page, [page, wireChannelButtons](int idx){
        if (auto *ch = page->channelAt(idx)) wireChannelButtons(ch);
    });
    for (auto *existing : page->channels()) wireChannelButtons(existing);

    // One-time YouTube discovery hint over the first channel's name. Gated on a
    // QSettings one-shot flag so it only ever appears once, and only while the
    // streaming feature is enabled. Deferred so the channel is laid out first.
    if (model && model->getStreamingEnabled()) {
        QSettings dsc("GameBaiters", "Soundboard");
        if (!dsc.value("ytDiscoveryShown2", false).toBool() && !page->channels().isEmpty()) {
            QTimer::singleShot(1200, page, [page]{
                if (page->channels().isEmpty()) return;
                if (auto *ch = page->channels().first()) {
                    ch->showDiscoveryBubble(QObject::tr(
                        "Did you know? You can paste and play a YouTube video "
                        "directly — just paste the link as the channel name. Try it now!"));
                    QSettings s("GameBaiters", "Soundboard");
                    s.setValue("ytDiscoveryShown2", true);
                }
            });
        }
    }

    // Sampler -> waveform indicator. Slot N drives channel widget N.
    if (sampler) {
        QObject::connect(sampler, &Sampler::onStartPlaying, page,
                         [page, sampler](int slot, bool preview, QString filename){
            // Preview never owns a channel widget.
            if (preview) return;
            if (slot < 0 || slot >= page->channels().size()) return;
            auto *ch = page->channels().at(slot);
            // s_slotStreamTitle was populated at load; consume the loading flag.
            s_slotStreamLoading.remove(slot);
            // Audio is actually flowing: hide the "Opening audio stream…"
            // marquee and release a stale seek-pending cursor lock from the
            // previous content (a network seek whose commit was discarded
            // mid-rotation would otherwise freeze the cursor forever).
            ch->setStreamLoading(false);
            ch->setProperty("pendingSeekActive", false);
            const bool isStream = s_slotStreamTitle.contains(slot);
            const bool isLive   = s_slotStreamLive.contains(slot);
            if (isLive) {
                // LIVE stream: no waveform (endless, unseekable). Purple notice,
                // reverse/vinyl/skip disabled, no analyser on the URL. Live can't
                // be saved to a file, so no download control.
                ch->waveform()->setLiveStream(true);
                ch->waveform()->setStreamLabel(s_slotStreamTitle.value(slot, filename));
                ch->waveform()->setPlaying(true);
                ch->setExportIsDownload(false);
                LastPlayedCtx lctx;
                lctx.btnIdx   = s_slotToBtnIdx.value(slot, -1);
                lctx.filename = filename;
                s_lastPlayedCtx[slot] = lctx;
                return;
            }
            ch->waveform()->setLiveStream(false);
            SoundInfo info;
            info.filename = filename;
            // Normal network VOD draws a full (progressive) waveform just like a
            // local file — only true LIVE streams skip it (handled above).
            info.isStreamUrl = isStream;
            ch->waveform()->setSound(info);
            ch->waveform()->setPlaying(true);
            // Finite stream: show the video TITLE in the file-label instead of
            // the ugly direct CDN URL, and surface the prominent "Save audio"
            // download button (a VOD stream can be saved to a file).
            if (isStream) {
                ch->waveform()->setStreamLabel(s_slotStreamTitle.value(slot, filename));
                ch->setExportIsDownload(true);
            } else {
                ch->setExportIsDownload(false);
            }
            // Feed the actual crop applied to this slot so the waveform
            // can mark its start / end points. clearPlayback() on stop
            // or sound change wipes it, and looping never re-emits this
            // signal so the markers stay put across loops.
            double cropStart = 0.0, cropEnd = -1.0;
            sampler->getSlotCrop(slot, cropStart, cropEnd);
            ch->waveform()->setCropRange(cropStart, cropEnd);
            // Record the play context for the replay-from-cursor path.
            // s_slotToBtnIdx is set by the SoundButton click handler
            // BEFORE this signal fires; if absent (macro restore,
            // drag-from-file) we have no btnIdx -> store -1 and the
            // replay handler will fall back to "no reusable source".
            LastPlayedCtx ctx;
            ctx.btnIdx   = s_slotToBtnIdx.value(slot, -1);
            ctx.filename = filename;
            s_lastPlayedCtx[slot] = ctx;
        }, Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onStopPlaying, page,
                         [page, sampler, model](int slot){
            if (slot < 0 || slot >= page->channels().size()) return;
            // Preview shares slot indices but doesn't own the UI.
            if (sampler && sampler->getState(slot) == Sampler::ePLAYING_PREVIEW) return;
            // Multi-channel infinity: auto-remove the channel widget on
            // stop for slots that were spawned by the infinity path.
            // A LOOP or REVERSE channel is not auto-removed unless the
            // user explicitly hit the red stop button (which set
            // infinityPendingRemove BEFORE stopPlayback). Any other
            // path (natural end, stop-all, cleared) removes.
            //
            // Deferred via singleShot(0) so this handler finishes ALL
            // its state cleanup (wave paint, replay flag, s_slotToBtnIdx)
            // before we start shifting channel indices under it.
            auto infinityCheck = [page](int checkSlot){
                auto *ch = page->channelAt(checkSlot);
                if (!ch) return;
                if (!ch->property("infinityAuto").toBool()) return;
                const bool loopOrReverse = ch->waveform()->isLooping()
                                        || ch->waveform()->isReversed();
                const bool pending = ch->property("infinityPendingRemove").toBool();
                if (loopOrReverse && !pending) {
                    // Deferred stop (natural end of loop cycle, etc.);
                    // keep the channel alive until the user clicks red
                    // stop. Clear any stale pending flag defensively.
                    ch->setProperty("infinityPendingRemove", false);
                    return;
                }
                ch->setProperty("infinityAuto", false);
                ch->setProperty("infinityPendingRemove", false);
                QTimer::singleShot(0, page, [ch]{ ch->requestRemove(); });
            };
            // STALE-STOP GUARD. onStopPlaying is queued (Qt::QueuedConnection)
            // so the handler can fire AFTER a fresh playback has already
            // taken the slot over (rapid sequence: pressing cell B while
            // cell A is playing on the same slot — Sampler emits stop for
            // the old, start for the new, and we get the queued stop after
            // s_slotToBtnIdx[slot] has been re-bound to the new btnIdx).
            // Wiping the live mapping here was the root cause of the
            // user-reported "right-click crop edits silently fail to
            // persist after switching cells" regression: the wipe killed
            // s_slotToBtnIdx[slot] → applyCropEdit fell to btn = -1 →
            // save was skipped.
            //
            // If the sampler reports the slot is actively playing or
            // paused, a new playback owns it now — keep all state intact
            // and just drop the stale hard-clear flag so a LATER stop
            // doesn't mis-fire on it.
            if (sampler) {
                auto st = sampler->getState(slot);
                if (st == Sampler::ePLAYING || st == Sampler::ePAUSED) {
                    s_pendingHardClear.remove(slot);
                    return;
                }
            }
            // Live-stream slot genuinely stopped (a stream RELOAD leaves the
            // slot playing/paused and already returned above). Restore the
            // channel's real name + unlock reverse/vinyl.
            if (!s_slotStreamLoading.contains(slot) && s_slotStreamTitle.contains(slot))
                clearSlotStream(page, slot);
            auto *wave = page->channels().at(slot)->waveform();

            // Hard-clear path: clearRequested marked this slot before
            // calling sampler->stopPlayback. Wipe the channel back to
            // the empty "(no file)" state and drop every map entry so
            // the X button + reload glyph go away. Without this branch
            // the synchronous cleanup done in clearRequested would be
            // overwritten right here by the soft replay-ready path.
            if (s_pendingHardClear.remove(slot)) {
                wave->setPlaying(false);
                wave->setReplayReady(false);
                wave->setFilename(QString());
                wave->clearPlayback();
                s_lastPlayedCtx.remove(slot);
                s_slotToBtnIdx.remove(slot);
                infinityCheck(slot);
                return;
            }

            // Replay mode is OPTIONAL. OFF = pre-replay-mode behavior:
            // a stop / end fully wipes the channel back to empty,
            // identical to clicking the red X. The reload glyph never
            // appears. PauseAllMode is also gated on this so the
            // "Replay all" mode never engages.
            if (model && !model->getReplayModeEnabled()) {
                wave->setPlaying(false);
                wave->setReplayReady(false);
                wave->setFilename(QString());
                wave->clearPlayback();
                s_lastPlayedCtx.remove(slot);
                s_slotToBtnIdx.remove(slot);
                infinityCheck(slot);
                return;
            }

            wave->setPlaying(false);
            // Replay-from-cursor UX: KEEP filename + waveform + crop
            // markers visible after stop so the channel still shows
            // what it just played. Switch the play/pause button glyph
            // to the reload icon via setReplayReady so the user can
            // restart the sound with a single click.
            //
            // Cursor policy: ALWAYS snap to cropStart on a soft stop,
            // whether the audio reached the end on its own or the user
            // hit the red stop button. The "default position" is the
            // head of the cropped region; the user can still move the
            // cursor via waveform click / skip before clicking replay,
            // and that parked position will be honoured.
            wave->setPlaybackFraction(wave->cropStartFraction());
            wave->setReplayReady(true);
            // s_slotToBtnIdx is intentionally NOT removed here - the
            // waveform's right-click crop editor needs to keep finding
            // the cell, and the replay handler reads it back. It is
            // cleared on clearRequested (red X), removeChannelRequested,
            // or when a fresh playback overwrites it.
            infinityCheck(slot);
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
        // Unplayable file -> in-line red error banner on the channel's
        // waveform label, NOT a modal popup. Diagnose the precise cause
        // via QFileInfo so the user sees "File not found" vs "Empty
        // file" vs "Unsupported format" instead of one generic blob.
        // The banner clears the next time setFilename / setSound runs
        // on this channel (i.e. when anything else is played here).
        QObject::connect(sampler, &Sampler::onPlaybackError, page,
                         [page](int slot, QString filename){
            if (slot < 0 || slot >= page->channels().size()) return;
            QString base = filename;
            int sl = qMax(base.lastIndexOf('/'), base.lastIndexOf('\\'));
            if (sl >= 0) base = base.mid(sl + 1);
            if (base.isEmpty()) base = QObject::tr("(unknown file)");

            const bool isNet = filename.startsWith("http://") || filename.startsWith("https://")
                            || s_slotStreamUrl.contains(slot);
            QString reason;
            if (isNet) {
                // A network URL never "exists" on disk — don't report File not
                // found; clear the stream state and restore the channel name.
                reason = QObject::tr("Network stream error");
                base   = s_slotStreamTitle.value(slot, QObject::tr("stream"));
                clearSlotStream(page, slot);
            } else if (filename.isEmpty()) {
                reason = QObject::tr("No file assigned");
            } else {
                QFileInfo fi(filename);
                if (!fi.exists())          reason = QObject::tr("File not found");
                else if (!fi.isReadable()) reason = QObject::tr("Permission denied");
                else if (fi.size() == 0)   reason = QObject::tr("Empty file");
                else                       reason = QObject::tr("Unsupported format or corrupt file");
            }

            const QString msg = QObject::tr("%1 \xE2\x80\x94 %2")
                                    .arg(reason, base);
            page->channels().at(slot)->waveform()->setError(msg);
        }, Qt::QueuedConnection);

        // Release the per-channel "seek pending" cursor lock when the
        // async seek worker has actually committed the new position
        // (Sampler::seek now returns instantly; the FFmpeg scan runs
        // on a worker thread). Until this fires the 30 Hz position
        // poll keeps the cursor at the click target instead of
        // reading the still-pre-seek decoder position.
        QObject::connect(sampler, &Sampler::onSeekCommitted, page,
                         [page](int slot){
            if (slot < 0 || slot >= page->channels().size()) return;
            auto *ch = page->channels().at(slot);
            if (ch) ch->setProperty("pendingSeekActive", false);
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
                         [model, sampler, slot, isPrimary, ch](int v){
            extremeLog("FxPanel::pitchChanged slot=%d val=%d", slot, v);
            if (isPrimary) model->setPitchValue(v);
            if (sampler) {
                // Match legacy ConfigQt scaling: factor = 3^(v/100).
                float factor = AudioUtils::sliderToPitchFactor(v);
                sampler->setSlotPitchFactor(slot, factor);
                if (isPrimary) sampler->setPitchFactor(factor);
            }
            ch->waveform()->setLiveFx(v, ch->fx()->speed(), ch->fx()->reverb());
        });
        QObject::connect(ch->fx(), &FxPanel::speedChanged,
                         [model, sampler, slot, isPrimary, ch](int v){
            extremeLog("FxPanel::speedChanged slot=%d val=%d", slot, v);
            if (isPrimary) model->setSpeedValue(v);
            if (sampler) {
                float factor = AudioUtils::sliderToPitchFactor(v);
                sampler->setSlotSpeedFactor(slot, factor);
                if (isPrimary) sampler->setSpeedFactor(factor);
            }
            ch->waveform()->setLiveFx(ch->fx()->pitch(), v, ch->fx()->reverb());
        });
        QObject::connect(ch->fx(), &FxPanel::reverbChanged,
                         [model, sampler, slot, isPrimary, ch](int v){
            extremeLog("FxPanel::reverbChanged slot=%d val=%d", slot, v);
            if (isPrimary) model->setReverbValue(v);
            if (sampler) sampler->setSlotReverbMix(slot, v / 100.0f);
            ch->waveform()->setLiveFx(ch->fx()->pitch(), ch->fx()->speed(), v);
        });
        QObject::connect(ch->fx(), &FxPanel::syncChanged,
                         [model, isPrimary](bool s){
            if (isPrimary) model->setSyncPitchSpeed(s);
        });

        // Reverb ENGINE settings (gear next to the Reverb slider).
        // Channel-wide choice, outside the sandbox: algorithmic
        // (classic Freeverb) vs convolution + IR preset / custom IR.
        // Edits flow through the normal sandbox-state path so they are
        // applied live AND persisted with the channel state.
        //
        // UI = a small Qt::Popup anchored right above the gear (not a
        // full dialog): every control applies instantly and clicking
        // anywhere outside dismisses it. Wrapped in a self-referencing
        // shared function so the Load-IR flow can reopen it after the
        // modal file dialog (which force-closes any Qt::Popup).
        {
            auto openEnginePopup = std::make_shared<std::function<void()>>();
            *openEnginePopup = [sampler, slot, ch, openEnginePopup]{
                Theme::Derived d = Theme::derive(Theme::colors());
                auto *pop = new QWidget(ch, Qt::Popup | Qt::FramelessWindowHint);
                pop->setAttribute(Qt::WA_DeleteOnClose);
                pop->setProperty("isGBSoundboard", true);
                pop->setObjectName(QStringLiteral("revEnginePopup"));
                // Explicit surface + transparent labels: an unstyled
                // widget inside the TS3 host process picks the HOST
                // stylesheet colors (same leak as the fx sliders).
                pop->setStyleSheet(QString(
                    "#revEnginePopup { background-color: %1;"
                    " border: 1px solid %2; }"
                    "#revEnginePopup QLabel { background: transparent;"
                    " color: %3; }")
                    .arg(d.surface.name(), d.borderStrong.name(),
                         d.text.name()));
                pop->setFixedWidth(280);

                auto st = std::make_shared<SandboxState>(ch->sandboxState());
                auto apply = [sampler, slot, ch, st]{
                    ch->setSandboxState(*st);
                    if (sampler) sampler->setSlotSandboxState(slot, *st);
                    if (ChannelStatePersistence::isEnabled())
                        ChannelStatePersistence::saveState(ch->channelId(),
                                                           ch->state());
                };

                auto *lay = new QVBoxLayout(pop);
                lay->setContentsMargins(10, 8, 10, 8);
                lay->setSpacing(6);

                auto *title = new QLabel(QObject::tr("Reverb engine"), pop);
                {
                    QFont f = title->font();
                    f.setBold(true);
                    title->setFont(f);
                }
                lay->addWidget(title);

                auto *grid = new QGridLayout;
                grid->setHorizontalSpacing(8);
                grid->setVerticalSpacing(6);
                grid->addWidget(new QLabel(QObject::tr("Engine:"), pop), 0, 0);
                auto *engBox = new QComboBox(pop);
                engBox->addItem(QObject::tr("Algorithmic (classic)"));
                engBox->addItem(QObject::tr("Convolution (IR)"));
                engBox->setCurrentIndex(st->reverbConvMode == 1 ? 1 : 0);
                engBox->setToolTip(QObject::tr(
                    "Algorithmic = the classic Freeverb engine (cheap).\n"
                    "Convolution = real impulse-response reverb: denser,\n"
                    "richer tails at a higher CPU cost. The wet amount is\n"
                    "this channel's Reverb slider in both engines."));
                grid->addWidget(engBox, 0, 1);
                grid->addWidget(new QLabel(QObject::tr("IR preset:"), pop), 1, 0);
                auto *irBox = new QComboBox(pop);
                irBox->addItem(QObject::tr("Hall"));
                irBox->addItem(QObject::tr("Church"));
                irBox->addItem(QObject::tr("Room"));
                irBox->addItem(QObject::tr("Spring"));
                int pr = st->reverbConvPreset;
                irBox->setCurrentIndex((pr >= 0 && pr <= 3) ? pr : 0);
                irBox->setEnabled(st->reverbConvIrPath.isEmpty());
                grid->addWidget(irBox, 1, 1);
                grid->setColumnStretch(1, 1);
                lay->addLayout(grid);

                auto *fileRow = new QHBoxLayout;
                auto *loadBtn = new QPushButton(QObject::tr("Load IR..."), pop);
                loadBtn->setToolTip(QObject::tr(
                    "Use any audio file as the impulse response (first 2 s)."));
                auto *clearBtn = new QPushButton(QObject::tr("Preset IR"), pop);
                clearBtn->setToolTip(QObject::tr(
                    "Drop the custom IR file and use the preset above."));
                fileRow->addWidget(loadBtn);
                fileRow->addWidget(clearBtn);
                fileRow->addStretch(1);
                lay->addLayout(fileRow);

                auto *pathLbl = new QLabel(pop);
                pathLbl->setStyleSheet(QStringLiteral(
                    "background: transparent; color: #8aa6c0; font-size: 10px;"));
                pathLbl->setWordWrap(true);
                pathLbl->setText(st->reverbConvIrPath.isEmpty()
                    ? QObject::tr("Using preset IR")
                    : QObject::tr("IR: %1").arg(st->reverbConvIrPath));
                lay->addWidget(pathLbl);

                QObject::connect(engBox,
                                 qOverload<int>(&QComboBox::currentIndexChanged),
                                 pop, [st, apply](int v){
                    st->reverbConvMode = v;
                    apply();
                });
                QObject::connect(irBox,
                                 qOverload<int>(&QComboBox::currentIndexChanged),
                                 pop, [st, apply](int v){
                    st->reverbConvPreset = v;
                    apply();
                });
                QObject::connect(loadBtn, &QPushButton::clicked, pop,
                                 [pop, st, apply, ch, openEnginePopup]{
                    // The modal file dialog would force-close the popup
                    // anyway - close it deliberately, run the picker,
                    // then REOPEN the popup rebuilt from fresh state so
                    // the user sees the result where they left off.
                    // (pop must not be touched after close(): it is
                    // WA_DeleteOnClose and dies inside the dialog's
                    // nested event loop.)
                    pop->close();
                    QString p = QFileDialog::getOpenFileName(ch,
                        QObject::tr("Load impulse response"), QString(),
                        QObject::tr("Audio files (*.wav *.flac *.mp3 *.ogg *.m4a);;All files (*.*)"));
                    if (!p.isEmpty()) {
                        st->reverbConvIrPath = p;
                        st->reverbConvMode = 1;   // an IR implies convolution
                        apply();
                    }
                    (*openEnginePopup)();
                });
                QObject::connect(clearBtn, &QPushButton::clicked, pop,
                                 [pop, st, apply, irBox, pathLbl]{
                    st->reverbConvIrPath.clear();
                    apply();
                    irBox->setEnabled(true);
                    pathLbl->setText(QObject::tr("Using preset IR"));
                    Q_UNUSED(pop);
                });

                // Anchor right above the gear, right-aligned to it;
                // fall back below when there is no room on screen.
                pop->adjustSize();
                QRect btn = ch->fx()->reverbEngineBtnGlobalRect();
                QPoint tl(btn.right() - pop->width(),
                          btn.top() - pop->height() - 6);
                QScreen *scr = QGuiApplication::screenAt(btn.center());
                if (!scr) scr = QGuiApplication::primaryScreen();
                if (scr) {
                    QRect av = scr->availableGeometry();
                    if (tl.x() < av.left()) tl.setX(av.left());
                    if (tl.x() + pop->width() > av.right())
                        tl.setX(av.right() - pop->width());
                    if (tl.y() < av.top()) tl.setY(btn.bottom() + 6);
                }
                pop->move(tl);
                pop->show();
            };
            QObject::connect(ch->fx(), &FxPanel::reverbEngineClicked,
                             ch, [openEnginePopup]{ (*openEnginePopup)(); });
        }
        QObject::connect(ch->waveform(), &WaveformPlayer::stopClicked,
                         [sampler, slot, ch]{
            // Infinity mode uses the "infinityPendingRemove" property as
            // the "user asked for the RED-STOP-BUTTON death" flag. Set
            // it BEFORE stopPlayback so the queued onStopPlaying handler
            // sees the intent even when loop / reverse is on (those
            // would otherwise keep the auto channel alive on any other
            // stop path).
            if (ch->property("infinityAuto").toBool())
                ch->setProperty("infinityPendingRemove", true);
            if (sampler) sampler->stopPlayback(slot);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::playClicked,
                         [sampler, slot]{ if (sampler) sampler->unpausePlayback(slot); });
        QObject::connect(ch->waveform(), &WaveformPlayer::pauseClicked,
                         [sampler, slot]{ if (sampler) sampler->pausePlayback(slot); });
        // Replay-from-cursor. Audio is finished or was stopped. Resolve
        // the originating cell, replay the sound, then seek to wherever
        // the user parked the cursor (clamped to the crop range so the
        // restart never lands outside the audible window).
        QObject::connect(ch->waveform(), &WaveformPlayer::replayClicked,
                         [model, sampler, slot, ch]{
            if (!sampler) return;
            auto it = s_lastPlayedCtx.find(slot);
            if (it == s_lastPlayedCtx.end()) return;
            const int btnIdx = it->btnIdx;
            const QString expectedFile = it->filename;

            // The cell may have been emptied / replaced since the
            // original play. If so, drop the replay context and clear
            // the channel: the soundboard's source of truth is the
            // cell, not the channel.
            const SoundInfo *info = (btnIdx >= 0)
                ? model->getSoundInfo(btnIdx) : nullptr;
            if (!info || info->filename.isEmpty()
                || info->filename != expectedFile) {
                s_lastPlayedCtx.remove(slot);
                s_slotToBtnIdx.remove(slot);
                auto *wave = ch->waveform();
                wave->setPlaying(false);
                wave->setReplayReady(false);
                wave->setFilename(QString());
                wave->clearPlayback();
                return;
            }

            // Snapshot the cursor BEFORE play (the start signal will
            // overwrite m_playbackPosition once the audio thread begins
            // emitting positions). Clamp to [cropStart, cropEnd - eps]
            // so we never restart past the end of the cut.
            const double curFrac   = ch->waveform()->cursorFraction();
            const double startFrac = ch->waveform()->cropStartFraction();
            const double endFrac   = ch->waveform()->cropEndFraction();
            const double totalLen  = ch->waveform()->totalLength();
            double seekFrac = curFrac;
            if (seekFrac < startFrac) seekFrac = startFrac;
            if (seekFrac > endFrac - 0.005) seekFrac = startFrac; // wrap from end
            const double seekSec = seekFrac * totalLen;

            sampler->stopPlayback(slot);  // belt and braces; should be no-op
            if (!sampler->playSoundInSlot(slot, *info, false))
                return;
            s_slotToBtnIdx[slot] = btnIdx;

            // Mirror the buttonClicked path's volume + FX setup so the
            // replay honours the channel's current sliders + per-button
            // FX state.
            sampler->setSlotVolumeLocal (slot, ch->volume()->local());
            sampler->setSlotVolumeRemote(slot, ch->volume()->remote());
            const bool globalFx = model->getGlobalFxEnabled();
            if (globalFx && info->fxRemember) {
                sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(info->fxPitch));
                sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(info->fxSpeed));
                sampler->setSlotReverbMix  (slot, info->fxReverb / 100.0f);
            } else if (globalFx) {
                sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(ch->fx()->pitch()));
                sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(ch->fx()->speed()));
                sampler->setSlotReverbMix  (slot, ch->fx()->reverb() / 100.0f);
            } else {
                sampler->setSlotPitchFactor(slot, 1.0f);
                sampler->setSlotSpeedFactor(slot, 1.0f);
                sampler->setSlotReverbMix  (slot, 0.0f);
            }

            // Honour the parked cursor unless it would land at the very
            // start of the crop region (in which case the natural play
            // entry point is already correct - no seek needed).
            if (seekSec > 0.05)
                sampler->seek(seekSec, slot);
        });
        // Red "X" next to the filename label. Wipes every trace of the
        // sound from the channel: stops any residual playback, drops
        // replay state + slot/btn bookkeeping, hides clear button.
        QObject::connect(ch->waveform(), &WaveformPlayer::clearRequested,
                         [sampler, slot, ch]{
            // Belt-and-braces: flush any pending dirty model state
            // BEFORE we tear down the slot mapping. setSoundInfo now
            // writes immediately for cell-metadata edits so this is
            // mostly a no-op, but a stray slider/volume mark-dirty
            // that hasn't been flushed yet would otherwise be at risk
            // if the user closes TS3 right after clicking X.
            ConfigModel::flushPendingWrite();
            // Mark the slot for a hard clear BEFORE asking Sampler to
            // stop - the stop() call queues an onStopPlaying signal
            // that would otherwise transition the channel back into
            // soft replay-ready state a couple of ms later and undo
            // our work. The hard-clear branch in the onStopPlaying
            // handler reads the flag and short-circuits.
            s_pendingHardClear.insert(slot);
            if (sampler) sampler->stopPlayback(slot);
            // Synchronous UI cleanup so the X button + reload glyph
            // disappear instantly - the queued onStopPlaying tick
            // arrives a frame later and the hard-clear branch then
            // confirms the same state (idempotent).
            auto *wave = ch->waveform();
            wave->setPlaying(false);
            wave->setReplayReady(false);
            wave->setFilename(QString());
            wave->clearPlayback();
            s_lastPlayedCtx.remove(slot);
            s_slotToBtnIdx.remove(slot);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::skip,
                         [sampler, slot, ch](int sec){
            if (!sampler) return;
            auto *wave = ch->waveform();
            const auto st = sampler->getState(slot);
            if (st == Sampler::ePLAYING || st == Sampler::ePAUSED) {
                double cur = sampler->getPosition(slot);
                sampler->seek(cur + sec, slot);
                wave->notifySeek();
                return;
            }
            // Replay / idle state: skip only moves the visual cursor.
            // The next replay click reads it back via cursorFraction().
            const double totalLen = wave->totalLength();
            if (totalLen <= 0.0) return;
            double curFrac = wave->cursorFraction();
            if (curFrac < 0.0) curFrac = wave->cropStartFraction();
            const double startFrac = wave->cropStartFraction();
            const double endFrac   = wave->cropEndFraction();
            double newFrac = curFrac + double(sec) / totalLen;
            if (newFrac < startFrac) newFrac = startFrac;
            if (newFrac > endFrac)   newFrac = endFrac;
            wave->setPlaybackFraction(newFrac);
            // Stopped-track time label sync: setPlaybackFraction only
            // paints the cursor — the M:SS readout in the top-right
            // would stay frozen on the last live position without an
            // explicit setPosition.
            wave->setPosition(newFrac * totalLen, totalLen);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::seekRequested,
                         [sampler, slot, ch](double frac){
            if (!sampler) return;
            auto *wave = ch->waveform();
            const auto st = sampler->getState(slot);
            if (st == Sampler::ePLAYING || st == Sampler::ePAUSED) {
                double len = sampler->getLength(slot);
                if (len > 0.0) {
                    // GUI-side debounce for click-spam on long audio.
                    // Rapid back-and-forth waveform clicks each fired
                    // an FFmpeg backward scan (300+ ms on a 10-h MP3)
                    // serialised behind Sampler::seek → user-visible
                    // freeze. Coalesce here so only the LATEST target
                    // within an 80 ms window reaches Sampler::seek.
                    // The cursor jumps immediately (setPlaybackFraction
                    // below) so single clicks still feel snappy.
                    //
                    // "pendingSeekActive" property gates the 30 Hz
                    // position poll: while a seek is pending the poll
                    // must NOT overwrite the snapped cursor with the
                    // current decoder position (still on the old spot
                    // until the debounce fires) — that was the
                    // user-reported "click backward and the cursor
                    // freezes (= reverts to audible position)" bug.
                    const double target = frac * len;
                    QTimer *t = ch->findChild<QTimer*>(
                        QStringLiteral("seekDebounceTimer"),
                        Qt::FindDirectChildrenOnly);
                    if (!t) {
                        t = new QTimer(ch);
                        t->setObjectName(QStringLiteral("seekDebounceTimer"));
                        t->setSingleShot(true);
                        QObject::connect(t, &QTimer::timeout,
                            [sampler, slot, ch]{
                            if (!sampler) return;
                            double tgt = ch->property("pendingSeekSec")
                                            .toDouble();
                            sampler->seek(tgt, slot);
                            // Lock STAYS set: Sampler::seek is now
                            // async (returns instantly while the
                            // backward FFmpeg scan runs on a worker).
                            // The lock is released by the
                            // onSeekCommitted signal connection below
                            // once the scan actually completes — only
                            // then is cachedPositionSec the seeked
                            // target and the cursor safe to follow.
                            auto *wv = ch->waveform();
                            if (wv) wv->notifySeek();
                        });
                    }
                    ch->setProperty("pendingSeekSec", target);
                    ch->setProperty("pendingSeekActive", true);
                    t->start(80);
                    // Snap the visual cursor immediately so the user
                    // sees the click registered even before the
                    // FFmpeg seek lands. The poll-lock above prevents
                    // it from being overwritten during the debounce.
                    wave->setPlaybackFraction(frac);
                }
                return;
            }
            // Replay / idle state: clamp to crop range and just move
            // the cursor. No audio fires until the user clicks reload.
            const double startFrac = wave->cropStartFraction();
            const double endFrac   = wave->cropEndFraction();
            if (frac < startFrac) frac = startFrac;
            if (frac > endFrac)   frac = endFrac;
            wave->setPlaybackFraction(frac);
            // Stopped-track time label sync — mirrors the skip handler
            // above so the M:SS readout follows every cursor placement.
            const double totalLen = wave->totalLength();
            if (totalLen > 0.0)
                wave->setPosition(frac * totalLen, totalLen);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::loopToggled,
                         [sampler, slot](bool on){
            extremeLog("WaveformPlayer::loopToggled slot=%d on=%d", slot, on ? 1 : 0);
            if (sampler) sampler->setSlotLoop(slot, on);
        });
        QObject::connect(ch->waveform(), &WaveformPlayer::reverseToggled,
                         [sampler, slot](bool on){
            extremeLog("WaveformPlayer::reverseToggled slot=%d on=%d", slot, on ? 1 : 0);
            if (sampler) sampler->setSlotReverse(slot, on);
        });

        // ---- Tape stop (vinyl brake, D1) ----
        // One popup per channel, created up front (cheap). The popup's
        // disc gestures map to the Sampler tape API; a 20 Hz poll keeps
        // the disc animation in sync with the actual brake phase. The
        // poll early-outs while the popup is hidden, so idle cost is a
        // timer tick + one bool.
        {
            auto *vinyl = new VinylPopup(ch);
            auto *phasePoll = new QTimer(vinyl);
            phasePoll->setInterval(50);
            QObject::connect(phasePoll, &QTimer::timeout, vinyl,
                             [sampler, slot, vinyl]{
                if (!vinyl->isVisible()) return;
                int phase = sampler ? sampler->tapeState(slot) : 0;
                vinyl->setTapePhase(phase);
                bool playing = sampler &&
                    (sampler->getState(slot) == Sampler::ePLAYING);
                vinyl->setPlaying(playing || phase == 2);
            });
            phasePoll->start();
            QObject::connect(ch->waveform(), &WaveformPlayer::vinylClicked,
                             vinyl, [ch, vinyl]{
                vinyl->popupAt(ch->waveform()->vinylButtonGlobalPos());
            });
            // Arm the tape ring ONLY while the vinyl popup is open, and
            // DISARM on close. Arming builds a ~1 s decode-ahead window
            // of already-decoded audio, so the play head trails the
            // decoder by that much - which means live pitch / speed /
            // reverb edits (applied in the decoder) would be heard ~1 s
            // late while armed. Keeping the tape armed for the whole
            // playback (the old play-start arming) imposed that delay on
            // EVERY sound. Now normal playback has zero tape latency and
            // instant FX; the window builds the moment the user opens
            // the disc, a beat before they scratch.
            QObject::connect(vinyl, &VinylPopup::armChanged, vinyl,
                             [sampler, slot](bool on){
                if (sampler) sampler->tapeArm(slot, on);
            });
            QObject::connect(vinyl, &VinylPopup::scratchBegan, vinyl,
                             [sampler, slot]{
                if (sampler) sampler->tapeScratchBegin(slot);
            });
            QObject::connect(vinyl, &VinylPopup::scratchMoved, vinyl,
                             [sampler, slot](float deltaSec){
                // True scratch inside the ring; past its walls the
                // router turns overflow into seek + rebase needle
                // jumps - the drag traverses the whole file endlessly.
                if (sampler) sampler->tapeScratchDelta(slot, deltaSec);
            });
            QObject::connect(vinyl, &VinylPopup::scratchScrolled, vinyl,
                             [sampler, slot](float deltaSec){
                // Wheel: same router, same endless traversal.
                if (sampler) sampler->tapeScratchDelta(slot, deltaSec);
            });
            QObject::connect(vinyl, &VinylPopup::scratchEnded, vinyl,
                             [sampler, slot](int ms){
                if (sampler) sampler->tapeScratchEnd(slot, static_cast<float>(ms));
            });
            QObject::connect(vinyl, &VinylPopup::oneShotRequested, vinyl,
                             [sampler, slot](int ms){
                // Brake runs to a full stop; the audio thread auto-
                // pauses the slot when the ramp reaches zero.
                if (sampler) sampler->tapeStop(slot, static_cast<float>(ms));
            });
            QObject::connect(vinyl, &VinylPopup::resumeRequested, vinyl,
                             [sampler, slot](int ms){
                if (sampler) sampler->tapeRelease(slot, static_cast<float>(ms));
            });
            ch->setVinylButtonVisible(model->getShowVinylButton());
        }

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
        // Copy ONLY the per-channel settings the user expects to
        // propagate (vol / fx / sandbox / loop / reverse / sync). The
        // primary channel's filename + playback position belong to
        // its currently-loaded sound and must NOT leak into a fresh
        // channel: copying them made the new channel pretend the
        // primary's sound was already loaded there too, showing the
        // filename label + waveform + replay icon for a sound it had
        // never played.
        ChannelState st = primary->state();
        st.filename.clear();
        st.playbackPos = 0.0;
        fresh->applyState(st);
    });

    QObject::connect(page->resetButton(), &ResetChannelsBtn::resetRequested,
                     [page, sampler, model]{
        if (sampler) sampler->stopPlayback(-1);
        bool rmExtra  = model->getResetAllRemoveExtra();
        bool rstVol   = model->getResetAllVolume();
        bool rstFx    = model->getResetAllFx();
        bool rstSbx   = model->getResetAllSandbox();

        if (rmExtra) {
            while (page->channels().size() > 1)
                page->removeChannel(page->channels().size() - 1);
        }

        ChannelState def;
        for (int i = 0; i < page->channels().size(); ++i) {
            auto *ch = page->channels().at(i);
            ChannelState cur = ch->state();
            if (rstVol)   { cur.volumeLocal = def.volumeLocal; cur.volumeRemote = def.volumeRemote; }
            if (rstFx)    { cur.pitch = def.pitch; cur.speed = def.speed; cur.reverb = def.reverb; cur.fxSync = def.fxSync; }
            // Reset ALWAYS clears the preloaded audio (same effect as
            // clicking the X next to the filename). Mirror the X
            // handler's cleanup so the waveform / filename / replay
            // glyph all disappear instantly and per-slot context
            // (last-played, slot->button map) drops.
            cur.filename.clear();
            if (rstSbx)   { cur.sandbox = SandboxState(); }
            s_pendingHardClear.insert(i);
            ch->applyState(cur);
            auto *wave = ch->waveform();
            if (wave) {
                wave->setPlaying(false);
                wave->setReplayReady(false);
                wave->setFilename(QString());
                wave->clearPlayback();
            }
            s_lastPlayedCtx.remove(i);
            s_slotToBtnIdx.remove(i);
            if (i == 0) {
                ch->setTitle(QObject::tr("Channel 1"));
                ChannelStatePersistence::saveName(0, ch->title());
            }
            if (sampler && rstSbx) sampler->clearSlotSandbox(i);
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
    // Per-channel UI settings propagate onto every channel that is added
    // AFTER the wire step (session restore + all subsequent user adds).
    // Without this, verticalMeter / showSkipButtons live only inside the
    // SettingsWindow widget - never applied to the actual channel row -
    // so the user saw "the settings did not survive the restart" (the
    // model + widget round-trip worked, only the visible propagation
    // was skipped).
    QObject::connect(page, &MainPage::channelAdded, page, [page, model](int idx){
        if (auto *ch = page->channelAt(idx)) {
            ch->setMeterVertical(model->getVerticalMeter());
            ch->setSkipButtonsVisible(model->getShowSkipButtons());
            ch->waveform()->setAdaptToFx(model->getAdaptWaveformToFx());
            ch->waveform()->setShowCropMarkers(model->getShowCropMarkers());
            ch->waveform()->setSpectrogramView(model->getSpectrogramView());
            ch->setFxVisible(model->getGlobalFxEnabled());
            ch->setWaveformVisible(!model->getHideWaveform());
            ch->setMeterVisible(model->getAudioMeterVisible());
            ch->setExportVisible(model->getAudioExportEnabled());
            ch->setSandboxFeatureEnabled(model->getAudioSandboxEnabled());
            ch->setVinylButtonVisible(model->getShowVinylButton());
        }
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
        sampler->setPitchFactor(AudioUtils::sliderToPitchFactor(model->getPitchValue() ));
        sampler->setSpeedFactor(AudioUtils::sliderToPitchFactor(model->getSpeedValue() ));
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

    // Main-page rows / cols selectors: bidirectionally bound to the
    // model. Initial values come from the model; user edits update it
    // (which in turn fires NOTIFY_SET_ROWS / NOTIFY_SET_COLS, debounced
    // for grid rebuild). QSignalBlocker keeps the model->spin push
    // from re-firing the spin->model edit handler.
    if (auto *rs = page->rowsSpin()) {
        QSignalBlocker b(rs);
        rs->setValue(model->getRows());
    }
    if (auto *cs = page->colsSpin()) {
        QSignalBlocker b(cs);
        cs->setValue(model->getCols());
    }
    QObject::connect(page->rowsSpin(), QOverload<int>::of(&QSpinBox::valueChanged),
                     [model](int v){ model->setRows(v); });
    QObject::connect(page->colsSpin(), QOverload<int>::of(&QSpinBox::valueChanged),
                     [model](int v){ model->setCols(v); });

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

    // Pause-all is a tristate now:
    //   * Pause / Resume all - the classic toggle, when at least one
    //     channel is actively playing or paused.
    //   * Replay all          - when every channel is stopped but at
    //     least one still has a last-played sound on record. Click
    //     restarts each replay-ready channel from its parked cursor.
    //   * Disabled            - nothing playing, nothing replayable.
    auto *pauseBtn = page->pauseAllBtn();
    pauseBtn->setCheckable(false);    // we drive the icon manually now

    enum class PauseAllMode { Idle, Pause, Resume, Replay };
    auto modeProp = std::make_shared<int>(0);

    auto computeMode = [sampler, page, model]() -> PauseAllMode {
        if (!sampler) return PauseAllMode::Idle;
        bool anyPlaying = false, anyPaused = false;
        const int n = page->channels().size();
        for (int i = 0; i < n; ++i) {
            auto st = sampler->getState(i);
            if (st == Sampler::ePLAYING)      anyPlaying = true;
            else if (st == Sampler::ePAUSED)  anyPaused  = true;
        }
        if (anyPlaying) return PauseAllMode::Pause;
        if (anyPaused)  return PauseAllMode::Resume;
        // Replay mode is OPTIONAL. When OFF, channels are wiped on
        // stop so s_lastPlayedCtx is always empty for non-active
        // slots — but defend against any stale entries anyway by
        // gating the Replay branch on the model flag.
        if (model && model->getReplayModeEnabled()
            && !s_lastPlayedCtx.isEmpty()) return PauseAllMode::Replay;
        return PauseAllMode::Idle;
    };
    auto applyMode = [pauseBtn, modeProp](PauseAllMode m){
        *modeProp = static_cast<int>(m);
        switch (m) {
        case PauseAllMode::Pause:
            pauseBtn->setEnabled(true);
            pauseBtn->setText(QObject::tr("Pause all"));
            pauseBtn->setIcon(IconFactory::pause());
            pauseBtn->setToolTip(QObject::tr("Pause every active channel"));
            break;
        case PauseAllMode::Resume:
            pauseBtn->setEnabled(true);
            pauseBtn->setText(QObject::tr("Resume all"));
            pauseBtn->setIcon(IconFactory::play());
            pauseBtn->setToolTip(QObject::tr("Resume every paused channel"));
            break;
        case PauseAllMode::Replay:
            pauseBtn->setEnabled(true);
            pauseBtn->setText(QObject::tr("Replay all"));
            pauseBtn->setIcon(IconFactory::reload());
            pauseBtn->setToolTip(QObject::tr(
                "Replay every channel that still has a sound loaded"));
            break;
        case PauseAllMode::Idle:
            pauseBtn->setEnabled(false);
            pauseBtn->setText(QObject::tr("Pause all"));
            pauseBtn->setIcon(IconFactory::pause());
            pauseBtn->setToolTip(QObject::tr(
                "No active or replayable channels"));
            break;
        }
    };
    auto refreshPauseAll = [computeMode, applyMode]{
        applyMode(computeMode());
    };
    refreshPauseAll();

    QObject::connect(pauseBtn, &QPushButton::clicked, pauseBtn,
                     [pauseBtn, sampler, page, model, modeProp, refreshPauseAll]{
        const PauseAllMode m = static_cast<PauseAllMode>(*modeProp);
        if (!sampler) return;
        switch (m) {
        case PauseAllMode::Pause:
            sampler->pausePlayback(-1);
            break;
        case PauseAllMode::Resume:
            sampler->unpausePlayback(-1);
            break;
        case PauseAllMode::Replay:
            for (auto it = s_lastPlayedCtx.begin();
                 it != s_lastPlayedCtx.end(); ++it) {
                const int slot   = it.key();
                const int btnIdx = it->btnIdx;
                if (slot < 0 || slot >= page->channels().size()) continue;
                if (btnIdx < 0) continue;
                const SoundInfo *info = model->getSoundInfo(btnIdx);
                if (!info || info->filename.isEmpty()
                    || info->filename != it->filename) continue;
                sampler->stopPlayback(slot);
                if (!sampler->playSoundInSlot(slot, *info, false)) continue;
                s_slotToBtnIdx[slot] = btnIdx;
                auto *ch = page->channels().at(slot);
                sampler->setSlotVolumeLocal (slot, ch->volume()->local());
                sampler->setSlotVolumeRemote(slot, ch->volume()->remote());
                const bool globalFx = model->getGlobalFxEnabled();
                if (globalFx && info->fxRemember) {
                    sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(info->fxPitch));
                    sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(info->fxSpeed));
                    sampler->setSlotReverbMix  (slot, info->fxReverb / 100.0f);
                } else if (globalFx) {
                    sampler->setSlotPitchFactor(slot, AudioUtils::sliderToPitchFactor(ch->fx()->pitch()));
                    sampler->setSlotSpeedFactor(slot, AudioUtils::sliderToPitchFactor(ch->fx()->speed()));
                    sampler->setSlotReverbMix  (slot, ch->fx()->reverb() / 100.0f);
                } else {
                    sampler->setSlotPitchFactor(slot, 1.0f);
                    sampler->setSlotSpeedFactor(slot, 1.0f);
                    sampler->setSlotReverbMix  (slot, 0.0f);
                }
            }
            break;
        case PauseAllMode::Idle:
            break;
        }
        refreshPauseAll();
    });

    if (sampler) {
        QObject::connect(sampler, &Sampler::onStartPlaying, pauseBtn,
                         [refreshPauseAll](int, bool, QString){ refreshPauseAll(); },
                         Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onStopPlaying, pauseBtn,
                         [refreshPauseAll](int){ refreshPauseAll(); },
                         Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onPausePlaying, pauseBtn,
                         [refreshPauseAll](int){ refreshPauseAll(); },
                         Qt::QueuedConnection);
        QObject::connect(sampler, &Sampler::onUnpausePlaying, pauseBtn,
                         [refreshPauseAll](int){ refreshPauseAll(); },
                         Qt::QueuedConnection);
    }
    // Belt-and-braces 500 ms tick. Catches replay-state changes that
    // don't flow through Sampler signals (clearRequested handler, cell
    // unbind detected at replay time, etc.).
    auto *pauseAllTimer = new QTimer(pauseBtn);
    pauseAllTimer->setInterval(500);
    QObject::connect(pauseAllTimer, &QTimer::timeout, pauseBtn, refreshPauseAll);
    pauseAllTimer->start();

    QObject::connect(page->stopAllBtn(), &QPushButton::clicked,
                     pauseBtn, [sampler, page, refreshPauseAll]{
        // Stop All acts as a global red-stop click for every infinity
        // auto channel: mark them all pendingRemove so onStopPlaying
        // auto-removes them even if loop / reverse is on.
        for (auto *ch : page->channels()) {
            if (ch && ch->property("infinityAuto").toBool())
                ch->setProperty("infinityPendingRemove", true);
        }
        if (sampler) sampler->stopPlayback(-1);
        refreshPauseAll();
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
                    sampler->setSlotPitchFactor (i, AudioUtils::sliderToPitchFactor(st.pitch));
                    sampler->setSlotSpeedFactor (i, AudioUtils::sliderToPitchFactor(st.speed));
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

    // ~30 Hz playback position poll for waveform overlay + time labels.
    // Lower than the old 60 Hz: at 60 Hz, the loop took 2 mutex-bearing
    // getPosition / getLength calls + a forced widget update PER channel
    // PER tick. With N idle channels that was 120N locks/sec + 60N
    // queued paint events fighting scroll repaints on the GUI thread -
    // a measurable system-wide lag (worse with the Leia engine, which
    // also holds m_mutex inside the audio block). 30 Hz still feels
    // smooth for the cursor and halves the GUI thread workload.
    //
    // Silent slots are skipped via the lock-free getState() atomic;
    // clearPlayback() is only emitted on the playing -> silent edge,
    // so an idle channel costs one atomic load per tick. Active slots
    // keep the original mutex-bearing pos/length read.
    if (sampler) {
        auto wasActive = std::make_shared<QVector<bool>>();
        // Edge-tracked "this slot is currently buffering" bitset, so the loading
        // marquee is toggled only on change (a network stream feeding recovery
        // silence after a stall shows the "caricando" strip; cleared on resume).
        auto bufActive = std::make_shared<QVector<bool>>();
        // Rising-edge tracker for the "network error" toast (a stream that could
        // not recover a stall at the seek target and fell back to restart-from-
        // start). Shown once per failure, not every 33 ms.
        auto failActive = std::make_shared<QVector<bool>>();
        auto *posTimer = new QTimer(page);
        posTimer->setInterval(33);
        QObject::connect(posTimer, &QTimer::timeout, page, [page, sampler, model, wasActive, bufActive, failActive]{
            // Skip entire iteration when the GUI is hidden OR nothing
            // is playing - both are user-visible criteria for "no work
            // needed". Without the anyPlaying gate the timer kept
            // iterating channels + calling Sampler::getState on an
            // idle slot every 33 ms even when no audio was running.
            if (!page->isVisible()) return;
            if (!sampler->anyPlaying()) return;
            const int n = page->channels().size();
            if (wasActive->size() < n) wasActive->resize(n);
            if (bufActive->size() < n) bufActive->resize(n);
            if (failActive->size() < n) failActive->resize(n);
            for (int i = 0; i < n; ++i) {
                Sampler::state_e st = sampler->getState(i);
                if (st == Sampler::ePLAYING_PREVIEW) continue;
                bool active = (st == Sampler::ePLAYING || st == Sampler::ePAUSED);
                auto *ch = page->channels().at(i);
                if (!active) {
                    if ((*bufActive)[i]) { (*bufActive)[i] = false; ch->setStreamLoading(false); }
                    (*failActive)[i] = false;
                    // A silent slot has no in-flight seek: drop a stale
                    // cursor lock so the next playback's cursor moves.
                    ch->setProperty("pendingSeekActive", false);
                    if ((*wasActive)[i]) {
                        // Replay UX: KEEP the waveform / filename / time
                        // label after the playing -> silent transition.
                        // onStopPlaying already snapped the cursor + set
                        // the channel to replay-ready, so calling
                        // clearPlayback() here would wipe the visual
                        // context the user is about to act on (the X
                        // button + the reload icon both rely on the
                        // filename still being visible).
                        (*wasActive)[i] = false;
                    }
                    continue;
                }
                (*wasActive)[i] = true;
                // Network stream recovering from a stall (e.g. a forward seek):
                // show the "caricando" marquee while it feeds silence, hide it
                // when audio resumes. Edge-tracked so we don't re-toggle every
                // 33 ms. Only affects streams (local files never buffer).
                bool buffering = sampler->getSlotNetBuffering(i);
                if (buffering != (*bufActive)[i]) {
                    (*bufActive)[i] = buffering;
                    if (buffering)
                        ch->setStreamLoadingText(QObject::tr("Buffering stream…"));
                    ch->setStreamLoading(buffering);
                }
                // Network stall the decoder could not recover on its own
                // (in-place reopen + restart-and-scan both exhausted). Before
                // showing an error, try ONE automatic reconnect per minute:
                // re-resolve the page URL through the streaming engine (the
                // direct CDN URL may simply have expired) and resume at the
                // position where playback died. The error toast is reserved
                // for the case where even that fails — so the user only ever
                // sees an error when something is genuinely broken.
                bool failed = sampler->getSlotNetFailed(i);
                if (failed && !(*failActive)[i]) {
                    const QString pageUrl = s_slotStreamUrl.value(i);
                    const bool hasPlaylist = !s_slotPlaylistPanel.value(i).isNull();
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    if (!pageUrl.isEmpty() && !hasPlaylist
                        && nowMs - s_slotNetRetryMs.value(i, 0) > 60000) {
                        s_slotNetRetryMs[i] = nowMs;
                        const double resumePos = sampler->getPosition(i);
                        StreamResolver::instance().invalidate(pageUrl);
                        // One-shot: once the reconnected stream actually
                        // starts, jump back to where playback died.
                        auto *once = new QObject(page);
                        QObject::connect(sampler, &Sampler::onStartPlaying, once,
                            [sampler, i, resumePos, once](int startedSlot, bool preview, QString){
                                if (preview || startedSlot != i) return;
                                if (resumePos > 3.0) sampler->seek(resumePos, i);
                                once->deleteLater();
                            });
                        // Safety GC: if the reconnect never starts (resolve
                        // failed → toast already shown), drop the hook.
                        QTimer::singleShot(30000, once, &QObject::deleteLater);
                        loadStreamIntoSlot(page, sampler, model, i, pageUrl,
                                           ch->showingStreamLink(), /*autoPlay*/true,
                                           /*keepPlaylist*/true, /*isAutoRetry*/true);
                    } else {
                        // Genuinely broken (reconnect budget exhausted, or a
                        // playlist slot — there the autoplay advance is the
                        // recovery and a reconnect would race it into a
                        // double load): surface the error.
                        showStreamErrorBubble(ch, QObject::tr(
                            "Network error — this stream could not be played. "
                            "Try again in a moment."));
                    }
                }
                (*failActive)[i] = failed;
                // Skip the cursor refresh while a debounced seek is
                // pending for this channel: the click handler snapped
                // the cursor to the click target; reading the live
                // decoder position here (which still reflects the
                // PRE-seek playhead until the debounce fires
                // Sampler::seek) would reset the cursor backward to
                // the audible position and make the click look
                // ignored. The label also follows the pending target
                // so the displayed time matches what the user is
                // about to seek to.
                bool seekPending = ch->property("pendingSeekActive")
                                       .toBool();
                double pos = sampler->getPosition(i);
                double len = sampler->getLength(i);
                if (len > 0.0) {
                    if (seekPending) {
                        double tgt = ch->property("pendingSeekSec")
                                         .toDouble();
                        ch->waveform()->setPosition(tgt, len);
                    } else {
                        ch->waveform()->setPlaybackFraction(pos / len);
                        ch->waveform()->setPosition(pos, len);
                    }
                } else if (!ch->waveform()->isReplayReady()) {
                    // No length AND not in replay-ready mode -> the slot
                    // genuinely has no sound loaded. Safe to wipe.
                    ch->waveform()->clearPlayback();
                    (*wasActive)[i] = false;
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
        QObject::connect(ch, &Channel::sandboxStateChanged, [sampler, page, model](int slot, const SandboxState &s){
            if (sampler) sampler->setSlotSandboxState(slot, s);
            if (model->getAdaptWaveformToFx() && slot >= 0 && slot < page->channels().size())
                page->channels().at(slot)->waveform()->setSandboxState(s);
        });
        QObject::connect(ch, &Channel::sandboxResetRequested, [sampler, model, page](int slot){
            extremeLog("sandboxResetRequested slot=%d resetChVol=%d resetChFx=%d resetChFile=%d resetChSbx=%d",
                       slot,
                       model->getResetChVolume() ? 1 : 0,
                       model->getResetChFx()     ? 1 : 0,
                       model->getResetChFile()   ? 1 : 0,
                       model->getResetChSandbox()? 1 : 0);
            if (sampler) sampler->clearSlotSandbox(slot);
            if (slot >= 0 && slot < page->channels().size()) {
                auto *rch = page->channels().at(slot);
                ChannelState cur = rch->state();
                ChannelState def;
                if (model->getResetChVolume()) { cur.volumeLocal = def.volumeLocal; cur.volumeRemote = def.volumeRemote; }
                if (model->getResetChFx())     { cur.pitch = def.pitch; cur.speed = def.speed; cur.reverb = def.reverb; cur.fxSync = def.fxSync; }
                // When the "Stop playback + clear loaded audio" checkbox is on
                // the per-channel reset must mirror the X-button cleanup
                // (filename wipe + waveform reset + replay glyph gone +
                // slot bookkeeping cleared). Without these explicit
                // calls the slot kept playing the old sound because
                // applyState() only restores widget state — it does not
                // talk to the sampler. checkbox OFF = old behaviour
                // (filename retained, audio keeps playing).
                if (model->getResetChFile()) {
                    extremeLog("  -> hard wipe: stopPlayback + clear file slot=%d", slot);
                    s_pendingHardClear.insert(slot);
                    if (sampler) sampler->stopPlayback(slot);
                    cur.filename.clear();
                    auto *wave = rch->waveform();
                    if (wave) {
                        wave->setPlaying(false);
                        wave->setReplayReady(false);
                        wave->setFilename(QString());
                        wave->clearPlayback();
                    }
                    s_lastPlayedCtx.remove(slot);
                    s_slotToBtnIdx.remove(slot);
                }
                cur.sandbox = SandboxState();
                rch->applyState(cur);
            }
        });
        // Dedicated "Save audio" button on stream channels (separate from the
        // DSP "Export audio" — see startStreamDownloadFlow).
        QObject::connect(ch, &Channel::downloadRequested, page, [page](int slot){
            startStreamDownloadFlow(page, slot);
        });
        QObject::connect(ch, &Channel::exportRequested, page, [page, model](int slot){
            // slot here is Channel::m_id which equals the channel's
            // index in MainPage::channels() so long as channels are
            // appended (current behaviour). channelAt(slot) double-
            // guards against the case where the channel was removed
            // between click + slot-dispatch.
            auto *src_ch = page->channelAt(slot);
            if (!src_ch) return;
            // Stream channel: "Export audio" is a TRUE DSP bake here too —
            // download the source to a temp copy, then encode it through the
            // channel's fx + sandbox chain (startStreamExportFlow). "Save
            // audio" (the green button) remains the plain source download.
            if (s_slotStreamUrl.contains(slot)) {
                startStreamExportFlow(page, model, slot);
                return;
            }
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
            // Multi-format export. The selected filter is reported back
            // by QFileDialog so we can append the right extension if the
            // user didn't type one. AudioExporter detects the actual
            // format from the final filename and encodes accordingly.
            QString selectedFilter;
            const QString filters =
                QObject::tr("WAV (PCM 16-bit) (*.wav);;FLAC (lossless) (*.flac);;"
                            "OGG Vorbis (*.ogg);;AAC / M4A (*.m4a);;All files (*.*)");
            QString dst = QFileDialog::getSaveFileName(page,
                QObject::tr("Export audio with DSP"),
                QString(), filters, &selectedFilter);
            if (dst.isEmpty()) return;
            // If the filename has no extension matching one of the
            // supported formats, append the one implied by the selected
            // filter (defaulting to WAV).
            auto endsWithI = [&](const QString &s, const char *ext) {
                return dst.endsWith(QString::fromLatin1(ext), Qt::CaseInsensitive);
            };
            if (!(endsWithI(dst, ".wav") || endsWithI(dst, ".flac") ||
                  endsWithI(dst, ".ogg") || endsWithI(dst, ".oga") ||
                  endsWithI(dst, ".m4a") || endsWithI(dst, ".mp4") ||
                  endsWithI(dst, ".aac")))
            {
                if      (selectedFilter.contains(".flac")) dst += QStringLiteral(".flac");
                else if (selectedFilter.contains(".ogg"))  dst += QStringLiteral(".ogg");
                else if (selectedFilter.contains(".m4a"))  dst += QStringLiteral(".m4a");
                else                                       dst += QStringLiteral(".wav");
            }
            // Snapshot the channel's LIVE settings - same factor scaling
            // the sampler slot uses (3^(slider/100)) so the exported WAV
            // matches the audible signal. Sandbox state is taken by
            // value so subsequent slider tweaks during the export don't
            // mutate the bake.
            const float pitchFactor  = AudioUtils::sliderToPitchFactor(src_ch->fx()->pitch() );
            const float speedFactor  = AudioUtils::sliderToPitchFactor(src_ch->fx()->speed() );
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
        ch->waveform()->setAdaptToFx(model->getAdaptWaveformToFx());
        ch->waveform()->setShowCropMarkers(model->getShowCropMarkers());
        ch->waveform()->setStreamGradientEnabled(model->getStreamFxGradient());
        ch->waveform()->setStreamGradientStyle(QColor(model->getWaveAnimColorA()),
                                               QColor(model->getWaveAnimColorB()),
                                               model->getWaveAnimSpeed(),
                                               model->getWaveAnimIntensity());
        ch->waveform()->setFormatBadgeMode(model->getFormatBadgeMode());
        ch->waveform()->setStreamBadgeEnabled(model->getShowStreamBadge());
        if (model->getAdaptWaveformToFx()) {
            ch->waveform()->setSandboxState(ch->sandboxState());
            // Seed the live FxPanel state too so the waveform reflects
            // the channel's CURRENT pitch/speed/reverb the moment the
            // adaptive view is turned on, instead of waiting for the
            // user to nudge a slider before any visualisation appears.
            ch->waveform()->setLiveFx(ch->fx()->pitch(),
                                       ch->fx()->speed(),
                                       ch->fx()->reverb());
        }
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
    // its channel's ChannelMeter widget. ChannelMeter::setPeak handles
    // its own change-detect, so idle channels (peak == hold == 0) cost
    // a setPeak comparison but skip the actual paint. Pumping every
    // tick is required for the peak-hold marker animation to decay
    // back to floor after a sound stops - the earlier single-drain
    // pattern froze the marker mid-decay once playback ended.
    if (sampler) {
        auto *meterTimer = new QTimer(page);
        meterTimer->setInterval(40);
        // drainTicks keeps the loop pumping (0, 0) frames for a short
        // window AFTER anyPlaying() returns false, so per-channel meter
        // peak-hold AND the per-band EQ LED bars finish decaying to
        // zero. Without this the early return on !anyPlaying() froze
        // every visual at its last-frame value the instant the user
        // stopped the last sound. 75 ticks @ 40 ms = 3 s, enough for
        // the slowest decay (EqBandWidget release coefficient 0.10)
        // to reach ~99 % settled. Counter is reset to 75 every tick
        // while anyPlaying is true, so any new playback restarts the
        // window from scratch.
        QObject::connect(meterTimer, &QTimer::timeout, page, [page, sampler, model, drainTicks = 0]() mutable {
            if (!model->getAudioMeterVisible()) return;
            if (!page->isVisible()) return;
            if (sampler->anyPlaying()) {
                drainTicks = 75;
            } else {
                if (drainTicks <= 0) return;
                --drainTicks;
            }
            const int n = page->channels().size();
            for (int i = 0; i < n; ++i) {
                Sampler::state_e st = sampler->getState(i);
                bool active = (st == Sampler::ePLAYING || st == Sampler::ePAUSED);
                float l = 0.0f, r = 0.0f;
                if (active) sampler->getSlotPeak(i, l, r);
                // Silent slots pass through (0, 0). setPeak's hold path
                // keeps decaying multiplicatively until it reaches the
                // floor, then change-detect kills the paint events.
                page->channels().at(i)->setMeterPeak(l, r);
                page->channels().at(i)->pushSandboxLevel(l, r);
                float bands[16];
                sampler->getSlotEqBandLevels(i, bands);
                page->channels().at(i)->pushSandboxEqLevels(bands);
            }
        });
        meterTimer->start();

        // Separate, slow timer pushes the per-channel DSP CPU% into any
        // sandbox dialog that is currently open. 1 Hz tick - the
        // measurement is rolling and the user does not need 60 Hz on a
        // diagnostic readout. Drained when the page is hidden so there
        // is no measurement noise while the soundboard isn't on screen.
        //
        // The previous "!anyPlaying" early-exit made the CPU label
        // FREEZE at the last reading whenever every slot was idle or
        // the user toggled the sandbox off - the label never returned
        // to "<0.1%". Always pushing the current cpuPercent() value
        // (which is 0 at rest because the audio thread accumulated
        // zero frames) keeps the display honest. Channel::pushSandboxCpu
        // is a no-op when the per-channel sandbox dialog isn't open,
        // so the loop is effectively free for the common case.
        auto *cpuTimer = new QTimer(page);
        cpuTimer->setInterval(1000);
        QObject::connect(cpuTimer, &QTimer::timeout, page, [page, sampler](){
            if (!page->isVisible()) return;
            const int n = page->channels().size();
            for (int i = 0; i < n; ++i)
                page->channels().at(i)->pushSandboxCpu(
                    sampler->getSlotCpuPercent(i));
        });
        cpuTimer->start();
    }
}

}
