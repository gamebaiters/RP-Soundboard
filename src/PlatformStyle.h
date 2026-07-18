#pragma once

class QWidget;

// Platform-specific widget-style normalization.
//
// On macOS the native "macintosh" QStyle renders our dark_style.qss
// box-model rules incompletely (aqua metrics for combo boxes, spin
// boxes and checkboxes, focus rings painted over stylesheet borders,
// wrong control heights), which breaks the soundboard layout. The fix
// is the standard one: give every soundboard widget the Fusion base
// style so the stylesheet renders with the same metrics as on
// Windows/Linux. The host (TS3) application style is NEVER touched —
// the style is applied per-widget, recursively, and an event filter
// keeps covering widgets created later inside the same tree (channels
// added at runtime, lazily-created dialogs, ...).
//
// On Windows and Linux apply() is a no-op.
namespace PlatformStyle {

// Apply the platform fix to `root` and its current + future
// descendants. Safe to call more than once on the same tree.
void apply(QWidget *root);

}
