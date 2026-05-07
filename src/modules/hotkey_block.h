// HotkeyBlock - tiny shared registry of button ids whose TS3 hotkey is to be
// IGNORED at trigger time. The TS3 SDK gives us no way to actually unbind a
// hotkey from the host's profile, so "Reset hotkey" clears the visible
// overlay AND blocks the button here. Setting a fresh hotkey clears the
// block again.

#pragma once

namespace HotkeyBlock {
    bool isBlocked(int buttonIdx);
    void setBlocked(int buttonIdx, bool blocked);
    void blockAll(int totalButtons);
    void clearAll();
    // Persistence: load on plugin init so a Reset Hotkeys click survives
    // a TS3 client restart even though TS3 keeps the binding in its
    // hotkey profile.
    void load();
    void save();
}
