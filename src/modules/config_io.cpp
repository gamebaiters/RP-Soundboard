#include "config_io.h"
#include "../ConfigModel.h"
#include "../common.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>
#include <QCoreApplication>

namespace ConfigIO {

static QString tempIniPath() {
    QString base = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (base.isEmpty()) base = QDir::tempPath();
    return base + "/rpsb_io_" + QUuid::createUuid().toString(QUuid::Id128) + ".ini";
}

QByteArray serialize(const ConfigModel &model) {
    // Round-trip through ConfigModel::writeConfig (INI) so we preserve every
    // field the existing model knows about, including the new isMacro /
    // macroState additions handled by SoundInfo.
    QString iniPath = tempIniPath();
    const_cast<ConfigModel &>(model).writeConfig(iniPath);
    QByteArray ini;
    {
        QFile f(iniPath);
        if (f.open(QIODevice::ReadOnly)) ini = f.readAll();
    }
    QFile::remove(iniPath);

    QJsonObject env;
    env["schema"]  = kSchemaName;
    env["version"] = kSchemaVersion;
    env["ini"]     = QString::fromUtf8(ini.toBase64());
    return QJsonDocument(env).toJson(QJsonDocument::Indented);
}

bool exportToFile(const QString &path, const ConfigModel &model) {
    QByteArray data = serialize(model);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(data) == data.size();
}

ImportResult deserialize(const QByteArray &data, ConfigModel &model) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return ImportResult::ParseError;

    auto env = doc.object();
    if (env.value("schema").toString() != kSchemaName)
        return ImportResult::SchemaError;
    int v = env.value("version").toInt(-1);
    if (v != kSchemaVersion)
        return ImportResult::VersionMismatch;

    QByteArray b64 = env.value("ini").toString().toUtf8();
    if (b64.isEmpty()) return ImportResult::SchemaError;
    QByteArray ini = QByteArray::fromBase64(b64);
    if (ini.isEmpty()) return ImportResult::SchemaError;

    QString iniPath = tempIniPath();
    {
        QFile f(iniPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return ImportResult::FileError;
        if (f.write(ini) != ini.size()) {
            QFile::remove(iniPath);
            return ImportResult::FileError;
        }
    }
    model.readConfig(iniPath);
    QFile::remove(iniPath);
    return ImportResult::Ok;
}

ImportResult importFromFile(const QString &path, ConfigModel &model) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return ImportResult::FileError;
    QByteArray data = f.readAll();
    if (data.isEmpty()) return ImportResult::FileError;
    return deserialize(data, model);
}

bool exportIniToFile(const QString &path, const ConfigModel &model) {
    const_cast<ConfigModel &>(model).writeConfig(path);
    return QFile::exists(path);
}

ImportResult importIniFromFile(const QString &path, ConfigModel &model) {
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return ImportResult::FileError;
    f.close();
    model.readConfig(path);
    return ImportResult::Ok;
}

bool exportProfileIni(const QString &path, ConfigModel &model, int configIdx) {
    if (configIdx < 0 || configIdx >= NUM_CONFIGS) return false;
    // Round-trip via a full INI dump on a temp file, then rewrite only
    // the keys belonging to the chosen profile. Keeps the legacy layout
    // (files / files2..N + num_rows / num_rows2..N) so old builds can
    // open it.
    QString tmp = tempIniPath();
    model.writeConfig(tmp);
    QSettings src(tmp, QSettings::IniFormat);
    QSettings dst(path, QSettings::IniFormat);
    dst.clear();

    QString filesKey = (configIdx == 0) ? QStringLiteral("files")
                                        : QStringLiteral("files%1").arg(configIdx + 1);
    QString rowsKey  = (configIdx == 0) ? QStringLiteral("num_rows")
                                        : QStringLiteral("num_rows%1").arg(configIdx + 1);
    QString colsKey  = (configIdx == 0) ? QStringLiteral("num_cols")
                                        : QStringLiteral("num_cols%1").arg(configIdx + 1);

    src.beginGroup(filesKey);
    for (const QString &k : src.childKeys()) dst.setValue(filesKey + "/" + k, src.value(k));
    for (const QString &g : src.childGroups()) {
        src.beginGroup(g);
        for (const QString &k : src.childKeys())
            dst.setValue(filesKey + "/" + g + "/" + k, src.value(k));
        src.endGroup();
    }
    src.endGroup();
    dst.setValue(rowsKey, src.value(rowsKey));
    dst.setValue(colsKey, src.value(colsKey));
    dst.sync();
    QFile::remove(tmp);
    return true;
}

ImportResult importProfileIni(const QString &path, ConfigModel &model, int configIdx) {
    if (configIdx < 0 || configIdx >= NUM_CONFIGS) return ImportResult::SchemaError;
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return ImportResult::FileError;
    f.close();

    // Strategy: load the source INI into a temporary ConfigModel-friendly
    // file by remapping the source's "files" section into the chosen
    // profile's section in our active config file, then re-readConfig.
    QSettings src(path, QSettings::IniFormat);
    QString tmp = tempIniPath();
    {
        // Start from the model's CURRENT state so we keep the other 3
        // profiles intact when we reload.
        model.writeConfig(tmp);
    }
    QSettings dst(tmp, QSettings::IniFormat);

    QString srcFilesKey = src.contains("files/0/file_name") || src.childGroups().contains("files")
                          ? QStringLiteral("files") : QString();
    if (srcFilesKey.isEmpty()) {
        for (const QString &g : src.childGroups()) {
            if (g.startsWith("files")) { srcFilesKey = g; break; }
        }
    }
    if (srcFilesKey.isEmpty()) { QFile::remove(tmp); return ImportResult::SchemaError; }

    QString dstFilesKey = (configIdx == 0) ? QStringLiteral("files")
                                            : QStringLiteral("files%1").arg(configIdx + 1);
    QString dstRowsKey  = (configIdx == 0) ? QStringLiteral("num_rows")
                                            : QStringLiteral("num_rows%1").arg(configIdx + 1);
    QString dstColsKey  = (configIdx == 0) ? QStringLiteral("num_cols")
                                            : QStringLiteral("num_cols%1").arg(configIdx + 1);

    // Wipe the destination profile, then copy from src.
    dst.beginGroup(dstFilesKey);
    dst.remove("");
    dst.endGroup();
    src.beginGroup(srcFilesKey);
    for (const QString &k : src.childKeys()) dst.setValue(dstFilesKey + "/" + k, src.value(k));
    for (const QString &g : src.childGroups()) {
        src.beginGroup(g);
        for (const QString &k : src.childKeys())
            dst.setValue(dstFilesKey + "/" + g + "/" + k, src.value(k));
        src.endGroup();
    }
    src.endGroup();
    QVariant rv = src.value("num_rows", src.value("num_rows1"));
    QVariant cv = src.value("num_cols", src.value("num_cols1"));
    if (rv.isValid()) dst.setValue(dstRowsKey, rv);
    if (cv.isValid()) dst.setValue(dstColsKey, cv);
    dst.sync();

    model.readConfig(tmp);
    QFile::remove(tmp);
    return ImportResult::Ok;
}

QString humanError(ImportResult r) {
    switch (r) {
        case ImportResult::Ok:              return QObject::tr("Imported successfully");
        case ImportResult::FileError:       return QObject::tr("Could not read the file");
        case ImportResult::ParseError:      return QObject::tr("File is not valid JSON");
        case ImportResult::SchemaError:     return QObject::tr("File is not a valid soundboard configuration");
        case ImportResult::VersionMismatch: return QObject::tr("Unsupported configuration version");
    }
    return QObject::tr("Unknown error");
}

}
