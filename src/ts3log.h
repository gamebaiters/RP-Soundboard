// src/ts3log.h
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------

#pragma once
#ifndef rpsbsrc__ts3log_H__
#define rpsbsrc__ts3log_H__

#include "common.h"

void logMessage(const char *msg, LogLevel level,  ...);

#define logError(msg, ...) logMessage(msg, LogLevel_ERROR, ##__VA_ARGS__)
#define logInfo(msg, ...) logMessage(msg, LogLevel_INFO, ##__VA_ARGS__)
#define logWarning(msg, ...) logMessage(msg, LogLevel_WARNING, ##__VA_ARGS__)
#define logDebug(msg, ...) logMessage(msg, LogLevel_DEBUG, ##__VA_ARGS__)
#define logCritical(msg, ...) logMessage(msg, LogLevel_CRITICAL, ##__VA_ARGS__)

// Extreme-logging gate. When the user enables "Logging estremo" in
// Settings, this fans out into the same debug pipeline as logDebug
// (file + in-memory ring). Off = zero overhead — the printf args are
// never evaluated. Hot-loop friendly.
//
// plugin.h declares this same symbol inside an extern "C" block, so
// the extern here MUST also be C-linkage — otherwise C++ name mangling
// disagrees with the C definition in plugin.cpp and the link breaks
// with "unresolved external symbol".
#ifdef __cplusplus
extern "C" {
#endif
extern int g_rpsbExtremeLogging;
#ifdef __cplusplus
}
#endif
#define extremeLog(msg, ...) do { \
    if (g_rpsbExtremeLogging) logMessage("[XLOG] " msg, LogLevel_DEBUG, ##__VA_ARGS__); \
} while (0)


UINT checkError(UINT code, const char *msg, ...);

#ifdef __cplusplus
#include <QString>
#include <QVector>

// Snapshot of the in-memory log ring used by the in-app log viewer.
// Each entry carries the level (LogLevel_*) and the message text
// already prefixed with an ISO-ish timestamp. Bounded to ~1000 lines
// so the snapshot copy stays cheap. Thread-safe.
struct LogRingEntry {
    int     level;
    QString text;
};

QVector<LogRingEntry> logRingSnapshot();
void logRingClear();
#endif

#ifdef __cplusplus
extern "C" {
#endif
// Pushes a pre-formatted debug-log line into the in-memory ring used by
// the in-app log viewer. Callsites are the static dbgLog/sdbgLog
// helpers in inputfileffmpeg.cpp and samples.cpp - so the viewer
// mirrors exactly what gets written to the physical rpsb_debug.log
// when the user has the "Write debug log file" setting enabled.
// `line` should be the same text the file got, without a trailing
// newline.
void rpsbDebugRingPush(const char *line);
#ifdef __cplusplus
}
#endif

#endif // rpsbsrc__ts3log_H__
