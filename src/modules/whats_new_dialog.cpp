#include "whats_new_dialog.h"
#include "../version/version.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QPushButton>
#include <QLabel>
#include <QFile>
#include <QSettings>
#include <QRegularExpression>

namespace {
constexpr const char *kLastSeenKey = "lastSeenVersion";

// Pull the section of release-notes.txt for the requested version.
// Sections are bounded by lines of 60+ '=' characters (CLAUDE.md's
// release-notes format rule). Returns empty if not found.
QString extractSectionFor(const QString &notes, const QString &versionString) {
    // Strip the leading 'v' from "v2.2.7" if present so we match what
    // release-notes.txt actually writes ("Soundboard v2.2.7").
    QString needle = QStringLiteral("Soundboard ") + versionString;
    int header = notes.indexOf(needle);
    if (header < 0) {
        QString withoutV = versionString;
        if (withoutV.startsWith('v') || withoutV.startsWith('V'))
            withoutV = withoutV.mid(1);
        needle = QStringLiteral("Soundboard v") + withoutV;
        header = notes.indexOf(needle);
        if (header < 0) return QString();
    }

    int sectionStart = notes.lastIndexOf('\n', header);
    if (sectionStart < 0) sectionStart = 0; else sectionStart += 1;

    QRegularExpression sep(QStringLiteral("={60,}"));
    QRegularExpressionMatchIterator it = sep.globalMatch(notes, header);
    int sectionEnd = notes.length();
    // The first '=' run after the header line is the underline UNDER
    // the version title. The SECOND run is the boundary to the next
    // version section. Find the second match.
    bool first = true;
    while (it.hasNext()) {
        auto m = it.next();
        if (first) { first = false; continue; }
        sectionEnd = m.capturedStart();
        // Pull back to the start of that line so we don't include the
        // separator itself.
        int lineStart = notes.lastIndexOf('\n', sectionEnd);
        if (lineStart >= 0) sectionEnd = lineStart;
        break;
    }
    return notes.mid(sectionStart, sectionEnd - sectionStart).trimmed();
}
} // namespace

WhatsNewDialog::WhatsNewDialog(const QString &versionString,
                               const QString &notesBody,
                               QWidget *parent)
    : QDialog(parent)
    , m_body(new QTextBrowser(this))
    , m_close(new QPushButton(tr("Close"), this))
{
    setProperty("isGBSoundboard", true);
    setWindowTitle(tr("What's new in GameBaiters Soundboard %1").arg(versionString));
    resize(680, 520);

    auto *root = new QVBoxLayout(this);

    auto *header = new QLabel(tr("Welcome to <b>GameBaiters Soundboard %1</b>!")
                              .arg(versionString), this);
    header->setTextFormat(Qt::RichText);
    header->setStyleSheet("font-size: 14px;");
    root->addWidget(header);

    m_body->setReadOnly(true);
    m_body->setOpenExternalLinks(true);
    m_body->setPlainText(notesBody.isEmpty()
        ? tr("(no release notes were bundled for this version)")
        : notesBody);
    root->addWidget(m_body, 1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_close);
    root->addLayout(btnRow);

    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
}

void WhatsNewDialog::showIfUpdated(QWidget *parent)
{
    QSettings s(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
    int lastSeen = s.value(QString::fromLatin1(kLastSeenKey), 0).toInt();
    const int current = TS3SB_VERSION_BUILD;

    // Brand-new install (no key yet) -> seed silently. We do not want to
    // dump a release-notes wall on a first-time user who has never used
    // any previous version. The "what's new" notion only makes sense for
    // upgrades.
    if (lastSeen == 0) {
        s.setValue(QString::fromLatin1(kLastSeenKey), current);
        return;
    }
    if (lastSeen >= current) return;

    QFile f(QStringLiteral(":/release-notes.txt"));
    QString body;
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        body = extractSectionFor(QString::fromUtf8(f.readAll()),
                                  QStringLiteral(TS3SB_VERSION_S));

    auto *dlg = new WhatsNewDialog(QStringLiteral(TS3SB_VERSION_S), body, parent);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // Persist current version BEFORE showing so a hard TS3 crash during
    // dialog display still records "user has seen this version" - we
    // never want the same release-notes popping up twice.
    s.setValue(QString::fromLatin1(kLastSeenKey), current);
    dlg->show();
}
