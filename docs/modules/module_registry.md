# module_registry

Source: `src/modules/module_registry.{h,cpp}`

Single source of truth: every module that participates in the rebuilt
soundboard UI is listed in `kModules` (cpp). One entry per module.

## API
- `ModuleRegistry::all() -> const QVector<ModuleInfo>&`
- `ModuleRegistry::find(QString id) -> const ModuleInfo*`

## ModuleInfo
- `id` - unique stable identifier (also used for state lookups).
- `file` - relative path under `src/`.
- `summary` - one-line description.

## Adding a new module
1. Create `src/modules/<id>.{h,cpp}`.
2. Append entry in `module_registry.cpp::kModules`.
3. Append source files to `files.cmake`.
4. Add `docs/modules/<id>.md` peer doc.

## Dependencies
QString, QVector. No Qt widgets, no other modules.
