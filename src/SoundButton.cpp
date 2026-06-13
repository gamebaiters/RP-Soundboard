#include "SoundButton.h"
#include "main.h"
#include "ConfigModel.h"
#include "modules/theme.h"
#include "modules/file_metadata.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUuid>
#include <QDrag>
#include <QPainter>
#include <QFontMetrics>
#include <QFileInfo>
#include <QEvent>


const QString &getButtonMime()
{
	static QString uuid = QUuid::createUuid().toString();
	return uuid;
}


SoundButton::SoundButton(QWidget *parent) :
	QPushButton(parent),
	pressing(false),
	dragging(false),
	macroDecoration(false),
	hasOwnStyle(false),
	tooltipPrimed(false)
{
	setAcceptDrops(true);
	setProperty("buttonVariant", QVariant(QString("audio")));
	setObjectName("GBSoundCell");
}


SoundButton::~SoundButton()
{}


void SoundButton::dragEnterEvent(QDragEnterEvent *evt)
{
	if (evt->mimeData()->hasUrls() || evt->mimeData()->hasFormat(getButtonMime()))
	{
		applyBackgroundColor(QColor(153, 204, 255));
		evt->acceptProposedAction();
	}
}


void SoundButton::dragMoveEvent(QDragMoveEvent *evt)
{
	if (evt->mimeData()->hasUrls() || evt->mimeData()->hasFormat(getButtonMime()))
		evt->acceptProposedAction();
}


void SoundButton::dragLeaveEvent(QDragLeaveEvent *)
{
	sb_disableHotkeysTemporarily(false);

	pressing = false;
	dragging = false;
	applyBackgroundColor(backgroundColor);
}


void SoundButton::dropEvent(QDropEvent *evt)
{
	pressing = false;
	dragging = false;
	applyBackgroundColor(backgroundColor);
	SoundButton *button = nullptr;
	if (evt->mimeData()->hasUrls())
	{
		emit fileDropped(evt->mimeData()->urls());
	}
	else if (evt->mimeData()->hasFormat(getButtonMime()) && evt->source() != this)
	{
		button = evt->mimeData()->property("sourceButton").value<SoundButton*>();
		if (button)
		{
			button->adjustSize();
			emit buttonDropped(button);
		}
	}

	QPushButton::dropEvent(evt);
}


void SoundButton::mousePressEvent(QMouseEvent *evt)
{
	// 'switch config' bound to mouse-1 deletes the button before
	// mouseReleaseEvent fires, so clicked() never emits. Disable hotkeys
	// while the button is held down to dodge that race.
	sb_disableHotkeysTemporarily(true);

	pressing = true;
	dragStart = evt->pos();
	QPushButton::mousePressEvent(evt);
}


void SoundButton::mouseMoveEvent(QMouseEvent *evt)
{
	if (pressing && !dragging &&
		(evt->pos() - dragStart).manhattanLength() > 5)
	{
		dragging = true;

		QMimeData *mimeData = new QMimeData;
		mimeData->setData(getButtonMime(), QByteArray());
		mimeData->setProperty("sourceButton", QVariant::fromValue(this));

		QDrag *drag = new QDrag(this);
		drag->setMimeData(mimeData);
		drag->setPixmap(QPixmap(":/icon/img/speaker_icon_3_64.png"));
		drag->exec();
	}

	QPushButton::mouseMoveEvent(evt);
}


void SoundButton::setBackgroundColor(const QColor &color)
{
	backgroundColor = color;
	applyBackgroundColor(color);
}


void SoundButton::setMacroDecoration(bool on)
{
	if (macroDecoration == on) return;
	macroDecoration = on;
	applyBackgroundColor(backgroundColor);
}

void SoundButton::applyBackgroundColor(const QColor &color)
{
	// Qt 5.15.2 QColor() default-ctor: isValid()=false yet alpha()==255.
	// Plain `alpha()!=0` would always take the custom-color branch and
	// force every cell to render solid black.
	const bool hasCustom = color.isValid() && color.alpha() != 0;
	if (macroDecoration) {
		setStyleSheet(
			"  color: #ffffff;"
			"  background-color: qlineargradient(x1:0, y1:0, x2:0, y2:1,"
			"      stop:0 #6a1b9a, stop:1 #4a148c);"
			"  border: 2px solid #ffd54f;"
			"  border-radius: 6px;"
			"  padding: 2px 4px;"
			"  font-weight: bold;");
		hasOwnStyle = true;
	} else if (hasCustom) {
		float brightness = 0.2126f * color.redF()
		                 + 0.7152f * color.greenF()
		                 + 0.0722f * color.blueF();
		QColor textColor = brightness < 0.5f ? Qt::white : Qt::black;
		setStyleSheet(QString(
			"color: %1; background-color: %2;"
			" border-radius: 6px; padding: 2px 4px;")
			.arg(textColor.name(), color.name()));
		hasOwnStyle = true;
	} else {
		// Empty stylesheet -> qApp's themed QPushButton rule wins.
		setStyleSheet(QString());
		hasOwnStyle = false;
	}
	update();
}


void SoundButton::mouseReleaseEvent(QMouseEvent *evt)
{
	sb_disableHotkeysTemporarily(false);

	pressing = false;
	dragging = false;
	QPushButton::mouseReleaseEvent(evt);
}


void SoundButton::setSoundFilePath(const QString &path)
{
	if (soundFilePath == path) return;
	soundFilePath = path;
	tooltipPrimed = false;
	// Empty path -> wipe any prior metadata tooltip so the cell does not
	// keep advertising the previous file. Real probe happens lazily on
	// the next hover.
	if (path.isEmpty())
		setToolTip(QString());
}

void SoundButton::enterEvent(QEvent *evt)
{
	if (!tooltipPrimed && !soundFilePath.isEmpty()) {
		QString tip = FileMetadata::tooltipFor(soundFilePath);
		if (!tip.isEmpty()) setToolTip(tip);
		// Mark primed even on probe failure so we do not hammer FFmpeg
		// on every hover for a truly unreadable file. FileMetadata's
		// internal cache will short-circuit anyway, but this avoids
		// touching it at all on each enter.
		tooltipPrimed = true;
	}
	QPushButton::enterEvent(evt);
}

void SoundButton::setBackgroundImage(const QString &path)
{
	if (path == backgroundImagePath) return;
	backgroundImagePath = path;
	if (path.isEmpty() || !QFileInfo::exists(path)) {
		backgroundPixmap = QPixmap();
	} else {
		backgroundPixmap.load(path);
	}
	update();
}


void SoundButton::paintEvent(QPaintEvent *evt)
{
	QPushButton::paintEvent(evt);
	if (backgroundPixmap.isNull()) return;

	QPainter p(this);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);
	QRect imgRect = rect().adjusted(2, 2, -2, -2);
	p.drawPixmap(imgRect, backgroundPixmap);

	QString t = text();
	if (t.isEmpty()) return;
	QFontMetrics fm(font());
	QRect tr = fm.boundingRect(rect().adjusted(4, 4, -4, -4),
	                           Qt::AlignCenter | Qt::TextWordWrap, t);
	tr.adjust(-6, -3, 6, 3);
	tr.moveCenter(rect().center());
	p.fillRect(tr, QColor(0, 0, 0, 200));
	p.setPen(Qt::white);
	p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, t);
}
