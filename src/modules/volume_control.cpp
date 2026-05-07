#include "volume_control.h"
#include "help_bubble.h"
#include "fine_slider.h"
#include "theme.h"

#include <QGridLayout>
#include <QLabel>
#include <QSlider>
#include <QToolButton>
#include <QVariant>

static QString fmtPct(int v) { return QString::number(v) + "%"; }

VolumeControl::VolumeControl(QWidget *parent)
    : QWidget(parent)
    , m_local(new FineSlider(Qt::Horizontal, this))
    , m_remote(new FineSlider(Qt::Horizontal, this))
    , m_localLabel(new QLabel(this))
    , m_remoteLabel(new QLabel(this))
    , m_link(new QToolButton(this))
    , m_linkDelta(0)
    , m_internalSync(false)
{
    m_local->setRange(0, 100);
    m_remote->setRange(0, 100);
    m_local->setSingleStep(1);
    m_remote->setSingleStep(1);
    m_local->setPageStep(5);
    m_remote->setPageStep(5);
    m_local->setValue(100);
    m_remote->setValue(100);
    m_local->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_remote->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    auto *capLocal  = new QLabel(tr("Local"),  this);
    auto *capRemote = new QLabel(tr("Remote"), this);
    capLocal ->setMinimumWidth(48);
    capRemote->setMinimumWidth(48);
    capLocal ->setMaximumWidth(60);
    capRemote->setMaximumWidth(60);
    m_localLabel->setText(fmtPct(m_local->value()));
    m_remoteLabel->setText(fmtPct(m_remote->value()));
    m_localLabel->setMinimumWidth(36);
    m_remoteLabel->setMinimumWidth(36);

    m_link->setText(QString::fromUtf8("\xF0\x9F\x94\x97")); // 🔗
    m_link->setCheckable(true);
    m_link->setToolTip(tr("Link local + remote volumes"));
    m_link->setObjectName("vc_link_btn");
    m_link->setProperty("syncRole", QVariant(QString("link")));
    refreshTheme();
    connect(m_link, &QToolButton::toggled, this, [this](bool){ refreshTheme(); });

    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0,0,0,0);
    grid->setHorizontalSpacing(2);
    grid->setVerticalSpacing(2);
    grid->setColumnStretch(0, 0);   // caption fixed
    grid->setColumnStretch(1, 1);   // slider grows
    grid->setColumnStretch(2, 0);   // value fixed
    auto *help = new HelpBubble(tr(
        "Local volume = what you hear on your machine.\n"
        "Remote volume = what the other people in voice hear.\n"
        "Click the chain icon to link the two: moving one slider then\n"
        "moves the other by the same amount, preserving the offset."), this);

    grid->addWidget(capLocal,      0, 0);
    grid->addWidget(m_local,       0, 1);
    grid->addWidget(m_localLabel,  0, 2);
    grid->addWidget(m_link,        0, 3, 2, 1, Qt::AlignVCenter);
    grid->addWidget(help,          0, 4, 2, 1, Qt::AlignVCenter);
    grid->addWidget(capRemote,     1, 0);
    grid->addWidget(m_remote,      1, 1);
    grid->addWidget(m_remoteLabel, 1, 2);

    connect(m_local,  &QSlider::valueChanged, this, &VolumeControl::onLocalSliderMoved);
    connect(m_remote, &QSlider::valueChanged, this, &VolumeControl::onRemoteSliderMoved);
    connect(m_link,   &QToolButton::toggled,  this, &VolumeControl::onLinkClicked);
}

int  VolumeControl::local()  const { return m_local->value(); }
int  VolumeControl::remote() const { return m_remote->value(); }
bool VolumeControl::linked() const { return m_link->isChecked(); }

void VolumeControl::setLocal(int v)  { QSignalBlocker b(m_local);  m_local->setValue(v);  m_localLabel->setText(fmtPct(v));  }
void VolumeControl::setRemote(int v) { QSignalBlocker b(m_remote); m_remote->setValue(v); m_remoteLabel->setText(fmtPct(v)); }

void VolumeControl::setLinked(bool on) {
    bool wasOn = m_link->isChecked();
    {
        QSignalBlocker b(m_link);
        m_link->setChecked(on);
    }
    // Always refresh: the "Reset channels" path can call us with on
    // matching the current button state and we still need the
    // stylesheet recomputed because channelState was previously
    // diverging from the button - refreshing on every call is the
    // simplest way to keep them in lockstep.
    refreshTheme();
    if (wasOn == on) return;
    if (on) {
        // 1:1 link: snap remote to local immediately on toggle.
        m_internalSync = true;
        QSignalBlocker br(m_remote);
        m_remote->setValue(m_local->value());
        m_remoteLabel->setText(fmtPct(m_local->value()));
        m_internalSync = false;
        emit remoteChanged(m_local->value());
    }
    emit linkedChanged(on);
}

void VolumeControl::applyLinkDelta(QSlider *driver, QSlider *follower, int newValue) {
    // 1:1 lock - follower mirrors driver value exactly.
    Q_UNUSED(driver);
    m_internalSync = true;
    QSignalBlocker b(follower);
    follower->setValue(newValue);
    if (follower == m_remote) m_remoteLabel->setText(fmtPct(newValue));
    else                      m_localLabel->setText(fmtPct(newValue));
    m_internalSync = false;
    if (follower == m_remote) emit remoteChanged(newValue);
    else                      emit localChanged(newValue);
}

void VolumeControl::onLocalSliderMoved(int v) {
    m_localLabel->setText(fmtPct(v));
    if (m_link->isChecked() && !m_internalSync) {
        applyLinkDelta(m_local, m_remote, v);
    }
    emit localChanged(v);
}

void VolumeControl::onRemoteSliderMoved(int v) {
    m_remoteLabel->setText(fmtPct(v));
    if (m_link->isChecked() && !m_internalSync) {
        applyLinkDelta(m_remote, m_local, v);
    }
    emit remoteChanged(v);
}

void VolumeControl::refreshTheme() {
    Theme::Derived d = Theme::derive(Theme::colors());
    bool on = m_link->isChecked();
    // Mirror FxPanel slider QSS so all sliders share the same groove
    // and handle palette. Custom theme: groove=d.button (matches bottom
    // bar). Default palette: #1e1e1e (visibly darker than the #3a3a3a
    // channel frame).
    const QString grooveBg = Theme::colors().enabled
        ? d.button.name() : QStringLiteral("#1e1e1e");
    QString sliderCss = QStringLiteral(
        "QSlider { background: transparent; }"
        "QSlider::groove:horizontal { background: ") + grooveBg +
        QStringLiteral("; height: 6px; border-radius: 3px; }"
        "QSlider::handle:horizontal { background: ") + d.slider.name() +
        QStringLiteral("; width: 14px; margin: -5px 0; border-radius: 7px;"
        " border: 1px solid ") + d.border.name() + QStringLiteral("; }"
        "QSlider::handle:horizontal:hover { background: ") + d.accent.name() +
        QStringLiteral("; }"
        "QSlider::sub-page:horizontal { background: ") + d.accent.name() +
        QStringLiteral("; border-radius: 3px; }");
    m_local ->setStyleSheet(sliderCss);
    m_remote->setStyleSheet(sliderCss);
    QString base = on
        ? QString("QToolButton { background-color: #2e7d32; border: 1px solid #66bb6a;"
                  " border-radius: 5px; color: white; padding: 2px 4px; }")
        : QString("QToolButton { background-color: %1; border: 1px solid %2;"
                  " border-radius: 5px; color: %3; padding: 2px 4px; }")
              .arg(d.surface.name(), d.border.name(), d.textMuted.name());
    QString disabled = QString(
        "QToolButton:disabled { background-color: %1; border: 1px solid %2;"
        " color: %3; }").arg(d.disabledSurface.name(), d.disabledBorder.name(),
                             d.textMuted.name());
    m_link->setStyleSheet(base + disabled);
}

void VolumeControl::onLinkClicked(bool on) {
    if (on) {
        // Snap immediately on enable.
        m_internalSync = true;
        QSignalBlocker br(m_remote);
        m_remote->setValue(m_local->value());
        m_remoteLabel->setText(fmtPct(m_local->value()));
        m_internalSync = false;
        emit remoteChanged(m_local->value());
    }
    emit linkedChanged(on);
}
