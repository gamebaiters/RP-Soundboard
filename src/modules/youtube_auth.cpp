// src/modules/youtube_auth.cpp
//----------------------------------
// See youtube_auth.h. Browser-based Google sign-in for the stream engine,
// DevTools-protocol cookie capture, encrypted session store.
//----------------------------------

#include "youtube_auth.h"
#include "stream_resolver.h"

#include <QProcess>
#include <QTimer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSettings>
#include <QSaveFile>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QUrl>
#include <QHash>
#include <QSet>
#include <QPointer>
#include <functional>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincrypt.h>     // CryptProtectData / CryptUnprotectData (crypt32)
#else
#include <stdio.h>        // ::rename - atomic replace of the session file
#endif

#include "../ts3log.h"    // logInfo / logWarning / extremeLog

namespace {

// Google's own YouTube sign-in entry point: after the sign-in it bounces
// through youtube.com/signin, which is what sets the YouTube session cookies.
const char kLoginUrl[] =
	"https://accounts.google.com/ServiceLogin?service=youtube&passive=true"
	"&continue=https%3A%2F%2Fwww.youtube.com%2Fsignin%3Faction_handle_signin%3Dtrue"
	"%26app%3Ddesktop%26next%3D%252F";
// Same endpoint Chromium itself uses to show "signed in as" — read inside the
// sign-in browser, so it is an ordinary first-party request.
const char kListAccountsUrl[] =
	"https://accounts.google.com/ListAccounts?gpsia=1&source=ChromiumBrowser&json=standard";
// A static page on youtube.com: parking the tab here before the final read
// means no YouTube page script can rotate the session behind our back.
const char kParkUrl[] = "https://www.youtube.com/robots.txt";

const int kSignInTimeoutMs = 10 * 60 * 1000;   // the user may need a while (2FA)
const int kImportTimeoutMs = 3 * 60 * 1000;    // macOS keychain prompt waits on the user

const char kStoreMagicDpapi[] = "GBYT1W\n";     // Windows: DPAPI blob follows
const char kStoreMagicPlain[] = "GBYT1P\n";     // elsewhere: owner-only plaintext

// Is `domain` (leading dot allowed) equal to or a subdomain of `base`?
bool domainIs(QString domain, const QString &base)
{
	domain = domain.trimmed().toLower();
	if (domain.startsWith(QLatin1String("#httponly_"))) domain = domain.mid(10);
	while (domain.startsWith('.')) domain.remove(0, 1);
	return domain == base || domain.endsWith(QLatin1Char('.') + base);
}

bool isKeptDomain(const QString &domain)
{
	return domainIs(domain, QStringLiteral("youtube.com"))
	    || domainIs(domain, QStringLiteral("google.com"));
}

bool isYouTubeAuthCookie(const QString &name)
{
	return name == QLatin1String("SAPISID")
	    || name == QLatin1String("__Secure-3PAPISID")
	    || name == QLatin1String("__Secure-1PAPISID");
}

QString randomHex(int bytes)
{
	QByteArray b(bytes, '\0');
	for (int i = 0; i < bytes; ++i)
		b[i] = char(QRandomGenerator::global()->bounded(256));
	return QString::fromLatin1(b.toHex());
}

// Owner-only (0600) file. On Windows the ACL of the user's temp/profile dir
// already confines it to the user; Qt's permission call is a no-op there.
bool writePrivateFile(const QString &path, const QByteArray &data)
{
	QFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
		return false;
	f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	const bool ok = f.write(data) == data.size();
	f.close();
	if (!ok) QFile::remove(path);
	return ok;
}

QString firstErrorLine(const QByteArray &stderrBytes)
{
	const QStringList lines = QString::fromUtf8(stderrBytes).split('\n');
	for (const QString &ln : lines) {
		const QString t = ln.trimmed();
		if (t.startsWith(QLatin1String("ERROR:"), Qt::CaseInsensitive)) {
			QString d = t.mid(6).trimmed();
			if (d.size() > 160) d = d.left(158) + QStringLiteral("…");
			return d;
		}
	}
	return QString();
}

#ifdef _WIN32
// Fixed per-app entropy: another program running as the same Windows user
// can't decrypt the blob by just calling CryptUnprotectData on it.
const char kDpapiEntropy[] = "GameBaiters-Soundboard/YouTube-session/v1";

bool dpapiProtect(const QByteArray &plain, QByteArray &out)
{
	DATA_BLOB in{ DWORD(plain.size()), (BYTE *)plain.constData() };
	DATA_BLOB ent{ DWORD(sizeof(kDpapiEntropy) - 1), (BYTE *)kDpapiEntropy };
	DATA_BLOB res{ 0, nullptr };
	if (!CryptProtectData(&in, L"GameBaiters Soundboard - YouTube session", &ent,
	                      nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &res))
		return false;
	out = QByteArray((const char *)res.pbData, int(res.cbData));
	SecureZeroMemory(res.pbData, res.cbData);
	LocalFree(res.pbData);
	return true;
}

bool dpapiUnprotect(const QByteArray &blob, QByteArray &out)
{
	DATA_BLOB in{ DWORD(blob.size()), (BYTE *)blob.constData() };
	DATA_BLOB ent{ DWORD(sizeof(kDpapiEntropy) - 1), (BYTE *)kDpapiEntropy };
	DATA_BLOB res{ 0, nullptr };
	if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr,
	                        CRYPTPROTECT_UI_FORBIDDEN, &res))
		return false;
	out = QByteArray((const char *)res.pbData, int(res.cbData));
	SecureZeroMemory(res.pbData, res.cbData);
	LocalFree(res.pbData);
	return true;
}
#endif

// Single-shot delay whose timer is a CHILD of `owner`: destroying the owner
// cancels it. (QTimer::singleShot(ms, ctx, fn) is NOT used here: its internal
// timer outlives `ctx`, and a functor slot object destroyed after the plugin
// DLL is unloaded jumps into unmapped code - see shutdown().)
void after(QObject *owner, int ms, std::function<void()> fn)
{
	auto *t = new QTimer(owner);
	t->setSingleShot(true);
	QObject::connect(t, &QTimer::timeout, owner, [t, fn]{ t->deleteLater(); fn(); });
	t->start(ms);
}

QSettings &prefs()
{
	static QSettings s("GameBaiters", "Soundboard");
	return s;
}

} // namespace

//================================================================
// CdpClient — the smallest websocket client that speaks the Chrome DevTools
// protocol over loopback. Qt5::WebSockets is not shipped by the TS3 client, so
// RFC 6455 framing is done by hand: one text message per JSON command/reply,
// client frames masked, server frames possibly fragmented, ping -> pong.
//================================================================
class CdpClient : public QObject
{
public:
	using Reply = std::function<void(const QJsonObject &result, const QString &error)>;

	std::function<void()> onOpen;
	std::function<void()> onClosed;

	explicit CdpClient(QObject *parent) : QObject(parent), m_sock(new QTcpSocket(this))
	{
		connect(m_sock, &QTcpSocket::connected, this, [this]{ sendHandshake(); });
		connect(m_sock, &QTcpSocket::readyRead, this, [this]{ onReadyRead(); });
		connect(m_sock, &QTcpSocket::disconnected, this, [this]{ die(); });
		// Connection refused / reset ends in UnconnectedState. (Not the
		// errorOccurred signal: that one is Qt 5.15+, and on macOS the plugin
		// runs against whatever Qt the TS3 client bundles.)
		connect(m_sock, &QAbstractSocket::stateChanged, this,
		        [this](QAbstractSocket::SocketState st){
			if (st == QAbstractSocket::UnconnectedState) die();
		});
	}

	void open(const QString &host, quint16 port, const QString &path)
	{
		m_host = host; m_port = port; m_path = path.isEmpty() ? QStringLiteral("/") : path;
		m_sock->connectToHost(host, port);
	}

	bool isOpen() const { return m_open && !m_dead; }

	// Fire a command. `cb` (optional) gets the "result" object or an error text.
	void call(const QString &method, const QJsonObject &params = QJsonObject(),
	          Reply cb = nullptr, const QString &sessionId = QString())
	{
		if (!isOpen()) { if (cb) cb(QJsonObject(), QStringLiteral("not connected")); return; }
		const int id = m_nextId++;
		QJsonObject msg{ { "id", id }, { "method", method } };
		if (!params.isEmpty())    msg.insert("params", params);
		if (!sessionId.isEmpty()) msg.insert("sessionId", sessionId);
		if (cb) m_pending.insert(id, cb);
		sendFrame(0x1, QJsonDocument(msg).toJson(QJsonDocument::Compact));
	}

	// shutdown() + drop our lambdas from the socket, so destroying this object
	// later runs no functor code of ours.
	void detach()
	{
		shutdown();
		m_sock->disconnect(this);
	}

	// Drop every callback and the connection. Nothing fires after this.
	void shutdown()
	{
		onOpen = nullptr;
		onClosed = nullptr;
		m_pending.clear();
		m_dead = true;
		m_open = false;
		m_sock->abort();
	}

private:
	void sendHandshake()
	{
		QByteArray raw(16, '\0');
		for (int i = 0; i < 16; ++i) raw[i] = char(QRandomGenerator::global()->bounded(256));
		m_key = raw.toBase64();
		// No Origin header: the DevTools server only checks Origin when one is
		// present (--remote-allow-origins), and Host must be an IP/localhost.
		QByteArray req;
		req += "GET " + m_path.toUtf8() + " HTTP/1.1\r\n";
		req += "Host: " + m_host.toUtf8() + ":" + QByteArray::number(m_port) + "\r\n";
		req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
		req += "Sec-WebSocket-Key: " + m_key + "\r\n";
		req += "Sec-WebSocket-Version: 13\r\n\r\n";
		m_sock->write(req);
	}

	void onReadyRead()
	{
		m_buf += m_sock->readAll();
		if (!m_open) {
			const int end = m_buf.indexOf("\r\n\r\n");
			if (end < 0) {
				if (m_buf.size() > 64 * 1024) die();
				return;
			}
			const QByteArray head = m_buf.left(end);
			m_buf.remove(0, end + 4);
			const QByteArray expect = QCryptographicHash::hash(
				m_key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
				QCryptographicHash::Sha1).toBase64();
			if (!head.startsWith("HTTP/1.1 101") || !head.contains(expect)) {
				logWarning("[yt-auth] devtools handshake refused");
				die();
				return;
			}
			m_open = true;
			if (onOpen) onOpen();
			if (m_dead) return;
		}
		parseFrames();
	}

	void parseFrames()
	{
		for (;;) {
			if (m_dead) return;
			if (m_buf.size() < 2) return;
			const quint8 b0 = quint8(m_buf[0]), b1 = quint8(m_buf[1]);
			const bool fin = b0 & 0x80;
			const int op = b0 & 0x0f;
			const bool masked = b1 & 0x80;
			quint64 len = b1 & 0x7f;
			int pos = 2;
			if (len == 126) {
				if (m_buf.size() < 4) return;
				len = (quint64(quint8(m_buf[2])) << 8) | quint8(m_buf[3]);
				pos = 4;
			} else if (len == 127) {
				if (m_buf.size() < 10) return;
				len = 0;
				for (int i = 0; i < 8; ++i) len = (len << 8) | quint8(m_buf[2 + i]);
				pos = 10;
			}
			if (len > 64ull * 1024 * 1024) { die(); return; }   // sanity
			QByteArray mask;
			if (masked) {
				if (m_buf.size() < pos + 4) return;
				mask = m_buf.mid(pos, 4);
				pos += 4;
			}
			if (quint64(m_buf.size()) < pos + len) return;
			QByteArray payload = m_buf.mid(pos, int(len));
			m_buf.remove(0, pos + int(len));
			if (masked)
				for (int i = 0; i < payload.size(); ++i) payload[i] = payload[i] ^ mask[i % 4];

			switch (op) {
			case 0x0:   // continuation
				m_frag += payload;
				if (fin) { const QByteArray msg = m_frag; m_frag.clear(); handleMessage(msg); }
				break;
			case 0x1: case 0x2:
				if (fin) handleMessage(payload);
				else     m_frag = payload;
				break;
			case 0x8:   // close
				die();
				return;
			case 0x9:   // ping
				sendFrame(0xA, payload);
				break;
			default:    // pong / reserved
				break;
			}
		}
	}

	void handleMessage(const QByteArray &msg)
	{
		const QJsonDocument d = QJsonDocument::fromJson(msg);
		if (!d.isObject()) return;
		const QJsonObject o = d.object();
		if (!o.contains("id")) return;   // events: not subscribed to anything
		const int id = o.value("id").toInt();
		Reply cb = m_pending.take(id);
		if (!cb) return;
		if (o.contains("error"))
			cb(QJsonObject(), o.value("error").toObject().value("message").toString(
				QStringLiteral("error")));
		else
			cb(o.value("result").toObject(), QString());
	}

	void sendFrame(int op, const QByteArray &payload)
	{
		if (m_dead) return;
		QByteArray f;
		f.append(char(0x80 | op));
		const quint64 n = quint64(payload.size());
		if (n < 126) {
			f.append(char(0x80 | n));
		} else if (n < 65536) {
			f.append(char(0x80 | 126));
			f.append(char((n >> 8) & 0xff));
			f.append(char(n & 0xff));
		} else {
			f.append(char(0x80 | 127));
			for (int i = 7; i >= 0; --i) f.append(char((n >> (8 * i)) & 0xff));
		}
		char mask[4];
		for (char &c : mask) c = char(QRandomGenerator::global()->bounded(256));
		f.append(mask, 4);
		QByteArray body = payload;
		for (int i = 0; i < body.size(); ++i) body[i] = body[i] ^ mask[i % 4];
		f += body;
		m_sock->write(f);
	}

	void die()
	{
		if (m_dead) return;
		m_dead = true;
		m_open = false;
		m_pending.clear();
		auto cb = onClosed;
		onClosed = nullptr;
		onOpen = nullptr;
		if (cb) cb();
	}

	QTcpSocket *m_sock;
	QString     m_host;
	quint16     m_port = 0;
	QString     m_path;
	QByteArray  m_key;
	QByteArray  m_buf;
	QByteArray  m_frag;
	bool        m_open = false;
	bool        m_dead = false;
	int         m_nextId = 1;
	QHash<int, Reply> m_pending;
};

//================================================================
YouTubeAuth &YouTubeAuth::instance()
{
	static YouTubeAuth s_inst;
	return s_inst;
}

YouTubeAuth::YouTubeAuth(QObject *parent)
	: QObject(parent)
	, m_poll(new QTimer(this))
	, m_deadline(new QTimer(this))
{
	connect(m_poll, &QTimer::timeout, this, [this]{ browserPoll(); });
	m_deadline->setSingleShot(true);
	connect(m_deadline, &QTimer::timeout, this, [this]{
		finishFlow(false, tr("Sign-in timed out. Try again when you're ready."));
	});
}

//----------------------------------------------------------------
// Session store location: the per-user data dir, OUTSIDE the plugin folder
// (survives plugin updates / reinstalls) and shared with the standalone app.
QString YouTubeAuth::storeDir()
{
	QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	if (base.isEmpty()) base = QDir::homePath();
	const QString dir = base + "/GameBaiters/Soundboard";
	QDir().mkpath(dir);
	return dir;
}

QString YouTubeAuth::storePath()
{
	return storeDir() + "/youtube_session.dat";
}

//----------------------------------------------------------------
bool YouTubeAuth::loadSession(QByteArray &netscape) const
{
	QFile f(storePath());
	if (!f.open(QIODevice::ReadOnly)) return false;
	const QByteArray raw = f.readAll();
	f.close();
#ifdef _WIN32
	const QByteArray magic(kStoreMagicDpapi);
	if (!raw.startsWith(magic)) return false;
	if (!dpapiUnprotect(raw.mid(magic.size()), netscape)) {
		logWarning("[yt-auth] saved session could not be decrypted (other Windows user?)");
		return false;
	}
#else
	const QByteArray magic(kStoreMagicPlain);
	if (!raw.startsWith(magic)) return false;
	netscape = raw.mid(magic.size());
#endif
	return !netscape.isEmpty();
}

//----------------------------------------------------------------
bool YouTubeAuth::saveSession(const QByteArray &netscape, const QString &method,
                              const QString &name, const QString &email)
{
	const QString path = storePath();
#ifdef _WIN32
	QByteArray blob;
	if (!dpapiProtect(netscape, blob)) {
		logWarning("[yt-auth] DPAPI encryption failed (%lu)", (unsigned long)GetLastError());
		return false;
	}
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly)) return false;
	f.write(kStoreMagicDpapi);
	f.write(blob);
	if (!f.commit()) return false;
#else
	// Owner-only temp file, then an atomic rename: the session never exists
	// on disk with wider permissions, not even for a moment.
	const QString tmp = path + ".tmp";
	QFile::remove(tmp);
	if (!writePrivateFile(tmp, QByteArray(kStoreMagicPlain) + netscape)) return false;
	if (::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(path).constData()) != 0) {
		QFile::remove(tmp);
		return false;
	}
#endif
	QSettings &s = prefs();
	s.setValue("youtube/account_name",  name);
	s.setValue("youtube/account_email", email);
	s.setValue("youtube/connected_at",  QDateTime::currentDateTime().toString(Qt::ISODate));
	s.setValue("youtube/method",        method);
	s.remove("youtube/session_rejected");
	s.sync();
	logInfo("[yt-auth] session saved (%s)", method.toUtf8().constData());
	return true;
}

//----------------------------------------------------------------
YouTubeAuth::State YouTubeAuth::state() const
{
	if (m_flow != Flow::None) return State::SigningIn;
	if (!hasSession())        return State::SignedOut;
	return prefs().value("youtube/session_rejected", false).toBool() ? State::Rejected
	                                                                 : State::SignedIn;
}

bool YouTubeAuth::hasSession() const { return QFileInfo::exists(storePath()); }
QString YouTubeAuth::accountName()  const { return prefs().value("youtube/account_name").toString(); }
QString YouTubeAuth::accountEmail() const { return prefs().value("youtube/account_email").toString(); }

QDateTime YouTubeAuth::connectedAt() const
{
	return QDateTime::fromString(prefs().value("youtube/connected_at").toString(), Qt::ISODate);
}

QString YouTubeAuth::methodLabel() const
{
	const QString m = prefs().value("youtube/method").toString();
	if (m.startsWith(QLatin1String("browser:")))
		return tr("signed in with Google in %1").arg(m.mid(8));
	if (m.startsWith(QLatin1String("import:")))
		return tr("session imported from %1").arg(m.mid(7));
	if (m == QLatin1String("file"))
		return tr("imported from a cookies.txt file");
	return QString();
}

bool YouTubeAuth::useForEveryVideo()
{
	return prefs().value("youtube/use_always", false).toBool();
}

void YouTubeAuth::setUseForEveryVideo(bool on)
{
	prefs().setValue("youtube/use_always", on);
}

//----------------------------------------------------------------
YouTubeAuth::Browser YouTubeAuth::findBrowser()
{
	auto exists = [](const QString &p){ return !p.isEmpty() && QFileInfo(p).isFile(); };
#if defined(_WIN32)
	// Chrome first (most users' Google browser), Edge is on every Win10/11.
	struct Cand { const char *name; const char *rel; const char *appPath; };
	static const Cand cands[] = {
		{ "Google Chrome",  "Google/Chrome/Application/chrome.exe",              "chrome.exe"  },
		{ "Microsoft Edge", "Microsoft/Edge/Application/msedge.exe",             "msedge.exe"  },
		{ "Brave",          "BraveSoftware/Brave-Browser/Application/brave.exe", "brave.exe"   },
		{ "Vivaldi",        "Vivaldi/Application/vivaldi.exe",                   "vivaldi.exe" },
		{ "Chromium",       "Chromium/Application/chrome.exe",                   nullptr       },
	};
	const QStringList bases = {
		qEnvironmentVariable("ProgramFiles"),
		qEnvironmentVariable("ProgramW6432"),
		qEnvironmentVariable("ProgramFiles(x86)"),
		qEnvironmentVariable("LOCALAPPDATA"),
	};
	for (const Cand &c : cands) {
		for (const QString &b : bases) {
			if (b.isEmpty()) continue;
			const QString p = QDir::fromNativeSeparators(b) + '/' + c.rel;
			if (exists(p)) return { c.name, QDir::toNativeSeparators(p) };
		}
		if (c.appPath) {   // registered install somewhere else (custom dir)
			for (const char *hive : { "HKEY_LOCAL_MACHINE", "HKEY_CURRENT_USER" }) {
				QSettings reg(QString("%1\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%2")
				                  .arg(QLatin1String(hive), QLatin1String(c.appPath)), QSettings::NativeFormat);
				const QString p = reg.value("Default").toString().remove('"');
				if (exists(p)) return { c.name, p };
			}
		}
	}
#elif defined(__APPLE__)
	struct Cand { const char *name; const char *bundle; };
	static const Cand cands[] = {
		{ "Google Chrome",  "Google Chrome.app/Contents/MacOS/Google Chrome"   },
		{ "Microsoft Edge", "Microsoft Edge.app/Contents/MacOS/Microsoft Edge" },
		{ "Brave",          "Brave Browser.app/Contents/MacOS/Brave Browser"   },
		{ "Vivaldi",        "Vivaldi.app/Contents/MacOS/Vivaldi"               },
		{ "Chromium",       "Chromium.app/Contents/MacOS/Chromium"             },
	};
	for (const Cand &c : cands)
		for (const QString &b : { QString("/Applications"), QString(QDir::homePath() + "/Applications") }) {
			const QString p = b + '/' + c.bundle;
			if (exists(p)) return { c.name, p };
		}
#else
	struct Cand { const char *name; const char *exe; };
	static const Cand cands[] = {
		{ "Google Chrome",  "google-chrome-stable"  }, { "Google Chrome",  "google-chrome" },
		{ "Chromium",       "chromium"              }, { "Chromium",       "chromium-browser" },
		{ "Microsoft Edge", "microsoft-edge-stable" }, { "Microsoft Edge", "microsoft-edge" },
		{ "Brave",          "brave-browser"         }, { "Brave",          "brave" },
		{ "Vivaldi",        "vivaldi-stable"        },
	};
	for (const Cand &c : cands) {
		const QString p = QStandardPaths::findExecutable(c.exe);
		if (!p.isEmpty()) return { c.name, p };
	}
#endif
	return {};
}

//----------------------------------------------------------------
QList<QPair<QString, QString>> YouTubeAuth::importableBrowsers()
{
	QList<QPair<QString, QString>> l;
	l << qMakePair(QStringLiteral("firefox"), QStringLiteral("Firefox"));
#ifdef __APPLE__
	l << qMakePair(QStringLiteral("safari"),  QStringLiteral("Safari"));
#endif
	l << qMakePair(QStringLiteral("chrome"),   QStringLiteral("Google Chrome"))
	  << qMakePair(QStringLiteral("edge"),     QStringLiteral("Microsoft Edge"))
	  << qMakePair(QStringLiteral("brave"),    QStringLiteral("Brave"))
	  << qMakePair(QStringLiteral("opera"),    QStringLiteral("Opera"))
	  << qMakePair(QStringLiteral("vivaldi"),  QStringLiteral("Vivaldi"))
	  << qMakePair(QStringLiteral("chromium"), QStringLiteral("Chromium"));
	return l;
}

//----------------------------------------------------------------
QByteArray YouTubeAuth::filterNetscape(const QByteArray &netscape)
{
	QByteArray out("# Netscape HTTP Cookie File\n"
	               "# GameBaiters Soundboard - YouTube session (youtube.com / google.com only)\n");
	const QList<QByteArray> lines = netscape.split('\n');
	for (QByteArray ln : lines) {
		if (ln.endsWith('\r')) ln.chop(1);
		if (ln.trimmed().isEmpty()) continue;
		if (ln.startsWith('#') && !ln.startsWith("#HttpOnly_")) continue;
		const QList<QByteArray> f = ln.split('\t');
		if (f.size() < 7) continue;
		if (!isKeptDomain(QString::fromUtf8(f[0]))) continue;
		out += ln + '\n';
	}
	return out;
}

bool YouTubeAuth::netscapeHasYouTubeLogin(const QByteArray &netscape)
{
	const qint64 now = QDateTime::currentSecsSinceEpoch();
	const QList<QByteArray> lines = netscape.split('\n');
	for (QByteArray ln : lines) {
		if (ln.endsWith('\r')) ln.chop(1);
		if (ln.startsWith('#') && !ln.startsWith("#HttpOnly_")) continue;
		const QList<QByteArray> f = ln.split('\t');
		if (f.size() < 7) continue;
		if (!domainIs(QString::fromUtf8(f[0]), QStringLiteral("youtube.com"))) continue;
		if (!isYouTubeAuthCookie(QString::fromUtf8(f[5])) || f[6].trimmed().isEmpty()) continue;
		const qint64 exp = f[4].toLongLong();
		if (exp != 0 && exp < now) continue;   // expired
		return true;
	}
	return false;
}

QByteArray YouTubeAuth::cookiesToNetscape(const QJsonArray &cookies)
{
	QByteArray out;
	for (const QJsonValue &v : cookies) {
		const QJsonObject c = v.toObject();
		const QString domain = c.value("domain").toString();
		if (domain.isEmpty() || !isKeptDomain(domain)) continue;
		const bool session = c.value("session").toBool(false);
		const double exp   = c.value("expires").toDouble(-1);
		const qint64 expiry = (session || exp <= 0) ? 0 : qint64(exp);
		QStringList f;
		f << domain
		  << (domain.startsWith('.') ? "TRUE" : "FALSE")
		  << c.value("path").toString("/")
		  << (c.value("secure").toBool(false) ? "TRUE" : "FALSE")
		  << QString::number(expiry)
		  << c.value("name").toString()
		  << c.value("value").toString();
		out += f.join('\t').toUtf8() + '\n';
	}
	return filterNetscape(out);
}

//----------------------------------------------------------------
// Best-effort "who is signed in" from the ListAccounts reply. Its shape is
// ["gaia.l.a.r",[["gaia.l.a",1,"Name","mail@gmail.com",...]]] today; walk it
// generically (name = the string right before the e-mail) and fall back to a
// bare e-mail regex, so a format change degrades to "no name", never breaks.
void YouTubeAuth::parseAccountInfo(const QString &text, QString &name, QString &email)
{
	static const QRegularExpression mailRe(
		QStringLiteral("^[A-Za-z0-9._%+\\-]+@[A-Za-z0-9.\\-]+\\.[A-Za-z]{2,}$"));
	std::function<bool(const QJsonValue &)> walk = [&](const QJsonValue &v) -> bool {
		if (v.isArray()) {
			const QJsonArray a = v.toArray();
			for (int i = 0; i < a.size(); ++i) {
				if (a[i].isString() && mailRe.match(a[i].toString()).hasMatch()) {
					email = a[i].toString();
					if (i > 0 && a[i - 1].isString()) {
						const QString n = a[i - 1].toString();
						if (!n.contains('@') && !n.startsWith(QLatin1String("http")) &&
						    !n.startsWith(QLatin1String("gaia.")))
							name = n;
					}
					return true;
				}
			}
			for (const QJsonValue &e : a) if (walk(e)) return true;
		} else if (v.isObject()) {
			const QJsonObject o = v.toObject();
			const QString m = o.value("email").toString();
			if (mailRe.match(m).hasMatch()) {
				email = m;
				name = o.value("display_name").toString(o.value("name").toString());
				return true;
			}
			for (const QJsonValue &e : o) if (walk(e)) return true;
		}
		return false;
	};
	const int a = text.indexOf('['), o = text.indexOf('{');
	int start = (a < 0) ? o : (o < 0 ? a : qMin(a, o));
	if (start >= 0) {
		const int end = qMax(text.lastIndexOf(']'), text.lastIndexOf('}'));
		if (end > start) {
			const QJsonDocument d = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8());
			if (d.isArray())  walk(d.array());
			if (d.isObject()) walk(d.object());
		}
	}
	if (email.isEmpty()) {
		static const QRegularExpression anyMail(
			QStringLiteral("[A-Za-z0-9._%+\\-]+@[A-Za-z0-9.\\-]+\\.[A-Za-z]{2,}"));
		const auto m = anyMail.match(text);
		if (m.hasMatch()) email = m.captured(0);
	}
}

//================================================================
// Browser sign-in flow
//================================================================
void YouTubeAuth::startBrowserSignIn()
{
	if (m_flow != Flow::None) return;

	const Browser b = findBrowser();
	if (!b.isValid()) {
		emit signInFinished(false, tr(
			"No Chrome, Edge or Brave browser was found on this computer.\n"
			"Use \"Other ways\" to take the session from Firefox or a cookies.txt file."));
		return;
	}

	// Fresh, throw-away profile inside the stream scratch dir: no history,
	// extensions or saved accounts of the user's normal profile are touched,
	// and the scratch wipe at start/shutdown removes it whatever happens.
	m_profileDir = StreamResolver::workDir() + "/glogin_" + randomHex(6);
	QDir(m_profileDir).removeRecursively();
	QDir().mkpath(m_profileDir);

	m_flow = Flow::Browser;
	m_step = Step::Launching;
	m_browserName = b.name;
	m_browserOut.clear();
	m_pendingName.clear();
	m_pendingEmail.clear();
	m_captured = false;

	m_browser = new QProcess(this);
	m_browser->setProcessChannelMode(QProcess::MergedChannels);
	connect(m_browser, &QProcess::readyReadStandardOutput, this, [this]{ browserReadOutput(); });
	connect(m_browser, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart && m_flow == Flow::Browser)
			finishFlow(false, tr("Couldn't start %1.").arg(m_browserName));
	});
	connect(m_browser, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
	        this, [this](int, QProcess::ExitStatus){
		if (m_flow == Flow::Browser && !m_captured)
			finishFlow(false, tr("The browser window was closed before the sign-in was completed."));
	});

	const QStringList args = {
		"--user-data-dir=" + QDir::toNativeSeparators(m_profileDir),
		"--remote-debugging-port=0",       // OS-picked free port, loopback only
		"--no-first-run",
		"--no-default-browser-check",
		"--disable-default-apps",
		"--disable-sync",
		"--new-window",
		QString::fromLatin1(kLoginUrl),
	};
	logInfo("[yt-auth] opening Google sign-in in %s", b.name.toUtf8().constData());
	m_browser->start(b.path, args);

	m_poll->start(400);
	m_deadline->start(kSignInTimeoutMs);

	emit stateChanged();
	emit signInProgress(tr("Sign in to Google in the %1 window that just opened…").arg(b.name));
}

//----------------------------------------------------------------
// Drain the browser's console output (a full pipe would stall it) and pick up
// "DevTools listening on ws://127.0.0.1:PORT/devtools/browser/ID".
void YouTubeAuth::browserReadOutput()
{
	if (!m_browser) return;
	const QByteArray chunk = m_browser->readAllStandardOutput();
	if (m_step != Step::Launching) return;   // endpoint already known
	m_browserOut += chunk;
	if (m_browserOut.size() > 256 * 1024) m_browserOut = m_browserOut.right(64 * 1024);
	static const QRegularExpression re(QStringLiteral("DevTools listening on (ws://[^\\s]+)"));
	const auto m = re.match(QString::fromUtf8(m_browserOut));
	if (!m.hasMatch()) return;
	const QUrl u(m.captured(1));
	if (u.port() <= 0) return;
	tryConnectDevTools(u.host(), quint16(u.port()), u.path());
}

//----------------------------------------------------------------
void YouTubeAuth::browserPoll()
{
	if (m_flow != Flow::Browser) return;

	if (m_step == Step::Launching) {
		// Same endpoint, written by the browser into its profile dir (covers
		// builds that don't print it to the console).
		QFile f(m_profileDir + "/DevToolsActivePort");
		if (f.open(QIODevice::ReadOnly)) {
			const QList<QByteArray> lines = f.readAll().split('\n');
			if (lines.size() >= 2) {
				const int port = lines[0].trimmed().toInt();
				const QString path = QString::fromUtf8(lines[1].trimmed());
				if (port > 0 && path.startsWith('/'))
					tryConnectDevTools(QStringLiteral("127.0.0.1"), quint16(port), path);
			}
		}
		return;
	}

	if (m_step != Step::WaitingForLogin || !m_cdp || m_pollBusy) return;
	m_pollBusy = true;
	m_cdp->call("Storage.getCookies", QJsonObject(),
		[this](const QJsonObject &r, const QString &err){
			m_pollBusy = false;
			if (m_flow != Flow::Browser || m_step != Step::WaitingForLogin) return;
			if (!err.isEmpty()) {
				extremeLog("[yt-auth] Storage.getCookies: %s", err.toUtf8().constData());
				return;
			}
			onCookiesPolled(r.value("cookies").toArray());
		});
}

//----------------------------------------------------------------
void YouTubeAuth::tryConnectDevTools(const QString &host, quint16 port, const QString &path)
{
	if (m_step != Step::Launching || m_cdp) return;
	m_step = Step::Connecting;
	extremeLog("[yt-auth] devtools endpoint %s:%u%s", host.toUtf8().constData(),
	           (unsigned)port, path.toUtf8().constData());
	m_cdp = new CdpClient(this);
	m_cdp->onOpen = [this]{
		logInfo("[yt-auth] connected to the sign-in browser, waiting for the user");
		m_step = Step::WaitingForLogin;
		m_poll->setInterval(1000);
	};
	m_cdp->onClosed = [this]{
		if (m_flow != Flow::Browser || m_captured) return;
		// Usually the user closed the window: the process-finished handler
		// says so with the better message; give it a moment to arrive.
		after(this, 1500, [this]{
			if (m_flow == Flow::Browser && !m_captured)
				finishFlow(false, tr("Lost contact with the sign-in browser."));
		});
	};
	m_cdp->open(host, port, path);
}

//----------------------------------------------------------------
void YouTubeAuth::onCookiesPolled(const QJsonArray &cookies)
{
	bool loginInfo = false, sid = false;
	for (const QJsonValue &v : cookies) {
		const QJsonObject c = v.toObject();
		if (!domainIs(c.value("domain").toString(), QStringLiteral("youtube.com"))) continue;
		const QString n = c.value("name").toString();
		if (c.value("value").toString().isEmpty()) continue;
		if (n == QLatin1String("LOGIN_INFO")) loginInfo = true;
		if (isYouTubeAuthCookie(n))           sid = true;
	}
	if (loginInfo && sid)
		beginFinishing();
}

//----------------------------------------------------------------
// Signed in. Close every tab the user was on, open ONE tab on ListAccounts to
// read the account name, then park it on a static youtube.com page.
void YouTubeAuth::beginFinishing()
{
	m_step = Step::Finishing;
	m_poll->stop();
	logInfo("[yt-auth] YouTube session detected, finishing");
	emit signInProgress(tr("Signed in! Saving your YouTube session…"));

	m_cdp->call("Target.getTargets", QJsonObject(),
		[this](const QJsonObject &r, const QString &){
			if (m_flow != Flow::Browser) return;
			QStringList oldPages;
			for (const QJsonValue &v : r.value("targetInfos").toArray()) {
				const QJsonObject t = v.toObject();
				if (t.value("type").toString() == QLatin1String("page"))
					oldPages << t.value("targetId").toString();
			}
			m_cdp->call("Target.createTarget",
				QJsonObject{ { "url", QString::fromLatin1(kListAccountsUrl) } },
				[this, oldPages](const QJsonObject &r2, const QString &err){
					if (m_flow != Flow::Browser) return;
					for (const QString &id : oldPages)
						m_cdp->call("Target.closeTarget", QJsonObject{ { "targetId", id } });
					const QString target = r2.value("targetId").toString();
					if (!err.isEmpty() || target.isEmpty()) { finishStepCapture(); return; }
					finishStepAccountInfo(target);
				});
		});
}

//----------------------------------------------------------------
void YouTubeAuth::finishStepAccountInfo(const QString &targetId)
{
	m_cdp->call("Target.attachToTarget",
		QJsonObject{ { "targetId", targetId }, { "flatten", true } },
		[this](const QJsonObject &r, const QString &err){
			if (m_flow != Flow::Browser) return;
			const QString session = r.value("sessionId").toString();
			if (!err.isEmpty() || session.isEmpty()) { finishStepCapture(); return; }

			// Poll the page text until it has loaded (bounded: ~8 s), then
			// park the tab on robots.txt and do the final capture.
			auto tries = std::make_shared<int>(0);
			auto park = [this, session]{
				if (m_flow != Flow::Browser || !m_cdp) return;
				m_cdp->call("Page.navigate",
					QJsonObject{ { "url", QString::fromLatin1(kParkUrl) } }, nullptr, session);
				after(m_cdp, 1500, [this]{ finishStepCapture(); });
			};
			// The step holds itself only weakly; the pending callback / timer
			// holds the strong ref, so the chain frees itself when it ends.
			auto poll = std::make_shared<std::function<void()>>();
			std::weak_ptr<std::function<void()>> weak = poll;
			*poll = [this, session, tries, park, weak]{
				auto self = weak.lock();
				if (!self || m_flow != Flow::Browser || !m_cdp) return;
				m_cdp->call("Runtime.evaluate",
					QJsonObject{
						{ "expression", QStringLiteral(
							"document.readyState === 'complete' && document.body"
							" ? document.body.innerText : ''") },
						{ "returnByValue", true } },
					[this, tries, park, self](const QJsonObject &rr, const QString &){
						if (m_flow != Flow::Browser || !m_cdp) return;
						const QString text = rr.value("result").toObject().value("value").toString();
						if (!text.isEmpty()) {
							parseAccountInfo(text, m_pendingName, m_pendingEmail);
							logInfo("[yt-auth] account: %s",
							        m_pendingEmail.isEmpty() ? "(unknown)" : "identified");
							park();
							return;
						}
						if (++*tries >= 16) { park(); return; }
						after(m_cdp, 500, [self]{ (*self)(); });
					}, session);
			};
			(*poll)();
		});
}

//----------------------------------------------------------------
void YouTubeAuth::finishStepCapture()
{
	if (m_flow != Flow::Browser || !m_cdp || m_captured) return;
	m_cdp->call("Storage.getCookies", QJsonObject(),
		[this](const QJsonObject &r, const QString &err){
			if (m_flow != Flow::Browser || m_captured) return;
			const QByteArray jar = cookiesToNetscape(r.value("cookies").toArray());
			if (!err.isEmpty() || !netscapeHasYouTubeLogin(jar)) {
				finishFlow(false, tr("Couldn't read the YouTube session from the browser. Please try again."));
				return;
			}
			m_captured = true;
			if (!saveSession(jar, "browser:" + m_browserName, m_pendingName, m_pendingEmail)) {
				finishFlow(false, tr("Signed in, but the session couldn't be saved on this computer."));
				return;
			}
			const QString who = !m_pendingEmail.isEmpty() ? m_pendingEmail
			                  : (!m_pendingName.isEmpty() ? m_pendingName : QString());
			finishFlow(true, who.isEmpty() ? tr("YouTube account connected.")
			                               : tr("Connected as %1.").arg(who));
		});
}

//----------------------------------------------------------------
void YouTubeAuth::finishFlow(bool ok, const QString &message)
{
	if (m_flow == Flow::None) return;
	const Flow f = m_flow;
	m_flow = Flow::None;
	if (f == Flow::Browser) {
		teardownBrowser(true);
	} else if (m_import) {
		QProcess *p = m_import;
		m_import = nullptr;
		retire(p);
	}
	if (!m_importTmp.isEmpty()) { QFile::remove(m_importTmp); m_importTmp.clear(); }
	if (ok) logInfo("[yt-auth] sign-in finished OK");
	else    logInfo("[yt-auth] sign-in ended: %s", message.toUtf8().constData());
	emit stateChanged();
	emit signInFinished(ok, message);
}

//----------------------------------------------------------------
// Close the sign-in browser and erase its throw-away profile. Graceful = ask it
// to close over DevTools first (lets it flush and exit cleanly), kill after 4 s.
// Every object that still carries one of our lambdas while it winds down is
// parked in m_closing so shutdown() can destroy it synchronously before the
// plugin DLL goes away.
void YouTubeAuth::teardownBrowser(bool graceful)
{
	m_poll->stop();
	m_deadline->stop();
	m_pollBusy = false;

	CdpClient *cdp = m_cdp;
	m_cdp = nullptr;
	QProcess *p = m_browser;
	m_browser = nullptr;
	const QString profile = m_profileDir;
	m_profileDir.clear();

	if (p) p->disconnect(this);
	const bool running = p && p->state() != QProcess::NotRunning;

	if (graceful && running && cdp && cdp->isOpen()) {
		cdp->call("Browser.close");
		cdp->setParent(p);           // dies with the process object
		connect(p, &QProcess::readyReadStandardOutput, p, [p]{ p->readAllStandardOutput(); });
		connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
		        this, [this, p, profile](int, QProcess::ExitStatus){ retire(p); wipeProfile(profile); });
		after(p, 4000, [p]{ if (p->state() != QProcess::NotRunning) p->kill(); });
		m_closing << QPointer<QObject>(p);
		return;
	}

	if (cdp) retire(cdp);
	if (p) {
		if (running) { p->kill(); p->waitForFinished(1500); }
		retire(p);
	}
	wipeProfile(profile);
}

//----------------------------------------------------------------
// Drop every connection we own on `o` (so no functor of ours outlives the
// DLL) and delete it on the next event-loop turn; shutdown() deletes it at
// once if that turn never comes.
void YouTubeAuth::retire(QObject *o)
{
	if (!o) return;
	if (auto *p = qobject_cast<QProcess *>(o)) {
		p->disconnect();
		if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(1500); }
	}
	// Our functors also live on children: after()/watchdog timers and a
	// CdpClient reparented onto the sign-in process.
	if (auto *c = dynamic_cast<CdpClient *>(o)) c->detach();
	for (QObject *ch : o->findChildren<QObject *>()) {
		if (auto *c = dynamic_cast<CdpClient *>(ch)) c->detach();
		else if (auto *t = qobject_cast<QTimer *>(ch)) t->disconnect();
	}
	if (!m_closing.contains(o)) m_closing << QPointer<QObject>(o);
	o->deleteLater();
}

//----------------------------------------------------------------
void YouTubeAuth::wipeProfile(const QString &profile)
{
	if (profile.isEmpty()) return;
	if (!QDir(profile).removeRecursively())   // a renderer may still hold a lock
		after(this, 3000, [profile]{ QDir(profile).removeRecursively(); });
}

//----------------------------------------------------------------
void YouTubeAuth::cancelSignIn()
{
	if (m_flow == Flow::None) return;
	logInfo("[yt-auth] sign-in cancelled by the user");
	if (m_flow == Flow::Browser) {
		// Not graceful on purpose: the user wants the window gone now.
		m_flow = Flow::None;
		teardownBrowser(false);
		emit stateChanged();
		emit signInFinished(false, tr("Sign-in cancelled."));
		return;
	}
	finishFlow(false, tr("Sign-in cancelled."));
}

//================================================================
// Import flows
//================================================================
void YouTubeAuth::importFromBrowser(const QString &ytdlpBrowserId)
{
	if (m_flow != Flow::None) return;
	QString human = ytdlpBrowserId;
	for (const auto &b : importableBrowsers())
		if (b.first == ytdlpBrowserId) human = b.second;

	m_flow = Flow::Import;
	m_importBrowser = human;
	m_importTmp = StreamResolver::workDir() + "/ytimport_" + randomHex(6) + ".txt";
	QFile::remove(m_importTmp);

	m_import = new QProcess(this);
#ifdef _WIN32
	m_import->setCreateProcessArgumentsModifier(
		[](QProcess::CreateProcessArguments *a){ a->flags |= CREATE_NO_WINDOW; });
#endif
	m_import->setWorkingDirectory(StreamResolver::workDir());

	// No URL on purpose: yt-dlp loads the browser's cookie store, then saves
	// the jar to --cookies on exit (even when it then stops with "no URL").
	// We keep only the youtube.com / google.com part of what it wrote.
	QStringList args = StreamResolver::commonArgs();
	args << "--cookies-from-browser" << ytdlpBrowserId
	     << "--cookies" << m_importTmp;

	QTimer *wd = new QTimer(m_import);
	wd->setSingleShot(true);
	QProcess *proc = m_import;
	connect(wd, &QTimer::timeout, proc, [proc]{
		if (proc->state() != QProcess::NotRunning) proc->kill();
	});
	wd->start(kImportTimeoutMs);

	connect(proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e){
		if (e == QProcess::FailedToStart && m_flow == Flow::Import)
			finishFlow(false, tr("Streaming engine unavailable."));
	});
	connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
	        this, [this, proc](int, QProcess::ExitStatus){
		if (m_flow != Flow::Import || m_import != proc) return;
		const QByteArray err = proc->readAllStandardError();
		QByteArray raw;
		{
			QFile f(m_importTmp);
			if (f.open(QIODevice::ReadOnly)) raw = f.readAll();
		}
		QFile::remove(m_importTmp);
		const QByteArray jar = filterNetscape(raw);
		if (!netscapeHasYouTubeLogin(jar)) {
			const QString detail = firstErrorLine(err);
			if (!err.trimmed().isEmpty())
				extremeLog("[yt-auth] import stderr: %s", err.trimmed().constData());
			QString msg = tr("No signed-in YouTube session was found in %1.\n"
			                 "Open youtube.com in %1, sign in, then try again.").arg(m_importBrowser);
			if (!detail.isEmpty() && !detail.contains(QLatin1String("provide at least one URL")))
				msg = tr("Couldn't read the session from %1:\n%2").arg(m_importBrowser, detail);
#ifdef _WIN32
			// Chromium browsers on Windows lock their cookie store behind
			// app-bound encryption; reading it from outside usually fails.
			if (m_importBrowser != QLatin1String("Firefox"))
				msg += QStringLiteral("\n") + tr("On Windows, Chrome-based browsers lock their sign-in data:\n"
				                                 "use \"Sign in with Google\" instead, or Firefox.");
#endif
			finishFlow(false, msg);
			return;
		}
		if (!saveSession(jar, "import:" + m_importBrowser, QString(), QString())) {
			finishFlow(false, tr("The session couldn't be saved on this computer."));
			return;
		}
		finishFlow(true, tr("YouTube account connected (from %1).").arg(m_importBrowser));
	});

	logInfo("[yt-auth] importing the session from %s", ytdlpBrowserId.toUtf8().constData());
	proc->start(StreamResolver::ytDlpPath(), args);
	emit stateChanged();
	emit signInProgress(tr("Reading the YouTube session from %1…").arg(human));
}

//----------------------------------------------------------------
bool YouTubeAuth::importCookiesFile(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly)) {
		if (error) *error = tr("Couldn't open the file.");
		return false;
	}
	if (f.size() > 8 * 1024 * 1024) {
		if (error) *error = tr("That file is too big to be a cookies.txt export.");
		return false;
	}
	const QByteArray jar = filterNetscape(f.readAll());
	f.close();
	if (!netscapeHasYouTubeLogin(jar)) {
		if (error) *error = tr("This file doesn't contain a signed-in YouTube session\n"
		                       "(expected a Netscape-format cookies.txt exported while signed in to youtube.com).");
		return false;
	}
	if (!saveSession(jar, "file", QString(), QString())) {
		if (error) *error = tr("The session couldn't be saved on this computer.");
		return false;
	}
	emit stateChanged();
	return true;
}

//----------------------------------------------------------------
void YouTubeAuth::signOut()
{
	cancelSignIn();
	QFile::remove(storePath());
	QSettings &s = prefs();
	s.remove("youtube/account_name");
	s.remove("youtube/account_email");
	s.remove("youtube/connected_at");
	s.remove("youtube/method");
	s.remove("youtube/session_rejected");
	s.sync();
	logInfo("[yt-auth] signed out, session deleted");
	emit stateChanged();
}

//================================================================
// StreamResolver side
//================================================================
QString YouTubeAuth::materializeCookieFile(const QString &dir)
{
	QByteArray jar;
	if (!loadSession(jar)) return QString();
	const QString path = dir + "/ytck_" + randomHex(8) + ".txt";
	if (!writePrivateFile(path, jar)) return QString();
	return path;
}

void YouTubeAuth::reportSessionRejected(const QString &reason)
{
	if (!hasSession() || prefs().value("youtube/session_rejected", false).toBool()) return;
	prefs().setValue("youtube/session_rejected", true);
	logWarning("[yt-auth] YouTube rejected the saved session: %s", reason.toUtf8().constData());
	emit stateChanged();
}

void YouTubeAuth::reportSessionWorked()
{
	if (!prefs().value("youtube/session_rejected", false).toBool()) return;
	prefs().remove("youtube/session_rejected");
	logInfo("[yt-auth] saved session works again");
	emit stateChanged();
}

//----------------------------------------------------------------
// sb_kill path: everything synchronous. Kill the sign-in browser / import, and
// DELETE (not deleteLater) every object carrying our lambdas: a deferred
// delete processed after the DLL is unmapped would call into freed code.
void YouTubeAuth::shutdown()
{
	m_poll->stop();
	m_deadline->stop();
	const QString profile = m_profileDir;
	if (m_flow == Flow::Browser) {
		m_flow = Flow::None;
		teardownBrowser(false);
	}
	if (m_import) {
		QProcess *p = m_import;
		m_import = nullptr;
		retire(p);
	}
	m_flow = Flow::None;

	const QList<QPointer<QObject>> closing = m_closing;
	m_closing.clear();
	for (const QPointer<QObject> &o : closing) {
		if (!o) continue;
		if (auto *p = qobject_cast<QProcess *>(o.data())) {
			p->disconnect();
			if (p->state() != QProcess::NotRunning) { p->kill(); p->waitForFinished(1500); }
		}
		delete o.data();
	}
	// Pending after() timers (profile-wipe retries, ...) are our children.
	for (QTimer *t : findChildren<QTimer *>(QString(), Qt::FindDirectChildrenOnly))
		if (t != m_poll && t != m_deadline) delete t;

	if (!profile.isEmpty()) QDir(profile).removeRecursively();
	if (!m_importTmp.isEmpty()) { QFile::remove(m_importTmp); m_importTmp.clear(); }
}
