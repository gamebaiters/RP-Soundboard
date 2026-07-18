#include "MicAmbience.h"
#include "inputfile.h"
#include "SampleProducer.h"

#include <QCoreApplication>
#include <QFile>
#include <cmath>
#include <cstring>
#include <algorithm>

// Real CC0 field recordings (freesound.org previews, Creative Commons
// Zero - no attribution required), embedded as 32 kHz mono 16-bit WAVs
// in the plugin resources (:/ambience/ambNN.wav). Earlier attempts to
// SYNTHESIZE these textures procedurally were rejected by the user -
// no amount of DSP recipe tuning made a fake washing machine read as a
// washing machine, so the loops are recordings now.
//
// Source sound ids (freesound): 704535 washing machine, 741343
// microwave, 340656 pillar drill, 537621 hair dryer, 159348 vacuum,
// 321885 rain, 210220 wind, 223093 city, 706352 bar chatter, 753538
// frying bacon, 35291 tv static, 511509 jackhammer, 501172 angle
// grinder, 204946 hammering, 491405 crowd in panic, 325808 engine loop.
// (A second batch - baby/mosquito/dogs/snoring/car alarm, ids 16-20 -
// was tried twice and removed on user feedback: quality too low, and
// the wavs cost ~2 MB. Ids stay append-only: any NEW ambience continues
// from 16.)
//
// Loading: parse the RIFF chunks, convert s16 -> float, linear-resample
// to the mixer rate (48 kHz), 250 ms crossfade to make the loop
// seamless, then RMS-match so every ambience is equally loud under the
// single volume slider.

namespace {

// Blend the last `fade` samples into the first `fade` so wrapping the
// read index is click-free. The buffer shrinks by `fade`.
void makeSeamless(std::vector<float> &v, int fade)
{
    if ((int)v.size() <= fade * 2) return;
    const int n = (int)v.size() - fade;
    for (int i = 0; i < fade; ++i) {
        float t = float(i) / float(fade);
        v[i] = v[i] * t + v[n + i] * (1.0f - t);
    }
    v.resize(n);
}

// Loudness-match by RMS + tanh soft clip so quiet field recordings and
// hot ones end up equally loud under the volume slider.
void normalizeRms(std::vector<float> &v, float targetRms)
{
    double acc = 0.0;
    for (float x : v) acc += double(x) * x;
    float rms = float(std::sqrt(acc / std::max<size_t>(1, v.size())));
    if (rms < 1e-6f) return;
    float g = targetRms / rms;
    for (float &x : v) x = std::tanh(x * g);
}

// Minimal RIFF/WAVE reader for the PCM16 mono files we ship. Walks the
// chunk list (ffmpeg emits a LIST chunk before data) and returns the
// samples as floats plus the source rate.
bool loadWavPcm16(const QString &path, std::vector<float> &out, int &rate)
{
    QFile f(path);
    if (!f.open(QFile::ReadOnly)) return false;
    QByteArray all = f.readAll();
    if (all.size() < 44) return false;
    const char *d = all.constData();
    if (std::memcmp(d, "RIFF", 4) != 0 || std::memcmp(d + 8, "WAVE", 4) != 0)
        return false;

    int pos = 12;
    int channels = 0, bits = 0;
    rate = 0;
    const char *data = nullptr;
    int dataLen = 0;
    while (pos + 8 <= all.size()) {
        const char *id = d + pos;
        uint32_t len;
        std::memcpy(&len, d + pos + 4, 4);
        const char *body = d + pos + 8;
        if (pos + 8 + (int)len > all.size()) break;
        if (std::memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            uint16_t fmt, ch, bps;
            uint32_t sr;
            std::memcpy(&fmt, body, 2);
            std::memcpy(&ch, body + 2, 2);
            std::memcpy(&sr, body + 4, 4);
            std::memcpy(&bps, body + 14, 2);
            if (fmt != 1) return false;      // PCM only
            channels = ch; rate = int(sr); bits = bps;
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = body; dataLen = int(len);
        }
        pos += 8 + int(len) + (len & 1);     // chunks are word-aligned
    }
    if (!data || channels < 1 || bits != 16 || rate <= 0) return false;

    const int frames = dataLen / 2 / channels;
    out.resize(size_t(frames));
    const int16_t *s = reinterpret_cast<const int16_t *>(data);
    constexpr float kInv = 1.0f / 32768.0f;
    for (int i = 0; i < frames; ++i) {
        // Fold multi-channel down to mono (we ship mono, belt+braces).
        int acc = 0;
        for (int c = 0; c < channels; ++c) acc += s[i * channels + c];
        out[size_t(i)] = (acc / channels) * kInv;
    }
    return true;
}

// Linear resampler - fine for ambience beds.
std::vector<float> resampleLinear(const std::vector<float> &in,
                                  int fromRate, int toRate)
{
    if (in.empty() || fromRate == toRate) return in;
    const double ratio = double(fromRate) / double(toRate);
    const size_t outN = size_t(double(in.size()) / ratio);
    std::vector<float> out(outN);
    for (size_t i = 0; i < outN; ++i) {
        double srcPos = i * ratio;
        size_t i0 = size_t(srcPos);
        size_t i1 = std::min(i0 + 1, in.size() - 1);
        float t = float(srcPos - double(i0));
        out[i] = in[i0] * (1.0f - t) + in[i1] * t;
    }
    return out;
}

struct Entry {
    const char *name;       // untranslated source string
    const char *resource;   // :/ambience/... wav
    float rms;              // loudness target
};

const Entry kCatalog[] = {
    { QT_TRANSLATE_NOOP("MicAmbience", "Washing machine"), ":/ambience/amb00.wav", 0.11f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Microwave"),       ":/ambience/amb01.wav", 0.10f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Drill"),           ":/ambience/amb02.wav", 0.12f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Hair dryer"),      ":/ambience/amb03.wav", 0.11f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Vacuum cleaner"),  ":/ambience/amb04.wav", 0.11f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Rain"),            ":/ambience/amb05.wav", 0.09f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Wind"),            ":/ambience/amb06.wav", 0.10f },
    { QT_TRANSLATE_NOOP("MicAmbience", "City traffic"),    ":/ambience/amb07.wav", 0.10f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Crowd chatter"),   ":/ambience/amb08.wav", 0.10f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Frying pan"),      ":/ambience/amb09.wav", 0.09f },
    { QT_TRANSLATE_NOOP("MicAmbience", "TV static"),       ":/ambience/amb10.wav", 0.08f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Jackhammer"),      ":/ambience/amb11.wav", 0.12f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Angle grinder"),   ":/ambience/amb12.wav", 0.12f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Hammering"),       ":/ambience/amb13.wav", 0.11f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Screaming crowd"), ":/ambience/amb14.wav", 0.10f },
    { QT_TRANSLATE_NOOP("MicAmbience", "Running engine"),  ":/ambience/amb15.wav", 0.11f },
};

} // namespace

namespace MicAmbience {

int count()
{
    return int(sizeof(kCatalog) / sizeof(kCatalog[0]));
}

QString name(int id)
{
    if (id < 0 || id >= count()) return QString();
    return QCoreApplication::translate("MicAmbience", kCatalog[id].name);
}

std::vector<float> generate(int id, int sampleRate)
{
    if (id < 0 || id >= count() || sampleRate <= 0) return {};
    std::vector<float> raw;
    int srcRate = 0;
    if (!loadWavPcm16(QString::fromLatin1(kCatalog[id].resource), raw, srcRate))
        return {};
    std::vector<float> v = resampleLinear(raw, srcRate, sampleRate);
    makeSeamless(v, sampleRate / 4);        // 250 ms loop crossfade
    normalizeRms(v, kCatalog[id].rms);
    return v;
}

namespace {
// Same collector pattern as ConvolutionReverb::loadIrFile.
struct CollectProducer : public SampleProducer {
    std::vector<float> data;   // interleaved stereo floats
    size_t maxSamples;
    explicit CollectProducer(size_t cap) : maxSamples(cap) { data.reserve(cap); }
    void produce(const short *samples, int count) override {
        constexpr float kInv = 1.0f / 32768.0f;
        for (int i = 0; i < count * 2 && data.size() < maxSamples; ++i)
            data.push_back(samples[i] * kInv);
    }
};
} // namespace

std::vector<float> loadCustomFile(const QString &path, int sampleRate)
{
    if (path.isEmpty() || sampleRate <= 0) return {};
    constexpr double kMaxSeconds = 30.0;
    InputFileOptions opt;
    opt.outputChannelLayout = InputFileOptions::STEREO;
    opt.outputSampleRate = sampleRate;
    InputFile *f = CreateInputFileFFmpeg(opt);
    if (!f) return {};

    std::vector<float> mono;
    QByteArray utf8 = path.toUtf8();
    try {
        if (f->open(utf8.constData()) == 0) {
            const size_t cap = size_t(kMaxSeconds * sampleRate) * 2;
            CollectProducer collector(cap);
            while (!f->done() && collector.data.size() < cap) {
                if (f->readSamples(&collector) <= 0) break;
            }
            f->close();
            const size_t frames = collector.data.size() / 2;
            if (frames >= size_t(sampleRate) / 2) {   // >= 0.5 s of audio
                mono.resize(frames);
                for (size_t i = 0; i < frames; ++i)
                    mono[i] = (collector.data[i * 2 + 0]
                             + collector.data[i * 2 + 1]) * 0.5f;
            }
        }
    } catch (...) {
        mono.clear();
    }
    delete f;
    if (mono.empty()) return {};

    makeSeamless(mono, sampleRate / 4);
    normalizeRms(mono, 0.10f);
    return mono;
}

}
