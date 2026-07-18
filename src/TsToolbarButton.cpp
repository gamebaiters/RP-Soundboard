#include "TsToolbarButton.h"
#include "main.h"          // sb_openDialog()
#include "ts3log.h"
#include "modules/icon_factory.h"

#include <QApplication>
#include <QMainWindow>
#include <QToolBar>
#include <QToolButton>
#include <QAction>
#include <QWidgetAction>
#include <QPointer>
#include <QTimer>
#include <QEvent>
#include <QIcon>
#include <QSignalBlocker>

#include <algorithm>

// v3 - OVERLAY architecture. v1/v2 added a QWidgetAction to the
// client's toolbar; the client's ImprovedToolBar re-laid EVERYTHING
// around the foreign action (all native buttons shrank, the away
// button corrupted, the cluster shifted). Lesson: never participate in
// the host toolbar's layout at all.
//
// The button is now a plain CHILD WIDGET of the toolbar, positioned
// manually with move() right after the last button of the left
// cluster (their exact rects come from QToolBar::actionGeometry, so
// the placement is pixel-accurate). The toolbar's own layout never
// sees us -> it cannot be disturbed, by construction. An event filter
// repositions the overlay on every toolbar Resize / LayoutRequest.

namespace {

QPointer<QToolBar>    s_toolbar;
QPointer<QToolButton> s_btn;
QPointer<QWidget>     s_window;   // the soundboard window being mirrored
QObject              *s_ctx = nullptr;   // context for timers + filters
int                   s_attempts = 0;
bool                  s_userEnabled = true;   // Settings gate
bool                  s_repositionPending = false;

void tryInstall();

// Sync the check state with the soundboard window's visibility, so the
// button stays truthful no matter how the window was opened/closed
// (Plugins menu, hotkey, its own [X], ...).
class WindowWatcher : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (obj == s_window && s_btn) {
            if (ev->type() == QEvent::Show) {
                QSignalBlocker b(s_btn);
                s_btn->setChecked(true);
            } else if (ev->type() == QEvent::Hide ||
                       ev->type() == QEvent::Close) {
                QSignalBlocker b(s_btn);
                s_btn->setChecked(false);
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
WindowWatcher *s_watcher = nullptr;      // child of s_ctx

// Compute the overlay geometry: flush against the right edge of the
// LEFT button cluster (= where the user expects a "native" button),
// vertically centered, sized to the toolbar height.
void reposition()
{
    if (!s_btn || !s_toolbar) return;
    QToolBar *tb = s_toolbar;
    const int W = tb->width();
    const int H = tb->height();
    if (W <= 0 || H <= 0) return;
    // Walk the actions IN ORDER and follow the contiguous run of
    // button-shaped rects from the left: that is the icon cluster.
    // Stop at the first wide rect (the expanding spacer - its rect
    // STARTS left of centre, which is why a naive "max right edge in
    // the left half" landed the button mid-bar) or at a big x jump
    // (right-aligned widgets).
    int rightEdge = -1;
    QRect lastRect;      // rect of the button we will sit NEXT TO
    const auto acts = tb->actions();
    for (QAction *a : acts) {
        if (!a->isVisible()) continue;
        const QRect r = tb->actionGeometry(a);
        if (!r.isValid() || r.width() <= 0) continue;
        const bool buttonLike = r.width() <= H * 2 + 8;
        const bool contiguous = (rightEdge < 0) || (r.x() <= rightEdge + 24);
        if (!buttonLike || !contiguous || r.x() > W / 2) break;
        if (r.right() > rightEdge) { rightEdge = r.right(); lastRect = r; }
    }
    if (rightEdge < 0) rightEdge = 2;   // empty bar: park at the left

    // Disappear ONLY on a real geometric collision (like a squeezed-out
    // native icon): with the right-side controls (volume, tray - the
    // leftmost action rect past the bar's midpoint) or with the bar
    // edge itself. Earlier "overflow detected" heuristics fired way too
    // early - the button vanished while the speaker was still far away.
    // Auto-size by CLONING the neighbour: native buttons are "always
    // perfect" because the toolbar gives every one the same box + the
    // toolbar's iconSize - so copy exactly that instead of computing
    // our own numbers. Same size, same top edge, same icon metric as
    // the button we sit next to; only the 4px gap is ours.
    const QSize btnSize = lastRect.isValid()
        ? lastRect.size()
        : QSize(std::min(H - 4, 24), std::min(H - 4, 24));
    QSize icoSize = tb->iconSize();
    if (!icoSize.isValid() || icoSize.width() <= 0)
        icoSize = QSize(btnSize.width() - 4, btnSize.height() - 4);
    icoSize = icoSize.boundedTo(btnSize - QSize(2, 2));

    int rightClusterStart = W;
    for (QAction *a : acts) {
        if (!a->isVisible()) continue;
        const QRect r = tb->actionGeometry(a);
        if (!r.isValid() || r.width() <= 0) continue;
        if (r.x() > W / 2)
            rightClusterStart = std::min(rightClusterStart, r.x());
    }
    // Vanish ONLY on a real overlap - the previous safety margins
    // (-4 px against the right cluster, -20 px against the bar edge)
    // made the button disappear while the speaker icon was still far
    // away. Native icons hold their place until they would actually be
    // covered, so match that exactly: the only forbidden states are
    // "our box crosses the first right-side control" and "our box runs
    // past the toolbar".
    const int x = rightEdge + 4;
    if (x + btnSize.width() > rightClusterStart
        || x + btnSize.width() > W) {
        s_btn->hide();
        return;
    }

    s_btn->setFixedSize(btnSize);
    s_btn->setIconSize(icoSize);
    s_btn->move(x, lastRect.isValid() ? lastRect.y() : (H - btnSize.height()) / 2);
    s_btn->raise();
    s_btn->show();
}

void scheduleReposition()
{
    if (s_repositionPending || !s_ctx) return;
    s_repositionPending = true;
    QTimer::singleShot(0, s_ctx, []{
        s_repositionPending = false;
        reposition();
    });
}

// Keeps the overlay glued to the cluster across window resizes and the
// toolbar's own relayouts.
class ToolbarWatcher : public QObject {
public:
    using QObject::QObject;
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (obj == s_toolbar) {
            switch (ev->type()) {
            case QEvent::Resize:
            case QEvent::LayoutRequest:
            case QEvent::Show:
            case QEvent::StyleChange:
            case QEvent::ActionAdded:
            case QEvent::ActionRemoved:
            case QEvent::ActionChanged:
                scheduleReposition();
                break;
            default: break;
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};
ToolbarWatcher *s_tbWatcher = nullptr;   // child of s_ctx

// The client has several bars (main buttons, bookmarks, ...): pick the
// visible one with the MOST actions - that is the button strip.
QToolBar *findHostToolbar()
{
    QToolBar *best = nullptr;
    int bestScore = -1;
    const auto tops = qApp->topLevelWidgets();
    for (QWidget *w : tops) {
        auto *mw = qobject_cast<QMainWindow *>(w);
        if (!mw) continue;
        const auto bars = mw->findChildren<QToolBar *>();
        for (QToolBar *tb : bars) {
            int score = tb->actions().size() + (tb->isVisible() ? 100 : 0);
            if (score > bestScore) { bestScore = score; best = tb; }
        }
    }
    return best;
}

// Tear down only the button + toolbar filter (used by the Settings
// toggle; remove() then only has the context left to kill).
void destroyButton()
{
    if (s_toolbar && s_tbWatcher)
        s_toolbar->removeEventFilter(s_tbWatcher);
    if (s_btn) {
        s_btn->hide();
        delete s_btn.data();
    }
    s_btn.clear();
    s_toolbar.clear();
}

void tryInstall()
{
    if (!s_userEnabled) return;
    if (s_btn) return;                    // already installed
    QToolBar *tb = findHostToolbar();
    if (!tb) {
        // Client UI not built yet (or redesigned). Retry a few times,
        // then give up silently - the Plugins menu still works.
        if (++s_attempts <= 8 && s_ctx)
            QTimer::singleShot(1500, s_ctx, []{ tryInstall(); });
        return;
    }

    logInfo("TsToolbarButton: overlay on toolbar '%s' (%s), %d actions, iconSize %dx%d",
            tb->objectName().toUtf8().constData(),
            tb->metaObject()->className(),
            (int)tb->actions().size(),
            tb->iconSize().width(), tb->iconSize().height());

    // NOTE deliberately NO icon-size "repair" here. An earlier build
    // forced tb->setIconSize(24) when the bar reported small icons -
    // but small icons can simply be the user's TS3 THEME, and the
    // forced size stomped that theme on every start. Rule going
    // forward: the overlay reads host properties, it NEVER writes
    // them. (Removing the override lets the theme size apply again on
    // the next client start.)

    auto *btn = new QToolButton(tb);      // plain child - NO action
    btn->setIcon(IconFactory::soundboard());
    btn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    btn->setCheckable(true);
    btn->setAutoRaise(true);
    btn->setToolTip(QObject::tr(
        "GameBaiters Soundboard - show / hide (plugin)"));
    QObject::connect(btn, &QToolButton::toggled, s_ctx, [](bool on){
        if (on) {
            sb_openDialog();              // creates + watches the window
        } else if (s_window) {
            s_window->hide();             // hideEvent flushes the config
        }
    });

    s_toolbar = tb;
    s_btn = btn;
    tb->removeEventFilter(s_tbWatcher);   // idempotent re-install
    tb->installEventFilter(s_tbWatcher);
    if (s_window && s_window->isVisible()) {
        QSignalBlocker b(btn);
        btn->setChecked(true);
    }
    reposition();
}

} // namespace

namespace TsToolbarButton {

void install()
{
    if (!qApp) return;
    if (!s_ctx) {
        s_ctx = new QObject();
        s_watcher = new WindowWatcher(s_ctx);
        s_tbWatcher = new ToolbarWatcher(s_ctx);
    }
    s_attempts = 0;
    tryInstall();
}

void setUserEnabled(bool on)
{
    s_userEnabled = on;
    if (!on) {
        destroyButton();
    } else {
        install();
    }
}

void watchWindow(QWidget *w)
{
    if (!w || !s_watcher) return;
    if (s_window == w) return;
    if (s_window) s_window->removeEventFilter(s_watcher);
    s_window = w;
    w->installEventFilter(s_watcher);
    if (s_btn) {
        QSignalBlocker b(s_btn);
        s_btn->setChecked(w->isVisible());
    }
}

void remove()
{
    // Every plugin-owned object out of the host BEFORE the DLL unloads.
    if (s_window && s_watcher) s_window->removeEventFilter(s_watcher);
    s_window.clear();
    destroyButton();
    delete s_ctx;                         // also deletes both watchers
    s_ctx = nullptr;
    s_watcher = nullptr;
    s_tbWatcher = nullptr;
}

}
