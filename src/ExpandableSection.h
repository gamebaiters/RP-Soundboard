#pragma once
#include <QFrame>
#include <QGridLayout>
#include <QParallelAnimationGroup>
#include <QScrollArea>
#include <QToolButton>
#include <QWidget>

class ExpandableSection : public QWidget
{
	Q_OBJECT
public:
	explicit ExpandableSection(const QString &title = "", int animationDuration = 300, QWidget *parent = 0);
	void setContentLayout(QLayout & contentLayout);
	bool isExpanded() const { return toggleButton.isChecked(); }

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
};

