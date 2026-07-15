// src/modules/stream_resolver.h
//----------------------------------
// GameBaiters Soundboard - v2.3.1 URL / YouTube live-streaming feature.
//
// StreamResolver turns a page URL (youtube.com/watch, youtu.be, /shorts, or any
// http[s] media page yt-dlp supports) into a DIRECT, streamable media URL plus
// the HTTP headers the CDN expects. It shells out to the BUNDLED yt-dlp binary
// (see ytDlpPath) via QProcess, fully async, off the GUI thread, with no console
// window. The tool is invisible to the user: never named in UI, never installed
// by the user, discovered only in the plugin's own asset directory.
//
// The heavy fact that makes this cheap: yt-dlp only RESOLVES (prints the URL) -
// it never downloads media. Our own static FFmpeg (network+schannel, see
// InputFileFFmpeg) then streams the resolved URL with HTTP range requests.
//
// Direct googlevideo URLs are short-lived (~6 h, IP-bound), so results are
// cached by video-ID with a TTL under that; callers persist the CANONICAL page
// URL and re-resolve on play.
//----------------------------------

#pragma once
#ifndef rpsbsrc__stream_resolver_H__
#define rpsbsrc__stream_resolver_H__

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>
#include <QSet>
#include <QDateTime>

class QProcess;

// One resolved stream. isValid() == a usable direct URL was obtained.
struct ResolvedStream
{
	QString   directUrl;          // direct CDN media URL (googlevideo etc.)
	QString   userAgent;          // UA the CDN request must carry
	QString   headers;            // CRLF-joined extra headers (no User-Agent)
	QString   title;              // human title (cell label)
	double    durationSec = -1.0; // total length, <0 = unknown
	bool      isLive = false;     // true = LIVE stream (no seek, no waveform,
	                              // no reverse/vinyl/paulstretch, link-only save)
	QDateTime resolvedAt;         // for TTL expiry

	bool isValid() const { return !directUrl.isEmpty(); }
};

// One playlist entry (flat resolve - only id/title, resolved to a direct URL
// lazily when the user actually loads it).
struct PlaylistEntry
{
	QString pageUrl;              // canonical watch URL to feed resolve()
	QString title;               // human title for the list row
};

class StreamResolver : public QObject
{
	Q_OBJECT
public:
	static StreamResolver &instance();

	// Fresh cached entry for pageUrl, or an invalid ResolvedStream if absent or
	// expired. Never blocks, never spawns.
	ResolvedStream cached(const QString &pageUrl) const;

	// Drop the cached entry for pageUrl so the next resolve() spawns a FRESH
	// yt-dlp run. Used by the automatic stream-reconnect path: a direct URL
	// can die before its TTL (IP change, CDN throttle, expiry), and resuming
	// through the stale cache would just fail again.
	void invalidate(const QString &pageUrl);

	// Stable cache key: the YouTube video id (watch?v=, youtu.be/, /shorts/,
	// /embed/) or, for anything else, the whole URL.
	static QString videoKey(const QString &pageUrl);

	// http:// or https:// prefix (cheap gate before offering to resolve).
	static bool looksLikeUrl(const QString &s);

	// True if the URL points at a playlist (a "list=" query item, or a
	// youtube.com/playlist path). A bare watch?v=...&list=... counts too so the
	// caller can offer "whole playlist or just this video".
	static bool looksLikePlaylist(const QString &s);

	// Preferred audio quality, persisted in QSettings("GameBaiters","Soundboard")
	// under "stream/quality": "best" | "balanced" (default) | "data". Maps to the
	// yt-dlp -f selector used for both resolve and download.
	static QString preferredQuality();
	static void    setPreferredQuality(const QString &q);
	static QString formatSelector();   // -f value derived from preferredQuality()

	// Absolute path to the bundled yt-dlp binary in the plugin asset dir; falls
	// back to the bare name (PATH lookup) if the bundled copy is missing.
	static QString ytDlpPath();

	// PERSISTENT cache dir for the engine (NOT the temp scratch, which is wiped
	// every session). yt-dlp caches the deciphered YouTube player / nsig JS
	// here; with no cache it re-downloads and re-interprets that JS on EVERY
	// resolve, which is the single biggest cause of slow + flaky link loading
	// (worst on macOS, where the engine is a self-extracting bundle to begin
	// with). Small, self-contained, under the app's own cache location.
	static QString cacheDir();

	// Flags every yt-dlp invocation carries: persistent cache, no colour codes,
	// and --ignore-config so a yt-dlp config file the user happens to have on
	// their system (very common on macOS via Homebrew) can never rewrite our
	// format selection or output paths behind our back.
	static QStringList commonArgs();

	// Watchdog budgets. macOS gets far more headroom: yt-dlp_macos is a
	// self-extracting PyInstaller bundle whose cold start alone can take
	// several seconds (plus Gatekeeper on first run), so the old flat 25 s
	// killed legitimate resolves and surfaced them as "couldn't load".
	static int resolveTimeoutMs();
	static int playlistTimeoutMs();

	// Startup routine, once per session: pay the engine's cold-start cost
	// (self-extraction, Gatekeeper scan, page cache) in the background, and run
	// the engine self-update AT MOST ONCE A DAY. It used to run on every single
	// startup — on macOS that meant `-U` could still be rewriting the ~40 MB
	// binary when the user pasted their first link, and the resolve then spawned
	// a half-replaced executable (hang / bogus failure).
	void warmUp();

	// Abort an in-flight self-update. A user action always outranks the
	// updater: a resolve arriving mid-update is queued and, if the update has
	// not finished within a short grace, the update is cancelled so the link
	// loads NOW (the next session will update instead).
	void cancelUpdate();

	// Dedicated scratch dir (under the OS temp) used as the working directory
	// for EVERY yt-dlp process, so any stray file it might write is contained
	// there and never pollutes the user's disk. Created on demand.
	static QString workDir();
	// Wipe the scratch dir. Called on soundboard start AND shutdown so no
	// temporary stream file ever accumulates (a 10-hour video must never land
	// on disk). Safe to call anytime.
	static void cleanTempDir();

	// Async: query the bundled yt-dlp version -> versionReady(). Cheap.
	void queryVersion();
	// Async: run `yt-dlp -U` (self-update). Streams updateStatus() then
	// updateFinished(ok,msg). Non-blocking; the About dialog drives it.
	void updateEngine();

	// Kill EVERY running yt-dlp child (resolve / version / update) and clear
	// state. MUST be called from sb_kill BEFORE Qt teardown so a long-running
	// resolve or an in-flight `-U` can never become a ghost process or hang
	// plugin unload. Safe to call repeatedly.
	void shutdown();

	// Async: fetch the flat list of a playlist's entries -> playlistResolved().
	// On failure emits failed(pageUrl, ...). Never downloads media.
	void resolvePlaylist(const QString &pageUrl);

	// Async: DOWNLOAD (not stream) the best audio of pageUrl to destFile via the
	// bundled engine, re-encoding to the container implied by destFile's
	// extension when needed. Streams downloadProgress(0..100, or -1 unknown) and
	// ends with downloadFinished(ok, message, destFile). Refuses live streams.
	void downloadAudio(const QString &pageUrl, const QString &destFile);
	// Kill any in-flight download started by downloadAudio.
	void cancelDownload();
	// Kill any in-flight resolve OR playlist-resolve process for pageUrl (the
	// user cancelled the load). No signal is emitted for a cancelled request.
	void cancelResolve(const QString &pageUrl);

public slots:
	// Kick an async resolve. On success emits resolved(); on any failure emits
	// failed(). A fresh cache hit emits resolved() on the next event-loop turn
	// without spawning. Concurrent calls for the same key coalesce onto the one
	// in-flight process.
	void resolve(const QString &pageUrl);

signals:
	void resolved(const QString &pageUrl, const ResolvedStream &stream);
	void failed(const QString &pageUrl, const QString &error);
	// Fine-grained resolve stages ("Starting the stream engine…",
	// "Contacting the site…", "Extracting the audio track info…", ...) so
	// the channel's loading strip can narrate what is actually happening
	// instead of sitting on one generic message. Purely informational.
	void resolveProgress(const QString &pageUrl, const QString &stage);
	void versionReady(const QString &version);      // "" = unavailable
	void updateStatus(const QString &line);         // progress line
	void updateFinished(bool ok, const QString &message);
	void playlistResolved(const QString &pageUrl, const QString &playlistTitle,
	                      const QVector<PlaylistEntry> &entries);
	void downloadProgress(const QString &pageUrl, int percent); // -1 = unknown
	void downloadFinished(bool ok, const QString &message, const QString &destFile);

private:
	explicit StreamResolver(QObject *parent = nullptr);
	void startProcess(const QString &pageUrl);
	void finishProcess(QProcess *proc, const QString &pageUrl);
	// Parse whatever the resolve has printed SO FAR. Returns true once a
	// complete, usable JSON object is in `out` — the resolve is then answered
	// immediately, without waiting for the engine process to exit (its own
	// teardown costs another second or more on macOS).
	static bool parseResolveJson(const QByteArray &out, ResolvedStream &s);
	// Detach a finished/answered resolve proc: drop bookkeeping, let it exit on
	// its own and self-delete (killing a self-extracting engine mid-run would
	// leave its extraction dir behind on disk).
	void retireProcess(QProcess *proc);
	// Start every resolve that was parked while the self-update was running.
	void flushQueuedResolves();

	QHash<QString, ResolvedStream> m_cache;     // key = videoKey()
	QHash<QProcess *, QString>     m_inflight;  // running resolve proc -> pageUrl
	QHash<QProcess *, QByteArray>  m_outBuf;    // incremental stdout per resolve
	QHash<QProcess *, QString>     m_playlistInflight; // playlist resolve proc -> url
	QSet<QProcess *>               m_aux;       // running version/update procs
	QProcess *                     m_download = nullptr; // active downloadAudio proc
	QProcess *                     m_updateProc = nullptr; // active `-U` proc
	QStringList                    m_queuedResolves;    // parked during an update
	bool                           m_updating = false;
	bool                           m_warmedUp = false;

	static const qint64 kTtlSec    = 5 * 3600;  // < ~6 h googlevideo expiry
};

#endif // rpsbsrc__stream_resolver_H__
