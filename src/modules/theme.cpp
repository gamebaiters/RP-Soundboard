#include "theme.h"
#include "../style_helper.h"

#include <QApplication>
#include <QPointer>
#include <QVector>
#include <QStyle>

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

void setColors(const Colors &c) {
    active() = c;
    refreshAllThemedWidgets();
}

void trackThemedWidget(QWidget *w) {
    if (!w) return;
    auto &list = trackedWidgets();
    for (auto &p : list) if (p.data() == w) return;
    list.append(QPointer<QWidget>(w));
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

QString compositeStyleSheet() {
    return StyleHelper::loadDarkStyle() + "\n" + variantStyleSheet();
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
