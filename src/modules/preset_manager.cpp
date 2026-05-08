#include "preset_manager.h"
#include <QSettings>
#include <QFileInfo>
#include <QDir>

static QString getConfigDir() {
    QString appData = QDir::homePath();
#ifdef Q_OS_WIN
    appData = QDir::toNativeSeparators(
        QDir::homePath() + "/AppData/Roaming/TS3Client");
#elif defined(Q_OS_MACOS)
    appData = QDir::homePath() + "/Library/Application Support/TeamSpeak 3/";
#else
    appData = QDir::homePath() + "/.ts3client";
#endif
    return appData;
}

QString PresetManager::presetsFilePath() {
    return getConfigDir() + QDir::separator() + "rp_soundboard_presets.ini";
}

QVector<PresetManager::Preset> PresetManager::loadSection(const QString &section) {
    QVector<Preset> result;
    QSettings s(presetsFilePath(), QSettings::IniFormat);
    s.beginGroup(section);
    int count = s.value("count", 0).toInt();
    for (int i = 0; i < count; ++i) {
        Preset p;
        p.name = s.value(QString("preset_%1_name").arg(i)).toString();
        p.data = s.value(QString("preset_%1_data").arg(i)).toString();
        if (!p.name.isEmpty())
            result.append(p);
    }
    s.endGroup();
    return result;
}

void PresetManager::saveSection(const QString &section, const QVector<Preset> &presets) {
    QSettings s(presetsFilePath(), QSettings::IniFormat);
    s.beginGroup(section);
    s.remove("");
    s.setValue("count", presets.size());
    for (int i = 0; i < presets.size(); ++i) {
        s.setValue(QString("preset_%1_name").arg(i), presets[i].name);
        s.setValue(QString("preset_%1_data").arg(i), presets[i].data);
    }
    s.endGroup();
    s.sync();
}

QVector<PresetManager::Preset> PresetManager::loadEqPresets() {
    return loadSection("eq_presets");
}

void PresetManager::saveEqPreset(const QString &name, const QString &data) {
    auto presets = loadEqPresets();
    bool found = false;
    for (auto &p : presets) {
        if (p.name == name) { p.data = data; found = true; break; }
    }
    if (!found) presets.append({name, data});
    saveSection("eq_presets", presets);
}

void PresetManager::deleteEqPreset(const QString &name) {
    auto presets = loadEqPresets();
    for (int i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name) { presets.removeAt(i); break; }
    }
    saveSection("eq_presets", presets);
}

QVector<PresetManager::Preset> PresetManager::loadSandboxPresets() {
    return loadSection("sandbox_presets");
}

void PresetManager::saveSandboxPreset(const QString &name, const QString &data) {
    auto presets = loadSandboxPresets();
    bool found = false;
    for (auto &p : presets) {
        if (p.name == name) { p.data = data; found = true; break; }
    }
    if (!found) presets.append({name, data});
    saveSection("sandbox_presets", presets);
}

void PresetManager::deleteSandboxPreset(const QString &name) {
    auto presets = loadSandboxPresets();
    for (int i = 0; i < presets.size(); ++i) {
        if (presets[i].name == name) { presets.removeAt(i); break; }
    }
    saveSection("sandbox_presets", presets);
}
