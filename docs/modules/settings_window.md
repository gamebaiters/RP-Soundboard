# settings_window

Source: `src/modules/settings_window.{h,cpp}`

Separate top-level dialog (`QDialog`, non-modal) for global options.
Replaces the inline `settingsWidget` and `configsWidget` in
`config_qt.ui`. Each option lives in its own child widget grouped by
section.

## Sections
- **Audio**: earrape protection, link volumes, remember pitch/speed/
  reverb, multi-soundboard, mute on my client, mute myself during
  playback.
- **Button grid**: rows, columns.
- **Hotkeys**: show on buttons, disable hotkeys.
- **Configuration import / export**: launches `ConfigIO`.

## API
For every option a `getter`, a `setSomething(...)` slot, and a
`somethingChanged(...)` signal exist. The host wires each signal to
`ConfigModel::set...` and each getter is read-only / for sanity reads.

Plus:
- `exportRequested()` — triggered by Export button.
- `importRequested()` — triggered by Import button.

## Dependencies
QCheckBox, QSpinBox, QPushButton, QGroupBox, QFormLayout, QVBoxLayout.
No other modules; ConfigIO is invoked by the host on the
`exportRequested`/`importRequested` signals.

## Where it's launched from
The "settings" gear button at the bottom-right of the main window
(Phase 4.1) opens this dialog.
