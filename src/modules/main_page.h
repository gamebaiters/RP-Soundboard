// MainPage - the page-level layout for the rebuilt soundboard UI.
// LAYOUT ASSEMBLY ONLY. Logic lives in main_page_wiring.{h,cpp}.
//
// Layout (top -> bottom):
//   1. SearchBar                       (always visible at the top)
//   2. ButtonGrid                      (rows x cols of SoundButtons)
//   3. Channels area (1..N Channel widgets, scrollable)
//   4. Bottom bar: ResetChannelsBtn (left) | spacer | gear (right)
//
// MainPage exposes raw module pointers so the wiring layer can connect
// them to ConfigModel / Sampler / hotkey system. MainPage itself never
// imports ConfigModel or Sampler.

#pragma once

#include <QWidget>
#include <QVector>

class SearchBar;
class ButtonGrid;
class Channel;
class ResetChannelsBtn;
class SettingsWindow;
class QToolButton;
class QVBoxLayout;
class QScrollArea;
class QCheckBox;

class MainPage : public QWidget {
    Q_OBJECT
public:
    explicit MainPage(QWidget *parent = nullptr);

    SearchBar         *searchBar()        { return m_search; }
    ButtonGrid        *buttonGrid()       { return m_grid; }
    ResetChannelsBtn  *resetButton()      { return m_reset; }
    QToolButton       *settingsButton()   { return m_settingsBtn; }
    SettingsWindow    *settingsWindow()   { return m_settings; }
    QCheckBox         *muteLocallyBox()   { return m_muteLocally; }
    QCheckBox         *muteMyselfBox()    { return m_muteMyself; }
    QCheckBox         *previewOnlyBox()   { return m_previewOnly; }
    class QPushButton *addChannelBtn()    { return m_addChannelBtn; }
    class QPushButton *pauseAllBtn()      { return m_pauseAllBtn; }
    class QPushButton *stopAllBtn()       { return m_stopAllBtn; }

    QVector<Channel *> channels() const   { return m_channels; }
    Channel           *channelAt(int idx) const;
    Channel           *addChannel();      // appends a new Channel widget
    void               removeChannel(int idx);
    void               setChannelCount(int n);
    // Recompute the channels-area min/max height from current channel
    // count + waveform-visible flag so the soundboard can shrink when
    // there's only one channel.
    void               updateChannelsAreaHeight(bool waveformVisible);

    // Show/hide the "not connected" overlay covering the soundboard surface.
    void               setConnected(bool connected);
    bool               isConnected() const { return m_connected; }

    // Trigger a button as if clicked - used by the hotkey path so it
    // shares the same pipeline (round-robin slot, channel FX, error
    // dialogs, preview-stop) as a normal mouse click.
    void               triggerButton(int idx);

    // Re-apply theme-derived bg on m_channelsHost so the empty area
    // below a single channel doesn't show as un-themed grey on themes
    // with non-default backgrounds.
    void               refreshTheme();

signals:
    void channelAdded(int idx);
    void channelRemoved(int idx);

protected:
    void resizeEvent(class QResizeEvent *e) override;

private:
    SearchBar          *m_search;
    ButtonGrid         *m_grid;
    QScrollArea        *m_channelsScroll;
    QWidget            *m_channelsHost;
    QVBoxLayout        *m_channelsLayout;
    QVector<Channel *>  m_channels;
    ResetChannelsBtn   *m_reset;
    QToolButton        *m_settingsBtn;
    SettingsWindow     *m_settings;
    QCheckBox          *m_muteLocally;
    QCheckBox          *m_muteMyself;
    QCheckBox          *m_previewOnly;
    class QPushButton  *m_addChannelBtn;
    class QPushButton  *m_pauseAllBtn;
    class QPushButton  *m_stopAllBtn;
    class QFrame       *m_disconnectedOverlay;
    bool                m_connected = true;
};
