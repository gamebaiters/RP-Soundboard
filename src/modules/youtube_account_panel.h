// src/modules/youtube_account_panel.h
//----------------------------------
// Settings › Streaming › "YouTube account": the whole UI of the YouTube
// sign-in feature (see youtube_auth.h). Self-contained: talks straight to the
// YouTubeAuth singleton, which persists everything itself the moment a
// sign-in completes - nothing goes through ConfigModel or the dirty flag.
//
//   Not connected  ->  [Sign in with Google]  [Other ways ▾]
//   Signing in     ->  progress line           [Cancel]
//   Connected      ->  ✓ Connected as …        [Switch account] [Disconnect]
//                      [ ] Use the account for every YouTube video
//   Rejected       ->  ⚠ session no longer accepted  [Reconnect] [Disconnect]
//----------------------------------

#pragma once
#ifndef rpsbsrc__youtube_account_panel_H__
#define rpsbsrc__youtube_account_panel_H__

#include <QWidget>
#include <QString>

class QLabel;
class QPushButton;
class QToolButton;
class QCheckBox;

class YouTubeAccountPanel : public QWidget
{
	Q_OBJECT
public:
	explicit YouTubeAccountPanel(QWidget *parent = nullptr);

private:
	void refresh();
	void onSignInFinished(bool ok, const QString &message);
	void confirmSignOut();
	void importFile();

	QLabel      *m_status   = nullptr;   // one bold line: state
	QLabel      *m_detail   = nullptr;   // muted: how / when, or last result
	QPushButton *m_signIn   = nullptr;   // "Sign in with Google" / "Switch account" / "Reconnect"
	QPushButton *m_cancel   = nullptr;
	QPushButton *m_signOut  = nullptr;
	QToolButton *m_more     = nullptr;   // "Other ways" menu
	QCheckBox   *m_useAlways = nullptr;
	QWidget     *m_alwaysRow = nullptr;      // checkbox + its help bubble
	QString      m_progress;             // live line while signing in
	QString      m_lastResult;           // last sign-in result (shown once)
	bool         m_lastOk = true;
};

#endif // rpsbsrc__youtube_account_panel_H__
