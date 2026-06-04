// Single-purpose FFmpeg-backed audio encoder used by the channel-export
// path. Wraps avformat / avcodec / swresample into a "open / write /
// close" interface that takes interleaved S16 stereo PCM at a fixed
// sample rate and produces a finished file in any of the supported
// container/codec combinations.
//
// Why FFmpeg and not the previous hand-rolled WAV writer: extending the
// export to non-WAV formats (FLAC / OGG Vorbis / AAC-M4A) needs a real
// codec pipeline. We already link FFmpeg statically for decoding, so
// piggybacking on its encoders keeps the dependency footprint flat.

#pragma once

#include <QString>
#include <cstdint>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;
struct AVFrame;
struct AVPacket;
struct SwrContext;
}

class AudioEncoderFFmpeg {
public:
    enum Format {
        FmtWav,         // pcm_s16le in wav container
        FmtFlac,        // flac in native flac container
        FmtOggVorbis,   // vorbis in ogg container
        FmtAacM4a,      // aac in mp4 (ipod) container
    };

    // Pick a Format from the file extension. Returns true + fmt on
    // recognised extensions; false on unknown (caller should reject
    // or fall back to wav).
    static bool formatFromExtension(const QString &filename, Format &out);
    // Default extension used by a format (".wav", ".flac", ...).
    static const char *defaultExtension(Format fmt);
    // Human-readable name ("WAV", "FLAC", ...).
    static const char *displayName(Format fmt);

    AudioEncoderFFmpeg();
    ~AudioEncoderFFmpeg();

    // Open the output file. sampleRate / channels describe the input
    // PCM that will be passed to write(). The encoder converts to the
    // codec's native sample format internally.
    bool open(const QString &filename, Format fmt, int sampleRate, int channels);

    // Push interleaved S16 samples (channels interleaved). frames is
    // the per-channel sample count. Returns false on encoder / mux
    // error; the caller may stop and inspect lastError().
    bool write(const int16_t *samples, int frames);

    // Drain the encoder + write trailer + close the file. Safe to
    // call multiple times; after the first successful call further
    // calls are no-ops.
    bool close();

    QString lastError() const { return m_error; }

private:
    bool encodeAndWrite(AVFrame *frame);
    bool writeFrameFromBuffer();

    AVFormatContext *m_fmt    = nullptr;
    AVCodecContext  *m_codec  = nullptr;
    AVStream        *m_stream = nullptr;
    AVFrame         *m_frame  = nullptr;     // re-used encoder input frame
    AVPacket        *m_pkt    = nullptr;
    SwrContext      *m_swr    = nullptr;

    int      m_sampleRate     = 0;
    int      m_channels       = 0;
    int      m_frameSize      = 0;           // codec's preferred frame size
    int      m_inSampleFmt    = 0;           // AV_SAMPLE_FMT_S16
    int      m_outSampleFmt   = 0;           // codec's native fmt
    int64_t  m_nextPts        = 0;
    bool     m_headerWritten  = false;
    bool     m_closed         = false;

    // Interleaved S16 buffer used to chunk arbitrary write() sizes into
    // exact m_frameSize blocks for the encoder.
    int16_t *m_bufS16         = nullptr;
    int      m_bufCap         = 0;
    int      m_bufFill        = 0;

    QString  m_error;
};
