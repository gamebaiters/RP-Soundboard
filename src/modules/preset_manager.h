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

    // Mic FX voice presets (mic sandbox state + live pitch), same file, own
    // section. Used by the Microphone channel's Save/Delete/Share controls.
    static QVector<Preset> loadMicPresets();
    static void saveMicPreset(const QString &name, const QString &data);
    static void deleteMicPreset(const QString &name);

private:
    static QString presetsFilePath();
    static QVector<Preset> loadSection(const QString &section);
    static void saveSection(const QString &section, const QVector<Preset> &presets);
};
