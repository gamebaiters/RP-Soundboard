# channel

Source: `src/modules/channel.{h,cpp}`

A single playback channel: composes `VolumeControl`, `FxPanel`,
`WaveformPlayer`, plus an "+ Add channel" button. Each channel has a
unique `channelId` and is independent of other channels.

## Visual
```
+------------------------------------------------+
| WaveformPlayer (transport + filename + waveform)|
| --------------------------------------------- |
| VolumeControl   |   FxPanel                   |
+------------------------------------------------+
                                  [+ Add channel]
```

## API

| Method | Returns |
|---|---|
| `channelId()` | int (constructor-supplied unique id) |
| `state()` | `ChannelState` snapshot of all child values |
| `applyState(ChannelState)` | Sets every child to match. Used by macros + persistence. |
| `volume()` / `fx()` / `waveform()` | Child widget pointers (host wires sampler signals here). |

| Signal | Fires when |
|---|---|
| `stateChanged(int channelId)` | Any child value changed. |
| `addChannelRequested(int afterChannelId)` | "+ Add channel" pressed. |

## ChannelState struct
```cpp
struct ChannelState {
    int     volumeLocal;
    int     volumeRemote;
    bool    volumesLinked;
    int     pitch;
    int     speed;
    int     reverb;
    bool    fxSync;
    QString filename;
    double  playbackPos;
};
```

Used by:
- macros (Phase 3.4): right-click → snapshot.
- channel_state_persistence (Phase 4.5).
- import/export (Phase 4.3).

## Dependencies
volume_control, fx_panel, waveform_player. QFrame, QHBoxLayout,
QVBoxLayout, QPushButton.

## Cross-module rules
Channel never reads/writes another channel's state. All inter-channel
coordination flows through the host page (Phase 6.1).
