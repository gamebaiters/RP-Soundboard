// src/updater_qt.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//
// GameBaiters rework (v2.3.3): the window is now built in code (no .ui).
// The QProgressBar was replaced with a custom-painted bar — on the Windows
// native style (and especially with user font scaling / ClearType tweaks)
// the QSS-styled chunk repainted unreliably: stuck at 0%, fill leaking
// outside the groove, missing terminal state. Same fix as the export
// dialog's ExportBar. The window is also far more verbose now: stage text,
// MB / percent / speed, and a live step log.
//----------------------------------

#pragma once
#ifndef rpsbsrc__updater_qt_H__
#define rpsbsrc__updater_qt_H__


#include <QDialog>
#include <QUrl>
#include <QFileInfo>
#include <QFile>
#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QNetworkReply>

class QLabel;
class QPushButton;
class QPlainTextEdit;


class UpdaterWindow : public QDialog
{
	Q_OBJECT

public:
	explicit UpdaterWindow(QWidget *parent = 0);
	void startDownload(const QUrl &url, const QFileInfo &fileInfo, bool execute = false);
	void startRequest(const QUrl & url);
	inline bool getSuccess() const {
		return m_success;
	}

public slots:
	void onReadyRead();
	void onDownloadProgress(qint64 bytes, qint64 total);
	void onClickedCancel();
	void onFinished();

signals:
	void finished();

private:
	bool executeFile();
	// One line into the visible step log + the plugin log (verbose updater).
	void appendLog(const QString &line);
	void setStatus(const QString &text);

	QLabel         *m_titleLabel  = nullptr;
	QLabel         *m_statusLabel = nullptr;
	QWidget        *m_bar         = nullptr;  // custom-painted bar (UpdaterBar)
	QPlainTextEdit *m_log         = nullptr;  // live step log
	QPushButton    *m_cancelBtn   = nullptr;

	QElapsedTimer   m_speedTimer;             // download speed estimation
	qint64          m_lastBytes   = 0;
	double          m_speedBps    = 0.0;

	QUrl m_url;
	QFileInfo m_fileinfo;
	QFile *m_file;
	QNetworkAccessManager *m_manager;
	QNetworkReply *m_reply;
	int m_redirects;
	bool m_execute;
	bool m_canceled;
	bool m_success;
};


#endif // rpsbsrc__updater_qt_H__
