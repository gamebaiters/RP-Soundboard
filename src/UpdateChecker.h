// src/UpdateChecker.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__UpdateChecker_H__
#define rpsbsrc__UpdateChecker_H__

#include <QObject>
#include <QXmlStreamReader>
#include <QNetworkRequest>

class QNetworkReply;
class QNetworkAccessManager;
class UpdaterWindow;
class ConfigModel;

class UpdateChecker : public QObject
{
	Q_OBJECT

public:
	struct version_info_t
	{
		QString productName;
		int build;
		QString latestDownload;
		QString version;
		QString featuresUrl;
		QString features;

		void reset();
		bool valid();
	};

public:
	explicit UpdateChecker(QObject *parent = NULL);
	~UpdateChecker() override;
	void startCheck(bool explicitCheck = true, ConfigModel *config = NULL);
	static QByteArray getUserAgent();
	static void setUserAgent(QNetworkRequest &request);

public slots:
	void onFinishedUpdate();
	void onFinishDownload(QNetworkReply *reply);

private:
	void parseXml(QIODevice *device);
	void parseProduct(QXmlStreamReader &xml);
	void parseProductInner(QXmlStreamReader &xml);
	void onFinishDownloadXml(QNetworkReply *reply);
	void onFinishDownloadFeatures(QNetworkReply * reply);
	void askUserForUpdate();
#if defined(__APPLE__)
	// Real GitHub check: HEAD the actual macOS asset for m_verInfo
	// before ever prompting, so an explicit "Check for Updates" on
	// macOS only offers an update when a macOS build genuinely exists
	// for that version (the self-hosted macOS CI job can lag days
	// behind Windows/Linux).
	QString macAssetUrl() const;
	void probeMacAssetThenProceed();
	void onFinishMacAssetProbe(QNetworkReply *reply);
#endif

private:
	enum class Loading
	{
		mainXml,
		features,
#if defined(__APPLE__)
		macAssetProbe,
#endif
	} loading;

	QNetworkAccessManager *m_mgr;
	version_info_t m_verInfo;
	UpdaterWindow *m_updater;
	ConfigModel *m_config;
	bool m_explicitCheck;
};

#endif // rpsbsrc__UpdateChecker_H__
