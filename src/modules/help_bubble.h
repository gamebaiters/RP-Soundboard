// HelpBubble - reusable '?' icon that shows a help popup. Used everywhere
// a feature needs an inline explanation. Wraps the existing SpeechBubble
// for the popup itself; provides a tiny QToolButton you can drop into any
// layout next to the control you're documenting.

#pragma once

#include <QToolButton>
#include <QString>

class SpeechBubble;

class HelpBubble : public QToolButton {
    Q_OBJECT
public:
    explicit HelpBubble(const QString &text, QWidget *parent = nullptr);

    QString helpText() const { return m_text; }
    void    setHelpText(const QString &text);

    // Convenience: create a HelpBubble + add it to the same parent layout
    // immediately after `target`. Returns the created button (caller may
    // ignore the pointer).
    static HelpBubble *attachAfter(QWidget *target, const QString &helpText);

private slots:
    void onClicked();

private:
    QString       m_text;
    SpeechBubble *m_bubble;
};
