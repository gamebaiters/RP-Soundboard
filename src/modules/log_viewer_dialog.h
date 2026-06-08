// In-app plugin log viewer. Reads from the in-memory ring populated by
// ts3log.cpp (logMessage). Auto-refreshes at 2 Hz, supports a level
// filter, clear, and copy-to-clipboard. Lives outside the TS3 client
// log so the user can isolate plugin output without grepping the host
// log for "[SB]" prefixes.
#pragma once

#include <QDialog>

class QTextEdit;
class QComboBox;
class QPushButton;
class QCheckBox;
class QTimer;

class LogViewerDialog : public QDialog {
    Q_OBJECT
public:
    explicit LogViewerDialog(QWidget *parent = nullptr);

protected:
    // Only refresh while visible. When the user closes / minimises the
    // viewer we stop the polling timer so logging-on does not lag the
    // soundboard UI in the background.
    void showEvent(class QShowEvent *e) override;
    void hideEvent(class QHideEvent *e) override;

private slots:
    void refresh();
    void onClearClicked();
    void onCopyClicked();
    void onFilterChanged(int);

private:
    // Resolve the physical debug log path (TS3 config dir + rpsb_debug.log).
    // Empty when the config dir is not yet known. Lazy-cached.
    QString fileLogPath() const;

    QTextEdit      *m_text;
    QComboBox      *m_level;
    QCheckBox      *m_autoScroll;
    QPushButton    *m_clear;
    QPushButton    *m_copy;
    QPushButton    *m_close;
    QTimer         *m_timer;
    int             m_minLevel;
    // File-tail tracking: when the debug-log file is enabled and
    // present, the viewer reads its content directly so it mirrors
    // EXACTLY what hits disk (the ring buffer caps at ~1000 entries
    // and rotates over long sessions; the file does not). m_fileSize
    // is the last byte count we successfully read up to.
    mutable QString m_cachedFileLogPath;
    qint64          m_fileSize    = 0;
    bool            m_fileTailed  = false;
    // Cached signature of the last successful refresh - skips the
    // expensive rebuild when neither the ring nor the file has grown.
    int             m_lastRingSize = -1;
    qint64          m_lastFileSize = -1;
    int             m_lastMinLevel = -2;
};
