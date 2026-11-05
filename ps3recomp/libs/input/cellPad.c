/*
 * ps3recomp - cellPad HLE implementation
 *
 * Reads real gamepad input from the host and translates to PS3 pad format.
 *
 * Backend selection:
 *   - Windows default: XInput (no extra dependencies)
 *   - Everywhere else / if PS3RECOMP_PAD_USE_SDL2 is defined: SDL2 GameController
 *
 * Define PS3RECOMP_PAD_USE_SDL2 to force SDL2 backend on Windows.
 */

#include <time.h>
#include "cellPad.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../runtime/platform/win32_compat.h"   /* GetTickCount64 on POSIX */
#include "../../runtime/ppu/ppu_memory.h"   /* GUEST_PTR, vm_write*: guest EA -> host */
#ifdef PS3RECOMP_SDL3
#include "pad_sdl3.h"           /* PlayStation / Steam controllers through SDL3 (pad_sdl3.c) */
#endif

/* ---------------------------------------------------------------------------
 * Backend selection
 * -----------------------------------------------------------------------*/

#if defined(PS3RECOMP_PAD_USE_SDL2)
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#elif defined(_WIN32)
  #define PAD_BACKEND_SDL2  0
  #define PAD_BACKEND_XINPUT 1
#else
  #define PAD_BACKEND_SDL2  1
  #define PAD_BACKEND_XINPUT 0
#endif

#if PAD_BACKEND_XINPUT
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <xinput.h>
  #pragma comment(lib, "xinput.lib")
#endif

#if PAD_BACKEND_SDL2
  #include <SDL2/SDL.h>
#endif

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

#define PAD_MAX_HOST_PORTS  4  /* XInput supports max 4; SDL may support more */

typedef struct {
    int  connected;
    u16  buttons;           /* CELL_PAD_CTRL_* bitmask */
    u8   analog_lx;         /* 0-255, center=128 */
    u8   analog_ly;
    u8   analog_rx;
    u8   analog_ry;
    u8   trigger_l2;        /* 0-255 */
    u8   trigger_r2;        /* 0-255 */
    /* Pressure-sensitive face buttons (0-255) */
    u8   press_right;
    u8   press_left;
    u8   press_up;
    u8   press_down;
    u8   press_triangle;
    u8   press_circle;
    u8   press_cross;
    u8   press_square;
    u8   press_l1;
    u8   press_r1;
} PadHostState;

static int           s_pad_initialized = 0;
static u32           s_max_connect = 0;
static u32           s_port_setting[CELL_PAD_MAX_PORT_NUM];
static PadHostState  s_host_state[PAD_MAX_HOST_PORTS];

/* Optional title-owned filter.  The generic runtime only exposes the completed
 * host-endian pad packet; a port can install a filter without teaching the
 * shared input backend about game-specific controls.  Called before pressure
 * words are derived so injected digital buttons receive matching pressure. */
void (*g_pad_filter)(u32 port, u16* buttons, s32 len) = NULL;
/* Per-port "a report has arrived since your last read" flag; see cellPadGetData. */
static int s_data_fresh[PAD_MAX_HOST_PORTS];
/* Press latch (2026-10-04): every button seen down by ANY host poll since the
 * guest's last delivered packet. A title that reads the pad once per frame at a
 * low frame rate (inFamous: ~4 reads/s at 4 fps) otherwise never sees a normal
 * key tap that starts and ends between two reads -- "Press Start" ignored. */
static volatile long s_latch[PAD_MAX_HOST_PORTS];
#ifdef _WIN32
  #define PAD_LATCH_OR(p, v)   InterlockedOr(&s_latch[p], (long)(v))
  #define PAD_LATCH_TAKE(p)    ((u16)InterlockedExchange(&s_latch[p], 0))
#else
  #define PAD_LATCH_OR(p, v)   __sync_fetch_and_or(&s_latch[p], (long)(v))
  #define PAD_LATCH_TAKE(p)    ((u16)__sync_lock_test_and_set(&s_latch[p], 0))
#endif

#if PAD_BACKEND_SDL2
static SDL_GameController* s_sdl_controllers[PAD_MAX_HOST_PORTS];
static int s_sdl_inited = 0;
#endif

/* ---------------------------------------------------------------------------
 * XInput backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_XINPUT

/* Deadzone for analog sticks (same as XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) */
#define PAD_STICK_DEADZONE  7849
#define PAD_TRIGGER_THRESHOLD 30

static u8 pad_xinput_stick_to_u8(short raw, short deadzone)
{
    float normalized;
    if (raw > deadzone)
        normalized = (float)(raw - deadzone) / (float)(32767 - deadzone);
    else if (raw < -deadzone)
        normalized = (float)(raw + deadzone) / (float)(32767 - deadzone);
    else
        normalized = 0.0f;

    /* Map -1.0..1.0 to 0..255 with center at 128 */
    int val = (int)(normalized * 127.0f) + 128;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* Keyboard fallback for port 0.
 *
 * The only backend here is XInput, so on a machine with no controller plugged
 * in a title gets a pad that is reported present and never presses anything --
 * it can be watched but not played. That is what The Simpsons Arcade Game hit:
 * it reached its attract loop and no input existed to start a game.
 *
 * Only fills in for port 0, only when XInput found nothing there, so a real
 * controller always wins and nothing changes for a port that has one. Keys are
 * read only while a window of THIS process is in the foreground, so typing in
 * another application does not drive the game. Set PAD_NO_KEYBOARD=1 to
 * disable it entirely.
 *
 * Arrows = d-pad, Z/X/A/S = cross/circle/square/triangle, Q/W = L1/R1,
 * 1/2 = L2/R2, Enter = START, Tab = SELECT. The left stick mirrors the d-pad
 * so a title that reads the stick instead is playable too. */
/* v219 PAD_FILE stick hold (bench driver): lx, ly, rx, ry */
static unsigned char g_pf_stick[4] = {128, 128, 128, 128};
volatile unsigned g_pad_file_held = 0;   /* v219: PAD_FILE mask being held (rsx_ui_overlay.c dialogs/menu) */
static int g_pf_stick_on = 0;

#ifdef _WIN32
static int pad_host_window_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

/* v150 KEYBOARD + MOUSE (PAD_KBM, default 1; 0 = the old key map below).
 *   W/A/S/D = left stick (walk), hold Shift = run (full deflection)
 *   mouse   = right stick (camera), left click = R1 (shoot), right click = L1 (aim)
 *   Space = Cross (jump), F = Square, E = Triangle, C = Circle, R = R2, Q = L2,
 *   Ctrl = d-pad up (Precision), middle click = R3, V = L3, Enter = START, Tab = SELECT.
 *   On the map, mouse motion / WASD pan and the mouse wheel zooms.
 *   Hold Alt to free the mouse pointer. (Esc still closes the window.)
 * The pointer is hidden and re-centred while the game window has focus; mouse motion becomes a
 * right-stick deflection that decays over ~30 ms, with a small offset past the game's stick dead zone.
 * Tuning: PAD_MOUSE_SENS (default 6), PAD_MOUSE_INVERT_Y=1, PAD_MOUSE_DZ (default 22),
 * PAD_KBM_WALK (walk deflection 0..127, default 64). Merged into port 0 when a controller is present. */
static int pad_kbm_on(void);
static int pad_envi(const char* n, int d);
/* v193: which device the player used last -- read by the renderer to show keyboard/mouse or controller button
 * prompts (rsx_d3d12_backend.c KBM GLYPHS). 1 = keyboard/mouse, 0 = controller, -1 = nothing yet. */
volatile int g_input_kbm = -1;
static void pad_note_device(int kbm)
{
    if (g_input_kbm != kbm) {
        g_input_kbm = kbm;
        static int n = 0; if ((n < 40 && n++ < 40)) { printf("[cellPad] v193 input device -> %s (button prompts follow)\n", kbm ? "keyboard/mouse" : "controller"); fflush(stdout); }
    }
}
volatile int g_pad_mouse_captured = 0;   /* read by the D3D12 window proc (WM_SETCURSOR) */
/* v153 RAW MOUSE: the D3D12 window registers for WM_INPUT and adds the device's raw counts here (no Windows
 * pointer acceleration, no counts lost between GetCursorPos and SetCursorPos, no clamping at the screen edge
 * on a fast flick). Used instead of the cursor delta once the first raw packet arrives; PAD_RAWINPUT=0 = off. */
volatile long g_pad_raw_mdx = 0, g_pad_raw_mdy = 0, g_pad_raw_wheel = 0;
volatile int  g_pad_raw_ok = 0;
/* v151 DIRECT MOUSE (PAD_MOUSE_DIRECT, default 1): v150 fed the mouse through the right-stick BYTES, so the game's
 * dead zone, response curve and the stick's full-deflection speed limit applied ("feels like a controller").
 * Now the poll thread only accumulates raw pixel deltas; once per game frame the lifted pad update
 * (func_00152EFC, right after func_001523C0 converts the right stick -- hook at lr 0x153968) calls
 * ps3_pad_mouse_inject(), which writes the stick FLOATS the camera reads (ctrl+0x10 x, +0x14 y [+ = up],
 * +0x18 / +0x1F4 magnitude) from mouse VELOCITY (pixels per second), linear, no dead zone, and allowed past 1.0
 * (PAD_MOUSE_MAX, default 4) so fast flicks turn faster than a stick can. PAD_MOUSE_SPEED = pixels/s that equal
 * a full stick (default 600; lower = faster). Controller right stick still works when the mouse is still. */
static volatile int s_pad_xinput_merged = 0;   /* v165 diag: a controller owns port 0 (keys/mouse merged) */
static volatile long s_mdx = 0, s_mdy = 0;   /* accumulated pixels since the last game frame */
/* v152: the game's stick getter (func_00152240) squares the stick (out = v*|v|), so v151's linear velocity turned
 * slow moves into almost nothing and fast ones into a lot ("has to catch up"). We now write sign(v)*sqrt(|v|),
 * which the getter squares back to v: camera speed is linear in mouse speed. And the camera adds a damped spring
 * offset (camera obj +0x250 offset, +0x260 velocity, func_00073AB8 loc_00074190) that eases the view after a turn;
 * while the mouse is moving it is cleared every frame (ps3_pad_mouse_cam_spring) so the view stops where the mouse
 * stops. PAD_MOUSE_LINEAR=0 / PAD_MOUSE_NOSPRING=0 turn either off. */
static volatile unsigned long long s_mouse_active_ms = 0;
static volatile int s_pad_kbm_map_active = 0;
static int pad_mouse_direct(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PAD_MOUSE_DIRECT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* v236 MOUSE -> LEFT STICK WHEN NOTHING READS THE RIGHT STICK (PAD_MOUSE_LSTICK, default 1; 0 = off).
 * The Sasha boss "struggle" (GrappleGame: "Use the left stick to aim at the target, press X repeatedly") aims with the
 * LEFT stick only and the camera is scripted, so the mouse did nothing and WASD (8 digital directions at full or walk
 * deflection) could not hold the reticle on the target. Detection is generic, no struggle-specific address: the game
 * reads the right stick only through func_00152240(ctrl, which=1, ..) from its two camera updates (lr 0x73FEC =
 * third-person camera, only when the stick is deflected; lr 0x856F0 = per-stick "control kind" camera). Hand patches
 * there call ps3_pad_rstick_read(). When the mouse writes a clear right-stick deflection for PAD_MOUSE_LSTICK_FRAMES
 * (6) mouse frames in a row and no camera read it, the mouse also drives the LEFT stick floats (ctrl+0x04 x, +0x08 y
 * [+ = up], +0x0C / +0x19C magnitude; velocity like mouse look, |v| <= 1, x PAD_MOUSE_LSTICK_GAIN). The first camera
 * read of the right stick, 2 s without mouse motion, or the pause menu end it. WASD / a controller stick still win.
 * Log: [mouse-lstick] on enter / leave + read counts per site and the 0x856F0 control kinds seen. */
static volatile LONG s_rs_reads = 0, s_rs_n73 = 0, s_rs_n85 = 0;
static volatile LONG s_rs_kind[8];
static volatile int s_kbm_lstick_keys = 0;      /* WASD is moving the left stick this poll (pad_poll_kbm) */
void ps3_pad_rstick_read(u32 site, u32 which, u32 kind)
{
    if (!which) return;
    InterlockedIncrement(&s_rs_reads);
    if (site == 0x73FECu) InterlockedIncrement(&s_rs_n73);
    else { InterlockedIncrement(&s_rs_n85); InterlockedIncrement(&s_rs_kind[kind < 7u ? kind : 7u]); }
}
static void pad_mouse_lstick(u32 ctrl, long dx, long dy, double dt, int captured)
{
    static int on = -1, need = 6; static float gain = 1.0f, spd = 450.0f;
    if (on < 0) { on = pad_envi("PAD_MOUSE_LSTICK", 1); need = pad_envi("PAD_MOUSE_LSTICK_FRAMES", 6); if (need < 2) need = 2;
                  const char* e = getenv("PAD_MOUSE_LSTICK_GAIN"); if (e && *e) gain = (float)atof(e); if (gain <= 0.0f) gain = 1.0f;
                  e = getenv("PAD_MOUSE_SPEED"); if (e && *e) spd = (float)atof(e); if (spd < 10.0f) spd = 10.0f;
                  printf("[mouse-lstick] v236 mouse drives the left stick when no camera reads the right stick: %s (frames %d, gain %.2f)\n",
                         on ? "ON" : "off", need, gain); fflush(stdout); }
    if (!on) return;
    static LONG s_judge = -1;          /* s_rs_reads right after the last clear mouse frame, -1 = nothing to judge */
    static int s_unread = 0, s_active = 0, s_nlog = 0;
    static float s_lx = 0.0f, s_ly = 0.0f;
    static unsigned long long s_last_move = 0;
    extern int (*volatile g_uiov_paused_fn)(void);
    const unsigned long long now = GetTickCount64();
    const int paused = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    const int moved = captured && (dx || dy);
    const LONG reads = s_rs_reads;
#define LSTICK_LOG(what) do { if (s_nlog < 60) { s_nlog++; \
        printf("[mouse-lstick] v236 %s | right-stick reads: 3rd-person cam %ld, kind cam %ld (kinds 0:%ld 1:%ld 2:%ld 3:%ld 4:%ld)\n", what, \
               (long)s_rs_n73, (long)s_rs_n85, (long)s_rs_kind[0], (long)s_rs_kind[1], (long)s_rs_kind[2], (long)s_rs_kind[3], (long)s_rs_kind[4]); \
        fflush(stdout); } } while (0)
    if (s_judge >= 0 && reads != s_judge) {            /* a camera consumed the right stick since the last mouse frame */
        if (s_active) LSTICK_LOG("off (a camera reads the right stick again)");
        s_active = 0; s_unread = 0; s_judge = -1;
    }
    if (paused || !captured) { if (s_active) LSTICK_LOG(paused ? "off (paused)" : "off (mouse released)");
                               s_active = 0; s_unread = 0; s_judge = -1; s_lx = s_ly = 0.0f; return; }
    float tx = 0.0f, ty = 0.0f;
    if (moved) {
        s_last_move = now;
        tx = (float)((double)dx / dt) / spd;
        ty = -(float)((double)dy / dt) / spd;           /* mouse up -> stick up (+) */
        if (s_judge >= 0 && !s_active && ++s_unread >= need) { s_active = 1; s_lx = s_ly = 0.0f; LSTICK_LOG("ON (mouse moves, nothing reads the right stick)"); }
        /* judge only clear deflections (the 3rd-person camera skips its read below a small magnitude) */
        s_judge = (sqrtf(tx * tx + ty * ty) >= 0.25f) ? reads : -1;
        if (s_judge < 0 && !s_active) s_unread = 0;
    } else if (s_active && now - s_last_move > 2000) {
        LSTICK_LOG("off (mouse idle 2 s)"); s_active = 0; s_unread = 0; s_judge = -1;
    }
    if (!s_active) return;
    /* light smoothing so frames without a mouse packet do not drop the stick to zero */
    s_lx = s_lx * 0.35f + tx * gain * 0.65f;
    s_ly = s_ly * 0.35f + ty * gain * 0.65f;
    float m = sqrtf(s_lx * s_lx + s_ly * s_ly);
    if (m > 1.0f) { s_lx /= m; s_ly /= m; m = 1.0f; }
    if (m < 0.002f) { s_lx = s_ly = 0.0f; return; }    /* still: leave the converted stick (controller) alone */
    if (s_kbm_lstick_keys) return;                     /* WASD wins */
    { union { u32 u; float f; } cx; cx.u = vm_read32(ctrl + 0x0C);
      if (cx.f > 0.2f) return; }                       /* a controller stick is already deflected: it wins */
    union { float f; u32 u; } ux, uy, um;
    ux.f = s_lx; uy.f = s_ly; um.f = m;
    vm_write32(ctrl + 0x04, ux.u);
    vm_write32(ctrl + 0x08, uy.u);
    vm_write32(ctrl + 0x0C, um.u);
    vm_write32(ctrl + 0x19C, um.u);
#undef LSTICK_LOG
}
void ps3_pad_mouse_inject(u32 ctrl)
{
    if (!pad_kbm_on() || !pad_mouse_direct() || !ctrl) return;
    if (s_pad_kbm_map_active) {
        InterlockedExchange(&s_mdx, 0);
        InterlockedExchange(&s_mdy, 0);
        return;                         /* map consumes the mouse as the left stick */
    }
    if (vm_read32(ctrl + 0x20) != 0) return;               /* pad port 0 only */
    static LARGE_INTEGER s_f, s_t0; LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!s_f.QuadPart) { QueryPerformanceFrequency(&s_f); s_t0 = t; return; }
    double dt = (double)(t.QuadPart - s_t0.QuadPart) / (double)s_f.QuadPart;
    s_t0 = t;
    const long dx = InterlockedExchange(&s_mdx, 0), dy = InterlockedExchange(&s_mdy, 0);
    pad_mouse_lstick(ctrl, dx, dy, dt < 0.004 ? 0.004 : (dt > 0.1 ? 0.1 : dt), g_pad_mouse_captured);   /* v236 */
    if (!g_pad_mouse_captured || (dx == 0 && dy == 0)) return;
    if (dt < 0.004) dt = 0.004;
    if (dt > 0.1) dt = 0.1;
    static float s_speed = -1.0f, s_max = 8.0f; static int s_inv = 0;
    { extern volatile int g_ui_mouse_speed; if (g_ui_mouse_speed > 0 && s_speed > 0.0f) s_speed = (float)g_ui_mouse_speed; }   /* v194 */
    if (s_speed < 0.0f) { const char* e = getenv("PAD_MOUSE_SPEED"); s_speed = e ? (float)atof(e) : 450.0f; if (s_speed < 10.0f) s_speed = 10.0f;
                          e = getenv("PAD_MOUSE_MAX"); if (e) s_max = (float)atof(e); if (s_max < 1.0f) s_max = 1.0f;
                          s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
                          printf("[cellPad] v151 direct mouse look: full stick = %.0f px/s, cap %.1f\n", s_speed, s_max); fflush(stdout); }
    float x = (float)((double)dx / dt) / s_speed;
    float y = -(float)((double)dy / dt) / s_speed;             /* mouse down -> look down (stick y + = up) */
    if (s_inv) y = -y;
    float m = sqrtf(x * x + y * y);
    if (m > s_max) { x *= s_max / m; y *= s_max / m; m = s_max; }
    s_mouse_active_ms = GetTickCount64();
    if (pad_envi("PAD_MOUSE_LINEAR", 1)) {                     /* pre-compensate the getter's v*|v| */
        x = (x >= 0 ? 1.0f : -1.0f) * sqrtf(fabsf(x));
        y = (y >= 0 ? 1.0f : -1.0f) * sqrtf(fabsf(y));
        m = sqrtf(m);
    }
    union { float f; u32 u; } cx, cy, cm;
    cx.f = x; cy.f = y; cm.f = m;
    /* v165 MOUSEDIAG (default on, rate-limited): what the stick conversion left in the floats, what the mouse
     * writes, the game's look-invert flags ([[toc-0x7DC0]-0x7FB4]+0xB0 x / +0xB1 y, read by func_00152240) and
     * whether a controller is merged into port 0 -- for the 'mouse inverted after using the gamepad' report. */
    { static int s_md = -1; static unsigned long long s_mt = 0; static int s_n = 0; static int s_inv_prev = -1;
      if (s_md < 0) s_md = pad_envi("PAD_MOUSE_DIAG", 1);
      if (s_md) {
          const u32 pool = vm_read32(0x0078BFC0u - 0x7DC0u), g = pool ? vm_read32(pool - 0x7FB4u) : 0;
          const int inv = g ? (int)((vm_read8(g + 0xB0) ? 1 : 0) | (vm_read8(g + 0xB1) ? 2 : 0)) : -1;
          union { u32 u; float f; } px, py; px.u = vm_read32(ctrl + 0x10); py.u = vm_read32(ctrl + 0x14);
          const unsigned long long now = GetTickCount64();
          if (inv != s_inv_prev) { printf("[mousediag] game look-invert flags now x=%d y=%d (was %d)\n", inv & 1, (inv >> 1) & 1, s_inv_prev); fflush(stdout); s_inv_prev = inv; }
          if (now - s_mt >= 250 && s_n < 400) { s_mt = now; s_n++;
              printf("[mousediag] dx %ld dy %ld -> write x %.3f y %.3f | stick conv x %.3f y %.3f | pad bytes rx %u ry %u | ctrl merged %d | inv x%d y%d | raw %d\n",
                     dx, dy, x, y, px.f, py.f, (unsigned)s_host_state[0].analog_rx, (unsigned)s_host_state[0].analog_ry,
                     s_pad_xinput_merged, inv & 1, (inv >> 1) & 1, g_pad_raw_ok); fflush(stdout); } } }
    vm_write32(ctrl + 0x10, cx.u);
    vm_write32(ctrl + 0x14, cy.u);
    vm_write32(ctrl + 0x18, cm.u);
    vm_write32(ctrl + 0x1F4, cm.u);
}
/* v153 RAW TURN: the real cause of the "drift / has to catch up" feel. The camera (func_00073AB8) does not turn at
 * stick * rate: it keeps a turn VELOCITY (vr29 -> cam+0x1D0) and moves it toward stick * rate by at most a fixed
 * step per frame (func_00382AE8 = clamp the change's length), i.e. turn ACCELERATION -- the view speeds up after
 * the mouse starts and keeps sliding after it stops. While the mouse is driving the camera that clamp is skipped
 * (velocity = target every frame) and the stick axis snap (pure horizontal / vertical within a few degrees of an
 * axis, loc_00075CB0 / loc_00075D18) is off, so the view turns exactly with the mouse and stops with it.
 * Controller stick unchanged. PAD_MOUSE_RAWTURN=0 = off. */
int ps3_pad_mouse_raw_turn(void)
{
    static int on = -1;
    if (on < 0) { on = pad_envi("PAD_MOUSE_RAWTURN", 1);
                  printf("[cellPad] v153 mouse raw turn (no turn acceleration / axis snap): %s\n", on ? "ON" : "off"); fflush(stdout); }
    if (!on || !g_pad_mouse_captured || !pad_kbm_on() || !pad_mouse_direct()) return 0;
    return (GetTickCount64() - s_mouse_active_ms) < 400;
}
void ps3_pad_mouse_cam_spring(u32 cam)
{
    static int on = -1;
    if (on < 0) on = pad_envi("PAD_MOUSE_NOSPRING", 1);
    if (!on || !cam || !pad_kbm_on() || !pad_mouse_direct()) return;
    if (GetTickCount64() - s_mouse_active_ms > 120) return;    /* only while the mouse is turning the camera */
    for (u32 o = 0; o < 0x30; o += 4) vm_write32(cam + 0x240 + o, 0);   /* v153: + the 0x240 spring */
}
static int pad_kbm_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PAD_KBM"); v = (e && *e == '0') ? 0 : 1;
                 printf("[cellPad] v150 keyboard+mouse controls %s (PAD_KBM)\n", v ? "ON" : "off"); fflush(stdout); }
    return v;
}
static int pad_envi(const char* n, int d) { const char* e = getenv(n); return e && *e ? atoi(e) : d; }
static u8 pad_axis(float v)   /* v in -127..127 -> 0..255, 128 = centre */
{
    if (v > 127.0f) v = 127.0f;
    if (v < -127.0f) v = -127.0f;
    int r = 128 + (int)(v >= 0 ? v + 0.5f : v - 0.5f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return (u8)r;
}
static void pad_poll_kbm(int merge)
{
    PadHostState* hs = &s_host_state[0];
    static float s_mx = 0.0f, s_my = 0.0f;
    static int s_sens = -1, s_inv = 0, s_dz = 0, s_walk = 0, s_run_default = 0;
    if (s_sens < 0) { s_sens = pad_envi("PAD_MOUSE_SENS", 6); s_inv = pad_envi("PAD_MOUSE_INVERT_Y", 0);
                      s_dz = pad_envi("PAD_MOUSE_DZ", 22); s_walk = pad_envi("PAD_KBM_WALK", 64);
                      s_run_default = pad_envi("PAD_RUN_DEFAULT", 0) != 0; }
    const int focused = pad_host_window_focused();
    const long raw_dx = InterlockedExchange(&g_pad_raw_mdx, 0), raw_dy = InterlockedExchange(&g_pad_raw_mdy, 0);
    const long raw_wheel = InterlockedExchange(&g_pad_raw_wheel, 0);
    if (!focused) {
        s_pad_kbm_map_active = 0;
        g_pad_mouse_captured = 0;
        s_mx = s_my = 0.0f;
        if (!merge) {
            hs->buttons = 0;
            hs->analog_lx = hs->analog_ly = 128;
            hs->analog_rx = hs->analog_ry = 128;
            hs->trigger_l2 = hs->trigger_r2 = 0;
            hs->connected = 1;
        }
        return;
    }
#define KD(vk) ((GetAsyncKeyState(vk) & 0x8000) != 0)
    /* v155 MENU MODE: Esc = Start (opens/closes the pause menu) and frees the mouse pointer (visible, not
     * recentred, mouse buttons not mapped) until Esc again or a left click in the game window. */
    static int s_menu_free = 0, s_esc_prev = 0, s_lmb_block = 0;
    {
        const int esc = KD(VK_ESCAPE);
        extern volatile int g_uiov_menu_open;   /* v194: Esc belongs to the settings menu while it is open */
        if (esc && !s_esc_prev && !g_uiov_menu_open) s_menu_free = !s_menu_free;
        s_esc_prev = esc;
        if (s_menu_free && KD(VK_LBUTTON)) {
            HWND fw = GetForegroundWindow(); POINT cp; RECT cr;
            if (fw && GetCursorPos(&cp) && ScreenToClient(fw, &cp) && GetClientRect(fw, &cr) &&
                cp.x >= 0 && cp.y >= 0 && cp.x < cr.right && cp.y < cr.bottom) {
                s_menu_free = 0; s_lmb_block = 1;   /* the recapturing click is not a shot */
            }
        }
        if (s_lmb_block && !KD(VK_LBUTTON)) s_lmb_block = 0;
    }
    /* v196 MENU KEYS: while the game is paused (pause menu and its pages; g_uiov_paused_fn = stubs.cpp probe)
     * W/A/S/D = d-pad, Enter = Cross, Backspace = Circle. Decided per key press: a key already held when the
     * pause started keeps its old meaning until released (Enter that opened the menu does not select Powers,
     * W held while running does not scroll the list). Esc stays Start (close the pause menu). */
    extern int (*volatile g_uiov_paused_fn)(void);
    static int s_mk_mode[5];   /* W A S D Enter: 0 up, 1 game/map meaning, 2 menu meaning */
    const int paused_now = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    /* Tab opens the world map. The pause probe takes 300 ms to notice that simulation stopped, so retain a
     * short pending window after the key press. Circle/Esc and a second Tab close it immediately; if the game
     * never pauses, the pending state expires instead of hijacking normal mouse-look. */
    static int s_map_mode = 0, s_tab_prev = 0;
    static unsigned long long s_map_pending_until = 0;
    const int tab_now = KD(VK_TAB);
    const unsigned long long input_now = GetTickCount64();
    if (tab_now && !s_tab_prev) {
        s_map_mode = !s_map_mode;
        s_map_pending_until = s_map_mode ? input_now + 1500 : 0;
    }
    s_tab_prev = tab_now;
    if (s_map_mode && (KD('C') || KD(VK_ESCAPE) || KD(VK_BACK))) s_map_mode = 0;
    if (s_map_mode && !paused_now && input_now >= s_map_pending_until) s_map_mode = 0;
    const int map_now = s_map_mode && (paused_now || input_now < s_map_pending_until);
    s_pad_kbm_map_active = map_now;
    static const int mk_vk[5] = { 'W', 'A', 'S', 'D', VK_RETURN };
    for (int i = 0; i < 5; i++) {
        if (!KD(mk_vk[i])) s_mk_mode[i] = 0;
        else if (!s_mk_mode[i]) s_mk_mode[i] = (paused_now && !map_now) ? 2 : 1;
    }
#define MK_GAME(i) (s_mk_mode[i] == 1)
#define MK_MENU(i) (s_mk_mode[i] == 2)
    u16 btns = 0;
    if (MK_MENU(0)) btns |= CELL_PAD_CTRL_UP;
    if (MK_MENU(1)) btns |= CELL_PAD_CTRL_LEFT;
    if (MK_MENU(2)) btns |= CELL_PAD_CTRL_DOWN;
    if (MK_MENU(3)) btns |= CELL_PAD_CTRL_RIGHT;
    if (MK_MENU(4)) btns |= CELL_PAD_CTRL_CROSS;
    if (paused_now && KD(VK_BACK)) btns |= CELL_PAD_CTRL_CIRCLE;
    if (KD(VK_CONTROL)) btns |= CELL_PAD_CTRL_UP;       /* Precision power/mode */
    if (paused_now && !map_now && KD(VK_UP)) btns |= CELL_PAD_CTRL_UP; /* menu navigation only */
    if (KD(VK_DOWN))    btns |= CELL_PAD_CTRL_DOWN;
    if (KD(VK_LEFT))    btns |= CELL_PAD_CTRL_LEFT;
    if (KD(VK_RIGHT))   btns |= CELL_PAD_CTRL_RIGHT;
    if (KD(VK_SPACE))   btns |= CELL_PAD_CTRL_CROSS;
    if (KD('C'))         btns |= CELL_PAD_CTRL_CIRCLE;
    if (KD('F'))        btns |= CELL_PAD_CTRL_SQUARE;
    if (KD('E'))        btns |= CELL_PAD_CTRL_TRIANGLE;
    if (!s_menu_free && KD(VK_RBUTTON)) btns |= CELL_PAD_CTRL_L1;
    if (!s_menu_free && !s_lmb_block && KD(VK_LBUTTON)) btns |= CELL_PAD_CTRL_R1;
    if (KD('Q'))        btns |= CELL_PAD_CTRL_L2;
    if (KD('R'))        btns |= CELL_PAD_CTRL_R2;
    if (KD('V'))        btns |= CELL_PAD_CTRL_L3;
    if (!s_menu_free && KD(VK_MBUTTON)) btns |= CELL_PAD_CTRL_R3;
    if (MK_GAME(4) || KD(VK_ESCAPE)) btns |= CELL_PAD_CTRL_START;   /* v155: Esc = Start (pause) */
    if (KD(VK_TAB))     btns |= CELL_PAD_CTRL_SELECT;
    /* left stick */
    float lx = 0.0f, ly = 0.0f;
    if (MK_GAME(1)) lx -= 1.0f;
    if (MK_GAME(3)) lx += 1.0f;
    if (MK_GAME(0)) ly -= 1.0f;   /* PS3: up = 0 */
    if (MK_GAME(2)) ly += 1.0f;
#undef MK_GAME
#undef MK_MENU
    const int keyboard_moving = (lx != 0.0f || ly != 0.0f);
    s_kbm_lstick_keys = keyboard_moving;   /* v236 */
    if (keyboard_moving) {
        /* When running is the default, holding Shift instead selects walking speed. */
        const int running = (KD(VK_SHIFT) != 0) != s_run_default;
        const float mag = running ? 127.0f : (float)s_walk;
        const float n = (lx != 0.0f && ly != 0.0f) ? 0.70710678f : 1.0f;
        lx *= mag * n; ly *= mag * n;
    }
    /* mouse -> right stick */
    const int alt = KD(VK_MENU);
#undef KD
    HWND fg = GetForegroundWindow();
    int have_mouse = 0;
    extern volatile int g_uiov_menu_open;   /* v194: settings menu open -> free the mouse pointer */
    if (fg && !alt && !s_menu_free && !g_uiov_menu_open) {
        RECT rc; POINT c, p;
        if (GetClientRect(fg, &rc) && rc.right > 8 && rc.bottom > 8) {
            c.x = (rc.right - rc.left) / 2; c.y = (rc.bottom - rc.top) / 2;
            ClientToScreen(fg, &c);
            if (GetCursorPos(&p)) {
                if (g_pad_mouse_captured && pad_mouse_direct()) {
                    if (map_now) {
                        const float dx = (float)(g_pad_raw_ok ? raw_dx : p.x - c.x);
                        const float dy = (float)(g_pad_raw_ok ? raw_dy : p.y - c.y);
                        s_mx = s_mx * 0.6f + dx * (float)s_sens;
                        s_my = s_my * 0.6f + (s_inv ? -dy : dy) * (float)s_sens;
                    } else if (g_pad_raw_ok) {   /* v153: raw device counts */
                        InterlockedExchangeAdd(&s_mdx, raw_dx);
                        InterlockedExchangeAdd(&s_mdy, raw_dy);
                        s_mx = s_my = 0.0f;
                    } else {
                        InterlockedExchangeAdd(&s_mdx, (long)(p.x - c.x));
                        InterlockedExchangeAdd(&s_mdy, (long)(p.y - c.y));
                        s_mx = s_my = 0.0f;
                    }
                } else if (g_pad_mouse_captured) {
                    const float dx = (float)(p.x - c.x), dy = (float)(p.y - c.y);
                    s_mx = s_mx * 0.6f + dx * (float)s_sens;
                    s_my = s_my * 0.6f + (s_inv ? -dy : dy) * (float)s_sens;
                } else { s_mx = s_my = 0.0f; }
                if (p.x != c.x || p.y != c.y) SetCursorPos(c.x, c.y);
                g_pad_mouse_captured = 1;
                have_mouse = 1;
            }
        }
    }
    if (!have_mouse) { g_pad_mouse_captured = 0; s_mx = s_my = 0.0f; }
    float rx = 0.0f, ry = 0.0f;
    if (s_mx > 0.5f || s_mx < -0.5f) rx = (s_mx > 0 ? 1.0f : -1.0f) * (float)s_dz + s_mx;
    if (s_my > 0.5f || s_my < -0.5f) ry = (s_my > 0 ? 1.0f : -1.0f) * (float)s_dz + s_my;
    const int mouse_motion = (rx != 0.0f || ry != 0.0f);
    static float s_map_zoom = 0.0f;
    static unsigned long long s_map_zoom_until = 0;
    if (map_now && raw_wheel) {
        s_map_zoom = raw_wheel > 0 ? -127.0f : 127.0f;
        s_map_zoom_until = input_now + 100;
    }
    if (!map_now || input_now >= s_map_zoom_until) s_map_zoom = 0.0f;
    if (map_now) {
        if (mouse_motion) { lx = rx; ly = ry; }
        rx = 0.0f; ry = s_map_zoom;             /* wheel -> right-stick vertical zoom */
    }
    const int map_panning = map_now && mouse_motion;
    const int moving = keyboard_moving || map_panning;
    const int looking = (!map_now && mouse_motion) || (map_now && ry != 0.0f);
    /* v193: keyboard/mouse activity -> keyboard/mouse prompts (raw mouse jitter of a few counts is ignored) */
    { const long am = (raw_dx < 0 ? -raw_dx : raw_dx) + (raw_dy < 0 ? -raw_dy : raw_dy);
      const int clicks = ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000) != 0;
      if (btns || moving || am > 3 || clicks) pad_note_device(1);
      else if (g_input_kbm < 0 && !merge) pad_note_device(1); }

    if (merge) {   /* a controller owns port 0: add the keys, take the sticks only while used */
        hs->buttons |= btns;
        if (moving)  { hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly); }
        if (looking) { hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry); }
        if (btns & CELL_PAD_CTRL_L2) hs->trigger_l2 = 255;
        if (btns & CELL_PAD_CTRL_R2) hs->trigger_r2 = 255;
    } else {
        hs->buttons = btns;
        hs->connected = 1;
        hs->analog_lx = pad_axis(lx); hs->analog_ly = pad_axis(ly);
        hs->analog_rx = pad_axis(rx); hs->analog_ry = pad_axis(ry);
        hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
        hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);
    }
    const u16 b = hs->buttons;
    hs->press_up       = (b & CELL_PAD_CTRL_UP)       ? 255 : 0;
    hs->press_down     = (b & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
    hs->press_left     = (b & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
    hs->press_right    = (b & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
    hs->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
    hs->press_circle   = (b & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
    hs->press_cross    = (b & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
    hs->press_square   = (b & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
    hs->press_l1       = (b & CELL_PAD_CTRL_L1)       ? 255 : 0;
    hs->press_r1       = (b & CELL_PAD_CTRL_R1)       ? 255 : 0;
    { static int said = 0; if (!said && (btns || moving || looking)) { said = 1;
        printf("[cellPad] v150 keyboard/mouse input active on port 0%s\n", merge ? " (merged with controller)" : ""); fflush(stdout); } }
}

static void pad_poll_keyboard(int merge)
{
    static int off = -1;
    if (off < 0) off = getenv("PAD_NO_KEYBOARD") ? 1 : 0;
    if (off) return;
    if (pad_kbm_on()) { pad_poll_kbm(merge); return; }

    PadHostState* hs = &s_host_state[0];
    if (merge && !pad_host_window_focused()) return;
    if (!pad_host_window_focused()) {
        /* Release everything on focus loss, or a key held while alt-tabbing
         * would stay down forever. */
        hs->buttons = 0;
        hs->analog_lx = hs->analog_ly = 128;
        hs->analog_rx = hs->analog_ry = 128;
        hs->connected = 1;
        return;
    }

    static const struct { int vk; u16 btn; } map[] = {
        { VK_UP,     CELL_PAD_CTRL_UP },      { VK_DOWN,  CELL_PAD_CTRL_DOWN },
        { VK_LEFT,   CELL_PAD_CTRL_LEFT },    { VK_RIGHT, CELL_PAD_CTRL_RIGHT },
        { 'Z',       CELL_PAD_CTRL_CROSS },   { 'X',      CELL_PAD_CTRL_CIRCLE },
        { 'A',       CELL_PAD_CTRL_SQUARE },  { 'S',      CELL_PAD_CTRL_TRIANGLE },
        { 'Q',       CELL_PAD_CTRL_L1 },      { 'W',      CELL_PAD_CTRL_R1 },
        { '1',       CELL_PAD_CTRL_L2 },      { '2',      CELL_PAD_CTRL_R2 },
        { VK_RETURN, CELL_PAD_CTRL_START },   { VK_TAB,   CELL_PAD_CTRL_SELECT },
    };

    u16 btns = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++)
        if (GetAsyncKeyState(map[i].vk) & 0x8000) btns |= map[i].btn;

    if (merge) {
        /* A controller (or a virtual one: Steam Input, DS4Windows...) owns port 0;
         * OR the keys in instead of ignoring them -- the user pressed Enter on
         * inFamous and nothing happened because XInput reported a device. */
        hs->buttons |= btns;
        if (btns) { static int said2 = 0; if (!said2) { said2 = 1;
            printf("[cellPad] keyboard merged into XInput port 0 (first key 0x%04X)\n", btns); fflush(stdout); } }
        return;
    }
    hs->buttons   = btns;
    hs->connected = 1;
    hs->analog_lx = (u8)((btns & CELL_PAD_CTRL_LEFT) ? 0 :
                         (btns & CELL_PAD_CTRL_RIGHT) ? 255 : 128);
    hs->analog_ly = (u8)((btns & CELL_PAD_CTRL_UP) ? 0 :
                         (btns & CELL_PAD_CTRL_DOWN) ? 255 : 128);
    hs->analog_rx = hs->analog_ry = 128;
    hs->trigger_l2 = (u8)((btns & CELL_PAD_CTRL_L2) ? 255 : 0);
    hs->trigger_r2 = (u8)((btns & CELL_PAD_CTRL_R2) ? 255 : 0);

    { static int said = 0;
      if (!said && btns) { said = 1;
          printf("[cellPad] keyboard fallback active on port 0 (no XInput device)\n");
          fflush(stdout); } }
}
#endif

/* v221B PAD_XI_REPROBE_MS (default 1000; 0 = old): XInputGetState on an EMPTY slot is a device enumeration
 * (tens to hundreds of us each -- Microsoft's guidance is to re-check empty slots only every few seconds), and the
 * 250 Hz poll thread probed all 4 slots every 4 ms: pad_thread_main used ~21 % of a P-core in the b027 bench.
 * A slot whose last XInputGetState failed is now re-probed only every PAD_XI_REPROBE_MS; in between it reports
 * "not connected" exactly as a failed probe does. Connected slots are read every poll as before. Only effect:
 * a controller plugged in mid-game is picked up up to 1 s later. ABX 0x100 = old (probe every poll). */
static unsigned long long pad_now_ms(void);
extern volatile int g_abx;
static void pad_poll_xinput(void)
{
    static int reprobe_ms = -1;
    static unsigned long long next_probe[PAD_MAX_HOST_PORTS];
    if (reprobe_ms < 0) {
        const char* e = getenv("PAD_XI_REPROBE_MS");
        reprobe_ms = (e && *e) ? atoi(e) : 1000;
        if (reprobe_ms < 0) reprobe_ms = 0;
        printf("[cellPad] v221B empty XInput slots re-probed every %d ms (PAD_XI_REPROBE_MS; 0 = every poll; ABX 0x100 = every poll)\n", reprobe_ms);
        fflush(stdout);
    }
    const int backoff = reprobe_ms > 0 && !(g_abx & 0x100);
    const unsigned long long now = backoff ? pad_now_ms() : 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        XINPUT_STATE state;
        memset(&state, 0, sizeof(state));

        if (backoff && next_probe[i] && now < next_probe[i]) {   /* v221B: empty slot, not due yet */
            s_host_state[i].connected = 0;
            continue;
        }
        DWORD result = XInputGetState((DWORD)i, &state);
        if (result != ERROR_SUCCESS) {
            s_host_state[i].connected = 0;
            next_probe[i] = backoff ? now + (unsigned long long)reprobe_ms : 0;   /* v221B */
            continue;
        }
        next_probe[i] = 0;   /* v221B: connected -> read every poll */

        s_host_state[i].connected = 1;
        XINPUT_GAMEPAD* gp = &state.Gamepad;

        /* Map XInput buttons to PS3 CELL_PAD_CTRL_* */
        u16 btns = 0;
        if (gp->wButtons & XINPUT_GAMEPAD_BACK)           btns |= CELL_PAD_CTRL_SELECT;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_THUMB)     btns |= CELL_PAD_CTRL_L3;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_THUMB)    btns |= CELL_PAD_CTRL_R3;
        if (gp->wButtons & XINPUT_GAMEPAD_START)          btns |= CELL_PAD_CTRL_START;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_UP)        btns |= CELL_PAD_CTRL_UP;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_RIGHT)     btns |= CELL_PAD_CTRL_RIGHT;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_DOWN)      btns |= CELL_PAD_CTRL_DOWN;
        if (gp->wButtons & XINPUT_GAMEPAD_DPAD_LEFT)      btns |= CELL_PAD_CTRL_LEFT;
        if (gp->bLeftTrigger > PAD_TRIGGER_THRESHOLD)     btns |= CELL_PAD_CTRL_L2;
        if (gp->bRightTrigger > PAD_TRIGGER_THRESHOLD)    btns |= CELL_PAD_CTRL_R2;
        if (gp->wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER)  btns |= CELL_PAD_CTRL_L1;
        if (gp->wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) btns |= CELL_PAD_CTRL_R1;
        if (gp->wButtons & XINPUT_GAMEPAD_Y)              btns |= CELL_PAD_CTRL_TRIANGLE;
        if (gp->wButtons & XINPUT_GAMEPAD_B)              btns |= CELL_PAD_CTRL_CIRCLE;
        if (gp->wButtons & XINPUT_GAMEPAD_A)              btns |= CELL_PAD_CTRL_CROSS;
        if (gp->wButtons & XINPUT_GAMEPAD_X)              btns |= CELL_PAD_CTRL_SQUARE;

        s_host_state[i].buttons = btns;
        /* v193: controller activity -> controller prompts (sticks past ~37 %, so drift does not flip it) */
        { const int st = (gp->sThumbLX > 12000 || gp->sThumbLX < -12000 || gp->sThumbLY > 12000 || gp->sThumbLY < -12000 ||
                          gp->sThumbRX > 12000 || gp->sThumbRX < -12000 || gp->sThumbRY > 12000 || gp->sThumbRY < -12000);
          if (btns || st) pad_note_device(0);
          else if (g_input_kbm < 0) pad_note_device(0); }

        /* Analog sticks. PS3 Y axis is inverted vs XInput (up = 0). Reflect about
         * 128 (256 - x), NOT 255 - x: the latter turns a centered stick into 127,
         * whose bits (0x7F) alias SELECT+START and can make the guest self-exit.
         * (via sagemono, PR #42) */
        s_host_state[i].analog_lx = pad_xinput_stick_to_u8(gp->sThumbLX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ly = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbLY, PAD_STICK_DEADZONE));
        s_host_state[i].analog_rx = pad_xinput_stick_to_u8(gp->sThumbRX, PAD_STICK_DEADZONE);
        s_host_state[i].analog_ry = (u8)(256 - pad_xinput_stick_to_u8(gp->sThumbRY, PAD_STICK_DEADZONE));

        /* Triggers */
        s_host_state[i].trigger_l2 = gp->bLeftTrigger;
        s_host_state[i].trigger_r2 = gp->bRightTrigger;

        /* Pressure-sensitive buttons: XInput has digital only, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    /* XInput needs no explicit init */
}

static void pad_shutdown_backend(void)
{
    /* XInput needs no explicit shutdown */
}

#endif /* PAD_BACKEND_XINPUT */

#if defined(PS3RECOMP_SDL3) && PAD_BACKEND_XINPUT
/* PlayStation / Steam controllers (pad_sdl3.c) take the host ports XInput left empty, in connection order.
 * s_sdl_slot[port] = their pad_sdl3 index, for rumble (-1 = not an SDL pad). */
static int s_sdl_slot[PAD_MAX_HOST_PORTS] = { -1, -1, -1, -1 };
static void pad_merge_sdl3(void)
{
    PadSdlPad sp[PAD_MAX_HOST_PORTS];
    const int ns = pad_sdl3_get(sp, PAD_MAX_HOST_PORTS);
    int k = 0;
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        s_sdl_slot[i] = -1;
        if (s_host_state[i].connected || k >= ns) continue;
        const PadSdlPad* p = &sp[k];
        PadHostState* h = &s_host_state[i];
        const u16 b = p->buttons;
        h->connected = 1; h->buttons = b;
        h->analog_lx = p->lx; h->analog_ly = p->ly; h->analog_rx = p->rx; h->analog_ry = p->ry;
        h->trigger_l2 = p->l2; h->trigger_r2 = p->r2;
        h->press_up = (b & CELL_PAD_CTRL_UP) ? 255 : 0;             h->press_down = (b & CELL_PAD_CTRL_DOWN) ? 255 : 0;
        h->press_left = (b & CELL_PAD_CTRL_LEFT) ? 255 : 0;         h->press_right = (b & CELL_PAD_CTRL_RIGHT) ? 255 : 0;
        h->press_triangle = (b & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0; h->press_circle = (b & CELL_PAD_CTRL_CIRCLE) ? 255 : 0;
        h->press_cross = (b & CELL_PAD_CTRL_CROSS) ? 255 : 0;       h->press_square = (b & CELL_PAD_CTRL_SQUARE) ? 255 : 0;
        h->press_l1 = (b & CELL_PAD_CTRL_L1) ? 255 : 0;             h->press_r1 = (b & CELL_PAD_CTRL_R1) ? 255 : 0;
        /* controller activity -> controller prompts (sticks past ~37 %, like the XInput path) */
        const int moved = p->lx < 80 || p->lx > 176 || p->ly < 80 || p->ly > 176 || p->rx < 80 || p->rx > 176 || p->ry < 80 || p->ry > 176;
        if (b || moved) pad_note_device(0);
        else if (g_input_kbm < 0) pad_note_device(0);
        s_sdl_slot[i] = k++;
    }
}
#endif

/* ---------------------------------------------------------------------------
 * SDL2 backend
 * -----------------------------------------------------------------------*/

#if PAD_BACKEND_SDL2

static u8 pad_sdl_axis_to_u8(int raw)
{
    /* SDL axis: -32768..32767 -> 0..255 with center at 128 */
    int val = ((raw + 32768) * 255) / 65535;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static u8 pad_sdl_trigger_to_u8(int raw)
{
    /* SDL trigger: 0..32767 -> 0..255 */
    int val = (raw * 255) / 32767;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static void pad_poll_sdl2(void)
{
    SDL_GameControllerUpdate();

    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (!s_sdl_controllers[i]) {
            /* Try to open newly connected controllers */
            if (SDL_IsGameController(i)) {
                s_sdl_controllers[i] = SDL_GameControllerOpen(i);
            }
        }

        SDL_GameController* gc = s_sdl_controllers[i];
        if (!gc || !SDL_GameControllerGetAttached(gc)) {
            s_host_state[i].connected = 0;
            if (gc) {
                SDL_GameControllerClose(gc);
                s_sdl_controllers[i] = NULL;
            }
            continue;
        }

        s_host_state[i].connected = 1;

        u16 btns = 0;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))          btns |= CELL_PAD_CTRL_SELECT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))     btns |= CELL_PAD_CTRL_L3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))    btns |= CELL_PAD_CTRL_R3;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))         btns |= CELL_PAD_CTRL_START;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))       btns |= CELL_PAD_CTRL_UP;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))    btns |= CELL_PAD_CTRL_RIGHT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))     btns |= CELL_PAD_CTRL_DOWN;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))     btns |= CELL_PAD_CTRL_LEFT;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  btns |= CELL_PAD_CTRL_L1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) btns |= CELL_PAD_CTRL_R1;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))             btns |= CELL_PAD_CTRL_TRIANGLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))             btns |= CELL_PAD_CTRL_CIRCLE;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))             btns |= CELL_PAD_CTRL_CROSS;
        if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))             btns |= CELL_PAD_CTRL_SQUARE;

        /* Triggers via axis */
        int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 3000) btns |= CELL_PAD_CTRL_L2;
        if (rt > 3000) btns |= CELL_PAD_CTRL_R2;

        s_host_state[i].buttons = btns;

        /* Analog sticks */
        s_host_state[i].analog_lx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX));
        s_host_state[i].analog_ly = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY));
        s_host_state[i].analog_rx = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX));
        s_host_state[i].analog_ry = pad_sdl_axis_to_u8(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY));

        /* Triggers */
        s_host_state[i].trigger_l2 = pad_sdl_trigger_to_u8(lt);
        s_host_state[i].trigger_r2 = pad_sdl_trigger_to_u8(rt);

        /* Pressure: SDL has digital buttons, so 0 or 255 */
        s_host_state[i].press_up       = (btns & CELL_PAD_CTRL_UP)       ? 255 : 0;
        s_host_state[i].press_down     = (btns & CELL_PAD_CTRL_DOWN)     ? 255 : 0;
        s_host_state[i].press_left     = (btns & CELL_PAD_CTRL_LEFT)     ? 255 : 0;
        s_host_state[i].press_right    = (btns & CELL_PAD_CTRL_RIGHT)    ? 255 : 0;
        s_host_state[i].press_triangle = (btns & CELL_PAD_CTRL_TRIANGLE) ? 255 : 0;
        s_host_state[i].press_circle   = (btns & CELL_PAD_CTRL_CIRCLE)   ? 255 : 0;
        s_host_state[i].press_cross    = (btns & CELL_PAD_CTRL_CROSS)    ? 255 : 0;
        s_host_state[i].press_square   = (btns & CELL_PAD_CTRL_SQUARE)   ? 255 : 0;
        s_host_state[i].press_l1       = (btns & CELL_PAD_CTRL_L1)       ? 255 : 0;
        s_host_state[i].press_r1       = (btns & CELL_PAD_CTRL_R1)       ? 255 : 0;
    }
}

static void pad_init_backend(void)
{
    if (!s_sdl_inited) {
        if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0) {
            SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
        }
        s_sdl_inited = 1;
    }
    memset(s_sdl_controllers, 0, sizeof(s_sdl_controllers));

    /* Open any controllers already connected */
    int num = SDL_NumJoysticks();
    for (int i = 0; i < num && i < PAD_MAX_HOST_PORTS; i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_controllers[i] = SDL_GameControllerOpen(i);
        }
    }
}

static void pad_shutdown_backend(void)
{
    for (int i = 0; i < PAD_MAX_HOST_PORTS; i++) {
        if (s_sdl_controllers[i]) {
            SDL_GameControllerClose(s_sdl_controllers[i]);
            s_sdl_controllers[i] = NULL;
        }
    }
}

#endif /* PAD_BACKEND_SDL2 */

/* ---------------------------------------------------------------------------
 * Poll dispatcher
 * -----------------------------------------------------------------------*/

/* Re-poll the host at most every PAD_POLL_INTERVAL_MS; serve cached state in
 * between.
 *
 * XInputGetState is a driver call, and on a slot with nothing plugged in it is
 * an expensive one: measured at 97.6 us for a sweep of the 7 host ports on a
 * machine with no controller attached. cellPadGetData ran that sweep on EVERY
 * call, and a guest that polls the pad from a spin loop -- The Simpsons Arcade
 * Game does, in its input stage -- therefore spends its whole main thread
 * inside the input driver.
 *
 * 4 ms is 250 Hz, well above the 60 Hz a title can actually observe (and above
 * a real DualShock 3's own report rate), so no game can tell the difference,
 * including one sampling for button-press edges. */
#define PAD_POLL_INTERVAL_MS 4

static unsigned long long pad_now_ms(void)
{
#ifdef _WIN32
    return (unsigned long long)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull +
           (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* v179 (FPS): when the host poll thread runs, guest calls (cellPadGetData & co., on the game's MAIN thread) only read
 * the state it keeps fresh -- the XInput/keyboard sweep cost the main thread ~8 % of its time (perf179 profile:
 * pad_poll_backend under the frame loop). PAD_GUEST_POLL=1 = old behaviour (guest calls sweep too). */
static volatile long s_pad_thread_live = 0;
static void pad_poll_backend_do(void);
static void pad_poll_backend(void)
{
    static int gp = -1;
    if (gp < 0) { const char* e = getenv("PAD_GUEST_POLL"); gp = (e && *e == '1') ? 1 : 0; }
    if (s_pad_thread_live && !gp) return;
    pad_poll_backend_do();
}
static void pad_poll_backend_do(void)
{
    /* Benign race: two threads may poll in the same tick. That costs one extra
     * sweep, never a missed or stale-forever read. */
    static unsigned long long s_last_ms = 0;
    unsigned long long now = pad_now_ms();
    if (s_last_ms && now - s_last_ms < PAD_POLL_INTERVAL_MS) return;
    s_last_ms = now;
    /* A real pad streams reports at roughly this rate whether or not anything
     * changed, and libpad buffers them; a fresh sweep is a fresh report for
     * every port. cellPadGetData hands each one out exactly once. */
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++) s_data_fresh[_i] = 1;

#ifdef _WIN32
    /* v221B PAD_COST=1: [PADCOST] every 10 s = polls/s and us per poll in the controller sweep and the keyboard/
     * mouse part (host poll thread cost; bench diagnostics). */
    static int cost = -1; static LARGE_INTEGER cf, c0; static unsigned long long c_n, c_xi, c_kb;
    if (cost < 0) { const char* e = getenv("PAD_COST"); cost = (e && *e == '1'); QueryPerformanceFrequency(&cf); QueryPerformanceCounter(&c0); }
    LARGE_INTEGER t0, t1, t2; if (cost) QueryPerformanceCounter(&t0);
#endif
#if PAD_BACKEND_XINPUT
    pad_poll_xinput();
#ifdef PS3RECOMP_SDL3
    pad_merge_sdl3();   /* PlayStation / Steam controllers into the ports XInput left empty */
#endif
#elif PAD_BACKEND_SDL2
    pad_poll_sdl2();
#endif
#ifdef _WIN32
    if (cost) QueryPerformanceCounter(&t1);
    s_pad_xinput_merged = s_host_state[0].connected;
    pad_poll_keyboard(s_host_state[0].connected);
    if (cost) {
        QueryPerformanceCounter(&t2);
        c_n++; c_xi += (unsigned long long)(t1.QuadPart - t0.QuadPart); c_kb += (unsigned long long)(t2.QuadPart - t1.QuadPart);
        const double win = (double)(t2.QuadPart - c0.QuadPart) / (double)cf.QuadPart;
        if (win >= 10.0) {
            const double us = 1e6 / (double)cf.QuadPart;
            fprintf(stderr, "[PADCOST] v221B %.0f polls/s; controller sweep %.1f us/poll, keyboard+mouse %.1f us/poll = %.2f %% of a core (abx 0x%X)\n",
                    c_n / win, c_xi * us / (double)c_n, c_kb * us / (double)c_n, 100.0 * (double)(c_xi + c_kb) / (double)cf.QuadPart / win, (unsigned)g_abx);
            c_n = c_xi = c_kb = 0; c0 = t2;
        }
    }
#endif
    for (int _i = 0; _i < PAD_MAX_HOST_PORTS; _i++)
        if (s_host_state[_i].buttons) PAD_LATCH_OR(_i, s_host_state[_i].buttons);
}

/* Host-side poller: samples the controller/keyboard every PAD_POLL_INTERVAL_MS
 * independently of how often the guest reads, feeding the press latch.
 * PAD_NO_THREAD=1 disables it (polling then happens only inside GetData). */
#ifdef _WIN32
static volatile LONG s_pad_thread_run = 0;
static HANDLE s_pad_thread = NULL;
static DWORD WINAPI pad_thread_main(LPVOID arg)
{
    (void)arg;
    InterlockedExchange(&s_pad_thread_live, 1);   /* v179 */
    while (s_pad_thread_run) {
#ifdef PS3RECOMP_SDL3
        pad_sdl3_update();   /* SDL3 gamepads: every SDL call happens on this thread */
#endif
        pad_poll_backend_do();
        Sleep(PAD_POLL_INTERVAL_MS);
    }
    return 0;
}
static void pad_thread_start(void)
{
    if (s_pad_thread || getenv("PAD_NO_THREAD")) return;
    s_pad_thread_run = 1;
    s_pad_thread = CreateThread(NULL, 0, pad_thread_main, NULL, 0, NULL);
    printf("[cellPad] host poll thread %s (every %d ms, presses latched until read)\n",
           s_pad_thread ? "started" : "FAILED", PAD_POLL_INTERVAL_MS);
    fflush(stdout);
}
static void pad_thread_stop(void)
{
    if (!s_pad_thread) return;
    s_pad_thread_run = 0;
    InterlockedExchange(&s_pad_thread_live, 0);   /* v179 */
    WaitForSingleObject(s_pad_thread, 200);
    CloseHandle(s_pad_thread);
    s_pad_thread = NULL;
}
#else
static void pad_thread_start(void) {}
static void pad_thread_stop(void) {}
#endif

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellPadInit(u32 max_connect)
{
    printf("[cellPad] Init(max_connect=%u)\n", max_connect);

    if (s_pad_initialized)
        return CELL_PAD_ERROR_ALREADY_OPENED;

    /* The bound is the 127-slot pad table, not the 7 ports: larger requests
     * are accepted and clamped. Resistance: Fall of Man asks for 8; rejected,
     * the library stayed closed, every GetInfo failed and the title never
     * polled the pad -- stuck on "Press the START button". */
    if (max_connect == 0 || max_connect > 127)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_pad_initialized = 1;
    s_max_connect = max_connect > CELL_PAD_MAX_PORT_NUM ? CELL_PAD_MAX_PORT_NUM : max_connect;
    memset(s_port_setting, 0, sizeof(s_port_setting));
    memset(s_host_state, 0, sizeof(s_host_state));

    pad_init_backend();

    /* Do an initial poll to detect connected controllers */
    pad_poll_backend();
    pad_thread_start();

    return CELL_OK;
}

s32 cellPadEnd(void)
{
    printf("[cellPad] End()\n");

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    pad_thread_stop();
    pad_shutdown_backend();

    s_pad_initialized = 0;
    s_max_connect = 0;
    return CELL_OK;
}

void cellPad_poll(void)
{
    if (s_pad_initialized) {
        pad_poll_backend();
    }
}

/* HLE args are GUEST effective addresses; guest structs are BIG-ENDIAN. The
 * vm_write helpers from ppu_memory.h translate + byte-swap. (cellPad was
 * written for host pointers and was never actually invoked until its NIDs
 * were fixed.) These used to be redeclared here with a 64-bit address
 * parameter, which conflicts with the header now that it is included. */

#ifdef _WIN32
/* v219 MEMCMD=<path> -- bench/debug memory tool (main thread, between game frames, checked every 100 ms).
 * The file holds ONE command; it is executed, its output appended to <path>.out (and stdout), and the file emptied.
 * Used to find Cole's position in guest RAM and teleport him (city bench), Cheat-Engine style:
 *   init <lo> <hi>          candidates = every 4-aligned big-endian float in [lo,hi] (|v| >= 0.5) in committed guest RAM
 *   changed | same | inc | dec   keep candidates whose value changed / stayed / grew / shrank since the last step
 *   list <n>                print the count and the first n candidates (addr value)
 *   nbr <addr> [n]          dump n (default 16) floats starting at addr
 *   find3 <x> <y> <z> <eps> every address holding 3 consecutive floats within eps of x y z (max 256)
 *   poke <addr> <v>         write a float;  poke3 <addr> <x> <y> <z>
 *   tp <x> <y> <z> <eps> <tx> <ty> <tz> [frames]   find3 + write tx ty tz to every hit, re-written for [frames] polls
 *   tpd <x> <y> <z> <eps> <dx> <dy> <dz> [frames]  same, target = found position + d (keeps the other copies consistent) */
static u32*  s_mc_addr = NULL; static float* s_mc_val = NULL; static size_t s_mc_n = 0, s_mc_cap = 0;
static u32   s_mc_hold_addr[256]; static float s_mc_hold_v[256][3]; static int s_mc_hold_n = 0, s_mc_hold_frames = 0;
static u32   s_mc_hf_addr = 0; static float s_mc_hf_v = 0; static int s_mc_hf_polls = 0;   /* trainer hunt: holdf */
static FILE* s_mc_out = NULL;
static void mc_log(const char* fmt, ...)
{
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stdout); fflush(stdout);
    if (s_mc_out) { fputs(b, s_mc_out); fflush(s_mc_out); }
}
static float mc_rd(u32 a) { u32 r; memcpy(&r, vm_base + a, 4); r = _byteswap_ulong(r); float f; memcpy(&f, &r, 4); return f; }
static void  mc_wr(u32 a, float f) { u32 r; memcpy(&r, &f, 4); vm_write32(a, r); }
/* walk committed, readable guest pages in [0x00010000, 0xC0000000) */
typedef void (*mc_range_fn)(u32 lo, u32 hi, void* ctx);
static void mc_ranges(mc_range_fn fn, void* ctx)
{
    unsigned long long a = 0x00010000ull;
    while (a < 0xC0000000ull) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
        unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress - vm_base) + mi.RegionSize;
        if (end > 0xC0000000ull) end = 0xC0000000ull;
        const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
        if (mi.State == MEM_COMMIT && (mi.Protect & ok) && !(mi.Protect & PAGE_GUARD)) fn((u32)a, (u32)(end - 1), ctx);
        if (end <= a) break;
        a = end;
    }
}
typedef struct { float lo, hi; } mc_init_ctx;
static void mc_push(u32 a, float v)
{
    if (s_mc_n == s_mc_cap) {
        size_t nc = s_mc_cap ? s_mc_cap * 2 : (1u << 20);
        if (nc > (size_t)96 << 20) return;   /* cap 96M candidates */
        u32* na = (u32*)realloc(s_mc_addr, nc * 4); float* nv = (float*)realloc(s_mc_val, nc * 4);
        if (!na || !nv) return;
        s_mc_addr = na; s_mc_val = nv; s_mc_cap = nc;
    }
    s_mc_addr[s_mc_n] = a; s_mc_val[s_mc_n] = v; s_mc_n++;
}
static void mc_init_fn(u32 lo, u32 hi, void* c)
{
    mc_init_ctx* x = (mc_init_ctx*)c;
    for (unsigned long long a = lo; a + 3 <= hi; a += 4) {
        float v = mc_rd((u32)a);
        if (v == v && v >= x->lo && v <= x->hi && fabsf(v) >= 0.5f) mc_push((u32)a, v);
    }
}
typedef struct { float x, y, z, eps; u32 hits[256]; int n; } mc_find_ctx;
static void mc_find_fn(u32 lo, u32 hi, void* c)
{
    mc_find_ctx* x = (mc_find_ctx*)c;
    for (unsigned long long a = lo; a + 11 <= hi; a += 4) {
        float v = mc_rd((u32)a);
        if (!(fabsf(v - x->x) <= x->eps)) continue;   /* NaN-safe: NaN never matches */
        if (!(fabsf(mc_rd((u32)a + 4) - x->y) <= x->eps) || !(fabsf(mc_rd((u32)a + 8) - x->z) <= x->eps)) continue;
        if (x->n < 256) x->hits[x->n++] = (u32)a;
    }
}
/* Research: "joints <radius>" lists blocks of bone matrices (orthonormal 3x3 + translation within
 * <radius> cm of Cole's position) -- 4x4 with the translation in floats 12..14 (row-vector) or 3,7,11 (column),
 * and 3x4 rows (translation in 3,7,11). Consecutive hits at a fixed stride = one skeleton's pose buffer. */
static volatile u32 s_mc_jflat_addr; static volatile int s_mc_jflat_n; static volatile float s_mc_jflat_z;
static DWORD WINAPI mc_jflat_thread(LPVOID unused)
{   /* research: overwrite continuously so the write wins whatever the producer/consumer timing is */
    (void)unused;
    for (;;) {
        const int n = s_mc_jflat_n; const u32 a = s_mc_jflat_addr; const float z = s_mc_jflat_z;
        if (n <= 0) { Sleep(5); continue; }
        for (int jf = 0; jf < n; jf++) mc_wr(a + 64u * (u32)jf + 56u, z);
    }
    return 0;
}
typedef struct { float px, py, pz, r; u32 hits[2048]; u8 kind[2048]; int n; } mc_joint_ctx;
static int mc_unit3(float a, float b, float c) { const float l = a * a + b * b + c * c; return l > 0.81f && l < 1.21f; }
static void mc_joint_fn(u32 lo, u32 hi, void* cv)
{
    mc_joint_ctx* x = (mc_joint_ctx*)cv;
    for (unsigned long long a = (lo + 15ull) & ~15ull; a + 64 <= hi; a += 16) {
        float m[16];
        const float t0 = mc_rd((u32)a + 48), t1 = mc_rd((u32)a + 52), t2 = mc_rd((u32)a + 56);
        const float c0 = mc_rd((u32)a + 12), c1 = mc_rd((u32)a + 28), c2 = mc_rd((u32)a + 44);
        int kind = 0;
        if (fabsf(t0 - x->px) <= x->r && fabsf(t1 - x->py) <= x->r && fabsf(t2 - x->pz) <= x->r) kind = 1;
        else if (fabsf(c0 - x->px) <= x->r && fabsf(c1 - x->py) <= x->r && fabsf(c2 - x->pz) <= x->r) kind = 2;
        if (!kind) continue;
        for (int i = 0; i < 12; i++) m[i] = mc_rd((u32)a + 4 * i);
        if (!mc_unit3(m[0], m[1], m[2]) || !mc_unit3(m[4], m[5], m[6]) || !mc_unit3(m[8], m[9], m[10])) continue;
        if (fabsf(m[0] * m[4] + m[1] * m[5] + m[2] * m[6]) > 0.1f) continue;
        if (x->n < 2048) { x->hits[x->n] = (u32)a; x->kind[x->n] = (u8)kind; x->n++; }
    }
}
static int mc_still_readable(u32 a)
{
    MEMORY_BASIC_INFORMATION mi;
    return VirtualQuery(vm_base + a, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

/* v219 MEMCMD goto autopilot: "goto <tx> <ty> <tol> <timeout_s> [zmax]" steers Cole's left stick toward a world point,
 * closed-loop on the player position global (default 0x00915430 = x y z, cm, Z up; "posaddr <a>" to change).
 * The stick is camera-relative, so the camera yaw is estimated online from the measured velocity vs the stick
 * direction (starts at 0 = camera facing +y/north, the hideout spawn view). Stalled > 1 s -> CROSS (jump/climb),
 * every 3rd stall CIRCLE (let go of a ledge). At the target but above zmax (roof, pillar): CIRCLE until below.
 * "stop" ends it. Never touches the right stick (camera). */
static u32   s_gt_pos = 0x00915430u;
static int   s_gt_on = 0; static float s_gt_tx, s_gt_ty, s_gt_tol, s_gt_zmax = 1e9f; static unsigned long long s_gt_end = 0;
static float s_gt_theta = 0.0f, s_gt_phis = 0.0f; static float s_gt_px = 0, s_gt_py = 0;
static unsigned long long s_gt_ts = 0, s_gt_stall = 0, s_gt_log = 0, s_gt_btn_until = 0, s_gt_steady = 0;
static int   s_gt_nstall = 0; static unsigned s_mc_btn = 0;
static float mc_wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }
static void mc_goto_tick(void)
{
    if (!s_gt_on) return;
    const unsigned long long now = GetTickCount64();
    const float x = mc_rd(s_gt_pos), y = mc_rd(s_gt_pos + 4), z = mc_rd(s_gt_pos + 8);
    const float dx = s_gt_tx - x, dy = s_gt_ty - y, dist = sqrtf(dx * dx + dy * dy);
    s_mc_btn = now < s_gt_btn_until ? s_mc_btn : 0;
    if (dist <= s_gt_tol && z > s_gt_zmax && now < s_gt_end && x == x) {   /* above the target (roof / pillar): let go */
        g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 1;
        if (now >= s_gt_btn_until + 600) { s_mc_btn = 0x2000u; s_gt_btn_until = now + 200; }
        if (now - s_gt_log >= 1000) { s_gt_log = now; mc_log("[goto] over target at z %.0f > %.0f -> CIRCLE\n", z, s_gt_zmax); }
        return;
    }
    if (dist <= s_gt_tol || now >= s_gt_end || !(x == x)) {
        s_gt_on = 0; s_mc_btn = 0;
        g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 0;   /* release to the real pad (was stuck at centre) */
        mc_log("[goto] %s at %.0f %.0f %.0f (dist %.0f, theta %.0f deg)\n", dist <= s_gt_tol ? "ARRIVED" : "TIMEOUT", x, y, z, dist, s_gt_theta * 57.2958f);
        return;
    }
    if (!s_gt_ts) { s_gt_ts = now; s_gt_px = x; s_gt_py = y; s_gt_stall = now; s_gt_steady = now; }
    if (now - s_gt_ts >= 250) {
        const float dt = (float)(now - s_gt_ts) / 1000.0f, vx = (x - s_gt_px) / dt, vy = (y - s_gt_py) / dt;
        const float sp = sqrtf(vx * vx + vy * vy);
        if (sp > 150.0f && now - s_gt_steady >= 250 && now >= s_gt_btn_until) {     /* learn the camera yaw */
            const float th = mc_wrap(atan2f(vy, vx) - s_gt_phis);
            s_gt_theta = mc_wrap(s_gt_theta + 0.5f * mc_wrap(th - s_gt_theta));
        }
        if (sp > 60.0f) s_gt_stall = now;
        else if (now - s_gt_stall > 1000) {
            s_gt_nstall++; s_gt_stall = now;
            s_mc_btn = (s_gt_nstall % 3 == 0) ? 0x2000u : 0x4000u; s_gt_btn_until = now + 200;
            mc_log("[goto] stalled at %.0f %.0f %.0f -> %s\n", x, y, z, s_mc_btn == 0x2000u ? "CIRCLE" : "CROSS");
        }
        s_gt_ts = now; s_gt_px = x; s_gt_py = y;
    }
    const float phis = mc_wrap(atan2f(dy, dx) - s_gt_theta);
    if (fabsf(mc_wrap(phis - s_gt_phis)) > 0.35f) s_gt_steady = now;   /* big steer change: wait before learning */
    s_gt_phis = phis;
    int lx = 128 + (int)(127.0f * cosf(phis)), ly = 128 - (int)(127.0f * sinf(phis));
    g_pf_stick[0] = (unsigned char)(lx < 0 ? 0 : lx > 255 ? 255 : lx);
    g_pf_stick[1] = (unsigned char)(ly < 0 ? 0 : ly > 255 ? 255 : ly);
    g_pf_stick[2] = g_pf_stick[3] = 128; g_pf_stick_on = 1;
    if (now - s_gt_log >= 1000) { s_gt_log = now;
        mc_log("[goto] pos %.0f %.0f %.0f dist %.0f theta %.0f stick %d %d\n", x, y, z, dist, s_gt_theta * 57.2958f, lx, ly); }
}
static void mc_exec(char* line)
{
    char cmd[16] = {0}; double p[8] = {0}; unsigned addr = 0;
    int n = sscanf(line, "%15s", cmd);
    if (n < 1) return;
    mc_log("[memcmd] > %s", line); if (!strchr(line, '\n')) mc_log("\n");
    if (!strcmp(cmd, "init")) {
        mc_init_ctx c; c.lo = -1e9f; c.hi = 1e9f;
        sscanf(line, "%*s %lf %lf", &p[0], &p[1]); c.lo = (float)p[0]; c.hi = (float)p[1];
        s_mc_n = 0; mc_ranges(mc_init_fn, &c);
        mc_log("[memcmd] init: %zu candidates\n", s_mc_n);
    } else if (!strcmp(cmd, "changed") || !strcmp(cmd, "same") || !strcmp(cmd, "inc") || !strcmp(cmd, "dec")) {
        const int m = cmd[0] == 'c' ? 0 : cmd[0] == 's' ? 1 : cmd[0] == 'i' ? 2 : 3;
        size_t k = 0;
        for (size_t i = 0; i < s_mc_n; i++) {
            const u32 a = s_mc_addr[i];
            if (((i & 4095) == 0) && !mc_still_readable(a)) continue;
            const float v = mc_rd(a), o = s_mc_val[i];
            const int keep = m == 0 ? (v != o) : m == 1 ? (v == o) : m == 2 ? (v > o) : (v < o);
            if (keep && v == v) { s_mc_addr[k] = a; s_mc_val[k] = v; k++; }
        }
        s_mc_n = k; mc_log("[memcmd] %s: %zu candidates\n", cmd, s_mc_n);
    } else if (!strcmp(cmd, "list")) {
        int cnt = 32; sscanf(line, "%*s %d", &cnt);
        mc_log("[memcmd] %zu candidates\n", s_mc_n);
        for (size_t i = 0; i < s_mc_n && (int)i < cnt; i++) mc_log("[memcmd]   0x%08X %.4f\n", s_mc_addr[i], mc_rd(s_mc_addr[i]));
    } else if (!strcmp(cmd, "nbr")) {
        int cnt = 16; sscanf(line, "%*s %x %d", (int*)&addr, &cnt);
        addr &= ~3u;
        for (int i = 0; i < cnt; i++) mc_log("[memcmd]   0x%08X +%03X %.4f (0x%08X)\n", addr + 4 * i, 4 * i, mc_rd(addr + 4 * i), vm_read32(addr + 4 * i));
    } else if (!strcmp(cmd, "poke")) {
        sscanf(line, "%*s %x %lf", (int*)&addr, &p[0]); mc_wr(addr, (float)p[0]);
        mc_log("[memcmd] poke 0x%08X = %.4f\n", addr, mc_rd(addr));
    } else if (!strcmp(cmd, "holdf")) {   /* holdf <hexEA> <f32> <polls>: write one float now and on every pad poll (= every game frame)
                                            * for <polls> more frames; "holdf 0 0 0" stops. Trainer velocity test (+0xE20..) */
        int n = 0; sscanf(line, "%*s %x %lf %d", (int*)&addr, &p[0], &n);
        s_mc_hf_addr = addr; s_mc_hf_v = (float)p[0]; s_mc_hf_polls = addr ? n : 0;
        if (addr) mc_wr(addr, s_mc_hf_v);
        mc_log("[memcmd] holdf 0x%08X = %.4f for %d frames\n", addr, addr ? mc_rd(addr) : 0.0f, s_mc_hf_polls);
    } else if (!strcmp(cmd, "poke3")) {
        sscanf(line, "%*s %x %lf %lf %lf", (int*)&addr, &p[0], &p[1], &p[2]);
        for (int k = 0; k < 3; k++) mc_wr(addr + 4 * k, (float)p[k]);
        mc_log("[memcmd] poke3 0x%08X = %.3f %.3f %.3f\n", addr, mc_rd(addr), mc_rd(addr + 4), mc_rd(addr + 8));
    } else if (!strcmp(cmd, "find3") || !strcmp(cmd, "tp") || !strcmp(cmd, "tpd")) {
        int frames = 1;
        int got = sscanf(line, "%*s %lf %lf %lf %lf %lf %lf %lf %d", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &frames);
        static mc_find_ctx c; memset(&c, 0, sizeof c);
        c.x = (float)p[0]; c.y = (float)p[1]; c.z = (float)p[2]; c.eps = got >= 4 ? (float)p[3] : 0.01f;
        mc_ranges(mc_find_fn, &c);
        mc_log("[memcmd] find3 %.3f %.3f %.3f eps %.3f: %d hits\n", c.x, c.y, c.z, c.eps, c.n);
        s_mc_hold_n = 0;
        for (int i = 0; i < c.n; i++) {
            const u32 a = c.hits[i];
            float v[3] = { mc_rd(a), mc_rd(a + 4), mc_rd(a + 8) };
            mc_log("[memcmd]   0x%08X %.3f %.3f %.3f\n", a, v[0], v[1], v[2]);
            if (cmd[0] == 't' && got >= 7) {
                float t[3];
                for (int k = 0; k < 3; k++) t[k] = cmd[2] == 'd' ? v[k] + (float)p[4 + k] : (float)p[4 + k];
                for (int k = 0; k < 3; k++) mc_wr(a + 4 * k, t[k]);
                s_mc_hold_addr[s_mc_hold_n] = a; memcpy(s_mc_hold_v[s_mc_hold_n], t, sizeof t); s_mc_hold_n++;
            }
        }
        s_mc_hold_frames = cmd[0] == 't' ? (frames > 0 ? frames : 1) - 1 : 0;
        if (cmd[0] == 't') mc_log("[memcmd] teleported %d copies (held %d more polls)\n", s_mc_hold_n, s_mc_hold_frames);
    } else if (!strcmp(cmd, "joints")) {
        static mc_joint_ctx c; memset(&c, 0, sizeof c);
        double r = 250; sscanf(line, "%*s %lf", &r);
        c.px = mc_rd(0x00915430u); c.py = mc_rd(0x00915434u); c.pz = mc_rd(0x00915438u); c.r = (float)r;
        mc_ranges(mc_joint_fn, &c);
        mc_log("[memcmd] joints near %.1f %.1f %.1f r %.0f: %d hits\n", c.px, c.py, c.pz, c.r, c.n);
        for (int i = 0; i < c.n; i++) {
            const u32 a = c.hits[i]; const int k = c.kind[i];
            const float tx = k == 1 ? mc_rd(a + 48) : mc_rd(a + 12), ty = k == 1 ? mc_rd(a + 52) : mc_rd(a + 28), tz = k == 1 ? mc_rd(a + 56) : mc_rd(a + 44);
            mc_log("[memcmd]   0x%08X %s d=(%.1f %.1f %.1f) r0=(%.3f %.3f %.3f)\n", a, k == 1 ? "rowT" : "colT",
                   tx - c.px, ty - c.py, tz - c.pz, mc_rd(a), mc_rd(a + 4), mc_rd(a + 8));
        }
    } else if (!strcmp(cmd, "jflat")) {   /* jflat <addr> <count> <z>: every poll, force each 64-byte matrix's z */
        double zv = 0; s_mc_jflat_n = 0;
        { unsigned ja = 0; int jn = 0; sscanf(line, "%*s %x %d %lf", &ja, &jn, &zv); s_mc_jflat_n = 0; s_mc_jflat_addr = ja; s_mc_jflat_z = (float)zv; s_mc_jflat_n = jn; }
        { static int started = 0; if (!started) { started = 1; CreateThread(NULL, 0, mc_jflat_thread, NULL, 0, NULL); } }
        mc_log("[memcmd] jflat 0x%08X x%d z=%.1f\n", s_mc_jflat_addr, s_mc_jflat_n, s_mc_jflat_z);
    } else if (!strcmp(cmd, "dump")) {   /* dump <addr> <bytes> <file>: raw guest bytes (big-endian) to a file */
        unsigned n = 0; char fn[400] = {0};
        sscanf(line, "%*s %x %x %399s", (int*)&addr, &n, fn);
        FILE* f = fn[0] ? fopen(fn, "wb") : NULL;
        if (f && n && n <= (64u << 20)) { fwrite(vm_base + addr, 1, n, f); fclose(f); mc_log("[memcmd] dump 0x%08X +0x%X -> %s\n", addr, n, fn); }
        else { if (f) fclose(f); mc_log("[memcmd] dump failed\n"); }
    } else if (!strcmp(cmd, "dumpall")) {   /* dumpall <prefix>: every committed guest range except RSX VRAM ->
                                              * <prefix>.idx (start size per line) + <prefix>.bin (concatenated) */
        char pre[400] = {0};
        sscanf(line, "%*s %399s", pre);
        if (!pre[0]) { mc_log("[memcmd] dumpall needs a prefix\n"); return; }
        char fn[420]; snprintf(fn, sizeof fn, "%s.bin", pre);
        FILE* bin = fopen(fn, "wb");
        snprintf(fn, sizeof fn, "%s.idx", pre);
        FILE* idx = fopen(fn, "w");
        unsigned long long total = 0; int nr = 0;
        if (bin && idx) {
            unsigned long long a = 0x00010000ull;
            while (a < 0xF0000000ull) {
                MEMORY_BASIC_INFORMATION mi;
                if (!VirtualQuery(vm_base + a, &mi, sizeof mi)) break;
                unsigned long long end = (unsigned long long)((uint8_t*)mi.BaseAddress - vm_base) + mi.RegionSize;
                if (end > 0xF0000000ull) end = 0xF0000000ull;
                const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE;
                const int vram = a >= 0x10000000ull && a < 0x20000000ull;
                if (mi.State == MEM_COMMIT && (mi.Protect & ok) && !(mi.Protect & PAGE_GUARD) && !vram) {
                    fwrite(vm_base + a, 1, (size_t)(end - a), bin);
                    fprintf(idx, "%08llX %llX\n", a, end - a);
                    total += end - a; nr++;
                }
                if (end <= a) break;
                a = end;
            }
        }
        if (bin) fclose(bin);
        if (idx) fclose(idx);
        mc_log("[memcmd] dumpall %s: %d ranges, %llu MB\n", pre, nr, total >> 20);
    } else if (!strcmp(cmd, "wwatch")) {   /* wwatch <hexEA> [hexlen]: aim the PPU_WWATCH store watch live (needs PPU_WWATCH
                                             * set at launch); hits go to stderr as [ww] with the writing guest function */
        extern volatile uint32_t g_ww_live, g_ww_live_len; extern volatile long g_ww_live_gen; extern uint32_t g_ww_lo, g_ww_hi;
        unsigned len = 0x20; sscanf(line, "%*s %x %x", (int*)&addr, &len);
        if (len < 0x20) len = 0x20;
        g_ww_live_len = len; g_ww_live = addr; g_ww_lo = addr & ~15u; g_ww_hi = (addr & ~15u) + len; g_ww_live_gen++;
        mc_log("[memcmd] wwatch 0x%08X +0x%X%s\n", addr, len, getenv("PPU_WWATCH") ? "" : "  (PPU_WWATCH was not set at launch: lifted stores bypass the watch!)");
    } else if (!strcmp(cmd, "goto")) {
        double t[5] = {0, 0, 150, 60, 1e9};
        sscanf(line, "%*s %lf %lf %lf %lf %lf", &t[0], &t[1], &t[2], &t[3], &t[4]);
        s_gt_zmax = (float)t[4];
        s_gt_tx = (float)t[0]; s_gt_ty = (float)t[1]; s_gt_tol = (float)t[2];
        s_gt_end = GetTickCount64() + (unsigned long long)(t[3] * 1000.0); s_gt_ts = 0; s_gt_nstall = 0; s_gt_on = 1;
        mc_log("[goto] -> %.0f %.0f (tol %.0f, %.0f s) from %.0f %.0f %.0f\n", s_gt_tx, s_gt_ty, s_gt_tol, t[3], mc_rd(s_gt_pos), mc_rd(s_gt_pos + 4), mc_rd(s_gt_pos + 8));
    } else if (!strcmp(cmd, "theta")) {
        double t = 0; sscanf(line, "%*s %lf", &t); s_gt_theta = (float)(t / 57.2958); mc_log("[goto] theta = %.0f deg\n", t);
    } else if (!strcmp(cmd, "stop")) {
        s_gt_on = 0; s_mc_btn = 0; g_pf_stick[0] = g_pf_stick[1] = 128; g_pf_stick_on = 0; mc_log("[goto] stopped, sticks released\n");
    } else if (!strcmp(cmd, "posaddr")) {
        sscanf(line, "%*s %x", &s_gt_pos); mc_log("[goto] position at 0x%08X\n", s_gt_pos);
    } else if (!strcmp(cmd, "pos")) {
        mc_log("[memcmd] pos %.1f %.1f %.1f\n", mc_rd(s_gt_pos), mc_rd(s_gt_pos + 4), mc_rd(s_gt_pos + 8));
    } else mc_log("[memcmd] unknown command\n");
}
static void pad_memcmd_poll(void)
{
    static int on = -1; static char path[512]; static unsigned long long next = 0;
    if (on < 0) { const char* e = getenv("MEMCMD"); on = (e && *e) ? 1 : 0;
                  if (on) { snprintf(path, sizeof path, "%s", e); char op[520]; snprintf(op, sizeof op, "%s.out", e); s_mc_out = fopen(op, "ab");
                            printf("[memcmd] v219 memory tool on (%s)\n", path); fflush(stdout); } }
    if (!on) return;
    if (s_mc_hf_polls > 0) { s_mc_hf_polls--; mc_wr(s_mc_hf_addr, s_mc_hf_v); }
    if (s_mc_hold_frames > 0) { s_mc_hold_frames--;
        for (int i = 0; i < s_mc_hold_n; i++) for (int k = 0; k < 3; k++) mc_wr(s_mc_hold_addr[i] + 4 * k, s_mc_hold_v[i][k]); }
    mc_goto_tick();
    const unsigned long long now = GetTickCount64();
    if (now < next) return;
    next = now + 100;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[256]; buf[0] = 0;
    const int got = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if (!got || !buf[0] || buf[0] == '\n' || buf[0] == '\r') return;
    { FILE* w = fopen(path, "wb"); if (w) fclose(w); }
    for (char* q = buf; *q; q++) if (*q == '\r') *q = ' ';
    mc_exec(buf);
}
#else
static void pad_memcmd_poll(void) {}
static unsigned s_mc_btn = 0;
#endif

/* rsx_ui_trainer.h: set by the title (e.g. the inFamous trainer), NULL = nothing. */
void (*volatile g_pad_title_tick_fn)(void) = 0;
/* rsx_ui_trainer.h: port 0 as the game last received it (left stick x,y 0..255; digital DIGITAL2<<8 | DIGITAL1). */
volatile unsigned char g_pad_title_lstick[2] = {128, 128};
volatile unsigned g_pad_title_digital = 0;

s32 cellPadGetData(u32 port_no, CellPadData* data_guest)
{
    { static int _once = 0;
      if (!_once++) printf("[cellPad] GetData polling begins (port %u)\n", port_no); }
    { static unsigned long long _hb_n = 0, _hb_t = 0; _hb_n++;
      unsigned long long _now = pad_now_ms();
      if (!_hb_t) _hb_t = _now;
      if (_now - _hb_t >= 10000) { _hb_t = _now;
          printf("[cellPad] heartbeat: %llu GetData calls so far, port0 buttons=0x%04X connected=%d\n",
                 _hb_n, (unsigned)s_host_state[0].buttons, s_host_state[0].connected); fflush(stdout); } }
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= s_max_connect || !data_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadData _d; CellPadData* data = &_d;
    memset(data, 0, sizeof(CellPadData));

    /* Poll fresh state */
    pad_poll_backend();

    /* YDKJ_INJECT_PAD: the game's wait loop polls the pad (cellPadSetActDirect /
     * GetData) -- if it's parked on a "press button" prompt after loading, no
     * host input means it waits forever. Inject a real button PULSE (CROSS+START+
     * CIRCLE) so it advances. Legit input simulation, not forged pixels. */
    /* NOTE: cache getenv() -- this runs on every poll from multiple threads, and the
     * uncached version crashed deterministically (varying fault address = race). Every
     * other probe in this tree uses the cached `static int x=-1` pattern; match it. */
    static int  s_inj   = -1;
    static u16  s_btn   = 0x0008u;   /* default START */
    static long s_delay = 3000;
    if (s_inj < 0) {
        s_inj = getenv("YDKJ_INJECT_PAD") ? 1 : 0;
        const char* be = getenv("YDKJ_PAD_BTN");   if (be) s_btn   = (u16)strtoul(be,0,0);
        const char* de = getenv("YDKJ_PAD_DELAY"); if (de) s_delay = strtol(de,0,0);
    }
    /* Gate on WALL-CLOCK, not poll count: the game polls the pad hundreds of thousands
     * of times inside an early wait loop, so any count-based delay fires during init
     * (crash: guest ctr=0x0005F8A0 r3=0 -- button handler on an unconstructed object).
     * YDKJ_PAD_DELAY is now SECONDS to wait before the first injected press. */
    if (s_inj && port_no < PAD_MAX_HOST_PORTS) {
        static long _pc = 0; _pc++;
        /* Cheap counter gate FIRST: clock() is a slow call and this poll loop is hammered
         * by the guest from multiple threads; sampling it every poll perturbs timing. Only
         * consult the clock once every 256 polls, and never after arming. */
        static int     s_armed = 0;
        static clock_t s_t0    = 0;
        if (!s_armed) {
            if ((_pc & 0xFF) == 0) {
                if (!s_t0) s_t0 = clock();
                else if ((double)(clock() - s_t0) / (double)CLOCKS_PER_SEC >= (double)s_delay) s_armed = 1;
            }
            goto skip_inject;
        }
        u16 btn = s_btn; long delay = 0;
        /* Delay past init, then a single clean pulse every ~180 calls (press 15, release 165). */
        if (_pc > delay) {
            long ph = (_pc - delay) % 180;
            if (ph < 15) s_host_state[port_no].buttons |= btn;
            else         s_host_state[port_no].buttons &= ~btn;
            s_host_state[port_no].connected = 1;
        }
    }
skip_inject: ;

    if (port_no >= PAD_MAX_HOST_PORTS || (!s_host_state[port_no].connected && port_no != 0)) {
        goto emit;   /* data stays zeroed -> len=0 */
    }
    /* port 0 is always a valid (virtual) pad; s_host_state[0] is neutral if no
     * physical device, which yields no-buttons-pressed data below. */

    {
    PadHostState* hs = &s_host_state[port_no];
    /* An in-game system dialog (libs/video/rsx_ui_overlay.c) owns the pad while it is up, as on
     * hardware: the title sees a neutral pad, and the press that answered the dialog (held, or
     * latched) never reaches it. */
    int pad_gate_block = 0;
    { extern int uiov_pad_gate(void);
      static PadHostState s_neutral = { 1, 0, 128, 128, 128, 128 };
      int gate = uiov_pad_gate();
      if (gate) (void)PAD_LATCH_TAKE(port_no);
      if (gate == 1) { s_neutral.connected = hs->connected; hs = &s_neutral; pad_gate_block = 1; } }
    u32 setting = s_port_setting[port_no];

    /* Determine data length based on port settings */
    s32 len = CELL_PAD_LEN_CHANGE_DEFAULT;
    if (setting & CELL_PAD_SETTING_SENSOR_ON)
        len = CELL_PAD_LEN_CHANGE_SENSOR_ON;
    else if (setting & CELL_PAD_SETTING_PRESS_ON)
        len = CELL_PAD_LEN_CHANGE_PRESS_ON;

    data->len = len;
    /* button[0] is reserved and 0 on hardware; button[1] carries the pad mode
     * in its high nibble (0x7) and len/2 in its low one. GH3 drops any packet
     * whose button[0] is non-zero -- with len (8) there, every press was
     * received and thrown away, and "Press any button" never advanced. */
    data->button[0] = 0;
    data->button[1] = (u16)(0x70 | ((len / 2) & 0xF));

    /* Digital buttons. hs->buttons packs both halves into one 16-bit mask
     * (SELECT=bit0..LEFT=bit7 = DIGITAL1; L2=bit8..SQUARE=bit15 = DIGITAL2).
     * Split correctly — writing the whole value into DIGITAL1 with DIGITAL2=0
     * left every face button (cross/circle/triangle/square, L1/L2/R1/R2) dead.
     * (via sagemono, PR #42) */
    /* Held buttons plus anything pressed since the last delivered packet. The
     * latch is only consumed when this packet is actually delivered (len > 0,
     * see emit), so a press is never eaten by a len = 0 read. */
    u16 eff_btns = (u16)(hs->buttons | (pad_gate_block ? 0 : (u16)s_latch[port_no]));
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] = (u16)(eff_btns & 0xFF);
    data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] = (u16)((eff_btns >> 8) & 0xFF);
    { static int said = 0;
      if (eff_btns && !hs->buttons && said < 5) { said++;
          printf("[cellPad] delivered a latched press 0x%04X (released before this read)\n", eff_btns);
          fflush(stdout); } }

    /* PAD_AUTOPRESS: headless bring-up input -- pulse CROSS then START every few
     * seconds of polling so boot screens that wait for input advance without a
     * human at the pad. Env-gated test scaffolding, off by default. */
    { static int s_ap = -1; static unsigned s_apn = 0;
      if (s_ap < 0) s_ap = getenv("PAD_AUTOPRESS") ? 1 : 0;
      if (s_ap && port_no == 0) {
          unsigned ph = s_apn++ % 240;
          if (ph < 12)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= 0x40;   /* CROSS */
          else if (ph >= 120 && ph < 132)
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= 0x08;   /* START */
      } }

    /* PAD_SCRIPT="<sec>:<mask>,<sec>:<mask>,..." -- press a NAMED button at a
     * given wall-clock second, each held ~250 ms. PAD_AUTOPRESS only pulses
     * CROSS and START, which is enough to clear a "press start" screen but not
     * to navigate a menu: You Don't Know Jack needs DOWN to move off its name
     * field before CROSS means anything. Masks are CELL_PAD_CTRL_* packed as
     * (DIGITAL2 << 8) | DIGITAL1: START 0x0008, UP 0x0010, DOWN 0x0040,
     * LEFT 0x0080, RIGHT 0x0020, CROSS 0x4000, CIRCLE 0x2000, TRIANGLE 0x1000.
     * Legit input simulation on the same footing as PAD_AUTOPRESS/PAD_STICK. */
    { static int s_sc = -1;
      static struct { double t; unsigned mask; } ev[32]; static int n_ev = 0;
      static unsigned long long t0 = 0;   /* not ULONGLONG: this builds on POSIX too */
      if (s_sc < 0) {
          s_sc = 0;
          const char* e = getenv("PAD_SCRIPT");
          if (e && *e) {
              s_sc = 1; t0 = GetTickCount64();
              const char* p = e;
              while (*p && n_ev < 32) {
                  double t = strtod(p, (char**)&p);
                  if (*p == ':') p++;
                  unsigned m = (unsigned)strtoul(p, (char**)&p, 0);
                  ev[n_ev].t = t; ev[n_ev].mask = m; n_ev++;
                  while (*p == ',' || *p == ' ') p++;
              }
              printf("[cellPad] PAD_SCRIPT: %d event(s)\n", n_ev);
          }
      }
      if (s_sc && port_no == 0) {
          double now = (double)(GetTickCount64() - t0) / 1000.0;
          /* Latch, do not window. A fixed time window is missed whenever the
           * title stops polling the pad for longer than the window (it does
           * exactly that on a loading screen), so the press silently never
           * happens. Fire on the first poll at or after the event time and
           * hold it for a fixed number of polls, which is what a real press
           * looks like regardless of poll rate. */
          static int fired[32], held[32];
          for (int i = 0; i < n_ev; i++) {
              if (!fired[i] && now >= ev[i].t) { fired[i] = 1; held[i] = 40; }
              if (held[i] > 0) {
                  held[i]--;
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(ev[i].mask & 0xFF);
                  data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((ev[i].mask >> 8) & 0xFF);
                  static int said[32];
                  if (!said[i]) { said[i] = 1;
                      printf("[cellPad] PAD_SCRIPT t=%.1fs press 0x%04X\n", ev[i].t, ev[i].mask); }
              }
          }
      } }

    /* PAD_FILE=<path> -- closed-loop input. Each poll, if the file holds a
     * button mask (same encoding as PAD_SCRIPT), press it for a fixed number
     * of polls and empty the file. A driver outside the process can then look
     * at the log or a frame dump, decide which screen the title is on, and
     * write the next button -- which a fixed time schedule cannot do, because
     * a menu system with several screens wanders when driven blind.
     *
     *   echo 0x0008 > pad.txt    # START
     *   echo 0x4000 > pad.txt    # CROSS
     *   echo 0x0040 4 > pad.txt  # DOWN for 4 polls (menus auto-repeat a
     *                            # long hold; the default is 40)
     */
    if (port_no == 0) pad_memcmd_poll();   /* v219 MEMCMD bench memory tool */
    /* Title per-frame hook (rsx_ui_trainer.h): the game's main PPU thread inside an HLE call, between frames. */
    if (port_no == 0 && g_pad_title_tick_fn) g_pad_title_tick_fn();
    { static int s_pf = -1; static char pf_path[512];
      static unsigned held_mask = 0; static int held_n = 0;
      if (s_pf < 0) { const char* e = getenv("PAD_FILE");
                      s_pf = (e && *e) ? 1 : 0;
                      if (s_pf) { strncpy(pf_path, e, sizeof pf_path - 1); pf_path[sizeof pf_path - 1] = 0; } }
      if (s_pf && port_no == 0) {
          static unsigned long long pf_next = 0; const unsigned long long pf_now = GetTickCount64();   /* v219: fopen per poll cost ~1 % of the main thread */
          if (held_n <= 0 && pf_now >= pf_next) {
              pf_next = pf_now + 50;
              FILE* f = fopen(pf_path, "rb");
              if (f) {
                  char buf[96]; buf[0] = 0; int took = 0;
                  if (fgets(buf, sizeof buf, f)) {
                      char* rest;
                      unsigned m = (unsigned)strtoul(buf, &rest, 0);
                      int n = (int)strtol(rest, &rest, 0);
                      /* v219: optional "lx ly rx ry" (0-255) after the hold count = hold the sticks there until the
                       * next stick line ("0 0 128 128 128 128" centres them). Mask 0 = sticks only. */
                      int sv[4], ns = sscanf(rest, "%d %d %d %d", &sv[0], &sv[1], &sv[2], &sv[3]);
                      if (ns == 4) { for (int k = 0; k < 4; k++) g_pf_stick[k] = (unsigned char)(sv[k] < 0 ? 0 : sv[k] > 255 ? 255 : sv[k]);
                                     g_pf_stick_on = 1; took = 1;
                                     printf("[cellPad] PAD_FILE sticks %d %d %d %d\n", sv[0], sv[1], sv[2], sv[3]); fflush(stdout); }
                      if (m) { held_mask = m; held_n = n > 0 ? n : 40; took = 1;
                               printf("[cellPad] PAD_FILE press 0x%04X\n", m); fflush(stdout); }
                  }
                  fclose(f);
                  if (took) { FILE* w = fopen(pf_path, "wb"); if (w) fclose(w); }
              }
          }
          g_pad_file_held = held_n > 0 ? held_mask : 0;   /* v219: the in-game UI overlay reads it too */
          if (held_n > 0) { held_n--;
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(held_mask & 0xFF);
              data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((held_mask >> 8) & 0xFF); }
      } }

    /* Analog sticks */
    /* PAD_STICK="lx,ly,rx,ry" (0-255, 128 = centred): hold the analog sticks at
     * fixed positions. Rubber Ducky drives its camera from the sticks and never
     * moves on its own, so with no host pad the view is frozen wherever it
     * started -- which can leave the subject of the demo off-screen or too far
     * away to make out. Legit input simulation, same footing as YDKJ_INJECT_PAD. */
    { static int s_st = -1;
      static unsigned char s_v[4] = {128,128,128,128};
      /* Optional 5th/6th fields: the seconds the hold starts and ends, so the menus
       * before gameplay are not driven by it (PAD_STICK="128,0,128,128,300,320"). */
      static double s_from = 0, s_until = 1e30; static unsigned long long s_t0 = 0;
      if (!s_t0) s_t0 = GetTickCount64();
      if (s_st < 0) { const char* e = getenv("PAD_STICK");
        s_st = e ? 1 : 0;
        if (e) { int a=128,b=128,c=128,d=128;
                 sscanf(e, "%d,%d,%d,%d,%lf,%lf", &a,&b,&c,&d,&s_from,&s_until);
                 s_v[0]=(unsigned char)a; s_v[1]=(unsigned char)b;
                 s_v[2]=(unsigned char)c; s_v[3]=(unsigned char)d; } }
      if (s_st && (double)(GetTickCount64() - s_t0) / 1000.0 >= s_from && (double)(GetTickCount64() - s_t0) / 1000.0 < s_until) { hs->analog_lx = s_v[0]; hs->analog_ly = s_v[1];
                  hs->analog_rx = s_v[2]; hs->analog_ry = s_v[3];
                  hs->connected = 1; }
      /* PAD_SWEEP=<seconds per step>: walk the sticks through a fixed set of
       * deflections instead of holding one. Guessing a single stick position and
       * re-running costs ~5 minutes a try; a sweep answers "can input move the
       * camera anywhere useful" in one run, and the frame dumps say which step
       * paid off. Steps: centre, each left-stick direction, then each right. */
      { static int sw = -1; static clock_t t0 = 0;
        if (sw < 0) { const char* e = getenv("PAD_SWEEP");
                      sw = e ? atoi(e) : 0; if (sw < 0) sw = 0; }
        if (sw) {
            static const unsigned char steps[9][4] = {
                {128,128,128,128},
                {128,  0,128,128}, {128,255,128,128},
                {  0,128,128,128}, {255,128,128,128},
                {128,128,128,  0}, {128,128,128,255},
                {128,128,  0,128}, {128,128,255,128},
            };
            if (!t0) t0 = clock();
            long el = (long)((double)(clock() - t0) / (double)CLOCKS_PER_SEC);
            int k = (int)((el / (sw > 0 ? sw : 1)) % 9);
            hs->analog_lx = steps[k][0]; hs->analog_ly = steps[k][1];
            hs->analog_rx = steps[k][2]; hs->analog_ry = steps[k][3];
            hs->connected = 1;
            { static int lastk = -1;
              if (k != lastk) { lastk = k;
                fprintf(stderr, "[PADSWEEP] step %d: l=(%u,%u) r=(%u,%u)%c", k,
                        steps[k][0], steps[k][1], steps[k][2], steps[k][3], 10); } }
        } } }
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = hs->analog_rx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = hs->analog_ry;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = hs->analog_lx;
    data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = hs->analog_ly;
    if (g_pf_stick_on && port_no == 0) {   /* v219 PAD_FILE stick hold */
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X]  = g_pf_stick[0];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y]  = g_pf_stick[1];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X] = g_pf_stick[2];
        data->button[CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y] = g_pf_stick[3];
    }
    if (port_no == 0 && s_mc_btn) {   /* v219 MEMCMD goto: jump / let go */
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] |= (u16)(s_mc_btn & 0xFF);
        data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] |= (u16)((s_mc_btn >> 8) & 0xFF);
    }

    if (g_pad_filter)
        g_pad_filter(port_no, data->button, data->len);
    if (port_no == 0) {
        g_pad_title_lstick[0] = (unsigned char)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X];
        g_pad_title_lstick[1] = (unsigned char)data->button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y];
        g_pad_title_digital = (((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xFFu) << 8) |
                              ((unsigned)data->button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xFFu);
    }

    /* Pressure-sensitive buttons (only meaningful if PRESS_ON) */
    if (setting & CELL_PAD_SETTING_PRESS_ON) {
        data->button[CELL_PAD_BTN_OFFSET_PRESS_RIGHT]    = hs->press_right;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_LEFT]     = hs->press_left;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_UP]       = hs->press_up;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_DOWN]     = hs->press_down;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = hs->press_triangle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CIRCLE]   = hs->press_circle;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_CROSS]    = hs->press_cross;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_SQUARE]   = hs->press_square;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L1]       = hs->press_l1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R1]       = hs->press_r1;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_L2]       = hs->trigger_l2;
        data->button[CELL_PAD_BTN_OFFSET_PRESS_R2]       = hs->trigger_r2;
        /* A button whose digital bit is set reads full pressure. Injected
         * input (PAD_FILE/PAD_SCRIPT/PAD_AUTOPRESS, keyboard) only sets the
         * digital bits, and the host values above zeroed any pressure it had
         * written -- so a title keying its menus off the pressure words (GH3,
         * Tornado Outbreak's title screen) never saw the press. */
        static const struct { u8 word; u8 bit; u8 off; } pm[] = {
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x20, CELL_PAD_BTN_OFFSET_PRESS_RIGHT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x80, CELL_PAD_BTN_OFFSET_PRESS_LEFT },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x10, CELL_PAD_BTN_OFFSET_PRESS_UP },
            { CELL_PAD_BTN_OFFSET_DIGITAL1, 0x40, CELL_PAD_BTN_OFFSET_PRESS_DOWN },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x10, CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x20, CELL_PAD_BTN_OFFSET_PRESS_CIRCLE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x40, CELL_PAD_BTN_OFFSET_PRESS_CROSS },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x80, CELL_PAD_BTN_OFFSET_PRESS_SQUARE },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x04, CELL_PAD_BTN_OFFSET_PRESS_L1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x08, CELL_PAD_BTN_OFFSET_PRESS_R1 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x01, CELL_PAD_BTN_OFFSET_PRESS_L2 },
            { CELL_PAD_BTN_OFFSET_DIGITAL2, 0x02, CELL_PAD_BTN_OFFSET_PRESS_R2 } };
        for (unsigned k = 0; k < sizeof pm / sizeof pm[0]; k++)
            if ((data->button[pm[k].word] & pm[k].bit) && data->button[pm[k].off] < 255)
                data->button[pm[k].off] = 255;
    }

    /* Sensor data (only meaningful if SENSOR_ON) */
    if (setting & CELL_PAD_SETTING_SENSOR_ON) {
        /* Default sensor values: accelerometer at rest (512 = 1g center) */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 399; /* gravity */
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 512;
        data->button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 512;
    }
    }  /* end connected block */

emit:
    /* A non-zero len means "here is a CHANGE packet" -- the constant is literally
     * named CELL_PAD_LEN_CHANGE_DEFAULT. Hardware reports len = 0 once the port
     * has nothing new, and titles DRAIN on that: The Simpsons Arcade Game's
     * input stage is
     *
     *     while (cellPadGetData(port, &d) == CELL_OK && d.len > 0) { ... }
     *
     * (func_0013CFA8, loop 0x13D648 -> 0x13D040). Reporting a change on every
     * call made that loop infinite: the main thread pegged one core inside
     * libpad and the title never got past its first rendered screen.
     *
     * So hand out one packet per host report and report len = 0 for any read
     * after that until the next one arrives (pad_poll_backend sets the flag when
     * it actually re-polls, every PAD_POLL_INTERVAL_MS). That is what the
     * hardware does: a pad streams reports at a fixed rate whether or not
     * anything changed, and libpad buffers them.
     *
     * Reporting only on CHANGE instead is wrong and was tried: a HELD button
     * produces exactly one packet, so a title that reads button[] only when
     * len > 0 sees the press for a single frame and then sees nothing. In this
     * game that came out as jump being the only control that seemed to work and
     * attack firing once.
     *
     * PAD_ALWAYS_CHANGE=1 restores the old always-a-packet behaviour. */
    {
        static int always = -1;
        if (always < 0) always = getenv("PAD_ALWAYS_CHANGE") ? 1 : 0;
        if (!always && port_no < PAD_MAX_HOST_PORTS && data->len) {
            if (!s_data_fresh[port_no]) data->len = 0;   /* nothing new: end the drain */
            else                        s_data_fresh[port_no] = 0;
        }
        if (port_no < PAD_MAX_HOST_PORTS && data->len) (void)PAD_LATCH_TAKE(port_no);
    }
    {
        unsigned int ea = (unsigned int)(uintptr_t)data_guest;
        vm_write32((unsigned long long)ea + 0, (unsigned int)data->len);   /* len (s32) */
        for (int _i = 0; _i < CELL_PAD_MAX_CODES; _i++)                    /* button[] u16 */
            vm_write16((unsigned long long)ea + 4 + (unsigned int)_i * 2, data->button[_i]);
    }
    return CELL_OK;
}

/* cellPadGetInfo (v1 API, NID 0x3AAAD464) — used by PSL1GHT's ioPadGetInfo.
 * Layout (RPCS3 cellPad.h CellPadInfo, all big-endian):
 *   +0x00 u32 max_connect        +0x04 u32 now_connect   +0x08 u32 system_info
 *   +0x0C u16 vendor_id[127]     +0x10A u16 product_id[127] +0x208 u8 status[127]
 *   (CELL_MAX_PADS / PSL1GHT MAX_PADS = 127 -- NOT 7; see below)
 * Leaving this unimplemented let the guest read stack garbage as pad state and
 * run off into the weeds before ever reaching gcm init. */
s32 cellPadGetInfo(CellPadInfo2* info)
{
    uint32_t gaddr = (uint32_t)(uintptr_t)info;
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!gaddr)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    /* Present a virtual pad on port 0 when nothing physical is attached, the
     * same as cellPadGetInfo2 does.
     *
     * The two entry points disagreed: GetInfo2 carried the fallback and GetInfo
     * did not, so which one a title imports decided whether it believed a
     * controller existed. The Orange Box's chooser imports GetInfo (v1) and
     * never GetInfo2, so it saw zero pads, opened "SIXAXIS wireless controller
     * not detected", took the auto-OK, and opened it again -- forever, before
     * it ever finished booting. Both entry points describe the same hardware
     * and have to answer the same way. */
    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++)
        if (s_host_state[i].connected || i == 0) connected++;

    vm_write32(gaddr + 0x00, s_max_connect);
    vm_write32(gaddr + 0x04, connected);
    vm_write32(gaddr + 0x08, 0);                       /* system_info */
    /* CellPadInfo (v1) sizes its arrays by CELL_MAX_PADS = 127, not by the 7
     * ports: vendor_id[127] @0x0C, product_id[127] @0x10A, status[127] @0x208.
     * Packed for 7 ports, status landed at 0x28 and GH3 -- which reads
     * info+0x208+port -- saw port 0 disconnected and never polled the pad. */
    for (u32 i = 0; i < CELL_PAD_MAX_PORT_NUM; i++) {
        int on = (i < s_max_connect && i < PAD_MAX_HOST_PORTS &&
                  (s_host_state[i].connected || i == 0));
        vm_write16(gaddr + 0x00C + i * 2, on ? 0x054C : 0);   /* vendor: Sony  */
        vm_write16(gaddr + 0x10A + i * 2, on ? 0x0268 : 0);   /* product: DS3  */
        vm_write8(gaddr + 0x208 + i, on ? CELL_PAD_STATUS_CONNECTED : 0);
    }
    return CELL_OK;
}

s32 cellPadGetInfo2(CellPadInfo2* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Poll to get latest connection state */
    pad_poll_backend();

    /* Build in a local host struct, then copy to guest memory big-endian. */
    CellPadInfo2 _in; CellPadInfo2* info = &_in;
    memset(info, 0, sizeof(CellPadInfo2));
    info->max_connect = s_max_connect;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < PAD_MAX_HOST_PORTS; i++) {
        /* Always present a virtual pad on port 0 (standard emulator behavior) so
         * boot flows that block until a controller is connected can proceed even
         * with no physical device attached. */
        if (s_host_state[i].connected || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    /* copy to guest big-endian (CellPadInfo2 is all u32 fields) */
    {
        unsigned int ea = (unsigned int)(uintptr_t)info_guest;
        for (unsigned int _o = 0; _o < sizeof(CellPadInfo2); _o += 4)
            vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o));
    }
    return CELL_OK;
}

/* cellPadPeriphGetInfo -- the peripheral-class view of the pad ports (NID
 * 0x4CC9B68D). LBP polls this during boot; leaving it unresolved returned
 * CELL_OK with the caller's CellPadPeriphInfo left as uninitialised stack
 * garbage, so the game read junk port_status/pclass_type for all 7 ports.
 *
 * Layout (SDK cell/pad/pad_codes.h) -- all u32, so the whole struct marshals
 * big-endian in one sweep like cellPadGetInfo2:
 *   max_connect, now_connect, system_info,
 *   port_status[7], port_setting[7], device_capability[7],
 *   device_type[7], pclass_type[7], pclass_profile[7]
 * We report a standard DUALSHOCK-class pad (pclass_type STANDARD = 0, no
 * profile bits) mirroring the ports cellPadGetInfo2 already reports. */
s32 cellPadPeriphGetInfo(CellPadPeriphInfo* info_guest)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (!info_guest)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    pad_poll_backend();

    CellPadPeriphInfo _in; CellPadPeriphInfo* info = &_in;
    memset(info, 0, sizeof(CellPadPeriphInfo));
    info->max_connect = s_max_connect;
    info->system_info = 0;

    u32 connected = 0;
    for (u32 i = 0; i < s_max_connect && i < CELL_PAD_MAX_PORT_NUM; i++) {
        /* Same virtual-pad-on-port-0 rule as cellPadGetInfo2, so both views of
         * the ports agree (a game that cross-checks them must see one truth). */
        if ((i < PAD_MAX_HOST_PORTS && s_host_state[i].connected) || i == 0) {
            info->port_status[i]       = CELL_PAD_STATUS_CONNECTED;
            info->port_setting[i]      = s_port_setting[i];
            info->device_capability[i] = CELL_PAD_CAPABILITY_PS3_CONFORMITY
                                       | CELL_PAD_CAPABILITY_PRESS_MODE
                                       | CELL_PAD_CAPABILITY_SENSOR_MODE
                                       | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                                       | CELL_PAD_CAPABILITY_ACTUATOR;
            info->device_type[i]       = CELL_PAD_DEV_TYPE_STANDARD;
            info->pclass_type[i]       = CELL_PAD_PCLASS_TYPE_STANDARD;
            info->pclass_profile[i]    = 0;
            connected++;
        } else {
            info->port_status[i] = CELL_PAD_STATUS_DISCONNECTED;
        }
    }
    info->now_connect = connected;

    { unsigned int ea = (unsigned int)(uintptr_t)info_guest;
      for (unsigned int _o = 0; _o < sizeof(CellPadPeriphInfo); _o += 4)
          vm_write32((unsigned long long)ea + _o, *(u32*)((char*)info + _o)); }

    { static int _n = 0;
      if ((_n < 2 && _n++ < 2))
          printf("[cellPad] PeriphGetInfo(max=%u now=%u) -> STANDARD class\n",
                 info->max_connect, connected); }
    return CELL_OK;
}

s32 cellPadSetPortSetting(u32 port_no, u32 port_setting)
{
    printf("[cellPad] SetPortSetting(port=%u, setting=0x%X)\n",
           port_no, port_setting);

    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    s_port_setting[port_no] = port_setting;
    return CELL_OK;
}

/* cellPadInfoPressMode (NID 0x0E2DFAAD) / cellPadSetPressMode (NID 0xF83F8182)
 *
 * Press mode makes the digital buttons report an analog 0..255 pressure in the
 * extended part of CellPadData. A DS3 supports it, and the virtual pad reported
 * by cellPadGetInfo/GetInfo2 already advertises CELL_PAD_CAPABILITY_PRESS_MODE,
 * so these have to agree with that claim rather than fall through to the
 * unresolved-NID path.
 *
 * Info returns 1 when the port supports press mode; Set toggles it, recorded in
 * the same s_port_setting word cellPadSetPortSetting uses so a title that reads
 * the setting back sees what it asked for. */
s32 cellPadInfoPressMode(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    return 1;                                  /* supported */
}

s32 cellPadSetPressMode(u32 port_no, u32 mode)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;
    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;
    if (mode)
        s_port_setting[port_no] |= CELL_PAD_SETTING_PRESS_ON;
    else
        s_port_setting[port_no] &= ~(u32)CELL_PAD_SETTING_PRESS_ON;
    return CELL_OK;
}

s32 cellPadGetCapabilityInfo(u32 port_no, CellPadCapabilityInfo* info)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !info)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    u32 info_ea = (u32)(uintptr_t)info;
    for (u32 i = 0; i < CELL_PAD_MAX_CODES; i++)
        vm_write32(info_ea + i * 4, 0);

    /* Report standard DualShock 3 capabilities */
    vm_write32(info_ea, CELL_PAD_CAPABILITY_PS3_CONFORMITY
                        | CELL_PAD_CAPABILITY_PRESS_MODE
                        | CELL_PAD_CAPABILITY_SENSOR_MODE
                        | CELL_PAD_CAPABILITY_HP_ANALOG_STICK
                        | CELL_PAD_CAPABILITY_ACTUATOR);

    return CELL_OK;
}

s32 cellPadSetActDirect(u32 port_no, CellPadActParam* param)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM || !param)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    param = GUEST_PTR(param, CellPadActParam*);
#if defined(PS3RECOMP_SDL3) && PAD_BACKEND_XINPUT
    if (!pad_rumble_enabled()) return CELL_OK;   /* PAD_RUMBLE=0 */
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_slot[port_no] >= 0) {   /* a PlayStation / Steam controller */
        pad_sdl3_rumble(s_sdl_slot[port_no], param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL], param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE]);
        return CELL_OK;
    }
#endif

#if PAD_BACKEND_XINPUT
    /* Map to XInput vibration */
    if (port_no < PAD_MAX_HOST_PORTS && s_host_state[port_no].connected) {
        XINPUT_VIBRATION vib;
        vib.wLeftMotorSpeed  = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257);
        vib.wRightMotorSpeed = (WORD)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257);
        XInputSetState((DWORD)port_no, &vib);
    }
#endif

#if PAD_BACKEND_SDL2
    if (port_no < PAD_MAX_HOST_PORTS && s_sdl_controllers[port_no]) {
        SDL_GameControllerRumble(
            s_sdl_controllers[port_no],
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_LARGE] * 257),
            (Uint16)(param->motor[CELL_PAD_ACTUATOR_PARAM_SMALL] * 257),
            100 /* duration ms */
        );
    }
#endif

    return CELL_OK;
}

s32 cellPadClearBuf(u32 port_no)
{
    if (!s_pad_initialized)
        return CELL_PAD_ERROR_NOT_OPENED;

    if (port_no >= CELL_PAD_MAX_PORT_NUM)
        return CELL_PAD_ERROR_INVALID_PARAMETER;

    /* Nothing to clear in our implementation -- state is polled fresh */
    return CELL_OK;
}

#ifndef _WIN32
void ps3_pad_mouse_inject(u32 ctrl) { (void)ctrl; }   /* v151: KBM is Windows-only */
void ps3_pad_rstick_read(u32 site, u32 which, u32 kind) { (void)site; (void)which; (void)kind; }   /* v236 */
void ps3_pad_mouse_cam_spring(u32 cam) { (void)cam; }
int  ps3_pad_mouse_raw_turn(void) { return 0; }
#endif
