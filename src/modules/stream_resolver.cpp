// src/modules/stream_resolver.cpp
//----------------------------------
// See stream_resolver.h. Async yt-dlp bridge, hidden + bundled.
//----------------------------------

#include "stream_resolver.h"

#include <QProcess>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QUrl>
#include <QUrlQuery>
#include <QTimer>
#include <QFileInfo>
#include <QStringList>
#include <QTimeZone>
#include <QSettings>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QDir>

#ifdef _WIN32
#include <windows.h>
#endif

// common.h MUST precede plugin.h: it pulls the TeamSpeak SDK typedefs
// (uint64 etc.) that plugin.h's declarations use but does not include itself.
#include "../common.h"   // TS3 SDK typedefs, ts3Functions, PATH_BUFSIZE
#include "../plugin.h"   // getPluginID()
#include "../ts3log.h"   // logInfo / logWarning / extremeLog

//----------------------------------------------------------------
StreamResolver &StreamResolver::instance()
{
	static StreamResolver s_inst;
	return s_inst;
}

StreamResolver::StreamResolver(QObject *parent) : QObject(parent) {}

//----------------------------------------------------------------
bool StreamResolver::looksLikeUrl(const QString &s)
{
	const QString t = s.trimmed();
	return t.startsWith("http://", Qt::CaseInsensitive)
	    || t.startsWith("https://", Qt::CaseInsensitive);
}

//----------------------------------------------------------------
bool StreamResolver::looksLikePlaylist(const QString &s)
{
	const QUrl url(s.trimmed());
	const QString host = url.host().toLower();
	if (!host.contains("youtube.com") && !host.contains("youtu.be"))
		return false;
	if (url.path().startsWith("/playlist")) return true;
	const QString list = QUrlQuery(url).queryItemValue("list");
	// A "Radio"/"Mix" pseudo-playlist (list=RD...) is per-video and endless -
	// treat it as a single video, not a real playlist the user curated.
	if (list.isEmpty()) return false;
	if (list.startsWith("RD") || list.startsWith("UL")) return false;
	return true;
}

//----------------------------------------------------------------
QString StreamResolver::preferredQuality()
{
	QSettings s("GameBaiters", "Soundboard");
	return s.value("stream/quality", "balanced").toString();
}

void StreamResolver::setPreferredQuality(const QString &q)
{
	QSettings s("GameBaiters", "Soundboard");
	s.setValue("stream/quality", q);
}

QString StreamResolver::formatSelector()
{
	const QString q = preferredQuality();
	if (q == "best")
		return "bestaudio/best";
	if (q == "data")   // data saver: smallest audio-only track
		return "worstaudio[ext=m4a]/worstaudio/bestaudio";
	// balanced (default): m4a/AAC first (seekable mov demuxer), else bestaudio
	return "bestaudio[ext=m4a]/bestaudio";
}

//----------------------------------------------------------------
// Stable cache key: the YouTube 11-char video id when we can find one, else the
// full (trimmed) URL so non-YouTube sources still cache.
QString StreamResolver::videoKey(const QString &pageUrl)
{
	const QUrl url(pageUrl.trimmed());
	const QString host = url.host().toLower();
	const QString path = url.path();

	if (host.contains("youtu.be")) {
		const QString id = path.mid(1); // "/<id>"
		if (!id.isEmpty()) return "yt:" + id.left(11);
	}
	if (host.contains("youtube.com")) {
		if (path.startsWith("/watch")) {
			const QString v = QUrlQuery(url).queryItemValue("v");
			if (!v.isEmpty()) return "yt:" + v.left(11);
		}
		// /shorts/<id>, /embed/<id>, /v/<id>
		const QStringList parts = path.split('/', Qt::SkipEmptyParts);
		if (parts.size() >= 2 &&
		    (parts[0] == "shorts" || parts[0] == "embed" || parts[0] == "v"))
			return "yt:" + parts[1].left(11);
	}
	return "url:" + pageUrl.trimmed();
}

//----------------------------------------------------------------
QString StreamResolver::ytDlpPath()
{
	QString dir;
	{
		char buf[PATH_BUFSIZE] = {0};
		ts3Functions.getPluginPath(buf, PATH_BUFSIZE, getPluginID());
		dir = QString::fromUtf8(buf);
	}
	if (!dir.isEmpty() && !dir.endsWith('/') && !dir.endsWith('\\'))
		dir += '/';
	dir += "rp_soundboard/";

#if defined(_WIN32)
	const QString exe = dir + "yt-dlp.exe";
	const QString fallback = "yt-dlp.exe";
#elif defined(__APPLE__)
	const QString exe = dir + "yt-dlp_macos";
	const QString fallback = "yt-dlp";
#else
	const QString exe = dir + "yt-dlp";
	const QString fallback = "yt-dlp";
#endif
	if (QFileInfo::exists(exe))
		return exe;
	return fallback; // last-ditch: let the OS resolve it on PATH
}

//----------------------------------------------------------------
QString StreamResolver::workDir()
{
	QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
	if (base.isEmpty()) base = QDir::tempPath();
	QString dir = base + "/gbsb_stream";
	QDir().mkpath(dir);
	return dir;
}

//----------------------------------------------------------------
void StreamResolver::cleanTempDir()
{
	QDir d(workDir());
	if (!d.exists()) return;
	int n = 0;
	const QFileInfoList entries = d.entryInfoList(
		QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
	for (const QFileInfo &fi : entries) {
		if (fi.isDir()) { QDir(fi.absoluteFilePath()).removeRecursively(); ++n; }
		else            { if (QFile::remove(fi.absoluteFilePath())) ++n; }
	}
	if (n > 0) logInfo("[stream] temp cleaner removed %d item(s)", n);
}

//----------------------------------------------------------------
ResolvedStream StreamResolver::cached(const QString &pageUrl) const
{
	const QString key = videoKey(pageUrl);
	auto it = m_cache.constFind(key);
	if (it == m_cache.constEnd() || !it->isValid())
		return ResolvedStream();
	if (it->resolvedAt.secsTo(QDateTime::currentDateTime()) > kTtlSec)
		return ResolvedStream(); // expired
	return *it;
}

//----------------------------------------------------------------
void StreamResolver::resolve(const QString &pageUrl)
{
	const QString url = pageUrl.trimmed();
	if (!looksLikeUrl(url)) {
		emit failed(url, tr("Not a valid link."));
		return;
	}

	// Fresh cache hit -> emit on the next event-loop turn (keep the API async so
	// callers can always assume a signal, never an inline call).
	ResolvedStream hit = cached(url);
	if (hit.isValid()) {
		QTimer::singleShot(0, this, [this, url, hit]() {
			emit resolved(url, hit);
		});
		return;
	}

	// Coalesce: a process for this exact page URL already running -> let it be,
	// it will emit resolved()/failed() for the same url.
	if (m_inflight.values().contains(url))
		return;

	startProcess(url);
}

//----------------------------------------------------------------
void StreamResolver::startProcess(const QString &pageUrl)
{
	QProcess *proc = new QProcess(this);

#ifdef _WIN32
	// No cmd-window flash. yt-dlp is a console app; without this a black box
	// blinks on every resolve.
	proc->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments *args) {
			args->flags |= CREATE_NO_WINDOW;
		});
#endif

	const QStringList args = {
		"-f", formatSelector(),               // quality from settings
		"--no-playlist",
		"--no-warnings",
		"--no-progress",
		"--no-cache-dir",                     // never write a yt-dlp cache tree
		"--skip-download",                    // RESOLVE only — never touch disk
		"--no-write-info-json", "--no-write-thumbnail", "--no-write-playlist-metafiles",
		// One line of JSON with exactly the fields we need; url respects -f.
		"--print", "%(.{title,duration,is_live,url,http_headers})j",
		pageUrl
	};

	proc->setWorkingDirectory(workDir());   // contain any stray write

	logInfo("[stream] resolving: %s", pageUrl.toUtf8().constData());
	extremeLog("[stream] yt-dlp %s \"%s\"", args.mid(0, args.size() - 1).join(' ').toUtf8().constData(),
	           pageUrl.toUtf8().constData());

	m_inflight.insert(proc, pageUrl);

	// Watchdog: kill a hung resolve so a dead URL fails instead of leaking.
	QTimer *wd = new QTimer(proc);
	wd->setSingleShot(true);
	connect(wd, &QTimer::timeout, proc, [proc]() {
		if (proc->state() != QProcess::NotRunning)
			proc->kill();
	});
	wd->start(kTimeoutMs);

	connect(proc, &QProcess::errorOccurred, this,
		[this, proc, pageUrl](QProcess::ProcessError e) {
			if (e == QProcess::FailedToStart) {
				m_inflight.remove(proc);
				proc->deleteLater();
				emit failed(pageUrl, tr("Streaming engine unavailable."));
			}
		});

	connect(proc,
		static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		this, [this, proc, pageUrl](int, QProcess::ExitStatus) {
			finishProcess(proc, pageUrl);
		});

	proc->start(ytDlpPath(), args);
}

//----------------------------------------------------------------
void StreamResolver::finishProcess(QProcess *proc, const QString &pageUrl)
{
	m_inflight.remove(proc);
	const QByteArray out = proc->readAllStandardOutput();
	const QByteArray err = proc->readAllStandardError();
	const int code = proc->exitCode();
	proc->deleteLater();

	if (code != 0 || out.trimmed().isEmpty()) {
		logWarning("[stream] resolve failed (exit %d): %s", code,
		           pageUrl.toUtf8().constData());
		if (!err.trimmed().isEmpty())
			extremeLog("[stream] yt-dlp stderr: %s", err.trimmed().constData());
		emit failed(pageUrl, tr("Couldn't load this link."));
		return;
	}

	// yt-dlp prints one JSON object line (may be preceded by stray lines on odd
	// extractors -> scan for the first '{').
	int brace = out.indexOf('{');
	QJsonParseError perr;
	QJsonDocument doc = (brace >= 0)
		? QJsonDocument::fromJson(out.mid(brace), &perr)
		: QJsonDocument();
	if (!doc.isObject()) {
		emit failed(pageUrl, tr("Couldn't load this link."));
		return;
	}

	const QJsonObject o = doc.object();
	ResolvedStream s;
	s.directUrl = o.value("url").toString();
	s.title     = o.value("title").toString();
	// duration may be int, double or null
	const QJsonValue dur = o.value("duration");
	s.durationSec = dur.isDouble() ? dur.toDouble() : -1.0;
	// Live detection: is_live is the authoritative flag; fall back to an
	// unknown-duration HLS manifest (m3u8) which is effectively live/unseekable.
	s.isLive = o.value("is_live").toBool(false);
	if (!s.isLive && s.durationSec <= 0.0 && s.directUrl.contains(".m3u8"))
		s.isLive = true;

	if (s.directUrl.isEmpty()) {
		emit failed(pageUrl, tr("Couldn't load this link."));
		return;
	}

	// Flatten http_headers -> User-Agent + CRLF-joined remainder. Strip headers
	// that would fight FFmpeg's own transport: Accept-Encoding (we build FFmpeg
	// without zlib, so a gzip'd response is undecodable) and Range (FFmpeg
	// issues its own range requests).
	const QJsonObject hdrs = o.value("http_headers").toObject();
	QStringList extra;
	for (auto it = hdrs.constBegin(); it != hdrs.constEnd(); ++it) {
		const QString k = it.key();
		const QString v = it.value().toString();
		if (k.compare("User-Agent", Qt::CaseInsensitive) == 0) {
			s.userAgent = v;
			continue;
		}
		if (k.compare("Accept-Encoding", Qt::CaseInsensitive) == 0) continue;
		if (k.compare("Range",           Qt::CaseInsensitive) == 0) continue;
		extra << (k + ": " + v);
	}
	if (!extra.isEmpty())
		s.headers = extra.join("\r\n") + "\r\n";

	s.resolvedAt = QDateTime::currentDateTime();
	m_cache.insert(videoKey(pageUrl), s);

	logInfo("[stream] resolved: '%s' (%.0fs%s) <- %s",
	        s.title.toUtf8().constData(), s.durationSec,
	        s.isLive ? ", LIVE" : "",
	        pageUrl.toUtf8().constData());
	extremeLog("[stream] direct url: %s", s.directUrl.toUtf8().constData());

	emit resolved(pageUrl, s);
}

//----------------------------------------------------------------
// Spawn an auxiliary yt-dlp process (version / update) with no console window
// and a watchdog. `args` already includes the subcommand.
static QProcess *makeHidden(QObject *owner)
{
	QProcess *proc = new QProcess(owner);
#ifdef _WIN32
	proc->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments *a){ a->flags |= CREATE_NO_WINDOW; });
#endif
	proc->setWorkingDirectory(StreamResolver::workDir());  // contain stray writes
	return proc;
}

//----------------------------------------------------------------
void StreamResolver::queryVersion()
{
	QProcess *proc = makeHidden(this);
	m_aux.insert(proc);

	QTimer *wd = new QTimer(proc);
	wd->setSingleShot(true);
	connect(wd, &QTimer::timeout, proc, [proc]{
		if (proc->state() != QProcess::NotRunning) proc->kill();
	});
	wd->start(kTimeoutMs);

	connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart) {
			m_aux.remove(proc);
			proc->deleteLater();
			logWarning("[stream] yt-dlp version query: engine unavailable");
			emit versionReady(QString());
		}
	});
	connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		this, [this, proc](int code, QProcess::ExitStatus){
			m_aux.remove(proc);
			QString v = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
			proc->deleteLater();
			if (code != 0) v.clear();
			logInfo("[stream] yt-dlp version: %s",
			        v.isEmpty() ? "(unavailable)" : v.toUtf8().constData());
			emit versionReady(v);
		});

	proc->start(ytDlpPath(), QStringList{ "--version", "--no-cache-dir" });
}

//----------------------------------------------------------------
void StreamResolver::updateEngine()
{
	QProcess *proc = makeHidden(this);
	m_aux.insert(proc);
	logInfo("[stream] yt-dlp self-update starting");
	emit updateStatus(tr("Checking for updates…"));

	QTimer *wd = new QTimer(proc);
	wd->setSingleShot(true);
	connect(wd, &QTimer::timeout, proc, [proc]{
		if (proc->state() != QProcess::NotRunning) proc->kill();
	});
	wd->start(120000);  // downloads can take a little while (2 min cap)

	connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc]{
		const QString chunk = QString::fromUtf8(proc->readAllStandardOutput());
		const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
		for (const QString &line : lines) {
			logInfo("[stream] update: %s", line.trimmed().toUtf8().constData());
			emit updateStatus(line.trimmed());
		}
	});
	connect(proc, &QProcess::errorOccurred, this, [this, proc](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart) {
			m_aux.remove(proc);
			proc->deleteLater();
			logWarning("[stream] update: engine unavailable");
			emit updateFinished(false, tr("Streaming engine unavailable."));
		}
	});
	connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		this, [this, proc](int code, QProcess::ExitStatus){
			m_aux.remove(proc);
			const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
			const QString er  = QString::fromUtf8(proc->readAllStandardError()).trimmed();
			proc->deleteLater();
			const bool ok = (code == 0);
			logInfo("[stream] yt-dlp update finished (exit %d)", code);
			if (!er.isEmpty()) extremeLog("[stream] update stderr: %s", er.toUtf8().constData());
			emit updateFinished(ok, ok ? (out.isEmpty() ? tr("Up to date.")
			                                             : out.section('\n', -1))
			                           : tr("Update failed."));
		});

	proc->start(ytDlpPath(), QStringList{ "-U", "--no-cache-dir" });
}

//----------------------------------------------------------------
void StreamResolver::resolvePlaylist(const QString &pageUrl)
{
	const QString url = pageUrl.trimmed();
	if (!looksLikeUrl(url)) { emit failed(url, tr("Not a valid link.")); return; }

	QProcess *proc = makeHidden(this);
	m_aux.insert(proc);
	m_playlistInflight.insert(proc, url);   // for cancelResolve(url)

	// --flat-playlist: never touches the videos, only enumerates ids/titles.
	const QStringList args = {
		"--flat-playlist",
		"--skip-download",              // enumerate only — never download
		"--no-warnings",
		"--no-progress",
		"--no-cache-dir",
		"--no-write-info-json", "--no-write-playlist-metafiles",
		"--print", "%(.{id,title,url,webpage_url,playlist_title})j",
		url
	};
	logInfo("[stream] resolving playlist: %s", url.toUtf8().constData());

	QTimer *wd = new QTimer(proc);
	wd->setSingleShot(true);
	connect(wd, &QTimer::timeout, proc, [proc]{
		if (proc->state() != QProcess::NotRunning) proc->kill();
	});
	wd->start(60000);

	connect(proc, &QProcess::errorOccurred, this, [this, proc, url](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart) {
			m_aux.remove(proc); m_playlistInflight.remove(proc); proc->deleteLater();
			emit failed(url, tr("Streaming engine unavailable."));
		}
	});
	connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		this, [this, proc, url](int code, QProcess::ExitStatus){
			m_aux.remove(proc);
			m_playlistInflight.remove(proc);
			const QByteArray out = proc->readAllStandardOutput();
			proc->deleteLater();
			if (code != 0 || out.trimmed().isEmpty()) {
				emit failed(url, tr("Couldn't load this playlist."));
				return;
			}
			QVector<PlaylistEntry> entries;
			QString playlistTitle;
			const QList<QByteArray> lines = out.split('\n');
			for (const QByteArray &ln : lines) {
				const QByteArray t = ln.trimmed();
				if (t.isEmpty() || t[0] != '{') continue;
				QJsonDocument d = QJsonDocument::fromJson(t);
				if (!d.isObject()) continue;
				const QJsonObject o = d.object();
				PlaylistEntry e;
				e.pageUrl = o.value("webpage_url").toString();
				if (e.pageUrl.isEmpty()) {
					const QString u = o.value("url").toString();
					const QString id = o.value("id").toString();
					if (looksLikeUrl(u))      e.pageUrl = u;
					else if (!id.isEmpty())   e.pageUrl = "https://www.youtube.com/watch?v=" + id;
				}
				e.title = o.value("title").toString();
				if (playlistTitle.isEmpty())
					playlistTitle = o.value("playlist_title").toString();
				if (!e.pageUrl.isEmpty()) entries.push_back(e);
			}
			if (entries.isEmpty()) { emit failed(url, tr("Couldn't load this playlist.")); return; }
			logInfo("[stream] playlist '%s': %d entries",
			        playlistTitle.toUtf8().constData(), (int)entries.size());
			emit playlistResolved(url, playlistTitle, entries);
		});

	proc->start(ytDlpPath(), args);
}

//----------------------------------------------------------------
void StreamResolver::downloadAudio(const QString &pageUrl, const QString &destFile)
{
	if (m_download) {
		emit downloadFinished(false, tr("A download is already in progress."), destFile);
		return;
	}
	QProcess *proc = makeHidden(this);
	m_download = proc;

	// Download the bestaudio track DIRECTLY to file — no --extract-audio, so
	// yt-dlp never spawns an external ffmpeg child (which could linger as a
	// ghost on kill, and isn't guaranteed on PATH). The chosen format is
	// audio-only (m4a) so the .m4a file is already a clean playable audio file.
	const QStringList args = {
		"-f", formatSelector(),
		"--no-playlist",
		"--no-warnings",
		"--no-cache-dir",
		"--no-part",
		"--newline",                 // one progress line per update (parseable)
		"-o", destFile,
		"--force-overwrites",
		pageUrl
	};
	logInfo("[stream] download start -> %s", destFile.toUtf8().constData());
	emit downloadProgress(pageUrl, -1);

	connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc, pageUrl]{
		const QString chunk = QString::fromUtf8(proc->readAllStandardOutput());
		static const QRegularExpression pctRe("\\[download\\]\\s+([0-9]+(?:\\.[0-9]+)?)%");
		const QStringList lines = chunk.split('\n', Qt::SkipEmptyParts);
		for (const QString &line : lines) {
			extremeLog("[stream] dl: %s", line.trimmed().toUtf8().constData());
			auto m = pctRe.match(line);
			if (m.hasMatch())
				emit downloadProgress(pageUrl, (int)m.captured(1).toDouble());
		}
	});
	connect(proc, &QProcess::errorOccurred, this, [this, proc, destFile](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart) {
			if (m_download == proc) m_download = nullptr;
			proc->deleteLater();
			emit downloadFinished(false, tr("Streaming engine unavailable."), destFile);
		}
	});
	connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		this, [this, proc, destFile](int code, QProcess::ExitStatus){
			if (m_download == proc) m_download = nullptr;
			const QString er = QString::fromUtf8(proc->readAllStandardError()).trimmed();
			proc->deleteLater();
			const bool ok = (code == 0);
			logInfo("[stream] download finished (exit %d) -> %s", code, destFile.toUtf8().constData());
			if (!ok && !er.isEmpty()) extremeLog("[stream] dl stderr: %s", er.toUtf8().constData());
			emit downloadFinished(ok, ok ? tr("Download complete.") : tr("Download failed."), destFile);
		});

	proc->start(ytDlpPath(), args);
}

//----------------------------------------------------------------
void StreamResolver::cancelResolve(const QString &pageUrl)
{
	const QString url = pageUrl.trimmed();
	if (url.isEmpty()) return;
	auto killMatch = [&](QHash<QProcess *, QString> &m){
		for (auto it = m.begin(); it != m.end(); ) {
			if (it.value() == url) {
				QProcess *p = it.key();
				it = m.erase(it);
				m_aux.remove(p);
				if (p) {
					p->disconnect();   // its finished lambda must not run now
					if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(1500); }
					p->deleteLater();
				}
			} else {
				++it;
			}
		}
	};
	logInfo("[stream] cancel resolve: %s", url.toUtf8().constData());
	killMatch(m_inflight);
	killMatch(m_playlistInflight);
}

//----------------------------------------------------------------
void StreamResolver::cancelDownload()
{
	if (!m_download) return;
	QProcess *p = m_download;
	m_download = nullptr;
	p->disconnect();
	if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(1500); }
	p->deleteLater();
	logInfo("[stream] download cancelled");
}

//----------------------------------------------------------------
void StreamResolver::shutdown()
{
	logInfo("[stream] shutdown: killing %d resolve + %d aux process(es)%s",
	        (int)m_inflight.size(), (int)m_aux.size(),
	        m_download ? " + 1 download" : "");
	auto killProc = [](QProcess *p){
		if (!p) return;
		p->disconnect();               // its finished-lambda must not run now
		if (p->state() != QProcess::NotRunning) {
			p->kill();
			p->waitForFinished(1500);  // kill is near-instant; bounded anyway
		}
		p->deleteLater();
	};
	for (auto it = m_inflight.begin(); it != m_inflight.end(); ++it)
		killProc(it.key());
	m_inflight.clear();
	for (QProcess *p : m_aux)
		killProc(p);
	m_aux.clear();
	// Playlist procs were also inserted into m_aux (killed above); just drop
	// the dangling pointers here so no double-free happens.
	m_playlistInflight.clear();
	killProc(m_download);
	m_download = nullptr;
	// Wipe any scratch file so nothing survives the session.
	cleanTempDir();
}
