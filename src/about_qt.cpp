// src/about_qt.cpp
//----------------------------------
// GameBaiters Soundboard
// Copyright (c) 2026 GameBaitersCrew
// Fork rewritten from scratch on top of the original
// RP Soundboard source by Marius Graefe (2015-2019).
//----------------------------------

#include "about_qt.h"
#include "buildinfo.h"
#include "main.h"
#include "modules/theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QPainter>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QDesktopServices>
#include <QUrl>
#include <QRandomGenerator>
#include <QPixmap>

#include <cmath>

extern "C"
{
const char *av_version_info();
}

namespace {
QColor lerpColor(const QColor &a, const QColor &b, double t) {
    t = qBound(0.0, t, 1.0);
    return QColor(
        a.red()   + int((b.red()   - a.red())   * t),
        a.green() + int((b.green() - a.green()) * t),
        a.blue()  + int((b.blue()  - a.blue())  * t),
        a.alpha() + int((b.alpha() - a.alpha()) * t));
}
constexpr double kNovaDuration  = 5.8;
constexpr double kBlackDuration = 2.5;
constexpr double kResetFade     = 1.5;
constexpr int    kNovaThreshold = 22;
}

AboutQt::AboutQt(QWidget *parent) :
    QWidget(parent, Qt::Window | Qt::WindowTitleHint | Qt::WindowCloseButtonHint)
{
    setWindowTitle(tr("About GameBaiters Soundboard"));
    setFixedSize(560, 620);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(Theme::compositeStyleSheet());
    Theme::trackThemedWidget(this);

    const char *ffVer = av_version_info();

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 16, 24, 12);
    root->setSpacing(6);

    // Logo + title row
    auto *headerRow = new QHBoxLayout;
    headerRow->setSpacing(10);
    headerRow->addStretch(1);

    QPixmap icon(":/icon/img/rpmb_icon_64.png");
    if (!icon.isNull()) {
        auto *iconLabel = new QLabel(this);
        iconLabel->setPixmap(icon.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        iconLabel->setFixedSize(48, 48);
        headerRow->addWidget(iconLabel);
    }

    auto *titleBlock = new QVBoxLayout;
    titleBlock->setSpacing(0);
    auto *title = new QLabel(tr("GameBaiters Soundboard"), this);
    title->setStyleSheet("font-size: 20px; font-weight: bold; color: #e0e0e0;");
    titleBlock->addWidget(title);
    auto *subtitle = new QLabel(tr("by GameBaitersCrew"), this);
    subtitle->setStyleSheet("font-size: 12px; color: #aaa;");
    titleBlock->addWidget(subtitle);
    headerRow->addLayout(titleBlock);
    headerRow->addStretch(1);
    root->addLayout(headerRow);

    root->addSpacing(4);

    QString verText = QString("%1  |  Build %2 %3\nFFmpeg: %4 | Qt %5")
        .arg(buildinfo_getPluginVersion())
        .arg(buildinfo_getBuildDate())
        .arg(buildinfo_getBuildTime())
        .arg(ffVer ? ffVer : "unknown")
        .arg(QT_VERSION_STR);
    auto *version = new QLabel(verText, this);
    version->setStyleSheet("font-size: 11px; color: #999; background: rgba(0,0,0,60); "
                           "border-radius: 4px; padding: 6px;");
    version->setAlignment(Qt::AlignCenter);
    root->addWidget(version);

    root->addSpacing(2);

    auto *credits = new QLabel(this);
    credits->setTextFormat(Qt::RichText);
    credits->setOpenExternalLinks(true);
    credits->setWordWrap(true);
    credits->setAlignment(Qt::AlignCenter);
    credits->setStyleSheet("font-size: 11px; color: #ccc;");
    credits->setText(
        tr("Copyright &copy; 2026 <b>GameBaitersCrew</b><br>"
           "<a style='color:#6cb4ee;' href='https://github.com/gamebaiters/RP-Soundboard'>"
           "github.com/gamebaiters/RP-Soundboard</a>"
           " &nbsp;|&nbsp; "
           "<a style='color:#6cb4ee;' href='https://www.gamebaiters.net'>"
           "www.gamebaiters.net</a><br><br>"
           "Fork rewritten from scratch on top of the original<br>"
           "RP Soundboard source by Marius Gr&auml;fe (2015&ndash;2019).<br><br>"
           "<span style='font-size:10px; color:#888;'>"
           "Uses FFmpeg (LGPLv2.1) &mdash; source: "
           "<a style='color:#6cb4ee;' href='https://ffmpeg.org/download.html'>"
           "ffmpeg.org</a> tag n6.1.1</span>"));
    root->addWidget(credits);

    root->addSpacing(8);

    // Quick-start basics - the essentials anyone needs, no audio sandbox.
    auto *basics = new QLabel(this);
    basics->setObjectName("aboutBasics");
    basics->setTextFormat(Qt::RichText);
    basics->setWordWrap(true);
    basics->setStyleSheet(
        "#aboutBasics { background: rgba(0,0,0,70); border-radius: 6px;"
        " padding: 10px; color: #cfcfcf; font-size: 11px; }");
    basics->setText(tr(
        "<b style='color:#e8e8e8;'>Quick start</b><br>"
        "&bull;&nbsp; Connect to a TeamSpeak server — the soundboard "
        "sends audio into your voice channel.<br>"
        "&bull;&nbsp; Drag an audio file onto a grid button to assign "
        "it, then click the button to play.<br>"
        "&bull;&nbsp; Each channel has a <b>Local</b> volume (what you "
        "hear) and a <b>Remote</b> volume (what others hear).<br>"
        "&bull;&nbsp; <b>Stop all</b> / <b>Pause all</b> control every "
        "channel at once.<br>"
        "&bull;&nbsp; For the complete guide click <b>How to use the "
        "soundboard</b> below."));
    root->addWidget(basics);

    // Full user guide moved out of the TS3 plugin menu into the About
    // dialog so the menu stays short and the guide lives next to the
    // quick-start it complements.
    auto *howToRow = new QHBoxLayout;
    howToRow->addStretch(1);
    auto *howToBtn = new QPushButton(tr("How to use the soundboard"), this);
    howToBtn->setCursor(Qt::PointingHandCursor);
    howToBtn->setStyleSheet(
        "QPushButton { background-color: #2d5fb6; color: white;"
        " border: 1px solid #1d3f80; border-radius: 5px;"
        " padding: 6px 14px; font-weight: bold; }"
        "QPushButton:hover { background-color: #3d77d6; }");
    howToRow->addWidget(howToBtn);
    howToRow->addStretch(1);
    root->addLayout(howToRow);
    connect(howToBtn, &QPushButton::clicked, this, []{ sb_openHowTo(); });

    root->addSpacing(4);

    auto *footer = new QLabel(
        tr("<span style='font-size:9px; color:#666;'>"
           "Un vero e proprio mixer da DJ a portata di mano.</span>"), this);
    footer->setTextFormat(Qt::RichText);
    footer->setAlignment(Qt::AlignCenter);
    root->addWidget(footer);

    connect(&m_novaTimer, &QTimer::timeout, this, [this]{
        double t = m_novaClock.elapsed() / 1000.0;
        if (m_novaActive && t > kNovaDuration) {
            m_novaActive = false;
            m_novaResetting = true;
            m_resetClock.start();
        }
        if (m_novaResetting) {
            double rt = m_resetClock.elapsed() / 1000.0;
            if (rt > kBlackDuration + kResetFade) {
                m_novaResetting = false;
                m_novaTimer.stop();
                m_bgClicks = 0;
            }
        }
        if (!m_novaActive && !m_novaResetting)
            m_novaTimer.stop();
        update();
    });
}

AboutQt::~AboutQt()
{
}

void AboutQt::startSupernova()
{
    m_novaActive = true;
    m_novaResetting = false;
    m_novaClock.start();
    m_novaTimer.start(30);
}

void AboutQt::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const double t = m_novaActive ? m_novaClock.elapsed() / 1000.0 : -1.0;
    const QColor kDark(0x1e, 0x1e, 0x1e);
    const QColor kBlack(0x04, 0x04, 0x08);
    const double clickProgress = qBound(0.0, m_bgClicks / double(kNovaThreshold), 1.0);

    // Reset-phase opacity: 0 during black, fades to 1 during smooth reset
    double resetAlpha = 1.0;
    if (m_novaResetting) {
        double rt = m_resetClock.elapsed() / 1000.0;
        if (rt < kBlackDuration)
            resetAlpha = 0.0;
        else
            resetAlpha = qBound(0.0, (rt - kBlackDuration) / kResetFade, 1.0);
    }

    // --- Background ---
    QColor bg = kDark;
    if (m_novaResetting) {
        double rt = m_resetClock.elapsed() / 1000.0;
        if (rt < kBlackDuration)
            bg = kBlack;
        else
            bg = lerpColor(kBlack, kDark, (rt - kBlackDuration) / kResetFade);
    } else if (t >= 3.0 && t < 3.6) {
        bg = lerpColor(kDark, QColor(200, 220, 255), (t - 3.0) / 0.6);
    } else if (t >= 3.6 && t < 4.2) {
        bg = lerpColor(QColor(200, 220, 255), kBlack, (t - 3.6) / 0.6);
    } else if (t >= 4.2 && t < kNovaDuration) {
        bg = kBlack;
    }
    p.fillRect(rect(), bg);

    // --- Starfield ---
    double starMul = 1.0;
    double starGrow = 1.0;

    if (m_novaResetting) {
        starMul = resetAlpha;
    } else if (t >= 0.0 && t < 2.5) {
        double s = t / 2.5;
        starMul  = 1.0 + s * 5.0;
        starGrow = 1.0 + s * 2.5;
    } else if (t >= 2.5 && t < 3.6) {
        starMul  = 6.0 * (1.0 - (t - 2.5) / 1.1);
        starGrow = 3.5 * (1.0 - (t - 2.5) / 1.1);
    } else if (t >= 3.6) {
        starMul = 0.0;
    }

    if (starMul > 0.01) {
        p.setPen(Qt::NoPen);
        QRandomGenerator rng(42);
        for (int i = 0; i < 80; ++i) {
            int x   = rng.bounded(width());
            int y   = rng.bounded(height());
            int ba  = 20 + rng.bounded(40);
            float bs = 1.0f + rng.bounded(2);
            int a   = qMin(255, int(ba * starMul));
            float sz = bs * float(starGrow);
            p.setBrush(QColor(200, 220, 255, a));
            p.drawEllipse(QPointF(x, y), qreal(sz), qreal(sz));
        }
    }

    // --- Progressive sun: yellow -> orange -> red as clicks grow ---
    if (m_bgClicks > 0 && !m_novaActive && !m_novaResetting) {
        QPointF center(width() / 2.0, height() / 2.0);
        double cp = clickProgress;

        QColor sunInner, sunMid, sunOuter;
        if (cp < 0.33) {
            double s = cp / 0.33;
            sunInner = lerpColor(QColor(255, 255, 200), QColor(255, 240, 140), s);
            sunMid   = lerpColor(QColor(255, 230, 100), QColor(255, 200, 60),  s);
            sunOuter = lerpColor(QColor(255, 200, 50),  QColor(255, 160, 30),  s);
        } else if (cp < 0.66) {
            double s = (cp - 0.33) / 0.33;
            sunInner = lerpColor(QColor(255, 240, 140), QColor(255, 200, 80),  s);
            sunMid   = lerpColor(QColor(255, 200, 60),  QColor(255, 140, 30),  s);
            sunOuter = lerpColor(QColor(255, 160, 30),  QColor(230, 80, 10),   s);
        } else {
            double s = (cp - 0.66) / 0.34;
            sunInner = lerpColor(QColor(255, 200, 80),  QColor(255, 120, 60),  s);
            sunMid   = lerpColor(QColor(255, 140, 30),  QColor(220, 50, 20),   s);
            sunOuter = lerpColor(QColor(230, 80, 10),   QColor(180, 20, 5),    s);
        }

        double glowR = 20.0 + cp * 120.0;
        int glowA = 20 + int(cp * 160);
        QRadialGradient corona(center, glowR);
        corona.setColorAt(0.0, QColor(sunInner.red(), sunInner.green(), sunInner.blue(), glowA));
        corona.setColorAt(0.4, QColor(sunMid.red(), sunMid.green(), sunMid.blue(), glowA / 2));
        corona.setColorAt(0.8, QColor(sunOuter.red(), sunOuter.green(), sunOuter.blue(), glowA / 5));
        corona.setColorAt(1.0, QColor(sunOuter.red() / 2, sunOuter.green() / 2, sunOuter.blue() / 2, 0));
        p.setBrush(corona);
        p.setPen(Qt::NoPen);
        p.drawEllipse(center, glowR, glowR);

        double coreR = 2.0 + cp * 20.0;
        int coreA = 60 + int(cp * 195);
        QRadialGradient core(center, coreR);
        core.setColorAt(0.0, QColor(255, 255, 230, coreA));
        core.setColorAt(0.6, QColor(sunInner.red(), sunInner.green(), sunInner.blue(), coreA));
        core.setColorAt(1.0, QColor(sunMid.red(), sunMid.green(), sunMid.blue(), 0));
        p.setBrush(core);
        p.drawEllipse(center, coreR, coreR);
    }

    // --- Blue supernova radial glow ---
    if (t >= 0.0 && t < 3.6) {
        QPointF center(width() / 2.0, height() / 2.0);
        double s = t / 3.6;
        double maxR = std::sqrt(double(width() * width() + height() * height())) * 0.5;
        double radius = 22.0 + s * s * maxR * 1.3;
        int alpha = qMin(220, int(s * 400));

        QRadialGradient grad(center, radius);
        grad.setColorAt(0.0, QColor(180, 220, 255, alpha));
        grad.setColorAt(0.25, QColor(80, 160, 255, alpha * 3 / 4));
        grad.setColorAt(0.5, QColor(30, 80, 220, alpha / 2));
        grad.setColorAt(0.75, QColor(10, 30, 140, alpha / 4));
        grad.setColorAt(1.0, QColor(5, 10, 60, 0));
        p.setBrush(grad);
        p.setPen(Qt::NoPen);
        p.drawEllipse(center, radius, radius);
    }

    // --- Supernova shockwave ring: a thin bright ring racing outward ---
    if (t >= 0.05 && t < 2.3) {
        QPointF center(width() / 2.0, height() / 2.0);
        double s = (t - 0.05) / 2.25;
        double maxR = std::sqrt(double(width() * width() + height() * height())) * 0.5;
        double ringR = s * maxR * 1.15;
        int ringA = int(210 * (1.0 - s) * (1.0 - s));
        if (ringA > 4) {
            double thick = 2.5 + s * 10.0;
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(225, 240, 255, ringA), thick));
            p.drawEllipse(center, ringR, ringR);
            p.setPen(QPen(QColor(120, 190, 255, ringA / 2), thick * 2.0));
            p.drawEllipse(center, ringR, ringR);
        }
    }

    // (No animation caption text - it sat behind the dialog's labels
    // and was unreadable, so it was removed.)

    // --- Fade children back in during reset ---
    if (m_novaResetting && resetAlpha < 1.0) {
        int overlayA = int(255 * (1.0 - resetAlpha));
        QColor overlay(bg.red(), bg.green(), bg.blue(), overlayA);
        for (auto *child : findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isVisible())
                p.fillRect(child->geometry(), overlay);
        }
    }
}

void AboutQt::mousePressEvent(QMouseEvent *evt)
{
    // A click ANYWHERE in the window feeds the easter egg. Plain labels
    // ignore mouse presses so their clicks propagate here; only links
    // in the credits label consume their own clicks.
    if (m_novaActive) {
        // Already animating — ignore
    } else if (m_novaResetting) {
        m_bgClicks = 1;
        m_novaResetting = false;
        update();
    } else {
        ++m_bgClicks;
        if (m_bgClicks >= kNovaThreshold)
            startSupernova();
        else
            update();
    }

    QWidget::mousePressEvent(evt);
}
