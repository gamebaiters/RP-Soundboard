# button_grid

Source: `src/modules/button_grid.{h,cpp}`

Rows x cols layout of `SoundButton` instances. Visual style of buttons
preserved (each cell is a `SoundButton`). Module owns no audio logic; it
only emits trigger / context / drop / reorder signals.

Macro buttons + special buttons get their distinct visual via the Qt
property `buttonVariant` set on each `SoundButton`. Theme classes are
defined in `theme.{h,cpp}` (Phase 5.4).

## Visual
```
+------+ +------+ +------+ +------+
| btn0 | | btn1 | | btn2 | | btn3 |   <- macro button might have a
+------+ +------+ +------+ +------+      different border via QSS
| btn4 | | btn5 | ...
+------+ +------+
```

## API

| Slot | Effect |
|---|---|
| `setRowsCols(int rows, int cols)` | Rebuilds the grid. |
| `setSounds(QList<SoundInfo>)` | Replaces all per-button data. |
| `setSoundAt(int idx, SoundInfo)` | Updates a single cell. |
| `setHotkeyOverlay(int idx, QString)` | Sets the displayed hotkey badge. |
| `setShowHotkeys(bool)` | Toggles overlay rendering globally. |
| `setSearchFilter(QString)` | Hides cells whose label doesn't match. |

| Signal | Fires when |
|---|---|
| `buttonTriggered(int idx)` | Left-click on cell `idx`. |
| `buttonRightClicked(int idx, QPoint globalPos)` | Right-click before menu opens. |
| `buttonFileDropped(int idx, QList<QUrl>)` | File(s) drag-dropped onto cell. |
| `buttonReordered(int from, int to)` | Cell `from` was dropped on cell `to`. |
| `editButtonRequested(int idx)` | Context-menu "Edit..." chosen. |
| `clearButtonRequested(int idx)` | Context-menu "Clear" chosen. |
| `setHotkeyRequested(int idx)` | Context-menu "Set hotkey..." chosen. |
| `createMacroRequested(int idx)` | Context-menu "Create macro from current channel" chosen. |

## Context menu
Right-click on any button:
- Edit...
- Clear
- Set hotkey...
- (separator)
- Create macro from current channel

## Dependencies
SoundButton (existing), SoundInfo, QGridLayout, QMenu, QStyle.
No dependency on Channel, but the host wires `createMacroRequested`
into a `Channel::state()` snapshot.

## Cross-module rules
ButtonGrid never reaches into Channel. The host bridges the two.
