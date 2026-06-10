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
// Sections in release-notes.txt are headed by lines of the form
//   "GameBaiters Soundboard vX.Y.Z"
// followed by a '====' underline. We anchor the section start at the
// header for the requested version and the section end at the NEXT
// such header (or end of file). The previous implementation relied on
// '={60,}' separator lines between sections — that broke as soon as
// a release was cut without an explicit separator, dumping every
// older version's notes into the dialog along with the current one.
QString extractSectionFor(const QString &notes, const QString &versionString) {
    QString withoutV = versionString;
    if (withoutV.startsWith('v') || withoutV.startsWith('V'))
        withoutV = withoutV.mid(1);
    QString headerNeedle = QStringLiteral("GameBaiters Soundboard v") + withoutV;
    int header = notes.indexOf(headerNeedle);
    if (header < 0) return QString();

    // sectionStart = first char after the title line.
    int titleEnd = notes.indexOf(QChar('\n'), header);
    if (titleEnd < 0) return QString();
    int sectionStart = titleEnd + 1;

    // Skip the '===' underline right under the title so it does not
    // show up at the top of the body. An underline is a line that is
    // non-empty and consists ONLY of '=' characters.
    int underlineEnd = notes.indexOf(QChar('\n'), sectionStart);
    if (underlineEnd > sectionStart) {
        QString line = notes.mid(sectionStart, underlineEnd - sectionStart).trimmed();
        if (!line.isEmpty() && line.count(QChar('=')) == line.size())
            sectionStart = underlineEnd + 1;
    }

    // sectionEnd = start of the NEXT version header (any X.Y.Z), or
    // end of file. Robust against missing '={60,}' separators between
    // sections.
    QRegularExpression nextHdr(
        QStringLiteral("^GameBaiters Soundboard v\\d+\\.\\d+\\.\\d+"),
        QRegularExpression::MultilineOption);
    QRegularExpressionMatch m = nextHdr.match(notes, sectionStart);
    int sectionEnd = m.hasMatch() ? m.capturedStart() : notes.length();
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
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
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
