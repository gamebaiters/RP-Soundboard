# theme

Source: `src/modules/theme.{h,cpp}`

Centralized button-variant styles. Defines QSS rules for the
`buttonVariant` Qt property used across modules:

| variant   | used by                | look                         |
|-----------|------------------------|------------------------------|
| `audio`   | regular SoundButton    | unchanged (dark_style.qss)   |
| `macro`   | macro SoundButton      | yellow border                |
| `special` | ResetChannelsBtn etc.  | red background, white text   |

## API
```cpp
namespace Theme {
    QString variantStyleSheet();   // QSS snippet for variants
    void    apply(QApplication *); // appends to current app stylesheet
}
```

`apply()` is **idempotent**: it embeds a marker comment in the
appended QSS and skips re-appending if already present. Safe to call
multiple times.

## Where it's wired
The host (Phase 6.1 main layout) calls `Theme::apply(qApp)` once at
startup, after `style_helper` loads `dark_style.qss`. Module never
modifies dark_style.qss directly — variants are purely additive.

## Dependencies
QApplication, QString. No other modules.
