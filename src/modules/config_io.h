// ConfigIO - export/import full configuration to JSON file. Validates
// the file before applying. Rejects corrupted or schema-incompatible
// payloads with a non-zero ImportResult code.

#pragma once

#include <QString>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonArray>

class ConfigModel;

namespace ConfigIO {

enum class ImportResult {
    Ok = 0,
    FileError,         // could not open/read file
    ParseError,        // JSON parse failed
    SchemaError,       // missing required fields or wrong types
    VersionMismatch    // unknown schema version
};

constexpr int kSchemaVersion = 1;
constexpr const char *kSchemaName = "rp_soundboard_fx";

bool         exportToFile(const QString &path, const ConfigModel &model);
ImportResult importFromFile(const QString &path, ConfigModel &model);

// Raw legacy INI - what every prior soundboard release wrote / read. Lets
// users move configs between the new UI and old builds without manual
// conversion. Picked automatically when the chosen file has a .ini suffix.
bool         exportIniToFile(const QString &path, const ConfigModel &model);
ImportResult importIniFromFile(const QString &path, ConfigModel &model);

// Single-profile export / import (writes/reads only the section for the
// given config index 0..NUM_CONFIGS-1, in the legacy INI layout used by
// the old config selector).
bool         exportProfileIni(const QString &path, ConfigModel &model, int configIdx);
ImportResult importProfileIni(const QString &path, ConfigModel &model, int configIdx);

// FULL native backup: EVERYTHING the soundboard persists, in one
// self-contained .gbsb file - the three INIs (buttons/settings,
// channels, presets) as raw bytes + every key of the
// QSettings("GameBaiters","Soundboard") store (micfx, ambience,
// splitter, UI sections, sandbox mask, ...). Restore rewrites all of
// it verbatim, so the soundboard comes back EXACTLY as it was.
bool         fullBackupToFile(const QString &path, ConfigModel &model);
ImportResult fullRestoreFromFile(const QString &path);

// Lower-level helpers (also used by tests).
QByteArray   serialize(const ConfigModel &model);
ImportResult deserialize(const QByteArray &data, ConfigModel &model);

QString      humanError(ImportResult r);

}
