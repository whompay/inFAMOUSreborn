/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#ifndef _WIN32
#include <sys/mman.h>
#endif
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <io.h>
#include <timeapi.h>
#else
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
void     cellGcm_fifo_kick_wait(unsigned ms); /* idle wait, woken by a kick (v28) */
extern volatile long g_rsx_idle_wait;
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" void rsx_d3d12_heartbeat(void);   /* [HEARTBEAT] every 10 s (HEARTBEAT=0 off) */
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

extern "C" volatile int g_fifo_where;   /* v207 [REFPOLL] FIFO-thread state at dry fence polls (cellGcmSys.c) */
static void present_guest_frame(void)
{
    const int fw0 = g_fifo_where; g_fifo_where = 3;
    rsx_backend_present();
    g_fifo_where = fw0;
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
}

#ifdef PORT_PGO_GEN
extern "C" int __llvm_profile_write_file(void);   /* v189c: also used by the window-close exit */
#endif
extern "C" volatile int g_abx;   /* v198: ppu_loader.cpp (in-run A/B bits) */
static DWORD WINAPI frame_clock(LPVOID)
{
#ifdef _WIN32
    /* The RSX pump releases guest fences and therefore sits on the critical
     * path of nearly every frame.  Keep it responsive under Windows' desktop
     * scheduler without using the starvation-prone TIME_CRITICAL class. */
    if (getenv("PS3_FAST_HOST"))
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "inFamous (BCES00220)";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
    ULONGLONG next_tick = GetTickCount64();

    /* v28 (FPS): the loop slept a flat Sleep(4) after every drain, so a guest
     * that had just advanced `put` (or SPU jobs waiting on an RSX label) waited
     * up to ~5 ms for the next drain -- repeatedly per frame. Now it waits on
     * the GCM kick event, which guest stores to the FIFO `put` register set
     * while this thread is idle (vm_write32 -> cellGcm_put_kick). RSX_IDLE_SLEEP=1
     * restores the flat Sleep(4). */
    static int s_flat = -1;
    if (s_flat < 0) { const char* e = getenv("RSX_IDLE_SLEEP"); s_flat = (e && *e == '1') ? 1 : 0; }
    for (;;) {
#ifdef _WIN32
        /* v198 ABX 0x20000: FIFO thread at HIGHEST (in-run A/B; it publishes the fence the main thread waits on) */
        { static int s_prio = -1; const int want = (g_abx & 0x20000) ? 1 : 0;
          if (want != s_prio) { s_prio = want;
              SetThreadPriority(GetCurrentThread(), want ? THREAD_PRIORITY_HIGHEST
                                                         : (getenv("PS3_FAST_HOST") ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL)); } }
#endif
        if (s_flat) Sleep(4);
        else { static int s_idle_ms = -1;   /* v90 RSX_IDLE_MS (default 1, was a fixed 4): cap on a missed put-kick */
               if (s_idle_ms < 0) { const char* e = getenv("RSX_IDLE_MS"); s_idle_ms = (e && *e) ? atoi(e) : 1; if (s_idle_ms < 1) s_idle_ms = 1;
                                    fprintf(stderr, "[rsx] FIFO idle wait cap %d ms (RSX_IDLE_MS)\n", s_idle_ms); }
               g_rsx_idle_wait = 1; g_fifo_where = 1; cellGcm_fifo_kick_wait((unsigned)s_idle_ms); g_rsx_idle_wait = 0; g_fifo_where = 0; }
        ULONGLONG now = GetTickCount64();
#if defined(_WIN32)
        if (rsx_ok) rsx_d3d12_heartbeat();   /* self-rate-limited to one line per 10 s */
#endif

        int fired = 0;
        while ((long long)(now - next_tick) >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) { g_fifo_where = 2; cellGcm_rsx_process_fifo(); g_fifo_where = 0; }
            next_tick += 16;             /* ~60 Hz */
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            g_fifo_where = 2; cellGcm_rsx_process_fifo(); g_fifo_where = 4;

            if (rsx_backend_pump() != 0) {
                rsx_ok = 0;              /* window closed */
                /* v189c: closing the window (X / Alt+F4) used to leave the process running headless, so the
                 * launcher never returned (logs ended in ^C when the console was killed, the map copy and any
                 * chained step -- pgo_all's merge/build -- never ran). Now: flush and end the process.
                 * CLOSE_KEEPS_RUNNING=1 = old behaviour. TerminateProcess (not exit()): no atexit/static
                 * destructors racing the guest threads; the PGO profile is written explicitly first. */
                if (!getenv("CLOSE_KEEPS_RUNNING")) {
                    fprintf(stderr, "[boot] window closed -> exiting\n");
#ifdef PORT_PGO_GEN
                    __llvm_profile_write_file();
#endif
                    fflush(NULL);
                    TerminateProcess(GetCurrentProcess(), 0);
                }
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            g_fifo_where = 0;
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                rsx_backend_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

#ifdef _WIN32
/* v221B PS3_LARGE_PAGES (default OFF): back the HOT guest ranges with 2 MB large pages instead of demand-committed
 * 4 KB pages, to cut DTLB misses of the lifted PPU code (b027: about half of the hottest lifted-code samples sit
 * on a guest load = memory latency; with 4 KB pages the ~200 MB the game touches need ~50k PTEs, far beyond the
 * STLB's ~2k entries). Same bytes, same addresses, same zero-filled fresh memory -> no guest-visible change;
 * the large ranges are committed (and locked in RAM) up front instead of 64 KB at a time on first touch.
 *   PS3_LARGE_PAGES=1       ranges "0-10000000,40000000-50000000" (main memory incl. ELF/.bss/main stack +
 *                           the sys_memory window = the game heap; 512 MB of physical RAM, locked)
 *   PS3_LARGE_PAGES=<list>  own ranges, hex "lo-hi[,lo-hi...]" (rounded out to the large-page size)
 *   PS3_LARGE_PAGES=grant   one-time: give the current user "Lock pages in memory" (SeLockMemoryPrivilege) via
 *                           LsaAddAccountRights -- needs an ELEVATED run; then sign out + in. The run itself
 *                           continues without large pages.
 * Needs SeLockMemoryPrivilege in the process token; without it (or on any failure) it logs why and falls back to
 * the normal demand-commit reservation. Not combined with VC_WW (write-watch needs the single reservation).
 * Layout: the 4 GB(+64 KB) arena is assembled at one free, large-page-aligned base from separate allocations:
 * MEM_RESERVE gaps (demand-committed by vm_commit_veh exactly as before) and MEM_RESERVE|MEM_COMMIT|
 * MEM_LARGE_PAGES pieces. A self-test then calls VirtualAlloc(MEM_COMMIT) inside every large piece (that is
 * what vm_commit / sys_memory_allocate / the VEH do) and gives up on large pages if Windows refuses it.
 * Log tag [LGPG]. */
#include <ntsecapi.h>
#pragma comment(lib, "advapi32.lib")
static bool lp_privilege(void)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        fprintf(stderr, "[LGPG] OpenProcessToken failed (%lu)\n", GetLastError()); return false; }
    TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueA(NULL, "SeLockMemoryPrivilege", &tp.Privileges[0].Luid) &&
              AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
    const DWORD err = GetLastError();   /* AdjustTokenPrivileges "succeeds" with ERROR_NOT_ALL_ASSIGNED */
    CloseHandle(tok);
    if (!ok || err == ERROR_NOT_ALL_ASSIGNED) {
        fprintf(stderr, "[LGPG] SeLockMemoryPrivilege NOT held (err %lu) -> no large pages. Grant it once: run with "
                        "PS3_LARGE_PAGES=grant ELEVATED (or secpol.msc > Local Policies > User Rights Assignment > "
                        "Lock pages in memory > add your user), then sign out and in.\n", err);
        return false;
    }
    return true;
}
static void lp_grant(void)
{
    HANDLE tok = NULL; BYTE buf[256]; DWORD len = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) ||
        !GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len)) {
        fprintf(stderr, "[LGPG] grant: cannot read the user SID (%lu)\n", GetLastError()); if (tok) CloseHandle(tok); return; }
    CloseHandle(tok);
    LSA_OBJECT_ATTRIBUTES oa; memset(&oa, 0, sizeof oa);
    LSA_HANDLE pol = NULL;
    NTSTATUS st = LsaOpenPolicy(NULL, &oa, POLICY_CREATE_ACCOUNT | POLICY_LOOKUP_NAMES, &pol);
    if (st) { fprintf(stderr, "[LGPG] grant: LsaOpenPolicy failed (win32 err %lu) -- run ELEVATED\n", LsaNtStatusToWinError(st)); return; }
    WCHAR name[] = L"SeLockMemoryPrivilege";
    LSA_UNICODE_STRING r; r.Buffer = name; r.Length = (USHORT)(wcslen(name) * sizeof(WCHAR)); r.MaximumLength = r.Length + sizeof(WCHAR);
    st = LsaAddAccountRights(pol, ((TOKEN_USER*)buf)->User.Sid, &r, 1);
    LsaClose(pol);
    if (st) fprintf(stderr, "[LGPG] grant: LsaAddAccountRights failed (win32 err %lu)\n", LsaNtStatusToWinError(st));
    else    fprintf(stderr, "[LGPG] grant: 'Lock pages in memory' given to the current user. SIGN OUT AND IN, then use PS3_LARGE_PAGES=1.\n");
}
static bool alloc_guest_vm_large(const char* spec)
{
    const SIZE_T lps = GetLargePageMinimum();
    if (!lps) { fprintf(stderr, "[LGPG] no large-page support -> off\n"); return false; }
    if (!lp_privilege()) return false;
    enum { MAXR = 8 };
    unsigned long long lo[MAXR], hi[MAXR]; int n = 0;
    const char* p = (spec[0] == '1' && !spec[1]) ? "0-10000000,40000000-50000000" : spec;
    while (*p && n < MAXR) {
        char* e1; unsigned long long a = strtoull(p, &e1, 16);
        if (*e1 != '-') break;
        char* e2; unsigned long long b = strtoull(e1 + 1, &e2, 16);
        a &= ~(unsigned long long)(lps - 1); b = (b + lps - 1) & ~(unsigned long long)(lps - 1);
        if (b > VM_SIZE) b = VM_SIZE & ~(unsigned long long)(lps - 1);
        if (b > a && (n == 0 || a >= hi[n - 1])) { lo[n] = a; hi[n] = b; n++; }
        else { fprintf(stderr, "[LGPG] bad/unsorted range in '%s' -> off\n", spec); return false; }
        p = e2; if (*p == ',') p++;
    }
    if (!n) { fprintf(stderr, "[LGPG] no ranges in '%s' -> off\n", spec); return false; }
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t* probe = (uint8_t*)VirtualAlloc(NULL, VM_SIZE + lps, MEM_RESERVE, PAGE_NOACCESS);
        if (!probe) { fprintf(stderr, "[LGPG] cannot reserve %llu bytes (%lu)\n", (unsigned long long)(VM_SIZE + lps), GetLastError()); return false; }
        uint8_t* base = (uint8_t*)(((uintptr_t)probe + lps - 1) & ~(uintptr_t)(lps - 1));
        VirtualFree(probe, 0, MEM_RELEASE);
        void* got[2 * MAXR + 1]; int ng = 0; bool fail = false, retry = false;
        unsigned long long cur = 0;
        for (int i = 0; i <= n && !fail; i++) {
            const unsigned long long gend = (i < n) ? lo[i] : VM_SIZE;
            if (gend > cur) {   /* normal gap, demand-committed by vm_commit_veh */
                void* g = VirtualAlloc(base + cur, (SIZE_T)(gend - cur), MEM_RESERVE, PAGE_READWRITE);
                if (!g) { fail = retry = true; break; }
                got[ng++] = g;
            }
            if (i < n) {
                LARGE_INTEGER t0, t1, f; QueryPerformanceCounter(&t0);
                void* l = VirtualAlloc(base + lo[i], (SIZE_T)(hi[i] - lo[i]), MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
                QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&f);
                const DWORD err = GetLastError();
                if (!l) {
                    fprintf(stderr, "[LGPG] large-page piece 0x%08llX-0x%08llX failed (err %lu%s)\n", lo[i], hi[i], err,
                            err == ERROR_NO_SYSTEM_RESOURCES ? ": not enough contiguous free RAM" : "");
                    fail = true; retry = (err == ERROR_INVALID_ADDRESS); break;
                }
                got[ng++] = l;
                fprintf(stderr, "[LGPG] guest 0x%08llX-0x%08llX: %llu MB on %llu KB pages (%.0f ms)\n", lo[i], hi[i],
                        (hi[i] - lo[i]) >> 20, (unsigned long long)lps >> 10, 1000.0 * (t1.QuadPart - t0.QuadPart) / f.QuadPart);
                cur = hi[i];
            }
        }
        if (!fail)   /* self-test: what vm_commit / sys_memory_allocate / the commit VEH will do inside a large piece */
            for (int i = 0; i < n && !fail; i++)
                if (!VirtualAlloc(base + lo[i] + 0x10000, 0x10000, MEM_COMMIT, PAGE_READWRITE)) {
                    fprintf(stderr, "[LGPG] MEM_COMMIT inside a large-page piece is refused (err %lu) -> large pages off "
                                    "(vm_commit would fail there)\n", GetLastError());
                    fail = true;
                }
        if (!fail) {
            vm_base = base;
            fprintf(stderr, "[LGPG] v221B guest arena at %p: %d large-page range(s) + demand-committed gaps\n", (void*)base, n);
            return true;
        }
        for (int k = 0; k < ng; k++) VirtualFree(got[k], 0, MEM_RELEASE);
        if (!retry) return false;
    }
    fprintf(stderr, "[LGPG] could not place the arena (address races) -> off\n");
    return false;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    /* v219 VC_WW=1: write-watched reservation (GetWriteWatch = which pages anybody wrote; hardware dirty bits,
     * no cost on ordinary writes) so the RSX vertex cache can skip re-hashing unchanged meshes. */
    { const char* ww = getenv("VC_WW");
      const DWORD wwf = (ww && *ww == '1') ? MEM_WRITE_WATCH : 0;
      { const char* lpe = getenv("PS3_LARGE_PAGES");   /* v221B (default off) */
        if (lpe && !strcmp(lpe, "grant")) lp_grant();
        else if (lpe && *lpe && *lpe != '0') {
            if (wwf) fprintf(stderr, "[LGPG] PS3_LARGE_PAGES ignored: VC_WW=1 needs the write-watched reservation\n");
            else alloc_guest_vm_large(lpe);
        } }
      if (!vm_base)
      vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE | wwf, PAGE_READWRITE);
      if (!vm_base && wwf) vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE); }
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is committed up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It stops below the 0xE0000000 mark, so ppu_vm_size arms the
     * loader's bounds check for what is left. */
    /* Reserve the full 4 GB guest space, 4 GB-aligned, read/write and lazily
     * backed (MAP_NORESERVE). It must be page-aligned: sys_memory and the
     * mmapper commit/protect sub-ranges with mprotect, which fails with
     * EINVAL on a calloc'd (16-byte-offset) block, so every
     * sys_memory_allocate returned ENOMEM and the title ran on NULL heaps. */
    {
        const size_t want = (size_t)1 << 32;
        uint8_t* raw = (uint8_t*)mmap(nullptr, want * 2, PROT_NONE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (raw == MAP_FAILED) return false;
        uint8_t* al = (uint8_t*)(((uintptr_t)raw + want - 1) & ~(uintptr_t)(want - 1));
        if (al > raw) munmap(raw, (size_t)(al - raw));
        if (raw + want * 2 > al + want) munmap(al + want, (size_t)((raw + want * 2) - (al + want)));
        if (mprotect(al, want, PROT_READ | PROT_WRITE) != 0) return false;
        vm_base = al;
    }
    ppu_vm_size = 0;
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* Double-clicking the exe passes no arguments. Find the disc dump next to the
 * port (<exe dir>\..\..\Infamous (Europe) (Fr,De,Es,It,Nl)\...\EBOOT.elf) and
 * apply the switches run.cmd sets, so the exe works on its own too. */
static char g_default_elf[1024];
static void set_default_env(const char* k, const char* v)
{
    if (getenv(k)) return;
#ifdef _WIN32
    _putenv_s(k, v);
#else
    setenv(k, v, 0);
#endif
}
static const char* find_default_elf(const char* argv0)
{
    char dir[1024] = {0};
#ifdef _WIN32
    GetModuleFileNameA(NULL, dir, sizeof(dir) - 1);
#else
    strncpy(dir, argv0 ? argv0 : "", sizeof(dir) - 1);
#endif
    char* slash = strrchr(dir, '\\');
    char* fslash = strrchr(dir, '/');
    if (fslash > slash) slash = fslash;
    if (slash) *slash = 0; else strcpy(dir, ".");
    static const char* rel[] = {
        "/../../Infamous (Europe) (Fr,De,Es,It,Nl)/PS3_GAME/USRDIR/EBOOT.elf",
        "/../Infamous (Europe) (Fr,De,Es,It,Nl)/PS3_GAME/USRDIR/EBOOT.elf",
    };
    for (const char* r : rel) {
        snprintf(g_default_elf, sizeof(g_default_elf), "%s%s", dir, r);
        FILE* f = fopen(g_default_elf, "rb");
        if (f) { fclose(f); return g_default_elf; }
    }
    return NULL;
}

#ifdef PORT_PGO_GEN
/* v189 PGO training build: write the profile every 30 s as well as at exit, so a crash, a ^C or a closed
 * console still leaves usable data (counters are cumulative; each write replaces the file). */
static DWORD WINAPI pgo_dump_thread(LPVOID)
{
    for (;;) { Sleep(30000); __llvm_profile_write_file(); }
    return 0;
}
#endif
/* v194: settings.ini (written by the in-game Settings menu, F10) -> environment, before anything reads it.
 * Overrides the launcher's values. Only the menu's keys are accepted. */
static void load_settings_ini(void)
{
    FILE* f = fopen("settings.ini", "r");
    if (!f) return;
    static const char* keys[] = { "FULLSCREEN", "PRESENT_VSYNC", "UI_FPS_CAP", "KBM_GLYPHS", "PAD_MOUSE_SPEED", "PAD_RUN_DEFAULT",
                                  "RES_SCALE", "SHADOWS", "TEX_ANISO", "TEX_MIPS",
                                  "UPSCALER", "FSR_FG", "PS3_LANG" };   /* v243K menu rows (were silently dropped) */
    char line[256]; int n = 0;
    while (fgets(line, sizeof line, f)) {
        char* eq = strchr(line, '='); if (!eq || line[0] == ';' || line[0] == '#') continue;
        *eq = 0; char* v = eq + 1; v[strcspn(v, "\r\n")] = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
            if (!strcmp(line, keys[i])) { _putenv_s(line, v); n++; break; }
    }
    fclose(f);
    if (!getenv("INFAMOUS_FPS_CAP_KEY")) _putenv_s("INFAMOUS_FPS_CAP_KEY", "1");   /* the frame limiter hook must exist */
    fprintf(stderr, "[settings] v194 settings.ini: %d values loaded\n", n);
}

/* v224: play launchers redirect stdout to a log file, but the old runner forced
 * that stream unbuffered and the guest TTY path also fflush()ed every write.
 * Besides the write syscall, this serialises every emitting guest thread on the
 * CRT FILE lock. Keep the old immediate behaviour for an interactive console
 * and for diagnostic runs which explicitly request it; otherwise amortise file
 * and pipe output in a process-lifetime buffer. Normal exit and the window-close
 * path both flush all streams. */
static int s_stdout_immediate = 1;
extern "C" int ps3_stdout_immediate(void)
{
    return s_stdout_immediate;
}
/* v241C LOG_ASYNC (default 0 = off = exactly the old direct writes).
 * stdout and stderr are redirected to files by the launchers, and both are unbuffered (_IONBF), so every printf /
 * fprintf of a game thread was a synchronous WriteFile into the log file ON THAT THREAD: b910 main thread 0.5-2 % of
 * its samples inside NtWriteFile from printf (bursts of cellSpurs workload lines, [HITCH] frames with 78 of 176
 * samples in it), the SPURS audio kernel thread (w3) 1.95 % in fprintf(stderr).
 * LOG_ASYNC=1: fd 1 and fd 2 write into anonymous pipes (4 MiB each); a HELPER PROCESS (this exe started with
 * --v241c-logdrain, which runs nothing but the drain loops below) copies the bytes to the original file handles.
 *  - Bytes identical: the CRT still does its text-mode translation on fd 1/2 (the pipe fds are _O_TEXT); the helper
 *    copies raw bytes in order, one thread per stream.
 *  - Nothing is lost on a crash, a fatal exit or the bench runner's kill: data a writer has handed to the pipe sits in
 *    the kernel pipe buffer, which the helper (another process) keeps reading until EOF, i.e. until every write end
 *    is closed = the game process is gone. The helper then appends one [LOG_ASYNC] stats line to the err log.
 *  - A full pipe blocks the writer (identical bytes, like the old slow file write); the helper counts the reads that
 *    found the pipe >= 75 % full ("near-full") so a stall is visible in that stats line.
 *  - The helper runs on the efficiency cores when the CPU is hybrid (CPU sets, only those inside the process affinity
 *    mask; none there = no restriction), off the main/FIFO/render cores. No hard-coded core.
 *  - Any setup failure -> old direct path (logged).
 * LOG_ASYNC_PHASE=0|1: pipes only in PS3_AB phases of that parity (fd 1/2 are re-pointed with _dup2 within ~50 ms of
 * the phase switch; a few lines written right at a switch may land out of order between the two paths). */
#ifdef _WIN32
#include <fcntl.h>
extern "C" volatile int g_ab_phase;
namespace v241c_la {
struct Stream { HANDLE rd, out; unsigned long long bytes, reads, nearfull, maxfill; };
static const DWORD PIPE_SZ = 4u << 20;
static DWORD WINAPI drain(LPVOID p)
{
    Stream* s = (Stream*)p;
    char* buf = (char*)malloc(1 << 18);
    if (!buf) return 0;
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(s->rd, buf, 1u << 18, &n, NULL) || n == 0) break;   /* EOF / broken pipe = writers gone */
        DWORD avail = 0;
        if (PeekNamedPipe(s->rd, NULL, 0, NULL, &avail, NULL)) {
            const unsigned long long fill = (unsigned long long)n + avail;
            if (fill > s->maxfill) s->maxfill = fill;
            if (fill >= (PIPE_SZ / 4) * 3) s->nearfull++;
        }
        s->reads++; s->bytes += n;
        const char* q = buf;
        while (n) { DWORD w = 0; if (!WriteFile(s->out, q, n, &w, NULL) || !w) { n = 0; break; } q += w; n -= w; }
    }
    free(buf);
    return 0;
}
static void place_on_efficiency_cores(void)
{
    typedef BOOL (WINAPI *GSCSI)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG, HANDLE, ULONG);
    typedef BOOL (WINAPI *SPDCS)(HANDLE, const ULONG*, ULONG);
    HMODULE k = GetModuleHandleA("kernel32.dll");
    GSCSI gi = k ? (GSCSI)GetProcAddress(k, "GetSystemCpuSetInformation") : NULL;
    SPDCS sp = k ? (SPDCS)GetProcAddress(k, "SetProcessDefaultCpuSets") : NULL;
    if (!gi || !sp) return;
    ULONG len = 0; gi(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len || len > (1u << 20)) return;
    unsigned char* b = (unsigned char*)malloc(len); if (!b) return;
    if (gi((PSYSTEM_CPU_SET_INFORMATION)b, len, &len, GetCurrentProcess(), 0)) {
        int lo = 256, hi = -1; ULONG ids[256]; ULONG n = 0;
        for (ULONG o = 0; o < len; ) { PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(b + o);
            if (c->Type == CpuSetInformation) { int e = c->CpuSet.EfficiencyClass; if (e < lo) lo = e; if (e > hi) hi = e; }
            o += c->Size; if (!c->Size) break; }
        /* only E-cores inside the process affinity mask (BENCH_AFFINITY / weak-PC runs): never widen it */
        DWORD_PTR pm = 0, sm = 0; if (!GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) pm = 0;
        if (hi > lo && pm)
            for (ULONG o = 0; o < len && n < 256; ) { PSYSTEM_CPU_SET_INFORMATION c = (PSYSTEM_CPU_SET_INFORMATION)(b + o);
                if (c->Type == CpuSetInformation && c->CpuSet.EfficiencyClass == lo && c->CpuSet.Group == 0 &&
                    c->CpuSet.LogicalProcessorIndex < 8 * sizeof(DWORD_PTR) && ((pm >> c->CpuSet.LogicalProcessorIndex) & 1))
                    ids[n++] = c->CpuSet.Id;
                o += c->Size; if (!c->Size) break; }
        if (n) sp(GetCurrentProcess(), ids, n);
    }
    free(b);
}
/* the helper process: argv = exe --v241c-logdrain <rd1> <file1> <rd2> <file2> (inherited handle values) */
static int helper_main(int argc, char** argv)
{
    if (argc < 6) return 3;
    place_on_efficiency_cores();
    Stream s[2];
    for (int i = 0; i < 2; i++) {
        s[i].rd  = (HANDLE)(uintptr_t)_strtoui64(argv[2 + 2 * i], NULL, 16);
        s[i].out = (HANDLE)(uintptr_t)_strtoui64(argv[3 + 2 * i], NULL, 16);
        s[i].bytes = s[i].reads = s[i].nearfull = s[i].maxfill = 0;
    }
    HANDLE t = CreateThread(NULL, 0, drain, &s[0], 0, NULL);
    drain(&s[1]);
    if (t) WaitForSingleObject(t, INFINITE); else drain(&s[0]);
    char ln[400];
    const int m = snprintf(ln, sizeof ln,
        "[LOG_ASYNC] v241C drain helper done: stdout %llu bytes / %llu reads, stderr %llu bytes / %llu reads; max pipe fill "
        "%llu / %llu KiB of %lu; near-full (>=75 %%, writers may have blocked) %llu / %llu\r\n",
        s[0].bytes, s[0].reads, s[1].bytes, s[1].reads, s[0].maxfill >> 10, s[1].maxfill >> 10,
        (unsigned long)(PIPE_SZ >> 10), s[0].nearfull, s[1].nearfull);
    DWORD w = 0; if (m > 0) WriteFile(s[1].out, ln, (DWORD)m, &w, NULL);
    TerminateProcess(GetCurrentProcess(), 0);   /* skip the game's static destructors / PGO profile write */
    return 0;
}
static int s_pfd[2] = { -1, -1 }, s_ofd[2] = { -1, -1 }, s_cur = 0, s_phase = -1;
static void set_path(int on)
{
    if (on == s_cur) return;
    for (int k = 0; k < 2; k++) _dup2(on ? s_pfd[k] : s_ofd[k], 1 + k);
    s_cur = on;
}
static DWORD WINAPI phase_thread(LPVOID)
{
    for (;;) { set_path((g_ab_phase & 1) == s_phase); Sleep(50); }
    return 0;
}
static void start(void)
{
    const char* e = getenv("LOG_ASYNC");
    if (!(e && *e == '1')) return;
    { const char* p = getenv("LOG_ASYNC_PHASE"); if (p && (*p == '0' || *p == '1')) s_phase = *p - '0'; }
    fflush(stdout); fflush(stderr);
    HANDLE rd[2] = { NULL, NULL }, wr[2] = { NULL, NULL }, oh[2] = { NULL, NULL };
    int ok = 1;
    for (int k = 0; k < 2 && ok; k++) {
        const HANDLE cur = (HANDLE)_get_osfhandle(1 + k);
        if (cur == INVALID_HANDLE_VALUE || cur == NULL) { ok = 0; break; }
        if (!DuplicateHandle(GetCurrentProcess(), cur, GetCurrentProcess(), &oh[k], 0, TRUE, DUPLICATE_SAME_ACCESS)) { ok = 0; break; }
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, FALSE };
        if (!CreatePipe(&rd[k], &wr[k], &sa, PIPE_SZ)) { ok = 0; break; }
        if (!SetHandleInformation(rd[k], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) { ok = 0; break; }
    }
    PROCESS_INFORMATION pi = {};
    if (ok) {
        SIZE_T asz = 0; InitializeProcThreadAttributeList(NULL, 1, 0, &asz);
        LPPROC_THREAD_ATTRIBUTE_LIST al = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(asz);
        HANDLE inh[4] = { rd[0], oh[0], rd[1], oh[1] };
        ok = al && InitializeProcThreadAttributeList(al, 1, 0, &asz) &&
             UpdateProcThreadAttribute(al, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inh, sizeof inh, NULL, NULL);
        wchar_t exe[MAX_PATH]; char cmd[600];
        if (ok && GetModuleFileNameW(NULL, exe, MAX_PATH)) {
            snprintf(cmd, sizeof cmd, "inFamousRecomp --v241c-logdrain %llx %llx %llx %llx",
                     (unsigned long long)(uintptr_t)rd[0], (unsigned long long)(uintptr_t)oh[0],
                     (unsigned long long)(uintptr_t)rd[1], (unsigned long long)(uintptr_t)oh[1]);
            wchar_t wcmd[600]; MultiByteToWideChar(CP_ACP, 0, cmd, -1, wcmd, 600);
            STARTUPINFOEXW si = {}; si.StartupInfo.cb = sizeof si; si.lpAttributeList = al;
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;   /* helper's own std handles: none */
            ok = CreateProcessW(exe, wcmd, NULL, NULL, TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                                NULL, NULL, &si.StartupInfo, &pi) ? 1 : 0;
        } else ok = 0;
        if (al) { DeleteProcThreadAttributeList(al); free(al); }
    }
    for (int k = 0; k < 2; k++) {   /* the parent keeps only the write ends */
        if (rd[k]) CloseHandle(rd[k]);
        if (oh[k]) CloseHandle(oh[k]);
    }
    if (ok) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        for (int k = 0; k < 2 && ok; k++) {
            s_ofd[k] = _dup(1 + k);
            s_pfd[k] = _open_osfhandle((intptr_t)wr[k], _O_TEXT);
            if (s_ofd[k] < 0 || s_pfd[k] < 0) ok = 0;
        }
    }
    if (!ok) {
        for (int k = 0; k < 2; k++) { if (s_pfd[k] >= 0) _close(s_pfd[k]); else if (wr[k]) CloseHandle(wr[k]);
                                      if (s_ofd[k] >= 0) _close(s_ofd[k]); s_pfd[k] = s_ofd[k] = -1; }
        fprintf(stderr, "[LOG_ASYNC] v241C setup failed (err %lu) -> direct writes as before\n", GetLastError());
        return;
    }
    fprintf(stderr, "[LOG_ASYNC] v241C ON: stdout/stderr through 4 MiB pipes drained by helper pid %lu%s\n",
            (unsigned long)pi.dwProcessId, s_phase >= 0 ? (s_phase ? " (odd PS3_AB phases only)" : " (even PS3_AB phases only)") : "");
    fflush(stderr);
    if (s_phase < 0) set_path(1);
    else CreateThread(NULL, 0, phase_thread, NULL, 0, NULL);
}
} /* namespace v241c_la */
#endif
#ifdef _WIN32
/* ---------------------------------------------------------------------------
 * v242B CPU feature check (runs as the process ENTRY POINT, before the C runtime and every static constructor --
 * the runtime and the lifted code are built with AVX2/FMA/BMI2 and would die with an illegal instruction on an
 * older or AVX-less CPU before main() could say anything).
 *   AVX2 exe (default build): needs AVX + AVX2 + FMA + BMI1 + BMI2 + LZCNT + MOVBE + POPCNT and OS support for the
 *     AVX state (XCR0). Missing -> start inFamousRecomp_sse4.exe from the same folder (PORT_CPU_COMPAT build, same
 *     command line and std handles, killed with us via a job object) and return its exit code; no such file ->
 *     message box + stderr line, exit 3.
 *   compat exe (PORT_CPU_COMPAT=ON): needs SSE4.1 + SSSE3 (the floor of the runtime/SPU code); missing -> message.
 *   PS3_CPU_CHECK=0 skips the check, PS3_CPU_CHECK=2 makes the AVX2 exe act as if AVX2 were missing (tests the relaunch).
 * Only kernel32/user32 calls here: the CRT is not initialised yet.
 * -----------------------------------------------------------------------*/
#include <intrin.h>
extern "C" unsigned long mainCRTStartup(void*);
static int v242b_cpu_level(void)   /* 2 = AVX2 build OK, 1 = SSE4.1 build OK, 0 = neither */
{
    int r[4];
    __cpuid(r, 0); const int maxl = r[0];
    __cpuid(r, 1); const unsigned c1 = (unsigned)r[2];
    const int sse41 = (c1 >> 19) & 1, ssse3 = (c1 >> 9) & 1, sse2 = 1;
    const int fma = (c1 >> 12) & 1, movbe = (c1 >> 22) & 1, popcnt = (c1 >> 23) & 1, osx = (c1 >> 27) & 1, avx = (c1 >> 28) & 1;
    int avx2 = 0, bmi1 = 0, bmi2 = 0, lzcnt = 0, osavx = 0;
    if (maxl >= 7) { __cpuidex(r, 7, 0); avx2 = (r[1] >> 5) & 1; bmi1 = (r[1] >> 3) & 1; bmi2 = (r[1] >> 8) & 1; }
    __cpuid(r, (int)0x80000000); if ((unsigned)r[0] >= 0x80000001u) { __cpuid(r, (int)0x80000001); lzcnt = (r[2] >> 5) & 1; }
    if (osx) { unsigned lo, hi; __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0)); osavx = (lo & 6u) == 6u; (void)hi; }
    if (sse2 && ssse3 && sse41 && avx && osavx && avx2 && fma && bmi1 && bmi2 && lzcnt && movbe && popcnt) return 2;
    if (sse2 && ssse3 && sse41) return 1;
    return 0;
}
static void v242b_err(const char* t)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE); DWORD w;
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, t, (DWORD)lstrlenA(t), &w, NULL);
}
static __declspec(noinline) void v242b_cpu_fallback(int lvl, int need)
{
    if (need == 2 && lvl >= 1) {
        WCHAR path[2048]; DWORD n = GetModuleFileNameW(NULL, path, 2000);
        while (n && path[n - 1] != L'\\' && path[n - 1] != L'/') n--;
        const WCHAR* nm = L"inFamousRecomp_sse4.exe";
        for (int i = 0; nm[i] && n < 2040; i++) path[n++] = nm[i];
        path[n] = 0;
        if (n && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
            v242b_err("[CPU] v242B: this CPU lacks AVX2/FMA/BMI2 -> starting the compatibility build inFamousRecomp_sse4.exe\n");
            STARTUPINFOW si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE); si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            HANDLE hs[3] = { si.hStdInput, si.hStdOutput, si.hStdError };
            for (int i = 0; i < 3; i++) if (hs[i] && hs[i] != INVALID_HANDLE_VALUE) SetHandleInformation(hs[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof pi);
            HANDLE job = CreateJobObjectW(NULL, NULL);
            if (job) { JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; ZeroMemory(&li, sizeof li);
                       li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                       SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li); }
            if (CreateProcessW(path, GetCommandLineW(), NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
                if (job) AssignProcessToJobObject(job, pi.hProcess);
                ResumeThread(pi.hThread); CloseHandle(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
                ExitProcess(code);
            }
            v242b_err("[CPU] v242B: could not start inFamousRecomp_sse4.exe\n");
        }
    }
    const char* m = (need == 2 && lvl >= 1)
        ? "This processor has no AVX2/FMA/BMI2 (Intel before Haswell 2013, Pentium/Celeron without AVX, AMD before Excavator 2015).\n\n"
          "The compatibility build inFamousRecomp_sse4.exe was not found next to inFamousRecomp.exe. Put it there and start the game again."
        : "This processor lacks SSE4.1/SSSE3, which this game needs (Intel Core 2 Penryn 2008 or newer, AMD Bulldozer 2011 or newer).";
    v242b_err("[CPU] v242B: unsupported CPU: "); v242b_err(m); v242b_err("\n");
    MessageBoxA(NULL, m, "inFamous -- CPU not supported", MB_OK | MB_ICONERROR);
    ExitProcess(3);
}
static __declspec(noinline) int v242b_check_env(void)   /* PS3_CPU_CHECK: 0 = skip, 2 = simulate missing AVX2 */
{
    WCHAR v[8]; const DWORD n = GetEnvironmentVariableW(L"PS3_CPU_CHECK", v, 8);
    return (n && n < 8) ? (int)(v[0] - L'0') : 1;
}
#if defined(PORT_CPU_COMPAT)
#define V242B_NEED 1
#else
#define V242B_NEED 2
#endif
extern "C" __declspec(safebuffers) unsigned long v242b_entry(void* peb)
{
    const int mode = v242b_check_env();
    if (mode != 0) {
        int lvl = v242b_cpu_level();
        if (mode == 2 && V242B_NEED == 2 && lvl == 2) lvl = 1;
        if (lvl < V242B_NEED) v242b_cpu_fallback(lvl, V242B_NEED);
    }
    return mainCRTStartup(peb);
}
#endif
static void configure_stdout_buffering(void)
{
    const char* force = getenv("PS3_STDOUT_UNBUFFERED");
#ifdef _WIN32
    const int interactive = _isatty(_fileno(stdout));
#else
    const int interactive = isatty(fileno(stdout));
#endif
    /* LOG_ASYNC=1: fd 1 writes into the drain pipe (v241c_la), so an unbuffered stdout costs no file I/O on the
     * writing thread -- and nothing stays in a process buffer that a killed game (black screen -> Task Manager)
     * would lose: the D3D12 device-removed / fence-stuck lines are exactly what a bug report needs. */
    const char* la = getenv("LOG_ASYNC");
    s_stdout_immediate = interactive || (force && *force && *force != '0') || (la && *la == '1');
    if (s_stdout_immediate) {
        setvbuf(stdout, NULL, _IONBF, 0);
    } else {
        static char stdout_buffer[256 * 1024];
        setvbuf(stdout, stdout_buffer, _IOFBF, sizeof stdout_buffer);
    }
}
int main(int argc, char** argv)
{
#ifdef _WIN32
    /* v241C LOG_ASYNC drain helper process (see v241c_la): nothing else runs in it */
    if (argc >= 2 && strcmp(argv[1], "--v241c-logdrain") == 0) return v241c_la::helper_main(argc, argv);
    /* v242B PS3_AFFINITY=<hex mask> (default unset = nothing): restrict the whole process to those logical CPUs
     * BEFORE any thread exists (weak-PC emulation / user choice). bench_drive's BENCH_AFFINITY only applies the mask
     * after the start, so threads sized at boot saw all CPUs. Bits outside the system mask are ignored. */
    unsigned long long v242b_aff = 0;
    { const char* a = getenv("PS3_AFFINITY");
      if (a && *a) { DWORD_PTR pm = 0, sm = 0; unsigned long long m = strtoull(a, NULL, 16);
        if (m && GetProcessAffinityMask(GetCurrentProcess(), &pm, &sm)) { m &= (unsigned long long)sm;
          if (m && SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)m)) v242b_aff = m; } } }
#endif
    /* Must precede the first access to stdout (including the no-argument boot
     * messages below); changing buffering after I/O is undefined by stdio. */
    configure_stdout_buffering();
    load_settings_ini();
    /* v242B LOW_CPU=1 (default OFF): opt-in preset for weak CPUs. These are QUALITY levers -- the game looks/animates
     * differently from the default -- so the preset only exists for users who choose it:
     *   ANIM_LOD=1        (v216) low-detail skeletons (job size <= ANIM_LOD_MAX) update their animation every other
     *                     frame; the player (Cole) is never skipped. Less PPU (w16) + SPU (w2) work per frame.
     *   ADAPT_LOD=1, ADAPT_LOD_MIN=0.7  (v237B) below 58 fps the adaptive draw distance may go down to 70 % of the
     *                     original console distances (default floor 1.0 = console) -> fewer objects culled/drawn.
     * The preset WINS over the launcher's values of these keys (play_pgo.cmd / ship_env set ANIM_LOD=0). */
    { const char* lc = getenv("LOW_CPU");
      if (lc && *lc == '1') {
          static const char* kv[][2] = { { "ANIM_LOD", "1" }, { "ADAPT_LOD", "1" }, { "ADAPT_LOD_MIN", "0.7" } };
          for (size_t i = 0; i < sizeof kv / sizeof kv[0]; i++) {
              _putenv_s(kv[i][0], kv[i][1]);
          }
          fprintf(stderr, "[LOW_CPU] v242B low-end preset ON (quality levers): ANIM_LOD=%s ADAPT_LOD=%s ADAPT_LOD_MIN=%s\n",
                  getenv("ANIM_LOD"), getenv("ADAPT_LOD"), getenv("ADAPT_LOD_MIN"));
      } }
#ifdef _WIN32
    v241c_la::start();   /* v241C LOG_ASYNC=1 (default off; after settings.ini, which may set it) */
    if (v242b_aff) fprintf(stderr, "[CPU] v242B PS3_AFFINITY: process limited to mask 0x%llX (%d logical CPUs)\n",
                           v242b_aff, (int)__builtin_popcountll(v242b_aff));
    fprintf(stderr, "[CPU] v242B %s build, CPU feature level %d (2 = AVX2/FMA/BMI2, 1 = SSE4.1 only)\n",
#if defined(PORT_CPU_COMPAT)
            "SSE4.1 compatibility (PORT_CPU_COMPAT)",
#else
            "AVX2",
#endif
            v242b_cpu_level());
#endif
#ifdef PORT_PGO_GEN
    CreateThread(NULL, 0, pgo_dump_thread, NULL, 0, NULL);
    fprintf(stderr, "[PGO] v189 instrumented build: profile written every 30 s + at exit (LLVM_PROFILE_FILE)\n");
#endif
    char* default_argv[3] = {0};
    if (argc < 2) {
        const char* elf = find_default_elf(argv[0]);
        if (!elf) {
            printf("usage: %s <PPU ELF>\n", argv[0]);
            return 2;
        }
        printf("[boot] no ELF given, using %s\n", elf);
        set_default_env("RD_SPU_INTERP", "1");
        set_default_env("INFAMOUS_BINK_COMPLETION", "1");
        default_argv[0] = argv[0];
        default_argv[1] = (char*)elf;
        argc = 2;
        argv = default_argv;
    }
    /* SP Movie Player pool handshake (func_0018621C) waits on its lwcond once,
     * without a predicate loop -> those two waits must really block until the
     * main thread signals. */
    set_default_env("PS3_LWCOND_STRICT_LR", "18631C,18638C");
    /* Run the interpreted Bink worker as a persistent SPU thread (boot once,
     * resume at its receive-event wait) instead of re-booting it per frame.
     * RD_SPU_PERSIST=0 reverts. */
    set_default_env("RD_SPU_PERSIST", "1");
    /* Run the Bink decoder as the lifted (native) SPU thread it is, with a real
     * sys_spu_thread_receive_event, instead of re-running it in the SPU
     * interpreter on the movie thread for every frame.
     * ~07:00. SPU_LIFTED_RECV=0 reverts to the interpreted path. */
    set_default_env("SPU_LIFTED_RECV", "1");
    /* Direct tail calls between lifted SPU blocks of a raw SPU thread (needs
     * tools/spu_fasttail.py applied to the image; currently the Bink worker).
     * ~15% faster movie decode. SPU_FAST_TAIL=0 reverts. */
    set_default_env("SPU_FAST_TAIL", "1");

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    if (getenv("PS3_FAST_HOST")) {
        SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
        PROCESS_POWER_THROTTLING_STATE pts = {0};
        pts.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pts.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        pts.StateMask = 0;  /* explicitly opt out of EcoQoS/background throttling */
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &pts, sizeof pts);
    }
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    int rc = ppu_run(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
