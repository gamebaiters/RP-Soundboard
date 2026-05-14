// src/about_qt.h - GameBaiters Soundboard About Dialog
#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

class AboutQt : public QWidget
{
	Q_OBJECT

public:
	explicit AboutQt(QWidget *parent = nullptr);
	~AboutQt();

protected:
	void paintEvent(QPaintEvent *evt) override;
	void mousePressEvent(QMouseEvent *evt) override;

private:
	void startSupernova();

	int           m_bgClicks   = 0;
	bool          m_novaActive = false;
	bool          m_novaResetting = false;
	QElapsedTimer m_novaClock;
	QElapsedTimer m_resetClock;
	QTimer        m_novaTimer;
};
