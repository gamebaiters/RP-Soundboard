// Layout assembly only. Cross-module wiring lives in main_page_wiring.cpp.

#include "main_page.h"

#include "search_bar.h"
#include "button_grid.h"
#include "channel.h"
#include "reset_channels_btn.h"
#include "settings_window.h"
#include "onboarding_overlay.h"
#include "icon_factory.h"
#include "../style_helper.h"
#include "help_bubble.h"
#include "theme.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <algorithm>
#include <QToolButton>
#include <QPushButton>
#include <QButtonGroup>
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
    // Allow the user to shrink the window down to a compact dock-style
    // strip without Qt blocking it on internal sub-widget min sizes.
    setMinimumSize(480, 320);

    m_grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Smaller min lets the user shrink the soundboard window down to a
    // compact strip when they only need the channel row + transport.
    m_grid->setMinimumHeight(120);

    m_channelsLayout->setContentsMargins(0,0,0,0);
    m_channelsLayout->setSpacing(4);
    m_channelsLayout->addStretch(1);
    m_channelsHost->setLayout(m_channelsLayout);
    m_channelsScroll->setWidgetResizable(true);
    m_channelsScroll->setFrameShape(QFrame::NoFrame);
    m_channelsScroll->setWidget(m_channelsHost);
    m_channelsScroll->setMinimumHeight(130);
    // Max height is set dynamically by updateChannelsAreaHeight so a
    // single channel always fits without triggering the vertical
    // scrollbar (one channel + waveform + sandbox btn + meter still
    // overflows a hard 500px cap). When more channels are added the
    // area grows up to ~3 channels then starts scrolling.
    m_channelsScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
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
    m_pauseAllBtn->setIcon(IconFactory::pause());
    m_stopAllBtn->setIcon(IconFactory::stop());
    m_pauseAllBtn->setIconSize(QSize(16,16));
    m_stopAllBtn->setIconSize(QSize(16,16));
    m_addChannelBtn->setToolTip(tr("Add another independent playback channel."));
    m_pauseAllBtn->setToolTip(tr("Pause every channel at once. Click again to resume."));
    m_stopAllBtn->setToolTip(tr("Stop playback on every channel immediately."));
    m_muteLocally->setToolTip(tr(
        "Stop the soundboard from playing through your own speakers.\n"
        "Other people on the server still hear it normally."));
    m_muteMyself->setToolTip(tr(
        "Mute your microphone automatically while a sound is playing,\n"
        "so your own voice is not mixed on top of the soundboard audio."));

    // Profile switcher buttons
    m_profileGroup = new QButtonGroup(this);
    m_profileGroup->setExclusive(true);
    for (int i = 0; i < 4; ++i) {
        m_profileButtons[i] = new QToolButton(this);
        m_profileButtons[i]->setText(tr("P%1").arg(i + 1));
        m_profileButtons[i]->setCheckable(true);
        m_profileButtons[i]->setMinimumSize(36, 28);
        m_profileButtons[i]->setToolTip(tr("Profile %1").arg(i + 1));
        m_profileButtons[i]->setProperty("profileRole", QVariant(QString("selector")));
        m_profileGroup->addButton(m_profileButtons[i], i);
    }
    m_profileButtons[0]->setChecked(true);

    // Macro restore button (hidden by default)
    m_restoreMacroBtn = new QPushButton(tr("Undo macro"), this);
    m_restoreMacroBtn->setMinimumHeight(28);
    m_restoreMacroBtn->setVisible(false);
    m_restoreMacroBtn->setToolTip(tr(
        "Revert every channel to the state it had right before the\n"
        "last macro button was pressed."));
    m_restoreMacroBtn->setStyleSheet(
        "QPushButton { background-color: #3c6e9c; color: white; padding: 2px 10px;"
        " border-radius: 4px; } QPushButton:hover { background-color: #4a8bc2; }");

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(10,6,10,6);
    bottom->setSpacing(8);
    bottom->addWidget(m_addChannelBtn);
    bottom->addWidget(m_pauseAllBtn);
    bottom->addWidget(m_stopAllBtn);
    bottom->addWidget(m_restoreMacroBtn);
    bottom->addSpacing(16);
    bottom->addWidget(m_muteLocally);
    bottom->addWidget(m_muteMyself);
    m_previewOnly->setToolTip(tr(
        "Mute the soundboard on the server while still hearing it locally.\n"
        "Useful to test a sound or check timing before playing it for\n"
        "everyone in voice."));
    bottom->addWidget(m_previewOnly);
    bottom->addSpacing(12);
    for (int i = 0; i < 4; ++i) bottom->addWidget(m_profileButtons[i]);
    bottom->addWidget(new HelpBubble(tr(
        "Profiles (P1-P4): switch between four independent button-grid\n"
        "configurations. Each profile stores its own set of sounds, names\n"
        "and positions. Channel volume/FX/sandbox settings are shared\n"
        "across all profiles."), this));
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

    // First-run welcome overlay. Self-dismisses (and self-deletes) if it
    // has already been shown once before.
    (new OnboardingOverlay(this))->showIfFirstRun();
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
    // Compute height that snugly fits N channels - we use sizeHint of
    // the host so the scroll area never leaves dead grey space below
    // the last channel. Falls back to empirical per-channel guesses
    // before any channel widget has had a chance to lay out.
    const int n = m_channels.size() > 0 ? m_channels.size() : 1;
    int hostHint = 0;
    if (m_channelsHost) {
        m_channelsHost->adjustSize();
        hostHint = m_channelsHost->sizeHint().height();
    }
    const int perChannelFallback = waveformVisible ? 195 : 110;
    const int channelsFallback   = n * perChannelFallback + (n > 1 ? (n - 1) * 4 : 0);
    int target = std::max(hostHint, channelsFallback);
    // Cap so additional channels start to scroll rather than push the
    // button grid off-screen.
    const int hardCap = waveformVisible ? 600 : 340;
    target = std::min(target, hardCap);
    const int floor   = waveformVisible ? 150 : 90;
    target = std::max(target, floor);
    m_channelsScroll->setMinimumHeight(target);
    m_channelsScroll->setMaximumHeight(target);
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
