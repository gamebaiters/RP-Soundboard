// src/updater_qt.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "buildinfo.h"
#include "updater_qt.h"
#include "style_helper.h"
#include "ts3log.h"
#include <QMessageBox>
#include <QProcess>
#include <QFile>
#include <QTextStream>
#include <QDir>



//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
UpdaterWindow::UpdaterWindow( QWidget *parent /*= 0*/ ) :
	QDialog(parent),
	ui(new Ui::updaterWindow),
	m_file(NULL),
	m_manager(NULL),
	m_reply(NULL),
	m_redirects(0),
	m_execute(false),
	m_canceled(false),
	m_success(false)
{
	ui->setupUi(this);
	this->setStyleSheet(StyleHelper::loadDarkStyle());
	connect(ui->buttonBox, SIGNAL(clicked(QAbstractButton*)), this, SLOT(onClickedCancel(QAbstractButton*)));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::startDownload(const QUrl &url, const QFileInfo &fileInfo, bool execute /*= false*/)
{
	m_url = url;
	m_fileinfo = fileInfo;
	m_execute = execute;

	if(m_fileinfo.fileName().isEmpty())
	{
		QFileInfo info(url.path());
		m_fileinfo.setFile(info.fileName());
	}

	logInfo("Downloading update from '%s' to '%s'", m_url.toString().toUtf8().data(), m_fileinfo.absoluteFilePath().toUtf8().data());

	m_file = new QFile(m_fileinfo.filePath());
	if(!m_file->open(QIODevice::WriteOnly))
	{
		logError("Unable to write to file %s", m_fileinfo.absoluteFilePath().toUtf8().data());
		QMessageBox::information(this, "Error", "Unable to save the file.");
		delete m_file;
		m_file = NULL;
		return;
	}

	ui->statusLabel->setText("Downloading Update...");
	m_manager = new QNetworkAccessManager(this);
	startRequest(url);	
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::startRequest(const QUrl &url)
{
	QNetworkRequest request(url);
	request.setRawHeader("User-Agent", QByteArray("RP Soundboard Updater, ") + buildinfo_getPluginVersion());
	m_reply = m_manager->get(QNetworkRequest(url));
	connect(m_reply, SIGNAL(readyRead()), this, SLOT(onReadyRead()));
	connect(m_reply, SIGNAL(downloadProgress(qint64,qint64)), this, 
		SLOT(onDownloadProgress(qint64, qint64)));
	connect(m_reply, SIGNAL(finished()), this, SLOT(onFinished()));
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::onReadyRead()
{
	if(m_file)
		m_file->write(m_reply->readAll());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::onDownloadProgress(qint64 bytes, qint64 total)
{
	if(m_canceled)
		return;

	ui->progressBar->setMaximum(total);
	ui->progressBar->setValue(bytes);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::onFinished()
{
	if(m_canceled)
	{
		logDebug("Canceled download of update");
		m_canceled = false;
		if(m_file)
		{
			m_file->close();
			m_file->remove();
			delete m_file;
			m_file = NULL;
		}
		m_reply->deleteLater();
		m_reply = NULL;
		this->hide();
	}
	else
	{
		m_file->flush();
		m_file->close();

		QVariant redirect = m_reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
		if(m_reply->error())
		{
			m_file->remove();
			QMessageBox::information(this, "Error", QString("Could not download file: ") + m_reply->errorString());
		}
		else if(redirect.isValid())
		{
			if(m_redirects < 10)
			{
				m_redirects++;
				QUrl url = m_url.resolved(redirect.toUrl());
				logInfo("Download of update redirected to %s", url.toString().toUtf8().data());
				m_url = url;
				m_reply->deleteLater();
				m_reply = NULL;
				m_file->open(QIODevice::WriteOnly);
				m_file->resize(0);
				startRequest(url);
				return;
			}
			else
			{
				logError("Download of update redirected more than 10 times, maybe a redirection loop?");
				m_success = false;
			}
		}
		else
		{
			m_success = true;
			if(m_execute)
				m_success = executeFile();
			this->hide();
		}

		m_reply->deleteLater();
		m_reply = NULL;
		delete m_file;
		m_file = NULL;
		m_manager = NULL;
		m_redirects = 0;
	}

	emit finished();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool UpdaterWindow::executeFile()
{
#ifdef _WIN32
	QString helperPath = QDir::temp().absoluteFilePath("rpsb_update_helper.bat");
	QFile helper(helperPath);
	if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		logError("Could not write update helper to %s", helperPath.toUtf8().data());
		return false;
	}
	QString tplugin = QDir::toNativeSeparators(m_fileinfo.absoluteFilePath());
	QTextStream out(&helper);
	out << "@echo off\r\n";
	out << "REM GameBaiters Soundboard auto-update helper (generated at runtime)\r\n";
	out << "echo Closing TeamSpeak 3 to apply Soundboard update...\r\n";
	out << "timeout /t 2 /nobreak >nul\r\n";
	out << "taskkill /F /IM ts3client_win64.exe >nul 2>&1\r\n";
	out << "taskkill /F /IM ts3client_win32.exe >nul 2>&1\r\n";
	out << "timeout /t 1 /nobreak >nul\r\n";
	out << "del /F /Q \"%APPDATA%\\TS3Client\\plugins\\rp_soundboard_fx_win64.dll\" 2>nul\r\n";
	out << "del /F /Q \"%APPDATA%\\TS3Client\\plugins\\rp_soundboard_fx_win32.dll\" 2>nul\r\n";
	out << "del /F /Q \"%APPDATA%\\TS3Client\\plugins\\rp_soundboard_win64.dll\" 2>nul\r\n";
	out << "del /F /Q \"%APPDATA%\\TS3Client\\plugins\\rp_soundboard_win32.dll\" 2>nul\r\n";
	out << "del /F /Q \"%APPDATA%\\TS3Client\\plugins\\rp_soundboard.dll\" 2>nul\r\n";
	out << "start \"\" \"" << tplugin << "\"\r\n";
	helper.close();

	bool status = QProcess::startDetached("cmd.exe",
		QStringList() << "/c" << helperPath);
	if (!status)
	{
		logError("Could not start update helper at %s", helperPath.toUtf8().data());
		return false;
	}
	return true;
#elif defined(__linux__) || defined(__APPLE__)
	// Unix auto-update helper. The script:
	//   1. Waits for TS3 to release the plugin library
	//   2. Force-kills any lingering ts3client process (Linux + macOS)
	//   3. Deletes every prior plugin .so / .dylib variant
	//      Config rp_soundboard.ini at the TS3 root is preserved.
	//   4. Hands off to TS3's package_inst (Linux) or `open` (macOS)
	QString helperPath = QDir::temp().absoluteFilePath("rpsb_update_helper.sh");
	QFile helper(helperPath);
	if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		logError("Could not write update helper to %s", helperPath.toUtf8().data());
		return false;
	}
	QString tplugin = m_fileinfo.absoluteFilePath();
	QTextStream out(&helper);
	out << "#!/usr/bin/env bash\n";
	out << "# GameBaiters Soundboard auto-update helper (generated at runtime)\n";
	out << "set -u\n";
	out << "sleep 2\n";
#ifdef __APPLE__
	out << "osascript -e 'tell application \"TeamSpeak 3\" to quit' 2>/dev/null || true\n";
	out << "sleep 1\n";
	out << "pkill -9 -x ts3client 2>/dev/null || true\n";
	out << "pkill -9 -if 'TeamSpeak 3' 2>/dev/null || true\n";
#else
	out << "pkill -9 -x ts3client_linux_amd64 2>/dev/null || true\n";
	out << "pkill -9 -x ts3client_linux_x86 2>/dev/null || true\n";
#endif
	out << "sleep 1\n";
	out << "for base in \"$HOME/.ts3client\" \"$HOME/Library/Application Support/TS3Client\"; do\n";
	out << "    [ -d \"$base/plugins\" ] || continue\n";
	out << "    for lib in \\\n";
	out << "        librp_soundboard_fx.so   librp_soundboard.so   libsoundboard.so   \\\n";
	out << "        rp_soundboard_fx.so      rp_soundboard.so      soundboard.so      \\\n";
	out << "        librp_soundboard_fx.dylib librp_soundboard.dylib libsoundboard.dylib \\\n";
	out << "        rp_soundboard_fx.dylib   rp_soundboard.dylib   soundboard.dylib   \\\n";
	out << "        librp_soundboard_fx_linux_amd64.so librp_soundboard_linux_amd64.so; do\n";
	out << "        rm -f \"$base/plugins/$lib\" 2>/dev/null\n";
	out << "    done\n";
	out << "done\n";
#ifdef __APPLE__
	out << "open \"" << tplugin << "\" 2>/dev/null || true\n";
#else
	out << "if command -v package_inst >/dev/null 2>&1; then\n";
	out << "    package_inst \"" << tplugin << "\"\n";
	out << "elif command -v xdg-open >/dev/null 2>&1; then\n";
	out << "    xdg-open \"" << tplugin << "\"\n";
	out << "else\n";
	out << "    echo \"package_inst not found in PATH; install manually: " << tplugin << "\" >&2\n";
	out << "fi\n";
#endif
	helper.close();

	helper.setPermissions(helper.permissions()
		| QFile::ExeUser | QFile::ExeGroup | QFile::ExeOther);

	bool status = QProcess::startDetached("/bin/bash",
		QStringList() << helperPath);
	if (!status)
	{
		logError("Could not start update helper at %s", helperPath.toUtf8().data());
		return false;
	}
	return true;
#else
	bool status = QProcess::startDetached("package_inst",
		QStringList(m_fileinfo.absoluteFilePath()));
	if(!status)
		logError("Error starting package_inst with cmd line \"%s\"",
			m_fileinfo.absoluteFilePath().toUtf8().data());
	return status;
#endif
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdaterWindow::onClickedCancel(QAbstractButton*)
{
	if(!m_canceled)
	{
		logDebug("Cancelling download of update...");
		m_canceled = true;
		ui->statusLabel->setText("Canceling Download...");
		m_reply->abort();
	}
}


