# channel_state_persistence

Source: `src/modules/channel_state_persistence.{h,cpp}`

Per-channel-id remembered FX state. Optional (off by default; toggled
in SettingsWindow). Stored under the TS3 config dir as a separate INI:

```
<TS3 config>/rp_soundboard_channels.ini
```

The TS3 config dir is resolved at runtime via `getTs3ConfigPath()`
(plugin.cpp). Forward-declared inside this module to avoid pulling
the full TS3 SDK headers.

## API

```cpp
namespace ChannelStatePersistence {
    bool    isEnabled();
    void    setEnabled(bool on);

    bool    loadState(int channelId, ChannelState &out);
    bool    saveState(int channelId, const ChannelState &state);
    void    clearAll();

    QString filePath();   // resolved path, empty if config dir unknown
}
```

## File layout
```ini
[meta]
enabled=true

[channel_0]
state="<JSON-encoded ChannelState>"

[channel_1]
state="..."
```

## Dependencies
ChannelState (from `channel.h`). QSettings, QString.
Forward-declares `extern "C" const char *getTs3ConfigPath()` so it
does NOT include `plugin.h` (which would pull in TS3 SDK types like
`uint64` that break MOC compilation).

## Wiring
- SettingsWindow's `rememberPitchSpeedChanged(bool)` -> `setEnabled(on)`.
- Channel's `stateChanged(int)` (when enabled) -> `saveState(id, ch->state())`.
- On Channel construction (when enabled) -> `loadState(id)` + `applyState(...)`.
