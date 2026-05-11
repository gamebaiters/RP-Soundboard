#include "export_progress_dialog.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QFrame>
#include <QFileInfo>
#include <QFont>
#include <QCloseEvent>
#include <QKeyEvent>

ExportProgressDialog::ExportProgressDialog(const QString &outputFile, QWidget *parent)
    : QDialog(parent)
    , m_outputFile(outputFile)
{
    setWindowTitle(tr("Exporting audio"));
    setProperty("isGBSoundboard", true);
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    // Frameless + drop shadow vibe so the dialog reads as a floating
    // card rather than a stock OS window.
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setFixedSize(440, 220);

    m_card = new QFrame(this);
    m_card->setObjectName("exportCard");

    // Header row: glyph + title.
    m_icon = new QLabel(QString::fromUtf8("\xE2\x9F\xB3"), m_card); // ⟳ (rotating glyph)
    QFont iconFont = m_icon->font();
    iconFont.setPointSize(28);
    iconFont.setBold(true);
    m_icon->setFont(iconFont);
    m_icon->setFixedSize(48, 48);
    m_icon->setAlignment(Qt::AlignCenter);

    m_title = new QLabel(tr("Exporting audio"), m_card);
    QFont titleFont = m_title->font();
    titleFont.setPointSize(13);
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    auto *headerRow = new QHBoxLayout;
    headerRow->setContentsMargins(0, 0, 0, 0);
    headerRow->setSpacing(12);
    headerRow->addWidget(m_icon, 0, Qt::AlignVCenter);
    headerRow->addWidget(m_title, 1, Qt::AlignVCenter);

    // Filename row - elided in style sheet so long paths don't break
    // the card layout.
    QFileInfo fi(m_outputFile);
    m_filename = new QLabel(fi.fileName(), m_card);
    m_filename->setToolTip(m_outputFile);
    QFont fnFont = m_filename->font();
    fnFont.setPointSize(9);
    m_filename->setFont(fnFont);
    m_filename->setStyleSheet("QLabel { color: rgba(255,255,255,0.65); }");

    // Progress bar - flat, themed via Theme::derive accent. We force
    // setFormat("") + setTextVisible(false) because some Windows
    // styles still draw the percent label on top of a styled chunk
    // and that leaks through as a phantom grey rectangle in the
    // middle of the bar.
    m_bar = new QProgressBar(m_card);
    m_bar->setRange(0, 100);
    m_bar->setValue(0);
    m_bar->setTextVisible(false);
    m_bar->setFormat(QString());
    m_bar->setFixedHeight(12);

    m_status = new QLabel(tr("Starting…"), m_card);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont statusFont = m_status->font();
    statusFont.setPointSize(9);
    m_status->setFont(statusFont);

    // Action row.
    m_cancel = new QPushButton(tr("Cancel"), m_card);
    m_cancel->setFixedHeight(30);
    m_cancel->setMinimumWidth(96);
    m_cancel->setCursor(Qt::PointingHandCursor);
    connect(m_cancel, &QPushButton::clicked, this, [this]{
        if (!m_finished) emit cancelRequested();
        m_cancel->setEnabled(false);
        m_status->setText(tr("Cancelling…"));
    });

    m_dismiss = new QPushButton(tr("Close"), m_card);
    m_dismiss->setFixedHeight(30);
    m_dismiss->setMinimumWidth(96);
    m_dismiss->setCursor(Qt::PointingHandCursor);
    m_dismiss->setVisible(false);
    connect(m_dismiss, &QPushButton::clicked, this, &QDialog::accept);

    auto *btnRow = new QHBoxLayout;
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(8);
    btnRow->addStretch(1);
    btnRow->addWidget(m_cancel);
    btnRow->addWidget(m_dismiss);

    auto *cardLay = new QVBoxLayout(m_card);
    cardLay->setContentsMargins(20, 18, 20, 16);
    cardLay->setSpacing(12);
    cardLay->addLayout(headerRow);
    cardLay->addWidget(m_filename);
    cardLay->addWidget(m_bar);
    cardLay->addWidget(m_status);
    cardLay->addStretch(1);
    cardLay->addLayout(btnRow);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->addWidget(m_card);

    Theme::trackThemedWidget(this);
    applyTheme();
}

void ExportProgressDialog::applyTheme() {
    Theme::Derived d = Theme::derive(Theme::colors());
    const QString accent = d.accent.name();
    const QString accentHover = d.accent.lighter(115).name();
    const QString surface = d.surface.name();
    const QString surfaceAlt = d.surfaceAlt.name();
    const QString text = d.text.name();
    const QString border = d.borderStrong.name();

    m_card->setStyleSheet(QString(
        "#exportCard { background-color: %1; border: 1px solid %2;"
        " border-radius: 10px; }").arg(surface, border));

    m_title->setStyleSheet(QString("QLabel { color: %1; }").arg(text));
    m_status->setStyleSheet(QString("QLabel { color: %1; }").arg(text));
    m_filename->setStyleSheet(QString(
        "QLabel { color: %1; font-style: italic; }")
        .arg(d.borderStrong.name()));
    m_icon->setStyleSheet(QString("QLabel { color: %1; }").arg(accent));

    m_bar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: 1px solid %2;"
        " border-radius: 6px; text-align: center; color: transparent; }"
        "QProgressBar::chunk { background-color: %3; border-radius: 5px;"
        " margin: 0px; }")
        .arg(surfaceAlt, border, accent));

    const QString btnQss = QString(
        "QPushButton { background-color: %1; color: %2;"
        " border: 1px solid %3; border-radius: 6px; padding: 4px 14px; }"
        "QPushButton:hover { background-color: %4; }"
        "QPushButton:pressed { background-color: %3; }"
        "QPushButton:disabled { color: rgba(255,255,255,0.4); }")
        .arg(surfaceAlt, text, accent, accentHover);
    m_cancel->setStyleSheet(btnQss);
    m_dismiss->setStyleSheet(btnQss);
}

void ExportProgressDialog::setProgress(int percent) {
    if (m_finished) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    m_bar->setValue(percent);
    m_status->setText(tr("Encoding… %1%").arg(percent));
}

void ExportProgressDialog::setFinished(bool ok, const QString &error) {
    m_finished = true;
    m_cancel->setVisible(false);
    m_dismiss->setVisible(true);
    m_dismiss->setDefault(true);
    m_dismiss->setFocus();

    Theme::Derived d = Theme::derive(Theme::colors());
    const QString okColor   = "#4caf50";   // green
    const QString failColor = "#e04141";   // red
    const QString accent    = ok ? okColor : failColor;

    if (ok) {
        m_icon->setText(QString::fromUtf8("\xE2\x9C\x93"));   // ✓
        m_title->setText(tr("Export completed"));
        m_status->setText(tr("Saved to %1").arg(m_outputFile));
        m_dismiss->setText(tr("OK"));
    } else {
        m_icon->setText(QString::fromUtf8("\xE2\x9C\x95"));   // ✕
        m_title->setText(tr("Export failed"));
        m_status->setText(error.isEmpty() ? tr("Unknown error") : error);
        m_dismiss->setText(tr("Close"));
    }

    m_icon->setStyleSheet(QString("QLabel { color: %1; }").arg(accent));
    // Apply new stylesheet BEFORE the value flip so the chunk paints
    // with the final colour in one repaint and the bar ends fully
    // filled (the previous code reset value=100 first, then the QSS
    // change kicked a fresh layout pass that briefly redrew the
    // chunk at the now-stale intermediate width).
    m_bar->setStyleSheet(QString(
        "QProgressBar { background-color: %1; border: 1px solid %2;"
        " border-radius: 6px; text-align: center; color: transparent; }"
        "QProgressBar::chunk { background-color: %3; border-radius: 5px;"
        " margin: 0px; }")
        .arg(d.surfaceAlt.name(), d.borderStrong.name(), accent));
    m_bar->setRange(0, 100);
    m_bar->setValue(100);
    m_bar->update();
}
