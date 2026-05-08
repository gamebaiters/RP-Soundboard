#pragma once

#include <QString>
#include <QVector>

class PresetManager {
public:
    struct Preset {
        QString name;
        QString data;
    };

    static QVector<Preset> loadEqPresets();
    static void saveEqPreset(const QString &name, const QString &data);
    static void deleteEqPreset(const QString &name);

    static QVector<Preset> loadSandboxPresets();
    static void saveSandboxPreset(const QString &name, const QString &data);
    static void deleteSandboxPreset(const QString &name);

private:
    static QString presetsFilePath();
    static QVector<Preset> loadSection(const QString &section);
    static void saveSection(const QString &section, const QVector<Preset> &presets);
};
