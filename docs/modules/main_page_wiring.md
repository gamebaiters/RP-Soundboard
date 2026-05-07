# main_page_wiring

Source: `src/modules/main_page_wiring.{h,cpp}`

Cross-module wiring for `MainPage`. Lives separately so the layout
file (`main_page.cpp`) stays free of business logic — see
`architecture.md` rule "no cross-module state mutation".

## API
```cpp
namespace MainPageWiring {
    void wire(MainPage *page, ConfigModel *model, Sampler *sampler);
}
```

`wire(...)`:
- pushes current ConfigModel state into modules (sounds, settings, grid).
- ensures at least one Channel exists.
- connects every module's signals to ConfigModel setters and to
  Sampler (when `sampler` is non-null).

## What gets wired

| Source signal | Effect |
|---|---|
| `SearchBar::filterChanged` | `ButtonGrid::setSearchFilter` |
| `ButtonGrid::buttonTriggered` | macro -> Channel::applyState; else Sampler::playFile |
| `ButtonGrid::buttonFileDropped` | new SoundInfo with file path -> ConfigModel |
| `ButtonGrid::clearButtonRequested` | reset SoundInfo for that index |
| `ButtonGrid::createMacroRequested` | snapshot Channel#0 state -> SoundInfo.macroState |
| `ButtonGrid::editButtonRequested` | open ButtonAdvancedPanel non-modal |
| `ButtonAdvancedPanel::soundInfoAccepted` | persist into ConfigModel |
| `ButtonAdvancedPanel::macroSnapshotRequested` | snapshot Channel#0 into the panel's SoundInfo |
| `Channel::volume->localChanged` | model + Sampler::setVolumeLocal |
| `Channel::volume->remoteChanged` | model + Sampler::setVolumeRemote |
| `Channel::fx->pitchChanged` | model + Sampler::setPitchFactor |
| `Channel::fx->speedChanged` | model + Sampler::setSpeedFactor |
| `Channel::fx->reverbChanged` | model |
| `Channel::fx->syncChanged` | model |
| `Channel::stateChanged` | (if persistence on) ChannelStatePersistence::saveState |
| `Channel::waveform->stopClicked` | Sampler::stopPlayback |
| `ResetChannelsBtn::resetRequested` | every Channel::applyState({}) + sampler stop |
| `SettingsWindow::*Changed` | matching ConfigModel setter |
| `SettingsWindow::exportRequested` | QFileDialog -> ConfigIO::exportToFile |
| `SettingsWindow::importRequested` | QFileDialog -> ConfigIO::importFromFile + refresh |
| `MainPage::settingsButton clicked` | settingsWindow->show() |

## Dependencies
main_page, search_bar, button_grid, channel, fx_panel,
volume_control, waveform_player, reset_channels_btn, settings_window,
button_advanced_panel, config_io, channel_state_persistence.
ConfigModel + Sampler (existing).

## Notes
The current wiring focuses on **single-channel** operation
(Channel#0 drives global volume / FX). A future iteration can map
Channel#N -> Sampler slot N once multi-mode UX is finalized.

`wire(...)` does NOT replace ConfigQt yet; the legacy window remains
the live `sb_openDialog` target until the user decides to switch.
The smoke test path (Phase 6.5) is: load the DLL in TS3, verify it
does not crash, then optionally toggle the swap.
