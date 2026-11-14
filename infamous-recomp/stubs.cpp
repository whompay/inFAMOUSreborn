/*
 * ps3recomp game project -- game-specific hooks and overrides.
 *
 * main.cpp is the runner and is game-agnostic. This file is where a port puts
 * everything that is not: the PRX loading hook it must answer, overrides for
 * firmware functions the HLE library gets wrong for this title, and patches
 * over recompiled code.
 *
 * It starts out as the empty version of all of that, which is what a game with
 * no lifted PRX and no overrides needs.
 */

#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cmath>
#include <string>

#include "ppu_recomp.h"
#ifndef _WIN32
/* Win32 spelling and API shims for the POSIX build: the port's hooks below use
 * __declspec, __stdcall, GetTickCount64 and VirtualQuery directly, as the
 * Windows build always has (where the compiler and <windows.h> supply them). */
#include "win32_compat.h"
#include "msvc_compat.h"
#endif
/* v237B ADAPT_LOD: adaptive draw-distance factor (infamous_clock_probe.h writes it, lodx_v231.h reads it; 1 = off) */
extern "C" { volatile float g_v237_lodf = 1.0f; }
#include "infamous_clock_probe.h"
#ifndef PORT_NO_TRAINER
#include "trainer.h"   /* F10 trainer */
#else
/* PORT_BUILD_TRAINER=OFF: trainer.cpp is not in the build; keep its hooks no-ops. */
static void infamous_trainer_install(void) {}
extern "C" int infamous_trainer_anim_hook(uint32_t anim_obj) { (void)anim_obj; return 0; }
#endif
#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"
extern "C" uint8_t* vm_base;   /* C linkage: defined in main.cpp */

extern "C" {
void ps3_hle_register_ctx(uint32_t nid, const char* name, void (*fn)(ppu_context*));
s32 _cellSpursLFQueuePushBody(u64 queue_ea, u64 data_ea, u32 isBlocking);
}

/* This export is absent from the generated firmware registry even though the
 * game imports it.  Keep the title-specific bridge here: a context handler is
 * explicit about the guest ABI and wins over the generic NID table. */
static void infamous_cellSpursLFQueuePushBody(ppu_context* ctx)
{
    const s32 rc = _cellSpursLFQueuePushBody(ctx->gpr[3], ctx->gpr[4],
                                             (u32)ctx->gpr[5]);
    ctx->gpr[3] = (u64)(s64)rc;
}

/* ---------------------------------------------------------------------------
 * PRX loading
 *
 * main.cpp calls this once, after the lifted function table is registered and
 * vm_base is live, before the game runs. A title that lifts a real system PRX
 * -- libsre, which is cellSpurs and cellSync -- loads its image into guest RAM
 * here and registers its exports. Most ports never need to: the HLE library
 * answers those modules instead.
 * -----------------------------------------------------------------------*/

void ps3_sampler_start(void);   /* ppu_loader.cpp: guest-PC sampler, opt-in via PS3_SAMPLE=<ms> */
extern "C" const char* g_hle_inflight[];    /* ppu_hle.cpp; 64 entries */
extern "C" void ppu_report_guest_lrs(void); /* ppu_loader.cpp */

/* INFAMOUS_HANGDUMP=<seconds>[,<seconds>...]: at each time, report which HLE
 * call every guest thread is parked in and each thread's guest lr -- a hang
 * snapshot that needs no thread suspension. */
static void infamous_hangdump_start(void)
{
    const char* e = std::getenv("INFAMOUS_HANGDUMP");
    if (!e) return;
    std::string spec(e);
    std::thread([spec] {
        int elapsed = 0;
        size_t pos = 0;
        while (pos < spec.size()) {
            int at = std::atoi(spec.c_str() + pos);
            if (at > elapsed) std::this_thread::sleep_for(std::chrono::seconds(at - elapsed));
            elapsed = at;
            std::fprintf(stderr, "[hangdump] t=%ds guest threads inside an HLE:\n", at);
            for (unsigned t = 0; t < 64u; t++)
                if (g_hle_inflight[t])
                    std::fprintf(stderr, "[hangdump]   guest-tid %-3u in %s\n", t, g_hle_inflight[t]);
            std::fprintf(stderr, "[hangdump] guest threads, by guest lr:\n");
            ppu_report_guest_lrs();
            /* INFAMOUS_HANGDUMP_MEM=<hexEA>[,<hexEA>...]: 32 bytes at each. */
            if (const char* m = std::getenv("INFAMOUS_HANGDUMP_MEM")) {
                for (const char* p = m; *p;) {
                    /* "*EA+off" reads a guest pointer at EA and dumps
                     * at pointer+off (both hex). */
                    bool deref = (*p == '*');
                    if (deref) p++;
                    char* q = nullptr;
                    uint32_t ea = (uint32_t)std::strtoul(p, &q, 16);
                    if (deref) ea = vm_read32(ea);
                    if (q && *q == '+') ea += (uint32_t)std::strtoul(q + 1, nullptr, 16);
                    std::fprintf(stderr, "[hangdump] mem 0x%08X:", ea);
                    for (int i = 0; i < 8; i++) std::fprintf(stderr, " %08X", vm_read32(ea + i * 4));
                    std::fprintf(stderr, "\n");
                    const char* c = std::strchr(p, ',');
                    p = c ? c + 1 : p + std::strlen(p);
                }
            }
            /* INFAMOUS_HANGDUMP_RAW=<hexEA>:<hexLen>[,...]: write that guest range
             * to hangdump_<t>s_<EA>.bin (e.g. Bink planes, display buffers). */
            if (const char* m = std::getenv("INFAMOUS_HANGDUMP_RAW")) {
                for (const char* p = m; *p;) {
                    char* q = nullptr;
                    uint32_t ea = (uint32_t)std::strtoul(p, &q, 16);
                    uint32_t len = (q && *q == ':') ? (uint32_t)std::strtoul(q + 1, nullptr, 16) : 0x1000;
                    char name[96];
                    std::snprintf(name, sizeof(name), "hangdump_%ds_%08X.bin", at, ea);
                    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(vm_base + ea, 1, len, f); std::fclose(f); }
                    std::fprintf(stderr, "[hangdump] raw 0x%08X+0x%X -> %s\n", ea, len, name);
                    const char* c = std::strchr(p, ',');
                    p = c ? c + 1 : p + std::strlen(p);
                }
            }
            std::fflush(stderr);
            size_t c = spec.find(',', pos);
            pos = c == std::string::npos ? spec.size() : c + 1;
        }
    }).detach();
}

// v194: "is the game paused?" for the settings-menu hint (rsx_ui_overlay.c): the world clock (base+4) stands still
// while the real clock (base+0x24) keeps running -- that is the pause menu (SIM-FROZEN in infamous_clock_probe.h).
extern "C" int (*volatile g_uiov_paused_fn)(void);
#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long long __stdcall GetTickCount64(void);
#endif
static int infamous_paused_probe(void)
{
    using infamous_clock_probe::word; using infamous_clock_probe::base;
    static uint32_t lw = 0, lr = 0; static unsigned long long tw = 0, tr = 0;
    if (!vm_base) return 0;
    const unsigned long long now = GetTickCount64();
    const uint32_t w = word(base + 4), r = word(base + 0x24);
    if (w != lw) { lw = w; tw = now; }
    if (r != lr) { lr = r; tr = now; }
    return tw && tr && now - tw > 300 && now - tr < 300;
}

#ifdef _WIN32
/* ===========================================================================
 * v195 NATIVE PAUSE-MENU ENTRIES: "Settings" and "Quit Game" inside the game's own pause list.
 *
 * The pause menu (ui_pause_menu, core.xpp) is a script for the game's bytecode VM (interpreter func_000DF220,
 * opcode table at [toc-0x7E60]-0x7F90). Its commands are the array lOidCmdAll {powers, help, options, cutscenes,
 * ddrops, abandon, quit_demo, saved_data}; FCmdAvailable(cmd) filters it into lOidCmd, the build loop gives each
 * available command one of the 7 row widgets (ami_item1..7, text = the command's lang string), and OnCmd(cmd)
 * runs the chosen one. Patched in guest memory once the script is loaded (anchor = OnCmd's quit_demo case,
 * every other site checked word by word against the retail bytes first; nothing is written on a mismatch):
 *   - lOidCmdAll -> 9 entries (new buffer in the dead space of a stripped assert, 0x16B4 past the anchor):
 *     powers, help, options, cutscenes, ddrops, abandon, saved, SETTINGS (oid 0x2094D, u_menu_restart_demo,
 *     text -> "Settings"), QUIT (quit_demo, text "Quit Demo" -> "Quit Game").
 *   - FCmdAvailable: quit_demo "mode == 2" (demo build) -> "mode != 2"; unknown command -> available (SETTINGS).
 *   - OnCmd: quit_demo's native (demo exit) -> pm_native_quit; unknown command -> pm_native_settings. Both just
 *     ask the port overlay (rsx_ui_overlay.c g_uiov_request) for the settings page / quit confirmation.
 *   - build: the stripped "assert(count > 0)" + count -> pm_native_trim(lOidCmd) then count: there are only 7 rows,
 *     so with more than 7 available commands Watch Movies, then Dead Drops, then Saved Data are left out.
 * Natives = OPDs in the same dead space -> fake code addresses 0x00FF0000.. registered with ppu_register_function.
 * PAUSE_NATIVE=0 disables. Log: "[pausemenu] ...".
 * =========================================================================*/
extern "C" void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
extern "C" volatile int g_uiov_request;   /* rsx_ui_overlay.c */
extern "C" volatile unsigned long long g_pause_menu_built_ms;   /* rsx_ui_overlay.c: pause watermark */
struct inf_mbi { void* base; void* abase; unsigned long aprot; unsigned short part; size_t size; unsigned long state, prot, type; };
extern "C" __declspec(dllimport) size_t __stdcall VirtualQuery(const void*, inf_mbi*, size_t);

static const uint32_t PM_FN_SETTINGS = 0x00FF0000u, PM_FN_QUIT = 0x00FF0010u, PM_FN_TRIM = 0x00FF0020u;
static const uint32_t PM_TOC = 0x0078BFC0u;
static const uint32_t PM_OID_SETTINGS = 0x0002094Du, PM_OID_QUIT = 0x00019F4Cu;

static void pm_native_settings(ppu_context* ctx) { g_uiov_request = 1; std::fprintf(stderr, "[pausemenu] Settings\n"); ctx->gpr[3] = 0; }
static void pm_native_quit(ppu_context* ctx)     { g_uiov_request = 2; std::fprintf(stderr, "[pausemenu] Quit Game\n"); ctx->gpr[3] = 0; }
static void pm_native_trim(ppu_context* ctx)
{
    g_pause_menu_built_ms = GetTickCount64();   /* the pause menu is being built: it is open now */
    const uint32_t args = (uint32_t)ctx->gpr[4];
    const uint32_t h = args ? vm_read32(args) : 0;
    ctx->gpr[3] = 0;
    if (h < 0x10000u || h >= 0xC0000000u) return;
    const uint32_t data = vm_read32(h), n0 = vm_read32(h + 4);
    if (data < 0x10000u || data >= 0xC0000000u || n0 > 32) return;
    uint32_t v[32], n = n0;
    for (uint32_t i = 0; i < n; i++) v[i] = vm_read32(data + 4 * i);
    static const uint32_t drop[] = { 0x00019F4Au /* Watch Movies */, 0x00019F49u /* Dead Drops */, 0x00019F4Du /* Saved Data */ };
    for (uint32_t d = 0; d < 3 && n > 7; d++)
        for (uint32_t i = 0; i < n; i++)
            if (v[i] == drop[d]) { for (uint32_t j = i; j + 1 < n; j++) v[j] = v[j + 1]; n--; break; }
    if (n > 7) n = 7;
    if (n != n0) {
        for (uint32_t i = 0; i < n; i++) vm_write32(data + 4 * i, v[i]);
        vm_write32(h + 4, n);
    }
    static int logs = 0;
    if (logs++ < 20) {
        std::fprintf(stderr, "[pausemenu] rows %u of %u:", n, n0);
        for (uint32_t i = 0; i < n; i++) std::fprintf(stderr, " %05X", v[i]);
        std::fprintf(stderr, "\n");
    }
}

/* expected words; 0xFFFFFFFF = relocated at load (not compared) */
static const uint32_t W_ = 0xFFFFFFFFu;
static bool pm_readable(uint32_t ea, uint32_t len)
{
    for (uint64_t a = ea & ~0xFFFull; a < (uint64_t)ea + len; a += 0x1000) {
        inf_mbi m; if (!VirtualQuery(vm_base + a, &m, sizeof m)) return false;
        if (m.state != 0x1000 || !m.prot || (m.prot & 0x101)) return false;
    }
    return true;
}
static bool pm_match(uint32_t ea, const uint32_t* w, int n)
{
    if (!pm_readable(ea, 4u * n)) return false;
    for (int i = 0; i < n; i++) if (w[i] != W_ && vm_read32(ea + 4u * i) != w[i]) return false;
    return true;
}
static void pm_write(uint32_t ea, const uint32_t* w, int n) { for (int i = 0; i < n; i++) vm_write32(ea + 4u * i, w[i]); }

static const uint32_t PM_ANCHOR_W[] = { 0x01040400u, 0x00019F4Cu, 0x70000000u, 0x3A0B0000u, 0x43000014u, 0x38000000u, 0x00000000u, 0x0077C170u, 0x4200012Cu };
static const uint8_t  PM_ANCHOR_B[16] = { 0x01,0x04,0x04,0x00, 0x00,0x01,0x9F,0x4C, 0x70,0x00,0x00,0x00, 0x3A,0x0B,0x00,0x00 };

static bool pm_patch_script(uint32_t A)
{
    if (!pm_match(A, PM_ANCHOR_W, 9)) return false;
    static const uint32_t ondef[] = { 0x4200001Cu, 0x3B040000u, 0x1C040004u, 0x00000000u, 0xA2040005u, W_, 0x3A040000u, 0x40080000u };
    static const uint32_t avq[] = { 0x01040400u, 0x00019F4Cu, 0x70000000u, 0x3A0B0000u, 0x4300002Cu, 0x3B010000u, 0x3B070000u,
                                    0x38041001u, 0x00000000u, W_, 0x01040400u, 0x00000002u, 0x70000000u };
    static const uint32_t avd[] = { 0x01040400u, 0x00018BDCu, 0x70000000u, 0x3A0B0000u, 0x4300000Cu, 0x02010000u, 0x42000008u, 0x02000000u, 0x40080100u };
    static const uint32_t bld[] = { 0x3B040000u, 0x3B040000u, 0x1C040004u, 0x00000014u, 0x22000401u, 0x96000000u, 0x3A040000u, 0x0C040000u, 0x00000018u,
                                    0x3B010000u, 0x3B070000u, 0x1C040004u, 0x00000018u, 0x01040400u, 0x00000000u, 0x6E000000u, 0x3A0B0000u,
                                    0x9F000000u, 0x00C700C7u, 0x000C0018u, 0x00000000u, W_, 0x3A010000u, 0x01040400u };
    static const uint32_t as2[] = { 0x3B010000u, 0x3B070000u, 0x1C040004u, 0x00000024u, 0x3B040000u, 0x3B040000u, 0x1C040104u, 0x00000054u,
                                    0x22000401u, 0x96000000u, 0x3A040000u, 0x6C000000u, 0x3A0B0000u, 0x9F000000u, 0x01F701F7u, 0x0014003Du,
                                    0x00000000u, W_, 0x3A010000u, 0x3B010000u };
    const uint32_t a_ondef = A + 0x130, a_avq = A + 0x690, a_avd = A + 0x728, a_bld = A + 0x85C, a_as2 = A + 0x16B4, a_hdr = A + 0x707C;
    const uint32_t old = vm_read32(a_hdr);
    static const uint32_t cmds8[] = { 0x19F46u, 0x19F47u, 0x19F48u, 0x19F4Au, 0x19F49u, 0x19F4Bu, 0x19F4Cu, 0x19F4Du };
    const char* bad = nullptr;
    if (!pm_match(a_ondef, ondef, 8)) bad = "OnCmd default";
    else if (!pm_match(a_avq, avq, 13)) bad = "FCmdAvailable quit";
    else if (!pm_match(a_avd, avd, 9)) bad = "FCmdAvailable default";
    else if (!pm_match(a_bld, bld, 24)) bad = "build count";
    else if (!pm_match(a_as2, as2, 20)) bad = "assert space";
    else if (!pm_readable(a_hdr, 16)) bad = "lOidCmdAll header";
    else if (vm_read32(a_hdr + 4) != 8 || vm_read32(a_hdr + 8) != 7 || old < 0x10000u || !pm_match(old, cmds8, 8)) bad = "lOidCmdAll";
    if (bad) {
        static uint32_t said[8]; static int ns;
        bool seen = false; for (int i = 0; i < ns; i++) seen |= said[i] == A;
        if (!seen) { if (ns < 8) said[ns++] = A; std::fprintf(stderr, "[pausemenu] script at 0x%08X differs (%s) -- not patched\n", A, bad); }
        return false;
    }

    /* dead space: jump over it, then the 9-entry command array and three OPDs */
    const uint32_t arr = a_as2 + 4, opd_s = a_as2 + 0x28, opd_q = a_as2 + 0x30, opd_t = a_as2 + 0x38;
    const uint32_t space[19] = { 0x4200004Cu,
        0x19F46u, 0x19F47u, 0x19F48u, 0x19F4Au, 0x19F49u, 0x19F4Bu, 0x19F4Du, PM_OID_SETTINGS, PM_OID_QUIT,
        PM_FN_SETTINGS, PM_TOC, PM_FN_QUIT, PM_TOC, PM_FN_TRIM, PM_TOC, 0, 0, 0 };
    pm_write(a_as2, space, 19);
    /* build: trim(lOidCmd) then count it */
    const uint32_t b2[23] = { 0x3B040000u, 0x1C040004u, 0x00000014u, 0x22000401u, 0x38040000u, 0x00000001u, opd_t, 0x3A040000u,
                              0x3B040000u, 0x3B040000u, 0x1C040004u, 0x00000014u, 0x22000401u, 0x96000000u, 0x3A040000u, 0x0C040000u, 0x00000018u,
                              0, 0, 0, 0, 0, 0 };
    pm_write(a_bld, b2, 23);
    vm_write32(a_avd + 0x1C, 0x02010000u);          /* unknown command -> available */
    vm_write32(a_avq + 0x30, 0x71000000u);          /* quit_demo: mode != 2 */
    const uint32_t od[6] = { 0x38000000u, 0x00000000u, opd_s, 0, 0, 0 };
    pm_write(a_ondef + 4, od, 6);                    /* OnCmd unknown command -> Settings */
    vm_write32(A + 0x1C, opd_q);                     /* OnCmd quit_demo -> Quit Game */
    vm_write32(a_hdr + 4, 9);                        /* lOidCmdAll -> the new array */
    vm_write32(a_hdr, arr);
    std::fprintf(stderr, "[pausemenu] v195 pause menu patched (script 0x%08X): + Settings, Quit Game\n", A);
    return true;
}

/* "<key>\0<text>\0" -> new text (when it fits) */
static int pm_patch_lang_at(uint8_t* p, size_t keylen, const char* nt)
{
    uint8_t* t = p + keylen + 1; size_t L = 0;
    while (L < 64 && t[L]) L++;
    const size_t nl = std::strlen(nt);
    if (L == 0 || L >= 64 || nl > L || !std::memcmp(t, nt, nl)) return 0;
    std::memcpy(t, nt, nl); std::memset(t + nl, 0, L - nl);
    return 1;
}

static void pm_thread()
{
    using namespace std::chrono;
    std::this_thread::sleep_for(seconds(3));
    uint32_t A = 0; bool lq = false, ls = false, script_done = false;
    for (int pass = 0; pass < 600 && !(script_done && lq && ls); pass++) {
        if (!script_done && pm_match(0x4222AA84u, PM_ANCHOR_W, 9)) { A = 0x4222AA84u; script_done = pm_patch_script(A); }
        /* scan committed guest memory 0x40000000..0x60000000 for the script anchor and the two lang keys */
        static const char k_q[] = "u_menu_quit_demo", k_s[] = "u_menu_restart_demo";
        uint8_t* lo = vm_base + 0x40000000ull; uint8_t* hi = vm_base + 0x60000000ull;
        for (uint8_t* p = lo; p < hi && !(script_done && lq && ls);) {
            inf_mbi m; if (!VirtualQuery(p, &m, sizeof m)) break;
            uint8_t* rb = (uint8_t*)m.base; uint8_t* re = rb + m.size; if (re > hi) re = hi;
            const bool rd = m.state == 0x1000 && !(m.prot & 0x101) && m.prot;
            if (rd) {
                for (uint8_t* q = (rb < p ? p : rb); q + 32 < re; q += 4) {
                    if (!script_done && q[0] == 0x01 && !std::memcmp(q, PM_ANCHOR_B, 16)) {
                        const uint32_t ea = (uint32_t)(q - vm_base);
                        if (pm_match(ea, PM_ANCHOR_W, 9)) { A = ea; script_done = pm_patch_script(ea); }
                    }
                }
                if (!(lq && ls))
                    for (uint8_t* q = (rb < p ? p : rb); q + 96 < re; q++) {
                        if (q[0] != 'u' || q[1] != '_' || q[2] != 'm') continue;
                        if (!std::memcmp(q, k_q, sizeof k_q)) { if (pm_patch_lang_at(q, sizeof k_q - 1, "Quit Game")) { lq = true; std::fprintf(stderr, "[pausemenu] text Quit Game @0x%08X\n", (uint32_t)(q - vm_base)); } }
                        else if (!std::memcmp(q, k_s, sizeof k_s)) { if (pm_patch_lang_at(q, sizeof k_s - 1, "Settings")) { ls = true; std::fprintf(stderr, "[pausemenu] text Settings @0x%08X\n", (uint32_t)(q - vm_base)); } }
                    }
            }
            p = rb + m.size;
        }
        if (!(script_done && lq && ls)) std::this_thread::sleep_for(seconds(2));
    }
    std::fprintf(stderr, "[pausemenu] done: script %s, %d texts\n", script_done ? "ok" : "NOT FOUND", (int)lq + (int)ls);
}
static void pm_start()
{
    const char* e = std::getenv("PAUSE_NATIVE");
    if (e && *e == '0') return;
    /* registered before the game runs (the lookup hash is not built for concurrent inserts) */
    ppu_register_function(PM_FN_SETTINGS, pm_native_settings);
    ppu_register_function(PM_FN_QUIT, pm_native_quit);
    ppu_register_function(PM_FN_TRIM, pm_native_trim);
    std::thread(pm_thread).detach();
}
#else
/* The native pause-menu entries are drawn and answered by the Win32 overlay
 * (rsx_ui_overlay.c); without it the game's own pause menu is unchanged. */
static void pm_start(void) {}
#endif

extern "C" void ps3_load_prx_modules(void)
{
    g_uiov_paused_fn = infamous_paused_probe;
    infamous_clock_probe::init();
    ps3_sampler_start();
    infamous_hangdump_start();
    pm_start();   /* v195 native pause-menu entries */
    infamous_trainer_install();   /* F10 trainer (trainer.cpp); TRAINER=0 = off */
    ps3_hle_register_ctx(0x8A85674Du, "_cellSpursLFQueuePushBody",
                         infamous_cellSpursLFQueuePushBody);
}

/* ---------------------------------------------------------------------------
 * Example: Override a specific NID
 *
 * The module system resolves function imports by NID.  To replace a default
 * HLE stub with custom logic, register your function at init time.
 * -----------------------------------------------------------------------*/

/*
 * Example: override cellGcmSetFlipMode (NID 0xA53D12AE) to force vsync.
 *
 *   static void my_cellGcmSetFlipMode(u32 mode)
 *   {
 *       printf("[hook] cellGcmSetFlipMode: forcing VSYNC (was %u)\n", mode);
 *       // Force vsync regardless of what the game requested
 *       cellGcmSetFlipMode(CELL_GCM_DISPLAY_VSYNC);
 *   }
 *
 * Then in your init code:
 *
 *   ps3::modules::override_nid(0xA53D12AE,
 *       reinterpret_cast<void*>(my_cellGcmSetFlipMode));
 */

/* ---------------------------------------------------------------------------
 * Example: Hook into module loading
 *
 * You can intercept cellSysmoduleLoadModule to perform custom actions
 * when a particular module is loaded.
 * -----------------------------------------------------------------------*/

/*
 *   static s32 my_loadmodule_hook(u16 id)
 *   {
 *       printf("[hook] Module 0x%04X loaded\n", id);
 *
 *       if (id == CELL_SYSMODULE_AUDIO) {
 *           // Perform custom audio backend initialization
 *           init_my_audio_backend();
 *       }
 *
 *       // Call the original implementation
 *       return cellSysmoduleLoadModule(id);
 *   }
 */

/* ---------------------------------------------------------------------------
 * Game-specific patches
 *
 * Sometimes you need to patch recompiled code at specific addresses.
 * Use the patch system to modify behavior without editing generated code.
 * -----------------------------------------------------------------------*/

/*
 *   // Skip an integrity check at guest address 0x00812FA0
 *   ps3::patches::nop_range(0x00812FA0, 0x00812FB0);
 *
 *   // Replace a function call with a direct return
 *   ps3::patches::force_return(0x009A1000, 0);  // return 0
 */

/* ---------------------------------------------------------------------------
 * Movie texture pool race (patch in recompiled/ppu_recomp_000.cpp, func_00185050)
 *
 * The SP Movie Player thread lays out its Bink Y/cR/cB textures from a pool
 * pointer at player+0x4C that the main thread stores later, while the player
 * holds its own lock. On a PS3 the player is still reading the .bik header
 * from disc by then; here it gets there first, reads 0, turns every plane
 * into IO offset -1 / EA 0, and the SPU decoder then writes each decoded
 * frame over the game's own code and data starting at address 0.
 *
 * The patch makes setup report "not ready" (as it already does for other
 * unmet conditions) when the pool is still 0, so the player retries on its
 * next pass, after it has dropped the lock and the main thread has filled
 * the pointer in.
 * -----------------------------------------------------------------------*/
uint32_t infamous_movie_pool(uint32_t player)
{
    uint32_t pool = vm_read32(player + 0x4C);
    static int n = 0;
    if (n++ < 8)
        std::fprintf(stderr, "[infamous] movie texture pool player=0x%08X pool=0x%08X%s\n",
                     player, pool, pool ? "" : " (not ready, retrying)");
    return pool;
}

/* The player's setup (func_00185050) runs before the main thread has stored
 * the texture pool at player+0x4C. On a PS3 the player is still reading the
 * .bik header from disc at that point; here it is not. Returning "not ready"
 * (the patch below) is NOT retried by the player -- func_0018621C treats it as
 * a failed setup, the movie is abandoned after one empty Bink pass (2.4M SPU
 * insns), and the boot then wedges with the main thread holding lwmutex
 * 0x925818. Wait for the pool instead, before setup takes the movie lock, so
 * the main thread is never blocked by us. Bounded so a movie that genuinely has
 * no pool still falls through to the old path. INFAMOUS_MOVIE_POOL_WAIT_MS
 * (default 0 = disabled since the real lwcond fix: the player's own +0x48
 * handshake now delivers the pool, and this wait cost 10 s on every TV movie,
 * which never use the shared pool). */
void infamous_movie_pool_wait(uint32_t player)
{
    static int limit_ms = -1;
    if (limit_ms < 0) {
        const char* e = std::getenv("INFAMOUS_MOVIE_POOL_WAIT_MS");
        limit_ms = e ? std::atoi(e) : 0;   /* 2026-10-03: off by default; the real lwcond handshake delivers the pool */
    }
    if (!player || limit_ms == 0 || vm_read32(player + 0x4C)) return;
    int waited = 0;
    while (waited < limit_ms && !vm_read32(player + 0x4C)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        waited++;
    }
    std::fprintf(stderr, "[infamous] movie texture pool wait player=0x%08X -> pool=0x%08X after ~%d ms\n",
                 player, vm_read32(player + 0x4C), waited);
}

/* v151 [CELL] probe (called from func_0012B6F0 = streaming-cell "ready" bit test, hand patch in ppu_recomp_002.cpp).
 * Measures how long the game waits for a cell to become ready after it first asks (actors/props are held until
 * their cell is ready). Every 10 s: queries, not-ready answers, cells that turned ready + wait mean/max, cells
 * still waiting (and for how long), ready bits set. CELL_PROBE=0 disables. */
#include <chrono>
extern "C" void inf_cell_probe(uint32_t g, int32_t idx, int ready)
{
    static int on = -1;
    if (on < 0) { const char* e = std::getenv("CELL_PROBE"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || idx < 0 || idx >= 128) return;
    using C = std::chrono::steady_clock;
    static C::time_point win0 = C::now(), first[128]; static bool waiting[128];
    static unsigned long q = 0, nr = 0, done = 0; static double sum_ms = 0, max_ms = 0; static uint32_t last_g = 0;
    const C::time_point now = C::now();
    q++; last_g = g;
    if (!ready) { nr++; if (!waiting[idx]) { waiting[idx] = true; first[idx] = now; } }
    else if (waiting[idx]) {
        waiting[idx] = false; done++;
        const double ms = std::chrono::duration<double, std::milli>(now - first[idx]).count();
        sum_ms += ms; if (ms > max_ms) max_ms = ms;
    }
    if (now - win0 >= std::chrono::seconds(10)) {
        int nw = 0; double oldest = 0;
        for (int i = 0; i < 128; i++) if (waiting[i]) { nw++; double ms = std::chrono::duration<double, std::milli>(now - first[i]).count(); if (ms > oldest) oldest = ms; }
        int bits = 0;
        if (last_g) for (int w = 0; w < 4; w++) { uint32_t v = vm_read32(last_g + 4u * w); while (v) { bits += v & 1u; v >>= 1; } }
        std::fprintf(stderr, "[CELL] v151 10s: %lu ready-queries (%lu not ready); %lu cells became ready, wait mean %.0f ms max %.0f ms; %d still waiting (oldest %.0f ms); %d ready bits\n",
                     q, nr, done, done ? sum_ms / done : 0.0, max_ms, nw, oldest, bits);
        win0 = now; q = nr = done = 0; sum_ms = max_ms = 0;
    }
}

/* ============================================================================================================
 * v216 ANIMATION LOD (experiment). Every animated object has its own BatchJobList job, kicked lazily by
 * func_000285A0(obj) (obj+0x694 = kicked flag, obj+0x684 = job object) and polled by the frame update until done
 * (the main thread's biggest wait, w16). With ANIM_LOD=1 (or ABX 0x800000 for an in-run A/B) objects whose last
 * job had a SMALL parameter block (<= ANIM_LOD_MAX, default 0x10 units -- the low-detail skeletons) skip the kick
 * on every other frame: func_000285A0 reports "done" without a new job, so the object keeps the pose its previous
 * job produced. Objects are split into two halves by address so the skipped work is spread over both frames.
 * func_00401570 (AddJob wrapper, r3 = obj+0x684) records each object's job size class (descriptor word 8 hi16;
 * only BatchJobList jobs of the animation module 0x61D000 are counted). [ANIMLOD] every ~10 s.
 * ============================================================================================================ */
extern "C" volatile int g_abx;
extern "C" unsigned ppu_boot_frames_presented(void);
namespace {
struct AlEnt { uint32_t obj; uint16_t sz; uint16_t player; uint32_t chk; };   /* v219: player = Cole's object (never skipped) */
static AlEnt s_al[8192];
static unsigned long long s_al_kick[16], s_al_skip, s_al_frames0, s_al_calls;
static unsigned s_al_last_frame = 0;
/* v219 COLE GUARD: the LOD must never skip the player's own animation job (v217: player anim drives movement ->
 * weird gravity / speed-ups). Cole's world position is the vec4 at 0x00915430 (found with the MEMCMD memory tool:
 * x y z in cm, Z up). Each animation object is scanned (every ~2 s until found) for that position -- 3 consecutive
 * floats, or a 4x4 matrix column (stride 16) -- within 2 cm; a match marks it as the player and it is never skipped.
 * ANIM_LOD_PLAYER_POS=0 disables the guard (old behaviour). */
static uint32_t s_al_player = 0; static uint16_t s_al_player_sz = 0; static unsigned s_al_player_off = 0;
static uint32_t s_al_unnoted[256]; static int s_al_nun = 0; static uint32_t s_al_player_unnoted = 0;   /* v219c */
static float al_f(uint32_t a) { uint32_t u = vm_read32(a); float f; std::memcpy(&f, &u, 4); return f; }
static bool al_is_player(uint32_t obj, unsigned* off_out)
{
    static int on = -1; if (on < 0) { const char* e = std::getenv("ANIM_LOD_PLAYER_POS"); on = (e && *e == '0') ? 0 : 1; }
    if (!on) return false;
    const float px = al_f(0x00915430u), py = al_f(0x00915434u), pz = al_f(0x00915438u);
    if (!(px == px) || (px == 0.0f && py == 0.0f)) return false;
    /* v219b: the word after the position (0x00915444, written by guest fn 0x0049A9C0) is Cole's ENTITY pointer
     * (found with PPU_WWATCH=00915430). His anim object is the one the entity points into, or that points into it. */
    const uint32_t ent = vm_read32(0x00915444u);
    if (ent >= 0x10000u && ent < 0xF0000000u) {
        for (uint32_t o = 0; o < 0x3000u; o += 4) {
            const uint32_t w = vm_read32(ent + o);
            if (w >= obj && w < obj + 0x800u) { *off_out = 0x40000000u | o; return true; }
        }
        for (uint32_t o = 0; o < 0x800u; o += 4) {
            const uint32_t w = vm_read32(obj + o);
            if (w >= ent && w < ent + 0x3000u) { *off_out = 0x20000000u | o; return true; }
        }
    }
    for (uint32_t o = 0; o < 0xC00u; o += 4) {
        const float x = al_f(obj + o);
        if (!(std::fabs(x - px) <= 2.0f)) continue;
        if (std::fabs(al_f(obj + o + 4) - py) <= 2.0f && std::fabs(al_f(obj + o + 8) - pz) <= 2.0f) { *off_out = o; return true; }
        if (std::fabs(al_f(obj + o + 16) - py) <= 2.0f && std::fabs(al_f(obj + o + 32) - pz) <= 2.0f) { *off_out = o | 0x80000000u; return true; }
    }
    return false;
}
static int al_max(void) { static int v = -1; if (v < 0) { const char* e = std::getenv("ANIM_LOD_MAX"); v = e ? (int)std::strtol(e, 0, 0) : 0x30; } return v; }
static int al_env(void) { static int v = -1; if (v < 0) { const char* e = std::getenv("ANIM_LOD"); v = (e && *e == '1') ? 1 : 0;
    std::fprintf(stderr, "[ANIMLOD] v216 animation LOD: %s (ANIM_LOD=1 always, ABX 0x800000 in-run A/B), small jobs <= 0x%X units\n", v ? "ON" : "A/B bit only", al_max()); } return v; }
static bool al_enabled(void) { return al_env() || (g_abx & 0x800000); }
static AlEnt* al_find(uint32_t obj, bool add)
{
    uint32_t h = (obj * 2654435761u) >> 19;
    for (int k = 0; k < 64; k++, h = (h + 1) & 8191) {
        if (s_al[h].obj == obj) return &s_al[h];
        if (!s_al[h].obj) { if (!add) return nullptr; s_al[h].obj = obj; return &s_al[h]; }
    }
    return nullptr;
}
static void al_report(unsigned frame)
{
    if (!s_al_frames0) { s_al_frames0 = frame; return; }
    if (frame - s_al_frames0 < 600) return;
    const double nf = (double)(frame - s_al_frames0);
    char line[512]; int o = 0; line[0] = 0; unsigned long long tot = 0;
    for (int i = 0; i < 16; i++) { tot += s_al_kick[i]; if (s_al_kick[i]) o += std::snprintf(line + o, sizeof line - o, " 0x%02X:%.1f", i * 16, s_al_kick[i] / nf); }
    std::fprintf(stderr, "[ANIMLOD] per frame: %.1f anim jobs kicked (size class:count%s), %.1f skipped by LOD (abx 0x%X) | player obj 0x%08X class 0x%X | unrecorded objs %d (Cole 0x%08X)\n",
                 tot / nf, line, s_al_skip / nf, (unsigned)g_abx, s_al_player, (unsigned)s_al_player_sz, s_al_nun, s_al_player_unnoted);
    { static int dump = -1; if (dump < 0) { const char* e = std::getenv("ANIMLOD_DUMP"); dump = (e && *e == '1') ? 1 : 0; }
      if (dump && !s_al_player_unnoted) for (int i = 0; i < s_al_nun; i++) { unsigned off = 0;   /* v219c re-check */
          if (al_is_player(s_al_unnoted[i], &off)) { s_al_player_unnoted = s_al_unnoted[i];
              std::fprintf(stderr, "[ANIMLOD] v219c COLE = unrecorded anim object 0x%08X (match 0x%X) -> never skipped by the size LOD\n", s_al_unnoted[i], off); break; } }
      if (dump) {   /* v219: every anim object (address:size class) -- to locate Cole's (compare with MEMCMD find3) */
          char b[2048]; int k = 0, n = 0; b[0] = 0;
          for (int i = 0; i < 8192 && k < (int)sizeof b - 24; i++) if (s_al[i].obj) { n++;
              k += std::snprintf(b + k, sizeof b - k, " %08X:%X%s", s_al[i].obj, s_al[i].sz, s_al[i].player ? "P" : ""); }
          std::fprintf(stderr, "[ANIMLOD] objs (%d):%s\n", n, b); } }
    for (int i = 0; i < 16; i++) s_al_kick[i] = 0;
    s_al_skip = 0; s_al_frames0 = frame;
}
}
extern "C" void infamous_anim_lod_note(ppu_context* ctx)
{
    /* The shipping launcher disables animation LOD. Keep its experimental
     * bookkeeping out of the always-hot AddJob path in that configuration:
     * besides hashing every animation job, it periodically scans several KB
     * of guest memory per object to identify the player. The live ABX bit can
     * still enable the experiment without restarting. */
    if (!al_enabled()) return;
    const uint32_t jo = (uint32_t)ctx->gpr[3];
    if (jo < 0x10000u || jo >= 0xF0000000u) return;
    { static int n = 12; if (n < 12) { n++;   /* v216b (off since v218): what the AddJob wrapper really gets (first calls) */
        const uint32_t a0 = vm_read32(jo), a1 = vm_read32(jo + 4);
        std::fprintf(stderr, "[ANIMLOD] note r3=0x%08X [0]=0x%08X [4]=0x%08X lr=0x%08X |", jo, a0, a1, (uint32_t)ctx->lr);
        for (int i = 0; i < 12 && a0 >= 0x10000u && a0 < 0xF0000000u; i++) std::fprintf(stderr, " %08X", vm_read32(a0 + 4u * i));
        std::fprintf(stderr, " |");
        for (int i = 0; i < 12 && a1 >= 0x10000u && a1 < 0xF0000000u; i++) std::fprintf(stderr, " %08X", vm_read32(a1 + 4u * i));
        std::fprintf(stderr, "\n"); } }
    /* v218: [0] = the job's parameter block (per-frame 0x48CFxxxx command stream), [4] = its size in 16-byte units
     * (perf217: 0x30 / 0x40 ...). Only calls from the animation kick func_000282C8 (lr 0x28554) count. */
    if ((uint32_t)ctx->lr != 0x00028554u) return;
    const uint32_t szc = vm_read32(jo + 4);
    if (!szc || szc > 0x1000u) return;
    if (AlEnt* e = al_find(jo - 0x684u, true)) {
        e->sz = (uint16_t)szc;
        if (e->player) s_al_player_sz = (uint16_t)szc;
        else {
            const unsigned fr = ppu_boot_frames_presented();
            if (!e->chk || fr - e->chk >= 120) { e->chk = fr ? fr : 1; unsigned off = 0;
                if (al_is_player(e->obj, &off)) { e->player = 1; s_al_player = e->obj; s_al_player_sz = (uint16_t)szc; s_al_player_off = off;
                    std::fprintf(stderr, "[ANIMLOD] v219 Cole's animation object = 0x%08X (%s +0x%X, entity 0x%08X), job size class 0x%X -> never skipped\n",
                                 e->obj, (off & 0x40000000u) ? "entity points into it from" : (off & 0x20000000u) ? "it points into the entity from" :
                                 (off & 0x80000000u) ? "position matrix column at" : "position at", off & 0xFFFFu, vm_read32(0x00915444u), szc); } }
        }
    }
    s_al_kick[szc >= 0x100u ? 15 : (szc >> 4) & 15]++;
}
/* trainer.cpp: the Player Model stand-in's animation job is replaced by Cole's bones (1 = skip, report done) */
extern "C" int infamous_trainer_anim_hook(uint32_t anim_obj);
extern "C" int infamous_anim_lod_skip(ppu_context* ctx)
{
    if (infamous_trainer_anim_hook((uint32_t)ctx->gpr[3])) return 1;
    if (!al_enabled()) return 0;
    const unsigned frame = ppu_boot_frames_presented();
    if (frame != s_al_last_frame) { s_al_last_frame = frame; al_report(frame); }
    const uint32_t obj = (uint32_t)ctx->gpr[3];
    if (obj < 0x10000u || obj >= 0xF0000000u) return 0;
    if (vm_read8(obj + 0x694)) return 0;                  /* a job is in flight: let the game poll it */
    const AlEnt* e = al_find(obj, false);
    if (!e) {   /* v219c: objects the skip hook sees that the size table never recorded (other kick paths) -- never
                 * skipped in MAX mode; logged once each and checked for Cole (ANIMLOD_DUMP=1) */
        bool known = false; for (int i = 0; i < s_al_nun; i++) if (s_al_unnoted[i] == obj) { known = true; break; }
        if (!known && s_al_nun < 256) { s_al_unnoted[s_al_nun++] = obj;
            static int dump = -1; if (dump < 0) { const char* d = std::getenv("ANIMLOD_DUMP"); dump = (d && *d == '1') ? 1 : 0; }
            if (dump) { unsigned off = 0; const bool pl = al_is_player(obj, &off); if (pl) s_al_player_unnoted = obj;
                std::fprintf(stderr, "[ANIMLOD] v219c unrecorded anim object 0x%08X (never skipped unless ANIM_LOD_ALL)%s\n", obj,
                             pl ? " = COLE (entity/position match)" : ""); } }
    }
    static int s_all = -1; if (s_all < 0) { const char* v = std::getenv("ANIM_LOD_ALL"); s_all = (v && *v == '1') ? 1 : 0; }
    if (e && e->player) return 0;                          /* v219: never Cole */
    if (!s_all && (!e || !e->sz || e->sz > al_max())) return 0;   /* unknown or a detailed skeleton: always update */
    if (((frame + (obj >> 9)) & 1u) != 0) return 0;      /* this object's update frame */
    s_al_skip++;
    return 1;
}

/* v233: car pop-in probe + CAR_DIST_SCALE / CAR_TRAFFIC_RADIUS knobs (inert unless set; see car_v233.h) */
#include "car_v233.h"

/* v243K (from PR #25): the game language for the in-game Settings menu's Language row: PS3_LANG (inert unless it
 * names a language the US release lacks; see the header) */
#include "infamous_lang.h"
