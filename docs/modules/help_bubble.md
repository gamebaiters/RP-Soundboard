# help_bubble

Source: `src/modules/help_bubble.{h,cpp}`

Reusable `?` icon next to any control. On hover: a tooltip. On click:
a richer `SpeechBubble` popup attached to the icon.

Wraps the existing `SpeechBubble` class for the popup itself; provides
a tiny `QToolButton` that drops cleanly into any layout.

## Visual
```
[ Pitch ] [=======|=======] [+12]   [?]
                                     ^
                                     hover -> tooltip
                                     click -> SpeechBubble popup
```

## API

```cpp
class HelpBubble : public QToolButton {
    HelpBubble(const QString &text, QWidget *parent = nullptr);
    void  setHelpText(const QString &);
    QString helpText() const;

    static HelpBubble *attachAfter(QWidget *target, const QString &helpText);
};
```

`attachAfter` is the easy way: pass a widget already inserted in a
QBoxLayout (HBox/VBox) and the help icon will be placed right after it.

## Usage example
```cpp
auto *slider = new QSlider(...);
layout->addWidget(slider);
HelpBubble::attachAfter(slider, tr("Drag to change pitch. -50 = lower, +50 = higher."));
```

## Where it's used
- VolumeControl: link toggle
- FxPanel: pitch / speed / reverb / sync / reset
- WaveformPlayer: every transport button
- SearchBar
- ResetChannelsBtn
- SettingsWindow
- ButtonAdvancedPanel macro section
- Macro buttons in the grid (via custom tooltip on the SoundButton)

## Dependencies
SpeechBubble (existing). QToolButton, QBoxLayout. No other modules.
