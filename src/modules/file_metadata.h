// Lightweight one-shot audio metadata probe used for cell tooltips.
// Decodes (codec name, sample rate, channels, bitrate, duration) via
// FFmpeg avformat_find_stream_info, caches per absolute path + mtime
// so repeated tooltip lookups never re-open the file. Hover-driven.
#pragma once

#include <QString>

namespace FileMetadata {

// Returns a human-readable multi-line tooltip describing the audio file
// at `absolutePath`. Empty path -> empty string. Files that fail to
// open get an "Unreadable file" stub so the user is never left with a
// stale prior tooltip. Thread-safe; safe to call from the GUI thread.
QString tooltipFor(const QString &absolutePath);

// Drop a cached entry so the next tooltipFor() call re-probes. Useful
// when the user has just replaced the file at this path.
void invalidate(const QString &absolutePath);

} // namespace FileMetadata
