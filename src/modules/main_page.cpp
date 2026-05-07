// Layout assembly only. Cross-module wiring lives in main_page_wiring.cpp.

#include "main_page.h"

#include "search_bar.h"
#include "button_grid.h"
#include "channel.h"
#include "reset_channels_btn.h"
#include "settings_window.h"
#include "../style_helper.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QToolButton>
#include <QPushButton>
#include <QFrame>
#include <QSizePolicy>
#include <QCheckBox>
#include <QLabel>
#include <QResizeEvent>

MainPage::MainPage(QWidget *parent)
    : QWidget(parent)
    , m_search(new SearchBar(this))
    , m_grid(new ButtonGrid(this))
    , m_channelsScroll(new QScrollArea(this))
    , m_channelsHost(new QWidget)
    , m_channelsLayout(new QVBoxLayout)
    , m_reset(new ResetChannelsBtn(this))
    , m_settingsBtn(new QToolButton(this))
    , m_settings(new SettingsWindow(this))
    , m_muteLocally(new QCheckBox(tr("Mute on my client"), this))
    , m_muteMyself(new QCheckBox(tr("Mute myself during playback"), this))
    , m_previewOnly(new QCheckBox(tr("Preview only (server hears nothing)"), this))
    , m_addChannelBtn(new QPushButton(tr("+ Add channel"), this))
    , m_pauseAllBtn(new QPushButton(tr("Pause all"), this))
    , m_stopAllBtn(new QPushButton(tr("Stop all"), this))
{
    setWindowTitle(tr("GameBaiters - Soundboard"));
    // Soundboard root: the qApp scoped stylesheet matches via the
    // [isGBSoundboard] descendant selector. Keeps theming off the TS3 host.
    setProperty("isGBSoundboard", true);
    m_channelsHost->setObjectName("channelsHost");
    resize(1400, 900);

    m_grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_grid->setMinimumHeight(300);

    m_channelsLayout->setContentsMargins(0,0,0,0);
    m_channelsLayout->setSpacing(4);
    m_channelsLayout->addStretch(1);
    m_channelsHost->setLayout(m_channelsLayout);
    m_channelsScroll->setWidgetResizable(true);
    m_channelsScroll->setFrameShape(QFrame::NoFrame);
    m_channelsScroll->setWidget(m_channelsHost);
    // Pane fits one channel; extras scroll inside.
    // updateChannelsAreaHeight flips the height for the no-waveform mode.
    m_channelsScroll->setMinimumHeight(230);
    m_channelsScroll->setMaximumHeight(230);
    m_channelsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_channelsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Bottom bar - Reset (left) | spacer | Settings (right, gear icon)
    m_settingsBtn->setText(QString::fromUtf8("\xE2\x9A\x99 ") + tr("Settings"));
    m_settingsBtn->setToolTip(tr("Open settings (separate window)"));
    m_settingsBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_settingsBtn->setMinimumSize(110, 30);
    m_settingsBtn->setProperty("buttonVariant", QVariant(QString("audio")));
    m_settingsBtn->setObjectName("settingsButton");

    // Bottom row layout:
    //   [+Add] [Pause] [Stop] ... [Mute local] [Mute self] | [Settings] [Reset]
    m_addChannelBtn->setMinimumHeight(28);
    m_pauseAllBtn->setMinimumHeight(28);
    m_stopAllBtn->setMinimumHeight(28);
    m_pauseAllBtn->setIcon(QIcon(":/icon/img/pausebutton_32.png"));
    m_stopAllBtn->setIcon(QIcon(":/icon/img/stoparrow_32.png"));
    m_pauseAllBtn->setIconSize(QSize(16,16));
    m_stopAllBtn->setIconSize(QSize(16,16));

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(10,6,10,6);
    bottom->setSpacing(8);
    bottom->addWidget(m_addChannelBtn);
    bottom->addWidget(m_pauseAllBtn);
    bottom->addWidget(m_stopAllBtn);
    bottom->addSpacing(16);
    bottom->addWidget(m_muteLocally);
    bottom->addWidget(m_muteMyself);
    m_previewOnly->setToolTip(tr(
        "Mute the soundboard on the server while still hearing it locally.\n"
        "Useful to test a sound or check timing before playing it for\n"
        "everyone in voice."));
    bottom->addWidget(m_previewOnly);
    bottom->addStretch(1);
    bottom->addWidget(m_settingsBtn);
    bottom->addSpacing(8);
    bottom->addWidget(m_reset);

    auto *separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    root->addWidget(m_search);
    // Grid stretches; channels area stays anchored above the bottom bar.
    root->addWidget(m_grid, 1);
    root->addWidget(separator);
    root->addWidget(m_channelsScroll, 0);
    root->addLayout(bottom);

    // Disconnected overlay covers the whole window when no TS3 server.
    m_disconnectedOverlay = new QFrame(this);
    m_disconnectedOverlay->setObjectName("disconnectedOverlay");
    m_disconnectedOverlay->setStyleSheet(
        "#disconnectedOverlay { background-color: rgba(15, 15, 15, 220); }"
        "#disconnectedOverlay QLabel { color: #ffffff; font-size: 16px; "
        " font-weight: bold; padding: 24px; }");
    auto *ovLay = new QVBoxLayout(m_disconnectedOverlay);
    ovLay->setContentsMargins(0,0,0,0);
    auto *ovMsg = new QLabel(tr(
        "You are not connected to a TeamSpeak server.\n\n"
        "Connect to a server to use the soundboard."), m_disconnectedOverlay);
    ovMsg->setAlignment(Qt::AlignCenter);
    ovMsg->setWordWrap(true);
    ovLay->addStretch(1);
    ovLay->addWidget(ovMsg, 0, Qt::AlignCenter);
    ovLay->addStretch(1);
    m_disconnectedOverlay->hide();
}

void MainPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    if (m_disconnectedOverlay) {
        m_disconnectedOverlay->setGeometry(rect());
        m_disconnectedOverlay->raise();
    }
}

void MainPage::triggerButton(int idx) {
    if (m_grid) m_grid->triggerByIndex(idx);
}

void MainPage::refreshTheme() {
    if (!m_channelsHost) return;
    Theme::Derived d = Theme::derive(Theme::colors());
    m_channelsHost->setStyleSheet(QString(
        "#channelsHost { background-color: %1; }").arg(d.bg.name()));
}

void MainPage::updateChannelsAreaHeight(bool waveformVisible) {
    int h = waveformVisible ? 230 : 130;
    m_channelsScroll->setMinimumHeight(h);
    m_channelsScroll->setMaximumHeight(h);
}

void MainPage::setConnected(bool connected) {
    m_connected = connected;
    if (!m_disconnectedOverlay) return;
    if (connected) {
        m_disconnectedOverlay->hide();
    } else {
        m_disconnectedOverlay->setGeometry(rect());
        m_disconnectedOverlay->raise();
        m_disconnectedOverlay->show();
    }
}

Channel *MainPage::channelAt(int idx) const {
    return (idx >= 0 && idx < m_channels.size()) ? m_channels[idx] : nullptr;
}

Channel *MainPage::addChannel() {
    int id = m_channels.size();
    auto *ch = new Channel(id, m_channelsHost);
    m_channels.append(ch);
    // insert before the trailing stretch so channels stack at the top
    int insertAt = m_channelsLayout->count() > 0 ? m_channelsLayout->count() - 1 : 0;
    m_channelsLayout->insertWidget(insertAt, ch);
    emit channelAdded(id);
    return ch;
}

void MainPage::removeChannel(int idx) {
    if (idx < 0 || idx >= m_channels.size()) return;
    auto *ch = m_channels.takeAt(idx);
    m_channelsLayout->removeWidget(ch);
    ch->deleteLater();
    emit channelRemoved(idx);
}

void MainPage::setChannelCount(int n) {
    while (m_channels.size() < n) addChannel();
    while (m_channels.size() > n) removeChannel(m_channels.size() - 1);
}
