#include "help_bubble.h"
#include "../SpeechBubble.h"

#include <QBoxLayout>
#include <QToolTip>
#include <QCursor>
#include <QVariant>

HelpBubble::HelpBubble(const QString &text, QWidget *parent)
    : QToolButton(parent)
    , m_text(text)
    , m_bubble(nullptr)
{
    setText("?");
    setAutoRaise(false);
    setToolTip(text);
    setFocusPolicy(Qt::NoFocus);
    setProperty("helpRole", QVariant(QString("bubble")));
    setMinimumSize(18, 18);
    setMaximumSize(20, 20);
    connect(this, &QToolButton::clicked, this, &HelpBubble::onClicked);
}

void HelpBubble::setHelpText(const QString &text) {
    m_text = text;
    setToolTip(text);
    if (m_bubble) m_bubble->setText(text);
}

HelpBubble *HelpBubble::attachAfter(QWidget *target, const QString &helpText) {
    if (!target || !target->parentWidget()) return new HelpBubble(helpText);
    auto *parentLayout = target->parentWidget()->layout();
    auto *btn = new HelpBubble(helpText, target->parentWidget());
    if (auto *box = qobject_cast<QBoxLayout *>(parentLayout)) {
        int idx = box->indexOf(target);
        if (idx >= 0) box->insertWidget(idx + 1, btn);
        else          box->addWidget(btn);
    }
    return btn;
}

void HelpBubble::onClicked() {
    // QToolTip::showText is reliable across Qt platform plugins, unlike the
    // SpeechBubble dialog which has window-flag quirks that left the popup
    // invisible in some hosts. Show under the button for 30 s; user can
    // dismiss by clicking elsewhere.
    QPoint anchor = mapToGlobal(QPoint(0, height()));
    QToolTip::showText(anchor, m_text, this, QRect(), 30000);
}
