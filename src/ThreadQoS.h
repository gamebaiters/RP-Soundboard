// src/ThreadQoS.h
//----------------------------------------------------------------------------
// Worker-thread QoS promotion (macOS).
//
// TS3 on macOS can call into the plugin from a low-QoS thread, and
// std::thread INHERITS the creating thread's QoS class. On Darwin a
// background/utility QoS class is not just "efficiency cores": the kernel
// maps it onto the SOCKET traffic class of every connection the thread
// creates, so its TCP flows are LEDBAT-style background-throttled to a
// trickle no matter how fast the link is. That starved the streaming
// decoder ("bandwidth is tiny despite a fast connection" — slow loads,
// mid-play stalls) whenever the inherited QoS happened to be low.
//
// Every decode / network worker calls sbPromoteThreadQoS() as its first
// statement. USER_INITIATED = "the user is actively waiting on this work"
// (they pressed play): foreground scheduling AND the foreground network
// service class. No-op on Windows/Linux, where thread priority never
// throttles sockets.
//----------------------------------------------------------------------------
#pragma once

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>
static inline void sbPromoteThreadQoS()
{
	pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
}
#else
static inline void sbPromoteThreadQoS() {}
#endif
