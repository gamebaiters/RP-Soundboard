// src/howto_qt.h - GameBaiters Soundboard How-To guide
#pragma once

#include <QWidget>

class QTextBrowser;

// Standalone, detailed user guide. Opened from the TS3 "Plugins" menu
// entry directly below "About". All text is built from tr() strings so
// the guide follows the active UI language and re-translates live.
class HowToDialog : public QWidget
{
    Q_OBJECT
public:
    explicit HowToDialog(QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *e) override;

private:
    void rebuildContent();

    QTextBrowser *m_browser = nullptr;
};
