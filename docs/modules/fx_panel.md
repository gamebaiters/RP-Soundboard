# fx_panel

Source: `src/modules/fx_panel.{h,cpp}`

Pitch / Speed / Reverb sliders + pitch<->speed sync toggle + reset.
Pure UI: emits value-changed signals, holds no audio state.
Reverb is independent of the sync toggle.

## Visual
```
[Pitch ] [======|=========] [+12]   [chain]
[Speed ] [======|=========] [+12]
[Reverb] [|===============] [  3]   [Reset]
```

## Ranges
- Pitch  : -50..+50 (kPitchMin/Max)
- Speed  : -50..+50
- Reverb :   0..100

## API

| Slot | Effect |
|---|---|
| `setPitch(int)` / `setSpeed(int)` / `setReverb(int)` | Set slider values. |
| `setSync(bool)` | Toggle pitch<->speed link. Aligns speed to pitch on enable. |
| `resetAll()` | Resets pitch/speed/reverb to 0 and emits each. |

| Getter | Returns |
|---|---|
| `pitch()` / `speed()` / `reverb()` | int |
| `sync()` | bool |

| Signal | Fires when |
|---|---|
| `pitchChanged(int)` | Pitch slider moved (incl. via sync). |
| `speedChanged(int)` | Speed slider moved (incl. via sync). |
| `reverbChanged(int)` | Reverb slider moved. |
| `syncChanged(bool)` | Sync toggle changed. |
| `resetClicked()` | Reset button pressed (after `resetAll()` runs). |

## Sync behavior
With sync ON, pitch and speed mirror each other 1:1. Reverb stays
independent.

## Dependencies
QSlider, QLabel, QToolButton, QGridLayout. No other modules.
