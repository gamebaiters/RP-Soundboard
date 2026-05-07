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
    // No confirmation. Reset is cheap, reversible by re-tweaking sliders.
    emit resetRequested();
}
