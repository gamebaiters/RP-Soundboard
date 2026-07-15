// src/modules/audio_probe.cpp
//----------------------------------
// See audio_probe.h. Header-only FFmpeg probe + per-file memoisation.
//----------------------------------

#include "audio_probe.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/samplefmt.h>
}

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace {

// Cache key includes size + mtime so re-encoding a file in place invalidates
// the entry instead of showing a stale badge forever.
QString cacheKey(const QFileInfo &fi)
{
	return fi.absoluteFilePath() + '|' + QString::number(fi.size()) + '|'
	     + QString::number(fi.lastModified().toMSecsSinceEpoch());
}

bool isLosslessCodec(AVCodecID id)
{
	switch (id) {
		case AV_CODEC_ID_FLAC:
		case AV_CODEC_ID_ALAC:
		case AV_CODEC_ID_APE:
		case AV_CODEC_ID_WAVPACK:
		case AV_CODEC_ID_TTA:
		case AV_CODEC_ID_TAK:
		case AV_CODEC_ID_MLP:
		case AV_CODEC_ID_TRUEHD:
			return true;
		default:
			break;
	}
	// Every raw PCM flavour (wav / aiff / au / caf ...) is lossless.
	const char *n = avcodec_get_name(id);
	return n && qstrncmp(n, "pcm_", 4) == 0;
}

// Format label: the file extension is what the user actually thinks of as
// "the format" (FLAC / MP3 / WAV), so prefer it. Fall back to the codec name
// for extensionless files.
QString formatLabel(const QFileInfo &fi, AVCodecID id)
{
	QString ext = fi.suffix().toUpper();
	if (!ext.isEmpty() && ext.size() <= 5)
		return ext;
	const char *n = avcodec_get_name(id);
	return n ? QString::fromLatin1(n).toUpper() : QString();
}

QString rateLabel(int hz)
{
	if (hz <= 0) return QString();
	const double khz = hz / 1000.0;
	QString s = QString::number(khz, 'f', 1);
	if (s.endsWith(".0")) s.chop(2);
	return s + "kHz";
}

} // namespace

//----------------------------------------------------------------
QString AudioProbeInfo::badge() const
{
	if (!valid) return QString();
	const QString rate = rateLabel(sampleRate);
	QString quality;
	if (lossless && bits > 0)      quality = QString::number(bits) + "bit";
	else if (bitrateKbps > 0)      quality = QString::number(bitrateKbps) + "kbps";

	QString out = format;
	if (!quality.isEmpty()) out += ' ' + quality;
	if (!rate.isEmpty())    out += (quality.isEmpty() ? " " : "/") + rate;
	return out.trimmed();
}

//----------------------------------------------------------------
QString AudioProbeInfo::formatBadge() const
{
	return valid ? format : QString();
}

//----------------------------------------------------------------
QString AudioProbeInfo::qualityBadge() const
{
	if (!valid) return QString();
	const QString rate = rateLabel(sampleRate);
	QString quality;
	if (lossless && bits > 0)      quality = QString::number(bits) + "bit";
	else if (bitrateKbps > 0)      quality = QString::number(bitrateKbps) + "kbps";
	if (!quality.isEmpty() && !rate.isEmpty()) return quality + '/' + rate;
	if (!quality.isEmpty())                    return quality;
	return rate;
}

//----------------------------------------------------------------
int AudioProbeInfo::qualityRank() const
{
	if (!valid) return 0;
	if (lossless) {
		// Hi-res = beyond CD quality on either axis.
		if (bits > 16 || sampleRate > 48000) return 4;
		return 3;
	}
	if (bitrateKbps >= 256) return 2;
	if (bitrateKbps >= 128) return 1;
	return 0;
}

//----------------------------------------------------------------
QColor AudioProbeInfo::formatColor() const
{
	// One recognisable hue per family. Values picked dark enough that
	// white badge text passes the luminance auto-contrast in the
	// channel header on both light and dark base themes.
	const QString f = format;
	if (f == "FLAC")                                   return QColor(0x2e, 0x8b, 0x50); // green
	if (f == "ALAC")                                   return QColor(0x2f, 0x9e, 0x82); // mint
	if (f == "WAV" || f == "AIFF" || f == "AIF"
	    || f == "AU" || f == "CAF")                    return QColor(0x3f, 0x6f, 0xa5); // steel blue
	if (f == "APE" || f == "WV"  || f == "TTA"
	    || f == "TAK")                                 return QColor(0x4a, 0x8a, 0x3c); // moss
	if (f == "MP3")                                    return QColor(0xc2, 0x6a, 0x1d); // orange
	if (f == "OGG" || f == "VORBIS" || f == "OGA")     return QColor(0x7a, 0x4f, 0xb5); // purple
	if (f == "OPUS")                                   return QColor(0xb0, 0x3d, 0x7a); // magenta
	if (f == "M4A" || f == "AAC" || f == "MP4")        return QColor(0xb5, 0x3a, 0x3a); // red
	if (f == "WMA")                                    return QColor(0x50, 0x63, 0x80); // blue-grey
	return QColor(0x5b, 0x6b, 0x7a);                                                   // slate
}

//----------------------------------------------------------------
QColor AudioProbeInfo::qualityColor() const
{
	switch (qualityRank()) {
		case 4:  return QColor(0xa8, 0x83, 0x1f); // gold  - hi-res lossless
		case 3:  return QColor(0x2e, 0x8b, 0x50); // green - lossless
		case 2:  return QColor(0x2a, 0x7f, 0x8f); // teal  - high lossy
		case 1:  return QColor(0xb0, 0x72, 0x28); // amber - mid lossy
		default: return QColor(0x8a, 0x4a, 0x42); // dull red - low lossy
	}
}

//----------------------------------------------------------------
AudioProbeInfo AudioProbe::probe(const QString &path)
{
	static QMutex                        s_mutex;
	static QHash<QString, AudioProbeInfo> s_cache;

	AudioProbeInfo info;
	if (path.isEmpty() || path.contains("://")) return info;   // URL -> WEB badge

	const QFileInfo fi(path);
	if (!fi.exists() || !fi.isFile()) return info;

	const QString key = cacheKey(fi);
	{
		QMutexLocker lock(&s_mutex);
		auto it = s_cache.constFind(key);
		if (it != s_cache.constEnd()) return *it;
	}

	AVFormatContext *ctx = nullptr;
	AVDictionary    *opts = nullptr;
	// Header only: a 256 KB probe / 0.5 s analyse is far more than any codec
	// needs for sample rate + bit depth, and keeps this callable straight from
	// the GUI thread when a channel loads a sound.
	av_dict_set(&opts, "probesize",        "262144",  0);
	av_dict_set(&opts, "analyzeduration",  "500000",  0);

	const QByteArray p = QFile::encodeName(path);
	if (avformat_open_input(&ctx, p.constData(), nullptr, &opts) < 0) {
		av_dict_free(&opts);
		QMutexLocker lock(&s_mutex);
		s_cache.insert(key, info);   // negative cache: don't retry every load
		return info;
	}
	av_dict_free(&opts);

	if (avformat_find_stream_info(ctx, nullptr) >= 0) {
		const int idx = av_find_best_stream(ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (idx >= 0) {
			const AVCodecParameters *par = ctx->streams[idx]->codecpar;
			info.valid      = true;
			info.format     = formatLabel(fi, par->codec_id);
			info.sampleRate = par->sample_rate;
			info.lossless   = isLosslessCodec(par->codec_id);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 37, 100)
			info.channels   = par->ch_layout.nb_channels;
#else
			info.channels   = par->channels;
#endif
			if (info.lossless) {
				// bits_per_raw_sample is what FLAC / ALAC report; PCM carries it
				// in the codec id itself; the decoded sample format is the last
				// resort (and is what a 24-bit FLAC decodes into: s32 -> 32,
				// hence the raw-sample field first).
				int bits = par->bits_per_raw_sample;
				if (bits <= 0) bits = av_get_bits_per_sample(par->codec_id);
				if (bits <= 0 && par->format >= 0)
					bits = 8 * av_get_bytes_per_sample((AVSampleFormat)par->format);
				info.bits = bits;
			}
			qint64 br = par->bit_rate;
			if (br <= 0) br = ctx->bit_rate;
			if (br > 0) info.bitrateKbps = (int)((br + 500) / 1000);
			if (info.format.isEmpty()) info.valid = false;
		}
	}
	avformat_close_input(&ctx);

	QMutexLocker lock(&s_mutex);
	s_cache.insert(key, info);
	return info;
}
