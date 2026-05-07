# search_bar

Source: `src/modules/search_bar.{h,cpp}`

Top-pinned filter input. Real-time filter on every keystroke.

## Visual
```
[ Search ] [ Search buttons...                           x ]
```

## API

| Slot | Effect |
|---|---|
| `setFilter(QString)` | Set text programmatically. |
| `clear()` | Empties the field. |

| Getter | Returns |
|---|---|
| `filter()` | Current text. |

| Signal | Fires when |
|---|---|
| `filterChanged(QString)` | Emitted on every text change. |

## Dependencies
QLineEdit, QLabel, QHBoxLayout. No other modules.

## Wiring
The host page wires `filterChanged` to `ButtonGrid::setSearchFilter`.
SearchBar itself never knows about ButtonGrid.
