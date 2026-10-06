// src/modules/youtube_auth.h
//----------------------------------
// GameBaiters Soundboard - v2.5.0 YouTube account sign-in for the stream engine.
//
// WHY: some videos only resolve for a signed-in YouTube account (age-restricted,
// members-only, private/unlisted the user has access to, Watch Later / Liked
// playlists, and the "Sign in to confirm you're not a bot" wall). yt-dlp can
// only get past those with the account's YouTube cookies.
//
// HOW (no credentials ever touch the soundboard):
//   1. We open the user's OWN installed Chromium browser (Chrome, then Edge,
//      Brave, Vivaldi, Chromium) in a brand-new, throw-away profile, straight
//      on Google's real sign-in page (accounts.google.com, address bar visible).
//      The user signs in exactly as they always do: passkey, 2FA, phone prompt.
//   2. The browser is started with --remote-debugging-port=0 (loopback only).
//      Over the DevTools protocol we ONLY talk to the browser target
//      (Storage.getCookies), never to the sign-in page itself, so Google sees
//      a plain, non-automated browser.
//   3. As soon as the YouTube session cookies appear we close the other tabs,
//      read the account name (accounts.google.com/ListAccounts, in that same
//      browser), park the tab on youtube.com/robots.txt (so no YouTube page
//      can rotate the session afterwards), read the cookies once more, close
//      the browser and delete the throw-away profile. This is exactly the
//      export flow the yt-dlp wiki recommends, automated.
//   4. Only the youtube.com / google.com cookies are kept, encrypted at rest:
//      Windows DPAPI (bound to the Windows user), owner-only file elsewhere.
//      Every yt-dlp run gets its OWN short-lived plaintext copy in the stream
//      scratch dir (yt-dlp rewrites its --cookies file on exit; a shared file
//      would race between parallel resolves), deleted when the process dies.
//
// Fallbacks for machines without a Chromium browser: import the session of a
// browser the user is already signed into (yt-dlp --cookies-from-browser,
// works best with Firefox) or a cookies.txt file.
//
// All of it runs on the GUI thread (QProcess + QTcpSocket, fully async); none
// of it is ever reached from a TS3 callback.
//----------------------------------

#pragma once
#ifndef rpsbsrc__youtube_auth_H__
#define rpsbsrc__youtube_auth_H__

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QPointer>

class QProcess;
class QTimer;
class QJsonArray;
class CdpClient;   // minimal loopback DevTools websocket client (youtube_auth.cpp)

class YouTubeAuth : public QObject
{
	Q_OBJECT
public:
	enum class State {
		SignedOut,      // no saved session
		SigningIn,      // browser window open / import running
		SignedIn,       // saved session, believed valid
		Rejected,       // saved session, but YouTube said it is no longer valid
	};

	struct Browser {
		QString name;   // human name ("Google Chrome")
		QString path;   // executable
		bool isValid() const { return !path.isEmpty(); }
	};

	static YouTubeAuth &instance();

	State   state() const;
	bool    hasSession() const;            // SignedIn or Rejected (cookies on disk)
	QString accountName() const;           // "" if unknown
	QString accountEmail() const;          // "" if unknown
	QDateTime connectedAt() const;
	// One-line human description of how the session was obtained.
	QString methodLabel() const;

	// "Use my account for every video" (default OFF = only when a video needs
	// it: the first attempt is anonymous and the account is used only for the
	// automatic retry after a sign-in/age/bot error). Keeping the account out
	// of ordinary resolves keeps it out of yt-dlp traffic as much as possible.
	static bool useForEveryVideo();
	static void setUseForEveryVideo(bool on);

	// First Chromium-family browser installed on this machine, or invalid.
	static Browser findBrowser();

	// Browsers yt-dlp can import an existing session from on this platform
	// (yt-dlp ids: "firefox", "chrome", ...) with their human names.
	static QList<QPair<QString, QString>> importableBrowsers();

	// --- sign-in flows (async; end with signInFinished) -------------------
	// Opens Google's sign-in page in the user's browser (see header comment).
	void startBrowserSignIn();
	// Import from a browser the user is already signed into (yt-dlp id).
	void importFromBrowser(const QString &ytdlpBrowserId);
	// Import a Netscape cookies.txt file (synchronous, small file).
	bool importCookiesFile(const QString &path, QString *error);
	// Abort a running sign-in / import. Emits signInFinished(false, ...).
	void cancelSignIn();
	bool isSigningIn() const { return m_flow != Flow::None; }

	// Forget the account: delete the encrypted session + metadata.
	void signOut();

	// --- used by StreamResolver ---------------------------------------------
	// Write a private plaintext copy of the session for ONE yt-dlp process into
	// `dir` and return its path ("" if no session). The caller deletes it once
	// the process is gone.
	QString materializeCookieFile(const QString &dir);
	// yt-dlp reported the session as dead (rotated / signed out elsewhere).
	void reportSessionRejected(const QString &reason);
	// A run that used the session went through cleanly.
	void reportSessionWorked();

	// Kill any running sign-in browser / import process. Called from
	// StreamResolver::shutdown() (i.e. sb_kill) BEFORE the scratch dir wipe.
	void shutdown();

	// Pure helpers, exposed for StreamResolver.
	// true if the Netscape text carries a signed-in YouTube session.
	static bool netscapeHasYouTubeLogin(const QByteArray &netscape);
	// Keep only youtube.com / google.com lines (+ the Netscape header).
	static QByteArray filterNetscape(const QByteArray &netscape);

signals:
	void stateChanged();
	// Human progress line while a sign-in / import runs.
	void signInProgress(const QString &line);
	void signInFinished(bool ok, const QString &message);

private:
	explicit YouTubeAuth(QObject *parent = nullptr);

	enum class Flow { None, Browser, Import };
	enum class Step { Launching, Connecting, WaitingForLogin, Finishing };

	// Secure store
	static QString storeDir();
	static QString storePath();
	bool    loadSession(QByteArray &netscape) const;
	bool    saveSession(const QByteArray &netscape, const QString &method,
	                    const QString &name, const QString &email);

	// Browser flow internals
	void browserReadOutput();
	void browserPoll();
	void tryConnectDevTools(const QString &host, quint16 port, const QString &path);
	void onCookiesPolled(const QJsonArray &cookies);
	void beginFinishing();
	void finishStepAccountInfo(const QString &targetId);
	void finishStepCapture();
	void finishFlow(bool ok, const QString &message);
	void teardownBrowser(bool graceful);
	void retire(QObject *o);
	void wipeProfile(const QString &profile);

	static QByteArray cookiesToNetscape(const QJsonArray &cookies);
	static void parseAccountInfo(const QString &text, QString &name, QString &email);

	Flow                 m_flow = Flow::None;
	Step                 m_step = Step::Launching;
	QProcess *           m_browser = nullptr;
	QString              m_browserName;
	QString              m_profileDir;
	QByteArray           m_browserOut;
	CdpClient *          m_cdp = nullptr;
	QTimer *             m_poll = nullptr;
	QTimer *             m_deadline = nullptr;
	bool                 m_pollBusy = false;
	bool                 m_captured = false;   // session saved: browser exit is expected now
	QList<QPointer<QObject>> m_closing;        // winding-down objects shutdown() must delete
	QString              m_pendingName;
	QString              m_pendingEmail;
	QProcess *           m_import = nullptr;
	QString              m_importTmp;
	QString              m_importBrowser;
};

#endif // rpsbsrc__youtube_auth_H__
