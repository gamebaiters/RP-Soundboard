# config_io

Source: `src/modules/config_io.{h,cpp}`

Export and import the full configuration to a JSON file. Validates the
imported file before applying — corrupted or schema-incompatible
payloads are rejected with a non-zero `ImportResult` code.

## API

```cpp
namespace ConfigIO {
    enum class ImportResult {
        Ok, FileError, ParseError, SchemaError, VersionMismatch
    };

    bool         exportToFile(const QString &path, const ConfigModel &model);
    ImportResult importFromFile(const QString &path, ConfigModel &model);

    // Lower-level (used by tests)
    QByteArray   serialize(const ConfigModel &model);
    ImportResult deserialize(const QByteArray &data, ConfigModel &model);

    QString      humanError(ImportResult r);
}
```

## File format
JSON envelope:
```json
{
  "schema": "rp_soundboard_fx",
  "version": 1,
  "ini": "<base64-encoded INI text>"
}
```

The `ini` payload is the same format `ConfigModel::writeConfig` already
emits, base64-wrapped to keep newlines from breaking JSON. This means
every field the model knows about (including the new `isMacro` /
`macroState` fields on SoundInfo) round-trips automatically.

## Validation
`deserialize` rejects:
- Non-JSON input          -> ParseError
- Wrong/missing schema    -> SchemaError
- Unknown version         -> VersionMismatch
- Missing/empty ini blob  -> SchemaError

## Dependencies
ConfigModel (existing). QFile, QJsonDocument, QStandardPaths, QUuid.
No widgets — it's a pure storage module.
