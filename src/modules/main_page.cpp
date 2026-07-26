// Layout assembly only. Cross-module wiring lives in main_page_wiring.cpp.

#include "main_page.h"

#include "search_bar.h"
#include "button_grid.h"
#include "channel.h"
#include "mic_channel.h"
#include "channel_state_persistence.h"
#include "reset_channels_btn.h"
#include "settings_window.h"
#include "onboarding_overlay.h"
#include "icon_factory.h"
#include "../style_helper.h"
#include "help_bubble.h"
#include "theme.h"
#include "../ConfigModel.h"   // flushPendingWrite() on window close/hide
#include "../PlatformStyle.h" // macOS: Fusion base style for QSS fidelity
#include "../main.h"          // sb_isSelfTalking() for the talk indicator

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include <algorithm>
#include <QToolButton>
#include <QPushButton>
#include <QButtonGroup>
#include <QFrame>
#include <QSizePolicy>
#include <QCheckBox>
#include <QLabel>
#include <QResizeEvent>
#include <QSpinBox>
#include <QCloseEvent>
#include <QHideEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QShowEvent>
#include <QSettings>
#include <QPainter>
#include <cmath>

// Own-voice indicator for the bottom bar: LED + "Voice" caption.
//
// It answers ONE question - is my own voice reaching the server right now -
// and it is deliberately NOT TS3's raw talk status: while the soundboard
// plays, the plugin forces continuous transmission, so that status is lit
// even in dead silence. The audio side runs a real VAD on the captured mic
// block (sb_isMicVoiceDetected) and separately reports whether that voice
// survives the mute checkboxes and TS3's own gate (sb_isMicVoicePassing).
//
// Three states:
//   idle   - grey  : no voice detected on the microphone
//   held   - amber : voice detected but BLOCKED (mute-myself during
//                    playback, or TS3 is not transmitting: PTT up / below
//                    the client's VAD threshold)
//   live   - green : voice detected AND leaving for the server
//
// Painted, never QSS, so it follows Theme::colors() and cannot inherit the
// TS3 host palette.
class TalkStateDot : public QWidget {
public:
    explicit TalkStateDot(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(22);
        m_caption = MainPage::tr("Voice");
        setMinimumWidth(sizeHint().width());
        refreshTooltip();
    }
    QSize sizeHint() const override {
        return QSize(kDotBox + 4 + fontMetrics().horizontalAdvance(m_caption), 22);
    }
    void setVoiceState(bool detected, bool passing) {
        if (m_detected == detected && m_passing == passing) return;
        m_detected = detected;
        m_passing  = passing;
        refreshTooltip();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const Theme::Derived &d = Theme::derivedCached();
        // Green / amber are semantic (go / held), same language as the link
        // and warning controls elsewhere in the bar.
        const QColor live(0x3f, 0xb9, 0x50);
        const QColor held(0xe6, 0xa8, 0x3c);
        QColor fill, text;
        if (m_detected && m_passing)  { fill = live; text = live; }
        else if (m_detected)          { fill = held; text = held; }
        else                          { fill = d.disabledSurface; text = d.textMuted; }

        // The box is exactly as wide as the halo, and the halo is centred in
        // it: no dead padding on either side, so the LED sits at the normal
        // layout spacing from its neighbours like every other widget on the
        // bar. (Two earlier versions got this wrong in both directions: a
        // halo painted at x=-1 that the mic-FX button clipped, then a box
        // with 4 px of slack that opened a visible hole.)
        const qreal cy = height() / 2.0;
        const qreal r  = 6.0;
        const qreal cx = kDotBox / 2.0;
        // Halo while live so the state reads from the corner of the eye
        // without any animation.
        if (m_detected) {
            QColor halo = fill;
            halo.setAlpha(70);
            p.setPen(Qt::NoPen);
            p.setBrush(halo);
            p.drawEllipse(QPointF(cx, cy), kHaloR, kHaloR);
        }
        p.setPen(QPen(m_detected ? fill.darker(170) : d.border, 1.0));
        p.setBrush(fill);
        p.drawEllipse(QPointF(cx, cy), r, r);

        p.setPen(text);
        p.drawText(QRect(kDotBox + 4, 0, width() - kDotBox - 4, height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_caption);
    }
private:
    static constexpr qreal kHaloR  = 8.0;   // halo radius (dot r = 6)
    static const int       kDotBox = 16;    // = 2 * kHaloR, no slack
    void refreshTooltip() {
        // MainPage context so the string sits with the rest of the bottom bar.
        QString state = m_detected
            ? (m_passing
                ? MainPage::tr("Your voice is being detected AND sent to the server.")
                : MainPage::tr("Your voice is detected but is NOT reaching the "
                               "server (muted during playback, or TeamSpeak is "
                               "not transmitting)."))
            : MainPage::tr("No voice detected on your microphone.");
        setToolTip(state + QStringLiteral("\n\n") + MainPage::tr(
            "Green = heard by the others. Amber = you are talking but nothing "
            "goes out. Grey = silence.\n"
            "Based on the microphone signal itself, not on the soundboard: "
            "playing a sound does not light it up."));
    }
    QString m_caption;
    bool m_detected = false;
    bool m_passing  = false;
};

// Divider between the sound-button grid and the channels pane.
//
// This used to be a QSS rule on QSplitter::handle (transparent background +
// border-top + a margin meant to centre it). Two attempts at tuning that
// margin still left the rule sitting against the button grid instead of in
// the middle of the gap - the stylesheet style does not place a sub-control
// border where the box model says it should. Painting the handle ourselves
// removes the guesswork: the line is at height()/2, full stop.
class CenteredSplitterHandle : public QSplitterHandle {
public:
    CenteredSplitterHandle(Qt::Orientation o, QSplitter *parent)
        : QSplitterHandle(o, parent) {
        setAttribute(Qt::WA_Hover, true);   // hover highlight without a filter
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        const Theme::Derived &d = Theme::derivedCached();
        p.setPen(QPen(underMouse() ? d.accent : d.border, 1));
        if (orientation() == Qt::Horizontal) {
            const int x = width() / 2;
            p.drawLine(x, 0, x, height());
        } else {
            const int y = height() / 2;
            p.drawLine(0, y, width(), y);
        }
    }
};

class DividerSplitter : public QSplitter {
public:
    DividerSplitter(Qt::Orientation o, QWidget *parent)
        : QSplitter(o, parent) {}
protected:
    QSplitterHandle *createHandle() override {
        return new CenteredSplitterHandle(orientation(), this);
    }
};

// Factory split between the button grid and the channels pane. Retuned
// after the channel row lost ~30 px of height: the old 640/220 left the
// channels pane visibly padded out. Also the target of the Settings
// "restore default proportions" button.
static const QList<int> kDefaultSplit = { 700, 170 };

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
    // Bottom-bar labels are SHORT *and* say what the switch DOES, in terms
    // of who ends up hearing the soundboard. "Mute here" / "Mute mic" were
    // short but told you nothing. The two audience switches are deliberately
    // symmetric - "Only for others" (not on my speakers) vs "Only for me"
    // (server hears nothing) - and the mic one names its own action. Full
    // sentences live in the tooltips.
    , m_muteLocally(new QCheckBox(tr("Only for others"), this))
    , m_muteMyself(new QCheckBox(tr("Auto-mute my mic"), this))
    , m_previewOnly(new QCheckBox(tr("Only for me"), this))
    , m_addChannelBtn(new QPushButton(tr("+ Channel"), this))
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
    // applyResponsiveLayout() progressively hides secondary UI as the
    // window shrinks, so even this small only the grid remains.
    setMinimumSize(380, 240);

    m_grid->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Smaller min lets the user shrink the soundboard window down to a
    // compact strip when they only need the channel row + transport.
    m_grid->setMinimumHeight(120);

    m_channelsLayout->setContentsMargins(0,0,0,0);
    m_channelsLayout->setSpacing(4);
    m_channelsLayout->addStretch(1);
    m_channelsHost->setLayout(m_channelsLayout);

    // Pinned Microphone FX row - ALWAYS index 0 of the channels column,
    // above every playback channel. Self-contained (talks straight to
    // the MicFx singleton); visibility = feature switch AND the user's
    // last toggle (restored below once m_micFxBtn exists).
    m_micChannel = new MicChannel(m_channelsHost);
    m_channelsLayout->insertWidget(0, m_micChannel);
    m_micChannel->hide();
    m_channelsScroll->setWidgetResizable(true);
    m_channelsScroll->setFrameShape(QFrame::NoFrame);
    m_channelsScroll->setWidget(m_channelsHost);
    m_channelsScroll->setMinimumHeight(128);
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
        "Mute on my client.\n"
        "Stop the soundboard from playing through your own speakers.\n"
        "Other people on the server still hear it normally."));
    m_muteMyself->setToolTip(tr(
        "Mute myself during playback.\n"
        "Mute your microphone automatically while a sound is playing,\n"
        "so your own voice is not mixed on top of the soundboard audio."));

    // Profile switcher buttons
    m_profileGroup = new QButtonGroup(this);
    m_profileGroup->setExclusive(true);
    // Visible border so the P1..P4 read as buttons even when no theme
    // is active. The global QSS had no profileRole rule, so without
    // this the QToolButtons rendered borderless (looked like labels).
    // Per-widget stylesheet beats the cascade — checked variant shows
    // an accent fill so the active profile stays unambiguous.
    static const char *kProfileBtnQss =
        "QToolButton {"
        "  background-color: #2b2c30;"
        "  color: #d8d8d8;"
        "  border: 1px solid #5a5d63;"
        "  border-radius: 4px;"
        "  padding: 2px 6px;"
        "}"
        "QToolButton:hover {"
        "  background-color: #3a3c40;"
        "  border-color: #7a7d83;"
        "}"
        "QToolButton:pressed {"
        "  background-color: #1f2024;"
        "}"
        "QToolButton:checked {"
        "  background-color: #3c6e9c;"
        "  color: white;"
        "  border-color: #4a8bc2;"
        "}";
    for (int i = 0; i < 4; ++i) {
        m_profileButtons[i] = new QToolButton(this);
        m_profileButtons[i]->setText(tr("P%1").arg(i + 1));
        m_profileButtons[i]->setCheckable(true);
        m_profileButtons[i]->setMinimumSize(36, 28);
        m_profileButtons[i]->setToolTip(tr("Profile %1").arg(i + 1));
        m_profileButtons[i]->setProperty("profileRole", QVariant(QString("selector")));
        m_profileButtons[i]->setStyleSheet(kProfileBtnQss);
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

    // Mic FX toggle button (V1): shows/hides the pinned MicChannel
    // row. Hidden entirely (with the row) when the Settings feature
    // switch is off. Row visibility preference persists.
    m_micFxBtn = new QPushButton(QString::fromUtf8("\xF0\x9F\x8E\xA4"), this);
    m_micFxBtn->setMinimumHeight(28);
    m_micFxBtn->setMaximumWidth(40);
    m_micFxBtn->setCheckable(true);
    m_micFxBtn->setToolTip(tr(
        "Show / hide the Microphone FX panel (real-time voice changer)."));
    connect(m_micFxBtn, &QPushButton::toggled, this, [this](bool on){
        if (m_micChannel) m_micChannel->setVisible(on && m_micFeatureOn);
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        st.setValue(QStringLiteral("micfx/panel_visible"), on);
    });
    {
        // Restore the user's last panel state.
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        bool vis = st.value(QStringLiteral("micfx/panel_visible"), false).toBool();
        m_micFxBtn->setChecked(vis);
        if (m_micChannel) m_micChannel->setVisible(vis && m_micFeatureOn);
    }

    // The whole bottom bar lives in a host widget so the responsive
    // collapse can hide it in one call when the window gets too short.
    m_bottomBar = new QWidget(this);
    auto *bottom = new QHBoxLayout(m_bottomBar);
    bottom->setContentsMargins(10,6,10,6);
    bottom->setSpacing(8);
    bottom->addWidget(m_addChannelBtn);
    bottom->addWidget(m_micFxBtn);
    bottom->addWidget(m_pauseAllBtn);
    bottom->addWidget(m_stopAllBtn);
    bottom->addWidget(m_restoreMacroBtn);
    // Voice LED first in the "who can hear me" group, then the three mutes.
    // NO extra spacing before it: the widget already hugs its own halo, and
    // an addSpacing() here left a lopsided hole between the mic-FX button
    // and the LED that made the whole bar look misaligned.
    m_talkDot = new TalkStateDot(this);
    bottom->addWidget(m_talkDot, 0, Qt::AlignVCenter);
    bottom->addWidget(m_muteLocally);
    bottom->addWidget(m_muteMyself);
    m_previewOnly->setToolTip(tr(
        "Preview only — the server hears nothing.\n"
        "Mute the soundboard on the server while still hearing it locally.\n"
        "Useful to test a sound or check timing before playing it for\n"
        "everyone in voice."));
    bottom->addWidget(m_previewOnly);
    // Smooth preview-only blink. Three pieces:
    //   1) Stylesheet-based color override (the global QSS sets the
    //      QCheckBox color and beats QPalette role colours, so the
    //      previous palette-only attempt rendered no visible change
    //      under the themed UI).
    //   2) 30 Hz timer + sin-wave phase between two warning colours
    //      (warm orange <-> deep red) so the pulse fades smoothly
    //      rather than hopping between two discrete states.
    //   3) syncPreviewBlink() is also invoked from showEvent — the
    //      wiring restores the checkbox under a QSignalBlocker, so
    //      a checked-at-startup state never triggers ::toggled and
    //      the blink stayed dead. showEvent fires after wiring has
    //      pushed the restored model into the widgets.
    m_previewBlinkTimer = new QTimer(this);
    m_previewBlinkTimer->setInterval(33);   // ~30 Hz fade frame rate
    m_previewBlinkPhase = new QElapsedTimer();
    connect(m_previewBlinkTimer, &QTimer::timeout, this, [this]{
        // Period 1200 ms full cycle (back-and-forth fade). Half-cycle
        // ~600 ms — comfortably recognisable, not seizure-inducing.
        constexpr double kPeriodMs = 1200.0;
        const double phase = std::fmod(
            static_cast<double>(m_previewBlinkPhase->elapsed()), kPeriodMs)
            / kPeriodMs;
        // 0..1 triangular via |sin| — symmetric ramp up/down.
        const double t = 0.5 - 0.5 * std::cos(phase * 2.0 * 3.14159265358979);
        // Lerp from warm orange #ff8a1e to deep red #d4262e. Both
        // stay highly saturated so neither phase looks dim.
        auto lerp = [](int a, int b, double k){
            return static_cast<int>(a + (b - a) * k + 0.5);
        };
        int rr = lerp(0xff, 0xd4, t);
        int gg = lerp(0x8a, 0x26, t);
        int bb = lerp(0x1e, 0x2e, t);
        QString hex = QString::asprintf("#%02x%02x%02x", rr, gg, bb);
        // Per-widget stylesheet overrides the inherited QSS — Qt
        // applies widget styles on top of cascade. Bold weight was
        // removed (per user request) so the label width stays
        // constant and surrounding controls do not get pushed
        // around when the box toggles. Colour fade alone is enough
        // signal.
        m_previewOnly->setStyleSheet(QString(
            "QCheckBox { color: %1; }").arg(hex));
    });
    connect(m_previewOnly, &QCheckBox::toggled, this,
            [this](bool){ syncPreviewBlink(); });
    bottom->addSpacing(12);
    for (int i = 0; i < 4; ++i) bottom->addWidget(m_profileButtons[i]);
    m_profilesHelp = new HelpBubble(tr(
        "Profiles (P1-P4): switch between four independent button-grid\n"
        "configurations. Each profile stores its own set of sounds, names\n"
        "and positions. Channel volume/FX/sandbox settings are shared\n"
        "across all profiles."), this);
    bottom->addWidget(m_profilesHelp);
    bottom->addStretch(1);
    // Rows / cols selectors next to Settings — used to live inside the
    // Settings dialog but the user wanted them in arm's reach without
    // opening another window. Same QSpinBox API the SettingsWindow
    // copies used to expose, so the wiring + model layer is unchanged.
    m_rowsSpin = new QSpinBox(this);
    m_rowsSpin->setRange(1, 50);
    m_rowsSpin->setValue(4);
    m_rowsSpin->setMinimumHeight(28);
    m_rowsSpin->setToolTip(tr("Number of button rows in the grid"));
    m_colsSpin = new QSpinBox(this);
    m_colsSpin->setRange(1, 50);
    m_colsSpin->setValue(8);
    m_colsSpin->setMinimumHeight(28);
    m_colsSpin->setToolTip(tr("Number of button columns in the grid"));
    m_rowsLbl = new QLabel(tr("Rows:"), this);
    m_colsLbl = new QLabel(tr("Cols:"), this);
    bottom->addWidget(m_rowsLbl);
    bottom->addWidget(m_rowsSpin);
    bottom->addSpacing(4);
    bottom->addWidget(m_colsLbl);
    bottom->addWidget(m_colsSpin);
    bottom->addSpacing(8);
    bottom->addWidget(m_settingsBtn);
    bottom->addSpacing(8);
    bottom->addWidget(m_reset);

    // Grid | channels divider: a real QSplitter, so the bar between
    // the sound buttons and the channels area is DRAGGABLE - the user
    // decides how much height each pane gets. Position persists.
    m_splitter = new DividerSplitter(Qt::Vertical, this);
    m_splitter->setObjectName(QStringLiteral("gridChannelsSplitter"));
    m_splitter->setChildrenCollapsible(false);
    // Odd width so the self-painted 1 px rule lands on a whole pixel with
    // equal air above and below; also a comfortable grab area.
    m_splitter->setHandleWidth(13);
    m_splitter->addWidget(m_grid);
    m_splitter->addWidget(m_channelsScroll);
    m_splitter->setStretchFactor(0, 1);   // extra space goes to the grid
    m_splitter->setStretchFactor(1, 0);
    connect(m_splitter, &QSplitter::splitterMoved, this,
            [this](int, int){ saveSplitterState(); });
    {
        // Restore the user's saved pane split (whole-state restore also
        // brings back the exact handle position across resizes).
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        QByteArray state = st.value(
            QStringLiteral("mainpage/splitter_state")).toByteArray();
        if (!state.isEmpty())
            m_splitter->restoreState(state);
        else
            m_splitter->setSizes(kDefaultSplit);
    }

    // Hard rule between the whole grid+channels box and the global controls
    // underneath, so it reads as "everything above is the soundboard, this
    // strip drives it" instead of one continuous pile of widgets.
    m_bottomSeparator = new QFrame(this);
    m_bottomSeparator->setObjectName(QStringLiteral("bottomBarSeparator"));
    m_bottomSeparator->setFixedHeight(1);
    m_bottomSeparator->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 6, 8, 6);
    root->setSpacing(6);
    root->addWidget(m_search);
    root->addWidget(m_splitter, 1);
    root->addWidget(m_bottomSeparator);
    root->addWidget(m_bottomBar);

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

    // Voice LED poll. The capture thread writes atomics; touching widgets
    // from there is forbidden, so the GUI samples them. 20 Hz keeps the LED
    // in step with speech onsets, and the repaint only happens on an actual
    // state change.
    m_talkPollTimer = new QTimer(this);
    m_talkPollTimer->setInterval(50);
    connect(m_talkPollTimer, &QTimer::timeout, this, [this]{
        if (!m_talkDot || !m_talkDot->isVisible()) return;
        m_talkDot->setVoiceState(sb_isMicVoiceDetected(), sb_isMicVoicePassing());
    });
    m_talkPollTimer->start();

    // macOS: Fusion base style on the whole tree (and every widget
    // added later) so dark_style.qss renders with sane metrics. No-op
    // on Windows/Linux; the TS3 host style is never touched.
    PlatformStyle::apply(this);
}

void MainPage::resizeEvent(QResizeEvent *e) {
    QWidget::resizeEvent(e);
    applyResponsiveLayout();
    if (m_disconnectedOverlay) {
        m_disconnectedOverlay->setGeometry(rect());
        m_disconnectedOverlay->raise();
    }
}

void MainPage::resetLayoutProportions() {
    if (!m_splitter) return;
    // Wipe the saved state FIRST so a failure between here and the save
    // below still leaves the next start on the factory split.
    {
        QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
        st.remove(QStringLiteral("mainpage/splitter_state"));
    }
    m_splitter->setSizes(kDefaultSplit);
    updateChannelsAreaHeight(m_channelsScroll
                             && m_channelsScroll->isVisible());
    saveSplitterState();
}

void MainPage::saveSplitterState() {
    if (!m_splitter) return;
    QSettings st(QStringLiteral("GameBaiters"), QStringLiteral("Soundboard"));
    st.setValue(QStringLiteral("mainpage/splitter_state"),
                m_splitter->saveState());
}

void MainPage::applyResponsiveLayout() {
    if (!m_bottomBar || !m_splitter) return;
    const int w = width();
    const int h = height();

    // ---- horizontal: drop optional bottom-bar groups when squeezed ----
    // Measured, not guessed. Fixed pixel breakpoints stopped working the
    // moment the UI font changed size (bigger labels => wider buttons =>
    // the bar overflowed its own minimum and Settings ended up drawn on
    // top of Reset). The layout itself knows how much room it needs:
    // drop one optional group at a time and re-ask until the minimum
    // fits the available width.
    auto applyGroups = [this](bool mute, bool prof, bool grid) {
        // The LED has its OWN settings switch on top of the mute group's:
        // it can be turned off while the checkboxes stay.
        if (m_talkDot)     m_talkDot->setVisible(mute && m_wantVoiceDot);
        if (m_muteLocally) m_muteLocally->setVisible(mute);
        if (m_muteMyself)  m_muteMyself->setVisible(mute);
        if (m_previewOnly) m_previewOnly->setVisible(mute);
        for (int i = 0; i < 4; ++i)
            if (m_profileButtons[i]) m_profileButtons[i]->setVisible(prof);
        if (m_profilesHelp) m_profilesHelp->setVisible(prof);
        if (m_rowsLbl)  m_rowsLbl->setVisible(grid);
        if (m_rowsSpin) m_rowsSpin->setVisible(grid);
        if (m_colsLbl)  m_colsLbl->setVisible(grid);
        if (m_colsSpin) m_colsSpin->setVisible(grid);
        if (QLayout *l = m_bottomBar->layout()) {
            l->invalidate();
            l->activate();
        }
    };
    auto barFits = [this, w]() {
        QLayout *l = m_bottomBar->layout();
        // A little slack for the window margins so the last group is not
        // kept at the exact pixel where it would clip.
        return !l || l->minimumSize().width() <= w - 16;
    };

    // Order of sacrifice (user call): Rows/Cols FIRST — they are a layout
    // convenience you set once — then the profile switcher, and the mute
    // group LAST because those checkboxes decide who hears what and must
    // stay reachable as long as anything on the bar does.
    const bool wantMute = m_wantMuteChecks;
    const bool wantProf = m_wantProfiles;
    const bool wantGrid = m_wantGridSize;
    applyGroups(wantMute, wantProf, wantGrid);
    if (!barFits() && wantGrid) applyGroups(wantMute, wantProf, false);
    if (!barFits() && wantProf) applyGroups(wantMute, false, false);
    if (!barFits() && wantMute) applyGroups(false, false, false);

    // ---- vertical: channels area -> bottom bar -> search bar ----
    // The grid never hides: sound buttons are the whole point.
    const int gridMin = 130;   // grid min height + layout spacing
    const int searchH  = m_search ? m_search->sizeHint().height() : 0;
    const int bottomH  = m_bottomBar->sizeHint().height();
    const int chanH    = m_channelsScroll->minimumHeight();

    bool showSearch   = h >= gridMin + searchH + 18;
    bool showBottom   = h >= gridMin + (showSearch ? searchH : 0) + bottomH + 24;
    bool showChannels = showBottom &&
        h >= gridMin + (showSearch ? searchH : 0) + bottomH + chanH + 30;

    if (m_search) m_search->setVisible(showSearch);
    m_bottomBar->setVisible(showBottom);
    // Hiding the channels pane collapses its splitter slot; the grid
    // takes the whole splitter automatically.
    m_channelsScroll->setVisible(showChannels);
}

void MainPage::closeEvent(QCloseEvent *e) {
    // Event-triggered save: closing the soundboard window persists any
    // dirty model state so the user's volume / theme / sandbox settings
    // survive closing-without-quitting-TS3.
    ConfigModel::flushPendingWrite();
    QWidget::closeEvent(e);
}

void MainPage::hideEvent(QHideEvent *e) {
    // Same as closeEvent - covers the case where TS3 sends a hide
    // without firing closeEvent (e.g. tab switch in older builds).
    ConfigModel::flushPendingWrite();
    QWidget::hideEvent(e);
}

void MainPage::showEvent(QShowEvent *e) {
    QWidget::showEvent(e);
    // Re-sync the preview-only blink. The wiring restores the
    // checkbox under a QSignalBlocker so a checked-at-startup state
    // never triggers ::toggled — calling syncPreviewBlink here picks
    // it up regardless of how the state got set.
    syncPreviewBlink();
}

void MainPage::syncPreviewBlink() {
    if (!m_previewOnly || !m_previewBlinkTimer || !m_previewBlinkPhase)
        return;
    if (m_previewOnly->isChecked()) {
        if (!m_previewBlinkTimer->isActive()) {
            m_previewBlinkPhase->start();
            m_previewBlinkTimer->start();
        }
        // Force one immediate frame so the user sees the warning
        // colour before the first timer tick fires (~33 ms otherwise).
        m_previewOnly->setStyleSheet(
            QStringLiteral("QCheckBox { color: #ff8a1e; }"));
    } else {
        m_previewBlinkTimer->stop();
        // Clear the per-widget override so the inherited QSS theme
        // styling resumes — empty string disables widget-level QSS.
        m_previewOnly->setStyleSheet(QString());
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
    // Visible grab bar between grid and channels: themed strip that
    // lights up with the accent on hover so it reads as draggable.
    if (m_splitter) {
        // The handle paints itself (CenteredSplitterHandle) and reads the
        // theme at paint time - just make sure the QSS cascade leaves it
        // alone and force a repaint so a theme switch is visible at once.
        m_splitter->setStyleSheet(QStringLiteral(
            "QSplitter#gridChannelsSplitter::handle { background: transparent; }"));
        m_splitter->update();
        for (int i = 1; i < m_splitter->count(); ++i)
            if (QWidget *h = m_splitter->handle(i)) h->update();
    }
    if (m_bottomSeparator) {
        m_bottomSeparator->setStyleSheet(QString(
            "#bottomBarSeparator { background-color: %1; }").arg(d.border.name()));
    }
    if (m_micChannel) m_micChannel->refreshTheme();
}

void MainPage::setMuteChecksVisible(bool on) {
    m_wantMuteChecks = on;
    applyResponsiveLayout();
}

void MainPage::setProfileButtonsVisible(bool on) {
    m_wantProfiles = on;
    applyResponsiveLayout();
}

void MainPage::setGridSizeVisible(bool on) {
    m_wantGridSize = on;
    applyResponsiveLayout();
}

void MainPage::setVoiceIndicatorVisible(bool on) {
    m_wantVoiceDot = on;
    applyResponsiveLayout();
}

void MainPage::setMicFxFeatureVisible(bool on) {
    m_micFeatureOn = on;
    if (m_micFxBtn) m_micFxBtn->setVisible(on);
    if (m_micChannel)
        m_micChannel->setVisible(on && m_micFxBtn && m_micFxBtn->isChecked());
}

void MainPage::updateChannelsAreaHeight(bool waveformVisible) {
    // The channels-area HEIGHT is user-controlled now: the splitter
    // between the grid and the channels pane is draggable and its
    // position persists. This function only maintains a sensible
    // FLOOR so the pane can never be squeezed into an unusable strip;
    // the old fixed min==max cap is gone (it would fight the splitter).
    // Retuned with the compacted channel row (18 px sliders, tighter
    // margins): the old 150/90 floor reserved space the channel no longer
    // needs and kept the grid smaller than it had to be.
    const int floor = waveformVisible ? 128 : 76;
    m_channelsScroll->setMinimumHeight(floor);
    m_channelsScroll->setMaximumHeight(QWIDGETSIZE_MAX);
    // The channels-area floor feeds the vertical collapse thresholds.
    applyResponsiveLayout();
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
    // Renumber channels [idx .. end-1] so m_id matches positional index
    // again - without this, the per-channel sandbox dialog title, signal
    // emissions (sandboxStateChanged(id,...) etc.) and any external
    // persistence keyed by channelId all drift after a middle-channel
    // removal. Persistence shift below keeps the saved INI aligned with
    // the new positions.
    ChannelStatePersistence::shiftDownFrom(idx, m_channels.size());
    for (int i = idx; i < m_channels.size(); ++i)
        m_channels.at(i)->setChannelId(i);
    emit channelRemoved(idx);
}

void MainPage::setChannelCount(int n) {
    while (m_channels.size() < n) addChannel();
    while (m_channels.size() > n) removeChannel(m_channels.size() - 1);
}
