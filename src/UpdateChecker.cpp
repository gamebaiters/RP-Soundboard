//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// GameBaiters fork: update channel served from gamebaiters branch on GitHub.
// Expected version.xml at GAMEBAITERS_VERSION_URL — see GAMEBAITERS_BRANCH.md.
//----------------------------------

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QMessageBox>
#include <QApplication>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

#include "buildinfo.h"

#include "UpdateChecker.h"
#include "ts3log.h"
#include "updater_qt.h"
#include "ConfigModel.h"

#define CHECK_URL "https://raw.githubusercontent.com/gamebaiters/RP-Soundboard/gamebaiters/version.xml"


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
std::string toStdStringUtf8(const QString &str)
{
	QByteArray arr = str.toUtf8();
	std::string res(arr.constData(), arr.size());
	return res;
}

//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool isValid(const QXmlStreamReader &xml)
{
	return !(xml.hasError() || xml.atEnd());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
UpdateChecker::UpdateChecker( QObject *parent /*= NULL*/ ) :
	QObject(parent),
	m_mgr(NULL),
	m_updater(NULL),
	m_config(NULL),
	m_explicitCheck(false)
{

}

//---------------------------------------------------------------
// The updater window is a PARENTLESS top-level QWidget (new
// UpdaterWindow()), so it is not reaped by Qt's parent-child
// teardown. Left alive at plugin unload it kept a vtable into
// freed DLL code and crashed the client on exit whenever an update
// was in flight. Destroy it (and the network manager) explicitly.
//---------------------------------------------------------------
UpdateChecker::~UpdateChecker()
{
	if (m_updater)
	{
		m_updater->disconnect(this);
		delete m_updater;
		m_updater = NULL;
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::startCheck(bool explicitCheck, ConfigModel *config)
{
	m_explicitCheck = explicitCheck;
	m_config = config;
	
	uint currentTime = QDateTime::currentDateTime().toTime_t();
	if (!m_explicitCheck && m_config && currentTime < m_config->getNextUpdateCheck())
		return;

	m_mgr = new QNetworkAccessManager(this);
	connect(m_mgr, SIGNAL(finished(QNetworkReply*)), this, SLOT(onFinishDownload(QNetworkReply*)));

	QUrl url(CHECK_URL);
	QNetworkRequest request;
	request.setUrl(url);
	setUserAgent(request);
	loading = Loading::mainXml;
	m_mgr->get(request);
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::onFinishDownload(QNetworkReply *reply)
{
	switch (loading)
	{
	case Loading::mainXml:
		onFinishDownloadXml(reply);
		break;
	case Loading::features:
		onFinishDownloadFeatures(reply);
		break;
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::onFinishDownloadXml(QNetworkReply *reply)
{
	if(reply->error() != QNetworkReply::NoError)
	{
		std::string err = toStdStringUtf8(reply->errorString());
		logError("UpdateChecker: Error requesting version document %s.\nError-String: %s", CHECK_URL, err.c_str());
	}
	else
	{
		parseXml(reply);
		if(m_verInfo.valid() && m_verInfo.build > buildinfo_getVersionNumber(3))
		{
			if (!m_verInfo.featuresUrl.isEmpty())
			{
				QNetworkRequest request;
				request.setUrl(QUrl(m_verInfo.featuresUrl));
				setUserAgent(request);
				loading = Loading::features;
				m_mgr->get(request);
			}
			else
				askUserForUpdate();
		}
		else // no new version
		{
			if (m_config)
			{
				// No update found -> don't bother checking again for a day
				uint currentTime = QDateTime::currentDateTime().toTime_t();
				m_config->setNextUpdateCheck(currentTime + 60 * 60 * 24);
			}

			if (m_explicitCheck)
			{
				QMessageBox::information(NULL, tr("Update Check"),
					tr("Your version of GameBaiters - Soundboard is up to date."));
			}
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::onFinishDownloadFeatures(QNetworkReply *reply)
{
	if (reply->error() != QNetworkReply::NoError)
	{
		std::string err = toStdStringUtf8(reply->errorString());
		std::string url = toStdStringUtf8(reply->url().toString());
		logError("UpdateChecker: Error requesting features document %s.\nError-String: %s", url.c_str(), err.c_str());
	}
	else
	{
		m_verInfo.features = reply->readAll();
	}

	askUserForUpdate();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::parseXml(QIODevice *device)
{
	m_verInfo.reset();

	QXmlStreamReader xml;
	xml.setDevice(device);

	while(isValid(xml))
	{
		QXmlStreamReader::TokenType token = xml.readNext();
		if(token == QXmlStreamReader::StartElement && xml.name() == "product")
			parseProduct(xml);
	}

	xml.clear();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::parseProduct( QXmlStreamReader &xml )
{
	if (xml.attributes().value("descVersion") == "1" && 
		xml.attributes().value("name") == "rp_soundboard")
	{
		m_verInfo.productName = xml.attributes().value("name").toString();
		xml.readNext();
		while(isValid(xml) && !(xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == "product"))
		{
			if(xml.tokenType() == QXmlStreamReader::StartElement)
				parseProductInner(xml);
			xml.readNext();
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::parseProductInner( QXmlStreamReader &xml )
{
	if(xml.name() == "latestVersion")
	{
		xml.readNext();
		m_verInfo.build = xml.text().toInt();
	}
	else if(xml.name() == "latestVersionString")
	{
		xml.readNext();
		m_verInfo.version = xml.text().toString();
	}
	else if(xml.name() == "latestDownload")
	{
		xml.readNext();
		while(isValid(xml) && !(xml.tokenType() == QXmlStreamReader::EndElement && xml.name() == "latestDownload"))
		{
			if(xml.tokenType() == QXmlStreamReader::StartElement && xml.name() == "url")
			{
				xml.readNext();
				m_verInfo.latestDownload = xml.text().toString();
			}
			xml.readNext();
		}
	}
	else if (xml.name() == "featuresUrl")
	{
		xml.readNext();
		m_verInfo.featuresUrl = xml.text().toString();
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::askUserForUpdate()
{
	QMessageBox msgBox0;
	msgBox0.setTextFormat(Qt::RichText);
	msgBox0.setText(tr("A new version of GameBaiters - Soundboard is available (%1).<br /><br />"
		"Would you like to download and install it?").arg(m_verInfo.version));
	msgBox0.setIcon(QMessageBox::Information);
	msgBox0.setWindowTitle(tr("New version of GameBaiters - Soundboard!"));
	msgBox0.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
	msgBox0.setDefaultButton(QMessageBox::Yes);
	if (m_verInfo.features.length() > 0)
		msgBox0.setDetailedText(m_verInfo.features);
	if(msgBox0.exec() == QMessageBox::Yes)
	{
		// version.xml ships a single Windows download URL; rewrite the
		// file name to match the running platform so Linux + macOS pull
		// their own .ts3_plugin instead of the Windows one (which would
		// fail to load on every other OS). The release pipeline always
		// uploads all three artefacts to the same tag URL.
		QString dl = m_verInfo.latestDownload;
#if defined(__APPLE__)
		dl.replace(QStringLiteral("_win64.ts3_plugin"),
		           QStringLiteral("_macos_x86_64.ts3_plugin"));
#elif defined(__linux__)
		dl.replace(QStringLiteral("_win64.ts3_plugin"),
		           QStringLiteral("_linux_amd64.ts3_plugin"));
#endif
		QUrl url(dl);
		QFileInfo info(QDir::temp(), url.fileName());
		m_updater = new UpdaterWindow();
		connect(m_updater, SIGNAL(finished()), this, SLOT(onFinishedUpdate()));
		m_updater->show();
		m_updater->startDownload(url, info, true);
	}
	else
	{
		if (m_config)
		{
			// Don't bother user for 3 days
			uint currentTime = QDateTime::currentDateTime().toTime_t();
			m_config->setNextUpdateCheck(currentTime + 60 * 60 * 24 * 3);
		}
	}
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::onFinishedUpdate()
{
	if(m_updater->getSuccess())
	{
		// The update helper (already launched) waits for a GRACEFUL client
		// exit before touching anything; closing all windows is how we ask
		// TeamSpeak to quit cleanly. See UpdaterWindow::executeFile.
		logInfo("[updater] download ok, helper launched - asking TeamSpeak to close");
		QApplication::closeAllWindows();
	}
	else
	{
		logInfo("[updater] update failed - offering the manual download link");
		QMessageBox msgBox;
		msgBox.setTextFormat(Qt::RichText);
		msgBox.setText(tr("The Update to %1 failed.<br /><br />"
			"Please download it manually here: <br /><a href=\"%2\">%2</a>")
			.arg(m_verInfo.version.isEmpty() ? tr("build %1").arg(m_verInfo.build) : tr("version %1").arg(m_verInfo.version))
			.arg(m_verInfo.latestDownload));
		msgBox.setIcon(QMessageBox::Information);
		msgBox.setWindowTitle(tr("Update failed"));
		msgBox.setStandardButtons(QMessageBox::Close);
		msgBox.setDefaultButton(QMessageBox::Close);
		msgBox.exec();
	}

	m_updater->deleteLater();
	m_updater = NULL;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::version_info_t::reset()
{
	productName = QString();
	build = 0;
	latestDownload = QString();
	version = QString();
	featuresUrl = QString();
	features = QString();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
bool UpdateChecker::version_info_t::valid()
{
	return !productName.isNull() && !productName.isEmpty() &&
		!latestDownload.isNull() && !latestDownload.isEmpty() &&
		build > 0;
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
QByteArray UpdateChecker::getUserAgent() // static
{
	return QByteArray("GameBaiters - Soundboard Update Checker, ") + buildinfo_getPluginVersion();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
void UpdateChecker::setUserAgent(QNetworkRequest& request) // static
{
	request.setRawHeader("User-Agent", getUserAgent());
}
