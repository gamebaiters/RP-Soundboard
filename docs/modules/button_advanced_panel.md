# button_advanced_panel

Source: `src/modules/button_advanced_panel.{h,cpp}`

Per-button advanced options as a **separate window** (`QDialog`,
non-modal). Replaces the inline `soundsettings_qt` dialog.
Reuses `FxPanel` for pitch/speed/reverb so the same module powers
both Channel and Button.

## Sections
- File (path + browse)
- Display (custom text + custom color)
- Volume modifier (-30..+30 dB)
- Crop (enabled + start + stop after/at)
- FX (FxPanel) + remember-fx checkbox
- Hotkey
- Macro (snapshot from current channel / clear)
- OK / Cancel

## API

| Method | Effect |
|---|---|
| `setSoundInfo(SoundInfo)` | Loads all fields from a SoundInfo. |
| `soundInfo() const` | Returns SoundInfo built from current fields. |
| `setHotkeyText(QString)` | Updates the hotkey button label. |

| Signal | Fires when |
|---|---|
| `hotkeyAssignRequested()` | "Set hotkey" pressed; host opens TS3 hotkey dialog. |
| `macroSnapshotRequested()` | "Snapshot current channel" pressed; host fills macroState. |
| `macroCleared()` | "Clear macro" pressed. |
| `soundInfoAccepted(SoundInfo)` | OK pressed; host should persist. |

## Macro flow (Phase 3.4)
1. User clicks "Snapshot current channel" -> panel emits
   `macroSnapshotRequested()`.
2. Host calls `Channel::state().toJson()` and writes the bytes into
   the panel's stored SoundInfo (via setSoundInfo with macroState
   filled).
3. Panel re-renders status label "Macro saved (N bytes)".
4. On OK, `soundInfoAccepted(SoundInfo)` fires; ConfigModel persists.

When `isMacro` is true, ButtonGrid sets the `buttonVariant` property to
`"macro"` so the QSS theme can decorate the button distinctively.

## Dependencies
fx_panel (Phase 2.3); QDialog, QFormLayout, QGroupBox, QLineEdit,
QSlider, QSpinBox, QCheckBox, QComboBox, QFileDialog, QColorDialog.
