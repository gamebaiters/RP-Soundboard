#include "channel_state_persistence.h"

#include <QSettings>
#include <QString>
#include <QJsonDocument>

// Forward-declare to avoid pulling the full TS3 SDK types in plugin.h
// into this module (it pulls uint64 etc. which break MOC compilation).
extern "C" const char *getTs3ConfigPath();

namespace ChannelStatePersistence {

static bool s_enabled = false;

QString filePath() {
    const char *cfg = getTs3ConfigPath();
    if (!cfg || !cfg[0]) return QString();
    return QString::fromUtf8(cfg) + "rp_soundboard_channels.ini";
}

bool isEnabled() { return s_enabled; }

void setEnabled(bool on) {
    s_enabled = on;
    QString path = filePath();
    if (path.isEmpty()) return;
    QSettings s(path, QSettings::IniFormat);
    s.setValue("meta/enabled", on);
}

bool loadState(int channelId, ChannelState &out) {
    if (!s_enabled) return false;
    QString path = filePath();
    if (path.isEmpty()) return false;
    QSettings s(path, QSettings::IniFormat);
    QString key = QString("channel_%1/state").arg(channelId);
    QByteArray blob = s.value(key).toByteArray();
    if (blob.isEmpty()) return false;
    return ChannelState::fromJson(blob, out);
}

bool saveState(int channelId, const ChannelState &state) {
    if (!s_enabled) return false;
    QString path = filePath();
    if (path.isEmpty()) return false;
    QSettings s(path, QSettings::IniFormat);
    QString key = QString("channel_%1/state").arg(channelId);
    s.setValue(key, state.toJson());
    return true;
}

void clearAll() {
    QString path = filePath();
    if (path.isEmpty()) return;
    QSettings s(path, QSettings::IniFormat);
    s.clear();
}

QString loadName(int channelId) {
    QString path = filePath();
    if (path.isEmpty()) return QString();
    QSettings s(path, QSettings::IniFormat);
    return s.value(QString("channel_%1/name").arg(channelId)).toString();
}

void saveName(int channelId, const QString &name) {
    QString path = filePath();
    if (path.isEmpty()) return;
    QSettings s(path, QSettings::IniFormat);
    s.setValue(QString("channel_%1/name").arg(channelId), name);
}

int loadChannelCount() {
    QString path = filePath();
    if (path.isEmpty()) return 1;
    QSettings s(path, QSettings::IniFormat);
    return qBound(1, s.value("meta/channel_count", 1).toInt(), 16);
}

void saveChannelCount(int count) {
    QString path = filePath();
    if (path.isEmpty()) return;
    QSettings s(path, QSettings::IniFormat);
    s.setValue("meta/channel_count", count);
}

}
