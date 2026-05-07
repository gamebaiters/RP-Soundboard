# New Modular Architecture

Target: a soundboard UI made of small, independent Qt widgets, each in
its own .h/.cpp pair under `src/modules/`. Every module owns a single
responsibility, exposes a narrow signal/slot contract, and depends on
no other module's internals.

---

## Folder layout

```
src/
  modules/
    module_registry.{h,cpp}
    volume_control.{h,cpp}
    fx_panel.{h,cpp}
    waveform_player.{h,cpp}
    channel.{h,cpp}
    button_grid.{h,cpp}
    search_bar.{h,cpp}
    button_advanced_panel.{h,cpp}
    settings_window.{h,cpp}
    config_io.{h,cpp}
    reset_channels_btn.{h,cpp}
    channel_state_persistence.{h,cpp}
    help_bubble.{h,cpp}        // wraps SpeechBubble
    theme.{h,cpp}              // button variants (macro / special / normal)
  // existing files stay; legacy config_qt.cpp shrinks to layout glue.
```

Each module gets a peer .md in `docs/modules/<module>.md`.

---

## Module contracts (signals/slots only — no shared state)

### `VolumeControl`
Inputs (slots):
- `setLocal(int 0-100)` / `setRemote(int 0-100)`
- `setLinked(bool)`
Outputs (signals):
- `localChanged(int)` / `remoteChanged(int)` / `linkedChanged(bool)`

### `FxPanel`
Inputs:
- `setPitch(int)` / `setSpeed(int)` / `setReverb(int)` / `setSync(bool)`
Outputs:
- `pitchChanged(int)` / `speedChanged(int)` / `reverbChanged(int)`
- `syncChanged(bool)` / `reset()` (button click)

### `WaveformPlayer`
Inputs:
- `setSound(SoundInfo)` / `setPosition(double)` / `clearPlayback()`
- `setFilename(QString)`
- `setPlaying(bool)` / `setPaused(bool)`
Outputs:
- `play()` / `pause()` / `stop()`
- `skip(int seconds)` (signed; +5/+10/-5/-10)
- `seekRequested(double 0-1)`

### `Channel`
Composes VolumeControl + FxPanel + WaveformPlayer + filename label
+ "add channel" button. Exposes:
- `channelId() const`
- `state() const → ChannelState struct` (snapshot for macros)
- `applyState(ChannelState)`
- signals: `stateChanged()`, `addChannelRequested()`,
  every child signal re-emitted with `channelId` prefixed.

### `ButtonGrid`
- `setRowsCols(int rows, int cols)`
- `setSounds(QList<SoundInfo>)`
- `setShowHotkeys(bool)`
- `setSearchFilter(QString)` (received from SearchBar)
- emits: `buttonTriggered(int idx)`, `buttonRightClicked(int idx)`,
  `buttonFileDropped(int idx, QList<QUrl>)`, `buttonReordered(int from, int to)`.

### `SearchBar`
- emits: `filterChanged(QString)`. Pinned to top.

### `ButtonAdvancedPanel` (separate window, not inline)
- Replaces soundsettings_qt; reuses `FxPanel` internally.
- `setSoundInfo(SoundInfo)` / emits `soundInfoChanged(SoundInfo)`,
  `assignMacroFromCurrentChannel(int channelId)`.

### `SettingsWindow` (separate top-level window)
- Owns `ExpandableSection` groups for: Audio, Grid, Hotkeys, Persistence, Import/Export.
- Each option = isolated child widget snippet.
- emits: `<option>Changed(...)`, `exportRequested()`, `importRequested(QString path)`.

### `ConfigIO`
- `exportToFile(QString path, ConfigModel&)`
- `importFromFile(QString path, ConfigModel&) → ImportResult`
  (validation: reject corrupted or schema-incompat files).

### `ResetChannelsBtn`
- emits: `resetRequested()` (after a confirmation modal). Only resets
  channel state — does not touch ConfigModel.

### `ChannelStatePersistence`
- Loads/saves per-channel-id FX state via QSettings.
- `loadState(int channelId) → ChannelState`
- `saveState(int channelId, ChannelState)`
- Toggled by SettingsWindow (off by default).

### `HelpBubble`
Thin wrapper / typedef around the existing SpeechBubble:
- `attachTo(QWidget*)` / `setText(...)` / `show()` / `hide()`.
- Used everywhere a `?` icon is needed.

### `theme.{h,cpp}` + `dark_style.qss` extension
Defines QSS classes:
- `.macro-button` — distinct border / background.
- `.special-button` — reset/save state distinct color.
- `.audio-button` — default sound button.

---

## Communication rules

1. **No cross-module state mutation.** A module never reads/writes
   another module's internal members. Communication is by signals
   only, brokered by the page-level layout (Phase 6.1) or by
   ConfigModel for persistent state.
2. **No circular deps.** Modules form a DAG:
   `theme → help_bubble → {volume_control, fx_panel, waveform_player,
    button_advanced_panel, search_bar, button_grid, settings_window,
    reset_channels_btn} → channel → main layout`.
3. **Module registry** lists every module, its file, and its public
   signal/slot list. Edit one place when adding a new module.

---

## Data ownership

| Lives in | Data |
|---|---|
| `ConfigModel` (existing) | Persistent app config: rows, cols, global FX, audio flags, button bindings, configs, hotkey toggles, multi-mode. |
| `SoundInfo` (existing, extended) | Per-button: file, FX, custom color, hotkey, **macro snapshot** (new optional ChannelState). |
| `Sampler` (existing) | Live audio engine state: per-slot playback position, FX values, mute. |
| `ChannelStatePersistence` | Per-channel-id remembered FX (only when "remember FX" is on). |
| Modules themselves | UI-only state (slider position, search text). They mirror ConfigModel/Sampler but never own ground truth. |

---

## ChannelState struct (new, in `modules/channel.h`)

```cpp
struct ChannelState {
    int volumeLocal;     // 0-100
    int volumeRemote;    // 0-100
    bool volumesLinked;
    int pitch;           // FxPanel range
    int speed;
    int reverb;
    bool fxSync;         // pitch↔speed link
    QString filename;    // last loaded
    double playbackPos;  // seconds, optional for macros
};
```

Used by:
- macros (Phase 3.4 right-click → snapshot).
- channel-state-persistence (Phase 4.5).
- channel.applyState() / channel.state().

---

## Migration plan summary

| Phase | Touches |
|---|---|
| 1 | docs/modules/ only; build script `--local` flag. No source changes. |
| 2 | New `modules/` files. `config_qt.cpp` still active; modules built but not yet wired into main UI. |
| 3 | ButtonGrid + SearchBar replace `gridWidget` + `filterEdit`. ButtonAdvancedPanel replaces soundsettings_qt. |
| 4 | SettingsWindow replaces `settingsWidget` + `configsWidget`. ResetChannelsBtn added. |
| 5 | HelpBubble wired into every module. Hotkey overlays. Theme variants. Legacy bottom UI removed. |
| 6 | `config_qt.cpp` becomes a thin shell that just assembles modules. Smoke test on Windows. |
