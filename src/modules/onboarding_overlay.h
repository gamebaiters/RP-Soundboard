#pragma once

#include <QWidget>

class QEvent;

// Full-window translucent welcome overlay shown once, on the very first
// launch. Self-contained: it stores a "shown" flag in QSettings and
// resizes with its parent through an event filter, so the host only has
// to construct it and call showIfFirstRun().
class OnboardingOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit OnboardingOverlay(QWidget *parent);

    // True once the overlay has been dismissed at least once.
    static bool alreadyShown();

    // Shows the overlay only if it has never been dismissed; otherwise
    // deletes itself immediately.
    void showIfFirstRun();

protected:
    void paintEvent(QPaintEvent *e) override;
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    void dismiss();
};
