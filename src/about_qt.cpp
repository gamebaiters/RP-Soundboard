// src/about_qt.cpp
//----------------------------------
// GameBaiters Soundboard
// Copyright (c) 2026 GameBaiters - https://gamebaiters.net
// Fork rewritten from scratch on top of the original
// RP Soundboard source by Marius Graefe (2015-2019).
//----------------------------------


#include "about_qt.h"
#include "buildinfo.h"
#include "style_helper.h"

extern "C"
{
const char *av_version_info();
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
AboutQt::AboutQt(QWidget *parent) :
	QWidget(parent, Qt::Window | Qt::WindowTitleHint /*| Qt::CustomizeWindowHint*/ | Qt::WindowCloseButtonHint),
	ui(new Ui::AboutQt)
{
	const char *ffmpeg_version = av_version_info();
	ui->setupUi(this);
	this->setStyleSheet(StyleHelper::loadDarkStyle());
	ui->l_version->setText(QString(buildinfo_getPluginVersion()) +
		"\nBuild on " + buildinfo_getBuildDate() + " " + buildinfo_getBuildTime() +
	    "\nFFmpeg Version: " + (ffmpeg_version ? ffmpeg_version : "unknown") + 
		"\nLinked against Qt " QT_VERSION_STR);
	setFixedSize(size());
}


//---------------------------------------------------------------
// Purpose: 
//---------------------------------------------------------------
AboutQt::~AboutQt()
{
	delete ui;
}
