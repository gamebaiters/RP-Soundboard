#include "export_progress_dialog.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QFileInfo>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QCloseEvent>
#include <QKeyEvent>
#include <QTimer>
#include <QElapsedTimer>

#include <algorithm>

// Custom-painted progress bar. The previous implementation used a
// QProgressBar styled via QSS; on the Windows native style the chunk
// fill repaints were unreliable - sometimes the bar stuck at 0%, the
// chunk-painted rect leaked outside the rounded background, or the
// terminal "100% green / red" never showed. Painting it ourselves
// (rounded background, single rounded fill clipped to width * target)
// removes the QSS round-trip entirely and the bar is always correct.
// Animation: a 60 Hz QTimer easing m_drawValue toward m_targetValue
// gives smooth motion without requiring Q_OBJECT on the bar itself.
namespace {

class ExportBar : public QWidget {
public:
    explicit ExportBar(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(14);
        setMaximumHeight(14);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        m_anim = new QTimer(this);
        m_anim->setInterval(16);    // 60 Hz
        QObject::connect(m_anim, &QTimer::timeout, this, [this]{ tick(); });
    }

    void setTarget(qreal v) {
        v = std::max(0.0, std::min(1.0, v));
        if (qFuzzyCompare(m_target, v)) return;
        m_target = v;
        if (!m_anim->isActive()) m_anim->start();
    }

    // Snap the bar to a value immediately (no animation). Used for the
    // terminal success/failure state so the bar lands at exactly 100%.
    void snapTo(qreal v) {
        v = std::max(0.0, std::min(1.0, v));
        m_target = v;
        m_drawn  = v;
        m_anim->stop();
        update();
    }

    void setFillColor(const QColor &c)   { m_fill = c;   update(); }
    void setTrackColor(const QColor &c)  { m_track = c;  update(); }
    void setBorderColor(const QColor &c) { m_border = c; update(); }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        const qreal r = height() / 2.0;
        const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);

        QPainterPath base;
        base.addRoundedRect(full, r, r);

        p.fillPath(base, m_track);
        p.setPen(QPen(m_border, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(base);

        if (m_drawn > 0.0) {
            p.save();
            p.setClipPath(base);
            const qreal fillW = full.width() * m_drawn;
            QRectF fillRect(full.left(), full.top(), fillW, full.height());
            p.setPen(Qt::NoPen);
            p.setBrush(m_fill);
            p.drawRect(fillRect);
            p.restore();
        }
    }

private:
    void tick() {
        constexpr qreal kSlew = 0.18;  // ~5 frames to 95% (~80 ms)
        const qreal diff = m_target - m_drawn;
        if (std::abs(diff) < 1e-4) {
            m_drawn = m_target;
            m_anim->stop();
        } else {
            m_drawn += diff * kSlew;
        }
        update();
    }

    qreal   m_target = 0.0;
    qreal   m_drawn  = 0.0;
    QTimer *m_anim   = nullptr;
    QColor  m_fill   = QColor(0x3f, 0xb0, 0xe0);
    QColor  m_track  = QColor(40, 40, 50);
    QColor  m_border = QColor(60, 60, 70);
};

} // namespace

ExportProgressDialog::ExportProgressDialog(const QString &outputFile, QWidget *parent)
    : QDialog(parent)
    , m_outputFile(outputFile)
{
    setWindowTitle(tr("Exporting audio"));
    setProperty("isGBSoundboard", true);
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setFixedSize(440, 220);

    m_card = new QFrame(this);
    m_card->setObjectName("exportCard");

    m_icon = new QLabel(QString::fromUtf8("\xE2\x9F\xB3"), m_card);
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

    QFileInfo fi(m_outputFile);
    m_filename = new QLabel(fi.fileName(), m_card);
    m_filename->setToolTip(m_outputFile);
    QFont fnFont = m_filename->font();
    fnFont.setPointSize(9);
    m_filename->setFont(fnFont);
    m_filename->setStyleSheet("QLabel { color: rgba(255,255,255,0.65); }");

    auto *bar = new ExportBar(m_card);
    m_barWidget = bar;

    m_status = new QLabel(tr("Starting…"), m_card);
    m_status->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont statusFont = m_status->font();
    statusFont.setPointSize(9);
    m_status->setFont(statusFont);

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
    cardLay->addWidget(bar);
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

    if (auto *bar = static_cast<ExportBar*>(m_barWidget)) {
        bar->setFillColor(d.accent);
        bar->setTrackColor(d.surfaceAlt);
        bar->setBorderColor(d.borderStrong);
    }

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
    percent = std::max(0, std::min(100, percent));
    if (auto *bar = static_cast<ExportBar*>(m_barWidget))
        bar->setTarget(percent / 100.0);
    m_status->setText(tr("Encoding… %1%").arg(percent));
}

void ExportProgressDialog::setFinished(bool ok, const QString &error) {
    m_finished = true;
    m_cancel->setVisible(false);
    m_dismiss->setVisible(true);
    m_dismiss->setDefault(true);
    m_dismiss->setFocus();

    const QColor okColor(0x4c, 0xaf, 0x50);
    const QColor failColor(0xe0, 0x41, 0x41);
    const QColor accent = ok ? okColor : failColor;

    if (ok) {
        m_icon->setText(QString::fromUtf8("\xE2\x9C\x93"));
        m_title->setText(tr("Export completed"));
        m_status->setText(tr("Saved to %1").arg(m_outputFile));
        m_dismiss->setText(tr("OK"));
    } else {
        m_icon->setText(QString::fromUtf8("\xE2\x9C\x95"));
        m_title->setText(tr("Export failed"));
        m_status->setText(error.isEmpty() ? tr("Unknown error") : error);
        m_dismiss->setText(tr("Close"));
    }

    m_icon->setStyleSheet(QString("QLabel { color: %1; }").arg(accent.name()));
    if (auto *bar = static_cast<ExportBar*>(m_barWidget)) {
        bar->setFillColor(accent);
        bar->snapTo(1.0);
    }
}
