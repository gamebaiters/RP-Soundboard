#include "file_metadata.h"

#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QCoreApplication>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace {
struct Entry {
    qint64  mtimeMs = 0;
    qint64  sizeBytes = 0;
    QString tooltip;
};

QHash<QString, Entry> g_cache;
QMutex                g_mutex;

QString fmtDuration(double seconds) {
    if (seconds <= 0.0) return QStringLiteral("--:--");
    int s = static_cast<int>(seconds + 0.5);
    int m = s / 60;
    s = s % 60;
    if (m >= 60) {
        int h = m / 60; m = m % 60;
        return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0'))
                                  .arg(s, 2, 10, QChar('0'));
    }
    return QString("%1:%2").arg(m).arg(s, 2, 10, QChar('0'));
}

QString fmtBitrate(int64_t bps) {
    if (bps <= 0) return QString();
    if (bps >= 1000)
        return QString("%1 kbps").arg(static_cast<int>((bps + 500) / 1000));
    return QString("%1 bps").arg(bps);
}

QString humanCodec(const char *name) {
    if (!name) return QStringLiteral("?");
    QString s = QString::fromLatin1(name).toUpper();
    if (s == QLatin1String("MP3FLOAT")) return QStringLiteral("MP3");
    if (s == QLatin1String("PCM_S16LE")) return QStringLiteral("PCM 16-bit");
    if (s == QLatin1String("PCM_S24LE")) return QStringLiteral("PCM 24-bit");
    if (s == QLatin1String("PCM_F32LE")) return QStringLiteral("PCM 32-bit float");
    if (s == QLatin1String("VORBIS"))    return QStringLiteral("Ogg Vorbis");
    return s;
}

// Build the tooltip without touching the cache (caller holds / releases
// the mutex around the probe). Returns the formatted string; on probe
// failure returns an explicit "Unreadable file" message so the caller
// can still cache it (preventing repeated failed probes per hover).
QString probe(const QString &absolutePath) {
    QFileInfo fi(absolutePath);
    if (!fi.exists())     return QCoreApplication::translate("FileMetadata", "File not found");
    if (!fi.isReadable()) return QCoreApplication::translate("FileMetadata", "Permission denied");
    if (fi.size() == 0)   return QCoreApplication::translate("FileMetadata", "Empty file");

    AVFormatContext *fmt = nullptr;
    const QByteArray pathU8 = absolutePath.toUtf8();
    if (avformat_open_input(&fmt, pathU8.constData(), nullptr, nullptr) < 0)
        return QCoreApplication::translate("FileMetadata", "Unreadable file");
    if (avformat_find_stream_info(fmt, nullptr) < 0) {
        avformat_close_input(&fmt);
        return QCoreApplication::translate("FileMetadata", "Unreadable file");
    }

    int audioIdx = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        if (fmt->streams[i]->codecpar
         && fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIdx = static_cast<int>(i); break;
        }
    }
    if (audioIdx < 0) {
        avformat_close_input(&fmt);
        return QCoreApplication::translate("FileMetadata", "No audio stream");
    }

    AVCodecParameters *par = fmt->streams[audioIdx]->codecpar;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(par->codec_id);
    QString codecName = humanCodec(desc ? desc->name : nullptr);
    int sampleRate = par->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
    int channels = par->ch_layout.nb_channels;
#else
    int channels = par->channels;
#endif

    // Prefer stream-level bitrate; fall back to container's bit_rate.
    int64_t bitrate = par->bit_rate > 0 ? par->bit_rate : fmt->bit_rate;

    // Duration in seconds. AV_TIME_BASE is microseconds.
    double durationSec = (fmt->duration > 0)
        ? static_cast<double>(fmt->duration) / AV_TIME_BASE
        : 0.0;

    avformat_close_input(&fmt);

    QStringList lines;
    lines << absolutePath;
    QString fmtLine = QCoreApplication::translate("FileMetadata", "Codec: %1").arg(codecName);
    if (sampleRate > 0)
        fmtLine += QString(", %1 kHz").arg(sampleRate / 1000.0, 0, 'f',
                                          (sampleRate % 1000) ? 1 : 0);
    if (channels > 0)
        fmtLine += QCoreApplication::translate("FileMetadata", ", %1 ch").arg(channels);
    QString br = fmtBitrate(bitrate);
    if (!br.isEmpty()) fmtLine += ", " + br;
    lines << fmtLine;
    lines << QCoreApplication::translate("FileMetadata", "Duration: %1").arg(fmtDuration(durationSec));
    if (fi.size() > 0) {
        double mb = double(fi.size()) / (1024.0 * 1024.0);
        if (mb >= 1.0)
            lines << QCoreApplication::translate("FileMetadata", "Size: %1 MB").arg(mb, 0, 'f', 2);
        else
            lines << QCoreApplication::translate("FileMetadata", "Size: %1 KB").arg(fi.size() / 1024.0, 0, 'f', 1);
    }
    return lines.join('\n');
}
} // namespace

namespace FileMetadata {

QString tooltipFor(const QString &absolutePath) {
    if (absolutePath.isEmpty()) return QString();
    QFileInfo fi(absolutePath);
    qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    qint64 size  = fi.size();

    {
        QMutexLocker lk(&g_mutex);
        auto it = g_cache.find(absolutePath);
        if (it != g_cache.end()
         && it->mtimeMs == mtime && it->sizeBytes == size)
            return it->tooltip;
    }

    QString tip = probe(absolutePath);

    {
        QMutexLocker lk(&g_mutex);
        Entry &e = g_cache[absolutePath];
        e.mtimeMs   = mtime;
        e.sizeBytes = size;
        e.tooltip   = tip;
    }
    return tip;
}

void invalidate(const QString &absolutePath) {
    QMutexLocker lk(&g_mutex);
    g_cache.remove(absolutePath);
}

} // namespace FileMetadata
