# Modular UI Rework - Changelog

## 2026-05-05 - Initial modular rewrite (Phases 1-6, local-only build)

Local-only refactor introducing a fully modular UI under `src/modules/`.
**No git push performed. No GitHub release. Local Windows build only.**

### Phase 1 - Audit & docs
- Created `docs/modules/` with `audit.md`, `entrypoints.md`,
  `architecture.md`.
- Created `build_local.ps1` (mandatory `-Local` switch, never publishes,
  auto-detects Qt 5.15.2 msvc2019_64 + VS2022, auto-downloads FFmpeg
  6.1.1 shared dev libs from gyan.dev release if not already in `ffmpeg/`).

### Phase 2 - Core modules
- `module_registry.{h,cpp}` - single source of truth listing every module.
- `volume_control.{h,cpp}` - dual local/remote sliders + chain link.
- `fx_panel.{h,cpp}` - pitch/speed/reverb sliders + sync + reset.
- `waveform_player.{h,cpp}` - waveform + transport + filename + time.
- `channel.{h,cpp}` - composes the three above; `ChannelState` struct
  with JSON serialization (used by macros + persistence).

### Phase 3 - Button grid + search + macros
- `search_bar.{h,cpp}` - top-pinned filter input.
- `button_grid.{h,cpp}` - rows x cols of `SoundButton`s, drag/drop,
  context menu (Edit / Clear / Set hotkey / Create macro).
- `button_advanced_panel.{h,cpp}` - per-button options as a separate
  `QDialog`; reuses `FxPanel`.
- `SoundInfo` extended with `isMacro` + `macroState` (QByteArray);
  read/write by `ConfigModel`.

### Phase 4 - Settings window + reset
- `settings_window.{h,cpp}` - separate `QDialog` for global options,
  grouped by Audio / Grid / Hotkeys / Import-Export.
- `config_io.{h,cpp}` - JSON envelope (schema "rp_soundboard_fx" v1)
  wrapping the existing INI payload; validates on import.
- `reset_channels_btn.{h,cpp}` - dedicated red "special" button with
  confirmation dialog; resets only channel state.
- `channel_state_persistence.{h,cpp}` - per-channel-id remembered FX
  in `<TS3 cfg>/rp_soundboard_channels.ini`. Off by default.

### Phase 5 - Help bubbles + theme
- `help_bubble.{h,cpp}` - reusable `?` icon wrapping `SpeechBubble`.
- `theme.{h,cpp}` - QSS variants for `buttonVariant` property:
  `audio` (default), `macro` (yellow border), `special` (red).
- Help bubbles wired into VolumeControl, FxPanel, WaveformPlayer,
  SearchBar, SettingsWindow.

### Phase 6 - Integration
- `main_page.{h,cpp}` - PURE LAYOUT assembly:
  SearchBar (top) -> ButtonGrid -> Channels (scrollable) -> bottom bar
  (ResetChannelsBtn + Settings).
- `main_page_wiring.{h,cpp}` - all cross-module wiring lives here.
  Connects modules to existing ConfigModel + Sampler.

### Build tooling
- `build_local.ps1` -Local -ConfigureOnly -Clean -Static
- `files.cmake` extended (no CMakeLists.txt change needed).
- All Phase 1-6 modules compile clean against Qt 5.15.2 / MSVC 19.44 /
  FFmpeg 6.1.1 dynamic.

### Open items
- `ConfigQt` (legacy 1876-LOC window) is unchanged; remains the live
  target of `sb_openDialog`. Swap to `MainPage` is one edit in
  `main.cpp` once user has smoke-tested MainPage.
- Multi-channel-to-Sampler-slot wiring uses Channel#0 only for now;
  per-slot mapping (Sampler has up to 5 slots) is the next iteration.
- `AudioEffectsDialog.{h,cpp}` flagged for removal in audit but kept
  for now (vestigial).
- `dark_style.qss` untouched; theme variants are additive QSS
  appended at runtime by `Theme::apply(qApp)`.

### Files NOT modified
- `plugin.{h,cpp}`, `main.{h,cpp}`, `samples.{h,cpp}`,
  `inputfileffmpeg.cpp`, `ConfigModel.cpp`, `SoundButton.{h,cpp}`,
  `SoundView` / `soundview_qt.{h,cpp}`, `SpeechBubble.{h,cpp}`,
  `ExpandableSection.{h,cpp}`, all macOS/Linux paths.
- macOS code paths (debug log, updater, codesign, talk-state) are
  untouched per the "do not break macOS" constraint.
