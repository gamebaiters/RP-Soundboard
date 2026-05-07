# Codebase Audit — Pre-Rewrite Snapshot

Date: 2026-05-05
Scope: `src/` of the GameBaiters RP-Soundboard fork (Qt C++ TS3 plugin).

This document freezes the **current** state of the codebase before the
modular UI rework. Source of truth for every file's responsibility,
public API, dependencies, and rewrite-disposition.

---

## 1. UI widgets (user-facing)

### `config_qt.{h,cpp,ui}` — main window — **REPLACE / DECOMPOSE**
- ~2,080 LOC. Monolithic: button grid, playback bar, waveform, dual
  volume sliders, pitch/speed/combined/reverb sliders, multi-soundboard
  per-slot bars (5 slots), config selector + hotkeys, settings panel,
  configs panel.
- Owns: `gridWidget`, `waveformView` (SoundView), `sl_volumeLocal`,
  `sl_volumeRemote`, `sl_pitch/speed/combined/reverb` (in
  `pitchSpeedContainer`, dynamically built), `multiBarWidget`,
  `settingsWidget`, `configsWidget`, `filterEdit`, `b_pause/b_stop/
  b_skip_*`, `playingIconLabel/playingLabel/lb_playback_time`.
- 50+ slots cover playback, FX, volumes, filter, button drop, multi-
  config selection.
- Depends on: ConfigModel, SpeechBubble, ExpandableSection, SoundButton,
  SoundView, AudioEffectsDialog, ui_config_qt.h.
- Decomposition (Phase 2-3): VolumeControl, FxPanel, WaveformPlayer,
  Channel (slot), ButtonGrid, SearchBar, SettingsWindow, ResetChannels.

### `soundsettings_qt.{h,cpp,ui}` — per-button advanced dialog — **DECOMPOSE / REUSE**
- ~472 LOC. File path, volume modifier, crop, per-sound FX (pitch/
  speed/reverb/sync), custom color, hotkey assignment.
- Will become **`ButtonAdvancedPanel`** (Phase 3.3) + share `FxPanel`
  with channels.

### `soundview_qt.{h,cpp}` — waveform widget — **REUSE**
- ~273 LOC. Custom-painted waveform, playback overlay, click-to-seek.
- API: `setSound(SoundInfo)`, `setPlaybackPosition(double)`,
  `clearPlayback()`, signal `seekRequested(double)`.
- Becomes building block of `WaveformPlayer` (Phase 2.4).

### `SoundButton.{h,cpp}` — single button — **REUSE / EXTEND**
- ~186 LOC. `QPushButton` subclass with drag/drop (file + button-
  reorder) and custom background color.
- Signals: `fileDropped(QList<QUrl>&)`, `buttonDropped(SoundButton*)`.
- Will gain: macro-style decoration, hotkey overlay (rendered by
  ButtonGrid, not the button itself).

### `ExpandableSection.{h,cpp,ui}` — collapsible container — **REUSE**
- ~87 LOC. Animated toggle, generic. No coupling to soundboard.
- Used by Phase 4 SettingsWindow groups.

### `SpeechBubble.{h,cpp}` — floating tooltip — **REUSE AS HelpBubble**
- ~261 LOC. Self-contained `QDialog` with paintable arrow, attachment
  to widget, closable toggle, optional "bubble" style.
- API: `setText`, `attachTo`, `setBackgroundColor`, `setClosable`,
  `setBubbleStyle`. Signal: `closePressed`.
- Becomes the Phase 5.1 `HelpBubble` module verbatim.

### `AudioEffectsDialog.{h,cpp}` — **REMOVE** (vestigial, not wired to current UI).

### `about_qt.{h,cpp,ui}` — static About dialog — **KEEP** (no rewrite).

### `updater_qt.{h,cpp,ui}` + `updater.ui` — auto-update dialog — **KEEP**.

---

## 2. Models / data

### `ConfigModel.{h,cpp}` — **KEEP, EXTEND**
- ~669 LOC. 4 named configs (slots), per-config: rows/cols, volumes,
  global FX, audio flags (earrape, mute-myself, mute-locally), bubble
  build-version flags, hotkey toggles.
- Observer pattern: `notifications_e` enum. Will be extended for new
  modules (per-channel state, macros).

### `SoundInfo.{h,cpp}` — **KEEP, EXTEND for macros**
- ~193 LOC. Per-button: filename, custom text/color, volume mod, crop
  start/stop, FX values + sync flag, "remember" flag.
- Phase 3.4 macro buttons store full channel snapshot inside SoundInfo
  (or alongside, TBD in architecture.md).

### `SampleBuffer.{h,cpp}` — **KEEP** (audio engine plumbing, untouched).

---

## 3. Audio engine

### `samples.{h,cpp}` — **KEEP, EXTEND signals**
- ~959 LOC. 5-slot multi-mode mixer; per-slot file decode, FX, volume,
  capture/playback path, earrape limiter.
- Signals: `onStartPlaying/StopPlaying/PausePlaying/UnpausePlaying`
  (slot, preview, filename).
- Per-channel state ops already exist: `setSlotVolumeLocal/Remote`,
  per-slot pitch/speed/intensity/reverb. UI only needs to wire to them.

### `SampleProducerThread.{h,cpp}` — **KEEP**.
### `SampleVisualizerThread.{h,cpp}` — **KEEP**.
### `inputfileffmpeg.cpp` — **KEEP** (decoder, recently fixed for WAV /
  pitch / speed / reverb in 1.0.118-119).
### `peakmeter.h` — **KEEP** (earrape limiter).

---

## 4. Plugin glue (TS3 SDK bridge)

### `plugin.{h,cpp}` — **KEEP**
- ~642 LOC. TS3 plugin entry, hotkey/menu wiring, `getTs3ConfigPath()`
  helper added in 1.0.119. Don't touch unless adding new menu items.

### `main.{h,cpp}` — **KEEP, EXTEND**
- ~504 LOC. C-API bridge (`sb_*`). Phase 6 may add `sb_resetChannels()`,
  `sb_triggerMacro()`, etc.

### `TalkStateManager.{h,cpp}` — **KEEP**.
### `UpdateChecker.{h,cpp}` — **KEEP**.
### `CmdQueue.{h,cpp}` — **KEEP**.
### `ts3log.{h,cpp}` — **KEEP**.

---

## 5. Utilities

| File | Purpose | Disposition |
|---|---|---|
| `HighResClock.{h,cpp}` | clock wrapper | KEEP |
| `common.h` | TS3 SDK + constants | KEEP |
| `bytebuffer.h` | empty placeholder | KEEP / IGNORE |
| `style_helper.{h,cpp}` | dark stylesheet loader | EXTEND (theme tokens) |
| `dark_style.qss` | stylesheet | EXTEND (macro/special variants) |
| `buildinfo.{c,h}` | build/version metadata C ABI | KEEP |

---

## 6. CANDIDATE REUSE summary

1. SpeechBubble → HelpBubble (Phase 5.1).
2. SoundView → embedded inside WaveformPlayer (Phase 2.4).
3. ExpandableSection → groups inside SettingsWindow (Phase 4).
4. PeakMeter → unchanged (audio path).
5. TalkStateManager / UpdateChecker / Sampler → unchanged.

---

## 7. CANDIDATE REPLACE / DECOMPOSE summary

| Source | Becomes |
|---|---|
| config_qt grid block | `modules/button-grid` (Phase 3.1) |
| config_qt filter edit | `modules/search-bar` (Phase 3.2) |
| config_qt playback bar | `modules/waveform-player` (Phase 2.4) |
| config_qt volume sliders + link | `modules/volume-control` (Phase 2.2) |
| config_qt pitch/speed/reverb sliders | `modules/fx-panel` (Phase 2.3) |
| config_qt multiBarWidget (5 slots) | N×`modules/channel` (Phase 2.5) |
| config_qt settingsWidget + configsWidget | `modules/settings-window` (Phase 4.1-2) |
| soundsettings_qt | `modules/button-advanced-panel` (Phase 3.3, reuses FxPanel) |
| AudioEffectsDialog | removed |
