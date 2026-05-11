#include "audio_exporter.h"
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

bool writeWavHeader(QFile &f, int sampleRate, int64_t framesWritten) {
    // Classic RIFF/WAVE has a 4 GiB size cap baked into its int32 size
    // fields. A long Paulstretch export on a several-minute source can
    // exceed that. Clamp + bail so we don't silently truncate to a
    // corrupt file.
    const int64_t bytes = framesWritten * 2 * static_cast<int64_t>(sizeof(short));
    if (bytes <= 0 || bytes > static_cast<int64_t>(0x7FFFFFFF) - 36)
        return false;
    int32_t dataSize = static_cast<int32_t>(bytes);
    int32_t fileSize = dataSize + 36;
    if (!f.seek(0)) return false;
    if (f.write("RIFF", 4) != 4) return false;
    if (f.write(reinterpret_cast<const char*>(&fileSize), 4) != 4) return false;
    if (f.write("WAVE", 4) != 4) return false;
    if (f.write("fmt ", 4) != 4) return false;
    int32_t fmtSize = 16;
    if (f.write(reinterpret_cast<const char*>(&fmtSize), 4) != 4) return false;
    int16_t audioFmt = 1;
    f.write(reinterpret_cast<const char*>(&audioFmt), 2);
    int16_t numCh = 2;
    f.write(reinterpret_cast<const char*>(&numCh), 2);
    int32_t sr = sampleRate;
    f.write(reinterpret_cast<const char*>(&sr), 4);
    int32_t byteRate = sampleRate * 2 * 2;
    f.write(reinterpret_cast<const char*>(&byteRate), 4);
    int16_t blockAlign = 4;
    f.write(reinterpret_cast<const char*>(&blockAlign), 2);
    int16_t bitsPerSample = 16;
    f.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    f.write("data", 4);
    f.write(reinterpret_cast<const char*>(&dataSize), 4);
    return true;
}

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
}

void AudioExporter::run() {
    // The whole pipeline goes under a try/catch so any DSP / FFmpeg
    // exception turns into a "failed" report instead of crashing the
    // host process (TS3). Defensive checks first - empty paths or a
    // missing source file would otherwise sail through to FFmpeg and
    // segfault deep in libavformat.
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

        // FxPanel effects baked in through the decoder's filter graph:
        // pitch + speed go through abuffer rate lie + aresample +
        // atempo, reverb goes through the freeverb stage at end of
        // decoder. Same path the live sampler slot uses, so the WAV
        // matches what the user heard.
        if (m_pitch != 1.0f) decoder->setPitchFactor(m_pitch);
        if (m_speed != 1.0f) decoder->setSpeedFactor(m_speed);
        if (m_reverb > 0.0f) decoder->setReverbMix(m_reverb);

        const int sampleRate = opts.outputSampleRate;
        int64_t totalFrames = decoder->outputSamplesEstimation();
        if (totalFrames <= 0) totalFrames = static_cast<int64_t>(sampleRate) * 600;

        // SlotDsp aggregates ~12 DSP modules with their own buffers.
        // Heap-allocate to keep the QThread stack small.
        std::unique_ptr<SlotDsp> dsp(new SlotDsp());
        dsp->setSampleRate(sampleRate);
        // Force the master enable bit to mirror the channel's checkbox
        // so the user's "sandbox off" state truly bypasses every effect
        // module regardless of the per-effect enables saved in state.
        SandboxState eff = m_sandbox;
        eff.enabled = m_sandboxEnabled && m_sandbox.enabled;
        dsp->applyState(eff);

        QFile outFile(m_outputFile);
        if (!outFile.open(QIODevice::WriteOnly)) {
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

        const bool useStretch = eff.enabled && eff.stretchEnabled;

        if (useStretch) {
            // Paulstretch path. SlotDsp uses a separate feed/produce
            // protocol for stretch (vs. in-place process() for every
            // other effect). The export must mirror what the sampler
            // does at runtime so the WAV ends up time-stretched with
            // the user's full DSP chain on top.
            //
            // For each output block we ask the dsp how many source
            // frames it needs (= ceil(outBlock / factor)), feed that
            // many from the decoded ring, then pull a full output
            // block. Loop until we've produced ~sourceFrames * factor
            // of output OR the decoder runs dry.
            const float stretchFactor = std::max(1.0f, eff.stretchFactor);
            int64_t targetOutputFrames = static_cast<int64_t>(static_cast<double>(totalFrames) * stretchFactor);
            if (targetOutputFrames <= 0) targetOutputFrames = totalFrames;

            const int outBlock = 4096;
            std::vector<short> stretchOut(static_cast<size_t>(outBlock) * 2);
            std::vector<short> ring;                  // append-only decoded source
            size_t ringPos = 0;                       // read head in shorts
            bool sourceDrained = false;

            while (!isInterruptionRequested() && framesWritten < targetOutputFrames) {
                // Top the ring up so the stretch always has enough
                // source for one full input window plus the block it's
                // about to ask for. The threshold is generous to avoid
                // starvation glitches that show up as silence chunks
                // every few seconds.
                const size_t shortsAvail = ring.size() - ringPos;
                const size_t topUpTarget = static_cast<size_t>(outBlock) * 4;
                while (!sourceDrained && shortsAvail / 2 < topUpTarget) {
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
                    // Reclaim memory periodically so long files don't
                    // blow the heap on huge stretch factors.
                    if (ringPos > 1 << 20) {
                        ring.erase(ring.begin(), ring.begin() + ringPos);
                        ringPos = 0;
                    }
                } else if (sourceDrained) {
                    break;
                }

                float pL = 0.0f, pR = 0.0f;
                dsp->produceStretchedShort(stretchOut.data(), outBlock, 2, pL, pR, /*isCapture=*/false);

                qint64 wrote = outFile.write(
                    reinterpret_cast<const char*>(stretchOut.data()),
                    static_cast<qint64>(outBlock) * 2 * sizeof(short));
                if (wrote <= 0) {
                    outFile.close();
                    QFile::remove(m_outputFile);
                    emit exportFinished(false, tr("Disk write failed"));
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
            // Non-stretch path: standard in-place DSP block transform.
            while (!isInterruptionRequested() && !decoder->done()) {
                collector.buf.clear();
                int ret = decoder->readSamples(&collector);
                if (ret < 0 && collector.buf.empty()) break;
                if (ret == 0 && collector.buf.empty()) {
                    // No bytes available this tick; keep polling unless the
                    // decoder reports done() next iteration.
                    continue;
                }

                const int frames = static_cast<int>(collector.buf.size()) / 2;
                if (frames <= 0) continue;

                float pL = 0.0f, pR = 0.0f;
                dsp->process(collector.buf.data(), frames, 2, pL, pR, false);

                qint64 wrote = outFile.write(
                    reinterpret_cast<const char*>(collector.buf.data()),
                    static_cast<qint64>(frames) * 2 * sizeof(short));
                if (wrote <= 0) {
                    outFile.close();
                    QFile::remove(m_outputFile);
                    emit exportFinished(false, tr("Disk write failed"));
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

        if (!writeWavHeader(outFile, sampleRate, framesWritten)) {
            outFile.close();
            emit exportFinished(false, tr("Failed to write WAV header"));
            return;
        }
        outFile.close();

        if (isInterruptionRequested()) {
            QFile::remove(m_outputFile);
            emit exportFinished(false, tr("Export cancelled"));
        } else if (framesWritten <= 0) {
            QFile::remove(m_outputFile);
            emit exportFinished(false, tr("No audio data produced"));
        } else {
            emit exportFinished(true, QString());
        }
    } catch (const std::exception &e) {
        emit exportFinished(false, tr("Export failed: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        emit exportFinished(false, tr("Export failed: unknown error"));
    }
}
