#include "module_registry.h"

namespace ModuleRegistry {

static const QVector<ModuleInfo> kModules = {
    { "volume_control",            "modules/volume_control.{h,cpp}",            "Local + remote volume sliders with link toggle" },
    { "fx_panel",                  "modules/fx_panel.{h,cpp}",                  "Pitch/Speed/Reverb sliders with sync + reset" },
    { "waveform_player",           "modules/waveform_player.{h,cpp}",           "Waveform + transport (play/pause/stop/skip)" },
    { "channel",                   "modules/channel.{h,cpp}",                   "Single playback channel (volume + fx + waveform)" },
    { "button_grid",               "modules/button_grid.{h,cpp}",               "Sound button grid + drag/drop + hotkey overlay" },
    { "search_bar",                "modules/search_bar.{h,cpp}",                "Top-pinned filter input for the button grid" },
    { "button_advanced_panel",     "modules/button_advanced_panel.{h,cpp}",     "Per-button advanced options (separate window)" },
    { "settings_window",           "modules/settings_window.{h,cpp}",           "Global settings as a separate top-level window" },
    { "config_io",                 "modules/config_io.{h,cpp}",                 "Import/export config to JSON with validation" },
    { "reset_channels_btn",        "modules/reset_channels_btn.{h,cpp}",        "Reset only channel state (not global config)" },
    { "channel_state_persistence", "modules/channel_state_persistence.{h,cpp}", "Per-channel-id remembered FX (optional)" },
    { "help_bubble",               "modules/help_bubble.{h,cpp}",               "Reusable '?' help bubble (wraps SpeechBubble)" },
    { "theme",                     "modules/theme.{h,cpp}",                     "Button variant style helpers (macro/special/audio)" },
};

const QVector<ModuleInfo> &all() { return kModules; }

const ModuleInfo *find(const QString &id) {
    for (const auto &m : kModules) if (m.id == id) return &m;
    return nullptr;
}

}
