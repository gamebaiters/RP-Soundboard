#pragma once

#include <QThread>
#include <QString>
#include "../dsp/SandboxState.h"

class AudioExporter : public QThread {
    Q_OBJECT
public:
    AudioExporter(const QString &inputFile, const QString &outputFile,
                  const SandboxState &sandbox, double sampleRate = 48000.0,
                  QObject *parent = nullptr);
    void run() override;

signals:
    void progress(int percent);
    void exportFinished(bool success, const QString &error);

private:
    QString m_inputFile;
    QString m_outputFile;
    SandboxState m_sandbox;
    double m_sampleRate;
};
