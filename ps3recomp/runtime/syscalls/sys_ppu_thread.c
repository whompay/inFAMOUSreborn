/*
 * ps3recomp - PPU thread management syscalls (implementation)
 */

#include <setjmp.h>

/* sys_ppu_thread_exit never returns on hardware. Returning CELL_OK to the
 * guest lets it run on past the call: Twisted Metal has a thread that exits
 * from inside a loop and so called exit 1,435 times in one run, staying alive
 * and holding whatever it held. Arm a jump in the thread proc and unwind to it
 * instead. PS3_NO_THREAD_EXIT_UNWIND=1 restores the old behaviour. */
#if defined(_WIN32)
#  define PPU_TLS __declspec(thread)
#else
#  define PPU_TLS __thread
#endif
static PPU_TLS jmp_buf s_exit_jmp;
static PPU_TLS int     s_exit_armed = 0;

#include <stddef.h>
#include "sys_ppu_thread.h"
#include "../platform/win32_compat.h"   /* GetCurrentThreadId on POSIX */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   /* getenv (else return value truncated to int on x64) */
#ifndef _WIN32
#include <errno.h>
#endif

/* Host stack reserved per guest thread, for pthread_attr_setstacksize. The
 * same 256 MB the Win32 branch hands _beginthreadex a few hundred lines
 * down; keep the two in step so a stack-depth bug reproduces on both. */
#define PPU_HOST_STACK_BYTES  (256u * 1024u * 1024u)

#ifdef _WIN32
#include <process.h>   /* _beginthreadex: CRT-aware thread creation (raw CreateThread
                        * leaves per-thread CRT state uninit -> buffered fread() silently
                        * returns 0 on those threads). */
#endif

/* ---------------------------------------------------------------------------
 * Globals
 * -----------------------------------------------------------------------*/
ppu_thread_info       g_ppu_threads[PPU_THREAD_MAX];
vm_stack_alloc        g_vm_stack_alloc;
ppu_thread_entry_fn   g_ppu_thread_entry_trampoline = NULL;

/* Simple mutex for thread table access */
#ifdef _WIN32
static CRITICAL_SECTION s_table_lock;
static int              s_table_lock_init = 0;
#else
static pthread_mutex_t  s_table_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static void table_lock(void)
{
#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
    EnterCriticalSection(&s_table_lock);
#else
    pthread_mutex_lock(&s_table_lock);
#endif
}

static void table_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_table_lock);
#else
    pthread_mutex_unlock(&s_table_lock);
#endif
}

/* Find a free slot. Returns index or -1. Must be called under lock. */
static int find_free_slot(void)
{
    for (int i = 0; i < PPU_THREAD_MAX; i++) {
        if (g_ppu_threads[i].state == PPU_THREAD_STATE_FREE)
            return i;
    }
    return -1;
}

/* Find thread by ID. The ID is the index + 1 (0 is invalid). */
static ppu_thread_info* find_thread(uint64_t thread_id)
{
    if (thread_id == 0 || thread_id > PPU_THREAD_MAX) return NULL;
    ppu_thread_info* t = &g_ppu_threads[thread_id - 1];
    if (t->state == PPU_THREAD_STATE_FREE) return NULL;
    return t;
}

/* ---------------------------------------------------------------------------
 * Host thread entry point
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
static DWORD WINAPI ppu_host_thread_proc(LPVOID param)
#else
static void* ppu_host_thread_proc(void* param)
#endif
{
    ppu_thread_info* info = (ppu_thread_info*)param;

#ifdef _WIN32
    /* Reserve stack for the STACK_OVERFLOW handler, same as main() does for the
     * main thread. Deep recompiled call chains DO overflow even a 256 MB stack
     * (a lifter bug that turns a tail call into recursion is unbounded), and
     * without the guarantee the handler faults while reporting -- the process
     * then dies silently with an access violation INSIDE the handler and the
     * backtrace that would name the recursing function is lost. */
    { ULONG g = 256 * 1024; SetThreadStackGuarantee(&g); }
#endif

    /* Register this thread's context for lwarx/stwcx cross-thread reservation
     * invalidation (ppu_loader.cpp) -- so a concurrent stwcx breaks this thread's
     * reservation and prevents ABA corruption of the guest's lock-free lists. */
    { extern void ppu_resv_register(ppu_context*); ppu_resv_register(&info->ctx); }

#ifdef _WIN32
    /* v183 AUDIO_THREAD_PCORE (default 1; 0 = off): keep the sound pipeline's PPU threads (brbDriver/brbClient =
     * the SPU mixer's driver, snd_stream_service_thread, Bink Audio) on the performance cores. They sleep ~97% of
     * the time, so on a hybrid CPU Windows tends to park them on E-cores; their short bursts of PPU work (building
     * the next mix pass) then run ~2x slower, which is on the mixer's critical path in heavy scenes. Same CPU-set
     * restriction the main and SPU threads already use; priority stays HIGHEST (v144). */
    { static int s_ap = -1; if (s_ap < 0) { const char* e = getenv("AUDIO_THREAD_PCORE"); s_ap = (e && *e == '0') ? 0 : 1; }
      if (s_ap && (strstr(info->name, "brb") || strstr(info->name, "snd_stream") || strstr(info->name, "Audio"))) {
          extern void ps3_thread_prefer_pcores(const char* who, int prio);
          ps3_thread_prefer_pcores(info->name, THREAD_PRIORITY_HIGHEST);
      } }
#endif

    fprintf(stderr, "[THREAD %llu] host thread started, entry=0x%08llX hosttid=%lu\n",
            (unsigned long long)info->ctx.thread_id,
            (unsigned long long)info->entry_addr, (unsigned long)GetCurrentThreadId());

    /* Invoke the recompiled entry point */
    if (g_ppu_thread_entry_trampoline) {
        s_exit_armed = 1;
        if (setjmp(s_exit_jmp) == 0)
            g_ppu_thread_entry_trampoline(&info->ctx);
        else
            fprintf(stderr, "[THREAD %llu] unwound out of sys_ppu_thread_exit\n",
                    (unsigned long long)info->ctx.thread_id);
        s_exit_armed = 0;
        fprintf(stderr, "[THREAD %llu] entry RETURNED (r3=0x%llX) -- thread finished\n",
                (unsigned long long)info->ctx.thread_id,
                (unsigned long long)info->ctx.gpr[3]);

        /* An established interrupt handler does not finish when its body
         * returns. lv2 re-enters such a thread at its ENTRY on the next
         * interrupt -- ps1_netemu's two SPU handlers both end in `blr` right
         * after their `sc 88` (eoi), so a return IS the end of one pass, not
         * the end of the thread. Wait for the next interrupt and run it again;
         * without this each handler ran once and the SPUs were never answered,
         * which is why SPUs 0-3 parked on SPU_RdInMbox and SPU 4 exited. */
        { extern int ps3_intr_is_handler(unsigned long long);
          extern int ps3_intr_wait(unsigned long long);
          if (ps3_intr_is_handler(info->ctx.thread_id)) {
              fprintf(stderr, "[intr] thread %llu is an interrupt handler -- \n",
                      (unsigned long long)info->ctx.thread_id);
              while (ps3_intr_wait(info->ctx.thread_id)) {
                  s_exit_armed = 1;
                  if (setjmp(s_exit_jmp) == 0)
                      g_ppu_thread_entry_trampoline(&info->ctx);
                  s_exit_armed = 0;
              }
          } }
    } else {
        fprintf(stderr, "[THREAD %llu] g_ppu_thread_entry_trampoline is NULL — thread is a no-op!\n",
                (unsigned long long)info->ctx.thread_id);
    }

    /* Mark as finished */
    table_lock();
    info->exit_status = (int64_t)info->ctx.gpr[3];

    if (info->state == PPU_THREAD_STATE_DETACHED) {
        /* Detached threads self-clean */
        info->state = PPU_THREAD_STATE_FREE;
    } else {
        info->state = PPU_THREAD_STATE_FINISHED;
    }
    table_unlock();

    /* Signal anyone waiting for join */
#ifdef _WIN32
    SetEvent(info->finish_event);
    return 0;
#else
    pthread_mutex_lock(&info->finish_mutex);
    info->finished = 1;
    pthread_cond_signal(&info->finish_cond);
    pthread_mutex_unlock(&info->finish_mutex);
    return NULL;
#endif
}

/* ---------------------------------------------------------------------------
/* PPU_THREADGATE: PS3 priority scheduling — a newly created same/lower-priority
 * thread does NOT run until the creating thread blocks. Our HLE spawns host threads
 * immediately, so a worker (GThread entry=0x5353C0) can read its job object's
 * [arg+0x10] owner link BEFORE the main thread finishes linking it -> null -> spin.
 * Fix: create workers SUSPENDED and resume them only when a thread first blocks on
 * an event-queue wait (by then the creator has finished initialization). */
#ifdef _WIN32
static HANDLE g_gate_pending[256];
static int    g_gate_n = 0;
static int    g_gate_on = -1;
void ydkj_release_pending_threads(void)
{
    if (g_gate_on <= 0) return;
    table_lock();
    int n = g_gate_n; g_gate_n = 0;
    for (int i = 0; i < n; i++) if (g_gate_pending[i]) ResumeThread(g_gate_pending[i]);
    table_unlock();
    if (n) fprintf(stderr, "[THREADGATE] released %d pending worker thread(s) on first block\n", n);
}
#else
void ydkj_release_pending_threads(void) {}
#endif

/* ---------------------------------------------------------------------------

 * Main-thread registration. The main guest thread used to run with
 * thread_id 0 -- an id every owner-tracking primitive treated specially:
 * sys_lwmutex mapped 0 to owner id 1 (COLLIDING with the first created
 * thread, so main and that thread mutually satisfied each other's recursive
 * re-lock check and both "owned" the lock -- LBP: main + "bringup" emitted
 * GCM concurrently and fences vanished), and sys_mutex uses owner_tid==0 as
 * its FREE marker (a mutex held by main read as free). Claim slot 0 (id 1)
 * for main before any sys_ppu_thread_create so every thread has a unique
 * nonzero id and the special cases die.
 * -----------------------------------------------------------------------*/
uint64_t ppu_thread_register_main(void)
{
    table_lock();
    ppu_thread_info* t = &g_ppu_threads[0];
    if (t->state == PPU_THREAD_STATE_FREE) {
        memset(t, 0, sizeof(*t));
        t->ctx.thread_id = 1;
        t->state    = PPU_THREAD_STATE_RUNNING;
        t->joinable = 0;                    /* nobody joins the main thread */
        strncpy(t->name, "main", sizeof(t->name) - 1);
#ifdef _WIN32
        t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
        pthread_mutex_init(&t->finish_mutex, NULL);
        pthread_cond_init(&t->finish_cond, NULL);
#endif
    }
    table_unlock();
    return 1;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_create
 *
 * r3 = pointer to receive thread ID (u64*)
 * r4 = entry point address
 * r5 = argument (passed in new thread's r3)
 * r6 = priority (s32)
 * r7 = stack size
 * r8 = flags
 * r9 = thread name pointer
 * -----------------------------------------------------------------------*/

/* CPU-time accessor for the lwmutex convoy trace: user+kernel CPU consumed by
 * guest thread `tid`'s HOST thread, in microseconds. Distinguishes a lock
 * holder that is CPU-BOUND in recompiled code (cpu delta ~= wall delta) from
 * one BLOCKED in a wait (cpu delta ~= 0). Returns 0 if unknown. */
unsigned long long ppu_thread_cpu_us(unsigned tid)
{
#ifdef _WIN32
    if (tid < 2 || tid > PPU_THREAD_MAX) return 0;   /* main (tid 1) not in table */
    ppu_thread_info* t = &g_ppu_threads[tid - 1];
    if (t->state == PPU_THREAD_STATE_FREE || !t->host_thread) return 0;
    FILETIME c, e, k, u;
    if (!GetThreadTimes(t->host_thread, &c, &e, &k, &u)) return 0;
    ULARGE_INTEGER ku, uu;
    ku.LowPart = k.dwLowDateTime; ku.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (ku.QuadPart + uu.QuadPart) / 10ull;      /* 100ns -> us */
#else
    (void)tid; return 0;
#endif
}
/* Last syscall/HLE callsite of guest thread `tid` (see prof_pc). */
unsigned ppu_thread_prof_pc(unsigned tid)
{
    if (tid < 2 || tid > PPU_THREAD_MAX) return 0;
    ppu_thread_info* t = &g_ppu_threads[tid - 1];
    if (t->state == PPU_THREAD_STATE_FREE) return 0;
    return t->prof_pc;
}

/* Profiler accessor: snapshot slot idx (0-based). Returns 1 if the slot holds
 * a live thread, filling tid/cia/name. Lets a host-side sampling profiler
 * iterate guest threads without depending on ppu_thread_info's layout. */
static unsigned s_prof_main_pc = 0;   /* main guest thread: ctx lives outside the table */

int ppu_prof_snapshot(int idx, unsigned* tid, unsigned* cia, const char** name)
{
    if (idx < 0 || idx >= PPU_THREAD_MAX) return 0;
    ppu_thread_info* t = &g_ppu_threads[idx];
    if (idx == 0 && t->state == PPU_THREAD_STATE_FREE) {
        /* slot 0 stays FREE (the main thread never registers) -- serve its
         * dispatcher breadcrumb here so the profiler sees tid 1. */
        *tid = 1; *cia = s_prof_main_pc; *name = "main";
        return s_prof_main_pc != 0;
    }
    if (t->state == PPU_THREAD_STATE_FREE) return 0;
    *tid  = (unsigned)(idx + 1);
    *cia  = t->prof_pc ? t->prof_pc : (unsigned)t->ctx.cia;
    *name = t->name;
    return 1;
}

/* Called from the lv2/HLE dispatchers with the guest ctx (== &info->ctx). */
void ppu_prof_stamp(void* vctx, unsigned lr)
{
    /* container-of: every dispatched ctx is embedded in its ppu_thread_info */
    char* p = (char*)vctx - offsetof(ppu_thread_info, ctx);
    ppu_thread_info* t = (ppu_thread_info*)p;
    int in_range = (t >= g_ppu_threads && t < g_ppu_threads + PPU_THREAD_MAX);
    if (!in_range) { s_prof_main_pc = lr; return; }
    { static int _n = 0; if ((_n < 0 && _n++ < 0))
        fprintf(stderr, "[prof-stamp] ctx=%p base=%p in_range=%d lr=0x%X\n",
                vctx, (void*)g_ppu_threads, in_range, lr); }
    if (in_range)
        t->prof_pc = lr;
}

int64_t sys_ppu_thread_create(ppu_context* ctx)
{
    uint32_t tid_out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t entry        = LV2_ARG_U64(ctx, 1);
    uint64_t arg          = LV2_ARG_U64(ctx, 2);
    int32_t  priority     = LV2_ARG_S32(ctx, 3);
    uint32_t stack_size   = LV2_ARG_U32(ctx, 4);
    /* uint64_t flags     = LV2_ARG_U64(ctx, 5); */
    uint32_t name_addr    = LV2_ARG_PTR(ctx, 6);

    if (stack_size == 0) stack_size = VM_PPU_STACK_SIZE;
    if (stack_size < 0x4000) stack_size = 0x4000; /* 16 KB minimum */

    table_lock();

    int slot = find_free_slot();
    if (slot < 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }

    ppu_thread_info* t = &g_ppu_threads[slot];
    memset(t, 0, sizeof(*t));

    /* Allocate guest stack */
    uint32_t stack_addr = vm_stack_allocate(&g_vm_stack_alloc, stack_size);
    if (stack_addr == 0) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ENOMEM;
    }

    /* Set up the PPU context for the new thread */
    ppu_context_init(&t->ctx);
    t->ctx.cia = entry;
    t->ctx.gpr[3] = arg;
    ppu_set_stack(&t->ctx, (uint64_t)stack_addr, (uint64_t)stack_size);
    /* Copy TOC from creating thread */
    t->ctx.gpr[2] = ctx->gpr[2];

    uint64_t thread_id = (uint64_t)(slot + 1);
    t->ctx.thread_id = thread_id;

    t->state      = PPU_THREAD_STATE_RUNNING;
    t->priority   = priority;
    t->joinable   = 1;
    t->stack_addr = stack_addr;
    t->stack_size = stack_size;
    t->entry_addr = entry;

    /* Copy thread name if provided */
    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    /* Create synchronization for join */
#ifdef _WIN32
    t->finish_event = CreateEventA(NULL, TRUE, FALSE, NULL);
#else
    pthread_mutex_init(&t->finish_mutex, NULL);
    pthread_cond_init(&t->finish_cond, NULL);
    t->finished = 0;
#endif

    /* Write thread ID to output pointer */
    if (tid_out_addr != 0) {
        uint64_t* out = (uint64_t*)vm_to_host(tid_out_addr);
        /* Store as big-endian u64 */
        uint64_t be_id = thread_id;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        be_id = ((be_id >> 56) & 0xFF) |
                ((be_id >> 40) & 0xFF00) |
                ((be_id >> 24) & 0xFF0000) |
                ((be_id >>  8) & 0xFF000000ULL) |
                ((be_id <<  8) & 0xFF00000000ULL) |
                ((be_id << 24) & 0xFF0000000000ULL) |
                ((be_id << 40) & 0xFF000000000000ULL) |
                ((be_id << 56) & 0xFF00000000000000ULL);
#endif
        *out = be_id;
    }

    fprintf(stderr, "[SYS] sys_ppu_thread_create tid=%llu name=\"%s\" entry=0x%08llX arg=0x%llX stack=0x%X prio=%d\n",
            (unsigned long long)thread_id, t->name, (unsigned long long)entry, (unsigned long long)arg,
            stack_size, priority);
    /* YDKJ: dump the worker arg-object: func_000750A8 (thread body) does
     * this=[arg+0x8], vtable=[arg+0xC], method=[vtable+0]. If this(+0x8) is null
     * the worker dispatches its job on a null object -> construction never runs. */
    { extern uint8_t* vm_base; uint32_t a=(uint32_t)arg;
      if(a && a<0x50000000u && getenv("PPU_THREADARG")){
        #define RB(o) (((uint32_t)vm_base[(a+(o))&0x0FFFFFFFu]<<24)|((uint32_t)vm_base[(a+(o)+1)&0x0FFFFFFFu]<<16)|((uint32_t)vm_base[(a+(o)+2)&0x0FFFFFFFu]<<8)|vm_base[(a+(o)+3)&0x0FFFFFFFu])
        uint32_t self=RB(0x0), thisp=RB(0x8), vtbl=RB(0xC);
        fprintf(stderr,"[THREADARG] arg=0x%08X [+0]=0x%08X this[+8]=0x%08X vtbl[+C]=0x%08X\n", a, self, thisp, vtbl);
        #undef RB
      } }

    /* Diagnostic (YDKJ_NOHDLR): suppress libsre's SPURS handler threads (entry in
     * the libsre image range) -- they assert that the SPU side isn't operational
     * and crash. Skipping them lets the main thread (already past
     * cellSpursInitialize) keep running, to see how far it gets. The thread is
     * "created" (tid returned) but never spawned. */
    if (getenv("YDKJ_NOHDLR") && entry >= 0x30000000 && entry < 0x30040000) {
        fprintf(stderr, "[SYS]   (suppressed libsre handler thread entry=0x%08llX)\n",
                (unsigned long long)entry);
        t->state = PPU_THREAD_STATE_RUNNING; /* leave it parked */
        table_unlock();
        return CELL_OK;
    }

    /* Create the host thread. Give it a large RESERVED stack: each recompiled
     * guest call is a real host call, so deep guest call chains nest deeply on
     * the host stack and overflow the 1 MB default. Reserve 256 MB (committed
     * lazily by the OS via STACK_SIZE_PARAM_IS_A_RESERVATION). */
#ifdef _WIN32
    if (g_gate_on < 0) g_gate_on = getenv("PPU_THREADGATE") ? 1 : 0;
    /* Gate only guest worker threads (game .text entry), never libsre/system threads. */
    unsigned _initflag = STACK_SIZE_PARAM_IS_A_RESERVATION;
    int _gate_this = (g_gate_on > 0 && entry >= 0x10000 && entry < 0x10000000);
    if (_gate_this) _initflag |= CREATE_SUSPENDED;
    t->host_thread = (HANDLE)_beginthreadex(NULL, 256u * 1024 * 1024,
                                  (unsigned (__stdcall*)(void*))ppu_host_thread_proc, t,
                                  _initflag, (unsigned*)&t->host_tid);
    if (t->host_thread == NULL) {
        t->state = PPU_THREAD_STATE_FREE;
        CloseHandle(t->finish_event);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
    if (_gate_this && g_gate_n < 256) g_gate_pending[g_gate_n++] = t->host_thread;
    /* The guest's top priorities (0 is highest, 3071 lowest) are its audio and
     * I/O pollers. At normal host priority they wake late under a busy frame:
     * The Simpsons Arcade Game's music thread (prio 0) polls the audio read
     * index every 2.5 ms and misses a 5.3 ms block whenever it is delayed, and
     * each miss is an audible gap. A mild boost, not TIME_CRITICAL: these
     * threads sleep between polls.
     * ponytail: two host levels only; map the full range if a title needs it. */
    if (priority < 256)
        SetThreadPriority(t->host_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    /* v144 AUDIO_THREAD_BOOST (default 1): inFamous' sound pipeline threads (brbDriver/brbClient = the
     * mixer driver, snd_stream_service_thread x16 = streamed sounds such as Cole's powers at guest prio 401,
     * "Bink Audio Thread") run at HIGHEST so the ~20 busy ABOVE_NORMAL SPU lane threads cannot delay them.
     * play139-143: the game's mixer output arrives in bursts with stalls of tens of ms (holes) while the SPU
     * mixer itself (w3) has its own core and is not always saturated. */
    { static int s_ab = -1; if (s_ab < 0) { const char* e = getenv("AUDIO_THREAD_BOOST"); s_ab = (e && *e == '0') ? 0 : 1; }
      if (s_ab && (strstr(t->name, "brb") || strstr(t->name, "snd_stream") || strstr(t->name, "Audio"))) {
          SetThreadPriority(t->host_thread, THREAD_PRIORITY_HIGHEST);
          fprintf(stderr, "[SYS] v144 audio thread \"%s\" (guest prio %d) -> host HIGHEST\n", t->name, priority);
      } }
#else
    /* Same reservation, for the same reason. This is the HOST stack the
     * recompiled C frames run on, not the guest stack (allocated above out of
     * guest VM), and the default is nowhere near enough: 512 KB on Darwin,
     * where a recompiled call chain that spills a whole ppu_context per frame
     * overflows in a few hundred frames. Like the Win32 branch it is a
     * reservation, not a commitment -- the pages are mapped lazily. */
    int rc;
    {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        if (pthread_attr_setstacksize(&attr, PPU_HOST_STACK_BYTES) == 0) {
            rc = pthread_create(&t->host_thread, &attr, ppu_host_thread_proc, t);
        } else {
            rc = EINVAL;
        }
        pthread_attr_destroy(&attr);
        /* A host that will not hand out that much address space (a strict
         * overcommit policy, a low RLIMIT_AS) gets the thread anyway on the
         * default stack. A shallow guest thread runs fine there, and a deep
         * one crashing beats not starting at all. */
        if (rc != 0) {
            fprintf(stderr, "[SYS] tid=%llu: no %zu MB host stack (%d), "
                            "falling back to the default\n",
                    (unsigned long long)thread_id,
                    (size_t)(PPU_HOST_STACK_BYTES / (1024 * 1024)), rc);
            rc = pthread_create(&t->host_thread, NULL, ppu_host_thread_proc, t);
        }
    }
    if (rc != 0) {
        t->state = PPU_THREAD_STATE_FREE;
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
        table_unlock();
        return (int64_t)(int32_t)CELL_EAGAIN;
    }
#endif

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_exit
 *
 * r3 = exit status
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_exit(ppu_context* ctx)
{
    uint64_t status = LV2_ARG_U64(ctx, 0);
    uint64_t tid = ctx->thread_id;
    fprintf(stderr, "[SYS] sys_ppu_thread_exit(tid=%llu status=%llu)\n",
            (unsigned long long)tid, (unsigned long long)status);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (t) {
        t->exit_status = (int64_t)status;
        if (t->state == PPU_THREAD_STATE_DETACHED) {
            t->state = PPU_THREAD_STATE_FREE;
        } else {
            t->state = PPU_THREAD_STATE_FINISHED;
        }

#ifdef _WIN32
        SetEvent(t->finish_event);
#else
        pthread_mutex_lock(&t->finish_mutex);
        t->finished = 1;
        pthread_cond_signal(&t->finish_cond);
        pthread_mutex_unlock(&t->finish_mutex);
#endif
    }
    table_unlock();

    /* Hardware never returns from this. Unwind to the thread proc so the guest
     * cannot keep running past its own exit. */
    {
        static int allow = -1;
        if (allow < 0) allow = getenv("PS3_NO_THREAD_EXIT_UNWIND") ? 0 : 1;
        if (allow && s_exit_armed) { s_exit_armed = 0; longjmp(s_exit_jmp, 1); }
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_join
 *
 * r3 = thread_id
 * r4 = pointer to receive exit status (s64*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_join(ppu_context* ctx)
{
    uint64_t tid          = LV2_ARG_U64(ctx, 0);
    uint32_t status_addr  = LV2_ARG_PTR(ctx, 1);
    { static int n=0; if(n < 30 && n++<30) fprintf(stderr,"[WAIT] ppu_thread_join(tid=%llu)\n", (unsigned long long)tid); }

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }
    if (!t->joinable || t->state == PPU_THREAD_STATE_DETACHED) {
        table_unlock();
        return (int64_t)(int32_t)CELL_EINVAL;
    }
    table_unlock();

    /* Wait for completion */
#ifdef _WIN32
    WaitForSingleObject(t->finish_event, INFINITE);
#else
    pthread_mutex_lock(&t->finish_mutex);
    while (!t->finished) {
        pthread_cond_wait(&t->finish_cond, &t->finish_mutex);
    }
    pthread_mutex_unlock(&t->finish_mutex);
#endif

    /* Write exit status */
    if (status_addr != 0) {
        int64_t es = t->exit_status;
        int64_t* out = (int64_t*)vm_to_host(status_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint64_t u = (uint64_t)es;
        u = ((u >> 56) & 0xFF) |
            ((u >> 40) & 0xFF00) |
            ((u >> 24) & 0xFF0000) |
            ((u >>  8) & 0xFF000000ULL) |
            ((u <<  8) & 0xFF00000000ULL) |
            ((u << 24) & 0xFF0000000000ULL) |
            ((u << 40) & 0xFF000000000000ULL) |
            ((u << 56) & 0xFF00000000000000ULL);
        *out = (int64_t)u;
#else
        *out = es;
#endif
    }

    /* Clean up */
#ifdef _WIN32
    table_lock();
    CloseHandle(t->host_thread);
    CloseHandle(t->finish_event);
    t->host_thread = NULL;
    t->finish_event = NULL;
#else
    /* Reap the host thread with the table lock RELEASED.
     *
     * pthread_join blocks until the thread procedure returns, and that
     * procedure's epilogue takes the table lock to stamp its own state. The
     * joiner is woken well before that epilogue runs, because a guest thread
     * normally ends at sys_ppu_thread_exit, which signals `finished` from
     * INSIDE the thread body and then longjmps back out to the epilogue. So a
     * joiner that holds the lock across the join is waiting for a thread that
     * is waiting for the lock -- and the whole title stops with no message.
     *
     * Windows never saw it: its half of this block is CloseHandle, which
     * records nothing about whether the thread has finished and never waits.
     * The two halves have to be read as one thing, and only one of them was.
     * Take the lock again afterwards for the teardown, which is what actually
     * needs it: nothing can claim this slot in between, because it is still
     * FINISHED rather than FREE until the line below. */
    pthread_join(t->host_thread, NULL);
    table_lock();
    pthread_mutex_destroy(&t->finish_mutex);
    pthread_cond_destroy(&t->finish_cond);
#endif
    t->state = PPU_THREAD_STATE_FREE;
    table_unlock();

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_detach
 *
 * r3 = thread_id
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_detach(ppu_context* ctx)
{
    uint64_t tid = LV2_ARG_U64(ctx, 0);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (t->state == PPU_THREAD_STATE_FINISHED) {
        /* Already finished, free it */
#ifdef _WIN32
        CloseHandle(t->host_thread);
        CloseHandle(t->finish_event);
#else
        pthread_detach(t->host_thread);
        pthread_mutex_destroy(&t->finish_mutex);
        pthread_cond_destroy(&t->finish_cond);
#endif
        t->state = PPU_THREAD_STATE_FREE;
    } else {
        t->state = PPU_THREAD_STATE_DETACHED;
        t->joinable = 0;
#ifndef _WIN32
        pthread_detach(t->host_thread);
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_yield
 * -----------------------------------------------------------------------*/
/* Every bit ever observed in the emulated PS1 I_STAT / I_MASK. */
static uint32_t g_ps1_istat_or, g_ps1_imask_or;
static uint32_t g_ps1_sr_or, g_ps1_cause_or, g_ps1_line_or;
static unsigned long g_cdl_hits;
static uint32_t g_yield_ctr;
static uint32_t g_cdl_v0, g_cdl_s0, g_cdl_s1, g_cdl_s6;

int64_t sys_ppu_thread_yield(ppu_context* ctx)
{
    /* PS1_SPINWAIT=1: who is yielding, and what is it waiting for?
     *
     * The chain, established from lr + ctr on a caught stall: the interpreter
     * calls the event scheduler (bl 0x105fa8, so lr = 0x00106824), the
     * scheduler fires an event callback through bctrl at 0x106050 with
     * ctr = 0x000D2298, and func_000D2298 spins:
     *
     *   000D22C0  lwz  r31, 0(r30)      ; r30 = *(TOC-0x7D4C)
     *   000D22C4  lwz  r0,  0x88(r30)
     *   000D22D8  beq  cr7, 0xd22f4     ; equal -> done waiting
     *   000D22E4  sc   (43 = yield)
     *   000D22F0  bne  cr7, 0xd22e0     ; still unequal -> yield again
     *
     * Deriving r30 statically gave 0x001B9298, whose contents are byte-identical
     * to the ELF image and look like {code,toc} pairs -- so either that
     * derivation is wrong or the struct is never initialised. Read r30 out of
     * the live context instead of deriving it: no TOC arithmetic, no
     * assumption about which callback is running. */
    { static int sw = -1;
      if (sw < 0) sw = getenv("PS1_SPINWAIT") ? 1 : 0;
      if (sw) {
          static unsigned long n;
          extern uint32_t vm_read32(uint64_t);
          const uint32_t r30 = (uint32_t)ctx->gpr[30];
          if (++n <= 6 || (n % 500000ul) == 0)
              fprintf(stderr, "[spinwait] n=%lu lr=%08X ctr=%08X r2=%08X"
                              " r30=%08X w0=%08X w88=%08X r31=%08X\n",
                      n, (uint32_t)ctx->lr, (uint32_t)ctx->ctr,
                      (uint32_t)ctx->gpr[2], r30,
                      vm_read32(r30), vm_read32(r30 + 0x88u),
                      (uint32_t)ctx->gpr[31]);
      } }

    /* PS1_R3000_PC=1: the R3000's LIVE program counter.
     *
     * The interpreter (func_001066A8, 0x1066A8..0x108348) loads PC from its
     * state block at +0x108 on entry and writes it back only in its epilogue --
     * and it never returns, so that field is stale from boot and useless. It
     * keeps the live PC in r26 instead.
     *
     * It also calls sys_ppu_thread_yield from inside its own loop, millions of
     * times (lr=0x00106824 and 0x001068F4). So the guest context arriving here
     * carries r26: the PS1 program counter, sampled for free.
     *
     * This is the only handle on "what is the PS1 actually executing?" -- the
     * emulator retires over a billion R3000 instructions while emitting no GP0
     * drawing commands, and nothing else distinguishes "running the game" from
     * "spinning in a wait loop". */
    { static int pc_on = -1;
      if (pc_on < 0) pc_on = getenv("PS1_R3000_PC") ? 1 : 0;
      if (pc_on) {
          const uint32_t lr = (uint32_t)ctx->lr;
          if (lr >= 0x001066A8u && lr < 0x00108348u) {
              /* Bucket by 4 KB so a tight loop shows as one hot bucket rather
               * than a smear, and report the top few periodically. */
              /* 64-byte buckets, not 4 KB: 4 KB was enough to see the PS1
               * move between BIOS and RAM, but naming the actual loop needs
               * resolution finer than a page. */
              enum { NB = 192 };
              static uint32_t key[NB]; static unsigned long cnt[NB];
              static unsigned long total;
              const uint32_t pc = (uint32_t)ctx->gpr[26];
              /* Accumulate every bit ever seen in the emulated PS1 I_STAT and
               * I_MASK. The state block (*(TOC-0x79FC) = 0x0076C080) holds
               * I_STAT at +0x688 and I_MASK at +0x68C -- read out of
               * func_0010577C, the registered handler for 0x1F801070.
               *
               * A store watch on those words WOULD show every write, but it
               * costs a check on every vm_write32 and slows the run enough to
               * change the outcome: watched runs reach 23 BIOS buckets, unwatched
               * ones 79-87. Same observer effect as the framebuffer dump earlier.
               * Sampling and OR-ing is free, and "which bits ever appeared" is
               * exactly the question. */
              { extern uint32_t vm_read32(uint64_t);
                static uint32_t st_or, mk_or, sr_or, cs_or, ln_or;
                st_or |= vm_read32(0x0076C080u + 0x688u);
                mk_or |= vm_read32(0x0076C080u + 0x68Cu);
                /* Cop0 SR (+0xB0) and CAUSE (+0xB4), plus the PS1 IRQ line flag
                 * (+0x694) -- all read out of func_001063AC, which is the
                 * registered write handler for 0x1F801070 and the ONLY place
                 * that asserts the interrupt:
                 *
                 *   r0 = CAUSE; r0 |= 0x400;  CAUSE = r0     ; assert IP bit 10
                 *   if (!(SR & 1)) skip                      ; SR.IEc
                 *   if (((SR & CAUSE) >> 8) & 0xFF) take_it  ; SR.IM & CAUSE.IP
                 *
                 * The mechanism is present and correct, so whether the
                 * interrupt can ever be TAKEN comes down to SR: bit 0 (IEc)
                 * and bit 10 (IM2, 0x400) both have to be set. */
                sr_or |= vm_read32(0x0076C080u + 0x0B0u);
                cs_or |= vm_read32(0x0076C080u + 0x0B4u);
                ln_or |= vm_read32(0x0076C080u + 0x694u);
                g_ps1_istat_or = st_or; g_ps1_imask_or = mk_or;
                g_ps1_sr_or = sr_or; g_ps1_cause_or = cs_or; g_ps1_line_or = ln_or;
                /* Latch the CD poll loop's own R3000 registers, but ONLY on a
                 * sample where the pc is actually inside CdReadSector
                 * (0xBFC5361C..0xBFC538B0). Reading them unconditionally gave
                 * s1=0 on a run that never reached the loop, and s1 must be 1
                 * there (addiu $s1,$zero,1 at 0xBFC53654) -- so unconditional
                 * reads are meaningless and the self-check correctly rejected
                 * them.
                 *
                 * Register file is at state + reg*4 (lwzx r11,r23,r9 at
                 * 0x1068B0 with r9 = (reg & 0x1F) << 2): $v0 +0x08, $s0 +0x40,
                 * $s1 +0x44, $s6 +0x58. s1 == 1 in the output confirms that. */
                /* Name the R3000 instruction being executed at each yield.
                 *
                 * lr is useless for this: every opcode handler is reached by
                 * bctr, which does not write lr, so a yielding handler carries
                 * the interpreter's last `bl` address instead of its own. The
                 * dispatched target is in ctr, and the instruction itself is at
                 * the R3000 pc -- read it and take its opcode.
                 *
                 * PS1 RAM is little-endian in guest memory (the interpreter uses
                 * lwbrx), so byte-swap. Histogram by primary opcode, and for
                 * SPECIAL (0) by funct, which is what selects the handler. */
                { static unsigned long op_n[64], sp_n[64];
                  static unsigned long tot;
                  const uint32_t ram = vm_read32(0x001BC35Cu);
                  const uint32_t insn =
                      __builtin_bswap32(vm_read32(ram + (pc & 0x001FFFFCu)));
                  const uint32_t op = insn >> 26;
                  op_n[op & 63]++;
                  if (op == 0) sp_n[insn & 63]++;
                  g_yield_ctr = (uint32_t)ctx->ctr;
                  if ((++tot % 200000ul) == 0) {
                      fprintf(stderr, "[yieldop] %lu yields; ctr=0x%08X;"
                                      " top R3000 opcodes:", tot, g_yield_ctr);
                      for (int q = 0; q < 6; q++) {
                          int best = -1; unsigned long bv = 0;
                          for (int k = 0; k < 64; k++)
                              if (op_n[k] > bv) { bv = op_n[k]; best = k; }
                          if (best < 0 || !bv) break;
                          fprintf(stderr, " op%02X=%lu", best, bv);
                          op_n[best] = 0;
                      }
                      for (int q = 0; q < 3; q++) {
                          int best = -1; unsigned long bv = 0;
                          for (int k = 0; k < 64; k++)
                              if (sp_n[k] > bv) { bv = sp_n[k]; best = k; }
                          if (best < 0 || !bv) break;
                          fprintf(stderr, " special:funct%02X=%lu", best, bv);
                          sp_n[best] = 0;
                      }
                      fprintf(stderr, "\n");
                      /* Bug 1: func_00105FA8 fires every event whose due time
                       * has passed and RE-APPENDS the node to the same ring
                       * with its due time UNCHANGED (0x106020..0x10603C), so a
                       * callback that does not re-arm its own node leaves it
                       * permanently due and the loop never reaches the
                       * sentinel. Dump the ring so the culprit names itself.
                       *
                       * Sentinel = the list-head cell at state+0x540; it IS a
                       * node, its own +8 is the stop time. Node layout, read
                       * off the fire path at 0x105FF0..0x106050:
                       *   +0 next  +4 prev  +8 due  +0xC callback OPD
                       *   +0x10 callback arg (loaded into r3)
                       * NOT "+0xC flag / +0x10 OPD" as the commit before this
                       * one said: r10 comes from +0xC and is dereferenced as
                       * the OPD at 0x106040/0x10604C, and +0xC is what gets
                       * zeroed once the node fires. */
                      { const uint32_t st = 0x0076C080u, sent = st + 0x540u;
                        uint32_t n = vm_read32(sent);
                        fprintf(stderr, "[evring] total=%u sent{due=%u next=%08X"
                                        " prev=%08X}",
                                vm_read32(st + 0x124u), vm_read32(sent + 8u),
                                n, vm_read32(sent + 4u));
                        for (int q = 0; q < 8 && n && n != sent; q++) {
                            const uint32_t opd = vm_read32(n + 0xCu);
                            fprintf(stderr, "  [%08X due=%u opd=%08X fn=%08X"
                                            " arg=%08X]",
                                    n, vm_read32(n + 8u), opd,
                                    opd ? vm_read32(opd) : 0u,
                                    vm_read32(n + 0x10u));
                            n = vm_read32(n);
                        }
                        fprintf(stderr, "\n"); }
                  } }
                /* PS1_LOOPWATCH=<hex pc>: sample the R3000 register file only
                 * when the pc is inside a 256-byte window, and report which of
                 * the interesting registers actually CHANGE across samples.
                 *
                 * For the decode loop at 0x80164F00: $a0 is the packed word the
                 * bit-field extractions (srl 19 / srl 22) read, $a1 the output
                 * cursor, $a3 the Huffman table base. If $a0 never changes the
                 * bitstream is not advancing and the loop is chewing one word
                 * forever; if it changes, the input is moving and the fault is
                 * in the extraction or the table lookup. Sampling at yields is
                 * enough because this window already takes 15,213 of them. */
                { static int lw = -2; static uint32_t lwb;
                  if (lw == -2) { const char* e = getenv("PS1_LOOPWATCH");
                                  lw = e ? 1 : 0;
                                  lwb = e ? (uint32_t)strtoul(e, 0, 16) : 0u; }
                  if (lw && (pc & ~0xFFu) == (lwb & ~0xFFu)) {
                      static unsigned long ln;
                      static uint32_t seen_a0[8], seen_a1[8]; static int n0, n1;
                      const uint32_t sb = 0x0076C080u;
                      const uint32_t a0 = vm_read32(sb + 4u * 4u);
                      const uint32_t a1 = vm_read32(sb + 5u * 4u);
                      const uint32_t a3 = vm_read32(sb + 7u * 4u);
                      int f0 = 0, f1 = 0;
                      for (int z = 0; z < n0; z++) if (seen_a0[z] == a0) f0 = 1;
                      for (int z = 0; z < n1; z++) if (seen_a1[z] == a1) f1 = 1;
                      if (!f0 && n0 < 8) seen_a0[n0++] = a0;
                      if (!f1 && n1 < 8) seen_a1[n1++] = a1;
                      if (++ln <= 8 || (ln % 20000) == 0) {
                          fprintf(stderr, "[loopwatch] n=%lu pc=0x%08X"
                                          " a0=%08X a1=%08X a3=%08X"
                                          " distinct[a0=%d a1=%d]\n",
                                  ln, pc, a0, a1, a3, n0, n1);
                      }
                  } }
                if (pc >= 0xBFC5361Cu && pc <= 0xBFC538B0u) {
                    const uint32_t sb = 0x0076C080u;
                    g_cdl_hits++;
                    g_cdl_v0 = vm_read32(sb + 0x08u);
                    g_cdl_s0 = vm_read32(sb + 0x40u);
                    g_cdl_s1 = vm_read32(sb + 0x44u);
                    g_cdl_s6 = vm_read32(sb + 0x58u);
                } }
              /* PS1_PC_CENSUS=1: has the R3000 EVER executed in a given range?
               *
               * The top-N bucket report answers "where is it now"; it cannot
               * answer "did it ever run X", and I have been INFERRING that
               * answer for CdInit from the fact that its stores never land.
               * This measures it instead: a flag per 64-byte bucket across the
               * BIOS, reported once, so "CdInit was entered" becomes an
               * observation rather than a deduction. */
              { static int cen = -1;
                if (cen < 0) cen = getenv("PS1_PC_CENSUS") ? 1 : 0;
                if (cen) {
                    /* 0xBFC00000..0xBFC80000 in 64-byte buckets = 8192 flags */
                    static unsigned char seen[8192];
                    static unsigned long cn = 0;
                    if (pc >= 0xBFC00000u && pc < 0xBFC80000u)
                        seen[(pc - 0xBFC00000u) >> 6] = 1;
                    if ((++cn % 600000ul) == 0) {
                        /* CdInit spans 0xBFC52B9C..0xBFC52C60 -> buckets for
                         * 0xBFC52B80, BC0, C00, C40. Report those explicitly,
                         * plus a total, so a zero is legible. */
                        /* CdInit's four buckets, then the BIOS interrupt
                         * dispatcher's CD branch (0xBFC046AC calls DeliverEvent
                         * for class 3 when I_STAT bit 2 is set) and the bucket
                         * holding the I_STAT read that gates it. If the CD
                         * branch never executes, the CD interrupt never fires. */
                        static const uint32_t probe[6] = {
                            0xBFC52B80u, 0xBFC52BC0u, 0xBFC52C00u, 0xBFC52C40u,
                            0xBFC04680u, 0xBFC046A0u };
                        unsigned tot = 0;
                        for (unsigned q = 0; q < 8192; q++) tot += seen[q];
                        fprintf(stderr, "[census] %lu samples, %u/8192 BIOS buckets"
                                        " ever executed; CdInit:", cn, tot);
                        for (int q = 0; q < 6; q++)
                            fprintf(stderr, " %08X=%d", probe[q],
                                    seen[(probe[q] - 0xBFC00000u) >> 6]);
                        /* The PS1 kernel's A/B/C call gates live at RAM 0xA0,
                         * 0xB0 and 0xC0 -- every BIOS service call jumps there
                         * with the function index in $t1. Whether they execute
                         * at all separates "the game makes no BIOS calls" from
                         * "it makes them, but never asks for CdInit". Address
                         * bits: KUSEG/KSEG0/KSEG1 all alias, so mask to 0x1FFFFF
                         * before bucketing. */
                        { static unsigned char gate[4];
                          const uint32_t phys = pc & 0x1FFFFFu;
                          if (phys < 0x100u) {
                              if (phys >= 0xA0u && phys < 0xB0u) gate[0] = 1;
                              else if (phys >= 0xB0u && phys < 0xC0u) gate[1] = 1;
                              else if (phys >= 0xC0u && phys < 0xD0u) gate[2] = 1;
                              else gate[3] = 1;
                          }
                          fprintf(stderr, "  A-gate=%d B-gate=%d C-gate=%d other-low=%d",
                                  gate[0], gate[1], gate[2], gate[3]); }
                        /* Read the five CD event handles HERE, in the same
                         * report as the census. They used to be read from the
                         * render heartbeat, which is a different thread on a
                         * different schedule -- and comparing a census from one
                         * run against a handle read from another is precisely
                         * the error that produced six retractions. PS1 RAM is
                         * little-endian in guest memory (the interpreter uses
                         * lwbrx), so swap. */
                        { extern uint32_t vm_read32(uint64_t);
                          const uint32_t ram = vm_read32(0x001BC35Cu);
                          static const uint32_t sl[5] = { 0xB218u, 0xB21Cu, 0xB220u,
                                                          0xB224u, 0xB228u };
                          fprintf(stderr, "  handles:");
                          for (int q = 0; q < 5; q++)
                              fprintf(stderr, " %08X",
                                      __builtin_bswap32(vm_read32(ram + sl[q])));
                          /* And each CD event's STATUS, from its EvCB. This is
                           * the delivery question: EvStACTIVE (0x2000) means
                           * open and waiting; EvStALREADY (0x4000) means it has
                           * been DELIVERED and not yet consumed. All 0x2000
                           * forever = the CD interrupt never fires. Read here,
                           * in the same report, deliberately. */
                          { const uint32_t tot = __builtin_bswap32(vm_read32(ram + 0x0120u));
                            const uint32_t tb = tot & 0x1FFFFFu;
                            fprintf(stderr, "  I_STAT_or=%08X I_MASK_or=%08X"
                                          " SR_or=%08X CAUSE_or=%08X line_or=%X",
                                  g_ps1_istat_or, g_ps1_imask_or,
                                  g_ps1_sr_or, g_ps1_cause_or, g_ps1_line_or);
                          /* The CD poll loop's own R3000 registers. The
                           * interpreter addresses the register file at
                           * state + reg*4 -- `lwzx r11, r23, r9` at 0x1068B0
                           * with r9 = (reg & 0x1F) << 2. So $v0 is +0x08,
                           * $s0 +0x40, $s1 +0x44, $s6 +0x58.
                           *
                           * SELF-VALIDATING: the loop at 0xBFC5384C sets
                           * $s1 = 1 (addiu $s1,$zero,1 at 0xBFC53654) and
                           * compares TestEvent's result against it. If $s1
                           * reads 1, the register offset is confirmed. If it
                           * reads anything else, this probe is wrong and the
                           * numbers beside it mean nothing.
                           *
                           * $s6 is the retry counter tested against 10
                           * (slti $v0,$s6,0xa); $v0 is TestEvent's last result. */
                          fprintf(stderr, "  in_cdloop=%lu", g_cdl_hits);
                          if (g_cdl_hits)
                              fprintf(stderr, " r3000[v0=%u s0=%d s1=%u s6=%u]%s",
                                      g_cdl_v0, (int32_t)g_cdl_s0, g_cdl_s1,
                                      g_cdl_s6,
                                      g_cdl_s1 == 1u ? "" : "  <== s1!=1, PROBE INVALID");
                          fprintf(stderr, "  ev[cls/status]:");
                            for (int q = 0; q < 5; q++) {
                                const uint32_t h =
                                    __builtin_bswap32(vm_read32(ram + sl[q]));
                                if ((h >> 24) != 0xF1u) { fprintf(stderr, " -"); continue; }
                                const uint32_t cb = tb + (h & 0xFFFFu) * 0x1Cu;
                                fprintf(stderr, " %X/%04X",
                                        __builtin_bswap32(vm_read32(ram + cb)) & 0xFu,
                                        __builtin_bswap32(vm_read32(ram + cb + 4)) & 0xFFFFu);
                            } }
                        }
                        fprintf(stderr, "\n");
                    }
                } }
              const uint32_t b = pc & ~0x3Fu;
              int i = 0;
              for (; i < NB; i++) { if (cnt[i] && key[i] == b) break;
                                    if (!cnt[i]) { key[i] = b; break; } }
              if (i < NB) cnt[i]++;
              ++total;
              if (total == 20000ul || (total % 200000ul) == 0) {
                  fprintf(stderr, "[r3000pc] %lu yields from the interpreter;"
                                  " hottest PS1 PC buckets:\n", total);
                  for (int k = 0; k < NB; k++) {
                      int best = -1; unsigned long bv = 0;
                      for (int q = 0; q < NB; q++)
                          if (cnt[q] > bv) { bv = cnt[q]; best = q; }
                      if (best < 0 || k >= 6) break;
                      fprintf(stderr, "   pc~0x%08X  %lu (%.1f%%)\n",
                              key[best], cnt[best],
                              100.0 * (double)cnt[best] / (double)total);
                      cnt[best] = 0;   /* consume for this report */
                  }
                  /* The hottest bucket names WHERE the R3000 is; it cannot
                   * say what it is waiting for. Dump the loop body and the
                   * register file at the same instant, from the same thread,
                   * so the two are readable together instead of guessed at.
                   *
                   * `top` is recomputed here because the report loop above
                   * consumes cnt[] as it prints. PS1 RAM is little-endian in
                   * guest memory (the interpreter uses lwbrx), so swap.
                   * Register file: state + reg*4 (lwzx r11, r23, r9 at
                   * 0x1068B0 with r9 = (reg & 0x1F) << 2). */
                  { uint32_t top = 0; unsigned long tv = 0;
                    for (int q = 0; q < NB; q++)
                        if (cnt[q] > tv) { tv = cnt[q]; top = key[q]; }
                    if (!top) top = key[0];
                    { extern uint32_t vm_read32(uint64_t);
                      const uint32_t ram = vm_read32(0x001BC35Cu);
                      const uint32_t base = (top & 0x001FFFC0u);
                      fprintf(stderr, "[r3000mem] %08X:", 0x80000000u | base);
                      for (int q = 0; q < 16; q++)
                          fprintf(stderr, " %08X",
                                  __builtin_bswap32(vm_read32(ram + base + q * 4u)));
                      fprintf(stderr, "\n");
                      { static const char* rn[32] = {
                            "zr","at","v0","v1","a0","a1","a2","a3",
                            "t0","t1","t2","t3","t4","t5","t6","t7",
                            "s0","s1","s2","s3","s4","s5","s6","s7",
                            "t8","t9","k0","k1","gp","sp","fp","ra" };
                        fprintf(stderr, "[r3000reg]");
                        for (int q = 1; q < 32; q++)
                            fprintf(stderr, " %s=%08X", rn[q],
                                    vm_read32(0x0076C080u + (uint32_t)q * 4u));
                        fprintf(stderr, "\n"); } } }
                  for (int q = 0; q < NB; q++) { cnt[q] = 0; key[q] = 0; }
              }
          }
      } }
    (void)ctx;
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_priority
 *
 * r3 = thread_id
 * r4 = pointer to receive priority (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_priority(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t prio_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        /* The main thread (and any thread we didn't spawn via sys_ppu_thread_create)
         * isn't in our table. Returning ESRCH here is fatal for engines that query
         * their own priority at startup (PhyreEngine PApplication::PlatformInit ->
         * "Error initializing PSSG"). Report a sane default priority + success. */
        table_unlock();
        if (prio_addr != 0) {
            int32_t prio = 1000;
            int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            uint32_t u = (uint32_t)prio;
            u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
                ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
            *out = (int32_t)u;
#else
            *out = prio;
#endif
        }
        return CELL_OK;
    }

    if (prio_addr != 0) {
        int32_t prio = t->priority;
        int32_t* out = (int32_t*)vm_to_host(prio_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)prio;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = prio;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_set_priority
 *
 * r3 = thread_id
 * r4 = priority
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_set_priority(ppu_context* ctx)
{
    uint64_t tid      = LV2_ARG_U64(ctx, 0);
    int32_t  priority = LV2_ARG_S32(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    t->priority = priority;
    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_rename
 *
 * r3 = thread_id
 * r4 = name pointer
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_rename(ppu_context* ctx)
{
    uint64_t tid       = LV2_ARG_U64(ctx, 0);
    uint32_t name_addr = LV2_ARG_PTR(ctx, 1);

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        return (int64_t)(int32_t)CELL_ESRCH;
    }

    if (name_addr != 0) {
        const char* name = (const char*)vm_to_host(name_addr);
        strncpy(t->name, name, sizeof(t->name) - 1);
        t->name[sizeof(t->name) - 1] = '\0';
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_join_state
 *
 * r3 = pointer to receive join state (s32*)
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_join_state(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    int32_t joinable = t ? t->joinable : 0;
    table_unlock();

    if (out_addr != 0) {
        int32_t* out = (int32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t u = (uint32_t)joinable;
        u = ((u >> 24) & 0xFF) | ((u >> 8) & 0xFF00) |
            ((u <<  8) & 0xFF0000) | ((u << 24) & 0xFF000000u);
        *out = (int32_t)u;
#else
        *out = joinable;
#endif
    }

    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sys_ppu_thread_get_stack_information
 *
 * r3 = pointer to receive stack info struct
 *   struct { u32 addr; u32 size; }
 * -----------------------------------------------------------------------*/
int64_t sys_ppu_thread_get_stack_information(ppu_context* ctx)
{
    uint32_t out_addr = LV2_ARG_PTR(ctx, 0);
    uint64_t tid = ctx->thread_id;

    table_lock();
    ppu_thread_info* t = find_thread(tid);
    if (!t) {
        table_unlock();
        /* For the main thread, return sensible defaults */
        if (out_addr != 0) {
            uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
            /* byteswap both fields */
            uint32_t sa = VM_STACK_BASE;
            uint32_t ss = VM_PPU_STACK_SIZE;
            sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
                 ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
            ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
                 ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
            out[0] = sa;
            out[1] = ss;
#else
            out[0] = VM_STACK_BASE;
            out[1] = VM_PPU_STACK_SIZE;
#endif
        }
        return CELL_OK;
    }

    if (out_addr != 0) {
        uint32_t* out = (uint32_t*)vm_to_host(out_addr);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(_WIN32)
        uint32_t sa = t->stack_addr;
        uint32_t ss = t->stack_size;
        sa = ((sa >> 24) & 0xFF) | ((sa >> 8) & 0xFF00) |
             ((sa <<  8) & 0xFF0000) | ((sa << 24) & 0xFF000000u);
        ss = ((ss >> 24) & 0xFF) | ((ss >> 8) & 0xFF00) |
             ((ss <<  8) & 0xFF0000) | ((ss << 24) & 0xFF000000u);
        out[0] = sa;
        out[1] = ss;
#else
        out[0] = t->stack_addr;
        out[1] = t->stack_size;
#endif
    }

    table_unlock();
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Registration
 * -----------------------------------------------------------------------*/
void sys_ppu_thread_init(lv2_syscall_table* tbl)
{
    /* Initialize stack allocator */
    vm_stack_alloc_init(&g_vm_stack_alloc);

    /* Clear thread table */
    memset(g_ppu_threads, 0, sizeof(g_ppu_threads));

#ifdef _WIN32
    if (!s_table_lock_init) {
        InitializeCriticalSection(&s_table_lock);
        s_table_lock_init = 1;
    }
#endif

    lv2_syscall_register(tbl, SYS_PPU_THREAD_CREATE,              sys_ppu_thread_create);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_EXIT,                sys_ppu_thread_exit);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_YIELD,               sys_ppu_thread_yield);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_JOIN,                sys_ppu_thread_join);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_DETACH,              sys_ppu_thread_detach);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_JOIN_STATE,      sys_ppu_thread_get_join_state);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_SET_PRIORITY,        sys_ppu_thread_set_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_PRIORITY,        sys_ppu_thread_get_priority);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_GET_STACK_INFORMATION, sys_ppu_thread_get_stack_information);
    lv2_syscall_register(tbl, SYS_PPU_THREAD_RENAME,              sys_ppu_thread_rename);
}

/* ===========================================================================
 * v98b FREEZE WATCHDOG ("[FREEZE]" lines). Always on; FREEZE_DUMP=0 turns it off.
 *
 * Gameplay freeze (play97: characters stop, NPCs vanish, T-pose, Cole hangs in the
 * air) while frames keep coming. play97 shows the main thread keeps submitting and
 * waiting on SPU task lists at the normal rate through the run, so the frozen state
 * is not a hung SPURS list. To make the next run decisive this records, per guest
 * thread, the lv2 syscall / HLE import it is currently inside (number or NID, r3-r7,
 * guest lr, entry time) and dumps every guest thread -- name, prio, call, args, how
 * long it has been blocked, and the state of the object it blocks on -- when:
 *   - the user presses FREEZE_KEY (default F8, game window focused) during a freeze
 *     -> "[FREEZE] ===== dump (key F8) ..."   <-- ask the user to press F8 when frozen
 *   - >= 4 workloads of SPURS instance 0 report idle > SPURS_STALL_MS within 1 s with
 *     no PPU task wait open (the [STALL] "no restart" condition) -> "dump (spurs-idle)"
 *     (rate limited: once per 10 s)
 * Plus, without a trigger:
 *   - "[FREEZE] long block": a thread inside one call for > FREEZE_BLOCK_MS (default
 *     2000) -- once per call, max 400 lines.
 *   - "[FREEZE-SLOW] 10s": every call that took >= 30 ms in the last 10 s, grouped by
 *     call (count, total, max) -- shows e.g. slow cellFsRead (streaming starvation)
 *     or a convoy on one lwmutex next to the normal idle waits.
 * Cost: two 48-byte record copies + one GetTickCount64 per syscall/HLE call.
 * =========================================================================*/
#include "sys_mutex.h"
#include "sys_cond.h"
#include "sys_semaphore.h"
#include "sys_event.h"

typedef struct ps3_fz_rec {
    uint32_t kind;          /* 0 none, 1 lv2 syscall, 2 HLE import */
    uint32_t num;           /* syscall number / NID */
    uint32_t lr, seq;
    uint32_t a[5];          /* r3..r7 */
    uint32_t pad;
    uint64_t t0;            /* fz_now_ms() at entry */
} ps3_fz_rec;
#define FZ_MAXT 64
static ps3_fz_rec s_fz[FZ_MAXT];
static uint32_t s_fz_rep_seq[FZ_MAXT];
static volatile long s_fz_on = -1;
static volatile long s_fz_spurs_req = 0;
extern const char* g_hle_inflight[64];       /* ppu_hle.cpp */
const char* ps3_hle_ctx_name(uint32_t nid);  /* ppu_hle.cpp (v98b) */
extern uint32_t vm_read32(uint64_t);

static uint64_t fz_now_ms(void)
{
#ifdef _WIN32
    /* v103B: QPC instead of GetTickCount64 (15.6 ms ticks made every 16 ms timed wait read as 31-32 ms in
     * [FREEZE-SLOW]; FREEZE_TICKCLOCK=1 = old clock). */
    static LONGLONG s_f = 0; static int s_tick = -1;
    if (s_tick < 0) { const char* e = getenv("FREEZE_TICKCLOCK"); s_tick = (e && *e == '1') ? 1 : 0; }
    if (s_tick) return (uint64_t)GetTickCount64();
    if (!s_f) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_f = f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart / s_f) * 1000u + (uint64_t)((q.QuadPart % s_f) * 1000 / s_f);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}
static volatile uint64_t s_fz_t0_ms = 0;
static double fz_t(void) { return (double)(fz_now_ms() - s_fz_t0_ms) / 1000.0; }
/* v106: the [FREEZE] time base (same t= as "F8 pressed at t=..."), for other modules' logs. */
double ps3_fz_t(void) { return fz_t(); }

/* ---- slow-call table (written by exits, printed by the watchdog) ---- */
#define FZ_SLOW_N 64
static struct { volatile long key; volatile long n; volatile long long tot, mx; uint32_t kind, num; } s_fz_slow[FZ_SLOW_N];
static void fz_slow_add(uint32_t kind, uint32_t num, uint64_t ms)
{
    long key = (long)(((kind & 3u) << 30) | (num & 0x3FFFFFFFu));
    if (kind == 2) key = (long)(0x80000000u | (num >> 1));   /* NIDs: keep 31 bits */
    uint32_t h = ((uint32_t)key * 2654435761u) >> 26;
    for (int i = 0; i < FZ_SLOW_N; i++) {
        int s = (int)((h + (uint32_t)i) & (FZ_SLOW_N - 1));
        long k = s_fz_slow[s].key;
        if (k == 0) { if (InterlockedCompareExchange(&s_fz_slow[s].key, key, 0) == 0) { s_fz_slow[s].kind = kind; s_fz_slow[s].num = num; }
                     else if (s_fz_slow[s].key != key) continue; k = key; }
        if (k != key) continue;
        InterlockedIncrement(&s_fz_slow[s].n);
        s_fz_slow[s].tot += (long long)ms;   /* racy add: diagnostics only */
        if ((long long)ms > s_fz_slow[s].mx) s_fz_slow[s].mx = (long long)ms;
        return;
    }
}

/* ---- v103B [FREEZE-SLOWOBJ]: WHICH lock/queue the >= 30 ms waits were on (exit-exact) ----
 * FREEZE-SLOW only named the call ("sys_lwmutex_lock 23x max 110 ms"); after the first look-at
 * sequence in play99/100/102b those lwmutex waits appear and nobody knew which mutex. Key = (call,
 * object); per entry: count, total, max, waiter tid mask, last caller lr. */
#define FZ_OBJ_N 64
static struct { volatile long key_hi; volatile uint32_t obj; volatile long n; volatile long long tot, mx;
                volatile unsigned long long tids; volatile uint32_t lr, cls; } s_fz_obj[FZ_OBJ_N];
static const char* const s_fz_cls_name[] = { "?", "lwmutex_lock", "lwcond_wait(relock)", "mutex_lock", "cond_wait", "event_queue_receive", "semaphore_wait", "event_flag_wait" };
static uint32_t fz_obj_class(uint32_t kind, uint32_t num)
{
    if (kind == 1) {
        switch (num) { case 102: return 3; case 107: return 4; case 130: return 5; case 92: return 6; case 85: return 7; default: return 0; }
    }
    if (kind == 2) {
        const char* nm = ps3_hle_ctx_name(num);
        if (!nm) return 0;
        if (!strcmp(nm, "sys_lwmutex_lock")) return 1;
        if (!strcmp(nm, "sys_lwcond_wait")) return 2;
    }
    return 0;
}
static void fz_obj_add(uint32_t cls, uint32_t obj, int tid, uint32_t lr, uint64_t ms)
{
    if (!cls) return;
    long key = (long)((cls << 24) | 1u);
    uint32_t h = ((obj ^ (cls * 0x9E3779B9u)) * 2654435761u) >> 26;
    for (int i = 0; i < FZ_OBJ_N; i++) {
        int s = (int)((h + (uint32_t)i) & (FZ_OBJ_N - 1));
        if (s_fz_obj[s].key_hi == 0) {
            if (InterlockedCompareExchange(&s_fz_obj[s].key_hi, key, 0) == 0) { s_fz_obj[s].obj = obj; s_fz_obj[s].cls = cls; }
            else continue;
        }
        while (s_fz_obj[s].cls == 0) { /* publisher between CAS and the stores (tiny window) */ }
        if (s_fz_obj[s].key_hi != key || s_fz_obj[s].obj != obj) continue;
        InterlockedIncrement(&s_fz_obj[s].n);
        s_fz_obj[s].tot += (long long)ms;   /* racy: diagnostics only */
        if ((long long)ms > s_fz_obj[s].mx) s_fz_obj[s].mx = (long long)ms;
        if (tid >= 0 && tid < 64) s_fz_obj[s].tids |= 1ull << tid;
        s_fz_obj[s].lr = lr;
        return;
    }
}
static const char* fz_tname(int tid);
static void fz_obj_report(void)
{
    char b[1600]; int p = snprintf(b, sizeof b, "[FREEZE-SLOWOBJ] 10s waits >= 30 ms by object:");
    int any = 0;
    for (int pass = 0; pass < 6; pass++) {
        int bi = -1; long long bt = 0;
        for (int i = 0; i < FZ_OBJ_N; i++) if (s_fz_obj[i].key_hi && s_fz_obj[i].n > 0 && s_fz_obj[i].tot > bt) { bt = s_fz_obj[i].tot; bi = i; }
        if (bi < 0) break;
        long n = InterlockedExchange(&s_fz_obj[bi].n, 0);
        long long tot = s_fz_obj[bi].tot, mx = s_fz_obj[bi].mx; unsigned long long tids = s_fz_obj[bi].tids;
        s_fz_obj[bi].tot = 0; s_fz_obj[bi].mx = 0; s_fz_obj[bi].tids = 0;
        uint32_t cls = s_fz_obj[bi].cls; if (cls > 7) cls = 0;
        char tl[160]; int q = 0; tl[0] = 0;
        for (int t = 1; t < 64 && q < (int)sizeof tl - 40; t++) if ((tids >> t) & 1) q += snprintf(tl + q, sizeof tl - q, "%s%d:%.12s", q ? "," : "", t, fz_tname(t));
        if (p < (int)sizeof b - 260)
            p += snprintf(b + p, sizeof b - p, " | %s 0x%08X %ldx %lld ms max %lld waiters %s lr 0x%08X", s_fz_cls_name[cls], s_fz_obj[bi].obj, n, tot, mx, tl, s_fz_obj[bi].lr);
        any = 1;
    }
    if (any) fprintf(stderr, "%s\n", b);
}
/* ---- v103B [FREEZE-CONVOY]: sampled every watchdog tick (100 ms). A thread blocked >= 20 ms in a
 * lwmutex/lwcond-relock/sys_mutex wait is attributed to (object, owner); the OWNER's state is recorded:
 * blocked in another call (convoy), or running guest code -- with its host CPU time over the tick
 * (ppu_thread_cpu_us): ~100% = long critical section, ~0% = the owner is runnable but not running
 * (starved / preempted by the SPU lane threads) => priority/CPU starvation, not a lock bug. */
#define FZ_CV_N 32
static struct { uint32_t cls, obj; long samples; long owner_blocked, owner_running, owner_unknown;
                long long cpu_us, wall_us; int last_owner; uint32_t owner_pc; char owner_call[48]; long long max_ms; unsigned long long waiters; } s_fz_cv[FZ_CV_N];
static unsigned long long s_fz_cpu_prev[FZ_MAXT]; static uint64_t s_fz_cpu_prev_t[FZ_MAXT];
static void fz_call_str(const ps3_fz_rec* r, int tid, char* b, size_t cap);
static void fz_convoy_sample(uint64_t now)
{
    extern unsigned long long ppu_thread_cpu_us(unsigned tid);
    for (int tid = 1; tid < FZ_MAXT; tid++) {
        ps3_fz_rec r = s_fz[tid];
        if (!r.kind || !r.t0 || r.t0 > now || now - r.t0 < 20) continue;
        uint32_t cls = fz_obj_class(r.kind, r.num);
        if (cls != 1 && cls != 2 && cls != 3) continue;
        uint32_t obj = r.a[0]; int own = 0;
        if (cls != 3 && obj < 0x10000u) continue;
        if (cls == 1 && obj) own = (int)vm_read32(obj);
        else if (cls == 2 && obj) { obj = vm_read32(obj); if (obj) own = (int)vm_read32(obj); }   /* lwcond -> its lwmutex */
        else if (cls == 3 && obj && obj <= SYS_MUTEX_MAX) own = (int)g_sys_mutexes[obj - 1].owner_tid;
        int si = -1;
        for (int i = 0; i < FZ_CV_N; i++) if (s_fz_cv[i].obj == obj && s_fz_cv[i].cls == cls) { si = i; break; }
        if (si < 0) for (int i = 0; i < FZ_CV_N; i++) if (!s_fz_cv[i].samples) { si = i; s_fz_cv[i].obj = obj; s_fz_cv[i].cls = cls; break; }
        if (si < 0) continue;
        s_fz_cv[si].samples++;
        s_fz_cv[si].waiters |= 1ull << tid;
        if ((long long)(now - r.t0) > s_fz_cv[si].max_ms) s_fz_cv[si].max_ms = (long long)(now - r.t0);
        s_fz_cv[si].last_owner = own;
        if (own <= 0 || own >= FZ_MAXT || own == tid) { s_fz_cv[si].owner_unknown++; continue; }
        ps3_fz_rec o = s_fz[own];
        if (o.kind) { s_fz_cv[si].owner_blocked++; fz_call_str(&o, own, s_fz_cv[si].owner_call, sizeof s_fz_cv[si].owner_call); }
        else {
            s_fz_cv[si].owner_running++;
            s_fz_cv[si].owner_pc = own > 1 && own <= PPU_THREAD_MAX ? g_ppu_threads[own - 1].prof_pc : 0;
            unsigned long long c = ppu_thread_cpu_us((unsigned)own);
            if (c && s_fz_cpu_prev[own] && s_fz_cpu_prev_t[own] && now > s_fz_cpu_prev_t[own] && now - s_fz_cpu_prev_t[own] < 500) {
                s_fz_cv[si].cpu_us += (long long)(c - s_fz_cpu_prev[own]);
                s_fz_cv[si].wall_us += (long long)(now - s_fz_cpu_prev_t[own]) * 1000;
            }
        }
    }
    for (int t = 2; t < FZ_MAXT && t <= PPU_THREAD_MAX; t++) {   /* CPU baselines for the next tick */
        if (g_ppu_threads[t - 1].state == PPU_THREAD_STATE_FREE) { s_fz_cpu_prev[t] = 0; continue; }
        s_fz_cpu_prev[t] = ppu_thread_cpu_us((unsigned)t); s_fz_cpu_prev_t[t] = now;
    }
}
static void fz_convoy_report(void)
{
    for (int pass = 0; pass < 4; pass++) {
        int bi = -1; long bs = 0;
        for (int i = 0; i < FZ_CV_N; i++) if (s_fz_cv[i].samples > bs) { bs = s_fz_cv[i].samples; bi = i; }
        if (bi < 0) break;
        char tl[160]; int q = 0; tl[0] = 0;
        for (int t = 1; t < 64 && q < (int)sizeof tl - 40; t++) if ((s_fz_cv[bi].waiters >> t) & 1) q += snprintf(tl + q, sizeof tl - q, "%s%d:%.12s", q ? "," : "", t, fz_tname(t));
        uint32_t cls = s_fz_cv[bi].cls; if (cls > 7) cls = 0;
        fprintf(stderr, "[FREEZE-CONVOY] 10s %s 0x%08X: %ld samples (100 ms ticks) of waiters %s, max blocked %lld ms; owner (last tid %d %s): blocked %ld (%s), running guest code %ld (pc 0x%08X, host CPU %.0f%% of wall%s), unknown/free %ld\n",
                s_fz_cls_name[cls], s_fz_cv[bi].obj, s_fz_cv[bi].samples, tl, s_fz_cv[bi].max_ms, s_fz_cv[bi].last_owner,
                fz_tname(s_fz_cv[bi].last_owner), s_fz_cv[bi].owner_blocked, s_fz_cv[bi].owner_call[0] ? s_fz_cv[bi].owner_call : "-",
                s_fz_cv[bi].owner_running, s_fz_cv[bi].owner_pc,
                s_fz_cv[bi].wall_us ? 100.0 * (double)s_fz_cv[bi].cpu_us / (double)s_fz_cv[bi].wall_us : -1.0,
                s_fz_cv[bi].wall_us && s_fz_cv[bi].cpu_us * 4 < s_fz_cv[bi].wall_us ? " => owner STARVED of CPU" : "",
                s_fz_cv[bi].owner_unknown);
        s_fz_cv[bi].samples = 0;   /* printed: drop from this window */
    }
    memset(s_fz_cv, 0, sizeof s_fz_cv);
}
/* v103B: SPURS state for the F8 dump (cellSpurs.c registers it; NULL when SPURS is not up). */
void (*volatile g_ps3_fz_spurs_dump)(const char* why) = 0;
void (*volatile g_ps3_fz_title_dump)(const char* why) = 0;

static int fz_on(void)
{
    if (s_fz_on < 0) {
        const char* e = getenv("FREEZE_DUMP");
        s_fz_on = (e && *e == '0') ? 0 : 1;
    }
    return (int)s_fz_on;
}
static void fz_start_watch(void);
static const char* fz_tname(int tid);
static void fz_call_str(const ps3_fz_rec* r, int tid, char* b, size_t cap);

/* ---- v107 FIOS_TRACE=1: microsecond timeline of every syscall/HLE call ------
 * Every ps3_fz_exit appends (exit time, duration, tid, call, r3, r5, lr) to a ring
 * (2^17 events, ~4 s at the observed call rate; snd_stream threads skipped: 12k
 * idle lwcond polls/s). The watchdog writes a window of the ring to a text file:
 *   F8          -> fios_trace_f8_<n>.txt     the 3 s before the key press (max 8)
 *   read burst  -> fios_trace_burst_<n>.txt  1 s before .. 1.5 s after the first
 *                  100 ms tick with >= 8 cellFsRead completions (max 4, 10 s apart)
 * Purpose: where the ~9.5 ms per 64 KB chunk goes during the read bursts that
 * precede the world-movement hitches (play106: bursts cap at ~105 reads/s, the
 * FIOS SPU task only ~2 ms of it). */
typedef struct { uint64_t t_us; uint32_t dur_us, num, a0, a2, lr; uint8_t tid, kind, pad[2]; } fz_tr_ev;
#define FZ_TR_N (1u << 17)
static fz_tr_ev* s_tr;
static volatile long s_tr_head;
static int s_tr_on = -1;
static uint8_t s_tr_skip[FZ_MAXT];   /* 0 unknown, 1 record, 2 skip */
static volatile long s_tr_io_tick;   /* cellFsRead completions since the last watchdog tick */
static uint64_t fz_now_us(void)
{
#ifdef _WIN32
    static LONGLONG s_f = 0;
    if (!s_f) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_f = f.QuadPart; }
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    return (uint64_t)(q.QuadPart / s_f) * 1000000u + (uint64_t)((q.QuadPart % s_f) * 1000000 / s_f);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
static int fz_tr_on(void)
{
    if (s_tr_on < 0) {
        const char* e = getenv("FIOS_TRACE");
        int on = (e && *e == '1') ? 1 : 0;
        if (on) { s_tr = (fz_tr_ev*)calloc(FZ_TR_N, sizeof(fz_tr_ev)); if (!s_tr) on = 0; }
        if (on) fprintf(stderr, "[FIOS-TRACE] on: F8 -> fios_trace_f8_N.txt (3 s before), read burst -> fios_trace_burst_N.txt\n");
        s_tr_on = on;
    }
    return s_tr_on;
}
static void fz_tr_add(int tid, const ps3_fz_rec* r, uint64_t now_us)
{
    if (s_tr_skip[tid] == 0) {
        const char* nm = fz_tname(tid);
        if (nm && *nm) s_tr_skip[tid] = strncmp(nm, "snd_stream", 10) ? 1 : 2;
    }
    if (s_tr_skip[tid] == 2) return;
    long i = InterlockedIncrement(&s_tr_head) - 1;
    fz_tr_ev* e = &s_tr[(unsigned long)i & (FZ_TR_N - 1)];
    e->t_us = 0;   /* torn-entry marker while writing */
    e->dur_us = (uint32_t)now_us - r->pad; e->num = r->num; e->a0 = r->a[0]; e->a2 = r->a[2]; e->lr = r->lr;
    e->tid = (uint8_t)tid; e->kind = (uint8_t)r->kind;
    e->t_us = now_us;
}
static void fz_tr_dump(const char* tag, int n, uint64_t from_us, uint64_t to_us)
{
    char path[64]; snprintf(path, sizeof path, "fios_trace_%s_%d.txt", tag, n);
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[FIOS-TRACE] cannot write %s\n", path); return; }
    const double t0 = (double)s_fz_t0_ms / 1000.0;
    fprintf(f, "# %s: t=%.3f..%.3f s ([FREEZE] time base). Columns: exit_t(s) tid dur_us kind num a0(r3) a2(r5) lr\n",
            path, from_us / 1e6 - t0, to_us / 1e6 - t0);
    for (int tid = 1; tid < FZ_MAXT; tid++) { const char* nm = fz_tname(tid); if (nm && *nm) fprintf(f, "# tid %d %s\n", tid, nm); }
    long head = s_tr_head, lo = head > (long)FZ_TR_N ? head - (long)FZ_TR_N : 0, cnt = 0;
    for (long i = lo; i < head; i++) {
        fz_tr_ev e = s_tr[(unsigned long)i & (FZ_TR_N - 1)];
        if (!e.t_us || e.t_us < from_us || e.t_us > to_us) continue;
        char call[96]; ps3_fz_rec r; memset(&r, 0, sizeof r); r.kind = e.kind; r.num = e.num;
        fz_call_str(&r, e.tid, call, sizeof call);
        fprintf(f, "%.6f %u %u %s | %08X %08X %08X\n", e.t_us / 1e6 - t0, e.tid, e.dur_us, call, e.a0, e.a2, e.lr);
        cnt++;
    }
    fclose(f);
    fprintf(stderr, "[FIOS-TRACE] wrote %s: %ld events, t=%.1f..%.1f s\n", path, cnt, from_us / 1e6 - t0, to_us / 1e6 - t0);
}

void ps3_fz_enter(ppu_context* ctx, uint32_t kind, uint32_t num, void* vsave)
{
    ps3_fz_rec* save = (ps3_fz_rec*)vsave;
    uint64_t tid = ctx ? ctx->thread_id : 0;
    if (tid >= FZ_MAXT || !fz_on()) { save->kind = 0xFFFFFFFFu; return; }
    if (!s_fz_t0_ms) fz_start_watch();
    ps3_fz_rec* r = &s_fz[tid];
    *save = *r;
    ps3_fz_rec n;
    n.kind = 0; n.num = num; n.lr = (uint32_t)ctx->lr; n.seq = r->seq + 1;
    for (int i = 0; i < 5; i++) n.a[i] = (uint32_t)ctx->gpr[3 + i];
    n.pad = s_tr_on > 0 ? (uint32_t)fz_now_us() : 0; n.t0 = fz_now_ms();   /* v107: pad = entry us (FIOS_TRACE) */
    *r = n;
    r->kind = kind;   /* publish last */
}
/* [FREEZE-IO]: cellFsRead (NID 0x4D5FF8E2) calls, bytes asked, slowest, per second. */
static volatile long s_io_n[10], s_io_kb[10]; static volatile long long s_io_mx;
void ps3_fz_exit(ppu_context* ctx, const void* vsave)
{
    const ps3_fz_rec* save = (const ps3_fz_rec*)vsave;
    if (save->kind == 0xFFFFFFFFu) return;
    uint64_t tid = ctx->thread_id;
    ps3_fz_rec* r = &s_fz[tid];
    uint64_t d = fz_now_ms() - r->t0;
    if (s_tr_on > 0) fz_tr_add((int)tid, r, fz_now_us());
    if (d >= 30) { fz_slow_add(r->kind, r->num, d);
        uint32_t cls = fz_obj_class(r->kind, r->num);   /* v103B [FREEZE-SLOWOBJ] */
        if (cls >= 1 && cls <= 3) fz_obj_add(cls, (cls == 2 && r->a[0] >= 0x10000u) ? vm_read32(r->a[0]) : r->a[0], (int)tid, r->lr, d); }   /* lwcond: its lwmutex */
    if (r->kind == 2 && r->num == 0x4D5FF8E2u) {
        int sec = (int)((fz_now_ms() / 1000u) % 10u);
        InterlockedIncrement(&s_io_n[sec]); s_io_kb[sec] += (long)(r->a[2] >> 10);
        InterlockedIncrement(&s_tr_io_tick);
        { extern volatile long long g_fr_reads; g_fr_reads++; }   /* v113 FRAMEREC */
        if ((long long)d > s_io_mx) s_io_mx = (long long)d;
    }
    uint32_t seq = r->seq;
    r->kind = 0;
    *r = *save;
    r->seq = seq;   /* keep the sequence moving so "long block" reports stay once per call */
}

/* ---- names ---- */
static const char* fz_sc_name(uint32_t n)
{
    switch (n) {
    case 41: return "ppu_thread_exit"; case 43: return "ppu_thread_yield"; case 44: return "ppu_thread_join";
    case 82: return "event_flag_create"; case 85: return "event_flag_wait"; case 87: return "event_flag_set";
    case 92: return "semaphore_wait"; case 94: return "semaphore_post";
    case 95: return "lwmutex_create(k)"; case 97: return "lwmutex_lock(k)"; case 98: return "lwmutex_unlock(k)";
    case 102: return "mutex_lock"; case 104: return "mutex_unlock"; case 107: return "cond_wait"; case 108: return "cond_signal";
    case 109: return "cond_signal_all"; case 113: return "lwcond_queue_wait(k)"; case 122: return "rwlock_rlock"; case 125: return "rwlock_wlock";
    case 130: return "event_queue_receive"; case 134: return "event_port_send?"; case 138: return "event_port_send";
    case 141: return "timer_usleep"; case 142: return "timer_sleep";
    case 165: return "spu_thread_get_exit_status"; case 178: return "spu_thread_group_join"; case 179: return "spu_thread_group_terminate?";
    case 190: return "spu_thread_write_ls?"; case 192: return "spu_thread_group_yield?";
    case 801: return "fs_open"; case 802: return "fs_read"; case 803: return "fs_write"; case 818: return "fs_lseek";
    default: return 0;
    }
}
static void fz_call_str(const ps3_fz_rec* r, int tid, char* b, size_t cap)
{
    if (r->kind == 1) {
        const char* nm = fz_sc_name(r->num);
        if (nm) snprintf(b, cap, "syscall %u %s", r->num, nm);
        else snprintf(b, cap, "syscall %u", r->num);
    } else if (r->kind == 2) {
        const char* nm = (tid >= 0 && tid < 64) ? g_hle_inflight[tid] : 0;
        if (!nm) nm = ps3_hle_ctx_name(r->num);
        snprintf(b, cap, "HLE 0x%08X %s", r->num, nm ? nm : "?");
    } else snprintf(b, cap, "running guest code");
}
static void fz_obj_str(const ps3_fz_rec* r, int tid, char* b, size_t cap)
{
    b[0] = 0;
    if (r->kind == 1) {
        uint32_t id = r->a[0];
        switch (r->num) {
        case 102: case 104:
            if (id && id <= SYS_MUTEX_MAX) { sys_mutex_info* m = &g_sys_mutexes[id - 1];
                snprintf(b, cap, "mutex %u active %d owner tid %llu count %d timeout %u us", id, m->active,
                         (unsigned long long)m->owner_tid, (int)m->lock_count, r->a[1]); }
            break;
        case 107:
            if (id && id <= SYS_COND_MAX) { sys_cond_info* c = &g_sys_conds[id - 1]; int nw = 0;
                for (struct sys_cond_waiter* w = c->waiters; w && nw < 64; w = *(struct sys_cond_waiter**)w) nw++;
                uint32_t mid = c->mutex_id; sys_mutex_info* m = (mid && mid <= SYS_MUTEX_MAX) ? &g_sys_mutexes[mid - 1] : 0;
                snprintf(b, cap, "cond %u active %d waiters %d mutex %u (owner tid %llu) timeout %u us", id, c->active, nw, mid,
                         m ? (unsigned long long)m->owner_tid : 0ull, r->a[1]); }
            break;
        case 92:
            if (id && id <= SYS_SEMAPHORE_MAX) { sys_semaphore_info* s = &g_sys_semaphores[id - 1];
                snprintf(b, cap, "sema %u active %d value %d/%d waiters %d timeout %u us", id, s->active, (int)s->value,
                         (int)s->max_value, (int)s->waiters, r->a[1]); }
            break;
        case 85:
            if (id && id <= SYS_EVENT_FLAG_MAX) { sys_event_flag_info* f = &g_sys_event_flags[id - 1];
                snprintf(b, cap, "evflag %u active %d pattern 0x%llX want 0x%X mode 0x%X timeout %u us", id, f->active,
                         (unsigned long long)f->pattern, r->a[1], r->a[2], r->a[4]); }
            break;
        case 130:
            if (id && id <= SYS_EVENT_QUEUE_MAX) { sys_event_queue_info* q = &g_sys_event_queues[id - 1];
                snprintf(b, cap, "queue %u active %d depth %d/%d cancelled %d key 0x%llX timeout %u us", id, q->active, q->count,
                         q->capacity, q->cancelled, (unsigned long long)q->key, r->a[2]); }
            break;
        case 141: snprintf(b, cap, "usleep %u us", r->a[0]); break;
        case 44:
            if (id >= 1 && id <= PPU_THREAD_MAX) snprintf(b, cap, "join tid %u state %d (%s)", id,
                g_ppu_threads[id - 1].state, g_ppu_threads[id - 1].name);
            break;
        default: break;
        }
    } else if (r->kind == 2) {
        const char* nm = (tid >= 0 && tid < 64) ? g_hle_inflight[tid] : 0;
        if (!nm) nm = ps3_hle_ctx_name(r->num);
        if (nm && (!strcmp(nm, "sys_lwmutex_lock") || !strcmp(nm, "sys_lwmutex_trylock")) && r->a[0])
            snprintf(b, cap, "lwmutex 0x%08X owner %u recur %u timeout %u us", r->a[0], vm_read32(r->a[0]),
                     vm_read32(r->a[0] + 0x0C), r->a[1]);
        else if (nm && !strcmp(nm, "sys_lwcond_wait") && r->a[0]) {
            uint32_t m = vm_read32(r->a[0]);
            snprintf(b, cap, "lwcond 0x%08X mutex 0x%08X (owner %u recur %u) timeout %u us", r->a[0], m,
                     m ? vm_read32(m) : 0, m ? vm_read32(m + 0x0C) : 0, r->a[1]);
        }
    }
}
static const char* fz_tname(int tid)
{
    if (tid == 1) return "main";
    if (tid >= 1 && tid <= PPU_THREAD_MAX && g_ppu_threads[tid - 1].state != PPU_THREAD_STATE_FREE)
        return g_ppu_threads[tid - 1].name;
    return "?";
}

static void fz_dump(const char* why)
{
    uint64_t now = fz_now_ms();
    fprintf(stderr, "[FREEZE] ===== dump (%s) t=%.1fs: every guest PPU thread =====\n", why, fz_t());
    for (int tid = 1; tid < FZ_MAXT; tid++) {
        int live = (tid == 1) || (tid <= PPU_THREAD_MAX && (g_ppu_threads[tid - 1].state == PPU_THREAD_STATE_RUNNING ||
                                                          g_ppu_threads[tid - 1].state == PPU_THREAD_STATE_DETACHED));
        ps3_fz_rec r = s_fz[tid];
        if (!live && !r.kind) continue;
        if (r.t0 > now || !r.t0) r.t0 = now;   /* entered after we read the clock / torn read */
        char call[96], obj[256];
        fz_call_str(&r, tid, call, sizeof call);
        fz_obj_str(&r, tid, obj, sizeof obj);
        unsigned pc = tid > 1 && tid <= PPU_THREAD_MAX ? g_ppu_threads[tid - 1].prof_pc : 0;
        int prio = tid > 1 && tid <= PPU_THREAD_MAX ? g_ppu_threads[tid - 1].priority : 0;
        if (r.kind)
            fprintf(stderr, "[FREEZE]  tid %2d %-26.26s prio %4d: %s for %llu ms, lr 0x%08X args %08X %08X %08X %08X %08X%s%s\n",
                    tid, fz_tname(tid), prio, call, (unsigned long long)(now - r.t0), r.lr,
                    r.a[0], r.a[1], r.a[2], r.a[3], r.a[4], obj[0] ? " | " : "", obj);
        else
            fprintf(stderr, "[FREEZE]  tid %2d %-26.26s prio %4d: running guest code (last call site 0x%08X)\n",
                    tid, fz_tname(tid), prio, pc);
    }
    fprintf(stderr, "[FREEZE] ===== end dump =====\n");
    if (g_ps3_fz_title_dump) g_ps3_fz_title_dump(why);
    if (g_ps3_fz_spurs_dump) g_ps3_fz_spurs_dump(why);   /* v103B: every SPURS workload + lane */
    fflush(stderr);
}

/* v144: called by cellAudio.c when a run of audio holes starts (late guest blocks): dump every guest PPU
 * thread's current call so the stalled stage of the sound pipeline shows up. Needs FREEZE_DUMP=1 (the
 * per-syscall tracking). At most AUDIO_HOLE_DUMPS (default 12) dumps, >= 3 s apart. */
void ps3_freeze_dump_now(const char* why)
{
    if (!fz_on()) return;
    static int s_max = -1; if (s_max < 0) { const char* e = getenv("AUDIO_HOLE_DUMPS"); s_max = e ? atoi(e) : 12; }
    static volatile long s_n = 0; static uint64_t s_last = 0;
    uint64_t now = fz_now_ms();
    if (s_n >= s_max || (s_last && now - s_last < 3000)) return;
    s_last = now; InterlockedIncrement(&s_n);
    fz_dump(why);
}

/* Called from cellSpurs.c next to the [STALL] "no restart" line. */
void ps3_freeze_note_spurs_idle(int inst, unsigned wid)
{
    static volatile long long s_win0 = 0; static volatile long s_cnt = 0;
    (void)wid;
    if (inst != 0 || !fz_on()) return;
    long long now = (long long)fz_now_ms();
    if (now - s_win0 > 1000) { s_win0 = now; s_cnt = 0; }
    if (InterlockedIncrement(&s_cnt) == 4) InterlockedExchange(&s_fz_spurs_req, 1);
}

static void fz_slow_report(void)
{
    char b[1400]; int p = snprintf(b, sizeof b, "[FREEZE-SLOW] 10s calls >= 30 ms:");
    int any = 0;
    for (int pass = 0; pass < 12; pass++) {   /* top 12 by max */
        int bi = -1; long long bm = -1;
        for (int i = 0; i < FZ_SLOW_N; i++) if (s_fz_slow[i].key && s_fz_slow[i].n > 0 && s_fz_slow[i].mx > bm) { bm = s_fz_slow[i].mx; bi = i; }
        if (bi < 0) break;
        long key = s_fz_slow[bi].key; long n = InterlockedExchange(&s_fz_slow[bi].n, 0);
        long long tot = s_fz_slow[bi].tot, mx = s_fz_slow[bi].mx; s_fz_slow[bi].tot = 0; s_fz_slow[bi].mx = 0;
        char nm[64];
        (void)key;
        if (s_fz_slow[bi].kind == 2) {
            const char* s = ps3_hle_ctx_name(s_fz_slow[bi].num);
            if (s) snprintf(nm, sizeof nm, "%s", s); else snprintf(nm, sizeof nm, "HLE 0x%08X", s_fz_slow[bi].num);
        } else {
            uint32_t num = s_fz_slow[bi].num; const char* s = fz_sc_name(num);
            if (s) snprintf(nm, sizeof nm, "%s", s); else snprintf(nm, sizeof nm, "sc%u", num);
        }
        if (p < (int)sizeof b - 90) p += snprintf(b + p, sizeof b - p, " | %s %ldx %lld ms max %lld", nm, n, tot, mx);
        any = 1;
    }
    if (any) fprintf(stderr, "%s\n", b);
}

#ifdef _WIN32
static unsigned __stdcall fz_watch_thread(void* p)
#else
static void* fz_watch_thread(void* p)
#endif
{
    (void)p;
    int blk_ms = 2000; { const char* e = getenv("FREEZE_BLOCK_MS"); if (e) blk_ms = atoi(e); if (blk_ms < 100) blk_ms = 100; }
    int key = 0x77; /* VK_F8 */
    { const char* e = getenv("FREEZE_KEY"); if (e && (e[0] == 'F' || e[0] == 'f')) { int f = atoi(e + 1); if (f >= 1 && f <= 12) key = 0x6F + f; } }
    fprintf(stderr, "[FREEZE] v98b freeze watchdog ON (FREEZE_DUMP=0 off): press F%d in the game window during a freeze to dump all PPU threads; "
                    "auto dump when SPURS inst 0 goes idle; long block > %d ms logged\n", key - 0x6F, blk_ms);
    uint64_t last_slow = fz_now_ms(), last_spurs = 0; long nlong = 0; int key_was = 0; (void)key_was;
    for (;;) {
#ifdef _WIN32
        Sleep(100);
#else
        { struct timespec ts = { 0, 100000000L }; nanosleep(&ts, 0); }
#endif
        uint64_t now = fz_now_ms();
#ifdef _WIN32
        {   int down = (GetAsyncKeyState(key) & 0x8000) != 0;
            if (down && !key_was) {
                HWND fg = GetForegroundWindow(); DWORD pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &pid);
                if (pid == GetCurrentProcessId()) {
                    char w[32]; snprintf(w, sizeof w, "key F%d", key - 0x6F);
                    fprintf(stderr, "[FREEZE] USER MARK: F%d pressed at t=%.1fs (user saw the freeze)\n", key - 0x6F, fz_t());
                    fz_dump(w); fz_slow_report(); fz_obj_report(); fz_convoy_report();
                    {   extern void (*volatile g_ps3_fr_dump)(void);   /* v113 FRAMEREC: 50 ms slices of the last ~6 s */
                        if (g_ps3_fr_dump) g_ps3_fr_dump();
                        extern void ps3_prof_hitch_dump(double ms); ps3_prof_hitch_dump(3000.0);
                        extern void ps3_prof_hitch_timeline(void); ps3_prof_hitch_timeline();
                        extern void ps3_icalltl_dump(void); ps3_icalltl_dump(); }   /* v121 ICALLTL */   /* v120 */   /* main-thread profile, last 3 s */
                    {   static int nf8 = 0; uint64_t u = fz_now_us();
                        if (s_tr_on > 0 && nf8 < 8) fz_tr_dump("f8", ++nf8, u - 3000000u, u); }
                }
            }
            key_was = down; }
#endif
        if (s_fz_spurs_req) {
            InterlockedExchange(&s_fz_spurs_req, 0);
            if (fz_t() > 60.0 && (!last_spurs || now - last_spurs > 10000)) { last_spurs = now; fz_dump("spurs-idle: >= 4 inst-0 workloads idle, no PPU task wait"); }
        }
        for (int tid = 1; tid < FZ_MAXT; tid++) {
            ps3_fz_rec r = s_fz[tid];
            if (!r.kind || r.seq == s_fz_rep_seq[tid]) continue;
            if (!r.t0 || r.t0 > now || now - r.t0 < (uint64_t)blk_ms) continue;   /* t0 0 = torn read during exit */
            if ((r.kind == 1 && r.num == 41) || (r.kind == 2 && r.num == 0xAFF080A4u)) continue;   /* thread exit unwinds, never returns */
            s_fz_rep_seq[tid] = r.seq;
            if (++nlong > 400) continue;
            char call[96], obj[256];
            fz_call_str(&r, tid, call, sizeof call);
            fz_obj_str(&r, tid, obj, sizeof obj);
            fprintf(stderr, "[FREEZE] long block t=%.1fs: tid %d %s in %s for %llu ms, lr 0x%08X args %08X %08X %08X%s%s\n",
                    fz_t(), tid, fz_tname(tid), call, (unsigned long long)(now - r.t0), r.lr, r.a[0], r.a[1], r.a[2],
                    obj[0] ? " | " : "", obj);
            /* lwmutex / sys_mutex: say what the OWNER is doing (convoy root) */
            {   int own = 0;
                if (r.kind == 2 && !strncmp(obj, "lwmutex ", 8)) own = (int)vm_read32(r.a[0]);
                else if (r.kind == 1 && r.num == 102 && r.a[0] && r.a[0] <= SYS_MUTEX_MAX) own = (int)g_sys_mutexes[r.a[0] - 1].owner_tid;
                if (own > 0 && own < FZ_MAXT && own != tid) {
                    ps3_fz_rec o = s_fz[own]; char c2[96], o2[256];
                    if (o.t0 > now) o.t0 = now;
                    fz_call_str(&o, own, c2, sizeof c2); fz_obj_str(&o, own, o2, sizeof o2);
                    unsigned opc = own > 1 && own <= PPU_THREAD_MAX ? g_ppu_threads[own - 1].prof_pc : 0;
                    if (o.kind)
                        fprintf(stderr, "[FREEZE]   owner tid %d %s is in %s for %llu ms, lr 0x%08X args %08X %08X %08X%s%s\n",
                                own, fz_tname(own), c2, (unsigned long long)(now - o.t0), o.lr, o.a[0], o.a[1], o.a[2], o2[0] ? " | " : "", o2);
                    else
                        fprintf(stderr, "[FREEZE]   owner tid %d %s is running guest code (last call site 0x%08X)\n", own, fz_tname(own), opc);
                }
            }
        }
        fz_convoy_sample(now);   /* v103B */
        if (s_tr_on > 0) {   /* v107: read-burst trigger */
            static uint64_t trig_us = 0, last_trig_us = 0; static int nb = 0;
            long io = InterlockedExchange(&s_tr_io_tick, 0);
            uint64_t u = fz_now_us();
            if (!trig_us && nb < 4 && io >= 8 && (!last_trig_us || u - last_trig_us > 10000000u)) {
                trig_us = u; last_trig_us = u;
                fprintf(stderr, "[FIOS-TRACE] read burst at t=%.1fs (%ld reads in 100 ms) -> capturing\n", fz_t(), io);
            }
            if (trig_us && u - trig_us >= 1500000u) { fz_tr_dump("burst", ++nb, trig_us - 1000000u, u); trig_us = 0; }
        }
        if (now - last_slow >= 10000) { last_slow = now; fz_slow_report(); fz_obj_report(); fz_convoy_report(); }
        {   /* [FREEZE-IO] once per 10 s: per-second cellFsRead counts of the last 10 s */
            static uint64_t last_io = 0; static int last_sec = -1;
            int sec = (int)((now / 1000u) % 10u);
            if (sec != last_sec) {   /* new second: clear the slot we are about to fill */
                if (last_sec >= 0) { int nx = sec; s_io_n[nx] = 0; s_io_kb[nx] = 0; }
                last_sec = sec;
            }
            if (!last_io) last_io = now;
            if (now - last_io >= 10000) {
                last_io = now;
                char b[400]; int p = snprintf(b, sizeof b, "[FREEZE-IO] t=%.0fs cellFsRead per s (oldest first):", fz_t());
                long tn = 0, tk = 0;
                for (int i = 1; i <= 10; i++) { int k = (sec + i) % 10; if (k == sec) continue;
                    p += snprintf(b + p, sizeof b - p, " %ld", s_io_n[k]); tn += s_io_n[k]; tk += s_io_kb[k]; }
                fprintf(stderr, "%s | %ld reads %ld KB, slowest %lld ms\n", b, tn, tk, s_io_mx);
                s_io_mx = 0;
            }
        }
    }
#ifdef _WIN32
    return 0;
#else
    return 0;
#endif
}
static void fz_start_watch(void)
{
    static volatile long s_once = 0;
    if (InterlockedExchange(&s_once, 1)) { while (!s_fz_t0_ms) { /* tiny window */ } return; }
    fz_tr_on();   /* v107: decide FIOS_TRACE before the first traced exit */
#ifdef _WIN32
    HANDLE h = (HANDLE)_beginthreadex(NULL, 256 * 1024, fz_watch_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
#else
    pthread_t th; pthread_attr_t at;
    if (pthread_attr_init(&at) == 0) {
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&th, &at, fz_watch_thread, NULL) == 0) pthread_detach(th);
        else fprintf(stderr, "[FREEZE] watchdog thread creation failed (errno %d)\n", errno);
        pthread_attr_destroy(&at);
    } else {
        fprintf(stderr, "[FREEZE] watchdog thread attributes failed (errno %d)\n", errno);
    }
#endif
    s_fz_t0_ms = fz_now_ms();
}
