#include "log_viewer_dialog.h"
#include "../ts3log.h"
#include "../common.h"
#include "../plugin.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QTextDocument>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QLabel>
#include <QTimer>
#include <QClipboard>
#include <QApplication>
#include <QScrollBar>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QShowEvent>
#include <QHideEvent>

extern "C" const char *getTs3ConfigPath();

namespace {
// TS3 LogLevel ordering: CRITICAL=0, ERROR=1, WARNING=2, DEBUG=3,
// INFO=4, DEVEL=5. The viewer filter is "show everything AT or
// MORE-SEVERE than the chosen level", so lower numeric = more severe.
// "All" sets the threshold above INFO so every entry passes.
const char *levelLabel(int lv) {
    switch (lv) {
        case LogLevel_CRITICAL: return "CRIT";
        case LogLevel_ERROR:    return "ERR ";
        case LogLevel_WARNING:  return "WARN";
        case LogLevel_DEBUG:    return "DBG ";
        case LogLevel_INFO:     return "INFO";
        case LogLevel_DEVEL:    return "DEV ";
        default:                return "????";
    }
}

QString levelColor(int lv) {
    switch (lv) {
        case LogLevel_CRITICAL: return QStringLiteral("#ff3030");
        case LogLevel_ERROR:    return QStringLiteral("#ff5555");
        case LogLevel_WARNING:  return QStringLiteral("#ffb74d");
        case LogLevel_INFO:     return QStringLiteral("#9ccc65");
        case LogLevel_DEBUG:    return QStringLiteral("#90caf9");
        default:                return QStringLiteral("#cccccc");
    }
}
} // namespace

LogViewerDialog::LogViewerDialog(QWidget *parent)
    : QDialog(parent)
    , m_text(new QTextEdit(this))
    , m_level(new QComboBox(this))
    , m_autoScroll(new QCheckBox(tr("Auto-scroll"), this))
    , m_clear(new QPushButton(tr("Clear"), this))
    , m_copy(new QPushButton(tr("Copy all"), this))
    , m_close(new QPushButton(tr("Close"), this))
    , m_timer(new QTimer(this))
    , m_minLevel(LogLevel_INFO)
{
    setProperty("isGBSoundboard", true);
    setWindowTitle(tr("GameBaiters Soundboard - Plugin log"));
    resize(820, 500);

    m_text->setReadOnly(true);
    m_text->setLineWrapMode(QTextEdit::NoWrap);
    m_text->document()->setMaximumBlockCount(2000);
    m_text->setFont(QFont(QStringLiteral("Consolas"), 9));

    m_level->addItem(tr("All"),      -1);
    m_level->addItem(tr("Debug+"),   LogLevel_DEBUG);
    m_level->addItem(tr("Info+"),    LogLevel_INFO);
    m_level->addItem(tr("Warning+"), LogLevel_WARNING);
    m_level->addItem(tr("Error+"),   LogLevel_ERROR);
    m_level->setCurrentIndex(0);

    m_autoScroll->setChecked(true);

    auto *root = new QVBoxLayout(this);

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Level:"), this));
    topRow->addWidget(m_level);
    topRow->addSpacing(12);
    topRow->addWidget(m_autoScroll);
    topRow->addStretch(1);
    topRow->addWidget(m_copy);
    topRow->addWidget(m_clear);
    root->addLayout(topRow);

    root->addWidget(m_text, 1);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    btnRow->addWidget(m_close);
    root->addLayout(btnRow);

    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_clear, &QPushButton::clicked, this, &LogViewerDialog::onClearClicked);
    connect(m_copy,  &QPushButton::clicked, this, &LogViewerDialog::onCopyClicked);
    connect(m_level, qOverload<int>(&QComboBox::currentIndexChanged),
            this,    &LogViewerDialog::onFilterChanged);
    connect(m_timer, &QTimer::timeout, this, &LogViewerDialog::refresh);

    // Timer started on showEvent and stopped on hideEvent so a closed
    // viewer never costs CPU - the earlier 500 ms always-on tick was
    // why "Write debug log file" lagged the soundboard. Refresh tick
    // also slowed 500 ms -> 1000 ms; 1 Hz is plenty for log inspection.
    refresh();
}

void LogViewerDialog::showEvent(QShowEvent *e)
{
    QDialog::showEvent(e);
    // Force a fresh build the first tick after a re-show by clearing
    // the dedup signature.
    m_lastRingSize = -1;
    m_lastFileSize = -1;
    refresh();
    m_timer->start(1000);
}

void LogViewerDialog::hideEvent(QHideEvent *e)
{
    m_timer->stop();
    QDialog::hideEvent(e);
}

QString LogViewerDialog::fileLogPath() const
{
    if (!m_cachedFileLogPath.isEmpty()) return m_cachedFileLogPath;
    const char *cfg = getTs3ConfigPath();
    if (!cfg || !cfg[0]) return QString();
    m_cachedFileLogPath = QString::fromUtf8(cfg) + "rpsb_debug.log";
    return m_cachedFileLogPath;
}

void LogViewerDialog::refresh()
{
    // Fast path: nothing changed since the last refresh, skip the
    // expensive HTML rebuild + QTextEdit reparse entirely. We compare
    // ring entry count, on-disk file size and the active min-level
    // filter. The ring rotates so its SIZE is a stable signal (entries
    // are added monotonically up to the cap, then trimmed from the
    // front - the COUNT only changes while filling) but combined with
    // file size + filter we get a cheap "did anything user-visible
    // change" gate.
    QVector<LogRingEntry> snap = logRingSnapshot();

    qint64 fileSize = -1;
    QString fileLogPath_ = fileLogPath();
    bool readFile = g_rpsbLogsEnabled && !fileLogPath_.isEmpty();
    if (readFile) {
        QFileInfo fi(fileLogPath_);
        if (fi.exists()) fileSize = fi.size();
    }

    if (snap.size() == m_lastRingSize
     && fileSize     == m_lastFileSize
     && m_minLevel   == m_lastMinLevel) {
        return;
    }
    m_lastRingSize = snap.size();
    m_lastFileSize = fileSize;
    m_lastMinLevel = m_minLevel;

    QString html;
    html.reserve(snap.size() * 80 + 4096);

    auto appendLine = [&html, this](int level, const QString &text){
        if (m_minLevel >= 0 && level > m_minLevel) return;
        html += QString("<span style=\"color:%1;white-space:pre;\">%2 %3</span><br/>")
                    .arg(levelColor(level),
                         QString::fromLatin1(levelLabel(level)),
                         text.toHtmlEscaped());
    };

    for (const auto &e : snap) appendLine(e.level, e.text);

    if (readFile && fileSize > 0) {
        QFile f(fileLogPath_);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            // Cap tail at 200 KB so a multi-megabyte log file does not
            // melt the GUI thread on every refresh. The QTextDocument's
            // max block count caps display further.
            constexpr qint64 kMaxTailBytes = 200 * 1024;
            qint64 readFrom = (fileSize > kMaxTailBytes)
                ? fileSize - kMaxTailBytes
                : 0;
            if (readFrom > 0) f.seek(readFrom);
            QByteArray all = f.readAll();
            f.close();
            if (readFrom > 0) {
                int nl = all.indexOf('\n');
                if (nl >= 0 && nl < all.size() - 1)
                    all = all.mid(nl + 1);
            }
            m_fileTailed = true;
            m_fileSize   = fileSize;
            QStringList lines = QString::fromUtf8(all).split('\n');
            for (const QString &ln : lines) {
                if (ln.isEmpty()) continue;
                appendLine(LogLevel_DEBUG, ln);
            }
        }
    }

    bool wasAtBottom = m_autoScroll->isChecked();
    int savedPos = m_text->verticalScrollBar()->value();
    int savedMax = m_text->verticalScrollBar()->maximum();
    bool atBottom = wasAtBottom || (savedPos == savedMax);

    m_text->setHtml(html);

    if (atBottom) {
        auto *bar = m_text->verticalScrollBar();
        bar->setValue(bar->maximum());
    } else {
        m_text->verticalScrollBar()->setValue(savedPos);
    }
}

void LogViewerDialog::onClearClicked()
{
    logRingClear();
    m_text->clear();
}

void LogViewerDialog::onCopyClicked()
{
    QApplication::clipboard()->setText(m_text->toPlainText());
}

void LogViewerDialog::onFilterChanged(int idx)
{
    m_minLevel = m_level->itemData(idx).toInt();
    refresh();
}
