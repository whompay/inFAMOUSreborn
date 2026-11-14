/*
 * ps3recomp - hitch-attribution counters on POSIX hosts
 *
 * include/ps3emu/hz_stats.h is shared by the runtime, but the counters and the
 * main-thread predicate used to live in the D3D12 backend, so a non-Windows
 * build linked against them and failed. Supply the same definitions here for
 * every host that has no D3D12 backend. D3D12 keeps its own on Windows, where
 * the renderer is the only reader and it knows the render thread.
 */
#include "ps3emu/hz_stats.h"

#ifndef _WIN32

#include <pthread.h>

volatile long long g_hz[32];

static pthread_t s_hz_main;
static int       s_hz_have_main = 0;

void ps3_hz_set_main(void)
{
    s_hz_main = pthread_self();
    s_hz_have_main = 1;
}

int ps3_hz_is_main(void)
{
    return s_hz_have_main && pthread_equal(s_hz_main, pthread_self());
}

#endif /* !_WIN32 */
