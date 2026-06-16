#pragma once

#include <QThread>
#include <QString>
#include "../dsp/SandboxState.h"

class AudioExporter : public QThread {
    Q_OBJECT
public:
    // pitchFactor, speedFactor, reverbMix mirror the channel's FxPanel
    // sliders at click time (factor = 3^(slider/100)) and are applied
    // by the decoder filter graph. sandbox is applied AFTER the decoder,
    // honouring SandboxState::pipelineOrder so the exported WAV matches
    // exactly what comes out of the sampler slot.
    AudioExporter(const QString &inputFile, const QString &outputFile,
                  float pitchFactor, float speedFactor, float reverbMix,
                  const SandboxState &sandbox, bool sandboxEnabled,
                  double sampleRate = 48000.0,
                  QObject *parent = nullptr);
    ~AudioExporter() override;
    void run() override;

    // sb_kill calls this once during plugin teardown: every still-running
    // exporter gets requestInterruption() + a brief join window. Without
    // this, a parentless exporter would keep its QThread alive after the
    // plugin DLL unloaded, leaving TS3.exe as a zombie in task manager.
    static void cancelAllAndWait(int waitMsPerThread = 250);

signals:
    void progress(int percent);
    void exportFinished(bool success, const QString &error);

private:
    QString      m_inputFile;
    QString      m_outputFile;
    float        m_pitch;
    float        m_speed;
    float        m_reverb;
    SandboxState m_sandbox;
    bool         m_sandboxEnabled;
    double       m_sampleRate;
};
