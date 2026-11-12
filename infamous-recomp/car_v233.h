/* ============================================================================================================
 * v233 -- CARS POPPING IN: probe + knobs (included once from stubs.cpp; everything is inert unless an
 * env switch below is set).
 *
 * RE (EBOOT):
 *  * func_00114F60(inst r3, view r4, ..) = PPU visibility test of one dynamic model instance (called by
 *    Lo::IsVisible func_00280440 with this+0x11C, and from 0xA9DE0 / 0x1687C0). Distance band test:
 *    visible only if tab[inst+0x2E]^2 <= dx^2+dy^2+(0.6dz)^2 < tab[inst+0x2F + 1]^2 (tab = EBOOT 0x0078AD80 x
 *    camera LOD scale [view+0x2E0]+0x2A8; the same table LOD_CLASS_SCALE scales), then per-part LOD bands
 *    (func_001122A8, part +0xC/+0xD classes, same d^2 in f1 = f31) and a frustum test.
 *  * TrafficManager singleton = [0x00917144] (ctor func_00303748: VEHICLE_MAX +0x60 = 25, DRIVER_MAX +0x64 = 3,
 *    TRAFFIC_RADIUS +0xA0 = 14000 cm, DRIVER_RADIUS +0xA4 = 2500 cm; update func_00306370 called from the main loop:
 *    slots [+0x130] (count +0x134, stride 0x330, slot+0x80 = position, slot+0x180 = -1 empty), vehicle objects
 *    [+0x140][slot]; TRAFFIC_RADIUS setter func_00302D60 also sets the dirty byte 0x00917140).
 *
 * Hooks (hand patches in recompiled/ppu_recomp_000.cpp, func_00114F60):
 *    loc_001151C4 (common exit, r3 = result):   car_v233_vis(ctx)
 *    after "lfs f31,0x90(r1)" (0x115088, d^2):   ctx->fpr[31] *= car_v233_dscale(ctx)
 *
 * Env:
 *   CARVIS=1                 [CARVIS] log: traffic spawn/despawn (distance to Cole / camera, in front or not),
 *                            first-visible distance of every vehicle instance, 10 s summaries.
 *   CAR_DIST_SCALE=<k>       vehicle instances (traffic vehicles + instances sharing a vehicle's model pointer,
 *                            i.e. parked cars) are distance-tested at d/k: their draw distance AND their LOD switch
 *                            distances grow by k. Nothing else changes. Default off (1).
 *   CAR_TRAFFIC_RADIUS=<cm>  override TrafficManager TRAFFIC_RADIUS (game default 14000). Default off.
 *   CAR_SPAWN_MIN_DIST=<cm>  traffic spawn test (func_00306370 @0x307464..0x3074A8): a candidate spawn point that is
 *                            INSIDE the camera frustum (func_003323E0, sphere radius [0x76B580] = 1000 cm) is accepted
 *                            when it is farther than sqrt([0x76B564]) = 5000 cm from the camera. This knob rewrites that
 *                            constant (d^2) -> visible spawns only beyond <cm>. Default off (game 5000).
 *   CAR_SPAWN_VIS_RADIUS=<cm> radius of that frustum test (game 1000): larger = points near the screen edge also count
 *                            as visible (cars spawned just off-screen pop in less when the camera turns). Default off.
 *   CAR_V233_PHASE=1         in-run A/B for both knobs: active on even PS3_AB phases, original on odd ones.
 * ============================================================================================================ */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>

extern "C" unsigned ppu_boot_frames_presented(void);
extern "C" volatile int g_ab_phase;

namespace carv {
static int s_init = 0, s_vis = 0, s_phase = 0;
static float s_k = 1.0f, s_radius = 0.0f, s_spmin = 0.0f, s_spvr = 0.0f;
static const uint32_t EA_SPAWN_MIN2 = 0x0076B564u, EA_SPAWN_VISR = 0x0076B580u;   /* traffic-module literal pool */
static inline bool va(uint32_t a) { return a >= 0x10000u && a < 0xF0000000u && !(a & 3u); }
static inline float rf(uint32_t a) { uint32_t u = vm_read32(a); float f; std::memcpy(&f, &u, 4); return f; }
static inline void wf(uint32_t a, float f) { uint32_t u; std::memcpy(&u, &f, 4); vm_write32(a, u); }
static void init()
{
    if (s_init) return;
    s_init = 1;
    const char* e;
    e = std::getenv("CARVIS");             s_vis = (e && *e == '1') ? 1 : 0;
    e = std::getenv("CAR_DIST_SCALE");     s_k = e ? (float)std::atof(e) : 1.0f; if (!(s_k > 0.05f && s_k < 20.0f)) s_k = 1.0f;
    e = std::getenv("CAR_TRAFFIC_RADIUS"); s_radius = e ? (float)std::atof(e) : 0.0f; if (!(s_radius > 100.0f && s_radius < 200000.0f)) s_radius = 0.0f;
    e = std::getenv("CAR_SPAWN_MIN_DIST"); s_spmin = e ? (float)std::atof(e) : 0.0f; if (!(s_spmin > 100.0f && s_spmin < 100000.0f)) s_spmin = 0.0f;
    e = std::getenv("CAR_SPAWN_VIS_RADIUS"); s_spvr = e ? (float)std::atof(e) : 0.0f; if (!(s_spvr > 10.0f && s_spvr < 50000.0f)) s_spvr = 0.0f;
    /* signature check: only patch the EBOOT constants if they hold the original values */
    if ((s_spmin > 0.0f || s_spvr > 0.0f) && !(rf(EA_SPAWN_MIN2) == 2.5e7f && rf(EA_SPAWN_VISR) == 1000.0f)) {
        std::fprintf(stderr, "[CARVIS] v233A spawn constants not found (0x%08X=%g 0x%08X=%g) -> CAR_SPAWN_* ignored\n",
                     EA_SPAWN_MIN2, rf(EA_SPAWN_MIN2), EA_SPAWN_VISR, rf(EA_SPAWN_VISR));
        s_spmin = s_spvr = 0.0f;
    }
    e = std::getenv("CAR_V233_PHASE");     s_phase = (e && *e == '1') ? 1 : 0;
    if (s_vis || s_k != 1.0f || s_radius > 0.0f || s_spmin > 0.0f || s_spvr > 0.0f)
        std::fprintf(stderr, "[CARVIS] v233A car probe %s, CAR_DIST_SCALE x%.2f, CAR_TRAFFIC_RADIUS %.0f, CAR_SPAWN_MIN_DIST %.0f, CAR_SPAWN_VIS_RADIUS %.0f (0 = game)%s\n",
                     s_vis ? "ON" : "off", s_k, s_radius, s_spmin, s_spvr, s_phase ? ", CAR_V233_PHASE: even=new, odd=original" : "");
}
static inline bool knobs_now() { return !s_phase || !(g_ab_phase & 1); }

/* ---- instance table (open addressing, cleared lazily by lifetime) ---- */
enum { F_TRAFFIC = 1, F_MODEL = 2, F_VISLOG = 4, F_QLOG = 8 };
struct Inst { uint32_t inst, model; unsigned first, last, vis; float dfirst, dvis; uint8_t flags, nearc, farc, view; uint32_t lr; };
static Inst s_tab[8192];
static Inst* find(uint32_t inst, bool add)
{
    uint32_t h = (inst * 2654435761u) >> 19;
    for (int k = 0; k < 128; k++, h = (h + 1) & 8191) {
        if (s_tab[h].inst == inst) return &s_tab[h];
        if (!s_tab[h].inst) { if (!add) return nullptr; std::memset(&s_tab[h], 0, sizeof(Inst)); s_tab[h].inst = inst; return &s_tab[h]; }
    }
    return nullptr;
}
/* vehicle model pointers ([inst+0]) learned from traffic vehicles */
static uint32_t s_models[64]; static int s_nmodels = 0;
static bool is_vmodel(uint32_t m) { for (int i = 0; i < s_nmodels; i++) if (s_models[i] == m) return true; return false; }
static void add_vmodel(uint32_t m) { if (!va(m) || is_vmodel(m) || s_nmodels >= 64) return; s_models[s_nmodels++] = m;
    if (s_vis) std::fprintf(stderr, "[CARVIS] vehicle model #%d = 0x%08X\n", s_nmodels, m); }

/* ---- per-frame state ---- */
static unsigned s_frame = 0;
static float s_cam[3], s_cole[3];
static uint32_t s_views[8]; static unsigned long s_vcalls[8];
static uint32_t s_slot_obj[64];
/* stats (10 s windows) */
static unsigned long s_spawn_n, s_spawn_front, s_spawn_lt60, s_despawn_n, s_vis_n, s_vis_new, s_vis_hist[16], s_spawn_hist[16];
static unsigned long s_gen_new, s_gen_newvis, s_gen_newvis_near;
static double s_t0 = 0;

static int view_idx(uint32_t v)
{
    for (int i = 0; i < 8; i++) { if (s_views[i] == v) return i; if (!s_views[i]) { s_views[i] = v; return i; } }
    return 7;
}
static float dist3(const float* a, const float* b) { const float x = a[0]-b[0], y = a[1]-b[1], z = a[2]-b[2]; return std::sqrt(x*x + y*y + z*z); }
/* angle (deg) between the horizontal camera->Cole direction (third-person view direction) and camera->p */
static float view_angle(const float* p)
{
    float fx = s_cole[0] - s_cam[0], fy = s_cole[1] - s_cam[1];
    float px = p[0] - s_cam[0], py = p[1] - s_cam[1];
    const float fl = std::sqrt(fx*fx + fy*fy), pl = std::sqrt(px*px + py*py);
    if (fl < 1.0f || pl < 1.0f) return 0.0f;
    float c = (fx*px + fy*py) / (fl * pl); if (c > 1) c = 1; if (c < -1) c = -1;
    return std::acos(c) * 57.29578f;
}
static double now_s() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

static void traffic_tick()
{
    if (s_spmin > 0.0f) { const float d = knobs_now() ? s_spmin : 5000.0f; if (rf(EA_SPAWN_MIN2) != d * d) wf(EA_SPAWN_MIN2, d * d); }
    if (s_spvr > 0.0f)  { const float r = knobs_now() ? s_spvr : 1000.0f;  if (rf(EA_SPAWN_VISR) != r) wf(EA_SPAWN_VISR, r); }
    const uint32_t tm = vm_read32(0x00917144u);
    if (!va(tm)) return;
    if (s_radius > 0.0f) {
        const float want = knobs_now() ? s_radius : 14000.0f;   /* odd phase = game default */
        if (rf(tm + 0xA0) != want) { wf(tm + 0xA0, want); vm_write8(0x00917140u, 1); }
    }
    const uint32_t slots = vm_read32(tm + 0x130), objs = vm_read32(tm + 0x140);
    int n = (int)vm_read32(tm + 0x134);
    if (!va(slots) || !va(objs) || n <= 0) return;
    if (n > 64) n = 64;
    for (int i = 0; i < n; i++) {
        const uint32_t sl = slots + 0x330u * (uint32_t)i;
        uint32_t obj = vm_read32(objs + 4u * (uint32_t)i);
        if ((int32_t)vm_read32(sl + 0x180) == -1) obj = 0;
        if (obj && !va(obj)) obj = 0;
        const uint32_t prev = s_slot_obj[i];
        if (obj == prev) {
            if (obj) { const uint32_t inst = vm_read32(obj + 0x11C);
                if (va(inst)) { Inst* e = find(inst, true); if (e) e->flags |= F_TRAFFIC; } }
            continue;
        }
        float p[3] = { rf(sl + 0x80), rf(sl + 0x84), rf(sl + 0x88) };
        if (prev) {
            s_despawn_n++;
            if (s_vis) std::fprintf(stderr, "[CARVIS] f%u despawn slot %d obj 0x%08X at d_cole %.0f d_cam %.0f ang %.0f\n",
                                    s_frame, i, prev, dist3(p, s_cole), dist3(p, s_cam), view_angle(p));
        }
        s_slot_obj[i] = obj;
        if (obj) {
            const uint32_t inst = vm_read32(obj + 0x11C);
            uint32_t model = 0;
            if (va(inst)) { model = vm_read32(inst); add_vmodel(model);
                Inst* e = find(inst, true); if (e) e->flags |= F_TRAFFIC; }
            const float dc = dist3(p, s_cam), ang = view_angle(p);
            s_spawn_n++; if (ang < 50.0f) s_spawn_front++; if (dc < 6000.0f) s_spawn_lt60++;
            s_spawn_hist[dc >= 15000.0f ? 15 : (int)(dc / 1000.0f)]++;
            if (s_vis) std::fprintf(stderr, "[CARVIS] f%u spawn slot %d obj 0x%08X inst 0x%08X model 0x%08X at (%.0f %.0f %.0f) d_cole %.0f d_cam %.0f ang %.0f%s | radius %.0f max %d\n",
                                    s_frame, i, obj, inst, model, p[0], p[1], p[2], dist3(p, s_cole), dc, ang, ang < 50.0f ? " FRONT" : "",
                                    rf(tm + 0xA0), (int)vm_read32(tm + 0x60));
        }
    }
}

static void report()
{
    const double t = now_s();
    if (s_t0 == 0) { s_t0 = t; return; }
    if (t - s_t0 < 10.0) return;
    s_t0 = t;
    const uint32_t tm = vm_read32(0x00917144u);
    char vh[256]; int o = 0; vh[0] = 0;
    for (int i = 0; i < 16; i++) o += std::snprintf(vh + o, sizeof vh - o, " %lu", s_vis_hist[i]);
    char sh[256]; int q = 0; sh[0] = 0;
    for (int i = 0; i < 16; i++) q += std::snprintf(sh + q, sizeof sh - q, " %lu", s_spawn_hist[i]);
    char vc[256]; int r = 0; vc[0] = 0;
    for (int i = 0; i < 8 && s_views[i]; i++) r += std::snprintf(vc + r, sizeof vc - r, " %08X:%lu", s_views[i], s_vcalls[i]);
    int act = 0; for (int i = 0; i < 64; i++) if (s_slot_obj[i]) act++;
    if (s_vis) {
        std::fprintf(stderr, "[CARVIS] 10s: traffic %d active (max %d, radius %.0f, driver_r %.0f, visible-spawn min %.0f, vis-test r %.0f) | spawns %lu (front %lu, <60m %lu) despawns %lu | spawn d_cam hist/10m:%s\n",
                     act, va(tm) ? (int)vm_read32(tm + 0x60) : -1, va(tm) ? rf(tm + 0xA0) : 0.0f, va(tm) ? rf(tm + 0xA4) : 0.0f,
                     std::sqrt(rf(EA_SPAWN_MIN2)), rf(EA_SPAWN_VISR),
                     s_spawn_n, s_spawn_front, s_spawn_lt60, s_despawn_n, sh);
        std::fprintf(stderr, "[CARVIS] 10s: vehicle insts first-visible %lu (%lu visible within 2 frames of first query) | first-visible d hist/10m:%s | generic new %lu, new+visible %lu (<40m %lu) | views%s | k %.2f%s\n",
                     s_vis_n, s_vis_new, vh, s_gen_new, s_gen_newvis, s_gen_newvis_near, vc, s_k, knobs_now() ? "" : " (phase: original)");
    }
    s_spawn_n = s_spawn_front = s_spawn_lt60 = s_despawn_n = s_vis_n = s_vis_new = 0;
    s_gen_new = s_gen_newvis = s_gen_newvis_near = 0;
    std::memset(s_vis_hist, 0, sizeof s_vis_hist); std::memset(s_spawn_hist, 0, sizeof s_spawn_hist); std::memset(s_vcalls, 0, sizeof s_vcalls);
}

static void frame_tick(unsigned fr)
{
    s_frame = fr;
    s_cole[0] = rf(0x00915430u); s_cole[1] = rf(0x00915434u); s_cole[2] = rf(0x00915438u);
    traffic_tick();
    report();
}
} /* namespace carv */

/* CAR_PARK_SCALE=<k> (default 1 = game): size of the parked-car spawner's regions. The spawner (update func_00249A30,
 * every 360 ms from the main loop via func_003248E0; spawns func_00249230 -> vehicle create func_00218F30, the same
 * path TrafficManager uses) asks the spatial database which parking spots lie inside a point shape from its data,
 * rotated to the camera heading and scaled by f2 in func_002488C8: 1.0 for the spawn region, 1.25 for the keep region
 * (func_002491F0; func_00248B88 passes 1.0). car_park_scale_mode multiplies f2 by k in func_002491F0, so the regions
 * grow by k and wrecks appear / vanish k times further out (found 2026-10-30).
 * The same region code serves other populators (street-lamp fill lights: scaling every caller put many more lights
 * in range and broke them), so only spawner instances that create vehicles are scaled: func_00249A30 brackets its
 * update with car_pop_enter / car_pop_leave, and a vehicle created inside (factory func_004F5240 -> car_pop_vehicle)
 * marks that instance. Unmarked instances, and other callers of the region code, keep the game's size. */
static volatile uint32_t s_pop_cur = 0;
static uint32_t s_pop_veh[64];
static int s_pop_nveh = 0;
static bool car_pop_is_veh(uint32_t self) { for (int i = 0; i < s_pop_nveh; i++) if (s_pop_veh[i] == self) return true; return false; }
extern "C" void car_pop_enter(uint32_t self) { s_pop_cur = self; }
extern "C" void car_pop_leave(void) { s_pop_cur = 0; }
/* Hook in func_002491F0 (only called by func_00249A30, three times per update with r8 = mode: 1 = list [cfg+0x20],
 * scale 1.0; 0 = list [cfg+0x0], scale 1.25; 2 = list [cfg+0x40], scale 1.0, aimed by another input).
 * CAR_PARK_MODES (bitmask, default 7 = all) picks which regions follow CAR_PARK_SCALE; the regions only work together:
 * scaling modes 0+1 or 1 alone left almost no parked cars (tested 2026-10-30). */
extern "C" double car_park_scale_mode(int mode)
{
    static double k = -1.0; static int mask = 7;
    if (k < 0.0) {
        const char* e = std::getenv("CAR_PARK_SCALE");
        k = e ? std::atof(e) : 1.0;
        if (!(k >= 0.25 && k <= 8.0)) k = 1.0;
        e = std::getenv("CAR_PARK_MODES"); if (e && *e) mask = std::atoi(e);
        std::fprintf(stderr, "[CARPARK] parked-car spawner regions x%.2f (CAR_PARK_SCALE; 1 = game), modes mask %d (CAR_PARK_MODES)\n", k, mask);
    }
    const uint32_t cur = s_pop_cur;
    if (k == 1.0 || mode < 0 || mode > 7 || !((mask >> mode) & 1) || !cur || !car_pop_is_veh(cur)) return 1.0;
    { static int seen = 0; if (!((seen >> mode) & 1)) { seen |= 1 << mode;
        std::fprintf(stderr, "[CARPARK] spawner 0x%08X mode %d region scaled x%.2f\n", cur, mode, k); } }
    return k;
}

/* Entry of func_004F5240, the type-registry factory of the vehicle class (vtable 0x00730038, size 0x420; registry
 * entry EBOOT 0x007502F4): a vehicle created inside a spawner update marks that spawner as a vehicle spawner, so its
 * regions follow CAR_PARK_SCALE. */
extern "C" void car_pop_vehicle(void)
{
    const uint32_t cur = s_pop_cur;
    if (cur && !car_pop_is_veh(cur) && s_pop_nveh < 64) { s_pop_veh[s_pop_nveh++] = cur;
        std::fprintf(stderr, "[CARPARK] spawner 0x%08X creates vehicles -> its regions follow CAR_PARK_SCALE\n", cur); }
}

/* d^2 multiplier for the distance tests of func_00114F60 (1 = unchanged) */
extern "C" double car_v233_dscale(ppu_context* ctx)
{
    using namespace carv;
    if (!s_init) init();
    if (s_k == 1.0f || !knobs_now()) return 1.0;
    const uint32_t inst = (uint32_t)ctx->gpr[27];
    Inst* e = find(inst, false);
    bool veh = e && (e->flags & (F_TRAFFIC | F_MODEL));
    if (!e || !veh) {   /* parked cars etc.: same model pointer as a traffic vehicle */
        const uint32_t m = va(inst) ? vm_read32(inst) : 0;
        if (m && is_vmodel(m)) { if (!e) e = find(inst, true); if (e) { e->flags |= F_MODEL; e->model = m; } veh = true; }
    }
    return veh ? 1.0 / ((double)s_k * (double)s_k) : 1.0;
}

/* exit of func_00114F60: r3 = visible?, r27 = instance, r26 = view, f31 = (scaled) d^2 */
extern "C" void car_v233_vis(ppu_context* ctx)
{
    using namespace carv;
    if (!s_init) init();
    if (!s_vis && s_k == 1.0f && s_radius <= 0.0f && s_spmin <= 0.0f && s_spvr <= 0.0f) return;
    const unsigned fr = ppu_boot_frames_presented();
    if (fr != s_frame) frame_tick(fr);
    if (!s_vis) return;
    const uint32_t inst = (uint32_t)ctx->gpr[27], view = (uint32_t)ctx->gpr[26];
    if (!va(inst) || !va(view)) return;
    const int vi = view_idx(view); s_vcalls[vi]++;
    { const uint32_t cam = vm_read32(view + 0x2E0);
      if (vi == 0 && va(cam)) { s_cam[0] = rf(cam + 0x9B0); s_cam[1] = rf(cam + 0x9B4); s_cam[2] = rf(cam + 0x9B8); } }
    const bool valid = ((vm_read16(inst + 0x92) & vm_read32(view + 0x284)) & 7u) != 0;
    const int res = (int)(uint32_t)ctx->gpr[3];
    Inst* e = find(inst, true);
    if (!e) return;
    double d2 = ctx->fpr[31];
    const bool veh = (e->flags & (F_TRAFFIC | F_MODEL)) || is_vmodel(vm_read32(inst));
    if (veh && s_k != 1.0f && knobs_now()) d2 *= (double)s_k * (double)s_k;   /* undo the scale for the log */
    const float d = (valid && d2 >= 0) ? (float)std::sqrt(d2) : -1.0f;
    if (!e->last || fr - e->last > 30) {   /* new lifetime */
        const uint8_t keep = e->flags & (F_TRAFFIC | F_MODEL);
        e->flags = keep; e->first = fr; e->vis = 0; e->dfirst = d; e->dvis = 0; e->view = (uint8_t)vi;
        e->nearc = vm_read8(inst + 0x2E); e->farc = vm_read8(inst + 0x2F); e->model = vm_read32(inst);
        e->lr = (uint32_t)vm_read64(ctx->gpr[1] + 0x170);
        s_gen_new++;
    }
    e->last = fr;
    if (vi != 0 || e->vis) return;          /* first-visible events: first view only */
    if (res != 1) return;
    e->vis = fr; e->dvis = d;
    const bool newvis = fr - e->first <= 2;
    if (newvis) { s_gen_newvis++; if (d >= 0 && d < 4000.0f) s_gen_newvis_near++; }
    if (veh) {
        s_vis_n++; if (newvis) s_vis_new++;
        if (d >= 0) s_vis_hist[d >= 15000.0f ? 15 : (int)(d / 1000.0f)]++;
        const uint32_t camp = vm_read32(view + 0x2E0);
        const float cs = va(camp) ? rf(camp + 0x2A8) : 0.0f;
        const uint32_t tab = 0x0078AD80u;
        const float nearlim = (e->nearc < 7 ? rf(tab + 4u * e->nearc) : 0.0f) * cs;
        const float farlim = (e->farc < 7 ? rf(tab + 4u * (e->farc + 1u)) : 0.0f) * cs;
        std::fprintf(stderr, "[CARVIS] f%u VISIBLE %s inst 0x%08X model 0x%08X d %.0f (first query f%u d %.0f%s) cls %u/%u band %.0f..%.0f (x%.2f) lr 0x%08X cam-cole %.0f\n",
                     fr, (e->flags & F_TRAFFIC) ? "traffic" : "parked/other", inst, e->model, d, e->first, e->dfirst,
                     newvis ? " = NEW+VISIBLE" : "", e->nearc, e->farc, nearlim, farlim, cs, e->lr, dist3(s_cam, s_cole));
    } else if (newvis && d >= 0 && d < 4000.0f && !(e->flags & F_QLOG)) {
        static unsigned long nlog = 0;
        if (nlog++ < 400) std::fprintf(stderr, "[CARVIS] f%u generic new+visible inst 0x%08X model 0x%08X d %.0f cls %u/%u lr 0x%08X\n",
                                       fr, inst, e->model, d, e->nearc, e->farc, e->lr);
        e->flags |= F_QLOG;
    }
}
