// ChannelStatePersistence - per-channel-id remembered FX state. Optional
// (off by default; toggled in SettingsWindow). Stored under the TS3
// config dir as a separate INI: rp_soundboard_channels.ini

#pragma once

#include "channel.h"
#include <QString>

namespace ChannelStatePersistence {

bool         isEnabled();
void         setEnabled(bool on);

bool         loadState(int channelId, ChannelState &out);
bool         saveState(int channelId, const ChannelState &state);
void         clearAll();

QString      filePath();   // resolved path under TS3 config dir

// Channel names persist independently of the enabled flag (they're always
// remembered so user-typed labels don't disappear when the FX-remember
// toggle is off).
QString      loadName(int channelId);
void         saveName(int channelId, const QString &name);

// Channel count persists separately so the "Restore last session" option
// can rebuild the same number of channels on next start. Always written /
// read (independent of the FX-remember enabled flag).
int          loadChannelCount();
void         saveChannelCount(int count);

// Cascade-shift persisted state + name from positional index `from+1`
// down to `from`. Called after MainPage::removeChannel(idx) so the
// persisted (positional) record stays aligned with the renumbered
// channel ids. `totalRemaining` is the post-remove channel count.
// Safe no-op when nothing is saved past `from`.
void         shiftDownFrom(int from, int totalRemaining);

}
