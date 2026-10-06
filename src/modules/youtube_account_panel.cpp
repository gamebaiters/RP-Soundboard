// src/modules/youtube_account_panel.cpp
//----------------------------------
// See youtube_account_panel.h.
//----------------------------------

#include "youtube_account_panel.h"
#include "youtube_auth.h"
#include "help_bubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QCheckBox>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QLocale>

YouTubeAccountPanel::YouTubeAccountPanel(QWidget *parent)
	: QWidget(parent)
{
	YouTubeAuth &A = YouTubeAuth::instance();

	auto *lay = new QVBoxLayout(this);
	lay->setContentsMargins(0, 0, 0, 0);
	lay->setSpacing(4);

	m_status = new QLabel(this);
	QFont bf = m_status->font();
	bf.setBold(true);
	m_status->setFont(bf);
	m_status->setWordWrap(true);
	lay->addWidget(m_status);

	m_detail = new QLabel(this);
	m_detail->setWordWrap(true);
	m_detail->setStyleSheet("color: palette(mid);");
	lay->addWidget(m_detail);

	// --- action row -------------------------------------------------------
	auto *row = new QHBoxLayout;
	row->setContentsMargins(0, 2, 0, 0);
	row->setSpacing(6);

	m_signIn = new QPushButton(this);
	QFont sf = m_signIn->font();
	sf.setBold(true);
	m_signIn->setFont(sf);
	const YouTubeAuth::Browser b = YouTubeAuth::findBrowser();
	m_signIn->setToolTip(b.isValid()
		? tr("Opens Google's own sign-in page in %1, in a separate private window.\n"
		     "You sign in there as usual (passkey, 2-step verification...):\n"
		     "the soundboard never sees your password.").arg(b.name)
		: tr("No Chrome, Edge or Brave browser was found: use \"Other ways\"."));
	connect(m_signIn, &QPushButton::clicked, this, [this]{
		m_lastResult.clear();
		YouTubeAuth::instance().startBrowserSignIn();
	});
	row->addWidget(m_signIn);

	m_cancel = new QPushButton(tr("Cancel"), this);
	connect(m_cancel, &QPushButton::clicked, this, []{ YouTubeAuth::instance().cancelSignIn(); });
	row->addWidget(m_cancel);

	m_signOut = new QPushButton(tr("Disconnect"), this);
	m_signOut->setToolTip(tr("Delete the saved YouTube session from this computer."));
	connect(m_signOut, &QPushButton::clicked, this, [this]{ confirmSignOut(); });
	row->addWidget(m_signOut);

	m_more = new QToolButton(this);
	m_more->setText(tr("Other ways"));
	m_more->setPopupMode(QToolButton::InstantPopup);
	m_more->setToolButtonStyle(Qt::ToolButtonTextOnly);
	auto *menu = new QMenu(m_more);
	QMenu *fromBrowser = menu->addMenu(tr("Use a browser where I'm already signed in to YouTube"));
	for (const auto &ib : YouTubeAuth::importableBrowsers()) {
		const QString id = ib.first;
		QAction *act = fromBrowser->addAction(ib.second);
		connect(act, &QAction::triggered, this, [this, id]{
			m_lastResult.clear();
			YouTubeAuth::instance().importFromBrowser(id);
		});
	}
	menu->addAction(tr("Import a cookies.txt file..."), this, [this]{ importFile(); });
	m_more->setMenu(menu);
	row->addWidget(m_more);

	row->addWidget(new HelpBubble(tr(
		"How the YouTube sign-in works:\n"
		"- Google's real sign-in page opens in your own browser (Chrome, Edge...),\n"
		"  in a fresh, separate window. You sign in there exactly as usual.\n"
		"- The soundboard never sees your password. As soon as the sign-in\n"
		"  completes it keeps ONLY the YouTube session and closes that window.\n"
		"- The session stays on this computer only: encrypted with your Windows\n"
		"  account (on macOS / Linux: a file only your user can read). It\n"
		"  survives restarts and updates - you sign in once.\n"
		"- Disconnect deletes it. To also end it on Google's side:\n"
		"  myaccount.google.com > Security > Your devices.\n"
		"\n"
		"It is only needed for videos YouTube won't show to anonymous visitors:\n"
		"age-restricted, members-only, private, your Watch Later / Liked\n"
		"playlists, and the \"confirm you're not a bot\" check."), this));
	row->addStretch(1);
	lay->addLayout(row);

	m_useAlways = new QCheckBox(tr("Use the account for every YouTube video"));
	m_useAlways->setChecked(YouTubeAuth::useForEveryVideo());
	connect(m_useAlways, &QCheckBox::toggled, this, [](bool on){
		YouTubeAuth::setUseForEveryVideo(on);
	});
	{
		m_alwaysRow = new QWidget(this);
		auto *ur = new QHBoxLayout(m_alwaysRow);
		ur->setContentsMargins(0, 0, 0, 0);
		ur->setSpacing(6);
		ur->addWidget(m_useAlways);
		ur->addWidget(new HelpBubble(tr(
			"OFF (recommended): links load anonymously first; the account is used\n"
			"automatically ONLY when YouTube asks for a sign-in (age-restricted,\n"
			"members-only, private, \"confirm you're not a bot\").\n"
			"ON: every YouTube link is loaded with your account."), m_alwaysRow));
		ur->addStretch(1);
		lay->addWidget(m_alwaysRow);
	}

	connect(&A, &YouTubeAuth::stateChanged, this, [this]{ refresh(); });
	connect(&A, &YouTubeAuth::signInProgress, this, [this](const QString &line){
		m_progress = line;
		refresh();
	});
	connect(&A, &YouTubeAuth::signInFinished, this,
	        [this](bool ok, const QString &msg){ onSignInFinished(ok, msg); });

	refresh();
}

//----------------------------------------------------------------
void YouTubeAccountPanel::refresh()
{
	const YouTubeAuth &A = YouTubeAuth::instance();
	const YouTubeAuth::State st = A.state();

	const bool busy      = st == YouTubeAuth::State::SigningIn;
	const bool connected = st == YouTubeAuth::State::SignedIn;
	const bool rejected  = st == YouTubeAuth::State::Rejected;

	m_cancel->setVisible(busy);
	m_signIn->setVisible(!busy);
	m_signOut->setVisible(!busy && (connected || rejected));
	m_more->setVisible(!busy && !connected && !rejected);
	m_alwaysRow->setVisible(connected || rejected);

	if (busy) {
		m_status->setText(tr("Signing in..."));
		m_detail->setText(m_progress);
		return;
	}
	m_progress.clear();

	if (connected || rejected) {
		const QString name  = A.accountName();
		const QString email = A.accountEmail();
		QString who;
		if (!name.isEmpty() && !email.isEmpty()) who = QStringLiteral("%1 (%2)").arg(name, email);
		else if (!email.isEmpty())               who = email;
		else                                     who = name;

		if (rejected) {
			m_status->setText(tr("⚠ YouTube no longer accepts the saved session"));
			m_signIn->setText(tr("Reconnect"));
		} else {
			m_status->setText(who.isEmpty() ? tr("✓ YouTube account connected")
			                                : tr("✓ Connected as %1").arg(who));
			m_signIn->setText(tr("Switch account"));
		}

		QStringList parts;
		const QString how = A.methodLabel();
		if (!how.isEmpty()) parts << how;
		const QDateTime at = A.connectedAt();
		if (at.isValid()) parts << tr("since %1").arg(QLocale().toString(at.date(), QLocale::ShortFormat));
		QString detail = parts.join(QStringLiteral(" · "));
		if (rejected) {
			if (!detail.isEmpty()) detail += '\n';
			detail += tr("It expired or was signed out (e.g. from Google's \"Your devices\" page).\n"
			             "Reconnect to keep loading videos that need a sign-in.");
		} else if (!m_lastResult.isEmpty() && m_lastOk) {
			detail = m_lastResult + (detail.isEmpty() ? QString() : QStringLiteral("\n") + detail);
		}
		m_detail->setText(detail);
		return;
	}

	// Signed out
	m_signIn->setText(tr("Sign in with Google"));
	m_status->setText(tr("Not connected"));
	QString detail = tr("Only needed for age-restricted, members-only or private videos, your\n"
	                    "Watch Later / Liked playlists, and YouTube's \"confirm you're not a bot\" check.");
	if (!m_lastResult.isEmpty() && !m_lastOk)
		detail = m_lastResult;
	m_detail->setText(detail);
}

//----------------------------------------------------------------
void YouTubeAccountPanel::onSignInFinished(bool ok, const QString &message)
{
	m_lastOk = ok;
	m_lastResult = message;
	refresh();
}

//----------------------------------------------------------------
void YouTubeAccountPanel::confirmSignOut()
{
	const auto r = QMessageBox::question(this, tr("Disconnect YouTube account"),
		tr("Delete the saved YouTube session from this computer?\n\n"
		   "Videos that need a sign-in will stop loading until you connect again."),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (r != QMessageBox::Yes) return;
	m_lastResult.clear();
	YouTubeAuth::instance().signOut();
}

//----------------------------------------------------------------
void YouTubeAccountPanel::importFile()
{
	const QString path = QFileDialog::getOpenFileName(this, tr("Import cookies.txt"),
		QString(), tr("Cookie files (*.txt);;All files (*)"));
	if (path.isEmpty()) return;
	QString err;
	if (YouTubeAuth::instance().importCookiesFile(path, &err)) {
		m_lastOk = true;
		m_lastResult = tr("YouTube account connected.");
	} else {
		m_lastOk = false;
		m_lastResult = err;
	}
	refresh();
}
