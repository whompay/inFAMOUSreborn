/*
 * rsx_ui_overlay.c -- in-game system-dialog overlay (cellMsgDialog) for the D3D12 backend.
 *
 * Replaces the blocking Win32 MessageBox with a prompt drawn into the game's own frame, the
 * way the PS3 system software overlays its dialogs: the title keeps running, the dialog owns
 * the controller until it is answered, and the answer reaches the title through the normal
 * sysutil callback (cellMsgDialog_tick).
 *
 * Look: the title's own HUD typeface (font_main_latin) and controller glyphs (font_icons),
 * extracted from the game data by infamous-recomp/tools/extract_ui_font.py into
 * assets/ui_font.bin -- see that script for the format. No asset -> uiov_dialog_available()
 * is 0 and cellMsgDialog keeps its MessageBox fallback.
 *
 * Threads: cellMsgDialog (PPU thread) opens/polls/closes; the render thread draws and reads
 * input (uiov_render, once per presented frame); cellPadGetData asks uiov_pad_gate() whether
 * the title may see the pad. All shared state is behind s_cs.
 *
 * Input: Cross = XInput A / Enter / Space / Z, Circle = XInput B / Esc / Backspace / X / C,
 * left/right = d-pad / left stick / arrow keys / A,D / Tab; the mouse hovers and clicks buttons.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rsx_ui_trainer.h"
/* rsx_ui_trainer.h: the title's trainer page (F10); NULL = no trainer. */
const uiov_trainer_api* volatile g_uiov_trainer = 0;

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include "rsx_d3d12_abi.h"   /* struct-returning D3D12 calls for every Windows SDK */
#include <d3dcompiler.h>
#include <xinput.h>
#include <math.h>
#include <limits.h>

#define UIOV_VW_H      720.0f     /* virtual layout height; width follows the backbuffer aspect */
#define UIOV_MAX_VERTS 12000
#define UIOV_FRAMES    3

typedef struct { float x, y, u, v, r, g, b, a; } UVert;
typedef struct { unsigned code; short x, y, w, h, xo, yo, adv, pad; } UGlyph;

/* ---------------- font asset ---------------- */
static int      s_font_state;            /* 0 untried, 1 ok, -1 failed */
static unsigned s_aw, s_ah, s_nt, s_tline, s_ni, s_iline, s_wx, s_wy;
static UGlyph*  s_gl;                    /* s_nt text glyphs then s_ni icon glyphs */
static unsigned char* s_pix;             /* RGBA8 atlas (freed after upload) */

static int font_try(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8]; unsigned h[8];
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "IFUIFNT1", 8) || fread(h, 4, 8, f) != 8) { fclose(f); return 0; }
    s_aw = h[0]; s_ah = h[1]; s_nt = h[2]; s_tline = h[3]; s_ni = h[4]; s_iline = h[5]; s_wx = h[6]; s_wy = h[7];
    if (s_aw != 1024 || s_ah != 1024 || !s_nt || s_nt + s_ni > 4096) { fclose(f); return 0; }
    s_gl = (UGlyph*)calloc(s_nt + s_ni, sizeof(UGlyph));
    s_pix = (unsigned char*)malloc((size_t)s_aw * s_ah * 4);
    int ok = s_gl && s_pix;
    for (unsigned i = 0; ok && i < s_nt + s_ni; i++) {
        unsigned char r[20];
        if (fread(r, 1, 20, f) != 20) { ok = 0; break; }
        memcpy(&s_gl[i].code, r, 4); memcpy(&s_gl[i].x, r + 4, 16);
    }
    if (ok && fread(s_pix, 1, (size_t)s_aw * s_ah * 4, f) != (size_t)s_aw * s_ah * 4) ok = 0;
    fclose(f);
    if (!ok) { free(s_gl); free(s_pix); s_gl = NULL; s_pix = NULL; return 0; }
    fprintf(stderr, "[uiov] dialog font loaded: %s (%u text + %u icon glyphs)\n", path, s_nt, s_ni);
    return 1;
}

static int font_load(void)
{
    if (s_font_state) return s_font_state > 0;
    s_font_state = -1;
    const char* e = getenv("UI_FONT");
    if (e && *e && font_try(e)) { s_font_state = 1; return 1; }
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\ui_font.bin", "\\..\\assets\\ui_font.bin", "\\..\\..\\assets\\ui_font.bin" };
        for (int i = 0; i < 3; i++) {
            char p[MAX_PATH + 64]; snprintf(p, sizeof p, "%s%s", exe, rel[i]);
            if (font_try(p)) { s_font_state = 1; return 1; }
        }
    }
    fprintf(stderr, "[uiov] no assets\\ui_font.bin (run infamous-recomp\\tools\\extract_ui_font.py) -> MessageBox dialogs\n");
    return 0;
}

static const UGlyph* glyph(unsigned code, int icon)
{
    unsigned a = icon ? s_nt : 0, b = icon ? s_nt + s_ni : s_nt;
    for (unsigned i = a; i < b; i++) if (s_gl[i].code == code) return &s_gl[i];
    return NULL;
}

/* ---------------- dialog state ---------------- */
static CRITICAL_SECTION s_cs;
static volatile LONG s_cs_ready;
static void lock(void)
{
    if (s_cs_ready != 2) {
        if (InterlockedCompareExchange(&s_cs_ready, 1, 0) == 0) { InitializeCriticalSection(&s_cs); s_cs_ready = 2; }
        else while (s_cs_ready != 2) Sleep(0);
    }
    EnterCriticalSection(&s_cs);
}
static void unlock(void) { LeaveCriticalSection(&s_cs); }

static struct {
    int active;               /* dialog on screen and owning input */
    int kind;                 /* 1 = OK, 2 = YES/NO */
    int sel;                  /* 0 = first button (OK/Yes), 1 = No */
    int result;               /* 0 pending, 1 OK/YES, 2 NO -- read by the PPU side */
    char msg[2048];
    ULONGLONG t_open, t_close;
    int need_clear;           /* pad latch must be dropped once after the dialog */
    int held;                 /* a dialog key is still held after the answer */
    unsigned prev;            /* input bits last frame (edge detection) */
    float fade_close;         /* alpha the dialog had when it was answered (fade-out) */
    int last_kind, last_sel; char last_msg[2048];
} D;
static volatile ULONGLONG s_last_draw_ms;

int uiov_dialog_available(void)
{
    if (!font_load()) return 0;
    ULONGLONG now = GetTickCount64();
    return s_last_draw_ms && now - s_last_draw_ms < 1500;   /* the renderer is presenting frames */
}

unsigned long long uiov_last_draw_ms(void) { return s_last_draw_ms; }

void uiov_dialog_open(const char* msg, int kind, int default_no)
{
    lock();
    memset(D.msg, 0, sizeof D.msg);
    strncpy(D.msg, msg ? msg : "", sizeof D.msg - 1);
    D.kind = kind; D.sel = (kind == 2 && default_no) ? 1 : 0; D.result = 0;
    D.t_open = GetTickCount64(); D.active = 1; D.prev = 0xFFFFFFFFu;   /* keys already down at open are ignored */
    unlock();
    fprintf(stderr, "[uiov] dialog open (%s): %s\n", kind == 2 ? "yes/no" : "ok", msg ? msg : "");
}

int uiov_dialog_result(void)
{
    lock(); int r = D.result; unlock();
    return r;
}

void uiov_dialog_close(void)
{
    lock();
    if (D.active) { D.active = 0; D.t_close = GetTickCount64(); D.need_clear = 1; D.fade_close = 1.0f;
                    memcpy(D.last_msg, D.msg, sizeof D.msg); D.last_kind = D.kind; D.last_sel = D.sel; }
    D.result = 0;
    unlock();
}

/* 1 = hide the pad from the title, 2 = drop the press latch once then deliver, 0 = normal. */
static volatile int s_menu_open = 0;            /* v194 settings menu (render thread owns it) */
static volatile ULONGLONG s_menu_close_ms = 0;
static volatile int s_menu_need_clear = 0;
volatile int g_uiov_menu_open = 0;              /* read by cellPad.c (free the mouse, no Esc toggle) */
static volatile int s_tr_open = 0;              /* F10 trainer page (render thread owns it) */
static volatile ULONGLONG s_tr_close_ms = 0;
static volatile int s_tr_need_clear = 0;
int uiov_pad_gate(void)
{
    if (s_menu_open) return 1;                   /* v194: the settings menu owns the pad */
    if (s_tr_open) return 1;                     /* F10 trainer page owns the pad */
    if (s_tr_close_ms && GetTickCount64() - s_tr_close_ms < 250) return 1;
    if (s_tr_need_clear) { s_tr_need_clear = 0; return 2; }
    if (s_menu_close_ms && GetTickCount64() - s_menu_close_ms < 250) return 1;
    if (s_menu_need_clear) { s_menu_need_clear = 0; return 2; }
    if (s_cs_ready != 2) return 0;
    lock();
    int g = 0;
    ULONGLONG now = GetTickCount64();
    if (D.active || (D.held && now - D.t_close < 3000) || (D.t_close && now - D.t_close < 300)) g = 1;
    else if (D.need_clear) { D.need_clear = 0; g = 2; }
    unlock();
    return g;
}

/* ---------------- input ---------------- */
enum { IN_OK = 1, IN_BACK = 2, IN_LEFT = 4, IN_RIGHT = 8, IN_TAB = 16, IN_UP = 32, IN_DOWN = 64, IN_SELECT = 128 };
static unsigned read_input(HWND hwnd)
{
    unsigned b = 0;
    for (DWORD i = 0; i < 4; i++) {
        XINPUT_STATE st; memset(&st, 0, sizeof st);
        if (XInputGetState(i, &st) != ERROR_SUCCESS) continue;
        WORD w = st.Gamepad.wButtons;
        if (w & XINPUT_GAMEPAD_A) b |= IN_OK;
        if (w & XINPUT_GAMEPAD_B) b |= IN_BACK;
        if ((w & XINPUT_GAMEPAD_DPAD_LEFT) || st.Gamepad.sThumbLX < -16000) b |= IN_LEFT;
        if ((w & XINPUT_GAMEPAD_DPAD_RIGHT) || st.Gamepad.sThumbLX > 16000) b |= IN_RIGHT;
        if ((w & XINPUT_GAMEPAD_DPAD_UP) || st.Gamepad.sThumbLY > 16000) b |= IN_UP;
        if ((w & XINPUT_GAMEPAD_DPAD_DOWN) || st.Gamepad.sThumbLY < -16000) b |= IN_DOWN;
        if (w & XINPUT_GAMEPAD_BACK) b |= IN_SELECT;
    }
    { extern volatile unsigned g_pad_file_held;   /* v219: PAD_FILE (unattended bench driver), PS3 button mask */
      const unsigned m = g_pad_file_held;
      if (m & 0x4000) b |= IN_OK;     if (m & 0x2000) b |= IN_BACK;
      if (m & 0x0080) b |= IN_LEFT;   if (m & 0x0020) b |= IN_RIGHT;
      if (m & 0x0010) b |= IN_UP;     if (m & 0x0040) b |= IN_DOWN;
      if (m & 0x0001) b |= IN_SELECT; }
    if (hwnd && GetForegroundWindow() == hwnd) {
        #define K(vk) (GetAsyncKeyState(vk) & 0x8000)
        if (K(VK_RETURN) || K(VK_SPACE) || K('Z')) b |= IN_OK;
        if (K(VK_ESCAPE) || K(VK_BACK) || K('X') || K('C')) b |= IN_BACK;
        if (K(VK_LEFT) || K('A')) b |= IN_LEFT;
        if (K(VK_RIGHT) || K('D')) b |= IN_RIGHT;
        if (K(VK_TAB)) b |= IN_TAB;
        if (K(VK_UP) || K('W')) b |= IN_UP;
        if (K(VK_DOWN) || K('S')) b |= IN_DOWN;
        #undef K
    }
    return b;
}

/* ---------------- geometry ---------------- */
static UVert* s_v; static unsigned s_nv;
static float s_k, s_vw;                    /* backbuffer px per virtual px, virtual width */

static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                    const float c0[4], const float c1[4], int horiz)
{
    if (s_nv + 6 > UIOV_MAX_VERTS) return;
    /* c0 at top/left, c1 at bottom/right */
    const float* tl = c0; const float* tr = horiz ? c1 : c0; const float* bl = horiz ? c0 : c1; const float* br = c1;
    UVert q[4] = {
        { x0, y0, u0, v0, tl[0], tl[1], tl[2], tl[3] }, { x1, y0, u1, v0, tr[0], tr[1], tr[2], tr[3] },
        { x0, y1, u0, v1, bl[0], bl[1], bl[2], bl[3] }, { x1, y1, u1, v1, br[0], br[1], br[2], br[3] } };
    UVert* o = s_v + s_nv;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[2]; o[4] = q[1]; o[5] = q[3];
    s_nv += 6;
}
static void rect(float x0, float y0, float x1, float y1, const float c0[4], const float c1[4], int horiz)
{
    float u = (s_wx + 0.5f) / s_aw, v = (s_wy + 0.5f) / s_ah;
    quad_uv(x0, y0, x1, y1, u, v, u, v, c0, c1, horiz);
}
static void rgba(float o[4], float r, float g, float b, float a) { o[0] = r; o[1] = g; o[2] = b; o[3] = a; }

static unsigned utf8_next(const unsigned char** p)
{
    const unsigned char* s = *p; unsigned c = *s++;
    if (c >= 0xC0 && c < 0xE0 && (*s & 0xC0) == 0x80) { c = ((c & 0x1F) << 6) | (*s++ & 0x3F); }
    else if (c >= 0xE0 && c < 0xF0 && (s[0] & 0xC0) == 0x80 && (s[1] & 0xC0) == 0x80) { c = ((c & 0x0F) << 12) | ((s[0] & 0x3F) << 6) | (s[1] & 0x3F); s += 2; }
    else if (c >= 0x80) c = '?';
    *p = s; return c;
}
static unsigned map_char(unsigned c)
{
    if (c == 0x2018 || c == 0x2019) return '\'';
    if (c == 0x201C || c == 0x201D) return '"';
    if (c == 0x2013 || c == 0x2014) return '-';
    return c;
}
static float text_width(const char* s, int n, float size)
{
    float w = 0; const unsigned char* p = (const unsigned char*)s; const unsigned char* e = p + n;
    while (p < e && *p) { unsigned c = map_char(utf8_next(&p)); const UGlyph* g = glyph(c, 0); if (!g) g = glyph(' ', 0); if (g) w += g->adv; }
    return w * size / s_tline;
}
static void text_draw(const char* s, int n, float x, float y, float size, const float col[4])
{
    float sc = size / s_tline, pen = x; const unsigned char* p = (const unsigned char*)s; const unsigned char* e = p + n;
    while (p < e && *p) {
        unsigned c = map_char(utf8_next(&p)); const UGlyph* g = glyph(c, 0);
        if (!g) g = glyph(' ', 0);
        if (!g) continue;
        if (g->w > 2 && g->h > 2) {
            float x0 = pen + g->xo * sc, y0 = y + g->yo * sc;
            quad_uv(x0, y0, x0 + g->w * sc, y0 + g->h * sc,
                    (float)g->x / s_aw, (float)g->y / s_ah, (float)(g->x + g->w) / s_aw, (float)(g->y + g->h) / s_ah, col, col, 0);
        }
        pen += g->adv * sc;
    }
}
static float icon_draw(unsigned code, float x, float y, float size, float alpha)
{
    const UGlyph* g = glyph(code, 1); if (!g) return 0;
    float sc = size / s_iline, c[4]; rgba(c, 1, 1, 1, alpha);
    quad_uv(x, y, x + g->w * sc, y + g->h * sc, (float)g->x / s_aw, (float)g->y / s_ah,
            (float)(g->x + g->w) / s_aw, (float)(g->y + g->h) / s_ah, c, c, 0);
    return g->adv * sc;
}

/* word-wrap into at most 12 lines: (start, len) pairs */
static int wrap(const char* msg, float size, float maxw, int st[12], int ln[12])
{
    int n = 0, len = (int)strlen(msg), i = 0;
    while (i < len && n < 12) {
        while (i < len && msg[i] == ' ') i++;
        int start = i, last_ok = -1, j = i;
        while (j <= len) {
            if (j == len || msg[j] == ' ' || msg[j] == '\n') {
                if (last_ok >= 0 && text_width(msg + start, j - start, size) > maxw) break;
                last_ok = j;
                if (j == len || msg[j] == '\n') break;
            }
            j++;
        }
        if (last_ok < 0) last_ok = j;
        st[n] = start; ln[n] = last_ok - start; n++;
        i = last_ok + ((last_ok < len && (msg[last_ok] == '\n' || msg[last_ok] == ' ')) ? 1 : 0);
    }
    return n;
}

typedef struct { float x0, y0, x1, y1; } BtnRect;
static BtnRect s_br[2];          /* last drawn button rects (mouse hit test), render thread only */
static int s_nb;

/* Builds the dialog; returns the button hit rectangles (virtual px). */
static int build_dialog(const char* msg, int kind, int sel, float alpha, float grow, BtnRect* br)
{
    const float cx = s_vw * 0.5f, cy = UIOV_VW_H * 0.5f;
    float c0[4], c1[4];
    static const float BLUE[3] = { 0.36f, 0.72f, 1.0f };
    /* dim the scene */
    rgba(c0, 0, 0, 0, 0.45f * alpha); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);
    float size = 30.0f, lh = 40.0f; int st[12], ln[12];
    int nl = wrap(msg, size, 720.0f, st, ln);
    if (nl > 5) { size = 25.0f; lh = 33.0f; nl = wrap(msg, size, 760.0f, st, ln); }
    float bh = (nl * lh + 110.0f) * grow, y0 = cy - bh * 0.5f, y1 = cy + bh * 0.5f, fade = 36.0f;
    /* the band, with soft top/bottom edges */
    rgba(c0, 0, 0, 0, 0); rgba(c1, 0, 0, 0, 0.80f * alpha);
    rect(0, y0 - fade, s_vw, y0, c0, c1, 0);
    rect(0, y0, s_vw, y1, c1, c1, 0);
    rect(0, y1, s_vw, y1 + fade, c1, c0, 0);
    /* electric edge lines: glow + core, fading out toward the ends */
    float lx0 = cx - 500.0f, lx1 = cx + 500.0f, ys[2] = { y0, y1 - 2.0f };
    for (int i = 0; i < 2; i++) {
        float y = ys[i];
        rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0); rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.13f * alpha);
        rect(lx0, y - 3, cx, y + 5, c0, c1, 1); rect(cx, y - 3, lx1, y + 5, c1, c0, 1);
        rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.95f * alpha);
        rect(lx0, y, cx, y + 2, c0, c1, 1); rect(cx, y, lx1, y + 2, c1, c0, 1);
    }
    if (grow < 0.999f) return 0;
    /* message */
    float ty = y0 + 28.0f; float white[4]; rgba(white, 0.96f, 0.96f, 0.96f, alpha);
    for (int i = 0; i < nl; i++) {
        float w = text_width(msg + st[i], ln[i], size);
        text_draw(msg + st[i], ln[i], cx - w * 0.5f, ty - size * 0.18f, size, white);
        ty += lh;
    }
    /* buttons */
    const float isz = 30.0f, gap = 60.0f, lsz = 26.0f;
    int nb = kind == 2 ? 2 : 1;
    const char* lab[2] = { kind == 2 ? "Yes" : "OK", "No" };
    unsigned ico[2] = { 0xF083u, 0xF080u };            /* cross, circle */
    float bw[2], tot = 0;
    for (int i = 0; i < nb; i++) { bw[i] = isz + 10.0f + text_width(lab[i], (int)strlen(lab[i]), lsz); tot += bw[i]; }
    tot += gap * (nb - 1);
    float x = cx - tot * 0.5f, by = y1 - 62.0f;
    for (int i = 0; i < nb; i++) {
        int on = (i == sel);
        if (on && nb > 1) { rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0.9f * alpha); rect(x - 14, by + isz + 8, x + bw[i] + 14, by + isz + 10, c0, c0, 0); }
        icon_draw(ico[i], x, by, isz, alpha);
        float col[4]; if (on || nb == 1) rgba(col, 1, 1, 1, alpha); else rgba(col, 0.62f, 0.62f, 0.62f, alpha);
        text_draw(lab[i], (int)strlen(lab[i]), x + isz + 10.0f, by + isz * 0.5f - lsz * 0.5f - lsz * 0.12f, lsz, col);
        if (br) { br[i].x0 = x - 14; br[i].y0 = by - 6; br[i].x1 = x + bw[i] + 14; br[i].y1 = by + isz + 12; }
        x += bw[i] + gap;
    }
    return nb;
}

/* ---------------- D3D12 ---------------- */
static int s_gfx_state;                     /* 0 untried, 1 ok, -1 failed */
static ID3D12RootSignature* s_rs;
static ID3D12PipelineState* s_pso;
static ID3D12DescriptorHeap* s_heap;
static ID3D12Resource* s_tex;
static ID3D12Resource* s_tex_up;
static int s_tex_uploaded;
static ID3D12Resource* s_vb[UIOV_FRAMES];
static UVert* s_vbmap[UIOV_FRAMES];
static unsigned s_vbi;

static int gfx_init(ID3D12Device* dev, DXGI_FORMAT rtfmt)
{
    if (s_gfx_state) return s_gfx_state > 0;
    s_gfx_state = -1;
    HRESULT hr;
    /* root signature: 4 VS constants (scale/offset), one SRV table, static linear-clamp sampler */
    D3D12_DESCRIPTOR_RANGE rg = {0};
    rg.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rg.NumDescriptors = 1;
    rg.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    D3D12_ROOT_PARAMETER rp[2]; memset(rp, 0, sizeof rp);
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[0].Constants.Num32BitValues = 4;
    rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1; rp[1].DescriptorTable.pDescriptorRanges = &rg;
    rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sd = {0};
    sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sd.MaxLOD = D3D12_FLOAT32_MAX; sd.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rd = {0};
    rd.NumParameters = 2; rd.pParameters = rp; rd.NumStaticSamplers = 1; rd.pStaticSamplers = &sd;
    rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *sig = NULL, *err = NULL;
    hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] root signature serialize failed\n"); if (err) err->lpVtbl->Release(err); return 0; }
    hr = dev->lpVtbl->CreateRootSignature(dev, 0, sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                                         &IID_ID3D12RootSignature, (void**)&s_rs);
    sig->lpVtbl->Release(sig);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] root signature failed 0x%08lX\n", hr); return 0; }

    static const char vs[] =
        "cbuffer c : register(b0) { float4 so; };\n"
        "struct I { float2 p:POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "O main(I i) { O o; o.p = float4(i.p.x * so.x - 1.0, 1.0 - i.p.y * so.y, 0, 1); o.t = i.t; o.c = i.c; return o; }\n";
    static const char ps[] =
        "Texture2D tx : register(t0); SamplerState sm : register(s0);\n"
        "struct O { float4 p:SV_POSITION; float2 t:TEXCOORD; float4 c:COLOR; };\n"
        "float4 main(O i) : SV_TARGET { return tx.Sample(sm, i.t) * i.c; }\n";
    ID3DBlob *vb = NULL, *pb = NULL;
    hr = D3DCompile(vs, sizeof vs - 1, "uiov_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &err);
    if (SUCCEEDED(hr)) hr = D3DCompile(ps, sizeof ps - 1, "uiov_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &err);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] shader compile failed: %s\n", err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "?"); return 0; }
    D3D12_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
    pd.pRootSignature = s_rs;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 3;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    D3D12_RENDER_TARGET_BLEND_DESC* bd = &pd.BlendState.RenderTarget[0];
    bd->BlendEnable = TRUE; bd->SrcBlend = D3D12_BLEND_SRC_ALPHA; bd->DestBlend = D3D12_BLEND_INV_SRC_ALPHA; bd->BlendOp = D3D12_BLEND_OP_ADD;
    bd->SrcBlendAlpha = D3D12_BLEND_ONE; bd->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; bd->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    bd->LogicOp = D3D12_LOGIC_OP_NOOP; bd->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX; pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = rtfmt; pd.SampleDesc.Count = 1;
    hr = dev->lpVtbl->CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void**)&s_pso);
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
    if (FAILED(hr)) { fprintf(stderr, "[uiov] PSO failed 0x%08lX\n", hr); return 0; }

    D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->lpVtbl->CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_heap))) { fprintf(stderr, "[uiov] heap failed\n"); return 0; }

    D3D12_HEAP_PROPERTIES hpd = {0}, hpu = {0}; hpd.Type = D3D12_HEAP_TYPE_DEFAULT; hpu.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = s_aw; td.Height = s_ah; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpd, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                                                    &IID_ID3D12Resource, (void**)&s_tex))) { fprintf(stderr, "[uiov] texture failed\n"); return 0; }
    D3D12_RESOURCE_DESC bdsc = {0};
    bdsc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bdsc.Width = (UINT64)s_aw * s_ah * 4; bdsc.Height = 1; bdsc.DepthOrArraySize = 1;
    bdsc.MipLevels = 1; bdsc.SampleDesc.Count = 1; bdsc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bdsc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                    &IID_ID3D12Resource, (void**)&s_tex_up))) { fprintf(stderr, "[uiov] upload failed\n"); return 0; }
    void* m = NULL; D3D12_RANGE nr = { 0, 0 };
    if (FAILED(s_tex_up->lpVtbl->Map(s_tex_up, 0, &nr, &m))) return 0;
    memcpy(m, s_pix, (size_t)s_aw * s_ah * 4);     /* 1024 * 4 = 4096-byte rows: already 256-aligned */
    s_tex_up->lpVtbl->Unmap(s_tex_up, 0, NULL);
    free(s_pix); s_pix = NULL;

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE ch; RSX_D3D12_CPU_HEAP_START(s_heap)(s_heap, &ch);
    dev->lpVtbl->CreateShaderResourceView(dev, s_tex, &sv, ch);

    bdsc.Width = (UINT64)UIOV_MAX_VERTS * sizeof(UVert);
    for (int i = 0; i < UIOV_FRAMES; i++) {
        if (FAILED(dev->lpVtbl->CreateCommittedResource(dev, &hpu, D3D12_HEAP_FLAG_NONE, &bdsc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                        &IID_ID3D12Resource, (void**)&s_vb[i]))) { fprintf(stderr, "[uiov] vb failed\n"); return 0; }
        if (FAILED(s_vb[i]->lpVtbl->Map(s_vb[i], 0, &nr, (void**)&s_vbmap[i]))) return 0;
    }
    s_gfx_state = 1;
    fprintf(stderr, "[uiov] in-game dialog renderer ready\n");
    return 1;
}

/* Called by the D3D12 backend once per presented frame, with the backbuffer bound-able as RTV
 * and in RENDER_TARGET state. Leaves the command list with our root signature/heap bound;
 * the backend only copies/transitions afterwards and re-binds everything next frame. */

/* ===========================================================================
 * v194 SETTINGS MENU (port overlay, same look as the system dialogs).
 * Open: F10 any time; while the game is paused (g_uiov_paused_fn, stubs.cpp: the game's world clock stopped while
 * its real clock runs = pause menu) a hint "F10 / Back  Settings & Quit" is shown -- controller Back or a click on
 * the hint opens it too. Up/Down select, Left/Right (or click / right click) change, Enter/A on a row, Esc/B close.
 * Values live in settings.ini (current directory = infamous-recomp\ for every launcher); main.cpp loads it into the
 * environment at startup (it overrides the launcher). Live settings apply at once through the g_* hooks below;
 * the ones marked "restart" are read once at startup.
 * =========================================================================*/
volatile int g_ui_fps_cap = -2;      /* -2 not set (launcher F7 behaviour), 0 off, 1 auto (v209), else fps -- infamous_clock_probe.h */
volatile int g_ui_kbm_glyphs = -1;   /* -1 env, 0 controller, 1 auto, 2 keyboard/mouse -- rsx_d3d12_backend.c */
volatile int g_ui_mouse_speed = 0;   /* 0 env, else px/s for a full stick -- cellPad.c */
volatile int g_set_vsync = -1;       /* -1 env, 0/1 -- rsx_d3d12_backend.c present_sync_interval */
extern volatile int g_win_fullscreen;   /* rsx_d3d12_backend.c window proc (F11) */
extern volatile int g_fs_cutscene_open; /* runtime/ppu/ppu_fs.cpp: story-cutscene movies open right now */
int (*volatile g_uiov_paused_fn)(void) = 0;
volatile unsigned long long g_pause_menu_built_ms = 0;   /* infamous-recomp/stubs.cpp: last time the pause menu was built */
/* v195: set by the native pause-menu entries (stubs.cpp, script natives on the PPU thread): 1 = Settings, 2 = Quit Game */
volatile int g_uiov_request = 0;
/* v243K: val/name 8 -> 12 (Language has 12 choices) */
typedef struct { const char* key; const char* label; int restart; int n; const char* val[12]; const char* name[12]; int cur, saved; } MItem;
static MItem s_mi[] = {
    { "FULLSCREEN",      "Display Mode",       0, 2, { "0", "1" }, { "Windowed", "Fullscreen" } },
    { "PRESENT_VSYNC",   "VSync",              0, 2, { "0", "1" }, { "Off", "On" } },
    { "UI_FPS_CAP",      "Frame Rate Limit",   0, 6, { "0", "60", "50", "45", "40", "30" }, { "Off", "60 FPS", "50 FPS", "45 FPS", "40 FPS", "30 FPS" } },   /* v209b: Auto removed */
    { "KBM_GLYPHS",      "Button Prompts",     0, 3, { "1", "0", "2" }, { "Automatic", "Controller", "Keyboard & Mouse" } },
    { "PAD_MOUSE_SPEED", "Mouse Speed",        0, 5, { "900", "650", "450", "325", "225" }, { "1 (Slow)", "2", "3", "4", "5 (Fast)" } },
    { "PAD_RUN_DEFAULT", "Default Movement",   1, 2, { "0", "1" }, { "Walk (Shift: Run)", "Run (Shift: Walk)" } },
    { "RES_SCALE",       "Resolution",         1, 5, { "1", "1.5", "2", "2.5", "3" }, { "720p", "1080p", "1440p", "1800p", "2160p (4K)" } },
    { "SHADOWS",         "Shadows",            1, 2, { "1", "0" }, { "On", "Off" } },
    { "TEX_ANISO",       "Texture Filtering",  1, 5, { "-1", "0", "4", "8", "16" }, { "Game Default", "Bilinear", "4x Anisotropic", "8x Anisotropic", "16x Anisotropic" } },
    { "TEX_MIPS",        "Mipmaps",            1, 2, { "1", "0" }, { "On", "Off" } },
    /* v243K (from PR #25): DLSS / FSR upscaling and FSR frame generation -- rows exist only in a build made with the
     * NVIDIA DLSS SDK (PS3RECOMP_NGX) and/or the AMD FSR SDK (PS3RECOMP_FFX); rsx_dlss.cpp reads UPSCALER / FSR_FG. */
#if defined(PS3RECOMP_NGX) && defined(PS3RECOMP_FFX)
    { "UPSCALER",        "Upscaling",          1, 9, { "off", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance", "fsr_native", "fsr_quality", "fsr_balanced", "fsr_performance" },
                                                     { "Off", "NVIDIA DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance" } },
#elif defined(PS3RECOMP_NGX)
    { "UPSCALER",        "Upscaling",          1, 5, { "off", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance" },
                                                     { "Off", "NVIDIA DLAA", "DLSS Quality", "DLSS Balanced", "DLSS Performance" } },
#elif defined(PS3RECOMP_FFX)
    { "UPSCALER",        "Upscaling",          1, 5, { "off", "fsr_native", "fsr_quality", "fsr_balanced", "fsr_performance" },
                                                     { "Off", "FSR Native AA", "FSR Quality", "FSR Balanced", "FSR Performance" } },
#endif
#ifdef PS3RECOMP_FFX
    { "FSR_FG",          "Frame Generation",   1, 2, { "0", "1" }, { "Off", "On (FSR)" } },
#endif
    /* v243K (from PR #25): game language (dialogue, subtitles, HUD); cellSysutil.c + infamous_lang.h read PS3_LANG */
    { "PS3_LANG",        "Language",           1, 12, { "english", "british", "french", "german", "italian", "spanish", "dutch", "polish", "portuguese", "russian", "korean", "chinese_t" },
                                                      { "English (US)", "English (UK)", "French", "German", "Italian", "Spanish", "Dutch", "Polish", "Portuguese", "Russian", "Korean (text)", "Chinese (text)" } },
};
#define MI_N ((int)(sizeof s_mi / sizeof s_mi[0]))
enum { MR_RESUME = MI_N, MR_QUIT = MI_N + 1, MR_CONTROLS = MI_N + 2, MR_N = MI_N + 3 };
/* Keep settings in s_mi for persistence; group three input rows under Controls. */
enum { MC_FIRST = 3, MC_END = 6 };
static int s_controls_page;
static int menu_visible_rows(void)
{
    return s_controls_page ? MC_END - MC_FIRST + 1 : MI_N;
}
static int menu_item_at_row(int row)
{
    if (s_controls_page)
        return row < MC_END - MC_FIRST ? MC_FIRST + row : MR_RESUME;
    if (row < MC_FIRST) return row;
    if (row == MC_FIRST) return MR_CONTROLS;
    if (row < MI_N - (MC_END - MC_FIRST) + 1) return row + (MC_END - MC_FIRST) - 1;
    if (row == MI_N - (MC_END - MC_FIRST) + 1) return MR_RESUME;
    return MR_QUIT;
}
static int s_menu_inited, s_menu_sel, s_menu_confirm, s_menu_csel, s_restart_note;
static ULONGLONG s_menu_t_open;
static unsigned s_menu_prev;
static float s_row_y0[MR_N], s_row_y1[MR_N], s_row_x0, s_row_x1, s_hint_r[4];
static int s_hint_on, s_menu_quitonly;
static int s_wm_on; static ULONGLONG s_wm_last;   /* v238 pause watermark */
static float s_cbtn[2][4];   /* v195: confirm buttons (mouse) */
static int mi_match(const MItem* m, const char* v)
{
    if (!v || !*v) return -1;
    for (int i = 0; i < m->n; i++) {
        if (!strcmp(m->val[i], v)) return i;
        if (!strcmp(m->key, "RES_SCALE") && fabs(atof(m->val[i]) - atof(v)) < 0.01) return i;
    }
    return -1;
}
static void menu_apply(int i, HWND hwnd)
{
    const MItem* m = &s_mi[i]; const char* v = m->val[m->cur];
    _putenv_s(m->key, v);
    if (!strcmp(m->key, "FULLSCREEN")) { if (hwnd && (atoi(v) != 0) != (g_win_fullscreen != 0)) PostMessageA(hwnd, WM_KEYDOWN, VK_F11, 0); }
    else if (!strcmp(m->key, "PRESENT_VSYNC")) g_set_vsync = atoi(v);
    else if (!strcmp(m->key, "UI_FPS_CAP")) g_ui_fps_cap = atoi(v);
    else if (!strcmp(m->key, "KBM_GLYPHS")) g_ui_kbm_glyphs = atoi(v);
    else if (!strcmp(m->key, "PAD_MOUSE_SPEED")) g_ui_mouse_speed = atoi(v);
}
static void menu_save(void)
{
    FILE* f = fopen("settings.ini", "w");
    if (!f) { fprintf(stderr, "[settings] could not write settings.ini\n"); return; }
    fprintf(f, "; inFamous recomp settings (written by the in-game Settings menu, pause menu > Settings). Loaded at startup; overrides the launcher.\n");
    for (int i = 0; i < MI_N; i++) fprintf(f, "%s=%s\n", s_mi[i].key, s_mi[i].val[s_mi[i].cur]);
    fclose(f);
    fprintf(stderr, "[settings] saved settings.ini\n");
}
static void menu_init(HWND hwnd)
{
    s_menu_inited = 1;
    for (int i = 0; i < MI_N; i++) {
        MItem* m = &s_mi[i];
        int k = mi_match(m, getenv(m->key));
        if (k < 0) {   /* not set: show the built-in default */
            if (!strcmp(m->key, "PAD_MOUSE_SPEED")) k = 2;
            else if (!strcmp(m->key, "TEX_ANISO")) k = 0;
            else k = 0;
        }
        m->cur = m->saved = k;
    }
    /* live settings from settings.ini take effect now (restart ones were read at startup) */
    { FILE* f = fopen("settings.ini", "r");
      if (f) { fclose(f);
          for (int i = 0; i < MI_N; i++) if (!s_mi[i].restart) menu_apply(i, hwnd); } }
}
static void menu_open(void)
{
    s_menu_open = 1; g_uiov_menu_open = 1; s_controls_page = 0; s_menu_sel = 0; s_menu_confirm = 0; s_menu_t_open = GetTickCount64();
    s_menu_prev = 0xFFFFFFFFu;
    fprintf(stderr, "[settings] menu open\n");
}
static void menu_close(void)
{
    s_menu_open = 0; g_uiov_menu_open = 0; s_menu_close_ms = GetTickCount64(); s_menu_need_clear = 1; s_menu_quitonly = 0;
    int dirty = 0; for (int i = 0; i < MI_N; i++) if (s_mi[i].cur != s_mi[i].saved) dirty = 1;
    if (dirty) { menu_save(); for (int i = 0; i < MI_N; i++) s_mi[i].saved = s_mi[i].cur; }
    fprintf(stderr, "[settings] menu closed\n");
}
static void menu_change(int i, int d, HWND hwnd)
{
    MItem* m = &s_mi[i];
    m->cur = (m->cur + d + m->n) % m->n;
    menu_apply(i, hwnd);
    if (m->restart) s_restart_note = 1;
    fprintf(stderr, "[settings] %s = %s%s\n", m->key, m->val[m->cur], m->restart ? " (after restart)" : "");
}
/* virtual-px mouse position; 0 if the cursor is not over the game window */
static int mouse_virt(HWND hwnd, unsigned bbw, unsigned bbh, float* vx, float* vy)
{
    POINT pt; RECT rc;
    if (!hwnd || GetForegroundWindow() != hwnd || !GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc) || rc.bottom <= 0) return 0;
    if (pt.x < 0 || pt.y < 0 || pt.x >= rc.right || pt.y >= rc.bottom) return 0;
    *vx = pt.x * (UIOV_VW_H / (float)rc.bottom) * ((float)rc.bottom / rc.right) * ((float)bbw / bbh);
    *vy = pt.y * (UIOV_VW_H / (float)rc.bottom);
    return 1;
}
/* returns 1 when the menu (or the hint) wants to draw this frame */
static int menu_tick(HWND hwnd, unsigned bbw, unsigned bbh)
{
    ULONGLONG now = GetTickCount64();
    if (!s_menu_inited) menu_init(hwnd);
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    static int f10_was, sel_was, lb_was, rb_was;
    const int f10 = 0;   /* F10 = trainer page now; Settings opens from the pause menu (g_uiov_request) */
    const int paused = g_uiov_paused_fn ? g_uiov_paused_fn() : 0;
    if (!s_menu_open) {
        int open = f10 && !f10_was;
        /* v195: the pause menu's own "Settings" / "Quit Game" entries (native, stubs.cpp) ask for us */
        const int req = g_uiov_request;
        if (req) { g_uiov_request = 0; if (!D.active) open = req; }
        /* v195: the old "F10 / Back" hint is gone (UI_PAUSE_HINT=1 brings it back) */
        static int hint_env = -1;
        if (hint_env < 0) { const char* e = getenv("UI_PAUSE_HINT"); hint_env = e && *e == '1'; }
        s_hint_on = hint_env && paused && !D.active;
        /* v238 pause-screen watermark, only on the real pause menu. "paused" (world clock stopped) is also true
         * in cutscenes, so it is shown only if the pause menu was built shortly before or during this pause. */
        { static int was_paused; static ULONGLONG t_pause;
          if (paused && !was_paused) t_pause = now;
          was_paused = paused;
          const ULONGLONG built = g_pause_menu_built_ms;
          s_wm_on = paused && built && built + 2000 >= t_pause && !D.active && !g_fs_cutscene_open; }
        if (s_hint_on) {
            unsigned in = read_input(hwnd);   /* only polled while paused (XInput on empty slots is slow) */
            if ((in & IN_SELECT) && !sel_was) open = 1;
            sel_was = (in & IN_SELECT) != 0;
            const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
            float vx, vy;
            if (lb && !lb_was && mouse_virt(hwnd, bbw, bbh, &vx, &vy) &&
                vx >= s_hint_r[0] && vx <= s_hint_r[2] && vy >= s_hint_r[1] && vy <= s_hint_r[3]) open = 1;
            lb_was = lb;
        } else sel_was = 0;
        f10_was = f10;
        if (open && !D.active) {
            menu_open();
            if (open == 2) { s_menu_quitonly = 1; s_menu_confirm = 1; s_menu_csel = 1; }
        }
        else return s_hint_on || s_wm_on;
    }
    /* ---- menu input ---- */
    unsigned in = read_input(hwnd);
    if (s_menu_prev == 0xFFFFFFFFu) s_menu_prev = in;   /* keys held at open are ignored */
    unsigned dn = in & ~s_menu_prev; s_menu_prev = in;
    if (f10 && !f10_was) { f10_was = f10; menu_close(); return 0; }
    f10_was = f10;
    if (now - s_menu_t_open < 150) dn = 0;
    if (s_menu_confirm) {
        if (dn & (IN_LEFT | IN_RIGHT | IN_UP | IN_DOWN | IN_TAB)) s_menu_csel ^= 1;
        if (dn & IN_BACK) { s_menu_confirm = 0; if (s_menu_quitonly) { menu_close(); return 0; } }
        { /* v195: mouse on the confirm buttons: hover selects, click activates */
          static int clb_was; float vx, vy;
          const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
          if (mouse_virt(hwnd, bbw, bbh, &vx, &vy))
              for (int i = 0; i < 2; i++)
                  if (vx >= s_cbtn[i][0] && vx <= s_cbtn[i][2] && vy >= s_cbtn[i][1] && vy <= s_cbtn[i][3]) {
                      s_menu_csel = i; if (lb && !clb_was) dn |= IN_OK; }
          clb_was = lb; }
        if (dn & IN_OK) {
            if (s_menu_csel == 0) {   /* Quit: the v189c window-close path ends the process */
                fprintf(stderr, "[settings] Quit Game\n");
                menu_close();
                if (hwnd) PostMessageA(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
            s_menu_confirm = 0;
            if (s_menu_quitonly) { menu_close(); return 0; }
        }
    } else {
        const int rows = menu_visible_rows();
        if (dn & IN_UP)   s_menu_sel = (s_menu_sel + rows - 1) % rows;
        if (dn & IN_DOWN) s_menu_sel = (s_menu_sel + 1) % rows;
        const int item = menu_item_at_row(s_menu_sel);
        if (item < MI_N) {
            if (dn & IN_LEFT)  menu_change(item, -1, hwnd);
            if (dn & (IN_RIGHT | IN_OK)) menu_change(item, +1, hwnd);
        } else if (dn & (IN_RIGHT | IN_OK)) {
            if (item == MR_CONTROLS) { s_controls_page = 1; s_menu_sel = 0; }
            else if (item == MR_RESUME) {
                if (s_controls_page) { s_controls_page = 0; s_menu_sel = MC_FIRST; }
                else { menu_close(); return 0; }
            }
            else if (item == MR_QUIT && (dn & IN_OK)) { s_menu_confirm = 1; s_menu_csel = 1; }
        }
        if (dn & IN_BACK) {
            if (s_controls_page) { s_controls_page = 0; s_menu_sel = MC_FIRST; }
            else { menu_close(); return 0; }
        }
        if (dn & IN_SELECT) { menu_close(); return 0; }
    }
    /* mouse: hover selects, left click = next value / activate, right click = previous value */
    { float vx, vy; static POINT s_last;
      const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), rb = fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
      if (!s_menu_confirm && mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_row_x0 && vx <= s_row_x1) {
          POINT pt; GetCursorPos(&pt);
          for (int r = 0; r < menu_visible_rows(); r++) if (vy >= s_row_y0[r] && vy <= s_row_y1[r]) {
              if (pt.x != s_last.x || pt.y != s_last.y) s_menu_sel = r;
              const int item = menu_item_at_row(r);
              if (lb && !lb_was) {
                  if (item < MI_N) menu_change(item, +1, hwnd);
                  else if (item == MR_CONTROLS) {
                      s_controls_page = 1; s_menu_sel = 0;
                      s_last = pt; lb_was = lb; return 1;
                  } else if (item == MR_RESUME) {
                      if (s_controls_page) {
                          s_controls_page = 0; s_menu_sel = MC_FIRST;
                          s_last = pt; lb_was = lb; return 1;
                      }
                      s_last = pt; lb_was = lb; menu_close(); return 0;
                  } else { s_menu_confirm = 1; s_menu_csel = 1; }
              }
              if (rb && !rb_was && item < MI_N) menu_change(item, -1, hwnd);
          }
          s_last = pt;
      } else if (s_menu_confirm && lb && !lb_was) { s_menu_csel ^= 0; }
      lb_was = lb; rb_was = rb; }
    return 1;
}
/* v238 PAUSE WATERMARK: faint centred title in the pause screen's empty top black bar (the game draws nothing there).
 * UI_WATERMARK=<text> overrides it, UI_WATERMARK=0 turns it off; UI_WATERMARK_ALPHA (default 0.22). Fades in 0.3 s. */
static void build_watermark(void)
{
    static const char* txt = NULL; static float a_max = 0.22f;
    if (!txt) { const char* e = getenv("UI_WATERMARK"); txt = (e && *e) ? e : "Infamous Reborn";
                if (!strcmp(txt, "0")) txt = "";
                e = getenv("UI_WATERMARK_ALPHA"); if (e && *e) a_max = (float)atof(e);
                if (a_max < 0.0f) a_max = 0.0f; if (a_max > 1.0f) a_max = 1.0f; }
    if (!*txt) return;
    static ULONGLONG t_on = 0; const ULONGLONG now = GetTickCount64();
    if (!t_on || now - s_wm_last > 200) t_on = now;   /* new pause -> restart the fade */
    s_wm_last = now;
    float k = (float)(now - t_on) / 300.0f; if (k > 1.0f) k = 1.0f;
    const float sz = 34.0f, w = text_width(txt, (int)strlen(txt), sz);
    float c[4]; rgba(c, 1.0f, 1.0f, 1.0f, a_max * k);
    text_draw(txt, (int)strlen(txt), (s_vw - w) * 0.5f, 42.0f - sz * 0.62f, sz, c);   /* bar = top ~80 of 720 */
}
static void build_hint(void)
{
    const char* t = "Back   Settings & Quit";
    float sz = 22.0f, w = text_width(t, (int)strlen(t), sz), x1 = s_vw - 40.0f, x0 = x1 - w - 32.0f, y0 = 30.0f, y1 = y0 + 40.0f;   /* top-right: the game's pause screen keeps that black bar empty */
    float c0[4], c1[4], wt[4];
    rgba(c0, 0, 0, 0, 0.55f); rect(x0, y0, x1, y1, c0, c0, 0);
    rgba(c1, 0.36f, 0.72f, 1.0f, 0.9f); rect(x0, y1 - 2, x1, y1, c1, c1, 0);
    rgba(wt, 0.95f, 0.95f, 0.95f, 1.0f);
    text_draw(t, (int)strlen(t), x0 + 16.0f, y0 + 20.0f - sz * 0.62f, sz, wt);
    s_hint_r[0] = x0; s_hint_r[1] = y0; s_hint_r[2] = x1; s_hint_r[3] = y1;
}
static void build_menu(void)
{
    static const float BLUE[3] = { 0.36f, 0.72f, 1.0f };
    /* v243K: rows shrink when the list grows (720-unit virtual height): 40 units / 26 pt up to 11 rows */
    const int rows = menu_visible_rows();
    const float rh = rows <= 11 ? 40.0f : (376.0f / (float)rows > 30.0f ? 376.0f / (float)rows : 30.0f);
    const float cx = s_vw * 0.5f, lsz = rh * 0.65f;
    float c0[4], c1[4], wt[4], gr[4], bl[4];
    if (s_menu_quitonly) {   /* v195: "Quit Game" from the pause menu: only the confirmation */
        rgba(wt, 0.97f, 0.97f, 0.97f, 1); rgba(gr, 0.60f, 0.60f, 0.60f, 1); rgba(bl, BLUE[0], BLUE[1], BLUE[2], 1);
        goto confirm;
    }
    rgba(c0, 0, 0, 0, 0.55f); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);
    const float top = 92.0f, bot = UIOV_VW_H - 70.0f, fade = 30.0f;
    rgba(c0, 0, 0, 0, 0); rgba(c1, 0, 0, 0, 0.86f);
    rect(0, top - fade, s_vw, top, c0, c1, 0); rect(0, top, s_vw, bot, c1, c1, 0); rect(0, bot, s_vw, bot + fade, c1, c0, 0);
    float lx0 = cx - 520.0f, lx1 = cx + 520.0f, ys[2] = { top, bot - 2.0f };
    for (int i = 0; i < 2; i++) {
        rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0); rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.95f);
        rect(lx0, ys[i], cx, ys[i] + 2, c0, c1, 1); rect(cx, ys[i], lx1, ys[i] + 2, c1, c0, 1);
    }
    rgba(wt, 0.97f, 0.97f, 0.97f, 1); rgba(gr, 0.60f, 0.60f, 0.60f, 1); rgba(bl, BLUE[0], BLUE[1], BLUE[2], 1);
    const char* title = s_controls_page ? "CONTROLS" : "SETTINGS";
    const int title_len = (int)strlen(title);
    text_draw(title, title_len, cx - text_width(title, title_len, 38.0f) * 0.5f, top + 14.0f, 38.0f, wt);
    const float x0 = cx - 400.0f, x1 = cx + 400.0f;
    s_row_x0 = x0 - 20.0f; s_row_x1 = x1 + 20.0f;
    float y = top + (rows <= 11 ? 72.0f : 62.0f);
    for (int r = 0; r < rows; r++) {
        const int item = menu_item_at_row(r);
        if (item == MR_RESUME) y += rows <= 11 ? 14.0f : 6.0f;
        const int on = (r == s_menu_sel) && !s_menu_confirm;
        s_row_y0[r] = y - 4.0f; s_row_y1[r] = y + rh - 4.0f;
        if (on) { rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0.0f); rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.22f);
                  rect(x0 - 20.0f, y - 4.0f, cx, y + rh - 6.0f, c0, c1, 1); rect(cx, y - 4.0f, x1 + 20.0f, y + rh - 6.0f, c1, c0, 1);
                  rect(x0 - 20.0f, y + rh - 6.0f, x1 + 20.0f, y + rh - 4.0f, bl, bl, 0); }
        const float* col = on ? wt : gr;
        if (item < MI_N) {
            const MItem* m = &s_mi[item];
            text_draw(m->label, (int)strlen(m->label), x0, y, lsz, col);
            if (m->restart) { const char* rs = "(restart)"; float lw = text_width(m->label, (int)strlen(m->label), lsz);
                              float g2[4]; rgba(g2, 0.45f, 0.45f, 0.45f, 1); text_draw(rs, 9, x0 + lw + 12.0f, y + rh * 0.15f, lsz * 0.7f, g2); }   /* v243K: scales with the row */
            char v[96]; snprintf(v, sizeof v, "%s   %s   %s", on ? "<" : " ", m->name[m->cur], on ? ">" : " ");
            text_draw(v, (int)strlen(v), x1 - text_width(v, (int)strlen(v), lsz), y, lsz, on ? wt : gr);
        } else {
            const char* t = item == MR_CONTROLS ? "Controls  >" :
                            item == MR_RESUME ? (s_controls_page ? "Back to Settings" : "Back") : "Quit Game";
            text_draw(t, (int)strlen(t), x0, y, lsz, col);
        }
        y += rh;
    }
    const char* foot = s_menu_confirm ? "" :
        s_controls_page ? "Up/Down select    Left/Right or click change    Esc / B back" :
                          "Up/Down select    Left/Right or click change    Esc / B close";
    if (*foot) text_draw(foot, (int)strlen(foot), cx - text_width(foot, (int)strlen(foot), 19.0f) * 0.5f, bot - 34.0f, 19.0f, gr);
    if (s_restart_note) { const char* n = "Settings marked (restart) apply the next time the game starts.";
        float y2[4]; rgba(y2, 1.0f, 0.82f, 0.35f, 1); text_draw(n, (int)strlen(n), cx - text_width(n, (int)strlen(n), 19.0f) * 0.5f, bot - 62.0f, 19.0f, y2); }
confirm:
    if (s_menu_confirm) {
        const float lx0 = cx - 520.0f, lx1 = cx + 520.0f;
        rgba(c0, 0, 0, 0, 0.6f); rect(0, 0, s_vw, UIOV_VW_H, c0, c0, 0);
        const float by0 = UIOV_VW_H * 0.5f - 80.0f, by1 = UIOV_VW_H * 0.5f + 80.0f;
        rgba(c1, 0.02f, 0.02f, 0.03f, 0.95f); rect(0, by0, s_vw, by1, c1, c1, 0);
        rect(lx0, by0, lx1, by0 + 2, bl, bl, 0); rect(lx0, by1 - 2, lx1, by1, bl, bl, 0);
        const char* q = "Quit the game? Unsaved progress will be lost.";
        text_draw(q, (int)strlen(q), cx - text_width(q, (int)strlen(q), 28.0f) * 0.5f, by0 + 26.0f, 28.0f, wt);
        const char* yn[2] = { "Quit", "Cancel" };
        for (int i = 0; i < 2; i++) {
            float bx = cx + (i ? 60.0f : -60.0f - text_width(yn[0], 4, 28.0f));
            const float* cc = (i == s_menu_csel) ? wt : gr;
            text_draw(yn[i], (int)strlen(yn[i]), bx, by1 - 62.0f, 28.0f, cc);
            s_cbtn[i][0] = bx - 16; s_cbtn[i][1] = by1 - 70.0f; s_cbtn[i][2] = bx + text_width(yn[i], (int)strlen(yn[i]), 28.0f) + 16; s_cbtn[i][3] = by1 - 18.0f;
            if (i == s_menu_csel) rect(bx - 10, by1 - 26.0f, bx + text_width(yn[i], (int)strlen(yn[i]), 28.0f) + 10, by1 - 24.0f, bl, bl, 0);
        }
    }
}
/* ---------------- F10 trainer page (rsx_ui_trainer.h) ----------------
 * The title supplies the rows (g_uiov_trainer); this draws them in a left-side panel so the game stays visible, owns
 * the pad while open (uiov_pad_gate) and turns input into change() calls.  While closed it draws the title's short
 * "active cheats" line (hud) at the top centre, except on the pause screen. */
#define TR_MAX_ROWS 64
static int s_tr_sel = -1, s_tr_scroll;
static unsigned s_tr_prev, s_tr_rep_key;
static ULONGLONG s_tr_t_open, s_tr_rep_t;
static float s_tr_y0[TR_MAX_ROWS], s_tr_y1[TR_MAX_ROWS], s_tr_x0, s_tr_x1;   /* last layout, for the mouse */
static int s_tr_hud_on; static char s_tr_hud[128];
static int tr_row(const uiov_trainer_api* t, int i, uiov_trainer_row* r)
{
    memset(r, 0, sizeof *r);
    return t->row(i, r);
}
static int tr_selectable(const uiov_trainer_api* t, int i)
{
    uiov_trainer_row r;
    return tr_row(t, i, &r) && r.kind != UIOV_TR_HEADER;
}
static void tr_move(const uiov_trainer_api* t, int n, int d)
{
    if (n <= 0) { s_tr_sel = -1; return; }
    int i = s_tr_sel < 0 ? (d > 0 ? -1 : 0) : s_tr_sel;
    for (int k = 0; k < n; k++) { i = (i + d + n) % n; if (tr_selectable(t, i)) { s_tr_sel = i; return; } }
    s_tr_sel = -1;
}
static void tr_open(void)
{
    s_tr_open = 1; g_uiov_menu_open = 1; s_tr_t_open = GetTickCount64(); s_tr_prev = 0xFFFFFFFFu; s_tr_rep_key = 0;
    fprintf(stderr, "[trainer] open\n");
}
static void tr_close(void)
{
    s_tr_open = 0; g_uiov_menu_open = 0; s_tr_close_ms = GetTickCount64(); s_tr_need_clear = 1;
    fprintf(stderr, "[trainer] closed\n");
}
/* returns 1 when the page (or the active-cheat line) wants to draw this frame */
static int tr_tick(HWND hwnd, unsigned bbw, unsigned bbh)
{
    const uiov_trainer_api* t = g_uiov_trainer;
    static int f10_was, lb_was, rb_was;
    if (!t) { s_tr_hud_on = 0; if (s_tr_open) tr_close(); return 0; }
    const int fg = hwnd && GetForegroundWindow() == hwnd;
    const int f10 = fg && (GetAsyncKeyState(VK_F10) & 0x8000);
    const int press = f10 && !f10_was;
    f10_was = f10;
    if (!s_tr_open) {
        if (press && !D.active && !s_menu_open) tr_open();
        else { s_tr_hud_on = t->hud ? t->hud(s_tr_hud, sizeof s_tr_hud) : 0; return s_tr_hud_on; }
    } else if (press) { tr_close(); return 0; }
    s_tr_hud_on = 0;
    int n = t->count ? t->count() : 0; if (n > TR_MAX_ROWS) n = TR_MAX_ROWS;
    if (s_tr_sel < 0 || s_tr_sel >= n || !tr_selectable(t, s_tr_sel)) { s_tr_sel = -1; tr_move(t, n, +1); }
    /* keys / pad */
    const ULONGLONG now = GetTickCount64();
    unsigned in = read_input(hwnd);
    if (s_tr_prev == 0xFFFFFFFFu) s_tr_prev = in;   /* keys held at open are ignored */
    unsigned dn = in & ~s_tr_prev; s_tr_prev = in;
    if (now - s_tr_t_open < 150) dn = 0;
    { const unsigned dirs = in & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);   /* held direction auto-repeat */
      if (dn & dirs) { s_tr_rep_key = dn & dirs; s_tr_rep_t = now + 350; }
      else if (dirs & s_tr_rep_key) { if (now >= s_tr_rep_t) { dn |= dirs & s_tr_rep_key; s_tr_rep_t = now + 80; } }
      else s_tr_rep_key = 0; }
    if (dn & IN_UP)   tr_move(t, n, -1);
    if (dn & IN_DOWN) tr_move(t, n, +1);
    if (s_tr_sel >= 0) {
        uiov_trainer_row r;
        if (tr_row(t, s_tr_sel, &r)) {
            if (r.kind == UIOV_TR_CHOICE) {
                if (dn & IN_LEFT) t->change(s_tr_sel, -1);
                if (dn & (IN_RIGHT | IN_OK)) t->change(s_tr_sel, +1);
            } else if (r.kind == UIOV_TR_ACTION && (dn & IN_OK)) t->change(s_tr_sel, 0);
        }
    }
    if (dn & (IN_BACK | IN_SELECT)) { tr_close(); return 0; }
    /* mouse: hover selects, left click = next value / run, right click = previous value */
    { float vx, vy; static POINT s_last;
      const int lb = fg && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), rb = fg && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
      if (mouse_virt(hwnd, bbw, bbh, &vx, &vy) && vx >= s_tr_x0 && vx <= s_tr_x1) {
          POINT pt; GetCursorPos(&pt);
          for (int i = 0; i < n; i++) {
              if (!(s_tr_y1[i] > s_tr_y0[i] && vy >= s_tr_y0[i] && vy <= s_tr_y1[i])) continue;
              uiov_trainer_row r;
              if (!tr_row(t, i, &r) || r.kind == UIOV_TR_HEADER) continue;
              if (pt.x != s_last.x || pt.y != s_last.y) s_tr_sel = i;
              if (lb && !lb_was) { if (r.kind == UIOV_TR_CHOICE) t->change(i, +1); else if (r.kind == UIOV_TR_ACTION) t->change(i, 0); }
              if (rb && !rb_was && r.kind == UIOV_TR_CHOICE) t->change(i, -1);
          }
          s_last = pt;
      }
      lb_was = lb; rb_was = rb; }
    return 1;
}
static void build_trainer(void)
{
    const uiov_trainer_api* t = g_uiov_trainer;
    if (!t) return;
    static const float BLUE[3] = { 0.36f, 0.72f, 1.0f };
    float c0[4], c1[4], wt[4], gr[4], dim[4], bl[4], grn[4];
    rgba(wt, 0.97f, 0.97f, 0.97f, 1); rgba(gr, 0.62f, 0.62f, 0.62f, 1); rgba(dim, 0.40f, 0.40f, 0.40f, 1);
    rgba(bl, BLUE[0], BLUE[1], BLUE[2], 1); rgba(grn, 0.55f, 1.0f, 0.55f, 1);
    const float px0 = 32.0f, px1 = px0 + 600.0f, top = 36.0f, bot = UIOV_VW_H - 36.0f;
    rgba(c0, 0, 0, 0, 0.84f); rgba(c1, 0, 0, 0, 0.0f);
    rect(px0, top, px1, bot, c0, c0, 0);
    rect(px1, top, px1 + 48.0f, bot, c0, c1, 1);
    rect(px0, top, px1, top + 2, bl, bl, 0);
    rect(px0, bot - 2, px1, bot, bl, bl, 0);
    const float x0 = px0 + 30.0f, x1 = px1 - 30.0f;
    const char* title = t->title ? t->title : "TRAINER";
    text_draw(title, (int)strlen(title), x0, top + 16.0f, 34.0f, wt);
    float y = top + 62.0f;
    { char st[128]; if (t->status && t->status(st, sizeof st)) text_draw(st, (int)strlen(st), x0, y, 19.0f, bl); }
    y += 36.0f;
    const float rh = 31.0f, lsz = 21.0f, list_bot = bot - 44.0f;
    s_tr_x0 = px0; s_tr_x1 = px1;
    int n = t->count ? t->count() : 0; if (n > TR_MAX_ROWS) n = TR_MAX_ROWS;
    int vis = (int)((list_bot - y) / rh); if (vis < 1) vis = 1;
    if (s_tr_sel >= 0) {
        if (s_tr_sel < s_tr_scroll) s_tr_scroll = s_tr_sel;
        if (s_tr_sel >= s_tr_scroll + vis) s_tr_scroll = s_tr_sel - vis + 1;
        if (s_tr_scroll > 0 && s_tr_sel == s_tr_scroll) {   /* keep a section title above the selection visible */
            uiov_trainer_row h; if (tr_row(t, s_tr_scroll - 1, &h) && h.kind == UIOV_TR_HEADER) s_tr_scroll--; }
    }
    if (s_tr_scroll > n - vis) s_tr_scroll = n - vis > 0 ? n - vis : 0;
    for (int i = 0; i < TR_MAX_ROWS; i++) s_tr_y0[i] = s_tr_y1[i] = 0.0f;
    if (s_tr_scroll > 0) text_draw("...", 3, x1 - text_width("...", 3, lsz), y - 26.0f, lsz, gr);
    for (int i = s_tr_scroll; i < n && i < s_tr_scroll + vis; i++, y += rh) {
        uiov_trainer_row r;
        if (!tr_row(t, i, &r)) continue;
        if (r.kind == UIOV_TR_HEADER) {
            text_draw(r.label, (int)strlen(r.label), x0, y + 8.0f, 17.0f, bl);
            rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0.30f); rect(x0, y + rh - 5.0f, x1, y + rh - 4.0f, c0, c0, 0);
            continue;
        }
        const int on = i == s_tr_sel, locked = r.kind == UIOV_TR_LOCKED;
        s_tr_y0[i] = y - 4.0f; s_tr_y1[i] = y + rh - 4.0f;
        if (on) {
            rgba(c0, BLUE[0], BLUE[1], BLUE[2], 0.0f); rgba(c1, BLUE[0], BLUE[1], BLUE[2], 0.26f);
            rect(x0 - 16.0f, y - 4.0f, x1 + 16.0f, y + rh - 6.0f, c1, c0, 1);
            rect(x0 - 16.0f, y - 4.0f, x0 - 12.0f, y + rh - 6.0f, bl, bl, 0);
        }
        text_draw(r.label, (int)strlen(r.label), x0, y, lsz, locked ? dim : on ? wt : gr);
        char v[96];
        if (r.kind == UIOV_TR_CHOICE) snprintf(v, sizeof v, "%s  %s  %s", on ? "<" : " ", r.value, on ? ">" : " ");
        else snprintf(v, sizeof v, "%s", r.value);
        if (*v) {
            const float vsz = locked ? lsz * 0.78f : lsz;
            const float* vc = locked ? dim : (r.kind == UIOV_TR_CHOICE && r.on) ? grn : on ? wt : gr;
            text_draw(v, (int)strlen(v), x1 - text_width(v, (int)strlen(v), vsz), y + (lsz - vsz) * 0.5f, vsz, vc);
        }
    }
    if (s_tr_scroll + vis < n) text_draw("...", 3, x1 - text_width("...", 3, lsz), y - 8.0f, lsz, gr);
    const char* foot = "Up/Down select   Left/Right change   Enter run   F10/Esc close";
    text_draw(foot, (int)strlen(foot), x0, bot - 32.0f, 15.0f, gr);
}
static void tr_hud_build(void)
{
    if (!s_tr_hud_on || !s_tr_hud[0]) return;
    const int len = (int)strlen(s_tr_hud);
    const float sz = 17.0f, w = text_width(s_tr_hud, len, sz), x0 = (s_vw - w) * 0.5f - 12.0f, y0 = 6.0f;
    float bg[4], fg[4];
    rgba(bg, 0, 0, 0, 0.45f); rgba(fg, 0.55f, 1.0f, 0.55f, 0.92f);
    rect(x0, y0, x0 + w + 24.0f, y0 + 28.0f, bg, bg, 0);
    text_draw(s_tr_hud, len, x0 + 12.0f, y0 + 14.0f - sz * 0.62f, sz, fg);
}
static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh);
/* v212 F1 FPS counter: frames this overlay was drawn for (= presented guest frames), averaged over 0.5 s, top-left. */
volatile int g_uiov_show_fps = 0;
static double s_fps_val = 0.0;
static void fps_tick(void)
{
    static LARGE_INTEGER f, t0; static unsigned n = 0;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); t0 = t; }
    n++;
    const double dt = (double)(t.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    if (dt >= 0.5) { s_fps_val = n / dt; n = 0; t0 = t; }
}
static void fps_build(void)
{
    if (!g_uiov_show_fps) return;
    char t[32]; snprintf(t, sizeof t, "%.0f FPS", s_fps_val);
    const float sz = 26.0f, w = text_width(t, (int)strlen(t), sz);
    float bg[4], fg[4];
    rgba(bg, 0, 0, 0, 0.55f); rect(16.0f, 16.0f, 16.0f + w + 24.0f, 16.0f + 40.0f, bg, bg, 0);
    if (s_fps_val >= 58.0) rgba(fg, 0.55f, 1.0f, 0.55f, 1); else if (s_fps_val >= 45.0) rgba(fg, 1.0f, 0.9f, 0.4f, 1); else rgba(fg, 1.0f, 0.5f, 0.45f, 1);
    text_draw(t, (int)strlen(t), 28.0f, 16.0f + 20.0f - sz * 0.62f, sz, fg);
}
static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh);
static void fps_only(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh, DXGI_FORMAT rtfmt)
{
    if (!g_uiov_show_fps || !font_load() || !gfx_init(dev, rtfmt)) return;
    s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
    s_vbi = (s_vbi + 1) % UIOV_FRAMES;
    s_v = s_vbmap[s_vbi]; s_nv = 0;
    fps_build();
    if (s_nv) uiov_draw_list(cl, rtv, bbw, bbh);
}
void uiov_render(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv,
                 unsigned bbw, unsigned bbh, DXGI_FORMAT rtfmt, HWND hwnd)
{
    fps_tick();   /* v212 */
    /* v194: settings menu / pause hint (when no system dialog is up) */
    if (!D.active && font_load()) {
        s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
        int want = s_menu_open ? 0 : tr_tick(hwnd, bbw, bbh);   /* F10 trainer page / active-cheat line */
        if (!s_tr_open) want |= menu_tick(hwnd, bbw, bbh);
        if (want && gfx_init(dev, rtfmt)) {
            s_last_draw_ms = GetTickCount64();
            s_vbi = (s_vbi + 1) % UIOV_FRAMES;
            s_v = s_vbmap[s_vbi]; s_nv = 0;
            if (s_tr_open) build_trainer();
            else if (s_menu_open) build_menu();
            else { if (s_hint_on) build_hint(); if (s_wm_on) build_watermark(); else tr_hud_build(); }   /* v238 */
            fps_build();   /* v212 */
            if (s_nv) uiov_draw_list(cl, rtv, bbw, bbh);
            return;
        }
    }
    ULONGLONG now = GetTickCount64();
    s_last_draw_ms = now;
    if (s_cs_ready != 2) { fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }   /* no dialog was ever opened */
    lock();
    int active = D.active;
    float a_close = 0.0f;
    if (!active && D.t_close && now - D.t_close < 140) a_close = D.fade_close * (1.0f - (now - D.t_close) / 140.0f);
    /* v168: nothing on screen and no answering key to wait for -> no input polling (XInputGetState on empty
     * slots is slow and this runs every frame). */
    if (!active && !D.held && a_close <= 0.0f) { unlock(); fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }
    /* input */
    unsigned in = read_input(hwnd);
    /* v168 FIX: 'held' only covers the key that ANSWERED the dialog, until it is released (v164 re-armed it on
     * every later Space/C/X/Z/Enter/Esc press after any dialog -> the game saw a neutral pad: no jump, WASD dropped). */
    if (!active && D.held && (!(in & (IN_OK | IN_BACK)) || now - D.t_close > 3000)) D.held = 0;
    BtnRect br[2]; int nb = 0;
    if (active && now - D.t_open > 200) {
        unsigned prev = D.prev == 0xFFFFFFFFu ? in : D.prev;
        unsigned dn = in & ~prev;
        D.prev = in;
        if (D.kind == 2 && (dn & (IN_LEFT | IN_RIGHT | IN_TAB))) D.sel ^= 1;
        int answer = 0;
        if (dn & IN_OK)   answer = (D.kind == 2 && D.sel == 1) ? 2 : 1;
        if (dn & IN_BACK) answer = D.kind == 2 ? 2 : 1;
        /* mouse: hover selects, click answers (positions from the last frame's layout) */
        static int s_lbtn;
        int lb = (hwnd && GetForegroundWindow() == hwnd && (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) ? 1 : 0;
        POINT pt; RECT rc;
        if (hwnd && s_nb && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc) && rc.bottom > 0) {
            float vx = pt.x * (UIOV_VW_H / (float)rc.bottom), vy = pt.y * (UIOV_VW_H / (float)rc.bottom);
            vx *= ((float)rc.bottom / rc.right) * ((float)bbw / bbh);   /* client aspect may differ from backbuffer */
            for (int i = 0; i < s_nb; i++)
                if (vx >= s_br[i].x0 && vx <= s_br[i].x1 && vy >= s_br[i].y0 && vy <= s_br[i].y1) {
                    static POINT s_last; if (pt.x != s_last.x || pt.y != s_last.y) D.sel = i;
                    s_last = pt;
                    if (lb && !s_lbtn) answer = (D.kind == 2 && i == 1) ? 2 : 1;
                }
        }
        s_lbtn = lb;
        if (answer) {
            D.result = answer; D.active = 0; D.t_close = now; D.need_clear = 1; D.held = 1;
            D.fade_close = 1.0f; memcpy(D.last_msg, D.msg, sizeof D.msg); D.last_kind = D.kind; D.last_sel = answer == 2 && D.kind == 2 ? 1 : 0;
            fprintf(stderr, "[uiov] dialog answered: %s\n", answer == 1 ? (D.kind == 2 ? "YES" : "OK") : "NO");
            active = 0; a_close = 1.0f;
        }
        if (!active) s_nb = 0;
    } else if (active) {
        D.prev = D.prev == 0xFFFFFFFFu ? in : D.prev;
    }
    if (!active && a_close <= 0.0f) { unlock(); fps_only(dev, cl, rtv, bbw, bbh, rtfmt); return; }
    char msg[2048]; int kind, sel; float alpha, grow = 1.0f;
    if (active) {
        memcpy(msg, D.msg, sizeof msg); kind = D.kind; sel = D.sel;
        float t = (now - D.t_open) / 180.0f; if (t > 1) t = 1;
        alpha = 1.0f - (1.0f - t) * (1.0f - t);
        grow = 0.85f + 0.15f * alpha; if (t >= 1) grow = 1.0f;
    } else { memcpy(msg, D.last_msg, sizeof msg); kind = D.last_kind; sel = D.last_sel; alpha = a_close; }
    unlock();

    if (!font_load() || !gfx_init(dev, rtfmt)) return;
    s_vbi = (s_vbi + 1) % UIOV_FRAMES;
    s_v = s_vbmap[s_vbi]; s_nv = 0;
    s_k = bbh / UIOV_VW_H; s_vw = bbw / s_k;
    nb = build_dialog(msg, kind, sel, alpha, grow, br);
    s_nb = active ? nb : 0;                      /* button rects for next frame's mouse test */
    if (s_nb) memcpy(s_br, br, sizeof(BtnRect) * s_nb);
    fps_build();   /* v212 */
    if (!s_nv) return;
    uiov_draw_list(cl, rtv, bbw, bbh);
}
static void uiov_draw_list(ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE rtv, unsigned bbw, unsigned bbh)
{
    if (!s_tex_uploaded) {
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource = s_tex_up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = s_aw; src.PlacedFootprint.Footprint.Height = s_ah;
        src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = s_aw * 4;
        cl->lpVtbl->CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = s_tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->lpVtbl->ResourceBarrier(cl, 1, &b);
        s_tex_uploaded = 1;
    }
    D3D12_VIEWPORT vp = { 0, 0, (float)bbw, (float)bbh, 0, 1 };
    D3D12_RECT sc = { 0, 0, (LONG)bbw, (LONG)bbh };
    cl->lpVtbl->OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    cl->lpVtbl->RSSetViewports(cl, 1, &vp);
    cl->lpVtbl->RSSetScissorRects(cl, 1, &sc);
    cl->lpVtbl->SetGraphicsRootSignature(cl, s_rs);
    cl->lpVtbl->SetDescriptorHeaps(cl, 1, &s_heap);
    D3D12_GPU_DESCRIPTOR_HANDLE gh; RSX_D3D12_GPU_HEAP_START(s_heap)(s_heap, &gh);
    cl->lpVtbl->SetGraphicsRootDescriptorTable(cl, 1, gh);
    float so[4] = { 2.0f / s_vw, 2.0f / UIOV_VW_H, 0, 0 };
    cl->lpVtbl->SetGraphicsRoot32BitConstants(cl, 0, 4, so, 0);
    cl->lpVtbl->SetPipelineState(cl, s_pso);
    cl->lpVtbl->IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = s_vb[s_vbi]->lpVtbl->GetGPUVirtualAddress(s_vb[s_vbi]);
    vbv.SizeInBytes = s_nv * sizeof(UVert); vbv.StrideInBytes = sizeof(UVert);
    cl->lpVtbl->IASetVertexBuffers(cl, 0, 1, &vbv);
    cl->lpVtbl->DrawInstanced(cl, s_nv, 1, 0, 0);
}

#else  /* !_WIN32: no overlay; cellMsgDialog keeps its non-interactive behaviour */
int  uiov_dialog_available(void) { return 0; }
unsigned long long uiov_last_draw_ms(void) { return 0; }
void uiov_dialog_open(const char* m, int k, int d) { (void)m; (void)k; (void)d; }
int  uiov_dialog_result(void) { return 0; }
void uiov_dialog_close(void) {}
int  uiov_pad_gate(void) { return 0; }
#endif
