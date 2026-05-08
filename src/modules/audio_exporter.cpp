#include "audio_exporter.h"
#include "../dsp/SlotDsp.h"
#include "../inputfile.h"
#include "../SampleProducer.h"

#include <QFile>
#include <cstring>
#include <vector>
#include <algorithm>

class CollectProducer : public SampleProducer {
public:
    std::vector<short> buf;
    int channels = 2;
    void produce(const short *samples, int count) override {
        buf.insert(buf.end(), samples, samples + count * channels);
    }
};

AudioExporter::AudioExporter(const QString &inputFile, const QString &outputFile,
                             const SandboxState &sandbox, double sampleRate,
                             QObject *parent)
    : QThread(parent)
    , m_inputFile(inputFile)
    , m_outputFile(outputFile)
    , m_sandbox(sandbox)
    , m_sampleRate(sampleRate)
{
}

void AudioExporter::run() {
    InputFileOptions opts;
    opts.outputChannelLayout = InputFileOptions::STEREO;
    opts.outputSampleRate = static_cast<int>(m_sampleRate);

    InputFile *decoder = CreateInputFileFFmpeg(opts);
    if (!decoder) {
        emit exportFinished(false, tr("Failed to create decoder"));
        return;
    }
    if (decoder->open(m_inputFile.toUtf8().constData()) != 0) {
        delete decoder;
        emit exportFinished(false, tr("Failed to open input file"));
        return;
    }

    int sampleRate = opts.outputSampleRate;
    int64_t totalFrames = decoder->outputSamplesEstimation();
    if (totalFrames <= 0) totalFrames = sampleRate * 600;

    SlotDsp dsp;
    dsp.setSampleRate(sampleRate);
    dsp.applyState(m_sandbox);

    QFile outFile(m_outputFile);
    if (!outFile.open(QIODevice::WriteOnly)) {
        delete decoder;
        emit exportFinished(false, tr("Failed to create output file"));
        return;
    }

    char header[44];
    std::memset(header, 0, 44);
    outFile.write(header, 44);

    CollectProducer collector;
    collector.channels = 2;
    int64_t framesWritten = 0;
    int lastPercent = -1;

    while (!isInterruptionRequested() && !decoder->done()) {
        collector.buf.clear();
        int ret = decoder->readSamples(&collector);
        if (ret <= 0 && collector.buf.empty()) break;

        int frames = static_cast<int>(collector.buf.size()) / 2;
        if (frames <= 0) continue;

        float pL = 0, pR = 0;
        dsp.process(collector.buf.data(), frames, 2, pL, pR, false);
        outFile.write(reinterpret_cast<const char*>(collector.buf.data()),
                      frames * 2 * sizeof(short));

        framesWritten += frames;
        int pct = static_cast<int>(framesWritten * 100 / totalFrames);
        if (pct != lastPercent) {
            lastPercent = pct;
            emit progress(std::min(pct, 100));
        }
    }

    int32_t dataSize = static_cast<int32_t>(framesWritten * 2 * sizeof(short));
    int32_t fileSize = dataSize + 36;
    outFile.seek(0);
    outFile.write("RIFF", 4);
    outFile.write(reinterpret_cast<const char*>(&fileSize), 4);
    outFile.write("WAVE", 4);
    outFile.write("fmt ", 4);
    int32_t fmtSize = 16;
    outFile.write(reinterpret_cast<const char*>(&fmtSize), 4);
    int16_t audioFmt = 1;
    outFile.write(reinterpret_cast<const char*>(&audioFmt), 2);
    int16_t numCh = 2;
    outFile.write(reinterpret_cast<const char*>(&numCh), 2);
    int32_t sr = sampleRate;
    outFile.write(reinterpret_cast<const char*>(&sr), 4);
    int32_t byteRate = sampleRate * 2 * 2;
    outFile.write(reinterpret_cast<const char*>(&byteRate), 4);
    int16_t blockAlign = 4;
    outFile.write(reinterpret_cast<const char*>(&blockAlign), 2);
    int16_t bitsPerSample = 16;
    outFile.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    outFile.write("data", 4);
    outFile.write(reinterpret_cast<const char*>(&dataSize), 4);
    outFile.close();

    delete decoder;

    if (isInterruptionRequested()) {
        QFile::remove(m_outputFile);
        emit exportFinished(false, tr("Export cancelled"));
    } else {
        emit exportFinished(true, QString());
    }
}
