// theme - centralized colors + button-variant styles. Holds the accent +
// waveform colors used by every dialog, exposes a setter that re-applies
// the stylesheet across all tracked widgets in real time.

#pragma once

#include <QString>
#include <QColor>
#include <QPointer>
#include <QVector>
#include <QWidget>

class QApplication;

namespace Theme {

struct Colors {
    QColor accent;       // QSS accent (slider sub-page, button:checked, etc.)
    QColor waveform;     // SoundView fill color
    QColor background;   // Window background; all greys derive from this.
    QColor text;         // Optional text override. Invalid -> auto-contrast
                         // against background.
    QColor button;       // Optional button bg override. Invalid -> auto from
                         // adjustL(bg, +0.26 * scale).
    // 0..100 - scales how far the auto-derived surfaces / borders /
    // hovers shift away from the chosen background. 0 = barely visible,
    // 50 = balanced default, 100 = very strong contrast.
    int    contrast = 50;
    bool   enabled = false;
};

// Full palette derived from the user's three picks. Drives every hex
// substitution in dark_style.qss + variantStyleSheet so changing the
// background retints the entire UI (buttons, borders, inputs, etc.) with
// auto-contrasted text.
struct Derived {
    QColor bg;
    QColor surface;        // channel frame bg + slider groove (medium contrast)
    QColor button;         // QPushButton bg + soundboard cells (stronger
                           // contrast than surface so buttons stay visible
                           // even on top of a themed channel area)
    QColor surfaceAlt;     // input bg / scrollbar groove / list bg
    QColor hover;
    QColor pressed;
    QColor border;
    QColor borderStrong;   // checkbox / radio outline
    QColor slider;         // slider handle
    QColor selectionBg;
    QColor altRow;
    QColor disabledBg;
    QColor disabledSurface;
    QColor disabledBorder;
    QColor text;
    QColor textMuted;
    QColor accent;
    QColor accentLight;
    QColor accentDark;
    QColor accentDisabled;
    QColor waveform;
};

Derived derive(const Colors &c);

// Memoised derive(colors()) - safe to call from paintEvent at any rate.
// Invalidated automatically by setColors().
const Derived &derivedCached();

// Default values match the original dark_style.qss palette.
Colors defaultColors();

Colors colors();
void   setColors(const Colors &c);

// Returns the QSS snippet that defines button variants. Append to the
// base dark_style.qss so each window's stylesheet has both rule sets.
QString variantStyleSheet();

// Composite = base dark_style.qss + variant block, with the active
// theme tokens substituted. Call this in each soundboard window's
// constructor instead of touching qApp - the theme MUST stay scoped to
// soundboard widgets so it never leaks into the host TeamSpeak client.
QString compositeStyleSheet();

// Font rule generated from the SYSTEM UI font (or the user's override),
// scoped to soundboard widgets. Part of compositeStyleSheet.
QString fontStyleSheet();
// User font-size override in points; 0 = follow the system font.
// Persisted by ConfigModel (ui_font_pt) and pushed here at startup /
// when the Settings spin box changes.
void setUiFontPointSize(int pt);
int  uiFontPointSize();

// Deprecated. Kept as a no-op so existing call sites still compile;
// applying the theme app-wide leaks colors into the host TeamSpeak
// client which is exactly what we want to avoid.
void apply(QApplication *app);

// Track a widget that holds a per-instance copy of the dark stylesheet
// (e.g. MainPage / SettingsWindow / ButtonAdvancedPanel). When colors
// change, all tracked widgets get setStyleSheet() called again with the
// freshly-themed dark style so the change is visible immediately.
void trackThemedWidget(QWidget *w);
void refreshAllThemedWidgets();

}
