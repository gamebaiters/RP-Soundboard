#include "hotkey_block.h"

#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>

extern "C" const char *getTs3ConfigPath();

namespace {
QSet<int> &registry() {
    static QSet<int> s;
    return s;
}
QString filePath() {
    const char *cfg = getTs3ConfigPath();
    if (!cfg || !cfg[0]) return QString();
    return QString::fromUtf8(cfg) + "rp_soundboard_hotkey_block.ini";
}
}

namespace HotkeyBlock {

bool isBlocked(int idx) { return registry().contains(idx); }

void setBlocked(int idx, bool b) {
    if (b) registry().insert(idx);
    else   registry().remove(idx);
    save();
}

void blockAll(int total) {
    for (int i = 0; i < total; ++i) registry().insert(i);
    save();
}

void clearAll() {
    registry().clear();
    save();
}

void load() {
    QString path = filePath();
    if (path.isEmpty()) return;
    QSettings s(path, QSettings::IniFormat);
    QString csv = s.value("blocked", QString()).toString();
    registry().clear();
    if (csv.isEmpty()) return;
    for (const QString &part : csv.split(',', Qt::SkipEmptyParts)) {
        bool ok = false;
        int idx = part.trimmed().toInt(&ok);
        if (ok) registry().insert(idx);
    }
}

void save() {
    QString path = filePath();
    if (path.isEmpty()) return;
    QStringList parts;
    parts.reserve(registry().size());
    for (int idx : registry()) parts.append(QString::number(idx));
    QSettings s(path, QSettings::IniFormat);
    s.setValue("blocked", parts.join(','));
}

}
