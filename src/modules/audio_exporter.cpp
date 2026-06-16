#include "audio_exporter.h"
#include "audio_encoder_ffmpeg.h"
#include "../dsp/SlotDsp.h"
#include "../inputfile.h"
#include "../SampleProducer.h"

#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <vector>
#include <algorithm>
#include <memory>
#include <exception>
#include <mutex>
#include <set>

namespace {
// Registry of live exporters. Wiring code creates exporters with
// parent=nullptr (so the worker is not yanked when the soundboard
// window closes mid-export). sb_kill therefore has no Qt object tree
// to walk, and a zombie exporter blocked the plugin DLL from
// unloading — TS3.exe stayed in task manager. The registry gives
// sb_kill a single chokepoint to cancel + join every still-running
// exporter.
std::mutex            g_exportersMu;
std::set<AudioExporter*> g_exporters;
} // namespace

namespace {

class CollectProducer : public SampleProducer {
public:
    std::vector<short> buf;
    int channels = 2;
    void produce(const short *samples, int count) override {
        if (!samples || count <= 0) return;
        const size_t n = static_cast<size_t>(count) * static_cast<size_t>(channels);
        const size_t cur = buf.size();
        buf.resize(cur + n);
        std::memcpy(buf.data() + cur, samples, n * sizeof(short));
    }
};

} // namespace

AudioExporter::AudioExporter(const QString &inputFile, const QString &outputFile,
                             float pitchFactor, float speedFactor, float reverbMix,
                             const SandboxState &sandbox, bool sandboxEnabled,
                             double sampleRate, QObject *parent)
    : QThread(parent)
    , m_inputFile(inputFile)
    , m_outputFile(outputFile)
    , m_pitch(pitchFactor > 0.0f ? pitchFactor : 1.0f)
    , m_speed(speedFactor > 0.0f ? speedFactor : 1.0f)
    , m_reverb(reverbMix < 0.0f ? 0.0f : (reverbMix > 1.0f ? 1.0f : reverbMix))
    , m_sandbox(sandbox)
    , m_sandboxEnabled(sandboxEnabled)
    , m_sampleRate(sampleRate > 0.0 ? sampleRate : 48000.0)
{
    std::lock_guard<std::mutex> lg(g_exportersMu);
    g_exporters.insert(this);
}

AudioExporter::~AudioExporter()
{
    {
        std::lock_guard<std::mutex> lg(g_exportersMu);
        g_exporters.erase(this);
    }
    // Belt-and-braces: a QThread dying with run() still active would
    // call std::terminate via ~QThread. The wiring already chains
    // finished -> deleteLater so we normally arrive here after run()
    // returned, but a stray code path could still get us here mid-run.
    if (isRunning()) {
        requestInterruption();
        if (!wait(500)) {
            terminate();
            wait(100);
        }
    }
}

void AudioExporter::cancelAllAndWait(int waitMsPerThread)
{
    // Snapshot the registry: the exporters' own finished -> deleteLater
    // chain may erase entries while we wait, and walking the live set
    // would invalidate iterators.
    std::vector<AudioExporter*> snap;
    {
        std::lock_guard<std::mutex> lg(g_exportersMu);
        snap.assign(g_exporters.begin(), g_exporters.end());
    }
    for (AudioExporter *e : snap) {
        if (e) e->requestInterruption();
    }
    for (AudioExporter *e : snap) {
        if (!e) continue;
        if (e->isRunning()) e->wait(waitMsPerThread);
    }
}

void AudioExporter::run() {
    try {
        if (m_inputFile.isEmpty()) {
            emit exportFinished(false, tr("No input file specified"));
            return;
        }
        if (m_outputFile.isEmpty()) {
            emit exportFinished(false, tr("No output file specified"));
            return;
        }
        if (!QFileInfo::exists(m_inputFile)) {
            emit exportFinished(false, tr("Input file not found: %1").arg(m_inputFile));
            return;
        }

        // Detect the output format from the filename. Unknown extensions
        // fall back to WAV so the user always gets a valid file even if
        // they mistyped the suffix.
        AudioEncoderFFmpeg::Format fmt = AudioEncoderFFmpeg::FmtWav;
        AudioEncoderFFmpeg::formatFromExtension(m_outputFile, fmt);

        InputFileOptions opts;
        opts.outputChannelLayout = InputFileOptions::STEREO;
        opts.outputSampleRate = static_cast<int>(m_sampleRate);

        std::unique_ptr<InputFile> decoder(CreateInputFileFFmpeg(opts));
        if (!decoder) {
            emit exportFinished(false, tr("Failed to create decoder"));
            return;
        }
        if (decoder->open(m_inputFile.toUtf8().constData()) != 0) {
            emit exportFinished(false, tr("Failed to open input file"));
            return;
        }

        if (m_pitch != 1.0f) decoder->setPitchFactor(m_pitch);
        if (m_speed != 1.0f) decoder->setSpeedFactor(m_speed);
        if (m_reverb > 0.0f) decoder->setReverbMix(m_reverb);

        const int sampleRate = opts.outputSampleRate;
        int64_t totalFrames = decoder->outputSamplesEstimation();
        if (totalFrames <= 0) totalFrames = static_cast<int64_t>(sampleRate) * 600;

        std::unique_ptr<SlotDsp> dsp(new SlotDsp());
        dsp->setSampleRate(sampleRate);
        SandboxState eff = m_sandbox;
        eff.enabled = m_sandboxEnabled && m_sandbox.enabled;
        dsp->applyState(eff);

        // FFmpeg-backed encoder. Replaces the previous hand-rolled WAV
        // writer so the export path can produce WAV / FLAC / OGG /
        // AAC-M4A from one code path.
        AudioEncoderFFmpeg encoder;
        if (!encoder.open(m_outputFile, fmt, sampleRate, 2)) {
            emit exportFinished(false,
                tr("Encoder init failed (%1): %2")
                    .arg(QString::fromUtf8(AudioEncoderFFmpeg::displayName(fmt)),
                         encoder.lastError()));
            return;
        }

        CollectProducer collector;
        collector.channels = 2;
        int64_t framesWritten = 0;
        int lastPercent = -1;

        const bool useStretch = eff.enabled && eff.stretchEnabled;

        if (useStretch) {
            const float stretchFactor = std::max(1.0f, eff.stretchFactor);
            int64_t targetOutputFrames = static_cast<int64_t>(static_cast<double>(totalFrames) * stretchFactor);
            if (targetOutputFrames <= 0) targetOutputFrames = totalFrames;

            const int outBlock = 4096;
            std::vector<short> stretchOut(static_cast<size_t>(outBlock) * 2);
            std::vector<short> ring;
            size_t ringPos = 0;
            bool sourceDrained = false;

            while (!isInterruptionRequested() && framesWritten < targetOutputFrames) {
                const size_t topUpTarget = static_cast<size_t>(outBlock) * 4;
                while (!sourceDrained && (ring.size() - ringPos) / 2 < topUpTarget) {
                    if (decoder->done()) { sourceDrained = true; break; }
                    collector.buf.clear();
                    int ret = decoder->readSamples(&collector);
                    if (ret < 0 && collector.buf.empty()) { sourceDrained = true; break; }
                    if (ret == 0 && collector.buf.empty()) { sourceDrained = true; break; }
                    ring.insert(ring.end(), collector.buf.begin(), collector.buf.end());
                    if (ring.size() - ringPos > topUpTarget * 2) break;
                }

                int sourceFramesAvail = static_cast<int>((ring.size() - ringPos) / 2);
                int needIn = dsp->inputFramesNeededFor(outBlock);
                if (needIn > sourceFramesAvail) needIn = sourceFramesAvail;

                if (needIn > 0) {
                    dsp->feedStretchShort(ring.data() + ringPos, needIn, /*isCapture=*/false);
                    ringPos += static_cast<size_t>(needIn) * 2;
                    if (ringPos > 1 << 20) {
                        ring.erase(ring.begin(), ring.begin() + ringPos);
                        ringPos = 0;
                    }
                } else if (sourceDrained) {
                    break;
                }

                float pL = 0.0f, pR = 0.0f;
                dsp->produceStretchedShort(stretchOut.data(), outBlock, 2, pL, pR, /*isCapture=*/false);

                if (!encoder.write(stretchOut.data(), outBlock)) {
                    encoder.close();
                    QFile::remove(m_outputFile);
                    emit exportFinished(false,
                        tr("Encoder write failed: %1").arg(encoder.lastError()));
                    return;
                }
                framesWritten += outBlock;

                int pct = static_cast<int>(framesWritten * 100 / std::max<int64_t>(1, targetOutputFrames));
                if (pct != lastPercent) {
                    lastPercent = pct;
                    emit progress(std::min(pct, 100));
                }
            }
        } else {
            while (!isInterruptionRequested() && !decoder->done()) {
                collector.buf.clear();
                int ret = decoder->readSamples(&collector);
                if (ret < 0 && collector.buf.empty()) break;
                if (ret == 0 && collector.buf.empty()) {
                    continue;
                }

                const int frames = static_cast<int>(collector.buf.size()) / 2;
                if (frames <= 0) continue;

                float pL = 0.0f, pR = 0.0f;
                dsp->process(collector.buf.data(), frames, 2, pL, pR, false);

                if (!encoder.write(collector.buf.data(), frames)) {
                    encoder.close();
                    QFile::remove(m_outputFile);
                    emit exportFinished(false,
                        tr("Encoder write failed: %1").arg(encoder.lastError()));
                    return;
                }
                framesWritten += frames;

                int pct = static_cast<int>(framesWritten * 100 / std::max<int64_t>(1, totalFrames));
                if (pct != lastPercent) {
                    lastPercent = pct;
                    emit progress(std::min(pct, 100));
                }
            }
        }

        if (!encoder.close()) {
            QFile::remove(m_outputFile);
            emit exportFinished(false,
                tr("Encoder close failed: %1").arg(encoder.lastError()));
            return;
        }

        if (isInterruptionRequested()) {
            QFile::remove(m_outputFile);
            emit exportFinished(false, tr("Export cancelled"));
        } else if (framesWritten <= 0) {
            QFile::remove(m_outputFile);
            emit exportFinished(false, tr("No audio data produced"));
        } else {
            emit progress(100);
            emit exportFinished(true, QString());
        }
    } catch (const std::exception &e) {
        emit exportFinished(false, tr("Export failed: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        emit exportFinished(false, tr("Export failed: unknown error"));
    }
}
