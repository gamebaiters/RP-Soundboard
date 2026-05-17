#include "reset_channels_btn.h"

#include <QMessageBox>
#include <QVariant>

ResetChannelsBtn::ResetChannelsBtn(QWidget *parent)
    : QPushButton(tr("Reset channels"), parent)
{
    setProperty("buttonVariant", QVariant(QString("special")));
    setToolTip(tr("Reset only channel state (volumes / pitch / speed / reverb).\n"
                  "Global settings and button assignments are preserved."));
    connect(this, &QPushButton::clicked, this, &ResetChannelsBtn::onClicked);
}

void ResetChannelsBtn::onClicked() {
    // Reset wipes volumes, FX and the whole audio sandbox of every
    // channel and is then persisted — confirm first so a stray click
    // can never silently clear the user's mixer.
    auto reply = QMessageBox::question(
        this, tr("Reset channels"),
        tr("Reset all channel state — volumes, pitch, speed, reverb and the "
           "audio sandbox — back to defaults?\n\nThis cannot be undone."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes)
        emit resetRequested();
}
