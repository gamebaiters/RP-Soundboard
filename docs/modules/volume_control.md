# volume_control

Source: `src/modules/volume_control.{h,cpp}`

Dual local/remote volume sliders with link toggle.
Pure UI: emits value-changed signals, holds no audio state.

## Visual
```
[Local ] [=========|=====] [ 80%]   [chain]
[Remote] [============|==] [ 95%]
```

## API

| Slot | Effect |
|---|---|
| `setLocal(int)` | Set local slider value (0-100). |
| `setRemote(int)` | Set remote slider value. |
| `setLinked(bool)` | Toggle link state. Captures current delta (`remote - local`). |

| Getter | Returns |
|---|---|
| `local()` | int 0-100 |
| `remote()` | int 0-100 |
| `linked()` | bool |

| Signal | Fires when |
|---|---|
| `localChanged(int)` | Local slider moved (incl. via link). |
| `remoteChanged(int)` | Remote slider moved (incl. via link). |
| `linkedChanged(bool)` | Link toggled. |

## Link behavior
On link toggle ON: capture `delta = remote - local`. While linked,
moving either slider drives the other to preserve `delta`, clamped
to `[0..100]`.

## Dependencies
QSlider, QLabel, QToolButton, QGridLayout. No other modules.
