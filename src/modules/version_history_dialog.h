// src/modules/version_history_dialog.h
//----------------------------------
// GameBaiters Soundboard
// Copyright (c) 2026 GameBaitersCrew
//----------------------------------
//
// Browseable version-history dialog. Reads the embedded
// release-notes.txt resource, splits it into per-version sections via
// the "GameBaiters Soundboard vX.Y.Z" header pattern, and renders
// every documented version as a collapsible/selectable list with the
// notes shown next to it. Opened from the About dialog.

#pragma once

#include <QDialog>
#include <QVector>
#include <QString>

class QListWidget;
class QListWidgetItem;
class QTextBrowser;
class QPushButton;

class VersionHistoryDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VersionHistoryDialog(QWidget *parent = nullptr);
    ~VersionHistoryDialog() override = default;

private slots:
    void onVersionSelected(QListWidgetItem *current, QListWidgetItem *previous);

private:
    struct VersionEntry {
        QString version;   // e.g. "2.2.9"
        QString body;      // already-trimmed notes for this version
    };

    void loadHistory();
    void renderBody(const VersionEntry &v);

    QListWidget   *m_list   = nullptr;
    QTextBrowser  *m_body   = nullptr;
    QPushButton   *m_close  = nullptr;
    QVector<VersionEntry> m_entries;
};
