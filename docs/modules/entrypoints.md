# UI Entry Points — Current Wiring

Map of "where does X happen today" before the rewrite. Use this to find
the existing code each new module replaces.

---

## Button rendering

| Where | What |
|---|---|
| `config_qt.cpp::createButtons()` | Builds QGridLayout of `SoundButton` instances at startup; reads rows×cols from ConfigModel. |
| `config_qt.cpp::onModelNotify(NOTIFY_SET_BUTTON)` | Updates a single button text/color/icon when SoundInfo changes. |
| `SoundButton.cpp` | Drag-drop + custom paint of background color. |
| `config_qt.cpp::showButtonContextMenu()` | Right-click → "Set sound", "Clear", "Set hotkey". Context menu logic. |
| `config_qt.cpp::onButtonFileDropped()` | File drag-drop creates new SoundInfo for the dropped file. |
| `config_qt.cpp::onButtonDroppedOnButton()` | Reorder buttons by drag-drop. |
| `config_qt.cpp::onFilterEditTextChanged()` | Hides buttons whose text doesn't match filter (search). |

Phase 3 takeover: `ButtonGrid` (3.1), `SearchBar` (3.2),
`ButtonAdvancedPanel` (3.3), Macro on right-click (3.4).

## Multi-soundboard (per-slot playback)

| Where | What |
|---|---|
| `config_qt.cpp::createMultiBars()` | Builds 5 horizontal bars inside `multiBarWidget` when multi-mode toggled on. Each bar = volume + FX + waveform + transport for one slot. |
| `config_qt.cpp::onStartPlayingSound(slot)` | Routes start event to the corresponding bar. |
| `samples.cpp` | `playFile(slot)`, `setSlotVolumeLocal/Remote`, `setSlotPitch/Speed/Reverb`. Engine already supports per-slot. |
| `ConfigModel.cpp::multi_*` flags | "Multi mode enabled", "remember pitch/speed per slot", per-slot stored values. |

Phase 2 takeover: `Channel` module (2.5) wraps VolumeControl + FxPanel
+ WaveformPlayer for a single slot. Multi-mode = N×Channel.

## Settings (global options today)

| Where | What |
|---|---|
| `config_qt.ui::settingsWidget` | Bottom-of-window panel: Earrape, Link Volumes, Remember Pitch/Speed/Reverb, Multi Soundboard, Mute on my client, Mute myself, Rows, Columns, Show hotkeys on buttons, Disable hotkeys. |
| `config_qt.ui::configsWidget` | 4-config grid + Export / Import buttons. |
| `ConfigModel.{h,cpp}` | Persists everything via QSettings. |
| `config_qt.cpp::onSetConfig()` | Switch active config. |
| `config_qt.cpp::onSaveModel/onLoadModel` | Export/Import full config to JSON file. |

Phase 4 takeover: separate `SettingsWindow` (4.1), `ConfigIO` (4.3),
`ResetChannelsBtn` (4.4), `ChannelStatePersistence` (4.5).

## Hotkeys

| Where | What |
|---|---|
| `plugin.cpp::ts3plugin_initHotkeys()` | Registers TS3 hotkey definitions (`button_N`, `cfg_N`, `play`, `stop`, `pause`, etc.). |
| `plugin.cpp::ts3plugin_onHotkeyEvent()` | Routes TS3 hotkey events to `sb_onHotkey()`. |
| `main.cpp::sb_onHotkey()` → `ConfigQt` | Dispatches: trigger button N / switch config / play / stop / pause. |
| `config_qt.cpp::openHotkeySetDialog()` | Calls TS3's hotkey assignment dialog. |
| `config_qt.cpp::getShortcutString(buttonId)` | Reads currently-assigned shortcut text from TS3 settings.db. |
| `cb_show_hotkeys_on_buttons` checkbox | Toggles hotkey overlay on each `SoundButton` (rendered inline in button text today). |
| `cb_disable_hotkeys` checkbox | Globally disables hotkey dispatch. |

Phase 5.3 takeover: hotkey overlay rendering moves from button text
into `ButtonGrid::paintHotkeyOverlay()` (or per-button overlay layer).

## Playback bar / transport

| Where | What |
|---|---|
| `config_qt.ui::horizontalFrame` | -10s / -5s / Stop / Pause / +5s / +10s / now-playing label / time / waveform / filter / status label. |
| `config_qt.cpp::onSkipBack10/5/Fwd5/10` | Calls `samples.seek()` with delta. |
| `config_qt.cpp::onWaveformSeek` | Click on waveform → seek fraction. |
| `SoundView` (`soundview_qt.cpp`) | Waveform paint + click-emits `seekRequested`. |
| `config_qt.cpp::onPlayingIconTimer` | Updates time label every 100 ms. |

Phase 2.4 takeover: `WaveformPlayer` owns this whole bar (per channel).

## Per-button advanced dialog (today)

| Where | What |
|---|---|
| `soundsettings_qt.{h,cpp,ui}` | Modal dialog: file path, volume mod, crop, per-sound FX, color, hotkey button. |
| Triggered by | `config_qt.cpp::showButtonContextMenu()` → "Edit". |

Phase 3.3 takeover: `ButtonAdvancedPanel` (separate panel/modal,
shares FxPanel module with Channel).
