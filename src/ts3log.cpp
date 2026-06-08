// src/ts3log.cpp
//----------------------------------
// RP Soundboard Source Code
// Copyright (c) 2015 Marius Graefe
// All rights reserved
// Contact: rp_soundboard@mgraefe.de
//----------------------------------


#include "common.h"
#include "ts3log.h"

#include <cstdarg>
#include <string>
#include <deque>
#include <mutex>

#include <QDateTime>

namespace {
// Bounded ring of recent log lines used by the in-app log viewer. Lives
// in this TU so every logMessage() callsite feeds it for free without
// touching every callsite. 1000 lines comfortably covers a long debug
// session without ballooning RSS - longest line is 512 chars + meta,
// worst case ~600 KB total.
constexpr int kRingCap = 1000;
std::deque<LogRingEntry> g_ring;
std::mutex               g_ringMutex;

void ringPush(int level, const char *body) {
    QString timestamp = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    LogRingEntry e;
    e.level = level;
    e.text  = QString("[%1] %2").arg(timestamp,
                                     QString::fromUtf8(body));
    std::lock_guard<std::mutex> lk(g_ringMutex);
    if (g_ring.size() >= kRingCap) g_ring.pop_front();
    g_ring.push_back(std::move(e));
}
} // namespace

void logMessage(const char *msg, LogLevel level,  ...)
{
	char buf[512];
	va_list argptr;

	va_start(argptr, level);
    vsnprintf(buf, 512, msg, argptr);
	va_end(argptr);

	ts3Functions.logMessage(buf, level, "SB", 0);
	ringPush(static_cast<int>(level), buf);
}


UINT checkError(UINT code, const char *msg, ...)
{
	if(code != ERROR_ok)
	{
		char buf[512];
		va_list argptr;

		va_start(argptr, msg);
        vsnprintf(buf, 512, msg, argptr);
		va_end(argptr);

		ts3Functions.logMessage(buf, LogLevel_ERROR, "SB", 0);
		ringPush(static_cast<int>(LogLevel_ERROR), buf);
	}

	return code;
}

QVector<LogRingEntry> logRingSnapshot()
{
	std::lock_guard<std::mutex> lk(g_ringMutex);
	QVector<LogRingEntry> out;
	out.reserve(static_cast<int>(g_ring.size()));
	for (const auto &e : g_ring) out.push_back(e);
	return out;
}

void logRingClear()
{
	std::lock_guard<std::mutex> lk(g_ringMutex);
	g_ring.clear();
}

extern "C" void rpsbDebugRingPush(const char *line)
{
	if (!line) return;
	ringPush(static_cast<int>(LogLevel_DEBUG), line);
}
