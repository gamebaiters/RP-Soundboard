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
#include "../SoundInfo.h"

class QGridLayout;
class SoundButton;

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

signals:
    void buttonTriggered(int idx);
    void buttonRightClicked(int idx, const QPoint &globalPos);
    void buttonFileDropped(int idx, const QList<QUrl> &urls);
    void buttonReordered(int fromIdx, int toIdx);
    void createMacroRequested(int idx);
    void editButtonRequested(int idx);
    void clearButtonRequested(int idx);
    void setHotkeyRequested(int idx);
    void chooseFileRequested(int idx);          // quick "open file" assign
    void renameMacroRequested(int idx);         // rename a macro button

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
};
