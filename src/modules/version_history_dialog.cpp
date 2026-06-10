#include "version_history_dialog.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QListWidget>
#include <QTextBrowser>
#include <QPushButton>
#include <QLabel>
#include <QFile>
#include <QRegularExpression>

VersionHistoryDialog::VersionHistoryDialog(QWidget *parent)
    : QDialog(parent)
    , m_list(new QListWidget(this))
    , m_body(new QTextBrowser(this))
    , m_close(new QPushButton(tr("Close"), this))
{
    setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    setWindowTitle(tr("Version history"));
    setProperty("isGBSoundboard", true);
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);
    resize(820, 560);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 10, 12, 10);
    root->setSpacing(8);

    auto *header = new QLabel(
        tr("Browse the full change log of every documented release."), this);
    header->setStyleSheet("color:#bbb; font-size:11px;");
    root->addWidget(header);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_list);
    splitter->addWidget(m_body);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 180, 600 });
    root->addWidget(splitter, 1);

    m_list->setMinimumWidth(140);
    m_body->setReadOnly(true);
    m_body->setOpenExternalLinks(true);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_close);
    root->addLayout(btnRow);

    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_list, &QListWidget::currentItemChanged,
            this, &VersionHistoryDialog::onVersionSelected);

    loadHistory();
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void VersionHistoryDialog::loadHistory()
{
    QFile f(QStringLiteral(":/release-notes.txt"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_body->setPlainText(tr("Release notes file is missing from the build."));
        return;
    }
    const QString notes = QString::fromUtf8(f.readAll());

    // Split on every "GameBaiters Soundboard vX.Y.Z" header so the
    // order in release-notes.txt is preserved (newest at the top).
    QRegularExpression hdr(
        QStringLiteral("^GameBaiters Soundboard v(\\d+\\.\\d+\\.\\d+)\\s*$"),
        QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator it = hdr.globalMatch(notes);

    QVector<int> headerStarts;
    QVector<QString> headerVersions;
    while (it.hasNext()) {
        auto m = it.next();
        headerStarts.append(m.capturedStart());
        headerVersions.append(m.captured(1));
    }
    for (int i = 0; i < headerStarts.size(); ++i) {
        int sectionStart = notes.indexOf(QChar('\n'), headerStarts.at(i));
        if (sectionStart < 0) continue;
        sectionStart += 1;
        int underlineEnd = notes.indexOf(QChar('\n'), sectionStart);
        if (underlineEnd > sectionStart) {
            QString line = notes.mid(sectionStart, underlineEnd - sectionStart).trimmed();
            if (!line.isEmpty() && line.count(QChar('=')) == line.size())
                sectionStart = underlineEnd + 1;
        }
        int sectionEnd = (i + 1 < headerStarts.size())
            ? headerStarts.at(i + 1) : notes.length();
        VersionEntry e;
        e.version = headerVersions.at(i);
        e.body    = notes.mid(sectionStart, sectionEnd - sectionStart).trimmed();
        m_entries.append(e);
        m_list->addItem(QStringLiteral("v") + e.version);
    }

    if (m_entries.isEmpty())
        m_body->setPlainText(tr("(no version sections parsed)"));
}

void VersionHistoryDialog::onVersionSelected(QListWidgetItem *current,
                                              QListWidgetItem * /*previous*/)
{
    if (!current) return;
    int row = m_list->row(current);
    if (row < 0 || row >= m_entries.size()) return;
    renderBody(m_entries.at(row));
}

void VersionHistoryDialog::renderBody(const VersionEntry &v)
{
    Theme::Derived d = Theme::derive(Theme::colors());
    const QString bg     = d.bg.name();
    const QString text   = d.text.name();
    const QString accent = d.accentLight.isValid() ? d.accentLight.name()
                                                    : d.accent.name();
    m_body->setStyleSheet(QString(
        "QTextBrowser { background-color: %1; color: %2; border: 1px solid %3; }")
        .arg(bg, text, d.border.name()));

    QString safe = v.body.toHtmlEscaped();
    safe.replace(QChar('\n'), QStringLiteral("<br>"));
    QString html = QString(
        "<html><head><style>"
        "body { font-family:'Segoe UI', sans-serif; font-size: 13px;"
        "       background-color: %3; color: %4; }"
        "h2 { color: %5; margin-top: 0; }"
        "</style></head><body>"
        "<h2>%1</h2><p>%2</p></body></html>")
        .arg(tr("GameBaiters Soundboard v%1").arg(v.version),
             safe, bg, text, accent);
    m_body->setHtml(html);
}
