#include "audio_encoder_ffmpeg.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include <QFileInfo>
#include <cstdlib>
#include <cstring>
#include <algorithm>

namespace {

const char *containerName(AudioEncoderFFmpeg::Format f) {
    switch (f) {
        case AudioEncoderFFmpeg::FmtWav:        return "wav";
        case AudioEncoderFFmpeg::FmtFlac:       return "flac";
        case AudioEncoderFFmpeg::FmtOggVorbis:  return "ogg";
        case AudioEncoderFFmpeg::FmtAacM4a:     return "ipod";   // mp4/m4a
    }
    return "wav";
}

AVCodecID codecFor(AudioEncoderFFmpeg::Format f) {
    switch (f) {
        case AudioEncoderFFmpeg::FmtWav:       return AV_CODEC_ID_PCM_S16LE;
        case AudioEncoderFFmpeg::FmtFlac:      return AV_CODEC_ID_FLAC;
        case AudioEncoderFFmpeg::FmtOggVorbis: return AV_CODEC_ID_VORBIS;
        case AudioEncoderFFmpeg::FmtAacM4a:    return AV_CODEC_ID_AAC;
    }
    return AV_CODEC_ID_PCM_S16LE;
}

// Pick a sample format supported by the encoder. The codec's
// sample-format list is canonical; we just take the first entry. For
// PCM/FLAC that's S16; for Vorbis/AAC it's FLTP.
// In FFmpeg 7.1+ AVCodec.sample_fmts is deprecated in favour of
// avcodec_get_supported_config(AV_CODEC_CONFIG_SAMPLE_FORMAT); the
// new API is forward-compatible with future codec internals.
AVSampleFormat pickSampleFmt(const AVCodec *codec) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const enum AVSampleFormat *fmts = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec,
            AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
            (const void **)&fmts, &n) >= 0 && fmts && n > 0) {
        return fmts[0];
    }
#else
    if (codec->sample_fmts) return codec->sample_fmts[0];
#endif
    return AV_SAMPLE_FMT_S16;
}

// Pick a sample rate supported by the encoder. If the codec lists no
// constraint, all rates are allowed; otherwise pick the closest to
// wanted. Same FFmpeg 7.1+ avcodec_get_supported_config migration as
// pickSampleFmt above.
int pickSampleRate(const AVCodec *codec, int wanted) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
    const int *rates = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, codec,
            AV_CODEC_CONFIG_SAMPLE_RATE, 0,
            (const void **)&rates, &n) < 0 || !rates || n <= 0) {
        return wanted;
    }
    int best = rates[0];
    int bestDiff = std::abs(best - wanted);
    for (int i = 1; i < n; ++i) {
        int diff = std::abs(rates[i] - wanted);
        if (diff < bestDiff) { best = rates[i]; bestDiff = diff; }
    }
    return best;
#else
    if (!codec->supported_samplerates) return wanted;
    int best = codec->supported_samplerates[0];
    int bestDiff = std::abs(best - wanted);
    for (const int *p = codec->supported_samplerates; *p; ++p) {
        int diff = std::abs(*p - wanted);
        if (diff < bestDiff) { best = *p; bestDiff = diff; }
    }
    return best;
#endif
}

} // namespace


bool AudioEncoderFFmpeg::formatFromExtension(const QString &filename, Format &out) {
    QString ext = QFileInfo(filename).suffix().toLower();
    if (ext == "wav")  { out = FmtWav;       return true; }
    if (ext == "flac") { out = FmtFlac;      return true; }
    if (ext == "ogg" || ext == "oga") { out = FmtOggVorbis; return true; }
    if (ext == "m4a" || ext == "mp4" || ext == "aac") { out = FmtAacM4a; return true; }
    return false;
}

const char *AudioEncoderFFmpeg::defaultExtension(Format fmt) {
    switch (fmt) {
        case FmtWav:        return ".wav";
        case FmtFlac:       return ".flac";
        case FmtOggVorbis:  return ".ogg";
        case FmtAacM4a:     return ".m4a";
    }
    return ".wav";
}

const char *AudioEncoderFFmpeg::displayName(Format fmt) {
    switch (fmt) {
        case FmtWav:        return "WAV";
        case FmtFlac:       return "FLAC";
        case FmtOggVorbis:  return "OGG Vorbis";
        case FmtAacM4a:     return "AAC (M4A)";
    }
    return "WAV";
}


AudioEncoderFFmpeg::AudioEncoderFFmpeg() = default;

AudioEncoderFFmpeg::~AudioEncoderFFmpeg() {
    if (!m_closed) close();
    if (m_swr)    { swr_free(&m_swr); }
    if (m_frame)  { av_frame_free(&m_frame); }
    if (m_pkt)    { av_packet_free(&m_pkt); }
    if (m_codec)  { avcodec_free_context(&m_codec); }
    if (m_fmt) {
        if (m_fmt->pb) avio_closep(&m_fmt->pb);
        avformat_free_context(m_fmt);
        m_fmt = nullptr;
    }
    if (m_bufS16) { std::free(m_bufS16); m_bufS16 = nullptr; }
}

bool AudioEncoderFFmpeg::open(const QString &filename, Format fmt,
                               int sampleRate, int channels) {
    if (sampleRate <= 0 || channels <= 0) {
        m_error = QStringLiteral("Invalid sample rate / channel count");
        return false;
    }

    m_sampleRate = sampleRate;
    m_channels   = channels;

    const QByteArray utf8 = filename.toUtf8();
    const char *container = containerName(fmt);

    int ret = avformat_alloc_output_context2(&m_fmt, nullptr, container, utf8.constData());
    if (ret < 0 || !m_fmt) {
        m_error = QStringLiteral("avformat_alloc_output_context2 failed");
        return false;
    }

    const AVCodecID codecId = codecFor(fmt);
    const AVCodec *codec = avcodec_find_encoder(codecId);
    if (!codec) {
        m_error = QStringLiteral("Encoder %1 not available in this FFmpeg build")
                    .arg(QString::fromUtf8(avcodec_get_name(codecId)));
        return false;
    }

    m_stream = avformat_new_stream(m_fmt, codec);
    if (!m_stream) {
        m_error = QStringLiteral("avformat_new_stream failed");
        return false;
    }

    m_codec = avcodec_alloc_context3(codec);
    if (!m_codec) {
        m_error = QStringLiteral("avcodec_alloc_context3 failed");
        return false;
    }

    m_codec->sample_rate = pickSampleRate(codec, sampleRate);
    m_codec->sample_fmt  = pickSampleFmt(codec);
    // Use the new ch_layout API throughout (n5+); the legacy
    // channels / channel_layout fields are deprecated in 6.x and gone
    // in 7.x. av_channel_layout_default gives stereo for 2ch, mono
    // for 1ch, 5.1 for 6ch, etc.
    av_channel_layout_uninit(&m_codec->ch_layout);
    av_channel_layout_default(&m_codec->ch_layout, channels);
    m_codec->time_base  = AVRational{1, m_codec->sample_rate};
    m_outSampleFmt      = m_codec->sample_fmt;
    m_inSampleFmt       = AV_SAMPLE_FMT_S16;

    // Reasonable defaults per format. The user can replace these via
    // future UI; for now they bake good-quality defaults.
    switch (fmt) {
        case FmtFlac:
            // FLAC compression level 0..12; 5 is a good speed/size balance.
            av_opt_set_int(m_codec, "compression_level", 5, AV_OPT_SEARCH_CHILDREN);
            break;
        case FmtOggVorbis:
            // Vorbis target VBR around q=5 (~160 kbps stereo).
            m_codec->bit_rate = 160000;
            break;
        case FmtAacM4a:
            m_codec->bit_rate = 192000;
            // Native AAC encoder is marked experimental in older builds.
            m_codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
            break;
        case FmtWav:
            break;
    }

    if (m_fmt->oformat->flags & AVFMT_GLOBALHEADER)
        m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(m_codec, codec, nullptr);
    if (ret < 0) {
        char buf[128];
        av_strerror(ret, buf, sizeof(buf));
        m_error = QStringLiteral("avcodec_open2 failed: %1").arg(QString::fromUtf8(buf));
        return false;
    }

    ret = avcodec_parameters_from_context(m_stream->codecpar, m_codec);
    if (ret < 0) {
        m_error = QStringLiteral("avcodec_parameters_from_context failed");
        return false;
    }
    m_stream->time_base = m_codec->time_base;

    if (!(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&m_fmt->pb, utf8.constData(), AVIO_FLAG_WRITE);
        if (ret < 0) {
            char buf[128];
            av_strerror(ret, buf, sizeof(buf));
            m_error = QStringLiteral("avio_open failed: %1").arg(QString::fromUtf8(buf));
            return false;
        }
    }

    ret = avformat_write_header(m_fmt, nullptr);
    if (ret < 0) {
        char buf[128];
        av_strerror(ret, buf, sizeof(buf));
        m_error = QStringLiteral("avformat_write_header failed: %1").arg(QString::fromUtf8(buf));
        return false;
    }
    m_headerWritten = true;

    // PCM encoders accept any frame_size; vorbis/aac/flac want a fixed
    // number of samples per frame (e.g. 1024 for aac, 4096 for flac,
    // variable for vorbis). frame_size = 0 means "any size".
    m_frameSize = m_codec->frame_size;
    if (m_frameSize <= 0) m_frameSize = 1024;

    // Pre-allocate the chunk buffer at 2 * frame_size capacity so a
    // single write() can drain into one or two encoder frames without
    // re-allocating.
    m_bufCap = m_frameSize * channels * 4;
    m_bufS16 = static_cast<int16_t *>(std::malloc(m_bufCap * sizeof(int16_t)));
    if (!m_bufS16) {
        m_error = QStringLiteral("Out of memory");
        return false;
    }
    m_bufFill = 0;

    m_frame = av_frame_alloc();
    m_pkt   = av_packet_alloc();
    if (!m_frame || !m_pkt) {
        m_error = QStringLiteral("av_frame_alloc / av_packet_alloc failed");
        return false;
    }
    m_frame->format      = m_outSampleFmt;
    av_channel_layout_copy(&m_frame->ch_layout, &m_codec->ch_layout);
    m_frame->sample_rate = m_codec->sample_rate;
    m_frame->nb_samples  = m_frameSize;
    ret = av_frame_get_buffer(m_frame, 0);
    if (ret < 0) {
        m_error = QStringLiteral("av_frame_get_buffer failed");
        return false;
    }

    // Set up sample-format conversion: input is S16 interleaved at
    // sampleRate; output is m_outSampleFmt at m_codec->sample_rate.
    // swr_alloc_set_opts2 is the n5+ replacement for the deprecated
    // av_opt_set_int channel_layout setters.
    m_swr = nullptr;
    {
        int swrRet = swr_alloc_set_opts2(&m_swr,
            &m_codec->ch_layout,  m_codec->sample_fmt,  m_codec->sample_rate,
            &m_codec->ch_layout,  AV_SAMPLE_FMT_S16,    sampleRate,
            0, nullptr);
        if (swrRet < 0 || !m_swr || swr_init(m_swr) < 0) {
            m_error = QStringLiteral("swr_init failed");
            return false;
        }
    }

    return true;
}

bool AudioEncoderFFmpeg::writeFrameFromBuffer() {
    // Drain exactly m_frameSize stereo samples from m_bufS16 into the
    // encoder. Caller guarantees m_bufFill >= m_frameSize * m_channels.
    const int inFrames = m_frameSize;

    // Reallocate the encoder frame buffer if previous flush emptied it.
    int ret = av_frame_make_writable(m_frame);
    if (ret < 0) {
        m_error = QStringLiteral("av_frame_make_writable failed");
        return false;
    }
    m_frame->nb_samples = inFrames;

    const uint8_t *inPlanes[1] = { reinterpret_cast<const uint8_t *>(m_bufS16) };
    int converted = swr_convert(m_swr,
                                m_frame->data, inFrames,
                                inPlanes,      inFrames);
    if (converted < 0) {
        m_error = QStringLiteral("swr_convert failed");
        return false;
    }
    m_frame->nb_samples = converted;
    m_frame->pts = m_nextPts;
    m_nextPts += converted;

    if (!encodeAndWrite(m_frame)) return false;

    // Shift the remainder down in the buffer.
    const int consumed = inFrames * m_channels;
    const int remain   = m_bufFill - consumed;
    if (remain > 0)
        std::memmove(m_bufS16, m_bufS16 + consumed, remain * sizeof(int16_t));
    m_bufFill = remain;
    return true;
}

bool AudioEncoderFFmpeg::write(const int16_t *samples, int frames) {
    if (!m_codec || m_closed) {
        m_error = QStringLiteral("Encoder not open");
        return false;
    }
    if (frames <= 0) return true;

    int offset = 0;
    while (offset < frames) {
        const int needed = m_frameSize * m_channels;
        const int spaceLeft = m_bufCap - m_bufFill;
        if (spaceLeft <= 0) {
            // Buffer full but not enough for one frame? Grow.
            int newCap = m_bufCap * 2;
            if (newCap < needed * 2) newCap = needed * 2;
            auto *grown = static_cast<int16_t *>(std::realloc(m_bufS16, newCap * sizeof(int16_t)));
            if (!grown) { m_error = QStringLiteral("Out of memory"); return false; }
            m_bufS16 = grown;
            m_bufCap = newCap;
            continue;
        }
        const int chunkFrames = std::min(frames - offset, spaceLeft / m_channels);
        std::memcpy(m_bufS16 + m_bufFill,
                    samples + offset * m_channels,
                    chunkFrames * m_channels * sizeof(int16_t));
        m_bufFill += chunkFrames * m_channels;
        offset    += chunkFrames;

        while (m_bufFill >= needed) {
            if (!writeFrameFromBuffer()) return false;
        }
    }
    return true;
}

bool AudioEncoderFFmpeg::encodeAndWrite(AVFrame *frame) {
    int ret = avcodec_send_frame(m_codec, frame);
    if (ret < 0 && ret != AVERROR_EOF) {
        char buf[128];
        av_strerror(ret, buf, sizeof(buf));
        m_error = QStringLiteral("avcodec_send_frame failed: %1").arg(QString::fromUtf8(buf));
        return false;
    }
    for (;;) {
        ret = avcodec_receive_packet(m_codec, m_pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) {
            char buf[128];
            av_strerror(ret, buf, sizeof(buf));
            m_error = QStringLiteral("avcodec_receive_packet failed: %1").arg(QString::fromUtf8(buf));
            return false;
        }
        av_packet_rescale_ts(m_pkt, m_codec->time_base, m_stream->time_base);
        m_pkt->stream_index = m_stream->index;
        ret = av_interleaved_write_frame(m_fmt, m_pkt);
        av_packet_unref(m_pkt);
        if (ret < 0) {
            char buf[128];
            av_strerror(ret, buf, sizeof(buf));
            m_error = QStringLiteral("av_interleaved_write_frame failed: %1").arg(QString::fromUtf8(buf));
            return false;
        }
    }
    return true;
}

bool AudioEncoderFFmpeg::close() {
    if (m_closed) return true;
    m_closed = true;

    if (m_codec) {
        // Drain any remaining partial frame. encoders that require
        // fixed-size frames will silently drop a trailing partial
        // chunk; the codec's get_buffer flushes don't help here. To
        // avoid silently losing the tail, pad the last partial frame
        // with zeros so it can be encoded as a full frame.
        if (m_bufFill > 0 && m_channels > 0) {
            const int needed = m_frameSize * m_channels;
            if (m_bufFill < needed) {
                if (m_bufFill + (needed - m_bufFill) > m_bufCap) {
                    int newCap = needed * 2;
                    auto *grown = static_cast<int16_t *>(std::realloc(m_bufS16, newCap * sizeof(int16_t)));
                    if (grown) { m_bufS16 = grown; m_bufCap = newCap; }
                }
                std::memset(m_bufS16 + m_bufFill, 0,
                            (needed - m_bufFill) * sizeof(int16_t));
                m_bufFill = needed;
            }
            writeFrameFromBuffer();
        }
        // Flush the encoder.
        encodeAndWrite(nullptr);
    }

    if (m_headerWritten && m_fmt) {
        av_write_trailer(m_fmt);
    }
    if (m_fmt && m_fmt->pb && !(m_fmt->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&m_fmt->pb);
    }
    return true;
}
