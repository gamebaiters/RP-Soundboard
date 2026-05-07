# main_page

Source: `src/modules/main_page.{h,cpp}`

The page-level layout for the rebuilt soundboard UI.
**LAYOUT ASSEMBLY ONLY.** All cross-module logic lives in
`main_page_wiring.{h,cpp}`.

## Layout (top -> bottom)
```
+-------------------------------------------------+
|   [SearchBar]                                   |  always visible
+-------------------------------------------------+
|   [ButtonGrid]                                  |  rows x cols of SoundButtons
+=================================================+
|   [Channel #0]    [Channel #1]   ...            |  scrollable
|   ...                                           |
+-------------------------------------------------+
| [ResetChannels]                       [Settings]|  bottom bar
+-------------------------------------------------+
```

## API
```cpp
class MainPage : public QWidget {
    SearchBar         *searchBar();
    ButtonGrid        *buttonGrid();
    ResetChannelsBtn  *resetButton();
    QToolButton       *settingsButton();
    SettingsWindow    *settingsWindow();

    QVector<Channel*>  channels() const;
    Channel           *channelAt(int idx) const;
    Channel           *addChannel();
    void               removeChannel(int idx);
    void               setChannelCount(int n);

signals:
    void channelAdded(int idx);
    void channelRemoved(int idx);
};
```

## Rules
- main_page.cpp must contain ZERO business logic.
  Wiring goes in main_page_wiring.cpp.
- The host (sb_openDialog or equivalent) creates a MainPage, calls
  `MainPageWiring::wire(page, configModel, sampler)`, then `show()`s
  the page.

## Dependencies
search_bar, button_grid, channel, reset_channels_btn,
settings_window. Plus QScrollArea, QToolButton.

## Migration status
- Phase 6.1: MainPage exists and compiles. NOT yet wired into
  `sb_openDialog`; `ConfigQt` (legacy) remains the live window
  pending user smoke test of MainPage.
- Phase 6.5: pending TS3 host load (user-driven).
