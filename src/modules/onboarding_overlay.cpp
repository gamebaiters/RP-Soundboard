#include "onboarding_overlay.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFrame>
#include <QPainter>
#include <QEvent>
#include <QSettings>

namespace {
const char *kOrg = "GameBaiters";
const char *kApp = "Soundboard";
const char *kKey = "onboardingShown";
}

bool OnboardingOverlay::alreadyShown()
{
    QSettings s(kOrg, kApp);
    return s.value(kKey, false).toBool();
}

OnboardingOverlay::OnboardingOverlay(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    hide();

    auto *root = new QVBoxLayout(this);
    root->addStretch(1);

    // Centered welcome card. Fixed size: the content is one rich-text
    // label, so a fixed card rect renders it with zero layout guessing
    // (nested word-wrap layouts were collapsing/overlapping before).
    auto *card = new QFrame(this);
    card->setObjectName("onboardingCard");
    card->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    card->setStyleSheet(
        "#onboardingCard {"
        "  background-color: #232733;"
        "  border: 1px solid #4a8bc2;"
        "  border-radius: 10px;"
        "}");

    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(30, 26, 30, 22);
    cardLay->setSpacing(14);

    // All copy lives in ONE rich-text label - one widget, one reliable
    // word-wrap pass, no inter-label overlap.
    auto *body = new QLabel(card);
    body->setTextFormat(Qt::RichText);
    body->setWordWrap(true);
    body->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    body->setText(tr(
        "<div style='color:#e6e6e6; font-size:13px;'>"
        "<p style='font-size:18px; font-weight:bold; color:#ffffff;'>"
        "Welcome to the GameBaiters Soundboard</p>"
        "<p>A few things to get you started:</p>"
        "<ul style='margin-left:-18px;'>"
        "<li style='margin-bottom:7px;'>Drag an audio file onto a grid "
        "button to assign a sound, then click the button to play it.</li>"
        "<li style='margin-bottom:7px;'>Use channels to play several "
        "sounds at once &mdash; each channel is fully independent.</li>"
        "<li style='margin-bottom:7px;'>Open a channel's Audio Sandbox "
        "for the 16-band EQ, 3D spatial audio and the DSP effect "
        "pipeline.</li>"
        "<li>Need the full guide? Open the Plugins menu and choose "
        "\"How to use the soundboard\".</li>"
        "</ul></div>"));
    // Auto-size: pin the label width, derive its height from the text.
    // One label = one reliable word-wrap pass; the frame then sizes
    // itself exactly to the content with no empty padding.
    const int kContentW = 430;
    body->setFixedWidth(kContentW);
    body->setFixedHeight(body->heightForWidth(kContentW));
    cardLay->addWidget(body);

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch(1);
    auto *gotIt = new QPushButton(tr("Got it"), card);
    gotIt->setMinimumSize(120, 32);
    gotIt->setStyleSheet(
        "QPushButton { background-color: #3c6e9c; color: white;"
        " border-radius: 5px; padding: 5px 18px; font-weight: bold; }"
        "QPushButton:hover { background-color: #4a8bc2; }");
    connect(gotIt, &QPushButton::clicked, this, &OnboardingOverlay::dismiss);
    btnRow->addWidget(gotIt);
    btnRow->addStretch(1);
    cardLay->addLayout(btnRow);

    // Frame sizes itself exactly around the content (no fixed guess).
    card->adjustSize();

    root->addWidget(card, 0, Qt::AlignHCenter);
    root->addStretch(1);

    if (parent) parent->installEventFilter(this);
}

void OnboardingOverlay::showIfFirstRun()
{
    if (alreadyShown()) {
        deleteLater();
        return;
    }
    if (parentWidget()) setGeometry(parentWidget()->rect());
    show();
    raise();
}

void OnboardingOverlay::dismiss()
{
    QSettings s(kOrg, kApp);
    s.setValue(kKey, true);
    s.sync();
    hide();
    deleteLater();
}

void OnboardingOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(10, 12, 16, 215));
}

bool OnboardingOverlay::eventFilter(QObject *o, QEvent *e)
{
    if (o == parentWidget() && e->type() == QEvent::Resize) {
        if (parentWidget()) setGeometry(parentWidget()->rect());
        raise();
    }
    return QWidget::eventFilter(o, e);
}
