# waveform_player

Source: `src/modules/waveform_player.{h,cpp}`

Waveform display + transport controls (play/pause, stop, +/-5, +/-10),
filename label, time readout. Wraps the existing `SoundView` widget for
the waveform paint and click-to-seek interaction.

## Visual
```
[-10s][-5s][Stop][Play/Pause][+5s][+10s]   filename.wav     0:32 / 1:48
[============================|=============================]   <- waveform
```

## API

| Slot | Effect |
|---|---|
| `setSound(SoundInfo)` | Loads the waveform + sets filename. |
| `setFilename(QString)` | Sets only the filename label. |
| `setPosition(double sec, double total)` | Updates "M:SS / M:SS" label. |
| `setPlaybackFraction(double f)` | Paints overlay marker (0..1). |
| `clearPlayback()` | Clears overlay + resets time label. |
| `setPlaying(bool)` / `setPaused(bool)` | Updates play/pause button text. |

| Signal | Fires when |
|---|---|
| `playClicked()` / `pauseClicked()` / `stopClicked()` | Transport buttons. |
| `skip(int seconds)` | Skip button. Signed: -10, -5, +5, +10. |
| `seekRequested(double f)` | Click on waveform; f in [0..1]. |

## Dependencies
QPushButton, QLabel, QHBoxLayout, QVBoxLayout, `SoundView`,
`SoundInfo`. No other modules.

## Notes
- `SoundView` is the existing waveform widget kept untouched (Phase 1
  marked it REUSE).
- The play/pause button text is purely cosmetic; the host wires
  `playClicked/pauseClicked` to the actual sampler.
