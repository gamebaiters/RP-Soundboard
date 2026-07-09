// ButtonGrid - rows x cols layout of SoundButton instances. Emits
// trigger / context / drop / reorder signals; host wires them to the
// sampler and ConfigModel. Variant styling driven by the dynamic
// "buttonVariant" Qt property -> { "audio", "macro", "special" }.

#pragma once

#include <QWidget>
#include <QList>
#include <QUrl>
#include <QPointer>
#include <QVector>
#include <functional>
#include "../SoundInfo.h"

class QGridLayout;
class SoundButton;

// A channel that currently holds a resolved YouTube/URL video, offered as a
// right-click target so the user can save its link into a button or download
// its audio into a button. Live streams are link-only (isLive == true).
struct StreamChannelInfo {
    int     slot = -1;
    QString channelName;
    QString pageUrl;
    QString title;
    bool    isLive = false;
    // A whole playlist is also loaded in this channel (its panel is open/known):
    // offer "save the whole playlist into this button". playlistUrl is the
    // playlist page URL (distinct from pageUrl, the current single track).
    bool    isPlaylist = false;
    QString playlistUrl;
    QString playlistTitle;
};

class ButtonGrid : public QWidget {
    Q_OBJECT
public:
    explicit ButtonGrid(QWidget *parent = nullptr);

    int  rows() const { return m_rows; }
    int  cols() const { return m_cols; }
    int  count() const { return m_buttons.size(); }

public slots:
    void setRowsCols(int rows, int cols);
    void setSounds(const QList<SoundInfo> &sounds);
    void setSoundAt(int idx, const SoundInfo &info);
    void setHotkeyOverlay(int idx, const QString &shortcut);
    void clearAllHotkeyOverlays();
    void setShowHotkeys(bool on);
    void setSearchFilter(const QString &filter);
    // Programmatic equivalent of clicking a button, used to route TS3
    // hotkey events through the same trigger pipeline as a real click.
    void triggerByIndex(int idx);
    // Re-apply the SoundButton appearance for every cell. Called when
    // the theme changes so default-bg buttons pick up the new colors.
    void refreshAppearance();
    // Provider queried when a right-click menu opens: returns the channels that
    // currently hold a resolved YouTube/URL video, so the menu can offer
    // "save link" / "download audio" into this button per channel.
    void setStreamChannelsProvider(std::function<QVector<StreamChannelInfo>()> fn) {
        m_streamProvider = std::move(fn);
    }

signals:
    void buttonTriggered(int idx);
    void buttonRightClicked(int idx, const QPoint &globalPos);
    void buttonFileDropped(int idx, const QList<QUrl> &urls);
    void buttonReordered(int fromIdx, int toIdx);
    void createMacroRequested(int idx);
    // "Save Mic FX package into macro": freeze the CURRENT microphone
    // effect chain (sandbox state + pitch) into this button.
    void createMicMacroRequested(int idx);
    void editButtonRequested(int idx);
    void clearButtonRequested(int idx);
    void setHotkeyRequested(int idx);
    void chooseFileRequested(int idx);          // quick "open file" assign
    void renameMacroRequested(int idx);         // rename a macro button
    void saveLinkRequested(int idx);            // paste a URL -> live-stream cell
    // Right-click on a cell while a channel holds a resolved YouTube video:
    // save that channel's link into this button (works for live too).
    void saveStreamLinkToButton(int idx, const QString &pageUrl, const QString &title);
    // Download the channel's YouTube AUDIO into this button (VOD only). The
    // wiring asks for a destination folder, downloads, then binds the file.
    void downloadStreamToButton(int idx, const QString &pageUrl, const QString &title);
    // Save a WHOLE playlist into this button: triggering the button later opens
    // the entire playlist (panel + autoplay) instead of a single video.
    void savePlaylistToButton(int idx, const QString &playlistUrl, const QString &title);

private slots:
    void onButtonClicked();
    void onButtonContextMenu(const QPoint &local);
    void onButtonFileDropped(const QList<QUrl> &urls);
    void onButtonDroppedOnButton(SoundButton *target);

private:
    void rebuildLayout();
    void applyButtonAppearance(int idx);
    void applyFilter();
    int  indexOf(SoundButton *b) const;
    void showContextMenu(int idx, const QPoint &globalPos);

    int                       m_rows;
    int                       m_cols;
    bool                      m_showHotkeys;
    QString                   m_filter;
    QGridLayout              *m_grid;
    QVector<SoundButton *>    m_buttons;
    QVector<SoundInfo>        m_sounds;
    QVector<QString>          m_overlays;  // hotkey strings, parallel to m_buttons
    std::function<QVector<StreamChannelInfo>()> m_streamProvider;
};
