#include "button_grid.h"
#include "../SoundButton.h"

#include <QGridLayout>
#include <QFileInfo>
#include <QMenu>
#include <QAction>
#include <QVariant>

ButtonGrid::ButtonGrid(QWidget *parent)
    : QWidget(parent)
    , m_rows(0)
    , m_cols(0)
    , m_showHotkeys(false)
    , m_grid(new QGridLayout(this))
{
    m_grid->setContentsMargins(0,0,0,0);
    m_grid->setSpacing(3);
}

void ButtonGrid::setRowsCols(int rows, int cols) {
    if (rows == m_rows && cols == m_cols) return;
    m_rows = rows; m_cols = cols;
    rebuildLayout();
}

void ButtonGrid::setSounds(const QList<SoundInfo> &sounds) {
    m_sounds = QVector<SoundInfo>::fromList(sounds);
    if (m_overlays.size() != m_sounds.size()) m_overlays.resize(m_sounds.size());
    rebuildLayout();
}

void ButtonGrid::setSoundAt(int idx, const SoundInfo &info) {
    if (idx < 0 || idx >= m_sounds.size()) return;
    m_sounds[idx] = info;
    applyButtonAppearance(idx);
}

void ButtonGrid::setHotkeyOverlay(int idx, const QString &shortcut) {
    if (idx < 0) return;
    if (idx >= m_overlays.size()) m_overlays.resize(idx + 1);
    m_overlays[idx] = shortcut;
    if (idx < m_buttons.size()) applyButtonAppearance(idx);
}

void ButtonGrid::clearAllHotkeyOverlays() {
    for (int i = 0; i < m_overlays.size(); ++i) m_overlays[i].clear();
    for (int i = 0; i < m_buttons.size(); ++i) applyButtonAppearance(i);
}

void ButtonGrid::triggerByIndex(int idx) {
    if (idx >= 0 && idx < m_buttons.size()) emit buttonTriggered(idx);
}

void ButtonGrid::refreshAppearance() {
    for (int i = 0; i < m_buttons.size(); ++i) applyButtonAppearance(i);
}

void ButtonGrid::setShowHotkeys(bool on) {
    if (m_showHotkeys == on) return;
    m_showHotkeys = on;
    for (int i = 0; i < m_buttons.size(); ++i) applyButtonAppearance(i);
}

void ButtonGrid::setSearchFilter(const QString &filter) {
    m_filter = filter;
    applyFilter();
}

void ButtonGrid::rebuildLayout() {
    // Wipe stretches from previous grid dimensions before building anew.
    for (int c = 0; c < m_grid->columnCount(); ++c) m_grid->setColumnStretch(c, 0);
    for (int r = 0; r < m_grid->rowCount();    ++r) m_grid->setRowStretch(r,    0);

    // Tear down old buttons IMMEDIATELY. Do NOT setParent(nullptr) on a
    // visible QWidget - that turns it into a top-level window briefly
    // until the deferred delete runs, which is exactly the "200 floating
    // TeamSpeak cells" effect on rows/cols change.
    while (auto *item = m_grid->takeAt(0)) delete item;
    for (auto *b : m_buttons) delete b;   // Qt unlinks from parent on dtor
    m_buttons.clear();

    int total = m_rows * m_cols;
    if (m_sounds.size() < total) m_sounds.resize(total);
    if (m_overlays.size() < total) m_overlays.resize(total);

    for (int i = 0; i < total; ++i) {
        auto *b = new SoundButton(this);
        b->setContextMenuPolicy(Qt::CustomContextMenu);
        // Small floor so many-column grids keep shrinking with the
        // window instead of overflowing it; text elides via SoundButton.
        b->setMinimumSize(34, 20);
        b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        // Tag each button with its grid index so external drop targets
        // (e.g. Channel widgets) can identify the source button via the
        // SoundButton-mime drag payload.
        b->setProperty("buttonIndex", i);
        connect(b, &QPushButton::clicked,                  this, &ButtonGrid::onButtonClicked);
        connect(b, &QWidget::customContextMenuRequested,   this, &ButtonGrid::onButtonContextMenu);
        connect(b, &SoundButton::fileDropped,              this, &ButtonGrid::onButtonFileDropped);
        connect(b, &SoundButton::buttonDropped,            this, &ButtonGrid::onButtonDroppedOnButton);
        m_grid->addWidget(b, i / m_cols, i % m_cols);
        m_buttons.append(b);
    }
    // Ensure every column / row stretches uniformly so the grid fills.
    for (int c = 0; c < m_cols; ++c) m_grid->setColumnStretch(c, 1);
    for (int r = 0; r < m_rows; ++r) m_grid->setRowStretch(r, 1);
    for (int i = 0; i < m_buttons.size(); ++i) applyButtonAppearance(i);
    applyFilter();
}

void ButtonGrid::applyButtonAppearance(int idx) {
    if (idx < 0 || idx >= m_buttons.size()) return;
    auto *b = m_buttons[idx];
    const auto &s = m_sounds[idx];

    // Set the buttonVariant property BEFORE any setStyleSheet so Qt's QSS
    // cascade resolves the qApp themed rule for audio/macro on first
    // polish; setting it after setStyleSheet retriggers re-eval and the
    // ancestor selector wins.
    b->setProperty("buttonVariant", QVariant(QString(
        s.isMacro ? "macro" : (s.isStreamUrl ? "stream" : "audio"))));

    QString label;
    if (!s.customText.isEmpty()) {
        label = s.customText;
    } else if (s.isMacro) {
        label = tr("(macro)");
    } else if (s.isStreamUrl) {
        label = s.streamTitle.isEmpty() ? tr("(link)") : s.streamTitle;
    } else if (s.filename.isEmpty()) {
        label = tr("(empty)");
    } else {
        label = QFileInfo(s.filename).completeBaseName();
    }
    if (m_showHotkeys && idx < m_overlays.size() && !m_overlays[idx].isEmpty())
        label += "\n[" + m_overlays[idx] + "]";
    b->setText(label);

    b->setMacroDecoration(s.isMacro);
    b->setStreamDecoration(s.isStreamUrl && !s.isMacro);
    if (s.customColorEnabled())
        b->setBackgroundColor(s.customColor);
    else
        b->setBackgroundColor(QColor());
    b->setBackgroundImage(s.imagePath);
    // Lazy hover tooltip: FileMetadata only probes on the first hover
    // after this path is set, so applying a whole grid stays cheap.
    b->setSoundFilePath(s.filename);
}

void ButtonGrid::applyFilter() {
    QString f = m_filter.trimmed().toLower();
    for (int i = 0; i < m_buttons.size() && i < m_sounds.size(); ++i) {
        if (f.isEmpty()) {
            m_buttons[i]->setVisible(true);
            continue;
        }
        const auto &s = m_sounds[i];
        QString label = s.customText.isEmpty()
            ? QFileInfo(s.filename).completeBaseName()
            : s.customText;
        m_buttons[i]->setVisible(label.toLower().contains(f));
    }
}

int ButtonGrid::indexOf(SoundButton *b) const {
    for (int i = 0; i < m_buttons.size(); ++i) if (m_buttons[i] == b) return i;
    return -1;
}

void ButtonGrid::onButtonClicked() {
    int idx = indexOf(qobject_cast<SoundButton *>(sender()));
    if (idx >= 0) emit buttonTriggered(idx);
}

void ButtonGrid::onButtonContextMenu(const QPoint &local) {
    auto *b = qobject_cast<SoundButton *>(sender());
    int idx = indexOf(b);
    if (idx < 0 || !b) return;
    QPoint globalPos = b->mapToGlobal(local);
    showContextMenu(idx, globalPos);
    emit buttonRightClicked(idx, globalPos);
}

void ButtonGrid::showContextMenu(int idx, const QPoint &globalPos) {
    const SoundInfo info = (idx >= 0 && idx < m_sounds.size()) ? m_sounds[idx] : SoundInfo();
    bool empty   = info.filename.isEmpty() && !info.isMacro;
    bool isMacro = info.isMacro;

    QMenu menu(this);
    QAction *aChoose = nullptr;
    QAction *aEdit   = nullptr;
    QAction *aRename = nullptr;
    QAction *aMacro  = nullptr;
    QAction *aMicMacro = nullptr;

    if (isMacro) {
        // Macros are frozen: only rename or clear.
        aRename = menu.addAction(tr("Rename macro..."));
    } else {
        aChoose = menu.addAction(tr("Choose file..."));
        if (!empty)
            aEdit = menu.addAction(tr("Edit..."));
    }
    QAction *aSaveLink = nullptr;
    if (!isMacro)
        aSaveLink = menu.addAction(tr("Save link..."));
    auto *aClear  = menu.addAction(tr("Clear"));
    aClear->setEnabled(!empty || isMacro);
    auto *aHotkey = menu.addAction(tr("Set hotkey..."));
    if (!isMacro) {
        menu.addSeparator();
        aMacro = menu.addAction(tr("Freeze all channels into macro"));
        aMicMacro = menu.addAction(tr("Save Mic FX package into macro"));
    }

    // Per-channel YouTube actions: for EACH channel that currently holds a
    // resolved video, offer "save link" (+ "download audio" for non-live).
    // Two channels with videos -> two groups of actions, etc.
    QVector<StreamChannelInfo> streams;
    if (m_streamProvider && !isMacro) streams = m_streamProvider();
    struct StreamAction { QAction *save; QAction *download; QAction *playlist; StreamChannelInfo info; };
    QVector<StreamAction> streamActions;
    if (!streams.isEmpty()) {
        menu.addSeparator();
        for (const StreamChannelInfo &sc : streams) {
            StreamAction sa; sa.info = sc;
            sa.save = sa.download = sa.playlist = nullptr;
            const QString who = sc.channelName.isEmpty()
                ? tr("Channel %1").arg(sc.slot + 1) : sc.channelName;
            // A single video loaded -> offer save-link (+ download for VOD).
            if (!sc.pageUrl.isEmpty()) {
                sa.save = menu.addAction(tr("Save %1's link here").arg(who));
                sa.download = sc.isLive ? nullptr
                    : menu.addAction(tr("Download %1's audio here…").arg(who));
            }
            // A whole playlist loaded -> offer save-whole-playlist.
            if (sc.isPlaylist && !sc.playlistUrl.isEmpty())
                sa.playlist = menu.addAction(tr("Save %1's whole playlist here").arg(who));
            streamActions.push_back(sa);
        }
    }

    QAction *chosen = menu.exec(globalPos);
    if (chosen) {
        for (const StreamAction &sa : streamActions) {
            if (chosen == sa.save) {
                emit saveStreamLinkToButton(idx, sa.info.pageUrl, sa.info.title);
                return;
            }
            if (sa.download && chosen == sa.download) {
                emit downloadStreamToButton(idx, sa.info.pageUrl, sa.info.title);
                return;
            }
            if (sa.playlist && chosen == sa.playlist) {
                emit savePlaylistToButton(idx, sa.info.playlistUrl, sa.info.playlistTitle);
                return;
            }
        }
    }
    if      (chosen && chosen == aChoose) emit chooseFileRequested(idx);
    else if (chosen && chosen == aEdit)   emit editButtonRequested(idx);
    else if (chosen && chosen == aRename) emit renameMacroRequested(idx);
    else if (chosen && chosen == aSaveLink) emit saveLinkRequested(idx);
    else if (chosen && chosen == aClear)  emit clearButtonRequested(idx);
    else if (chosen && chosen == aHotkey) emit setHotkeyRequested(idx);
    else if (chosen && chosen == aMacro)  emit createMacroRequested(idx);
    else if (chosen && chosen == aMicMacro) emit createMicMacroRequested(idx);
}

void ButtonGrid::onButtonFileDropped(const QList<QUrl> &urls) {
    int idx = indexOf(qobject_cast<SoundButton *>(sender()));
    if (idx >= 0) emit buttonFileDropped(idx, urls);
}

void ButtonGrid::onButtonDroppedOnButton(SoundButton *target) {
    int from = indexOf(qobject_cast<SoundButton *>(sender()));
    int to   = indexOf(target);
    if (from >= 0 && to >= 0 && from != to) emit buttonReordered(from, to);
}
