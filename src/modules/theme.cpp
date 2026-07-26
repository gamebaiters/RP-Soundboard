#include "theme.h"
#include "../style_helper.h"

#include <QApplication>
#include <QPointer>
#include <QVector>
#include <QStyle>
#include <QSlider>
#include <QPainter>
#include <QPen>
#include <QEvent>
#include <QChildEvent>
#include <QTimer>
#include <QFont>
#include <QScreen>

namespace Theme {

namespace {
Colors &active() {
    static Colors c = defaultColors();
    return c;
}
QVector<QPointer<QWidget>> &trackedWidgets() {
    static QVector<QPointer<QWidget>> v;
    return v;
}
QColor mix(const QColor &a, const QColor &b, qreal t) {
    return QColor::fromRgbF(
        a.redF()  *(1-t) + b.redF()  *t,
        a.greenF()*(1-t) + b.greenF()*t,
        a.blueF() *(1-t) + b.blueF() *t);
}
// HSL-space lightness shift: keeps saturation on saturated picks instead
// of washing them out the way mix-toward-black/white does.
//
// Qt's getHslF returns h=-1 for achromatic colors (greys). setHslF(-1, ...)
// leaves the QColor invalid (silent black). Clamping h>=0 round-trips
// greys cleanly - without it the default palette collapses to pure black.
QColor adjustL(const QColor &c, qreal delta) {
    qreal h, s, l, a;
    c.getHslF(&h, &s, &l, &a);
    if (h < 0.0) h = 0.0;
    l = qBound(0.0, l + delta, 1.0);
    QColor r;
    r.setHslF(h, s, l, a);
    return r;
}
QColor textOn(const QColor &bg) {
    return bg.lightnessF() < 0.5 ? QColor(0xec, 0xec, 0xec)
                                 : QColor(0x10, 0x10, 0x10);
}

// ---- bipolar-slider tagging -------------------------------------------
// Qt paints QSlider::sub-page from the groove's left edge up to the
// CENTRE of the handle. On a slider whose range straddles zero (pitch,
// speed, mic gain) the neutral position is the middle, so the accent
// fill covered half the groove while the value was 0 - it read as "set"
// when it was not. Tagging those sliders lets the stylesheet drop the
// fill (see dark_style.qss, sliderPolarity="bipolar").
// Paints a bipolar slider ENTIRELY by itself, by consuming the widget's
// paint event. Two earlier attempts went through QSlider::paintEvent
// overrides + stylesheet rules and both were defeated by the stylesheet
// style (empty sub-control rects, sub-page repainted by the host theme),
// so the fill kept starting at the left edge. Owning the whole paint is
// the only way that cannot be overridden: no QSS involvement, no style
// sub-control queries, no dependency on which QSlider subclass is used.
class BipolarPainter : public QObject {
public:
    static BipolarPainter *instance() {
        static BipolarPainter *p = new BipolarPainter();
        return p;
    }
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        if (ev->type() != QEvent::Paint) return false;
        auto *s = qobject_cast<QSlider *>(obj);
        if (!s || !s->isVisible()) return false;
        const QString polarity = s->property("sliderPolarity").toString();
        if (polarity.isEmpty()) return false;      // not one of ours
        const bool bipolar = (polarity == QLatin1String("bipolar"));

        const Derived &d = derivedCached();
        const bool horiz = (s->orientation() == Qt::Horizontal);
        const int  hw    = 14;                 // handle box (stylesheet)
        const int  track = 6;                  // groove thickness

        QPainter p(s);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);

        const int span = qMax(1, (horiz ? s->width() : s->height()) - hw);
        const int pos  = QStyle::sliderPositionFromValue(
                             s->minimum(), s->maximum(), s->value(), span,
                             horiz ? s->invertedAppearance()
                                   : !s->invertedAppearance()) + hw / 2;

        QRect trackRect, fillRect;
        // Bipolar: the bar grows out of the CENTRE. Unipolar (volume,
        // reverb, ...): from the start of the groove, like a normal
        // slider. Same geometry + same colours for both, so a channel
        // row never mixes two visual styles - which is what happened
        // while the unipolar ones were still painted by the host theme.
        const int origin = bipolar ? (horiz ? s->width() : s->height()) / 2
                                   : (horiz ? 0 : s->height());
        const int a = qMin(origin, pos), b = qMax(origin, pos);
        if (horiz) {
            const int y = (s->height() - track) / 2;
            trackRect = QRect(0, y, s->width(), track);
            fillRect  = QRect(a, y, b - a, track);
        } else {
            const int x = (s->width() - track) / 2;
            trackRect = QRect(x, 0, track, s->height());
            fillRect  = QRect(x, a, track, b - a);
        }

        const qreal r = track / 2.0;
        p.setBrush(s->isEnabled() ? d.surfaceAlt : d.disabledSurface);
        p.drawRoundedRect(trackRect, r, r);
        if ((horiz ? fillRect.width() : fillRect.height()) >= 2) {
            p.setBrush(s->isEnabled() ? d.accent : d.accentDisabled);
            p.drawRoundedRect(fillRect, r, r);
        }

        // Handle drawn by hand too - asking the style for it would drag
        // the stylesheet back into the picture. Metrics copied verbatim
        // from dark_style.qss (handle: 14 px across, margin -5 px over a
        // 6 px groove => 16 px along the groove, radius 7, slider grey
        // with a border, accent on hover) so a bipolar slider is visually
        // indistinguishable from every other slider in the soundboard.
        const int hLong  = track + 10;          // 16 px, matches margin -5
        const int hShort = hw;                  // 14 px
        QRect h = horiz
            ? QRect(pos - hShort / 2, (s->height() - hLong) / 2, hShort, hLong)
            : QRect((s->width() - hLong) / 2, pos - hShort / 2, hLong, hShort);
        p.setPen(QPen(s->isEnabled() ? d.border : d.disabledBorder, 1));
        p.setBrush(s->isEnabled()
                       ? (s->underMouse() ? d.accent : d.slider)
                       : QBrush(d.disabledSurface));
        p.drawRoundedRect(QRectF(h).adjusted(0.5, 0.5, -0.5, -0.5), 7.0, 7.0);
        return true;                            // paint fully handled
    }
};

void tagOneSlider(QSlider *s) {
    if (!s) return;
    // OPT-OUT: a slider that draws its own thing (EqBandWidget's segmented
    // LED column) must be left alone. The v2.3.5 "every soundboard slider is
    // painted by us" pass grabbed EVERY QSlider found by findChildren, and
    // BipolarPainter consumes the paint event - which silently replaced the
    // EQ band LEDs with a plain groove + handle. Any widget setting
    // selfPainted=true keeps its own paintEvent; remove the filter too, in
    // case an earlier pass already installed it.
    if (s->property("selfPainted").toBool()) {
        s->removeEventFilter(BipolarPainter::instance());
        return;
    }
    const bool bipolar = (s->minimum() < 0 && s->maximum() > 0)
                      || s->property("bipolarFill").toBool();
    const QString want = bipolar ? QStringLiteral("bipolar")
                                 : QStringLiteral("unipolar");
    if (s->property("sliderPolarity").toString() != want) {
        s->setProperty("sliderPolarity", want);
        if (s->style()) {
            s->style()->unpolish(s);
            s->style()->polish(s);
        }
    }
    // Every soundboard slider is painted by us - not just the bipolar
    // ones. Leaving the unipolar sliders to the stylesheet meant the
    // host theme could style them (bigger, lighter handle) while the
    // bipolar ones used our metrics, so a single row showed two
    // different slider designs.
    s->removeEventFilter(BipolarPainter::instance());
    s->installEventFilter(BipolarPainter::instance());
    s->setAttribute(Qt::WA_Hover, true);        // hover highlight works
    s->update();
}

void tagSlidersIn(QWidget *root) {
    if (!root) return;
    if (auto *s = qobject_cast<QSlider *>(root)) tagOneSlider(s);
    const auto sliders = root->findChildren<QSlider *>();
    for (QSlider *s : sliders) tagOneSlider(s);
}

// Keeps the tagging correct for sliders created (or re-ranged) after
// the window was built: runtime channels, lazily-built dialogs, the
// sandbox editor. Installed on every tracked top-level window.
class SliderTagger : public QObject {
public:
    static SliderTagger *instance() {
        static SliderTagger *t = new SliderTagger();
        return t;
    }
    void cover(QWidget *w) {
        if (!w) return;
        tagSlidersIn(w);
        w->removeEventFilter(this);
        w->installEventFilter(this);
        const auto kids = w->findChildren<QWidget *>();
        for (QWidget *c : kids) {
            c->removeEventFilter(this);
            c->installEventFilter(this);
        }
    }
protected:
    bool eventFilter(QObject *obj, QEvent *ev) override {
        switch (ev->type()) {
        case QEvent::ChildAdded: {
            QObject *child = static_cast<QChildEvent *>(ev)->child();
            if (child && child->isWidgetType()) {
                // The child is mid-construction: tag on the next tick,
                // when its range is set.
                QPointer<QWidget> w = static_cast<QWidget *>(child);
                QTimer::singleShot(0, w, [this, w]{ if (w) cover(w); });
            }
            break;
        }
        case QEvent::Show:
        case QEvent::Polish:
            if (obj->isWidgetType())
                tagSlidersIn(static_cast<QWidget *>(obj));
            break;
        default: break;
        }
        return QObject::eventFilter(obj, ev);
    }
};
}

Colors defaultColors() {
    Colors c;
    c.accent     = QColor(0x4a, 0x90, 0xe2);
    c.waveform   = QColor(0x4a, 0x90, 0xe2);
    c.background = QColor(0x2b, 0x2b, 0x2b);
    c.contrast   = 50;
    c.enabled    = false;
    return c;
}

Derived derive(const Colors &cIn) {
    Derived d;
    // Disabled = literal dark_style.qss palette so the default look is
    // byte-identical to the un-themed soundboard and the user's saved
    // bg can't bleed through when they opted out.
    if (!cIn.enabled) {
        d.bg              = QColor("#2b2b2b");
        d.surface         = QColor("#3a3a3a");
        d.button          = QColor("#3a3a3a");
        d.surfaceAlt      = QColor("#1e1e1e");
        d.hover           = QColor("#4a4a4a");
        d.pressed         = QColor("#1f1f1f");
        d.border          = QColor("#555555");
        d.borderStrong    = QColor("#777777");
        d.slider          = QColor("#888888");
        d.selectionBg     = QColor("#5a5a5a");
        d.altRow          = QColor("#252525");
        d.disabledBg      = QColor("#161616");
        d.disabledSurface = QColor("#2a2a2a");
        d.disabledBorder  = QColor("#2e2e2e");
        d.text            = QColor("#dcdcdc");
        d.textMuted       = QColor("#6a6a6a");
        d.accent          = QColor("#4a90e2");
        d.accentLight     = QColor("#6ab0ff");
        d.accentDark      = QColor("#3a78c2");
        d.accentDisabled  = QColor("#3a5a7a");
        d.waveform        = QColor("#4a90e2");
        return d;
    }
    const Colors &c = cIn;
    d.bg = c.background;
    // Lighten-on-dark / darken-on-light. <=0.5 catches saturated mid-
    // luminance picks (pure red/green/blue at l=0.5) so they still get
    // a visibly brighter button shade instead of going darker than bg.
    qreal dir = d.bg.lightnessF() <= 0.5 ? +1.0 : -1.0;
    // Contrast slider scales every L-shift: 0 -> 0.30x, 50 -> 1.00x,
    // 100 -> 1.80x.
    qreal s = 0.30 + qBound(0, c.contrast, 100) / 100.0 * 1.50;

    d.surfaceAlt      = adjustL(d.bg, -dir * 0.10 * s);
    d.surface         = adjustL(d.bg,  dir * 0.14 * s);
    d.button          = c.button.isValid() ? c.button
                                            : adjustL(d.bg, dir * 0.26 * s);
    d.hover           = adjustL(d.bg,  dir * 0.36 * s);
    d.pressed         = adjustL(d.bg, -dir * 0.10 * s);
    d.border          = adjustL(d.bg,  dir * 0.28 * s);
    d.borderStrong    = adjustL(d.bg,  dir * 0.40 * s);
    d.slider          = adjustL(d.bg,  dir * 0.48 * s);
    d.selectionBg     = adjustL(d.bg,  dir * 0.26 * s);
    d.altRow          = adjustL(d.bg,  dir * 0.06 * s);
    d.disabledBg      = adjustL(d.surfaceAlt, -dir * 0.06);
    d.disabledSurface = adjustL(d.surface,    -dir * 0.08);
    d.disabledBorder  = adjustL(d.border,     -dir * 0.10);
    d.text            = c.text.isValid() ? c.text : textOn(d.bg);
    d.textMuted       = mix(d.text, d.bg, 0.55);
    d.accent          = c.accent;
    d.accentLight     = adjustL(c.accent,  +0.10);
    d.accentDark      = adjustL(c.accent,  -0.10);
    d.accentDisabled  = mix(c.accent, d.surface, 0.55);
    d.waveform        = c.waveform;
    return d;
}

Colors colors() { return active(); }

// Memoised derive() of the ACTIVE palette. derive() runs a couple dozen
// HSL conversions; widgets that need derived colours in paintEvent
// (SoundView at 30 Hz per channel) read this cache instead of paying
// the recompute on every repaint. Invalidated by setColors().
namespace {
Derived &derivedCache() { static Derived d; return d; }
bool    &derivedCacheValid() { static bool v = false; return v; }
}

const Derived &derivedCached() {
    if (!derivedCacheValid()) {
        derivedCache() = derive(active());
        derivedCacheValid() = true;
    }
    return derivedCache();
}

void setColors(const Colors &c) {
    active() = c;
    derivedCacheValid() = false;
    refreshAllThemedWidgets();
}

void trackThemedWidget(QWidget *w) {
    if (!w) return;
    auto &list = trackedWidgets();
    for (auto &p : list) if (p.data() == w) return;
    list.append(QPointer<QWidget>(w));
    // Tag bipolar sliders now + keep tagging the ones built later.
    SliderTagger::instance()->cover(w);
    // HOST-THEME ISOLATION: the TS3 client applies its theme on qApp,
    // and those unscoped rules (switch checkboxes, 9pt fonts, slider
    // sub-page fills, ...) cascade into OUR windows and visually break
    // them. A WIDGET-level stylesheet always beats the app stylesheet
    // on conflicts, so every soundboard top-level window carries the
    // full composite sheet itself.
    if (w->isWindow())
        w->setStyleSheet(compositeStyleSheet());
}

// Bracket sentinels so we can replace ONLY the soundboard's contribution
// to qApp's stylesheet without nuking whatever the host (TS3) already
// installed. Replacing the entire qApp stylesheet wiped TS3's own dark
// theme the moment the soundboard window opened (white-background
// regression reported on 2026-05-07).
static const char *kRpsbBlockStart = "/*RPSB-BLOCK-START*/";
static const char *kRpsbBlockEnd   = "/*RPSB-BLOCK-END*/";

static void installRpsbBlock(QApplication *app)
{
    if (!app) return;
    QString existing = app->styleSheet();
    int s = existing.indexOf(QLatin1String(kRpsbBlockStart));
    if (s >= 0) {
        int e = existing.indexOf(QLatin1String(kRpsbBlockEnd), s);
        if (e >= 0) {
            int after = e + int(strlen(kRpsbBlockEnd));
            existing.remove(s, after - s);
        } else {
            existing.remove(s, existing.length() - s);
        }
    }
    QString block;
    block.reserve(8 * 1024);
    block += QLatin1String(kRpsbBlockStart);
    block += QLatin1Char('\n');
    block += compositeStyleSheet();
    block += QLatin1Char('\n');
    block += QLatin1String(kRpsbBlockEnd);
    if (!existing.isEmpty() && !existing.endsWith('\n')) existing += '\n';
    app->setStyleSheet(existing + block);
}

void refreshAllThemedWidgets() {
    // Append-only into qApp's stylesheet, bracketed by our markers, so
    // TS3's own qApp-level theme survives. Every selector inside the
    // block is scoped under [isGBSoundboard="true"] so the rules only
    // ever match soundboard widgets even though they sit on qApp.
    installRpsbBlock(qApp);
    // Repolish tracked widgets so theme changes apply this tick.
    auto &list = trackedWidgets();
    for (auto it = list.begin(); it != list.end();) {
        if (!it->data()) { it = list.erase(it); continue; }
        QWidget *w = it->data();
        // Re-arm the per-window isolation sheet with the fresh palette
        // (see trackThemedWidget): widget sheet > host app sheet.
        if (w->isWindow())
            w->setStyleSheet(compositeStyleSheet());
        tagSlidersIn(w);
        QList<QWidget*> subtree = w->findChildren<QWidget*>();
        subtree.prepend(w);
        for (auto *c : subtree) {
            if (c->style()) {
                c->style()->unpolish(c);
                c->style()->polish(c);
            }
            c->update();
        }
        ++it;
    }
}

namespace {
int &uiFontPtOverride() { static int pt = 0; return pt; }
}

void setUiFontPointSize(int pt) {
    if (pt < 0) pt = 0;
    if (pt > 0) pt = qBound(6, pt, 24);
    if (uiFontPtOverride() == pt) return;
    uiFontPtOverride() = pt;
    refreshAllThemedWidgets();
}

int uiFontPointSize() { return uiFontPtOverride(); }

QString fontStyleSheet() {
    // Base on the SYSTEM UI font: it already carries the desktop's DPI
    // scaling, so the soundboard reads exactly like every other app.
    // A hardcoded pixel size (the previous approach) turned microscopic
    // on scaled displays; a host theme's own font rule would otherwise
    // resize our carefully laid-out rows.
    const QFont sys = QApplication::font();
    QString family = sys.family();
    if (family.isEmpty()) family = QStringLiteral("Segoe UI");
    qreal pt = uiFontPtOverride() > 0 ? qreal(uiFontPtOverride())
                                      : sys.pointSizeF();
    if (pt <= 0.0) {
        // Font defined in pixels: convert with the screen's DPI.
        const int px = sys.pixelSize() > 0 ? sys.pixelSize() : 12;
        const qreal dpi = qApp->primaryScreen()
            ? qApp->primaryScreen()->logicalDotsPerInch() : 96.0;
        pt = px * 72.0 / (dpi > 0 ? dpi : 96.0);
    }
    if (pt < 6.0)  pt = 6.0;
    if (pt > 24.0) pt = 24.0;
    return QString(
        "QWidget[isGBSoundboard=\"true\"],\n"
        "QWidget[isGBSoundboard=\"true\"] QWidget {\n"
        "    font-family: \"%1\";\n"
        "    font-size: %2pt;\n"
        "    font-weight: normal;\n"
        "    font-style: normal;\n"
        "}\n").arg(family).arg(pt, 0, 'f', 1);
}

QString compositeStyleSheet() {
    return StyleHelper::loadDarkStyle() + "\n" + fontStyleSheet()
         + "\n" + variantStyleSheet();
}

static const char *kMarker = "/*RPSB-VARIANT-MARKER*/";

QString variantStyleSheet() {
    Derived d = derive(active());
    QString accent       = d.accent.name();
    QString accentLight  = d.accentLight.name();
    QString surface      = d.surface.name();
    QString border       = d.border.name();
    QString borderStrong = d.borderStrong.name();
    QString textMuted    = d.textMuted.name();
    // Every selector scoped under [isGBSoundboard="true"] so this CSS
    // can sit on qApp without leaking into the TS3 host.
    return QString::fromUtf8(kMarker) + R"(
/* Macro buttons - distinct yellow border */
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="macro"] {
    border: 2px solid #d39e00;
    border-radius: 6px;
}
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="macro"]:hover {
    border-color: #ffc83d;
}

/* Special buttons (Reset channels, Save state, etc.) */
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="special"] {
    background-color: #c63131;
    color: #ffffff;
    border: 1px solid #7c1c1c;
    border-radius: 6px;
    padding: 4px 10px;
}
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="special"]:hover {
    background-color: #e04141;
}
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="special"]:pressed {
    background-color: #a32626;
}

/* Default audio sound buttons - rounded corners + subtle border */
QWidget[isGBSoundboard="true"] QPushButton[buttonVariant="audio"] {
    border-radius: 6px;
    padding: 2px 4px;
}

/* Sync / link toggle buttons - active state highlights green */
QWidget[isGBSoundboard="true"] QToolButton[syncRole="link"] {
    border: 1px solid )" + border + R"(;
    border-radius: 5px;
    padding: 2px 4px;
    background-color: )" + surface + R"(;
    color: )" + textMuted + R"(;
}
QWidget[isGBSoundboard="true"] QToolButton[syncRole="link"]:checked {
    background-color: #2e7d32;
    border-color: #66bb6a;
    color: #ffffff;
}
QWidget[isGBSoundboard="true"] QToolButton[syncRole="link"]:hover {
    border-color: )" + borderStrong + R"(;
}

/* HelpBubble - just a clickable "?" character tinted by the accent. */
QWidget[isGBSoundboard="true"] QToolButton[helpRole="bubble"] {
    background-color: transparent;
    color: )" + accent + R"(;
    border: none;
    font-weight: bold;
}
QWidget[isGBSoundboard="true"] QToolButton[helpRole="bubble"]:hover {
    color: )" + accentLight + R"(;
}
)";
}

void apply(QApplication *app) {
    // Use the same append-only protocol as refreshAllThemedWidgets so
    // we never clobber TS3's existing qApp stylesheet.
    installRpsbBlock(app);
}

}
