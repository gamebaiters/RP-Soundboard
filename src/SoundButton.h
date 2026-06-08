#ifndef SOUNDBUTTON_H
#define SOUNDBUTTON_H

#include <QPushButton>
#include <QList>
#include <QUrl>
#include <QPixmap>
#include <QString>

class ConfigModel;

class SoundButton : public QPushButton
{
	Q_OBJECT

public:
	SoundButton(QWidget *parent);
	virtual ~SoundButton();

	virtual void dragEnterEvent(QDragEnterEvent *evt) override;
	virtual void dragMoveEvent(QDragMoveEvent *evt) override;
	virtual void dragLeaveEvent(QDragLeaveEvent *evt) override;
	virtual void dropEvent(QDropEvent *evt) override;
	virtual void mousePressEvent(QMouseEvent *evt) override;
	virtual void mouseReleaseEvent(QMouseEvent *evt) override;
	virtual void mouseMoveEvent(QMouseEvent *evt) override;
	virtual void paintEvent(QPaintEvent *evt) override;

	void setBackgroundColor(const QColor &color);
	// Macro decoration: applied on top of any custom color so macros are
	// always visually identifiable (yellow border).
	void setMacroDecoration(bool on);
	// Background image stretched across the whole button face. Empty
	// path clears it. The label is redrawn over the image with a
	// translucent black backdrop so the text stays readable.
	void setBackgroundImage(const QString &path);
	// Audio file path used for lazy tooltip-on-hover (FileMetadata).
	// Stored, not decoded - probing happens on enterEvent so opening the
	// soundboard does not pay the cost for every cell.
	void setSoundFilePath(const QString &path);

protected:
	virtual void enterEvent(QEvent *evt) override;

signals:
	void fileDropped(const QList<QUrl>&);
	void buttonDropped(SoundButton *button);

private:
	void applyBackgroundColor(const QColor &color);

	bool pressing;
	bool dragging;
	QPoint dragStart;
	QColor backgroundColor;
	bool macroDecoration;
	bool hasOwnStyle;
	QString backgroundImagePath;
	QPixmap backgroundPixmap;
	QString soundFilePath;
	// True once the lazy tooltip has been populated from FileMetadata
	// for `soundFilePath`. Cleared on setSoundFilePath() so a path
	// swap re-probes on the next hover.
	bool    tooltipPrimed;
};

#endif // SOUNDBUTTON_H
