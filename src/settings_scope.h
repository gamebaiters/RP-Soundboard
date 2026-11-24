// settings_scope.h - ONE place that names the QSettings store of the product.
//
// The TeamSpeak plugin and "Soundboard LIVE" (standalone/, compiles these
// sources in place) used to share QSettings("GameBaiters","Soundboard")
// (Windows: HKCU\Software\GameBaiters\Soundboard). That leaked LIVE state into
// the plugin: e.g. LIVE's custom microphone (micfx/enabled) came back active
// the next time TeamSpeak started. Every store construction now goes through
// this header:
//   * plugin build:  GB_SETTINGS_APP is not defined -> "Soundboard" (IDENTICAL
//                    to the historic store, nothing to migrate);
//   * LIVE build:    standalone/CMakeLists.txt defines GB_SETTINGS_APP as the
//                    bare token SoundboardLive (no quotes: stringified below,
//                    robust with MSVC + CMake escaping) -> "SoundboardLive".
// Never write QSettings("GameBaiters","Soundboard") literals again.
#pragma once

#include <QString>

#ifndef GB_SETTINGS_ORG
#define GB_SETTINGS_ORG GameBaiters
#endif
#ifndef GB_SETTINGS_APP
#define GB_SETTINGS_APP Soundboard
#endif

#define GB_SETTINGS_STR_(x) #x
#define GB_SETTINGS_STR(x)  GB_SETTINGS_STR_(x)

namespace GbSettings {

inline QString org() { return QString::fromLatin1(GB_SETTINGS_STR(GB_SETTINGS_ORG)); }
inline QString app() { return QString::fromLatin1(GB_SETTINGS_STR(GB_SETTINGS_APP)); }

} // namespace GbSettings
