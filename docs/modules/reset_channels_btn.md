# reset_channels_btn

Source: `src/modules/reset_channels_btn.{h,cpp}`

Single dedicated button. Resets ONLY channel state (volumes, FX,
waveform). Does NOT touch global settings or button assignments.
Requires a confirmation dialog before firing.

## Visual
```
[ Reset channels ]
```
The button has the `buttonVariant = "special"` property, so the QSS
theme paints it differently from regular sound buttons.

## API
| Signal | Fires when |
|---|---|
| `resetRequested()` | After the user clicks Yes in the confirmation. |

## Confirmation copy
> This will reset every channel (volumes, pitch, speed, reverb)
> to its default state. Global settings and button assignments
> will NOT be affected. Proceed?

## Dependencies
QPushButton, QMessageBox, QVariant. No other modules.

## Wiring
The host page connects `resetRequested` to a per-channel
`Channel::applyState(ChannelState{})` (the default-constructed state).
