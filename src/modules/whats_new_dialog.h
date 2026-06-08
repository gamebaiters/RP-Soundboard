// "What's new in this version" dialog. Shown once on startup whenever
// the persisted lastSeenVersion (QSettings) is older than the compiled
// TS3SB_VERSION_BUILD. Reads release-notes.txt from the Qt resource
// bundle and pulls out the section matching the current version.
#pragma once

#include <QDialog>

class QTextBrowser;
class QPushButton;

class WhatsNewDialog : public QDialog {
    Q_OBJECT
public:
    explicit WhatsNewDialog(const QString &versionString,
                            const QString &notesBody,
                            QWidget *parent = nullptr);

    // Convenience entry point: checks QSettings "lastSeenVersion" against
    // the compiled TS3SB_VERSION_BUILD, extracts the matching section
    // from the bundled release-notes.txt and pops the dialog if a new
    // version has run for the first time. No-op when the user has
    // already seen this version, when no notes resource is bundled, or
    // on a brand-new install (lastSeenVersion == 0).
    static void showIfUpdated(QWidget *parent = nullptr);

private:
    QTextBrowser *m_body;
    QPushButton  *m_close;
};
