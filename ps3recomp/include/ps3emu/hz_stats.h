/* v225A hitch attribution counters (2026-10-16).
 * Cumulative, never reset; the per-frame reporter in rsx_d3d12_backend.c (slowframe_report) diffs them at every
 * frame end and prints [SPIKE] / [FRAMEPCT] lines. Times are in TSC ticks (converted by the reporter). Every site
 * is a slow host operation (file read, shader compile, PSO create, texture upload, native inflate), so the cost is
 * two rdtsc + one locked add per event -- always on, no env needed. */
#ifndef PS3EMU_HZ_STATS_H
#define PS3EMU_HZ_STATS_H
#include <stdint.h>
#if defined(_MSC_VER) || defined(_WIN32)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#ifdef __cplusplus
extern "C" {
#endif
enum {
    HZ_FS_TSC = 0, HZ_FS_BYTES, HZ_FS_N,      /* guest file reads (cellFsRead / aio / sdata): host fread time */
    HZ_FS_MAIN_TSC, HZ_FS_MAIN_N,            /*   ... of them on the main guest thread */
    HZ_SHC_TSC, HZ_SHC_N, HZ_SHC_MISS,       /* cached_compile: disk-cache load or D3DCompile (misses) */
    HZ_VS_TSC, HZ_VS_N,                      /* vp_get_vs misses (FIFO thread): VP decompile + compile */
    HZ_PSO_TSC, HZ_PSO_N,                    /* vp_get_fp_pso misses (render thread): FP decompile + compile + PSO */
    HZ_TEX_TSC, HZ_TEX_NEW, HZ_TEX_BYTES,    /* vp_upload_tex_slot (render thread): all calls; creates; staged bytes */
    HZ_FIOS_TSC, HZ_FIOS_N, HZ_FIOS_MAIN_TSC,/* FIOS_HLE native inflate (on the pushing PPU thread) */
    HZ_GFLIP_N, HZ_GFLIP_LAST_TSC, HZ_GFLIP_MAX_TSC, /* guest flip requests (cellGcmSys gcm_flip_request): count,
                                                last time, longest gap since the reporter last read it (reset) */
    HZ_COUNT = 32
};
extern volatile long long g_hz[32];
int ps3_hz_is_main(void);                    /* calling host thread == main guest thread */
void ps3_hz_set_main(void);                  /* record the calling thread as the main guest thread */
static inline unsigned long long hz_tsc(void) { return __rdtsc(); }
#if defined(_MSC_VER) || defined(_WIN32)
static inline void hz_add(int i, long long v) { _InterlockedExchangeAdd64((volatile long long*)&g_hz[i], v); }
#else
static inline void hz_add(int i, long long v) { __atomic_fetch_add(&g_hz[i], v, __ATOMIC_RELAXED); }
#endif
/* guest frame cadence: call on every guest flip request */
static inline void hz_guest_flip(void)
{
    const long long now = (long long)__rdtsc(), last = g_hz[HZ_GFLIP_LAST_TSC];
    g_hz[HZ_GFLIP_LAST_TSC] = now;
    if (last && now - last > g_hz[HZ_GFLIP_MAX_TSC]) g_hz[HZ_GFLIP_MAX_TSC] = now - last;
    hz_add(HZ_GFLIP_N, 1);
}
#ifdef __cplusplus
}
#endif
#endif
