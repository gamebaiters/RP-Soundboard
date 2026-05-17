#pragma once
#include <QFrame>
#include <QGridLayout>
#include <QParallelAnimationGroup>
#include <QScrollArea>
#include <QString>
#include <QToolButton>
#include <QWidget>

class ExpandableSection : public QWidget
{
	Q_OBJECT
public:
	explicit ExpandableSection(const QString &title = "", int animationDuration = 300, QWidget *parent = 0);
	void setContentLayout(QLayout & contentLayout);
	bool isExpanded() const { return toggleButton.isChecked(); }

	// Give the section a stable id so its expanded/collapsed state is
	// remembered across dialog re-opens (stored in QSettings under
	// "sections/<key>"). Call this AFTER setContentLayout so the restored
	// state can animate correctly. Passing an empty key disables it.
	void setPersistenceKey(const QString &key);

public slots:
	void setExpanded(bool expanded);

private:
	QGridLayout mainLayout;
	QToolButton toggleButton;
	QFrame headerLine;
	QParallelAnimationGroup toggleAnimation;
	QScrollArea contentArea;
	int animationDuration = 300;
	int m_collapsedHeight = 0;
	int m_contentHeight   = 0;
	QString m_persistKey;
};

