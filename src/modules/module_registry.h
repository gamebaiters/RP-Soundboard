// Module registry - single source of truth for the modular UI system.
// Lists every module that participates in the rebuilt soundboard UI.
// Adding a new module = one entry here + new files under src/modules/.

#pragma once

#include <QString>
#include <QVector>

struct ModuleInfo {
    QString id;          // unique stable id, used for state lookups
    QString file;        // relative path inside src/
    QString summary;     // one-line description
};

namespace ModuleRegistry {
    const QVector<ModuleInfo> &all();
    const ModuleInfo *find(const QString &id);
}
