/*
 * ps3recomp - D3D12 RSX Backend
 *
 * Translates RSX GPU state to D3D12 rendering commands.
 *
 * Phase 1 implementation:
 *   - Win32 window + D3D12 device + swap chain
 *   - Clear render target to RSX clear color
 *   - Present with vsync
 *   - Basic vertex-colored triangle rendering
 *
 * This file is C with COM calls (D3D12 is a COM API).
 * We use the C interface (__uuidof not available in C, so we
 * define GUIDs manually).
 */

#ifdef _WIN32

#include <time.h>
#include "rsx_d3d12_backend.h"
#include "rsx_primitives.h"
#include "rsx_vertex_fetch.h"
#include "rsx_texture_layout.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "rsx_getenv_cache.h"   /* cached getenv() for hot paths (2026-10-04) */
#ifdef PS3RECOMP_DLSS
#include "rsx_dlss.h"           /* optional NVIDIA DLAA (DLSS=1), rsx_dlss.cpp */
/* v243K: per-draw DLSS bookkeeping only when DLSS / FSR (or its probe) is configured -> with it off the FIFO thread
 * runs exactly the pre-PR path (no qualify test, no draw-info fill, no zeta-vote map, VCB_DEDUP identity unchanged). */
static int dlss_hooks_on(void) { static int on = -1; if (on < 0) on = rsx_dlss_hooks_on(); return on; }
#endif
#include "ps3emu/hz_stats.h"      /* v225A hitch attribution counters */
#if defined(__SSE4_1__)
#include <smmintrin.h>   /* v30d: vertex fetch shuffles */
#include <immintrin.h>   /* v219h: AVX2 index read (read_guest_indices) */
#endif
#include <math.h>


#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* D3D12 headers */
#include <d3d12.h>
#include "rsx_d3d12_abi.h"   /* struct-returning D3D12 calls for every Windows SDK */
#include <d3d12sdklayers.h>   /* ID3D12Debug / ID3D12InfoQueue */
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>

/* We need these GUIDs — define them here to avoid uuid.lib dependency */
#include <initguid.h>

/* Link libraries */
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

/* ---------------------------------------------------------------------------
 * Constants
 * -----------------------------------------------------------------------*/

#define FRAME_COUNT         2   /* double buffering */
/* VP-path vertex capacity per frame (each vertex = 16 float4 = 256 B, indexed
 * draws are expanded). Was MAX_VERTICES (393216): inFamous' world frame asks for
 * more, the buffer filled every frame (PERF: exactly 393.2k verts/frame) and every
 * later draw silently got ZERO vertices -- the scene RT stayed untouched (the
 * 2026-10-04 magenta test). Default 2M (512 MB x2 parity), VP_VB_VERTS=<n>
 * overrides; init halves it until the upload heap allocation succeeds. */
static u32 s_vp_max_verts = 1048576u;   /* verts per CHUNK (256 MB); see VP_MAX_CHUNKS */
/* The VP vertex store is a list of chunks per frame parity, allocated on
 * demand (vp_reserve): inFamous' city frame submits ~2.6M expanded vertices
 * (10k draws; measured on the Linux repro with FRAME_DRAWS=1), which no single
 * fixed buffer sized for the common case holds, and one giant upload resource
 * risks the per-resource size limit. A draw that does not fit the current
 * chunk moves to the next one; render_frame rebinds the vertex buffer when the
 * chunk changes. Chunk 0 of each parity is allocated at init. */
#define VP_MAX_CHUNKS 12
#define VC_CHUNK_ID 0xC0u   /* v60 VCACHE: record's vertices live in the cache buffer */
static IDXGIAdapter3* s_adapter3;   /* video-memory budget queries ([VIDMEM]) */
static u32 s_sf_pso_miss, s_sf_vs_compiles;   /* per-frame, for [SLOWFRAME] */
/* SRV dedupe (2026-10-04): the per-draw descriptor windows are rewritten every
 * frame (16 units x ~10k draws = 160k CreateShaderResourceView calls), almost
 * all identical to last frame's. Skip a write whose (resource, format, mapping,
 * cube) matches what the slot already holds. Any resource Release bumps
 * s_srv_gen, which invalidates the whole shadow (a freed resource's address can
 * be reused by a new one). SRV_DEDUPE=0 disables. */
static u32 s_srv_gen = 1;
/* BISECT=1 (2026-10-04 evening): self-bisect of the v10 changes in ONE run. Once
 * the world is up (first frame with >= 500 draws) it steps through cumulative
 * phases every 20 s and saves bisect_p<N>_000.bmp 12 s into each phase:
 *   p0 everything on | p1 no pso memo | p2 + no SRV dedupe | p3 + address in the
 *   pipeline key | p4 + no shader disk cache. */
static int s_bis_nomemo, s_bis_nosrvdd, s_bis_kaddr, s_bis_nodisk, s_bis_literal, s_bis_active;
static u32 s_frame_no = 1;          /* bumped at the end of every render_frame */
volatile long g_v244_tex_null = 0, g_v244_tex_noslot = 0;   /* v244 [TEXNULL] counters */
static int s_cz_units[4]; static float s_cz_slope[4];   /* v244 coarse depth bias table (defined at coarse_bias_level) */
static u32 s_sf_srv_writes, s_sf_srv_skips;
static u32 s_sf_zconversions;
static int ps3_telem_enabled(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_TELEM"); v = e && *e == '1'; }
    return v;
}
static ID3D12Resource* s_vpc_res[3][VP_MAX_CHUNKS];   /* v207: x3 for RF_TRIPLE */
static unsigned char*  s_vpc_map[3][VP_MAX_CHUNKS];
static u32             s_vpc_cur;      /* chunk being filled, current parity */
static int             s_vpc_nogrow;   /* an allocation failed: stop growing */
static u32             s_vpc_count[3]; /* chunks allocated per parity */
/* v30d VTX_INDEXED: per-parity index upload buffers (see vib_build). Declared up
 * here because render_frame and the frame resets use them. */
#define VIB_BYTES (96u << 20)
static ID3D12Resource* s_ib_res[3];
static unsigned char*  s_ib_map[3];
static u32 s_ib_off = 0;
static int s_ib_fail = 0;
#define MAX_VERTICES    393216  /* per-frame vertex buffer (dbgfont submits
                                 * ~7.5k verts/frame; leave generous headroom) */
/* Texture units visible to guest fragment programs. The RSX has 16; this was
 * 4, and inFamous' in-world G-buffer programs sample units 0-6 -- their PSOs
 * failed with E_INVALIDARG (shader reads t4-t6 / s4-s6, root signature only
 * declares t0-t3 / s0-s3), so the scene's main pass never drew. */
#define RSX_D3D_TEX_UNITS 16
/* v231b: 16384 -> 28672. LOD_CLASS_SCALE (draw distance) pushes city frames past 16384 records -> FRAME_SPLIT
 * mid-frame executes on most frames (tail latency, black BENCH_SHOTs). 28672 keeps the shader-visible SRV heap
 * (DRAW_SRV_BASE + 2 halves x MAX_DRAWS x 16 + zeta) at ~922k < the 1,000,000 D3D12 tier-1/2 limit. */
#define MAX_DRAWS        28672  /* per-frame draw records (array size; the
                                 * usable count is s_max_draws, lowered at init
                                 * if the per-draw constant buffers cannot be
                                 * allocated). Was 2048: inFamous' city frame
                                 * issues ~10.2k BEGIN/END batches, and every
                                 * record past 2048 was silently dropped.
                                 * Old note: Sized from the
                                 * measured worst case (~1841, FRAME_BUDGET);
                                 * the per-draw constant buffer is
                                 * VP_CB_STRIDE * MAX_DRAWS * 2 bytes, so 16384
                                 * asked for a 264 MB upload allocation. */
/* Per-draw VP constant-buffer slot: vp_c[512] + posscale + posoffset
 * (514 vec4 = 8224 B) rounded up to D3D12's 256-byte CBV alignment.
 * Constants are snapshotted at RECORD time -- wave's passes each set their
 * own texScale/offset uniforms, so one per-frame snapshot ran every pass
 * with the LAST pass's constants. */
static u32 s_max_draws = MAX_DRAWS;
#define VP_CB_STRIDE      8448
/* b1 per-draw FP slot: rsx_texscale[4] + rsx_alphatest + fp_k[64]
 * = 69 float4 = 1104 B, rounded to D3D12's 256-byte CBV alignment. */
#define VP_FPCB_STRIDE    1280
#define VERTEX_STRIDE       36  /* bytes per host vertex: pos3 + col4 + uv2 */

typedef struct {
    u32 vb_byte_offset; /* offset into vb_mapped where this draw's verts live */
    u32 vertex_count;
    u32 topology;       /* rsx_topology (NOT a D3D value; see topo_to_d3d) */
    int textured;       /* 1 = sample the bound font/atlas texture (dbgfont) */
    int is_vp;          /* 1 = real VP path: vb_byte_offset indexes vp_vb (float4) */
    /* VP path per-draw shader/texture state, captured at draw_arrays time. */
    u32 fp_addr;        /* SET_SHADER_PROGRAM value (guest FP ucode location)   */
    int fp_exp32;       /* SET_SHADER_CONTROL 32-bit-exports bit at draw time   */
    u32 alpha_ctl;      /* alpha test: enable<<16 | (func&0xFF)<<8 | ref */
    u32 begin_epoch;    /* SET_BEGIN_END generation, for batch concatenation */
    u32 sref;           /* v33 guest stencil reference (OMSetStencilRef) */
    float zmin, zmax;   /* v33 guest depth range (SET_CLIP_MIN/MAX) -> viewport Min/MaxDepth */
    float dbmin, dbmax; /* v35 depth bounds (cull key bit 24 = test on) -> OMSetDepthBounds */
    u32 ct;             /* v37 SET_SURFACE_COLOR_TARGET at record time (0 = NONE: no colour writes) */
    const u8* fp_uc;    /* v33 snapshot of the FP ucode AT DRAW TIME (s_fpsnap arena) */
    u32 fp_uh;          /* v33 hash of that snapshot */
    u32 fp_code_hash, fp_const_count, fp_tex_mask, fp_meta_valid;
    u32 cull;           /* packed face culling: bit0 enable, bit1 cull FRONT
                         * (else BACK), bit2 front face is CCW. RSX culls back
                         * faces on most solid geometry; rendering everything
                         * double-sided lets a shell's interior faces show
                         * through and shade unlit (black). */
    u32 mrtk;           /* v90: MRT 1-3 write masks (bits 0-11, D3D order) | bit12 valid | bits 13-15 MRT blend OFF */
    u32 cmask;          /* D3D write mask from SET_COLOR_MASK at draw time
                         * (wave's sim passes write single lanes of the height
                         * maps; ignoring the mask stomped persistent state) */
    /* Per-unit textures (t0-t3): decompiled FPs sample up to 4 units
     * (demosaic's interpolation passes read 3). Captured at draw time. */
    struct {
        u32 off;        /* resolved vm offset (guest upload source), 0 = none */
        u32 raw;        /* raw RSX offset (offscreen-RT matching) */
        u32 w, h, fmt;  /* dims + RSX base format */
        u32 ctrl1;      /* NV4097 TEXTURE_CONTROL1: component remap crossbar */
        u32 mips;       /* SET_TEXTURE_FORMAT bits 16..31: mipmap level count.
                         * Cube faces sit one whole mip pyramid apart, so this
                         * is what sets the face stride. */
        u32 zf;         /* v34: TEXTURE_ADDRESS [31:28] depth-compare func (shadow lookups) */
        int cube;       /* SET_TEXTURE_FORMAT bit 2: a cube texture. Sampled
                         * with a 3-component direction, not a 2D uv -- treating
                         * one as 2D is what makes an environment-mapped chrome
                         * surface come out with black patches and banding. */
        int set;
    } tex[RSX_D3D_TEX_UNITS];
    int tex_rt[RSX_D3D_TEX_UNITS];      /* pre-pass: OffRT index sampled by unit, -1 = none */
    int tex_slot;       /* legacy single-slot path (atlas); -1 = none */
    int samp_tbl;       /* sampler-table index (SAMP_GUEST), 0 = default linear/wrap */
    int vs_idx;         /* VPVSEntry slot for this draw's vertex program (-1 = primary) */
    int blend;          /* guest blend enable at draw time */
    u32 blend_key;      /* packed guest blend state (factors+equation), PSO key */
    /* Render-to-texture: which colour surface this op targets. 0 = a display
     * buffer (the swap-chain backbuffer); else the resolved vm offset of an
     * offscreen surface (demosaic chains its effect passes through local-
     * memory buffers and composites from them). */
    u32 rt_off;
    /* Which per-draw constant slot this record's constants were written to.
     * vp_record_cb writes them keyed by the RECORD index, so any pass that
     * reorders or compacts s_d3d.draws (DRAW_KEEP_TEX, DRAW_LAST_TEX,
     * DRAW_LIMIT) would otherwise make every surviving draw read some other
     * object's MVP -- silently transforming it somewhere else entirely. */
    u32 cb_slot;
    u32 fcb_slot;       /* v219j: FP slot (b1) actually bound (== cb_slot unless FPCB_STAGE re-used an identical one) */
    u32 vcb_slot;       /* v219g: VP constant slot actually bound (== cb_slot unless VCB_DEDUP re-used an identical earlier slot) */
    u32 vb_chunk;       /* VP vertex chunk holding vb_byte_offset (s_vpc_*) */
    u32 vb_stride;      /* bytes per vertex of this draw (16 x slots the VP reads) */
    /* Depth surface (zeta) this op renders with, and its size (2026-10-04 evening). */
    u32 zeta_off, zw, zh;
    u32 disp_off;       /* display-surface op: the display buffer's VRAM offset (rt_off == 0) */
    u32 clr_mask;       /* is_clear: RSX CLEAR_SURFACE mask (bit0 Z, bit1 S, bits4-7 colour) */
    float clr_depth; u32 clr_stencil;
    int zsamp_zs;       /* prepass: zeta surface this draw samples as a texture (-1 none) */
    int zsamp_rt;       /* ... and the colour copy (OffRT slot) it reads instead */
    u32 shkey;          /* v34 SHADOWS: bits 0-3 = units sampled as a depth-compare shadow map
                         * (bound to the zeta surface itself), bits 4-15 = their zfunc (3 bits/unit) */
    int shz[4];         /* v34: zeta surface index per shadow unit (-1 none) */
    u32 srv_src, srv_src_frame;     /* v46 SRV_ALIAS: draw whose SRV window this one binds (valid when frame == s_frame_no) */
    ID3D12PipelineState* pso_memo;  /* pipeline resolved in the prepass ...      */
    u32 pso_memo_frame;             /* ... valid only when == s_frame_no          */
    u32 rt_mrt[3];      /* colour targets B,C,D (MRT1/2/3), 0 = none. Deferred
                         * shading G-buffers write 3-4 targets in one pass. */
    u32 rt_w, rt_h;     /* surface clip dims at record time (offscreen RT size) */
    int rt_slot;        /* off_rt slot resolved in render_frame's pre-pass (-1 none) */
    int mrt_slot[3];    /* same for rt_mrt[] */
    u32 rt_fmt;         /* RSX surface colour format (SET_SURFACE_FORMAT [4:0]) */
    /* Guest viewport rect at draw time (target pixels). Sub-viewport layouts
     * (wave's debug tiles) position quads with this, not with constants --
     * forcing full-target viewports drew every tile window-sized. */
    u32 vp_x, vp_y, vp_w, vp_h;
    /* Ordered clear op (offscreen surfaces only; display clears stay the
     * frame-start backbuffer clear). is_clear records also set is_vp so the
     * legacy replay pass skips them. */
    int   is_clear;
    float cc[4];
    /* v30d VTX_INDEXED: unique vertices in the VB window + a u32 index list. Valid
     * only when ib_stamp == s_frame_no (records are reused frame to frame). */
    u32 ib_off, index_count, ib_stamp;
} D3D12DrawRecord;

/* v57: descriptor-window cache for a run of ordinary draws.  The old SRV
 * alias only compared with the immediately preceding draw.  City rendering
 * alternates a small set of materials, so A,B,A,B still rebuilt 16 D3D12
 * descriptors four times.  Hash the complete captured texture state and pass
 * identity, then verify on a hit.  Entries are direct-mapped: a collision is
 * merely a missed optimization.  The cache is invalidated by any non-plain
 * draw, retaining v46's conservative render-target/depth ordering rule. */
#define SRV_ALIAS_CACHE 4096u
#define SRV_ALIAS_WAYS 4u
typedef struct { u64 hash; u32 draw, generation; } SRVAliasEntry;
static SRVAliasEntry s_srv_alias_cache[SRV_ALIAS_CACHE][SRV_ALIAS_WAYS];
static u32 s_srv_alias_generation = 1;
static int s_srv_assoc_cfg = -1, s_srv_assoc_phase = -1;
static u32 s_srv_assoc_bit = 0;
static u64 s_srv_assoc_way0 = 0, s_srv_assoc_extra = 0, s_srv_assoc_miss = 0;

/* v230 SRV_ALIAS_ASSOC: the material-window cache above used one direct-mapped
 * entry per bucket.  City traversal aliases 7k--10k ordinary draws/frame, but
 * still rebuilt 250--1200 complete 16-SRV windows/frame; alternating materials
 * that collide in the direct map continuously evict one another.  Retain four
 * recent candidates per bucket.  A hit is still accepted only after the same
 * full captured texture-state comparison, and render-target/depth barriers
 * still invalidate the complete generation, so this cannot change which
 * resource a draw samples.  It only avoids redundant descriptor creation.
 * Default off until the traversal A/B has landed. */
static int srv_alias_assoc_on(void)
{
    extern volatile int g_abx, g_ab_phase;
    if (s_srv_assoc_cfg < 0) {
        const char* e = getenv("SRV_ALIAS_ASSOC");
        const char* p = getenv("SRV_ALIAS_ASSOC_PHASE");
        const char* b = getenv("SRV_ALIAS_ASSOC_BIT");
        s_srv_assoc_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_srv_assoc_phase = atoi(p) & 1;
        if (b && *b) s_srv_assoc_bit = (u32)strtoul(b, NULL, 0);
        fprintf(stderr, "[SRV_ALIAS_ASSOC] v230 four-way descriptor-window hints: %s (phase parity %d, ABX 0x%X set = old direct map)%c",
                s_srv_assoc_cfg ? "ON" : "off", s_srv_assoc_phase, s_srv_assoc_bit, 10);
    }
    return s_srv_assoc_cfg &&
           (s_srv_assoc_phase < 0 || ((g_ab_phase & 1) == s_srv_assoc_phase)) &&
           !(s_srv_assoc_bit && ((u32)g_abx & s_srv_assoc_bit));
}

static void srv_alias_note(u32 bucket, SRVAliasEntry ent, int assoc)
{
    SRVAliasEntry* const w = s_srv_alias_cache[bucket];
    if (!assoc) { w[0] = ent; return; }
    int at = SRV_ALIAS_WAYS - 1;
    for (int i = 0; i < (int)SRV_ALIAS_WAYS; i++)
        if (w[i].generation == ent.generation && w[i].hash == ent.hash) { at = i; break; }
    for (int i = at; i > 0; i--) w[i] = w[i - 1];
    w[0] = ent;
}

static u64 srv_alias_hash(const D3D12DrawRecord* dr)
{
    u64 h = 1469598103934665603ull;
    u32 mask = dr->fp_meta_valid ? dr->fp_tex_mask : 0xFFFFu;
    h ^= mask; h *= 1099511628211ull;
    for (u32 u = 0; u < RSX_D3D_TEX_UNITS; u++) if (mask & (1u << u)) {
        const u32* p = (const u32*)&dr->tex[u];
        for (size_t i = 0; i < sizeof dr->tex[u] / sizeof *p; i++) { h ^= p[i]; h *= 1099511628211ull; }
    }
    h ^= dr->rt_off; h *= 1099511628211ull;
    h ^= dr->disp_off; h *= 1099511628211ull;
    for (int i = 0; i < 3; i++) { h ^= dr->rt_mrt[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

static int srv_alias_same(const D3D12DrawRecord* a, const D3D12DrawRecord* b)
{
    u32 am = a->fp_meta_valid ? a->fp_tex_mask : 0xFFFFu;
    u32 bm = b->fp_meta_valid ? b->fp_tex_mask : 0xFFFFu;
    if (am != bm) return 0;
    for (u32 u = 0; u < RSX_D3D_TEX_UNITS; u++)
        if ((am & (1u << u)) && memcmp(&a->tex[u], &b->tex[u], sizeof a->tex[u])) return 0;
    return a->rt_off == b->rt_off && a->disp_off == b->disp_off &&
           a->rt_mrt[0] == b->rt_mrt[0] && a->rt_mrt[1] == b->rt_mrt[1] &&
           a->rt_mrt[2] == b->rt_mrt[2];
}

static void srv_alias_barrier(void)
{
    if (++s_srv_alias_generation == 0) {
        memset(s_srv_alias_cache, 0, sizeof s_srv_alias_cache);
        s_srv_alias_generation = 1;
    }
}

/* Offscreen render target (render-to-texture). Persistent across frames --
 * pass N's output is sampled by pass N+1 and possibly by later frames. */
typedef struct {
    ID3D12Resource*       res;
    ID3D12Resource*       up;          /* init-upload staging (kept alive) */
    ID3D12Resource*       feedback;    /* immutable input while this RT is written */
    D3D12_RESOURCE_STATES feedback_st;
    u32                   off, w, h;   /* raw RSX offset + dims */
    u32                   dxgi;        /* DXGI_FORMAT of the resource */
    D3D12_RESOURCE_STATES st;          /* tracked resource state */
    int                   used;        /* referenced this frame */
    int                   cwritten;    /* v73c: a draw with a colour mask != 0 targeted it */
} OffRT;
/* 32: demosaic double-buffers a 6-surface chain, and inFamous ALIASES render
 * targets -- one VRAM offset (0xF340000) is drawn at 1280x720, 1280x180,
 * 512x288, 320x180, 256x256 and 1024x1024 within one frame. Each alias now gets
 * its own slot (keyed off+w+h+format) instead of recreating one slot. */
#define MAX_OFF_RTS  32
/* SRV heap layout (2026-10-04: the regions used to overlap -- vp_tex wrote at
 * 1+slot for 96 slots, straight over the RT SRVs at 5..20 and the per-draw
 * windows from 32, corrupting other draws' descriptors mid-frame):
 *   0..7            atlas / legacy
 *   RT_SRV_BASE     MAX_OFF_RTS offscreen-RT SRVs
 *   VPTEX_SRV_BASE  VP_TEX_SLOTS uploaded-texture SRVs
 *   DRAW_SRV_BASE   per-draw windows, RSX_D3D_TEX_UNITS each */
#define RT_SRV_BASE  8
/* Per-draw SRV windows: each VP draw gets 4 consecutive descriptors (t0-t3)
 * so multi-unit fragment programs see all their textures (demosaic's
 * interpolation passes sample 3 units). */
#define VPTEX_SRV_BASE (RT_SRV_BASE + MAX_OFF_RTS)
#define DRAW_SRV_BASE  (VPTEX_SRV_BASE + VP_TEX_SLOTS)
#define MAX_ZS 24          /* distinct depth surfaces (offset+size) */
static int rf_early_on(void);   /* v225A RF_EARLY: two halves of per-draw windows */
#define ZS_SRV_BASE   (DRAW_SRV_BASE + (rf_early_on() ? 2 : 1) * MAX_DRAWS * RSX_D3D_TEX_UNITS)   /* 16-slot window per zeta: t0 = depth */
#define SRV_HEAP_SIZE (ZS_SRV_BASE + MAX_ZS * RSX_D3D_TEX_UNITS)
static int fp_ctrl_bits(u32 sc);
static u32 fp_ctrl_word(int bits);

/* Per-frame VP texture slot: a guest texture uploaded for this frame's VP
 * draws (re-uploaded every frame -- gcm/cube's plasma animates in guest
 * memory). SRV lives at heap index 1+slot. */
typedef struct {
    ID3D12Resource* res;
    ID3D12Resource* up;
    u32 off, w, h, fmt; /* current contents (resource reused when dims match) */
    u32 csum;           /* sparse checksum of the source bytes last uploaded */
    u32 face_stride;    /* guest mip-chain stride: part of a cubemap's content key */
    int cube;           /* resource is a 6-face cube, sampled as TextureCube */
    u32 key;            /* the ORIGINAL bound offset -- the cache lookup key.
                         * off is the RESOLVED source after TEX_OFF_BIAS/TEX_REMAP
                         * shift it, so keying on off never matches the raw offset
                         * a later draw arrives with, and every draw re-uploads. */
    int used;           /* referenced this frame */
    u32 lru;            /* v32f: s_tex_frame of the last frame that bound it */
    int pheap;          /* v32f: pool heap index of a placed resource, -1 = committed */
    u32 ppage, pnpages; /* v32f: its 4 KB page range inside that heap */
    u32 nlev;           /* v146: mip levels in the host resource */
} VPTexSlot;
/* Distinct textures uploadable per frame. Four was enough for a UI/atlas-style
 * frame but not for a scene: this title binds ~20 distinct textures per frame,
 * so every draw past the fourth got slot -1 from vp_upload_tex_slot ("out of
 * slots") and drew UNTEXTURED. That reads as a correctly-lit but solid black
 * object, which is indistinguishable from a missing texture upload. */
#define VP_TEX_SLOTS 4096  /* v32f: was 1024 (cutscene cuts thrashed it: 2326 creates/10 s); 96, 256; inFamous' city frame binds > 256 distinct textures: slots were
                            * recycled (resource recreated + re-uploaded) every frame, which also reset
                            * the SRV dedupe each frame. 2026-10-04 evening. */

/* Decompiled-VS cache: one entry per distinct vertex-program ucode seen at
 * draw time (hashed). Apps switch VPs between draws (gcm/cube: its MVP cube VP
 * vs dbgfont-gcm's text VP); compiling only the first left later draws
 * transformed by the wrong program (text offscreen). */
typedef struct {
    u32 hash;               /* FNV-1a of the ucode */
    ID3DBlob* vs;
    int uses_c03;
    char* src;              /* HLSL source, kept for the PSO-failure dump */
    u32 cuse[16];           /* v46 VCB_PARTIAL: constants this VS reads (bit per vp_c[i]) */
    int cvalid, crel;       /* mask computed / VS indexes constants relatively (copy all) */
} VPVSEntry;
/* 2026-10-04: 16 -> 256. inFamous in-world uses far more than 16 VPs; eviction
 * memmoves every slot down, which invalidates EVERY FP PSO keyed on a shifted
 * vs_idx (vs_hash check) -> mass recompiles each frame. */
#define VP_VS_CACHE 2048 /* 2026-10-04 evening: 256 -> 2048 and round-robin replacement (no memmove). wave uses 5+ distinct VPs; at 4 the cache
                         * thrashed every frame and eviction shifted slots
                         * under recorded vs_idx values -- the display mesh
                         * drew with the WRONG vertex program (sub-rect +
                         * edge slivers instead of fullscreen) */
/* v34: vertex attribute layout last seen with each decompiled VS (capture writes it
 * next to the VS source in rsxcap/frame_NN/vp/ -- finding why a mesh's normals are 0). */
typedef struct { u8 en, type, size, freq; u16 stride; u8 loc; u8 pad; } VsAttrInfo;
static VsAttrInfo s_vs_attr[VP_VS_CACHE][16];
static int s_vsattr_chg = 1, s_vsattr_last = -1;   /* v221C (vp_record_cb) */

/* Compiled guest-FP pipeline cache: decompiled VS (by cache slot) + decompiled
 * PS (fragment ucode at fp_addr). */
typedef struct {
    u32 fp_addr;
    int vs_idx;             /* VPVSEntry slot this PSO's VS came from */
    u32 vs_hash;            /* validates the slot hasn't been evicted */
    u32 gen;
    u32 blend;              /* packed guest blend key (enable+factors+eq, PSO key) */
    int nrt;                /* bound colour target count (PSO key)       */
    u32 rtfmt;              /* DXGI format of the colour targets (PSO key) */
    int exp32;              /* 32-bit-exports control bit (PSO key)       */
    u32 ucode_hash;         /* FNV-1a of the FP ucode: apps re-patch inline
                             * constants per frame (wave's stamp position/
                             * amplitude) -- address-only keying served the
                             * stale compile forever. */
    u32 cmask;              /* colour write mask (PSO key) */
    u32 cull;               /* packed face culling (PSO key) */
    u32 kaddr;              /* fp_addr in the key only when an address-keyed debug
                             * override is active (FP_ONE/FP_KILL/...); else 0 --
                             * the pipeline is a function of the CODE (ucode_hash),
                             * and this title re-uploads the same programs to new
                             * addresses, which kept compiling new pipelines. */
    u32 cube_mask;          /* which units are cube textures (PSO key): the HLSL
                             * declares those samplers as TextureCube, so a cube
                             * and a 2D variant of the same program are different
                             * pipelines and must not share a cache entry. */
    ID3D12PipelineState* pso;
} VPFPEntry;
/* Guest-FP PSO cache. 16 entries thrashed badly: this title needs far more
 * distinct (fp, vs, blend, rt, cmask, ucode-hash) combinations than that in a
 * single frame, so the FIFO evicted entries that were needed again immediately
 * and ~17 shaders were recompiled EVERY frame -- about half the frame's CPU
 * time. Entries are small (a PSO pointer plus key fields). */
#define VP_FP_CACHE 4096   /* 2026-10-04 evening: 256 thrashed in inFamous' 10k-draw city frame (hundreds of
                            * recompiles per frame = 11 s frames); now hashed lookup + ring replacement */

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

typedef struct {
    /* Window */
    HWND hwnd;
    u32  width;
    u32  height;
    int  window_closed;

    /* D3D12 core */
    ID3D12Device*              device;
    ID3D12CommandQueue*        cmd_queue;
    IDXGISwapChain3*           swap_chain;
    ID3D12DescriptorHeap*      rtv_heap;
    u32                        rtv_descriptor_size;
    ID3D12Resource*            render_targets[FRAME_COUNT];
    ID3D12CommandAllocator*    cmd_allocators[FRAME_COUNT];
    ID3D12GraphicsCommandList* cmd_list;

    /* Synchronization */
    ID3D12Fence* fence;
    HANDLE       fence_event;
    u64          fence_values[FRAME_COUNT];
    u32          frame_index;

    /* Pipeline */
    ID3D12RootSignature*  root_signature;
    ID3D12PipelineState*  pipeline_state;         /* triangle class — default */
    ID3D12PipelineState*  pipeline_state_lines;   /* line class */
    ID3D12PipelineState*  pipeline_state_points;  /* point class */

    /* Depth/stencil */
    ID3D12DescriptorHeap* dsv_heap;
    ID3D12Resource*       depth_buffer;

    /* Dynamic vertex buffer (upload heap) */
    ID3D12Resource*       vertex_buffer;
    D3D12_VERTEX_BUFFER_VIEW vb_view;
    void*                 vb_mapped;      /* persistently mapped */
    u32                   vb_offset;      /* current write position */

    int                   pipeline_ready; /* 1 if root sig + PSO created */

    /* Debug: copy the presented backbuffer to disk as BMP for the first N
     * frames (enabled by env CELLMARK_DUMP). Lets us verify what actually
     * rendered without racing the window/process lifetime. */
    ID3D12Resource*       readback_buf;
    u32                   readback_pitch;
    int                   dump_frames_left;
    int                   dump_skip_left;   /* CELLMARK_DUMP_SKIP: presents to ignore first */

    /* Textured pipeline (dbgfont / 2D atlas quads). The font atlas is an 8-bit
     * coverage texture uploaded as R8_UNORM and sampled in the pixel shader. */
    ID3D12PipelineState*  pipeline_state_tex;   /* triangle + texture + blend */
    ID3D12DescriptorHeap* srv_heap;             /* shader-visible, 1 SRV slot  */
    ID3D12DescriptorHeap* samp_heap;            /* shader-visible sampler tables (SAMP_GUEST) */
    ID3D12Resource*       tex_resource;         /* current atlas texture       */
    ID3D12Resource*       tex_upload;           /* staging buffer for uploads  */
    u32                   tex_w, tex_h;         /* dims of tex_resource         */
    int                   tex_ready;            /* 1 once an atlas is uploaded  */
    u32                   tex_src_offset;       /* guest RSX offset of atlas    */
    int                   tex_bound;            /* a texture is bound for draws */
    int                   tex_dirty;            /* re-upload needed this frame  */

    /* Real RSX vertex-program path: the captured VP is decompiled to HLSL and
     * used as the vertex shader, fed the raw float4 attrib0 + the vp_c[]
     * constant bank. This produces exact position + texcoord (vs. the frac
     * approximation). Used for the 2D/dbgfont quad draws. */
    ID3D12RootSignature*  vp_root_sig;          /* CBV(b0) + SRV table + sampler */
    ID3D12PipelineState*  pipeline_state_vp;    /* decompiled VS + atlas PS      */
    ID3D12PipelineState*  pipeline_state_vp_color; /* decompiled VS + colour PS (untextured 3D) */
    ID3D12Resource*       vp_vb;                /* raw float4 attrib0, per-frame */
    void*                 vp_vb_mapped;
    u32                   vp_vb_offset;
    ID3D12Resource*       vp_cb;                /* per-draw VP constant slots    */
    void*                 vp_cb_mapped;
    ID3D12Resource*       vp_fpcb;              /* per-draw FP texscale (b1)     */
    void*                 vp_fpcb_mapped;
    int                   vp_ready;             /* VS+PSO compiled ok            */
    u32                   vp_compiled_bytes;    /* ucode size when last compiled */
    ID3DBlob*             vp_vs_blob;           /* kept for guest-FP PSO builds  */
    u32                   vp_gen;               /* bumped per VP recompile       */
    int                   vp_uses_c03;          /* VS references vp_c[0..3]      */
    VPTexSlot             vp_tex[VP_TEX_SLOTS]; /* per-frame VP texture slots    */
    VPVSEntry             vp_vs[VP_VS_CACHE];   /* per-draw decompiled VS cache  */
    int                   vp_vs_n;
    VPFPEntry             vp_fp[VP_FP_CACHE];   /* guest-FP PSO cache            */
    int                   vp_fp_n;
    u32                   srv_inc;              /* CBV_SRV_UAV descriptor size   */
    /* VP path: latest texture bound per unit (t0-t3). */
    struct { u32 off, raw, w, h, fmt, ctrl1, mips; int cube; int set; u32 samp; u32 zf; } cur_texs[RSX_D3D_TEX_UNITS];

    /* Render-to-texture: offscreen RT pool + their RTV heap. */
    OffRT                 off_rt[MAX_OFF_RTS];
    /* DRAW_ARRAYS batch merging (see d3d12_draw_arrays): the vertex index one
     * past the last batch, and whether the current BEGIN/END may still be
     * extended. */
    u32                   merge_first_end;
    int                   merge_prev_draw;
    ID3D12DescriptorHeap* rt_rtv_heap;          /* MAX_OFF_RTS RTVs (CPU only)   */

    /* Frame-parity double buffering for the per-draw upload streams (vp_vb
     * vertices, vp_cb constants, vp_fpcb texscales): records for frame N+1
     * are written while frame N's GPU work may still be reading -- writes go
     * to the other half. Toggled at the end of render_frame. */
    int                   vp_parity;

    /* Per-frame draw recording */
    int                   frame_recording; /* 1 if cmd list is open for recording */
    u32                   draw_count;      /* draws this frame */
    D3D12DrawRecord*      draws;           /* v59: points into s_draw_buf[0|1] (RF_ASYNC double buffer) */

    /* Pointer to current RSX state (set before draw calls) */
    const rsx_state*      current_rsx_state;

    /* Current frame state */
    float clear_color[4];  /* RGBA float */

    /* Stats */
    u64 frame_count;
    u64 last_fps_time;
    u32 fps;

    int initialized;
} D3D12State;

static D3D12State s_d3d;
/* v154 RES_SCALE (default 1 = PS3 720p): internal resolution. Every render target / depth surface at the
 * guest display size (1280x720) is allocated at s_bbw x s_bbh instead, the swap chain too; viewports of
 * draws into them are scaled, fragment WPOS is scaled back to guest pixels (b1 rsx_wscale), and everything
 * that samples them uses normalised coordinates, so nothing else changes. Other targets (shadow maps,
 * half-res buffers, lookup tables) keep their guest size. Hooked into particle_size(), which every
 * attachment allocation and the per-draw viewport already go through. */
static float s_res_scale = 1.0f;
static u32   s_bbw = 0, s_bbh = 0;
/* DLSS upscaling renders below the output size: RBW x RBH = the internal render size (s_bbw: render targets, depth,
 * the display buffer), BBW x BBH = the swap chain / window / overlay size (s_outw while upscaling, else the same). */
static u32   s_outw = 0, s_outh = 0;
static float s_out_scale = 1.0f;
#define RBW (s_bbw ? s_bbw : s_d3d.width)
#define RBH (s_bbh ? s_bbh : s_d3d.height)
#define BBW (s_outw ? s_outw : RBW)
#define BBH (s_outh ? s_outh : RBH)
static void res_scale_dims(u32* w, u32* h)
{
    if (s_res_scale > 1.0f && s_bbw && *w == s_d3d.width && *h == s_d3d.height) { *w = s_bbw; *h = s_bbh; }
}
#include "rsx_particle_resolution.h"
static void particle_size(u32* w, u32* h)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("PARTICLE_FULLRES"); on = e && atoi(e) != 0;
        fprintf(stderr, "[PARTICLE81] full-resolution particles %s\n", on ? "ON" : "OFF"); }
    rsx_particle_size(w, h, on, s_d3d.width, s_d3d.height);
    res_scale_dims(w, h);   /* v154 */
}
/* Command-list-local state: reset whenever the list is reset. All writers,
 * including depth conversion, use these helpers so replay can skip redundant
 * driver calls without leaving the cache stale after an auxiliary pass. */
static ID3D12PipelineState* s_bound_pso;
static D3D12_VIEWPORT s_bound_viewport;
static D3D12_RECT s_bound_scissor;
static int s_bound_rect_valid;
static void cmd_state_reset(void) { s_bound_pso = NULL; s_bound_rect_valid = 0; }
static void cmd_pso(ID3D12PipelineState* pso)
{
    if (pso != s_bound_pso) {
        s_d3d.cmd_list->lpVtbl->SetPipelineState(s_d3d.cmd_list, pso);
        s_bound_pso = pso;
    }
}
static void cmd_rect(const D3D12_VIEWPORT* vp, const D3D12_RECT* sc)
{
    if (!s_bound_rect_valid || memcmp(&s_bound_viewport, vp, sizeof(*vp))) {
        s_d3d.cmd_list->lpVtbl->RSSetViewports(s_d3d.cmd_list, 1, vp);
        s_bound_viewport = *vp;
    }
    if (!s_bound_rect_valid || memcmp(&s_bound_scissor, sc, sizeof(*sc))) {
        s_d3d.cmd_list->lpVtbl->RSSetScissorRects(s_d3d.cmd_list, 1, sc);
        s_bound_scissor = *sc;
    }
    s_bound_rect_valid = 1;
}
/* v59 RF_ASYNC: render-thread view of the frame being replayed (see render_frame()). */
static D3D12DrawRecord s_draw_buf[3][MAX_DRAWS];
/* v207 RF_TRIPLE (default 1; 0 = old double buffering): THREE record parities (vertex chunks, index buffer, per-draw
 * constant slots, FP snapshot arena, VCACHE copy lists, draw records). With two, the FIFO thread had to wait at every
 * flip until the GPU finished frame N-1 before recording N+1 into its parity (perf206: up to 1.3 s per 600 frames,
 * while the main thread sat in its frame-begin fence wait). With three, N+1 records into N-2's parity, which is
 * already free (exec N-1 on the render thread began with wait_for_gpu, and rf_wait_idle waited for exec N-1).
 * ABX 0x40000 = keep the old GPU wait while still rotating 3 parities (in-run A/B). */
extern volatile int g_abx;   /* ppu_loader.cpp (in-run A/B bits) */
static int s_npar = 0;
static int rf_npar(void)
{
    if (!s_npar) { const char* e = getenv("RF_TRIPLE"); s_npar = (e && *e == '1') ? 3 : 2;
        fprintf(stderr, "[RF_TRIPLE] %d record parities (v207; RF_TRIPLE=1 = 3 (perf207: no gain), ABX 0x40000 = keep the GPU wait)%c", s_npar, 10); }
    return s_npar;
}
static int s_par_prev = 1;   /* parity of the frame handed off before the current one */
static D3D12DrawRecord* s_rf_draws = s_draw_buf[0];
static u32 s_rf_count = 0, s_rf_frame_no = 0;
static int s_rf_parity = 0, s_rf_present = 1;
static u32 s_rf_flip_off = 0;
static HANDLE s_rf_gpuidle_ev = NULL;
static void rf_set_title(const char* t);
/* Guest sampler tables (SAMP_GUEST, 2026-10-04 v27). Key per unit:
 * bit0 mag linear, bit1 min linear, bits2-3 mip (0 none, 1 point, 2 linear),
 * bits 4-7/8-11/12-15 RSX wrap S/T/R (1 wrap, 2 mirror, 3 clamp-to-edge,
 * 4 border, 5 clamp, 6-8 mirror-once), bit 31 = unit bound. Key 0 = default
 * (linear, wrap). Tables are created once and never evicted, so no in-flight
 * GPU work can see a descriptor change. */
/* v156 SAMP_PACK (default 1; 0 = the v27..v155 layout): the table cache was 128 fixed 16-sampler
 * tables (= the whole 2048-descriptor shader-visible sampler heap) and has been FULL a few seconds
 * into every run since v148 (aniso/LOD-bias joined the key; v154 added border bits): "[SAMP] table
 * cache full (128) -- later combos use the closest table" in play148b..play155. Every later combo
 * got another draw's samplers for all units after the first mismatch (wrong wrap/clamp/border,
 * point vs linear, no mips, wrong aniso) -- scene-dependent wrong sampling. Now: (1) units the
 * draw's fragment program does not sample (fp_tex_mask) are keyed 0 -- stale RSX unit state no
 * longer multiplies the combos; (2) border-colour bits only count on units with a BORDER axis;
 * (3) tables are packed: a table holds units 0..highest used, the 16-wide root window just starts
 * at its first descriptor (descriptors past the used units belong to other tables and are never
 * sampled -- the FP mask already gives those units a NULL SRV). Capacity ~2000/avg-units tables.
 * Log [SAMP] v156. */
#define SAMP_TBL_MAX 1024
#define SAMP_HEAP_DESCS 2048
static u32  s_samp_keys[SAMP_TBL_MAX][16];
static u64  s_samp_hash[SAMP_TBL_MAX];
static u32  s_samp_off[SAMP_TBL_MAX];   /* v156: first descriptor of table t */
static u32  s_samp_next = 0;            /* v156: next free descriptor */
static int  s_samp_n = 0;
static u32  s_samp_inc = 0;
static int samp_pack_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("SAMP_PACK"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[SAMP] v156 FP-masked packed sampler tables %s (SAMP_PACK; 0 = old 128 x 16 cache)%c",
                         v ? "ON" : "off", 10); }
    return v;
}
static int samp_guest_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("SAMP_GUEST"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* v148 TEX_ANISO / TEX_LODBIAS: the guest's max anisotropy (TEXTURE_CONTROL0 bits 4-6: 1,2,4,6,8,
 * 10,12,16x) and LOD bias (TEXTURE_FILTER bits 0-12, signed 4.8) were dropped -- every sampler was
 * MaxAnisotropy 1, bias 0. Key bits 16-18 = aniso code, 19-26 = bias in 1/16 (signed).
 * TEX_ANISO unset/-1 = the guest's value, 0 = never, N = at least N (an enhancement, not PS3).
 * TEX_LODBIAS=0 ignores the guest bias. Log [TEXANISO]. */
static u32 samp_key_from_regs(u32 filter, u32 address, u32 ctrl0)
{
    u32 minf = (filter >> 16) & 0xFFu, magf = (filter >> 24) & 0xFu;
    u32 k = 0x80000000u;
    k |= ((ctrl0 >> 4) & 7u) << 16;
    { int b = (int)((filter & 0x1FFFu) << 19) >> 19;                /* s4.8 */
      int q = (b >= 0) ? (b + 8) / 16 : -((-b + 8) / 16);          /* -> 1/16 */
      if (q < -128) q = -128;
      if (q > 127) q = 127;
      k |= ((u32)q & 0xFFu) << 19; }
    { static u32 seen[16]; static int ns = 0; u32 sk = (ctrl0 & 0x70u) | ((filter & 0x1FFFu) << 8); int f = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == sk) f = 1;
      if (!f && ns < 16) { seen[ns++] = sk;
          fprintf(stderr, "[TEXANISO] guest max aniso code %u, lod bias %.3f (ctrl0 0x%08X filter 0x%08X)%c",
                  (ctrl0 >> 4) & 7u, (double)((int)((filter & 0x1FFFu) << 19) >> 19) / 256.0, ctrl0, filter, 10); } }
    if (magf != 1) k |= 1u;                                   /* 2 linear, 4 convolution */
    if (minf == 2 || minf == 4 || minf == 6 || minf == 7) k |= 2u;
    if (minf == 3 || minf == 4) k |= 1u << 2;
    else if (minf == 5 || minf == 6) k |= 2u << 2;
    k |= (address & 0xFu) << 4;
    k |= ((address >> 8) & 0xFu) << 8;
    k |= ((address >> 16) & 0xFu) << 12;
    return k;
}
/* v154 SAMP_BORDER (default 1): the guest border colour (TEXTURE_BORDER_COLOR, A8R8G8B8) for border-mode
 * units, key bits 27-30 = R,G,B,A (on/off per channel). Was always transparent black: inFamous' shadow-map
 * units use border mode with colour 0x00FFFFFF, i.e. "outside the shadow map = depth 1.0 = LIT"; we returned
 * depth 0 there = SHADOWED, so everything past the shadow cascade's edge (the far city) was darkened/blotchy
 * and the edge swam with the camera. */
static u32 samp_border_bits(u32 argb)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SAMP_BORDER"); on = (e && *e == '0') ? 0 : 1;
                  fprintf(stderr, "[SAMP] v154 guest border colour %s (SAMP_BORDER)%c", on ? "ON" : "off", 10); }
    if (!on) return 0;
    u32 k = 0;
    if (((argb >> 16) & 0xFFu) >= 0x80u) k |= 1u << 27;   /* R */
    if (((argb >>  8) & 0xFFu) >= 0x80u) k |= 1u << 28;   /* G */
    if (( argb        & 0xFFu) >= 0x80u) k |= 1u << 29;   /* B */
    if (((argb >> 24) & 0xFFu) >= 0x80u) k |= 1u << 30;   /* A */
    return k;
}
static D3D12_TEXTURE_ADDRESS_MODE samp_addr(u32 w)
{
    switch (w) {
    case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    case 6: case 7: case 8: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}
static int samp_table_get(const u32* keys)
{
    ID3D12DescriptorHeap* h = s_d3d.samp_heap;
    if (!h) return 0;
    /* Consecutive draws commonly share the complete sampler state. Verify
     * all units; never reuse solely on a hash or texture-address match. */
    static int last = -1;
    if (last >= 0 && last < s_samp_n && !memcmp(s_samp_keys[last], keys, sizeof s_samp_keys[last])) return last;
    u64 hs = 1469598103934665603ull;
    for (int u = 0; u < 16; u++) { hs ^= keys[u]; hs *= 1099511628211ull; }
    static u16 recent[256];
    u32 bucket = (u32)(hs ^ (hs >> 32)) & 255u;
    int hit = (int)recent[bucket] - 1;
    if (hit >= 0 && hit < s_samp_n && s_samp_hash[hit] == hs &&
        !memcmp(s_samp_keys[hit], keys, sizeof s_samp_keys[hit])) { last = hit; return hit; }
    for (int i = 0; i < s_samp_n; i++)
        if (s_samp_hash[i] == hs && !memcmp(s_samp_keys[i], keys, sizeof s_samp_keys[i])) {
            recent[bucket] = (u16)(i + 1); last = i; return i;
        }
    const int pack = samp_pack_on();
    u32 nd = 16u;                                   /* descriptors this table needs */
    if (pack) { nd = 1u; for (u32 u = 0; u < 16u; u++) if (keys[u]) nd = u + 1u; }
    const int tmax = pack ? SAMP_TBL_MAX : SAMP_HEAP_DESCS / 16;
    if (s_samp_n >= tmax || s_samp_next + 16u > SAMP_HEAP_DESCS) {
        static int warned = 0;
        if (!warned++) fprintf(stderr, "[SAMP] table cache full (%d tables, %u descriptors) -- later combos use the closest table%c",
                               s_samp_n, s_samp_next, 10);
        int best = 0, bestn = -1;
        if (pack) {   /* v156: most units equal among the units this draw samples */
            for (int i = 0; i < s_samp_n; i++) {
                int m = 0;
                for (int u = 0; u < 16; u++) if (keys[u] && s_samp_keys[i][u] == keys[u]) m++;
                if (m > bestn) { bestn = m; best = i; }
            }
        } else        /* closest: most leading units (0..15) identical */
        for (int i = 0; i < s_samp_n; i++) {
            int m = 0; while (m < 16 && s_samp_keys[i][m] == keys[m]) m++;
            if (m > bestn) { bestn = m; best = i; }
        }
        return best;
    }
    int t = s_samp_n;
    last = t;
    recent[bucket] = (u16)(t + 1);
    D3D12_CPU_DESCRIPTOR_HANDLE ch;
    RSX_D3D12_CPU_HEAP_START(h)(h, &ch);
    s_samp_off[t] = s_samp_next;
    ch.ptr += (SIZE_T)s_samp_next * s_samp_inc;
    s_samp_next += nd;
    for (u32 u = 0; u < nd; u++) {
        u32 k = keys[u];
        D3D12_SAMPLER_DESC sd; memset(&sd, 0, sizeof sd);
        if (!(k & 0x80000000u)) {
            sd.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
        } else {
            u32 mag = k & 1u, mn = (k >> 1) & 1u, mip = (k >> 2) & 3u;
            sd.Filter = (D3D12_FILTER)((mn << 4) | (mag << 2) | (mip == 2 ? 1u : 0u));
            sd.AddressU = samp_addr((k >> 4) & 0xFu);
            sd.AddressV = samp_addr((k >> 8) & 0xFu);
            sd.AddressW = samp_addr((k >> 12) & 0xFu);
            sd.BorderColor[0] = (k >> 27) & 1u ? 1.0f : 0.0f;   /* v154 guest border colour */
            sd.BorderColor[1] = (k >> 28) & 1u ? 1.0f : 0.0f;
            sd.BorderColor[2] = (k >> 29) & 1u ? 1.0f : 0.0f;
            sd.BorderColor[3] = (k >> 30) & 1u ? 1.0f : 0.0f;
            sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : 0.0f;
            {   /* v148 guest anisotropy / LOD bias (samp_key_from_regs) */
                static int fa = -2, lb = -1;
                if (fa == -2) { const char* e = getenv("TEX_ANISO"); fa = (e && *e) ? atoi(e) : -1; if (fa > 16) fa = 16; }
                if (lb < 0) { const char* e = getenv("TEX_LODBIAS"); lb = (e && *e == '0') ? 0 : 1; }
                static const UINT amap[8] = { 1, 2, 4, 6, 8, 10, 12, 16 };
                UINT na = amap[(k >> 16) & 7u];
                if (fa == 0) na = 1; else if (fa > 0 && (UINT)fa > na) na = (UINT)fa;
                if (lb && mip) sd.MipLODBias = (float)(int)(signed char)(u8)((k >> 19) & 0xFFu) / 16.0f;
#ifdef PS3RECOMP_DLSS
                if (mip) sd.MipLODBias += rsx_dlss_mip_bias();   /* DLAA: NVIDIA's negative texture LOD bias */
#endif
                if (na > 1 && mag && mn && mip == 2) {
                    sd.Filter = D3D12_FILTER_ANISOTROPIC;
                    sd.MaxAnisotropy = na;
                }
            }
        }
        if (sd.MaxAnisotropy < 1) sd.MaxAnisotropy = 1;
        sd.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        s_d3d.device->lpVtbl->CreateSampler(s_d3d.device, &sd, ch);
        ch.ptr += s_samp_inc;
    }
    memcpy(s_samp_keys[t], keys, sizeof s_samp_keys[t]);
    s_samp_hash[t] = hs;
    s_samp_n++;
    if (pack && (s_samp_n == 128 || s_samp_n == 256 || s_samp_n == 512 || s_samp_n == 768))
        fprintf(stderr, "[SAMP] v156 %d sampler tables, %u of %d descriptors used%c", s_samp_n, s_samp_next, SAMP_HEAP_DESCS, 10);
    { static int n = 0; if ((n < 24 && n++ < 24)) {
        fprintf(stderr, "[SAMP] table %d:", t);
        for (int u = 0; u < 16; u++) if (keys[u]) fprintf(stderr, " u%d=%05X", u, keys[u] & 0xFFFFu);
        fprintf(stderr, "%c", 10); } }
    return t;
}
static int samp_table_for_cur(void)
{
    if (!s_d3d.samp_heap) return 0;
    u32 keys[16];
    for (int u = 0; u < 16; u++) keys[u] = s_d3d.cur_texs[u].set ? s_d3d.cur_texs[u].samp : 0u;
    return samp_table_get(keys);
}
/* v156 SAMP_PACK: border bits only matter on a BORDER (4) axis. */
static u32 samp_key_norm(u32 k)
{
    if (!k) return 0;
    if (((k >> 4) & 0xFu) != 4u && ((k >> 8) & 0xFu) != 4u && ((k >> 12) & 0xFu) != 4u)
        k &= ~(0xFu << 27);
    return k;
}
/* v156 SAMP_PACK: the table for a recorded draw -- call after vp_record_cb (fp_tex_mask). */
static int samp_table_for_draw(const D3D12DrawRecord* dr)
{
    if (!s_d3d.samp_heap) return 0;
    u32 mask = dr->fp_meta_valid ? dr->fp_tex_mask : 0xFFFFu;
    u32 keys[16];
    for (int u = 0; u < 16; u++)
        keys[u] = (s_d3d.cur_texs[u].set && ((mask >> u) & 1u)) ? samp_key_norm(s_d3d.cur_texs[u].samp) : 0u;
    return samp_table_get(keys);
}

typedef struct { float v[4]; } VPSlot;
/* Packed VP vertices (2026-10-04): a vertex holds only slots 0..N-1, N = the
 * highest input register the vertex program reads + 1 (rsx_vp_analyze_inputs;
 * any uncertainty -> 16). The input layout keeps ATTRi at i*16; with a smaller
 * vertex-buffer stride the slots the VS never reads alias the next vertex,
 * which is harmless. inFamous' city frame: ~670 MB -> ~170 MB per frame of
 * upload memory, written by the CPU and read by the GPU every frame.
 * VP_PACK=0 restores the fixed 256-byte vertex. */
static u32 s_vpk = 16;                                 /* slots per vertex, current draw */
#define VP_VERT_STRIDE ((u32)(s_vpk * sizeof(VPSlot)))

/* ---- VP vertex chunks (see s_vpc_*) ---- */
static int vp_chunk_alloc(int par, u32 c)
{
    if (c >= VP_MAX_CHUNKS) return 0;
    if (s_vpc_map[par][c]) return 1;
    if (!s_d3d.device) return 0;
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (u64)s_vp_max_verts * 256u;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE nr = {0, 0};
    ID3D12Resource* r = NULL; void* m = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&r)) || !r)
        return 0;
    if (FAILED(r->lpVtbl->Map(r, 0, &nr, &m)) || !m) { r->lpVtbl->Release(r); return 0; }
    s_vpc_res[par][c] = r; s_vpc_map[par][c] = (unsigned char*)m;
    if (c + 1 > s_vpc_count[par]) s_vpc_count[par] = c + 1;
    return 1;
}

/* Base of the chunk the current draw uploads into. */
static unsigned char* vp_cur_base(void)
{
    return s_vpc_map[s_d3d.vp_parity][s_vpc_cur];
}

/* Vertices a draw occupies once expanded to a triangle list. */
static u32 vp_expanded(u32 primitive, u32 count)
{
    if (primitive == RSX_PRIMITIVE_QUADS) return count / 4 * 6;
    if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP || primitive == RSX_PRIMITIVE_TRIANGLE_FAN)
        return count >= 3 ? 3 * (count - 2) : 0;
    return count;
}

/* v102c FRAME WATCHDOG. inFamous ends a frame ONLY at a display-buffer colour clear (d3d12_clear_inner:
 * needs display draws in the batch + a flip since the last present + >= 1/4 of the largest frame);
 * once such a clear has presented (s_clear_presents > 0) the per-flip ticker present returns at once.
 * play102 (t~200 s) and play102b (t=546.7 s): those boundaries stopped -> one batch accumulated for
 * minutes (VPVB 1 -> 12 chunks), nothing executed, black screen, FIFO down to ~1 frame/s. The v102b
 * ACCUM guard sat in d3d12_present, which inFamous never reaches -- hence 0 [ACCUM] lines.
 * State below: render_frame() entries, flip count at the last one, and WHY every display/offscreen
 * clear since then was not a boundary; reported by frame_stall_report() ([FRAME-STALL]). */
static volatile u32 s_fs_rf_calls = 0;
static ULONGLONG s_fs_rf_tick = 0;
static unsigned s_fs_flip_at_rf = 0;
static u32 s_fs_clr_off = 0, s_fs_clr_nodisp = 0, s_fs_clr_sameflip = 0, s_fs_clr_small = 0, s_fs_clr_accum = 0;
static u32 s_fs_ticker_skips = 0, s_fs_forced = 0;
static u32 s_fs_frame_max_mirror = 0; static unsigned s_fs_last_present_flip_mirror = 0;
static void frame_stall_report(const char* why);
/* v103A FRAME_SPLIT (default 1; 0 = old): the record buffer holds s_max_draws (16384) records per batch and
 * every draw past that was DROPPED silently (vertices still converted) -- the tail of the frame (lighting,
 * transparents, post) vanished and, if no display draw had been recorded yet, no display clear could ever end
 * the batch again. Now a batch that reaches the cap mid-frame is executed right there (without Present) and
 * recording continues in a fresh batch; the parts' record count and display draws count toward the next
 * display-clear boundary (s_split_recs / s_split_disp), and the presented display buffer carries over. */
static u32 s_split_recs = 0; static int s_split_disp = 0;
/* v103A FRAME_STALL_MS (default 250; 0 = off): time-based twin of the v102c flip-based ticker fallback. The
 * ticker only runs when the guest FLIPS; if flips stop (or the same-flip / too-small tests keep failing) while
 * display clears keep coming, a display clear arriving >= FRAME_STALL_MS after the last Present ends the frame
 * anyway (present if the batch drew a display buffer, else execute without Present). */
static ULONGLONG s_fs_pres_tick = 0; static u32 s_fs_clr_forced = 0;
static int frame_stall_ms(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("FRAME_STALL_MS"); v = e ? atoi(e) : 250; if (v < 0) v = 0;
        fprintf(stderr, "[FRAME-STALL] v103A display clears end the frame regardless of the flip/size tests once %d ms passed"
                        " without a Present (FRAME_STALL_MS, 0 = off)%c", v, 10); }
    return v;
}
static u32 s_split_n = 0, s_split_win = 0, s_capfull_win = 0;
static void frame_split_check(void);
static u32 s_batch_epoch;   /* v219l (recs_have_display) */
/* v103A BLACK_PROBE (default 8 = every 8th presented frame; 0 = off): read back the presented image, log
 * [LUMA] every 10 s and [BLACK] when it turns black / recovers, and request a pass capture (BLACK_CAP, default
 * 2 per run, rsxcap/frame_200+) of a black frame. */
static int s_black_cap_req = 0;
/* Make room for `need` vertices: move to the next chunk (allocating it the
 * first time) when the current one cannot hold them. Called before a VP draw
 * captures its record offset. If no chunk can be had, nothing changes and
 * the upload functions truncate as before ([VPVB] OVERFLOW). */
static void vp_reserve(u32 need)
{
    int par = s_d3d.vp_parity;
    if (!s_vpc_map[par][s_vpc_cur]) return;
    if ((u64)s_d3d.vp_vb_offset + (u64)need * VP_VERT_STRIDE <= (u64)s_vp_max_verts * 256u) return;
    if ((u64)need * VP_VERT_STRIDE > (u64)s_vp_max_verts * 256u || s_vpc_nogrow) return;
    u32 nx = s_vpc_cur + 1;
    if (nx >= VP_MAX_CHUNKS) return;
    if (!s_vpc_map[par][nx]) {
        if (!vp_chunk_alloc(par, nx)) {
            s_vpc_nogrow = 1;
            fprintf(stderr, "[VPVB] could not allocate vertex chunk %u (parity %d) -- frames past %u MB truncate%c",
                    (unsigned)nx, par, (unsigned)(((u64)nx * s_vp_max_verts * 256u) >> 20), 10);
            return;
        }
        fprintf(stderr, "[VPVB] grew: parity %d now has %u chunks (%u MB)%c", par,
                (unsigned)s_vpc_count[par],
                (unsigned)(((u64)s_vpc_count[par] * s_vp_max_verts * 256u) >> 20), 10);
        if (s_vpc_count[par] >= 2) frame_stall_report("vertex chunk grew (one batch > 256 MB)");   /* v102c */
    }
    s_vpc_cur = nx;
    s_d3d.vp_vb_offset = 0;
}
char g_rsx_title_base[128] = "ps3recomp";
static u32 s_dbg_last_draws = 0;
static u64 s_req_verts = 0, s_req_draws = 0, s_drop_draws = 0;
/* Raw bind offset of the duck's texture, identified by CONTENT (duck.tga is
 * overwhelmingly yellow, avg 243,191,23). VRAM offsets move between runs, so a
 * hard-coded offset in a filter silently matches nothing and the run looks like
 * "renders nothing" -- which cost real time. DRAW_KEEP_TEX=duck resolves here. */
/* Copy of the last completed frame, bound wherever a draw samples a
 * DISPLAY-SIZED texture. On RSX this title renders its reflection into a corner
 * of the render surface and then samples that surface as a texture; our backend
 * renders into a D3D backbuffer, so the guest memory behind that sampler is
 * never written and it reads empty -- which is why the water had no reflection.
 * Feeding it the previous frame costs one frame of latency, which for a water
 * reflection is not visible. */
static ID3D12Resource* s_screen_copy = NULL;
/* v28: shadow-map stand-in (see the [SHADOW] bind in the prepass). */
static int shadows_on(void)
{
    /* v34 SHADOWS (default 1; 0 = old "everything lit"): a depth-format sampler on a
     * zeta surface is a shadow-map lookup -- bind the depth surface itself and do the
     * compare (+ 2x2 PCF, as the RSX does) in the fragment program. */
    static int v = -1;
    if (v < 0) { const char* e = getenv("SHADOWS"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[SHADOW] real shadow maps %s (SHADOWS)%c", v ? "ON (v34)" : "off", 10); }
    return v;
}
static int shadow_lit_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("SHADOW_LIT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* v30e SHADOW_SKIP (default on while SHADOW_LIT is on; =0 off): with shadow lookups
 * forced to "lit", the shadow-map passes are invisible work -- and they are most of
 * the frame: rsxcap of the city had 7042 of 10272 draws as colour-masked (cmask 0)
 * depth-only draws into zeta 0xF340000 (1024x1024), each converting its vertices on
 * the RSX thread. Skip a draw at record time when it writes NO colour and its depth
 * surface is one that has only ever been sampled as a shadow map (depth-format
 * texture, forced lit) and never through the depth->colour copy (fog/DOF/lighting
 * read 0x1010000 that way, so the main depth is never skipped). The sets are learned
 * from the replay, so the first frame or two still draw the shadow passes. */
#define ZSET_N 16
static u32 s_zset_shadow[ZSET_N]; static int s_zset_shadow_n;
static u32 s_zset_shadow_wh[ZSET_N];   /* w<<16|h of the shadow texture: only same-size passes are skipped */
static u32 s_zset_colour[ZSET_N]; static int s_zset_colour_n;
static u32 s_zset_colour_wh[ZSET_N];   /* v76: the same offset is reused at several dimensions */
static unsigned long long s_shadow_skipped;
static void zset_add_wh(u32* a, u32* awh, int* n, u32 v, u32 wh, const char* kind)
{
    v &= 0x0FFFFFFFu;
    for (int i = 0; i < *n; i++) if (a[i] == v && awh[i] == wh) return;
    if (*n < ZSET_N) {
        a[*n] = v; awh[*n] = wh; (*n)++;
        fprintf(stderr, "[SHADOW_SKIP] zeta 0x%X %ux%u learned as %s%c",
                v, wh >> 16, wh & 0xFFFFu, kind, 10);
    }
}
static int zset_has_wh(const u32* a, const u32* awh, int n, u32 v, u32 wh)
{
    v &= 0x0FFFFFFFu;
    for (int i = 0; i < n; i++) if (a[i] == v && awh[i] == wh) return 1;
    return 0;
}
static int shadow_skip_draw(const rsx_state* st)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SHADOW_SKIP"); on = (e && *e == '0') ? 0 : 1;
                  if (!shadow_lit_on() || shadows_on()) on = 0;   /* v34: the passes are visible now */
                  fprintf(stderr, "[SHADOW_SKIP] %s%c", on ? "ON (v30e)" : "off", 10); }
    if (!on || !st || !s_zset_shadow_n) return 0;
    if (st->color_mask & 0x01010101u) return 0;                 /* writes colour */
    u32 z = st->surface_zeta_offset;
    {   u32 w = st->surface_clip_w ? st->surface_clip_w : st->viewport_w;
        u32 h = st->surface_clip_h ? st->surface_clip_h : st->viewport_h;
        u32 wh = (w << 16) | (h & 0xFFFFu);
        /* v76: inFamous aliases 0x0F340000 as a 1024x1024 shadow atlas and a
         * 512x288 colour-readable depth surface.  The old address-only colour
         * set made the latter disable skipping for the former, leaving ~7000
         * otherwise invisible shadow draws in every city frame. */
        if (zset_has_wh(s_zset_colour, s_zset_colour_wh, s_zset_colour_n, z, wh)) return 0;
        if (!zset_has_wh(s_zset_shadow, s_zset_shadow_wh, s_zset_shadow_n, z, wh)) return 0;
    }
    if ((++s_shadow_skipped % 200000ull) == 1)
        fprintf(stderr, "[SHADOW_SKIP] %llu shadow-pass draws skipped so far%c", s_shadow_skipped, 10);
    return 1;
}
/* v30e PRESENT_VSYNC (default 0; =1 old): Present(1) blocked the RSX replay thread --
 * the frame limiter -- until the next vblank on every present (up to ~16 ms of a
 * ~110 ms frame). Flip-model windowed present with interval 0 does not tear (DWM). */
volatile int g_win_fullscreen = 0;   /* v194: F11 state, for the settings menu */
static int s_tearing_ok = 0;   /* v209 */
static UINT present_sync_interval(void)
{
    { extern volatile int g_set_vsync; if (g_set_vsync >= 0) return g_set_vsync ? 1u : 0u; }   /* v194 settings menu (live) */
    static int v = -1;
    if (v < 0) { const char* e = getenv("PRESENT_VSYNC"); v = (e && *e == '1') ? 1 : 0;
                 fprintf(stderr, "[D3D12] present sync interval %d (PRESENT_VSYNC)%c", v, 10); }
    return (UINT)v;
}
static ID3D12Resource* s_one_tex = NULL;
static ID3D12Resource* one_tex_get(void)
{
    static int failed = 0;
    if (s_one_tex || failed || !s_d3d.device) return s_one_tex;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 1; td.Height = 1; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
            &IID_ID3D12Resource, (void**)&s_one_tex))) { s_one_tex = NULL; failed = 1; }
    return s_one_tex;
}
/* Sub-viewport render-to-texture. This title renders its reflection/refraction
 * pre-pass into a REGION of the same surface (vp 0,208 512x512 and 0,208
 * 1024x512, cmask=F) and then samples a texture of exactly that size. On
 * hardware the pass lands in the buffer that texture points at; our backend
 * renders into D3D resources, so that guest buffer is never written and every
 * fluid sampler reads zero. Capture each region into a matching resource and
 * bind it for samplers whose guest source is empty and whose size matches.
 *
 * SUBVP_RTT=1 to enable. OFF by default because it cannot be validated on this
 * title yet: the fluid draws that would consume these captures never rasterize
 * (see below), so binding them changes nothing observable. The capture and
 * binding themselves are confirmed working -- SUBVP_DBG shows the 512x512 and
 * 256x256 regions captured and handed to fp 0x5DB81/0x5EA81/0x5EF81/0x5E381. */
#define SUBVP_SLOTS 4
static struct { ID3D12Resource* res; u32 x, y, w, h; } s_subvp[SUBVP_SLOTS];
static int s_subvp_n = 0;

static void subvp_note(u32 x, u32 y, u32 w, u32 h)
{
    if (!w || !h || w > s_d3d.width || h > s_d3d.height) return;
    if (w == s_d3d.width && h == s_d3d.height) return;      /* full-screen pass */
    for (int i = 0; i < s_subvp_n; i++)
        if (s_subvp[i].w == w && s_subvp[i].h == h) { s_subvp[i].x = x; s_subvp[i].y = y; return; }
    if (s_subvp_n >= SUBVP_SLOTS) return;
    s_subvp[s_subvp_n].x = x; s_subvp[s_subvp_n].y = y;
    s_subvp[s_subvp_n].w = w; s_subvp[s_subvp_n].h = h;
    s_subvp[s_subvp_n].res = NULL;
    s_subvp_n++;
}

/* Copy each noted region out of the backbuffer. Called at the same moment as
 * screen_copy_capture -- after the reduced-viewport passes and before the
 * full-screen scene overwrites them. */
static void subvp_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SUBVP_RTT"); en = e ? atoi(e) : 0; }
    if (!en || !s_d3d.device || !s_subvp_n) return;
    for (int i = 0; i < s_subvp_n; i++) {
        if (!s_subvp[i].res) {
            D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td = {0};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = s_subvp[i].w; td.Height = s_subvp[i].h;
            td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                    &IID_ID3D12Resource, (void**)&s_subvp[i].res)))
                { s_subvp[i].res = NULL; continue; }
        }
        D3D12_RESOURCE_BARRIER bb[2] = {0};
        bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb[0].Transition.pResource   = s_d3d.render_targets[fi];
        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb[1] = bb[0];
        bb[1].Transition.pResource   = s_subvp[i].res;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

        D3D12_TEXTURE_COPY_LOCATION dstl = {0}, srcl = {0};
        dstl.pResource = s_subvp[i].res;
        dstl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dstl.SubresourceIndex = 0;
        srcl.pResource = s_d3d.render_targets[fi];
        srcl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; srcl.SubresourceIndex = 0;
        D3D12_BOX box; box.left = s_subvp[i].x; box.top = s_subvp[i].y;
        box.front = 0; box.right = s_subvp[i].x + s_subvp[i].w;
        box.bottom = s_subvp[i].y + s_subvp[i].h; box.back = 1;
        if (box.right > s_d3d.width)  box.right  = s_d3d.width;
        if (box.bottom > s_d3d.height) box.bottom = s_d3d.height;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dstl, 0, 0, 0, &srcl, &box);

        bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);
        { static int _n = 0; if (getenv("SUBVP_DBG") && (_n < 8 && _n++ < 8))
            fprintf(stderr, "[SUBVP] captured %ux%u from (%u,%u)%c",
                    s_subvp[i].w, s_subvp[i].h, s_subvp[i].x, s_subvp[i].y, 10); }
    }
}
static void screen_copy_capture(u32 fi);   /* fwd */
static int  s_sc_dump_pending = 0;
static u32 s_duck_raw = 0;   /* as BOUND (what draw records carry) */
static u32 s_duck_off = 0;   /* as RESOLVED (what the uploader sees)   */
/* PERF=1: where a frame's CPU time actually goes. Guessing at this is how you
 * spend an afternoon optimising the wrong loop. */
static double s_perf_tex = 0.0, s_perf_frame = 0.0, s_perf_vtx = 0.0, s_perf_rf = 0.0;
static double s_perf_gpu = 0.0, s_perf_pre = 0.0, s_perf_srv = 0.0, s_perf_pso = 0.0;
static int s_perf_pso_calls = 0, s_perf_pso_miss = 0, s_perf_pso_hashbytes = 0;
static u64    s_perf_nverts = 0;
static u64    s_perf_vconv = 0;   /* vertices actually converted (vtx memo) */
static u64 s_vib_range_hits = 0, s_vib_hash_hits = 0;   /* v46 indexed draws: range path vs hash path */
static u64    s_perf_vmt = 0;     /* vertices converted on the VTX_MT worker path */
static u64    s_perf_texbytes = 0;
static int    s_perf_ntex = 0;
/* v31: texture upload breakdown (the cutscene hitch windows had tex 3.3-3.7 s of
 * 20 frames): new resources created, re-uploads of changed content, create time. */
static int    s_perf_tex_new = 0, s_perf_tex_reup = 0;
static int    s_perf_tex_cold = 0;
static double s_perf_tex_create = 0.0;
static u64    s_perf_tex_upbytes = 0;
/* v31 TEX_NOTZERO (default 1): create texture + upload resources with
 * D3D12_HEAP_FLAG_CREATE_NOT_ZEROED (0x1000, Win10 2004+). Both are fully written
 * before the GPU reads them. Falls back to NONE (and stays off) if the runtime refuses. */
static D3D12_HEAP_FLAGS tex_heap_flags(void)
{
    static int s = -1;
    if (s < 0) { const char* e = getenv("TEX_NOTZERO"); s = (e && *e == '0') ? 0 : 1; }
    return s ? (D3D12_HEAP_FLAGS)0x1000 : D3D12_HEAP_FLAG_NONE;
}
static void tex_heap_flags_off(void)
{
    static int once = 0;
    if (!once) { once = 1; fprintf(stderr, "[TEX] CREATE_NOT_ZEROED refused by the runtime -> plain heaps%c", 10); }
#ifdef _WIN32
    _putenv("TEX_NOTZERO=0");
#endif
}
static double perf_now(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static int perf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PERF"); v = e ? atoi(e) : 0; }
    return v;
}
/* DBG_LOCK: running clip-space centroid of the tracked mesh (RSX_DBG_VTX's
 * texture), so the debug camera can follow it. The ducks are driven by the
 * physics sim and drift every frame, so a fixed DBG_CENTER loses them as soon
 * as the magnification is high enough to make one recognisable. */
static float s_lock_x = 0.0f, s_lock_y = 0.0f;
static int   s_lock_valid = 0;
/* Accumulated over one frame, published at the frame boundary. Averaging as the
 * frame is recorded makes the aim wander while it is being used, which at high
 * magnification walks the target off-screen entirely. */
static double s_lock_sx = 0.0, s_lock_sy = 0.0;
static u32    s_lock_n  = 0;

/* ---------------------------------------------------------------------------
 * Win32 window
 * -----------------------------------------------------------------------*/

static LRESULT CALLBACK d3d12_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SETCURSOR: {   /* v150: hide the pointer while cellPad's mouse-look owns it */
        extern volatile int g_pad_mouse_captured;
        if (g_pad_mouse_captured && LOWORD(lp) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    }
    case WM_INPUT: {       /* v153: raw mouse counts for cellPad's mouse-look (no pointer acceleration) */
        extern volatile long g_pad_raw_mdx, g_pad_raw_mdy, g_pad_raw_wheel;
        extern volatile int  g_pad_raw_ok;
        RAWINPUT ri; UINT sz = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            InterlockedExchangeAdd(&g_pad_raw_mdx, (LONG)ri.data.mouse.lLastX);
            InterlockedExchangeAdd(&g_pad_raw_mdy, (LONG)ri.data.mouse.lLastY);
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                InterlockedExchangeAdd(&g_pad_raw_wheel, (LONG)(SHORT)ri.data.mouse.usButtonData);
            g_pad_raw_ok = 1;
        }
        break;             /* DefWindowProc does the raw-input cleanup */
    }
    case WM_CLOSE:
        s_d3d.window_closed = 1;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SYSCOMMAND:
        /* F10 (trainer page) and a lone Alt would otherwise enter the window's keyboard-menu mode, where the next
         * arrow key opens the system menu and parks this thread in a modal loop.  lp == 0 = menu activated by a key
         * without a mnemonic; Alt+Space (lp == ' ') and Alt+F4 (SC_CLOSE) still work. */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
        break;
    case WM_KEYDOWN:
        /* v155: Esc no longer closes the game (cellPad maps it to Start = pause menu); close with the
         * window's X. ESC_QUITS=1 restores the old behaviour. F1 = borderless fullscreen <-> window. */
        if (wp == VK_ESCAPE) {
            const char* e = getenv("ESC_QUITS");
            if (e && *e == '1') { s_d3d.window_closed = 1; DestroyWindow(hwnd); }
        } else if (wp == VK_F1 && !(lp & (1 << 30))) {   /* v212: F1 = FPS counter on/off (rsx_ui_overlay.c) */
            extern volatile int g_uiov_show_fps; g_uiov_show_fps = !g_uiov_show_fps;
        } else if (wp == VK_F11 && !(lp & (1 << 30))) {   /* F11 = fullscreen (v212: F1 no longer) */
            static int fs = 0; static WINDOWPLACEMENT wpl; static LONG style = 0;
            if (!fs) {
                MONITORINFO mi; mi.cbSize = sizeof mi;
                wpl.length = sizeof wpl;
                if (GetWindowPlacement(hwnd, &wpl) &&
                    GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
                    style = GetWindowLongA(hwnd, GWL_STYLE);
                    SetWindowLongA(hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
                    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                                 mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                    fs = 1; g_win_fullscreen = 1;
                }
            } else {
                SetWindowLongA(hwnd, GWL_STYLE, style ? style : (WS_OVERLAPPEDWINDOW | WS_VISIBLE));
                SetWindowPlacement(hwnd, &wpl);
                SetWindowPos(hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                             SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                fs = 0; g_win_fullscreen = 0;
            }
        }
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static HWND create_window(u32 width, u32 height, const char* title)
{
    WNDCLASSEXA wc = {0};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = d3d12_wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "ps3recomp_d3d12";
    RegisterClassExA(&wc);

    {   /* v154: a window larger than the screen's work area is shrunk (same aspect); DXGI scales the image */
        RECT wa;
        if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
            const double mw = (double)(wa.right - wa.left) * 0.95, mh = (double)(wa.bottom - wa.top) * 0.90;
            if (mw > 64 && mh > 64 && ((double)width > mw || (double)height > mh)) {
                double k = mw / (double)width; if (mh / (double)height < k) k = mh / (double)height;
                width = (u32)((double)width * k); height = (u32)((double)height * k);
            }
        }
    }
    RECT wr = {0, 0, (LONG)width, (LONG)height};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);

    /* Keep the display awake (see rsx_null_backend.c). */
    SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
    return CreateWindowExA(
        0, "ps3recomp_d3d12",
        title ? title : "ps3recomp (D3D12)",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        wr.right - wr.left, wr.bottom - wr.top,
        NULL, NULL, GetModuleHandle(NULL), NULL);
}

/* ---------------------------------------------------------------------------
 * D3D12 initialization
 * -----------------------------------------------------------------------*/

/* D3D12_DRED=1: Device Removed Extended Data. Turned on before the device is
 * created; after a device removal d3d12_dred_report() prints which command-list
 * op was the last to complete (counted in draws) and, for a GPU page fault, the
 * faulting VA next to the VAs of our big buffers. Needs no SDK debug layer.
 * Added 2026-10-04 for inFamous' first in-world frame (DEVICE_HUNG). */
static void d3d12_dred_enable(void)
{
    if (!getenv("D3D12_DRED")) return;
    ID3D12DeviceRemovedExtendedDataSettings* ds = NULL;
    HRESULT hr = D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void**)&ds);
    if (SUCCEEDED(hr) && ds) {
        ds->lpVtbl->SetAutoBreadcrumbsEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ds->lpVtbl->SetPageFaultEnablement(ds, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ds->lpVtbl->Release(ds);
        printf("[D3D12] DRED enabled (auto-breadcrumbs + page faults)\n");
    } else {
        printf("[D3D12] DRED unavailable (0x%08lX)\n", (unsigned long)hr);
    }
}

static void d3d12_dred_report(void);

static int init_d3d12(u32 width, u32 height)
{
    HRESULT hr;
    d3d12_dred_enable();

    /* Enable debug layer in debug builds */
    /* Debug layer: on in debug builds, or in any build when D3D12_DBG is set
     * (so a Release run can capture exact PSO/validation errors). */
    if (
#ifdef NDEBUG
        getenv("D3D12_DBG")
#else
        1
#endif
    ) {
        ID3D12Debug* debug_controller = NULL;
        hr = D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&debug_controller);
        if (SUCCEEDED(hr) && debug_controller) {
            debug_controller->lpVtbl->EnableDebugLayer(debug_controller);
            debug_controller->lpVtbl->Release(debug_controller);
            printf("[D3D12] Debug layer enabled\n");
        } else {
            printf("[D3D12] Debug layer requested but unavailable (0x%08lX) -- "
                   "install 'Graphics Tools' optional feature\n", hr);
        }
    }

    /* Create DXGI factory */
    IDXGIFactory4* factory = NULL;
    hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&factory);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateDXGIFactory1 failed (0x%08lX)\n", hr);
        return -1;
    }

    /* Create D3D12 device. On a dual-GPU laptop a NULL adapter usually lands on
     * the integrated GPU; explicitly pick the high-performance (discrete)
     * adapter via IDXGIFactory6. Set CELLMARK_IGPU to force the low-power one
     * (for A/B testing a suspected iGPU-driver stall). */
    {
        IDXGIFactory6* factory6 = NULL;
        if (SUCCEEDED(factory->lpVtbl->QueryInterface(
                factory, &IID_IDXGIFactory6, (void**)&factory6))) {
            DXGI_GPU_PREFERENCE pref = getenv("CELLMARK_IGPU")
                ? DXGI_GPU_PREFERENCE_MINIMUM_POWER
                : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
            IDXGIAdapter1* adapter = NULL;
            for (UINT ai = 0; factory6->lpVtbl->EnumAdapterByGpuPreference(
                     factory6, ai, pref, &IID_IDXGIAdapter1, (void**)&adapter)
                     != DXGI_ERROR_NOT_FOUND; ai++) {
                DXGI_ADAPTER_DESC1 ad;
                adapter->lpVtbl->GetDesc1(adapter, &ad);
                if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                    SUCCEEDED(D3D12CreateDevice((IUnknown*)adapter,
                        D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                        (void**)&s_d3d.device))) {
                    printf("[D3D12] adapter: %ls (VRAM %llu MB)\n", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20));
                    fprintf(stderr, "[D3D12] adapter: %ls (VRAM %llu MB, shared %llu MB)%c", ad.Description,
                           (unsigned long long)(ad.DedicatedVideoMemory >> 20),
                           (unsigned long long)(ad.SharedSystemMemory >> 20), 10);
                    if (!s_adapter3)
                        adapter->lpVtbl->QueryInterface(adapter, &IID_IDXGIAdapter3, (void**)&s_adapter3);
                    adapter->lpVtbl->Release(adapter);
                    break;
                }
                adapter->lpVtbl->Release(adapter);
                adapter = NULL;
            }
            factory6->lpVtbl->Release(factory6);
        }
    }
    if (!s_d3d.device) {
        hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0,
                               &IID_ID3D12Device, (void**)&s_d3d.device);
        if (FAILED(hr)) {
            printf("[D3D12] ERROR: D3D12CreateDevice failed (0x%08lX)\n", hr);
            factory->lpVtbl->Release(factory);
            return -1;
        }
        printf("[D3D12] Device created on default adapter (feature level 11.0)\n");
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d3d.device->lpVtbl->CreateCommandQueue(
        s_d3d.device, &queue_desc, &IID_ID3D12CommandQueue, (void**)&s_d3d.cmd_queue);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateCommandQueue failed\n");
        factory->lpVtbl->Release(factory);
        return -1;
    }

#ifdef PS3RECOMP_DLSS
    rsx_dlss_device_init(s_d3d.device, RBW, RBH, BBW, BBH);   /* DLSS=1: start NGX (no-op otherwise) */
    if (s_outw && !rsx_dlss_active()) {   /* upscaling asked for, but no DLSS here: render at the full size after all */
        s_bbw = s_outw; s_bbh = s_outh; s_res_scale = s_out_scale; s_outw = s_outh = 0;
        fprintf(stderr, "[RES] DLSS unavailable: rendering at the output size %ux%u%c", s_bbw, s_bbh, 10);
    }
#endif

    /* Create swap chain */
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {0};
    sc_desc.Width = BBW;     /* v154 */
    sc_desc.Height = BBH;
    sc_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = FRAME_COUNT;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    /* v209: tearing-capable swap chain (DXGI 1.5) so "VSync Off" can present immediately -- with a G-Sync/FreeSync
     * display in fullscreen this is what lets variable refresh follow the game's frame rate. PRESENT_TEARING=0 = old. */
    { IDXGIFactory5* f5 = NULL;
      if (SUCCEEDED(factory->lpVtbl->QueryInterface(factory, &IID_IDXGIFactory5, (void**)&f5)) && f5) {
          BOOL t = FALSE;
          if (SUCCEEDED(f5->lpVtbl->CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &t, sizeof t)) && t) s_tearing_ok = 1;
          f5->lpVtbl->Release(f5); }
      { const char* e = getenv("PRESENT_TEARING"); if (!(e && *e == '1')) s_tearing_ok = 0; }   /* v209b: opt-in (VSync Off keeps the old no-tear present) */
      if (s_tearing_ok) sc_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
      fprintf(stderr, "[D3D12] v209 tearing / variable refresh present: %s%c", s_tearing_ok ? "supported" : "off", 10); }

    IDXGISwapChain1* swap_chain1 = NULL;
    hr = factory->lpVtbl->CreateSwapChainForHwnd(
        factory, (IUnknown*)s_d3d.cmd_queue,
        s_d3d.hwnd, &sc_desc, NULL, NULL, &swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: CreateSwapChainForHwnd failed (0x%08lX)\n", hr);
        factory->lpVtbl->Release(factory);
        return -1;
    }

    /* Disable Alt+Enter fullscreen toggle */
    factory->lpVtbl->MakeWindowAssociation(factory, s_d3d.hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->lpVtbl->Release(factory);

    /* Query SwapChain3 interface */
    hr = swap_chain1->lpVtbl->QueryInterface(
        swap_chain1, &IID_IDXGISwapChain3, (void**)&s_d3d.swap_chain);
    swap_chain1->lpVtbl->Release(swap_chain1);
    if (FAILED(hr)) {
        printf("[D3D12] ERROR: QueryInterface for SwapChain3 failed\n");
        return -1;
    }

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

#ifdef PS3RECOMP_DLSS
    /* FSR_FG=1: AMD FSR frame generation replaces the swap chain with its proxy (which presents real + generated frames) */
    { const int fgw = rsx_dlss_wrap_swapchain((void**)&s_d3d.swap_chain, s_d3d.cmd_queue);
      if (fgw < 0) { printf("[D3D12] ERROR: no swap chain left after the frame generation wrap\n"); return -1; }
      if (fgw > 0) s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain); }
#endif
    /* Create RTV descriptor heap */
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {0};
    rtv_heap_desc.NumDescriptors = FRAME_COUNT;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
        s_d3d.device, &rtv_heap_desc, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rtv_heap);
    if (FAILED(hr)) return -1;

    s_d3d.rtv_descriptor_size = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    /* Create RTVs for each frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    RSX_D3D12_CPU_HEAP_START(s_d3d.rtv_heap)(s_d3d.rtv_heap, &rtv_handle);

    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.swap_chain->lpVtbl->GetBuffer(
            s_d3d.swap_chain, i, &IID_ID3D12Resource, (void**)&s_d3d.render_targets[i]);
        if (FAILED(hr)) return -1;

        s_d3d.device->lpVtbl->CreateRenderTargetView(
            s_d3d.device, s_d3d.render_targets[i], NULL, rtv_handle);
        rtv_handle.ptr += s_d3d.rtv_descriptor_size;
    }

    /* ---------------------------------------------------------------
     * Depth/stencil buffer
     * 24-bit depth + 8-bit stencil (DXGI_FORMAT_D24_UNORM_S8_UINT).
     * One shared depth texture across both frames — RSX games on PS3
     * typically use a single zeta surface.
     * ---------------------------------------------------------------*/
    {
        D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc = {0};
        dsv_heap_desc.NumDescriptors = 1;
        dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
            s_d3d.device, &dsv_heap_desc, &IID_ID3D12DescriptorHeap,
            (void**)&s_d3d.dsv_heap);
        if (FAILED(hr)) {
            printf("[D3D12] DSV heap creation failed\n");
            return -1;
        }

        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC depth_desc = {0};
        depth_desc.Dimension          = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        /* Sized to cover the LARGEST render target, not just the window:
         * D3D12 clips rasterization to the smallest bound attachment, and
         * this DSV is shared by every pass -- with a window-sized depth
         * buffer, draws into larger offscreen RTs (wave's 1920x1080 input
         * image) were silently cropped to the window rect. */
        depth_desc.Width              = BBW > 2048 ? BBW : 2048;   /* v154: >= the internal resolution */
        depth_desc.Height             = BBW > 2048 ? BBW : 2048;
        depth_desc.DepthOrArraySize   = 1;
        depth_desc.MipLevels          = 1;
        depth_desc.Format             = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_desc.SampleDesc.Count   = 1;
        depth_desc.Layout             = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depth_desc.Flags              = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE depth_clear = {0};
        depth_clear.Format                       = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depth_clear.DepthStencil.Depth           = 1.0f;
        depth_clear.DepthStencil.Stencil         = 0;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &depth_desc, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear,
            &IID_ID3D12Resource, (void**)&s_d3d.depth_buffer);
        if (FAILED(hr)) {
            printf("[D3D12] Depth buffer creation failed (0x%08lX)\n", hr);
            return -1;
        }

        D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc = {0};
        dsv_desc.Format         = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dsv_desc.ViewDimension  = D3D12_DSV_DIMENSION_TEXTURE2D;

        D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
        RSX_D3D12_CPU_HEAP_START(s_d3d.dsv_heap)(s_d3d.dsv_heap, &dsv_handle);
        s_d3d.device->lpVtbl->CreateDepthStencilView(
            s_d3d.device, s_d3d.depth_buffer, &dsv_desc, dsv_handle);

        printf("[D3D12] Depth buffer created (%ux%u D24S8)\n", width, height);
    }

    /* Create command allocators and command list */
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        hr = s_d3d.device->lpVtbl->CreateCommandAllocator(
            s_d3d.device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void**)&s_d3d.cmd_allocators[i]);
        if (FAILED(hr)) return -1;
    }

    hr = s_d3d.device->lpVtbl->CreateCommandList(
        s_d3d.device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_d3d.cmd_allocators[0], NULL,
        &IID_ID3D12GraphicsCommandList, (void**)&s_d3d.cmd_list);
    if (FAILED(hr)) return -1;

    /* Close the command list (it starts in recording state) */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);

    /* Create fence */
    hr = s_d3d.device->lpVtbl->CreateFence(
        s_d3d.device, 0, D3D12_FENCE_FLAG_NONE,
        &IID_ID3D12Fence, (void**)&s_d3d.fence);
    if (FAILED(hr)) return -1;

    s_d3d.fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    memset(s_d3d.fence_values, 0, sizeof(s_d3d.fence_values));

    /* ---------------------------------------------------------------
     * Create root signature with 16 root constants (one mat4 MVP at b0).
     * Visible only to the vertex shader — pixel shader doesn't need it.
     * ---------------------------------------------------------------*/
    {
        /* param 0: mat4 MVP as 16 root constants at b0 (vertex shader).
         * param 1: one SRV (t0) descriptor table for the atlas texture (pixel).
         * static sampler s0: linear clamp. The plain (untextured) PSO simply
         * never references t0/s0, so it ignores them. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors     = 1;
        srv_range.BaseShaderRegister = 0;   /* t0 */
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER root_params[2] = {0};
        root_params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        root_params[0].Constants.ShaderRegister = 0;   /* b0 */
        root_params[0].Constants.RegisterSpace  = 0;
        root_params[0].Constants.Num32BitValues = 16;  /* mat4 */
        root_params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_VERTEX;
        root_params[1].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        root_params[1].DescriptorTable.NumDescriptorRanges = 1;
        root_params[1].DescriptorTable.pDescriptorRanges   = &srv_range;
        root_params[1].ShaderVisibility         = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {0};
        samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister   = 0;   /* s0 */
        samp.MaxLOD           = D3D12_FLOAT32_MAX;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rs_desc = {0};
        rs_desc.NumParameters     = 2;
        rs_desc.pParameters       = root_params;
        rs_desc.NumStaticSamplers = 1;
        rs_desc.pStaticSamplers   = &samp;
        rs_desc.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS
                              | D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        ID3DBlob* signature_blob = NULL;
        ID3DBlob* error_blob = NULL;
        hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature_blob, &error_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature serialization failed: %s\n",
                   error_blob ? (const char*)error_blob->lpVtbl->GetBufferPointer(error_blob) : "?");
            if (error_blob) error_blob->lpVtbl->Release(error_blob);
            return -1;
        }

        hr = s_d3d.device->lpVtbl->CreateRootSignature(
            s_d3d.device, 0,
            signature_blob->lpVtbl->GetBufferPointer(signature_blob),
            signature_blob->lpVtbl->GetBufferSize(signature_blob),
            &IID_ID3D12RootSignature, (void**)&s_d3d.root_signature);
        signature_blob->lpVtbl->Release(signature_blob);
        if (FAILED(hr)) {
            printf("[D3D12] Root signature creation failed\n");
            return -1;
        }
    }

    /* ---------------------------------------------------------------
     * Compile shaders and create PSO
     * ---------------------------------------------------------------*/
    {
        /* Basic vertex-colored shader.
         * The MVP matrix arrives via root constants as 4 vec4 columns
         * (PS3/OpenGL column-major convention). We multiply explicitly so
         * we don't depend on HLSL's matrix packing — matches PS3 semantics
         * `gl_Position = MVP * vec4(pos, 1.0)`. */
        static const char vs_hlsl[] =
            "cbuffer cb0 : register(b0) {\n"
            "    float4 mvp_col0;\n"
            "    float4 mvp_col1;\n"
            "    float4 mvp_col2;\n"
            "    float4 mvp_col3;\n"
            "};\n"
            "struct VSInput  { float3 pos : POSITION; float4 col : COLOR; };\n"
            "struct VSOutput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "VSOutput main(VSInput i) {\n"
            "    VSOutput o;\n"
            "    float4 p = float4(i.pos, 1.0);\n"
            "    o.pos = mvp_col0 * p.x + mvp_col1 * p.y + mvp_col2 * p.z + mvp_col3 * p.w;\n"
            "    o.col = i.col;\n"
            "    return o;\n"
            "}\n";
        static const char ps_hlsl[] =
            "struct PSInput { float4 pos : SV_POSITION; float4 col : COLOR; };\n"
            "float4 main(PSInput i) : SV_TARGET { return i.col; }\n";

        ID3DBlob* vs_blob = NULL;
        ID3DBlob* ps_blob = NULL;
        ID3DBlob* err = NULL;

        hr = D3DCompile(vs_hlsl, sizeof(vs_hlsl) - 1, "vs_basic", NULL, NULL,
                        "main", "vs_5_0", 0, 0, &vs_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] VS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        hr = D3DCompile(ps_hlsl, sizeof(ps_hlsl) - 1, "ps_basic", NULL, NULL,
                        "main", "ps_5_0", 0, 0, &ps_blob, &err);
        if (FAILED(hr)) {
            printf("[D3D12] PS compile failed: %s\n",
                   err ? (const char*)err->lpVtbl->GetBufferPointer(err) : "unknown");
            if (err) err->lpVtbl->Release(err);
        }

        if (vs_blob && ps_blob) {
            D3D12_INPUT_ELEMENT_DESC input_layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28,
                 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            };

            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {0};
            pso_desc.pRootSignature = s_d3d.root_signature;
            pso_desc.VS.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob);
            pso_desc.VS.BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob);
            pso_desc.PS.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob);
            pso_desc.PS.BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob);
            pso_desc.InputLayout.pInputElementDescs = input_layout;
            pso_desc.InputLayout.NumElements = 3;
            pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
            pso_desc.SampleMask = UINT_MAX;
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso_desc.NumRenderTargets = 1;
            pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            pso_desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
            /* Depth test enabled, write enabled, LESS func.
             * Games with depth_test_enable=0 in RSX state can still render —
             * LESS just means new-z must be less than existing — but future
             * work should mirror RSX depth state into a PSO cache. */
            pso_desc.DepthStencilState.DepthEnable    = TRUE;
            pso_desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pso_desc.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pso_desc.DepthStencilState.StencilEnable  = FALSE;
            pso_desc.SampleDesc.Count = 1;

            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state);
            if (SUCCEEDED(hr)) {
                s_d3d.pipeline_ready = 1;
                printf("[D3D12] Pipeline state created (triangle class)\n");
            } else {
                printf("[D3D12] PSO TRIANGLE creation failed (0x%08lX)\n", hr);
            }

            /* Line-class PSO — same shader, LINE topology type. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_lines);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (line class)\n");
            else printf("[D3D12] PSO LINE creation failed (0x%08lX)\n", hr);

            /* Point-class PSO. */
            pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pso_desc,
                &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_points);
            if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (point class)\n");
            else printf("[D3D12] PSO POINT creation failed (0x%08lX)\n", hr);

            vs_blob->lpVtbl->Release(vs_blob);
            ps_blob->lpVtbl->Release(ps_blob);
        }

        /* -----------------------------------------------------------------
         * Textured triangle PSO (dbgfont / 2D atlas quads). Same MVP VS but
         * carrying UV; the PS samples the single-channel atlas as coverage
         * and modulates the vertex color, with straight alpha blending so
         * glyph edges composite over the framebuffer. Depth off (2D overlay).
         * ----------------------------------------------------------------*/
        {
            static const char vs_tex[] =
                "cbuffer cb0 : register(b0){float4 c0;float4 c1;float4 c2;float4 c3;};\n"
                "struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "struct VSOut{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "VSOut main(VSIn i){ VSOut o; float4 p=float4(i.pos,1.0);\n"
                "  o.pos=c0*p.x+c1*p.y+c2*p.z+c3*p.w; o.col=i.col; o.uv=i.uv; return o; }\n";
            static const char ps_tex[] =
                "Texture2D tex : register(t0);\n"
                "SamplerState smp : register(s0);\n"
                "struct PSIn{ float4 pos:SV_POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };\n"
                "float4 main(PSIn i):SV_TARGET{\n"
                /* dbgfont texcoords are compressed ~10x (U) / ~8x (V) vs. the
                 * atlas; scale to recover glyph cells. Exact per-glyph offset
                 * still being calibrated. */
                "  float2 uv2 = float2(i.uv.x*10.0, i.uv.y*8.0 + 0.59);\n"
                "  float cov = tex.Sample(smp, uv2).r;\n"
                "  return float4(i.col.rgb, i.col.a * cov); }\n";

            ID3DBlob *vtb = NULL, *ptb = NULL, *e2 = NULL;
            hr = D3DCompile(vs_tex, sizeof(vs_tex) - 1, "vs_tex", NULL, NULL, "main", "vs_5_0", 0, 0, &vtb, &e2);
            if (FAILED(hr)) printf("[D3D12] VS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");
            hr = D3DCompile(ps_tex, sizeof(ps_tex) - 1, "ps_tex", NULL, NULL, "main", "ps_5_0", 0, 0, &ptb, &e2);
            if (FAILED(hr)) printf("[D3D12] PS(tex) compile failed: %s\n", e2 ? (const char*)e2->lpVtbl->GetBufferPointer(e2) : "?");

            if (vtb && ptb) {
                D3D12_INPUT_ELEMENT_DESC il[] = {
                    {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                    {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                };
                D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
                pd.pRootSignature = s_d3d.root_signature;
                pd.VS.pShaderBytecode = vtb->lpVtbl->GetBufferPointer(vtb);
                pd.VS.BytecodeLength  = vtb->lpVtbl->GetBufferSize(vtb);
                pd.PS.pShaderBytecode = ptb->lpVtbl->GetBufferPointer(ptb);
                pd.PS.BytecodeLength  = ptb->lpVtbl->GetBufferSize(ptb);
                pd.InputLayout.pInputElementDescs = il;
                pd.InputLayout.NumElements = 3;
                pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                pd.BlendState.RenderTarget[0].BlendEnable    = TRUE;
                pd.BlendState.RenderTarget[0].SrcBlend       = D3D12_BLEND_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOp        = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].SrcBlendAlpha  = D3D12_BLEND_ONE;
                pd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
                pd.BlendState.RenderTarget[0].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
                pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                pd.SampleMask = UINT_MAX;
                pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                pd.NumRenderTargets = 1;
                pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
                pd.DepthStencilState.DepthEnable = FALSE;
                pd.DepthStencilState.StencilEnable = FALSE;
                pd.SampleDesc.Count = 1;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_tex);
                if (SUCCEEDED(hr)) printf("[D3D12] Pipeline state created (textured class)\n");
                else printf("[D3D12] PSO TEX creation failed (0x%08lX)\n", hr);
            }
            if (vtb) vtb->lpVtbl->Release(vtb);
            if (ptb) ptb->lpVtbl->Release(ptb);
        }

        /* SRV descriptor heap (shader-visible). Layout: slot 0 = legacy atlas
         * (dbgfont / textured 2D path), slots 1-4 = per-draw VP textures,
         * 5-20 = offscreen render targets (render-to-texture), rest spare.
         * All slots start as null SRVs so any 4-wide table window (the
         * root signature binds t0-t3) is valid on tier-1 hardware. */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = SRV_HEAP_SIZE;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.srv_heap);
            if (FAILED(hr)) printf("[D3D12] SRV heap creation failed (0x%08lX)\n", hr);
            else {
                s_d3d.srv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_SHADER_RESOURCE_VIEW_DESC nv = {0};
                nv.Format = DXGI_FORMAT_R8_UNORM;
                nv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                nv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                nv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE hh;
                RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &hh);
                for (int _i = 0; _i < SRV_HEAP_SIZE; _i++) {
                    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, NULL, &nv, hh);
                    hh.ptr += s_d3d.srv_inc;
                }
            }
        }

        /* RTV heap for offscreen render targets (CPU-visible only). */
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = MAX_OFF_RTS;
            hr = s_d3d.device->lpVtbl->CreateDescriptorHeap(
                s_d3d.device, &hd, &IID_ID3D12DescriptorHeap, (void**)&s_d3d.rt_rtv_heap);
            if (FAILED(hr)) printf("[D3D12] offscreen RTV heap creation failed (0x%08lX)\n", hr);
        }
    }

    /* ---------------------------------------------------------------
     * Create dynamic vertex buffer (upload heap, 4MB)
     * ---------------------------------------------------------------*/
    {
        D3D12_HEAP_PROPERTIES heap_props = {0};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;

        D3D12_RESOURCE_DESC buf_desc = {0};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Width = MAX_VERTICES * VERTEX_STRIDE;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &heap_props, D3D12_HEAP_FLAG_NONE,
            &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.vertex_buffer);
        if (SUCCEEDED(hr)) {
            D3D12_RANGE read_range = {0, 0};
            s_d3d.vertex_buffer->lpVtbl->Map(
                s_d3d.vertex_buffer, 0, &read_range, &s_d3d.vb_mapped);
            s_d3d.vb_view.BufferLocation =
                s_d3d.vertex_buffer->lpVtbl->GetGPUVirtualAddress(s_d3d.vertex_buffer);
            s_d3d.vb_view.StrideInBytes = VERTEX_STRIDE;
            s_d3d.vb_view.SizeInBytes = MAX_VERTICES * VERTEX_STRIDE;
            printf("[D3D12] Vertex buffer created (%u KB)\n",
                   (MAX_VERTICES * VERTEX_STRIDE) / 1024);
        }
    }

    /* ---------------------------------------------------------------
     * Real vertex-program path resources: root signature (CBV b0 for the
     * vp_c[] bank + SRV t0 + static sampler s0), a raw-float4 vertex buffer,
     * and the constant-bank buffer. The PSO itself is built lazily once the
     * game uploads its VP microcode (render_frame).
     * ---------------------------------------------------------------*/
    {
        /* 4-descriptor SRV table (t0-t3) so decompiled fragment programs can
         * sample up to 4 texture units; the hardcoded atlas/colour PSs use only
         * t0 and are unaffected. Matching 4 static samplers s0-s3. */
        D3D12_DESCRIPTOR_RANGE srv_range = {0};
        srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srv_range.NumDescriptors = RSX_D3D_TEX_UNITS;
        srv_range.BaseShaderRegister = 0;
        srv_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

        D3D12_ROOT_PARAMETER rp[3] = {0};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b0 = vp_c bank */
        rp[0].Descriptor.ShaderRegister = 0;
        rp[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp[1].DescriptorTable.NumDescriptorRanges = 1;
        rp[1].DescriptorTable.pDescriptorRanges = &srv_range;
        rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;   /* b1 = FP texscale */
        rp[2].Descriptor.ShaderRegister = 1;
        rp[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp[RSX_D3D_TEX_UNITS] = {0};
        for (int _s = 0; _s < RSX_D3D_TEX_UNITS; _s++) {
            samp[_s].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp[_s].AddressU = samp[_s].AddressV = samp[_s].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp[_s].ShaderRegister = (UINT)_s;
            samp[_s].MaxLOD = D3D12_FLOAT32_MAX;
            samp[_s].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        }
        /* s0 keeps point/clamp: the dbgfont atlas PS samples glyph cells and
         * linear filtering bleeds neighbouring glyphs. */
        samp[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samp[0].AddressU = samp[0].AddressV = samp[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

        D3D12_ROOT_SIGNATURE_DESC rd = {0};
        rd.NumParameters = 3; rd.pParameters = rp;
        rd.NumStaticSamplers = RSX_D3D_TEX_UNITS; rd.pStaticSamplers = samp;
        /* 2026-10-04 (v27, textures): the guest's per-unit TEXTURE_FILTER /
         * TEXTURE_ADDRESS were ignored -- s0 was POINT/CLAMP for every draw (a
         * dbgfont leftover) and s1-s15 LINEAR/WRAP, so inFamous' diffuse maps
         * (unit 0) were nearest-sampled = blocky, and tiling textures clamped.
         * Root param 3 = a 16-sampler table built per draw from the guest regs
         * (samp_table_for_cur). SAMP_GUEST=0 -> the old static samplers. */
        D3D12_ROOT_PARAMETER rp4[4];
        D3D12_DESCRIPTOR_RANGE samp_range = {0};
        if (samp_guest_on()) {
            D3D12_DESCRIPTOR_HEAP_DESC shd = {0};
            shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            shd.NumDescriptors = SAMP_HEAP_DESCS;   /* v156 (= the old 128 x 16) */
            shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &shd,
                    &IID_ID3D12DescriptorHeap, (void**)&s_d3d.samp_heap)))
                s_d3d.samp_heap = NULL;
            if (s_d3d.samp_heap) {
                s_samp_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
                    s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
                samp_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
                samp_range.NumDescriptors = RSX_D3D_TEX_UNITS;
                samp_range.BaseShaderRegister = 0;
                samp_range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
                memcpy(rp4, rp, sizeof rp);
                memset(&rp4[3], 0, sizeof rp4[3]);
                rp4[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
                rp4[3].DescriptorTable.NumDescriptorRanges = 1;
                rp4[3].DescriptorTable.pDescriptorRanges = &samp_range;
                rp4[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
                rd.NumParameters = 4; rd.pParameters = rp4;
                rd.NumStaticSamplers = 0; rd.pStaticSamplers = NULL;
                { u32 zk[RSX_D3D_TEX_UNITS] = {0}; samp_table_get(zk); }   /* table 0 = defaults */
                fprintf(stderr, "[SAMP] guest sampler tables on (%d max)%c", samp_pack_on() ? SAMP_TBL_MAX : SAMP_HEAP_DESCS / 16, 10);
            }
        }
        rd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ID3DBlob* sig = NULL; ID3DBlob* err = NULL;
        hr = D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (SUCCEEDED(hr)) {
            s_d3d.device->lpVtbl->CreateRootSignature(s_d3d.device, 0,
                sig->lpVtbl->GetBufferPointer(sig), sig->lpVtbl->GetBufferSize(sig),
                &IID_ID3D12RootSignature, (void**)&s_d3d.vp_root_sig);
            sig->lpVtbl->Release(sig);
        } else if (err) { printf("[D3D12] VP root sig: %s\n", (const char*)err->lpVtbl->GetBufferPointer(err)); err->lpVtbl->Release(err); }

        /* raw float4 vertex buffer + constant bank (both UPLOAD, persistently mapped) */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_RANGE nr = {0,0};

        /* generic VP vertex: 16 float4 slots, DOUBLE-buffered by frame parity so
         * a new frame's upload never overwrites vertices the in-flight GPU frame
         * is still reading. Failure would leave vp_vb_mapped NULL and every VP
         * draw silently declines to upload -- so halve until it fits. */
        { const char* e = getenv("VP_VB_VERTS");   /* now: vertices per chunk */
          if (e && strtoul(e, NULL, 0) >= 65536u) s_vp_max_verts = (u32)strtoul(e, NULL, 0);
          if (s_vp_max_verts > 4194304u) s_vp_max_verts = 4194304u; }
        for (;;) {
            fprintf(stderr, "[VPVB] vertex chunks: %u MB each (%u verts), up to %d per frame, x2 parity%c",
                    (unsigned)(((u64)s_vp_max_verts * 256) >> 20), (unsigned)s_vp_max_verts, VP_MAX_CHUNKS, 10);
            if (vp_chunk_alloc(0, 0) && vp_chunk_alloc(1, 0) && (rf_npar() < 3 || vp_chunk_alloc(2, 0))) {
                s_d3d.vp_vb = s_vpc_res[0][0];
                s_d3d.vp_vb_mapped = s_vpc_map[0][0];
                break;
            }
            for (int _p = 0; _p < 3; _p++) if (s_vpc_res[_p][0]) {
                s_vpc_res[_p][0]->lpVtbl->Release(s_vpc_res[_p][0]);
                s_vpc_res[_p][0] = NULL; s_vpc_map[_p][0] = NULL; s_vpc_count[_p] = 0; }
            if (s_vp_max_verts <= 65536u) {
                fprintf(stderr, "[VPVB] ALLOCATION FAILED -- no VP geometry will render%c", 10);
                break;
            }
            s_vp_max_verts /= 2;
        }

        /* Per-draw constant slots (b0, VP_CB_STRIDE) and FP slots (b1), x2
         * parity, for s_max_draws records. A failure here is silent and
         * catastrophic (vp_record_cb returns early, every draw loses its
         * constants and the scene transforms by zeros), so halve the record
         * cap until both fit, and say so. */
        for (;;) {
            bd.Width = (u64)VP_CB_STRIDE * s_max_draws * (u64)rf_npar();
            fprintf(stderr, "[VPCB] per-draw constant buffer: %llu MB (%u draws)%c",
                    (unsigned long long)(bd.Width >> 20), (unsigned)s_max_draws, 10);
            int ok = 0;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                    D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&s_d3d.vp_cb))) {
                s_d3d.vp_cb->lpVtbl->Map(s_d3d.vp_cb, 0, &nr, &s_d3d.vp_cb_mapped);
                bd.Width = (u64)VP_FPCB_STRIDE * s_max_draws * (u64)rf_npar();
                if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
                        D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                        &IID_ID3D12Resource, (void**)&s_d3d.vp_fpcb))) {
                    s_d3d.vp_fpcb->lpVtbl->Map(s_d3d.vp_fpcb, 0, &nr, &s_d3d.vp_fpcb_mapped);
                    ok = 1;
                } else {
                    s_d3d.vp_cb->lpVtbl->Release(s_d3d.vp_cb);
                    s_d3d.vp_cb = NULL; s_d3d.vp_cb_mapped = NULL;
                }
            }
            if (ok && s_d3d.vp_cb_mapped && s_d3d.vp_fpcb_mapped) break;
            if (s_max_draws <= 1024u) {
                fprintf(stderr, "[VPCB] ALLOCATION FAILED -- VP draws will have no constants%c", 10);
                break;
            }
            s_max_draws /= 2;
        }

        printf("[D3D12] VP path resources: rootsig=%p vb=%p cb=%p\n",
               (void*)s_d3d.vp_root_sig, (void*)s_d3d.vp_vb, (void*)s_d3d.vp_cb);
    }

    printf("[D3D12] Initialization complete (%ux%u, %u buffers, pipeline=%s)\n",
           width, height, FRAME_COUNT,
           s_d3d.pipeline_ready ? "ready" : "NOT ready");
    return 0;
}

/* ---------------------------------------------------------------------------
 * Frame sync helpers
 * -----------------------------------------------------------------------*/

static void wait_for_gpu(void)
{
    u32 fi = s_d3d.frame_index;
    s_d3d.fence_values[fi]++;
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, s_d3d.fence_values[fi]);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[fi]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[fi], s_d3d.fence_event);
        /* NEVER wait unbounded here: this runs on the vblank ticker, which also
         * drives the guest's vblank handlers, the FIFO drain, and the fence
         * publication. A device removal (TDR) leaves the D3D12 fence unsignaled
         * forever and an INFINITE wait silently froze the ENTIRE emulation --
         * observed on LBP as a boot that died the moment the 2048x2048
         * offscreen RT was created (last log line), every guest thread parked.
         * Degrade loudly instead: bounded waits + the removal reason, then
         * carry on (subsequent D3D calls fail visibly but the game keeps
         * running headless). */
        for (int tries = 0; tries < 3; tries++) {
            if (WaitForSingleObject(s_d3d.fence_event, 2000) != WAIT_TIMEOUT)
                return;
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            fprintf(stderr, "[D3D12] wait_for_gpu STUCK %ds: want %llu got %llu removed=0x%08lX\n",
                   2 * (tries + 1),
                   (unsigned long long)s_d3d.fence_values[fi],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stderr);
            if (rr != 0 /* S_OK: device still alive, keep waiting a bit */)
                break;
        }
    }
}

static void move_to_next_frame(void)
{
    u64 current_fence = s_d3d.fence_values[s_d3d.frame_index];
    s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue, s_d3d.fence, current_fence);

    s_d3d.frame_index = s_d3d.swap_chain->lpVtbl->GetCurrentBackBufferIndex(s_d3d.swap_chain);

    if (s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence) < s_d3d.fence_values[s_d3d.frame_index]) {
        s_d3d.fence->lpVtbl->SetEventOnCompletion(
            s_d3d.fence, s_d3d.fence_values[s_d3d.frame_index], s_d3d.fence_event);
        if (WaitForSingleObject(s_d3d.fence_event, 2000) == WAIT_TIMEOUT) {
            HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
            fprintf(stderr, "[D3D12] FENCE STUCK 2s: want %llu got %llu removed=0x%08lX\n",
                   (unsigned long long)s_d3d.fence_values[s_d3d.frame_index],
                   (unsigned long long)s_d3d.fence->lpVtbl->GetCompletedValue(s_d3d.fence),
                   (long)rr);
            fflush(stderr);
        }
    }

    s_d3d.fence_values[s_d3d.frame_index] = current_fence + 1;
}

/* ---------------------------------------------------------------------------
 * Render a frame (clear + present)
 * -----------------------------------------------------------------------*/

/* Write the mapped readback buffer (R8G8B8A8, row pitch = readback_pitch) out
 * as a 24-bit bottom-up BMP. Debug-only. */
/* Name prefix for the next readback dump: lets the same writer emit the
 * backbuffer and the screen-copy texture under different filenames. */
static const char* s_dump_name = NULL;

static void dump_backbuffer_bmp(void)
{
    if (!s_d3d.readback_buf) return;
    void* mapped = NULL;
    D3D12_RANGE rr = {0, (SIZE_T)s_d3d.readback_pitch * BBH};
    if (FAILED(s_d3d.readback_buf->lpVtbl->Map(s_d3d.readback_buf, 0, &rr, &mapped)) || !mapped)
        return;

    static int idx = 0;
    char path[512];
    const char* dir = getenv("CELLMARK_DUMP_DIR");   /* default: current dir */
    snprintf(path, sizeof(path), "%s%s%s_%03d.bmp",
             dir ? dir : "", dir ? "/" : "", s_dump_name ? s_dump_name : "frame", idx++);
    FILE* f = fopen(path, "wb");
    if (f) {
        u32 w = BBW, h = BBH;
        u32 padded = (w * 3 + 3) & ~3u;
        u32 imgsz  = padded * h;
        u32 filesz = 54 + imgsz;
        unsigned char hdr[54] = {0};
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2]=filesz&0xFF; hdr[3]=(filesz>>8)&0xFF; hdr[4]=(filesz>>16)&0xFF; hdr[5]=(filesz>>24)&0xFF;
        hdr[10]=54; hdr[14]=40;
        hdr[18]=w&0xFF; hdr[19]=(w>>8)&0xFF; hdr[20]=(w>>16)&0xFF; hdr[21]=(w>>24)&0xFF;
        hdr[22]=h&0xFF; hdr[23]=(h>>8)&0xFF; hdr[24]=(h>>16)&0xFF; hdr[25]=(h>>24)&0xFF;
        hdr[26]=1; hdr[28]=24;
        hdr[34]=imgsz&0xFF; hdr[35]=(imgsz>>8)&0xFF; hdr[36]=(imgsz>>16)&0xFF; hdr[37]=(imgsz>>24)&0xFF;
        fwrite(hdr, 1, 54, f);
        unsigned char* row = (unsigned char*)malloc(padded);
        if (row) {
            memset(row, 0, padded);
            for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
                unsigned char* src = (unsigned char*)mapped + (u32)y * s_d3d.readback_pitch;
                for (u32 x = 0; x < w; x++) {
                    row[x*3+0] = src[x*4+2]; /* B */
                    row[x*3+1] = src[x*4+1]; /* G */
                    row[x*3+2] = src[x*4+0]; /* R */
                }
                fwrite(row, 1, padded, f);
            }
            free(row);
        }
        fclose(f);
        printf("[D3D12] dumped %s (%ux%u)\n", path, BBW, BBH);
    }
    D3D12_RANGE wr = {0, 0};
    s_d3d.readback_buf->lpVtbl->Unmap(s_d3d.readback_buf, 0, &wr);
}

/* ---------------------------------------------------------------------------
 * RSX frame capture ("rsxcap") -- a purpose-built GPU debugger. RSX_CAP=start
 * [:count[:stride]] snapshots whole frames to <RSX_CAP_DIR>/frame_NN/: a text
 * manifest of every op (in submission order, with full surface/shader/texture
 * state) plus a BMP of the backbuffer and every offscreen colour RT used that
 * frame. One frame, captured atomically -- no cross-frame guessing.
 * -----------------------------------------------------------------------*/

/* Shared 24-bit BMP writer for readbacks. R8G8B8A8_UNORM straight; the half/
 * float RT formats are |v|-tonemapped so signed/HDR data is visible. */
static void cap_write_bmp(const char* path, const void* mapped, u32 w, u32 h,
                          u32 pitch, u32 dxgi)
{
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 rowb = (w * 3 + 3) & ~3u, datasz = rowb * h;
    u8 hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54; *(u32*)(hdr+14) = 40;
    *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
    *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
    fwrite(hdr, 1, 54, f);
    u8* line = (u8*)malloc(rowb);
    if (line) for (int y = (int)h - 1; y >= 0; y--) {   /* BMP is bottom-up */
        const u8* srow = (const u8*)mapped + (u64)y * pitch;
        memset(line, 0, rowb);
        for (u32 x = 0; x < w; x++) {
            float rv, gv, bv;
            if (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                const u16* hp = (const u16*)(srow + (u64)x * 8);
                float v[3];
                for (int c = 0; c < 3; c++) {
                    u16 hv = hp[c]; u32 s = (hv>>15)&1, e = (hv>>10)&0x1F, m = hv&0x3FF; float fv;
                    if (e == 0) fv = (float)m / 16777216.0f;
                    else { u32 fb = (s<<31)|((e-15+127)<<23)|(m<<13); memcpy(&fv,&fb,4); }
                    v[c] = fv;
                }
                rv = v[0]; gv = v[1]; bv = v[2];
            } else if (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                const float* fp = (const float*)(srow + (u64)x * 16);
                rv = fp[0]; gv = fp[1]; bv = fp[2];
            } else {
                const u8* p = srow + (u64)x * 4;
                rv = p[0]/255.0f; gv = p[1]/255.0f; bv = p[2]/255.0f;
            }
            float ar = rv<0?-rv:rv, ag = gv<0?-gv:gv, ab = bv<0?-bv:bv;
            if (ar>1) ar=1; if (ag>1) ag=1; if (ab>1) ab=1;
            line[x*3+0] = (u8)(ab*255.0f);
            line[x*3+1] = (u8)(ag*255.0f);
            line[x*3+2] = (u8)(ar*255.0f);
        }
        fwrite(line, 1, rowb, f);
    }
    free(line);
    fclose(f);
}

/* Copy one GPU resource into a fresh readback buffer and write it as a BMP.
 * Self-contained: waits, resets the frame command list, copies, executes,
 * waits, maps. `prior` is the resource's current tracked state (restored after
 * the copy so the caller's state tracking stays valid). */
static void cap_readback_write(ID3D12Resource* res, u32 w, u32 h, u32 dxgi,
                               D3D12_RESOURCE_STATES prior, u32 fi, const char* path)
{
    if (!res || !w || !h) return;
    u32 bpp = (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
              (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
    u32 pitch = (w * bpp + 255) & ~255u;

    ID3D12Resource* rb = NULL;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (u64)pitch * h; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&rb)) || !rb)
        return;

    wait_for_gpu();
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    cmd_state_reset();

    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = prior;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)dxgi;
    dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    src.pResource = res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        /* TEXUP_DBG=1: texture uploads per second. A cache that misses every
         * draw re-uploads megabytes per frame, which is the kind of GPU work
         * that trips a 2-second TDR while looking like nothing in a draw count. */
        { static int td = -1; if (td < 0) td = getenv("TEXUP_DBG") ? 1 : 0;
          if (td) { static long long n = 0, bytes = 0; static clock_t t0 = 0;
              n++; bytes += (long long)pitch * h;
              const clock_t now = clock();
              if (!t0) t0 = now;
              if (now - t0 >= CLOCKS_PER_SEC) {
                  fprintf(stderr, "[TEXUP] %lld uploads/s, %.1f MB/s\n",
                          n, bytes / 1048576.0);
                  n = 0; bytes = 0; t0 = now; } } }

    if (prior != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter  = prior;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }

    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cl[] = { (ID3D12CommandList*)s_d3d.cmd_list };
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cl);
    wait_for_gpu();

    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)pitch * h};
    if (SUCCEEDED(rb->lpVtbl->Map(rb, 0, &rr, &mp)) && mp) {
        cap_write_bmp(path, mp, w, h, pitch, dxgi);
        D3D12_RANGE wr = {0, 0};
        rb->lpVtbl->Unmap(rb, 0, &wr);
    }
    rb->lpVtbl->Release(rb);
}

/* v33 PASS CAPTURE: for a captured frame, every offscreen colour target is copied to a
 * readback buffer at the moment the replay LEAVES it (target switch, depth->colour
 * conversion, end of frame) -- i.e. the content each pass produced, before later passes
 * reuse the surface. rsxcap/frame_NN/passes/pNNN_opA-B_sS_<off>_<w>x<h>_d<dxgi>.raw
 * (tight rows, RGBA8 / RGBA16F / RGBA32F as named by dxgi) + passes.txt.
 * Armed by F9 or CAP_AUTO=<s,s,...> (seconds after the first >= 2000-draw frame). */
#define CP_MAX 320
static struct { u32 op0, op1; int slot; u32 off, w, h, dxgi, bpp, pitch, rowb; u64 rbo; } s_cp[CP_MAX];
static int s_cp_n = 0, s_cp_active = 0, s_cp_armed = 0, s_cp_pending = 0;
static ID3D12Resource* s_cp_rb = NULL;
static u64 s_cp_size = 0, s_cp_used = 0;
static u32 s_cp_lastop = 0;
static u32 cp_bpp(u32 dxgi)
{
    return (dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : (dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 :
           (dxgi == DXGI_FORMAT_R32_FLOAT || dxgi == DXGI_FORMAT_R8G8B8A8_UNORM || dxgi == DXGI_FORMAT_B8G8R8A8_UNORM ||
            dxgi == DXGI_FORMAT_R10G10B10A2_UNORM || dxgi == DXGI_FORMAT_R11G11B10_FLOAT || dxgi == DXGI_FORMAT_R16G16_FLOAT) ? 4 :
           (dxgi == DXGI_FORMAT_R16_FLOAT || dxgi == DXGI_FORMAT_R8G8_UNORM) ? 2 : (dxgi == DXGI_FORMAT_R8_UNORM) ? 1 : 0;
}
static void cp_begin_frame(void)
{
    s_cp_active = 0;
    if (!s_cp_armed || s_rf_count < 500) return;
    if (!s_cp_rb) {
        const char* e = getenv("CAP_PASS_MB"); long mb = e ? atol(e) : 1024;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (u64)mb << 20; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_cp_rb)) || !s_cp_rb) {
            fprintf(stderr, "[RSXCAP] pass readback buffer (%ld MB) failed%c", mb, 10); s_cp_armed = 0; s_cp_rb = NULL; return; }
        s_cp_size = rd.Width;
    }
    s_cp_armed = 0; s_cp_active = 1; s_cp_n = 0; s_cp_used = 0; s_cp_lastop = 0;
    fprintf(stderr, "[RSXCAP] pass capture ON for this frame (%u ops)%c", (unsigned)s_rf_count, 10);
}
static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to);
/* Copy the targets the replay is about to leave. op1 = last op that drew into them. */
static int s_cp_force = 0;   /* v74b: CAP_EACH_VS snapshots ignore CAP_PASS_ONLY */
static void cp_snap(const int* slots, int n, u32 op1)
{
    if (!s_cp_active) return;
    for (int i = 0; i < n; i++) {
        int sl = slots[i];
        if (sl < 0 || sl >= MAX_OFF_RTS || !s_d3d.off_rt[sl].res || s_cp_n >= CP_MAX) continue;
        OffRT* r = &s_d3d.off_rt[sl];
        { /* v71 CAP_PASS_ONLY=<hex off>[,<hex off>..]: snapshot only these targets (the budget is ~280
           * full-screen snapshots; the G-buffer alone used to fill it before the lighting/sky/post passes). */
          static int n = -1; static u32 only[16];
          if (n < 0) { n = 0; const char* e = getenv("CAP_PASS_ONLY");
              while (e && *e && n < 16) { only[n++] = (u32)strtoul(e, NULL, 16); e = strchr(e, ','); if (e) e++; } }
          if (n && !s_cp_force) { int hit = 0; for (int k = 0; k < n; k++) if (only[k] == r->off) hit = 1; if (!hit) continue; } }
        u32 bpp = cp_bpp(r->dxgi); if (!bpp) continue;
        u32 pitch = (r->w * bpp + 255) & ~255u;
        u64 o = (s_cp_used + 511) & ~(u64)511;
        if (o + (u64)pitch * r->h > s_cp_size) { static int w = 0; if ((w < 1 && w++ < 1)) fprintf(stderr, "[RSXCAP] pass buffer full%c", 10); continue; }
        off_rt_transition(sl, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_cp_rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = o;
        dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)r->dxgi;
        dst.PlacedFootprint.Footprint.Width = r->w; dst.PlacedFootprint.Footprint.Height = r->h;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
        src.pResource = r->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        s_cp[s_cp_n].op0 = s_cp_lastop; s_cp[s_cp_n].op1 = op1; s_cp[s_cp_n].slot = sl;
        s_cp[s_cp_n].off = r->off; s_cp[s_cp_n].w = r->w; s_cp[s_cp_n].h = r->h; s_cp[s_cp_n].dxgi = r->dxgi;
        s_cp[s_cp_n].bpp = bpp; s_cp[s_cp_n].pitch = pitch; s_cp[s_cp_n].rowb = r->w * bpp; s_cp[s_cp_n].rbo = o; s_cp_n++;
        s_cp_used = o + (u64)pitch * r->h;
    }
}
/* After the frame executed (GPU idle): write the snapshots into <dir>/passes. */
static void cp_write(const char* dir)
{
    if (!s_cp_pending || !s_cp_rb) return;
    s_cp_pending = 0;
    char pd[400], path[600];
    snprintf(pd, sizeof pd, "%s/passes", dir); CreateDirectoryA(pd, NULL);
    snprintf(path, sizeof path, "%s/passes.txt", pd);
    FILE* lf = fopen(path, "w");
    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_cp_used};
    if (FAILED(s_cp_rb->lpVtbl->Map(s_cp_rb, 0, &rr, &mp)) || !mp) { if (lf) fclose(lf); return; }
    for (int i = 0; i < s_cp_n; i++) {
        snprintf(path, sizeof path, "%s/p%03d_op%05u-%05u_s%d_%08X_%ux%u_d%u.raw", pd, i, s_cp[i].op0, s_cp[i].op1,
                 s_cp[i].slot, s_cp[i].off, s_cp[i].w, s_cp[i].h, s_cp[i].dxgi);
        FILE* f = fopen(path, "wb");
        if (f) { for (u32 y = 0; y < s_cp[i].h; y++)
                     fwrite((const u8*)mp + s_cp[i].rbo + (u64)y * s_cp[i].pitch, 1, (size_t)s_cp[i].rowb, f);
                 fclose(f); }
        if (lf) fprintf(lf, "p%03d ops %u-%u slot %d off 0x%08X %ux%u dxgi %u\n", i, s_cp[i].op0, s_cp[i].op1,
                        s_cp[i].slot, s_cp[i].off, s_cp[i].w, s_cp[i].h, s_cp[i].dxgi);
    }
    D3D12_RANGE wr = {0, 0}; s_cp_rb->lpVtbl->Unmap(s_cp_rb, 0, &wr);
    if (lf) fclose(lf);
    fprintf(stderr, "[RSXCAP] wrote %d pass snapshots to %s%c", s_cp_n, pd, 10);
}
static int off_rt_latest(u32 off);   /* v42: used by the capture texture dump */
static void rsx_capture_frame(u32 fi, u32 ndraws, u32 capidx)
{
    char dir[300]; const char* base = getenv("RSX_CAP_DIR");
    if (!base || !*base) base = "rsxcap";
    CreateDirectoryA(base, NULL);
    snprintf(dir, sizeof dir, "%s/frame_%02u", base, capidx);
    CreateDirectoryA(dir, NULL);
    char path[512];
    wait_for_gpu();
    cp_write(dir);   /* v33: pass snapshots of this frame, if it was pass-captured */
    /* v33: the decompiled HLSL of every fragment program this frame used */
    { char fd[400]; snprintf(fd, sizeof fd, "%s/fp", dir); CreateDirectoryA(fd, NULL);
      static u32 done[4096]; int nd = 0;
      extern int rsx_fp_decompile_ex(const u8*, u32, u32, u32, char*, u32);
      extern u32 cellGcmResolveLocated(int local, u32 offset);
      static char hl[65536];
      for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
          const D3D12DrawRecord* r = &s_rf_draws[d];
          if (r->is_clear || !r->fp_addr) continue;
          u32 key = r->fp_uh ? r->fp_uh : r->fp_addr;
          int k = 0; for (int i = 0; i < nd; i++) if (done[i] == key) { k = 1; break; }
          if (k || nd >= 4096) continue;
          done[nd++] = key;
          u32 off = cellGcmResolveLocated((r->fp_addr & 3u) == 1, r->fp_addr & ~3u);
          const u8* ucp = r->fp_uc ? r->fp_uc : vm_base + off;
          int n = rsx_fp_decompile_ex(ucp, 4096, fp_ctrl_word(r->fp_exp32), 0u, hl, sizeof hl);
          char fp_path[600]; snprintf(fp_path, sizeof fp_path, "%s/fp_%08X_%08X.hlsl", fd, r->fp_addr, r->fp_uh);
          FILE* f = fopen(fp_path, "w");
          if (f) { fprintf(f, "// fp=0x%08X exp32=%d n=%d (ucode as of capture time)\n%s\n", r->fp_addr, r->fp_exp32, n, n > 0 ? hl : "(fail)"); fclose(f); }
          snprintf(fp_path, sizeof fp_path, "%s/fp_%08X_%08X.bin", fd, r->fp_addr, r->fp_uh);
          f = fopen(fp_path, "wb"); if (f) { fwrite(ucp, 1, r->fp_uc ? 1024 : 4096, f); fclose(f); }
      } }

    /* v34: the decompiled VS of every vertex program this frame used, with the vertex
     * attribute layout it last ran with (type: 1 S1 2 F 3 SF 4 UB 5 S32K 6 CMP 7 UB256). */
    { char vd[400]; snprintf(vd, sizeof vd, "%s/vp", dir); CreateDirectoryA(vd, NULL);
      static u8 vdone[VP_VS_CACHE];
      memset(vdone, 0, sizeof vdone);
      for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
          const D3D12DrawRecord* r = &s_rf_draws[d];
          if (r->is_clear || r->vs_idx < 0 || r->vs_idx >= VP_VS_CACHE || vdone[r->vs_idx]) continue;
          vdone[r->vs_idx] = 1;
          char vp_path[600]; snprintf(vp_path, sizeof vp_path, "%s/vs_%04d.hlsl", vd, r->vs_idx);
          FILE* f = fopen(vp_path, "w");
          if (!f) continue;
          fprintf(f, "// vs_idx=%d hash=%08X first op%u fp=0x%08X\n", r->vs_idx,
                  (unsigned)s_d3d.vp_vs[r->vs_idx].hash, d, r->fp_addr);
          for (int a = 0; a < 16; a++) {
              const VsAttrInfo* ai = &s_vs_attr[r->vs_idx][a];
              if (ai->en) fprintf(f, "// attr%-2d type %u size %u stride %u freq %u %s\n", a, ai->type, ai->size,
                                  ai->stride, ai->freq, ai->loc ? "MAIN" : "LOCAL");
              (void)0;
          }
          { /* v35b: the first 3 CONVERTED vertices of this draw (what the VS reads, slot = attribute) */
            u32 vck = r->vb_chunk < VP_MAX_CHUNKS ? r->vb_chunk : 0;
            const unsigned char* vm = s_vpc_map[s_d3d.vp_parity][vck];
            u32 vstr = (r->vb_stride >= 16u && r->vb_stride <= 256u) ? r->vb_stride : 256u;
            if (vm && r->vertex_count && r->vb_byte_offset < s_vp_max_verts * 256u)
                for (u32 k = 0; k < 3 && k < r->vertex_count; k++) {
                    const float* fv = (const float*)(vm + r->vb_byte_offset + k * vstr);
                    fprintf(f, "// v%u:", k);
                    for (u32 sl = 0; sl < vstr / 16u; sl++)
                        fprintf(f, " [%u](%.3g %.3g %.3g %.3g)", sl, fv[sl*4], fv[sl*4+1], fv[sl*4+2], fv[sl*4+3]);
                    fprintf(f, "\n");
                } }
          fprintf(f, "%s\n", s_d3d.vp_vs[r->vs_idx].src ? s_d3d.vp_vs[r->vs_idx].src : "(no source)");
          fclose(f);
      }
      /* v51 CAP_VDUMP=<sref,sref,...> (default 7,3 = player / characters in inFamous): every
       * such draw's converted positions (slot 0 xyz) and its index list, to tell a broken
       * vertex buffer (SPU skinning / stale upload) from a rendering fault offline.
       * vp/d_<op>.bin: u32 nverts, nidx, stride, sref; nverts*float3; nidx*u32. */
      { static int nlist = -1; static u32 lst[8];
        static int excl = 0;   /* default: every sref EXCEPT 0 and 4 (world); the character
                                  * refs differ per scene (7/3 in the ruins, 5/6/1 at Archer Sq.) */
        if (nlist < 0) { const char* e = getenv("CAP_VDUMP"); if (!e) { e = "0,4"; excl = 1; } nlist = 0;
            while (*e && nlist < 8) { lst[nlist++] = (u32)strtoul(e, (char**)&e, 0); if (*e == ',') e++; else break; } }
        for (u32 d = 0; nlist && d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_rf_draws[d];
            if (r->is_clear || !r->vertex_count) continue;
            int hit = 0; for (int i = 0; i < nlist; i++) if (r->sref == lst[i]) hit = 1;
            if (hit == excl) continue;
            u32 vck = r->vb_chunk < VP_MAX_CHUNKS ? r->vb_chunk : 0;
            const unsigned char* vm = s_vpc_map[s_d3d.vp_parity][vck];
            u32 vstr = (r->vb_stride >= 16u && r->vb_stride <= 256u) ? r->vb_stride : 256u;
            if (!vm || (u64)r->vb_byte_offset + (u64)r->vertex_count * vstr > (u64)s_vp_max_verts * 256u) continue;
            u32 ni = ((r->ib_stamp == s_rf_frame_no || r->ib_stamp + 1u == s_rf_frame_no) && r->index_count && s_ib_map[s_d3d.vp_parity]) ? r->index_count : 0;
            char dp[600]; snprintf(dp, sizeof dp, "%s/d_%05u.bin", vd, d);
            FILE* bf = fopen(dp, "wb"); if (!bf) continue;
            u32 hd[4] = { r->vertex_count, ni, vstr, r->sref }; fwrite(hd, 4, 4, bf);
            for (u32 k = 0; k < r->vertex_count; k++) fwrite(vm + r->vb_byte_offset + (size_t)k * vstr, 4, 3, bf);
            if (ni) fwrite(s_ib_map[s_d3d.vp_parity] + r->ib_off, 4, ni, bf);
            fclose(bf);
        } } }

    /* v42: raw guest bytes of every SMALL texture the frame samples (<= 256 KB, not a render
     * target), deduped by address, so CPU/SPU-written lookup maps (inFamous' 64x36 fog-density
     * map in main memory) can be decoded and replayed offline. Reads only addresses the guest
     * bound as textures. tex/list.txt: name off w h fmt remap first-op. */
    { extern uint8_t* vm_base;
      char td[400]; snprintf(td, sizeof td, "%s/tex", dir); CreateDirectoryA(td, NULL);
      char lp[600]; snprintf(lp, sizeof lp, "%s/list.txt", td);
      FILE* lf = fopen(lp, "w");
      static u32 seen[4096]; int ns = 0;
      for (u32 d = 0; lf && vm_base && d < ndraws && d < MAX_DRAWS; d++) {
          const D3D12DrawRecord* r = &s_rf_draws[d];
          if (r->is_clear) continue;
          for (int u = 0; u < RSX_D3D_TEX_UNITS; u++) {
              if (!r->tex[u].set || !r->tex[u].off || !r->tex[u].w || !r->tex[u].h) continue;
              u32 bf = r->tex[u].fmt & 0x9Fu;
              u32 bpp = (bf == 0x81) ? 1u : (bf == 0x8B || bf == 0x92 || bf == 0x94 || (bf >= 0x82 && bf <= 0x84)) ? 2u
                      : (bf == 0x86) ? 1u : (bf == 0x87 || bf == 0x88) ? 1u : (bf == 0x9A) ? 8u : (bf == 0x9B) ? 16u : 4u;
              u32 sz = r->tex[u].w * r->tex[u].h * bpp;
              { /* v74c CAP_TEX_RAW=<hex raw off>[,..]: always dump these (any size up to 8 MB) */
                static int nf = -1; static u32 force[16];
                if (nf < 0) { nf = 0; const char* e = getenv("CAP_TEX_RAW");
                    while (e && *e && nf < 16) { force[nf++] = (u32)strtoul(e, NULL, 16); e = strchr(e, ','); if (e) e++; } }
                int fz = 0; for (int q = 0; q < nf; q++) if (force[q] == r->tex[u].raw) fz = 1;
                if (!fz) {
              if (sz == 0 || sz > 262144u || off_rt_latest(r->tex[u].raw) >= 0) continue;
              if (bf >= 0x86 && bf <= 0x88 && sz > 16384u) continue;   /* v43: big DXT art filled the cap first */
                } else if (sz > (8u << 20)) continue; }
              int k = 0; for (int i = 0; i < ns; i++) if (seen[i] == r->tex[u].off) { k = 1; break; }
              if (k || ns >= 4096) continue;
              seen[ns++] = r->tex[u].off;
              char np[700]; snprintf(np, sizeof np, "%s/t_%08X_%ux%u_%02X_%04X.raw", td, r->tex[u].off,
                                     r->tex[u].w, r->tex[u].h, r->tex[u].fmt & 0xFFu, r->tex[u].ctrl1 & 0xFFFFu);
              FILE* tf = fopen(np, "wb");
              if (tf) { fwrite(vm_base + r->tex[u].off, 1, sz, tf); fclose(tf); }
              fprintf(lf, "t_%08X off=0x%08X raw=0x%08X %ux%u fmt=0x%02X remap=0x%04X unit %d first op%u fp=0x%08X\n",
                      r->tex[u].off, r->tex[u].off, r->tex[u].raw, r->tex[u].w, r->tex[u].h, r->tex[u].fmt & 0xFFu,
                      r->tex[u].ctrl1 & 0xFFFFu, u, d, r->fp_addr);
          }
      }
      if (lf) fclose(lf); }

    /* Manifest: every op in submission order with full state. */
    snprintf(path, sizeof path, "%s/manifest.txt", dir);
    FILE* mf = fopen(path, "w");
    if (mf) {
        fprintf(mf, "frame_count=%llu draws=%u backbuffer=%ux%u\n",
                (unsigned long long)s_d3d.frame_count, ndraws, s_d3d.width, s_d3d.height);
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].used)
                fprintf(mf, "offrt[%d] off=0x%08X %ux%u dxgi=%u\n", i, s_d3d.off_rt[i].off,
                        s_d3d.off_rt[i].w, s_d3d.off_rt[i].h, s_d3d.off_rt[i].dxgi);
        fprintf(mf, "--- ops ---\n");
        for (u32 d = 0; d < ndraws && d < MAX_DRAWS; d++) {
            const D3D12DrawRecord* r = &s_rf_draws[d];
            if (r->is_clear)
                fprintf(mf, "op%03u CLEAR rt=0x%08X mrt=0x%X/0x%X/0x%X cc=(%.3f,%.3f,%.3f,%.3f)\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->cc[0], r->cc[1], r->cc[2], r->cc[3]);
            else {
                fprintf(mf, "op%03u DRAW  rt=0x%08X mrt=0x%X/0x%X/0x%X vp=%u,%u,%ux%u fp=0x%08X fph=%08X cull=%08X sref=%u zeta=0x%X vs=%d "
                            "n=%u cmask=%X blend=%d bk=0x%X at=%X ct=%X t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X\n",
                        d, r->rt_off, r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                        r->vp_x, r->vp_y, r->vp_w, r->vp_h,
                        r->fp_addr, r->fp_uh, r->cull, r->sref, r->zeta_off, r->vs_idx, r->vertex_count, r->cmask, r->blend, r->blend_key, r->alpha_ctl, r->ct,
                        r->tex[0].raw, r->tex[1].raw, r->tex[2].raw, r->tex[3].raw);
                if (s_d3d.vp_cb_mapped) {   /* v34: the z part of the VS epilogue this draw ran with */
                    const float* vx = (const float*)((const char*)s_d3d.vp_cb_mapped
                        + ((u64)(s_d3d.vp_parity) * s_max_draws + d) * VP_CB_STRIDE + RSX_MAX_VERTEX_CONSTANTS * 16);
                    fprintf(mf, "      z scale %g offset %g\n", vx[2], vx[6]);
                }
                for (int u = 0; u < RSX_D3D_TEX_UNITS; u++)   /* v41: all 16 units (the sun pass samples 4-6) */
                    if (r->tex[u].set)
                        fprintf(mf, "      t%d off=0x%08X fmt=0x%02X (%s%s) %ux%u remap=0x%04X\n", u,
                                r->tex[u].off, r->tex[u].fmt,
                                (r->tex[u].fmt & 0x20) ? "LN" : "SZ",
                                (r->tex[u].fmt & 0x40) ? ",NR" : "",
                                r->tex[u].w, r->tex[u].h, r->tex[u].ctrl1 & 0xFFFF);
            }
        }
        /* Per-draw VP constant banks for the first few offscreen-RT geometry
         * draws: c[0..3] is the projection row-major for most CG VPs; also scan
         * for the largest non-zero slot so a mis-placed MVP is obvious. Records
         * live at slot==draw-index; parity is post-^1 here (capture is after the
         * frame's vp_parity flip), so read the other half. */
        if (s_d3d.vp_cb_mapped) {
            u32 par = (u32)(s_d3d.vp_parity);
            fprintf(mf, "--- vp constants (first offscreen geometry draws) ---\n");
            int shown = 0;
            for (u32 d = 0; d < ndraws && d < MAX_DRAWS && shown < 4; d++) {
                const D3D12DrawRecord* r = &s_rf_draws[d];
                if (r->is_clear || !r->rt_off) continue;
                const float* c = (const float*)((const char*)s_d3d.vp_cb_mapped
                    + ((u64)par * s_max_draws + d) * VP_CB_STRIDE);
                int last_nz = -1; float maxabs = 0.0f;
                for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++) {
                    float a = c[i] < 0 ? -c[i] : c[i];
                    if (a > 1e-9f) last_nz = i;
                    if (a > maxabs) maxabs = a;
                }
                fprintf(mf, "op%03u vs=%d lastNZslot=%d maxabs=%.3f\n",
                        d, r->vs_idx, last_nz / 4, maxabs);
                if (shown == 0) {   /* full non-zero slot dump for the 1st draw */
                    for (int s = 0; s <= last_nz / 4; s++) {
                        const float* v = c + s * 4;
                        if ((v[0]||v[1]||v[2]||v[3]))
                            fprintf(mf, "   c[%3d] = %9.4f %9.4f %9.4f %9.4f\n",
                                    s, v[0], v[1], v[2], v[3]);
                    }
                }
                shown++;
            }
        }
        fclose(mf);
    }

    /* Backbuffer (in PRESENT state at capture time) + every used offscreen RT. */
    snprintf(path, sizeof path, "%s/backbuffer.bmp", dir);
    cap_readback_write(s_d3d.render_targets[fi], BBW, BBH,
                       DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_PRESENT, fi, path);
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* r = &s_d3d.off_rt[i];
        if (!r->res || !r->used) continue;
        snprintf(path, sizeof path, "%s/rt_%08X_%ux%u_s%d.bmp", dir, r->off, r->w, r->h, i);   /* aliases share an offset */
        cap_readback_write(r->res, r->w, r->h, r->dxgi, r->st, fi, path);
    }
    printf("[RSXCAP] frame %llu -> %s (%u ops)\n",
           (unsigned long long)s_d3d.frame_count, dir, ndraws);
}

/* Decompile the captured RSX vertex program to HLSL and build the VP PSO
 * (decompiled VS + atlas alpha-test PS). One-shot per program. */
static void compile_vp(void)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!st || st->vp_ucode_bytes < 16 || !s_d3d.vp_root_sig) return;

    static char hlsl[64 * 1024];
    int ni = rsx_vp_decompile(st->vp_ucode, st->vp_ucode_bytes, hlsl, sizeof hlsl);
    /* Does this VP read vp_c[0..3]? Gates the garbage-projection fallback:
     * programs keeping their MVP elsewhere (gcm/cube: c[256..259]) must not
     * have c[0..3] stomped nor the viewport z-lane overridden. */
    s_d3d.vp_uses_c03 = (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
                         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    if (ni <= 0) { printf("[VP] decompile failed (%d)\n", ni); s_d3d.vp_compiled_bytes = st->vp_ucode_bytes; return; }
    if (getenv("VP_DUMP")) {
        FILE* vf = fopen("vp_dump.hlsl", "w");
        if (vf) { fwrite(hlsl, 1, strlen(hlsl), vf); fclose(vf);
                  printf("[VP] dumped vp_dump.hlsl (%d instrs)\n", ni); }
        printf("[VPRAW] first 3 instrs (%u ucode bytes):\n", st->vp_ucode_bytes);
        for (u32 _q = 0; _q < 48 && _q < st->vp_ucode_bytes; _q += 16)
            printf("[VPRAW]  d0=%02X%02X%02X%02X d1=%02X%02X%02X%02X d2=%02X%02X%02X%02X d3=%02X%02X%02X%02X\n",
                st->vp_ucode[_q+0],st->vp_ucode[_q+1],st->vp_ucode[_q+2],st->vp_ucode[_q+3],
                st->vp_ucode[_q+4],st->vp_ucode[_q+5],st->vp_ucode[_q+6],st->vp_ucode[_q+7],
                st->vp_ucode[_q+8],st->vp_ucode[_q+9],st->vp_ucode[_q+10],st->vp_ucode[_q+11],
                st->vp_ucode[_q+12],st->vp_ucode[_q+13],st->vp_ucode[_q+14],st->vp_ucode[_q+15]);
    }

    /* Pixel shader mirrors dbgfont's FP: sample the atlas coverage at TEXCOORD0
     * (VP output o[7]), alpha-test at 0.5, output the vertex color (o[1]). */
    static const char ps[] =
        "Texture2D tex : register(t0); SamplerState smp : register(s0);\n"
        "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
        "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
        "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
        "float4 main(PSIn i):SV_TARGET{ float cov = tex.Sample(smp, i.t0.xy).r;\n"
        "  if (cov <= 0.5) discard; return float4(i.col0.rgb, 1); }\n";

    ID3DBlob *vb=NULL,*pb=NULL,*e=NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), "vp", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (FAILED(hr)) { printf("[VP] VS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }
    hr = D3DCompile(ps, sizeof(ps)-1, "vpps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr)) { printf("[VP] PS compile FAIL: %s\n", e?(const char*)e->lpVtbl->GetBufferPointer(e):"?"); if(e)e->lpVtbl->Release(e); if(vb)vb->lpVtbl->Release(vb); s_d3d.vp_compiled_bytes=st->vp_ucode_bytes; return; }

    /* The decompiled VS declares inputs a0:ATTR0 .. a15:ATTR15 and the HLSL
     * compiler keeps whichever the program body reads. D3D12 requires EVERY VS
     * input to have a matching input-layout element (by semantic name+index),
     * so declare all 16 ATTR slots. (The old single "POSITION" element matched
     * NO VS input once the decompiler switched to ATTR semantics -> PSO failed
     * with E_INVALIDARG for every VP, blanking cellmark's text and vkcube.)
     * The VP path uploads only attrib0 to vp_vb (one float4/vertex), so every
     * slot reads that same float4 at offset 0; attributes other than position
     * therefore alias attrib0 for now (colours/uv wrong until the VP path
     * uploads multiple attributes), but geometry is correct and the PSO is
     * valid. */
    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _e = 0; _e < 16; _e++) {
        il[_e].SemanticName = "ATTR";
        il[_e].SemanticIndex = _e;
        il[_e].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        il[_e].InputSlot = 0;
        /* 256-byte generic VP vertex: every ATTRi is a float4 slot at i*16
         * (read_vp_vertex converts each enabled RSX attrib by type; disabled
         * slots hold (0,0,0,1)). Covers tiny3d (a0/a3/a8), SDK gcm samples
         * (a0/a1/a2), dbgfont -- no aliasing. */
        il[_e].AlignedByteOffset = (UINT)(_e * 16);
        il[_e].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_e].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    /* Depth test ON (LESS_EQUAL, matching the guest's rsxtiny_DepthTestFunc 515 /
     * CELL_GCM_LEQUAL). Without it the cube's faces drew in submission order and
     * back faces overwrote the front -> you saw through the near face to the
     * darker interior. The depth buffer is bound + cleared to 1.0 each frame. */
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&s_d3d.pipeline_state_vp);

    /* Second PSO: same VS but a COLOUR-only PS (no texture sample, no alpha
     * discard) for untextured 3D geometry (vkcube's cube). The atlas PS above
     * samples a texture and discards where coverage<=0.5, which blanks any draw
     * with no atlas bound. */
    {
        static const char ps_col[] =
            "struct PSIn{ float4 pos:SV_Position; float4 col0:COLOR0; float4 col1:COLOR1; float4 fog:FOG;\n"
            "  float4 t0:TEXCOORD0; float4 t1:TEXCOORD1; float4 t2:TEXCOORD2; float4 t3:TEXCOORD3;\n"
            "  float4 t4:TEXCOORD4; float4 t5:TEXCOORD5; float4 t6:TEXCOORD6; float4 t7:TEXCOORD7; };\n"
            "float4 main(PSIn i):SV_TARGET{ return float4(i.col0.rgb, 1); }\n";
        ID3DBlob *pcb=NULL,*ec=NULL;
        if (SUCCEEDED(D3DCompile(ps_col, sizeof(ps_col)-1, "vppsc", NULL, NULL,
                                 "main", "ps_5_0", 0, 0, &pcb, &ec)) && pcb) {
            pd.PS.pShaderBytecode = pcb->lpVtbl->GetBufferPointer(pcb);
            pd.PS.BytecodeLength  = pcb->lpVtbl->GetBufferSize(pcb);
            HRESULT hr2 = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState,
                (void**)&s_d3d.pipeline_state_vp_color);
            printf(SUCCEEDED(hr2) ? "[VP] colour pipeline ready\n"
                                  : "[VP] colour PSO FAIL (0x%08lX)\n", hr2);
            pcb->lpVtbl->Release(pcb);
        }
        if (ec) ec->lpVtbl->Release(ec);
    }
    /* Keep the VS bytecode for guest-FP PSO builds (vp_get_fp_pso); bump the
     * generation so cached FP PSOs built against the old VS are rebuilt. */
    if (s_d3d.vp_vs_blob) s_d3d.vp_vs_blob->lpVtbl->Release(s_d3d.vp_vs_blob);
    s_d3d.vp_vs_blob = vb;
    s_d3d.vp_gen++;
    pb->lpVtbl->Release(pb);
    s_d3d.vp_compiled_bytes = st->vp_ucode_bytes;
    if (SUCCEEDED(hr)) { s_d3d.vp_ready = 1; printf("[VP] pipeline ready (%d instrs)\n", ni); }
    else {
        printf("[VP] PSO creation FAIL (0x%08lX)\n", hr);
        /* Drain the debug layer's message queue to get the EXACT validation
         * reason (E_INVALIDARG is otherwise opaque). One-shot. */
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            for (UINT64 mi = 0; mi < n; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)))
                    printf("[VP][DBG] %s\n", m->pDescription);
                free(m);
            }
            iq->lpVtbl->Release(iq);
        }
    }
}

/* Build (or fetch) a PSO pairing the current decompiled VS with the guest's
 * FRAGMENT program at fp_addr: read the FP ucode from guest memory, decompile
 * to HLSL (rsx_fp_decompiler), compile, and cache. This replaces the two
 * hardcoded pixel shaders for draws whose FP we can translate -- e.g.
 * gcm/cube's plasma FP `c = tex2D(t0, uv).x; out = (c, 0, c, 1)`. */
#include "rsx_fp_decompiler.h"

/* Hash + compile the CURRENT rsx_state vertex program into the VS cache;
 * returns the cache slot (or -1). Called at draw-record time so each draw
 * carries the VP that was loaded when it was submitted. */
static u32 vp_hash_ucode(const u8* p, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h ? h : 1u;
}
/* Hash ONE vertex program: from the start slot up to and including its END
 * instruction. Hashing to the end of the whole microcode store (the old key)
 * gave an unchanged program a new identity -- and a new VS compile plus a new
 * set of pipelines -- whenever any OTHER program after it in the store was
 * rewritten, and cost ~8 KB of hashing per draw (inFamous: 10k draws/frame). */
u32 rsx_vp_program_size_instrs(const u8* ucode, u32 max_bytes);
static u32 vp_hash_program(const u8* vuc, u32 vlen)
{
    u32 n = rsx_vp_program_size_instrs(vuc, vlen) * 16u;
    if (n == 0 || n > vlen) n = vlen;
    return vp_hash_ucode(vuc, n);
}

/* On-disk cache of compiled guest shaders (2026-10-04). inFamous needs ~2000
 * pipelines; every one ran D3DCompile on first use, every run. Keyed by a 64-bit
 * hash of (HLSL text, target); stored as shadercache\<hash>_<target>.cso next to
 * the working directory. SHADER_DISK_CACHE=0 disables. Same signature as
 * D3DCompile for the arguments used here. */
/* ---- v235D SHC_PACK=1 (default off): in-memory shader cache, one pack file ----------------------------------------
 * Evidence (play_pgo.err.log, v234): every pipeline / vertex-shader miss loads its DXBC from shadercache\<hash>.cso
 * and that fopen+fread costs 5-9 ms on this machine ([SPIKE] "shc 485.5/88 (0 compiled)" at load, "shc 22.1/3" in
 * the 41 ms frame at t=190) -- ~83% of all pso+vs miss time, on the render (FP) and FIFO (VS) threads.
 * SHC_PACK=1: a background thread reads shadercache\shcpack.bin (all blobs in one file) plus any .cso not in it
 * yet into a hash table; cached_compile serves (hash, target) from memory (bit-identical bytes), falls back to the
 * old per-file path when absent, and keeps writing new compiles as .cso (picked up by the next start, which then
 * rewrites the pack).  SHC_PACK_PHASE=0/1: memory path only on that A/B phase parity.  [SHC_PACK] lines report
 * loads / mean cost per path and parity. */
typedef struct { unsigned long long h; char tgt[8]; u32 len; const u8* data; } ShcpEnt;
#define SHCP_CAP 16384u
static ShcpEnt s_shcp[SHCP_CAP];
static u32 s_shcp_n = 0;
static SRWLOCK s_shcp_lock = SRWLOCK_INIT;
static int s_shcp_cfg = -1, s_shcp_phase = -1;
static volatile LONG64 s_shcp_cnt[2][3], s_shcp_tsc[2][3];   /* [parity][pack, file, compile] */
static volatile LONG s_shcp_total = 0;
static double s_shcp_tpus = 0.0;   /* TSC ticks per microsecond (set by the preload thread) */
static u32 shcp_slot(unsigned long long h, const char* tgt)
{
    u32 i = (u32)(h ^ (h >> 29)) & (SHCP_CAP - 1u);
    for (u32 k = 0; k < SHCP_CAP; k++, i = (i + 1u) & (SHCP_CAP - 1u)) {
        const ShcpEnt* e = &s_shcp[i];
        if (!e->data) return i;
        if (e->h == h && strncmp(e->tgt, tgt, 8) == 0) return i;
    }
    return SHCP_CAP;
}
/* data ownership passes to the table (never freed: the table lives for the process) */
static int shcp_insert(unsigned long long h, const char* tgt, const u8* data, u32 len)
{
    if (!data || !len || strlen(tgt) >= 8) return 0;
    int ok = 0;
    AcquireSRWLockExclusive(&s_shcp_lock);
    if (s_shcp_n < (SHCP_CAP / 4u) * 3u) {
        const u32 i = shcp_slot(h, tgt);
        if (i < SHCP_CAP && !s_shcp[i].data) {
            ShcpEnt* e = &s_shcp[i];
            e->h = h; memset(e->tgt, 0, sizeof e->tgt); memcpy(e->tgt, tgt, strlen(tgt)); e->len = len; e->data = data;
            s_shcp_n++; ok = 1;
        }
    }
    ReleaseSRWLockExclusive(&s_shcp_lock);
    return ok;
}
static int shcp_has(unsigned long long h, const char* tgt)
{
    if (strlen(tgt) >= 8) return 0;
    AcquireSRWLockShared(&s_shcp_lock);
    const u32 i = shcp_slot(h, tgt);
    const int r = i < SHCP_CAP && s_shcp[i].data != NULL;
    ReleaseSRWLockShared(&s_shcp_lock);
    return r;
}
static int shcp_find(unsigned long long h, const char* tgt, ID3DBlob** out)
{
    if (strlen(tgt) >= 8) return 0;
    int r = 0;
    AcquireSRWLockShared(&s_shcp_lock);
    const u32 i = shcp_slot(h, tgt);
    if (i < SHCP_CAP && s_shcp[i].data) {
        const ShcpEnt* e = &s_shcp[i];
        if (SUCCEEDED(D3DCreateBlob((SIZE_T)e->len, out)) && *out) {
            memcpy((*out)->lpVtbl->GetBufferPointer(*out), e->data, e->len); r = 1; }
    }
    ReleaseSRWLockShared(&s_shcp_lock);
    return r;
}
static void shcp_save(void)
{
    AcquireSRWLockShared(&s_shcp_lock);
    size_t total = 12;
    for (u32 i = 0; i < SHCP_CAP; i++) if (s_shcp[i].data) total += 8 + 8 + 4 + s_shcp[i].len;
    u8* b = (u8*)malloc(total);
    u32 n = 0; size_t o = 12;
    if (b) for (u32 i = 0; i < SHCP_CAP; i++) { const ShcpEnt* e = &s_shcp[i]; if (!e->data) continue;
        memcpy(b + o, &e->h, 8); memcpy(b + o + 8, e->tgt, 8); memcpy(b + o + 16, &e->len, 4);
        memcpy(b + o + 20, e->data, e->len); o += 20 + e->len; n++; }
    ReleaseSRWLockShared(&s_shcp_lock);
    if (!b) return;
    const u32 magic = 0x50434853u, ver = 1u;
    memcpy(b, &magic, 4); memcpy(b + 4, &ver, 4); memcpy(b + 8, &n, 4);
    int ok = 0;
    FILE* f = fopen("shadercache\\shcpack.bin.tmp", "wb");
    if (f) { ok = fwrite(b, 1, o, f) == o; fclose(f);
             if (ok) ok = MoveFileExA("shadercache\\shcpack.bin.tmp", "shadercache\\shcpack.bin", MOVEFILE_REPLACE_EXISTING) != 0;
             else DeleteFileA("shadercache\\shcpack.bin.tmp"); }
    free(b);
    fprintf(stderr, "[SHC_PACK] v235D wrote shadercache\\shcpack.bin: %u blobs, %.1f MB, %s%c", n, o / 1048576.0, ok ? "ok" : "FAILED", 10);
}
/* "<16 hex>_<target>.cso" -> hash + target */
static int shcp_parse_name(const char* nm, unsigned long long* h, char* tgt)
{
    size_t L = strlen(nm);
    if (L < 22 || nm[16] != '_' || _stricmp(nm + L - 4, ".cso") != 0) return 0;
    unsigned long long v = 0;
    for (int i = 0; i < 16; i++) { const char c = nm[i]; int d;
        if (c >= '0' && c <= '9') d = c - '0'; else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10; else return 0;
        v = (v << 4) | (unsigned)d; }
    const size_t tl = L - 4 - 17;
    if (tl == 0 || tl >= 8) return 0;
    memcpy(tgt, nm + 17, tl); tgt[tl] = 0;
    *h = v;
    return 1;
}
static DWORD WINAPI shcp_thread(LPVOID p)
{
    (void)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    LARGE_INTEGER qf, q0, q1; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    const unsigned long long c0 = hz_tsc();
    u32 npack = 0, nfile = 0; size_t packbytes = 0; int had_pack = 0;
    FILE* f = fopen("shadercache\\shcpack.bin", "rb");
    if (f) {
        fseek(f, 0, SEEK_END); const long sz = ftell(f); fseek(f, 0, SEEK_SET);
        u8* blob = (sz > 12) ? (u8*)malloc((size_t)sz) : NULL;
        if (blob && fread(blob, 1, (size_t)sz, f) == (size_t)sz) {
            u32 magic, ver, n; memcpy(&magic, blob, 4); memcpy(&ver, blob + 4, 4); memcpy(&n, blob + 8, 4);
            if (magic == 0x50434853u && ver == 1u) {
                had_pack = 1; packbytes = (size_t)sz;
                size_t o = 12;
                for (u32 k = 0; k < n && o + 20 <= (size_t)sz; k++) {
                    unsigned long long h; char tgt[9]; u32 len;
                    memcpy(&h, blob + o, 8); memcpy(tgt, blob + o + 8, 8); tgt[8] = 0; memcpy(&len, blob + o + 16, 4);
                    if (len == 0 || o + 20 + len > (size_t)sz) break;
                    if (shcp_insert(h, tgt, blob + o + 20, len)) npack++;
                    o += 20 + len;
                }
            } else { free(blob); blob = NULL; }
        } else { free(blob); blob = NULL; }
        fclose(f);
    }
    WIN32_FIND_DATAA fd; HANDLE fh = FindFirstFileA("shadercache\\*.cso", &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            unsigned long long h; char tgt[8];
            if (!shcp_parse_name(fd.cFileName, &h, tgt) || shcp_has(h, tgt)) continue;
            char path[300]; snprintf(path, sizeof path, "shadercache\\%s", fd.cFileName);
            FILE* g = fopen(path, "rb");
            if (!g) continue;
            fseek(g, 0, SEEK_END); const long n = ftell(g); fseek(g, 0, SEEK_SET);
            u8* d = (n > 0) ? (u8*)malloc((size_t)n) : NULL;
            if (d && fread(d, 1, (size_t)n, g) == (size_t)n && shcp_insert(h, tgt, d, (u32)n)) nfile++;
            else free(d);
            fclose(g);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    QueryPerformanceCounter(&q1);
    const double us = (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)qf.QuadPart;
    if (us > 0) s_shcp_tpus = (double)(hz_tsc() - c0) / us;
    fprintf(stderr, "[SHC_PACK] v235D preload: %u blobs from shcpack.bin (%.1f MB%s), %u loose .cso files, %u in memory, %.0f ms%c",
            npack, packbytes / 1048576.0, had_pack ? "" : ", no pack yet", nfile, s_shcp_n, us / 1000.0, 10);
    if (nfile || !had_pack) shcp_save();
    return 0;
}
static int shcp_cfg(void)
{
    if (s_shcp_cfg < 0) {
        const char* e = getenv("SHC_PACK"); const char* p = getenv("SHC_PACK_PHASE");
        s_shcp_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_shcp_phase = atoi(p) & 1;
        fprintf(stderr, "[SHC_PACK] v235D in-memory shader cache: %s (phase parity %d)%c", s_shcp_cfg ? "ON" : "off", s_shcp_phase, 10);
        if (s_shcp_cfg) { HANDLE th = CreateThread(NULL, 0, shcp_thread, NULL, 0, NULL); if (th) CloseHandle(th); else s_shcp_cfg = 0; }
    }
    return s_shcp_cfg;
}
static int shcp_use(void)
{
    extern volatile int g_ab_phase;
    return shcp_cfg() && (s_shcp_phase < 0 || ((g_ab_phase & 1) == s_shcp_phase));
}
static void shcp_note(int kind, unsigned long long t0)
{
    extern volatile int g_ab_phase;
    const int par = g_ab_phase & 1;
    InterlockedIncrement64(&s_shcp_cnt[par][kind]);
    InterlockedExchangeAdd64(&s_shcp_tsc[par][kind], (LONG64)(hz_tsc() - t0));
    const LONG n = InterlockedIncrement(&s_shcp_total);
    if (s_shcp_cfg == 1 && (n % 64) == 0 && s_shcp_tpus > 0.0) {
        for (int q = 0; q < 2; q++) {
            double a[3];
            for (int k = 0; k < 3; k++) a[k] = s_shcp_cnt[q][k] ? (double)s_shcp_tsc[q][k] / (double)s_shcp_cnt[q][k] / s_shcp_tpus : 0.0;
            fprintf(stderr, "[SHC_PACK] %ld loads; parity %d: pack %lld (mean %.0f us) | file %lld (mean %.0f us) | compile %lld (mean %.0f us)%c",
                    (long)n, q, (long long)s_shcp_cnt[q][0], a[0], (long long)s_shcp_cnt[q][1], a[1], (long long)s_shcp_cnt[q][2], a[2], 10);
        }
    }
}
static HRESULT cached_compile_impl(const char* src, size_t len, const char* name,
                              const D3D_SHADER_MACRO* defs, ID3DInclude* inc,
                              const char* entry, const char* target, UINT f1, UINT f2,
                              ID3DBlob** out, ID3DBlob** err)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("SHADER_DISK_CACHE"); on = (e && *e == '0') ? 0 : 1;
                  if (on) CreateDirectoryA("shadercache", NULL); }
    unsigned long long h = 1469598103934665603ull;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)src[i]; h *= 1099511628211ull; }
    for (const char* t = target; *t; t++) { h ^= (unsigned char)*t; h *= 1099511628211ull; }
    for (const char* t = entry; *t; t++) { h ^= (unsigned char)*t; h *= 1099511628211ull; }
    h ^= (unsigned long long)f1 * 0x9E3779B97F4A7C15ull;
    char path[128];
    snprintf(path, sizeof path, "shadercache\\%016llx_%s.cso", h, target);
    static unsigned hits = 0, misses = 0;
    const unsigned long long shcp_t0 = hz_tsc();   /* v235D */
    const int shcp = on && !s_bis_nodisk && shcp_use();
    if (shcp && shcp_find(h, target, out)) {
        if (err) *err = NULL;
        if ((++hits % 500) == 1)
            fprintf(stderr, "[SHADERCACHE] %u hits, %u compiles%c", hits, misses, 10);
        shcp_note(0, shcp_t0);
        return S_OK;
    }
    if (on && !s_bis_nodisk) {
        FILE* f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
            if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, out)) && *out) {
                if (fread((*out)->lpVtbl->GetBufferPointer(*out), 1, (size_t)n, f) == (size_t)n) {
                    fclose(f);
                    if (err) *err = NULL;
                    if ((++hits % 500) == 1)
                        fprintf(stderr, "[SHADERCACHE] %u hits, %u compiles%c", hits, misses, 10);
                    if (shcp) { const u32 bl = (u32)(*out)->lpVtbl->GetBufferSize(*out); u8* cp = (u8*)malloc(bl);
                                if (cp) { memcpy(cp, (*out)->lpVtbl->GetBufferPointer(*out), bl); if (!shcp_insert(h, target, cp, bl)) free(cp); } }
                    if (shcp_cfg()) shcp_note(1, shcp_t0);
                    return S_OK;
                }
                (*out)->lpVtbl->Release(*out); *out = NULL;
            }
            fclose(f);
        }
    }
    HRESULT hr = D3DCompile(src, len, name, defs, inc, entry, target, f1, f2, out, err);
    misses++; hz_add(HZ_SHC_MISS, 1);   /* v225A */
    if (on && !s_bis_nodisk && SUCCEEDED(hr) && *out) {
        char tmp[140]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE* f = fopen(tmp, "wb");
        if (f) {
            size_t n = (size_t)(*out)->lpVtbl->GetBufferSize(*out);
            int ok = fwrite((*out)->lpVtbl->GetBufferPointer(*out), 1, n, f) == n;
            fclose(f);
            if (ok) MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING); else DeleteFileA(tmp);
        }
        if (shcp) { const u32 bl = (u32)(*out)->lpVtbl->GetBufferSize(*out); u8* cp = (u8*)malloc(bl);
                    if (cp) { memcpy(cp, (*out)->lpVtbl->GetBufferPointer(*out), bl); if (!shcp_insert(h, target, cp, bl)) free(cp); } }
    }
    if (on && shcp_cfg()) shcp_note(2, shcp_t0);
    return hr;
}
/* v225A: time every shader-cache load / compile (hz_stats.h) */
static HRESULT cached_compile(const char* src, size_t len, const char* name,
                              const D3D_SHADER_MACRO* defs, ID3DInclude* inc,
                              const char* entry, const char* target, UINT f1, UINT f2,
                              ID3DBlob** out, ID3DBlob** err)
{
    const unsigned long long t0 = hz_tsc();
    HRESULT hr = cached_compile_impl(src, len, name, defs, inc, entry, target, f1, f2, out, err);
    const unsigned long long dt = hz_tsc() - t0;
    hz_add(HZ_SHC_TSC, (long long)dt); hz_add(HZ_SHC_N, 1);
    return hr;
}

/* VP_PACK: slots per vertex for the current vertex program = highest input
 * register it reads + 1, cached by ucode hash. Same program window as
 * vp_get_vs (transform_program_start). Any doubt -> 16. */
#include "rsx_vp_decompiler.h"
static void vp_pack_begin(const rsx_state* st)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("VP_PACK"); on = (e && *e == '0') ? 0 : 1; }
    s_vpk = 16;
    if (!on || !st || st->vp_ucode_bytes < 16) return;
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    /* v64: same microcode store + start as the last draw -> same answer, no rehash */
    extern volatile u32 g_rsx_vp_gen;
    static u32 c_gen = 0, c_start = 0xFFFFFFFFu, c_bytes = 0, c_k = 16; static const rsx_state* c_st = NULL;
    if (c_gen == g_rsx_vp_gen && c_start == vstart && c_bytes == st->vp_ucode_bytes && c_st == st) { s_vpk = c_k; return; }
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    u32 h = vp_hash_program(vuc, vlen);
    c_gen = g_rsx_vp_gen; c_start = vstart; c_bytes = st->vp_ucode_bytes; c_st = st; c_k = 16;
    enum { NC = 1024 };
    static u32 key[NC]; static u8 val[NC]; static u8 have[NC];
    u32 i = h & (NC - 1);
    if (have[i] && key[i] == h) { s_vpk = val[i]; c_k = s_vpk; return; }
    u32 k = 16;
    u32 instrs = rsx_vp_program_size_instrs(vuc, vlen);
    if (instrs) {
        rsx_vp_input_analysis a = { 0xFFFFu, 0 };
        if (rsx_vp_analyze_inputs(vuc, instrs * 16u, &a) == (int)instrs && a.exact && a.input_mask) {
            u32 m = a.input_mask & 0xFFFFu, hi = 0;
            for (u32 b = 0; b < 16; b++) if (m & (1u << b)) hi = b;
            k = hi + 1;
        }
    }
    { static int n = 0; if (n < 24) { n++;
        fprintf(stderr, "[VPPACK] vp hash=0x%08X: %u slots/vertex (%u B)%c", h, k, k * 16u, 10); } }
    key[i] = h; val[i] = (u8)k; have[i] = 1;
    s_vpk = k; c_k = k;
}

static int vp_get_vs_impl(const rsx_state* st)
{
    extern int rsx_vp_decompile(const uint8_t*, u32, char*, u32);
    if (!st || st->vp_ucode_bytes < 16) return -1;
    /* Start at the instruction SET_TRANSFORM_PROGRAM_START selects, not at 0.
     * The microcode store holds every resident program -- this title keeps its
     * scene, fluid, caustics and droplet programs in it at once -- so
     * decompiling from 0 ran the scene's transform for every draw. That is why
     * the fluid's 114300 vertices rasterized nothing: they were being pushed
     * through the wrong program.
     * VP_START_OFF=1 restores the old always-from-zero behaviour. */
    u32 vstart = 0;
    { static int off = -1; if (off < 0) off = getenv("VP_START_OFF") ? 1 : 0;
      if (!off) vstart = st->transform_program_start * 16u; }
    if (vstart >= st->vp_ucode_bytes) vstart = 0;
    /* v64: unchanged microcode store since the last draw -> its slot, without rehashing */
    extern volatile u32 g_rsx_vp_gen;
    static u32 g_gen = 0, g_start = 0xFFFFFFFFu, g_bytes = 0, g_hash = 0; static int g_slot = -1; static const rsx_state* g_st = NULL;
    if (g_slot >= 0 && g_gen == g_rsx_vp_gen && g_start == vstart && g_bytes == st->vp_ucode_bytes && g_st == st &&
        g_slot < s_d3d.vp_vs_n && s_d3d.vp_vs[g_slot].hash == g_hash) return g_slot;
    const u8* vuc = st->vp_ucode + vstart;
    u32 vlen = st->vp_ucode_bytes - vstart;
    u32 hash = vp_hash_program(vuc, vlen);
    g_gen = g_rsx_vp_gen; g_start = vstart; g_bytes = st->vp_ucode_bytes; g_st = st; g_hash = hash; g_slot = -1;
    /* Remember the last lookup: consecutive draws almost always share a VP. */
    { static u32 last_hash = 0; static int last_slot = -1;
      if (last_slot >= 0 && last_slot < s_d3d.vp_vs_n && last_hash == hash &&
          s_d3d.vp_vs[last_slot].hash == hash) { g_slot = last_slot; return last_slot; }
      for (int i = 0; i < s_d3d.vp_vs_n; i++)
          if (s_d3d.vp_vs[i].hash == hash) { last_hash = hash; last_slot = i; g_slot = i; return i; } }

    static char hlsl[262144];
    int ni = rsx_vp_decompile(vuc, vlen, hlsl, sizeof hlsl);
    if (ni <= 0) return -1;
    if (getenv("VP_DUMP")) { static int _d=0; if ((_d < 4 && _d++ < 4)) {
        FILE* f = fopen("vp2_dump.hlsl", _d==1 ? "w" : "a");
        if (f) { fprintf(f, "/* per-draw VS hash pending, %d instrs */%s%s", ni, hlsl, "\n"); fclose(f); } } }
    /* RSX_DBG_VP=<hex tex0 offset>: write the vertex program belonging to the draws
     * that bind that texture. vp2_dump.hlsl holds whichever four programs were
     * compiled first, which need not include the one under investigation -- and
     * reading the wrong program sends you chasing the wrong constants. */
    { static const char* dp = (const char*)1; static u32 wantp = 0; static int done = 0;
      if (dp == (const char*)1) { dp = getenv("RSX_DBG_VP");
                                  wantp = dp ? (u32)strtoul(dp, NULL, 16) : 0; }
      if (wantp && !done && s_d3d.cur_texs[0].raw == wantp) {
          done = 1;
          FILE* f = fopen("duck_vp.hlsl", "w");
          if (f) { fprintf(f, "/* duck VP, %d instrs, tex0=0x%X */%s", ni, wantp, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKVP] wrote duck_vp.hlsl (%d instrs)%c", ni, 10);
      } }
    /* VP_BYPASS=1: replace the guest transform with a direct map of attribute 0
     * into clip space. If geometry appears with this on, everything from the
     * input layout through raster/depth/present is sound and the fault is the
     * transform (constants or the decompiled program); if it stays blank, the
     * fault is upstream of the shader -- the attribute binding itself. Patched
     * in place, space-padded to the original length so offsets stay valid. */
    /* VP_W_ONE=1: keep the guest transform but force the clip-space w to 1.
     * Different from VP_BYPASS, which replaces the whole position: this tests
     * only the hypothesis that w is wrong. This title's VP ends
     *   { float4 _v = (float4)((vp_c[467]).xxxx); o[0].w = _v.w; }
     * with c467 = (-1, 1, 0, 0), so w = -1 and the clipper discards every
     * vertex. Patched in place, space-padded so offsets stay valid. */
    if (getenv("VP_W_ONE")) {
        static const char anchor[] = "o[0].w = _v.w;";
        static const char repl[]   = "o[0].w = 1.0;";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    if (getenv("VP_BYPASS")) {
        static const char anchor[] =
            "Out.pos = float4(_p.xyz * vp_posscale.xyz + _p.w * vp_posoffset.xyz, _p.w);";
        static const char repl[] = "Out.pos = float4(v[0].xy * 0.5, 0.5, 1.0);";
        char* at = hlsl;
        while ((at = strstr(at, anchor)) != NULL) {
            size_t n = sizeof(anchor) - 1, m = sizeof(repl) - 1;
            memcpy(at, repl, m);
            memset(at + m, ' ', n - m);
            at += n;
        }
    }
    ID3DBlob* vb = NULL; ID3DBlob* e = NULL;
    s_sf_vs_compiles++;
    HRESULT hr = cached_compile(hlsl, strlen(hlsl), "guest_vp2", NULL, NULL,
                            "main", "vs_5_0", 0, 0, &vb, &e);
    if (e) e->lpVtbl->Release(e);
    if (FAILED(hr) || !vb) {
        static int _e=0; if ((_e < 4 && _e++ < 4)) printf("[VP] per-draw VS compile FAIL (hash=0x%08X)\n", hash);
        return -1;
    }
    int slot;
    if (s_d3d.vp_vs_n < VP_VS_CACHE) slot = s_d3d.vp_vs_n++;
    else {  /* full: replace round-robin IN PLACE. A memmove shifted every slot,
             * invalidating every pipeline keyed on a vs_idx and every record that
             * already holds one; the pipelines still validate vs_hash. */
        static int rr = 0;
        slot = rr; rr = (rr + 1) % VP_VS_CACHE;
        if (s_d3d.vp_vs[slot].vs) s_d3d.vp_vs[slot].vs->lpVtbl->Release(s_d3d.vp_vs[slot].vs);
        free(s_d3d.vp_vs[slot].src);
        memset(&s_d3d.vp_vs[slot], 0, sizeof(VPVSEntry));
        { static int _w = 0; if ((_w < 4 && _w++ < 4))
            fprintf(stderr, "[VSCACHE] full (%d vertex programs) -- replacing slot %d%c", VP_VS_CACHE, slot, 10); }
    }
    s_d3d.vp_vs[slot].hash = hash;
    s_d3d.vp_vs[slot].vs   = vb;
    { size_t _l = strlen(hlsl) + 1; s_d3d.vp_vs[slot].src = (char*)malloc(_l);
      if (s_d3d.vp_vs[slot].src) memcpy(s_d3d.vp_vs[slot].src, hlsl, _l); }
    {   /* v46: which constants does this VS read? */
        VPVSEntry* ve = &s_d3d.vp_vs[slot];
        memset(ve->cuse, 0, sizeof ve->cuse); ve->crel = 0; ve->cvalid = 1;
        for (const char* q = strstr(hlsl, "vp_c["); q; q = strstr(q + 5, "vp_c[")) {
            const char* r = q + 5;
            if (*r >= '0' && *r <= '9') {
                u32 v = (u32)strtoul(r, NULL, 10);
                if (v < 512u) ve->cuse[v >> 5] |= 1u << (v & 31u);
            } else ve->crel = 1;
        }
    }
    s_d3d.vp_vs[slot].uses_c03 =
        (strstr(hlsl, "vp_c[0]") || strstr(hlsl, "vp_c[1]") ||
         strstr(hlsl, "vp_c[2]") || strstr(hlsl, "vp_c[3]")) ? 1 : 0;
    { static int _n=0; if ((_n < 6 && _n++ < 6)) printf("[VP] per-draw VS cached (hash=0x%08X, %d instrs, slot %d)\n", hash, ni, slot); }
    /* Build the base VP pipeline here if it does not exist yet. render_frame's
     * trigger reads s_d3d.current_rsx_state, which by frame end no longer has
     * the microcode -- so for a title that only ever compiles per-draw VS the
     * base pipeline was never built, s_d3d.vp_ready stayed 0, and the VP draw
     * pass it gates dropped every recorded is_vp draw. Here we are at RECORD
     * time with a live state that definitely has microcode. */
    if (!s_d3d.vp_ready && st && st->vp_ucode_bytes >= 16) {
        const rsx_state* prev = s_d3d.current_rsx_state;
        s_d3d.current_rsx_state = st;
        compile_vp();
        s_d3d.current_rsx_state = prev;
    }
    if (s_d3d.vp_vs[slot].hash == hash) g_slot = slot;   /* v64 */
    return slot;
}
/* v225A: time vertex-shader misses (decompile + compile) on the FIFO thread */
static int vp_get_vs(const rsx_state* st)
{
    const u32 m0 = s_sf_vs_compiles; const unsigned long long t0 = hz_tsc();
    const int r = vp_get_vs_impl(st);
    if (s_sf_vs_compiles != m0) { hz_add(HZ_VS_TSC, (long long)(hz_tsc() - t0)); hz_add(HZ_VS_N, 1); }
    return r;
}

/* --- Guest blend state -> packed PSO key ----------------------------------
 * RSX blend factors/equation carry GL enums; each SFACTOR/DFACTOR/EQUATION
 * word packs (alpha<<16)|colour. The key packs D3D12 enums so the PSO cache
 * distinguishes real blend modes: bit0 enable, [5:1] srcC, [10:6] dstC,
 * [15:11] srcA, [20:16] dstA, [23:21] opC, [26:24] opA. DeferredShading's
 * light accumulation is additive (ONE,ONE) -- the old hardcoded straight-
 * alpha PSO turned each light pass into a screen-blanking flash. */
static u8 gl_blend_factor_d3d(u32 f, int is_alpha)
{
    switch (f & 0xFFFF) {
    case 0x0000: return D3D12_BLEND_ZERO;
    case 0x0001: return D3D12_BLEND_ONE;
    case 0x0300: return is_alpha ? D3D12_BLEND_SRC_ALPHA     : D3D12_BLEND_SRC_COLOR;
    case 0x0301: return is_alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D12_BLEND_SRC_ALPHA;
    case 0x0303: return D3D12_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D12_BLEND_DEST_ALPHA;
    case 0x0305: return D3D12_BLEND_INV_DEST_ALPHA;
    case 0x0306: return is_alpha ? D3D12_BLEND_DEST_ALPHA     : D3D12_BLEND_DEST_COLOR;
    case 0x0307: return is_alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D12_BLEND_SRC_ALPHA_SAT;
    case 0x8001: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_COLOR   */
    case 0x8002: return D3D12_BLEND_INV_BLEND_FACTOR;
    case 0x8003: return D3D12_BLEND_BLEND_FACTOR;       /* CONSTANT_ALPHA ~ */
    case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
    default:     return D3D12_BLEND_ONE;
    }
}
static u8 gl_blend_op_d3d(u32 e)
{
    switch (e & 0xFFFF) {
    case 0x8007: return D3D12_BLEND_OP_MIN;
    case 0x8008: return D3D12_BLEND_OP_MAX;
    case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
    default:     return D3D12_BLEND_OP_ADD;   /* 0x8006 FUNC_ADD / unset */
    }
}
/* Pack the guest's face-culling state for the PSO key.
 * bit0 = cull enabled, bit1 = cull FRONT (else BACK), bit2 = front face is CCW.
 * RSX/GL enums: CULL_FACE FRONT=0x0404 BACK=0x0405 FRONT_AND_BACK=0x0408;
 * FRONT_FACE CW=0x0900 CCW=0x0901. rsx_commands seeds cull_face/front_face with
 * plain 1/0 before any register arrives, so treat those defaults as BACK/CW. */
static int zs_on(void);   /* depth surfaces, defined with the zeta table */
/* DISPLAY_AS_RT (2026-10-04 evening): display buffers are ordinary render targets.
 * inFamous uses display buffer 1 (0x1734000) as its LIGHTING ACCUMULATION target
 * and samples it as a texture afterwards (and in the bloom chain). Drawing every
 * display-surface op straight into the swapchain backbuffer left that texture
 * reading stale guest VRAM -> black after the title screen. Now display ops draw
 * into an OffRT keyed by the display buffer's offset; at the end of the frame the
 * buffer that received the last display draw is copied to the backbuffer.
 * DISPLAY_AS_RT=0 restores the old direct-to-backbuffer path. */
static int disp_as_rt(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("DISPLAY_AS_RT"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* v33 GUEST STENCIL. inFamous tags materials in the G-buffer pass (stencil ALWAYS/REPLACE,
 * ref 0..6, write mask 7) and its light/composite passes test it (NOTEQUAL ref 4 mask 4 ...).
 * The backend never enabled stencil, so every masked pass covered the whole screen.
 * Distinct stencil states are interned into s_stk[] and their index (1..255) rides in
 * bits 16..23 of the cull/depth PSO key; the reference is per draw (OMSetStencilRef).
 * GUEST_STENCIL=0 -> old behaviour (stencil off). */
typedef struct { u8 en; u8 rmask, wmask; u8 f[2], fail[2], zfail[2], zpass[2]; } StencilKey;
static StencilKey s_stk[256]; static int s_stk_n = 1;
static u8 st_op(u32 gl)
{
    switch (gl) {
    case 0x0000: return D3D12_STENCIL_OP_ZERO;      case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;  case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D12_STENCIL_OP_INVERT;    case 0x8507: return D3D12_STENCIL_OP_INCR;
    case 0x8508: return D3D12_STENCIL_OP_DECR;      default:     return D3D12_STENCIL_OP_KEEP;
    }
}
static u32 rsx_stencil_idx(const rsx_state* st)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GUEST_STENCIL"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !st || !st->stencil_test_enable) return 0;
    static const u8 fm[8] = { D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
        D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER, D3D12_COMPARISON_FUNC_NOT_EQUAL,
        D3D12_COMPARISON_FUNC_GREATER_EQUAL, D3D12_COMPARISON_FUNC_ALWAYS };
    StencilKey k; memset(&k, 0, sizeof k);
    k.en = 1; k.rmask = (u8)st->stencil_mask; k.wmask = (u8)st->stencil_wmask;
    k.f[0] = fm[st->stencil_func & 7u]; k.fail[0] = st_op(st->stencil_op_fail);
    k.zfail[0] = st_op(st->stencil_op_zfail); k.zpass[0] = st_op(st->stencil_op_zpass);
    if (st->stencil2_enable) {
        k.f[1] = fm[st->bstencil_func & 7u]; k.fail[1] = st_op(st->bstencil_op_fail);
        k.zfail[1] = st_op(st->bstencil_op_zfail); k.zpass[1] = st_op(st->bstencil_op_zpass);
    } else { k.f[1] = k.f[0]; k.fail[1] = k.fail[0]; k.zfail[1] = k.zfail[0]; k.zpass[1] = k.zpass[0]; }
    for (int i = 1; i < s_stk_n; i++) if (memcmp(&s_stk[i], &k, sizeof k) == 0) return (u32)i;
    if (s_stk_n >= 256) return 0;
    s_stk[s_stk_n] = k;
    { static int n = 0; if ((n < 24 && n++ < 24)) fprintf(stderr, "[STENCIL] state %d: func %u ops %u/%u/%u rmask 0x%02X wmask 0x%02X (back func %u)%c",
        s_stk_n, k.f[0], k.fail[0], k.zfail[0], k.zpass[0], k.rmask, k.wmask, k.f[1], 10); }
    return (u32)s_stk_n++;
}
/* v35 DEPTH_BOUNDS (default 1; 0 = ignore the guest's depth bounds test). Needs
 * D3D12_OPTIONS2.DepthBoundsTestSupported; the pipeline is then created from a state
 * stream with DEPTH_STENCIL1 (create_gfx_pso) and the bounds set per draw. */
/* v36 ALPHA_TEST (default 1; 0 = old: never tested). The D3D12 path never applied the
 * guest alpha test -- rsx_fp_apply_alpha_test* existed but nothing called them. */
static int atest_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("ALPHA_TEST"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
/* Wrap the program's final return in the alpha test: ref from b1 (rsx_b1_pre[4].y, written
 * per draw by vp_record_cb), func baked from the PSO key. Buffered-constant programs only. */
/* v166: signed-blend passes (see rsx_blend_key). The guest's 8-bit output byte b is a two's-complement value
 * s = (b >= 128 ? b - 256 : b) / 255; pass P returns max(s,0), pass N max(-s,0) for the signed channels. */
static int fp_signed_rewrite(char* hlsl, size_t cap, u32 blend)
{
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    char* expr = marker + 11;
    char* term = strstr(expr, ";\n}");
    if (!term || term == expr || (size_t)(term - expr) >= 160) return -1;
    char out[192]; memcpy(out, expr, (size_t)(term - expr)); out[term - expr] = 0;
    const int pn = (blend >> 30) & 1, sc = (blend >> 27) & 1, sa = (blend >> 28) & 1;
    char suf[1024];
    int n = snprintf(suf, sizeof suf,
        "    float4 _sg_o = (%s);\n"
        "    float4 _sg_b = round(saturate(_sg_o) * 255.0);\n"
        "    float4 _sg_s = (_sg_b >= 128.0 ? _sg_b - 256.0 : _sg_b) / 255.0;\n"
        "    float4 _sg_r = %s;\n"
        "    return float4(%s, %s);\n}\n",
        out, pn ? "max(-_sg_s, 0.0)" : "max(_sg_s, 0.0)",
        sc ? "_sg_r.rgb" : (pn ? "(float3)0" : "_sg_o.rgb"),
        sa ? "_sg_r.a"   : (pn ? "0.0" : "_sg_o.a"));
    if (n < 0 || (size_t)n >= sizeof suf) return -1;
    if ((size_t)(marker - hlsl) + (size_t)n + 1 > cap) return -1;
    memcpy(marker, suf, (size_t)n + 1);
    return 1;
}
static int fp_alpha_rewrite(char* hlsl, size_t cap, u32 func)
{
    if (!strstr(hlsl, "rsx_b1_pre")) return 0;
    func &= 7u;
    if (func == 7u) return 0;                                  /* ALWAYS */
    char* marker = NULL;
    for (char* p = strstr(hlsl, "    return "); p; p = strstr(p + 1, "    return ")) marker = p;
    if (!marker) return -1;
    char* expr = marker + 11;
    char* term = strstr(expr, ";\n}");
    if (!term || term == expr || (size_t)(term - expr) >= 120) return -1;
    char out[128]; memcpy(out, expr, (size_t)(term - expr)); out[term - expr] = 0;
    const int mrt = strstr(hlsl, "struct PSOutput") != NULL;
    static const char* op[8] = { 0, "<", "==", "<=", ">", "!=", ">=", 0 };
    char pass[96];
    if (func == 0) snprintf(pass, sizeof pass, "false");
    else snprintf(pass, sizeof pass, "%s.a %s rsx_b1_pre[4].y", mrt ? "_rsx_out.t0" : "_rsx_out", op[func]);
    char suf[512];
    int n = snprintf(suf, sizeof suf, "    %s _rsx_out = %s;\n    if (!(%s)) discard;\n    return _rsx_out;\n}\n",
                     mrt ? "PSOutput" : "float4", out, pass);
    if (n < 0 || (size_t)n >= sizeof suf) return -1;
    if ((size_t)(marker - hlsl) + (size_t)n + 1 > cap) return -1;
    memcpy(marker, suf, (size_t)n + 1);
    return 1;
}
/* v73: draw records' fp_exp32 carries two SHADER_CONTROL facts: bit0 = 32-bit
 * exports (0x40), bit1 = depth export (0xE: the FP writes depth from r1.z).
 * Both are in the PSO key. inFamous' half-res depth pass (fph F94709BD) is a
 * depth-export program; without it every smoke/fire particle failed its depth
 * test. FP_DEPTH_EXPORT=0 turns the export off. */
static int fp_depth_export_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("FP_DEPTH_EXPORT"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int fp_ctrl_bits(u32 sc)
{
    return ((sc & 0x40u) ? 1 : 0) | (((sc & 0x0Eu) && fp_depth_export_on()) ? 2 : 0);
}
static u32 fp_ctrl_word(int bits)
{
    return ((bits & 1) ? 0x40u : 0u) | ((bits & 2) ? 0x0Eu : 0u);
}
/* v42b: uploading Y16_X16 (0x95, inFamous' 64x64 lookup on unit 1, ~11k draws/frame) removed the
 * D3D12 device right after the world loaded (play42). Off until understood; TEX95_UPLOAD=1 = on. */
static int tex95_upload(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TEX95_UPLOAD"); v = (e && *e == '0') ? 0 : 1; }   /* v73: ON -- the crash was the SRV format (R8_UNORM view of an RGBA8 resource), now fixed */
    return v;
}
/* v43 CUBE UNITS: the live decompile passed tex_cube_mask = 0, so every unit was declared
 * Texture2D and sampled with .xy -- while the prepass binds cube units as TextureCube SRVs (or
 * cube-NULL). Reading a cube SRV through a Texture2D declaration is undefined (returns ~0 on the
 * user's RTX 4060). inFamous' sun/light FPs sample the ambient IRRADIANCE cube (unit 5) and a
 * reflection cube (unit 6) -> ambient = 0 -> everything out of direct sun is pitch black (night
 * scenes, shadowed sides, Cole). The old fix-up after decompile looked for a `.xy *
 * rsx_texscale[N].xy` tail the decompiler no longer emits. Now the decompiler gets the cube units
 * (it then declares rsx_texN per unit, TextureCube for cubes, and samples them with .xyz).
 * CUBE_DECOMP=0 = old. */
static u32 fp_cube_units(u32 cube_mask)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("CUBE_DECOMP"); v = (e && *e == '0') ? 0 : 1; }
    return v ? (cube_mask & 0xFFFFu) : 0u;
}
static int dbt_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("DEPTH_BOUNDS"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int dbt_supported(void)
{
    static int v = -1;
    if (v < 0 && s_d3d.device) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS2 o2; memset(&o2, 0, sizeof o2);
        v = (SUCCEEDED(s_d3d.device->lpVtbl->CheckFeatureSupport(s_d3d.device, D3D12_FEATURE_D3D12_OPTIONS2, &o2, sizeof o2))
             && o2.DepthBoundsTestSupported) ? 1 : 0;
        fprintf(stderr, "[DBT] depth bounds test %s by the device%c", v ? "supported" : "NOT supported", 10);
    }
    return v > 0;
}
static size_t pss_add(u8* buf, size_t off, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE t, const void* data, size_t sz, size_t al)
{
    off = (off + 7) & ~(size_t)7;
    memcpy(buf + off, &t, sizeof t);
    size_t d = (off + sizeof t + al - 1) & ~(al - 1);
    memcpy(buf + d, data, sz);
    return d + sz;
}
/* CreateGraphicsPipelineState, or -- when dbt -- the same pipeline with the depth bounds
 * test enabled via ID3D12Device2::CreatePipelineState. */
static HRESULT create_gfx_pso_raw(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt, ID3D12PipelineState** out)
{
    if (dbt && dbt_supported()) {
        ID3D12Device2* d2 = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device2, (void**)&d2)) && d2) {
            static u8 buf[1024]; size_t o = 0;
            D3D12_DEPTH_STENCIL_DESC1 ds; memset(&ds, 0, sizeof ds);
            ds.DepthEnable = pd->DepthStencilState.DepthEnable; ds.DepthWriteMask = pd->DepthStencilState.DepthWriteMask;
            ds.DepthFunc = pd->DepthStencilState.DepthFunc; ds.StencilEnable = pd->DepthStencilState.StencilEnable;
            ds.StencilReadMask = pd->DepthStencilState.StencilReadMask; ds.StencilWriteMask = pd->DepthStencilState.StencilWriteMask;
            ds.FrontFace = pd->DepthStencilState.FrontFace; ds.BackFace = pd->DepthStencilState.BackFace;
            ds.DepthBoundsTestEnable = TRUE;
            struct D3D12_RT_FORMAT_ARRAY rf; memset(&rf, 0, sizeof rf);
            rf.NumRenderTargets = pd->NumRenderTargets;
            for (int i = 0; i < 8; i++) rf.RTFormats[i] = pd->RTVFormats[i];
            ID3D12RootSignature* rs = pd->pRootSignature;
            UINT sm = pd->SampleMask;
            memset(buf, 0, sizeof buf);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &rs, sizeof rs, 8);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &pd->VS, sizeof pd->VS, 8);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &pd->PS, sizeof pd->PS, 8);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &pd->BlendState, sizeof pd->BlendState, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &sm, sizeof sm, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &pd->RasterizerState, sizeof pd->RasterizerState, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof ds, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &pd->InputLayout, sizeof pd->InputLayout, 8);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &pd->PrimitiveTopologyType, sizeof pd->PrimitiveTopologyType, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rf, sizeof rf, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &pd->DSVFormat, sizeof pd->DSVFormat, 4);
            o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &pd->SampleDesc, sizeof pd->SampleDesc, 4);
            o = (o + 7) & ~(size_t)7;
            D3D12_PIPELINE_STATE_STREAM_DESC sd; sd.SizeInBytes = o; sd.pPipelineStateSubobjectStream = buf;
            HRESULT hr = d2->lpVtbl->CreatePipelineState(d2, &sd, &IID_ID3D12PipelineState, (void**)out);
            d2->lpVtbl->Release(d2);
            { static int n = 0; if ((n < 3 && n++ < 3) || FAILED(hr))
                fprintf(stderr, "[DBT] pipeline with depth bounds test: hr=0x%08lX%c", (unsigned long)hr, 10); }
            if (SUCCEEDED(hr)) return hr;
        }
    }
    return s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, pd, &IID_ID3D12PipelineState, (void**)out);
}
/* ---- v225A PSO_LIB=1 (default off): persistent D3D12 pipeline library (shadercache\psolib.bin) --------------------
 * A pipeline miss costs FP decompile + shader-cache load + CreateGraphicsPipelineState; the driver compile inside the
 * last one is the slow part and runs on the render thread in the middle of a frame (traversal: every new material
 * = one long frame). With PSO_LIB the guest pipelines (vp root signature) are looked up by a hash of their full
 * description in an ID3D12PipelineLibrary loaded at start; misses are created as before and stored. A background
 * thread serialises the library every PSO_LIB_SAVE_S (default 20) s when it changed (Serialize is thread-safe per
 * the D3D12 docs; the library validates the desc on load, so a stale or colliding entry is just a miss). A driver
 * update makes CreatePipelineLibrary refuse the file -> start empty, overwritten at the next save.
 * [PSOLIB] lines: open/save + hit/miss counters. */
static ID3D12PipelineLibrary* s_psl = NULL;
static ID3D12PipelineLibrary1* s_psl1 = NULL;
static int s_psl_on = -1;
static int s_psl_phase = -1;   /* v235D PSO_LIB_PHASE=0/1: library lookups/stores only on that A/B phase parity */
static volatile LONG s_psl_dirty = 0;
static void* s_psl_blob = NULL;   /* the library keeps referencing it */
static volatile LONG s_psl_hit = 0, s_psl_miss = 0, s_psl_store = 0, s_psl_bad = 0;
static void psl_save(const char* why)
{
    if (!s_psl || !InterlockedExchange(&s_psl_dirty, 0)) return;
    const SIZE_T n = s_psl->lpVtbl->GetSerializedSize(s_psl);
    void* b = n ? malloc(n) : NULL;
    if (!b) return;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    HRESULT hr = s_psl->lpVtbl->Serialize(s_psl, b, n);
    int ok = 0;
    if (SUCCEEDED(hr)) {
        FILE* o = fopen("shadercache\\psolib.bin.tmp", "wb");
        if (o) { ok = fwrite(b, 1, n, o) == n; fclose(o);
                 if (ok) ok = MoveFileExA("shadercache\\psolib.bin.tmp", "shadercache\\psolib.bin", MOVEFILE_REPLACE_EXISTING) != 0; }
    }
    free(b);
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[PSOLIB] v225A saved (%s): %.1f MB, %s, %.0f ms; hits %ld, misses %ld, stored %ld%c", why, n / 1048576.0,
            ok ? "ok" : "FAILED", (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart,
            (long)s_psl_hit, (long)s_psl_miss, (long)s_psl_store, 10);
    if (!ok) InterlockedExchange(&s_psl_dirty, 1);
}
static DWORD WINAPI psl_save_thread(LPVOID p)
{
    const DWORD ms = (DWORD)(uintptr_t)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) { Sleep(ms); psl_save("periodic"); }
    return 0;
}
void rsx_d3d12_psolib_flush(void) { psl_save("exit"); }   /* v225A: called from the exit paths when available */
static int psl_init(void)
{
    if (s_psl_on >= 0) return s_psl_on;
    const char* e = getenv("PSO_LIB");
    s_psl_on = 0;
    { const char* ph = getenv("PSO_LIB_PHASE"); if (ph && *ph) s_psl_phase = atoi(ph) & 1; }
    if (!(e && *e == '1') || !s_d3d.device) return 0;
    fprintf(stderr, "[PSOLIB] v235D phase parity %d (PSO_LIB_PHASE)%c", s_psl_phase, 10);
    ID3D12Device1* d1 = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device, &IID_ID3D12Device1, (void**)&d1)) || !d1) {
        fprintf(stderr, "[PSOLIB] v225A no ID3D12Device1 -> off%c", 10); return 0; }
    CreateDirectoryA("shadercache", NULL);
    size_t n = 0; HRESULT hr = E_FAIL;
    FILE* f = fopen("shadercache\\psolib.bin", "rb");
    if (f) {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 0) { s_psl_blob = malloc((size_t)sz);
            if (s_psl_blob && fread(s_psl_blob, 1, (size_t)sz, f) == (size_t)sz) n = (size_t)sz; }
        fclose(f);
    }
    if (n) {
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, s_psl_blob, n, &IID_ID3D12PipelineLibrary, (void**)&s_psl);
        fprintf(stderr, "[PSOLIB] v225A opened shadercache\\psolib.bin (%.1f MB): hr=0x%08lX%s%c", n / 1048576.0, (unsigned long)hr,
                FAILED(hr) ? " (driver changed or file bad -> starting empty)" : "", 10);
    }
    if (FAILED(hr) || !s_psl) {
        s_psl = NULL; free(s_psl_blob); s_psl_blob = NULL;
        hr = d1->lpVtbl->CreatePipelineLibrary(d1, NULL, 0, &IID_ID3D12PipelineLibrary, (void**)&s_psl);
        if (FAILED(hr) || !s_psl) { fprintf(stderr, "[PSOLIB] v225A CreatePipelineLibrary failed hr=0x%08lX -> off%c", (unsigned long)hr, 10);
                                    d1->lpVtbl->Release(d1); s_psl = NULL; return 0; }
        fprintf(stderr, "[PSOLIB] v225A new empty pipeline library%c", 10);
    }
    d1->lpVtbl->Release(d1);
    if (FAILED(s_psl->lpVtbl->QueryInterface(s_psl, &IID_ID3D12PipelineLibrary1, (void**)&s_psl1))) s_psl1 = NULL;
    { const char* se = getenv("PSO_LIB_SAVE_S"); int s = (se && *se) ? atoi(se) : 20; if (s < 2) s = 2;
      HANDLE th = CreateThread(NULL, 0, psl_save_thread, (LPVOID)(uintptr_t)(s * 1000u), 0, NULL); if (th) CloseHandle(th); }
    s_psl_on = 1;
    return 1;
}
static unsigned long long psl_h(unsigned long long h, const void* p, size_t n)
{
    const u8* b = (const u8*)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
#define PSL_HV(h, v) do { const unsigned long long _v = (unsigned long long)(v); h = psl_h(h, &_v, sizeof _v); } while (0)
static void psl_name(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt, WCHAR* name)
{
    unsigned long long h = 1469598103934665603ull;
    PSL_HV(h, 0x225A0001u); PSL_HV(h, dbt);
    PSL_HV(h, pd->VS.BytecodeLength); h = psl_h(h, pd->VS.pShaderBytecode, pd->VS.BytecodeLength);
    PSL_HV(h, pd->PS.BytecodeLength); h = psl_h(h, pd->PS.pShaderBytecode, pd->PS.BytecodeLength);
    PSL_HV(h, pd->GS.BytecodeLength); if (pd->GS.BytecodeLength) h = psl_h(h, pd->GS.pShaderBytecode, pd->GS.BytecodeLength);
    PSL_HV(h, pd->HS.BytecodeLength); PSL_HV(h, pd->DS.BytecodeLength);
    PSL_HV(h, pd->BlendState.AlphaToCoverageEnable); PSL_HV(h, pd->BlendState.IndependentBlendEnable);
    for (int i = 0; i < 8; i++) { const D3D12_RENDER_TARGET_BLEND_DESC* r = &pd->BlendState.RenderTarget[i];
        PSL_HV(h, r->BlendEnable); PSL_HV(h, r->LogicOpEnable); PSL_HV(h, r->SrcBlend); PSL_HV(h, r->DestBlend); PSL_HV(h, r->BlendOp);
        PSL_HV(h, r->SrcBlendAlpha); PSL_HV(h, r->DestBlendAlpha); PSL_HV(h, r->BlendOpAlpha); PSL_HV(h, r->LogicOp); PSL_HV(h, r->RenderTargetWriteMask); }
    PSL_HV(h, pd->SampleMask);
    { const D3D12_RASTERIZER_DESC* r = &pd->RasterizerState;
      PSL_HV(h, r->FillMode); PSL_HV(h, r->CullMode); PSL_HV(h, r->FrontCounterClockwise); PSL_HV(h, r->DepthBias);
      h = psl_h(h, &r->DepthBiasClamp, 4); h = psl_h(h, &r->SlopeScaledDepthBias, 4);
      PSL_HV(h, r->DepthClipEnable); PSL_HV(h, r->MultisampleEnable); PSL_HV(h, r->AntialiasedLineEnable);
      PSL_HV(h, r->ForcedSampleCount); PSL_HV(h, r->ConservativeRaster); }
    { const D3D12_DEPTH_STENCIL_DESC* d = &pd->DepthStencilState;
      PSL_HV(h, d->DepthEnable); PSL_HV(h, d->DepthWriteMask); PSL_HV(h, d->DepthFunc); PSL_HV(h, d->StencilEnable);
      PSL_HV(h, d->StencilReadMask); PSL_HV(h, d->StencilWriteMask);
      const D3D12_DEPTH_STENCILOP_DESC* o2[2] = { &d->FrontFace, &d->BackFace };
      for (int i = 0; i < 2; i++) { PSL_HV(h, o2[i]->StencilFailOp); PSL_HV(h, o2[i]->StencilDepthFailOp);
                                    PSL_HV(h, o2[i]->StencilPassOp); PSL_HV(h, o2[i]->StencilFunc); } }
    PSL_HV(h, pd->InputLayout.NumElements);
    for (UINT i = 0; i < pd->InputLayout.NumElements; i++) { const D3D12_INPUT_ELEMENT_DESC* el = &pd->InputLayout.pInputElementDescs[i];
        if (el->SemanticName) h = psl_h(h, el->SemanticName, strlen(el->SemanticName));
        PSL_HV(h, el->SemanticIndex); PSL_HV(h, el->Format); PSL_HV(h, el->InputSlot); PSL_HV(h, el->AlignedByteOffset);
        PSL_HV(h, el->InputSlotClass); PSL_HV(h, el->InstanceDataStepRate); }
    PSL_HV(h, pd->IBStripCutValue); PSL_HV(h, pd->PrimitiveTopologyType); PSL_HV(h, pd->NumRenderTargets);
    for (int i = 0; i < 8; i++) PSL_HV(h, pd->RTVFormats[i]);
    PSL_HV(h, pd->DSVFormat); PSL_HV(h, pd->SampleDesc.Count); PSL_HV(h, pd->SampleDesc.Quality);
    PSL_HV(h, pd->NodeMask); PSL_HV(h, pd->Flags);
    swprintf(name, 40, L"v225p%016llx", h);
}
/* the depth-bounds stream of create_gfx_pso_raw, for LoadPipeline */
static void psl_dbt_stream(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, u8* buf, size_t cap, D3D12_PIPELINE_STATE_STREAM_DESC* sd)
{
    size_t o = 0;
    D3D12_DEPTH_STENCIL_DESC1 ds; memset(&ds, 0, sizeof ds);
    ds.DepthEnable = pd->DepthStencilState.DepthEnable; ds.DepthWriteMask = pd->DepthStencilState.DepthWriteMask;
    ds.DepthFunc = pd->DepthStencilState.DepthFunc; ds.StencilEnable = pd->DepthStencilState.StencilEnable;
    ds.StencilReadMask = pd->DepthStencilState.StencilReadMask; ds.StencilWriteMask = pd->DepthStencilState.StencilWriteMask;
    ds.FrontFace = pd->DepthStencilState.FrontFace; ds.BackFace = pd->DepthStencilState.BackFace;
    ds.DepthBoundsTestEnable = TRUE;
    struct D3D12_RT_FORMAT_ARRAY rf; memset(&rf, 0, sizeof rf);
    rf.NumRenderTargets = pd->NumRenderTargets;
    for (int i = 0; i < 8; i++) rf.RTFormats[i] = pd->RTVFormats[i];
    ID3D12RootSignature* rs = pd->pRootSignature;
    UINT sm = pd->SampleMask;
    memset(buf, 0, cap);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, &rs, sizeof rs, 8);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, &pd->VS, sizeof pd->VS, 8);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS, &pd->PS, sizeof pd->PS, 8);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND, &pd->BlendState, sizeof pd->BlendState, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK, &sm, sizeof sm, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, &pd->RasterizerState, sizeof pd->RasterizerState, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL1, &ds, sizeof ds, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_INPUT_LAYOUT, &pd->InputLayout, sizeof pd->InputLayout, 8);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, &pd->PrimitiveTopologyType, sizeof pd->PrimitiveTopologyType, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS, &rf, sizeof rf, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT, &pd->DSVFormat, sizeof pd->DSVFormat, 4);
    o = pss_add(buf, o, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, &pd->SampleDesc, sizeof pd->SampleDesc, 4);
    o = (o + 7) & ~(size_t)7;
    sd->SizeInBytes = o; sd->pPipelineStateSubobjectStream = buf;
}
static HRESULT create_gfx_pso(const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pd, int dbt, ID3D12PipelineState** out)
{
    extern volatile int g_ab_phase;
    if (!psl_init() || pd->pRootSignature != s_d3d.vp_root_sig ||
        (s_psl_phase >= 0 && (g_ab_phase & 1) != s_psl_phase)) return create_gfx_pso_raw(pd, dbt, out);
    const int use_dbt = dbt && dbt_supported();
    WCHAR name[40]; psl_name(pd, use_dbt, name);
    HRESULT hr = E_FAIL;
    if (use_dbt) {
        if (s_psl1) { static u8 buf[1024]; D3D12_PIPELINE_STATE_STREAM_DESC sd; psl_dbt_stream(pd, buf, sizeof buf, &sd);
                      hr = s_psl1->lpVtbl->LoadPipeline(s_psl1, name, &sd, &IID_ID3D12PipelineState, (void**)out); }
    } else hr = s_psl->lpVtbl->LoadGraphicsPipeline(s_psl, name, pd, &IID_ID3D12PipelineState, (void**)out);
    if (SUCCEEDED(hr) && *out) { InterlockedIncrement(&s_psl_hit); return hr; }
    if (hr != E_INVALIDARG) InterlockedIncrement(&s_psl_bad);   /* E_INVALIDARG = not in the library (or desc mismatch) */
    *out = NULL;
    hr = create_gfx_pso_raw(pd, dbt, out);
    InterlockedIncrement(&s_psl_miss);
    if (SUCCEEDED(hr) && *out && SUCCEEDED(s_psl->lpVtbl->StorePipeline(s_psl, name, *out))) {
        InterlockedIncrement(&s_psl_store); InterlockedExchange(&s_psl_dirty, 1); }
    { static int n = 0; if (n < 4 || ((s_psl_hit + s_psl_miss) % 256) == 0) { n++;
        fprintf(stderr, "[PSOLIB] hits %ld, misses %ld, stored %ld, other load errors %ld (last hr 0x%08lX)%c",
                (long)s_psl_hit, (long)s_psl_miss, (long)s_psl_store, (long)s_psl_bad, (unsigned long)hr, 10); } }
    return hr;
}
/* Per-draw bounds (only draws whose pipeline has the test). */
static void dbt_set(float mn, float mx)
{
    static ID3D12GraphicsCommandList* for_cl = NULL; static ID3D12GraphicsCommandList1* cl1 = NULL;
    if (for_cl != s_d3d.cmd_list) {
        if (cl1) { cl1->lpVtbl->Release(cl1); cl1 = NULL; }
        for_cl = s_d3d.cmd_list;
        if (for_cl) for_cl->lpVtbl->QueryInterface(for_cl, &IID_ID3D12GraphicsCommandList1, (void**)&cl1);
    }
    if (!cl1) return;
    if (!(mn >= 0.0f)) mn = 0.0f; if (mn > 1.0f) mn = 1.0f;
    if (!(mx >= 0.0f)) mx = 0.0f; if (mx > 1.0f) mx = 1.0f;
    if (mn > mx) { float t = mn; mn = mx; mx = t; }
    cl1->lpVtbl->OMSetDepthBounds(cl1, mn, mx);
}
static void rec_zrange(D3D12DrawRecord* dr)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("GUEST_DEPTH_RANGE"); on = (e && *e == '1') ? 1 : 0; }   /* v34: default OFF -- the VS epilogue already applies the guest z scale/offset (ZFLAT); a viewport range on top maps z twice */
    const rsx_state* st = s_d3d.current_rsx_state;
    float a = 0.0f, b = 1.0f;
    if (on && st) { a = st->clip_min; b = st->clip_max; }
    if (!(a >= 0.0f)) a = 0.0f; if (a > 1.0f) a = 1.0f;   /* also catches NaN */
    if (!(b >= 0.0f)) b = 0.0f; if (b > 1.0f) b = 1.0f;
    if (a > b) { float t = a; a = b; b = t; }
    dr->zmin = a; dr->zmax = b;
    dr->dbmin = st ? st->depth_bounds_min : 0.0f;   /* v35 */
    dr->dbmax = st ? st->depth_bounds_max : 1.0f;
}
static u32 rsx_cull_key(const rsx_state* st)
{
    if (!st) return 0;
    u32 k = 0;
    if (st->cull_face_enable) {
        k = 1u;
        if (st->cull_face == 0x0404u || st->cull_face == 0x0408u) k |= 2u;  /* FRONT */
    }
    /* v47: the front-face winding is needed without culling too -- two-sided materials
     * read FACING (SV_IsFrontFace), which follows FrontCounterClockwise. */
    if (st->front_face == 0x0901u) k |= 4u;                                 /* CCW */
    /* Guest depth state rides in the same key (bits 8..15) so pipelines differ by
     * it: bit 15 = present, bit 8 = test enable, bit 9 = write, bits 10-12 =
     * func index (RSX/GL 0x200+i: NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL
     * ALWAYS). Fixed LESS_EQUAL+write-always only worked while depth was wiped on
     * every target switch. GUEST_DEPTH_STATE=0 restores it. */
    static int gd = -1;
    if (gd < 0) { const char* e = getenv("GUEST_DEPTH_STATE"); gd = (e && *e == '0') ? 0 : 1; }
    if (gd && zs_on()) {
        k |= 0x8000u;
        if (st->depth_test_enable) k |= 0x100u;
        if (st->depth_mask)        k |= 0x200u;
        k |= (st->depth_func & 7u) << 10;
        k |= rsx_stencil_idx(st) << 16;   /* v33 */
        if (st->depth_bounds_enable && dbt_on()) k |= 1u << 24;   /* v35 */
    }
    /* v36 alpha test: bit 25 = on, bits 26-28 = func (RSX/GL 0x200+i); ref per draw in b1 */
    if (st->alpha_test_enable && atest_on()) k |= (1u << 25) | ((st->alpha_func & 7u) << 26);
    /* v39 ALPHA-TO-COVERAGE (NV4097 0x1D7C bit 4) on our single-sample targets = keep the fragment
     * when alpha >= 0.5 (what RPCS3 does without MSAA). Rides the alpha-test rewrite: bit 25 + func
     * GEQUAL, bit 29 marks it so vp_record_cb puts ref 0.5 in b1. A2C=0 off, A2C=2 only when the MSAA
     * enable bit is set too. Log [A2C]. */
    { extern u32 g_rsx_aa_control;
      static int am = -1; if (am < 0) { const char* e = getenv("A2C"); am = e ? atoi(e) : 1; }
      u32 aa = g_rsx_aa_control;
      if (am && (aa & 0x10u) && (am != 2 || (aa & 1u)) && !(st->alpha_test_enable && atest_on())) {
          k |= (1u << 25) | (6u << 26) | (1u << 29);
          static int _n = 0; if ((_n < 8 && _n++ < 8))
              fprintf(stderr, "[A2C] alpha-to-coverage draw (aa 0x%08X, fp 0x%X) -> discard alpha < 0.5%c",
                      aa, st->shader_program, 10);
      } }
    return k;
}

static u32 rsx_blend_key(const rsx_state* st, int enable)
{
    { static int _bd = -1; if (_bd < 0) _bd = getenv("BLENDDBG") ? 1 : 0;
      if (_bd) { static u32 seen[32]; static int ns=0;
        u32 sk = (enable?0x80000000u:0u) | (st ? (st->blend_sfactor & 0xFFFFu) : 0u);
        int f=0; for (int k2=0;k2<ns;k2++) if (seen[k2]==sk) f=1;
        if (!f && ns<32) { seen[ns++]=sk;
            fprintf(stderr, "[BLEND] enable=%d sfactor=0x%X dfactor=0x%X%c", enable,
                    st?st->blend_sfactor:0, st?st->blend_dfactor:0, 10); } } }
    if (!enable) return 0;
    /* Factors never programmed: keep the legacy straight-alpha behaviour
     * (dbgfont-style text enables blending without setting factors). */
    if (!st || (st->blend_sfactor == 0 && st->blend_dfactor == 0))
        return 1u
             | ((u32)D3D12_BLEND_SRC_ALPHA     << 1)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 6)
             | ((u32)D3D12_BLEND_ONE           << 11)
             | ((u32)D3D12_BLEND_INV_SRC_ALPHA << 16)
             | ((u32)D3D12_BLEND_OP_ADD << 21) | ((u32)D3D12_BLEND_OP_ADD << 24);
    u32 k = 1u
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor,       0) << 1)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor,       0) << 6)
         | ((u32)gl_blend_factor_d3d(st->blend_sfactor >> 16, 1) << 11)
         | ((u32)gl_blend_factor_d3d(st->blend_dfactor >> 16, 1) << 16)
         | ((u32)gl_blend_op_d3d(st->blend_equation)       << 21)
         | ((u32)gl_blend_op_d3d(st->blend_equation >> 16) << 24);
    /* v166 SIGNED BLEND (BLEND_SIGNED=0 = off): RSX FUNC_REVERSE_SUBTRACT_SIGNED 0xF005, FUNC_ADD_SIGNED 0xF006,
     * FUNC_REVERSE_ADD_SIGNED 0xF007 read the SOURCE bytes as two's complement (0x80..0xFF = negative) and add them
     * to the (offset-binary) destination: dst + signed(src). inFamous' smoke heat haze (fph 641CCF5F/2A21BF04,
     * ONE/ONE, 0xF006F006) accumulates screen offsets that way into a 0.5-cleared 320x180 target; as a plain
     * unsigned ADD every negative offset saturated the channel -> hard-edged "transparent holes" in smoke.
     * D3D12 has no signed op and clamps UNORM sources to [0,1], so the draw runs twice (see vp_get_fp_pso and the
     * draw loop): pass P adds max(s,0), pass N (key bit 30) reverse-subtracts max(-s,0).
     * Key bits: 27 = colour equation signed, 28 = alpha equation signed, 29 = colour equation is the
     * reverse-subtract form (dst - s), 30 = pass N. */
    { static int sg = -1; if (sg < 0) { const char* e = getenv("BLEND_SIGNED"); sg = (e && *e == '0') ? 0 : 1; }
      const u32 ec = st->blend_equation & 0xFFFFu, ea = st->blend_equation >> 16;
      const int sc = ec >= 0xF005u && ec <= 0xF007u, sa = ea >= 0xF005u && ea <= 0xF007u;
      if (sg && (sc || sa)) {
          k |= (sc ? 1u << 27 : 0u) | (sa ? 1u << 28 : 0u) | (ec == 0xF005u ? 1u << 29 : 0u);
          static int _n = 0; if ((_n < 4 && _n++ < 4))
              fprintf(stderr, "[BLENDSGN] v166 signed blend equation 0x%08X (sf 0x%X df 0x%X) -> two-pass dst + signed(src)%c",
                      st->blend_equation, st->blend_sfactor, st->blend_dfactor, 10);
      } }
    return k;
}

/* Bound colour target count for a draw record: 1 (target A) + the contiguous
 * prefix of MRT B/C/D offsets. */
static int dr_num_rts(const D3D12DrawRecord* dr)
{
    int n = 1;
    while (n < 4 && dr->rt_mrt[n - 1]) n++;
    return n;
}

/* Replace every occurrence of `find` with `repl` inside a NUL-terminated buffer,
 * shifting the tail. Used to retarget generated HLSL (2D sampler -> cube). */
static void hlsl_replace_all(char* buf, size_t cap, const char* find, const char* repl)
{
    size_t fl = strlen(find), rl = strlen(repl);
    if (!fl) return;
    char* at = buf;
    while ((at = strstr(at, find)) != NULL) {
        size_t used = strlen(buf) + 1;
        if (used + rl - fl > cap) return;
        memmove(at + rl, at + fl, used - (size_t)(at - buf) - fl);
        memcpy(at, repl, rl);
        at += rl;
    }
}

/* Which of a draw's texture units are cube textures, as a 4-bit mask. */
static u32 dr_cube_mask(const D3D12DrawRecord* dr)
{
    /* ON by default (RSX_CUBE_TEX=0 to disable). This was off while the cube path
     * cost 2262 PSO misses per 20 frames and 0.38 fps -- both caused by inline
     * fragment-program constants baking into the shader source. With those
     * constants hoisted into b1 the cube path costs 0 PSO misses, and with the
     * per-face stride fixed (each face is a whole mip pyramid, not one mip-0
     * image) all six faces decode as clean environment art. */
    static int en = -1;
    if (en < 0) { const char* e = getenv("RSX_CUBE_TEX"); en = e ? atoi(e) : 1; }
    if (!en) return 0;
    u32 m = 0;
    for (int u = 0; u < RSX_D3D_TEX_UNITS; u++) if (dr->tex[u].set && dr->tex[u].cube) m |= 1u << u;
    return m;
}


/* ---- fragment-program scanning (pipeline key + b1 constants) ----
 * Walks a program exactly like the decompiler (rsx_fp_decompile_internal /
 * rsx_fp_collect_constants): every instruction that names a CONST source --
 * branch instructions included -- is followed by one 16-byte inline constant.
 * (rsx_fp_extract_consts/rsx_fp_code_hash skip branch instructions, which would
 * misalign the slots against the decompiler's numbering.) */
#define FP_K_SLOTS 64
#define FP_K_REGTYPE 2u   /* FP_REG_TYPE_CONST */
u32 rsx_fp_read_word(const u8* p);
u32 rsx_fp_sanitize_constant_word(u32 w);
static int fp_has_const(u32 w1, u32 w2, u32 w3)
{
    /* FP_REG_TYPE: bits 1:0 of each source word (FP_REG_TYPE_MASK/SHIFT in
     * rsx_fp_decompiler.c), CONST = FP_K_REGTYPE. */
    return ((w1 & 3u) == FP_K_REGTYPE) || ((w2 & 3u) == FP_K_REGTYPE) || ((w3 & 3u) == FP_K_REGTYPE);
}
static int fp_scan(const u8* uc, u32 max_bytes, u32* kcount, u32* code_h, u32* full_h, u32* used)
{
    u32 off = 0, k = 0, ch = 2166136261u, fh = 2166136261u;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(uc + off + 0), w1 = rsx_fp_read_word(uc + off + 4);
        u32 w2 = rsx_fp_read_word(uc + off + 8), w3 = rsx_fp_read_word(uc + off + 12);
        for (u32 i = 0; i < 16; i++) { ch ^= uc[off + i]; ch *= 16777619u; fh ^= uc[off + i]; fh *= 16777619u; }
        off += 16;
        if (fp_has_const(w1, w2, w3)) {
            if (off + 16 > max_bytes) break;
            for (u32 i = 0; i < 16; i++) { fh ^= uc[off + i]; fh *= 16777619u; }
            off += 16; k++;
        }
        if (w0 & 1u) {   /* FP_END */
            ch ^= k; ch *= 16777619u;
            *kcount = k; *code_h = ch ? ch : 1u; *full_h = fh ? fh : 1u; *used = off;
            return 1;
        }
    }
    *kcount = k; *code_h = ch; *full_h = fh; *used = off;
    return 0;
}
static int fp_fill_consts(const u8* uc, u32 max_bytes, float* out, int max_out)
{
    u32 off = 0; int n = 0;
    while (off + 16 <= max_bytes) {
        u32 w0 = rsx_fp_read_word(uc + off + 0), w1 = rsx_fp_read_word(uc + off + 4);
        u32 w2 = rsx_fp_read_word(uc + off + 8), w3 = rsx_fp_read_word(uc + off + 12);
        off += 16;
        if (fp_has_const(w1, w2, w3)) {
            if (off + 16 > max_bytes) break;
            if (n < max_out)
                for (int i = 0; i < 4; i++) {
                    u32 cw = rsx_fp_sanitize_constant_word(rsx_fp_read_word(uc + off + i * 4));
                    memcpy(&out[n * 4 + i], &cw, 4);
                }
            n++; off += 16;
        }
        if (w0 & 1u) break;
    }
    return n;
}

void rsx_vtx_sync(void);   /* v33 VTX_ASYNC, below */
static volatile LONG s_ww_epoch;   /* v219 VC_WW epoch (defined with VC_FASTKEY) */
static int s_va_n; static u64 s_va_syncs, s_va_jobs_total;   /* tentative; defined below */
static u64 s_va_inline_draws, s_va_inline_verts;
static u32 va_min(void);
static int va_submit(const u32* list, u32 first, u32 n, VPSlot* out);
static void defer_release(ID3D12Resource* r);   /* below, with off_rt_get */
/* ---- guest-FP PSO cache index ---- */
#define FPI_SIZE 16384u
static int s_fpi[FPI_SIZE];      /* open addressing: VPFPEntry index + 1, 0 = empty */
static u32 s_fpi_inserts;
static u32 fp_key_hash(u32 fp_addr, int vs_idx, u32 vs_hash, u32 gen, u32 blend, int nrt,
                       u32 rtfmt, int exp32, u32 uhash, u32 cmask, u32 cull, u32 cube_mask)
{
    u32 k[12] = { fp_addr, (u32)vs_idx, vs_hash, gen, blend, (u32)nrt, rtfmt, (u32)exp32,
                  uhash, cmask, cull, cube_mask };
    u32 h = 2166136261u;
    for (int i = 0; i < 12; i++) { h ^= k[i]; h *= 16777619u; h ^= h >> 15; }
    return h;
}
static void fpi_insert(u32 kh, u32 idx);
static void fpi_rebuild(void)
{
    memset(s_fpi, 0, sizeof(s_fpi));
    s_fpi_inserts = 0;
    for (int i = 0; i < s_d3d.vp_fp_n; i++) {
        VPFPEntry* q = &s_d3d.vp_fp[i];
        if (!q->pso) continue;
        fpi_insert(fp_key_hash(q->kaddr, q->vs_idx, q->vs_hash, q->gen, q->blend, q->nrt,
                               q->rtfmt, q->exp32, q->ucode_hash, q->cmask, q->cull, q->cube_mask),
                   (u32)i);
    }
}
static void fpi_insert(u32 kh, u32 idx)
{
    /* Replaced entries leave stale index slots behind (they fail the key check
     * and are probed past); rebuild once they could crowd the table. */
    if (++s_fpi_inserts > FPI_SIZE / 2) { fpi_rebuild(); ++s_fpi_inserts; }
    for (u32 pr = 0; pr < FPI_SIZE; pr++) {
        u32 at = (kh + pr) & (FPI_SIZE - 1);
        if (!s_fpi[at] || s_fpi[at] == (int)idx + 1) { s_fpi[at] = (int)idx + 1; return; }
    }
}

/* v33 FP SNAPSHOTS. inFamous writes per-light fragment programs (constants baked into the
 * ucode) into a ring in main memory (0x00A3xxxx..0x00BExxxx) and REUSES ring slots within a
 * frame. The backend compiled programs in render_frame from guest memory -- i.e. whatever
 * the ring held at the END of the frame, not what each draw used: 1300 world draws were
 * shaded by a 2-instruction "output tex0" program. Each draw now snapshots its program
 * (up to FP_END) into a per-frame arena at record time; pipelines and constants use that. */
#define FPSNAP_BYTES (16u << 20)
static u8* s_fpsnap_a[3] = {NULL, NULL, NULL}; static u32 s_fpsnap_used_a[3] = {0, 0, 0};
static struct { u32 addr, h, len; const u8* p; } s_fpsc_a[3][1024];
/* v59: one arena per record parity -- the render thread reads frame N's snapshots while the
 * FIFO thread records frame N+1 into the other parity. */
#define s_fpsnap      s_fpsnap_a[s_d3d.vp_parity]
#define s_fpsnap_used s_fpsnap_used_a[s_d3d.vp_parity]
#define s_fpsc        s_fpsc_a[s_d3d.vp_parity]
static const u8* s_fp_uc_override = NULL;
static const D3D12DrawRecord* s_fp_meta_override = NULL;
static u32 s_fpsnap_ep = 1;   /* v221C FPR_FAST: bumped whenever an arena is reset (snapshot pointers die) */
static void fpsnap_reset(void) { s_fpsnap_used = 0; memset(s_fpsc, 0, sizeof s_fpsc); s_fpsnap_ep++; }
/* v64: fpsnap_get without the fp_scan (length + hash already known from the FP memo) */
static const u8* fpsnap_get_known(u32 fp_addr, const u8* src, u32 used, u32 fh)
{
    if (!s_fpsnap) { s_fpsnap = (u8*)malloc(FPSNAP_BYTES); if (!s_fpsnap) return NULL; }
    if (!used) return NULL;
    u32 slot = ((fp_addr >> 2) * 2654435761u ^ fh) & 1023u;
    if (s_fpsc[slot].p && s_fpsc[slot].addr == fp_addr && s_fpsc[slot].h == fh && s_fpsc[slot].len == used &&
        memcmp(s_fpsc[slot].p, src, used) == 0) return s_fpsc[slot].p;
    u32 need = (used + 64u + 15u) & ~15u;
    if (s_fpsnap_used + need > FPSNAP_BYTES) return NULL;
    u8* d = s_fpsnap + s_fpsnap_used; s_fpsnap_used += need;
    memcpy(d, src, used); memset(d + used, 0, need - used);
    s_fpsc[slot].addr = fp_addr; s_fpsc[slot].h = fh; s_fpsc[slot].len = used; s_fpsc[slot].p = d;
    return d;
}
/* v64 FP memo (FP_MEMO=0 off): ~10k draws/frame each re-scanned + FNV-hashed their fragment
 * program byte by byte (fp_fill_consts + fp_scan, ~1-2 us per draw on the FIFO thread). Now:
 * per fp address, a private copy of the program as last seen; if the guest bytes still match
 * it (one memcmp), reuse its inline constants, length and hash. Exact: any byte change misses. */
typedef struct { u32 addr, len, fh, nk, code_hash, const_count, tex_mask; u8* code; u32 cap; float k[FP_K_SLOTS * 4]; u8 isk[256]; u32 nisk;
                 const u8* sp; u32 sep, spar, smh, skw; float klast[FP_K_SLOTS * 4]; } FpMemo;   /* v219j nisk; v221C FPR_FAST: sp..klast */
/* Same program INSTRUCTIONS as the memo (inline constants may differ: the title re-patches
 * them per draw)? Then refill the constants from the guest bytes using the memo's layout.
 * Returns 1 (and a cheap 64-bit hash of all the bytes in *h) on a hit. */
static int fp_memo_match(const FpMemo* m, const u8* src, float* kout, u32* h)
{
    u32 nb = m->len >> 4, n = 0;
    u64 hh = 0x9E3779B97F4A7C15ull ^ m->len;
    for (u32 b = 0; b < nb; b++) {
        const u8* p = src + (b << 4);
        u64 w0, w1; memcpy(&w0, p, 8); memcpy(&w1, p + 8, 8);
        if (!m->isk[b]) {
            u64 c0, c1; memcpy(&c0, m->code + (b << 4), 8); memcpy(&c1, m->code + (b << 4) + 8, 8);
            if (w0 != c0 || w1 != c1) return 0;
        } else if (n < FP_K_SLOTS) {
            for (int i = 0; i < 4; i++) {
                u32 cw = rsx_fp_sanitize_constant_word(rsx_fp_read_word(p + i * 4));
                memcpy(&kout[n * 4 + i], &cw, 4);
            }
            n++;
        } else n++;
        hh = (hh ^ w0) * 0xFF51AFD7ED558CCDull; hh ^= hh >> 29;
        hh = (hh ^ w1) * 0xC4CEB9FE1A85EC53ull; hh ^= hh >> 31;
    }
    u32 r = (u32)(hh ^ (hh >> 32)); *h = r ? r : 1u;
    return 1;
}
static FpMemo s_fpm[512];
static u64 s_fpm_hit, s_fpm_miss;
static int fp_memo_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("FP_MEMO"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static int fp_binding_meta_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("FP_BINDING_META"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static const u8* fpsnap_get(u32 fp_addr, const u8* src, u32* hout)
{
    if (!s_fpsnap) { s_fpsnap = (u8*)malloc(FPSNAP_BYTES); if (!s_fpsnap) return NULL; }
    u32 k = 0, ch = 0, fh = 0, used = 0;
    fp_scan(src, 4096, &k, &ch, &fh, &used);
    if (!used) return NULL;
    u32 slot = ((fp_addr >> 2) * 2654435761u ^ fh) & 1023u;
    if (s_fpsc[slot].p && s_fpsc[slot].addr == fp_addr && s_fpsc[slot].h == fh && s_fpsc[slot].len == used &&
        memcmp(s_fpsc[slot].p, src, used) == 0) { *hout = fh; return s_fpsc[slot].p; }
    u32 need = (used + 64u + 15u) & ~15u;
    if (s_fpsnap_used + need > FPSNAP_BYTES) return NULL;   /* full: fall back to live memory */
    u8* d = s_fpsnap + s_fpsnap_used; s_fpsnap_used += need;
    memcpy(d, src, used); memset(d + used, 0, need - used);
    s_fpsc[slot].addr = fp_addr; s_fpsc[slot].h = fh; s_fpsc[slot].len = used; s_fpsc[slot].p = d;
    *hout = fh;
    return d;
}
/* v34 SHADOWS: turn every sample of a shadow-map unit into a depth compare.
 * shkey: bits 0-3 = units, bits 4-15 = zfunc (3 bits per unit; RSX/GL order NEVER LESS
 * EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS, result = TEXEL FUNC ref -- the RSX
 * compares the other way round from GL: inFamous sets GREATER, i.e. lit where the stored
 * depth is beyond the fragment; RPCS3 also reverses it). The emitter
 * writes a 2D sample as  rsx_tex[u].Sample(rsx_samp[u], (C).xy)  (TEX) or
 * ... (C).xy / (C).w)  (TXP); C is a float4 whose .z is the reference depth. The
 * texture is bound as the R24 view of the zeta surface; 2x2 PCF like the RSX's. */
static const char s_shadow_fn[] =
    "float4 rsx_shadow(Texture2D t, SamplerState s, float4 c, int proj, int zf) {\n"
    "    float3 p = proj ? c.xyz / c.w : c.xyz;\n"
    "    uint W, H; t.GetDimensions(W, H);\n"
    "    float2 ts = float2(W, H);\n"
    "    float2 f = frac(p.xy * ts - 0.5);\n"
    "    float4 d = t.GatherRed(s, p.xy);\n"
    "#if RSX_SHRP\n"
    /* v148: receiver-plane depth per PCF tap: dz/du, dz/dv of the receiver from the screen
     * derivatives (2x2 Jacobian), each gathered texel compared with the receiver's depth AT
     * THAT TEXEL'S CENTRE. Removes the acne v92/v94 fought without a distance-dependent bias. */
    "    float3 dx = ddx(p), dy = ddy(p);\n"
    "    float det = dx.x * dy.y - dx.y * dy.x;\n"
    "    float2 g = (abs(det) > 1e-24) ? float2(dx.z * dy.y - dx.y * dy.z, dx.x * dy.z - dy.x * dx.z) / det : (float2)0;\n"
    "    float2 o = f / ts;\n"
    "    float4 du = float4(-o.x, 1.0 / ts.x - o.x, 1.0 / ts.x - o.x, -o.x);\n"
    "    float4 dv = float4(1.0 / ts.y - o.y, 1.0 / ts.y - o.y, -o.y, -o.y);\n"
    "    float4 rp = g.x * du + g.y * dv;\n"
    "    if (any(abs(rp) > RSX_SHRPMAX) || any(isnan(rp))) rp = (float4)0;\n"
    "    float4 z = p.zzzz + rp - RSX_SHBIAS;\n"
    "#else\n"
    "    float zs = max(abs(ddx(p.z)), abs(ddy(p.z)));\n"
    "    float4 z = p.zzzz - (RSX_SHBIAS + RSX_SHSLOPE * min(zs, 0.01));\n"
    "#endif\n"
    "    float4 r = (float4)0;\n"
    "    if (zf == 1) r = (float4)(d < z); else if (zf == 2) r = (float4)(d == z);\n"
    "    else if (zf == 3) r = (float4)(d <= z); else if (zf == 4) r = (float4)(d > z);\n"
    "    else if (zf == 5) r = (float4)(d != z); else if (zf == 6) r = (float4)(d >= z);\n"
    "    else if (zf == 7) r = (float4)1;\n"
    "    float lit = lerp(lerp(r.w, r.z, f.x), lerp(r.x, r.y, f.x), f.y);\n"
    "    return lit.xxxx;\n"
    "}\n";
static int fp_shadow_rewrite(char* hlsl, size_t cap, u32 shkey)
{
    int patched = 0;
    for (int u = 0; u < 4; u++) {
        if (!((shkey >> u) & 1u)) continue;
        int zf = (int)((shkey >> (4 + 3 * u)) & 7u);
        { static int fl = -1; if (fl < 0) { const char* e = getenv("SHADOW_FLIP"); fl = (e && *e == '1') ? 1 : 0; }   /* v91: default OFF again -- with bx2 normals (v89) the rooftop sun pass lit exactly Cole's shadow shape (inverted) */   /* v48: default ON -- play47 cascades lit exactly where the casters are (inverted) */
          if (fl) { static const int sw[8] = {0, 4, 2, 6, 1, 5, 3, 7}; zf = sw[zf]; } }   /* SHADOW_FLIP=1: GL operand order */
        char needles[2][48];
        snprintf(needles[0], sizeof needles[0], "rsx_tex[%d].Sample(rsx_samp[%d], ", u, u);
        snprintf(needles[1], sizeof needles[1], "rsx_tex%d.Sample(rsx_samp[%d], ", u, u);
        for (int ni = 0; ni < 2; ni++) {
            char* from = hlsl;
            for (;;) {
                char* at = strstr(from, needles[ni]);
                if (!at) break;
                char* a = at + strlen(needles[ni]);          /* the coordinate */
                if (*a != '(') { from = a; continue; }
                char* q = a; int dep = 0;
                for (; *q; q++) { if (*q == '(') dep++; else if (*q == ')') { if (--dep == 0) break; } }
                if (!*q) break;
                char* g1e = q + 1;                           /* one past group 1 */
                int proj = 0; char* tail = NULL;
                if (strncmp(g1e, ".xy)", 4) == 0) tail = g1e + 4;
                else if (strncmp(g1e, ".xy / (", 7) == 0) {
                    char* r = g1e + 6; dep = 0;
                    for (; *r; r++) { if (*r == '(') dep++; else if (*r == ')') { if (--dep == 0) break; } }
                    if (!*r || strncmp(r + 1, ".w)", 3) != 0) { from = g1e; continue; }
                    proj = 1; tail = r + 4;
                } else { from = g1e; continue; }
                char head[64], trail[32], rbuf[1024];
                snprintf(head, sizeof head, ni ? "rsx_shadow(rsx_tex%d, rsx_samp[%d], " : "rsx_shadow(rsx_tex[%d], rsx_samp[%d], ", u, u);
                snprintf(trail, sizeof trail, ", %d, %d)", proj, zf);
                const size_t hl = strlen(head), gl = (size_t)(g1e - a), tl = strlen(trail);
                const size_t newlen = hl + gl + tl, oldlen = (size_t)(tail - at), used = strlen(hlsl);
                if (newlen >= sizeof rbuf) { from = tail; continue; }
                if (used - oldlen + newlen + 1 > cap) return -1;
                memcpy(rbuf, head, hl); memcpy(rbuf + hl, a, gl); memcpy(rbuf + hl + gl, trail, tl); rbuf[newlen] = 0;
                memmove(at + newlen, tail, strlen(tail) + 1);
                memcpy(at, rbuf, newlen);
                from = at + newlen;
                patched++;
            }
        }
    }
    if (patched) {   /* the helper references no globals: put it first */
        /* v92 SHADOW_BIAS (default 0.0015, in reference-depth units): after v91 un-flipped the
         * compare, sun-facing surfaces (Cole's head/jacket) showed shadow-acne stripes; the
         * caster depth and the receiver's reference differ by rounding. 0 = no bias. */
        /* v148 SHADOW_RP (default 1; 0 = the v92/v94 biases). The v94 slope term used the SCREEN
         * derivative of the reference depth, clamped at 0.01 x 1.5: in this deferred full-screen
         * light pass every 2x2 quad that straddles a depth edge (building silhouettes, window
         * recesses -- dense at distance, where a pixel spans metres) got a 0.015 bias, 5x the
         * game's own 0.003 shadow bias (the cascade FPs subtract 0.003*w for flagged materials),
         * plus the constant 0.0015 everywhere. Far buildings lost their shadows along edges and
         * the pattern crawled with every camera move. Now: per-tap receiver-plane depth (see
         * s_shadow_fn), corrections larger than SHADOW_RP_MAX (default 0.003 = the game's bias)
         * are treated as an edge and dropped, constant SHADOW_RP_BIAS (default 0.0003). */
        static char bias_hdr[192] = { 0 };
        if (!bias_hdr[0]) {
            const char* er = getenv("SHADOW_RP"); const int rp = (er && *er == '0') ? 0 : 1;
            if (rp) {
                const char* e = getenv("SHADOW_RP_BIAS"); double b = e ? atof(e) : 0.0003;
                const char* em = getenv("SHADOW_RP_MAX"); double mx = em ? atof(em) : 0.003;
                snprintf(bias_hdr, sizeof bias_hdr, "#define RSX_SHRP 1\n#define RSX_SHBIAS %.7f\n#define RSX_SHRPMAX %.7f\n", b, mx);
                fprintf(stderr, "[SHADOW] v148 receiver-plane PCF ON (SHADOW_RP): bias %.7f (SHADOW_RP_BIAS), max correction %.7f (SHADOW_RP_MAX)%c", b, mx, 10);
            } else {
            const char* e = getenv("SHADOW_BIAS"); double b = e ? atof(e) : 0.0015;
            { const char* e2 = getenv("SHADOW_SLOPE"); double sl = e2 ? atof(e2) : 1.5;   /* v94: + receiver-slope bias (acne on surfaces at grazing angles to the sun) */
              snprintf(bias_hdr, sizeof bias_hdr, "#define RSX_SHRP 0\n#define RSX_SHBIAS %.7f\n#define RSX_SHSLOPE %.4f\n", b, sl);
              fprintf(stderr, "[SHADOW] v94 slope bias %.4f x depth gradient (SHADOW_SLOPE)%c", sl, 10); }
            fprintf(stderr, "[SHADOW] v92 compare bias %.7f (SHADOW_BIAS) -- v148 receiver-plane PCF off (SHADOW_RP=0)%c", b, 10); }
        }
        const size_t bl = strlen(bias_hdr);
        const size_t fl = sizeof(s_shadow_fn) - 1, used = strlen(hlsl);
        if (used + fl + bl + 1 > cap) return -1;
        memmove(hlsl + fl + bl, hlsl, used + 1);
        memcpy(hlsl, bias_hdr, bl);
        memcpy(hlsl + bl, s_shadow_fn, fl);
    }
    return patched;
}
/* v89 TEX_XF (default 1; 0 = off): RSX texture-unit value transforms from TEXTURE_ADDRESS
 * (RPCS3 fragment_texture): bits 12-15 unsigned_remap (1 = BIASED: texel * 2 - 1, the
 * "_bx2" expand inFamous' deferred lights rely on for G-buffer normals), bits 20-23 gamma
 * (sRGB -> linear on R,G,B). Neither was emulated: normals stayed in 0..1 and albedo was
 * treated as linear -> flat, washed-out, too-bright lighting. Per draw a 2-bit mode per
 * unit 0-7 (bit0 expand, bit1 sRGB) rides in the PSO key (exp32 bits 8-23). */
static const char s_txf_fn[] =
    "float4 rsx_txf(float4 c, int m) {\n"
    "  if (m & 2) c.rgb = (c.rgb <= 0.04045) ? c.rgb / 12.92 : pow(max((c.rgb + 0.055) / 1.055, 0.0), 2.4);\n"
    "  if (m & 1) c.rgb = c.rgb * 2.0 - 1.0;   /* v93: RGB only -- alpha carries material/gloss data in 0..1 */\n"
    "  return c;\n}\n";
static int tex_xf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TEX_XF"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[TEX_XF] v89 texture unsigned-remap (bx2) + gamma (sRGB) %s (TEX_XF)%c", v ? "ON" : "off", 10); }
    return v;
}
static int fp_txf_rewrite(char* hlsl, size_t cap, u32 texk)
{
    int patched = 0;
    static const char* meth[3] = { "Sample(", "SampleBias(", "SampleLevel(" };
    for (int u = 0; u < 8; u++) {
        int m = (int)((texk >> (2 * u)) & 3u);
        if (!m) continue;
        for (int form = 0; form < 2; form++)
        for (int mi = 0; mi < 3; mi++) {
            char nd[48];
            snprintf(nd, sizeof nd, form ? "rsx_tex%d.%s" : "rsx_tex[%d].%s", u, meth[mi]);
            char* from = hlsl;
            for (;;) {
                char* at = strstr(from, nd);
                if (!at) break;
                /* rsx_tex1 must not match rsx_tex10..15 (form 1 has no bracket) */
                char* q = at + strlen(nd) - 1;              /* the '(' */
                int dep = 0;
                for (; *q; q++) { if (*q == '(') dep++; else if (*q == ')') { if (--dep == 0) break; } }
                if (!*q) break;
                char* end = q + 1;
                char trail[16]; snprintf(trail, sizeof trail, ", %d)", m);
                const size_t hl = 8 /* "rsx_txf(" */, tl = strlen(trail), used = strlen(hlsl);
                if (used + hl + tl + 1 > cap) return -1;
                memmove(end + tl, end, strlen(end) + 1);   /* trailer first (after the call) */
                memcpy(end, trail, tl);
                memmove(at + hl, at, strlen(at) + 1);       /* then the head */
                memcpy(at, "rsx_txf(", hl);
                from = end + hl + tl;
                patched++;
            }
        }
    }
    if (patched) {
        const size_t fl = sizeof(s_txf_fn) - 1, used = strlen(hlsl);
        if (used + fl + 1 > cap) return -1;
        memmove(hlsl + fl, hlsl, used + 1);
        memcpy(hlsl, s_txf_fn, fl);
    }
    return patched;
}
/* per-draw unit modes (from the zf field's bits 8-9, set in d3d12_bind_texture) */
static u32 dr_texk(const D3D12DrawRecord* dr)
{
    if (!tex_xf_on()) return 0;
    u32 k = 0;
    for (int u = 0; u < 8; u++)
        if (dr->tex[u].set) k |= ((dr->tex[u].zf >> 8) & 3u) << (2 * u);
    return k;
}
/* v90: per-draw MRT key from SET_COLOR_MASK_MRT / SET_BLEND_ENABLE_MRT (0 = mirror RT0, the old behaviour). */
static u32 rsx_mrt_key(const rsx_state* st)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("MRT_MASKS"); on = (e && *e == '0') ? 0 : 1;
                  fprintf(stderr, "[MRT] v90 per-target colour masks / blend enables %s (MRT_MASKS)%c", on ? "ON" : "off", 10); }
    if (!on || !st) return 0;
    u32 k = 0;
    if (st->mrt_seen & 1) {
        for (int r = 1; r <= 3; r++) {
            u32 n = (st->color_mask_mrt >> (4 * r)) & 0xFu;   /* a=bit0 r=bit1 g=bit2 b=bit3 */
            u32 d = ((n >> 1) & 1u) | (((n >> 2) & 1u) << 1) | (((n >> 3) & 1u) << 2) | ((n & 1u) << 3);
            k |= d << (4 * (r - 1));
        }
        k |= 1u << 12;
    }
    if (st->mrt_seen & 2)
        for (int r = 1; r <= 3; r++) if (!((st->blend_enable_mrt >> r) & 1u)) k |= 1u << (12 + r);
    /* v91: RT0 blending OFF but an MRT blend bit ON (inFamous' G-buffer decals: albedo is
     * written straight, the normal target blends with the global factors so its ALPHA --
     * material data -- is kept). Record the factors anyway (bit 20) and switch RT0 off (bit 19). */
    if ((st->mrt_seen & 2) && !st->blend_enable && (st->blend_enable_mrt & 0xEu)) k |= (1u << 19) | (1u << 20);
    return k;
}
static ID3D12PipelineState* vp_get_fp_pso_impl(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 cube_mask)
{
    if (nrt < 1) nrt = 1; if (nrt > 4) nrt = 4;
    if (rtfmt == 0) rtfmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);
    if (!fp_addr || !vm_base) return NULL;
    { static int _off=-1; if(_off<0)_off=getenv("FP_OFF")?1:0; if(_off) return NULL; }
    /* Resolve the VS: the draw's cached per-VP blob, else the primary. */
    ID3DBlob* vsb = NULL; u32 vs_hash = 0;
    if (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) {
        vsb = s_d3d.vp_vs[vs_idx].vs; vs_hash = s_d3d.vp_vs[vs_idx].hash;
    }
    if (!vsb) { vsb = s_d3d.vp_vs_blob; vs_idx = -1; }
    if (!vsb) return NULL;

    /* SET_SHADER_PROGRAM low bits = location+1 (same as textures): 1 = LOCAL
     * (VRAM), 2 = MAIN (gcm/cube emits 0x00B90001 for its VRAM-resident FP). */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 off = cellGcmResolveLocated((fp_addr & 0x3u) == 1, fp_addr & ~0x3u);
    if (off == 0xFFFFFFFFu) return NULL;
    /* v33: compile what the draw saw (snapshot), not what the FP ring holds now */
    const u8* fpu = s_fp_uc_override ? s_fp_uc_override : vm_base + off;

    /* Content hash: inline constants are patched in place per frame (wave's
     * stamp), so identity is the BYTES, not the address. */
    /* Hash the CODE, not the constants: with fp_k[] hoisted into the per-draw
     * constant buffer the compiled shader is invariant under constant changes,
     * so hashing them made the pipeline key move every draw. */
    /* Pipeline identity (2026-10-04 evening). The decompiler used to be called
     * in LITERAL mode -- every inline constant baked into the HLSL -- while the
     * key hashed only the instruction words (rsx_fp_code_hash). Programs that
     * differ only in constants then shared one pipeline: stale colours while
     * keyed by address, and a black title screen once keyed by content.
     * Now: BUFFERED mode. Constants are read from b1 (fp_constants[], filled per
     * draw by vp_record_cb from the draw's own ucode), so the compiled shader is
     * a function of the code alone, and the key is the code + constant count
     * (fp_scan). Programs with > 64 constants, or FP_CONSTBUF=0, fall back to
     * literal mode keyed on ALL bytes including the constants. */
    u32 uhash;
    int fp_buffered;
    u32 fp_kcount = 0;
    {
        static int s_cb0 = -1;
        if (s_cb0 < 0) { const char* e = getenv("FP_CONSTBUF"); s_cb0 = (e && e[0] == '0') ? 1 : 0; }
        u32 code_h = 0, full_h = 0, used = 0;
        int ok;
        /* The FIFO already verified every instruction byte against its memo.
         * Reuse that exact scan result for immutable snapshots in buffered mode.
         * Literal/debug shaders still hash the complete program and constants. */
        if (s_fp_meta_override && s_fp_meta_override->fp_meta_valid &&
            s_fp_uc_override && !s_cb0 && !s_bis_literal &&
            s_fp_meta_override->fp_const_count <= FP_K_SLOTS) {
            fp_kcount = s_fp_meta_override->fp_const_count;
            code_h = s_fp_meta_override->fp_code_hash;
            ok = 1;
        } else ok = fp_scan(fpu, 4096, &fp_kcount, &code_h, &full_h, &used);
        s_perf_pso_hashbytes += (int)used;
        fp_buffered = ok && !s_cb0 && !s_bis_literal && fp_kcount <= FP_K_SLOTS;
        uhash = fp_buffered ? (code_h ^ 0xB0FFE7EDu) : full_h;
    }

    s_perf_pso_calls++;
    /* Hashed index over the full key (s_fpi: entry index + 1, 0 = empty). */
    static int s_kaddr_on = -1;
    if (s_kaddr_on < 0)
        s_kaddr_on = (getenv("FP_KEY_ADDR") || getenv("FP_ONE") || getenv("FP_KILL") ||
                      getenv("FP_SHOW") || getenv("FP_IDCOLOR") || getenv("FP_TEX") ||
                      getenv("FP_PICK") || getenv("FP_FORCE")) ? 1 : 0;
    u32 kaddr = (s_kaddr_on || s_bis_kaddr) ? fp_addr : 0;
    u32 kh = fp_key_hash(kaddr, vs_idx, vs_hash, s_d3d.vp_gen, blend, nrt, (u32)rtfmt,
                         exp32, uhash, cmask, cull, cube_mask);
    for (u32 pr = 0; pr < FPI_SIZE; pr++) {
        int e = s_fpi[(kh + pr) & (FPI_SIZE - 1)];
        if (!e) break;
        VPFPEntry* q = &s_d3d.vp_fp[e - 1];
        if (e - 1 < s_d3d.vp_fp_n && q->kaddr == kaddr && q->vs_idx == vs_idx &&
            q->vs_hash == vs_hash && q->gen == s_d3d.vp_gen && q->blend == blend &&
            q->nrt == nrt && q->rtfmt == (u32)rtfmt && q->exp32 == exp32 &&
            q->ucode_hash == uhash && q->cmask == cmask && q->cull == cull &&
            q->cube_mask == cube_mask)
            return q->pso;
    }
    if (0) for (int i = 0; i < s_d3d.vp_fp_n; i++)
        if (s_d3d.vp_fp[i].fp_addr != fp_addr) continue;   /* cheap reject first */
        else if (s_d3d.vp_fp[i].vs_idx == vs_idx &&
            s_d3d.vp_fp[i].vs_hash == vs_hash && s_d3d.vp_fp[i].gen == s_d3d.vp_gen &&
            s_d3d.vp_fp[i].blend == blend && s_d3d.vp_fp[i].nrt == nrt &&
            s_d3d.vp_fp[i].rtfmt == (u32)rtfmt && s_d3d.vp_fp[i].exp32 == exp32 &&
            s_d3d.vp_fp[i].ucode_hash == uhash && s_d3d.vp_fp[i].cmask == cmask &&
            s_d3d.vp_fp[i].cull == cull &&
            s_d3d.vp_fp[i].cube_mask == cube_mask)
            return s_d3d.vp_fp[i].pso;
    s_perf_pso_miss++;      /* falls through to a full decompile + D3DCompile */
    s_sf_pso_miss++;
    /* PSOMISSDBG=1: on a miss, name the key field that differs from an existing
     * entry for the same program. "the cache misses" is not actionable; "it
     * misses on ucode_hash" is. */
    /* HASHDBG=1: how many DISTINCT ucode hashes each program produces. Inline
     * constants are patched into the shader bytes, so a program whose constants
     * animate yields a new hash -- and a new pipeline -- every time. */
    { static int hd = -1;
      if (hd < 0) { const char* e = getenv("HASHDBG"); hd = e ? atoi(e) : 0; }
      if (hd) { static u32 fps[32]; static u32 cnt[32]; static int nf = 0;
        int idx = -1;
        for (int i = 0; i < nf; i++) if (fps[i] == fp_addr) idx = i;
        if (idx < 0 && nf < 32) { idx = nf++; fps[idx] = fp_addr; cnt[idx] = 0; }
        if (idx >= 0) { cnt[idx]++;
          if ((cnt[idx] % 200) == 0)
            fprintf(stderr, "[HASHDBG] fp=0x%X has produced %u distinct compiles%c",
                    fp_addr, cnt[idx], 10); } } }
    { static int md = -1;
      if (md < 0) { const char* e = getenv("PSOMISSDBG"); md = e ? atoi(e) : 0; }
      if (md) { static int n = 0;
        for (int i = 0; i < s_d3d.vp_fp_n && n < 12; i++) {
            VPFPEntry* q = &s_d3d.vp_fp[i];
            if (q->fp_addr != fp_addr) continue;
            n++;
            fprintf(stderr, "[PSOMISS] fp=0x%X differs:%s%s%s%s%s%s%s%s%c", fp_addr,
                    q->vs_idx != vs_idx        ? " vs_idx"     : "",
                    q->vs_hash != vs_hash      ? " vs_hash"    : "",
                    q->gen != s_d3d.vp_gen     ? " gen"        : "",
                    q->blend != blend          ? " blend"      : "",
                    q->nrt != nrt              ? " nrt"        : "",
                    q->rtfmt != (u32)rtfmt     ? " rtfmt"      : "",
                    q->ucode_hash != uhash     ? " ucode_hash" : "",
                    q->cube_mask != cube_mask  ? " cube_mask"  : "", 10);
            break;
        } } }
    /* CUBEKEYDBG=1: which (program, cube mask) pairs are being compiled. If one
     * program shows several masks, the mask is unstable and is the reason the
     * pipeline cache thrashes with cube support on. */
    { static int ck = -1;
      if (ck < 0) { const char* e = getenv("CUBEKEYDBG"); ck = e ? atoi(e) : 0; }
      if (ck) { static u32 seen[64][2]; static int ns = 0; int known = 0;
        for (int i = 0; i < ns; i++)
            if (seen[i][0] == fp_addr && seen[i][1] == cube_mask) known = 1;
        if (!known && ns < 64) { seen[ns][0] = fp_addr; seen[ns][1] = cube_mask; ns++;
            fprintf(stderr, "[CUBEKEY] fp=0x%X cube_mask=0x%X (distinct pairs=%d)%c",
                    fp_addr, cube_mask, ns, 10); } } }
    static char hlsl[32768];
    /* The third parameter is SET_SHADER_CONTROL; the decompiler only reads its
     * 32-bit-exports bit (0x40), which every draw record carries as exp32. Pass
     * it (2026-10-03). The old RSX_FP_CTRL_AUTO guess ("wrote r0 -> export r0")
     * picked the stale intermediate for shaders that use r0 as scratch and put
     * the final colour in h0 -- inFamous' Bink YUV->RGB program (fp 0x0F32F780)
     * lost its bias add and rendered magenta. FP_CTRL_AUTO=1 restores the guess. */
    static int s_fp_ctrl_auto = -1;
    if (s_fp_ctrl_auto < 0) s_fp_ctrl_auto = getenv("FP_CTRL_AUTO") ? 1 : 0;
    u32 fp_ctrl = s_fp_ctrl_auto ? RSX_FP_CTRL_AUTO : fp_ctrl_word(exp32 & 0xFF);   /* v89: bits 8+ = TEX_XF key */
    int n;
    if (fp_buffered) {
        u32 kc = 0;
        n = rsx_fp_decompile_buffered_ex(fpu, 4096, fp_ctrl, fp_cube_units(cube_mask), hlsl, sizeof(hlsl), &kc);
        /* Re-declare b1 with the backend's layout: rsx_texscale[4] + rsx_alphatest
         * (float4 0..4), then fp_constants[] at float4 5 = ts[20], where
         * vp_record_cb writes them. */
        if (n > 0) {
            char* cb = strstr(hlsl, "cbuffer PSConstants : register(b1) {");
            char* ce = cb ? strstr(cb, "};\n") : NULL;
            static const char decl[] =
                "cbuffer PSConstants : register(b1) {\n"
                "    float4 rsx_b1_pre[5];\n"
                "    float4 fp_constants[64];\n"
                "    float4 fp_alpha;\n"
                "    float4 rsx_wscale;\n"   /* v154: b1[70] */
                "};\n";
            if (cb && ce) {
                ce += 3;
                size_t oldlen = (size_t)(ce - cb), newlen = sizeof(decl) - 1;
                size_t tail = strlen(ce) + 1;
                if ((size_t)(cb - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(cb + newlen, ce, tail);
                    memcpy(cb, decl, newlen);
                    /* v154 RES_SCALE: WPOS in guest pixels ("input .position" so the scan does not re-match) */
                    if (s_res_scale > 1.0f) hlsl_replace_all(hlsl, sizeof hlsl, "input.position", "(input .position * rsx_wscale)");
                } else n = -1;
            } else n = -1;
            { static int _n = 0; if ((_n < 3 && _n++ < 3))
                fprintf(stderr, "[FPBUF] fp=0x%X buffered: %u constants via b1%c", fp_addr, kc, 10); }
        }
        if (n <= 0) {   /* could not patch: literal mode, keyed... still by code; say so */
            static int _w = 0; if ((_w < 8 && _w++ < 8))
                fprintf(stderr, "[FPBUF] fp=0x%X buffered decompile/patch failed -- literal fallback%c", fp_addr, 10);
            n = rsx_fp_decompile_ex(fpu, 4096, fp_ctrl, fp_cube_units(cube_mask), hlsl, sizeof(hlsl));
        }
    } else {
        n = rsx_fp_decompile_ex(fpu, 4096, fp_ctrl, fp_cube_units(cube_mask), hlsl, sizeof(hlsl));
    }
    if (n <= 0) { static int _e=0; if((_e < 16 && _e++ < 16)) printf("[FP] decompile fail (fp=0x%08X)\n", fp_addr); return NULL; }
    /* DEPTH BLUR (fp code 29F3C342, see DEPTH_BLUR at the draw filter): the game's distance blur, which also hides
     * LOD pop-in. It mixes two 320x180 blurred copies (t2, t3) into the scene with blend = saturate(slope * u + off)
     * x strength, u ~ view distance from depth; k2 = k3 = (offA, slopeA, offB, slopeB), k4.x / k5.x = strengths.
     * Powered areas use A 1500..8000, B 8000..15000, strength 0.5; blackout areas A 400..900, B 900..11000, 0.65
     * (rsxcap 2026-10-30), i.e. the blur starts ~4x closer -- the "blackout blur".
     *   DEPTH_BLUR=2: never start or finish a ramp closer, or blend stronger, than the powered values (blackout
     *                 areas then fade like powered ones; settings already further / weaker are kept).
     *   DEPTH_BLUR_SMOOTH (default 1): sample t2 / t3 with a cubic B-spline (4 bilinear taps) instead of one
     *                 bilinear tap -- at RES_SCALE 2 the 320x180 copies are magnified 8x and bilinear shows blocks. */
    if (fp_buffered && (uhash ^ 0xB0FFE7EDu) == 0x29F3C342u) {
        static int mode = -1, smooth = -1;
        if (mode < 0) { const char* e = getenv("DEPTH_BLUR"); mode = (e && *e) ? atoi(e) : 1;
                        e = getenv("DEPTH_BLUR_SMOOTH"); smooth = (e && *e == '0') ? 0 : 1; }
        static const char helpers[] =
            "float4 rsx_dof_k(float4 k) {\n"
            "    if (k.y <= 0.0 || k.w <= 0.0) return k;\n"
            "    float sA = max(-k.x / k.y, 1500.0), eA = max((1.0 - k.x) / k.y, 8000.0);\n"
            "    float sB = max(-k.z / k.w, 8000.0), eB = max((1.0 - k.z) / k.w, 15000.0);\n"
            "    float cA = 1.0 / max(eA - sA, 1.0), cB = 1.0 / max(eB - sB, 1.0);\n"
            "    return float4(-sA * cA, cA, -sB * cB, cB);\n"
            "}\n"
            "float4 rsx_dof_s(float4 k) { k.x = min(k.x, 0.5); return k; }\n"
            "float4 rsx_dof_up(Texture2D t, SamplerState s, float2 uv) {\n"
            "    float w, h; t.GetDimensions(w, h); float2 ts = float2(w, h);\n"
            "    float2 p = uv * ts - 0.5, i = floor(p), f = p - i, f2 = f * f, f3 = f2 * f;\n"
            "    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;\n"
            "    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;\n"
            "    float2 g0 = w0 + w1, g1 = w2 + w3;\n"
            "    float2 c0 = (i - 0.5 + w1 / g0) / ts, c1 = (i + 1.5 + w3 / g1) / ts;\n"
            "    return g0.y * (g0.x * t.Sample(s, float2(c0.x, c0.y)) + g1.x * t.Sample(s, float2(c1.x, c0.y)))\n"
            "         + g1.y * (g0.x * t.Sample(s, float2(c0.x, c1.y)) + g1.x * t.Sample(s, float2(c1.x, c1.y)));\n"
            "}\n";
        char* mn = strstr(hlsl, "main(PSInput");
        while (mn && mn > hlsl && mn[-1] != '\n') mn--;
        if (mn && (mode == 2 || smooth) && strlen(hlsl) + sizeof helpers < sizeof hlsl) {
            const size_t at = (size_t)(mn - hlsl), hl = sizeof helpers - 1;
            memmove(hlsl + at + hl, hlsl + at, strlen(hlsl + at) + 1);
            memcpy(hlsl + at, helpers, hl);
            if (mode == 2) {
                hlsl_replace_all(hlsl, sizeof hlsl, "(fp_constants[2])", "(rsx_dof_k(fp_constants[2]))");
                hlsl_replace_all(hlsl, sizeof hlsl, "(fp_constants[3])", "(rsx_dof_k(fp_constants[3]))");
                hlsl_replace_all(hlsl, sizeof hlsl, "(fp_constants[4])", "(rsx_dof_s(fp_constants[4]))");
                hlsl_replace_all(hlsl, sizeof hlsl, "(fp_constants[5])", "(rsx_dof_s(fp_constants[5]))");
            }
            if (smooth) {
                hlsl_replace_all(hlsl, sizeof hlsl, "rsx_tex[2].Sample(rsx_samp[2], ", "rsx_dof_up(rsx_tex[2], rsx_samp[2], ");
                hlsl_replace_all(hlsl, sizeof hlsl, "rsx_tex[3].Sample(rsx_samp[3], ", "rsx_dof_up(rsx_tex[3], rsx_samp[3], ");
            }
            static int _n = 0; if (_n < 4 && _n++ < 4)
                fprintf(stderr, "[DEPTHBLUR] fp=0x%X: %s%s%c", fp_addr, mode == 2 ? "limited to the powered-area range" : "game range",
                        smooth ? ", B-spline upsample" : "", 10);
        }
    }
    /* FP_LIST=1: every program the title actually compiles, with its size. A
     * fragment program that hangs the GPU shows up here as an implausible
     * instruction count long before it shows up as a TDR. */
    { static int fl = -1; if (fl < 0) fl = getenv("FP_LIST") ? 1 : 0;
      if (fl) fprintf(stderr, "[FPLIST] fp=0x%08X instrs=%d hlsl=%u bytes%c",
                      fp_addr, n, (unsigned)strlen(hlsl), 10); }
    if (getenv("FP_DUMP")) { static int _d=0; if ((_d < 4 && _d++ < 4)) {
        FILE* f = fopen("fp_dump.hlsl", _d==1 ? "w" : "a");
        if (f) {
            const u8* uc = fpu;
            fprintf(f, "/* fp_addr=0x%08X vmoff=0x%08X raw:", fp_addr, off);
            for (int _b = 0; _b < 32; _b++) fprintf(f, " %02X", uc[_b]);
            fprintf(f, " */\n%s\n", hlsl); fclose(f);
        } } }

    /* FP_PICK=<hex fp_addr>: write just that program, whatever order it compiles
     * in. fp_dump.hlsl keeps only the first four, same trap as vp2_dump.hlsl. */
    { static const char* fpk = (const char*)1; static u32 wantf = 0; static int done = 0;
      if (fpk == (const char*)1) { fpk = getenv("FP_PICK");
                                   wantf = fpk ? (u32)strtoul(fpk, NULL, 16) : 0; }
      if (wantf && !done && fp_addr == wantf) { done = 1;
          FILE* f = fopen("duck_fp.hlsl", "w");
          if (f) { fprintf(f, "/* fp_addr=0x%08X, %d instrs */%s", fp_addr, n, hlsl);
                   fclose(f); }
          fprintf(stderr, "[DUCKFP] wrote duck_fp.hlsl (fp=0x%08X, %d instrs)%c",
                  fp_addr, n, 10);
      } }

    /* Debug FP_ONE=<hex fp_addr>: force that program's colour output to
     * all-ones (e.g. wave's colour-detect -> full mask -> island borders
     * everywhere -> the water sim must visibly radiate if it works). */
    { const char* f1 = getenv("FP_ONE");
      if (f1 && (u32)strtoul(f1, NULL, 16) == fp_addr) {
          /* Match the assignment, not one exact spelling of it: the final
           * register is r[N] for some programs and h[N] for others, and the old
           * fixed "= r[0];" anchor silently did nothing for every h[] shader --
           * so this switch appeared to have no effect and was useless for
           * identifying which program draws what. Rewrite the expression with a
           * proper length-changing splice. */
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) {
              char* semi = strchr(rp, ';');
              if (semi) {
                  static const char rep[] = "_po.c0 = (1.0).xxxx";
                  size_t oldlen = (size_t)(semi - rp), newlen = sizeof(rep) - 1;
                  size_t tail = strlen(semi) + 1;
                  if ((rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                      memmove(rp + newlen, semi, tail);
                      memcpy(rp, rep, newlen);
                  }
                  (void)oldlen;
              }
          }
      } }
    /* FP_TEX=1: make every program output its unit-0 texture sample verbatim.
     * Separates "the texture is wrong" from "the shading tints it" in one run,
     * without having to identify which program draws which surface first. */
    /* FP_SHOW=<hlsl expr>: replace a program's colour output with an arbitrary
     * expression (e.g. FP_SHOW=h[1]) so an intermediate register can be looked
     * at directly. Restrict with FP_TEX_FP=<hex>. Reading a 69-instruction
     * shader to guess which term carries a bad value is slower and less certain
     * than displaying the terms. */
    /* FP_KILL=<hex fp_addr>: make that program discard every fragment, so what
     * it covers becomes visible. Distinguishes "this surface shades black" from
     * "this surface is correct and something else is missing behind it". */
    { const char* kf = getenv("FP_KILL");
      if (kf && (u32)strtoul(kf, NULL, 16) == fp_addr) {
          char* rp = strstr(hlsl, "PSOut _po;");
          if (rp) {
              const char* ins = "discard; ";
              size_t nl = strlen(ins), tail = strlen(rp) + 1;
              if ((size_t)(rp - hlsl) + nl + tail < sizeof(hlsl)) {
                  memmove(rp + nl, rp, tail);
                  memcpy(rp, ins, nl);
                  fprintf(stderr, "[FPKILL] fp=0x%X discards%c", fp_addr, 10);
              }
          }
      } }
    { const char* sh = getenv("FP_SHOW");
      const char* only = getenv("FP_TEX_FP");
      if (sh && (!only || (u32)strtoul(only, NULL, 16) == fp_addr)) {
          char* rp = strstr(hlsl, "_po.c0 = ");
          if (rp) { char* semi = strchr(rp, ';');
            if (semi) {
                char rep[256];
                snprintf(rep, sizeof rep, "_po.c0 = float4((%s).xyz, 1.0)", sh);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPSHOW] fp=0x%X output replaced with %s%c",
                            fp_addr, sh, 10);
                }
            } }
      } }
    /* Retarget the samplers the guest bound as CUBE textures. The decompiler
     * always emits 2D samplers; a cube is sampled with a 3-component direction
     * and no texel-scale, and the emitted coordinate always ends in the fixed
     * tail ".xy * rsx_texscale[N].xy", so both edits are exact replacements. */
    if (cube_mask & 0xFFFFu) {   /* v34: bits 16+ are the shadow key */
        for (int _u = 0; _u < 4; _u++) {
            if (!(cube_mask & (1u << _u))) continue;
            char f1[64], r1[64], f2[64], r2[64];
            snprintf(f1, sizeof f1, "Texture2D    rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube  rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f1, sizeof f1, "Texture2D rsx_tex%d", _u);
            snprintf(r1, sizeof r1, "TextureCube rsx_tex%d", _u);
            hlsl_replace_all(hlsl, sizeof hlsl, f1, r1);
            snprintf(f2, sizeof f2, ".xy * rsx_texscale[%d].xy", _u);
            snprintf(r2, sizeof r2, ".xyz");
            hlsl_replace_all(hlsl, sizeof hlsl, f2, r2);
        }
        { static int _n = 0; if ((_n < 4 && _n++ < 4))
            fprintf(stderr, "[CUBE] fp=0x%X compiled with cube units mask 0x%X%c",
                    fp_addr, cube_mask, 10); }
    }
    if ((cull >> 25) & 1u) {   /* v36 ALPHA TEST */
        int ar = fp_alpha_rewrite(hlsl, sizeof hlsl, (cull >> 26) & 7u);
        { static int _n = 0; if ((_n < 6 && _n++ < 6) || ar < 0)
            fprintf(stderr, "[ATEST] fp=0x%X alpha test func %u -> %s%c", fp_addr, (cull >> 26) & 7u,
                    ar > 0 ? "applied" : ar == 0 ? "not needed / literal program" : "PATCH FAILED", 10); }
    }
    if ((blend & 1u) && (blend & (3u << 27)) && nrt == 1 && !strstr(hlsl, "struct PSOutput")) {   /* v166 */
        int sr = fp_signed_rewrite(hlsl, sizeof hlsl, blend);
        { static int _n = 0; if ((_n < 6 && _n++ < 6) || sr < 0)
            fprintf(stderr, "[BLENDSGN] fp=0x%X pass %c: %s%c", fp_addr, ((blend >> 30) & 1u) ? 'N' : 'P',
                    sr > 0 ? "output rewritten" : "PATCH FAILED", 10); }
    }
    if (cube_mask >> 16) {   /* v34 SHADOWS */
        /* v170 SUN_NDL_CLAMP (default 1; 0 = off): inFamous' sun-cascade light FPs (fph 89916613 / 86958E8F /
         * 58343B08 / ... one per time of day) compute the sun term as shadow * dot(N,-L) with N.L NOT clamped,
         * plus a separate cool back light saturate(-N.L). The casters are drawn front-face culled (cull 0x8F07)
         * so a surface facing AWAY from the sun compares against its own depth; the design needs that compare to
         * say "shadowed" (sun term 0). Our compare biases toward lit (v92/v148, against acne on double-sided
         * casters), so back-facing walls got a NEGATIVE sun term: the warm light was subtracted -> navy walls with
         * the real shadows (benches/lamps/railings) showing as lighter cut-outs. max(N.L,0) gives exactly the
         * shadowed result there and changes nothing on sun-facing surfaces. */
        { static int sc = -1; if (sc < 0) { const char* e = getenv("SUN_NDL_CLAMP"); sc = (e && *e == '0') ? 0 : 1; }
          static const char from[] = "(((r[2]).xyzw) * ((h[0]).xyzw)); h[4].w = _v.w;";
          static const char to[]   = "(((r[2]).xyzw) * max((h[0]).xyzw, 0.0)); h[4].w = _v.w;";
          if (sc && strstr(hlsl, from) && strstr(hlsl, "dot(((h[4]).xyzw).xyz, (-(")) {
              hlsl_replace_all(hlsl, sizeof hlsl, from, to);
              static int _n = 0; if ((_n < 8 && _n++ < 8))
                  fprintf(stderr, "[SUNNDL] v170 fp=0x%X: sun term clamped to shadow * max(N.L,0)%c", fp_addr, 10);
          } }
        int np = fp_shadow_rewrite(hlsl, sizeof hlsl, cube_mask >> 16);
        { static int _n = 0; if ((_n < 6 && _n++ < 6))
            fprintf(stderr, "[SHADOW] fp=0x%X: %d shadow lookup(s) compiled as depth compares (key 0x%X)%c",
                    fp_addr, np, cube_mask >> 16, 10); }
    }
    if (((u32)exp32 >> 8) & 0xFFFFu) {   /* v89 TEX_XF */
        int nx = fp_txf_rewrite(hlsl, sizeof hlsl, ((u32)exp32 >> 8) & 0xFFFFu);
        { static int _n = 0; if ((_n < 12 && _n++ < 12) || nx < 0)
            fprintf(stderr, "[TEX_XF] fp=0x%X: %d sample(s) wrapped (unit modes 0x%04X: 2 bits/unit, 1 = bx2, 2 = sRGB)%c",
                    fp_addr, nx, ((u32)exp32 >> 8) & 0xFFFFu, 10); }
    }
    /* v38 FP_NANDBG (default 1): where colour target 1 (the G-buffer NORMAL) would come out
     * NaN -- the output guard turns that into black, i.e. inFamous' black Cole/civilians --
     * write a code colour naming the guilty INPUT instead: red = tc1 (VS normal) non-finite or
     * zero, green = tc2 (tangent) non-finite or zero, blue = unit-0 sample non-finite; WHITE =
     * all inputs fine, the NaN is made inside the program. Pixels that are not NaN are
     * untouched, so the scene looks the same except the broken pixels. FP_NANDBG=0 = off. */
    { static int nd = -1; if (nd < 0) { const char* e = getenv("FP_NANDBG"); nd = e ? atoi(e) : 0; }   /* v46: diagnostic, default off now */
      static const char nan_from[] = "_out.t1 = (_o1 == _o1) ? _o1 : (float4)0;";
      if (nd && strstr(hlsl, nan_from)) {
          static const char nan_to[] =
              "{ uint4 _nb = asuint(_o1) & 0x7f800000u; bool _bad = any(_nb.xyz == 0x7f800000u);"
              " uint4 _ia = asuint(input.tc1) & 0x7f800000u; uint4 _ib = asuint(input.tc2) & 0x7f800000u;"
              " float4 _s0 = rsx_tex[0].Sample(rsx_samp[0], input.tc0.xy); uint4 _ic = asuint(_s0) & 0x7f800000u;"
              " float _dr = (any(_ia.xyz == 0x7f800000u) || dot(input.tc1.xyz, input.tc1.xyz) == 0.0) ? 1.0 : 0.0;"
              " float _dg = (any(_ib.xyz == 0x7f800000u) || dot(input.tc2.xyz, input.tc2.xyz) == 0.0) ? 1.0 : 0.0;"
              " float _db = any(_ic == 0x7f800000u) ? 1.0 : 0.0;"
              " float3 _dc = (_dr + _dg + _db > 0.0) ? float3(_dr, _dg, _db) : float3(1.0, 1.0, 1.0);"
              " uint _col = ((uint)input.position.x) & 3u;"
              " float4 _mx = (_col == 0u) ? _s0 : (_col == 1u) ? float4(input.tc1.xyz * 0.5 + 0.5, saturate(length(input.tc1.xyz) * 0.5))"
              " : (_col == 2u) ? float4(input.tc2.xyz * 0.5 + 0.5, input.tc2.w * 0.5 + 0.5) : float4(input.tc3.xyz * 0.5 + 0.5, 1.0);"
              " _out.t1 = _bad ? (NANDBG_MODE == 2 ? _mx : float4(_dc, 1.0)) : _o1; }";
          if (strstr(hlsl, "rsx_tex[0]") || strstr(hlsl, "Texture2D    rsx_tex[16]")) {
              hlsl_replace_all(hlsl, sizeof hlsl, nan_from, nan_to);
              /* v39: FP_NANDBG=2 (default) = the raw inputs, multiplexed by pixel column
               * (x%4: 0 unit-0 texel rgba, 1 tc1*0.5+0.5 / a=len/2, 2 tc2*0.5+0.5 / a=w*0.5+0.5,
               * 3 tc3*0.5+0.5); FP_NANDBG=1 = the v38 code colours. */
              hlsl_replace_all(hlsl, sizeof hlsl, "NANDBG_MODE", nd == 1 ? "1" : "2");
              static int _n = 0; if ((_n < 4 && _n++ < 4))
                  fprintf(stderr, "[NANDBG] fp=0x%X: normal-target NaN diagnostic compiled in%c", fp_addr, 10);
          }
      } }
    /* FP_IDCOLOR=1: give every fragment program a distinct flat colour derived
     * from its address. One frame then shows which program paints which surface,
     * instead of one run per candidate to test them by elimination. */
    if (getenv("FP_IDCOLOR")) {
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                /* Mix properly: every fp address in a title tends to share low
                 * bits (they all end 0x01/0x81 here), so a single multiply left
                 * one channel constant across all programs and made distinct
                 * shaders look like the same colour. */
                u32 hsh = fp_addr * 2654435761u;
                hsh ^= hsh >> 13; hsh *= 0x5BD1E995u; hsh ^= hsh >> 15;
                /* Force the top bit on in each channel: a hash that happens to
                 * land near black is indistinguishable from "this surface was
                 * never painted", which is the exact question the mode exists
                 * to answer. */
                float cr = (float)(((hsh >> 16) & 0x7F) | 0x80) / 255.0f;
                float cg = (float)(((hsh >>  8) & 0x7F) | 0x80) / 255.0f;
                float cb = (float)(((hsh      ) & 0x7F) | 0x80) / 255.0f;
                char rep[128];
                snprintf(rep, sizeof rep, "_po.c0 = float4(%.3f,%.3f,%.3f,1.0)", cr, cg, cb);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                    fprintf(stderr, "[FPID] fp=0x%X -> rgb(%.0f,%.0f,%.0f)%c",
                            fp_addr, cr*255, cg*255, cb*255, 10);
                }
            }
        }
    }
    /* FP_TEX=<unit>: output that unit's sample verbatim. FP_TEX_FP=<hex> limits
     * it to one program, so one surface can be inspected without repainting the
     * whole scene. FP_TEX_UV=1 samples with the same texcoord the program uses
     * for that unit rather than tc0. */
    if (getenv("FP_TEX") && (!getenv("FP_TEX_FP") ||
        (u32)strtoul(getenv("FP_TEX_FP"), NULL, 16) == fp_addr)) {
        int _un = atoi(getenv("FP_TEX"));
        if (_un < 0 || _un > 3) _un = 0;
        char* rp = strstr(hlsl, "_po.c0 = ");
        if (rp) {
            char* semi = strchr(rp, ';');
            if (semi) {
                char rep[160];
                /* FP_TEX_UV=1: sample at a fixed centre UV instead of the
                 * interpolated texcoord. If the texture is populated but the
                 * texcoords never arrive, the normal path samples texel (0,0)
                 * -- usually black -- and looks exactly like an empty texture.
                 * This tells the two apart. */
                if (getenv("FP_TEX_UV"))
                    snprintf(rep, sizeof rep,
                             "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, float2(0.5, 0.5))",
                             _un, _un);
                else
                snprintf(rep, sizeof rep,
                         "_po.c0 = rsx_tex%d.Sample(rsx_samp%d, input.tc0.xy * rsx_texscale[%d].xy)",
                         _un, _un, _un);
                size_t newlen = strlen(rep), tail = strlen(semi) + 1;
                fprintf(stderr, "[FP_TEX] program 0x%X rewritten to sample unit %d%c",
                        fp_addr, _un, 10);
                if ((size_t)(rp - hlsl) + newlen + tail < sizeof(hlsl)) {
                    memmove(rp + newlen, semi, tail);
                    memcpy(rp, rep, newlen);
                }
            }
        }
    }
    /* FP_FORCE=1: replace the translated body with solid magenta -- isolates
     * geometry/transform problems from texture/blend problems. */
    if (getenv("FP_FORCE")) {
        char* rp = strstr(hlsl, "return r[0];");
        if (rp) memcpy(rp, "return float4(1,0,1,1)", 23);
    }
    ID3DBlob* pb = NULL; ID3DBlob* e = NULL;
    HRESULT hr = cached_compile(hlsl, strlen(hlsl), "guest_fp", NULL, NULL,
                            "main", "ps_5_0", 0, 0, &pb, &e);
    if (FAILED(hr) || !pb) {
        static int _e2=0; if ((_e2 < 16 && _e2++ < 16))
            printf("[FP] PS compile FAIL (fp=0x%08X): %s\n", fp_addr,
                   e ? (const char*)e->lpVtbl->GetBufferPointer(e) : "?");
        if (e) e->lpVtbl->Release(e);
        return NULL;
    }
    if (e) e->lpVtbl->Release(e);

    D3D12_INPUT_ELEMENT_DESC il[16];
    for (int _i = 0; _i < 16; _i++) {
        il[_i].SemanticName = "ATTR"; il[_i].SemanticIndex = _i;
        il[_i].Format = DXGI_FORMAT_R32G32B32A32_FLOAT; il[_i].InputSlot = 0;
        il[_i].AlignedByteOffset = (UINT)(_i * 16);
        il[_i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        il[_i].InstanceDataStepRate = 0;
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vsb->lpVtbl->GetBufferPointer(vsb);
    pd.VS.BytecodeLength  = vsb->lpVtbl->GetBufferSize(vsb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb);
    pd.PS.BytecodeLength  = pb->lpVtbl->GetBufferSize(pb);
    pd.InputLayout.pInputElementDescs = il; pd.InputLayout.NumElements = 16;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    /* Guest face culling. CULL_OFF=1 forces double-sided, the old
     * unconditional behaviour -- geometry that vanishes under culling is a
     * winding/front-face problem, not a missing draw. */
    { static int _co = -1; if (_co < 0) _co = getenv("CULL_OFF") ? 1 : 0;
      pd.RasterizerState.CullMode = (!_co && (cull & 1u))
          ? ((cull & 2u) ? D3D12_CULL_MODE_FRONT : D3D12_CULL_MODE_BACK)
          : D3D12_CULL_MODE_NONE;
      /* CULL_FLIP=1: invert the front-face convention. Our VP path may hand
       * D3D the opposite winding from the guest's, in which case honouring
       * CULL_FACE culls exactly the faces it should keep. */
      static int _cfl = -1; if (_cfl < 0) _cfl = getenv("CULL_FLIP") ? 1 : 0;
      pd.RasterizerState.FrontCounterClockwise =
          (((cull & 4u) ? 1 : 0) ^ _cfl) ? TRUE : FALSE; }
    { const u32 czl = (cull >> 30) & 3u;   /* v244 coarse-world depth bias (lod_sky_apply / coarse_bias_level) */
      if (czl) { pd.RasterizerState.DepthBias = s_cz_units[czl]; pd.RasterizerState.SlopeScaledDepthBias = s_cz_slope[czl];
                 pd.RasterizerState.DepthBiasClamp = 0.0f;
                 static int _n = 0; if (_n < 4) { _n++; fprintf(stderr, "[COARSEZ] PSO with depth bias %d slope %.1f (level %u)%c",
                                                         s_cz_units[czl], s_cz_slope[czl], czl, 10); } } }
    /* Blend per the guest's packed key (see rsx_blend_key): dbgfont text needs
     * straight alpha; demosaic's effect passes blend OFF; DeferredShading's
     * light accumulation is additive ONE,ONE. */
    pd.BlendState.RenderTarget[0].BlendEnable    = (blend & 1) ? TRUE : FALSE;
    pd.BlendState.RenderTarget[0].SrcBlend       = (blend & 1) ? (D3D12_BLEND)((blend >> 1)  & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlend      = (blend & 1) ? (D3D12_BLEND)((blend >> 6)  & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOp        = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 21) & 0x7) : D3D12_BLEND_OP_ADD;
    pd.BlendState.RenderTarget[0].SrcBlendAlpha  = (blend & 1) ? (D3D12_BLEND)((blend >> 11) & 0x1F) : D3D12_BLEND_ONE;
    pd.BlendState.RenderTarget[0].DestBlendAlpha = (blend & 1) ? (D3D12_BLEND)((blend >> 16) & 0x1F) : D3D12_BLEND_ZERO;
    pd.BlendState.RenderTarget[0].BlendOpAlpha   = (blend & 1) ? (D3D12_BLEND_OP)((blend >> 24) & 0x7) : D3D12_BLEND_OP_ADD;
    if ((blend & 1u) && (blend & (3u << 27)) && nrt == 1) {   /* v166 signed blend passes */
        D3D12_RENDER_TARGET_BLEND_DESC* sb = &pd.BlendState.RenderTarget[0];
        const int pn = (blend >> 30) & 1, rev = (blend >> 29) & 1;
        if ((blend >> 27) & 1) { sb->BlendOp = (pn ^ rev) ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
                                 if (pn) sb->DestBlend = D3D12_BLEND_ONE; }
        else if (pn) { sb->SrcBlend = D3D12_BLEND_ZERO; sb->DestBlend = D3D12_BLEND_ONE; sb->BlendOp = D3D12_BLEND_OP_ADD; }
        if ((blend >> 28) & 1) { sb->BlendOpAlpha = pn ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
                                 if (pn) sb->DestBlendAlpha = D3D12_BLEND_ONE; }
        else if (pn) { sb->SrcBlendAlpha = D3D12_BLEND_ZERO; sb->DestBlendAlpha = D3D12_BLEND_ONE; sb->BlendOpAlpha = D3D12_BLEND_OP_ADD; }
    }
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = (UINT)nrt;
    for (int _r = 0; _r < nrt; _r++) {
        pd.RTVFormats[_r] = rtfmt;
        /* Zero-init leaves RenderTargetWriteMask 0 on secondary targets --
         * every MRT-B write would be masked off. Mirror RT0's blend state. */
        pd.BlendState.RenderTarget[_r] = pd.BlendState.RenderTarget[0];
    }
    /* Guest colour write mask (RGBA nibble, already D3D-ordered). cmask is 0xF
     * by default (unset colour_mask register decodes to all-on); a literal 0
     * therefore means the guest EXPLICITLY masked every channel -- a depth-only
     * pass (e.g. DeferredShading's shadow-map generation). Honour it: forcing 0
     * back to 0xF splatters the depth pass's fragment colour onto the target. */
    /* CMASK_FORCE=1: ignore the guest colour mask (write RGBA on every draw).
     * A mis-decoded mask makes geometry rasterize correctly and write nothing,
     * which is indistinguishable from "the draw never happened". */
    { static int _cf = -1; if (_cf < 0) _cf = getenv("CMASK_FORCE") ? 1 : 0;
      if (_cf) cmask = 0xF; }
    for (int _r = 0; _r < nrt; _r++)
        {   /* v90: MRT 1..3 masks / blend enables ride in cmask bits 4-19 (rsx_mrt_key << 4) */
            u32 _m = cmask & 0xF;
            if (_r > 0 && ((cmask >> 16) & 1u)) _m = (cmask >> (4 + 4 * (_r - 1))) & 0xFu;
            pd.BlendState.RenderTarget[_r].RenderTargetWriteMask = (UINT8)_m;
            if (_r > 0 && ((cmask >> (16 + _r)) & 1u)) pd.BlendState.RenderTarget[_r].BlendEnable = FALSE;
            if (_r == 0 && ((cmask >> 23) & 1u)) pd.BlendState.RenderTarget[0].BlendEnable = FALSE;   /* v91 */
        }
    /* v91: without IndependentBlendEnable D3D12 applies RenderTarget[0]'s blend state AND
     * write mask to every target -- v90's per-MRT masks were silently ignored. */
    if (nrt > 1) pd.BlendState.IndependentBlendEnable = TRUE;
    pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    pd.DepthStencilState.StencilEnable = FALSE;
    if (cull & 0x8000u) {   /* guest depth state (rsx_cull_key) */
        static const D3D12_COMPARISON_FUNC fmap[8] = {
            D3D12_COMPARISON_FUNC_NEVER, D3D12_COMPARISON_FUNC_LESS, D3D12_COMPARISON_FUNC_EQUAL,
            D3D12_COMPARISON_FUNC_LESS_EQUAL, D3D12_COMPARISON_FUNC_GREATER,
            D3D12_COMPARISON_FUNC_NOT_EQUAL, D3D12_COMPARISON_FUNC_GREATER_EQUAL,
            D3D12_COMPARISON_FUNC_ALWAYS };
        int test = (cull & 0x100u) != 0, write = (cull & 0x200u) != 0;
        pd.DepthStencilState.DepthEnable    = (test || write) ? TRUE : FALSE;
        /* RSX writes depth only when the test is enabled; test disabled + write
         * = "always pass, no write" on NV40 -- treat as no depth at all. */
        if (!test) { pd.DepthStencilState.DepthEnable = FALSE; write = 0; }
        pd.DepthStencilState.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc      = fmap[(cull >> 10) & 7u];
        u32 si = (cull >> 16) & 0xFFu;   /* v33 guest stencil */
        if (si && si < (u32)s_stk_n) {
            const StencilKey* sk = &s_stk[si];
            pd.DepthStencilState.StencilEnable = TRUE;
            pd.DepthStencilState.StencilReadMask = sk->rmask;
            pd.DepthStencilState.StencilWriteMask = sk->wmask;
            D3D12_DEPTH_STENCILOP_DESC* fd[2] = { &pd.DepthStencilState.FrontFace, &pd.DepthStencilState.BackFace };
            for (int _f = 0; _f < 2; _f++) {
                fd[_f]->StencilFunc = (D3D12_COMPARISON_FUNC)sk->f[_f];
                fd[_f]->StencilFailOp = (D3D12_STENCIL_OP)sk->fail[_f];
                fd[_f]->StencilDepthFailOp = (D3D12_STENCIL_OP)sk->zfail[_f];
                fd[_f]->StencilPassOp = (D3D12_STENCIL_OP)sk->zpass[_f];
            }
            /* NV40: with the depth test off, the stencil "zpass" op applies to every
             * fragment that passes stencil -- D3D does the same with DepthEnable FALSE. */
        }
    }
    /* DEPTH_OFF=1: drop the depth test for guest-FP draws, so submission order
     * alone decides what is on top. Paired with DRAW_LAST_TEX this puts one
     * object in front of everything and answers "is it merely occluded?" --
     * which reordering alone cannot, because a relocated draw still fails the
     * depth test against whatever is already in the buffer. Diagnostic. */
    { static int doff = -1;
      if (doff < 0) { const char* e = getenv("DEPTH_OFF"); doff = e ? atoi(e) : 0; }
      if (doff) { pd.DepthStencilState.DepthEnable = FALSE;
                  pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; } }
    pd.SampleDesc.Count = 1;

    ID3D12PipelineState* pso = NULL;
    hr = create_gfx_pso(&pd, (int)((cull >> 24) & 1u), &pso);   /* v35: + depth bounds test */
    /* 2026-10-04 (inFamous in-world): some PSOs fail with E_INVALIDARG. Log the
     * key, then retry (a) without blending -- blending on a non-blendable RT
     * format (float32/integer) is the usual cause -- and (b) on an RGBA8 target.
     * Failures are NEGATIVE-CACHED below: before this, a failing program was
     * re-decompiled + D3DCompile'd + re-created on EVERY draw. PSO_NOFALLBACK=1
     * disables the retries. */
    if (FAILED(hr)) {
        static int _e3=0; if ((_e3 < 32 && _e3++ < 32))
            printf("[FP] PSO FAIL (fp=0x%08X, 0x%08lX) rtfmt=%d nrt=%d blend=0x%08X cmask=0x%X cull=%u cube=0x%X vs=%d\n",
                   fp_addr, hr, (int)rtfmt, nrt, blend, cmask, cull, cube_mask, vs_idx);
        if (hr == (HRESULT)0x887A0005L) {   /* DXGI_ERROR_DEVICE_REMOVED */
            static int _dr = 0;
            if (!_dr++) { fprintf(stderr, "[D3D12] DEVICE REMOVED -- reason 0x%08lX (887A0006 hung, 887A0020 driver "
                               "internal, 80070057 invalid call); every later PSO/draw fails\n",
                               (unsigned long)s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device));
                          fflush(stderr);
                          d3d12_dred_report(); }
        }
        /* Write the failing pair (first 6) to pso_fail.hlsl: E_INVALIDARG that
         * survives the blend/format fallbacks is usually a VS-output / PS-input
         * signature mismatch, and only the two sources side by side show it. */
        { static int _pd = 0;
          if ((_pd < 6 && _pd++ < 6)) {
              FILE* f = fopen("pso_fail.hlsl", _pd == 1 ? "w" : "a");
              if (f) {
                  fprintf(f, "/* ===== PSO FAIL fp=0x%08X hr=0x%08lX nrt=%d rtfmt=%d vs_idx=%d ===== */\n",
                          fp_addr, hr, nrt, (int)rtfmt, vs_idx);
                  fprintf(f, "/* ----- VS ----- */\n%s\n",
                          (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n && s_d3d.vp_vs[vs_idx].src)
                              ? s_d3d.vp_vs[vs_idx].src : "/* (base VS, source not kept) */");
                  fprintf(f, "/* ----- PS ----- */\n%s\n\n", hlsl);
                  /* With D3D12_DBG=1 the debug layer says exactly what is wrong. */
                  ID3D12InfoQueue* iq = NULL;
                  if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                          s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
                      UINT64 nm = iq->lpVtbl->GetNumStoredMessages(iq);
                      for (UINT64 mi = 0; mi < nm; mi++) {
                          SIZE_T len = 0;
                          iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                          D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                          if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len))) {
                              fprintf(f, "/* [D3D12 debug] %s */\n", m->pDescription);
                              printf("[FP][DBG] %s\n", m->pDescription);
                          }
                          free(m);
                      }
                      iq->lpVtbl->ClearStoredMessages(iq);
                      iq->lpVtbl->Release(iq);
                  }
                  fclose(f);
              }
          } }
        static int _nf = -1; if (_nf < 0) _nf = getenv("PSO_NOFALLBACK") ? 1 : 0;
        if (!_nf) {
            for (int _r = 0; _r < nrt; _r++) pd.BlendState.RenderTarget[_r].BlendEnable = FALSE;
            hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
            if (SUCCEEDED(hr)) { static int _f1=0; if ((_f1 < 16 && _f1++ < 16)) printf("[FP] PSO fallback OK without blend (fp=0x%08X rtfmt=%d)\n", fp_addr, (int)rtfmt); }
            else {
                for (int _r = 0; _r < nrt; _r++) pd.RTVFormats[_r] = DXGI_FORMAT_R8G8B8A8_UNORM;
                hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(
                    s_d3d.device, &pd, &IID_ID3D12PipelineState, (void**)&pso);
                static int _f2=0; if ((_f2 < 16 && _f2++ < 16)) printf("[FP] PSO fallback RGBA8 no-blend %s (fp=0x%08X)\n", SUCCEEDED(hr) ? "OK" : "STILL FAILS", fp_addr);
                /* A PSO whose RTV format differs from the bound target cannot be used on it. */
                if (SUCCEEDED(hr) && rtfmt != DXGI_FORMAT_R8G8B8A8_UNORM) { pso->lpVtbl->Release(pso); pso = NULL; hr = E_FAIL; }
            }
        }
        if (FAILED(hr)) pso = NULL;
    }
    pb->lpVtbl->Release(pb);
    if (pso) { static int _ok=0; if ((_ok < 32 && _ok++ < 32)) printf("[FP] guest FP pipeline ready (fp=0x%08X)\n", fp_addr); }

    /* insert. When full, replace round-robin and release the old pipeline only
     * after the GPU is done with it (the command list being recorded may already
     * have bound it: an immediate Release was a GPU use-after-free). */
    u32 ins;
    if (s_d3d.vp_fp_n < VP_FP_CACHE) ins = (u32)s_d3d.vp_fp_n++;
    else {
        static u32 rr = 0;
        ins = rr; rr = (rr + 1) % VP_FP_CACHE;
        if (s_d3d.vp_fp[ins].pso) defer_release((ID3D12Resource*)s_d3d.vp_fp[ins].pso);
        s_d3d.vp_fp[ins].pso = NULL;
        { static int _w = 0; if ((_w < 4 && _w++ < 4))
            fprintf(stderr, "[PSOCACHE] full (%d pipelines) -- replacing entry %u%c", VP_FP_CACHE, ins, 10); }
    }
    { int saved_n = s_d3d.vp_fp_n; s_d3d.vp_fp_n = (int)ins;   /* the block below fills vp_fp[vp_fp_n] */
    s_d3d.vp_fp[s_d3d.vp_fp_n].fp_addr = fp_addr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_idx  = vs_idx;
    s_d3d.vp_fp[s_d3d.vp_fp_n].vs_hash = vs_hash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].gen     = s_d3d.vp_gen;
    s_d3d.vp_fp[s_d3d.vp_fp_n].blend   = blend;
    s_d3d.vp_fp[s_d3d.vp_fp_n].nrt     = nrt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].rtfmt   = (u32)rtfmt;
    s_d3d.vp_fp[s_d3d.vp_fp_n].exp32   = exp32;
    s_d3d.vp_fp[s_d3d.vp_fp_n].ucode_hash = uhash;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cmask   = cmask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cull    = cull;
    s_d3d.vp_fp[s_d3d.vp_fp_n].cube_mask = cube_mask;
    s_d3d.vp_fp[s_d3d.vp_fp_n].kaddr   = kaddr;
    s_d3d.vp_fp[s_d3d.vp_fp_n].pso     = pso;
    s_d3d.vp_fp_n = saved_n; }
    fpi_insert(kh, ins);
    return pso;
}
/* v225A: time pipeline misses (FP decompile + compile + PSO creation) on the render thread */
static ID3D12PipelineState* vp_get_fp_pso(int vs_idx, u32 fp_addr, u32 blend, int nrt,
                                          DXGI_FORMAT rtfmt, int exp32, u32 cmask, u32 cull,
                                          u32 cube_mask)
{
    const u32 m0 = s_sf_pso_miss; const unsigned long long t0 = hz_tsc();
    ID3D12PipelineState* r = vp_get_fp_pso_impl(vs_idx, fp_addr, blend, nrt, rtfmt, exp32, cmask, cull, cube_mask);
    if (s_sf_pso_miss != m0) { const unsigned long long dt = hz_tsc() - t0;
        hz_add(HZ_PSO_TSC, (long long)dt); hz_add(HZ_PSO_N, 1);
        /* v235D: pipeline-miss cost by A/B phase parity (SHC_PACK / PSO_LIB in-run A/B evidence) */
        extern volatile int g_ab_phase;
        static unsigned long long c0 = 0; static LARGE_INTEGER q0, qf;
        static u64 n[2], tsc[2], tot = 0;
        if (!c0) { c0 = t0; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf); }
        const int par = g_ab_phase & 1; n[par]++; tsc[par] += dt;
        if ((++tot % 32) == 0) {
            LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
            const double us = (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)qf.QuadPart;
            const double tpus = us > 0 ? (double)(hz_tsc() - c0) / us : 0.0;
            if (tpus > 0) fprintf(stderr, "[PSO_MISS] v235D %llu pipeline misses; parity 0: %llu, mean %.0f us | parity 1: %llu, mean %.0f us%c",
                (unsigned long long)tot, (unsigned long long)n[0], n[0] ? (double)tsc[0] / n[0] / tpus : 0.0,
                (unsigned long long)n[1], n[1] ? (double)tsc[1] / n[1] / tpus : 0.0, 10);
        } }
    return r;
}

/* Upload the guest texture for a VP draw into a per-frame texture slot
 * (re-uploaded every frame: gcm/cube's plasma animates in guest memory).
 * Returns the slot index (SRV at heap 1+slot) or -1. Must run while the
 * command list is open, before the draw passes. */
/* NV4097 TEXTURE_CONTROL1 component remap -> D3D12 Shader4ComponentMapping.
 * The crossbar decode itself is RSX semantics and lives in
 * rsx_texture_layout.c; all that is left here is packing the four selectors
 * into D3D12's word. Its force-zero/force-one encodings are 4 and 5, which is
 * what RSX_REMAP_ZERO/ONE already are, so the selectors pass straight through.
 *
 * out[] arrives in the crossbar's field order A,R,G,B; D3D12 wants
 * destR | destG<<3 | destB<<6 | destA<<9 | valid<<12. */
static u32 rsx_remap_to_d3d(u32 c1, u32 basef)
{
    u8 out[4];
    rsx_texture_component_remap(c1, basef, out);
    /* TEX_REMAP_ID=<n>: override the derived crossbar with a fixed mapping, to
     * test channel order directly. The resource holds the guest's A8R8G8B8
     * bytes straight through, so its components are (R=A, G=R, B=G, A=B) and
     * the shader needs destR=1, destG=2, destB=3, destA=0 to see real RGBA.
     *   1 = identity   2 = rotate (the ARGB fix)   3 = BGRA swap */
    { static int fixed = -1;
      if (fixed < 0) { const char* e = getenv("TEX_REMAP_ID"); fixed = e ? atoi(e) : 0; }
      if (fixed == 1) return 0u | (1u<<3) | (2u<<6) | (3u<<9) | (1u<<12);
      if (fixed == 2) return 1u | (2u<<3) | (3u<<6) | (0u<<9) | (1u<<12);
      if (fixed == 3) return 2u | (1u<<3) | (0u<<6) | (3u<<9) | (1u<<12); }
    /* D3D12 mapping: destR | destG<<3 | destB<<6 | destA<<9 | valid bit */
    { static int _rd = -1; if (_rd < 0) _rd = getenv("REMAPDBG") ? 1 : 0;
      if (_rd) { static u32 seen[32]; static int ns=0;
        u32 k = (basef << 16) | (c1 & 0xFFFFu); int f=0;
        for (int i2=0;i2<ns;i2++) if (seen[i2]==k) f=1;
        if (!f && ns<32) { seen[ns++]=k;
            fprintf(stderr, "[REMAP] basef=0x%02X c1=0x%04X -> destR=%u destG=%u destB=%u destA=%u%c",
                    basef, c1 & 0xFFFFu, out[1], out[2], out[3], out[0], 10); } } }
    return out[1] | (out[2] << 3) | (out[3] << 6) | (out[0] << 9) | (1u << 12);
}

/* Morton/Z-order texel offset for RSX swizzled textures (LN bit clear).
 * Interleaves x (even bit positions) and y (odd) until the smaller dimension's
 * bits run out, then the larger dimension's remaining bits ride above -- the
 * NV40 layout (matches RPCS3 convert_linear_swizzle). POT dims only, which is
 * what the hardware requires for swizzled textures. */

/* Sparse FNV-1a over a texture's source bytes -- enough to notice an animated
 * surface changing, cheap enough to run on every bind. Kept a function rather
 * than a statement-expression macro so the file still compiles under MSVC;
 * ps3recomp builds with clang-cl, but titles like Tokyo Jungle drive the build
 * through the Visual Studio generator.
 *
 * v234 TEX_CSUM_MLP keeps the exact sample offsets and exact FNV operation
 * order, but demand-loads eight independent words before consuming them.  A
 * large texture samples about 1024 cache lines; exposing eight outstanding
 * loads lets an out-of-order CPU overlap their latency instead of following
 * one long load/hash chain.  This is deliberately not the rejected v225
 * TEX_CSUM_PF look-ahead: every load here is an existing checksum sample and
 * is consumed immediately.  The scalar tail makes the result bit-identical.
 * TEX_CSUM_MLP_PHASE=0/1 enables it only on that A/B phase parity. */
static int s_tex_csum_mlp_cfg = -1, s_tex_csum_mlp_phase = -1;

static int tex_csum_mlp_on(void)
{
    extern volatile int g_ab_phase;
    if (s_tex_csum_mlp_cfg < 0) {
        const char* e = getenv("TEX_CSUM_MLP");
        const char* p = getenv("TEX_CSUM_MLP_PHASE");
        s_tex_csum_mlp_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_tex_csum_mlp_phase = atoi(p) & 1;
        fprintf(stderr, "[TEX_CSUM_MLP] v234 eight-load exact FNV batches: %s (phase parity %d)%c",
                s_tex_csum_mlp_cfg ? "ON" : "off", s_tex_csum_mlp_phase, 10);
    }
    return s_tex_csum_mlp_cfg &&
           (s_tex_csum_mlp_phase < 0 || ((g_ab_phase & 1) == s_tex_csum_mlp_phase));
}

static u32 tex_csum_step(const u8* base, u32 nbytes, u32 step)
{
    u32 h = 2166136261u, i = 0;
    if (tex_csum_mlp_on()) {
        const u64 span = (u64)step * 7u + 3u;
        for (; (u64)i + span < nbytes; i += step * 8u) {
            u32 w0, w1, w2, w3, w4, w5, w6, w7;
            memcpy(&w0, base + i,            sizeof w0);
            memcpy(&w1, base + i + step,     sizeof w1);
            memcpy(&w2, base + i + step * 2, sizeof w2);
            memcpy(&w3, base + i + step * 3, sizeof w3);
            memcpy(&w4, base + i + step * 4, sizeof w4);
            memcpy(&w5, base + i + step * 5, sizeof w5);
            memcpy(&w6, base + i + step * 6, sizeof w6);
            memcpy(&w7, base + i + step * 7, sizeof w7);
            h ^= w0; h *= 16777619u;
            h ^= w1; h *= 16777619u;
            h ^= w2; h *= 16777619u;
            h ^= w3; h *= 16777619u;
            h ^= w4; h *= 16777619u;
            h ^= w5; h *= 16777619u;
            h ^= w6; h *= 16777619u;
            h ^= w7; h *= 16777619u;
        }
    }
    for (; i + 3 < nbytes; i += step) {
        u32 w32; memcpy(&w32, base + i, sizeof w32);
        h ^= w32; h *= 16777619u;
    }
    return h;
}

static u32 tex_csum(const u8* base, u32 nbytes)
{
    return tex_csum_step(base, nbytes, nbytes > 4096u ? nbytes / 1024u : 4u);
}

static u32 tex_csum_dense(const u8* base, u32 nbytes)
{
    u32 step = nbytes > 65536u ? (nbytes / 16384u) & ~3u : 4u;
    if (!step) step = 4u;
    return tex_csum_step(base, nbytes, step);
}

/* A cube may change any face, not just +X. Hash the level-zero payload of
 * all faces using the same guest stride as the uploader. */
static u32 tex_csum_faces(const u8* base, u32 nbytes, u32 face_stride)
{
    u32 h = tex_csum(base, nbytes);
    if (face_stride) for (u32 f = 1; f < 6; f++)
        h = (h ^ tex_csum(base + f * face_stride, nbytes)) * 16777619u;
    return h;
}

/* v227A TEX_ASSOC: vp_upload_tex_slot used one hash hint for each of 8192
 * buckets.  A collision did not merely cost another probe: it fell back to a
 * cache-cold linear walk over all 4096 VPTexSlot records.  City traversal has
 * hundreds of first-use texture lookups per frame, so ordinary bucket
 * collisions showed up inside the 2.7--5.9 ms/frame [FRAMEPCT] texture time
 * even when there were zero allocations or uploads.
 *
 * Retain four recent slot numbers per bucket.  Every candidate is still
 * verified against the complete texture identity below, and a miss still does
 * the authoritative full scan, so this is only a lookup accelerator: cache
 * contents, checksum frequency, uploads, and rendered pixels are unchanged.
 * TEX_ASSOC=0 restores the one-hint path. TEX_ASSOC_PHASE=0/1 enables the new
 * path only on even/odd A/B phases. TEX_ASSOC_BIT may instead name an ABX bit
 * for a particular benchmark; it defaults to zero because the title's current
 * 31-bit ABX namespace has no genuinely unused bit. */
#define TEX_ASSOC_SETS 8192u
#define TEX_ASSOC_WAYS 4
static int s_tex_assoc[TEX_ASSOC_SETS][TEX_ASSOC_WAYS];
static int s_tex_assoc_cfg = -1;
static int s_tex_assoc_phase = -1;
static u32 s_tex_assoc_bit = 0;
static u64 s_tex_assoc_first = 0, s_tex_assoc_extra = 0, s_tex_assoc_scan = 0;

static int tex_assoc_on(void)
{
    extern volatile int g_abx, g_ab_phase;
    if (s_tex_assoc_cfg < 0) {
        const char* e = getenv("TEX_ASSOC");
        const char* b = getenv("TEX_ASSOC_BIT");
        const char* p = getenv("TEX_ASSOC_PHASE");
        s_tex_assoc_cfg = (e && *e == '0') ? 0 : 1;
        if (b && *b) s_tex_assoc_bit = (u32)strtoul(b, NULL, 0);
        if (p && *p) s_tex_assoc_phase = atoi(p) & 1;
        fprintf(stderr, "[TEX_ASSOC] v227A four-way exact texture-slot hints: %s (TEX_ASSOC=0 off; TEX_ASSOC_BIT=0x%X; phase parity %d)%c",
                s_tex_assoc_cfg ? "ON" : "off", s_tex_assoc_bit, s_tex_assoc_phase, 10);
    }
    return s_tex_assoc_cfg &&
           (s_tex_assoc_phase < 0 || ((g_ab_phase & 1) == s_tex_assoc_phase)) &&
           !(s_tex_assoc_bit && ((u32)g_abx & s_tex_assoc_bit));
}

static void tex_assoc_note(u32 bucket, int slot, int assoc)
{
    int* const w = s_tex_assoc[bucket];
    if (!assoc) { w[0] = slot + 1; return; }
    const int v = slot + 1;
    int at = TEX_ASSOC_WAYS - 1;
    for (int i = 0; i < TEX_ASSOC_WAYS; i++) if (w[i] == v) { at = i; break; }
    for (int i = at; i > 0; i--) w[i] = w[i - 1];
    w[0] = v;
}

static void defer_release(ID3D12Resource* r);   /* below, with off_rt_get */

/* v32e TEXTURE STAGING RING. Every cached texture used to own a persistent UPLOAD
 * buffer next to its DEFAULT texture: two CreateCommittedResource calls per new texture
 * (play32 PC log: 408 new textures in one camera cut = 2.73 s of create time, 1.85 fps
 * hitch; NtGdiDdDDICreateAllocation/DestroyAllocation2 high in the RSX thread's DLL
 * samples) and twice the memory. Uploads are only recorded inside render_frame, which
 * starts with wait_for_gpu(), so ONE persistently-mapped ring that is rewound at the top
 * of every render_frame can never be overwritten while the GPU still copies from it.
 * TEX_RING_MB=<n> (default 192, 0 = off -> per-texture upload buffers as before). */
static ID3D12Resource* s_tring = NULL;
static u8*  s_tring_map = NULL;
static u64  s_tring_size = 0, s_tring_off = 0;
static int  s_tring_state = -1;   /* -1 untried, 0 off/failed, 1 on */
static u64 s_tring_lo = 0, s_tring_hi = ~0ull;   /* v225A RF_EARLY: the half of the ring this frame may use */
static void tex_ring_reset(void) { s_tring_off = s_tring_lo; }
static int s_rf_early_frame = 0;   /* v225A: this exec prepares before the GPU wait */
static int s_rf_early_bit = 0;
static int rf_early_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("RF_EARLY"); v = (e && *e == '1') ? 1 : 0;
        const char* b = getenv("RF_EARLY_BIT"); s_rf_early_bit = (b && *b) ? (int)strtoul(b, NULL, 16) : 0;
        if (v && rf_npar() < 3) { v = 0; fprintf(stderr, "[RF_EARLY] v225A needs RF_TRIPLE=1 (3 record parities) -> off%c", 10); }
        fprintf(stderr, "[RF_EARLY] v225A prepass before the GPU wait: %s (RF_EARLY=1; ABX 0x%X set = old order)%c", v ? "ON" : "off", s_rf_early_bit, 10);
    }
    return v;
}
static int tex_ring_alloc(u64 need, u64* out_off)
{
    if (s_tring_state < 0) {
        const char* e = getenv("TEX_RING_MB"); long mb = e ? atol(e) : 192;
        s_tring_state = 0;
        if (mb > 0 && s_d3d.device) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (u64)mb << 20;
            bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&s_tring))) {
                D3D12_RANGE nr = {0, 0}; void* p = NULL;
                if (SUCCEEDED(s_tring->lpVtbl->Map(s_tring, 0, &nr, &p)) && p) {
                    s_tring_map = (u8*)p; s_tring_size = bd.Width; s_tring_state = 1;
                } else { s_tring->lpVtbl->Release(s_tring); s_tring = NULL; }
            }
        }
        fprintf(stderr, "[TEX] staging ring %s (%ld MB)%c", s_tring_state ? "ON" : "off", mb, 10);
    }
    if (s_tring_state != 1) return 0;
    u64 o = (s_tring_off + 511u) & ~(u64)511u;   /* D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT */
    if (o + need > s_tring_size || o + need > s_tring_hi) return 0;      /* full this frame: caller falls back */
    s_tring_off = o + need; *out_off = o;
    return 1;
}
/* v32f TEXTURE POOL. play32 (PC): pressing Start gave 9 s of black screen and the first
 * cutscene cuts 6-17 s stalls; PERF showed them all as texture CREATION ("texup 829 new
 * (create 6.01s)", "2326 new (create 10.55s)") -- ~3.5 ms per CreateCommittedResource,
 * each one a kernel allocation + residency op. Textures are now PLACED resources
 * sub-allocated from 64 MB DEFAULT heaps (one CreatePlacedResource is a few us).
 * Ranges of released textures return to the pool only in flush_graveyard (GPU idle).
 * TEX_POOL=0 restores committed resources. TEX_CACHE_MB (default 1024) bounds the
 * texture cache: past it a miss evicts the least-recently-used slot instead of
 * filling an empty one. */
#define TP_HEAP_BYTES (64ull << 20)
#define TP_PAGE       4096u
#define TP_PAGES      (u32)(TP_HEAP_BYTES / TP_PAGE)
#define TP_MAX_HEAPS  64
static ID3D12Heap* s_tp_heap[TP_MAX_HEAPS];
static u64 s_tp_bits[TP_MAX_HEAPS][TP_PAGES / 64];
static u32 s_tp_free[TP_MAX_HEAPS];
static int s_tp_n = 0, s_tp_state = -1;
static struct { int h; u32 p, n; } s_tp_pending[8192];
static int s_tp_pending_n = 0;
static u64 s_tex_bytes = 0;    /* bytes held by vp_tex resources (pool pages or estimate) */
static u32 s_tex_frame = 1;
static int tp_on(void)
{
    if (s_tp_state < 0) { const char* e = getenv("TEX_POOL"); s_tp_state = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[TEX] placed-texture pool %s%c", s_tp_state ? "ON (64 MB heaps)" : "off", 10); }
    return s_tp_state;
}
static void tp_mark(int h, u32 p, u32 n, int used)
{
    for (u32 i = p; i < p + n; i++) {
        if (used) s_tp_bits[h][i >> 6] |= 1ull << (i & 63);
        else      s_tp_bits[h][i >> 6] &= ~(1ull << (i & 63));
    }
    if (used) s_tp_free[h] -= n; else s_tp_free[h] += n;
}
static int tp_find(int h, u32 n, u32 al, u32* out)
{
    if (s_tp_free[h] < n) return 0;
    u32 p = 0;
    while (p + n <= TP_PAGES) {
        /* skip whole used words quickly */
        if ((p & 63) == 0 && s_tp_bits[h][p >> 6] == ~0ull) { p += 64; continue; }
        u32 i = 0;
        for (; i < n; i++) { u32 q = p + i; if (s_tp_bits[h][q >> 6] >> (q & 63) & 1) break; }
        if (i == n) { *out = p; return 1; }
        p = ((p + i + 1) + al - 1) / al * al;   /* restart past the used page, aligned */
    }
    return 0;
}
/* Allocate `bytes` with `align` from the pool. Returns heap index or -1. */
static int tp_alloc(u64 bytes, u64 align, u32* page, u32* npages)
{
    if (!tp_on() || bytes > TP_HEAP_BYTES) return -1;
    u32 n = (u32)((bytes + TP_PAGE - 1) / TP_PAGE), al = (u32)(align > TP_PAGE ? align / TP_PAGE : 1);
    for (int h = 0; h < s_tp_n; h++)
        if (s_tp_heap[h] && tp_find(h, n, al, page)) { tp_mark(h, *page, n, 1); *npages = n; return h; }
    if (s_tp_n >= TP_MAX_HEAPS) return -1;
    D3D12_HEAP_DESC hd = {0};
    hd.SizeInBytes = TP_HEAP_BYTES; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Alignment = 65536;
    hd.Flags = (D3D12_HEAP_FLAGS)(D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | tex_heap_flags());
    ID3D12Heap* hp = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap, (void**)&hp))) {
        hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
        if (FAILED(s_d3d.device->lpVtbl->CreateHeap(s_d3d.device, &hd, &IID_ID3D12Heap, (void**)&hp))) {
            static int w = 0; if ((w < 2 && w++ < 2)) fprintf(stderr, "[TEX] pool heap #%d refused -> committed fallback%c", s_tp_n, 10);
            return -1;
        }
    }
    int h = s_tp_n++;
    s_tp_heap[h] = hp; memset(s_tp_bits[h], 0, sizeof(s_tp_bits[h])); s_tp_free[h] = TP_PAGES;
    if (h < 4 || (h & 7) == 0) fprintf(stderr, "[TEX] pool heap #%d (%d MB total)%c", h, (h + 1) * 64, 10);
    if (!tp_find(h, n, al, page)) return -1;
    tp_mark(h, *page, n, 1); *npages = n; return h;
}
static void tp_defer_free(VPTexSlot* t)
{
    if (t->pheap >= 0 && t->pnpages) {
        if (s_tp_pending_n < (int)(sizeof(s_tp_pending) / sizeof(s_tp_pending[0]))) {
            s_tp_pending[s_tp_pending_n].h = t->pheap; s_tp_pending[s_tp_pending_n].p = t->ppage;
            s_tp_pending[s_tp_pending_n].n = t->pnpages; s_tp_pending_n++;
        }   /* else: leak the range (never seen; 8192 releases between two GPU idles) */
        s_tex_bytes -= (u64)t->pnpages * TP_PAGE;
    } else if (t->pnpages) s_tex_bytes -= (u64)t->pnpages * TP_PAGE;
    t->pheap = -1; t->ppage = 0; t->pnpages = 0;
}
static void tp_flush_frees(void)   /* call only once the GPU is idle (after graveyard Release) */
{
    for (int i = 0; i < s_tp_pending_n; i++) tp_mark(s_tp_pending[i].h, s_tp_pending[i].p, s_tp_pending[i].n, 0);
    s_tp_pending_n = 0;
}
/* v235C RF_EARLY two-generation frees: an early-order exec waits for frame N-1 only AFTER its own prepass, and the
 * ranges freed during that prepass may still back resources the (not yet submitted) list of frame N references. Free
 * just the entries that existed when the exec began; the newer ones wait for the next GPU wait. */
static int s_rf_tp_mark = 0;
static void tp_flush_frees_upto(int mark)
{
    if (mark > s_tp_pending_n) mark = s_tp_pending_n;
    if (mark <= 0) return;
    for (int i = 0; i < mark; i++) tp_mark(s_tp_pending[i].h, s_tp_pending[i].p, s_tp_pending[i].n, 0);
    memmove(&s_tp_pending[0], &s_tp_pending[mark], (size_t)(s_tp_pending_n - mark) * sizeof(s_tp_pending[0]));
    s_tp_pending_n -= mark;
}
static u64 tex_cache_budget(void)
{
    static u64 b = 0;
    if (!b) { const char* e = getenv("TEX_CACHE_MB"); long mb = e ? atol(e) : 1024; if (mb < 64) mb = 64; b = (u64)mb << 20; }
    return b;
}
static DXGI_FORMAT tex95_srv_dxgi(u32 fmt, u32 w, u32 h)
{
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    return (tl.fmt == RSX_TEXFMT_R16G16) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
}
/* v146 TEX_MIPS (default 1; 0 = old behaviour): upload the guest's whole mip chain, not just level 0.
 * Every texture used to be created with ONE level, so a texture the game samples with a mipmapped
 * (trilinear) filter was always read at full resolution: distant buildings shimmered/flickered while
 * moving, fine patterns (billboards, window grids) turned into moire stripes, and the GPU fetched
 * far more texture memory than needed. Swizzled (SZ) and compressed (DXT) textures only: their level
 * layout is unambiguous (rsx_texture_mip_chain); linear (LN) ones keep one level. */
static int tex_mips_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("TEX_MIPS"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[TEX] v146 mip chains %s (TEX_MIPS)%c", v ? "ON" : "off", 10); }
    return v;
}
/* ===========================================================================
 * v193 KBM GLYPHS: inFamous draws every button prompt from one icon font, font_icons (atlas font_icons_00.dds,
 * 512x256 DXT5, one level). assets/kbm_icons.bin (tools/make_kbm_icons.py) holds that atlas' original level 0
 * and a keyboard/mouse version with the same glyph cells (Space/C/F/E, mouse buttons, Q/R, V, WASD, mouse, Esc,
 * Tab, arrow keys -- the cellPad.c v150 bindings). When the player last used the keyboard/mouse
 * (g_input_kbm, cellPad.c) the upload of exactly that texture (size + format + checksum + full byte compare)
 * reads the KBM blocks instead of guest memory. The cached slot's checksum carries the mode, so a device
 * switch re-uploads it on the next bind = the prompts change on the next frame.
 * KBM_GLYPHS=0 never, 1 auto (default), 2 always KBM.
 * =========================================================================*/
extern volatile int g_input_kbm;   /* cellPad.c: 1 keyboard/mouse, 0 controller, -1 not known yet */
static int s_kg_state = 0, s_kg_cfg = -1;
/* v197: a table -- the prompt-font atlas (kbm_icons.bin, v193) + the HUD button-prompt textures
 * (kbm_prompts.bin: "IFKBMIC2", u32 n, n x {w, h, fmt, nb, csum, orig L0, KBM L0}; tools/make_kbm_prompts.py) */
typedef struct { u32 w, h, fmt, nb, csum; u8 *orig, *kbm; } KgEnt;
#define KG_MAX 32
static KgEnt s_kg[KG_MAX]; static int s_kg_n = 0;
#define KG_CSUM_KBM 0x4B424D31u
static int kg_add(FILE* f, const u32 h[5])
{
    if (s_kg_n >= KG_MAX || !h[3] || h[3] > (4u << 20)) return 0;
    KgEnt* e = &s_kg[s_kg_n];
    e->w = h[0]; e->h = h[1]; e->fmt = h[2]; e->nb = h[3]; e->csum = h[4];
    e->orig = (u8*)malloc(e->nb); e->kbm = (u8*)malloc(e->nb);
    if (!e->orig || !e->kbm || fread(e->orig, 1, e->nb, f) != e->nb || fread(e->kbm, 1, e->nb, f) != e->nb) {
        free(e->orig); free(e->kbm); return 0; }
    s_kg_n++; return 1;
}
static int kg_try(const char* path)
{
    FILE* f = fopen(path, "rb"); if (!f) return 0;
    char m[8]; u32 h[5]; int got = 0;
    if (fread(m, 1, 8, f) == 8) {
        if (!memcmp(m, "IFKBMIC1", 8)) { if (fread(h, 4, 5, f) == 5) got += kg_add(f, h); }
        else if (!memcmp(m, "IFKBMIC2", 8)) { u32 n = 0;
            if (fread(&n, 4, 1, f) == 1) for (u32 i = 0; i < n && i < KG_MAX; i++) { if (fread(h, 4, 5, f) != 5 || !kg_add(f, h)) break; got++; } }
    }
    fclose(f);
    if (got) fprintf(stderr, "[KBMGLYPH] v197 keyboard/mouse prompt textures loaded: %s (%d textures; KBM_GLYPHS=%d)%c", path, got, s_kg_cfg, 10);
    return got;
}
static int kg_ready(void)
{
    if (__builtin_expect(s_kg_state != 0, 1)) return s_kg_state > 0;
    s_kg_state = -1;
    { const char* e = getenv("KBM_GLYPHS"); s_kg_cfg = (e && *e) ? atoi(e) : 1; }
    /* v194: loaded even with KBM_GLYPHS=0 -- the settings menu can switch prompts at run time */
    char exe[MAX_PATH]; DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (n && n < MAX_PATH) {
        char* sl = strrchr(exe, '\\'); if (sl) *sl = 0;
        static const char* rel[] = { "\\assets\\", "\\..\\assets\\", "\\..\\..\\assets\\" };
        static const char* fn[] = { "kbm_icons.bin", "kbm_prompts.bin" };
        for (int k = 0; k < 2; k++)
            for (int i = 0; i < 3; i++) {
                char p2[MAX_PATH + 64]; snprintf(p2, sizeof p2, "%s%s%s", exe, rel[i], fn[k]);
                if (kg_try(p2)) break;
            }
    }
    if (s_kg_n) { s_kg_state = 1; return 1; }
    fprintf(stderr, "[KBMGLYPH] no assets\\kbm_icons.bin / kbm_prompts.bin -> controller prompts only%c", 10);
    return 0;
}
extern volatile int g_ui_kbm_glyphs;   /* v194 settings menu: -1 env, 0 controller, 1 auto, 2 keyboard/mouse */
static inline int kg_want_kbm(void)
{
    const int c = g_ui_kbm_glyphs >= 0 ? g_ui_kbm_glyphs : s_kg_cfg;
    return c == 2 || (c != 0 && g_input_kbm > 0);
}
/* one of the replaced textures (by size/format/checksum of the guest bytes); 0 = none, else index + 1 */
static inline int kg_hit(u32 w, u32 h, u32 basef, int cube, u32 cs)
{
    if (cube || basef != 0x88u || (w != 512u && w != 32u) || !kg_ready()) return 0;
    for (int i = 0; i < s_kg_n; i++)
        if (s_kg[i].w == w && s_kg[i].h == h && s_kg[i].fmt == basef && s_kg[i].csum == cs) return i + 1;
    return 0;
}
/* ---- v228 TEXCS: the frame's texture checksums in parallel at the top of the prepass --------------------------
 * Every cached texture bound in a frame is re-validated once per frame (tex_csum_faces / tex_csum_dense below:
 * 1024 sparse words). For any texture > 64 KB each sample is its own cache line, so ~1000 distinct city textures
 * per frame = ~1M scattered DRAM reads, issued one texture after another by the render thread: it is latency-
 * bound (fill buffers), not compute-bound. b043/b045 traversal: render-thread "tex" 5-6 ms/frame in the heavy
 * views while new textures averaged ~0.25/frame -> the time is this validation, not uploads.
 * TEXCS=1: before the per-draw loop the prepass collects every bound texture that hits a cached slot (same
 * identity test as vp_upload_tex_slot_impl), and the SAME checksum functions run for those slots on
 * TEXCS_THREADS workers (default 4, E-cores when the CPU is hybrid) plus the render thread itself. The
 * per-draw path then takes the precomputed value for that slot (verified: same frame, source offset, byte count,
 * face stride and checksum kind) instead of computing it. Values are bit-identical to the serial path; only the
 * moment within the same prepass differs (the guest runs concurrently either way). The render thread never waits
 * for a worker: whatever is not done when it runs out of jobs it computes itself. TEXCS_PHASE=0/1 = only on
 * A/B phases with (phase / TEXCS_PHASE_DIV) parity 0/1 (in-run A/B; DIV 2 + RF_EARLY_PHASE = a 2x2 factorial).
 * TEXCS_EARLY=1: build + publish the jobs before the render thread's GPU wait, harvest at the prepass (tcs_kick).
 * [TEXCS] every 600 frames. TEXCS unset/0 = old path exactly. Harness: tools/bench/texcs_harness.c. */
#define TEXCS_MAXJ 8192
typedef struct { const u8* base; u32 nb, fs, dense; int slot; volatile LONG64 res; } TexCsJob;
static TexCsJob s_tcj[2][TEXCS_MAXJ];            /* two arrays by generation parity (stragglers never see a refill) */
static u32 s_tcj_n[2];
static volatile LONG64 s_tcj_ctr = 0;            /* gen (bits 40..63) | n (bits 20..39) | next (bits 0..19) */
static volatile LONG s_tcj_wake = 0;             /* workers sleep on this (WaitOnAddress) */
static volatile LONG s_tcj_in[2] = { 0, 0 };     /* workers inside an array of that parity */
static u32 s_pcs_frame[VP_TEX_SLOTS], s_pcs_q[VP_TEX_SLOTS], s_pcs_off[VP_TEX_SLOTS], s_pcs_nb[VP_TEX_SLOTS],
           s_pcs_fs[VP_TEX_SLOTS], s_pcs_val[VP_TEX_SLOTS];
static u8 s_pcs_dense[VP_TEX_SLOTS];
static int s_tcs_cfg = -1, s_tcs_phase = -1, s_tcs_div = 1, s_tcs_nthr = 4;
static u64 s_tcs_frames, s_tcs_jobs, s_tcs_byw, s_tcs_take, s_tcs_skip_in, s_tcs_tsc;
static u32 tcs_compute(const TexCsJob* j)
{
    return j->dense ? tex_csum_dense(j->base, j->nb) : tex_csum_faces(j->base, j->nb, j->fs);
}
/* claim the next job of generation gen; -1 = none left (or the generation moved on) */
static int tcs_claim(u32 gen)
{
    for (;;) {
        const LONG64 v = s_tcj_ctr;
        if ((u32)((u64)v >> 40) != (gen & 0xFFFFFFu)) return -1;
        const u32 nx = (u32)v & 0xFFFFFu, n = (u32)((u64)v >> 20) & 0xFFFFFu;
        if (nx >= n) return -1;
        if (InterlockedCompareExchange64(&s_tcj_ctr, v + 1, v) == v) return (int)nx;
    }
}
static void tcs_run(u32 gen, int worker)
{
    TexCsJob* const a = s_tcj[gen & 1u];
    for (int j; (j = tcs_claim(gen)) >= 0; ) {
        const u32 cs = tcs_compute(&a[j]);
        InterlockedExchange64(&a[j].res, (LONG64)(((u64)gen << 32) | cs));
        if (worker) InterlockedIncrement64((volatile LONG64*)&s_tcs_byw);
    }
}
static DWORD WINAPI tcs_worker(LPVOID p)
{
    (void)p;
    LONG seen = 0;
    for (;;) {
        LONG g = s_tcj_wake;
        while (g == seen) { WaitOnAddress(&s_tcj_wake, &seen, sizeof(LONG), INFINITE); g = s_tcj_wake; }
        seen = g;
        const u32 gen = (u32)g;
        InterlockedIncrement(&s_tcj_in[gen & 1u]);
        tcs_run(gen, 1);
        InterlockedDecrement(&s_tcj_in[gen & 1u]);
    }
    return 0;
}
/* E-cores of a hybrid CPU (lowest EfficiencyClass) as a CPU-set list; 0 = not hybrid / unknown */
static ULONG tcs_ecore_sets(ULONG* ids, ULONG cap)
{
    ULONG len = 0; GetSystemCpuSetInformation(NULL, 0, &len, GetCurrentProcess(), 0);
    if (!len) return 0;
    u8* buf = (u8*)malloc(len); if (!buf) return 0;
    ULONG n = 0; BYTE lo = 255, hi = 0;
    if (GetSystemCpuSetInformation((PSYSTEM_CPU_SET_INFORMATION)buf, len, &len, GetCurrentProcess(), 0)) {
        for (ULONG o = 0; o < len; ) { const SYSTEM_CPU_SET_INFORMATION* s = (const SYSTEM_CPU_SET_INFORMATION*)(buf + o);
            if (s->Type == CpuSetInformation) { const BYTE e = s->CpuSet.EfficiencyClass; if (e < lo) lo = e; if (e > hi) hi = e; }
            o += s->Size; if (!s->Size) break; }
        if (hi > lo) for (ULONG o = 0; o < len && n < cap; ) { const SYSTEM_CPU_SET_INFORMATION* s = (const SYSTEM_CPU_SET_INFORMATION*)(buf + o);
            if (s->Type == CpuSetInformation && s->CpuSet.EfficiencyClass == lo) ids[n++] = s->CpuSet.Id;
            o += s->Size; if (!s->Size) break; }
    }
    free(buf);
    return n;
}
static int tcs_on(void)
{
    extern volatile int g_ab_phase;
    if (s_tcs_cfg < 0) {
        const char* e = getenv("TEXCS"); const char* p = getenv("TEXCS_PHASE"); const char* t = getenv("TEXCS_THREADS");
        s_tcs_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_tcs_phase = atoi(p) & 1;
        { const char* dv = getenv("TEXCS_PHASE_DIV"); if (dv && *dv) { s_tcs_div = atoi(dv); if (s_tcs_div < 1) s_tcs_div = 1; } }
        if (t && *t) { s_tcs_nthr = atoi(t); if (s_tcs_nthr < 0) s_tcs_nthr = 0; if (s_tcs_nthr > 16) s_tcs_nthr = 16; }
        if (s_tcs_cfg) {
            ULONG ids[64]; const char* ec = getenv("TEXCS_ECORE");
            const ULONG ne = (ec && *ec == '0') ? 0 : tcs_ecore_sets(ids, 64);
            for (int i = 0; i < s_tcs_nthr; i++) {
                HANDLE th = CreateThread(NULL, 64u << 10, tcs_worker, NULL, 0, NULL);
                if (!th) { s_tcs_nthr = i; break; }
                if (ne) SetThreadSelectedCpuSets(th, ids, ne);
                CloseHandle(th);
            }
            fprintf(stderr, "[TEXCS] v228 parallel texture checksums: ON, %d workers%s (TEXCS_THREADS; render thread joins in), phase parity %d of phase/%d%c",
                    s_tcs_nthr, ne ? " on the E-cores" : "", s_tcs_phase, s_tcs_div, 10);
        } else fprintf(stderr, "[TEXCS] v228 parallel texture checksums: off (TEXCS=1 on)%c", 10);
    }
    return s_tcs_cfg && (s_tcs_phase < 0 || (((g_ab_phase / s_tcs_div) & 1) == s_tcs_phase));
}
/* The per-draw path asks for slot i's checksum; 1 = precomputed this frame for exactly these inputs */
static inline int tcs_take(int i, u32 off, u32 nb, u32 fs, int dense, u32* cs)
{
    if (s_pcs_frame[i] != s_tex_frame || s_pcs_off[i] != off || s_pcs_nb[i] != nb || s_pcs_fs[i] != fs ||
        s_pcs_dense[i] != (u8)dense) return 0;
    *cs = s_pcs_val[i]; s_pcs_frame[i] = 0; s_tcs_take++;
    return 1;
}
/* v228 TEXCS_EARLY=1: the prescan + worker wake-up run BEFORE the render thread's GPU wait (tcs_kick, called with
 * stamp = the s_tex_frame the prepass will have), the render thread harvests at the prepass (tcs_prepare). The draw
 * records of this exec and the slot table are not touched by anyone else in between (the FIFO thread records into
 * another parity; slots change only in the prepass), so the job list is the same one the prepass would build. */
static int s_tcs_early = -1;
static u32 s_tcs_kgen = 0, s_tcs_kstamp = 0, s_tcs_kn = 0;   /* open early generation (0 = none) */
static unsigned long long s_tcs_kt0 = 0;
static int tcs_early_on(void)
{
    if (s_tcs_early < 0) { const char* e = getenv("TEXCS_EARLY"); s_tcs_early = (e && *e == '1') ? 1 : 0;
        fprintf(stderr, "[TEXCS] v228 early kick before the GPU wait: %s (TEXCS_EARLY)%c", s_tcs_early ? "ON" : "off", 10); }
    return s_tcs_early;
}
static u32 tcs_scan_publish(const D3D12DrawRecord* draws, u32 count, u32 g, u32 stamp);
static void tcs_kick(const D3D12DrawRecord* draws, u32 count, u32 stamp)
{
    extern uint8_t* vm_base;
    static u32 gen_k = 0x800000u;   /* early generations live in their own range (24-bit counter field) */
    s_tcs_kgen = 0;
    if (!tcs_on() || !vm_base || !tcs_early_on() || !s_tcs_nthr) return;
    const u32 g = ++gen_k; const u32 par = g & 1u;
    if (s_tcj_in[par]) { s_tcs_skip_in++; return; }
    s_tcs_kt0 = hz_tsc();
    s_tcs_kn = tcs_scan_publish(draws, count, g, stamp);
    s_tcs_kgen = g; s_tcs_kstamp = stamp;
}
static void tcs_harvest(u32 g, u32 n)
{
    TexCsJob* const a = s_tcj[g & 1u];
    tcs_run(g, 0);
    for (u32 k = 0; k < n; k++) {
        const LONG64 r = a[k].res; u32 cs;
        if ((u32)((u64)r >> 32) == g) cs = (u32)r; else cs = tcs_compute(&a[k]);   /* not done yet: do it here */
        const int s = a[k].slot;
        s_pcs_val[s] = cs; s_pcs_frame[s] = s_tex_frame;
    }
    InterlockedExchange64(&s_tcj_ctr, (LONG64)(((u64)(g & 0xFFFFFFu) << 40)));   /* close the generation */
}
static void tcs_report(unsigned long long dt, u32 n);
static void tcs_prepare(const D3D12DrawRecord* draws, u32 count)
{
    extern uint8_t* vm_base;
    static u32 gen = 0;
    if (s_tcs_kgen) {   /* TEXCS_EARLY: jobs were published before the GPU wait */
        const u32 g = s_tcs_kgen, n = s_tcs_kn; s_tcs_kgen = 0;
        if (s_tcs_kstamp == s_tex_frame) {
            const unsigned long long t0 = hz_tsc();
            if (n) tcs_harvest(g, n);
            const unsigned long long dt = hz_tsc() - t0;
            hz_add(HZ_TEX_TSC, (long long)dt);
            tcs_report(dt, n);
            return;
        }
        InterlockedExchange64(&s_tcj_ctr, (LONG64)(((u64)(g & 0xFFFFFFu) << 40)));   /* stale kick: close, do it now */
    }
    if (!tcs_on() || !vm_base) return;
    const unsigned long long t0 = hz_tsc();
    const u32 g = (++gen) & 0x7FFFFFu; const u32 par = g & 1u;
    if (s_tcj_in[par]) { s_tcs_skip_in++; return; }   /* a worker from two frames ago is still inside: serial frame */
    const u32 n = tcs_scan_publish(draws, count, g, s_tex_frame);
    if (n) tcs_harvest(g, n);
    const unsigned long long dt = hz_tsc() - t0;
    hz_add(HZ_TEX_TSC, (long long)dt);   /* counted as texture work, like vp_upload_tex_slot */
    tcs_report(dt, n);
}
/* prescan: queue one job per cached slot bound this frame; publish generation g and wake the workers */
static u32 tcs_scan_publish(const D3D12DrawRecord* draws, u32 count, u32 g, u32 stamp)
{
    extern uint8_t* vm_base;
    const u32 par = g & 1u;
    TexCsJob* const a = s_tcj[par];
    const int assoc = tex_assoc_on();
    const int nway = assoc ? TEX_ASSOC_WAYS : 1;
    u32 n = 0;
    for (u32 d = 0; d < count && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* dr = &draws[d];
        if (!dr->is_vp || dr->is_clear) continue;
        /* The declaration mask is immutable for the lifetime of this record.
         * Compute it lazily, then avoid rescanning all 16 texture records for
         * every populated unit during the checksum prepass. */
        u32 cube_mask = 0; int cube_mask_valid = 0;
        for (int u = 0; u < RSX_D3D_TEX_UNITS; u++) {
            if (!dr->tex[u].set || !dr->tex[u].off) continue;
            if (dr->fp_meta_valid && !(dr->fp_tex_mask & (1u << u))) continue;
            const u32 fmt = dr->tex[u].fmt, bf = fmt & 0x9F, w = dr->tex[u].w, h = dr->tex[u].h;
            if (!(bf == 0x81 || bf == 0x85 || bf == 0x8B || (bf >= 0x86 && bf <= 0x88)) || !w || !h) continue;
            if (!cube_mask_valid) { cube_mask = dr_cube_mask(dr); cube_mask_valid = 1; }
            const int cube = dr->tex[u].cube && (cube_mask & (1u << u));
            const u32 fs = cube ? rsx_texture_cube_face_stride(fmt, w, h, dr->tex[u].mips, 0) : 0;
            const u32 key = dr->tex[u].off;
            u32 hh = (key * 2654435761u) ^ (w * 40503u) ^ (h * 2246822519u) ^ (fmt * 3266489917u) ^ (u32)cube ^ fs;
            hh = (hh ^ (hh >> 15)) & 8191u;
            int slot = -1;
            for (int way = 0; way < nway; way++) {
                const int i = s_tex_assoc[hh][way] - 1;
                if (i < 0 || i >= VP_TEX_SLOTS) continue;
                const VPTexSlot* c = &s_d3d.vp_tex[i];
                if (c->res && c->key == key && c->w == w && c->h == h && c->fmt == fmt && c->cube == cube && c->face_stride == fs) { slot = i; break; }
            }
            if (slot < 0 || s_pcs_q[slot] == stamp || n >= TEXCS_MAXJ) continue;
            s_pcs_q[slot] = stamp;
            const VPTexSlot* c = &s_d3d.vp_tex[slot];
            rsx_tex_layout tl; rsx_texture_layout(fmt, w, h, &tl);
            const u32 nb = tl.compressed ? (tl.row_bytes * tl.rows) : (w * h * (tl.compressed ? 1u : tl.bytes_per_texel));
            TexCsJob* j = &a[n++];
            j->base = vm_base + c->off; j->nb = nb; j->fs = fs; j->dense = (bf == 0x81); j->slot = slot;
            j->res = 0;
            s_pcs_off[slot] = c->off; s_pcs_nb[slot] = nb; s_pcs_fs[slot] = fs; s_pcs_dense[slot] = (u8)(bf == 0x81);
            s_pcs_frame[slot] = 0;
        }
    }
    s_tcj_n[par] = n;
    if (n) {
        InterlockedExchange64(&s_tcj_ctr, (LONG64)(((u64)(g & 0xFFFFFFu) << 40) | ((u64)n << 20)));
        if (s_tcs_nthr) { InterlockedExchange(&s_tcj_wake, (LONG)g); WakeByAddressAll((PVOID)&s_tcj_wake); }
    }
    return n;
}
static void tcs_report(unsigned long long dt, u32 n)
{
    s_tcs_frames++; s_tcs_jobs += n; s_tcs_tsc += dt;
    if (s_tcs_frames >= 600) {
        static double tpms = 0.0;
        if (tpms == 0.0) { LARGE_INTEGER f, q0, q1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&q0);
            const unsigned long long c0 = hz_tsc(); Sleep(2); const unsigned long long c1 = hz_tsc(); QueryPerformanceCounter(&q1);
            tpms = (double)(c1 - c0) / ((double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart); }
        fprintf(stderr, "[TEXCS] last %llu frames: %.0f checksums/frame (%.0f%% by workers), %.0f taken/frame, phase %.2f ms/frame, %llu serial frames (worker still in)%c",
                (unsigned long long)s_tcs_frames, (double)s_tcs_jobs / s_tcs_frames,
                s_tcs_jobs ? 100.0 * (double)s_tcs_byw / (double)s_tcs_jobs : 0.0, (double)s_tcs_take / s_tcs_frames,
                tpms > 0.0 ? (double)s_tcs_tsc / tpms / (double)s_tcs_frames : 0.0, (unsigned long long)s_tcs_skip_in, 10);
        s_tcs_frames = s_tcs_jobs = s_tcs_byw = s_tcs_take = s_tcs_skip_in = s_tcs_tsc = 0;
    }
}
/* v235D TEX_FAST_DECODE=1 (default off): exact table-driven decode for uncompressed guest textures
 * (rsx_texture_decode_fast32 in rsx_texture_layout.c: 32-bit A8R8G8B8/D8R8G8B8 swizzled or linear with one
 * 32-bit load/rotate/store per texel, other swizzled formats with tabulated Morton offsets; bit-identical
 * output, verified on 7004 format/size cases).  The old path walks rsx_swizzle_offset's bit loop + a format
 * switch + 4 byte stores per texel into the write-combined upload ring (~18 ms per 1024^2 ARGB image).
 * TEX_FAST_DECODE_PHASE=0/1: fast path only on that A/B phase parity.  [TEX_FAST] reports uncompressed decode
 * cost by parity. */
static int s_tfd_cfg = -1, s_tfd_phase = -1;
static void tex_decode_d(void* dst, u32 dst_pitch, const u8* src, u32 w, u32 h, const rsx_tex_layout* tl, int rgba)
{
    extern volatile int g_ab_phase;
    if (s_tfd_cfg < 0) {
        const char* e = getenv("TEX_FAST_DECODE"); const char* p = getenv("TEX_FAST_DECODE_PHASE");
        s_tfd_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_tfd_phase = atoi(p) & 1;
        fprintf(stderr, "[TEX_FAST] v235D exact table-driven texture decode: %s (phase parity %d)%c", s_tfd_cfg ? "ON" : "off", s_tfd_phase, 10);
    }
    if (tl->compressed) { rsx_texture_decode(dst, dst_pitch, src, w, h, tl, rgba); return; }   /* row memcpy either way */
    const int par = g_ab_phase & 1;
    const int fast = s_tfd_cfg && (s_tfd_phase < 0 || par == s_tfd_phase);
    const unsigned long long t0 = hz_tsc();
    if (!(fast && rsx_texture_decode_fast32(dst, dst_pitch, src, w, h, tl, rgba)))
        rsx_texture_decode(dst, dst_pitch, src, w, h, tl, rgba);
    if (!s_tfd_cfg) return;
    static unsigned long long c0 = 0; static LARGE_INTEGER q0, qf;
    static u64 n[2], tx[2], tsc[2], tot = 0;
    if (!c0) { c0 = t0; QueryPerformanceCounter(&q0); QueryPerformanceFrequency(&qf); }
    n[par]++; tx[par] += (u64)w * h; tsc[par] += hz_tsc() - t0;
    if ((++tot % 1024) == 0) {
        LARGE_INTEGER q1; QueryPerformanceCounter(&q1);
        const double us = (double)(q1.QuadPart - q0.QuadPart) * 1e6 / (double)qf.QuadPart;
        const double tpus = us > 0 ? (double)(hz_tsc() - c0) / us : 0.0;
        if (tpus > 0) fprintf(stderr, "[TEX_FAST] %llu uncompressed decodes; parity 0: %llu, %.1f Mtexels, %.1f ms, %.2f ns/texel | parity 1: %llu, %.1f Mtexels, %.1f ms, %.2f ns/texel%c",
            (unsigned long long)tot,
            (unsigned long long)n[0], tx[0] / 1e6, tsc[0] / tpus / 1000.0, tx[0] ? tsc[0] / tpus * 1000.0 / (double)tx[0] : 0.0,
            (unsigned long long)n[1], tx[1] / 1e6, tsc[1] / tpus / 1000.0, tx[1] ? tsc[1] / tpus * 1000.0 / (double)tx[1] : 0.0, 10);
    }
}
static int vp_upload_tex_slot_impl(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    const int _kg_k = kg_want_kbm();   /* v193: one read per call (bind check and stored checksum agree) */
    /* TEX_BUDGET=<n>: cap texture uploads per frame. Off by default -- it was
     * tried against the TDR and does not help, because PERF shows the upload
     * path costing 0.01s for 775 calls. Kept because it is the right lever if a
     * title ever does hit an upload-bound frame. */
    { static int budget = -1;
      if (budget < 0) { const char* e = getenv("TEX_BUDGET"); budget = e ? atoi(e) : 0; }
      if (budget > 0) {
          static u64 last_frame = ~0ull; static int used = 0;
          if (last_frame != (u64)s_d3d.frame_count) { last_frame = (u64)s_d3d.frame_count; used = 0; }
          if (++used > budget) return -1;   /* try again next frame */
      } }
    extern uint8_t* vm_base;

    if (!off || !w || !h || !vm_base || !s_d3d.srv_heap) return -1;
    /* Format classes (base = fmt & 0x9F). The LBP loading screen uses:
     * 0x85 A8R8G8B8 (swizzled UI art), 0x8B G8B8 (the 1024x2048 linear FONT
     * atlas -- without it no text renders at all), 0x86/87/88 DXT1/23/45
     * (512x512 detail/LUT layers bound at t1/t3 on every draw). */
    /* Format class, row sizes and swizzling are RSX semantics: rsx_texture_
     * layout.c owns them so the Metal backend can read a guest texture without
     * re-deriving any of it. Only the DXGI mapping below is ours. */
    rsx_tex_layout tl;
    rsx_texture_layout(fmt, w, h, &tl);
    u32 basef = fmt & 0x9F;              /* still needed for the SRV remap */
    int argb = (tl.fmt == RSX_TEXFMT_R8G8B8A8);
    int dxt  = tl.compressed;
    /* Compressed rows are counted in blocks, so keep bpp at 1 for the byte
     * arithmetic the diagnostics below do. */
    u32 bpp = tl.compressed ? 1u : tl.bytes_per_texel;
    /* The DECODED image's format, which is not always the guest's: the packed
     * 16-bit colour formats arrive here unpacked to RGBA8, and DEPTH24_D8 as a
     * float. rsx_texture_layout.h says which class each format byte lands in. */
    DXGI_FORMAT dxfmt = (tl.fmt == RSX_TEXFMT_R8G8B8A8) ? DXGI_FORMAT_R8G8B8A8_UNORM
                      : (tl.fmt == RSX_TEXFMT_R8G8)     ? DXGI_FORMAT_R8G8_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC1)      ? DXGI_FORMAT_BC1_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC2)      ? DXGI_FORMAT_BC2_UNORM
                      : (tl.fmt == RSX_TEXFMT_BC3)      ? DXGI_FORMAT_BC3_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16)      ? DXGI_FORMAT_R16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16)   ? DXGI_FORMAT_R16G16_UNORM
                      : (tl.fmt == RSX_TEXFMT_R16G16F)  ? DXGI_FORMAT_R16G16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R16G16B16A16F) ? DXGI_FORMAT_R16G16B16A16_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32F)     ? DXGI_FORMAT_R32_FLOAT
                      : (tl.fmt == RSX_TEXFMT_R32G32B32A32F) ? DXGI_FORMAT_R32G32B32A32_FLOAT
                      : DXGI_FORMAT_R8_UNORM;
    /* DXT data is stored as linear 4x4 block rows (compressed formats are
     * never Morton-swizzled on RSX). Row of blocks = (w/4)*blocksize. */
    u32 blkrow  = dxt ? tl.row_bytes : 0;
    u32 blkrows = dxt ? tl.rows      : 0;
    const u32 face_stride = cube ? rsx_texture_cube_face_stride(fmt, w, h, mips, 0) : 0;
    const u32 key_off = off;         /* lookup key: the offset as bound */
    /* Sparse checksum of a texture's source bytes -- enough to notice an
     * animated surface changing, cheap enough to run on every bind. */
    /* (portable helper tex_csum() -- see above; a statement-expression macro
     * here was a GCC/Clang extension MSVC rejects.) */
    #define TEX_CSUM(base, nbytes) tex_csum((base), (nbytes))
    int slot = -1, freeslot = -1;
    /* Hashed hints: v227A keeps four recent candidates per bucket.  A candidate
     * is only a hint; the full identity comparison below remains authoritative. */
    u32 hh = (key_off * 2654435761u) ^ (w * 40503u) ^ (h * 2246822519u) ^ (fmt * 3266489917u) ^ (u32)cube ^ face_stride;
    hh = (hh ^ (hh >> 15)) & 8191u;
    const int assoc = tex_assoc_on();
    int hint = -1;
    const int nway = assoc ? TEX_ASSOC_WAYS : 1;
    for (int way = 0; way < nway; way++) {
        const int i = s_tex_assoc[hh][way] - 1;
        if (i < 0 || i >= VP_TEX_SLOTS) continue;
        const VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && c->key == key_off && c->w == w && c->h == h && c->fmt == fmt &&
            c->cube == cube && c->face_stride == face_stride) {
            hint = i;
            if (way == 0) s_tex_assoc_first++; else s_tex_assoc_extra++;
            break;
        }
    }
    if (hint < 0) s_tex_assoc_scan++;
    { static u32 report_frame = 0;
      if ((u32)(s_tex_frame - report_frame) >= 600u) {
          report_frame = s_tex_frame;
          fprintf(stderr, "[TEX_ASSOC] last 600 frames: first-way %llu, extra-way %llu, full scans %llu (mode %s)%c",
                  (unsigned long long)s_tex_assoc_first, (unsigned long long)s_tex_assoc_extra,
                  (unsigned long long)s_tex_assoc_scan, assoc ? "4-way" : "1-way", 10);
          s_tex_assoc_first = s_tex_assoc_extra = s_tex_assoc_scan = 0;
      } }
    for (int pass = 0; pass < 2 && slot < 0; pass++) {
      int lo = 0, hi = VP_TEX_SLOTS;
      if (pass == 0) { if (hint < 0 || hint >= VP_TEX_SLOTS) continue; lo = hint; hi = hint + 1; }
      for (int i = lo; i < hi; i++) {
        VPTexSlot* c = &s_d3d.vp_tex[i];
        if (c->res && c->key == key_off && c->w == w && c->h == h && c->fmt == fmt
            && c->cube == cube && c->face_stride == face_stride) {
            if (c->used) { tex_assoc_note(hh, i, assoc); return i; }   /* already bound this frame */
            c->lru = s_tex_frame;
            { static int nocache = -1;            /* TEX_NOCACHE=1: always re-upload */
              if (nocache < 0) { const char* e = getenv("TEX_NOCACHE");
                                 nocache = e ? atoi(e) : 0; }
              if (nocache) { slot = i; break; } }
            /* Held from an earlier frame: re-upload only if the guest bytes
             * changed. Most of this scene's textures are static, and converting
             * every one of them every frame was ~70% of the frame's CPU time. */
            u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
            /* 8-bit planes are video frames (inFamous' Bink Y/U/V): sample them
             * 16x denser than the general case, which looks at only 1024 words
             * -- one per 900 bytes of a 1280x720 plane -- and can miss a frame
             * that changed little, showing a stale picture. */
            u32 cs;
            if (!tcs_take(i, c->off, nb, face_stride, basef == 0x81, &cs))   /* v228 TEXCS: precomputed this frame? */
                cs = (basef == 0x81) ? tex_csum_dense(vm_base + c->off, nb)
                                     : tex_csum_faces(vm_base + c->off, nb, face_stride);
            if (_kg_k && kg_hit(w, h, basef, cube, cs)) cs ^= KG_CSUM_KBM;   /* v193: KBM icons are a different image */
            if (cs == c->csum) { c->used = 1; c->lru = s_tex_frame; tex_assoc_note(hh, i, assoc); return i; }
            slot = i; break;                      /* stale: fall through and redo */
        }
      }
    }
    /* v32f: choose the victim for a miss. Old rule (first slot not bound this frame)
     * evicted textures the next frame needs and recreated resources needlessly.
     * Now: an empty slot while under budget; else the least-recently-used unbound slot,
     * preferring one whose resource already has these dimensions (no create) unless it
     * is much more recent than the LRU one. */
    if (slot < 0) {
        int empty = -1, lru = -1, match = -1;
        for (int i = 0; i < VP_TEX_SLOTS; i++) {
            VPTexSlot* c = &s_d3d.vp_tex[i];
            if (c->used) continue;
            if (!c->res) { if (empty < 0) empty = i; continue; }
            if (lru < 0 || c->lru < s_d3d.vp_tex[lru].lru) lru = i;
            if (c->w == w && c->h == h && c->fmt == fmt && c->cube == cube
                && (match < 0 || c->lru < s_d3d.vp_tex[match].lru)) match = i;
        }
        /* v225B TEX_COLD_REUSE: traversal used to prefer an empty cache slot
         * while below TEX_CACHE_MB even when an old, unbound resource already
         * had the exact dimensions/format/cube layout.  A camera turn could
         * consequently call CreatePlacedResource 9--29 times in one frame
         * (a002 traversal: 10--25 ms in texture work) while compatible GPU
         * resources sat idle.  Recycle only a resource unused for N frames;
         * besides avoiding allocation this is safe with RF_EARLY because the
         * previous GPU frame cannot reference a 120-frame-cold slot.  The
         * normal layout/mip check below remains authoritative and recreates on
         * any mismatch, so this changes residency policy, never texture data.
         * TEX_COLD_REUSE=0 or the configurable A/B bit restores the old
         * empty-first rule.  0x800000 is available in the traversal route
         * (ANIM_LOD=1; DRW and V221A_PLACE are not using it). */
        static int cold_n = -1; static u32 cold_bit = 0x800000u;
        if (cold_n < 0) {
            const char* e = getenv("TEX_COLD_REUSE");
            /* Treat the normal boolean spelling as "enabled with the safe
             * default".  Values above one remain an expert override for the
             * cold-age threshold, while zero is the kill switch. */
            cold_n = 120;
            if (e && *e) {
                const int v = atoi(e);
                cold_n = (v == 0) ? 0 : ((v == 1) ? 120 : v);
            }
            if (cold_n < 0) cold_n = 0;
            const char* b = getenv("TEX_COLD_REUSE_BIT");
            if (b && *b) cold_bit = (u32)strtoul(b, NULL, 0);
            fprintf(stderr, "[TEX_COLD_REUSE] v225B compatible resource recycling after %d frames (0=off; ABX 0x%X=off; TEX_COLD_REUSE_BIT)%c",
                    cold_n, cold_bit, 10);
        }
        extern volatile int g_abx;
        const int cold_ok = cold_n > 0 && !(cold_bit && ((u32)g_abx & cold_bit)) && match >= 0 &&
                            (u32)(s_tex_frame - s_d3d.vp_tex[match].lru) >= (u32)cold_n;
        if (cold_ok) { freeslot = match; s_perf_tex_cold++; }
        else if (empty >= 0 && (s_tex_bytes < tex_cache_budget() || lru < 0)) freeslot = empty;
        else if (match >= 0 && (lru < 0 || s_d3d.vp_tex[match].lru <= s_d3d.vp_tex[lru].lru + 120)) freeslot = match;
        else if (lru >= 0) freeslot = lru;
        else freeslot = empty;
    }
    /* TEX_OFF_BIAS=1: sample the image one whole level-0 BELOW the bound offset.
     * Measured on the Rubber Ducky demo: at the bind offset every texture reads
     * as all-zero, while [off - w*h*bpp, off) holds the image (3800-4160 of 4298
     * sampled bytes non-zero, for four different textures at three sizes). So
     * the offset the guest programs points PAST level 0 rather than at it. */
    /* TEX_OFF_BIAS: 1 = one level-0 below the bound offset, 2 = one full MIP
     * CHAIN above it. The measured upload-to-bind deltas on the Rubber Ducky
     * demo are 0x2AAB00 for a 2MB level 0 and 0x555580 for a 4MB one -- both
     * exactly the mipmap pyramid total (L0 * 4/3), which is what the guest
     * reserves per texture. */
    { static int bias = -1;
      if (bias < 0) { const char* e = getenv("TEX_OFF_BIAS"); bias = e ? atoi(e) : 0; }
      if (bias == 1) { u32 sz = w * h * bpp; if (off > sz) off -= sz; }
      else if (bias >= 2) {
          u32 chain = 0;
          for (u32 mw = w, mh = h; mw && mh; mw >>= 1, mh >>= 1) {
              chain += mw * mh * bpp;
              if (mw == 1 && mh == 1) break;
          }
          /* bias 3: only shift when the bound offset really is empty, so a
           * texture that IS bound correctly keeps its own data. Applying the
           * chain unconditionally fixes the mis-bound majority but breaks any
           * correctly-bound minority, which shows up as objects going black. */
          int shift = 1;
          if (bias >= 3) {
              u32 nz = 0, sz = w * h * bpp;
              for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
              shift = (nz == 0);
          }
          if (shift) off += chain;
      } }
    /* TEX_REMAP=1: if the bound offset holds nothing, look the image up by size
     * in the VRAM upload registry (cellGcmSys). For a title whose upload address
     * and SET_TEXTURE_OFFSET disagree this puts the real bytes under the sampler
     * without guessing a delta. */
    { static int remap = -1;
      if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
      if (remap) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i = 0; i < sz && i < 0x20000u; i += 97) if (vm_base[off + i]) nz++;
        if (!nz) {
            extern u32 rsx_find_vram_upload(u32);
            u32 alt = rsx_find_vram_upload(sz);
            if (alt) {
                static int _n = 0;
                if ((_n < 8 && _n++ < 8)) fprintf(stderr, "[TEXREMAP] 0x%08X (%ux%u, %u bytes) -> 0x%08X%c",
                                      off, w, h, sz, alt, 10);
                off = alt;
            }
        } } }
    /* A stale cache entry (slot >= 0) re-uses its own slot; otherwise take a
     * free one. Without this the stale path fell through to freeslot and either
     * duplicated the texture into a second slot or bailed when none was free. */
    if (slot < 0 && freeslot < 0) { extern volatile long g_v244_tex_noslot; g_v244_tex_noslot++; return -1; }   /* out of slots this frame (v244: counted, [TEXNULL]) */
    /* TEX_SRCDBG=<N>: is the guest memory this slot uploads FROM actually
     * populated? An empty source and a broken sampler both render flat. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("TEX_SRCDBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap) { n++;
        u32 nz = 0, tot = 0;
        for (u32 i = 0; i < w * h * bpp && i < 0x40000u; i += 61) { tot++; if (vm_base[off + i]) nz++; }
        /* Several of this title's textures are bound at EXACTLY the end address
         * of an upload, so also sample the block immediately BELOW the bind
         * offset: if that holds the image, the bind is one whole texture high. */
        u32 nzb = 0, totb = 0;
        { u32 sz = w * h * bpp; u32 base = (off > sz) ? off - sz : 0;
          for (u32 i = 0; i < sz && i < 0x40000u; i += 61) { totb++; if (vm_base[base + i]) nzb++; } }
        long nearest = 0; u32 found = 0;
        if (!nz) {   /* empty: where IS the data? scan out from the bind offset */
            for (long d = 0x1000; d <= 0x1000000 && !found; d += 0x1000) {
                for (int s = 0; s < 2 && !found; s++) {
                    long a = (long)off + (s ? -d : d);
                    if (a < 0x1000) continue;
                    u32 c = 0;
                    for (u32 i = 0; i < 0x1000u; i += 61) if (vm_base[a + i]) c++;
                    if (c > 8) { nearest = (s ? -d : d); found = c; }
                }
            }
        }
        fprintf(stderr, "[TEXSRC] off=0x%08X %ux%u fmt=0x%02X bpp=%u nonzero=%u/%u"
                        "%s%+ld (page nonzero=%u)\n",
                off, w, h, fmt, bpp, nz, tot,
                nz ? "" : "  nearest data at ", nz ? 0L : nearest, found);
        fprintf(stderr, "[TEXSRC]    block BELOW bind (off-%u): nonzero=%u/%u%c",
                w * h * bpp, nzb, totb, 10);
        /* Walk DOWN in 4KB steps to the first populated page whose predecessor
         * is empty: that is where this texture's data actually begins, and the
         * delta from the bound offset is the rule we are missing. */
        { u32 step = 0x1000, prev_pop = 0; long start_delta = 0;
          for (long d = 0; d >= -(long)(w * h * bpp) * 2; d -= step) {
              u32 a = (u32)((long)off + d); u32 pop = 0;
              if (a < 0x1000) break;
              for (u32 i = 0; i < step; i += 53) if (vm_base[a + i]) pop++;
              if (!pop && prev_pop) { start_delta = d + step; break; }
              prev_pop = pop;
          }
          fprintf(stderr, "[TEXSRC]    data starts at off%+ld (level0=%u bytes)%c",
                  start_delta, w * h * bpp, 10); } } }
    if (slot < 0) slot = freeslot;
    if (slot >= 0) tex_assoc_note(hh, slot, assoc);
    VPTexSlot* t = &s_d3d.vp_tex[slot];
    /* The upload buffer's rows hold the DECODED image, so they are sized from
     * dst_row_bytes -- w * bpp is the source row, which is narrower for any
     * format rsx_texture_decode unpacks. */
    u32 pitch = (tl.dst_row_bytes + 255) & ~255u;
    int fresh = 0;
    /* v146: mip chain (level 0 stays where it always was in the upload buffer; levels 1.. of every
     * face are appended after all level-0 faces, each 512-byte aligned with its own 256-byte pitch) */
    rsx_tex_level _lv[RSX_MAX_TEXTURE_LEVELS];
    /* v193: keyboard/mouse prompt icons instead of the guest's atlas (see kg_hit) */
    const u8* _kg_src = NULL;
    if (_kg_k && dxt && !cube && basef == 0x88u && (w == 512u || w == 32u) && kg_ready()) {
        const u32 _nb = blkrow * blkrows;
        const int _e = kg_hit(w, h, basef, cube, tex_csum_faces(vm_base + off, _nb, 0)) - 1;
        if (_e >= 0 && s_kg[_e].nb == _nb && memcmp(vm_base + off, s_kg[_e].orig, _nb) == 0) {
            _kg_src = s_kg[_e].kbm;
            static int _n = 0; if (_n < 16) { _n++; fprintf(stderr, "[KBMGLYPH] prompt texture %d (%ux%u) -> keyboard/mouse (tex 0x%08X)%c", _e, w, h, off, 10); }
        }
    }
    { static int _was = -1; if (!_kg_k && _was == 1 && w == 512u && h == 256u && basef == 0x88u) {
        static int _n = 0; if ((_n < 4 && _n++ < 4)) fprintf(stderr, "[KBMGLYPH] prompt icons -> controller%c", 10); }
      if (w == 512u && h == 256u && basef == 0x88u) _was = _kg_k; }
    u32 _nlev = 1;
    if (tex_mips_on() && mips > 1 && !(fmt & 0x20u) && !_kg_src) {
        _nlev = rsx_texture_mip_chain(fmt, w, h, mips, 0, _lv);
        if (dxt && ((w & 3u) || (h & 3u))) _nlev = 1;           /* BC top level must be block-aligned */
        if (!dxt && !tl.swizzled) _nlev = 1;
        for (u32 _l = 1; _l < _nlev; _l++)                       /* every level must decode to the same host format */
            if (_lv[_l].tl.fmt != tl.fmt) { _nlev = _l; break; }
        if (_nlev < 1) _nlev = 1;
        if (_nlev > 1) {   /* the whole chain (every face) must be readable guest memory */
            const u64 _fb = cube ? (u64)rsx_texture_cube_face_stride(fmt, w, h, mips, 0) : 0;
            const u64 _end = (u64)off + _fb * (cube ? 5u : 0u) + _lv[_nlev - 1].offset + _lv[_nlev - 1].tl.face_bytes;
            MEMORY_BASIC_INFORMATION _mbi;
            const u8* _p0 = vm_base + off; const u8* _p1 = vm_base + (_end - 1);
            int _okm = 0;
            if (VirtualQuery(_p1, &_mbi, sizeof _mbi) && _mbi.State == MEM_COMMIT &&
                (const u8*)_mbi.BaseAddress <= _p0) _okm = 1;
            else if (VirtualQuery(_p0, &_mbi, sizeof _mbi) && _mbi.State == MEM_COMMIT &&
                     (const u8*)_mbi.BaseAddress + _mbi.RegionSize > _p1) _okm = 1;
            if (!_okm) {   /* spans regions: check both ends committed */
                MEMORY_BASIC_INFORMATION _m2;
                _okm = VirtualQuery(_p0, &_mbi, sizeof _mbi) && _mbi.State == MEM_COMMIT &&
                       VirtualQuery(_p1, &_m2, sizeof _m2) && _m2.State == MEM_COMMIT &&
                       (((const u8*)_m2.BaseAddress) - ((const u8*)_mbi.BaseAddress + _mbi.RegionSize)) == 0;
            }
            if (!_okm) { _nlev = 1; static int _n = 0; if ((_n < 4 && _n++ < 4)) fprintf(stderr, "[TEX] v146 mip chain of 0x%X not readable -> 1 level%c", off, 10); }
        }
        { static int _n = 0; if (_nlev > 1 && (_n < 6 && _n++ < 6))
            fprintf(stderr, "[TEX] v146 0x%X %ux%u fmt 0x%02X cube %d: %u mip levels (guest %u)%c", off, w, h, fmt, cube, _nlev, mips, 10); }
    }
    const u32 _rows0 = dxt ? blkrows : h;
    const u32 _nf = cube ? 6u : 1u;
    const u64 _mip_base = (((u64)pitch * _rows0 * _nf) + 511u) & ~(u64)511u;
    u64 _mip_off[6][RSX_MAX_TEXTURE_LEVELS]; u32 _mip_pitch[RSX_MAX_TEXTURE_LEVELS];
    u64 _mip_end = _mip_base;
    for (u32 _f = 0; _f < _nf; _f++)
        for (u32 _l = 1; _l < _nlev; _l++) {
            _mip_pitch[_l] = (_lv[_l].tl.dst_row_bytes + 255u) & ~255u;
            _mip_off[_f][_l] = _mip_end;
            _mip_end = (_mip_end + (u64)_mip_pitch[_l] * _lv[_l].tl.rows + 511u) & ~(u64)511u;
        }

    /* Also recreate when the CUBE-ness changes: a slot holding a 2D texture of
     * the same dimensions would otherwise be reused with 1 array slice while the
     * cube path copies 6 subresources and writes a 6x upload buffer -- which
     * crashes. */
    if (t->res && (t->w != w || t->h != h || t->fmt != fmt || t->cube != cube || t->nlev != _nlev)) {
        defer_release(t->res); t->res = NULL;   /* earlier draws in this list may use it */
        defer_release(t->up);  t->up = NULL;
        tp_defer_free(t);                       /* v32f: pool pages back after GPU idle */
    }
    if (!t->res) {
        double _tc0 = perf_on() ? perf_now() : 0.0;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h;
        td.DepthOrArraySize = cube ? 6 : 1;   /* cube = 6 array slices */
        td.MipLevels = (UINT16)_nlev;         /* v146 */
        td.Format = dxfmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        D3D12_HEAP_FLAGS _hf = tex_heap_flags();
        HRESULT _hr = E_FAIL;
        t->pheap = -1; t->ppage = 0; t->pnpages = 0;
        if (tp_on()) {   /* v32f: placed in a pool heap */
            D3D12_RESOURCE_ALLOCATION_INFO ai = {0};
            td.Alignment = 4096;   /* small-resource alignment when the driver allows it */
            RSX_D3D12_ALLOC_INFO(s_d3d.device)(s_d3d.device, &ai, 0, 1, &td);
            if (ai.Alignment != 4096) {
                td.Alignment = 0;
                RSX_D3D12_ALLOC_INFO(s_d3d.device)(s_d3d.device, &ai, 0, 1, &td);
            }
            u32 pg = 0, np = 0;
            int ph = (ai.SizeInBytes && ai.SizeInBytes != ~0ull)
                   ? tp_alloc(ai.SizeInBytes, ai.Alignment, &pg, &np) : -1;
            if (ph >= 0) {
                _hr = s_d3d.device->lpVtbl->CreatePlacedResource(s_d3d.device, s_tp_heap[ph],
                        (u64)pg * TP_PAGE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                        &IID_ID3D12Resource, (void**)&t->res);
                if (SUCCEEDED(_hr)) { t->pheap = ph; t->ppage = pg; t->pnpages = np; s_tex_bytes += (u64)np * TP_PAGE; }
                else { tp_mark(ph, pg, np, 0); t->res = NULL;
                       static int _pw = 0; if ((_pw < 4 && _pw++ < 4)) fprintf(stderr, "[TEX] CreatePlacedResource failed hr=0x%08X %ux%u fmt=%u%c", (unsigned)_hr, w, h, (unsigned)dxfmt, 10); }
            }
            td.Alignment = 0;
        }
        if (FAILED(_hr)) {
          _hr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, _hf, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&t->res);
          if (SUCCEEDED(_hr)) { t->pnpages = (u32)((_mip_end + TP_PAGE - 1) / TP_PAGE); s_tex_bytes += (u64)t->pnpages * TP_PAGE; }
        }
        if (FAILED(_hr) && _hf != D3D12_HEAP_FLAG_NONE) {
            tex_heap_flags_off(); _hf = D3D12_HEAP_FLAG_NONE;
            _hr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, _hf, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                &IID_ID3D12Resource, (void**)&t->res);
        }
        if (FAILED(_hr))
            return -1;
        fresh = 1; hz_add(HZ_TEX_NEW, 1);   /* v225A */
        if (perf_on()) { s_perf_tex_create += perf_now() - _tc0; s_perf_tex_new++; }
    } else if (perf_on()) s_perf_tex_reup++;
    /* v32e: stage through the ring when it has room; per-texture buffer otherwise */
    u64 _need = (_nlev > 1) ? _mip_end : (u64)pitch * (dxt ? blkrows : h) * (cube ? 6 : 1);   /* v146 */
    u64 _roff = 0;
    int _use_ring = tex_ring_alloc(_need, &_roff);
    if (!_use_ring && t->up && s_rf_early_frame) { defer_release(t->up); t->up = NULL; }   /* v225A RF_EARLY: GPU(N-1) may still read it */
    if (!_use_ring && !t->up) {
        double _tc0 = perf_on() ? perf_now() : 0.0;
        D3D12_HEAP_FLAGS _hf = tex_heap_flags();
        D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = _need;   /* v146: incl. mip levels */
        bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hu, _hf, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&t->up))) {
            if (fresh) { t->res->lpVtbl->Release(t->res); t->res = NULL; s_srv_gen++;
                         tp_defer_free(t); }   /* never used by the GPU; freed at next idle anyway */
            return -1;
        }
        if (perf_on()) s_perf_tex_create += perf_now() - _tc0;
    }
    if (perf_on()) s_perf_tex_upbytes += _need;
    hz_add(HZ_TEX_BYTES, (long long)_need);   /* v225A */

    void* mapped = NULL; D3D12_RANGE nr = {0,0};
    if (_use_ring) mapped = s_tring_map + _roff;
    else if (FAILED(t->up->lpVtbl->Map(t->up, 0, &nr, &mapped)) || !mapped) return -1;
    { static int _tp=0; if (getenv("RTT_DUMP") && (_tp < 6 && _tp++ < 6)) {
        const u8* sp = vm_base + off;
        fprintf(stderr, "[TEXUP] off=0x%X fmt=0x%X row0:", off, fmt);
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[_b]);
        fprintf(stderr, "  row240:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+_b]);
        fprintf(stderr, "  mid:");
        for (int _b=0;_b<8;_b++) fprintf(stderr, " %02X", sp[240*w*bpp+320*bpp+_b]);
        fprintf(stderr, "%s", "\n");
    } }
    /* Swizzled (LN bit 0x20 clear): texels live in Morton/Z-order, not rows.
     * Uploading them as linear rows produced the diagonal-stripe garbage on
     * LBP's loading screen (every texture there is 0x85 swizzled). POT dims
     * only -- the hardware requires that for swizzled textures anyway. */
    int swz = tl.swizzled;
    /* Cube textures store their 6 faces consecutively. Convert each into its own
     * slice of the upload buffer; the conversion below is unchanged and simply
     * runs once per face with off/mapped rebased. */
    const u32 _face_rows  = dxt ? blkrows : h;
    /* The face stride is NOT one mip-0 image: RSX stores each cube face as its
     * own complete mip pyramid, so face f starts a whole pyramid (128-byte
     * aligned) in. Assuming mip-0-sized strides made face 1 land inside face 0's
     * mip chain -- which is exactly what the dumps showed, every face after the
     * first a progressively smaller copy of the first. */
    /* Use the shared SOURCE layout. Compressed bpp above is deliberately 1
     * for upload arithmetic; using it to select block size made BC2/BC3
     * face strides half-sized and sampled lower mips as adjacent sky faces. */
    u32 _face_bytes = cube ? face_stride : tl.face_bytes;
    const u32 _nfaces     = cube ? 6u : 1u;
    const u32 _off0 = off; u8* const _map0 = (u8*)mapped;
    for (u32 _f = 0; _f < _nfaces; _f++) {
    off    = _off0 + _f * _face_bytes;
    if (cube && getenv("CUBEDBG")) { u32 _s=0; const u8* _p = vm_base + off;
        for (u32 _i = 0; _i < _face_bytes; _i += 97) _s = _s*131 + _p[_i];
        fprintf(stderr, "[CUBEFACE] f=%u off=0x%X bytes=%u bpp=%u swz=%d csum=%08X%c",
                _f, off, _face_bytes, bpp, swz, _s, 10); }
    mapped = _map0 + (u64)_f * pitch * _face_rows;
    /* Pixel conversion -- deswizzle, channel order, block copy -- is RSX
     * semantics and lives in rsx_texture_layout.c, so a second backend gets
     * it by calling this rather than porting the loops again. */
    tex_decode_d(mapped, pitch, _kg_src ? _kg_src : vm_base + off, w, h, &tl,
                       rsx_texture_argb_is_rgba());   /* v235D TEX_FAST_DECODE */

    /* The diagnostics below only apply to the linear 8-bit case (Bink video
     * planes); they used to sit inside that branch of the conversion. */
    if (!tl.compressed && tl.bytes_per_texel == 1 && !tl.swizzled) {
        /* MOVIE_FIND=1: when a 640x360 movie plane uploads ZERO, scan a wide
         * guest window for the REAL frame (a 640-wide region with content) so
         * we learn where the video decode actually wrote vs where the texture
         * points -- ends the frame-buffer-address guessing. */
        if (getenv("MOVIE_FIND") && w == 640 && h == 360) {
            extern uint8_t* vm_base;
            const u8* here = vm_base + off;
            u32 hz = 0; for (u32 i=0;i<w*h;i+=137) if (here[i]) { hz=1; break; }
            static int _mf = 0;
            if (!hz && (_mf < 3 && _mf++ < 3)) {
                fprintf(stderr, "[movie-find] bound plane ea=0x%X is ZERO; full scan...\n", off);
                /* Scan FULL VRAM + main heap in 0x8000 steps for a 640x360
                 * content block -- NO early cap (the previous found<6 stopped in
                 * the low-VRAM display buffers before ever reaching the movie
                 * region ~0x40E80000). Report the near-plane hits explicitly so
                 * ring-desync (frame present at a DIFFERENT slot) is separable
                 * from decode-never-ran (region entirely zero). */
                struct { u32 lo, hi; const char* tag; } rng[] = {
                    {0x40000000u, 0x41000000u, "VRAM"}, {0x00100000u, 0x10000000u, "MAIN"},
                    {0x48000000u, 0x4A000000u, "BINK"} };  /* Bink working set (handle ~0x4849E760, SPU I/O 0x4847/0x4945) */
                u32 plane_lo = (off > 0x400000u) ? off - 0x400000u : 0;
                u32 plane_hi = off + 0x400000u;
                int found = 0, nearc = 0;
                for (int r = 0; r < (int)(sizeof(rng)/sizeof(rng[0])); r++) {
                    u32 best_a = 0, best_nz = 0; u32 best_span = 0;
                    for (u32 a = rng[r].lo; a + w*h < rng[r].hi; a += 0x8000) {
                        const u8* p = vm_base + a;
                        u32 nz = 0; u8 mn2 = 255, mx2 = 0;
                        for (u32 i = 0; i < w*h; i += 257) { u8 v = p[i]; if (v) nz++; if (v<mn2) mn2=v; if (v>mx2) mx2=v; }
                        u32 span = mx2 - mn2;
                        if (nz > best_nz) { best_nz = nz; best_a = a; best_span = span; }
                        if (nz > 400 && span > 60) {  /* looks like image content */
                            int isnear = (a >= plane_lo && a <= plane_hi);
                            if (isnear) nearc++;
                            if (found < 40)
                                fprintf(stderr, "[movie-find]   CONTENT @0x%08X (%s) nz=%u span=%u%s\n",
                                        a, rng[r].tag, nz, span, isnear ? " <== NEAR bound plane" : "");
                            found++;
                        }
                    }
                    /* Always report the densest block in this range even below the
                     * content threshold -- the MM intro fades in from black, so the
                     * real first frames are low-span and would otherwise be invisible. */
                    fprintf(stderr, "[movie-find]   %s best @0x%08X nz=%u span=%u%s\n",
                            rng[r].tag, best_a, best_nz, best_span,
                            (best_a >= plane_lo && best_a <= plane_hi) ? " <== NEAR bound plane" : "");
                    /* Dump the densest block as a 640x360 grayscale BMP so the
                     * "is this the decoded movie frame" question is answered by
                     * eye. One dump per range per process. */
                    if (best_nz > 400 && best_span > 40) {
                        static int _bd[4] = {0,0,0,0};
                        if (r < 4 && !_bd[r]) { _bd[r] = 1;
                            const u8* sp = vm_base + best_a;
                            char pn[128];
                            snprintf(pn, sizeof(pn), "binkscan_%s_%08X.bmp", rng[r].tag, best_a);
                            FILE* f = fopen(pn, "wb");
                            if (f) {
                                u32 rowb = w*3, rowp = (rowb+3)&~3u, fsz = 54 + rowp*h;
                                u8 hd[54] = {'B','M'};
                                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                                hd[10]=54; hd[14]=40;
                                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                                hd[26]=1; hd[28]=24;
                                fwrite(hd,1,54,f);
                                for (int y=(int)h-1; y>=0; y--) {
                                    const u8* srow = sp + (u64)y*w;
                                    for (u32 x=0;x<w;x++){ u8 px[3]; px[0]=px[1]=px[2]=srow[x]; fwrite(px,1,3,f); }
                                    { u8 z[3]={0,0,0}; fwrite(z,1,rowp-rowb,f); }
                                }
                                fclose(f);
                                fprintf(stderr, "[movie-find]   dumped %s\n", pn);
                            }
                        }
                    }
                }
                fprintf(stderr, "[movie-find]   total=%d near-plane=%d (window 0x%X..0x%X)\n",
                        found, nearc, plane_lo, plane_hi);
                if (!found) fprintf(stderr, "[movie-find]   no 640x360 content block (>thresh) found anywhere\n");
            }
        }
        /* TEX_SAVE=1: also dump wide B8 uploads (Bink video planes are B8 --
         * the Y plane IS the movie frame in grayscale) + a content stat, so
         * "is the decoder producing pixels" is answerable by looking at a BMP. */
        if (getenv("TEX_SAVE") && w >= 256) {
            static int _bs = 0;
            const u8* sp = vm_base + off;
            u32 nz = 0; u8 mn = 255, mx = 0;
            for (u32 i = 0; i < w * h; i += 17) {
                u8 v = sp[i]; if (v) nz++; if (v < mn) mn = v; if (v > mx) mx = v;
            }
            fprintf(stderr, "[TEXB8] off=0x%X %ux%u nz=%u/%u min=%u max=%u\n",
                    off, w, h, nz, (w * h) / 17, mn, mx);
            if (_bs < 8 && mx > mn) { _bs++;
                char pn[128];
                snprintf(pn, sizeof(pn), "texb8_%08X_%ux%u_%d.bmp", off, w, h, _bs);
                FILE* f = fopen(pn, "wb");
                if (f) {
                    u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                    u32 fsz = 54 + rowp * h;
                    u8 hd[54] = {'B','M'};
                    hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                    hd[10]=54; hd[14]=40;
                    hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                    hd[26]=1; hd[28]=24;
                    fwrite(hd, 1, 54, f);
                    for (int y = (int)h - 1; y >= 0; y--) {
                        const u8* srow = sp + (u64)y * w;
                        for (u32 x = 0; x < w; x++) {
                            u8 px[3]; px[0]=px[1]=px[2]=srow[x];
                            fwrite(px, 1, 3, f);
                        }
                        { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                    }
                    fclose(f);
                    fprintf(stderr, "[TEX_SAVE] %s\n", pn);
                }
            }
        }
    }
    /* TEX_SAVE=1: dump the first few converted ARGB uploads as BMPs (rgb +
     * alpha channel separately) -- ground-truth for "is the guest texture
     * wrong or is the sampling wrong" questions (wave's hue palette). */
    if (argb && _f + 1 == _nfaces && getenv("TEX_SAVE")) { static int _ts = 0; if (_ts < 8) { _ts++;
        /* For a cube, write every face: the point of the dump is to check the
         * assumed 6-face layout against the game's cubemap art. */
        for (u32 _face = 0; _face < _nfaces; _face++)
        for (int pass = 0; pass < 2; pass++) {
            char pn[128];
            if (_nfaces > 1)
                snprintf(pn, sizeof(pn), "cube_%08X_%ux%u_f%u_%s.bmp", _off0, w, h,
                         _face, pass ? "a" : "rgb");
            else
                snprintf(pn, sizeof(pn), "tex_%08X_%ux%u_%s.bmp", off, w, h,
                     pass ? "a" : "rgb");
            FILE* f = fopen(pn, "wb");
            if (f) {
                u32 rowb = w * 3, rowp = (rowb + 3) & ~3u;
                u32 fsz = 54 + rowp * h;
                u8 hd[54] = {'B','M'};
                hd[2]=(u8)fsz; hd[3]=(u8)(fsz>>8); hd[4]=(u8)(fsz>>16); hd[5]=(u8)(fsz>>24);
                hd[10]=54; hd[14]=40;
                hd[18]=(u8)w; hd[19]=(u8)(w>>8); hd[22]=(u8)h; hd[23]=(u8)(h>>8);
                hd[26]=1; hd[28]=24;
                fwrite(hd, 1, 54, f);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = _map0 + (u64)_face * pitch * _face_rows
                                     + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        u8 px[3];
                        if (pass) { px[0]=px[1]=px[2]=srow[x*4+3]; }
                        else { px[0]=srow[x*4+2]; px[1]=srow[x*4+1]; px[2]=srow[x*4+0]; }
                        fwrite(px, 1, 3, f);
                    }
                    { u8 z[3] = {0,0,0}; fwrite(z, 1, rowp - rowb, f); }
                }
                fclose(f);
                fprintf(stderr, "[TEX_SAVE] %s\n", pn);
            }
        }
    } }
    /* TEX_AVGDBG=1: one line per distinct bound texture offset with its average
     * source colour. duck.tga is overwhelmingly yellow (avg ~243,191,23), so this
     * says whether the duck's image is bound at all -- and if it is, the duck is
     * being drawn somewhere and the question is where, not whether. */
    { static int avgd = -1;
      if (avgd < 0) { const char* e = getenv("TEX_AVGDBG"); avgd = e ? atoi(e) : 0; }
      if (avgd && !dxt && bpp == 4) {
        static u32 seen[64]; static int nseen = 0;
        int known = 0;
        for (int i2 = 0; i2 < nseen; i2++) if (seen[i2] == off) { known = 1; break; }
        if (!known && nseen < 64) {
            seen[nseen++] = off;
            /* Report the source as R,G,B,A -- the layout TEX_RGBA uploads (PSGL
             * writes GL_RGBA/UNSIGNED_INT_8_8_8_8, so bytes land R,G,B,A). Alpha
             * matters as much as colour here: a blended surface whose alpha is a
             * flat 255 renders opaque no matter how correct the blend state is. */
            u64 sr = 0, sg = 0, sb = 0, sa = 0; u32 cnt = 0, amin = 255, amax = 0;
            for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
                const u8* q = vm_base + off + i2;
                sr += q[0]; sg += q[1]; sb += q[2]; sa += q[3]; cnt++;
                if (q[3] < amin) amin = q[3];
                if (q[3] > amax) amax = q[3];
            }
            if (cnt) fprintf(stderr, "[TEXAVG] raw=0x%08X %4ux%-4u fmt=0x%02X"
                                     " avgRGBA=(%3u,%3u,%3u,%3u) alpha[%u..%u]%c",
                             key_off, w, h, fmt, (u32)(sr/cnt), (u32)(sg/cnt),
                             (u32)(sb/cnt), (u32)(sa/cnt), amin, amax, 10);
        }
      } }
    /* Content-identify the duck's texture so filters can say "duck". */
    if (!dxt && bpp == 4 && w == 512 && h == 512 && s_duck_raw != key_off) {
        u64 ar = 0, ag = 0, ab = 0; u32 n = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 211) {
            const u8* q = vm_base + off + i2;
            ar += q[0]; ag += q[1]; ab += q[2]; n++;
        }
        if (n && ar/n > 200 && ag/n > 140 && ab/n < 90) {
            s_duck_off = key_off;
            { static u32 last = 0; if (last != key_off) { last = key_off;
                fprintf(stderr, "[DUCKTEX] resolved=0x%08X identified by content%c", key_off, 10); } }
        }
    }
    /* TEX_HILITE=1: paint the duck's texture pure red. duck.tga is overwhelmingly
     * yellow (avg 243,191,23); with the R,G,B,A source order that is bytes
     * s[0] high, s[1] mid, s[2] low. Flooding it with an unmistakable colour
     * answers "where on screen is the duck drawn" directly, which neither the
     * vertex data nor a pixel count can. Diagnostic only. */
    { static int hil = -1;
      if (hil < 0) { const char* e = getenv("TEX_HILITE"); hil = e ? atoi(e) : 0; }
      if (hil && !dxt && bpp == 4 && w == 512 && h == 512) {
        u64 sr = 0, sg = 0, sb = 0; u32 cnt = 0;
        for (u32 i2 = 0; i2 + 3 < w * h * 4u; i2 += 4 * 37) {
            const u8* q = vm_base + off + i2;
            sr += q[0]; sg += q[1]; sb += q[2]; cnt++;
        }
        if (cnt) {
            u32 ar = (u32)(sr/cnt), ag = (u32)(sg/cnt), ab = (u32)(sb/cnt);
            if (ar > 200 && ag > 140 && ab < 90) {
                for (u32 y = 0; y < h; y++) {
                    u8* drow = (u8*)mapped + (u64)y * pitch;
                    for (u32 x = 0; x < w; x++) {
                        drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                        drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                    }
                }
                { static int _n = 0; if ((_n < 4 && _n++ < 4))
                    fprintf(stderr, "[TEXHILITE] duck texture off=0x%08X avg=(%u,%u,%u) -> red%c",
                            off, ar, ag, ab, 10); }
            }
        }
      } }
    /* TEX_MARKEMPTY=1: paint any texture whose guest source is entirely zero a
     * flat magenta instead of uploading the empty bytes. An unresolved texture
     * and a legitimately black surface are indistinguishable on screen, so this
     * says WHICH geometry is missing its image -- diagnostic only. */
    { static int mark = -1;
      if (mark < 0) { const char* e = getenv("TEX_MARKEMPTY"); mark = e ? atoi(e) : 0; }
      if (mark && !dxt) {
        u32 nz = 0, sz = w * h * bpp;
        for (u32 i2 = 0; i2 < sz && i2 < 0x40000u; i2 += 61) if (vm_base[off + i2]) nz++;
        if (!nz) {
            for (u32 y = 0; y < h; y++) {
                u8* drow = (u8*)mapped + (u64)y * pitch;
                for (u32 x = 0; x < w; x++) {
                    /* Pure RED. Magenta collided with this title's wall tint,
                     * so the marker was indistinguishable from real geometry. */
                    drow[x*4+0] = 0xFF; drow[x*4+1] = 0x00;
                    drow[x*4+2] = 0x00; drow[x*4+3] = 0xFF;
                }
            }
            { static int _n = 0; if ((_n < 8 && _n++ < 8))
                fprintf(stderr, "[TEXEMPTY] off=0x%08X %ux%u fmt=0x%02X -> magenta%c",
                        off, w, h, fmt, 10); }
        }
      } }
    }   /* end per-face conversion */
    off = _off0; mapped = _map0;
    for (u32 _f = 0; _f < _nf && _nlev > 1; _f++)          /* v146: mip levels 1.. */
        for (u32 _l = 1; _l < _nlev; _l++)
            tex_decode_d(_map0 + _mip_off[_f][_l], _mip_pitch[_l],   /* v235D TEX_FAST_DECODE */
                               vm_base + _off0 + (u64)_f * _face_bytes + _lv[_l].offset,
                               _lv[_l].w, _lv[_l].h, &_lv[_l].tl, rsx_texture_argb_is_rgba());
    if (!_use_ring) t->up->lpVtbl->Unmap(t->up, 0, NULL);

    if (!fresh) {   /* reused resource: PSR -> COPY_DEST first */
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
    dst.pResource = t->res; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = _use_ring ? s_tring : t->up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format   = dxfmt;
    src.PlacedFootprint.Footprint.Width    = w;
    src.PlacedFootprint.Footprint.Height   = h;
    src.PlacedFootprint.Footprint.Depth    = 1;
    src.PlacedFootprint.Footprint.RowPitch = pitch;
    for (u32 _f = 0; _f < _nfaces; _f++) {
        dst.SubresourceIndex        = _f * _nlev;      /* cube face = array slice (v146: x levels) */
        src.PlacedFootprint.Offset  = (_use_ring ? _roff : 0) + (u64)_f * pitch * _face_rows;
        src.PlacedFootprint.Footprint.Width    = w;
        src.PlacedFootprint.Footprint.Height   = h;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        for (u32 _l = 1; _l < _nlev; _l++) {   /* v146 */
            dst.SubresourceIndex = _f * _nlev + _l;
            src.PlacedFootprint.Offset = (_use_ring ? _roff : 0) + _mip_off[_f][_l];
            /* footprint dims as D3D12 itself reports them for this subresource (BC: whole blocks) */
            { D3D12_RESOURCE_DESC _rd; memset(&_rd, 0, sizeof _rd);
              _rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; _rd.Width = w; _rd.Height = h;
              _rd.DepthOrArraySize = (UINT16)_nf; _rd.MipLevels = (UINT16)_nlev; _rd.Format = dxfmt;
              _rd.SampleDesc.Count = 1; _rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
              D3D12_PLACED_SUBRESOURCE_FOOTPRINT _fp; memset(&_fp, 0, sizeof _fp);
              s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &_rd, _f * _nlev + _l, 1, 0, &_fp, NULL, NULL, NULL);
              src.PlacedFootprint.Footprint.Width  = _fp.Footprint.Width  ? _fp.Footprint.Width  : _lv[_l].w;
              src.PlacedFootprint.Footprint.Height = _fp.Footprint.Height ? _fp.Footprint.Height : _lv[_l].h; }
            src.PlacedFootprint.Footprint.RowPitch = _mip_pitch[_l];
            s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        }
    }
    {
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = t->res;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    /* SRV at heap slot 1+slot */
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = dxfmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    /* RSX B8 replicates the byte into all four channels (dbgfont's FP reads
     * coverage from .w); DXGI R8 defaults to (r,0,0,1), so swizzle (R,R,R,R)
     * = encoded 0x1000 (component 0 in all lanes + always-set bit). ARGB8
     * keeps the identity mapping. */
    sv.Shader4ComponentMapping = (basef == 0x81) ? 0x1000 : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = _nlev;   /* v146 */
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(VPTEX_SRV_BASE + slot) * s_d3d.srv_inc;
    if (cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = _nlev;   /* v146 */
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, t->res, &sv, sh);

    t->nlev = _nlev;   /* v146 */
    t->off = off; t->key = key_off; t->w = w; t->h = h; t->fmt = fmt;
    t->cube = cube; t->face_stride = face_stride; t->used = 1; t->lru = s_tex_frame;
    { u32 nb = dxt ? (blkrow * blkrows) : (w * h * bpp);
      t->csum = ((fmt & 0x9F) == 0x81) ? tex_csum_dense(vm_base + off, nb)
                                       : tex_csum_faces(vm_base + off, nb, face_stride);
      if (_kg_k && kg_hit(w, h, basef, cube, t->csum)) t->csum ^= KG_CSUM_KBM; }   /* v193: same rule as the bind check */
    #undef TEX_CSUM
    return slot;
}
/* v225A: texture lookup + decode + upload time on the render thread */
static int vp_upload_tex_slot(u32 off, u32 w, u32 h, u32 fmt, int cube, u32 mips)
{
    if (__builtin_expect(!ps3_telem_enabled(), 1))
        return vp_upload_tex_slot_impl(off, w, h, fmt, cube, mips);
    const unsigned long long t0 = hz_tsc();
    const int r = vp_upload_tex_slot_impl(off, w, h, fmt, cube, mips);
    hz_add(HZ_TEX_TSC, (long long)(hz_tsc() - t0));
    return r;
}

/* ---------------------------------------------------------------------------
 * Render-to-texture: offscreen RT pool.
 *
 * Draws/clears targeting a non-display surface (SET_SURFACE_COLOR_xOFFSET
 * not registered via cellGcmSetDisplayBuffer) render into a pooled RGBA8
 * texture keyed by the surface's resolved vm offset; a later draw binding a
 * texture at that offset samples the RT directly instead of guest memory.
 * RGBA8 for every RT (even half-float guest surfaces) keeps the existing
 * PSO/RTV formats -- values clamp to [0,1], which demosaic's RGB data fits.
 * -----------------------------------------------------------------------*/

/* Snapshot the VP constant bank + viewport epilogue for one draw into its
 * vp_cb slot. Runs at record time so every draw keeps the constants that
 * were live when the guest issued it. */
static int zflat_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("ZFLAT"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
/* v41 LOD_SKY_ONLY (default 1; 0 = off). inFamous draws a coarse copy of the whole world
 * (terrain + far skyline: FP = 2 instructions, colour = tex0 * const, alpha 0, blend off, depth
 * LEQUAL + write) into the light buffer after the lights. Its coarse surfaces sit in front of the
 * detailed scene over most of the screen (capture: 62% of pixels changed, depth up to 1.3e5
 * units nearer), which flattened the frame. On hardware it only fills where the scene drew
 * nothing; we don't know the game's mechanism yet (no stencil / clip / A2C / occlusion query
 * involved), so restrict it to pixels whose STORED depth is still the far plane, via the depth
 * bounds test [0.99999, 1]. Classified once per FP snapshot hash. Log [LODSKY]. */
/* v147 LOD_SKY_ONLY=2 (new default; 1 = the v41 depth-bounds version, 0 = off). The v41 version
 * keeps a coarse fragment only where the STORED depth is >= 0.99999. But the coarse world's own depth
 * is 0.9975..0.9999 (measured: rsxcap/old_play40 frame_100, ops 4370-5790, 149941 sky pixels, max
 * 0.99991) -- so as soon as one coarse mesh has been drawn at a pixel, every later coarse mesh there
 * fails the bounds test, even when it is IN FRONT. Coarse visibility was decided by draw order, not
 * depth: distant stand-in buildings cut each other off / looked see-through, and changed with the
 * game's chunk order every time the camera moved (the flicker on far-away buildings).
 * Mode 2 keeps the same region ("only where the scene drew nothing") but makes it a stencil MASK
 * computed once before the coarse run: a full-screen pass sets stencil bit 0x80 where the stored depth
 * is >= LOD_SKY_Z (default 0.99999) and clears it elsewhere (lsk_mark); the coarse draws then test
 * "bit 0x80 set" plus their normal LEQUAL depth test, so coarse meshes occlude each other correctly.
 * The guest never writes or reads stencil bit 7 (write masks 0x07/0x00, read masks 0x04/ALWAYS).
 * Coarse draws that already use guest stencil fall back to mode 1. Log [LODSKY] v147. */
#define LSK_BIT 0x80u
static volatile int g_lsk_f6_off = 0;   /* v152: F6 toggles the coarse-world restriction live (car pop-in test) */
static int lod_sky_on(void)
{
    /* v159 LOD_SKY_START_OFF=1: start with the coarse-world restriction OFF (F6 turns it back on).
     * cap159: the coarse city no longer paints over the detail buildings (F6 test), while the restriction
     * punched holes into coarse stand-ins wherever anything is behind them (see-through bridge). */
    { static int st = -1;
      if (st < 0) { const char* e = getenv("LOD_SKY_START_OFF"); st = (e && *e == '1') ? 1 : 0;
                    if (st) { g_lsk_f6_off = 1;
                        fprintf(stderr, "[LODSKY] v159 restriction starts OFF (LOD_SKY_START_OFF=1); F6 turns it on%c", 10); } } }
    if (g_lsk_f6_off) return 0;
    static int v = -1;
    if (v < 0) { const char* e = getenv("LOD_SKY_ONLY"); v = (e && *e) ? atoi(e) : 2; if (v < 0 || v > 2) v = 2;
                 fprintf(stderr, "[LODSKY] v147 mode %d (LOD_SKY_ONLY: 0 off, 1 v41 depth bounds, 2 sky-region stencil mask)%c", v, 10); }
    return v;
}
static float lsk_sky_z(void)
{
    static float z = -1.0f;
    if (z < 0.0f) { const char* e = getenv("LOD_SKY_Z"); z = e ? (float)atof(e) : 0.99999f;
                    if (!(z > 0.0f && z <= 1.0f)) z = 0.99999f; }
    return z;
}
static volatile int s_lsk_broken;   /* mark pipeline unavailable -> mode 1 from then on */
static u32 s_lsk_stk;   /* s_stk index of "stencil & 0x80 == 0x80" (0 = not interned yet) */
static u32 lsk_stencil_idx(void)
{
    if (s_lsk_stk) return s_lsk_stk;
    StencilKey k; memset(&k, 0, sizeof k);
    k.en = 1; k.rmask = (u8)LSK_BIT; k.wmask = 0;
    for (int f = 0; f < 2; f++) {
        k.f[f] = D3D12_COMPARISON_FUNC_EQUAL;
        k.fail[f] = k.zfail[f] = k.zpass[f] = D3D12_STENCIL_OP_KEEP;
    }
    for (int i = 1; i < s_stk_n; i++) if (memcmp(&s_stk[i], &k, sizeof k) == 0) { s_lsk_stk = (u32)i; return s_lsk_stk; }
    if (s_stk_n >= 256) return 0;
    s_stk[s_stk_n] = k;
    s_lsk_stk = (u32)s_stk_n++;
    fprintf(stderr, "[LODSKY] v147 stencil state %u: coarse world only where bit 0x80 (sky region) is set%c", s_lsk_stk, 10);
    return s_lsk_stk;
}
static int lsk_is_coarse(const D3D12DrawRecord* dr)
{
    return s_lsk_stk && dr->sref == LSK_BIT && ((dr->cull >> 16) & 0xFFu) == s_lsk_stk && !((dr->cull >> 24) & 1u);
}
/* v160 LOD_DIST_SCALE (default 2.0; 1 = off): push the game's per-object LOD fade ranges out (PC enhancement).
 * inFamous stores a 0x60-byte cull/LOD record per drawable (memdump RE):
 *   +0x10 bbox min xyz, 1.0 | +0x20 bbox max xyz, 1.0 | +0x3C fade near f32 | +0x40 fade far f32 | +0x54 0xFFFF01xx
 * (2117 records in the city heap; the "ghost" building = 35 -> 100). Each frame 4 MB of 0x40000000-0x4B000000 is
 * scanned; matching records get near/far x scale, the originals are remembered (no double scaling; a new record at a
 * reused address is re-detected because its far no longer equals the value we wrote). F5 = live on/off. */
typedef struct { u32 ea; float onear, ofar, sfar; } LodRec;
#define LOD_HT (1u << 17)
static LodRec* s_lod_ht = NULL;
static u32 s_lod_n = 0, s_lod_scan = 0x40000000u;
static float s_lod_k = -1.0f;
static int s_lod_on = 1;
static u32 lod_be(u32 ea) { extern uint8_t* vm_base; return __builtin_bswap32(*(volatile u32*)(vm_base + ea)); }
static void lod_wf(u32 ea, float f) { extern uint8_t* vm_base; u32 u; memcpy(&u, &f, 4); *(volatile u32*)(vm_base + ea) = __builtin_bswap32(u); }
static float lod_f(u32 ea) { u32 u = lod_be(ea); float f; memcpy(&f, &u, 4); return f; }
static int lod_is_rec(u32 a)
{
    if ((lod_be(a + 0x54) & 0xFFFFFF00u) != 0xFFFF0100u) return 0;
    if (lod_be(a + 0x1C) != 0x3F800000u || lod_be(a + 0x2C) != 0x3F800000u) return 0;
    for (int i = 0; i < 3; i++) {
        float mn = lod_f(a + 0x10 + 4 * i), mx = lod_f(a + 0x20 + 4 * i);
        if (!(mx > mn) || !(mx - mn < 5000.0f) || !(mn > -1e5f && mx < 1e5f)) return 0;
    }
    float nr = lod_f(a + 0x3C), fa = lod_f(a + 0x40);
    return (nr >= 0.0f && fa > nr && fa <= 40000.0f);
}
static LodRec* lod_slot(u32 a)
{
    u32 h = ((a >> 4) * 2654435761u) & (LOD_HT - 1);
    for (u32 i = 0; i < LOD_HT; i++) { LodRec* r = &s_lod_ht[(h + i) & (LOD_HT - 1)]; if (!r->ea || r->ea == a) return r; }
    return NULL;
}
/* v231 LOD_CLASS_SCALE (default 1 = off): the game's DRAW-DISTANCE table. EBOOT .data 0x0078AD80 (8 BE floats)
 * {0, 400, 750, 1500, 3500, 6000, 12000, 1e6} = per-class LOD switch distances (cm). Every frame func_00251490 copies
 * entries 1..7 x camera LOD scale (cam+0x2A8) into the world-cull SPU job's params (+0x48); the job
 * (jobmod_50f371dc382d, 0x61A8/0x6358) draws a model's DETAIL program while dist^2 < thr^2, else its coarse stand-in.
 * func_000C93D8 / 00112160 / 00114F60 read the same table for dynamic objects (cars / NPCs). Pop-in is original-game
 * behaviour; scaling entries 1..6 (not 0, not the 1e6 "never") pushes every detail swap out by k. F5 = live on/off
 * (writes the originals back). Found in v231. */
volatile int g_lodx_f5_off = 0;
/* v235A LOD_CLASS_SCALES (default unset = off): per-class factors for the six table entries (classes 0..5 =
 * 400 750 1500 3500 6000 12000 cm), applied on top of LOD_CLASS_SCALE (use LOD_CLASS_SCALE=1 with it).
 *   LOD_CLASS_SCALES=<set>[|<set>...]   <set> = "f0,f1,f2,f3,f4,f5" (missing entries = 1)
 * Several sets = in-run A/B: the active set rotates every LOD_CLASS_AB_FRAMES world frames (default 0 = follow
 * PS3_AB phases: set = g_ab_phase % nsets). LOD_CLASS_AB_SKIP (default 20) frames after a switch are not counted.
 * [LODXAB] prints FRAMEPCT-format 300-frame windows per set (tools/bench/lodxab.py). LOD_CLASS_SET_CTL=<file>
 * (one digit) forces a set (screenshots). g_lodx_ab_set = active set, read by the job hook (lodx_v233.h) so
 * LOD_JOB_CLASS_SCALES sets follow the same rotation. */
extern volatile int g_lodx_ab_set;   /* defined in spu/jobmod_50f371dc382d_e3D90/lodx_v233.h (spu_recomp.c TU) */
static int hz_cmp_f_lodx(const void* a, const void* b) { const float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
static int s_lcs_n = 0, s_lcs_frames = 0, s_lcs_skip = 20;
static float s_lcs[8][6];
static volatile int s_lcs_set = 0, s_lcs_since = 0;
static void lodx_ab_frame(double frame_ms, double rf_ms, double t_s)
{
    if (s_lcs_n < 2) return;
    static float win[8][300]; static double rsum[8]; static int nw[8];
    const int st = s_lcs_set & 7;
    if (s_lcs_since++ < s_lcs_skip) return;
    win[st][nw[st]++] = (float)frame_ms; rsum[st] += rf_ms;
    if (nw[st] < 300) return;
    float s[300]; memcpy(s, win[st], sizeof s); qsort(s, 300, sizeof s[0], hz_cmp_f_lodx);
    double wall = 0; int n20 = 0, n25 = 0, n33 = 0;
    for (int i = 0; i < 300; i++) { wall += s[i]; n20 += s[i] > 20.0f; n25 += s[i] > 25.0f; n33 += s[i] > 33.4f; }
    fprintf(stderr, "[LODXAB] set %d t=%.1f 300 frames: avg %.2f fps, p50 %.2f ms, p99 %.2f ms, 1%% low %.1f fps, 0.1%% low %.1f fps,"
                    " >20ms %d, >25ms %d, >33ms %d, worst %.1f ms | rcpu %.2f%c",
            st, t_s, 300000.0 / wall, s[149], s[296], 3000.0 / ((double)s[297] + s[298] + s[299]), 1000.0 / s[299],
            n20, n25, n33, s[299], rsum[st] / 300.0, 10);
    nw[st] = 0; rsum[st] = 0;
}
static void lodtab_tick(void)
{
    extern uint8_t* vm_base;
    static float k = -1.0f; static u32 ea = 0; static int state = -1, gave_up = 0;
    static const u32 sig[8] = { 0x00000000u, 0x43C80000u, 0x443B8000u, 0x44BB8000u, 0x455AC000u, 0x45BB8000u, 0x463B8000u, 0x49742400u };
    if (k < 0.0f) {
        const char* e = getenv("LOD_CLASS_SCALE"); k = e ? (float)atof(e) : 1.0f;
        if (!(k >= 0.25f && k <= 20.0f)) k = 1.0f;
        fprintf(stderr, "[LODX] v231 draw-distance table x%.2f (LOD_CLASS_SCALE; 1 = original), F5 = live on/off%c", k, 10);
        { const char* cs = getenv("LOD_CLASS_SCALES");   /* v235A per-class sets */
          if (cs && *cs) {
              const char* p = cs;
              while (*p && s_lcs_n < 8) {
                  for (int i = 0; i < 6; i++) s_lcs[s_lcs_n][i] = 1.0f;
                  int i = 0;
                  while (*p && *p != '|') {
                      char* e; float v = (float)strtod(p, &e);
                      if (e == p) { p++; continue; }
                      if (i < 6 && v >= 0.25f && v <= 20.0f) s_lcs[s_lcs_n][i] = v;
                      i++; p = e; if (*p == ',') p++;
                  }
                  fprintf(stderr, "[LODX] v235A LOD_CLASS_SCALES set %d: %.2f %.2f %.2f %.2f %.2f %.2f (x LOD_CLASS_SCALE %.2f)%c", s_lcs_n,
                          s_lcs[s_lcs_n][0], s_lcs[s_lcs_n][1], s_lcs[s_lcs_n][2], s_lcs[s_lcs_n][3], s_lcs[s_lcs_n][4], s_lcs[s_lcs_n][5], k, 10);
                  s_lcs_n++;
                  if (*p == '|') p++;
              }
              const char* fr = getenv("LOD_CLASS_AB_FRAMES"); if (fr && *fr) s_lcs_frames = atoi(fr);
              const char* sk = getenv("LOD_CLASS_AB_SKIP"); if (sk && *sk) s_lcs_skip = atoi(sk);
              if (s_lcs_n > 1) fprintf(stderr, "[LODX] v235A %d sets, rotate every %d frames (0 = PS3_AB phase), skip %d%c", s_lcs_n, s_lcs_frames, s_lcs_skip, 10);
          } }
    }
    if (s_lcs_n > 0) {   /* v235A: pick the active set */
        extern volatile int g_ab_phase;
        static const char* sctl = (const char*)1; static unsigned sn = 0, tick = 0; static int forced = -1;
        if (sctl == (const char*)1) sctl = getenv("LOD_CLASS_SET_CTL");
        if (sctl && *sctl && (sn++ % 10) == 0) { FILE* f = fopen(sctl, "rb"); if (f) { int c = fgetc(f); fclose(f);
            int nf = (c >= '0' && c <= '9' && c - '0' < s_lcs_n) ? c - '0' : -1;
            if (nf != forced) { forced = nf; fprintf(stderr, "[LODX] set ctl -> %d%c", nf, 10); } } }
        int set = 0;
        if (forced >= 0) set = forced;
        else if (s_lcs_n > 1 && s_lcs_frames > 0) set = (int)((tick++ / (unsigned)s_lcs_frames) % (unsigned)s_lcs_n);
        else if (s_lcs_n > 1) set = (int)((unsigned)g_ab_phase % (unsigned)s_lcs_n);
        if (set != s_lcs_set) { s_lcs_set = set; s_lcs_since = 0; }
        g_lodx_ab_set = set;
    }
    /* v231c: ctl file also drives LOD_JOB_SCALE (job hook), so it is read before the table-scale early-out */
    { static const char* ctl = (const char*)1; static unsigned n = 0;   /* bench A/B: LOD_CLASS_CTL=<file>, '0' = original, '1' = scaled */
      if (ctl == (const char*)1) ctl = getenv("LOD_CLASS_CTL");
      if (ctl && *ctl && (n++ % 10) == 0) { FILE* f = fopen(ctl, "rb"); if (f) { int c = fgetc(f); fclose(f);
          if (c == '0' || c == '1') { int off = (c == '0'); if (off != g_lodx_f5_off) { g_lodx_f5_off = off;
              fprintf(stderr, "[LODX] ctl file -> %s%c", off ? "original" : "scaled", 10); } } } } }
    if ((k == 1.0f && s_lcs_n == 0) || gave_up || !vm_base) return;
    if (!ea) {
        u32 cand = 0x0078AD80u, ok = 1;
        for (int i = 0; i < 8 && ok; i++) ok = lod_be(cand + 4u * i) == sig[i];
        if (!ok) {   /* other EBOOT revision: one scan of the PPU data area */
            cand = 0;
            for (u32 a = 0x00700000u; a < 0x009F0000u && !cand; a += 16) {
                if (lod_be(a + 4) != sig[1] || lod_be(a + 28) != sig[7]) continue;
                u32 m = 1; for (int i = 0; i < 8 && m; i++) m = lod_be(a + 4u * i) == sig[i];
                if (m) cand = a;
            }
        }
        if (!cand) { gave_up = 1; fprintf(stderr, "[LODX] draw-distance table NOT found -- scale inactive%c", 10); return; }
        ea = cand;
        fprintf(stderr, "[LODX] draw-distance table at 0x%08X: 400 750 1500 3500 6000 12000 -> x%.2f%c", ea, k, 10);
    }
    const int want = g_lodx_f5_off ? 0 : 1;
    /* re-assert every frame if someone rewrote it (cheap: 6 words) */
    for (int i = 1; i <= 6; i++) {
        float o; memcpy(&o, &sig[i], 4);
        float v = want ? o * k * (s_lcs_n ? s_lcs[s_lcs_set & 7][i - 1] : 1.0f) : o;
        if (lod_f(ea + 4u * i) != v) lod_wf(ea + 4u * i, v);
    }
    if (want != state) { state = want; fprintf(stderr, "[LODX] draw-distance table %s%c", want ? "SCALED" : "original", 10); }
}
static void lod_scale_tick(void)
{
    extern uint8_t* vm_base;
    if (s_lod_k < 0.0f) {
        const char* e = getenv("LOD_DIST_SCALE"); s_lod_k = e ? (float)atof(e) : 2.0f;
        if (!(s_lod_k >= 1.0f && s_lod_k <= 8.0f)) s_lod_k = 1.0f;
        fprintf(stderr, "[LODSCALE] v160 per-object LOD fade ranges x%.2f (LOD_DIST_SCALE; 1 = off), F5 = live on/off%c", s_lod_k, 10);
        if (s_lod_k > 1.0f) s_lod_ht = (LodRec*)calloc(LOD_HT, sizeof(LodRec));
    }
    if (!s_lod_ht || !vm_base) return;
    { static int f5_was = 0;
      const int f5 = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F5) & 0x8000);
      if (f5 && !f5_was) {
          s_lod_on = !s_lod_on; u32 restored = 0;
          for (u32 i = 0; i < LOD_HT; i++) { LodRec* r = &s_lod_ht[i]; if (!r->ea) continue;
              if (lod_f(r->ea + 0x40) == r->sfar && r->sfar != r->ofar) { lod_wf(r->ea + 0x3C, r->onear); lod_wf(r->ea + 0x40, r->ofar); restored++; }
          }
          memset(s_lod_ht, 0, LOD_HT * sizeof(LodRec)); s_lod_n = 0;   /* switched back on -> rescan from scratch */
          fprintf(stderr, "[LODSCALE] F5 -> %s (%u records known, %u restored)%c", s_lod_on ? "ON" : "OFF", s_lod_n, restored, 10);
      }
      f5_was = f5; }
    if (!s_lod_on) return;
    const u32 lo = 0x40000000u, hi = 0x4B000000u, step = 4u << 20;
    u32 a0 = s_lod_scan, a1 = a0 + step; if (a1 > hi) a1 = hi;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(vm_base + a0, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        u32 rend = (u32)((uintptr_t)mbi.BaseAddress + mbi.RegionSize - (uintptr_t)vm_base);
        if (a1 > rend) a1 = rend;
        for (u32 a = a0; a < a1 && a + 0x60 <= rend; a += 16) {   /* records may straddle the chunk end */
            if ((lod_be(a + 0x54) & 0xFFFFFF00u) != 0xFFFF0100u) continue;   /* cheap reject first */
            if (!lod_is_rec(a)) continue;
            LodRec* r = lod_slot(a); if (!r) continue;
            float nr = lod_f(a + 0x3C), fa = lod_f(a + 0x40);
            if (r->ea == a && fa == r->sfar) continue;                      /* already ours */
            if (!r->ea) s_lod_n++;
            r->ea = a; r->onear = nr; r->ofar = fa;
            float sn = nr * s_lod_k, sf = fa * s_lod_k; if (sf > 40000.0f) sf = 40000.0f;
            r->sfar = sf;
            lod_wf(a + 0x3C, sn); lod_wf(a + 0x40, sf);
        }
    } else a1 = a0 + step;
    s_lod_scan = (a1 >= hi) ? lo : a1;
    { static u32 last = 0; if (s_lod_scan == lo && s_lod_n != last) { last = s_lod_n;
        fprintf(stderr, "[LODSCALE] heap pass done: %u LOD records scaled x%.2f%c", s_lod_n, s_lod_k, 10); } }
}

/* v159 F3: full guest memory dump (+ the F9 capture) for reverse-engineering the game's LOD/streaming state.
 * memdump\\dump_NNN.bin = "PS3MEM01", u64 frame, then records {u32 guest ea, u32 size, bytes} for every
 * committed, readable region of the 4 GB guest window (vm_base). Guest threads keep running while it writes
 * (a ~1 s stall of the render thread only); good enough to locate structures. */
static void ps3_guest_memdump(void)
{
    extern uint8_t* vm_base;
    static int idx = 0;
    if (!vm_base) return;
    CreateDirectoryA("memdump", NULL);
    char path[64]; snprintf(path, sizeof path, "memdump\\dump_%03d.bin", idx++);
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "[MEMDUMP] cannot open %s%c", path, 10); return; }
    static const char magic[8] = {'P','S','3','M','E','M','0','1'};
    fwrite(magic, 1, 8, f);
    u64 frame = (u64)s_d3d.frame_count; fwrite(&frame, 8, 1, f);
    LARGE_INTEGER t0, t1, fq; QueryPerformanceCounter(&t0); QueryPerformanceFrequency(&fq);
    const uintptr_t base = (uintptr_t)vm_base, end = base + 0x100000000ull;
    uintptr_t a = base; u64 total = 0; int nreg = 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
        if (rs < a) rs = a;
        if (re > end) re = end;
        if (re <= rs) break;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (uintptr_t c = rs; c < re; ) {
                u32 sz = (u32)((re - c) > 0x40000000u ? 0x40000000u : (re - c));
                u32 ea = (u32)(c - base);
                fwrite(&ea, 4, 1, f); fwrite(&sz, 4, 1, f); fwrite((void*)c, 1, sz, f);
                total += sz; c += sz;
            }
            nreg++;
        }
        a = re;
    }
    fclose(f);
    QueryPerformanceCounter(&t1);
    fprintf(stderr, "[MEMDUMP] v159 F3: %s = %d regions, %llu MB of guest memory, frame %llu (%.0f ms)%c",
            path, nreg, (unsigned long long)(total >> 20), (unsigned long long)frame,
            (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart, 10);
}

/* v244 LOD_COARSE_LEVEL / LOD_COARSE_BIAS / LOD_COARSE_CTL: depth bias for the coarse-world pass (the fph 98123110 /
 * 9A50E9FB "colour = tex0 * const into the light buffer" draws that lod_sky_apply classifies). rsxcap 2026-10-21:
 * the DETAILED buildings are in the G-buffer, but the coarse stand-ins drawn afterwards
 * pass LEQUAL in front of them (their hulls sit on/just in front of the real facades) and write their flat texture
 * into the light buffer -> see-through grey/black "ghost" buildings. A depth bias pushes only the stand-ins back:
 * they lose where a real building occupies the pixel but still win where they are genuinely in front (the far bridge
 * that the v147 sky-only mask punched holes into). Level 0 = off, 1..3 = LOD_COARSE_BIAS "u1:s1,u2:s2,u3:s3"
 * (D24 units : slope scale). LOD_COARSE_CTL=<file> = a level digit re-read every 30 frames (bench A/B).
 * The level rides in cull bits 30-31, so each level is its own PSO. Log [COARSEZ]. */
static int s_cz_units[4] = { 0, 2048, 8192, 32768 };
static float s_cz_slope[4] = { 0.0f, 1.0f, 2.0f, 4.0f };
/* v244b LOD_COARSE_ZMAX (default 0.9999): the biased stand-ins get viewport MaxDepth = this. D3D12 clamps the biased
 * depth to the viewport range, so a far stand-in can never be pushed behind the sky dome (bench v244cz angle 4: the
 * unclamped levels 2/3 erased the far skyline / bridge statues) while it still loses to a real facade at its pixel. */
static float s_cz_zmax = 0.9999f;
static int coarse_bias_level(void)
{
    static int lvl = -1; static const char* ctl = NULL; static u32 last = 0xFFFFFFFFu;
    if (lvl < 0) {
        const char* e = getenv("LOD_COARSE_LEVEL"); lvl = (e && *e) ? atoi(e) : 0; if (lvl < 0 || lvl > 3) lvl = 0;
        const char* b = getenv("LOD_COARSE_BIAS");
        for (int i = 1; i <= 3 && b && *b; i++) {
            int u = 0; float sl = 0.0f;
            if (sscanf(b, "%d:%f", &u, &sl) >= 1) { s_cz_units[i] = u; s_cz_slope[i] = sl; }
            b = strchr(b, ','); if (b) b++;
        }
        ctl = getenv("LOD_COARSE_CTL"); if (ctl && !*ctl) ctl = NULL;
        { const char* z = getenv("LOD_COARSE_ZMAX"); if (z && *z) { float v = (float)atof(z); if (v > 0.5f && v <= 1.0f) s_cz_zmax = v; } }
        fprintf(stderr, "[COARSEZ] v244 coarse-world depth bias level %d (1: %d/%.1f, 2: %d/%.1f, 3: %d/%.1f D24 units/slope), zmax %.6f%s%c",
                lvl, s_cz_units[1], s_cz_slope[1], s_cz_units[2], s_cz_slope[2], s_cz_units[3], s_cz_slope[3], s_cz_zmax,
                ctl ? " -- LOD_COARSE_CTL file overrides" : "", 10);
    }
    if (ctl && s_frame_no / 30u != last) {
        last = s_frame_no / 30u;
        FILE* f = fopen(ctl, "rb");
        if (f) { int c = fgetc(f); fclose(f);
                 if (c >= '0' && c <= '3' && c - '0' != lvl) { lvl = c - '0'; fprintf(stderr, "[COARSEZ] ctl -> level %d%c", lvl, 10); } }
    }
    return lvl;
}
static void lod_sky_apply(D3D12DrawRecord* dr, const u8* uc)
{
    const int lsk_mode = lod_sky_on();
    const int lsk_use = lsk_mode && !(lsk_mode == 1 && !dbt_on());
    const int cz = coarse_bias_level();   /* v244 */
    if ((!lsk_use && !cz) || !uc || dr->blend || !dr->is_vp) return;
    if ((dr->cull & 0x8300u) != 0x8300u) return;            /* guest depth present, test + write */
    if ((dr->cull >> 24) & 1u) return;                        /* the guest already bounds it */
    extern int rsx_fp_decompile_ex(const u8*, u32, u32, u32, char*, u32);
    static u32 keys[1024]; static u8 vals[1024];
    u32 k = dr->fp_uh ? dr->fp_uh : dr->fp_addr;
    u32 i = (k * 2654435761u) >> 22;
    int is;
    if (k && keys[i] == k) is = vals[i];
    else {
        static char hl[65536];
        int n = rsx_fp_decompile_ex(uc, 4096, 0u, 0u, hl, sizeof hl);
        is = 0;
        if (n == 2 && !strstr(hl, "struct PSOutput")) {
            const char* s1 = strstr(hl, "{ float4 _v");
            const char* s2 = s1 ? strstr(s1 + 1, "{ float4 _v") : NULL;
            const char* tx = strstr(hl, "rsx_tex[0].Sample(");
            if (s1 && s2 && tx && tx > s1 && tx < s2 && !strstr(s2, ".Sample(")) is = 1;
        }
        keys[i] = k; vals[i] = (u8)is;
        if (is) { static int _n = 0; if ((_n < 8 && _n++ < 8))
            fprintf(stderr, "[LODSKY] fp=0x%X (fph %08X): coarse-world pass -> only where stored depth is far%c",
                    dr->fp_addr, dr->fp_uh, 10); }
    }
    if (is && cz) { dr->cull |= (u32)cz << 30;   /* v244 coarse depth bias level (PSO key) */
                    if (!(dr->zmax <= s_cz_zmax)) dr->zmax = s_cz_zmax; }   /* v244b: never behind the sky */
    if (is && lsk_use) {
        u32 si = (lsk_mode == 2 && !s_lsk_broken && zs_on() && dr->zeta_off && !((dr->cull >> 16) & 0xFFu)) ? lsk_stencil_idx() : 0u;
        if (si) {   /* v147: sky-region stencil mask (lsk_mark runs before the coarse run) */
            dr->cull = (dr->cull & ~0x00FF0000u) | (si << 16);
            dr->sref = LSK_BIT;
        } else if (dbt_on()) { dr->cull |= 1u << 24; dr->dbmin = 0.99999f; dr->dbmax = 1.0f; }
    }
}
/* v45 GLASS_NEEDS_BACKING (default 1; 0 = off). Cosmetic companion to LOD_SKY_ONLY: the city's
 * glass/window shaders (blended, colour mask RGB only, depth test + WRITE) were drawn where the depth
 * buffer is still the far plane -- their building bodies (the coarse world) are held back by
 * LOD_SKY_ONLY -- so they floated in the sky as translucent slabs ("ghost city"). Such draws now only
 * land where something opaque is already behind them: depth bounds [0, 0.99999] on the stored depth.
 * Particles / lightning / smoke do not write depth and are untouched. Log [GLASS]. */
static void glass_apply(D3D12DrawRecord* dr)
{
    /* v236: GLASS_NEEDS_BACKING=2 (default) = only while the coarse-world restriction is really active (lod_sky_on();
     * off by default since v159 LOD_SKY_START_OFF, F6 turns it on). Without that restriction there is no ghost city to
     * hide, and the signature also matched the Sasha boss arena's TAR floor (blended, RGB mask, depth write, few verts):
     * it was dropped wherever nothing was drawn underneath it -> "see straight through the ground". It also caught the
     * cloud dome (fph 4C75F513/4AC2CFBA logged as [GLASS] in play_pgo.err.log). 1 = old always-on rule, 0 = off. */
    static int on = -1;
    if (on < 0) { const char* e = getenv("GLASS_NEEDS_BACKING"); on = (e && *e) ? atoi(e) : 2; if (on < 0 || on > 2) on = 2;
                  fprintf(stderr, "[GLASS] v236 mode %d (GLASS_NEEDS_BACKING: 0 off, 1 always, 2 only with the coarse-world restriction)%c", on, 10); }
    if (!on || !dbt_on() || !dr->is_vp || !dr->blend || dr->cmask != 0x7u) return;
    if (on == 2 && !lod_sky_on()) return;
    if ((dr->cull & 0x8300u) != 0x8300u || ((dr->cull >> 24) & 1u)) return;
    if (dr->rt_mrt[0]) return;                                  /* single colour target only */
    { /* v72: the CLOUD DOME (vs 49, fph 4C75F513/4AC2CFBA, ~1000-1300 verts per draw) has the same
       * signature; restricting it to "over geometry" split the sky into a hard-edged cloudy block and a
       * clear one. Glass/window draws are small (24-367 verts in every capture). GLASS_MAXVERTS=<n>. */
      static int mv = -1; if (mv < 0) { const char* e = getenv("GLASS_MAXVERTS"); mv = e ? atoi(e) : 800; }
      if (mv > 0 && dr->vertex_count >= (u32)mv) return; }
    dr->cull |= 1u << 24; dr->dbmin = 0.0f; dr->dbmax = 0.99999f;
    { static u32 seen[16]; static int ns = 0; int k = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == dr->fp_uh) k = 1;
      if (!k && ns < 16) { seen[ns++] = dr->fp_uh;
          fprintf(stderr, "[GLASS] fp=0x%X (fph %08X, rt 0x%X): only over existing geometry%c",
                  dr->fp_addr, dr->fp_uh, dr->rt_off, 10); } }
}
/* v219m: cached shadow of the constants last written to a VP slot (only the VS's used runs are valid), so a draw whose
 * constants were RE-SENT with the same values (dirty flag set, bytes equal) can still re-use that slot. */
static float s_vcb_sh[512][4]; static int s_vcb_sh_ok = 0;
/* v219n: b013 showed no extra re-use (22.6 % with or without the compare) -> the compare + shadow copy are dead weight:
 * off unless VCB_CMP=1. */
static int s_vcb_cmp = -1;
static int vcb_runs_equal(const VPVSEntry* ve, const rsx_state* st)
{
    u32 i = 0;
    while (i < 512u) {
        const u32 w = ve->cuse[i >> 5] >> (i & 31u);
        if (!w) { i = (i | 31u) + 1u; continue; }
        i += (u32)__builtin_ctz(w);
        u32 j = i + 1u;
        while (j < 512u) {
            const u32 z = ~ve->cuse[j >> 5] >> (j & 31u);
            if (z) { j += (u32)__builtin_ctz(z); break; }
            j = (j | 31u) + 1u;
        }
        if (j > 512u) j = 512u;
        if (memcmp(st->vertex_constants[i], s_vcb_sh[i], (size_t)(j - i) * 16u)) return 0;
        i = j;
    }
    return 1;
}
/* v221C FPR_FAST (see vp_record_cb's FP block) */
static u64 s_fpr_fast, s_fpr_slow;
static int fpr_fast_on(void)
{
    static int v = -1;
    extern volatile int g_abx;
    if (__builtin_expect(v < 0, 0)) { const char* e = getenv("FPR_FAST"); v = (e && *e == '1') ? 1 : 0;
        fprintf(stderr, "[FPR_FAST] v221C per-record FP memo re-use (one memcmp against the frame snapshot): %s (FPR_FAST=1 = on; ABX 0x100000 = off)%c", v ? "ON" : "off", 10); }
    return v && !(g_abx & 0x100000);
}
static void vp_record_cb(u32 slot, int vs_idx, const D3D12DrawRecord* dr)
{
    if (dr) ((D3D12DrawRecord*)dr)->fp_meta_valid = 0;
    const rsx_state* st = s_d3d.current_rsx_state;
    if (!s_d3d.vp_cb_mapped || !st || slot >= s_max_draws) return;
    /* v221C: s_vs_attr (only read by the frame capture) is re-written only when the attribute layout changed since the
     * last write (d3d12_set_vertex_attribs sets s_vsattr_chg; every FORMAT/OFFSET method sets vertex_dirty, which the
     * BEGIN path turns into that call) or the VS differs from the last one written -- otherwise it would store the same
     * 16 values again (~1.4 % of the FIFO thread, ~9k records/frame). ABX 0x100000 = every record (old). */
    extern volatile int g_abx;
    if (vs_idx >= 0 && vs_idx < VP_VS_CACHE &&   /* v34 */
        (s_vsattr_chg || vs_idx != s_vsattr_last || (g_abx & 0x100000))) {
        s_vsattr_chg = 0; s_vsattr_last = vs_idx;
        for (int a = 0; a < 16; a++) {
            const rsx_vertex_attrib* va = &st->vertex_attribs[a];
            VsAttrInfo* o = &s_vs_attr[vs_idx][a];
            o->en = (u8)(va->enabled ? 1 : 0); o->type = (u8)va->type; o->size = (u8)va->size;
            o->freq = (u8)va->frequency; o->stride = (u16)va->stride; o->loc = (u8)(va->offset >> 31);
        }   /* for */
    }   /* v221C */
    /* FP texcoord scale (b1): 1/size for UNnormalized textures (fmt bit
     * 0x40 -- wave samples everything in texel space), 1.0 otherwise. */
    if (s_d3d.vp_fpcb_mapped) {
        /* v219j FPCB_STAGE (default 1; 0 = off; ABX 0x20000000 = off for in-run A/B): the FP slot used to be written
         * field by field straight into WRITE-COMBINED upload memory, interleaved with reads of the guest ucode
         * (fp_memo_match fills the inline constants word by word) -- scattered partial-line WC writes. Now the slot is
         * built in a cached staging block, then copied with one sequential memcpy of the used ranges ([0, 20 + 4 *
         * constants) and [280, 284)) -- or, when those ranges are byte-identical to the last FP slot written in this
         * batch, that slot is bound instead (dr->fcb_slot). Same bytes reach the shader for every word it reads. */
        float* const ts_wc = (float*)((char*)s_d3d.vp_fpcb_mapped
            + ((u64)s_d3d.vp_parity * s_max_draws + slot) * VP_FPCB_STRIDE);
        static int fst = -1;
        if (fst < 0) { const char* e = getenv("FPCB_STAGE"); fst = (e && *e == '0') ? 0 : 1;
            if (getenv("NO_ALPHATEST")) { }
            fprintf(stderr, "[FPCB_STAGE] v219j FP slot staged + one copy + identical-slot re-use: %s (ABX 0x20000000 = off)%c", fst ? "ON" : "off", 10); }
        extern volatile int g_abx;
        const int stage = fst && dr && !(g_abx & 0x20000000);
        __attribute__((aligned(64))) float tsb[VP_FPCB_STRIDE / 4];
        float* ts = stage ? tsb : ts_wc;
        u32 kwords = 0;   /* inline-constant floats written at ts[20..] */
        for (int _u = 0; _u < 4; _u++) {
            float sx = 1.0f, sy = 1.0f;
            if (dr && dr->tex[_u].set && (dr->tex[_u].fmt & 0x40) &&
                dr->tex[_u].w && dr->tex[_u].h) {
                sx = 1.0f / (float)dr->tex[_u].w;
                sy = 1.0f / (float)dr->tex[_u].h;
            }
            ts[_u*4+0] = sx; ts[_u*4+1] = sy; ts[_u*4+2] = 0.0f; ts[_u*4+3] = 0.0f;
        }
        /* rsx_alphatest (b1[4]): x=enable, y=ref/255, z=func-0x200. D3D12
         * has no fixed alpha test; guest FPs discard on it dynamically
         * (wave's colour wheel is a disc via alpha test -- without it the
         * palette drew as an opaque square). */
        static int no_alpha = -1;
        if (no_alpha < 0) no_alpha = getenv("NO_ALPHATEST") != NULL;
        if (dr && !no_alpha) {
            { static int _at = 0;
              if (((dr->alpha_ctl >> 16) & 1u) && (_at < 4 && _at++ < 4))
                  printf("[ALPHATEST] enable func=%u ref=%u\n",
                         (dr->alpha_ctl >> 8) & 0xFFu, dr->alpha_ctl & 0xFFu); }
            ts[16] = (float)((dr->alpha_ctl >> 16) & 1u);
            ts[17] = (float)(dr->alpha_ctl & 0xFFu) / 255.0f;
            ts[18] = (float)((dr->alpha_ctl >> 8) & 0x07u);
            ts[19] = 0.0f;
        } else {
            ts[16] = 0.0f; ts[17] = 0.0f; ts[18] = 7.0f; ts[19] = 0.0f;
        }
        if (dr && ((dr->cull >> 29) & 1u)) {   /* v39 alpha-to-coverage: alpha >= 0.5 */
            ts[16] = 1.0f; ts[17] = 0.5f; ts[18] = 6.0f; ts[19] = 0.0f;
        }
        {   /* v154 rsx_wscale (b1[70]): fragment WPOS -> guest pixels on a RES_SCALE target */
            float wx = 1.0f, wy = 1.0f;
            if (dr && s_res_scale > 1.0f) {
                u32 gw0 = dr->rt_off ? dr->rt_w : s_d3d.width, gh0 = dr->rt_off ? dr->rt_h : s_d3d.height;
                if (!gw0 || !gh0) { gw0 = s_d3d.width; gh0 = s_d3d.height; }
                u32 pw = gw0, ph = gh0; particle_size(&pw, &ph);
                wx = (float)gw0 / (float)pw; wy = (float)gh0 / (float)ph;
            }
            ts[280] = wx; ts[281] = wy; ts[282] = 1.0f; ts[283] = 1.0f;
        }

        /* fp_k[]: the program's inline fragment constants, re-read from the
         * guest ucode every draw. The decompiler emits fp_k[i] lookups instead
         * of baking these in as literals, so the compiled shader no longer
         * changes when the title re-patches them -- which is what forced the
         * pipeline cache to key on a hash of the ucode bytes. */
        if (dr && dr->fp_addr) {
            extern uint8_t* vm_base;
            extern u32 cellGcmResolveLocated(int, u32);
            u32 foff = cellGcmResolveLocated((dr->fp_addr & 0x3u) == 1,
                                             dr->fp_addr & ~0x3u);
            if (vm_base && foff != 0xFFFFFFFFu) {
                static int s_snap = -1;
                if (s_snap < 0) { const char* e = getenv("FP_SNAPSHOT"); s_snap = (e && *e == '0') ? 0 : 1; }
                u32 uh = 0;
                const u8* src = vm_base + foff;
                FpMemo* m = &s_fpm[((dr->fp_addr >> 2) * 2654435761u) >> 23];
                u32 mh = 0;
                /* v221C FPR_FAST (FPR_FAST=1 = on, default off until b029 is measured; ABX 0x100000 = off): the memo hit
                 * below walks the program 16 B at a time (compare, serial hash, re-sanitize every inline constant) and
                 * then memcmps it again against the frame's snapshot -- per record (~9k/frame). If the guest bytes equal
                 * the snapshot this memo entry produced in THIS arena epoch (one AVX memcmp), every output of that path
                 * (inline constants, hash, snapshot pointer, constant count) is the same as last time: re-use them. */
                if (fpr_fast_on() && s_snap && fp_memo_on() && m->addr == dr->fp_addr && m->len && m->sp &&
                    m->sep == s_fpsnap_ep && m->spar == (u32)s_d3d.vp_parity && memcmp(src, m->sp, m->len) == 0) {
                    memcpy(&ts[20], m->klast, (size_t)m->skw * 4u);
                    ((D3D12DrawRecord*)dr)->fp_uc = m->sp;
                    uh = m->smh; kwords = m->skw;
                    s_fpm_hit++; s_fpr_fast++;
                } else
                if (fp_memo_on() && m->addr == dr->fp_addr && m->len && fp_memo_match(m, src, &ts[20], &mh)) {
                    const u8* uc_ = s_snap ? fpsnap_get_known(dr->fp_addr, src, m->len, mh) : NULL;
                    ((D3D12DrawRecord*)dr)->fp_uc = uc_;
                    uh = uc_ ? mh : 0;
                    s_fpm_hit++;
                    kwords = 4u * (m->nisk < FP_K_SLOTS ? m->nisk : FP_K_SLOTS);   /* v219j: fp_memo_match wrote these */
                    if (fpr_fast_on() && uc_) {   /* v221C: remember this hit's outputs for the fast path above */
                        m->sp = uc_; m->sep = s_fpsnap_ep; m->spar = (u32)s_d3d.vp_parity; m->smh = mh; m->skw = kwords;
                        memcpy(m->klast, &ts[20], (size_t)kwords * 4u); s_fpr_slow++;
                    }
                } else {
                m->sp = NULL;   /* v221C: the memo is refilled (or dropped) below */
                int nk = fp_fill_consts(src, 4096, &ts[20], FP_K_SLOTS);   /* fp_constants[64] */
                kwords = 4u * (u32)(nk < 0 ? 0 : nk < FP_K_SLOTS ? nk : FP_K_SLOTS);   /* v219j */
                ((D3D12DrawRecord*)dr)->fp_uc = s_snap ? fpsnap_get(dr->fp_addr, src, &uh) : NULL;
                if (fp_memo_on()) {
                    u32 k_ = 0, ch_ = 0, fh_ = 0, used_ = 0;
                    if (fp_scan(src, 4096, &k_, &ch_, &fh_, &used_) && used_) {
                        if (m->cap < used_) { u8* nc = (u8*)realloc(m->code, used_); if (nc) { m->code = nc; m->cap = used_; } }
                        if (m->cap >= used_ && used_ <= 4096u) {
                            memcpy(m->code, src, used_);
                            m->addr = dr->fp_addr; m->len = used_; m->fh = fh_;
                            m->code_hash = ch_; m->const_count = k_;
                            m->tex_mask = rsx_fp_texture_mask(src, used_);
                            m->nk = (u32)(nk < FP_K_SLOTS ? nk : FP_K_SLOTS);
                            memcpy(m->k, &ts[20], (size_t)m->nk * 16u);
                            /* layout: which 16-byte blocks are inline constants (same walk as fp_scan) */
                            memset(m->isk, 0, sizeof m->isk);
                            for (u32 off = 0; off + 16 <= used_; ) {
                                u32 w1 = rsx_fp_read_word(src + off + 4), w2 = rsx_fp_read_word(src + off + 8), w3 = rsx_fp_read_word(src + off + 12);
                                off += 16;
                                if (fp_has_const(w1, w2, w3) && off + 16 <= used_) { m->isk[off >> 4] = 1; off += 16; }
                            }
                            m->nisk = 0; for (u32 b_ = 0; b_ < (used_ >> 4) && b_ < 256u; b_++) m->nisk += m->isk[b_];   /* v219j */
                        } else m->len = 0;
                    } else m->len = 0;
                    s_fpm_miss++;
                }
                }
                ((D3D12DrawRecord*)dr)->fp_uh = uh;
                if (fp_binding_meta_on() && fp_memo_on() && m->addr == dr->fp_addr && m->len && dr->fp_uc) {
                    D3D12DrawRecord* wr = (D3D12DrawRecord*)dr;
                    wr->fp_code_hash = m->code_hash; wr->fp_const_count = m->const_count;
                    wr->fp_tex_mask = m->tex_mask; wr->fp_meta_valid = 1;
                }
                lod_sky_apply((D3D12DrawRecord*)dr, dr->fp_uc ? dr->fp_uc : vm_base + foff);   /* v41 */
                glass_apply((D3D12DrawRecord*)dr);   /* v45 */
            }
            /* FPK_DBG=1: the fragment constants as the shader will see them.
             * A program whose only output is fp_k[0].xxxx renders black if
             * those constants never arrive, which looks identical from the
             * outside to geometry that never rasterised. */
            { static int fk = -1; if (fk < 0) fk = getenv("FPK_DBG") ? 1 : 0;
              static int n = 0;
              if (fk && n < 12 && dr->fp_addr != 0x00EC2B02u) { n++;
                  fprintf(stderr, "[FPK] tex0=0x%08X fp=0x%08X k0=(%g %g %g %g)"
                                  " k1=(%g %g %g %g)%c",
                          dr->tex[0].raw, dr->fp_addr,
                          ts[20], ts[21], ts[22], ts[23],
                          ts[24], ts[25], ts[26], ts[27], 10);
                  const u32 raw = dr->fp_addr & ~0x3u;
                  const u32 ea_main  = cellGcmResolveLocated(0, raw);
                  const u32 ea_local = cellGcmResolveLocated(1, raw);
                  fprintf(stderr, "[FPK]   ucode main@0x%08X:", ea_main);
                  for (int b = 0; b < 48; b++) fprintf(stderr, "%s%02X", (b%16)?" ":"  |", vm_base[ea_main + b]);
                  fputc(10, stderr); }
              /* distinct fragment programs seen -- if every draw shares one,
               * SET_SHADER_PROGRAM is not being tracked per draw. */
              if (fk) { static u32 seen[16]; static int ns = 0; int f = 0;
                  for (int q = 0; q < ns; q++) if (seen[q] == dr->fp_addr) f = 1;
                  if (!f && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[FPK] distinct fp #%d = 0x%08X%c", ns, dr->fp_addr, 10); } } }
        }
        if (stage) {   /* v219j: dedup or one sequential copy into the WC slot */
            static float s_fl[VP_FPCB_STRIDE / 4]; static u32 s_fl_k = 0xFFFFFFFFu, s_fl_slot = 0, s_fl_frame = 0xFFFFFFFFu; static int s_fl_par = -1;
            static u64 s_fl_hit = 0, s_fl_wr = 0; static u32 s_fl_rep = 0;
            const u32 nw = 20u + kwords;
            if (s_fl_k == kwords && slot > s_fl_slot && s_fl_frame == s_frame_no && s_fl_par == s_d3d.vp_parity &&
                !memcmp(s_fl, tsb, nw * 4u) && !memcmp(&s_fl[280], &tsb[280], 16)) {
                ((D3D12DrawRecord*)dr)->fcb_slot = s_fl_slot; s_fl_hit++;
            } else {
                memcpy(ts_wc, tsb, nw * 4u); memcpy(&ts_wc[280], &tsb[280], 16);
                memcpy(s_fl, tsb, nw * 4u); memcpy(&s_fl[280], &tsb[280], 16);
                s_fl_k = kwords; s_fl_slot = slot; s_fl_frame = s_frame_no; s_fl_par = s_d3d.vp_parity; s_fl_wr++;
            }
            if (s_frame_no / 600u != s_fl_rep) { s_fl_rep = s_frame_no / 600u;
                fprintf(stderr, "[FPCB_STAGE] %llu re-used, %llu written (%.1f %% re-used)%c", (unsigned long long)s_fl_hit,
                        (unsigned long long)s_fl_wr, 100.0 * s_fl_hit / (double)(s_fl_hit + s_fl_wr + 1), 10);
                s_fl_hit = s_fl_wr = 0; }
        }
    }
#ifdef PS3RECOMP_DLSS
    /* DLAA: does this draw rasterize the main scene (full-size target + depth, depth test on)? Such draws get the
     * frame's sub-pixel jitter in the epilogue below, so it is part of the VCB_DEDUP identity. */
    const int dlss_q = (dr && dlss_hooks_on()) ? rsx_dlss_draw_qualifies(st->depth_test_enable, dr->zeta_off, dr->zw, dr->zh, dr->rt_off,
                                                    dr->rt_w, dr->rt_h, dr->vp_w, dr->vp_h, s_d3d.width, s_d3d.height) : 0;
#else
    const int dlss_q = 0;
#endif
    {   /* v219g VCB_DEDUP (default 1; 0 = off; ABX 0x8000000 = off for in-run A/B): skip writing this draw's VP
         * constant slot when it would be byte-identical to the last slot written in this batch -- same VS entry (same
         * used-constant mask), no constant written since (rsx_state.vertex_constants_dirty, set by every
         * TRANSFORM_CONSTANT write incl. the v65 fast path, consumed here), same z viewport/clip inputs of the
         * epilogue -- and bind that earlier slot instead (dr->vcb_slot). The FP slot (b1) is still written per draw.
         * Slots only grow within a batch (a split restarts at 0 -> slot <= last -> full write), so the re-used slot
         * cannot be overwritten before the GPU reads it. vp_record_cb was ~10 % of the FIFO thread (b006): write-
         * combined copies of the used constants for ~13k draws/frame. */
        static int dd = -1;
        static u32 s_vcb_slot = 0, s_vcb_frame = 0xFFFFFFFFu; static int s_vcb_par = -1, s_vcb_vs = -9;
        static const void* s_vcb_ve = NULL; static u32 s_vcb_hash = 0; static float s_vcb_z[4]; static u64 s_vcb_hit = 0, s_vcb_full = 0; static u32 s_vcb_rep = 0;
        static int s_vcb_dq = -1;   /* DLAA jitter flag of the last full slot */
        if (s_vcb_cmp < 0) { const char* c = getenv("VCB_CMP"); s_vcb_cmp = (c && *c == '1') ? 1 : 0; }
        if (dd < 0) { const char* e = getenv("VCB_DEDUP"); dd = (e && *e == '0') ? 0 : 1;
            if (getenv("VP_MVP") || getenv("RSX_DBG_CB") || getenv("DBG_ZOOM") || getenv("VPC_DUMP_FP") || getenv("VPC_DUMP_VS") ||
                getenv("VP_FIXPROJ") || getenv("VP_NOFIXPROJ")) dd = 0;
            fprintf(stderr, "[VCB_DEDUP] v219g identical VP constant slots re-used: %s (ABX 0x8000000 = off)%c", dd ? "ON" : "off", 10); }
        rsx_state* stw = (rsx_state*)st;
        const int cchg = stw->vertex_constants_dirty; stw->vertex_constants_dirty = 0;
        const VPVSEntry* ve0 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) ? &s_d3d.vp_vs[vs_idx] : NULL;
        const float zin[4] = { st->viewport_scale[2], st->viewport_offset[2], st->clip_min, st->clip_max };
        extern volatile int g_abx;
        if (dd && dr && !(g_abx & 0x8000000) && ve0 && s_vcb_ve == (const void*)ve0 && s_vcb_hash == ve0->hash && s_vcb_vs == vs_idx &&
            slot > s_vcb_slot && s_vcb_frame == s_frame_no && s_vcb_par == s_d3d.vp_parity &&
            !memcmp(zin, s_vcb_z, sizeof zin) && s_vcb_dq == dlss_q &&
            (!cchg || (s_vcb_cmp > 0 && s_vcb_sh_ok && ve0->cvalid && !ve0->crel && !(g_abx & 0x40000000) && vcb_runs_equal(ve0, st)))) {   /* v219m: equal re-sent constants (VCB_CMP=1) */
            ((D3D12DrawRecord*)dr)->vcb_slot = s_vcb_slot;
            s_vcb_hit++;
            if (s_frame_no / 600u != s_vcb_rep) { s_vcb_rep = s_frame_no / 600u;
                fprintf(stderr, "[VCB_DEDUP] %llu re-used, %llu written (%.1f %% re-used)%c", (unsigned long long)s_vcb_hit,
                        (unsigned long long)s_vcb_full, 100.0 * s_vcb_hit / (double)(s_vcb_hit + s_vcb_full + 1), 10);
                s_vcb_hit = s_vcb_full = 0; }
            return;
        }
        s_vcb_full++;
        s_vcb_slot = slot; s_vcb_frame = s_frame_no; s_vcb_par = s_d3d.vp_parity; s_vcb_vs = vs_idx; s_vcb_ve = ve0; s_vcb_hash = ve0 ? ve0->hash : 0;
        memcpy(s_vcb_z, zin, sizeof zin);
        s_vcb_dq = dlss_q;
    }
    char* dst = (char*)s_d3d.vp_cb_mapped
        + ((u64)s_d3d.vp_parity * s_max_draws + slot) * VP_CB_STRIDE;
    {   /* v46 VCB_PARTIAL (default 1): copy only the constants this draw's VS reads -- the full
         * 8 KB bank per draw was ~110 MB/frame of write-combined writes in the city. */
        static int part = -1;
        if (part < 0) { const char* e = getenv("VCB_PARTIAL"); part = (e && *e == '0') ? 0 : 1; }
        const VPVSEntry* ve = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) ? &s_d3d.vp_vs[vs_idx] : NULL;
        extern volatile int g_abx;
        if (part && ve && ve->cvalid && !ve->crel && RSX_MAX_VERTEX_CONSTANTS == 512 && !(g_abx & 0x4000)) {
            /* v180: word-at-a-time run scan of the used-constant mask (ctz). The old loop below tested all 512
             * bits one by one for EVERY draw (~1000 cycles x ~13k draws/frame, most of vp_record_cb's ~14 % of
             * the FIFO thread in perf180). Same runs, same copies (checked against the old loop on 3 M masks).
             * ABX 0x4000 = old loop (in-run A/B). */
            u32 i = 0;
            while (i < 512u) {
                const u32 w = ve->cuse[i >> 5] >> (i & 31u);
                if (!w) { i = (i | 31u) + 1u; continue; }
                i += (u32)__builtin_ctz(w);
                u32 j = i + 1u;
                while (j < 512u) {
                    const u32 z = ~ve->cuse[j >> 5] >> (j & 31u);
                    if (z) { j += (u32)__builtin_ctz(z); break; }
                    j = (j | 31u) + 1u;
                }
                if (j > 512u) j = 512u;
                memcpy(dst + (size_t)i * 16u, st->vertex_constants[i], (size_t)(j - i) * 16u);
                if (s_vcb_cmp > 0) memcpy(s_vcb_sh[i], st->vertex_constants[i], (size_t)(j - i) * 16u);   /* v219m shadow (VCB_CMP=1) */
                i = j;
            }
            s_vcb_sh_ok = 1;
        } else if (part && ve && ve->cvalid && !ve->crel && RSX_MAX_VERTEX_CONSTANTS == 512) {
            s_vcb_sh_ok = 0;
            u32 i = 0;
            while (i < 512u) {
                if (!((ve->cuse[i >> 5] >> (i & 31u)) & 1u)) { i++; continue; }
                u32 j = i; while (j < 512u && ((ve->cuse[j >> 5] >> (j & 31u)) & 1u)) j++;
                memcpy(dst + (size_t)i * 16u, st->vertex_constants[i], (size_t)(j - i) * 16u);
                i = j;
            }
        } else {
            memcpy(dst, st->vertex_constants, RSX_MAX_VERTEX_CONSTANTS * 16);
            s_vcb_sh_ok = 0;
        }
    }
    /* VP_MVP=<N>: the constant bank as the shader will see it, for the first N
     * draws. A snapshot taken from a stale rsx_state looks exactly like a broken
     * vertex program from the outside -- both give zero fragments. */
    { static int cap = -1, seen = 0;
      if (cap < 0) { const char* e = getenv("VP_MVP"); cap = e ? atoi(e) : 0; }
      if (cap && seen < cap) { seen++;
        const float* c = (const float*)dst;
        int last_nz = -1;
        for (int i = 0; i < RSX_MAX_VERTEX_CONSTANTS * 4; i++)
            if (c[i] > 1e-9f || c[i] < -1e-9f) last_nz = i / 4;
        fprintf(stderr, "[VPMVP] slot=%u lastNZ=c%d  c260=(%g %g %g %g) c467=(%g %g %g %g)\n",
                slot, last_nz,
                c[260*4+0], c[260*4+1], c[260*4+2], c[260*4+3],
                c[467*4+0], c[467*4+1], c[467*4+2], c[467*4+3]); } }
    /* RSX_DBG_CB=<hex tex0 offset>: the constants as they land in the per-draw
     * constant buffer for that texture's draws -- i.e. exactly what the shader
     * samples, not what the CPU-side rsx_state holds. Those two agreeing is an
     * assumption worth checking directly when a draw transforms to nothing. */
    const float* vpx_dbg = (const float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    { static const char* dc = (const char*)1; static u32 wantc = 0; static int n = 0;
      if (dc == (const char*)1) { dc = getenv("RSX_DBG_CB");
                                  wantc = dc ? (u32)strtoul(dc, NULL, 16) : 0; }
      if (wantc && dr && dr->tex[0].raw == wantc && n < 4) { n++;
          const float* c = (const float*)dst;
          fprintf(stderr, "[DUCKCB] slot=%u vs_idx=%d posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  slot, vs_idx, vpx_dbg[0], vpx_dbg[1], vpx_dbg[2],
                  vpx_dbg[4], vpx_dbg[5], vpx_dbg[6], 10);
          for (int r = 256; r <= 259; r++)
              fprintf(stderr, "[DUCKCB]   c%d=(%g %g %g %g)%c", r,
                      c[r*4+0], c[r*4+1], c[r*4+2], c[r*4+3], 10);
      } }

    /* Viewport epilogue (see the render_frame notes this logic came from):
     * x/y identity, z lane remaps GL clip z when the guest programs one. */
    float* vpx = (float*)(dst + RSX_MAX_VERTEX_CONSTANTS * 16);
    const float* vs_ = st->viewport_scale;
    const float* vo_ = st->viewport_offset;
    vpx[0] = vpx[1] = vpx[3] = 1.0f;
    vpx[4] = vpx[5] = vpx[7] = 0.0f;
    if (vs_[2] != 0.0f) { vpx[2] = vs_[2]; vpx[6] = vo_[2]; }
    else if (zflat_on() && (vo_[2] != 0.0f || st->clip_min == st->clip_max)) {
        /* v34 FLAT DEPTH RANGE. cellGcmSetViewport(.., min, max) programs z scale =
         * (max-min)/2 and offset = (max+min)/2, so a pass drawn with min == max (inFamous'
         * far-LOD city / sky meshes use [1,1] so they only land where the depth buffer is
         * still clear) has z SCALE 0. That used to fall into the "guest never programmed
         * a viewport" branch below (scale 1, offset 0): those meshes kept their real depth
         * and, with blend off, painted flat unlit buildings over the lit frame. Keep the
         * guest's transform: z = offset * w. ZFLAT=0 -> old behaviour. */
        float zo = (vo_[2] != 0.0f) ? vo_[2] : st->clip_min;
        if (!(zo >= 0.0f)) zo = 0.0f; if (zo > 1.0f) zo = 1.0f;
        vpx[2] = 0.0f; vpx[6] = zo;
        { static int n = 0; if ((n < 4 && n++ < 4)) fprintf(stderr, "[ZFLAT] flat depth range: z = %g (vp scale %g offset %g, clip [%g, %g], fp 0x%X)%c",
              zo, vs_[2], vo_[2], st->clip_min, st->clip_max, dr ? dr->fp_addr : 0u, 10); }
    }
    else                { vpx[2] = 1.0f;   vpx[6] = 0.0f;   }
    /* DBG_ZOOM=<k> with DBG_CENTER="cx,cy": a debug camera applied in CLIP space,
     * not a framebuffer crop -- the geometry is re-rasterized at full resolution.
     * The shader epilogue already computes
     *     pos.xyz = _p.xyz * vp_posscale + _p.w * vp_posoffset
     * so recentring NDC point c and magnifying by k is exactly
     *     pos.xy = _p.xy * k + _p.w * (-c * k).
     * Rubber Ducky frames its ducks near the bottom edge of the viewport and its
     * camera only moves from the analog sticks, so without this there is no way
     * to look at the subject of the demo. It changes the view, nothing else:
     * same draws, same transform, same textures. */
    { static int z = -1; static float k = 1.0f, cx = 0.0f, cy = 0.0f;
      if (z < 0) { const char* e = getenv("DBG_ZOOM");
                   k = e ? (float)atof(e) : 1.0f;
                   z = (e && k > 0.0f) ? 1 : 0;
                   const char* ce = getenv("DBG_CENTER");
                   if (ce) { double a = 0, b = 0; sscanf(ce, "%lf,%lf", &a, &b);
                             cx = (float)a; cy = (float)b; } }
      if (z) { float ux = cx, uy = cy;
               static int lock = -1;
               if (lock < 0) { const char* le = getenv("DBG_LOCK"); lock = le ? atoi(le) : 0; }
               if (lock && s_lock_valid) { ux = s_lock_x; uy = s_lock_y; }
               vpx[0] = k; vpx[1] = k; vpx[4] = -ux * k; vpx[5] = -uy * k; } }

#ifdef PS3RECOMP_DLSS
    if (dr && dlss_hooks_on()) {   /* DLAA: camera tracking / probe see the unjittered epilogue, then the scene draws get the jitter */
        const VPVSEntry* dve = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n) ? &s_d3d.vp_vs[vs_idx] : NULL;
        rsx_dlss_draw_info di;
        di.frame = s_frame_no; di.rec_index = slot; di.vs_idx = vs_idx; di.vs_hash = dve ? dve->hash : 0;
        di.vs_src = dve ? dve->src : NULL; di.cuse = (dve && dve->cvalid && !dve->crel) ? dve->cuse : NULL;
        di.consts = (const float*)st->vertex_constants;
        di.rt_off = dr->rt_off; di.rt_w = dr->rt_w; di.rt_h = dr->rt_h; di.zeta_off = dr->zeta_off; di.zw = dr->zw; di.zh = dr->zh;
        di.vp_x = dr->vp_x; di.vp_y = dr->vp_y; di.vp_w = dr->vp_w; di.vp_h = dr->vp_h;
        di.fp_addr = dr->fp_addr; di.vcount = dr->vertex_count; di.color_target = st->color_target;
        di.mrt = (dr->rt_mrt[0] ? 1u : 0u) + (dr->rt_mrt[1] ? 1u : 0u) + (dr->rt_mrt[2] ? 1u : 0u);
        di.depth_test = st->depth_test_enable; di.depth_mask = st->depth_mask; di.blend = dr->blend;
        di.posscale[0] = vpx[0]; di.posscale[1] = vpx[1]; di.posscale[2] = vpx[2];
        di.posoffset[0] = vpx[4]; di.posoffset[1] = vpx[5]; di.posoffset[2] = vpx[6];
        di.guest_w = s_d3d.width; di.guest_h = s_d3d.height;
        rsx_dlss_note_draw(&di, dlss_q);
        if (dlss_q) rsx_dlss_jitter_ndc(&vpx[4], &vpx[5]);
    }
#endif

    /* VPC_DUMP_FP=<hex fp>: for the first 3 draws using that fragment program,
     * print the vertex constants a world*viewproj VP reads (c256..c269, c464..c467),
     * the RSX viewport, and the epilogue the shader gets. inFamous' G-buffer
     * VPs build o0 = c263..266 * (c256..259 * v0); the frame rasterises NOTHING
     * although the vertex data is sane (2026-10-04), so look at the matrices. */
    { static int on = -1; static u32 want = 0; static int n = 0;
      static int wvs = -2, wvn = 0; static int wvl[8];   /* v74b/c: VPC_DUMP_VS=<idx>[,<idx>..] */
      if (wvs == -2) { const char* e = getenv("VPC_DUMP_VS"); wvs = e ? 0 : -1;
          while (e && *e && wvn < 8) { wvl[wvn++] = atoi(e); e = strchr(e, ','); if (e) e++; } }
      int _wvh = 0; for (int q = 0; q < wvn; q++) if (vs_idx == wvl[q]) _wvh = 1;
      if (on < 0) { const char* e = getenv("VPC_DUMP_FP"); want = e ? (u32)strtoul(e, NULL, 16) : 0; on = (want || wvs >= 0) ? 1 : 0; }
      if (on && dr && ((want && dr->fp_addr == want) || _wvh) && n < 12) { n++;
          const float* cc = (const float*)dst;
          fprintf(stderr, "[VPC] draw slot=%u fp=0x%X vs_idx=%d tcl=%u vstart=%u viewport scale=(%g %g %g %g) offset=(%g %g %g %g)%c",
                  slot, dr->fp_addr, vs_idx, st->transform_constant_load, st->transform_program_start,
                  st->viewport_scale[0], st->viewport_scale[1], st->viewport_scale[2], st->viewport_scale[3],
                  st->viewport_offset[0], st->viewport_offset[1], st->viewport_offset[2], st->viewport_offset[3], 10);
          fprintf(stderr, "[VPC]   epilogue posscale=(%g %g %g) posoffset=(%g %g %g)%c",
                  vpx[0], vpx[1], vpx[2], vpx[4], vpx[5], vpx[6], 10);
          static const int regs[] = {0,1,2,3, 256,257,258,259,260,261,262,263,264,265,266,267,268,269, 464,465,466,467};
          for (unsigned k = 0; k < sizeof(regs)/sizeof(regs[0]); k++) {
              int r = regs[k];
              fprintf(stderr, "[VPC]   c%-3d = (%12g %12g %12g %12g)%c", r,
                      cc[r*4+0], cc[r*4+1], cc[r*4+2], cc[r*4+3], 10);
          }
          fflush(stderr);
      } }

    /* Garbage-projection fallback (vkcube; see the original comment). */
    int uses_c03 = (vs_idx >= 0 && vs_idx < s_d3d.vp_vs_n)
                       ? s_d3d.vp_vs[vs_idx].uses_c03 : s_d3d.vp_uses_c03;
    float* c = (float*)dst;
    float c00 = st->vertex_constants[0][0];   /* v46: dst[0] is not written when the VS does not read c0 */
    int garbage = !(c00 > 0.0f && c00 < 8.0f);
    if (getenv("VP_NOFIXPROJ")) garbage = 0;
    if ((garbage && uses_c03) || getenv("VP_FIXPROJ")) {
        { static int _fb = 0; if ((_fb < 6 && _fb++ < 6))
            printf("[VPFB] fallback proj on draw slot %u (c00=%g vs_idx=%d)\n",
                   slot, c00, vs_idx); }
        for (int _i = 0; _i < 16; _i++) c[_i] = 0.0f;
        c[0]=1.358f; c[5]=2.414f; c[11]=1.0f;
        c[10]=1.0f/99.0f; c[14]=-1.0f/99.0f;
        vpx[2] = 1.0f; vpx[6] = 0.0f;
    }
}

/* Which offscreen surface (if any) does the current RSX state render to?
 * Returns 0 for a display buffer (backbuffer), else the surface's RAW RSX
 * offset. RTs are keyed by raw offset -- a texture bound at the same raw
 * offset is the same buffer (surface and texture registers share the offset
 * space; location bits differ but one title doesn't alias local vs main at
 * one offset), which sidesteps guessing the surface's context DMA location. */
static u32 current_rt_off(u32* out_w, u32* out_h, u32 out_mrt[3])
{
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const rsx_state* st = s_d3d.current_rsx_state;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (out_mrt) out_mrt[0] = out_mrt[1] = out_mrt[2] = 0;
    if (!st) return 0;
    /* SET_SURFACE_COLOR_TARGET: 1 = A, 2 = B, 0x13 = MRT1 (A+B),
     * 0x17 = MRT2 (A+B+C), 0x1F = MRT3 (A+B+C+D). */
    int sel = (st->color_target == 2) ? 1 : 0;
    u32 raw = st->surface_color_offset[sel];
    if (out_mrt) {
        if (st->color_target >= 0x13) out_mrt[0] = st->surface_color_offset[1];
        if (st->color_target >= 0x17) out_mrt[1] = st->surface_color_offset[2];
        if (st->color_target >= 0x1F) out_mrt[2] = st->surface_color_offset[3];
    }
    { static int _rs = -1; if (_rs < 0) _rs = getenv("RT_SEQDBG") ? 1 : 0;
      static u32 _last = 0; static int _n2 = 0;
      if (_rs && st->surface_color_offset[0] != _last && _n2 < 400000) {
          _last = st->surface_color_offset[0]; _n2++;
          fprintf(stderr, "[RTSEQ] #%d -> 0x%X%c", _n2, _last, 10); } }
    { static int _ro = -1; if (_ro < 0) _ro = getenv("RT_OFFDBG") ? 1 : 0;
      static u32 _seen[32]; static int _ns = 0;
      if (_ro) { u32 _k = st->surface_color_offset[0] ^ (st->color_target << 24)
                        ^ (st->surface_clip_w << 8) ^ st->surface_format;
        int _f = 0; for (int _i = 0; _i < _ns; _i++) if (_seen[_i] == _k) _f = 1;
        if (!_f && _ns < 32) { _seen[_ns++] = _k;
        fprintf(stderr, "[RTOFF] fmt=0x%X tgt=0x%X off[0]=0x%X off[1]=0x%X zeta=0x%X clip=%ux%u disp=%d -> 0x%X\n",
                st->surface_format, st->color_target, st->surface_color_offset[0], st->surface_color_offset[1],
                st->surface_zeta_offset, st->surface_clip_w, st->surface_clip_h,
                cellGcmOffsetIsDisplay(raw), cellGcmOffsetIsDisplay(raw) ? 0 : raw); } } }
    if (cellGcmOffsetIsDisplay(raw)) return 0;
    /* RT_DISPLAY_BY_SIZE=1: also treat a single-target surface whose clip
     * exactly matches the display resolution as the backbuffer.
     *
     * cellGcmSetDisplayBuffer only registers the buffers the FLIP may point at;
     * a guest is free to render into a different surface of the same size and
     * flip to it later (PSGL does). Matching offsets alone then classifies every
     * draw as offscreen, has_display stays 0, and render_frame() -- which is
     * what draws ALL recorded geometry -- is never called. The backbuffer shows
     * only the clear, which looks exactly like "nothing rasterizes". */
    { static int _bs = -1; if (_bs < 0) _bs = getenv("RT_DISPLAY_BY_SIZE") ? 1 : 0;
      if (_bs && st->color_target < 0x13 &&
          st->surface_clip_w == s_d3d.width && st->surface_clip_h == s_d3d.height)
          return 0; }
    /* Surface clip dims when sane; else the window size. Any size works --
     * passes draw normalized full-surface quads -- this only picks resolution. */
    u32 w = st->surface_clip_w, h = st->surface_clip_h;
    if (w < 16 || w > 2048 || h < 16 || h > 2048) { w = 0; h = 0; }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return raw;
}

/* RSX surface colour format (SET_SURFACE_FORMAT bits [4:0]) -> DXGI. Float
 * targets matter: wave's water height maps store SIGNED values (F_W16..),
 * demosaic's differential planes likewise -- RGBA8 clamps them to zero. */
static DXGI_FORMAT rsx_surface_dxgi(u32 fmt)
{
    switch (fmt & 0x1F) {
    case 0x0B: return DXGI_FORMAT_R16G16B16A16_FLOAT; /* F_W16Z16Y16X16 */
    case 0x0C: return DXGI_FORMAT_R32G32B32A32_FLOAT; /* F_W32Z32Y32X32 */
    case 0x0D: return DXGI_FORMAT_R32_FLOAT;          /* F_X32          */
    default:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}

/* Deferred release. A resource the OPEN command list references must not be
 * freed until that list has executed: wait_for_gpu() only waits for lists
 * already submitted. off_rt_get() used to wait_for_gpu() + Release() mid-
 * recording, so an aliased RT's init copy (recorded earlier in the same list)
 * hit freed memory -> GPU page fault -> DEVICE_HUNG on inFamous' first world
 * frame (DRED: op 1 COPYTEXTURE_REGION, fault in a recently freed resource).
 * Flushed at the start of render_frame, after its wait_for_gpu(). */
static ID3D12Resource* s_graveyard[1024];
static int s_grave_n = 0;
static void defer_release(ID3D12Resource* r)
{
    if (!r) return;
    s_srv_gen++;
    if (s_grave_n < (int)(sizeof(s_graveyard) / sizeof(s_graveyard[0]))) { s_graveyard[s_grave_n++] = r; return; }
    static int _w = 0; if ((_w < 4 && _w++ < 4)) printf("[D3D12] graveyard full -- leaking a resource\n");
}
static void flush_graveyard(void)
{
    if (s_grave_n) s_srv_gen++;
    for (int i = 0; i < s_grave_n; i++) s_graveyard[i]->lpVtbl->Release(s_graveyard[i]);
    s_grave_n = 0;
}
/* v235C RF_EARLY: release only what was deferred before this exec began (see tp_flush_frees_upto). */
static int s_rf_grave_mark = 0;
static void flush_graveyard_upto(int mark)
{
    if (mark > s_grave_n) mark = s_grave_n;
    if (mark <= 0) return;
    s_srv_gen++;
    for (int i = 0; i < mark; i++) s_graveyard[i]->lpVtbl->Release(s_graveyard[i]);
    memmove(&s_graveyard[0], &s_graveyard[mark], (size_t)(s_grave_n - mark) * sizeof(s_graveyard[0]));
    s_grave_n -= mark;
}

/* Offset -> the alias most recently RENDERED to, in submission order. Updated
 * as render_frame walks the draws; persists across frames (a texture sampled
 * before this frame redraws its surface reads last frame's alias). */
static struct { u32 off; int slot; } s_rt_latest[64];
static int s_rt_latest_n = 0;
static void rt_latest_set(u32 off, int slot)
{
    if (!off || slot < 0) return;
    for (int i = 0; i < s_rt_latest_n; i++) if (s_rt_latest[i].off == off) { s_rt_latest[i].slot = slot; return; }
    if (s_rt_latest_n < 64) { s_rt_latest[s_rt_latest_n].off = off; s_rt_latest[s_rt_latest_n].slot = slot; s_rt_latest_n++; }
}
static int off_rt_find(u32 off);
static int off_rt_latest(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < s_rt_latest_n; i++)
        if (s_rt_latest[i].off == off) {
            int sl = s_rt_latest[i].slot;
            if (sl >= 0 && sl < MAX_OFF_RTS && s_d3d.off_rt[sl].res && s_d3d.off_rt[sl].off == off) return sl;
        }
    return off_rt_find(off);
}

static int off_rt_find(u32 off)
{
    if (!off) return -1;
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (s_d3d.off_rt[i].res && s_d3d.off_rt[i].off == off) return i;
    return -1;
}

/* Ensure an RT resource exists for this surface; (re)creates the RTV at
 * rt_rtv_heap[i] and the SRV at srv_heap[RT_SRV_BASE+i]. */
static int off_rt_get(u32 off, u32 w, u32 h, u32 rsx_fmt)
{
    if (!off || !s_d3d.rt_rtv_heap || !s_d3d.srv_heap) return -1;
    if (!w) w = s_d3d.width;
    if (!h) h = s_d3d.height;
    const u32 guest_w = w, guest_h = h;
    particle_size(&w, &h);
    DXGI_FORMAT want_fmt = rsx_surface_dxgi(rsx_fmt);
    /* RT_FORCE_RGBA8=1: allocate every offscreen target as R8G8B8A8 rather than
     * its native format. A title whose final composite reads a half-float
     * surface cannot be shown with a plain CopyTextureRegion, and giving it an
     * 8-bit target costs HDR precision but makes the composite a same-format
     * copy. A stopgap for looking at the frame, not a rendering fix. */
    { static int f8 = -1;
      if (f8 < 0) { const char* e = getenv("RT_FORCE_RGBA8"); f8 = e ? atoi(e) : 0; }
      if (f8) want_fmt = DXGI_FORMAT_R8G8B8A8_UNORM; }
    /* Exact match on offset + size + format: aliases at one offset are
     * different surfaces and each keeps its own resource. */
    int slot = -1;
    for (int i = 0; i < MAX_OFF_RTS; i++) {
        OffRT* c = &s_d3d.off_rt[i];
        if (c->res && c->off == off && c->w == w && c->h == h && c->dxgi == (u32)want_fmt) {
            c->used = 1; return i;
        }
    }
    for (int i = 0; i < MAX_OFF_RTS; i++)
        if (!s_d3d.off_rt[i].res) { slot = i; break; }
    if (slot < 0) {   /* evict an entry not used this frame; free it after the GPU is done */
        for (int i = 0; i < MAX_OFF_RTS; i++)
            if (!s_d3d.off_rt[i].used) { slot = i; break; }
        if (slot < 0) { static int _f = 0; if ((_f < 4 && _f++ < 4)) printf("[D3D12] offscreen RT table full (%d)\n", MAX_OFF_RTS); return -1; }
        for (int i = 0; i < s_rt_latest_n; i++) if (s_rt_latest[i].slot == slot) s_rt_latest[i].slot = -1;
        defer_release(s_d3d.off_rt[slot].res); s_d3d.off_rt[slot].res = NULL;
        defer_release(s_d3d.off_rt[slot].up);  s_d3d.off_rt[slot].up = NULL;
        defer_release(s_d3d.off_rt[slot].feedback); s_d3d.off_rt[slot].feedback = NULL;
    }
    OffRT* r = &s_d3d.off_rt[slot];
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {0};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = want_fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = td.Format;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, &cv,
            &IID_ID3D12Resource, (void**)&r->res)))
        return -1;
    r->off = off; r->w = w; r->h = h; r->dxgi = (u32)want_fmt; r->cwritten = 0;
    r->st = D3D12_RESOURCE_STATE_COPY_DEST;
    r->used = 1;

    /* Populate the new RT from guest memory: titles CPU-initialise their
     * render-target buffers (wave fills the height fields with texels of
     * (0,0,0, 1.0f) -- the .w is 'inverse of mass'; from an all-zero GPU
     * resource the water simulation can never boot). Guest data is
     * big-endian and tightly packed. */
    {
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveOffset(u32);
        u32 bpp = (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8u :
                  (want_fmt == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16u :
                  (want_fmt == DXGI_FORMAT_R32_FLOAT) ? 4u : 4u;
        u32 pitch = (w * bpp + 255) & ~255u;
        u32 src = cellGcmResolveOffset(off);
        if (vm_base && src != 0xFFFFFFFFu) {
            D3D12_HEAP_PROPERTIES hu = {0}; hu.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bd = {0};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (u64)pitch * h; bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hu, D3D12_HEAP_FLAG_NONE, &bd,
                    D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                    &IID_ID3D12Resource, (void**)&r->up))) {
                void* mp = NULL; D3D12_RANGE nr = {0,0};
                if (SUCCEEDED(r->up->lpVtbl->Map(r->up, 0, &nr, &mp)) && mp) {
                    const u8* sp = vm_base + src;
                    for (u32 y = 0; y < h; y++) {
                        const u8* srow = sp + (u64)((u64)y * guest_h / h) * guest_w * bpp;
                        u8* drow = (u8*)mp + (u64)y * pitch;
                        if (want_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
                            /* guest A8R8G8B8 (bytes A,R,G,B) -> R,G,B,A */
                            for (u32 x = 0; x < w; x++) {
                                u32 sx = (u32)((u64)x * guest_w / w);
                                drow[x*4+0] = srow[sx*4+1];
                                drow[x*4+1] = srow[sx*4+2];
                                drow[x*4+2] = srow[sx*4+3];
                                drow[x*4+3] = srow[sx*4+0];
                            }
                        } else if (want_fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            for (u32 x = 0; x < w * 4; x++) {   /* u16 halves, BE */
                                u32 sx = (u32)((u64)(x / 4) * guest_w / w) * 4 + x % 4;
                                drow[x*2+0] = srow[sx*2+1];
                                drow[x*2+1] = srow[sx*2+0];
                            }
                        } else {                                 /* u32 floats, BE */
                            u32 nw = (w * bpp) / 4;
                            for (u32 x = 0; x < nw; x++) {
                                u32 nc = bpp / 4, sx = (u32)((u64)(x / nc) * guest_w / w) * nc + x % nc;
                                drow[x*4+0] = srow[sx*4+3];
                                drow[x*4+1] = srow[sx*4+2];
                                drow[x*4+2] = srow[sx*4+1];
                                drow[x*4+3] = srow[sx*4+0];
                            }
                        }
                    }
                    r->up->lpVtbl->Unmap(r->up, 0, NULL);
                    D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                    cdst.pResource = r->res;
                    cdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    csrc.pResource = r->up;
                    csrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    csrc.PlacedFootprint.Footprint.Format   = want_fmt;
                    csrc.PlacedFootprint.Footprint.Width    = w;
                    csrc.PlacedFootprint.Footprint.Height   = h;
                    csrc.PlacedFootprint.Footprint.Depth    = 1;
                    csrc.PlacedFootprint.Footprint.RowPitch = pitch;
                    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                }
            }
        }
        {
            D3D12_RESOURCE_BARRIER b = {0};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = r->res;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            r->st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    RSX_D3D12_CPU_HEAP_START(s_d3d.rt_rtv_heap)(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    s_d3d.device->lpVtbl->CreateRenderTargetView(s_d3d.device, r->res, NULL, rh);

    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = td.Format;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(RT_SRV_BASE + slot) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r->res, &sv, sh);

    static int _log = 0;
    if ((_log < 8 && _log++ < 8))
        printf("[D3D12] offscreen RT %d: off=0x%X %ux%u (render-to-texture)\n",
               slot, off, w, h);
    return slot;
}

static D3D12_CPU_DESCRIPTOR_HANDLE off_rt_rtv(int slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE rh;
    RSX_D3D12_CPU_HEAP_START(s_d3d.rt_rtv_heap)(s_d3d.rt_rtv_heap, &rh);
    rh.ptr += (u64)slot * s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
        s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return rh;
}

/* Write a texture SRV (or a null SRV when res == NULL) at an absolute SRV
 * heap slot. Used to fill per-draw t0-t3 descriptor windows. */
static struct { ID3D12Resource* res; u32 fmt, map, cube, gen; } s_srvsh[2 * MAX_DRAWS * RSX_D3D_TEX_UNITS];   /* v225A: x2 for RF_EARLY halves */
static u32 s_dsrv_base = DRAW_SRV_BASE;   /* v225A: per-draw window base of the frame being prepared (RF_EARLY flips halves) */
static void srv_write_ex(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt,
                         UINT mapping, int cube)
{
    static int dd = -1;
    if (dd < 0) { const char* e = getenv("SRV_DEDUPE"); dd = (e && *e == '0') ? 0 : 1; }
    if (dd && !s_bis_nosrvdd && heap_slot >= DRAW_SRV_BASE &&
        heap_slot - DRAW_SRV_BASE < (u32)((rf_early_on() ? 2 : 1) * MAX_DRAWS * RSX_D3D_TEX_UNITS)) {
        u32 i = heap_slot - DRAW_SRV_BASE;
        if (s_srvsh[i].gen == s_srv_gen && s_srvsh[i].res == res && s_srvsh[i].fmt == (u32)fmt &&
            s_srvsh[i].map == (u32)mapping && s_srvsh[i].cube == (u32)(cube != 0)) {
            if (ps3_telem_enabled()) s_sf_srv_skips++;
            return;
        }
        s_srvsh[i].gen = s_srv_gen; s_srvsh[i].res = res; s_srvsh[i].fmt = (u32)fmt;
        s_srvsh[i].map = (u32)mapping; s_srvsh[i].cube = (u32)(cube != 0);
    }
    if (ps3_telem_enabled()) s_sf_srv_writes++;
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = mapping;
    sv.Texture2D.MipLevels = (UINT)-1;   /* v146: every level the resource has (uploaded textures carry mip chains now) */
    if (cube) {   /* must match the TextureCube declaration in the HLSL */
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = (UINT)-1;
        sv.TextureCube.MostDetailedMip = 0;
        sv.TextureCube.ResourceMinLODClamp = 0.0f;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &h);
    h.ptr += (u64)heap_slot * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, res, &sv, h);
}

static void srv_write(u32 heap_slot, ID3D12Resource* res, DXGI_FORMAT fmt, UINT mapping)
{
    srv_write_ex(heap_slot, res, fmt, mapping, 0);
}

static void off_rt_transition(int slot, D3D12_RESOURCE_STATES to)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r->res;
    b.Transition.StateBefore = r->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->st = to;
}

static ID3D12Resource* off_rt_feedback_get(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (r->feedback) return r->feedback;
    D3D12_RESOURCE_DESC td;
    RSX_D3D12_RESOURCE_DESC(r->res)(r->res, &td);
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,
        &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, NULL,
        &IID_ID3D12Resource, (void**)&r->feedback);
    r->feedback_st = D3D12_RESOURCE_STATE_COPY_DEST;
    fprintf(stderr, "[RT80] feedback snapshot %ux%u off=0x%X %s\n",
            r->w, r->h, r->off, SUCCEEDED(hr) ? "ready" : "FAILED");
    return r->feedback;
}

static void off_rt_feedback_copy(int slot)
{
    OffRT* r = &s_d3d.off_rt[slot];
    if (!r->feedback) return;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r->feedback;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (r->feedback_st != D3D12_RESOURCE_STATE_COPY_DEST) {
        b.Transition.StateBefore = r->feedback_st;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    }
    s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list, r->feedback, r->res);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    r->feedback_st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    off_rt_transition(slot, D3D12_RESOURCE_STATE_RENDER_TARGET);
}

/* ===========================================================================
 * Depth surfaces (zeta), 2026-10-04 evening.
 *
 * The backend used ONE shared 2048x2048 depth buffer for every pass and cleared
 * it on every render-target switch; guest depth clears were ignored. RSX keeps
 * a depth surface (zeta) per VRAM offset, cleared only by CLEAR_SURFACE, and
 * games read it back as a texture: inFamous' deferred lighting/fog/post sample
 * zeta 0x1010000 as an A8R8G8B8 texture. With no surface behind that address
 * the backend uploaded whatever stale bytes guest VRAM held there -- zero (the
 * whole lit scene black) or leftovers from earlier screens (a dim, smeared
 * picture), which is why the title screen came and went between identical runs.
 *
 * Now: one D3D12 depth buffer per (zeta offset, size), kept across passes and
 * frames, cleared only by the guest. A draw that samples a zeta offset reads a
 * colour copy made right before it by a small pass that packs the 24-bit depth
 * into the bytes an A8R8G8B8 fetch of a D24S8 word returns on RSX:
 * A = depth[23:16], R = depth[15:8], G = depth[7:0], B = stencil (0 here).
 * ZETA_SURFACES=0 restores the old shared/clear-on-switch buffer.
 * ZCONV_ORDER=1 packs R=hi,G=mid,B=lo,A=1 instead (if the title decodes .rgb).
 * ======================================================================== */
typedef struct {
    ID3D12Resource*       res;
    u32                   off, w, h;
    D3D12_RESOURCE_STATES st;
    int                   need_init;   /* contents undefined until the first clear */
    int                   dirty;       /* drawn since the last colour conversion */
    int                   conv_rt;     /* OffRT slot of its colour copy, -1 none */
} ZSurf;
static ZSurf s_zs[MAX_ZS];
static ID3D12DescriptorHeap* s_zs_dsv_heap;
static u32 s_zs_dsv_inc;
static ID3D12PipelineState* s_zconv_pso;
static int zs_on(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("ZETA_SURFACES"); on = (e && *e == '0') ? 0 : 1; }
    return on;
}
static int zs_find_off(u32 off, u32 w, u32 h)
{
    if (!off) return -1;
    particle_size(&w, &h);
    int best = -1;
    for (int i = 0; i < MAX_ZS; i++)
        if (s_zs[i].res && s_zs[i].off == off) {
            if (s_zs[i].w == w && s_zs[i].h == h) return i;    /* exact size first */
            if (best < 0 || s_zs[i].dirty) best = i;
        }
    return best;
}
static D3D12_CPU_DESCRIPTOR_HANDLE zs_dsv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    RSX_D3D12_CPU_HEAP_START(s_zs_dsv_heap)(s_zs_dsv_heap, &h);
    h.ptr += (u64)i * s_zs_dsv_inc;
    return h;
}
static void zs_transition(int i, D3D12_RESOURCE_STATES to)
{
    ZSurf* z = &s_zs[i];
    if (!z->res || z->st == to) return;
    D3D12_RESOURCE_BARRIER b = {0};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = z->res;
    b.Transition.StateBefore = z->st;
    b.Transition.StateAfter  = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
    z->st = to;
}
/* v33: pass capture of a depth surface: both planes (depth, stencil) via copyable footprints.
 * Written as pNNN_..._s<100+i>_<off>_<w>x<h>_d<fmt>.raw (plane 0 = depth, then plane 1). */
static void cp_snap_zs(int zi, u32 op1)
{
    if (!s_cp_active || zi < 0 || zi >= MAX_ZS || !s_zs[zi].res) return;
    { static int z = -1; if (z < 0) { const char* e = getenv("CAP_PASS_Z"); z = (e && *e == '0') ? 0 : 1; }
      if (!z) return; }   /* v71 CAP_PASS_Z=0: no depth/stencil snapshots */
    ZSurf* z = &s_zs[zi];
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = z->w; rd.Height = z->h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R24G8_TYPELESS; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT lay[2]; UINT nrow[2]; UINT64 rsz[2]; UINT64 total = 0;
    UINT nsub = 2;
    s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &rd, 0, 2, 0, lay, nrow, rsz, &total);
    if (total == ~0ull || !total) { nsub = 1; s_d3d.device->lpVtbl->GetCopyableFootprints(s_d3d.device, &rd, 0, 1, 0, lay, nrow, rsz, &total); }
    if (total == ~0ull || !total) return;
    zs_transition(zi, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT sb = 0; sb < nsub && s_cp_n < CP_MAX; sb++) {
        u64 o = (s_cp_used + 511) & ~(u64)511;
        u64 sz = (u64)lay[sb].Footprint.RowPitch * nrow[sb];
        if (o + sz > s_cp_size) return;
        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_cp_rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = lay[sb]; dst.PlacedFootprint.Offset = o;
        src.pResource = z->res; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = sb;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);
        s_cp[s_cp_n].op0 = s_cp_lastop; s_cp[s_cp_n].op1 = op1; s_cp[s_cp_n].slot = 100 + zi * 2 + (int)sb;
        s_cp[s_cp_n].off = z->off; s_cp[s_cp_n].w = lay[sb].Footprint.Width; s_cp[s_cp_n].h = nrow[sb];
        s_cp[s_cp_n].dxgi = lay[sb].Footprint.Format; s_cp[s_cp_n].bpp = 0;
        s_cp[s_cp_n].pitch = lay[sb].Footprint.RowPitch; s_cp[s_cp_n].rowb = (u32)rsz[sb]; s_cp[s_cp_n].rbo = o; s_cp_n++;
        s_cp_used = o + sz;
    }
}
static int zs_get(u32 off, u32 w, u32 h)
{
    if (!zs_on() || !off || !s_d3d.device) return -1;
    if (!w || !h) { w = s_d3d.width; h = s_d3d.height; }
    particle_size(&w, &h);
    for (int i = 0; i < MAX_ZS; i++)
        if (s_zs[i].res && s_zs[i].off == off && s_zs[i].w == w && s_zs[i].h == h) return i;
    if (!s_zs_dsv_heap) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {0};
        hd.NumDescriptors = MAX_ZS; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(s_d3d.device->lpVtbl->CreateDescriptorHeap(s_d3d.device, &hd,
                &IID_ID3D12DescriptorHeap, (void**)&s_zs_dsv_heap))) return -1;
        s_zs_dsv_inc = s_d3d.device->lpVtbl->GetDescriptorHandleIncrementSize(
            s_d3d.device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    }
    int slot = -1;
    for (int i = 0; i < MAX_ZS; i++) if (!s_zs[i].res) { slot = i; break; }
    if (slot < 0) { static int _w = 0; if ((_w < 4 && _w++ < 4))
        fprintf(stderr, "[ZETA] table full (%d) -- 0x%X %ux%u uses the shared depth buffer%c", MAX_ZS, off, w, h, 10);
        return -1; }
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R24G8_TYPELESS;          /* DSV D24S8 + SRV R24_X8 */
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE cv = {0};
    cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
    ID3D12Resource* r = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE,
            &rd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, &IID_ID3D12Resource, (void**)&r)) || !r) {
        static int _e = 0; if ((_e < 4 && _e++ < 4)) fprintf(stderr, "[ZETA] create %ux%u FAILED%c", w, h, 10);
        return -1;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv = {0};
    dv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    s_d3d.device->lpVtbl->CreateDepthStencilView(s_d3d.device, r, &dv, zs_dsv(slot));
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
    sv.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    D3D12_CPU_DESCRIPTOR_HANDLE sh;
    RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &sh);
    sh.ptr += (u64)(ZS_SRV_BASE + (u32)slot * RSX_D3D_TEX_UNITS) * s_d3d.srv_inc;
    s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r, &sv, sh);
    {   /* v156 ZCONV_STENCIL: window slot t1 = the stencil plane (read by the depth->colour copy) */
        D3D12_SHADER_RESOURCE_VIEW_DESC ssv = sv;
        ssv.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
        ssv.Texture2D.PlaneSlice = 1;
        sh.ptr += s_d3d.srv_inc;
        s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, r, &ssv, sh);
    }
    ZSurf* z = &s_zs[slot];
    z->res = r; z->off = off; z->w = w; z->h = h; z->st = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    z->need_init = 1; z->dirty = 0; z->conv_rt = -1;
    fprintf(stderr, "[ZETA] depth surface %d: off=0x%X %ux%u%c", slot, off, w, h, 10);
    return slot;
}
/* Clear before first use: a fresh resource's contents are undefined. */
static void zs_init_if_needed(int i)
{
    if (i < 0 || !s_zs[i].need_init) return;
    zs_transition(i, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zs_dsv(i),
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
    s_zs[i].need_init = 0;
}
/* v156 ZCONV_STENCIL (default 1; 0 = old): a D24S8 zeta read as A8R8G8B8 is, per 32-bit word,
 * A = depth[23:16], R = depth[15:8], G = depth[7:0], B = STENCIL. The colour copy wrote B = 0.
 * inFamous' sun/moon light pass (fp 0x00BC7302, fph A0009BB2) reads tex2 (= this copy): depth from
 * .wxy and `frac(floor(z*255+0.5)/2) >= 0.5` = stencil bit 0, which the G-buffer writes (REPLACE,
 * wmask 7) for characters (sref 1/5: Cole, civilians) and some static materials (sref 3/7). Bit 0
 * selects the hemisphere ambient (constants, desaturated to luminance) instead of the ambient-map
 * lookup tex5(N.xy) at full colour. With B = 0 every surface took the coloured map path: at night
 * a saturated navy ambient wherever the sun/moon is shadowed (blue brick) and the "cold/flat"
 * look on Cole (v93 note). LOD_SKY_ONLY=2's private stencil bit 0x80 is masked out. */
static int zconv_stencil_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("ZCONV_STENCIL"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[ZETA] v156 depth->colour copy carries the stencil byte in B (ZCONV_STENCIL) %s%c",
                         v ? "ON" : "off", 10); }
    return v;
}
static void zs_build_conv_pso(void)
{
    if (s_zconv_pso || !s_d3d.vp_root_sig) return;
    static const char vs[] =
        "struct O { float4 p : SV_Position; };\n"
        "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n";
    static char ps[1024];
    int order = 0; { const char* e = getenv("ZCONV_ORDER"); if (e) order = atoi(e); }
    const int zst = zconv_stencil_on();
    const unsigned smask = (lod_sky_on() == 2) ? 0x7Fu : 0xFFu;   /* v147 private bit 0x80 */
    char sline[160];
    if (zst) snprintf(sline, sizeof sline, "  float st = (float)(zs.Load(int3(int2(p.xy), 0)).y & %uu) / 255.0;\n", smask);
    else     snprintf(sline, sizeof sline, "  float st = 0.0;\n");
    snprintf(ps, sizeof ps,
        "Texture2D<float> zt : register(t0);\n"
        "Texture2D<uint2> zs : register(t1);\n"
        "float4 main(float4 p : SV_Position) : SV_Target {\n"
        "  float d = zt.Load(int3(int2(p.xy), 0));\n"
        "  uint v = (uint)round(saturate(d) * 16777215.0);\n"
        "  float hi = ((v >> 16) & 255) / 255.0, mid = ((v >> 8) & 255) / 255.0, lo = (v & 255) / 255.0;\n"
        "%s"
        "  %s\n}\n",
        sline,
        order == 1 ? "return float4(hi, mid, lo, 1.0);"
                   : "return float4(mid, lo, st, hi);");
    ID3DBlob *vb = NULL, *pb = NULL, *e = NULL;
    HRESULT hr = D3DCompile(vs, sizeof(vs) - 1, "zconv_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (e) { e->lpVtbl->Release(e); e = NULL; }
    if (SUCCEEDED(hr)) hr = D3DCompile(ps, strlen(ps), "zconv_ps", NULL, NULL, "main", "ps_5_0", 0, 0, &pb, &e);
    if (e) { fprintf(stderr, "[ZETA] conv shader: %s%c", (const char*)e->lpVtbl->GetBufferPointer(e), 10);
             e->lpVtbl->Release(e); }
    if (FAILED(hr) || !vb || !pb) { if (vb) vb->lpVtbl->Release(vb); if (pb) pb->lpVtbl->Release(pb); return; }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = s_d3d.vp_root_sig;
    pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
    pd.PS.pShaderBytecode = pb->lpVtbl->GetBufferPointer(pb); pd.PS.BytecodeLength = pb->lpVtbl->GetBufferSize(pb);
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pd.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pd.SampleDesc.Count = 1;
    hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
            &IID_ID3D12PipelineState, (void**)&s_zconv_pso);
    fprintf(stderr, "[ZETA] depth->colour conversion pipeline %s (order %d)%c",
            SUCCEEDED(hr) ? "ready" : "FAILED", order, 10);
    vb->lpVtbl->Release(vb); pb->lpVtbl->Release(pb);
}
/* v147 LOD_SKY_ONLY=2: stencil bit 0x80 = "stored depth >= LOD_SKY_Z" (see lod_sky_on). Full-screen
 * triangle at z = LOD_SKY_Z, depth test LEQUAL (no write): depth pass -> REPLACE (ref 0x80), depth fail
 * -> ZERO (pso 0, first coarse run after a depth/stencil clear) or KEEP (pso 1, later runs: pixels the
 * earlier coarse meshes covered stay in the region). Write mask 0x80 only. No colour targets. */
static ID3D12PipelineState* s_lsk_mark_pso[2];
static u8 s_lsk_marked[MAX_ZS];   /* zeta i marked since its last depth/stencil clear */
static void lsk_build_mark_pso(void)
{
    static int tried = 0;
    if (tried || !s_d3d.vp_root_sig) return;
    tried = 1;
    char vs[512];
    snprintf(vs, sizeof vs,
        "struct O { float4 p : SV_Position; };\n"
        "O main(uint id : SV_VertexID) { O o; float2 t = float2((id << 1) & 2, id & 2);\n"
        "  o.p = float4(t * float2(2, -2) + float2(-1, 1), %.9f, 1); return o; }\n", (double)lsk_sky_z());
    ID3DBlob *vb = NULL, *e = NULL;
    HRESULT hr = D3DCompile(vs, strlen(vs), "lsk_mark_vs", NULL, NULL, "main", "vs_5_0", 0, 0, &vb, &e);
    if (e) { fprintf(stderr, "[LODSKY] mark shader: %s%c", (const char*)e->lpVtbl->GetBufferPointer(e), 10);
             e->lpVtbl->Release(e); }
    if (FAILED(hr) || !vb) { if (vb) vb->lpVtbl->Release(vb); fprintf(stderr, "[LODSKY] v147 mark shader FAILED%c", 10); return; }
    for (int add = 0; add < 2; add++) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd; memset(&pd, 0, sizeof pd);
        pd.pRootSignature = s_d3d.vp_root_sig;
        pd.VS.pShaderBytecode = vb->lpVtbl->GetBufferPointer(vb); pd.VS.BytecodeLength = vb->lpVtbl->GetBufferSize(vb);
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 0;
        pd.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
        pd.SampleDesc.Count = 1;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        pd.DepthStencilState.StencilEnable = TRUE;
        pd.DepthStencilState.StencilReadMask = 0xFF;
        pd.DepthStencilState.StencilWriteMask = (UINT8)LSK_BIT;
        D3D12_DEPTH_STENCILOP_DESC so;
        so.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        so.StencilDepthFailOp = add ? D3D12_STENCIL_OP_KEEP : D3D12_STENCIL_OP_ZERO;
        so.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        so.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pd.DepthStencilState.FrontFace = so; pd.DepthStencilState.BackFace = so;
        hr = s_d3d.device->lpVtbl->CreateGraphicsPipelineState(s_d3d.device, &pd,
                &IID_ID3D12PipelineState, (void**)&s_lsk_mark_pso[add]);
        if (FAILED(hr)) s_lsk_mark_pso[add] = NULL;
        fprintf(stderr, "[LODSKY] v147 sky-region mark pipeline %d %s (z %.7f, hr 0x%08lX)%c", add,
                SUCCEEDED(hr) ? "ready" : "FAILED", (double)lsk_sky_z(), (unsigned long)hr, 10);
    }
    vb->lpVtbl->Release(vb);
}
/* Record the mark pass on zeta i (or the shared buffer, i < 0). Leaves NO colour target bound:
 * the caller must force the target rebind (cur_rt = -99, cur_zs = -2) and the stencil ref. */
static int lsk_mark(int i, D3D12_CPU_DESCRIPTOR_HANDLE shared_dsv)
{
    lsk_build_mark_pso();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = shared_dsv;
    u32 w = BBW, h = BBH;   /* v154 */
    int add = 0;
    if (i >= 0) {
        if (!s_zs[i].res) return 0;
        zs_transition(i, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        if (s_zs[i].need_init) s_lsk_marked[i] = 0;
        zs_init_if_needed(i);
        dsv = zs_dsv(i); w = s_zs[i].w; h = s_zs[i].h;
        add = s_lsk_marked[i] ? 1 : 0;
    }
    ID3D12PipelineState* p = s_lsk_mark_pso[add];
    if (!p) { if (!s_lsk_broken) fprintf(stderr, "[LODSKY] v147 no mark pipeline -> falling back to LOD_SKY_ONLY=1%c", 10);
              s_lsk_broken = 1; return 0; }
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 0, NULL, FALSE, &dsv);
    D3D12_VIEWPORT vp = {0, 0, (float)w, (float)h, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)w, (LONG)h};
    cmd_rect(&vp, &sc);
    cmd_pso(p);
    s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list, LSK_BIT);
    s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 0, 0);
    if (i >= 0) s_lsk_marked[i] = 1;   /* stencil only: the depth colour copy stays valid */
    { static int n = 0; if ((n < 6 && n++ < 6))
        fprintf(stderr, "[LODSKY] v147 sky-region mark on zeta %d (0x%X %ux%u, %s)%c", i,
                i >= 0 ? s_zs[i].off : 0u, w, h, add ? "additive" : "full", 10); }
    return 1;
}
/* Colour copy (OffRT, R8G8B8A8) of depth surface i, created on demand. The
 * OffRT is keyed by the zeta offset but never registered as "latest" for it,
 * so nothing else mistakes it for a colour surface. */
static int zs_color_rt(int i)
{
    ZSurf* z = &s_zs[i];
    if (z->conv_rt >= 0 && s_d3d.off_rt[z->conv_rt].res && s_d3d.off_rt[z->conv_rt].off == z->off &&
        s_d3d.off_rt[z->conv_rt].w == z->w && s_d3d.off_rt[z->conv_rt].h == z->h) {
        s_d3d.off_rt[z->conv_rt].used = 1;
        return z->conv_rt;
    }
    z->conv_rt = off_rt_get(z->off, z->w, z->h, 0x8 /* A8R8G8B8 */);
    return z->conv_rt;
}
/* v49: is OffRT slot sl only the colour copy of a depth surface? That copy is created
 * at the depth's own offset, so after the first conversion off_rt_latest(depth off)
 * found it and the "a real colour target wins" test below skipped the depth path for
 * good: the copy was never refreshed again. inFamous converts 0x1010000 once at the
 * title screen, and every lighting pass of the game then rebuilt positions from the
 * TITLE SCREEN depth -- the city-silhouette overlay, see-through NPC limbs.
 * ZCONV_STALE=1 restores the old behaviour. */
static int zs_is_conv_slot(int sl)
{
    static int stale = -1;
    if (stale < 0) { const char* e = getenv("ZCONV_STALE"); stale = (e && *e == '1') ? 1 : 0;
        fprintf(stderr, "[ZETA] v49 depth colour copy refreshed every use %s (ZCONV_STALE)%c", stale ? "OFF" : "ON", 10); }
    if (stale || sl < 0) return 0;
    for (int q = 0; q < MAX_ZS; q++) if (s_zs[q].res && s_zs[q].conv_rt == sl) return 1;
    return 0;
}
/* Run the conversion now (inside the replay, before the sampling draw). */
static void zs_convert(int i, int crt)
{
    ZSurf* z = &s_zs[i];
    zs_build_conv_pso();
    if (!s_zconv_pso || !z->res || crt < 0 || !s_d3d.off_rt[crt].res) return;
    if (ps3_telem_enabled()) s_sf_zconversions++;
    zs_init_if_needed(i);
    zs_transition(i, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    off_rt_transition(crt, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = off_rt_rtv(crt);
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv, FALSE, NULL);
    D3D12_VIEWPORT vp = {0, 0, (float)z->w, (float)z->h, 0.0f, 1.0f};
    D3D12_RECT sc = {0, 0, (LONG)z->w, (LONG)z->h};
    cmd_rect(&vp, &sc);
    cmd_pso(s_zconv_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    RSX_D3D12_GPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &g);
    g.ptr += (u64)(ZS_SRV_BASE + (u32)i * RSX_D3D_TEX_UNITS) * s_d3d.srv_inc;
    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, g);
    s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 0, 0);
    zs_transition(i, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    off_rt_transition(crt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    z->dirty = 0;
    { static int _n = 0; if ((_n < 6 && _n++ < 6))
        fprintf(stderr, "[ZETA] converted depth 0x%X %ux%u -> colour copy (rt slot %d)%c",
                z->off, z->w, z->h, crt, 10); }
}

/* Set by the present entry point: 1 = this batch ends with a swapchain
 * Present, 0 = execute the recorded draws only. An OFFSCREEN-only batch (a
 * render-to-texture pass) must still run -- it is what fills the texture a
 * later batch samples -- but presenting it would show a half-built frame. */
static int s_present_this_frame = 1;

/* Resolve a DRAW_*_TEX value: a hex raw offset, or the literal "duck" for the
 * content-identified duck texture. */
static u32 draw_filter_tex(const char* e)
{
    if (!e) return 0;
    if (e[0] == 'd' && e[1] == 'u') return s_duck_raw;
    return (u32)strtoul(e, NULL, 16);
}

/* Copy the current backbuffer into s_screen_copy. Called mid-frame the moment
 * the reduced-viewport passes finish -- this title renders its reflection into
 * the TOP-LEFT REGION of the render target and the main scene then overwrites
 * it, so a snapshot taken at end of frame contains the scene, not the
 * reflection. Also called at end of frame as a fallback for frames with no
 * reduced-viewport pass. */
static void screen_copy_capture(u32 fi)
{
    static int en = -1;
    if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
    if (!en || !s_d3d.device) return;
            if (!s_screen_copy) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC td = {0};
                td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                td.Width = BBW; td.Height = BBH;   /* v154: CopyResource from the backbuffer */
                td.DepthOrArraySize = 1; td.MipLevels = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                        s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, NULL,
                        &IID_ID3D12Resource, (void**)&s_screen_copy)))
                    s_screen_copy = NULL;
            }
            if (s_screen_copy) {
                D3D12_RESOURCE_BARRIER bb[2] = {0};
                bb[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb[0].Transition.pResource   = s_d3d.render_targets[fi];
                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bb[1] = bb[0];
                bb[1].Transition.pResource   = s_screen_copy;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                s_d3d.cmd_list->lpVtbl->CopyResource(s_d3d.cmd_list,
                    s_screen_copy, s_d3d.render_targets[fi]);

                bb[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bb[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, bb);

                /* SCREENCOPY_DUMP=1: stage the captured image into the readback
                 * buffer so it can be written out and LOOKED AT. Claiming a
                 * capture happened is not the same as showing what is in it. */
                { static int sd = -1;
                  if (sd < 0) { const char* e = getenv("SCREENCOPY_DUMP"); sd = e ? atoi(e) : 0; }
                  if (sd && s_d3d.readback_buf) {
                      D3D12_RESOURCE_BARRIER t = {0};
                      t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                      t.Transition.pResource   = s_screen_copy;
                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);

                      D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                      cd.pResource = s_d3d.readback_buf;
                      cd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      cd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
                      cd.PlacedFootprint.Footprint.Width    = BBW;
                      cd.PlacedFootprint.Footprint.Height   = BBH;
                      cd.PlacedFootprint.Footprint.Depth    = 1;
                      cd.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
                      cs.pResource = s_screen_copy;
                      cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                      s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);

                      t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                      t.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                      s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &t);
                      s_sc_dump_pending = 1;
                  } }
            }

}

/* rsx_topology -> D3D_PRIMITIVE_TOPOLOGY. Spelled out rather than relying on
 * the two enums happening to agree: the draw record carries the neutral value
 * so a second backend can read it, and this is the one place it becomes a D3D
 * one. */
static D3D12_PRIMITIVE_TOPOLOGY topo_to_d3d(u32 t)
{
    switch (t) {
    case RSX_TOPOLOGY_POINTS:         return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    case RSX_TOPOLOGY_LINES:          return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    case RSX_TOPOLOGY_LINE_STRIP:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case RSX_TOPOLOGY_TRIANGLE_STRIP: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case RSX_TOPOLOGY_TRIANGLES:      return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                          return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* GCM_GUEST_FB=<off>,<w>,<h>,<pitch> -- present the guest display buffer.
 *
 * A title that composites its final image with the RSX 2D engine assembles it
 * in GUEST memory: nv3089_blit is a CPU copy and never touches D3D12, and the
 * draw pass below only runs when a recorded draw targets the display surface,
 * which for such a title never happens. The frame is finished, in the wrong
 * memory, and the swapchain shows only the clear. Copy it across.
 *
 * Off unless the offset and geometry are given, so nothing else changes. */
/* GPU-side composite of an offscreen RT onto the backbuffer.
 *
 * A title that finishes its frame with the RSX 2D engine blits a rendered
 * surface into the display buffer. That surface lives in guest RAM, which the
 * D3D12 path never writes -- the scene is on the GPU -- so the CPU copy in
 * nv3089_blit faithfully moves zeros. The backend already holds the same
 * surface as a texture, so do the composite here instead. Triggered from
 * cellGcmSys under GCM_COMPOSITE_RT. */
static u32 s_composite_src = 0;
static int s_last_drawn_slot = -1;
static u32 s_last_drawn_off  = 0;

void rsx_d3d12_composite_from_offscreen(u32 raw_off) { s_composite_src = raw_off; }

static void composite_present(u32 fi)
{
    u32 want = s_composite_src;
    s_composite_src = 0;
    if (!want) return;
    /* want == 2 means "whatever this frame's draws targeted" -- the one thing
     * guaranteed to hold geometry, and immune to the offset collision above. */
    int i;
    if (want == 2u) { i = s_last_drawn_slot; want = s_last_drawn_off; }
    else            { i = off_rt_latest(want); }
    if (i < 0 || !s_d3d.off_rt[i].res) return;
    OffRT* r = &s_d3d.off_rt[i];
    /* Same-format copy only; a half-float source would need a converting draw. */
    if (r->dxgi != DXGI_FORMAT_R8G8B8A8_UNORM) {
        static int w1 = 0;
        if ((w1 < 2 && w1++ < 2))
            fprintf(stderr, "[COMPOSITE] RT 0x%08X is dxgi=%u, not R8G8B8A8 -- needs a draw%c",
                    want, r->dxgi, 10);
        return;
    }
    u32 cw = r->w < BBW ? r->w : BBW;
    u32 ch = r->h < BBH ? r->h : BBH;
    if (!cw || !ch) return;
    { static int n = 0;
      if ((n < 3 && n++ < 3)) fprintf(stderr, "[COMPOSITE] RT 0x%08X %ux%u -> backbuffer%c",
                           want, cw, ch, 10); }

    D3D12_RESOURCE_BARRIER b[2] = {0};
    b[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[0].Transition.pResource   = r->res;
    b[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[0].Transition.StateBefore = r->st;
    b[0].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[1] = b[0];
    b[1].Transition.pResource   = s_d3d.render_targets[fi];
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = r->res;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; sl.SubresourceIndex = 0;
    D3D12_BOX box = {0};
    box.left = 0; box.top = 0; box.front = 0;
    box.right = cw; box.bottom = ch; box.back = 1;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, &box);

    b[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b[0].Transition.StateAfter  = r->st;
    b[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b[1].Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 2, b);
}


static void guest_fb_present(u32 fi)
{
    static int inited = 0;
    static u32 off, gw, gh, gpitch, up_pitch;
    static ID3D12Resource* up = NULL;
    if (!inited) {
        inited = 1;
        const char* e = getenv("GCM_GUEST_FB");
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (!e || sscanf(e, "%i,%u,%u,%u", &a, &b, &c, &d) != 4 || !b || !c) return;
        off = a; gw = b; gh = c; gpitch = d;
        up_pitch = (gw * 4u + 255u) & ~255u;      /* D3D12 wants 256-byte rows */
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {0};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width  = (u64)up_pitch * gh; bd.Height = 1;
        bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&up)))
            up = NULL;
        fprintf(stderr, "[GUESTFB] presenting guest 0x%08X %ux%u pitch %u -> backbuffer%c",
                off, gw, gh, gpitch, 10);
    }
    if (!up) return;
    { extern u8* vm_base;
      if (!vm_base) return;
      /* RSX local memory is mapped at guest 0xC0000000. */
      const u8* srow = vm_base + 0xC0000000u + off;
      void* m = NULL; D3D12_RANGE nr = {0, 0};
      if (FAILED(up->lpVtbl->Map(up, 0, &nr, &m)) || !m) return;
      for (u32 y = 0; y < gh; y++) {
          const u8* s = srow + (size_t)y * gpitch;
          u8* dp = (u8*)m + (size_t)y * up_pitch;
          for (u32 x = 0; x < gw; x++) {   /* guest B,G,R,A -> backbuffer R,G,B,A */
              dp[x*4+0] = s[x*4+2]; dp[x*4+1] = s[x*4+1];
              dp[x*4+2] = s[x*4+0]; dp[x*4+3] = 0xFF;
          }
      }
      D3D12_RANGE wr = {0, (SIZE_T)up_pitch * gh};
      up->lpVtbl->Unmap(up, 0, &wr); }

    D3D12_RESOURCE_BARRIER gb = {0};
    gb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    gb.Transition.pResource   = s_d3d.render_targets[fi];
    gb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);

    D3D12_TEXTURE_COPY_LOCATION dl = {0}, sl = {0};
    dl.pResource = s_d3d.render_targets[fi];
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dl.SubresourceIndex = 0;
    sl.pResource = up; sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    sl.PlacedFootprint.Offset = 0;
    sl.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    sl.PlacedFootprint.Footprint.Width    = gw;
    sl.PlacedFootprint.Footprint.Height   = gh;
    sl.PlacedFootprint.Footprint.Depth    = 1;
    sl.PlacedFootprint.Footprint.RowPitch = up_pitch;
    s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dl, 0, 0, 0, &sl, NULL);

    gb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    gb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &gb);
}


/* SLOW-FRAME report (always on, rate-limited): a frame that takes > 1 s says
 * where the time went -- waiting for the GPU to finish the previous frame,
 * render_frame's own CPU work, or the guest/recording side -- plus the video
 * memory budget (an over-budget process gets its resources paged in and out
 * on every submission, which turns frames into seconds). */
static double s_sf_gpu_ms;
static LARGE_INTEGER s_sf_t0;
static void vidmem_report(const char* why)
{
    if (!s_adapter3) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO li, nl;
    memset(&li, 0, sizeof(li)); memset(&nl, 0, sizeof(nl));
    s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li);
    s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
    fprintf(stderr, "[VIDMEM] %s: local %llu/%llu MB, system(non-local) %llu/%llu MB (usage/budget)%c", why,
            (unsigned long long)(li.CurrentUsage >> 20), (unsigned long long)(li.Budget >> 20),
            (unsigned long long)(nl.CurrentUsage >> 20), (unsigned long long)(nl.Budget >> 20), 10);
}

/* Present() failure (2026-10-23): a removed/reset device used to go unnoticed -- the fences then complete at once, the
 * game keeps running and the window stays black with nothing in play.err.log. Log it to stderr (first 8 failures,
 * reason + video memory + DRED) and tell the player once. */
static volatile long s_present_fail_n = 0;
static volatile long s_present_fail_hr = 0;
static DWORD WINAPI d3d12_device_lost_box(LPVOID hwnd)   /* own thread: the render thread must not sit in a modal loop */
{
    MessageBoxA(NULL,   /* no owner: a cross-thread owner would attach to the game window's input queue */
                "The graphics driver stopped responding (GPU device removed), so the game can no longer show "
                "a picture.\n\nPlease restart the game. Choosing a lower Resolution in the launcher helps "
                "if this keeps happening.\n\nWhen reporting it, send play.err.log and play.out.log "
                "from the game folder.",
                "inFamous -- graphics device lost", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    (void)hwnd;
    return 0;
}
static void d3d12_present_failed(HRESULT ph)
{
    const long n = ++s_present_fail_n;
    s_present_fail_hr = (long)ph;
    if (n > 8) return;
    const HRESULT rr = s_d3d.device ? s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device) : S_OK;
    fprintf(stderr, "[D3D12] PRESENT FAILED #%ld hr=0x%08lX, device removed reason 0x%08lX (887A0005 removed, "
                    "887A0006 hung, 887A0007 reset, 887A0020 driver internal, 8007000E out of memory)\n",
            n, (unsigned long)ph, (unsigned long)rr);
    vidmem_report("present failed");
    fflush(stderr);
    if (ph == DXGI_ERROR_DEVICE_REMOVED || ph == DXGI_ERROR_DEVICE_RESET || rr != S_OK) {
        static int s_boxed = 0;
        if (!s_boxed) {
            s_boxed = 1;
            d3d12_dred_report();
            fflush(stdout); fflush(stderr);
            HANDLE th = CreateThread(NULL, 0, d3d12_device_lost_box, (LPVOID)s_d3d.hwnd, 0, NULL);
            if (th) CloseHandle(th);
        }
    }
}

/* [HEARTBEAT] (2026-10-23; HEARTBEAT=0 off): one stderr line every 10 s from the frame-clock thread, so a black-screen
 * report shows whether the guest still flips, whether we still Present, whether the device is alive and how video
 * memory stands. Only counters that already exist; GetDeviceRemovedReason / QueryVideoMemoryInfo are cheap. */
void rsx_d3d12_heartbeat(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("HEARTBEAT"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || !s_d3d.device) return;
    static ULONGLONG t0 = 0, last = 0;
    static unsigned flips0 = 0; static long long pres0 = 0; static u32 rf0 = 0;
    const ULONGLONG now = GetTickCount64();
    if (!t0) { t0 = last = now; return; }
    if (now - last < 10000) return;
    last = now;
    extern unsigned cellGcm_flip_request_count(void);
    extern volatile long long g_fr_presents;
    const unsigned fl = cellGcm_flip_request_count();
    const long long pr = g_fr_presents;
    const u32 rf = s_fs_rf_calls;
    const ULONGLONG lp = s_fs_pres_tick;
    const HRESULT rr = s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
    unsigned long long vu = 0, vb = 0;
    if (s_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO li; memset(&li, 0, sizeof li);
        if (SUCCEEDED(s_adapter3->lpVtbl->QueryVideoMemoryInfo(s_adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
            vu = li.CurrentUsage >> 20; vb = li.Budget >> 20; }
    }
    char lps[32];
    if (lp) snprintf(lps, sizeof lps, "%llu ms ago", (unsigned long long)(now - lp)); else snprintf(lps, sizeof lps, "never");
    fprintf(stderr, "[HEARTBEAT] t=%.0fs flips %u (+%u) presents %lld (+%lld) frames +%u | last present %s, last frame %u draws"
                    " | device 0x%08lX, present failures %ld (last 0x%08lX) | vram %llu/%llu MB\n",
            (double)(now - t0) / 1000.0, fl, fl - flips0, pr, pr - pres0, (unsigned)(rf - rf0), lps, (unsigned)s_dbg_last_draws,
            (unsigned long)rr, (long)s_present_fail_n, (unsigned long)s_present_fail_hr, vu, vb);
    fflush(stderr);
    flips0 = fl; pres0 = pr; rf0 = rf;
}
/* ---- v225A hitch / drop telemetry --------------------------------------------------------------------
 * [FRAMEPCT] every 300 world frames (the [FRAME78] windows): the real frame-time distribution -- p50 / p99, 1% low
 * (1000 / mean of the slowest 1% of frames), 0.1% low (slowest frame), counts > 20 / 25 / 33 ms, worst -- and which
 * subsystem dominated each > 25 ms frame. "[FRAMEPCT] run" = the same over the whole run so far (every 5 windows).
 * [SPIKE] one line per world frame > SPIKE_MS (default 25, 0 = off), at most SPIKE_MAX (default 400): where that
 * frame interval went on every thread we can see: render thread (GPU wait, CPU incl. PSO creation + texture
 * upload, idle = waiting for the next frame from the FIFO thread), FIFO thread (VS compiles, waits for the render
 * thread), guest file reads + FIOS inflate (main guest thread / all threads), SPU task waits (all PPU threads),
 * guest flips. Attribution = the component with the largest excess over its running average (exponential, normal
 * frames only). HZ_POS_EA=<hex guest addr of 3 BE floats> adds a position (bench: player 0x00915430).
 * HITCH_MS (default 250) / HITCH_MAX (default 30): frames longer than HITCH_MS dump the main-thread sample ring
 * ([HITCH], needs PS3_PROF_MAIN). Counters: include/ps3emu/hz_stats.h (always on, slow host operations only). */
volatile long long g_hz[32];
static volatile unsigned long s_hz_main_tid = 0;
void ps3_hz_set_main(void) { s_hz_main_tid = GetCurrentThreadId(); }
int ps3_hz_is_main(void) { return s_hz_main_tid != 0 && GetCurrentThreadId() == s_hz_main_tid; }
static double s_rf_wait_ms, s_rf_gpuwait_ms;   /* tentative definitions; initialised with the RF_ASYNC code below */
static int hz_env_int(const char* n, int dflt) { const char* e = getenv(n); return (e && *e) ? atoi(e) : dflt; }
static double s_hz_hitch_ms = -1.0;
static double hz_hitch_ms(void) { if (s_hz_hitch_ms < 0) { s_hz_hitch_ms = (double)hz_env_int("HITCH_MS", 250); if (s_hz_hitch_ms < 1) s_hz_hitch_ms = 1e9; } return s_hz_hitch_ms; }
enum { HC_GPU, HC_RCPU, HC_PSO, HC_TEX, HC_VS, HC_IO, HC_TW, HC_GUEST, HC_FIFO, HC_N };
static const char* const s_hc_name[HC_N] = { "gpu", "rcpu", "pso", "tex", "vs", "io", "tw", "guest", "fifo" };
static int hz_cmp_f(const void* a, const void* b) { const float x = *(const float*)a, y = *(const float*)b; return x < y ? -1 : x > y; }
/* v243C RFBOUND=1 (default off): is a frame bound by the main guest thread or by the RSX side? Per world frame:
 * main-thread fence-poll wait (cellGcmSys.c g_v243c_fence_qpc, by FIFO-thread state), FIFO waits for the render
 * thread / the GPU, render-thread exec CPU / GPU wait / idle. Classes: main = the main thread did not wait on the
 * RSX fence (< 0.5 ms); rsx-direct = it waited >= 0.5 ms while the FIFO thread itself was blocked on the render
 * thread or the GPU (>= 0.5 ms of the wait); fifo-walk = it waited while the FIFO thread was walking (incl. walking
 * late because of an earlier GPU wait). rf-sat = render thread idle < 0.5 ms (its cycle fills the frame). */
static void v243c_rfbound_frame(double frame_ms, double rf_ms, double gpu_ms, double idle, double fw, double fg, double tw)
{
    extern volatile long long g_v243c_fence_qpc[8]; extern int v243c_rfbound_on(void);
    extern volatile int g_abx, g_ab_phase;
    if (!v243c_rfbound_on()) return;
    static LARGE_INTEGER qf; static long long last[8]; static int init = 0, n = 0, nM = 0, nD = 0, nF = 0, nS = 0;
    static double a[12];
    if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    double q[8], fence = 0;
    for (int k = 0; k < 8; k++) { const long long v = g_v243c_fence_qpc[k]; q[k] = (double)(v - last[k]) * 1000.0 / (double)qf.QuadPart; last[k] = v; fence += q[k]; }
    if (!init) { init = 1; return; }
    const double qr = q[5] + q[6];
    if (fence < 0.5) nM++; else if (qr >= 0.5) nD++; else nF++;
    if (idle < 0.5) nS++;
    const double v[12] = { frame_ms, fence, q[2], q[5], q[6], q[1] + q[3] + q[4] + q[0] + q[7], fw, fg, rf_ms, gpu_ms, idle, tw };
    for (int k = 0; k < 12; k++) a[k] += v[k];
    if (++n == 300) {
        fprintf(stderr, "[RFBOUND] v243C 300 world frames: main-bound %d | rsx-direct %d | fifo-walk %d | rf-saturated %d | means ms: frame %.2f"
                        " fence %.2f (walk %.2f rf %.2f gpu %.2f other %.2f) fifo-wait rf %.2f gpu %.2f | rf exec %.2f gpuwait %.2f idle %.2f | tw(all) %.2f | abx 0x%X phase %d%c",
                nM, nD, nF, nS, a[0] / n, a[1] / n, a[2] / n, a[3] / n, a[4] / n, a[5] / n, a[6] / n, a[7] / n, a[8] / n, a[9] / n, a[10] / n, a[11] / n,
                (unsigned)g_abx, (int)g_ab_phase, 10);
        n = nM = nD = nF = nS = 0; memset(a, 0, sizeof a);
        {   /* v243C RFBOUND2: the walker's side of the fence wait (cellGcmSys.c g_v243c_st) */
            extern volatile long long g_v243c_st[16]; static long long l2[16];
            long long d2[16]; for (int k = 0; k < 16; k++) { d2[k] = g_v243c_st[k] - l2[k]; l2[k] = g_v243c_st[k]; }
            const double ms = 1000.0 / (double)qf.QuadPart;
            fprintf(stderr, "[RFBOUND2] v243C per frame: %.1f put stores, %.2f fence waits (backlog %.1f KB, %.2f ms after the last put), ACQUIRE spins %.2f = %.3f ms (%.3f ms while main waits)%c",
                    d2[0] / 300.0, d2[1] / 300.0, d2[1] ? d2[2] / 1024.0 / d2[1] : 0.0, d2[1] ? d2[3] * ms / d2[1] : 0.0,
                    d2[6] / 300.0, d2[4] * ms / 300.0, d2[5] * ms / 300.0, 10);
            fprintf(stderr, "[RFBOUND3] v243C walker per put: %.2f puts drained/frame, put -> drained %.2f ms, inside the FIFO walk %.2f ms%c",
                    d2[10] / 300.0, d2[10] ? d2[8] * ms / d2[10] : 0.0, d2[10] ? d2[9] * ms / d2[10] : 0.0, 10);
        }
    }
}
static void hz_frame_report(double frame_ms, double rf_ms, double gpu_ms, u32 draws, LARGE_INTEGER now, LARGE_INTEGER f)
{
    static int inited = 0, spike_ms = 25, spike_max = 400, nspk = 0; static u32 pos_ea = 0;
    static long long last[32]; static unsigned long long tsc0; static LARGE_INTEGER q0;
    static double fifo_w0, fifo_g0, tw0; static unsigned flips0;
    static double ema[HC_N]; static int ema_n = 0;
    extern volatile long long g_fr_tw_us[17];
    extern unsigned cellGcm_flip_request_count(void);
    const unsigned long long tsc = hz_tsc();
    if (!inited) {
        inited = 1; spike_ms = hz_env_int("SPIKE_MS", 25); spike_max = hz_env_int("SPIKE_MAX", 400);
        { const char* e = getenv("HZ_POS_EA"); if (e && *e) pos_ea = (u32)strtoul(e, NULL, 16); }
        tsc0 = tsc; q0 = now;
        for (int i = 0; i < 32; i++) last[i] = g_hz[i];
        fifo_w0 = s_rf_wait_ms; fifo_g0 = s_rf_gpuwait_ms; flips0 = cellGcm_flip_request_count();
        { double s = 0; for (int i = 0; i < 17; i++) s += (double)g_fr_tw_us[i]; tw0 = s; }
        fprintf(stderr, "[FRAMEPCT] v225A frame-time telemetry on: SPIKE_MS %d, SPIKE_MAX %d, HITCH_MS %.0f%c", spike_ms, spike_max, hz_hitch_ms(), 10);
        return;
    }
    /* TSC -> ms, calibrated against QPC over the whole run */
    const double qms = (double)(now.QuadPart - q0.QuadPart) * 1000.0 / (double)f.QuadPart;
    const double tms = (tsc > tsc0 && qms > 50.0) ? qms / (double)(tsc - tsc0) : 0.0;
    long long d[32];
    for (int i = 0; i < 32; i++) { const long long v = g_hz[i]; d[i] = v - last[i]; last[i] = v; }
    const double gmax = (double)InterlockedExchange64(&g_hz[HZ_GFLIP_MAX_TSC], 0) * tms;   /* longest guest frame (flip gap) */
    double fw = s_rf_wait_ms - fifo_w0; if (fw < 0) fw = s_rf_wait_ms; fifo_w0 = s_rf_wait_ms;   /* reset every 600 frames */
    double fg = s_rf_gpuwait_ms - fifo_g0; if (fg < 0) fg = s_rf_gpuwait_ms; fifo_g0 = s_rf_gpuwait_ms;
    double twn = 0; for (int i = 0; i < 17; i++) twn += (double)g_fr_tw_us[i];
    const double tw = (twn - tw0) / 1000.0; tw0 = twn;
    const unsigned fl = cellGcm_flip_request_count(); const unsigned flips = fl - flips0; flips0 = fl;
    if (draws < 500) return;   /* world frames only, like [FRAME78] */
    const double pso = d[HZ_PSO_TSC] * tms, tex = d[HZ_TEX_TSC] * tms, vs = d[HZ_VS_TSC] * tms;
    const double io = (d[HZ_FS_MAIN_TSC] + d[HZ_FIOS_MAIN_TSC]) * tms;
    double rcpu = rf_ms - pso - tex; if (rcpu < 0) rcpu = 0;
    double idle = frame_ms - gpu_ms - rf_ms; if (idle < 0) idle = 0;
    v243c_rfbound_frame(frame_ms, rf_ms, gpu_ms, idle, fw, fg, tw);   /* v243C RFBOUND (no-op unless RFBOUND=1) */
    double c[HC_N]; c[HC_GPU] = gpu_ms; c[HC_RCPU] = rcpu; c[HC_PSO] = pso; c[HC_TEX] = tex; c[HC_VS] = vs; c[HC_IO] = io; c[HC_TW] = tw;
    c[HC_GUEST] = gmax; c[HC_FIFO] = idle;
    int cls = -1;
    if (frame_ms > 25.0 && ema_n >= 30) {   /* largest excess over the running average */
        double ex[HC_N], best = -1e9;
        for (int k = 0; k < HC_N; k++) ex[k] = c[k] - ema[k];
        { double sub = 0; if (ex[HC_VS] > 0) sub += ex[HC_VS]; if (ex[HC_IO] > 0) sub += ex[HC_IO]; if (ex[HC_TW] > 0) sub += ex[HC_TW];
          if (ex[HC_GUEST] > 0) sub += ex[HC_GUEST];
          ex[HC_FIFO] -= sub; }   /* render idle not explained by a late guest frame or the upstream components we measure */
        for (int k = 0; k < HC_N; k++) if (ex[k] > best) { best = ex[k]; cls = k; }
    } else if (frame_ms <= 20.0) {
        const double a = ema_n < 30 ? 1.0 / (ema_n + 1) : 0.02;
        for (int k = 0; k < HC_N; k++) ema[k] += a * (c[k] - ema[k]);
        ema_n++;
    }
    if (spike_ms > 0 && frame_ms > (double)spike_ms && nspk < spike_max) {
        nspk++;
        char pos[64] = "";
        if (pos_ea) { extern uint8_t* vm_base; float p[3];
            for (int k = 0; k < 3; k++) { u32 b = __builtin_bswap32(*(volatile u32*)(vm_base + pos_ea + 4u * (u32)k)); memcpy(&p[k], &b, 4); }
            snprintf(pos, sizeof pos, " pos %.0f %.0f %.0f", p[0], p[1], p[2]); }
        extern volatile int g_abx;
        fprintf(stderr, "[SPIKE] t=%.2f %.1f ms -> %s | render: gpu %.1f cpu %.1f (pso %.1f/%lld tex %.1f new %lld %.1f MB) idle %.1f"
                        " | fifo: wait-render %.1f wait-gpu %.1f vs %.1f/%lld | io: main %.1f, all %.1f ms/%lld %.1f MB, fios %.1f/%lld"
                        " | tw(all) %.1f | guest: %lld flips, max gap %.1f | shc %.1f/%lld (%lld compiled) | flips %u draws %u abx 0x%X%s%c",
                qms / 1000.0, frame_ms, cls >= 0 ? s_hc_name[cls] : "-", gpu_ms, rf_ms, pso, d[HZ_PSO_N], tex, d[HZ_TEX_NEW],
                d[HZ_TEX_BYTES] / 1048576.0, idle, fw, fg, vs, d[HZ_VS_N], io, d[HZ_FS_TSC] * tms, d[HZ_FS_N],
                d[HZ_FS_BYTES] / 1048576.0, d[HZ_FIOS_TSC] * tms, d[HZ_FIOS_N], tw, d[HZ_GFLIP_N], gmax, d[HZ_SHC_TSC] * tms, d[HZ_SHC_N],
                d[HZ_SHC_MISS], flips, draws, (unsigned)g_abx, pos, 10);
    }
    /* window (300 world frames) + run distribution */
    static float win[300]; static int nw = 0, ncls[HC_N + 1], nwin = 0;
    static u32 rh[2501]; static u64 rn = 0; static double rwall = 0, rmax = 0;
    lodx_ab_frame(frame_ms, rf_ms, qms / 1000.0);   /* v235A LOD_CLASS_SCALES in-run A/B */
    win[nw++] = (float)frame_ms;
    static double wsum[12]; static long long wgf;   /* window means: gpu rcpu pso tex idle fifo-wait-render vs io tw gmax fios(all) fs(all) */
    { const double v[12] = { gpu_ms, rf_ms, pso, tex, idle, fw, vs, io, tw, gmax, d[HZ_FIOS_TSC] * tms, d[HZ_FS_TSC] * tms };
      for (int k = 0; k < 12; k++) wsum[k] += v[k]; wgf += d[HZ_GFLIP_N]; }
    if (frame_ms > 25.0) ncls[cls >= 0 ? cls : HC_N]++;
    { u32 b = frame_ms >= 250.0 ? 2500u : (u32)(frame_ms * 10.0); rh[b]++; rn++; rwall += frame_ms; if (frame_ms > rmax) rmax = frame_ms; }
    if (nw == 300) {
        static float s[300]; memcpy(s, win, sizeof s); qsort(s, 300, sizeof s[0], hz_cmp_f);
        double wall = 0; int n20 = 0, n25 = 0, n33 = 0;
        for (int i = 0; i < 300; i++) { wall += s[i]; n20 += s[i] > 20.0f; n25 += s[i] > 25.0f; n33 += s[i] > 33.4f; }
        const double low1 = 3000.0 / ((double)s[297] + s[298] + s[299]);
        char cl[256]; int p = 0;
        for (int k = 0; k <= HC_N; k++) if (ncls[k]) p += snprintf(cl + p, sizeof cl - p, " %s %d", k < HC_N ? s_hc_name[k] : "?", ncls[k]);
        extern volatile int g_abx;
        char fl[1024]; int fq = snprintf(fl, sizeof fl, "[FRAMEPCT] t=%.1f 300 frames: avg %.2f fps, p50 %.2f ms, p99 %.2f ms, 1%% low %.1f fps, 0.1%% low %.1f fps,"
                        " >20ms %d, >25ms %d, >33ms %d, worst %.1f ms | >25ms by:%s | abx 0x%X",
                qms / 1000.0, 300000.0 / wall, s[149], s[296], low1, 1000.0 / s[299], n20, n25, n33, s[299],
                p ? cl : " -", (unsigned)g_abx);
        if (fq < 0 || fq >= (int)sizeof fl) fq = 0;
        snprintf(fl + fq, sizeof fl - (size_t)fq, " | means ms: gpu %.2f rcpu %.2f (pso %.2f tex %.2f) idle %.2f fifo-wait-render %.2f vs %.2f io-main %.2f tw(all) %.2f"
                        " guest-max-gap %.1f fios(all) %.2f fs(all) %.2f, guest fps %.1f%c",
                wsum[0] / 300, wsum[1] / 300, wsum[2] / 300, wsum[3] / 300, wsum[4] / 300, wsum[5] / 300, wsum[6] / 300,
                wsum[7] / 300, wsum[8] / 300, wsum[9] / 300, wsum[10] / 300, wsum[11] / 300, wall > 0 ? 1000.0 * (double)wgf / wall : 0.0, 10);
        fputs(fl, stderr);
        memset(wsum, 0, sizeof wsum); wgf = 0;
        nw = 0; memset(ncls, 0, sizeof ncls);
        if (++nwin % 5 == 0) {   /* run distribution from the 0.1 ms histogram */
            u64 k1 = rn / 100, k01 = rn / 1000, acc = 0; double s1 = 0, s01 = 0; u64 c1 = 0, c01 = 0, c20 = 0, c25 = 0, c33 = 0; double p99 = 0, p50 = 0;
            for (int b = 2500; b >= 0; b--) {
                const double ms = b >= 2500 ? rmax : (b + 0.5) / 10.0;
                for (u32 j = 0; j < rh[b] && c1 < k1; j++) { s1 += ms; c1++; if (c01 < k01) { s01 += ms; c01++; } }
                if (acc < k1 && acc + rh[b] >= k1) p99 = ms;
                if (acc < rn / 2 && acc + rh[b] >= rn / 2) p50 = ms;
                acc += rh[b];
                if (b > 200) c20 += rh[b];
                if (b > 250) c25 += rh[b];
                if (b > 334) c33 += rh[b];
            }
            fprintf(stderr, "[FRAMEPCT] run t=%.1f: %llu frames, avg %.2f fps, p50 %.1f ms, p99 %.1f ms, 1%% low %.1f fps, 0.1%% low %.1f fps, worst %.1f ms,"
                            " >20ms %llu (%.2f%%), >25ms %llu (%.2f%%), >33ms %llu (%.2f%%)%c",
                    qms / 1000.0, (unsigned long long)rn, 1000.0 * rn / rwall, p50, p99, c1 ? 1000.0 * c1 / s1 : 0.0, c01 ? 1000.0 * c01 / s01 : (rmax > 0 ? 1000.0 / rmax : 0.0), rmax,
                    (unsigned long long)c20, 100.0 * c20 / rn, (unsigned long long)c25, 100.0 * c25 / rn,
                    (unsigned long long)c33, 100.0 * c33 / rn, 10);
        }
    }
}
static void slowframe_report(u32 draws)
{
    if (!ps3_telem_enabled()) return;
    static LARGE_INTEGER prev; static int n = 0;
    LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    if (prev.QuadPart) {
        double frame_ms = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)f.QuadPart;
        double rf_ms = (double)(now.QuadPart - s_sf_t0.QuadPart) * 1000.0 / (double)f.QuadPart - s_sf_gpu_ms;
        hz_frame_report(frame_ms, rf_ms, s_sf_gpu_ms, draws, now, f);   /* v225A [SPIKE]/[FRAMEPCT] */
        /* Frame-level measurements only: no per-draw timers or thread sampler.
         * Includes hitches and resets across menus so this is a gameplay window. */
        { static u32 nf, hist[501]; static double wall, render, wait;
          static u64 desc, zconv;
          if (draws >= 500) {
              u32 bucket = frame_ms >= 500.0 ? 500u : (u32)frame_ms;
              hist[bucket]++; nf++; wall += frame_ms; render += rf_ms; wait += s_sf_gpu_ms;
              desc += s_sf_srv_writes; zconv += s_sf_zconversions;
              if (nf == 300) {
                  u32 sum = 0, p95 = 0;
                  for (; p95 < 500; p95++) { sum += hist[p95]; if (sum >= 285) break; }
                  fprintf(stderr, "[FRAME78] 300 world frames: %.2f fps, mean %.2f ms, p95 %s%u ms, render CPU %.2f ms, GPU wait %.2f ms, SRV writes %.0f/frame, depth conversions %.1f/frame\n",
                      1000.0 * nf / wall, wall / nf, p95 == 500 ? ">=" : "<", p95 == 500 ? 500 : p95 + 1,
                      render / nf, wait / nf, (double)desc / nf, (double)zconv / nf);
                  nf = 0; wall = render = wait = 0; desc = zconv = 0; memset(hist, 0, sizeof hist);
              }
          } else { nf = 0; wall = render = wait = 0; desc = zconv = 0; memset(hist, 0, sizeof hist); }
        }
        /* v82: per-PS3_AB-phase world fps (no PERF timers needed). One line per finished phase. */
        { extern volatile int g_ab_mask; extern volatile int g_abx; static long long ab_last = -1; static u32 ab_n, ab_slow; static double ab_wall;
          long long m = (long long)(g_ab_mask & 0xFF) | ((long long)(unsigned)g_abx << 8);   /* v132: the factorial bits are part of the phase key; v219: 64-bit (ABX bit 23+ made the key negative -> never printed) */
          if (m != ab_last) {
              if (ab_last >= 0 && ab_n >= 30)
                  fprintf(stderr, "[ABFPS] phase %d x 0x%02X: %u world frames, %.2f fps, mean %.2f ms, %u frames > 50 ms\n",
                          (int)(ab_last & 0xFF), (unsigned)(ab_last >> 8), ab_n, 1000.0 * ab_n / ab_wall, ab_wall / ab_n, ab_slow);
              /* v219 [ABCPU]: CPU the REST of the machine used during that phase (system busy minus this process), in
               * logical-core units, so unattended bench phases disturbed by other programs can be dropped. */
              { static ULONGLONG c_sys = 0, c_idle = 0, c_proc = 0, c_t = 0;
                FILETIME fi_, fk_, fu_, pc_, pe_, pk_, pu_;
                if (GetSystemTimes(&fi_, &fk_, &fu_) && GetProcessTimes(GetCurrentProcess(), &pc_, &pe_, &pk_, &pu_)) {
                    #define FT64(f) (((ULONGLONG)(f).dwHighDateTime << 32) | (f).dwLowDateTime)
                    const ULONGLONG sys = FT64(fk_) + FT64(fu_), idle = FT64(fi_), proc = FT64(pk_) + FT64(pu_), t = GetTickCount64();
                    #undef FT64
                    if (c_t && ab_last >= 0 && ab_n >= 30) {
                        const double wall100ns = (double)(t - c_t) * 10000.0;
                        const double busy = (double)((sys - c_sys) - (idle - c_idle)) / wall100ns;
                        const double mine = (double)(proc - c_proc) / wall100ns;
                        fprintf(stderr, "[ABCPU] phase x 0x%02X: this process %.2f cores, rest of system %.2f cores, uptime %llu s\n",
                                (unsigned)(ab_last >> 8), mine, busy - mine > 0 ? busy - mine : 0.0, (unsigned long long)(t / 1000));
                    }
                    c_sys = sys; c_idle = idle; c_proc = proc; c_t = t;
                } }
              ab_last = m; ab_n = ab_slow = 0; ab_wall = 0;
          } else if (draws >= 500 && frame_ms < 250.0) { ab_n++; ab_wall += frame_ms; if (frame_ms > 50.0) ab_slow++; }
        }
        if (frame_ms > hz_hitch_ms()) { extern void ps3_prof_hitch_dump(double ms); ps3_prof_hitch_dump(frame_ms); }   /* v69; v225A HITCH_MS */
        if (frame_ms > 1000.0 && n < 40) { n++;
            fprintf(stderr, "[SLOWFRAME] %.0f ms: GPU wait %.0f ms, render_frame CPU %.0f ms, rest (guest+recording) %.0f ms;"
                            " %u draws, %u/%u vertex chunks (parity 0/1), %u pipeline compiles, %u VS compiles,"
                            " caches %d/%d pipelines %d/%d VS, SRV writes %u skipped %u%c",
                    frame_ms, s_sf_gpu_ms, rf_ms, frame_ms - s_sf_gpu_ms - rf_ms, draws,
                    (unsigned)s_vpc_count[0], (unsigned)s_vpc_count[1],
                    (unsigned)s_sf_pso_miss, (unsigned)s_sf_vs_compiles,
                    s_d3d.vp_fp_n, VP_FP_CACHE, s_d3d.vp_vs_n, VP_VS_CACHE,
                    (unsigned)s_sf_srv_writes, (unsigned)s_sf_srv_skips, 10);
            vidmem_report("slow frame");
        }
    }
    { static LARGE_INTEGER last; double since = last.QuadPart
          ? (double)(now.QuadPart - last.QuadPart) / (double)f.QuadPart : 1e9;
      if (since > 15.0) { last = now; vidmem_report("periodic");
          fprintf(stderr, "[CACHES] pipelines %d/%d, vertex shaders %d/%d%c",
                  s_d3d.vp_fp_n, VP_FP_CACHE, s_d3d.vp_vs_n, VP_VS_CACHE, 10); } }
    s_sf_pso_miss = 0; s_sf_vs_compiles = 0; s_sf_srv_writes = 0; s_sf_srv_skips = 0;
    s_sf_zconversions = 0;
    prev = now;
}

static void vc_exec_copies(int par); static void vc_report(void);
static ID3D12Resource* vc_resource(u64* size); static void vc_reset_parity(int par);
/* ---- v103A BLACK_PROBE ------------------------------------------------------------------------------
 * Every BLACK_PROBE-th presented frame (default 8, 0 = off) the image handed to Present is copied into a
 * readback buffer; after the next frame's GPU wait an 80x45 grid of it is averaged. 10-s summary [LUMA];
 * [BLACK] when two probes in a row on world frames (>= 2000 records) are black, with what that frame drew
 * into the display buffers (record/display-draw counts, the fragment programs of the display draws, the
 * presented buffer), and once more when the picture is back. Up to BLACK_CAP (default 2) black episodes
 * request a pass capture (rsxcap/frame_200..). */
static ID3D12Resource* s_bp_buf = NULL;
static u32 s_bp_w = 0, s_bp_h = 0, s_bp_pitch = 0;
static int s_bp_pending = 0, s_bp_fail = 0;
static int s_bp_last_slot = -1; static u64 s_bp_last_drawn = 0;
static struct { u32 recs, disp, disp_clr, nfp, fp[6], fpn[6], flip_off, pres_off; int slot, slot_drawn; ULONGLONG t; } s_bp_info;
static int black_probe_every(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("BLACK_PROBE"); v = e ? atoi(e) : 8; if (v < 0) v = 0;
        fprintf(stderr, "[LUMA] v103A presented-image probe every %d presented frames (BLACK_PROBE, 0 = off), black-frame"
                        " captures BLACK_CAP=%s%c", v, getenv("BLACK_CAP") ? getenv("BLACK_CAP") : "2 (default)", 10); }
    return v;
}
static int black_probe_want(void)
{
    static u32 n = 0;
    const int every = black_probe_every();
    if (!every || s_bp_fail || s_bp_pending) return 0;
    if (s_bp_buf && (s_bp_w != BBW || s_bp_h != BBH)) return 0;   /* backbuffer size changed */
    if (++n % (u32)every) return 0;
    if (!s_bp_buf) {
        s_bp_w = BBW; s_bp_h = BBH; s_bp_pitch = (s_bp_w * 4u + 255u) & ~255u;
        D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = (u64)s_bp_pitch * s_bp_h; rd.Height = 1;
        rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (!s_bp_w || !s_bp_h || FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_bp_buf)) || !s_bp_buf) {
            s_bp_buf = NULL; s_bp_fail = 1;
            fprintf(stderr, "[LUMA] readback buffer failed -> probe off%c", 10);
            return 0;
        }
    }
    return 1;
}
/* Render thread, right after the probe copy was recorded: what this frame drew into display buffers. */
static void black_probe_note_frame(void)
{
    memset(&s_bp_info, 0, sizeof(s_bp_info));
    s_bp_info.recs = s_rf_count + s_split_recs;
    for (u32 d = 0; d < s_rf_count && d < MAX_DRAWS; d++) {
        const D3D12DrawRecord* r = &s_rf_draws[d];
        if (r->rt_off != 0) continue;
        if (r->is_clear) { s_bp_info.disp_clr++; continue; }
        s_bp_info.disp++;
        u32 j = 0; while (j < s_bp_info.nfp && s_bp_info.fp[j] != r->fp_addr) j++;
        if (j == s_bp_info.nfp && s_bp_info.nfp < 6) { s_bp_info.fp[j] = r->fp_addr; s_bp_info.nfp++; }
        if (j < s_bp_info.nfp) s_bp_info.fpn[j]++;
    }
    s_bp_info.flip_off = s_rf_flip_off; s_bp_info.slot = s_bp_last_slot;
    s_bp_info.pres_off = (s_bp_last_slot >= 0 && s_bp_last_slot < MAX_OFF_RTS) ? s_d3d.off_rt[s_bp_last_slot].off : 0xFFFFFFFFu;
    s_bp_info.slot_drawn = (s_bp_last_slot >= 0 && s_bp_last_slot < 64) ? (int)((s_bp_last_drawn >> s_bp_last_slot) & 1ull) : -1;
    s_bp_info.t = GetTickCount64();
    s_bp_pending = 1;
}
static void black_probe_read(void)
{
    if (!s_bp_pending || !s_bp_buf) return;
    s_bp_pending = 0;
    void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_bp_pitch * s_bp_h};
    if (FAILED(s_bp_buf->lpVtbl->Map(s_bp_buf, 0, &rr, &mp)) || !mp) return;
    u64 sum = 0; u32 mx = 0, lit = 0, cnt = 0;
    for (u32 y = s_bp_h / 90u; y < s_bp_h; y += (s_bp_h / 45u ? s_bp_h / 45u : 1u))
        for (u32 x = s_bp_w / 160u; x < s_bp_w; x += (s_bp_w / 80u ? s_bp_w / 80u : 1u)) {
            const u8* q = (const u8*)mp + (u64)y * s_bp_pitch + (u64)x * 4u;
            u32 l = ((u32)q[0] + q[1] + q[2]) / 3u;
            sum += l; if (l > mx) mx = l; if (l > 24u) lit++; cnt++;
        }
    D3D12_RANGE wr = {0, 0}; s_bp_buf->lpVtbl->Unmap(s_bp_buf, 0, &wr);
    if (!cnt) return;
    const double mean = (double)sum / cnt;
    const int black = (mean < 3.0 && lit * 500u < cnt);   /* < 0.2% of the samples above 24/255 */
    static ULONGLONG t0 = 0, win0 = 0, black_t0 = 0; static int in_black = 0, streak = 0, ep = 0, caps_left = -1;
    static u32 w_n = 0, w_black = 0; static double w_sum = 0, w_min = 1e9, w_max = 0;
    const ULONGLONG now = GetTickCount64();
    if (!t0) t0 = now;
    if (!win0) win0 = now;
    if (caps_left < 0) { const char* e = getenv("BLACK_CAP"); caps_left = e ? atoi(e) : 2; if (caps_left < 0) caps_left = 0; }
    w_n++; w_black += black ? 1u : 0u; w_sum += mean; if (mean < w_min) w_min = mean; if (mean > w_max) w_max = mean;
    if (black) streak++; else streak = 0;
    if (!in_black && streak >= 2 && s_bp_info.recs >= 2000u) {
        in_black = 1; black_t0 = now; ep++;
        fprintf(stderr, "[BLACK] #%d t=%.1f s: presented image is black (mean %.2f, max %u, %u/%u samples lit); frame: %u records"
                        " (split parts %u), display draws %u, display clears %u, presented buffer 0x%X (slot %d, drawn this frame %d),"
                        " flipped 0x%X; display-draw FPs:",
                ep, (now - t0) / 1000.0, mean, mx, lit, cnt, s_bp_info.recs, s_split_n, s_bp_info.disp, s_bp_info.disp_clr,
                s_bp_info.pres_off, s_bp_info.slot, s_bp_info.slot_drawn, s_bp_info.flip_off);
        for (u32 j = 0; j < s_bp_info.nfp; j++) fprintf(stderr, " 0x%X x%u", s_bp_info.fp[j], s_bp_info.fpn[j]);
        fprintf(stderr, "%s%c", caps_left > 0 ? " -> requesting a pass capture" : "", 10);
        if (caps_left > 0) { caps_left--; s_black_cap_req = 1; }
        fflush(stderr);
    } else if (in_black && !black) {
        in_black = 0;
        fprintf(stderr, "[BLACK] #%d over after %.1f s (mean %.2f, %u/%u lit; frame %u records, display draws %u)%c",
                ep, (now - black_t0) / 1000.0, mean, lit, cnt, s_bp_info.recs, s_bp_info.disp, 10);
    }
    if (now - win0 >= 10000) {
        fprintf(stderr, "[LUMA] t=%.0f s, 10 s: %u probes, %u black, mean luma %.1f (min %.1f max %.1f)%s; frame splits %u,"
                        " draw calls dropped at a full record buffer %u%c",
                (now - t0) / 1000.0, w_n, w_black, w_n ? w_sum / w_n : 0.0, w_min, w_max, in_black ? " [BLACK now]" : "",
                s_split_win, s_capfull_win, 10);
        win0 = now; w_n = w_black = 0; w_sum = 0; w_min = 1e9; w_max = 0; s_split_win = 0; s_capfull_win = 0;
    }
}
/* v187 RFTIME: always-on per-phase wall time of render_frame_exec (QPC, ~12 reads/frame) so the
 * 10-13 ms "render CPU" in [FRAME78] can be split into phases; also counts what keeps RF_ASYNC sync
 * and how often the legacy font-atlas block re-creates its two committed resources. */
enum { RFT_GPU, RFT_SETUP, RFT_ATLAS, RFT_MISC, RFT_PRE, RFT_REC, RFT_TAIL1, RFT_SUBMIT, RFT_TAIL2,
       RFT_PRESENT, RFT_NEXT, RFT_WRAP, RFT_N };
static const char* const s_rft_name[RFT_N] = { "gpuwait", "setup", "atlas", "misc", "prepass", "record",
    "tail1", "close+exec", "tail2", "present", "next", "wrapper(vtxsync)" };
static double s_rft_acc[RFT_N]; static LARGE_INTEGER s_rft_last; static double s_rft_inv = 0.0;
static u32 s_rft_frames, s_rft_atlas_up, s_rft_atlas_skip, s_rft_texbound, s_rft_legacy, s_rft_legacy_tex;
static u32 s_legacy_draws, s_legacy_tex_draws;       /* this frame's non-VP records (reset by render_frame) */
static u32 s_rf_legacy_draws, s_rf_legacy_tex_draws; /* the frame being replayed */
static inline void rft_start(void)
{
    if (!ps3_telem_enabled()) return;
    if (s_rft_inv == 0.0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_rft_inv = 1000.0 / (double)f.QuadPart; }
    QueryPerformanceCounter(&s_rft_last);
}
static inline void rft_mark(int k)
{
    if (!ps3_telem_enabled()) return;
    LARGE_INTEGER n; QueryPerformanceCounter(&n);
    s_rft_acc[k] += (double)(n.QuadPart - s_rft_last.QuadPart) * s_rft_inv; s_rft_last = n;
}
static void rft_report(void)
{
    if (!ps3_telem_enabled()) return;
    if (++s_rft_frames < 600) return;
    char b[640]; int o = 0; double tot = 0.0;
    for (int k = 0; k < RFT_N; k++) tot += s_rft_acc[k];
    o += snprintf(b + o, sizeof b - o, "[RFTIME] last 600 execs, ms/frame: total %.2f |", tot / s_rft_frames);
    for (int k = 0; k < RFT_N && o < (int)sizeof b - 40; k++)
        o += snprintf(b + o, sizeof b - o, " %s %.2f", s_rft_name[k], s_rft_acc[k] / s_rft_frames);
    fprintf(stderr, "%s | atlas re-creates %u (skipped %u), frames with tex_bound %u, with non-VP draws %u (textured %u)%c",
            b, s_rft_atlas_up, s_rft_atlas_skip, s_rft_texbound, s_rft_legacy, s_rft_legacy_tex, 10);
    memset(s_rft_acc, 0, sizeof s_rft_acc);
    s_rft_frames = s_rft_atlas_up = s_rft_atlas_skip = s_rft_texbound = s_rft_legacy = s_rft_legacy_tex = 0;
}
/* v187 ATLAS_REUP (default 0): the legacy dbgfont atlas (tex_bound = the last texture bound was B8) used to be
 * re-created + re-uploaded (2 CreateCommittedResource + 2 Release + a row copy from guest memory) on EVERY
 * frame it was dirty. Only non-VP textured draws sample it (dbgfont path; inFamous draws everything through
 * the VP pass), so after the first upload it is refreshed only for frames that have such a draw.
 * ATLAS_REUP=1 = old (every dirty frame). */
static int atlas_reup_old(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("ATLAS_REUP"); v = (e && *e == '1') ? 1 : 0;
                 fprintf(stderr, "[D3D12] v187 legacy atlas refresh: %s (ATLAS_REUP)%c",
                         v ? "every dirty frame (old)" : "only for frames with non-VP textured draws", 10); }
    return v;
}
/* ===========================================================================
 * v199 GPUPROF (GPUPROF=1): GPU timestamps around every render-target pass of the VP draw loop
 * (+ the work before it and after it), read back after the next frame's wait_for_gpu (that frame is finished
 * then). Every 600 frames: [GPUPROF] mean GPU ms/frame and the 14 most expensive pass kinds, keyed by target
 * size x MRT count (+z = own depth surface, "back" = the display buffer). Tells where a GPU-bound frame
 * (FRAME78 "GPU wait") goes, e.g. at RES_SCALE 3.
 * =========================================================================*/
#define GQ_N 1024
enum { GQ_PRE = 1, GQ_VPBEGIN = 2, GQ_POST = 3, GQ_END = 4 };
static ID3D12QueryHeap* s_gq_heap = NULL; static ID3D12Resource* s_gq_rb = NULL;
static int s_gq_on = -1, s_gq_par = 0; static u32 s_gq_n[2]; static u64 s_gq_lab[2][GQ_N];
static double s_gq_tick_ms = 0.0;
typedef struct { u64 lab; double ms; u32 cnt; } GqAgg;
#define GQ_AGG 256
static GqAgg s_gq_agg[GQ_AGG]; static double s_gq_total_ms; static u32 s_gq_frames;
static int gq_active(void)
{
    if (__builtin_expect(s_gq_on >= 0, 1)) return s_gq_on;
    s_gq_on = 0;
    { const char* e = getenv("GPUPROF"); if (!e || *e != '1') return 0; }
    if (!s_d3d.device || !s_d3d.cmd_queue) return 0;
    UINT64 fq = 0;
    if (FAILED(s_d3d.cmd_queue->lpVtbl->GetTimestampFrequency(s_d3d.cmd_queue, &fq)) || !fq) return 0;
    s_gq_tick_ms = 1000.0 / (double)fq;
    D3D12_QUERY_HEAP_DESC qd; memset(&qd, 0, sizeof qd); qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qd.Count = 2 * GQ_N;
    if (FAILED(s_d3d.device->lpVtbl->CreateQueryHeap(s_d3d.device, &qd, &IID_ID3D12QueryHeap, (void**)&s_gq_heap)) || !s_gq_heap) return 0;
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof bd);
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 2 * GQ_N * 8; bd.Height = 1; bd.DepthOrArraySize = 1;
    bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&s_gq_rb)) || !s_gq_rb) return 0;
    s_gq_n[0] = s_gq_n[1] = 0;
    s_gq_on = 1;
    fprintf(stderr, "[GPUPROF] v199 GPU pass timestamps ON (GPUPROF=1), %.1f MHz%c", (double)fq / 1e6, 10);
    return 1;
}
static inline void gq_mark(u64 lab)
{
    if (s_gq_on <= 0) return;
    const int p = s_gq_par; const u32 n = s_gq_n[p];
    if (n >= GQ_N - 1) return;
    s_d3d.cmd_list->lpVtbl->EndQuery(s_d3d.cmd_list, s_gq_heap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(p * GQ_N + n));
    s_gq_lab[p][n] = lab; s_gq_n[p] = n + 1;
}
static void gq_label_str(u64 lab, char* o, size_t on)
{
    if (lab == GQ_PRE) { snprintf(o, on, "before VP pass (copies, conversions)"); return; }
    if (lab == GQ_VPBEGIN) { snprintf(o, on, "VP pass start"); return; }
    if (lab == GQ_POST) { snprintf(o, on, "after VP pass (composite, present copy)"); return; }
    const u32 w = (u32)(lab >> 44) & 0xFFFFF, h = (u32)(lab >> 24) & 0xFFFFF, nrt = (u32)(lab >> 8) & 0xF;
    snprintf(o, on, "%s %ux%u x%u%s", (lab & 0x10) ? "back" : "rt", w, h, nrt, (lab & 1) ? " +z" : "");
}
/* frame start (cmd list just reset, previous frame finished on the GPU): fold the previous frame in */
static void gq_frame_begin(void)
{
    if (!gq_active()) return;
    const int q = s_gq_par ^ 1;   /* the previous frame's half */
    const u32 n = s_gq_n[q];
    if (n >= 2) {
        D3D12_RANGE rr = { (SIZE_T)q * GQ_N * 8, (SIZE_T)(q * GQ_N + n) * 8 };
        void* m = NULL;
        if (SUCCEEDED(s_gq_rb->lpVtbl->Map(s_gq_rb, 0, &rr, &m)) && m) {
            const u64* t = (const u64*)m + (size_t)q * GQ_N;
            for (u32 i = 0; i + 1 < n; i++) {
                if (t[i + 1] < t[i]) continue;
                const double ms = (double)(t[i + 1] - t[i]) * s_gq_tick_ms;
                const u64 lab = s_gq_lab[q][i];
                u32 h = (u32)((lab * 0x9E3779B97F4A7C15ull) >> 56) % GQ_AGG;
                for (u32 k = 0; k < GQ_AGG; k++, h = (h + 1) % GQ_AGG) {
                    if (s_gq_agg[h].cnt == 0 || s_gq_agg[h].lab == lab) { s_gq_agg[h].lab = lab; s_gq_agg[h].ms += ms; s_gq_agg[h].cnt++; break; }
                }
            }
            if (t[n - 1] >= t[0]) s_gq_total_ms += (double)(t[n - 1] - t[0]) * s_gq_tick_ms;
            s_gq_frames++;
            D3D12_RANGE wr = {0, 0}; s_gq_rb->lpVtbl->Unmap(s_gq_rb, 0, &wr);
        }
    }
    if (s_gq_frames >= 600) {
        fprintf(stderr, "[GPUPROF] last %u frames: GPU %.2f ms/frame (start->end of the frame's command list); top passes:%c", s_gq_frames, s_gq_total_ms / s_gq_frames, 10);
        for (int r = 0; r < 14; r++) {
            int b = -1;
            for (int k = 0; k < GQ_AGG; k++) if (s_gq_agg[k].cnt && (b < 0 || s_gq_agg[k].ms > s_gq_agg[b].ms)) b = k;
            if (b < 0) break;
            char nm[96]; gq_label_str(s_gq_agg[b].lab, nm, sizeof nm);
            fprintf(stderr, "[GPUPROF]   %6.2f ms/frame  %5.1f passes/frame  %s%c", s_gq_agg[b].ms / s_gq_frames,
                    (double)s_gq_agg[b].cnt / s_gq_frames, nm, 10);
            s_gq_agg[b].cnt = 0; s_gq_agg[b].ms = 0;
        }
        memset(s_gq_agg, 0, sizeof s_gq_agg); s_gq_total_ms = 0; s_gq_frames = 0;
    }
    s_gq_par ^= 1; s_gq_n[s_gq_par] = 0;
    gq_mark(GQ_PRE);
}
/* before Close: end mark + resolve this frame's timestamps into its readback half */
static void gq_frame_end(void)
{
    if (s_gq_on <= 0) return;
    gq_mark(GQ_END);
    const int p = s_gq_par;
    if (s_gq_n[p]) s_d3d.cmd_list->lpVtbl->ResolveQueryData(s_d3d.cmd_list, s_gq_heap, D3D12_QUERY_TYPE_TIMESTAMP,
                                                           (UINT)(p * GQ_N), s_gq_n[p], s_gq_rb, (UINT64)p * GQ_N * 8);
}
static void render_frame_exec(void)
{
    double _rf0 = perf_on() ? perf_now() : 0.0;
    rft_start();
    u32 fi = s_d3d.frame_index;

    /* Drain the GPU before touching shared upload resources (vp_vb vertices,
     * vp_cb constants, per-frame texture staging): the previous frame's draws
     * may still be reading them, and overwriting mid-flight tears geometry
     * (gcm/cube: triangles mixing stale and new vertices -> missing/sliver
     * polygons). These workloads are a few draws/frame, so full serialisation
     * costs little. */
    /* v59: rsx_vtx_sync() + compile_vp() run in the render_frame() wrapper on the FIFO thread */
    { static u32 _vn = 0; if (ps3_telem_enabled() && s_va_n > 0 && (++_vn % 600) == 0) {
        fprintf(stderr, "[VTX_ASYNC] last 600 frames: %llu jobs, %llu waiting syncs, %llu tiny draws/%llu verts inline; indexed draws: %llu range, %llu hashed (v55)%c",
                (unsigned long long)s_va_jobs_total, (unsigned long long)s_va_syncs,
                (unsigned long long)s_va_inline_draws, (unsigned long long)s_va_inline_verts,
                (unsigned long long)s_vib_range_hits, (unsigned long long)s_vib_hash_hits, 10);
        s_va_jobs_total = 0; s_va_syncs = 0; s_va_inline_draws = 0; s_va_inline_verts = 0;
        s_vib_range_hits = 0; s_vib_hash_hits = 0;
        fprintf(stderr, "[FPMEMO] last 600 frames: %llu hits, %llu misses (v64)%c", (unsigned long long)s_fpm_hit, (unsigned long long)s_fpm_miss, 10);
        s_fpm_hit = s_fpm_miss = 0; } }
    const int _sf_telem = ps3_telem_enabled();
    LARGE_INTEGER _sf_f = {0}, _sf_a = {0}, _sf_b = {0};
    if (_sf_telem) { QueryPerformanceFrequency(&_sf_f); QueryPerformanceCounter(&_sf_a); }
    /* v225A RF_EARLY=1 (default off; needs RF_TRIPLE=1): the prepass (descriptor windows, texture uploads, pipeline
     * lookups -- ~5-8 ms in the city, CPU only) runs BEFORE waiting for the GPU to finish the previous frame, so the
     * GPU wait (2-5 ms in heavy views) overlaps it instead of adding to it. What the prepass writes that the previous
     * frame's GPU work may still read is split in two halves used alternately: the per-draw SRV windows and the
     * texture staging ring; per-texture fallback staging buffers are replaced instead of rewritten. Everything that
     * needs the previous frame finished (graveyard release, pool pages, probe readback, the FIFO's parity event) stays
     * after the wait. Frames with legacy (non-VP) draws or the font atlas run the old order. ABX 0x80000 = old order. */
    s_rf_early_frame = rf_early_on() && !s_d3d.tex_bound && !s_rf_legacy_draws && !(g_abx & s_rf_early_bit);
    { /* v228 RF_EARLY_PHASE=0/1: the early order only on even/odd A/B phases (in-run A/B without an ABX bit --
       * every ABX bit already means something when set). Layout (SRV halves, ring halves) stays RF_EARLY's. */
      static int ph = -2; extern volatile int g_ab_phase;
      if (ph == -2) { const char* e = getenv("RF_EARLY_PHASE"); ph = (e && *e) ? (atoi(e) & 1) : -1;
                      if (ph >= 0) fprintf(stderr, "[RF_EARLY] v228 early order only on A/B phase parity %d (RF_EARLY_PHASE)%c", ph, 10); }
      if (ph >= 0 && (g_ab_phase & 1) != ph) s_rf_early_frame = 0; }
    /* The early order resets cmd_allocators[fi] (below) before waiting for the GPU. That is safe only because fi
     * changes on every Present (FRAME_COUNT 2: the allocator last used two frames ago, finished by the previous
     * exec's wait). A FRAME_SPLIT part does not Present, so the next exec has the SAME fi and reset the allocator the
     * GPU was still executing from -> DEVICE_HUNG in that list's texture uploads (DRED: barrier/CopyTextureRegion,
     * 0 draws done; reproduced in seconds with FRAME_SPLIT_AT=2000, 2026-10-31). Same fi as the last exec -> wait
     * first (the old order) for this exec; the SRV / ring halves still alternate as RF_EARLY expects. */
    { static u32 s_prev_fi = 0xFFFFFFFFu;
      if (s_rf_early_frame && fi == s_prev_fi) {
          s_rf_early_frame = 0;
          static u32 n = 0; if (n++ < 4) fprintf(stderr, "[RF_EARLY] same back buffer as the previous exec (frame split / no Present): GPU wait first%c", 10);
      }
      s_prev_fi = fi; }
    { static u32 half = 0;
      if (rf_early_on()) half ^= 1u;
      s_dsrv_base = DRAW_SRV_BASE + half * (u32)(MAX_DRAWS * RSX_D3D_TEX_UNITS);
      if (rf_early_on() && s_tring_size) { s_tring_lo = half ? s_tring_size / 2 : 0; s_tring_hi = half ? s_tring_size : s_tring_size / 2; }
      else { s_tring_lo = 0; s_tring_hi = ~0ull; } }
    if (!s_rf_early_frame) tcs_kick(s_rf_draws, s_rf_count, s_tex_frame + 1);   /* v228 TEXCS_EARLY: checksums during the GPU wait */
    if (!s_rf_early_frame) {
    wait_for_gpu();
    rft_mark(RFT_GPU);
    black_probe_read();   /* v103A: the previous frame's probe copy has landed */
    if (s_rf_gpuidle_ev) SetEvent(s_rf_gpuidle_ev);   /* v59: the FIFO thread may now reuse the other parity */
    if (_sf_telem) {
        QueryPerformanceCounter(&_sf_b);
        s_sf_gpu_ms = (double)(_sf_b.QuadPart - _sf_a.QuadPart) * 1000.0 / (double)_sf_f.QuadPart;
        s_sf_t0 = _sf_a;
    }
    flush_graveyard();   /* everything deferred while recording earlier lists */
    tp_flush_frees();    /* v32f: their pool pages are free again */
    } else { rft_mark(RFT_GPU); if (_sf_telem) s_sf_t0 = _sf_a;
             s_rf_grave_mark = s_grave_n; s_rf_tp_mark = s_tp_pending_n; }   /* v235C: older generation only */
    s_tex_frame++;
    cp_begin_frame();    /* v33: pass capture armed by F9 / CAP_AUTO? */
    tex_ring_reset();    /* v32e: GPU idle -> the texture staging ring is free again (RF_EARLY: this frame's half) */

    /* Compile the real vertex program once its microcode is captured, and keep
     * the constant bank uploaded for the VS. */
    /* The base VP pipeline gates the ENTIRE vertex-program draw pass below
     * (`if (s_d3d.vp_ready ...)`). The old trigger also required the microcode
     * size to differ from the last compile, so a title whose first captured
     * program has the same byte count as vp_compiled_bytes never built the base
     * pipeline -- and then every is_vp draw record was silently dropped: draws
     * recorded, constants uploaded, per-draw VS compiled and cached, and not one
     * of them submitted. While !vp_ready there is nothing to be stale against,
     * so only the "have microcode" test belongs here. */

    /* Per-draw VP constants are snapshotted at record time (vp_record_cb). */

    { static int on = -1; static ULONGLONG t0 = 0; static int phase = -1, dumped = 0;
      static char nm[32];
      if (on < 0) on = getenv("BISECT") ? 1 : 0;
      if (on && !t0 && s_dbg_last_draws >= 500) t0 = GetTickCount64();
      if (on && t0) {
          ULONGLONG el = GetTickCount64() - t0;
          int ph = (int)(el / 20000ull); if (ph > 4) ph = 4;
          if (ph != phase) {
              phase = ph; dumped = 0;
              /* v13 order: p0 as shipped | p1 literal FP constants keyed on all
               * bytes | p2 + address in the key (= v8's shader setup) | p3 + memo
               * and SRV dedupe off | p4 + disk cache off. Cumulative. */
              s_bis_literal = ph >= 1;
              s_bis_kaddr   = ph >= 2;
              s_bis_nomemo  = ph >= 3;
              s_bis_nosrvdd = ph >= 3;
              s_bis_nodisk  = ph >= 4;
              fprintf(stderr, "[BISECT] phase %d: fp_consts=%s key=%s memo=%s srv_dedupe=%s disk_cache=%s (pipelines %d)%c", ph,
                      s_bis_literal ? "LITERAL" : "buffered", s_bis_kaddr ? "ADDRESS" : "code",
                      s_bis_nomemo ? "OFF" : "on", s_bis_nosrvdd ? "OFF" : "on",
                      s_bis_nodisk ? "OFF" : "on", s_d3d.vp_fp_n, 10);
          }
          if (!dumped && el >= (ULONGLONG)ph * 20000ull + 12000ull) {
              dumped = 1;
              snprintf(nm, sizeof nm, "bisect_p%d", ph);
              s_dump_name = nm;
              s_d3d.dump_frames_left = 1;
              s_d3d.dump_skip_left = 0;
              s_bis_active = 1;
              fprintf(stderr, "[BISECT] saving bisect_p%d_*.bmp%c", ph, 10);
          }
      } }
    /* Lazily create the readback buffer the first time a dump is requested. */
    if (s_d3d.dump_frames_left > 0 && !s_d3d.readback_buf) {
        s_d3d.readback_pitch = (BBW * 4 + 255) & ~255u;   /* v154: backbuffer size */
        D3D12_HEAP_PROPERTIES hp = {0};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {0};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (u64)s_d3d.readback_pitch * BBH;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.readback_buf);
    }

    /* Reset command allocator and list */
    s_d3d.cmd_allocators[fi]->lpVtbl->Reset(s_d3d.cmd_allocators[fi]);
    s_d3d.cmd_list->lpVtbl->Reset(s_d3d.cmd_list, s_d3d.cmd_allocators[fi], NULL);
    cmd_state_reset();
    gq_frame_begin();   /* v199 GPUPROF */
    vc_exec_copies(s_rf_parity);   /* v60 VCACHE: misses of this frame land before any draw */

    /* Upload the bound font atlas into an R8_UNORM texture and create its SRV.
     * The atlas is a linear 8-bit coverage map, so a straight row copy (no
     * deswizzle) suffices. It is a DYNAMIC glyph cache -- the game rasterizes
     * new glyphs into it over time -- so re-upload whenever it is dirty (set on
     * every bind), not just once. Uploading only on first bind left every glyph
     * added after frame 1 sampling stale texels -> torn text. The backend is
     * synchronous (wait_for_gpu each frame), so recreating the resource per
     * dirty frame is safe. */
    rft_mark(RFT_SETUP);
    if (ps3_telem_enabled() && s_d3d.tex_bound) s_rft_texbound++;
    if (ps3_telem_enabled() && s_rf_legacy_draws) s_rft_legacy++;
    if (ps3_telem_enabled() && s_rf_legacy_tex_draws) s_rft_legacy_tex++;
    if (s_d3d.tex_bound && s_d3d.tex_ready && s_d3d.tex_dirty && !s_rf_legacy_tex_draws && !atlas_reup_old())
        if (ps3_telem_enabled()) s_rft_atlas_skip++;   /* v187: nothing samples it this frame -> keep the last upload (stays dirty) */
    else
    if (s_d3d.tex_bound && (!s_d3d.tex_ready || s_d3d.tex_dirty) && s_d3d.srv_heap && s_d3d.pipeline_state_tex) {
        extern uint8_t* vm_base;
        if (ps3_telem_enabled()) s_rft_atlas_up++;
        u32 w = s_d3d.tex_w, h = s_d3d.tex_h;
        u32 pitch = (w + 255) & ~255u;   /* D3D12 requires 256-byte row pitch */

        if (s_d3d.tex_resource) { s_d3d.tex_resource->lpVtbl->Release(s_d3d.tex_resource); s_d3d.tex_resource = NULL; s_srv_gen++; }
        if (s_d3d.tex_upload)   { s_d3d.tex_upload->lpVtbl->Release(s_d3d.tex_upload);     s_d3d.tex_upload = NULL; }

        D3D12_HEAP_PROPERTIES hp_def = {0}; hp_def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {0};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        HRESULT thr = s_d3d.device->lpVtbl->CreateCommittedResource(
            s_d3d.device, &hp_def, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL,
            &IID_ID3D12Resource, (void**)&s_d3d.tex_resource);

        D3D12_HEAP_PROPERTIES hp_up = {0}; hp_up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {0};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = (u64)pitch * h; ud.Height = 1; ud.DepthOrArraySize = 1; ud.MipLevels = 1;
        ud.SampleDesc.Count = 1; ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (SUCCEEDED(thr))
            thr = s_d3d.device->lpVtbl->CreateCommittedResource(
                s_d3d.device, &hp_up, D3D12_HEAP_FLAG_NONE, &ud,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                &IID_ID3D12Resource, (void**)&s_d3d.tex_upload);

        if (SUCCEEDED(thr) && vm_base) {
            void* mapped = NULL;
            D3D12_RANGE nr = {0, 0};
            if (SUCCEEDED(s_d3d.tex_upload->lpVtbl->Map(s_d3d.tex_upload, 0, &nr, &mapped)) && mapped) {
                const u8* srcbase = vm_base + s_d3d.tex_src_offset;
                for (u32 y = 0; y < h; y++)
                    memcpy((u8*)mapped + (u64)y * pitch, srcbase + (u64)y * w, w);
                s_d3d.tex_upload->lpVtbl->Unmap(s_d3d.tex_upload, 0, NULL);

                D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
                dst.pResource = s_d3d.tex_resource;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = 0;
                src.pResource = s_d3d.tex_upload;
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = 0;
                src.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width    = w;
                src.PlacedFootprint.Footprint.Height   = h;
                src.PlacedFootprint.Footprint.Depth    = 1;
                src.PlacedFootprint.Footprint.RowPitch = pitch;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

                D3D12_RESOURCE_BARRIER tb = {0};
                tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                tb.Transition.pResource   = s_d3d.tex_resource;
                tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &tb);

                D3D12_SHADER_RESOURCE_VIEW_DESC sv = {0};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE sh;
                RSX_D3D12_CPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &sh);
                s_d3d.device->lpVtbl->CreateShaderResourceView(s_d3d.device, s_d3d.tex_resource, &sv, sh);

                s_d3d.tex_ready = 1;
                s_d3d.tex_dirty = 0;   /* content now in sync with guest atlas */
                { static int _au = 0; if ((_au < 3 && _au++ < 3))
                    printf("[D3D12] atlas uploaded (%ux%u R8) -> textured\n", w, h); }
            }
        }
    }
    rft_mark(RFT_ATLAS);

    /* Frames with no ops (init/boot presents) don't count against the cap --
     * PNG-decode-heavy titles burn hundreds of empty presents before the first
     * real draw. */
    if (getenv("RTT_DUMP") && s_rf_count > 0) { static int _f=0; int _cap = atoi(getenv("RTT_DUMP")); if (_cap < 2) _cap = 14; if ((_f < _cap && _f++ < _cap)) {
        fprintf(stderr, "[RTT] frame %d: %u ops\n", _f, s_rf_count);
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_rf_draws[_d];
            fprintf(stderr, "[RTT]  op%02u %s rt=0x%X mrt=0x%X/0x%X/0x%X t0=0x%X fp=0x%X n=%u cmask=%X vp=%u,%u %ux%u blend=%d\n",
                _d, r->is_clear?"CLR ":(r->is_vp?"draw":"leg "), r->rt_off,
                r->rt_mrt[0], r->rt_mrt[1], r->rt_mrt[2],
                r->tex[0].raw, r->fp_addr, r->vertex_count,
                r->cmask, r->vp_x, r->vp_y, r->vp_w, r->vp_h, r->blend);
        }
    } }

    /* Debug: RTT_PASS=N shows pass N's output directly on screen (drops later
     * ops, retargets pass N to the backbuffer). */
    { const char* rp = getenv("RTT_PASS");
      if (rp) {
        int keep = atoi(rp), seen = 0; u32 cut = s_rf_count;
        int viewrt = getenv("RTT_VIEWRT") != NULL;
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
            D3D12DrawRecord* r = &s_rf_draws[_d];
            if (r->is_clear || !r->is_vp) continue;
            if (seen == keep) {
                /* Alone: retarget op N to the backbuffer. With RTT_VIEWRT:
                 * keep op N intact and pull the display composite forward so
                 * the chosen RT is shown as of this point in the chain. */
                if (!viewrt) { r->rt_off = 0; r->rt_mrt[0] = r->rt_mrt[1] = r->rt_mrt[2] = 0; }
                cut = _d + 1;
                break;
            }
            seen++;
        }
        if (viewrt) {
            for (u32 _j = cut; _j < s_rf_count && _j < MAX_DRAWS; _j++) {
                D3D12DrawRecord* r = &s_rf_draws[_j];
                if (r->is_vp && !r->is_clear && r->rt_off == 0) {
                    if (cut < MAX_DRAWS) s_rf_draws[cut++] = *r;
                    break;
                }
            }
        }
        s_rf_count = (cut < s_rf_count) ? cut : s_rf_count;
      } }

    /* DRAW_LIMIT=<N>: keep only the first N recorded ops this frame. The duck's
     * draws are ops 00..~03 of every frame, so a small limit shows the ducks
     * alone -- separating "the ducks never rasterize" from "later geometry is
     * painted over them". Diagnostic only. */
    { static int lim = -1;
      if (lim < 0) { const char* e = getenv("DRAW_LIMIT"); lim = e ? atoi(e) : 0; }
      if (lim > 0 && s_rf_count > (u32)lim) s_rf_count = (u32)lim; }

    /* DEPTH_BLUR=0 (default 1 = the game's; 2 = limited to the powered-area range, applied when the program is
     * compiled -- see DEPTH BLUR there): drop the full-screen pass (rt = the scene, t0 depth, t2/t3 the 320x180 blurred
     * copies) that mixes a 320x180 blur into the 720p scene by depth -- the blackout areas' blur, which upscaled from
     * 320x180 looks blocky at RES_SCALE > 1. Skipping it leaves the scene as the fog pass wrote it (the pass reads
     * and writes the same target). Identified by the CODE-only hash of its fragment program (fp_scan code_h,
     * 29F3C342): the game loads its shaders at a different address each session (fp 0x00BCBA82, 0x00A45C02,
     * 0x00BCDC02 in three runs), and fp_uh ("fph" in rsxcap) covers the inline constants and differs between the
     * memo paths that fill it (73611844 vs 22435731 for identical bytes), so matching either flickered. code_h is
     * cached per fp_uh. */
    { static int on = -1;
      if (on < 0) { const char* e = getenv("DEPTH_BLUR"); on = (e && *e == '0') ? 1 : 0;
                    if (on) fprintf(stderr, "[DEPTHBLUR] depth blur pass (fp code 29F3C342) off%c", 10); }
      if (on) {
        static u32 ck[4096], cv[4096];
        extern uint8_t* vm_base;
        extern u32 cellGcmResolveLocated(int, u32);
        u32 keep = 0;
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
            const D3D12DrawRecord* r = &s_rf_draws[_d];
            u32 code = 0;
            if (!r->is_clear && r->fp_addr) {
                const u32 slot = r->fp_uh ? (r->fp_uh * 2654435761u) >> 20 : 0;
                if (r->fp_uh && ck[slot] == r->fp_uh) code = cv[slot];
                else {
                    const u8* uc = r->fp_uc;
                    u32 max = 1024;
                    if (!uc && vm_base) {
                        const u32 foff = cellGcmResolveLocated((r->fp_addr & 0x3u) == 1, r->fp_addr & ~0x3u);
                        if (foff != 0xFFFFFFFFu) { uc = vm_base + foff; max = 4096; }
                    }
                    u32 kc, fh, used;
                    if (uc && fp_scan(uc, max, &kc, &code, &fh, &used)) { if (r->fp_uh) { ck[slot] = r->fp_uh; cv[slot] = code; } }
                    else code = 0;
                }
            }
            if (code == 0x29F3C342u) continue;
            if (keep != _d) s_rf_draws[keep] = s_rf_draws[_d];
            keep++;
        }
        s_rf_count = keep;
      } }

    /* DRAW_SKIP_TEX=<hex raw offset>: drop every op binding that texture on
     * unit 0. The ducks sit behind the water's slab geometry, so removing the
     * occluder is the way to see whether the duck itself renders. Diagnostic. */
    { static const char* sk = (const char*)1; static u32 want = 0;
      if (sk == (const char*)1) sk = getenv("DRAW_SKIP_TEX");
      want = draw_filter_tex(sk);
      if (want) {
        u32 keep = 0;
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
            if (!s_rf_draws[_d].is_clear && s_rf_draws[_d].tex[0].raw == want) continue;
            if (keep != _d) s_rf_draws[keep] = s_rf_draws[_d];
            keep++;
        }
        s_rf_count = keep;
      } }

    /* DRAW_KEEP_TEX=<hex raw offset>: the inverse -- keep only the ops binding
     * that texture (plus clears). Isolates one object from everything drawn over
     * it, which is what finally shows the duck on its own. Diagnostic. */
    { static const char* kp = (const char*)1; static u32 want = 0;
      if (kp == (const char*)1) kp = getenv("DRAW_KEEP_TEX");
      want = draw_filter_tex(kp);
      /* DRAW_KEEP_NOCLEAR=1: also drop the clears. Keeping them preserves their
       * ORIGINAL position in the batch, so a clear that originally ran after the
       * kept draws still runs after them -- and wipes the very geometry being
       * isolated. That makes an object that renders fine look like it renders
       * nothing. */
      static int noclr = -1;
      if (noclr < 0) { const char* e = getenv("DRAW_KEEP_NOCLEAR"); noclr = e ? atoi(e) : 0; }
      /* DRAW_KEEP_BLEND=0|1: further restrict DRAW_KEEP_TEX to draws with that
       * blend flag. A mesh drawn twice -- once opaque, once blended -- is two
       * different passes (body vs reflection shell) that a texture filter alone
       * cannot separate. */
      static int kblend = -2;
      if (kblend == -2) { const char* e = getenv("DRAW_KEEP_BLEND");
                          kblend = e ? atoi(e) : -1; }
      if (want) {
        /* Emit the CLEARS FIRST, then the kept draws. Preserving the original
         * order means a clear that ran after them still runs after them and
         * wipes the isolated geometry; dropping the clears entirely (noclr)
         * leaves stale DEPTH that rejects most of its fragments. Neither shows
         * the object properly -- clear, then draw, does. */
        static D3D12DrawRecord kept[MAX_DRAWS];
        u32 nk = 0;
        if (!noclr)
            for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++)
                if (s_rf_draws[_d].is_clear && nk < MAX_DRAWS) kept[nk++] = s_rf_draws[_d];
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++)
            if (!s_rf_draws[_d].is_clear && s_rf_draws[_d].tex[0].raw == want
                && (kblend < 0 || s_rf_draws[_d].blend == kblend)
                && nk < MAX_DRAWS)
                kept[nk++] = s_rf_draws[_d];
        for (u32 _k = 0; _k < nk; _k++) s_rf_draws[_k] = kept[_k];
        s_rf_count = nk;
      } }

    /* DRAW_LAST_TEX=<hex raw offset>: move the ops binding that texture to the
     * END of the batch so nothing is drawn over them. The duck's draws are ops
     * 00..03 of every frame and the tub wall is drawn afterwards across the same
     * screen area, so anything that defeats the depth sort hides the duck
     * completely. Reordering separates "occluded" from "not rendered". */
    { static const char* lt = (const char*)1; static u32 want = 0;
      if (lt == (const char*)1) lt = getenv("DRAW_LAST_TEX");
      want = draw_filter_tex(lt);
      if (want && s_rf_count > 1) {
        static D3D12DrawRecord moved[MAX_DRAWS];
        u32 nm = 0, keep = 0;
        for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
            if (!s_rf_draws[_d].is_clear && s_rf_draws[_d].tex[0].raw == want) {
                if (nm < MAX_DRAWS) moved[nm++] = s_rf_draws[_d];
                continue;
            }
            if (keep != _d) s_rf_draws[keep] = s_rf_draws[_d];
            keep++;
        }
        for (u32 _m = 0; _m < nm && keep < MAX_DRAWS; _m++)
            s_rf_draws[keep++] = moved[_m];
        s_rf_count = keep;
      } }

    /* Render-to-texture pre-pass: make sure an offscreen RT resource exists for
     * every non-display surface targeted this frame (so draws binding it as a
     * texture can resolve to it below, whatever the op order). */
    for (int _i = 0; _i < MAX_OFF_RTS; _i++) s_d3d.off_rt[_i].used = 0;
    for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_rf_draws[_d];
        dr->rt_slot = (dr->is_vp && dr->rt_off)
            ? off_rt_get(dr->rt_off, dr->rt_w, dr->rt_h, dr->rt_fmt) : -1;
        if (dr->is_vp && !dr->rt_off && dr->disp_off && disp_as_rt())
            dr->rt_slot = off_rt_get(dr->disp_off, s_d3d.width, s_d3d.height, 0x8 /* A8R8G8B8 */);
        for (int _m = 0; _m < 3; _m++)
            dr->mrt_slot[_m] = (dr->is_vp && dr->rt_mrt[_m])
                ? off_rt_get(dr->rt_mrt[_m], dr->rt_w, dr->rt_h, dr->rt_fmt) : -1;
    }

    /* Per-frame VP textures + guest-FP pipelines: for each VP draw, upload the
     * texture it had bound at submit time into a slot (SRV heap 1+slot; plasma
     * animates so contents re-upload every frame) and pre-build its FP PSO.
     * A texture whose offset matches an offscreen RT samples the RT directly
     * (tex_slot 1000+idx) -- no guest-memory upload. */
    rft_mark(RFT_MISC);
    double _pre0 = perf_on() ? perf_now() : 0.0;
    for (int _i = 0; _i < VP_TEX_SLOTS; _i++) s_d3d.vp_tex[_i].used = 0;
    srv_alias_barrier();   /* v57: new frame, no descriptor window can cross it */
    tcs_prepare(s_rf_draws, s_rf_count);   /* v228 TEXCS: this frame's cached-texture checksums in parallel */
    static int _srv_alias = -1;
    if (_srv_alias < 0) { const char* e = getenv("SRV_ALIAS"); _srv_alias = (e && *e == '0') ? 0 : 1; }
    /* Snapshot feature state once for the batch instead of re-reading
     * static/phase state for every draw and texture unit in the hot
     * preparation loop.  A phase boundary naturally takes effect next batch. */
    const int _srv_assoc_frame = srv_alias_assoc_on();
    const int _shadows_frame = shadows_on();
    const int _zs_frame = zs_on();
    u64 _srv_aliased = 0, _srv_filled = 0;
    for (u32 _d = 0; _d < s_rf_count && _d < MAX_DRAWS; _d++) {
        D3D12DrawRecord* dr = &s_rf_draws[_d];
        if (!dr->is_vp || dr->is_clear) {
            /* A clear of an alias also makes it the offset's current surface. */
            if (dr->is_vp) { rt_latest_set(dr->rt_off ? dr->rt_off : dr->disp_off, dr->rt_slot);
                             for (int _m = 0; _m < 3; _m++) rt_latest_set(dr->rt_mrt[_m], dr->mrt_slot[_m]); }
            continue;
        }
        /* Debug: RTT_VIEWRT=<hex raw offset> makes display draws sample that
         * offscreen RT at t0 (the composite blit then shows it fullscreen). */
        { static const char* vr = (const char*)1;     /* hoisted: this runs per DRAW */
          if (vr == (const char*)1) vr = getenv("RTT_VIEWRT");
          if (vr && dr->rt_off == 0) {
              dr->tex[0].raw = (u32)strtoul(vr, NULL, 16);
              dr->tex[0].off = 0;
              dr->tex[0].set = 1;
          } }
        /* SCREENTEX=1: report draws that sample a DISPLAY-SIZED texture. On RSX
         * a title renders a reflection/refraction into a surface and then
         * samples it; our backend renders into a D3D backbuffer, so the guest
         * memory that sampler reads is never written and the texture comes back
         * empty. Finding which geometry does it is the first step to feeding it
         * the rendered image instead. */
        /* EMPTYTEX=1: which geometry binds a texture whose source is all zero.
         * An unresolved texture renders untextured, so this names the surfaces
         * affected instead of leaving it to guesswork. */
        { static int ed = -1;
          if (ed < 0) { const char* e = getenv("EMPTYTEX"); ed = e ? atoi(e) : 0; }
          extern uint8_t* vm_base;
          if (ed && vm_base) for (int _u = 0; _u < 4; _u++) {
              if (!dr->tex[_u].set || !dr->tex[_u].off) continue;
              u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
              for (u32 i2 = 0; i2 < sz && i2 < 0x20000u; i2 += 997)
                  if (vm_base[dr->tex[_u].off + i2]) { nz = 1; break; }
              if (!nz) {
                  static u32 seen[24]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 24) { seen[ns++] = dr->fp_addr;
                      /* Scan a window around the resolved address: if the pass
                       * that fills this surface ran but landed elsewhere, the
                       * data is nearby; if the whole window is zero it was never
                       * produced. */
                      /* Same offset through BOTH context DMAs: a texture the
                       * guest put in main memory but that we resolve as LOCAL
                       * reads as untouched VRAM, i.e. all zeros. */
                      { extern u32 cellGcmResolveLocated(int, u32);
                        u32 la = cellGcmResolveLocated(1, dr->tex[_u].raw);
                        u32 ma = cellGcmResolveLocated(0, dr->tex[_u].raw);
                        u32 ln = 0, mn = 0;
                        for (u32 i4 = 0; i4 < 0x20000u; i4 += 997) {
                            if (vm_base[la + i4]) ln++;
                            if (vm_base[ma + i4]) mn++;
                        }
                        fprintf(stderr, "[TEXLOC] raw=0x%08X LOCAL@0x%08X nz=%u  MAIN@0x%08X nz=%u%c",
                                dr->tex[_u].raw, la, ln, ma, mn, 10); }
                      { u32 base = dr->tex[_u].off & ~0xFFFFFu;
                        for (u32 w = 0; w < 0x400000u; w += 0x100000u) {
                            u32 nzc = 0;
                            for (u32 i3 = 0; i3 < 0x100000u; i3 += 1021)
                                if (vm_base[base + w + i3]) nzc++;
                            fprintf(stderr, "[EMPTYSCAN] 0x%08X..+1MB nonzero %u/1027%c",
                                    base + w, nzc, 10);
                        } }
                      fprintf(stderr, "[EMPTYTEX] fp=0x%X unit=%d raw=0x%08X res=0x%08X %ux%u"
                                      " fmt=0x%02X verts=%u vp=%u,%u %ux%u%c",
                              dr->fp_addr, _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                              dr->tex[_u].h, dr->tex[_u].fmt, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              }
          } }
        { static int sd = -1;
          if (sd < 0) { const char* e = getenv("SCREENTEX"); sd = e ? atoi(e) : 0; }
          if (sd) for (int _u = 0; _u < 4; _u++)
              if (dr->tex[_u].set && dr->tex[_u].w == s_d3d.width &&
                  dr->tex[_u].h == s_d3d.height) {
                  static u32 seen[16]; static int ns = 0; int known = 0;
                  for (int k = 0; k < ns; k++) if (seen[k] == dr->fp_addr) known = 1;
                  if (!known && ns < 16) { seen[ns++] = dr->fp_addr;
                      fprintf(stderr, "[SCREENTEX] unit=%d raw=0x%08X %ux%u fmt=0x%02X"
                                      " fp=0x%X verts=%u vp=%u,%u %ux%u%c",
                              _u, dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h,
                              dr->tex[_u].fmt, dr->fp_addr, dr->vertex_count,
                              dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h, 10); }
              } }
        /* Fill this draw's t0-t3 SRV window (DRAW_SRV_BASE + d*4): each unit
         * resolves to an offscreen RT (sampled directly), an uploaded guest
         * texture, or a null SRV. */
        double _sv0 = perf_on() ? perf_now() : 0.0;
        dr->zsamp_zs = -1; dr->zsamp_rt = -1;
        dr->shkey = 0; dr->shz[0] = dr->shz[1] = dr->shz[2] = dr->shz[3] = -1;   /* v34 */
        dr->srv_src = _d; dr->srv_src_frame = s_rf_frame_no;
        /* v46 SRV_ALIAS (default 1, 0 = off): 16 unit resolutions + descriptor writes per draw were
         * ~16 ms/frame (PERF srv). A draw whose 16 texture units are identical to the previous
         * plain draw's (no render-target / depth / shadow sources: those carry per-draw replay
         * work) binds that draw's window instead. Shadow-caster runs share one texture set. */
        int _aliased = 0;
        u64 _srv_hash = 0;
        int _srv_assoc = 0;
        if (_srv_alias && !_aliased) {
            _srv_hash = srv_alias_hash(dr);
            _srv_assoc = _srv_assoc_frame;
            const u32 _bucket = (u32)_srv_hash & (SRV_ALIAS_CACHE - 1u);
            const int _ways = _srv_assoc ? (int)SRV_ALIAS_WAYS : 1;
            for (int _way = 0; _way < _ways && !_aliased; _way++) {
                const SRVAliasEntry* ce = &s_srv_alias_cache[_bucket][_way];
                if (ce->generation == s_srv_alias_generation && ce->hash == _srv_hash && ce->draw < _d) {
                    const D3D12DrawRecord* sp = &s_rf_draws[ce->draw];
                    /* Verify the full key: hash collisions are never accepted. */
                    if (srv_alias_same(sp, dr)) {
                        for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++) dr->tex_rt[_u] = -1;
                        dr->srv_src = sp->srv_src;
                        _aliased = 1; _srv_aliased++;
                        if (_sf_telem) { if (_way == 0) s_srv_assoc_way0++; else s_srv_assoc_extra++; }
                    }
                }
            }
            if (_sf_telem && !_aliased) s_srv_assoc_miss++;
        }
        if (!_aliased) _srv_filled++;
        /* dr_cube_mask scans all 16 captured texture records and consults the
         * cube feature switch.  The mask cannot change while this draw is
         * prepared, so share one result across every texture-unit decision and
         * the PSO key instead of recomputing it up to five times per unit. */
        const u32 _cube_mask = dr_cube_mask(dr);
        for (int _u = 0; !_aliased && _u < RSX_D3D_TEX_UNITS; _u++) {
            u32 wslot = s_dsrv_base + _d * RSX_D3D_TEX_UNITS + (u32)_u;   /* v225A RF_EARLY halves */
            dr->tex_rt[_u] = -1;
            /* Stale RSX bindings are not dependencies of this fragment shader.
             * In particular they must not create depth conversions or shadow keys. */
            if (dr->fp_meta_valid && !(dr->fp_tex_mask & (1u << _u))) {
                srv_write_ex(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                             (int)((_cube_mask >> _u) & 1u));
                continue;
            }
            /* v34 SHADOWS: depth-format sampler (0x90-0x93) on a zeta surface of the same
             * size that this draw does not render into -> the depth itself (R24 view),
             * compared in the FP (fp_shadow_rewrite). Units 0-3 only (PSO key room). */
            if (_shadows_frame && _u < 4 && dr->tex[_u].set && !(_cube_mask & (1u << _u)) &&
                (dr->tex[_u].fmt & 0x9Fu) >= 0x90u && (dr->tex[_u].fmt & 0x9Fu) <= 0x93u) {
                int zi = zs_find_off(dr->tex[_u].raw & 0x7FFFFFFFu, dr->tex[_u].w, dr->tex[_u].h);
                if (zi < 0) zi = zs_find_off(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                u32 _zw = dr->tex[_u].w, _zh = dr->tex[_u].h; particle_size(&_zw, &_zh);   /* v154: scaled surfaces */
                if (zi >= 0 && s_zs[zi].res && s_zs[zi].w == _zw && s_zs[zi].h == _zh &&
                    !(dr->zeta_off && s_zs[zi].off == dr->zeta_off)) {
                    srv_write_ex(wslot, s_zs[zi].res, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                                 D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, 0);
                    dr->shkey |= (1u << _u) | ((dr->tex[_u].zf & 7u) << (4 + 3 * _u));
                    dr->shz[_u] = zi;
                    { static int _n = 0; if ((_n < 4 && _n++ < 4))
                        fprintf(stderr, "[SHADOW] draw %u unit %d: depth 0x%X %ux%u (fmt 0x%02X) compared with zfunc %u%c",
                                (unsigned)_d, _u, s_zs[zi].off, s_zs[zi].w, s_zs[zi].h, dr->tex[_u].fmt, dr->tex[_u].zf, 10); }
                    continue;
                }
            }
            /* Texture at a depth surface's offset (zeta read back as colour):
             * bind the colour copy that the replay refreshes before this draw.
             * Checked first: a 1280x720 depth texture would otherwise match the
             * SCREEN_AS_TEX substitution or fall through to stale guest VRAM. */
            if (_zs_frame && dr->tex[_u].set && !(_cube_mask & (1u << _u))) {
                int zi = -1;
                /* A real colour render target at this offset wins: only an address
                 * nothing renders colour into (inFamous: 0x1010000) is the depth. */
                { int _lr = off_rt_latest(dr->tex[_u].raw);
                  /* v73c: nor is a target only ever drawn with colour mask 0. inFamous'
                   * half-res depth 0x0F340000: the depth-export pass binds it as colour
                   * (cmask 0) AND zeta; the soft smoke then reads it as A8R8G8B8 = the
                   * depth. We bound the never-written colour target -> depth 0 -> every
                   * smoke fragment discarded. ZS_CWRITTEN=0 = old rule. */
                  static int _cw = -1; if (_cw < 0) { const char* e = getenv("ZS_CWRITTEN"); _cw = (e && *e == '0') ? 0 : 1; }
                  if (_lr < 0 || zs_is_conv_slot(_lr) || (_cw && !s_d3d.off_rt[_lr].cwritten)) {   /* v49: our own copy is not a colour target */
                    zi = zs_find_off(dr->tex[_u].raw & 0x7FFFFFFFu, dr->tex[_u].w, dr->tex[_u].h);
                    if (zi < 0) zi = zs_find_off(dr->tex[_u].raw, dr->tex[_u].w, dr->tex[_u].h);
                } }
                /* Only the A8R8G8B8 read-back (fmt base 0x85) is converted; depth-
                 * format reads (0x90-0x93, shadow maps with compare) are logged and
                 * keep the old path until they are implemented. */
                if (zi >= 0 && (dr->tex[_u].fmt & 0x9Fu) != 0x85u) {
                    static u32 seenf[8]; static int nsf = 0; int f = 0;
                    for (int q = 0; q < nsf; q++) if (seenf[q] == (dr->tex[_u].fmt & 0xFFu)) f = 1;
                    if (!f && nsf < 8) { seenf[nsf++] = dr->tex[_u].fmt & 0xFFu;
                        fprintf(stderr, "[ZETA] depth 0x%X sampled with texture fmt 0x%02X (%ux%u) -- not converted (TODO)%c",
                                s_zs[zi].off, dr->tex[_u].fmt, dr->tex[_u].w, dr->tex[_u].h, 10); }
                    zi = -1;
                }
                if (zi >= 0) {
                    int crt = zs_color_rt(zi);
                    if (crt >= 0) {
                        dr->zsamp_zs = zi; dr->zsamp_rt = crt;
                        dr->tex_rt[_u] = crt;
                        zset_add_wh(s_zset_colour, s_zset_colour_wh, &s_zset_colour_n,
                                    s_zs[zi].off,
                                    (dr->tex[_u].w << 16) | (dr->tex[_u].h & 0xFFFFu),
                                    "colour-sampled (same size is never skipped)");
                        srv_write(wslot, s_d3d.off_rt[crt].res, (DXGI_FORMAT)s_d3d.off_rt[crt].dxgi,
                                  rsx_remap_to_d3d(dr->tex[_u].ctrl1, 0x85u));
                        { static int _n = 0; if ((_n < 6 && _n++ < 6))
                            fprintf(stderr, "[ZETA] draw %u samples depth 0x%X on unit %d (fmt 0x%02X %ux%u) -> colour copy%c",
                                    (unsigned)_d, s_zs[zi].off, _u, dr->tex[_u].fmt, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        continue;
                    }
                }
            }
            /* The FP declares this unit as TextureCube iff its bit is in
             * dr_cube_mask. A 2D view in a TextureCube slot is undefined in
             * D3D12 and removed the device on the user's RTX 4060 (2026-10-04,
             * once units 4-15 were bound: inFamous has a cube map on unit 6).
             * So a cube unit only ever gets a cube upload or a cube-NULL view. */
            const int _decl_cube = (int)((_cube_mask >> _u) & 1u);
            /* v28: a DEPTH-format sampler (0x90-0x93) is a shadow-map lookup: the
             * RSX texture unit does the depth compare (TEXTURE_ADDRESS zfunc) and
             * the FP just reads the 0..1 result. Neither the compare nor the depth
             * surface is wired up yet; the unit used to keep the PREVIOUS texture
             * (random shadowing). Bind a view whose components are forced to 1 =
             * "fully lit" -- no shadows, but no garbage. SHADOW_LIT=0 -> old. */
            if (dr->tex[_u].set && !_decl_cube && (dr->tex[_u].fmt & 0x9Fu) >= 0x90u &&
                (dr->tex[_u].fmt & 0x9Fu) <= 0x93u) {
                ID3D12Resource* one = one_tex_get();
                if (one) {
                    srv_write_ex(wslot, one, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                                     D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
                                     D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
                                     D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1,
                                     D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1), 0);
                    zset_add_wh(s_zset_shadow, s_zset_shadow_wh, &s_zset_shadow_n,
                                dr->tex[_u].raw,
                                (dr->tex[_u].w << 16) | (dr->tex[_u].h & 0xFFFFu),
                                "shadow-only");
                    { static unsigned _n = 0; _n++;   /* v148: count (1,2,3, then every power of two) */
                      if (_n <= 3 || !(_n & (_n - 1)))
                        fprintf(stderr, "[SHADOW] unit %d depth tex 0x%X fmt 0x%02X %ux%u -> lit (1.0) [%u draws so far]%c",
                                _u, dr->tex[_u].raw, dr->tex[_u].fmt, dr->tex[_u].w, dr->tex[_u].h, _n, 10); }
                    continue;
                }
            }
            /* Display-sized sampler source -> the rendered frame. */
            /* Only a COLOUR-format sampler can be the rendered frame. inFamous'
             * Bink movie binds its 1280x720 luma plane (B8, fmt 0xA1) at exactly
             * the display size, and substituting the previous frame for it fed
             * the output back into the YUV->RGB shader every frame: the movie
             * came out as saturated black/white with ghost copies of the text
             * Single-channel and block-compressed
             * formats are never a colour buffer. SCREEN_AS_TEX=2 restores the
             * old any-format behaviour. */
            const u32 _sbf = dr->tex[_u].fmt & 0x9F;
            const int _scr_fmt_ok = !(_sbf == 0x81 || _sbf == 0x8B ||
                                      (_sbf >= 0x86 && _sbf <= 0x88));
            if (!_decl_cube && dr->tex[_u].set && s_screen_copy &&
                !(disp_as_rt() && off_rt_latest(dr->tex[_u].raw) >= 0) &&
                dr->tex[_u].w == s_d3d.width && dr->tex[_u].h == s_d3d.height) {
                static int en = -1;
                if (en < 0) { const char* e = getenv("SCREEN_AS_TEX"); en = e ? atoi(e) : 1; }
                if (en && (_scr_fmt_ok || en >= 2)) {
                    { static int _n = 0; if ((_n < 3 && _n++ < 3))
                        fprintf(stderr, "[SCREENTEX] bound frame copy at unit %d for"
                                        " fp=0x%X (%ux%u)%c", _u, dr->fp_addr,
                                dr->tex[_u].w, dr->tex[_u].h, 10); }
                    srv_write(wslot, s_screen_copy, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
            }
            /* A sampler whose guest buffer was never written, at the size of a
             * reduced-viewport pass we captured: that pass IS its producer. */
            if (!_decl_cube && dr->tex[_u].set && dr->tex[_u].off && s_subvp_n) {
                int hit = -1;
                for (int i = 0; i < s_subvp_n; i++)
                    if (s_subvp[i].res && s_subvp[i].w == dr->tex[_u].w &&
                        s_subvp[i].h == dr->tex[_u].h) { hit = i; break; }
                if (hit >= 0) {
                    extern uint8_t* vm_base;
                    u32 nz = 0, sz = dr->tex[_u].w * dr->tex[_u].h * 4u;
                    for (u32 i5 = 0; i5 < sz && i5 < 0x20000u; i5 += 997)
                        if (vm_base[dr->tex[_u].off + i5]) { nz = 1; break; }
                    if (!nz) {
                        { static int _n = 0; if (getenv("SUBVP_DBG") && (_n < 8 && _n++ < 8))
                            fprintf(stderr, "[SUBVP] unit %d fp=0x%X <- captured %ux%u%c",
                                    _u, dr->fp_addr, dr->tex[_u].w, dr->tex[_u].h, 10); }
                        srv_write(wslot, s_subvp[hit].res, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                        continue;
                    }
                }
            }
            if (dr->tex[_u].set) {
                int rt = _decl_cube ? -1 : off_rt_latest(dr->tex[_u].raw);
                if (rt >= 0) {
                    dr->tex_rt[_u] = rt;
                    /* Only actual shader dependencies reach here. A render target
                     * cannot be an SRV and RTV simultaneously; snapshot its input
                     * immediately before the draw (including MRT attachments). */
                    int feedback = rt == dr->rt_slot || rt == dr->mrt_slot[0] ||
                                   rt == dr->mrt_slot[1] || rt == dr->mrt_slot[2];
                    ID3D12Resource* source = feedback ? off_rt_feedback_get(rt) : s_d3d.off_rt[rt].res;
                    srv_write(wslot, source, (DXGI_FORMAT)s_d3d.off_rt[rt].dxgi,
                              (dr->tex[_u].fmt & 0x9Fu) == 0x85u ? rsx_remap_to_d3d(dr->tex[_u].ctrl1, 0x85u)
                                                                : D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING);
                    continue;
                }
                u32 _bf = dr->tex[_u].fmt & 0x9F;
                if (dr->tex[_u].off &&
                    (_bf == 0x81 || _bf == 0x85 || _bf == 0x8B || (_bf == 0x95 && tex95_upload()) /* v42b: DEVICE REMOVED when on -- off by default */ ||
                     (_bf >= 0x86 && _bf <= 0x88))) {
                    double _tt = perf_on() ? perf_now() : 0.0;
                    int _cube = dr->tex[_u].cube && (_cube_mask & (1u << _u));
                    int ts = vp_upload_tex_slot(dr->tex[_u].off, dr->tex[_u].w,
                                                dr->tex[_u].h, dr->tex[_u].fmt,
                                                _cube, dr->tex[_u].mips);
                    if (perf_on()) { s_perf_tex += perf_now() - _tt; s_perf_ntex++;
                        s_perf_texbytes += (u64)dr->tex[_u].w * dr->tex[_u].h * 4u; }
                    /* Both forms of the offset are in hand here; the filters
                     * compare the BOUND one. */
                    if (s_duck_off && dr->tex[_u].off == s_duck_off)
                        s_duck_raw = dr->tex[_u].raw;
                    if (ts >= 0) {
                        DXGI_FORMAT sf =
                            (_bf == 0x85) ? DXGI_FORMAT_R8G8B8A8_UNORM :
                            (_bf == 0x8B) ? DXGI_FORMAT_R8G8_UNORM :
                            (_bf == 0x86) ? DXGI_FORMAT_BC1_UNORM :
                            (_bf == 0x87) ? DXGI_FORMAT_BC2_UNORM :
                            (_bf == 0x88) ? DXGI_FORMAT_BC3_UNORM :
                            /* v73: 0x95 fell through to R8_UNORM -- an SRV format the
                             * RGBA8/R16G16 resource cannot be viewed as. THAT was the
                             * v42/v71 DEVICE REMOVED (0x887A0001 = invalid call). Use
                             * the class the uploader actually created. */
                            (_bf == 0x95) ? tex95_srv_dxgi(dr->tex[_u].fmt, dr->tex[_u].w, dr->tex[_u].h) :
                                            DXGI_FORMAT_R8_UNORM;
                        srv_write_ex(wslot, s_d3d.vp_tex[ts].res, sf,
                                  (_bf == 0x81) ? 0x1000
                                                : rsx_remap_to_d3d(dr->tex[_u].ctrl1, _bf),
                                  _cube);
                        continue;
                    }
                }
            }
            /* v244 [TEXNULL]: count texture units the game set that end up NULL (= a solid black object), reported
             * every 600 frames together with the out-of-slots count -- the background "black buildings" hunt.
             * Always on; costs one increment on the (rare) null path. */
            if (dr->tex[_u].set) {
                extern volatile long g_v244_tex_null, g_v244_tex_noslot;
                static u32 rep = 0; g_v244_tex_null++;
                if (s_frame_no / 600u != rep) { rep = s_frame_no / 600u;
                    fprintf(stderr, "[TEXNULL] v244 last ~600 frames: %ld set texture units bound NULL (black), %ld uploads out of slots%c",
                            g_v244_tex_null, g_v244_tex_noslot, 10);
                    g_v244_tex_null = 0; g_v244_tex_noslot = 0; } }
            /* TEX_BINDDBG=<N>: a unit that ends up with a NULL SRV. The draw is
             * still recorded, lit and transformed -- it just samples nothing,
             * which renders as a solid black object. */
            { static int cap = -1, n = 0;
              if (cap < 0) { const char* e = getenv("TEX_BINDDBG"); cap = e ? atoi(e) : 0; }
              if (cap && n < cap && dr->tex[_u].set) { n++;
                fprintf(stderr, "[TEXBIND] null SRV unit=%d raw=0x%X off=0x%X %ux%u fmt=0x%02X%c",
                        _u, dr->tex[_u].raw, dr->tex[_u].off, dr->tex[_u].w,
                        dr->tex[_u].h, dr->tex[_u].fmt, 10); } }
            srv_write_ex(wslot, NULL, DXGI_FORMAT_R8G8B8A8_UNORM,
                         D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, _decl_cube);
        }
        if (!_aliased) {   /* v57: cache plain windows; unsafe windows end the reusable run. */
            int _plain = (dr->zsamp_zs < 0 && dr->shkey == 0);
            for (int _u = 0; _plain && _u < RSX_D3D_TEX_UNITS; _u++) if (dr->tex_rt[_u] >= 0) _plain = 0;
            if (_plain && _srv_alias) {
                SRVAliasEntry ent;
                ent.hash = _srv_hash; ent.draw = _d; ent.generation = s_srv_alias_generation;
                srv_alias_note((u32)_srv_hash & (SRV_ALIAS_CACHE - 1u), ent, _srv_assoc);
            } else if (!_plain) {
                srv_alias_barrier();
            }
        }
        if (perf_on()) s_perf_srv += perf_now() - _sv0;
        double _ps0 = perf_on() ? perf_now() : 0.0;
        s_fp_meta_override = dr;
        dr->pso_memo = dr->fp_addr
            ? (s_fp_uc_override = dr->fp_uc, vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                            dr_num_rts(dr),
                            dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                       : DXGI_FORMAT_R8G8B8A8_UNORM,
                            (int)((u32)dr->fp_exp32 | (dr_texk(dr) << 8)), dr->cmask | (dr->mrtk << 4), dr->cull, _cube_mask | (dr->shkey << 16)))
            : NULL;
        s_fp_uc_override = NULL;
        s_fp_meta_override = NULL;
        dr->pso_memo_frame = s_rf_frame_no;
        if (perf_on()) s_perf_pso += perf_now() - _ps0;
        /* This draw's targets are now the newest surface at their offsets:
         * later draws sampling those offsets read THIS alias. */
        rt_latest_set(dr->rt_off ? dr->rt_off : dr->disp_off, dr->rt_slot);
        for (int _m = 0; _m < 3; _m++) rt_latest_set(dr->rt_mrt[_m], dr->mrt_slot[_m]);
        if (dr->cmask & 0xFu) {   /* v73c */
            if (dr->rt_slot >= 0 && dr->rt_slot < MAX_OFF_RTS) s_d3d.off_rt[dr->rt_slot].cwritten = 1;
            for (int _m = 0; _m < 3; _m++)
                if (dr->rt_mrt[_m] && dr->mrt_slot[_m] >= 0 && dr->mrt_slot[_m] < MAX_OFF_RTS)
                    s_d3d.off_rt[dr->mrt_slot[_m]].cwritten = 1;
        }
    }

    if (_sf_telem) { static u64 a = 0, f = 0; static u32 nfr = 0; a += _srv_aliased; f += _srv_filled;   /* v46 */
      if ((++nfr % 300) == 0) {
          fprintf(stderr, "[SRV_ALIAS] last 300 frames: %llu draws re-bound a previous window, %llu filled | v230 way0 %llu extra-way %llu misses %llu%c",
                  (unsigned long long)a, (unsigned long long)f,
                  (unsigned long long)s_srv_assoc_way0, (unsigned long long)s_srv_assoc_extra,
                  (unsigned long long)s_srv_assoc_miss, 10);
          a = f = 0; s_srv_assoc_way0 = s_srv_assoc_extra = s_srv_assoc_miss = 0;
      } }
    if (s_rf_early_frame) {   /* v225A RF_EARLY: now wait for the previous frame (see the top of render_frame_exec) */
        LARGE_INTEGER _e0 = {0}, _e1 = {0}; if (_sf_telem) QueryPerformanceCounter(&_e0);
        wait_for_gpu();
        black_probe_read();
        if (s_rf_gpuidle_ev) SetEvent(s_rf_gpuidle_ev);
        if (_sf_telem) {
            QueryPerformanceCounter(&_e1);
            s_sf_gpu_ms = (double)(_e1.QuadPart - _e0.QuadPart) * 1000.0 / (double)_sf_f.QuadPart;
        }
        flush_graveyard_upto(s_rf_grave_mark);   /* v235C: not what this frame's own prepass deferred */
        tp_flush_frees_upto(s_rf_tp_mark);
        { static u32 n = 0; if (_sf_telem && (++n % 1800) == 1) fprintf(stderr, "[RF_EARLY] v225A prepass before the GPU wait (frame %u: wait %.2f ms after the prepass)%c", n, s_sf_gpu_ms, 10); }
    }
    /* Transition render target to RENDER_TARGET state */
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = s_d3d.render_targets[fi];
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

    /* Get RTV handle for current frame */
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle;
    RSX_D3D12_CPU_HEAP_START(s_d3d.rtv_heap)(s_d3d.rtv_heap, &rtv_handle);
    rtv_handle.ptr += fi * s_d3d.rtv_descriptor_size;

    /* Get DSV handle (single depth buffer shared across frames) */
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle;
    RSX_D3D12_CPU_HEAP_START(s_d3d.dsv_heap)(s_d3d.dsv_heap, &dsv_handle);

    if (perf_on()) s_perf_pre += perf_now() - _pre0;
    rft_mark(RFT_PRE);
    /* Set render target + depth */
    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);

    /* Clear color and depth */
    s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
        s_d3d.cmd_list, rtv_handle, s_d3d.clear_color, 0, NULL);
    s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(
        s_d3d.cmd_list, dsv_handle,
        D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
        1.0f, 0, 0, NULL);

    /* Set viewport and scissor */
    D3D12_VIEWPORT viewport = {0, 0, (float)BBW, (float)BBH, 0.0f, 1.0f};
    D3D12_RECT scissor = {0, 0, (LONG)BBW, (LONG)BBH};
    cmd_rect(&viewport, &scissor);

    /* Bind pipeline state and push MVP if anything to draw */
    if (s_d3d.pipeline_ready && s_rf_count > 0) {
        s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.root_signature);
        s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &s_d3d.vb_view);

        /* Make the atlas SRV heap current and bind its table (t0) for textured
         * draws. Safe to set even if no draw is textured. */
        if (s_d3d.tex_ready && s_d3d.srv_heap) {
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            RSX_D3D12_GPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
        }

        /* Push the MVP matrix from RSX vertex constants slots 0..3.
         * If the game hasn't written any constants (e.g. placeholder data
         * already in clip space), fall back to identity. */
        float mvp[16];
        const rsx_state* st = s_d3d.current_rsx_state;
        int have_mvp = 0;
        if (st) {
            for (u32 r = 0; r < 4; r++) {
                for (u32 c = 0; c < 4; c++) {
                    float v = st->vertex_constants[r][c];
                    mvp[r * 4 + c] = v;
                    if (v != 0.0f) have_mvp = 1;
                }
            }
        }
        if (!have_mvp) {
            memset(mvp, 0, sizeof(mvp));
            mvp[0] = mvp[5] = mvp[10] = mvp[15] = 1.0f; /* identity */
        }
        s_d3d.cmd_list->lpVtbl->SetGraphicsRoot32BitConstants(
            s_d3d.cmd_list, 0 /*root param 0*/, 16, mvp, 0);

        /* D3D12_IQ=1: dump exactly what the GPU is about to see -- the MVP root
         * constants and the first uploaded host vertices. The legacy path draws
         * with no validation errors yet produces zero pixels, so the geometry
         * must be degenerate/off-screen post-VS. */
        { static int _vd = -1;
          if (_vd < 0) { const char* e = getenv("D3D12_IQ"); _vd = e ? 1 : 0; }
          static int _n = 0;
          if (_vd && _n < 3) {
            _n++;
            fprintf(stderr, "[D3D12-DBG] have_mvp=%d draw_count=%u vb_offset=%u\n",
                    have_mvp, s_rf_count, s_d3d.vb_offset);
            fprintf(stderr, "[D3D12-DBG] mvp rows: [%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]"
                            "[%.2f %.2f %.2f %.2f][%.2f %.2f %.2f %.2f]\n",
                    mvp[0],mvp[1],mvp[2],mvp[3], mvp[4],mvp[5],mvp[6],mvp[7],
                    mvp[8],mvp[9],mvp[10],mvp[11], mvp[12],mvp[13],mvp[14],mvp[15]);
            if (s_d3d.vb_mapped) {
                /* host vertex = 9 floats (pos3 + col4 + uv2), VERTEX_STRIDE=36 */
                const float* bv = (const float*)s_d3d.vb_mapped;
                for (int k = 0; k < 3; k++) {
                    const float* v = bv + k * 9;
                    fprintf(stderr, "[D3D12-DBG]  hostvert[%d] pos=(%.3f,%.3f,%.3f) col=(%.2f,%.2f,%.2f,%.2f)\n",
                            k, v[0], v[1], v[2], v[3], v[4], v[5], v[6]);
                }
            }
            for (u32 d = 0; d < s_rf_count && d < 3; d++) {
                const D3D12DrawRecord* dr = &s_rf_draws[d];
                fprintf(stderr, "[D3D12-DBG]  draw[%u] is_vp=%d is_clear=%d topo=%u cnt=%u vbofs=%u startv=%u rt=%u\n",
                        d, dr->is_vp, dr->is_clear, dr->topology, dr->vertex_count,
                        dr->vb_byte_offset, dr->vb_byte_offset / VERTEX_STRIDE, dr->rt_off);
            }
            fflush(stderr);
          } }

        /* Replay each recorded draw with its own primitive topology and
         * the matching PSO class (triangle / line / point). The PSO class
         * must match the topology or D3D12 rejects the draw. */
        u32 last_topo = 0xFFFFFFFFu;
        ID3D12PipelineState* last_pso = NULL;
        u32 draws = s_rf_count;
        if (draws > MAX_DRAWS) draws = MAX_DRAWS;
        for (u32 d = 0; d < draws; d++) {
            const D3D12DrawRecord* dr = &s_rf_draws[d];
            if (dr->is_vp) continue; /* drawn by the VP pass below */

            /* Select PSO: textured triangles (dbgfont) use the atlas PSO;
             * otherwise pick by topology class. */
            ID3D12PipelineState* target_pso = s_d3d.pipeline_state; /* default triangle */
            if (dr->textured && s_d3d.tex_ready && s_d3d.pipeline_state_tex) {
                target_pso = s_d3d.pipeline_state_tex;
            } else if (dr->topology == RSX_TOPOLOGY_POINTS) {
                target_pso = s_d3d.pipeline_state_points
                             ? s_d3d.pipeline_state_points : s_d3d.pipeline_state;
            } else if (dr->topology == RSX_TOPOLOGY_LINES ||
                       dr->topology == RSX_TOPOLOGY_LINE_STRIP) {
                target_pso = s_d3d.pipeline_state_lines
                             ? s_d3d.pipeline_state_lines : s_d3d.pipeline_state;
            }
            if (target_pso != last_pso) {
                cmd_pso(target_pso);
                last_pso = target_pso;
            }
            if (dr->topology != last_topo) {
                s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, topo_to_d3d(dr->topology));
                last_topo = dr->topology;
            }
            u32 start_vert = dr->vb_byte_offset / VERTEX_STRIDE;
            s_d3d.cmd_list->lpVtbl->DrawInstanced(
                s_d3d.cmd_list, dr->vertex_count, 1, start_vert, 0);
        }
    }

    /* VP pass: real decompiled vertex program + atlas alpha-test PS. Feeds raw
     * float4 attrib0 from vp_vb and the vp_c[] constant bank. */
    if (s_d3d.vp_ready && s_rf_count > 0) {
        int any = 0;
        for (u32 d = 0; d < s_rf_count && d < MAX_DRAWS; d++)
            if (s_rf_draws[d].is_vp) { any = 1; break; }
        /* Textured geometry (dbgfont atlas) uses the sampling PS; untextured 3D
         * (vkcube) uses the colour-only PS. Fall back to whichever exists. */
        ID3D12PipelineState* vpso =
            (s_d3d.tex_ready && s_d3d.pipeline_state_vp) ? s_d3d.pipeline_state_vp
                                                         : s_d3d.pipeline_state_vp_color;
        if (!vpso) vpso = s_d3d.pipeline_state_vp;
        { static int cap = -1, n = 0;
          if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
          if (cap && n < cap) { n++;
            u32 nvp = 0, nclr = 0;
            for (u32 d2 = 0; d2 < s_rf_count && d2 < MAX_DRAWS; d2++) {
                if (s_rf_draws[d2].is_vp)    nvp++;
                if (s_rf_draws[d2].is_clear) nclr++;
            }
            fprintf(stderr, "[VPPASS] records=%u is_vp=%u clears=%u any=%d vpso=%p rootsig=%p\n",
                    s_rf_count, nvp, nclr, any,
                    (void*)vpso, (void*)s_d3d.vp_root_sig); } }
        if (any && vpso) {
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootSignature(s_d3d.cmd_list, s_d3d.vp_root_sig);
            cmd_pso(vpso);
            s_d3d.cmd_list->lpVtbl->IASetPrimitiveTopology(s_d3d.cmd_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0,
                s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb));
            ID3D12DescriptorHeap* heaps[] = { s_d3d.srv_heap, s_d3d.samp_heap };
            s_d3d.cmd_list->lpVtbl->SetDescriptorHeaps(s_d3d.cmd_list, s_d3d.samp_heap ? 2 : 1, heaps);
            D3D12_GPU_DESCRIPTOR_HANDLE gh;
            RSX_D3D12_GPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &gh);
            s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
            D3D12_GPU_DESCRIPTOR_HANDLE samp_base = {0};
            int cur_samp = -1;
            if (s_d3d.samp_heap) {
                RSX_D3D12_GPU_HEAP_START(s_d3d.samp_heap)(s_d3d.samp_heap, &samp_base);
                s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 3, samp_base);
                cur_samp = 0;
            }
            D3D12_VERTEX_BUFFER_VIEW vbv;
            /* Chunk 0 of this frame's parity; draws in later chunks rebind
             * (cur_vchunk) right before they are issued. */
            u32 cur_vchunk = 0;
            ID3D12Resource* vchunk0 = s_vpc_res[s_rf_parity][0]
                                    ? s_vpc_res[s_rf_parity][0] : s_d3d.vp_vb;
            vbv.BufferLocation = vchunk0->lpVtbl->GetGPUVirtualAddress(vchunk0);
            vbv.SizeInBytes    = s_vp_max_verts * 256;
            vbv.StrideInBytes  = 256;   /* 16 float4 attrib slots */
            s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
            D3D12_GPU_DESCRIPTOR_HANDLE gh_base;
            RSX_D3D12_GPU_HEAP_START(s_d3d.srv_heap)(s_d3d.srv_heap, &gh_base);
            /* The record side already interns SRV windows and reuses identical
             * VP/FP constant slots. Carry that information through to the
             * command list: setting an unchanged root table/CBV is still a
             * driver call and this title submits thousands of draws per frame.
             * These caches are local to this root-signature lifetime; the one
             * auxiliary pass that overwrites t0 invalidates cur_srv below. */
            u64 cur_srv = gh_base.ptr;
            const u64 vp_cb_base = s_d3d.vp_cb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_cb);
            const u64 fp_cb_base = s_d3d.vp_fpcb
                                 ? s_d3d.vp_fpcb->lpVtbl->GetGPUVirtualAddress(s_d3d.vp_fpcb) : 0;
            u64 cur_vcb = vp_cb_base;
            u64 cur_fcb = ~(u64)0;
            D3D12_VERTEX_BUFFER_VIEW cur_vbv = vbv;
            u64 bind_srv_skip = 0, bind_vcb_skip = 0, bind_fcb_skip = 0, bind_vb_skip = 0;
            int cur_rt = -1;                       /* target A: -1 = backbuffer */
            float vp_last_w = 0, vp_last_h = 0;    /* for DRAWARGS */
            float dvp_x = 0, dvp_y = 0, dvp_w = 0, dvp_h = 0;  /* per-draw, for DRAWARGS */
            int cur_m[3] = {-1, -1, -1};           /* MRT B/C/D: -1 = unbound   */
            int cur_zs = -2;                       /* bound zeta surface (-1 shared, -2 none yet) */
            u32 cur_sref = 0xFFFFFFFFu;            /* v33 stencil reference */
            int lsk_prev = 0, lsk_prev_zs = -3;    /* v147: previous draw was coarse-world (LOD_SKY_ONLY=2) */
            int last_disp_slot = -1;               /* display buffer (as OffRT) of the last display draw */
            u64 disp_drawn = 0;                    /* v62b: OffRT slots that got real (non-clear) display draws */
            double _rec0 = perf_on() ? perf_now() : 0.0;
            int seen_small_vp = 0, captured = 0;
            gq_mark(GQ_VPBEGIN);   /* v199 */
#ifdef PS3RECOMP_DLSS
            const u32 dlss_last_scene = rsx_dlss_exec_last_scene_draw();   /* DLAA: copy the scene depth after this draw */
            const u32 dlss_composite = rsx_dlss_exec_composite_draw();     /* ... and the HUD-less image after this one */
#endif
            for (u32 d = 0; d < s_rf_count && d < MAX_DRAWS; d++) {
                const D3D12DrawRecord* dr = &s_rf_draws[d];
                if (!dr->is_vp) continue;
                /* v73d CAP_EACH_VS=<dec vs_idx>[,..]: during an F9 pass capture, snapshot the bound
                 * targets right after every draw using one of these vertex programs (per-op view
                 * of e.g. the sky/cloud/water draws without spending the budget on everything). */
                { static int ne = -1; static u32 each[16]; static int prev_hit = 0;
                  if (ne < 0) { ne = 0; const char* e = getenv("CAP_EACH_VS");
                      while (e && *e && ne < 16) { each[ne++] = (u32)strtoul(e, NULL, 10); e = strchr(e, ','); if (e) e++; } }
                  if (ne && s_cp_active) {
                      if (prev_hit && cur_rt != -99) {
                          int _sl[4] = {cur_rt, cur_m[0], cur_m[1], cur_m[2]};
                          s_cp_force = 1; cp_snap(_sl, 4, d ? d - 1 : 0); s_cp_force = 0; s_cp_lastop = d;
                          cur_rt = -99; cur_zs = -2;
                      }
                      prev_hit = 0;
                      if (!dr->is_clear) for (int k = 0; k < ne; k++) if (each[k] == (u32)dr->vs_idx) prev_hit = 1;
                  } }
                /* Depth sampled as a texture: refresh its colour copy first
                 * (only if the surface was drawn/cleared since the last copy). */
                if (!dr->is_clear && dr->zsamp_zs >= 0 && dr->zsamp_rt >= 0 &&
                    s_zs[dr->zsamp_zs].res && (s_zs[dr->zsamp_zs].dirty || s_zs[dr->zsamp_zs].need_init)) {
                    if (s_cp_active && cur_rt != -99) { int _sl[4] = {cur_rt, cur_m[0], cur_m[1], cur_m[2]};
                        cp_snap(_sl, 4, d ? d - 1 : 0); cp_snap_zs(cur_zs, d ? d - 1 : 0); s_cp_lastop = d; }
                    zs_convert(dr->zsamp_zs, dr->zsamp_rt);
                    cur_rt = -99;              /* force the target/viewport rebind below */
                    cur_zs = -2;
                    cur_srv = ~(u64)0;          /* zs_convert binds its private depth SRV table */
                }
                int want_zs = (zs_on() && dr->zeta_off) ? zs_get(dr->zeta_off, dr->zw, dr->zh) : -1;
                /* v147 LOD_SKY_ONLY=2: before the first draw of a coarse-world run, mark the sky
                 * region (stencil bit 0x80) on its zeta; then force the normal target rebind. */
                if (!dr->is_clear) {
                    const int lsk_c = lsk_is_coarse(dr);
                    if (lsk_c && want_zs >= 0 && !(lsk_prev && lsk_prev_zs == want_zs)) {
                        if (s_cp_active && cur_rt != -99) { int _sl[4] = {cur_rt, cur_m[0], cur_m[1], cur_m[2]};
                            cp_snap(_sl, 4, d ? d - 1 : 0); cp_snap_zs(cur_zs, d ? d - 1 : 0); s_cp_lastop = d; }
                        lsk_mark(want_zs, dsv_handle);
                        cur_rt = -99; cur_zs = -2; cur_sref = 0xFFFFFFFFu;
                    }
                    lsk_prev = lsk_c; lsk_prev_zs = want_zs;
                }
                /* The reduced-viewport passes (this title's reflection, drawn
                 * into a region of the same target) finish here -- snapshot
                 * before the full-screen scene overwrites them. */
                if (!dr->is_clear) {
                    if (dr->vp_w && dr->vp_w < s_d3d.width) {
                        seen_small_vp = 1;
                        subvp_note(dr->vp_x, dr->vp_y, dr->vp_w, dr->vp_h);
                    }
                    else if (seen_small_vp && !captured &&
                             dr->vp_w == s_d3d.width && dr->rt_off == 0) {
                        captured = 1;
                        screen_copy_capture(fi);
                        subvp_capture(fi);
                        s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                            s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                        cur_zs = -2;   /* the shared DSV is bound now: rebind the zeta next */
                    }
                }
                /* Render-to-texture: retarget when this op's surfaces differ.
                 * Depth is a single shared buffer, so clear it per switch. */
                int want  = dr->rt_off  ? (dr->rt_slot >= 0 ? dr->rt_slot : off_rt_latest(dr->rt_off))
                          : ((disp_as_rt() && dr->disp_off && dr->rt_slot >= 0) ? dr->rt_slot : -1);
                if (!dr->rt_off && !dr->is_clear && want >= 0) { last_disp_slot = want; if (want < 64) disp_drawn |= 1ull << want; }
                /* Remember where geometry actually went: off_rt_find() matches
                 * on offset alone and slots share offsets at different sizes,
                 * so the composite cannot just look the source up by address. */
                if (want >= 0) { s_last_drawn_slot = want; s_last_drawn_off = dr->rt_off; }
                int wantm[3];
                for (int _m = 0; _m < 3; _m++)
                    wantm[_m] = dr->rt_mrt[_m] ? (dr->mrt_slot[_m] >= 0 ? dr->mrt_slot[_m] : off_rt_latest(dr->rt_mrt[_m])) : -1;
                if (want != cur_rt || wantm[0] != cur_m[0] ||
                    wantm[1] != cur_m[1] || wantm[2] != cur_m[2] || want_zs != cur_zs) {
                    if (s_cp_active && cur_rt != -99) {   /* v33: snapshot what the last pass produced */
                        int _sl[4] = {cur_rt, cur_m[0], cur_m[1], cur_m[2]};
                        cp_snap(_sl, 4, d ? d - 1 : 0); cp_snap_zs(cur_zs, d ? d - 1 : 0); s_cp_lastop = d; }
                    D3D12_CPU_DESCRIPTOR_HANDLE dsv_use = dsv_handle;
                    if (want_zs >= 0) {
                        zs_transition(want_zs, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        zs_init_if_needed(want_zs);
                        dsv_use = zs_dsv(want_zs);
                    }
                    D3D12_CPU_DESCRIPTOR_HANDLE rh[4];
                    UINT nrt = 1;
                    rh[0] = rtv_handle;
                    D3D12_VIEWPORT vp = {0, 0, (float)BBW, (float)BBH, 0.0f, 1.0f};
                    if (want >= 0) {
                        off_rt_transition(want, D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[0] = off_rt_rtv(want);
                        vp.Width  = (float)s_d3d.off_rt[want].w;
                        vp.Height = (float)s_d3d.off_rt[want].h;
                    }
                    for (int _m = 0; _m < 3 && wantm[_m] >= 0; _m++) {
                        off_rt_transition(wantm[_m], D3D12_RESOURCE_STATE_RENDER_TARGET);
                        rh[nrt++] = off_rt_rtv(wantm[_m]);
                    }
                    D3D12_RECT sc = {0, 0, (LONG)vp.Width, (LONG)vp.Height};
                    vp_last_w = vp.Width; vp_last_h = vp.Height;
                    gq_mark(((u64)((u32)vp.Width & 0xFFFFF) << 44) | ((u64)((u32)vp.Height & 0xFFFFF) << 24) | ((u64)nrt << 8)
                            | (want < 0 ? 0x10u : 0u) | (want_zs >= 0 ? 1u : 0u));   /* v199 GPUPROF */
                    s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, nrt, rh, FALSE, &dsv_use);
                    cmd_rect(&vp, &sc);
                    /* Old behaviour (shared buffer): wipe depth on every target
                     * switch. With per-zeta surfaces depth persists like on RSX. */
                    if (want_zs < 0)
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, dsv_handle,
                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
                    cur_zs = want_zs;
                    /* RT_CLEARDBG=2: magenta-clear the surface the DRAWS bind,
                     * at the moment they bind it. The guest's own clears may go
                     * to a different surface entirely (they do here: clears to
                     * 0xAB0000, draws to 0x1BE0000), so clearing on the clear
                     * path tests the wrong target. Anything not magenta
                     * afterwards was rasterised. */
                    { static int cd2 = -1;
                      if (cd2 < 0) { const char* e = getenv("RT_CLEARDBG"); cd2 = e ? atoi(e) : 0; }
                      if (cd2 >= 2 && want >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(
                              s_d3d.cmd_list, off_rt_rtv(want), mag, 0, NULL);
                      } }
                    /* VP_TESTTRI=1: our own triangle, through the game's exact
                     * pipeline -- same vpso, same root signature, same vertex
                     * buffer, same RTV just bound. Written at vertex 1000 so it
                     * cannot collide with the guest's data. Pair with
                     * VP_BYPASS=1 so the shader is a trivial passthrough.
                     *
                     * If this renders, the GPU does read vp_vb and the fault is
                     * in the guest draws' own state. If it does not, nothing
                     * this backend draws reaches a render target at all. */
                    { static int tt = -1;
                      if (tt < 0) { const char* e = getenv("VP_TESTTRI"); tt = e ? atoi(e) : 0; }
                      if (tt && want >= 0 && s_d3d.vp_vb_mapped) {
                          float* vtx = (float*)((char*)s_vpc_map[s_rf_parity][cur_vchunk]
                              + (u64)1000 * 256u);
                          static const float tri[3][4] = {
                              {-2.0f, -2.0f, 0.0f, 1.0f},
                              { 2.0f, -2.0f, 0.0f, 1.0f},
                              { 0.0f,  2.0f, 0.0f, 1.0f} };
                          for (int _v = 0; _v < 3; _v++) {
                              float* dstv = vtx + (size_t)_v * (256u / 4u);
                              dstv[0] = tri[_v][0]; dstv[1] = tri[_v][1];
                              dstv[2] = tri[_v][2]; dstv[3] = tri[_v][3];
                          }
                          s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, 3, 1, 1000, 0);
                          { static int _n = 0;
                            if ((_n < 3 && _n++ < 3)) fprintf(stderr, "[TESTTRI] issued on rt=%d off=0x%08X%c",
                                                  want, s_d3d.off_rt[want].off, 10); }
                      } }
                    cur_rt = want;
                    for (int _m = 0; _m < 3; _m++) cur_m[_m] = wantm[_m];
                }
                if (dr->is_clear) {
                    /* Depth/stencil part of the guest clear (mask bits 0/1). */
                    if (want_zs >= 0 && (dr->clr_mask & 3u)) {
                        D3D12_CLEAR_FLAGS cf = (D3D12_CLEAR_FLAGS)0;
                        if (dr->clr_mask & 1u) cf |= D3D12_CLEAR_FLAG_DEPTH;
                        if (dr->clr_mask & 2u) cf |= D3D12_CLEAR_FLAG_STENCIL;
                        zs_transition(want_zs, D3D12_RESOURCE_STATE_DEPTH_WRITE);
                        s_d3d.cmd_list->lpVtbl->ClearDepthStencilView(s_d3d.cmd_list, zs_dsv(want_zs),
                            cf, dr->clr_depth, (UINT8)dr->clr_stencil, 0, NULL);
                        s_zs[want_zs].need_init = 0;
                        s_zs[want_zs].dirty = 1;
                        s_lsk_marked[want_zs] = 0;   /* v147: next coarse run re-marks the sky region from scratch */
                        lsk_prev = 0;
                    }
                    /* Colour part only if the mask asks for colour (bits 4-7). */
                    if (dr->is_clear == 2 || !(dr->clr_mask & 0xF0u)) continue;
                    D3D12_CPU_DESCRIPTOR_HANDLE rh =
                        (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                    /* RT_CLEARDBG=1: clear offscreen targets to magenta rather
                     * than the guest colour. "0.000% nonzero" cannot tell an
                     * untouched surface from one written black, and that
                     * ambiguity invalidates every read of an offscreen target.
                     * If the surface comes back magenta the clears land and the
                     * draws are writing black; if it stays black, nothing
                     * reaches it at all. */
                    { static int cd = -1;
                      if (cd < 0) { const char* e = getenv("RT_CLEARDBG"); cd = e ? atoi(e) : 0; }
                      if (cd && cur_rt >= 0) {
                          const float mag[4] = {1.0f, 0.0f, 1.0f, 1.0f};
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, mag, 0, NULL);
                      } else
                          s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rh, dr->cc, 0, NULL); }
                    for (int _m = 0; _m < 3; _m++)
                        if (cur_m[_m] >= 0) {
                            D3D12_CPU_DESCRIPTOR_HANDLE rhm = off_rt_rtv(cur_m[_m]);
                            s_d3d.cmd_list->lpVtbl->ClearRenderTargetView(s_d3d.cmd_list, rhm, dr->cc, 0, NULL);
                        }
                    continue;
                }
                /* Per-draw pipeline: prefer the guest's own compiled FP; fall
                 * back to the hardcoded atlas/colour PS pair. */
                ID3D12PipelineState* dpso =
                    (dr->pso_memo_frame == s_rf_frame_no && !s_bis_nomemo) ? dr->pso_memo :
                    dr->fp_addr ? (s_fp_uc_override = dr->fp_uc, vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key,
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt)
                                                           : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                (int)((u32)dr->fp_exp32 | (dr_texk(dr) << 8)), dr->cmask | (dr->mrtk << 4), dr->cull,
                                                dr_cube_mask(dr) | (dr->shkey << 16))) : NULL;
                s_fp_uc_override = NULL;
                if ((dr->cull >> 24) & 1u) dbt_set(dr->dbmin, dr->dbmax);   /* v35 */
                /* v34: shadow-map units read the zeta surface itself -> shader-resource state
                 * (the target rebind above returns it to DEPTH_WRITE when it is drawn again). */
                if (dr->shkey)
                    for (int _u = 0; _u < 4; _u++) {
                        int zi = dr->shz[_u];
                        if (!((dr->shkey >> _u) & 1u) || zi < 0 || zi == cur_zs || !s_zs[zi].res) continue;
                        zs_init_if_needed(zi);
                        zs_transition(zi, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    }
                cmd_pso(dpso ? dpso : vpso);
                /* Per-draw viewport: the guest rect when sane, else the
                 * full target. Scissor tracks the same rect. */
                {
                    float tw = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].w : (float)BBW;
                    float th = (cur_rt >= 0) ? (float)s_d3d.off_rt[cur_rt].h : (float)BBH;
                    int used_guest_vp = 0;   /* v154 */
                    D3D12_VIEWPORT dvp = {0, 0, tw, th, dr->zmin, dr->zmax};
                    if (!(dr->zmax > 0.0f) && !(dr->zmin > 0.0f)) dvp.MaxDepth = 1.0f;   /* records without a range */
                    if (dr->vp_w >= 2 && dr->vp_h >= 2 &&
                        (float)(dr->vp_x + dr->vp_w) <= tw + 0.5f &&
                        (float)(dr->vp_y + dr->vp_h) <= th + 0.5f) {
                        dvp.TopLeftX = (float)dr->vp_x;
                        dvp.TopLeftY = (float)dr->vp_y;
                        dvp.Width    = (float)dr->vp_w;
                        dvp.Height   = (float)dr->vp_h;
                        used_guest_vp = 1;
                    }
                    /* Scale the particle viewport along with BOTH attachments.
                     * Texture coordinates still use the guest's logical size.
                     * v154: also RES_SCALE targets (display draws carry no rt_w: guest = display size);
                     * only a guest viewport is scaled -- the full-target fallback already is. */
                    { u32 gw0 = dr->rt_off ? dr->rt_w : s_d3d.width, gh0 = dr->rt_off ? dr->rt_h : s_d3d.height;
                    if (used_guest_vp && gw0 && gh0) {
                        u32 pw = gw0, ph = gh0;
                        particle_size(&pw, &ph);
                        if ((pw != gw0 || ph != gh0) && pw == (u32)tw && ph == (u32)th) {
                            float sx = (float)pw / gw0, sy = (float)ph / gh0;
                            dvp.TopLeftX *= sx; dvp.TopLeftY *= sy;
                            dvp.Width *= sx; dvp.Height *= sy;
                        }
                    } }
                    D3D12_RECT dsc = {(LONG)dvp.TopLeftX, (LONG)dvp.TopLeftY,
                                      (LONG)(dvp.TopLeftX + dvp.Width),
                                      (LONG)(dvp.TopLeftY + dvp.Height)};
                    { /* v74b DBG_VS_LOG=<vs_idx>: how each such draw is replayed (target, depth, viewport, PSO). */
                      static int lv = -2, ln = 0, lvn = 0; static int lvl[8];
                      if (lv == -2) { const char* e = getenv("DBG_VS_LOG"); lv = e ? 0 : -1;
                          while (e && *e && lvn < 8) { lvl[lvn++] = atoi(e); e = strchr(e, ','); if (e) e++; } }
                      int _hit = 0; for (int q = 0; q < lvn; q++) if (dr->vs_idx == lvl[q]) _hit = 1;
                      if (_hit && dr->samp_tbl >= 0 && dr->samp_tbl < s_samp_n && ln < 120)
                          fprintf(stderr, "[VSLOG]   samp keys u0..5: %05X %05X %05X %05X %05X %05X%c",
                              s_samp_keys[dr->samp_tbl][0] & 0xFFFFFu, s_samp_keys[dr->samp_tbl][1] & 0xFFFFFu, s_samp_keys[dr->samp_tbl][2] & 0xFFFFFu,
                              s_samp_keys[dr->samp_tbl][3] & 0xFFFFFu, s_samp_keys[dr->samp_tbl][4] & 0xFFFFFu, s_samp_keys[dr->samp_tbl][5] & 0xFFFFFu, 10);
                      if (_hit && ln < 120) { ln++;
                          fprintf(stderr, "[VSLOG] d=%u fp=0x%X fph=%08X rt_off=0x%X disp=0x%X cur_rt=%d(off 0x%X %ux%u) zs=%d(off 0x%X %ux%u) vp=(%g,%g %gx%g z %g..%g) pso=%p cull=%08X bk=%08X cmask=%X n=%u vb_off=%u stride=%u shkey=%X%c",
                              d, dr->fp_addr, dr->fp_uh, dr->rt_off, dr->disp_off, cur_rt,
                              cur_rt >= 0 ? s_d3d.off_rt[cur_rt].off : 0, cur_rt >= 0 ? s_d3d.off_rt[cur_rt].w : 0, cur_rt >= 0 ? s_d3d.off_rt[cur_rt].h : 0,
                              cur_zs, cur_zs >= 0 ? s_zs[cur_zs].off : 0, cur_zs >= 0 ? s_zs[cur_zs].w : 0, cur_zs >= 0 ? s_zs[cur_zs].h : 0,
                              dvp.TopLeftX, dvp.TopLeftY, dvp.Width, dvp.Height, dvp.MinDepth, dvp.MaxDepth,
                              (void*)dpso, dr->cull, dr->blend_key, dr->cmask, dr->vertex_count, dr->vb_byte_offset, dr->vb_stride, dr->shkey, 10); } }
                    dvp_x = dvp.TopLeftX; dvp_y = dvp.TopLeftY;
                    dvp_w = dvp.Width;    dvp_h = dvp.Height;
                    cmd_rect(&dvp, &dsc);
                }
                /* Per-draw textures: bind this draw's t0-t3 SRV window.
                 * Any sampled offscreen RT transitions to PSR first (never
                 * one of the currently-bound colour targets). */
                for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++) {
                    int rt = dr->tex_rt[_u];
                    if (rt < 0) continue;
                    if (rt == cur_rt || rt == cur_m[0] || rt == cur_m[1] || rt == cur_m[2]) {
                        int duplicate = 0;
                        for (int q = 0; q < _u; q++) if (dr->tex_rt[q] == rt) duplicate = 1;
                        if (!duplicate) {
                            /* v156 FEEDBACK_RUN=<N> (default 0 = copy before every such draw, exact): a run of
                             * consecutive draws with the same fragment program sampling the target they write
                             * (inFamous' G-buffer decals, fph 1419CAD1/660FF6FE/5832CC6A: ~170 per city frame,
                             * each sampling albedo AND normals = ~340 full-target copies, 14.7 MB each at
                             * 2560x1440) refreshes the snapshot only every N draws. Approximation: decals of
                             * one run that overlap may not see each other within N draws. */
                            /* v157 FEEDBACK_RUN_ANYFP=1 (default when FEEDBACK_RUN > 1): a run also continues across
                             * the three decal FPs (any consecutive feedback draws into the same target). Any other
                             * draw in between breaks the run (d not consecutive) -> fresh snapshot. */
                            static int fbn = -1, fb_anyfp = 1;
                            static u32 fb_last_d[64], fb_fp[64], fb_frame[64], fb_runlen[64];
                            if (fbn < 0) { const char* e = getenv("FEEDBACK_RUN"); fbn = e ? atoi(e) : 0; if (fbn < 0) fbn = 0;
                                const char* a = getenv("FEEDBACK_RUN_ANYFP"); fb_anyfp = (a && *a == '0') ? 0 : 1;
                                fprintf(stderr, "[RT80] v157 feedback snapshot refresh every %d draw(s) of a %s run (FEEDBACK_RUN; 0/1 = every draw, exact; FEEDBACK_RUN_ANYFP=%d)%c",
                                        fbn > 1 ? fbn : 1, fb_anyfp ? "consecutive-feedback-draw" : "same-FP", fb_anyfp, 10); }
                            int skip = 0;
                            if (fbn > 1 && rt < 64) {
                                if (fb_frame[rt] == s_rf_frame_no && fb_last_d[rt] + 1u == d &&
                                    (fb_anyfp || (fb_fp[rt] == dr->fp_uh && dr->fp_uh)) &&
                                    fb_runlen[rt] + 1u < (u32)fbn) { skip = 1; fb_runlen[rt]++; }
                                else fb_runlen[rt] = 0;
                                fb_last_d[rt] = d; fb_fp[rt] = dr->fp_uh; fb_frame[rt] = s_rf_frame_no;
                            }
                            if (!skip) off_rt_feedback_copy(rt);
                        }
                    } else off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                }
                D3D12_GPU_DESCRIPTOR_HANDLE gh = gh_base;
                { u32 _ss = (dr->srv_src_frame == s_rf_frame_no && dr->srv_src <= d) ? dr->srv_src : d;   /* v46 SRV_ALIAS */
                  gh.ptr += (u64)(s_dsrv_base + _ss * RSX_D3D_TEX_UNITS) * s_d3d.srv_inc; }   /* v225A RF_EARLY halves */
                if (gh.ptr != cur_srv) {
                    s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 1, gh);
                    cur_srv = gh.ptr;
                } else bind_srv_skip++;
                if (s_d3d.samp_heap) {
                    int st_ = dr->samp_tbl;
                    if (st_ < 0 || st_ >= s_samp_n) st_ = 0;
                    if (st_ != cur_samp) {
                        D3D12_GPU_DESCRIPTOR_HANDLE sh_ = samp_base;
                        sh_.ptr += (u64)s_samp_off[st_] * s_samp_inc;   /* v156 packed tables */
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootDescriptorTable(s_d3d.cmd_list, 3, sh_);
                        cur_samp = st_;
                    }
                }
                /* Per-draw constants: this draw's vp_cb + FP texscale slots. */
                {
                    u64 va = vp_cb_base
                           + ((u64)s_rf_parity * s_max_draws + dr->vcb_slot) * VP_CB_STRIDE;   /* v219g */
                    if (va != cur_vcb) {
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 0, va);
                        cur_vcb = va;
                    } else bind_vcb_skip++;
                }
                if (s_d3d.vp_fpcb) {
                    u64 va = fp_cb_base
                           + ((u64)s_rf_parity * s_max_draws + dr->fcb_slot) * VP_FPCB_STRIDE;   /* v219j */
                    if (va != cur_fcb) {
                        s_d3d.cmd_list->lpVtbl->SetGraphicsRootConstantBufferView(s_d3d.cmd_list, 2, va);
                        cur_fcb = va;
                    } else bind_fcb_skip++;
                }
                /* DRAWARGS=<N>: the arguments actually handed to the GPU, plus
                 * the target they are aimed at. A zero vertex count, or a start
                 * vertex past the buffer, rasterises nothing while every other
                 * check upstream still says the draw is fine. */
                { static int dcap = -1, dn = 0;
                  if (dcap < 0) { const char* e = getenv("DRAWARGS"); dcap = e ? atoi(e) : 0; }
                  if (dcap && dn < dcap) { dn++;
                      D3D12_CPU_DESCRIPTOR_HANDLE _rh =
                          (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      fprintf(stderr, "[DRAWARGS] verts=%u start=%u rt=%d rec=0x%08X bound=0x%08X"
                              " tex0=0x%08X texrt=%d dvp=%.0f,%.0f %.0fx%.0f%c",
                              dr->vertex_count, dr->vb_byte_offset / 256, cur_rt,
                              dr->rt_off,
                              (cur_rt >= 0) ? s_d3d.off_rt[cur_rt].off : 0u,
                              dr->tex[0].raw, dr->tex_rt[0],
                              (double)dvp_x, (double)dvp_y,
                              (double)dvp_w, (double)dvp_h, 10); } }
                /* RT_REBIND=1: re-issue OMSetRenderTargets immediately before
                 * every draw instead of only when the target changes. A clear
                 * proves the RTV descriptor is good but not that the binding is
                 * still live at draw time -- ClearRenderTargetView writes
                 * through the descriptor and never consults the OM. If anything
                 * between the retarget and here drops the binding, this restores
                 * it. */
                { static int rb = -1;
                  if (rb < 0) { const char* e = getenv("RT_REBIND"); rb = e ? atoi(e) : 0; }
                  if (rb) {
                      D3D12_CPU_DESCRIPTOR_HANDLE brh[4];
                      UINT bn = 1;
                      brh[0] = (cur_rt >= 0) ? off_rt_rtv(cur_rt) : rtv_handle;
                      for (int _m = 0; _m < 3; _m++)
                          if (cur_m[_m] >= 0) brh[bn++] = off_rt_rtv(cur_m[_m]);
                      s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(
                          s_d3d.cmd_list, bn, brh, FALSE, &dsv_handle);
                  } }
                /* Each draw binds its own window: chunk base + its byte offset,
                 * with its own (packed) stride, and draws from vertex 0. */
                if (dr->vb_chunk == VC_CHUNK_ID) {   /* v60 VCACHE record: absolute indices into the cache */
                  u64 vcsz = 0; ID3D12Resource* vcr = vc_resource(&vcsz);
                  if (vcr) {
                      vbv.BufferLocation = vcr->lpVtbl->GetGPUVirtualAddress(vcr);
                      vbv.SizeInBytes    = (UINT)vcsz;
                      vbv.StrideInBytes  = (dr->vb_stride >= 16u && dr->vb_stride <= 256u) ? dr->vb_stride : 256u;
                      if (memcmp(&vbv, &cur_vbv, sizeof vbv)) {
                          s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
                          cur_vbv = vbv;
                      } else bind_vb_skip++;
                      cur_vchunk = VC_CHUNK_ID;
                  }
                } else
                { u32 vck = dr->vb_chunk < VP_MAX_CHUNKS ? dr->vb_chunk : 0;
                  ID3D12Resource* vc = s_vpc_res[s_rf_parity][vck];
                  u32 vstr = (dr->vb_stride >= 16u && dr->vb_stride <= 256u) ? dr->vb_stride : 256u;
                  if (vc && dr->vb_byte_offset < s_vp_max_verts * 256u) {
                      vbv.BufferLocation = vc->lpVtbl->GetGPUVirtualAddress(vc) + dr->vb_byte_offset;
                      vbv.SizeInBytes    = s_vp_max_verts * 256u - dr->vb_byte_offset;
                      vbv.StrideInBytes  = vstr;
                      if (memcmp(&vbv, &cur_vbv, sizeof vbv)) {
                          s_d3d.cmd_list->lpVtbl->IASetVertexBuffers(s_d3d.cmd_list, 0, 1, &vbv);
                          cur_vbv = vbv;
                      } else bind_vb_skip++;
                      cur_vchunk = vck;
                  } }
                if (dr->sref != cur_sref) { s_d3d.cmd_list->lpVtbl->OMSetStencilRef(s_d3d.cmd_list, dr->sref); cur_sref = dr->sref; }
                if (dr->ib_stamp == s_rf_frame_no && dr->index_count && s_ib_res[s_rf_parity]) {
                    D3D12_INDEX_BUFFER_VIEW ibv;
                    ibv.BufferLocation = s_ib_res[s_rf_parity]->lpVtbl->GetGPUVirtualAddress(s_ib_res[s_rf_parity]) + dr->ib_off;
                    ibv.SizeInBytes    = dr->index_count * 4u;
                    ibv.Format         = DXGI_FORMAT_R32_UINT;
                    s_d3d.cmd_list->lpVtbl->IASetIndexBuffer(s_d3d.cmd_list, &ibv);
                    s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list, dr->index_count, 1, 0, 0, 0);
                } else
                s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list,
                    dr->vertex_count, 1, 0, 0);
                /* v166 signed blend: pass N (negative part) of a FUNC_*_SIGNED draw -- see rsx_blend_key. */
                if (dpso && dr->fp_addr && (dr->blend_key & (3u << 27)) && dr_num_rts(dr) == 1) {
                    ID3D12PipelineState* pn = (s_fp_uc_override = dr->fp_uc, vp_get_fp_pso(dr->vs_idx, dr->fp_addr, dr->blend_key | (1u << 30),
                                                dr_num_rts(dr),
                                                dr->rt_off ? rsx_surface_dxgi(dr->rt_fmt) : DXGI_FORMAT_R8G8B8A8_UNORM,
                                                (int)((u32)dr->fp_exp32 | (dr_texk(dr) << 8)), dr->cmask | (dr->mrtk << 4), dr->cull,
                                                dr_cube_mask(dr) | (dr->shkey << 16)));
                    s_fp_uc_override = NULL;
                    if (pn) {
                        cmd_pso(pn);
                        if (dr->ib_stamp == s_rf_frame_no && dr->index_count && s_ib_res[s_rf_parity])
                            s_d3d.cmd_list->lpVtbl->DrawIndexedInstanced(s_d3d.cmd_list, dr->index_count, 1, 0, 0, 0);
                        else
                            s_d3d.cmd_list->lpVtbl->DrawInstanced(s_d3d.cmd_list, dr->vertex_count, 1, 0, 0);
                    }
                }
#ifdef PS3RECOMP_DLSS
                if (d == dlss_last_scene && cur_zs >= 0 && s_zs[cur_zs].res) {   /* scene complete: its depth for DLAA */
                    u32 zst = (u32)s_zs[cur_zs].st;
                    rsx_dlss_capture_depth(s_d3d.cmd_list, s_zs[cur_zs].res, &zst);
                    s_zs[cur_zs].st = (D3D12_RESOURCE_STATES)zst;
                }
                if (d == dlss_composite) {   /* post-processed scene on the display buffer, HUD not drawn yet */
                    if (cur_rt >= 0 && cur_rt < MAX_OFF_RTS && s_d3d.off_rt[cur_rt].res) {
                        u32 rst = (u32)s_d3d.off_rt[cur_rt].st;
                        rsx_dlss_capture_color(s_d3d.cmd_list, s_d3d.off_rt[cur_rt].res, &rst);
                        s_d3d.off_rt[cur_rt].st = (D3D12_RESOURCE_STATES)rst;
                    } else if (cur_rt == -1) {
                        u32 rst = (u32)D3D12_RESOURCE_STATE_RENDER_TARGET;
                        rsx_dlss_capture_color(s_d3d.cmd_list, s_d3d.render_targets[fi], &rst);
                    }
                }
#endif
                /* Conversion stores depth only, not stencil. Read-only particles
                 * must not force a full-screen conversion before the next particle. */
                if (cur_zs >= 0 && (!(dr->cull & 0x8000u) || (dr->cull & 0x300u) == 0x300u))
                    s_zs[cur_zs].dirty = 1;
                /* v156 ZCONV_STENCIL: the colour copy now carries stencil -> stencil writes dirty it too. */
                if (cur_zs >= 0 && zconv_stencil_on() && (dr->cull & 0x8000u)) {
                    u32 si = (dr->cull >> 16) & 0xFFu;
                    if (si && (int)si < s_stk_n && s_stk[si].wmask &&
                        (s_stk[si].zpass[0] != D3D12_STENCIL_OP_KEEP || s_stk[si].zfail[0] != D3D12_STENCIL_OP_KEEP ||
                         s_stk[si].fail[0] != D3D12_STENCIL_OP_KEEP || s_stk[si].zpass[1] != D3D12_STENCIL_OP_KEEP ||
                         s_stk[si].zfail[1] != D3D12_STENCIL_OP_KEEP || s_stk[si].fail[1] != D3D12_STENCIL_OP_KEEP))
                        s_zs[cur_zs].dirty = 1;
                }
                /* VP_SUBMIT=<N>: prove the VP pass actually reaches the GPU.
                 * "records exist" and "draws were submitted" are different
                 * claims, and every blank-output investigation conflates them. */
                { static int cap = -1, n = 0;
                  if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
                  if (cap && n < cap) { n++;
                    fprintf(stderr, "[VPSUBMIT] draw[%u] verts=%u pso=%s vp=%ux%u tex0=0x%X(set=%d) tex1=0x%X slot=%d fp=0x%X%c",
                            d, dr->vertex_count, dpso ? "guest-fp" : "fallback",
                            dr->vp_w, dr->vp_h, dr->tex[0].raw, dr->tex[0].set,
                            dr->tex[1].raw, dr->tex_slot, dr->fp_addr, 10);
                    fprintf(stderr, "            vs_idx=%d cb_slot=%u vbofs=%u cull=%u blend=%d%c",
                            dr->vs_idx, dr->cb_slot, dr->vb_byte_offset / 256,
                            dr->cull, dr->blend, 10); } }
            }
            {   /* Low-rate evidence for A/B runs; no work unless BIND_STATS=1. */
                static int bs = -1; static u64 sr = 0, vc = 0, fc = 0, vb = 0; static u32 nf = 0;
                if (bs < 0) bs = getenv("BIND_STATS") ? 1 : 0;
                if (bs) {
                    sr += bind_srv_skip; vc += bind_vcb_skip; fc += bind_fcb_skip; vb += bind_vb_skip;
                    if (++nf == 300) {
                        fprintf(stderr, "[BIND_CACHE] 300 frames: skipped %llu SRV, %llu VP-CBV, %llu FP-CBV, %llu VB binds%c",
                                (unsigned long long)sr, (unsigned long long)vc, (unsigned long long)fc,
                                (unsigned long long)vb, 10);
                        sr = vc = fc = vb = 0; nf = 0;
                    }
                }
            }
            if (s_cp_active) {   /* v33: final targets of the frame */
                int _sl[4] = {cur_rt, cur_m[0], cur_m[1], cur_m[2]};
                if (cur_rt != -99) { cp_snap(_sl, 4, s_rf_count ? s_rf_count - 1 : 0); cp_snap_zs(cur_zs, s_rf_count ? s_rf_count - 1 : 0); }
                s_cp_active = 0; s_cp_pending = 1;
                fprintf(stderr, "[RSXCAP] %d pass snapshots recorded (%llu MB)%c", s_cp_n, (unsigned long long)(s_cp_used >> 20), 10);
            }
            if (perf_on()) s_perf_gpu += perf_now() - _rec0;   /* reuse: record time */
            /* DISPLAY_AS_RT: present the display buffer the guest last FLIPPED
             * (cellGcmSys g_gcm_last_flip_off). "The buffer of the frame's last
             * display draw" was wrong for inFamous: its UI pass ("Press Start")
             * goes to the other ping-pong buffer at the start of our recorded
             * frame and was never presented. DISPRT_FLIP=0 -> old rule. */
            /* v103A FRAME_SPLIT: a part executed without Present remembers its display buffer; the presenting
             * part of the same frame falls back to it when it drew no display buffer itself. */
            { static int s_carry_slot = -1; static u64 s_carry_drawn = 0;
              if (!s_rf_present) { if (last_disp_slot >= 0) { s_carry_slot = last_disp_slot; s_carry_drawn |= disp_drawn; } }
              else { if (last_disp_slot < 0) last_disp_slot = s_carry_slot;
                     disp_drawn |= s_carry_drawn; s_carry_slot = -1; s_carry_drawn = 0; } }
            { static int s_df = -1;
              if (s_df < 0) { const char* e = getenv("DISPRT_FLIP"); s_df = (e && *e == '0') ? 0 : 1; }
              u32 fo = s_rf_flip_off;   /* v59: g_gcm_last_flip_off as of the hand-off */
              if (s_df && fo) {
                  for (int _k = 0; _k < MAX_OFF_RTS; _k++)
                      if (s_d3d.off_rt[_k].res && s_d3d.off_rt[_k].off == fo &&
                          s_d3d.off_rt[_k].w == RBW && s_d3d.off_rt[_k].h == RBH) {
                          /* v62b: only if this frame actually drew into it -- a flipped buffer
                           * we never drew this frame showed a black screen entering the world */
                          if (_k < 64 && (disp_drawn >> _k) & 1ull) last_disp_slot = _k;
                          else { static int _w = 0; if ((_w < 8 && _w++ < 8))
                              fprintf(stderr, "[DISPRT] flipped buffer 0x%X not drawn this frame -> presenting the last display draw (slot %d)%c", fo, last_disp_slot, 10); }
                          break; }
              } }
            s_bp_last_slot = last_disp_slot; s_bp_last_drawn = disp_drawn;   /* v103A [BLACK]: the buffer presented */
            if (last_disp_slot >= 0 && s_d3d.off_rt[last_disp_slot].res &&
                s_d3d.off_rt[last_disp_slot].w == RBW && s_d3d.off_rt[last_disp_slot].h == RBH &&
                s_d3d.off_rt[last_disp_slot].dxgi == (u32)DXGI_FORMAT_R8G8B8A8_UNORM) {
#ifdef PS3RECOMP_DLSS
              if (rsx_dlss_upscaling()) {   /* DLSS upscaling: the display buffer (render size) goes to DLSS, which fills the backbuffer */
                  off_rt_transition(last_disp_slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
                  rsx_dlss_capture_finished(s_d3d.cmd_list, s_d3d.off_rt[last_disp_slot].res);
                  off_rt_transition(last_disp_slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                  cur_rt = 0x7FFF;   /* force the backbuffer rebind below */
              } else
#endif
              {
                off_rt_transition(last_disp_slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_RESOURCE_BARRIER bb = {0};
                bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bb.Transition.pResource = s_d3d.render_targets[fi];
                bb.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
                bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bb);
                D3D12_TEXTURE_COPY_LOCATION cd = {0}, cs = {0};
                cd.pResource = s_d3d.render_targets[fi]; cd.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cs.pResource = s_d3d.off_rt[last_disp_slot].res; cs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cd, 0, 0, 0, &cs, NULL);
                bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
                s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &bb);
                off_rt_transition(last_disp_slot, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cur_rt = 0x7FFF;   /* force the backbuffer rebind below */
                { static int _n = 0; if ((_n < 3 && _n++ < 3))
                    fprintf(stderr, "[DISPRT] presented display buffer 0x%X (rt slot %d)%c",
                            s_d3d.off_rt[last_disp_slot].off, last_disp_slot, 10); }
              }   /* DLSS upscaling: end of the copy branch */
            }
            /* Leave the backbuffer bound for the dump/present epilogue. */
            if (cur_rt >= 0) {
                s_d3d.cmd_list->lpVtbl->OMSetRenderTargets(s_d3d.cmd_list, 1, &rtv_handle, FALSE, &dsv_handle);
                D3D12_VIEWPORT vp = {0, 0, (float)BBW, (float)BBH, 0.0f, 1.0f};
                D3D12_RECT sc = {0, 0, (LONG)BBW, (LONG)BBH};
                cmd_rect(&vp, &sc);
            }
        }
    }

    if (perf_on()) s_perf_rf += perf_now() - _rf0;
    /* MEMPEEK=<hex addr>:<n>: n big-endian floats at a guest address each frame,
     * with a min/max so a buffer of constants is distinguishable from real data. */
    { const char* mp = getenv("MEMPEEK");
      if (mp) { extern uint8_t* vm_base; static int _n = 0;
        u32 addr = 0; int cnt = 8; sscanf(mp, "%x:%d", &addr, &cnt);
        if (vm_base && addr && (_n < 6 && _n++ < 6)) {
            float lo = 1e30f, hi = -1e30f; u32 nz = 0;
            for (int i = 0; i < cnt; i++) {
                const u8* q = vm_base + addr + i * 4;
                u32 bits = ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3];
                float f; memcpy(&f, &bits, 4);
                if (f == f && f < 1e30f && f > -1e30f) { if (f < lo) lo = f; if (f > hi) hi = f; }
                if (bits) nz++;
            }
            fprintf(stderr, "[MEMPEEK] 0x%08X n=%d nonzero=%u range[%g..%g]%c",
                    addr, cnt, nz, lo, hi, 10);
        } } }
    /* VRAMSCAN=1: which 64 KB blocks of guest memory change between frames.
     * "nothing writes the buffer we read" is only half an answer -- this says
     * where the producer IS writing, so the two can be matched up. */
    { static int vs = -1; if (vs < 0) vs = getenv("VRAMSCAN") ? 1 : 0;
      if (vs) { extern uint8_t* vm_base;
        static u32 prev[512]; static int have = 0; static int reported = 0;
        u32 base = 0xC6000000u; u32 nblk = 512;          /* 32 MB window */
        const char* bs = getenv("VRAMSCAN_BASE");
        if (bs) base = (u32)strtoul(bs, NULL, 0);
        if (vm_base && reported < 6) {
            u32 changed = 0; char list[512]; int ln = 0;
            for (u32 b = 0; b < nblk; b++) {
                const u8* p = vm_base + base + b * 0x10000u;
                u32 h = 2166136261u;
                for (u32 i = 0; i < 0x10000u; i += 61) h = (h ^ p[i]) * 16777619u;
                if (have && h != prev[b]) { changed++;
                    if (ln < 400) ln += snprintf(list + ln, sizeof(list) - ln,
                                                 " +0x%X", b * 0x10000u); }
                prev[b] = h;
            }
            if (have && changed) { reported++;
                fprintf(stderr, "[VRAMSCAN] base=0x%08X %u/512 blocks changed:%s%c",
                        base, changed, list, 10); }
            have = 1;
        } } }
    /* FRAME_BUDGET=1: geometry a frame asked for vs what the batch can hold.
     * Reported from render_frame so it works under RSX_ACCUM_FRAME too -- it
     * used to live in the clear-boundary present path, which accum bypasses, so
     * it silently produced nothing in exactly the configuration being used. */
    { static int fb = -1;
      if (fb < 0) { const char* e = getenv("FRAME_BUDGET"); fb = e ? atoi(e) : 0; }
      if (fb && s_req_draws) { static int n = 0; if ((n < 40 && n++ < 40))
        fprintf(stderr, "[BUDGET] frame: requested %llu verts / %llu draws, "
                        "buffer holds %u verts, %llu draws truncated%c",
                (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                s_vp_max_verts, (unsigned long long)s_drop_draws, 10); }
      /* Always (first 8): a frame that overflowed the VP vertex buffer loses
       * every draw past the overflow point -- silently, before this. */
      if (s_drop_draws) { static int w = 0; if ((w < 8 && w++ < 8))
          fprintf(stderr, "[VPVB] OVERFLOW: frame requested %llu verts in %llu draws, buffer holds %u; "
                          "%llu draws truncated (raise VP_VB_VERTS)%c",
                  (unsigned long long)s_req_verts, (unsigned long long)s_req_draws,
                  s_vp_max_verts, (unsigned long long)s_drop_draws, 10); }
      s_req_verts = 0; s_req_draws = 0; s_drop_draws = 0; }
    if (perf_on()) {
        static double s_t_prev = 0.0; static int s_n = 0;
        double now = perf_now();
        if (s_t_prev > 0.0) s_perf_frame += now - s_t_prev;
        s_t_prev = now;
        if (++s_n % 20 == 0) {
            double fr = s_perf_frame > 0 ? s_perf_frame : 1;
            fprintf(stderr, "[PERF] %.2f fps | tex %.2fs (%.0f%%, %d calls) | vtx %.2fs"
                            " (%.0f%%, %.0fk verts, %.0fk converted, %.0fk mt) | render_frame %.2fs (%.0f%%)"
                            " | prepass %.2fs (%.0f%%) [srv %.2fs %.0f%% | pso %.2fs %.0f%% %d calls %d MISS %dKB hashed] | guest %.2fs (%.0f%%)"
                            " | texup %d new (create %.2fs) %d reup %d cold-reuse %.1fMB%c",
                    20.0 / fr,
                    s_perf_tex, 100.0 * s_perf_tex / fr, s_perf_ntex,
                    s_perf_vtx, 100.0 * s_perf_vtx / fr,
                    (double)s_perf_nverts / 1000.0, (double)s_perf_vconv / 1000.0, (double)s_perf_vmt / 1000.0,
                    s_perf_rf, 100.0 * s_perf_rf / fr,
                    s_perf_pre, 100.0 * s_perf_pre / fr,
                    s_perf_srv, 100.0 * s_perf_srv / fr,
                    s_perf_pso, 100.0 * s_perf_pso / fr,
                    s_perf_pso_calls, s_perf_pso_miss, s_perf_pso_hashbytes / 1024,
                    fr - s_perf_rf, 100.0 * (fr - s_perf_rf) / fr,
                    s_perf_tex_new, s_perf_tex_create, s_perf_tex_reup, s_perf_tex_cold,
                    (double)s_perf_tex_upbytes / (1024.0 * 1024.0), 10);
            s_perf_tex_new = 0; s_perf_tex_reup = 0; s_perf_tex_cold = 0; s_perf_tex_create = 0.0; s_perf_tex_upbytes = 0;
            s_perf_frame = 0.0; s_perf_tex = 0.0; s_perf_vtx = 0.0; s_perf_rf = 0.0;
            s_perf_gpu = 0.0; s_perf_pre = 0.0; s_perf_srv = 0.0; s_perf_pso = 0.0;
            s_perf_pso_calls = 0; s_perf_pso_miss = 0; s_perf_pso_hashbytes = 0;
            s_perf_ntex = 0; s_perf_texbytes = 0; s_perf_nverts = 0; s_perf_vconv = 0; s_perf_vmt = 0;
        }
    }
    { static int dt = -1;
      if (dt < 0) { const char* e = getenv("RSX_DBG_TRACK"); dt = e ? atoi(e) : 0; }
      if (dt) { static int fr = 0; fr++;
        if (s_lock_n)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: %u duck draws, ndc=(%.3f,%.3f)%c",
                    fr, s_lock_n, (float)(s_lock_sx/s_lock_n), (float)(s_lock_sy/s_lock_n), 10);
        else if (fr % 20 == 0)
            fprintf(stderr, "[RSX_DBG_TRACK] frame %d: NO duck draws%c", fr, 10);
      } }
    if (s_lock_n) {                       /* publish this frame's centroid */
        s_lock_x = (float)(s_lock_sx / s_lock_n);
        s_lock_y = (float)(s_lock_sy / s_lock_n);
        s_lock_valid = 1;
        s_lock_sx = s_lock_sy = 0.0; s_lock_n = 0;
    }
    s_dbg_last_draws = s_rf_count;
    rft_mark(RFT_REC);
    gq_mark(GQ_POST);   /* v199 */
    slowframe_report(s_rf_count);
    /* v59: per-frame record state is reset by the render_frame() wrapper (FIFO thread) */

    /* Debug: RTT_SAVERT=<hex raw offset>[:frame] copies that offscreen RT
     * into a readback buffer this frame and writes rt_save.bmp (half-float
     * RTs are tonemapped |v| -> byte). */
    static ID3D12Resource* s_rtsave_buf = NULL;
    static u32 s_rtsave_state = 0;   /* 1 = copy queued this frame */
    static u32 s_rtsave_w, s_rtsave_h, s_rtsave_pitch, s_rtsave_dxgi;
    { const char* sv = getenv("RTT_SAVERT");
      static int _done = 0;
      static u32 _skip = 0xFFFFFFFFu;
      if (_skip == 0xFFFFFFFFu) { const char* sk = getenv("RTT_SAVERT_SKIP");
          _skip = sk ? (u32)strtoul(sk, NULL, 0) : 0; }   /* frame 60 is far too
          early for a title that takes a minute to reach its menu */
      /* RTT_SAVERT_AFTER=<seconds> triggers on wall time instead of a frame
       * count. This title runs its menu at a few frames a second, so a frame
       * number is not a usable way to say "once it has settled". */
      static u32 _after = 0xFFFFFFFFu;
      if (_after == 0xFFFFFFFFu) { const char* sa = getenv("RTT_SAVERT_AFTER");
          _after = sa ? (u32)strtoul(sa, NULL, 0) : 0; }
      static clock_t _t0 = 0;
      if (!_t0) _t0 = clock();
      const int _due = _after
          ? ((u32)((clock() - _t0) / CLOCKS_PER_SEC) >= _after)
          : (s_d3d.frame_count > 60 + _skip);
      { static int _p1 = 0;
        if (sv && _due && !_p1) { _p1 = 1;
            printf("[RTT_SAVERT] due: want=0x%lX found_rt=%d frame=%u\n",
                   strtoul(sv, NULL, 16), off_rt_find((u32)strtoul(sv, NULL, 16)),
                   (unsigned)s_d3d.frame_count); fflush(stdout); } }
      if (sv && !_done && _due) {
        int rt = off_rt_find((u32)strtoul(sv, NULL, 16));
        fprintf(stderr, "[RTT_SAVERT] rt=%d res=%p\n", rt, rt >= 0 ? (void*)s_d3d.off_rt[rt].res : NULL);
        if (rt >= 0 && s_d3d.off_rt[rt].res) {
            OffRT* r = &s_d3d.off_rt[rt];
            u32 bpp = (r->dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 :
                      (r->dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) ? 16 : 4;
            u32 pitch = (r->w * bpp + 255) & ~255u;
            fprintf(stderr, "[RTT_SAVERT] rt=%d w=%u h=%u bpp=%u pitch=%u dxgi=%d bytes=%llu\n",
                    rt, r->w, r->h, bpp, pitch, (int)r->dxgi,
                    (unsigned long long)((u64)pitch * r->h));
            if (!s_rtsave_buf) {
                D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_READBACK;
                D3D12_RESOURCE_DESC rd = {0};
                rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                rd.Width = (u64)pitch * r->h; rd.Height = 1; rd.DepthOrArraySize = 1;
                rd.MipLevels = 1; rd.SampleDesc.Count = 1;
                rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                s_d3d.device->lpVtbl->CreateCommittedResource(
                    s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_COPY_DEST, NULL,
                    &IID_ID3D12Resource, (void**)&s_rtsave_buf);
            }
            if (!s_rtsave_buf) fprintf(stderr, "[RTT_SAVERT] readback buffer alloc FAILED\n");
            if (s_rtsave_buf) {
                off_rt_transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION cdst = {0}, csrc = {0};
                cdst.pResource = s_rtsave_buf;
                cdst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                cdst.PlacedFootprint.Footprint.Format   = (DXGI_FORMAT)r->dxgi;
                cdst.PlacedFootprint.Footprint.Width    = r->w;
                cdst.PlacedFootprint.Footprint.Height   = r->h;
                cdst.PlacedFootprint.Footprint.Depth    = 1;
                cdst.PlacedFootprint.Footprint.RowPitch = pitch;
                csrc.pResource = r->res;
                csrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &cdst, 0, 0, 0, &csrc, NULL);
                off_rt_transition(rt, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                s_rtsave_state = 1;
                s_rtsave_w = r->w; s_rtsave_h = r->h;
                s_rtsave_pitch = pitch; s_rtsave_dxgi = r->dxgi;
                fprintf(stderr, "[RTT_SAVERT] copy queued: rt=%d %ux%u pitch=%u\n",
                        rt, r->w, r->h, pitch);
                _done = 1;
            }
        }
      } }

    /* CELLMARK_DUMP_MINDRAWS=<N>: only consider frames carrying at least N draw
     * records. Most render_frame calls come from the guest flip with a batch the
     * clear boundary already drained, so they present an empty backbuffer -- and
     * sampling those wastes every dump on a black image while the frames that do
     * hold the scene go uncaptured. */
    { static int mind = -1;
      if (mind < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                      mind = e ? atoi(e) : 0; }
      /* draw_count is already reset by here; s_dbg_last_draws holds the
       * count this frame actually rendered. */
      if (mind > 0 && s_dbg_last_draws < (u32)mind) goto skip_dump_consider; }
    if (s_d3d.dump_skip_left > 0 && s_d3d.dump_frames_left > 0) s_d3d.dump_skip_left--;
skip_dump_consider: ;
    guest_fb_present(fi);
    composite_present(fi);
#ifdef PS3RECOMP_DLSS
    if (s_rf_present && rsx_dlss_active()) {   /* DLAA over the finished game image, before the port's overlay */
        const int dz = zs_find_off(rsx_dlss_exec_zeta(), s_d3d.width, s_d3d.height);
        rsx_dlss_frame_end(s_d3d.cmd_list, s_d3d.render_targets[fi], BBW, BBH,
                           dz >= 0 ? s_zs[dz].res : NULL, dz >= 0 ? (u32)s_zs[dz].st : 0u);
    }
#endif
    /* In-game system dialogs (cellMsgDialog) drawn over the finished frame -- rsx_ui_overlay.c. */
    { extern void uiov_render(ID3D12Device*, ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
                              unsigned, unsigned, DXGI_FORMAT, HWND);
      D3D12_CPU_DESCRIPTOR_HANDLE uh;
      RSX_D3D12_CPU_HEAP_START(s_d3d.rtv_heap)(s_d3d.rtv_heap, &uh);
      uh.ptr += fi * s_d3d.rtv_descriptor_size;
      uiov_render(s_d3d.device, s_d3d.cmd_list, uh, BBW, BBH, DXGI_FORMAT_R8G8B8A8_UNORM, s_d3d.hwnd); }
    int dumping = (s_d3d.dump_frames_left > 0 && s_d3d.readback_buf
                   && s_d3d.dump_skip_left == 0);
    { static int mind2 = -1;
      if (mind2 < 0) { const char* e = getenv("CELLMARK_DUMP_MINDRAWS");
                       mind2 = e ? atoi(e) : 0; }
      if (mind2 > 0 && s_dbg_last_draws < (u32)mind2) dumping = 0;
      /* BISECT: skip the near-empty clear-boundary presents; wait for a real frame. */
      if (s_bis_active && s_dbg_last_draws < 500u) dumping = 0;
      if (dumping && s_bis_active) {
          fprintf(stderr, "[BISECT] captured a frame with %u draws%c", (unsigned)s_dbg_last_draws, 10);
          s_bis_active = 0; } }
    /* CELLMARK_DUMP_EVERY=N: after each dump, skip N-1 frames -- samples the whole
     * run instead of one contiguous window, so a short burst of real content
     * can't fall between the dumped frames. */
    if (dumping) {
        static int s_every = -1;
        if (s_every < 0) { const char* e = getenv("CELLMARK_DUMP_EVERY");
                           s_every = e ? atoi(e) : 0; }
        if (s_every > 1) s_d3d.dump_skip_left = s_every - 1;
    }
    if (dumping) {
        /* RT -> COPY_SOURCE, copy into the readback buffer, then -> PRESENT. */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);

        D3D12_TEXTURE_COPY_LOCATION dst = {0}, src = {0};
        dst.pResource = s_d3d.readback_buf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width    = BBW;
        dst.PlacedFootprint.Footprint.Height   = BBH;
        dst.PlacedFootprint.Footprint.Depth    = 1;
        dst.PlacedFootprint.Footprint.RowPitch = s_d3d.readback_pitch;
        src.pResource = s_d3d.render_targets[fi];
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &dst, 0, 0, 0, &src, NULL);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
    } else {
        /* End-of-frame fallback snapshot: used when the frame never contained a
         * reduced-viewport pass to capture mid-frame. */
        screen_copy_capture(fi);
        /* v103A BLACK_PROBE: copy the image about to be presented into a readback buffer (read after the
         * next frame's GPU wait). */
        if (s_rf_present && black_probe_want()) {
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION bd = {0}, bs = {0};
            bd.pResource = s_bp_buf; bd.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            bd.PlacedFootprint.Offset = 0;
            bd.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
            bd.PlacedFootprint.Footprint.Width    = s_bp_w;
            bd.PlacedFootprint.Footprint.Height   = s_bp_h;
            bd.PlacedFootprint.Footprint.Depth    = 1;
            bd.PlacedFootprint.Footprint.RowPitch = s_bp_pitch;
            bs.pResource = s_d3d.render_targets[fi]; bs.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; bs.SubresourceIndex = 0;
            s_d3d.cmd_list->lpVtbl->CopyTextureRegion(s_d3d.cmd_list, &bd, 0, 0, 0, &bs, NULL);
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
            black_probe_note_frame();
        } else {
        /* Transition render target to PRESENT state */
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &barrier);
        }
    }

    /* Close and execute */
    rft_mark(RFT_TAIL1);
    gq_frame_end();   /* v199 GPUPROF */
    s_d3d.cmd_list->lpVtbl->Close(s_d3d.cmd_list);
    ID3D12CommandList* cmd_lists[] = {(ID3D12CommandList*)s_d3d.cmd_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue, 1, cmd_lists);
    rft_mark(RFT_SUBMIT);

    /* D3D12_IQ=1: drain the debug layer's message queue after submitting the
     * frame. The legacy (is_vp=0) DrawInstanced path silently produced ZERO
     * pixels in flOw's render injection while ClearRenderTargetView worked, and
     * every input (verts, offsets, layout, MVP, PSO, root sig, depth, cull) was
     * verified correct -- so the only remaining explanation is a validation
     * error the debug layer is swallowing. Off unless the env var is set. */
    { static int _iq = -1;
      if (_iq < 0) { const char* e = getenv("D3D12_IQ"); _iq = e ? atoi(e) : 0; if (e && !_iq) _iq = 1; }
      if (_iq && s_d3d.device) {
        ID3D12InfoQueue* iq = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->QueryInterface(
                s_d3d.device, &IID_ID3D12InfoQueue, (void**)&iq)) && iq) {
            UINT64 n = iq->lpVtbl->GetNumStoredMessages(iq);
            static int _printed = 0;
            for (UINT64 mi = 0; mi < n && _printed < 400; mi++) {
                SIZE_T len = 0;
                iq->lpVtbl->GetMessage(iq, mi, NULL, &len);
                D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
                if (m && SUCCEEDED(iq->lpVtbl->GetMessage(iq, mi, m, &len)) &&
                    (_iq >= 2 || m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)) {   /* v71: errors only (D3D12_IQ=2 = all) */
                    fprintf(stderr, "[D3D12-IQ][sev=%d id=%d] %s\n",
                            (int)m->Severity, (int)m->ID, m->pDescription);
                    _printed++;
                }
                free(m);
            }
            if (n) iq->lpVtbl->ClearStoredMessages(iq);
            iq->lpVtbl->Release(iq);
            fflush(stderr);
        }
      } }

    if (s_sc_dump_pending) {
        s_sc_dump_pending = 0;
        wait_for_gpu();
        s_dump_name = "screencopy";
        dump_backbuffer_bmp();
        s_dump_name = NULL;
    }
    if (dumping) {
        wait_for_gpu();            /* ensure the copy finished before mapping */
        dump_backbuffer_bmp();
        s_d3d.dump_frames_left--;
    }

    if (s_rtsave_state) {
        fprintf(stderr, "[RTT_SAVERT] writing rt_save.bmp %ux%u\n", s_rtsave_w, s_rtsave_h);
        if (!dumping) { double _g = perf_on() ? perf_now() : 0.0;
                        wait_for_gpu();
                        if (perf_on()) s_perf_gpu += perf_now() - _g; }
        void* mp = NULL; D3D12_RANGE rr = {0, (SIZE_T)s_rtsave_pitch * s_rtsave_h};
        if (SUCCEEDED(s_rtsave_buf->lpVtbl->Map(s_rtsave_buf, 0, &rr, &mp)) && mp) {
            FILE* f = fopen("rt_save.bmp", "wb");
            if (f) {
                u32 w = s_rtsave_w, h = s_rtsave_h;
                u32 rowb = (w * 3 + 3) & ~3u;
                u32 datasz = rowb * h;
                u8 hdr[54] = {0};
                hdr[0]='B'; hdr[1]='M';
                *(u32*)(hdr+2) = 54 + datasz; *(u32*)(hdr+10) = 54;
                *(u32*)(hdr+14) = 40; *(int*)(hdr+18) = (int)w; *(int*)(hdr+22) = (int)h;
                *(u16*)(hdr+26) = 1; *(u16*)(hdr+28) = 24; *(u32*)(hdr+34) = datasz;
                fwrite(hdr, 1, 54, f);
                u8* line = (u8*)malloc(rowb);
                for (int y = (int)h - 1; y >= 0; y--) {
                    const u8* srow = (const u8*)mp + (u64)y * s_rtsave_pitch;
                    memset(line, 0, rowb);
                    for (u32 x = 0; x < w; x++) {
                        float rv, gv, bv;
                        if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                            const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                            /* crude half->float: sign|exp|mant */
                            float v[3];
                            for (int c2 = 0; c2 < 3; c2++) {
                                u16 hv = hp16[c2];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                float fv;
                                if (exp == 0) fv = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&fv, &fb, 4); }
                                v[c2] = fv;
                            }
                            rv = v[0]; gv = v[1]; bv = v[2];
                        } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                            const float* fp32 = (const float*)(srow + (u64)x * 16);
                            rv = fp32[0]; gv = fp32[1]; bv = fp32[2];
                        } else {
                            const u8* p8 = srow + (u64)x * 4;
                            rv = p8[0] / 255.0f; gv = p8[1] / 255.0f; bv = p8[2] / 255.0f;
                        }
                        /* RTT_SAVEA=1: show the alpha lane in RED (gates
                         * like wave's mask.w live there). */
                        if (getenv("RTT_SAVEA")) {
                            float av;
                            if (s_rtsave_dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                const u16* hp16 = (const u16*)(srow + (u64)x * 8);
                                u16 hv = hp16[3];
                                u32 sign = (hv >> 15) & 1, exp = (hv >> 10) & 0x1F, man = hv & 0x3FF;
                                if (exp == 0) av = (float)man / 16777216.0f;
                                else { u32 fb = (sign << 31) | ((exp - 15 + 127) << 23) | (man << 13);
                                       memcpy(&av, &fb, 4); }
                            } else if (s_rtsave_dxgi == DXGI_FORMAT_R32G32B32A32_FLOAT) {
                                av = ((const float*)(srow + (u64)x * 16))[3];
                            } else {
                                av = (srow + (u64)x * 4)[3] / 255.0f;
                            }
                            rv = av;
                        }
                        /* |v| tonemap so signed heights are visible */
                        float ar = rv < 0 ? -rv : rv, ag = gv < 0 ? -gv : gv, ab = bv < 0 ? -bv : bv;
                        if (ar > 1) ar = 1; if (ag > 1) ag = 1; if (ab > 1) ab = 1;
                        line[x*3+0] = (u8)(ab * 255.0f);
                        line[x*3+1] = (u8)(ag * 255.0f);
                        line[x*3+2] = (u8)(ar * 255.0f);
                    }
                    fwrite(line, 1, rowb, f);
                }
                free(line);
                fclose(f);
                printf("[D3D12] wrote rt_save.bmp (%ux%u dxgi=%u)\n", w, h, s_rtsave_dxgi);
            }
            s_rtsave_buf->lpVtbl->Unmap(s_rtsave_buf, 0, NULL);
        }
        s_rtsave_state = 0;
    }

    /* rsxcap: snapshot whole frames (backbuffer + every offscreen RT + a draw
     * manifest) at RSX_CAP=start[:count[:stride]] frame_counts. Runs before the
     * present transition would matter -- the backbuffer is in PRESENT here and
     * is restored to PRESENT after each readback. */
    /* F9 in the game window: capture the next 3 frames with >= 500 draws to
     * rsxcap/frame_NN (no special launcher needed). 2026-10-04. */
    { static int f9_left = 0, f9_idx = 100, f9_wait = 0;
      { static int f6_was = 0;   /* v152: F6 = coarse-world restriction off/on (test for late-appearing cars) */
        const int f6 = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F6) & 0x8000);
        if (f6 && !f6_was) { g_lsk_f6_off = !g_lsk_f6_off;
            fprintf(stderr, "[LODSKY] v152 F6 -> coarse-world restriction %s%c", g_lsk_f6_off ? "OFF" : "ON", 10); }
        f6_was = f6; }
      { static int f4_was = 0;   /* v163c: F4 = job-205 main-view occlusion tree normal / neutral(+) / neutral(-) */
        extern volatile int g_occ_live;
        const int f4 = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F4) & 0x8000);
        if (f4 && !f4_was) { int m = g_occ_live < 0 ? 0 : g_occ_live; g_occ_live = (m + 1) % 3;
            fprintf(stderr, "[occ] F4 -> mode %d (%s)%c", g_occ_live,
                    g_occ_live == 0 ? "normal occlusion" : g_occ_live == 1 ? "occluder planes neutral d=+1e30" : "occluder planes neutral d=-1e30", 10); }
        f4_was = f4; }
      { /* v175 CULL_VP_SCALE (default 1 when RES_SCALE > 1; 0 off; F8 toggles live): inFamous' SPU geometry jobs cull
         * every triangle that covers no PIXEL CENTRE of the 1280x720 screen (EDGE-style small-primitive cull), using
         * the viewport block at EA 0x008E40F0 {.., w, h, mat4, offX, scaleX, scaleY, offY, zmin, zmax}. At RES_SCALE=2
         * those triangles DO cover pixel centres of our 2560x1440 targets -> pin-holes in dense meshes (Cole's
         * eyes/nostrils/mouth/ears; geom dump 18:54: 1440p raster of the dumped index lists has holes the 720p raster
         * of the same lists has not). Telling the cull our real resolution restores them. Logs [CULLVP]. */
        static int cv = -1, f8_was = 0, logged = 0; extern uint8_t* vm_base;
        if (cv < 0) { const char* e = getenv("CULL_VP_SCALE"); cv = (e && *e == '0') ? 0 : 1;
            fprintf(stderr, "[CULLVP] v175 SPU cull viewport scaling %s (RES_SCALE %.0f; F8 toggles)%c", cv ? "ON" : "off", s_res_scale, 10); }
        const int f8 = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (f8 && !f8_was) { cv = !cv; fprintf(stderr, "[CULLVP] F8 -> %s%c", cv ? "ON (cull at render resolution)" : "off (cull at 720p, game default)", 10); }
        f8_was = f8;
        if (vm_base && s_res_scale > 1.0f) {
            uint8_t* b = vm_base + 0x008E40F8u;
            const float k = cv ? s_res_scale : 1.0f;
            const u32 w1 = 1280u, h1 = 720u;
            u32 w = ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
            u32 h = ((u32)b[4] << 24) | ((u32)b[5] << 16) | ((u32)b[6] << 8) | b[7];
            float v[4]; for (int i = 0; i < 4; i++) { u32 x = ((u32)b[0x48 + 4*i] << 24) | ((u32)b[0x49 + 4*i] << 16) | ((u32)b[0x4A + 4*i] << 8) | b[0x4B + 4*i]; memcpy(&v[i], &x, 4); }
            /* recognise the block in either scale: w/h = 1280x720 * s, offX = scaleX = w/2, scaleY = -h/2, offY = h/2 */
            const float s0 = (float)w / (float)w1;
            const int ours = (w * h1 == h * w1) && (s0 == 1.0f || s0 == s_res_scale) &&
                             v[0] == 640.0f * s0 && v[1] == 640.0f * s0 && v[2] == -360.0f * s0 && v[3] == 360.0f * s0;
            if (ours && s0 != k) {
                const u32 nw = (u32)(w1 * k), nh = (u32)(h1 * k);
                const float nv[4] = { 640.0f * k, 640.0f * k, -360.0f * k, 360.0f * k };
                b[0] = nw >> 24; b[1] = nw >> 16; b[2] = nw >> 8; b[3] = nw; b[4] = nh >> 24; b[5] = nh >> 16; b[6] = nh >> 8; b[7] = nh;
                for (int i = 0; i < 4; i++) { u32 x; memcpy(&x, &nv[i], 4); b[0x48+4*i] = x >> 24; b[0x49+4*i] = x >> 16; b[0x4A+4*i] = x >> 8; b[0x4B+4*i] = x; }
                if (logged < 12) { logged++; fprintf(stderr, "[CULLVP] viewport block 0x008E40F8: %ux%u -> %ux%u%c", w, h, nw, nh, 10); }
            } else if (!ours && logged < 12 && logged >= 0) { logged = -1;
                fprintf(stderr, "[CULLVP] block at 0x008E40F8 not recognised (w=%u h=%u %g %g %g %g) -- left alone%c", w, h, v[0], v[1], v[2], v[3], 10); }
        } }
      { static int f5x_was = 0;  /* v231: F5 toggles the LOD_CLASS_SCALE draw distance live */
        const int f5x = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F5) & 0x8000);
        if (f5x && !f5x_was) { g_lodx_f5_off = !g_lodx_f5_off;
            fprintf(stderr, "[LODX] F5 -> draw-distance scale %s%c", g_lodx_f5_off ? "OFF (original pop-in)" : "ON", 10); }
        f5x_was = f5x; }
      lodtab_tick();             /* v231 LOD_CLASS_SCALE draw-distance table */
      lod_scale_tick();          /* v160 LOD_DIST_SCALE (+ F5 toggle) */
      { static int f3_was = 0;   /* v159: F3 = guest memory dump + the F9 capture */
        const int f3 = s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F3) & 0x8000);
        if (f3 && !f3_was) {
            ps3_guest_memdump();
            { static int ge = -1; if (ge < 0) { const char* e = getenv("GEOM_DUMP"); ge = (e && *e == '0') ? 0 : 1; }
              extern volatile int g_geom_arm_req; if (ge) g_geom_arm_req = 1; }   /* v174 */
            if (!f9_left) { f9_left = 3; s_cp_armed = 1;
                fprintf(stderr, "[RSXCAP] F3: capturing the next 3 frames (with the memory dump)%c", 10); }
        }
        f3_was = f3; }
      if (s_d3d.hwnd && GetForegroundWindow() == s_d3d.hwnd && (GetAsyncKeyState(VK_F9) & 0x8000)) {
          if (!f9_wait) { f9_left = 3; f9_wait = 1; s_cp_armed = 1;
              fprintf(stderr, "[RSXCAP] F9: capturing the next 3 frames%c", 10); }
      } else f9_wait = 0;
      /* v33 CAP_AUTO=<s,s,...>: like F9, at those seconds after the first frame with >= 2000 draws */
      { static int ca_n = -1; static double ca_t[16]; static int ca_i = 0; static DWORD w0 = 0;
        if (ca_n < 0) { ca_n = 0; const char* e = getenv("CAP_AUTO");
            while (e && *e && ca_n < 16) { ca_t[ca_n++] = atof(e); e = strchr(e, ','); if (e) e++; } }
        if (ca_n && !w0 && s_dbg_last_draws >= 2000) w0 = GetTickCount();
        if (ca_n && w0 && ca_i < ca_n && f9_left == 0 && !s_cp_pending &&
            (double)(GetTickCount() - w0) / 1000.0 >= ca_t[ca_i]) {
            ca_i++; f9_left = 1; s_cp_armed = 1;
            fprintf(stderr, "[RSXCAP] CAP_AUTO: capture #%d at %.0f s after the world%c", ca_i, ca_t[ca_i - 1], 10);
        } }
      /* v103A: [BLACK] asked for a capture of the black frame -> rsxcap/frame_200.. */
      { static int bl_idx = 200, bl_left = 0;
        if (s_black_cap_req && f9_left == 0 && bl_left == 0 && !s_cp_pending && !s_cp_armed && !s_cp_active) {
            s_black_cap_req = 0; bl_left = 1; s_cp_armed = 1;
            fprintf(stderr, "[RSXCAP] [BLACK] capture armed -> rsxcap/frame_%03d%c", bl_idx, 10);
        } else if (bl_left && s_dbg_last_draws >= 500 && !s_cp_armed && !s_cp_active) {
            fprintf(stderr, "[RSXCAP] [BLACK] capturing frame %u (%u draws) -> rsxcap/frame_%03d%c",
                    (unsigned)s_d3d.frame_count, (unsigned)s_dbg_last_draws, bl_idx, 10);
            rsx_capture_frame(fi, s_dbg_last_draws, (u32)bl_idx++);
            bl_left = 0;
        } }
      /* v219 BENCH_SHOT=<request file>: unattended bench driver vision. When the request file exists, the presented
       * backbuffer is written to <request file>.bmp and the request file is removed. Off (one getenv) without the env. */
      { static int bs = -1; static char bs_req[400], bs_bmp[420];
        if (bs < 0) { const char* e = getenv("BENCH_SHOT"); bs = (e && *e) ? 1 : 0;
                      if (bs) { snprintf(bs_req, sizeof bs_req, "%s", e); snprintf(bs_bmp, sizeof bs_bmp, "%s.bmp", e); } }
        static DWORD bs_last = 0; const DWORD bs_now = GetTickCount();
        if (bs && bs_now - bs_last >= 250 && f9_left == 0 && !s_cp_pending && !s_cp_armed && !s_cp_active) {
            bs_last = bs_now;
            if (GetFileAttributesA(bs_req) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileA(bs_bmp);
                cap_readback_write(s_d3d.render_targets[fi], BBW, BBH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                   D3D12_RESOURCE_STATE_PRESENT, fi, bs_bmp);
                DeleteFileA(bs_req);
                fprintf(stderr, "[BENCH] shot frame %llu (%u draws) -> %s%c", (unsigned long long)s_d3d.frame_count,
                        (unsigned)s_dbg_last_draws, bs_bmp, 10);
            } } }
      if (f9_left > 0 && s_dbg_last_draws >= 500 && !s_cp_armed && !s_cp_active) {
          fprintf(stderr, "[RSXCAP] capturing frame %u (%u draws) -> rsxcap/frame_%02u%c",
                  (unsigned)s_d3d.frame_count, (unsigned)s_dbg_last_draws, (unsigned)f9_idx, 10);
          rsx_capture_frame(fi, s_dbg_last_draws, (u32)f9_idx++);
          f9_left--;
      } }
    { const char* cap = getenv("RSX_CAP");
      if (cap) {
        static int s_cap_parsed = 0;
        static u32 s_cap_start = 0, s_cap_count = 1, s_cap_stride = 1, s_cap_done = 0;
        if (!s_cap_parsed) {
            s_cap_parsed = 1;
            u32 a = 0, b = 1, c = 1;
            int n = sscanf(cap, "%u:%u:%u", &a, &b, &c);
            s_cap_start  = (n >= 1 && a) ? a : 200;
            s_cap_count  = (n >= 2 && b) ? b : 1;
            s_cap_stride = (n >= 3 && c) ? c : 1;
        }
        if (s_cap_done < s_cap_count) {
            u32 target = s_cap_start + s_cap_done * s_cap_stride;
            /* RSX_CAP_MINDRAWS=<N> (2026-10-04): only frames with >= N draws count, so a
             * capture lands on a real world/title frame instead of a near-empty
             * clear-boundary present. Frame numbers are useless for "once the world is
             * up" in a title that boots for minutes. */
            static int s_cap_mind = -1;
            if (s_cap_mind < 0) { const char* e = getenv("RSX_CAP_MINDRAWS"); s_cap_mind = e ? atoi(e) : 0; }
            if (s_d3d.frame_count >= target && s_dbg_last_draws > 0 &&
                s_dbg_last_draws >= (u32)s_cap_mind) {
                fprintf(stderr, "[RSXCAP] capturing frame %u (%u draws) -> %s/frame_%02u%c",
                        (unsigned)s_d3d.frame_count, (unsigned)s_dbg_last_draws,
                        getenv("RSX_CAP_DIR") ? getenv("RSX_CAP_DIR") : "rsxcap", (unsigned)s_cap_done, 10);
                rsx_capture_frame(fi, s_dbg_last_draws, s_cap_done);
                s_cap_done++;
            }
        }
      } }

    /* Present */
    rft_mark(RFT_TAIL2);
    if (s_rf_present)
        { const UINT si = present_sync_interval();
          const HRESULT ph = s_d3d.swap_chain->lpVtbl->Present(s_d3d.swap_chain, si, (si == 0 && s_tearing_ok) ? DXGI_PRESENT_ALLOW_TEARING : 0); /* v209 tearing; v30e: PRESENT_VSYNC (default 0) */   /* skipped for an offscreen-only batch */
          if (FAILED(ph)) d3d12_present_failed(ph); }
    rft_mark(RFT_PRESENT);

    move_to_next_frame();

    s_d3d.frame_count++;
    { extern volatile long long g_fr_presents, g_fr_draws; g_fr_presents++; g_fr_draws += s_dbg_last_draws; }   /* v113 FRAMEREC */

    /* RPCS3-style titlebar stats, refreshed once a second: presented FPS,
     * draw count of the last frame, and the backbuffer size. */
    {
        static ULONGLONG s_tt0 = 0;
        static u64 s_tframes = 0;
        s_tframes++;
        ULONGLONG tnow = GetTickCount64();
        if (s_tt0 == 0) s_tt0 = tnow;
        if (tnow - s_tt0 >= 1000 && s_d3d.hwnd) {
            extern char g_rsx_title_base[128];
            char tb[256];
            /* v228: show the size actually rendered (RES_SCALE backbuffer), not the guest's 1280x720 */
            if (s_bbw && (s_bbw != s_d3d.width || s_bbh != s_d3d.height))
                snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u (game %ux%u)",
                         g_rsx_title_base,
                         s_tframes * 1000.0 / (double)(tnow - s_tt0),
                         s_dbg_last_draws, BBW, BBH, s_d3d.width, s_d3d.height);
            else
                snprintf(tb, sizeof(tb), "%s | FPS: %.2f | draws: %u | %ux%u",
                         g_rsx_title_base,
                         s_tframes * 1000.0 / (double)(tnow - s_tt0),
                         s_dbg_last_draws, s_d3d.width, s_d3d.height);
            rf_set_title(tb);   /* v59: applied on the window thread */
            s_tframes = 0;
            s_tt0 = tnow;
        }
    }
    rft_mark(RFT_NEXT);
    rft_report();
}

/* ===========================================================================
 * v59 RF_ASYNC -- the D3D12 replay of frame N runs on its own thread while the
 * FIFO thread already parses/records frame N+1.
 *
 * Why: the FIFO thread was doing everything in series -- parse, record, vertex
 * conversion AND render_frame (prepass + command recording + present, ~15 ms).
 * render_frame runs at frame N+1's first display clear, i.e. in the middle of the
 * FIFO stream; while it ran nothing was parsed, so the game's per-frame fence
 * (SET_REFERENCE at the end of each frame) was published that much later and the
 * main PPU thread waited for it (play56 sampler: main thread 27% parked in
 * func_001167C4 = "ref >= frame" poll). Now the FIFO thread only hands the frame
 * over (records are double-buffered; per-draw VB/IB/CB/FP snapshots were already
 * per parity) and keeps going.
 *
 * Synchronisation: one frame in flight. Before handing frame N+1 the FIFO thread
 * waits for the render thread to finish frame N; after the hand-off it waits until
 * the render thread has drained the GPU of frame N-1 (wait_for_gpu at the start of
 * the exec), because frame N+2's records will reuse that parity's upload memory.
 * All waits on the FIFO thread pump window messages (it owns the window; Present
 * on the render thread may need it). Frames with legacy (non-VP) draws or the
 * legacy font texture run synchronously. RF_ASYNC=0 -> old behaviour.
 * ======================================================================== */
static HANDLE s_rf_thread = NULL, s_rf_go_ev = NULL, s_rf_done_ev = NULL;
static volatile LONG s_rf_busy = 0;
static int s_rf_on = -1;
static char s_rf_title[256]; static volatile LONG s_rf_title_pending = 0;
static SRWLOCK s_rf_title_lock = SRWLOCK_INIT;
static unsigned long long s_rf_async_frames = 0, s_rf_sync_frames = 0;
static double s_rf_wait_ms = 0.0, s_rf_gpuwait_ms = 0.0;
static void rf_set_title(const char* t)
{
    AcquireSRWLockExclusive(&s_rf_title_lock);
    snprintf(s_rf_title, sizeof s_rf_title, "%s", t);
    s_rf_title_pending = 1;
    ReleaseSRWLockExclusive(&s_rf_title_lock);
}
static void rf_apply_title(void)
{
    if (!s_rf_title_pending || !s_d3d.hwnd) return;
    char t[256];
    AcquireSRWLockExclusive(&s_rf_title_lock);
    memcpy(t, s_rf_title, sizeof t); s_rf_title_pending = 0;
    ReleaseSRWLockExclusive(&s_rf_title_lock);
    SetWindowTextA(s_d3d.hwnd, t);
}
/* Wait for an event while dispatching this (window) thread's messages. */
extern volatile int g_fifo_where;   /* v207 (cellGcmSys.c): 5 = FIFO thread waiting for the render thread */
static void rf_wait_pumping(HANDLE ev)
{
    const int fw0 = g_fifo_where; g_fifo_where = 5;
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &ev, FALSE, 100, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { g_fifo_where = fw0; return; }
        if (r == WAIT_OBJECT_0 + 1) {
            MSG msg;
            while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { s_d3d.window_closed = 1; continue; }
                TranslateMessage(&msg); DispatchMessageA(&msg);
            }
        }
        if (r == WAIT_FAILED) { Sleep(1); }
    }
}
static DWORD WINAPI rf_thread_main(LPVOID p)
{
    (void)p;
    fprintf(stderr, "[THREADS] render thread hosttid=%lu%c", (unsigned long)GetCurrentThreadId(), 10);
    if (getenv("PS3_FAST_HOST")) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    { extern void ps3_render_core_claim(void); ps3_render_core_claim(); }   /* v233C RF_CORE (ppu_loader.cpp) */
    for (;;) {
        WaitForSingleObject(s_rf_go_ev, INFINITE);
        render_frame_exec();
        if (s_rf_gpuidle_ev) SetEvent(s_rf_gpuidle_ev);   /* in case the exec bailed early */
        InterlockedExchange(&s_rf_busy, 0);
        SetEvent(s_rf_done_ev);
    }
    return 0;
}
static int rf_async_on(void)
{
    if (s_rf_on < 0) {
        const char* e = getenv("RF_ASYNC"); s_rf_on = (e && *e == '0') ? 0 : 1;
        if (s_rf_on) {
            s_rf_go_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
            s_rf_done_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
            s_rf_gpuidle_ev = CreateEventA(NULL, TRUE, TRUE, NULL);   /* manual reset */
            s_rf_thread = (s_rf_go_ev && s_rf_done_ev && s_rf_gpuidle_ev)
                        ? CreateThread(NULL, 1u << 20, rf_thread_main, NULL, 0, NULL) : NULL;
            if (!s_rf_thread) s_rf_on = 0;
        }
        fprintf(stderr, "[RF_ASYNC] render thread %s (v59)%c", s_rf_on ? "ON" : "off", 10);
    }
    return s_rf_on;
}
static void rf_wait_idle(void)
{
    if (s_rf_busy) {
        const int telem = ps3_telem_enabled();
        double t0 = telem ? perf_now() : 0.0;
        while (s_rf_busy) rf_wait_pumping(s_rf_done_ev);
        if (telem) s_rf_wait_ms += (perf_now() - t0) * 1000.0;
    }
    rf_apply_title();
}
static void framesig_note(void);
static u32 s_fflip_presented;   /* v237A FLIP_FIFO_BOUNDARY (tentative; defined with s_clear_presents) */
static void render_frame(void)
{
    { extern unsigned cellGcm_flip_request_count(void);   /* v102c frame watchdog bookkeeping */
      framesig_note();
      s_fs_rf_calls++; s_fs_rf_tick = GetTickCount64(); s_fs_flip_at_rf = cellGcm_flip_request_count();
      s_fs_clr_off = s_fs_clr_nodisp = s_fs_clr_sameflip = s_fs_clr_small = s_fs_clr_accum = 0; s_fs_ticker_skips = 0; }
    { static int once = 0; if (!once) { once = 1;
        fprintf(stderr, "[THREADS] FIFO/RSX thread hosttid=%lu%c", (unsigned long)GetCurrentThreadId(), 10);
        { extern void ps3_thread_prefer_pcores(const char* who, int prio);
          ps3_thread_prefer_pcores("RSX FIFO", THREAD_PRIORITY_ABOVE_NORMAL);
          extern void ps3_fifo_core_claim(void); ps3_fifo_core_claim();   /* v219f PS3_FIFO_CORE (ppu_loader.cpp) */
          extern void ps3_prof_register_fifo_thread(void); ps3_prof_register_fifo_thread(); } } }
    { LARGE_INTEGER _w0 = {0}, _w1 = {0}; if (ps3_telem_enabled()) QueryPerformanceCounter(&_w0);
    rsx_vtx_sync();      /* v33: all queued vertex conversions land before the GPU reads them */
    if (ps3_telem_enabled()) QueryPerformanceCounter(&_w1);
    if (s_rft_inv != 0.0) s_rft_acc[RFT_WRAP] += (double)(_w1.QuadPart - _w0.QuadPart) * s_rft_inv; }
    s_rf_legacy_draws = s_legacy_draws; s_rf_legacy_tex_draws = s_legacy_tex_draws;
    s_legacy_draws = s_legacy_tex_draws = 0;
    if (s_d3d.current_rsx_state && !s_d3d.vp_ready &&
        s_d3d.current_rsx_state->vp_ucode_bytes >= 16)
        compile_vp();
    int async = rf_async_on();
    rf_wait_idle();      /* one frame in flight */
    if (async) {
        if (s_d3d.tex_bound) async = 0;   /* legacy font atlas path reads live state */
        else for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++)
            if (!s_d3d.draws[i].is_vp) { async = 0; break; }   /* legacy draws: legacy VB + live MVP */
    }
    s_rf_draws = s_d3d.draws; s_rf_count = s_d3d.draw_count;
    s_rf_parity = s_d3d.vp_parity; s_rf_frame_no = s_frame_no;
    s_rf_present = s_present_this_frame;
#ifdef PS3RECOMP_DLSS
    rsx_dlss_handoff(s_present_this_frame);   /* this batch's jitter/camera -> the render thread; next frame's jitter */
#endif
    if (s_present_this_frame) { s_split_recs = 0; s_split_disp = 0; s_fs_pres_tick = GetTickCount64(); }   /* v103A */
    { extern volatile u32 g_gcm_last_flip_off; extern volatile u32 g_gcm_fifo_flip_off;
      /* v62: the flip the FIFO has executed up to (marker), not the PPU's latest request */
      s_rf_flip_off = g_gcm_fifo_flip_off ? g_gcm_fifo_flip_off : g_gcm_last_flip_off;
      if (s_present_this_frame) s_fflip_presented = g_gcm_fifo_flip_off; }   /* v237A FLIP_FIFO_BOUNDARY */
    if (async) {
        ResetEvent(s_rf_gpuidle_ev);
        InterlockedExchange(&s_rf_busy, 1);
        SetEvent(s_rf_go_ev);
        if (ps3_telem_enabled()) s_rf_async_frames++;
    } else {
        render_frame_exec();
        rf_apply_title();
        if (ps3_telem_enabled()) s_rf_sync_frames++;
    }
    /* Next frame's recording state (was at the end of render_frame). */
    s_frame_no++;
    s_d3d.vb_offset  = 0;
    s_d3d.vp_vb_offset = 0;
    s_vpc_cur = 0;
    s_ib_off = 0;   /* v30d */
    s_d3d.draw_count = 0;
    s_batch_epoch++;   /* v219l */
    /* v207: next record parity. 2 parities: the other one (old). 3: the one used by neither frame N (just handed
     * off) nor N-1 -- frame N-2's, whose GPU work is complete (see rf_npar). */
    int rf_gpu_wait = 1;
    { const int cur = s_d3d.vp_parity, prv = s_par_prev;
      int nxt;
      if (rf_npar() >= 3) { nxt = (cur != prv) ? 3 - cur - prv : (cur + 1) % 3; if (!(g_abx & 0x40000)) rf_gpu_wait = 0; }
      else nxt = cur ^ 1;
      s_par_prev = cur;
      s_d3d.vp_parity = nxt; }
    s_d3d.draws = s_draw_buf[s_d3d.vp_parity];
    fpsnap_reset();      /* the arena of the parity we are about to record into */
    vc_reset_parity(s_d3d.vp_parity);   /* v60: copies of the frame we now record */
    if (async && rf_gpu_wait) {
        /* The parity we now record into was frame N-1's: wait until the render thread
         * has seen the GPU finish it (usually already true). */
        const int telem = ps3_telem_enabled();
        double t0 = telem ? perf_now() : 0.0;
        { extern int v243c_rfbound_on(void); extern volatile int g_v243c_fifo_gpuwait;   /* v243C RFBOUND */
          if (v243c_rfbound_on()) { g_v243c_fifo_gpuwait = 1; rf_wait_pumping(s_rf_gpuidle_ev); g_v243c_fifo_gpuwait = 0; }
          else rf_wait_pumping(s_rf_gpuidle_ev); }
        if (telem) s_rf_gpuwait_ms += (perf_now() - t0) * 1000.0;
    }
    { static u32 n = 0; if (ps3_telem_enabled() && ++n % 600 == 0) {
        fprintf(stderr, "[RF_ASYNC] last 600 frames: %llu async, %llu sync, FIFO thread waited %.0f ms for the render thread, %.0f ms for the GPU%c",
                s_rf_async_frames, s_rf_sync_frames, s_rf_wait_ms, s_rf_gpuwait_ms, 10);
        s_rf_async_frames = s_rf_sync_frames = 0; s_rf_wait_ms = s_rf_gpuwait_ms = 0.0;
        vc_report(); } }
}


/* v103A FRAME_SPLIT: called at the top of every recording entry (draw_arrays, draw_indexed, clear). */
static int frame_split_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("FRAME_SPLIT"); v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[FRAME-SPLIT] v103A: a batch that fills the record buffer (%u) mid-frame is executed and recording"
                        " continues (FRAME_SPLIT=%d; 0 = old: drop every later draw)%c", (unsigned)s_max_draws, v, 10); }
    return v;
}
/* v219l RECS_DISP_SCAN (default 1; 0 or ABX 0x40000000 = old full scans): "does the current batch already contain a
 * display draw?" was a full scan of the records (1 KB stride, cache-cold) on every display clear / flip / split check
 * -- d3d12_clear +0x250 was ~2 % of the FIFO thread after the PGO refresh. Records never change is_clear / rt_off after
 * creation and only grow until render_frame resets the batch (s_batch_epoch), so the scan resumes where it stopped. */
static u32 s_batch_epoch = 0;
static int recs_have_display(void)
{
    const u32 n = s_d3d.draw_count < MAX_DRAWS ? s_d3d.draw_count : MAX_DRAWS;
    static int on = -1; if (on < 0) { const char* e = getenv("RECS_DISP_SCAN"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || (g_abx & 0x40000000)) {
        for (u32 i = 0; i < n; i++) if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) return 1;
        return 0;
    }
    static u32 s_rds_n = 0, s_rds_epoch = 0xFFFFFFFFu; static int s_rds_found = 0;
    if (s_rds_epoch != s_batch_epoch || n < s_rds_n) { s_rds_epoch = s_batch_epoch; s_rds_n = 0; s_rds_found = 0; }
    if (s_rds_found) return 1;
    for (u32 i = s_rds_n; i < n; i++)
        if (!s_d3d.draws[i].is_clear && s_d3d.draws[i].rt_off == 0) { s_rds_found = 1; s_rds_n = n; return 1; }
    s_rds_n = n; return 0;
}
/* FRAME_SPLIT_AT=<n> (test; default 0 = the record cap): split batches at n records instead of s_max_draws, so the
 * split path runs on every busy frame -- reproduces the DEVICE_HUNG seen after bursts of real splits (2026-10-31,
 * LOD_CLASS_SCALE 2.5 city frames > 28672 records). Run with D3D12_DRED=1 for the breadcrumb report. */
static u32 frame_split_cap(void)
{
    static u32 at = 0xFFFFFFFFu;
    if (at == 0xFFFFFFFFu) { const char* e = getenv("FRAME_SPLIT_AT"); at = (e && *e) ? (u32)strtoul(e, NULL, 0) : 0;
        if (at) fprintf(stderr, "[FRAME-SPLIT] test: batches split at %u records (FRAME_SPLIT_AT)%c", at, 10); }
    return (at >= 256u && at < s_max_draws) ? at : s_max_draws;
}
static void frame_split_check(void)
{
    const u32 n = s_d3d.draw_count, cap = frame_split_cap();
    if (n + 64u < cap) return;                       /* fast path: room left */
    if (n < cap && s_d3d.merge_prev_draw) return;   /* let a BEGIN/END triangle stream finish merging */
    if (!s_d3d.initialized) return;
    if (!frame_split_on()) { if (n >= s_max_draws) s_capfull_win++; return; }
    int disp = recs_have_display();   /* v219l */
    s_split_recs += n; s_split_disp |= disp; s_split_n++; s_split_win++;
    if (s_split_n <= 20 || (s_split_n % 500) == 0) {
        extern unsigned cellGcm_flip_request_count(void);
        fprintf(stderr, "[FRAME-SPLIT] #%u: record buffer full (%u/%u) mid-frame -> executing this part without Present"
                        " (display draws in it %d; this frame so far %u records in parts; flips since the last frame %u)%c",
                s_split_n, n, (unsigned)s_max_draws, disp, s_split_recs,
                cellGcm_flip_request_count() - s_fs_flip_at_rf, 10);
    }
    s_present_this_frame = 0;
    render_frame();
    s_present_this_frame = 1;
    s_d3d.merge_prev_draw = 0;
}

/* ---------------------------------------------------------------------------
 * RSX backend callbacks
 * -----------------------------------------------------------------------*/

static int d3d12_init(void* ud, u32 width, u32 height)
{
    (void)ud;
    printf("[D3D12] Backend init(%ux%u)\n", width, height);
    return 0;
}

static void d3d12_shutdown(void* ud)
{
    (void)ud;
    printf("[D3D12] Backend shutdown\n");
}

static void d3d12_begin_frame(void* ud)
{
    (void)ud;
}

static void d3d12_end_frame(void* ud)
{
    (void)ud;
}

static u32 s_dbg_clears_since_present = 0;   /* CELLMARK_BLINKDBG */
static u32 s_clear_presents = 0;   /* presents issued at clear (frame boundary) */
/* v237A FLIP_FIFO_BOUNDARY (default 0 until benched; 1 = on; FLIP_FIFO_BOUNDARY_PHASE=0|1 = only on that PS3_AB
 * phase parity). b170 (user's area): the guest flipped ~54/s but only ~42 frames/s were presented -- 2/3 of the
 * > 25 ms "frames" were TWO guest frames merged into one batch ("flips 2", ~6k records). Cause: the display-clear
 * boundary below also demands eff_n >= s_frame_draws_max / 4, and that all-time max (20546 records, city views)
 * makes every light frame (~3k records: sky / water during the pan) an "intermediate": its clear is skipped and the
 * next frame is drawn into the same batch, so one of every two guest frames is rendered but never shown.
 * The guest flip count is taken at guest call time, which is why the size test existed (a clear can be walked
 * after the guest already flipped but before the FIFO reached that flip). inFamous also writes the v62 flip NOTE
 * into the FIFO; g_gcm_fifo_flip_off changes exactly when the walker passes it. So: if the walker has passed a
 * flip to a different display buffer than the one last presented, everything of that frame is in the batch and
 * this display clear belongs to a later frame -> it is a real boundary, the same-flip and size tests are skipped.
 * Otherwise (no note walked since, or the same buffer flipped twice) the old tests apply unchanged. Every draw is
 * still executed in the same order; the only difference is that the completed frame gets its own Present. */
static u32 s_fflip_presented = 0;   /* g_gcm_fifo_flip_off at the last render_frame that presented */
static int s_ffb_cfg = -1, s_ffb_phase = -1;
static u64 s_ffb_used = 0, s_ffb_small = 0, s_ffb_same = 0;
static int ffb_on(void)
{
    extern volatile int g_ab_phase;
    if (s_ffb_cfg < 0) {
        const char* e = getenv("FLIP_FIFO_BOUNDARY");
        const char* p = getenv("FLIP_FIFO_BOUNDARY_PHASE");
        s_ffb_cfg = (e && *e == '1') ? 1 : 0;
        if (p && *p) s_ffb_phase = atoi(p) & 1;
        fprintf(stderr, "[FLIP_FIFO_BOUNDARY] v237A display clear after a walked FIFO flip ends the frame: %s (phase parity %d)%c",
                s_ffb_cfg ? "ON" : "off", s_ffb_phase, 10);
    }
    return s_ffb_cfg && (s_ffb_phase < 0 || ((g_ab_phase & 1) == s_ffb_phase));
}

/* FRAME_BUDGET=1: how much geometry a guest frame actually asks for, versus
 * what the per-frame vertex buffer can hold. A frame that overflows is silently
 * truncated -- the tail of the scene never renders -- which reads as "the object
 * is missing" rather than "the batch is too small". */

static int blink_dbg(void)
{
    static int v = -1;
    if (v < 0) v = getenv("CELLMARK_BLINKDBG") ? 1 : 0;
    return v;
}

static void d3d12_present(void* ud, u32 buffer_id)
{
    (void)ud;
    (void)buffer_id;

    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present);
    s_dbg_clears_since_present = 0;

    /* An accumulated batch with draws but NONE targeting a display buffer is
     * offscreen pass work only (demosaic flips once per effect pass): showing
     * it would strobe the bare backbuffer clear. Keep accumulating -- the
     * composite draw that targets the display presents the whole chain, in
     * order, in one command list. Empty batches still present (boot/idle). */
    int flip_has_display = (s_d3d.draw_count == 0) || s_split_disp;   /* v103A: split parts */
    if (!flip_has_display && recs_have_display()) flip_has_display = 1;   /* v219l */

    if (s_d3d.initialized && flip_has_display)
        render_frame();

    /* v102b ACCUM guard (ACCUM_FLIP_MAX, default 8; 0 = old unbounded behaviour). play102 t~200 s: flips kept
     * arriving but no batch had a draw classified as "display" (rt_off == 0), so nothing was ever executed or
     * presented: one batch accumulated for ~20 s (VPVB parity 1 grew to 12 chunks = 3 GB), the screen stayed
     * black, the FIFO slowed to ~4 frames/s and finally stalled (main thread spinning on the GCM ref, all SPURS
     * work idle). Keeping offscreen-only work across a FEW flips is intended (multi-flip effect chains); keeping
     * it forever is not. After ACCUM_FLIP_MAX flips without a display draw, or with the record buffer full,
     * EXECUTE the batch without Present (the screen keeps the last presented image, as a PS3 shows the last
     * flipped buffer) and log [ACCUM] with what the batch drew into, so the next log says why nothing counted
     * as display. */
    { static int s_lim = -1; static u32 s_unpres = 0, s_nlog = 0;
      if (s_lim < 0) { const char* e = getenv("ACCUM_FLIP_MAX"); s_lim = e ? atoi(e) : 8; if (s_lim < 0) s_lim = 0; }
      if (flip_has_display) s_unpres = 0;
      else if (s_lim && ++s_unpres >= (u32)s_lim && s_d3d.initialized) {
          ++s_nlog;
          if (s_nlog <= 24 || (s_nlog % 300) == 0) {
              u32 offs[8] = {0}, cnt[8] = {0}, nclr = 0; int nd = 0;
              for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
                  if (s_d3d.draws[i].is_clear) { nclr++; continue; }
                  int j = 0; while (j < nd && offs[j] != s_d3d.draws[i].rt_off) j++;
                  if (j == nd && nd < 8) { offs[nd] = s_d3d.draws[i].rt_off; cnt[nd] = 0; nd++; }
                  if (j < nd) cnt[j]++;
              }
              const rsx_state* st = s_d3d.current_rsx_state;
              extern int cellGcmOffsetIsDisplay(u32 offset);
              fprintf(stderr, "[ACCUM] #%u: %u flips without a display draw, %u records (%u clears, cap %u) -> executing"
                              " without Present; cur surface tgt=0x%X off=0x%X/0x%X clip=%ux%u isdisp=%d/%d; targets:",
                      s_nlog, s_unpres, s_d3d.draw_count, nclr, s_max_draws,
                      st ? st->color_target : 0, st ? st->surface_color_offset[0] : 0, st ? st->surface_color_offset[1] : 0,
                      st ? st->surface_clip_w : 0, st ? st->surface_clip_h : 0,
                      st ? cellGcmOffsetIsDisplay(st->surface_color_offset[0]) : -1,
                      st ? cellGcmOffsetIsDisplay(st->surface_color_offset[1]) : -1);
              for (int j = 0; j < nd; j++) fprintf(stderr, " 0x%X x%u", offs[j], cnt[j]);
              fprintf(stderr, "%c", 10);
          }
          if (s_d3d.draw_count > 0) {
              s_present_this_frame = 0;
              render_frame();
              s_present_this_frame = 1;
          }
          s_unpres = 0;
      } }

    /* FPS tracking */
    ULONGLONG now = GetTickCount64();
    if (now - s_d3d.last_fps_time >= 1000) {
        s_d3d.fps = (u32)s_d3d.frame_count; /* rough estimate */
        s_d3d.last_fps_time = now;
        s_d3d.frame_count = 0;
    }
}

/* CLEAR_SURFACE mask: bit0 Z, bit1 S, bits 4-7 colour. Before 2026-10-04 the mask
 * was ignored: every clear cleared COLOUR (a depth-only clear wiped the target's
 * picture) and depth clears were dropped. CLEAR_MASK_FIX=0 restores that. */
static void rec_zeta(D3D12DrawRecord* dr);   /* below, before d3d12_draw_arrays */
static u32 clear_mask_eff(u32 flags)
{
    static int f = -1;
    if (f < 0) { const char* e = getenv("CLEAR_MASK_FIX"); f = (e && *e == '0') ? 0 : 1; }
    return f ? flags : 0xF3u;
}
static void d3d12_clear_inner(void* ud, u32 flags, u32 color, float depth, u8 stencil);
static void d3d12_clear(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    u32 rw = 0, rh = 0, rm[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rw, &rh, rm);
    /* v103A FRAME_SPLIT. v103D: not BEFORE a display clear -- a split there left the boundary that follows
     * an EMPTY batch to present (s_rf_count 0 -> no DISPLAY_AS_RT copy -> a bare clear-colour frame). A
     * display clear records nothing until after the boundary logic, so it is checked just before that. */
    if (rt != 0) frame_split_check();
    d3d12_clear_inner(ud, flags, color, depth, stencil);
    /* Display-surface clear: the colour part is the frame-start backbuffer clear
     * (handled inside, possibly presenting the previous frame first); the depth/
     * stencil part must reach the zeta surface IN ORDER, so record it after the
     * frame-boundary logic as a depth-only op (is_clear = 2). */
    u32 m = clear_mask_eff(flags);
    int want_col = disp_as_rt() && (m & 0xF0u);
    if (rt == 0 && ((zs_on() && (m & 3u)) || want_col)) frame_split_check();   /* v103D: see above */
    if (rt == 0 && ((zs_on() && (m & 3u)) || want_col) && s_d3d.draw_count < s_max_draws) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
        memset(dr, 0, sizeof(*dr));
        dr->is_vp = 1; dr->tex_slot = -1; dr->samp_tbl = samp_pack_on() ? 0 : samp_table_for_cur();
        dr->rt_off = 0;
        rec_zeta(dr);
        /* colour part (DISPLAY_AS_RT: clears the display buffer's OffRT in order) */
        dr->is_clear = want_col ? 1 : 2;
        dr->cc[0] = ((color >> 16) & 0xFF) / 255.0f; dr->cc[1] = ((color >> 8) & 0xFF) / 255.0f;
        dr->cc[2] = (color & 0xFF) / 255.0f;         dr->cc[3] = ((color >> 24) & 0xFF) / 255.0f;
        dr->clr_mask = zs_on() ? m : (m & 0xF0u); dr->clr_depth = depth; dr->clr_stencil = stencil;
    }
}
static void d3d12_clear_inner(void* ud, u32 flags, u32 color, float depth, u8 stencil)
{
    (void)ud;
    (void)flags;
    (void)depth;
    (void)stencil;

    /* Convert RSX ARGB u32 to float[4] RGBA */
    float cc[4];
    cc[0] = ((color >> 16) & 0xFF) / 255.0f; /* R */
    cc[1] = ((color >> 8) & 0xFF) / 255.0f;  /* G */
    cc[2] = (color & 0xFF) / 255.0f;          /* B */
    cc[3] = ((color >> 24) & 0xFF) / 255.0f;  /* A */
    /* CLEAR_RGB=r,g,b: override the guest clear colour. Black holes (geometry
     * that never rasterized) and black pixels a shader really wrote look
     * identical against a black clear; this separates them in one run. */
    { static int _cinit = 0; static float _co[3]; static int _con = 0;
      if (!_cinit) { _cinit = 1; const char* e = getenv("CLEAR_RGB");
          if (e && sscanf(e, "%f,%f,%f", &_co[0], &_co[1], &_co[2]) == 3) _con = 1; }
      if (_con) { cc[0] = _co[0]; cc[1] = _co[1]; cc[2] = _co[2]; } }

    u32 rt_w = 0, rt_h = 0, mrt[3] = {0, 0, 0};
    u32 rt = current_rt_off(&rt_w, &rt_h, mrt);

    /* An OFFSCREEN clear is just an ordered op in the current frame's pass
     * chain (demosaic clears each effect pass's surface) -- record it, don't
     * touch the frame boundary. */
    if (rt != 0) {
        s_fs_clr_off++;   /* v102c */
        if (s_d3d.draw_count < s_max_draws) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count++];
            memset(dr, 0, sizeof(*dr));
            dr->is_vp = 1; dr->is_clear = 1; dr->tex_slot = -1; dr->samp_tbl = samp_pack_on() ? 0 : samp_table_for_cur();
            dr->rt_off = rt; memcpy(dr->rt_mrt, mrt, sizeof(mrt));
            dr->rt_w = rt_w; dr->rt_h = rt_h;
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            memcpy(dr->cc, cc, sizeof(cc));
            rec_zeta(dr);
            dr->clr_mask = clear_mask_eff(flags); dr->clr_depth = depth; dr->clr_stencil = stencil;
        }
        return;
    }

    memcpy(s_d3d.clear_color, cc, sizeof(cc));

    /* A DISPLAY clear marks the start of a new visible frame. If a completed
     * frame is still accumulated (the drain gulped across a frame boundary --
     * guaranteed at the FIFO ring wrap, where rest-of-frame-N + clear-N+1
     * arrive in one batch), PRESENT it now instead of discarding it. Only a
     * batch that actually contains DISPLAY draws is a completed frame: a
     * batch of offscreen pass work (render-to-texture) must keep accumulating
     * until its composite draw arrives, or the screen strobes intermediates. */
    int have_display_draws = s_split_disp;   /* v103A: a split part already drew into a display buffer */
    if (!have_display_draws && recs_have_display()) have_display_draws = 1;   /* v219l */
    /* v103A: no Present for FRAME_STALL_MS -> this display clear ends the frame whatever the tests below say */
    const int fs_ms = frame_stall_ms();
    const int stalled = fs_ms && s_fs_pres_tick && s_d3d.initialized && s_d3d.draw_count > 0 &&
                        GetTickCount64() - s_fs_pres_tick >= (ULONGLONG)fs_ms;
    if (!have_display_draws) {
        s_fs_clr_nodisp++;   /* v102c */
        if (stalled) {   /* v103A: run the offscreen work (no Present) so the batch cannot grow without bound */
            s_fs_clr_forced++;
            frame_stall_report("display clear, no display draws, no Present for FRAME_STALL_MS: executing without Present");
            s_present_this_frame = 0; render_frame(); s_present_this_frame = 1;
        }
        return;   /* keep accumulating the in-progress frame */
    }

    /* A title that DOUBLE-BUFFERS the display (DeferredShading clears both
     * 0x0 and 0x440000 per frame, plus a HUD pass) issues several display
     * clears per real frame -- treating each as a boundary presented the
     * 1-2-draw intermediates, strobing black between the full 146-draw
     * frames. Anchor the boundary to the FLIP instead: only present once a
     * cellGcmSetFlip has landed since the last present. Titles that flip once
     * per frame (wave/cellmark/gcmcube) are unaffected -- their single
     * display clear still follows their single flip. Fall back to the old
     * clear-only heuristic for titles that never flip (fc stays 0). */
    static u32 s_frame_draws_max = 0;   /* running max frame size (typical full frame) */
    const u32 eff_n = s_d3d.draw_count + s_split_recs;   /* v103A: records of the split parts count */
    if (eff_n > s_frame_draws_max) s_frame_draws_max = eff_n < s_max_draws ? eff_n : s_max_draws;
    s_fs_frame_max_mirror = s_frame_draws_max;   /* v102c */
    {
        extern unsigned cellGcm_flip_request_count(void);
        static unsigned s_last_present_flip = 0;
        unsigned fc = cellGcm_flip_request_count();
        /* v237A FLIP_FIFO_BOUNDARY (see s_fflip_presented) */
        extern volatile u32 g_gcm_fifo_flip_off;
        const u32 ffo = g_gcm_fifo_flip_off;
        const int fifo_flip = ffo != 0 && ffo != s_fflip_presented && ffb_on();
        if (fc != 0) {
            s_fs_last_present_flip_mirror = s_last_present_flip;   /* v102c */
            if (fc == s_last_present_flip && !stalled) {
              if (!fifo_flip) {
                s_fs_clr_sameflip++;
                return;   /* no flip since last present -> not a real boundary */
              }
              s_ffb_same++;
            }
            /* A double-buffered title issues several display clears per flip
             * (DeferredShading: clear back-buffer, HUD pass, ...). They can
             * arrive in either order, so a small batch here may be an
             * intermediate that precedes the full frame's clear. Don't present
             * (or consume the flip) until the batch is a substantial fraction
             * of a full frame -- keep accumulating so the complete frame lands
             * in one present. Gauged against the running MAX frame size (not
             * the last, which a leaked tiny batch would poison), so it
             * self-scales per title with no fixed threshold. */
            if (s_frame_draws_max > 16 && eff_n < s_frame_draws_max / 4 && !stalled) {
              if (!fifo_flip) {
                s_fs_clr_small++;   /* v102c */
                return;   /* intermediate: keep accumulating, flip stays pending */
              }
              s_ffb_small++;   /* v237A: a complete frame the size test would have merged into the next one */
            }
            if (fifo_flip) s_ffb_used++;
            s_last_present_flip = fc;
        }
    }
    if (stalled) {   /* v103A: log which test it overrode (the counters in the line) */
        s_fs_clr_forced++;
        frame_stall_report("display clear forced to end the frame: no Present for FRAME_STALL_MS (flip/size tests bypassed)");
    }

    /* RSX_ACCUM_FRAME=1: never present at a clear boundary, so every draw in a
     * guest frame accumulates into one image. A title that clears several times
     * per frame (render-to-texture passes) otherwise gets presented mid-scene,
     * and each captured frame holds only a slice of the geometry -- the floor in
     * one, a wall in the next -- which makes a single model impossible to see. */
    { static int accum = -1;
      if (accum < 0) { const char* e = getenv("RSX_ACCUM_FRAME"); accum = e ? atoi(e) : 0; }
      if (accum) { s_fs_clr_accum++; return; } }
    if (s_d3d.initialized) {
        if (blink_dbg())
            printf("[CLEAR] presenting %u accumulated draws at frame boundary\n",
                   s_d3d.draw_count);
        render_frame();
        s_clear_presents++;
        if (s_ffb_cfg > 0 && (s_clear_presents % 600u) == 0) {   /* v237A */
            fprintf(stderr, "[FLIP_FIFO_BOUNDARY] last 600 clear presents: %llu ended by a walked FIFO flip (%llu the size test, %llu the same-flip test would have merged)%c",
                    (unsigned long long)s_ffb_used, (unsigned long long)s_ffb_small, (unsigned long long)s_ffb_same, 10);
            s_ffb_used = s_ffb_small = s_ffb_same = 0;
        }
    }
    s_dbg_clears_since_present++;
    rsx_vtx_sync();      /* v33: VB rewinds below */
    s_d3d.draw_count   = 0;
    fpsnap_reset();   /* v33 */
    s_d3d.vb_offset    = 0;
    s_d3d.vp_vb_offset = 0;
    s_vpc_cur = 0;
    s_ib_off = 0;   /* v30d */
}

static void d3d12_set_render_target(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    /* Log only the first few; set_render_target is called every frame and
     * floods the log otherwise. */
    static int s_count = 0;
    if (s_count < 5) {
        printf("[D3D12] set_render_target(%ux%u)\n",
               state->surface_clip_w, state->surface_clip_h);
        s_count++;
    }
}

static void d3d12_set_viewport(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: update D3D12 viewport from RSX state */
    (void)state;
}

/* Our host vertex layout for the fallback path: position (xyz) + color (rgba)
 * + texcoord (uv), 36 bytes. (The real VP path feeds raw float4 attrib0.) */
typedef struct { float x, y, z; float r, g, b, a; float u, v; } BasicVertex;


/* Read one RSX vertex (by absolute vertex index) from guest memory into our
 * host layout. Position is attrib 0 (float3+), color is attrib 3 (ubyte4 or
 * float4); missing attribs default to opaque white. RSX stores each 32-bit
 * component big-endian, so every lane is byte-swapped. */
static void read_rsx_vertex(const rsx_state* state, u32 vindex, BasicVertex* out)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    out->x = out->y = out->z = 0.0f;
    out->r = out->g = out->b = out->a = 1.0f;
    out->u = out->v = 0.0f;
    if (!state || !vm_base) return;

    const rsx_vertex_attrib* pos = &state->vertex_attribs[0];
    if (pos->enabled && pos->type == 2 /* float */ && pos->size >= 2) {
        u8* src = vm_base + cellGcmResolveOffset(pos->offset + vindex * pos->stride);
        out->x = rsx_rd_bef(src);
        out->y = rsx_rd_bef(src + 4);
        if (pos->size >= 3) out->z = rsx_rd_bef(src + 8);

        /* dbgfont (and similar 2D overlays) store screen positions biased far
         * outside clip space; their vertex program folds them back to clip
         * space using transform constants we don't execute. When a position is
         * well outside NDC, recover the fractional part as a normalized [0,1]
         * screen coord and map it to NDC (screen Y-down -> clip Y-up). In this
         * layout the attribute is [posX, posY, U, V] (size 4), so the last two
         * components are the atlas texcoords, not depth.
         * TODO: execute the real vertex-program / RSX viewport transform so
         * this is not needed. */
        if (out->x > 2.0f || out->x < -2.0f || out->y > 2.0f || out->y < -2.0f) {
            float sx = out->x - floorf(out->x);
            float sy = out->y - floorf(out->y);
            out->x = sx * 2.0f - 1.0f;
            out->y = 1.0f - sy * 2.0f;
            out->z = 0.0f;
            if (pos->size >= 4) {
                out->u = rsx_rd_bef(src + 8);   /* U */
                out->v = rsx_rd_bef(src + 12);  /* V */
            }
        }
    }

    const rsx_vertex_attrib* col = &state->vertex_attribs[3];
    if (col->enabled && col->size >= 3) {
        u8* src = vm_base + cellGcmResolveOffset(col->offset + vindex * col->stride);
        if (col->type == 4 /* ubyte */) {
            out->r = src[0] / 255.0f;
            out->g = src[1] / 255.0f;
            out->b = src[2] / 255.0f;
            out->a = (col->size >= 4) ? src[3] / 255.0f : 1.0f;
        } else if (col->type == 2 /* float */) {
            u32 fr, fg, fb, fa;
            memcpy(&fr, src,     4); fr = ((fr>>24)&0xFF)|((fr>>8)&0xFF00)|((fr<<8)&0xFF0000)|((fr<<24)&0xFF000000);
            memcpy(&fg, src + 4, 4); fg = ((fg>>24)&0xFF)|((fg>>8)&0xFF00)|((fg<<8)&0xFF0000)|((fg<<24)&0xFF000000);
            memcpy(&fb, src + 8, 4); fb = ((fb>>24)&0xFF)|((fb>>8)&0xFF00)|((fb<<8)&0xFF0000)|((fb<<24)&0xFF000000);
            memcpy(&out->r, &fr, 4); memcpy(&out->g, &fg, 4); memcpy(&out->b, &fb, 4);
            if (col->size >= 4) {
                memcpy(&fa, src + 12, 4); fa = ((fa>>24)&0xFF)|((fa>>8)&0xFF00)|((fa<<8)&0xFF0000)|((fa<<24)&0xFF000000);
                memcpy(&out->a, &fa, 4);
            }
        }
    }
}

/* Upload `count` sequential vertices [first, first+count). Returns the count
 * actually written (clamped to the remaining per-frame buffer). */
static u32 upload_vertices_from_rsx(u32 first, u32 count)
{
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (count > max_verts) count = max_verts;
    const rsx_state* state = s_d3d.current_rsx_state;
    for (u32 i = 0; i < count; i++)
        read_rsx_vertex(state, first + i, &verts[i]);
    s_d3d.vb_offset += count * sizeof(BasicVertex);
    return count;
}

/* Upload RSX QUADS (prim 8) as a triangle list: each 4-vertex quad v0..v3
 * (perimeter winding) splits into triangles (v0,v1,v2) and (v0,v2,v3).
 * D3D12 has no quad topology, so this expansion is how quads render at all.
 * Returns the number of triangle-list vertices emitted (6 per quad). */
static u32 upload_quads_from_rsx(u32 first, u32 count)
{
    const rsx_state* state = s_d3d.current_rsx_state;
    u32 quads = count / 4;
    u32 max_verts = (MAX_VERTICES * VERTEX_STRIDE - s_d3d.vb_offset) / sizeof(BasicVertex);
    if (quads * 6 > max_verts) quads = max_verts / 6;
    BasicVertex* verts = (BasicVertex*)((u8*)s_d3d.vb_mapped + s_d3d.vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        BasicVertex c[4];
        for (u32 k = 0; k < 4; k++)
            read_rsx_vertex(state, first + q * 4 + k, &c[k]);
        verts[o++] = c[0]; verts[o++] = c[1]; verts[o++] = c[2];
        verts[o++] = c[0]; verts[o++] = c[2]; verts[o++] = c[3];
    }
    s_d3d.vb_offset += o * sizeof(BasicVertex);
    return o;
}

/* Upload RSX QUADS as raw float4 attrib0 (byte-swapped) into vp_vb, expanded
 * to a triangle list (6 verts/quad). The decompiled vertex shader does the
 * transform. Returns emitted vertex count. */
/* Generic VP vertex: all 16 RSX vertex attributes, each converted to a float4
 * slot -- 256 bytes/vertex, input layout ATTRi @ i*16. Apps place attributes at
 * arbitrary indices (tiny3d: pos=a0 colour=a3 tex=a8; SDK gcm samples: pos=a0
 * colour=a1 tex=a2; dbgfont: a0..a2), so a hardcoded pos+colour pair can't
 * cover them: every enabled attrib is fetched from guest memory and converted
 * by its RSX type; disabled slots read (0,0,0,1). */
/* VPSlot, s_vpk and VP_VERT_STRIDE are defined near the top (VP vertex chunks). */


void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n);

/* Fill all 16 attribute slots for one vertex. The per-attribute work lives in
 * rsx_vertex_fetch.c so the Metal and null backends read guest vertices the
 * same way this one does, rather than each porting the logic again. */
/* 2026-10-04 (FPS): per-draw vertex FETCH PLAN. rsx_fetch_attrib() re-derived
 * everything per vertex per attribute: enabled/stride checks, the frequency
 * divider getenv, the offset->EA resolve (function call + page table) and the
 * format switch -- ~10M times per frame in inFamous (vtx ~75 ms/frame). The plan
 * resolves each attribute once per draw; the MAIN-memory (IO) resolve is linear
 * within a 1 MB page, so one cached page base per attribute is exact, and LOCAL
 * (VRAM) is linear throughout. Same semantics as rsx_fetch_attrib (constant
 * register default, size<4 keeps the register's components, imm-mode verts,
 * MODULO/DIVIDE frequency). VTX_FASTFETCH=0 -> old per-vertex path. */
typedef struct {
    u8 mode;            /* 0 = constant register only, 1 = array, 2 = immediate */
    u8 type, n, local;
    u32 off0, stride, freq, modulo;
    u32 cpage; const u8* cbase;
    float def[4];
} VFAttr;
static VFAttr s_vfa[16];
static int s_vf_ok = 0;
static int vf_fast_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VTX_FASTFETCH"); v = (e && *e == '0') ? 0 : 1;
                 if (getenv("VTX_POS")) v = 0;   /* keep the per-vertex debug print path */ }
    return v;
}
static void vf_plan(const rsx_state* st)
{
    extern uint8_t* vm_base;
    s_vf_ok = 0;
    if (!vf_fast_on() || !st) return;
    static int nofreq = -1;
    if (nofreq < 0) nofreq = getenv("VP_NOFREQ") ? 1 : 0;
    for (int i = 0; i < 16; i++) {
        VFAttr* f = &s_vfa[i];
        const rsx_vertex_attrib* a = &st->vertex_attribs[i];
        memcpy(f->def, st->vertex_data4f[i], sizeof f->def);
        if (st->imm_active) { f->mode = 2; continue; }
        if (!a->enabled || a->stride == 0 || !vm_base) { f->mode = 0; continue; }
        f->mode   = 1;
        f->type   = (u8)a->type;
        f->n      = (u8)(a->size ? (a->size > 4 ? 4 : a->size) : 4);
        f->local  = (a->offset & 0x80000000u) ? 0 : 1;
        f->off0   = a->offset & 0x7FFFFFFFu;
        f->stride = a->stride;
        f->freq   = (a->frequency > 1 && !nofreq) ? a->frequency : 0;
        f->modulo = (st->frequency_divider_op >> i) & 1u;
        f->cpage  = 0xFFFFFFFFu; f->cbase = NULL;
    }
    s_vf_ok = 1;
}
static inline const u8* vf_ptr(VFAttr* f, u32 off)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    u32 pg = off >> 20;
    if (pg != f->cpage) {
        f->cpage = pg;
        f->cbase = vm_base + cellGcmResolveLocated(f->local, pg << 20);
    }
    return f->cbase + (off & 0xFFFFFu);
}
static inline u32 vf_bswap32(u32 w) { return __builtin_bswap32(w); }
static inline void read_vp_vertex_fast_k(const rsx_state* st, VFAttr* vfa, u32 vpk, u32 vi, VPSlot* out16)
{
    for (int i = 0; i < (int)vpk; i++) {
        VFAttr* f = &vfa[i];
        float t[4];
        if (f->mode == 2) {
            if (vi < st->imm_count) memcpy(t, st->imm_verts[vi][i], sizeof t);
            else memcpy(t, f->def, sizeof t);
            memcpy(out16[i].v, t, sizeof t);
            continue;
        }
        memcpy(t, f->def, sizeof t);
        if (f->mode == 1) {
            u32 ei = vi;
            if (f->freq) ei = f->modulo ? (vi % f->freq) : (vi / f->freq);
            const u8* p = vf_ptr(f, f->off0 + ei * f->stride);
            u32 n = f->n;
            switch (f->type) {
            case 3:  for (u32 k = 0; k < n; k++) t[k] = rsx_rd_half_be_i(p + k * 2); break;
            case 4:  for (u32 k = 0; k < n; k++) t[k] = p[k] / 255.0f; break;
            case 1:  for (u32 k = 0; k < n; k++) t[k] = (float)(int16_t)((p[k*2] << 8) | p[k*2+1]) / 32767.0f; break;
            case 5:  for (u32 k = 0; k < n; k++) t[k] = (float)(int16_t)((p[k*2] << 8) | p[k*2+1]); break;
            case 7:  for (u32 k = 0; k < n; k++) t[k] = (float)p[k]; break;
            case 6:  rsx_cmp_decode_i(p, t); break;   /* v32: CMP packed normals */
            default: /* 2 = float32 BE (and the old fallback for unknown types) */
#if defined(__SSE4_1__)
                /* v30d: 3/4-component BE floats as one shuffle (bit-identical; never
                 * reads past the n*4 bytes the scalar loop reads). */
                if (n >= 3) {
                    const __m128i bs = _mm_setr_epi8(3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12);
                    __m128i x;
                    if (n == 4) x = _mm_loadu_si128((const __m128i*)p);
                    else { int w2; memcpy(&w2, p + 8, 4);
                           x = _mm_insert_epi32(_mm_loadl_epi64((const __m128i*)p), w2, 2); }
                    x = _mm_shuffle_epi8(x, bs);
                    if (n == 4) { _mm_storeu_si128((__m128i*)out16[i].v, x); continue; }
                    float tmp[4]; _mm_storeu_si128((__m128i*)tmp, x);
                    t[0] = tmp[0]; t[1] = tmp[1]; t[2] = tmp[2];
                    break;
                }
#endif
                for (u32 k = 0; k < n; k++) { u32 w; memcpy(&w, p + k * 4, 4); w = vf_bswap32(w); memcpy(&t[k], &w, 4); }
                break;
            }
        }
        memcpy(out16[i].v, t, sizeof t);
    }
}

static void read_vp_vertex_fast_p(const rsx_state* st, VFAttr* vfa, u32 vi, VPSlot* out16)
{
    read_vp_vertex_fast_k(st, vfa, s_vpk, vi, out16);
}
static void read_vp_vertex_fast(const rsx_state* st, u32 vi, VPSlot* out16)
{
    read_vp_vertex_fast_p(st, s_vfa, vi, out16);
}

static void read_vp_vertex(const rsx_state* state, u32 vi, VPSlot* out16)
{
    if (s_vf_ok) { read_vp_vertex_fast(state, vi, out16); return; }
    for (int i = 0; i < (int)s_vpk; i++) {
        rsx_fetch_attrib(state, i, vi, out16[i].v);
        if (i == 0) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[0];
            u32 n = a->size ? a->size : 4; if (n > 4) n = 4;
            rsx_vtx_pos_dbg(state, out16[0].v, n);
        }
    }
}

/* 2026-10-04 (FPS): per-draw vertex memo. Indexed meshes reference each vertex
 * from ~2-6 triangles and strips from 3, and every reference ran the full
 * per-attribute fetch (resolve + format decode) -- ~2.6M conversions per frame
 * in inFamous (PERF vtx ~60 ms/frame). Convert each guest index once per draw
 * into a CPU-side cache, then memcpy into the upload heap (never read back from
 * the write-combined upload memory). Direct-mapped by index, invalidated per draw
 * by a generation counter. RSX replay is single-threaded. VTX_MEMO=0 disables. */
#define VTX_MEMO_N 4096
static VPSlot s_vm_data[VTX_MEMO_N][16];
static u32 s_vm_idx[VTX_MEMO_N], s_vm_gen[VTX_MEMO_N], s_vm_g = 0;
static int vtx_memo_on(void) { static int v = -1; if (v < 0) { const char* e = getenv("VTX_MEMO"); v = (e && *e == '0') ? 0 : 1; } return v; }
static void vp_memo_begin(void)
{
    if (++s_vm_g == 0) { memset(s_vm_gen, 0, sizeof s_vm_gen); s_vm_g = 1; }
}
static void read_vp_vertex_memo(const rsx_state* state, u32 vi, VPSlot* dst)
{
    if (!vtx_memo_on() || state->imm_active) { read_vp_vertex(state, vi, dst); s_perf_vconv++; return; }
    u32 h = (vi ^ (vi >> 12)) & (VTX_MEMO_N - 1);
    if (s_vm_gen[h] != s_vm_g || s_vm_idx[h] != vi) {
        read_vp_vertex(state, vi, s_vm_data[h]);
        s_vm_gen[h] = s_vm_g; s_vm_idx[h] = vi; s_perf_vconv++;
    }
    memcpy(dst, s_vm_data[h], s_vpk * sizeof(VPSlot));
}

/* 2026-10-04 (FPS v27): MULTI-THREADED vertex conversion. In-game the RSX replay
 * thread was the frame limiter (81% busy, ~40% of it read_vp_vertex + memo) while
 * the i7 had idle cores. Each large draw's output vertex list (guest index per
 * output vertex) is built on the RSX thread, then split into contiguous ranges
 * converted by a small worker pool + the RSX thread itself, each with a private
 * fetch-plan copy (vf_ptr page cache) and a private memo. Synchronous: the RSX
 * thread waits for the draw to finish, so guest memory / rsx_state ordering is
 * exactly as before and the merge logic sees identical output. Workers spin
 * briefly (draws come in bursts) then sleep on an event.
 * VTX_MT=<workers> (default 4, 0 = off), VTX_MT_MIN=<verts> (default 768),
 * VTX_MT_SPIN_US (default 150). */
#define VMT_MAX 12
typedef struct { VPSlot data[VTX_MEMO_N][16]; u32 idx[VTX_MEMO_N], gen[VTX_MEMO_N], g; } VtxMemo;
typedef struct { HANDLE th, ev; VtxMemo* memo; volatile LONG sleeping, go; u32 lo, hi; } VtxWorker;
static VtxWorker s_vmt[VMT_MAX];
static VtxMemo*  s_vmt_mainmemo = NULL;
static int  s_vmt_n = -1;
static u32  s_vmt_min = 768;
static double s_vmt_spin_us = 150.0;
static const rsx_state* s_vmt_st;
static const u32* s_vmt_list; static u32 s_vmt_first; static VPSlot* s_vmt_out;
static volatile LONG s_vmt_pending = 0, s_vmt_nconv = 0;
static u32* s_vlist = NULL; static u32 s_vlist_cap = 0;

static u32 vmt_range(VtxMemo* m, u32 lo, u32 hi)
{
    const rsx_state* st = s_vmt_st;
    VFAttr vfa[16]; memcpy(vfa, s_vfa, sizeof vfa);
    VPSlot* out = s_vmt_out; const u32* L = s_vmt_list; u32 f0 = s_vmt_first;
    u32 k = s_vpk; size_t nb = (size_t)k * sizeof(VPSlot); u32 nc = 0;
    int memo = m && L && vtx_memo_on() && !st->imm_active;
    if (memo && ++m->g == 0) { memset(m->gen, 0, sizeof m->gen); m->g = 1; }
    for (u32 j = lo; j < hi; j++) {
        u32 vi = L ? L[j] : f0 + j;
        VPSlot* d = out + (size_t)j * k;
        if (!memo) { read_vp_vertex_fast_p(st, vfa, vi, d); nc++; continue; }
        u32 h = (vi ^ (vi >> 12)) & (VTX_MEMO_N - 1);
        if (m->gen[h] != m->g || m->idx[h] != vi) {
            read_vp_vertex_fast_p(st, vfa, vi, m->data[h]);
            m->gen[h] = m->g; m->idx[h] = vi; nc++;
        }
        memcpy(d, m->data[h], nb);
    }
    return nc;
}
static DWORD WINAPI vmt_thread(LPVOID p)
{
    VtxWorker* w = (VtxWorker*)p;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONGLONG spin_ticks = (LONGLONG)(s_vmt_spin_us * (double)fq.QuadPart / 1e6);
    for (;;) {
        LARGE_INTEGER t0; QueryPerformanceCounter(&t0);
        int it = 0;
        while (!w->go) {
            YieldProcessor();
            if ((++it & 63) == 0) {
                LARGE_INTEGER t1; QueryPerformanceCounter(&t1);
                if (t1.QuadPart - t0.QuadPart > spin_ticks) {
                    InterlockedExchange(&w->sleeping, 1);
                    if (w->go) {
                        /* raced with a post: if the poster already cleared the flag it
                         * will SetEvent -- consume it to keep the event balanced. */
                        if (InterlockedExchange(&w->sleeping, 0) == 0) WaitForSingleObject(w->ev, INFINITE);
                    } else {
                        WaitForSingleObject(w->ev, INFINITE);
                    }
                    QueryPerformanceCounter(&t0); it = 0;
                }
            }
        }
        InterlockedExchange(&w->go, 0);   /* full barrier: range below is the posted one */
        u32 nc = vmt_range(w->memo, w->lo, w->hi);
        InterlockedExchangeAdd(&s_vmt_nconv, (LONG)nc);
        InterlockedDecrement(&s_vmt_pending);
    }
    return 0;
}
static int vmt_init(void)
{
    if (s_vmt_n >= 0) return s_vmt_n;
    int n = 4;
    { const char* e = getenv("VTX_MT"); if (e && *e) n = atoi(e); }
    { const char* e = getenv("VTX_MT_MIN"); if (e && *e) s_vmt_min = (u32)atoi(e); }
    { const char* e = getenv("VTX_MT_SPIN_US"); if (e && *e) s_vmt_spin_us = atof(e); }
    if (n < 0) n = 0; if (n > VMT_MAX) n = VMT_MAX;
    if (n > 0) {
        s_vmt_mainmemo = (VtxMemo*)calloc(1, sizeof(VtxMemo));
        if (!s_vmt_mainmemo) n = 0;
    }
    int made = 0;
    for (int i = 0; i < n; i++) {
        VtxWorker* w = &s_vmt[i];
        w->memo = (VtxMemo*)calloc(1, sizeof(VtxMemo));
        w->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!w->memo || !w->ev) break;
        w->th = CreateThread(NULL, 256 * 1024, vmt_thread, w, 0, NULL);
        if (!w->th) break;
        made++;
    }
    s_vmt_n = made;
    fprintf(stderr, "[VTX_MT] %d vertex workers, min %u verts/draw, spin %.0f us%c", made, s_vmt_min, s_vmt_spin_us, 10);
    return s_vmt_n;
}
/* Convert n output vertices: guest index list[j] (or first + j when list is NULL)
 * into out[j * s_vpk]. Serial fallback reproduces the old paths exactly. */
static void vp_convert_list(const rsx_state* st, const u32* list, u32 first, u32 n, VPSlot* out)
{
    if (s_vf_ok && !st->imm_active) {
        if (n < va_min()) {
            VFAttr vfa[16]; memcpy(vfa, s_vfa, sizeof vfa);
            for (u32 j = 0; j < n; j++)
                read_vp_vertex_fast_p(st, vfa, list ? list[j] : first + j, &out[(size_t)j * s_vpk]);
            s_va_inline_draws++; s_va_inline_verts += n; s_perf_vconv += n;
            return;
        }
        if (va_submit(list, first, n, out)) { s_perf_vconv += n; return; }   /* v33 */
    }
    int nw = vmt_init();
    if (nw <= 0 || n < s_vmt_min || !s_vf_ok) {
        if (list) {
            vp_memo_begin();
            for (u32 j = 0; j < n; j++) read_vp_vertex_memo(st, list[j], &out[(size_t)j * s_vpk]);
        } else {
            for (u32 j = 0; j < n; j++) read_vp_vertex(st, first + j, &out[(size_t)j * s_vpk]);
            s_perf_vconv += n;
        }
        return;
    }
    /* use fewer workers for mid-sized draws: ~512 verts per participant minimum */
    u32 parts = (u32)nw + 1;
    { u32 want = n / 512; if (want < 2) want = 2; if (want < parts) parts = want; }
    u32 chunk = (n + parts - 1) / parts;
    s_vmt_st = st; s_vmt_list = list; s_vmt_first = first; s_vmt_out = out;
    s_vmt_nconv = 0;
    LONG posted = 0;
    for (int i = 0; i < nw && (u32)(i + 1) < parts; i++) {
        u32 lo = (u32)(i + 1) * chunk, hi = lo + chunk;
        if (lo >= n) break;
        if (hi > n) hi = n;
        s_vmt[i].lo = lo; s_vmt[i].hi = hi;
        posted++;
    }
    s_vmt_pending = posted;
    MemoryBarrier();
    for (int i = 0; i < posted; i++) {
        InterlockedExchange(&s_vmt[i].go, 1);
        if (InterlockedExchange(&s_vmt[i].sleeping, 0)) SetEvent(s_vmt[i].ev);
    }
    u32 nc = vmt_range(s_vmt_mainmemo, 0, chunk < n ? chunk : n);
    while (s_vmt_pending > 0) YieldProcessor();
    MemoryBarrier();
    s_perf_vconv += nc + (u32)s_vmt_nconv;
    s_perf_vmt += n;
}
static u32* s_vtmp = NULL; static u32 s_vtmp_cap = 0;
static u32* vlist_tmp_get(u32 n)
{
    if (n > s_vtmp_cap) {
        u32 c = s_vtmp_cap ? s_vtmp_cap : 65536;
        while (c < n) c *= 2;
        u32* p = (u32*)realloc(s_vtmp, (size_t)c * sizeof(u32));
        if (!p) return NULL;
        s_vtmp = p; s_vtmp_cap = c;
    }
    return s_vtmp;
}
static u32* vlist_get(u32 n)
{
    if (n > s_vlist_cap) {
        u32 c = s_vlist_cap ? s_vlist_cap : 65536;
        while (c < n) c *= 2;
        u32* p = (u32*)realloc(s_vlist, (size_t)c * sizeof(u32));
        if (!p) return NULL;
        s_vlist = p; s_vlist_cap = c;
    }
    return s_vlist;
}
/* ---- v30d VTX_INDEXED: indexed VP draws ----------------------------------
 * The VP path expanded every indexed draw into a flat triangle list: each OUTPUT
 * vertex (3 per triangle) got all its packed attribute slots written to the
 * upload heap (~4.3M verts x ~170 B = ~730 MB/frame in the city, PERF vtx 55% of
 * the RSX thread, which ran at 92% busy). Now each draw converts each UNIQUE
 * guest index once into the VB window and writes a 4-byte index per output
 * vertex into a per-parity index upload buffer; render_frame issues
 * DrawIndexedInstanced. Same vertex values, same triangle order and winding as
 * the expanded path (strips/fans use the same t,t+1,t+2 expansion). Batches that
 * the expanded path concatenated (begin_epoch merge, out-of-phase 256-index
 * batches) concatenate here too, with the index bias = vertices already in the
 * merged record. Anything unusual (immediate mode, no fast fetch plan, IB full,
 * previous record not indexed) takes the old path. VTX_INDEXED=0 -> old path. */
static int vtx_indexed_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VTX_INDEXED"); v = (e && *e == '0') ? 0 : 1;
                 fprintf(stderr, "[VTX_INDEXED] indexed VP draws: %s%c", v ? "ON (v30d)" : "off", 10); }
    return v;
}
static int vib_ready(int par)
{
    if (s_ib_map[par]) return 1;
    if (s_ib_fail || !s_d3d.device) return 0;
    D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof(hp)); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof(bd));
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = VIB_BYTES;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RANGE nr = {0, 0};
    ID3D12Resource* r = NULL; void* m = NULL;
    if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp,
            D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
            &IID_ID3D12Resource, (void**)&r)) || !r) { s_ib_fail = 1; return 0; }
    if (FAILED(r->lpVtbl->Map(r, 0, &nr, &m)) || !m) { r->lpVtbl->Release(r); s_ib_fail = 1; return 0; }
    s_ib_res[par] = r; s_ib_map[par] = (unsigned char*)m;
    fprintf(stderr, "[VTX_INDEXED] index buffer parity %d: %u MB%c", par, VIB_BYTES >> 20, 10);
    return 1;
}
/* Dedupe table (open addressing, generation-stamped so it is never cleared). */
#define VIB_HT_MAX (1u << 20)
static u32 *s_vht_key, *s_vht_val, *s_vht_gen; static u32 s_vht_g = 0;
/* Convert the unique guest indices of L[0..n) into out (stride s_vpk slots) and
 * write ib[j] = bias + local index. Returns the unique count (0 = failed). */
/* v33 VTX_ASYNC=<workers> (default 3, 0 = off): the RSX thread was 90% busy and ~35% of
 * that was converting guest vertices into the upload heap. Conversions of the indexed
 * (vib_build) and plain (vp_convert_list) paths are now queued as 512-vertex jobs and run on
 * worker threads while the RSX thread keeps parsing the FIFO. Every point where the guest
 * could observe the RSX's progress or memory could change under the conversion SYNCS
 * first (drains the queue, helping with the work): render_frame, a VB reset, back-end
 * label / semaphore writes, NV406E semaphore release, set-reference, and every 2D
 * blit/transfer method (rsx_vtx_sync via g_rsx_vtx_sync). Immediate-mode draws and draws
 * without a fast-fetch plan stay synchronous. */
typedef struct { VFAttr vfa[16]; u32 vpk; } VAPlan;
typedef struct { u32 plan; const u32* list; u32 first, lo, hi; VPSlot* out; } VAJob;
#define VA_PLANS 16384
#define VA_JOBS  65536
#define VA_CHUNK 512
static VAPlan* s_va_plan = NULL;
static VAJob   s_va_job[VA_JOBS];
static volatile LONG s_va_sub = 0, s_va_claim = 0, s_va_done = 0, s_va_sleep = 0;
static u32 s_va_plan_n = 0;
static u32* s_va_arena = NULL; static u32 s_va_arena_used = 0;
#define VA_ARENA (8u << 20)   /* u32 entries */
static int s_va_n = -1;
static HANDLE s_va_ev = NULL;
static u64 s_va_syncs = 0, s_va_jobs_total = 0;
static int va_take(void)   /* claim + run one job; 0 if nothing claimable */
{
    LONG c;
    for (;;) {
        c = s_va_claim;
        if (c >= s_va_sub) return 0;
        if (InterlockedCompareExchange(&s_va_claim, c + 1, c) == c) break;
    }
    VAJob jl = s_va_job[(u32)c % VA_JOBS];   /* local copy: the slot may be reused later */
    const VAJob* j = &jl;
    VAPlan* pl = &s_va_plan[j->plan];
    VFAttr vfa[16]; memcpy(vfa, pl->vfa, sizeof vfa);   /* private page-cache copy */
    u32 k = pl->vpk;
    for (u32 q = j->lo; q < j->hi; q++)
        read_vp_vertex_fast_k(NULL, vfa, k, j->list ? j->list[q] : j->first + q, j->out + (size_t)q * k);
    InterlockedIncrement(&s_va_done);
    return 1;
}
/* v242B VTX_ASYNC_LEAN (default ON; 0 = off = old; VTX_ASYNC_LEAN_PHASE=0|1 = only in PS3_AB phases of that parity ->
 * in-run A/B). b981 (8 CPUs): +1.65 fps, -39 % process CPU; b982 (28 CPUs, spin only): +2.1 fps, 1 % / 0.1 % lows +4.4 /
 * +4.9, -30 % process CPU -> on for every CPU count. b931/b933: the 3 workers burn
 * 50 % (28 CPUs) / 26 % (8 CPUs) of a core, mostly in the 4000-PAUSE spin after every job (~12k jobs/s spread over the
 * frame, "0 waiting syncs" = the FIFO thread never waits for them). Lean: only VTX_ASYNC_LEAN_WORKERS workers take jobs
 * (default 1 at <= 4 CPUs, 2 at <= 11, else all; the others park in Sleep(10)) and the idle spin is
 * VTX_ASYNC_LEAN_SPIN pauses (default 200) before the event wait. Same jobs, same conversion code, same syncs (the RSX
 * thread still helps in rsx_vtx_sync) -> identical output. Off: every worker active, 4000 pauses = exactly the old loop. */
static volatile int s_va_active = 64, s_va_spin = 4000;
static volatile LONG s_va_idx = 0;
extern int ps3_host_ncpu(void);   /* ppu_loader.cpp */
static void va_lean_tick(void)
{
    static int s_mode = -2, s_phase = -2, s_wk = -1, s_sp = -1, s_last = -1;
    static ULONGLONG s_t = 0;
    if (s_mode == -2) {
        const char* e = getenv("VTX_ASYNC_LEAN"); s_mode = (e && *e) ? atoi(e) : -1;
        e = getenv("VTX_ASYNC_LEAN_PHASE"); s_phase = (e && *e) ? (atoi(e) & 1) : -1;
        e = getenv("VTX_ASYNC_LEAN_WORKERS"); s_wk = (e && *e) ? atoi(e) : -1;
        e = getenv("VTX_ASYNC_LEAN_SPIN"); s_sp = (e && *e) ? atoi(e) : 200; if (s_sp < 1) s_sp = 1;
    }
    const ULONGLONG now = GetTickCount64();
    if (s_last >= 0 && now - s_t < 250) return;
    s_t = now;
    extern volatile int g_ab_phase;
    const int ncpu = ps3_host_ncpu();
    int on = (s_mode == 0) ? 0 : 1;   /* v242B: default on (was: auto < 16 CPUs in b981/b982) */
    if (on && s_phase >= 0 && (g_ab_phase & 1) != s_phase) on = 0;
    int act = s_va_n > 0 ? s_va_n : 1, spin = 4000;
    if (on) {
        int w = s_wk > 0 ? s_wk : (ncpu <= 4 ? 1 : ncpu <= 11 ? 2 : act);
        if (w > act) w = act;
        if (w < 1) w = 1;
        act = w; spin = s_sp;
    }
    const int key = on ? (act << 16) | (spin & 0xFFFF) : 0;
    if (key != s_last) {
        s_va_active = act; s_va_spin = spin; s_last = key;
        fprintf(stderr, "[VTX_ASYNC] v242B lean %s: %d of %d workers take jobs, idle spin %d pauses (ncpu %d, ab phase %d)%c",
                on ? "ON" : "off", act, s_va_n, spin, ncpu, (int)g_ab_phase, 10);
    }
}
static DWORD WINAPI va_thread(LPVOID p)
{
    (void)p;
    const int idx = (int)InterlockedIncrement(&s_va_idx) - 1;   /* v242B */
    fprintf(stderr, "[THREADS] vertex worker hosttid=%lu%c", (unsigned long)GetCurrentThreadId(), 10);
    for (;;) {
        if (idx >= s_va_active) { Sleep(10); continue; }   /* v242B lean: parked worker */
        if (va_take()) { if (s_va_sleep > 0 && s_va_claim < s_va_sub) SetEvent(s_va_ev); continue; }
        int it = 0; const int spin = s_va_spin;
        while (s_va_claim >= s_va_sub && ++it < spin) YieldProcessor();
        if (s_va_claim < s_va_sub) continue;
        InterlockedIncrement(&s_va_sleep);
        if (s_va_claim >= s_va_sub) WaitForSingleObject(s_va_ev, 2);
        InterlockedDecrement(&s_va_sleep);
    }
    return 0;
}
static int va_on(void)
{
    if (s_va_n >= 0) return s_va_n;
    int n = 3; { const char* e = getenv("VTX_ASYNC"); if (e && *e) n = atoi(e); }
    if (n > 8) n = 8;
    if (n > 0) {
        s_va_plan = (VAPlan*)calloc(VA_PLANS, sizeof(VAPlan));
        s_va_arena = (u32*)malloc((size_t)VA_ARENA * sizeof(u32));
        s_va_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        if (!s_va_plan || !s_va_arena || !s_va_ev) n = 0;
    }
    int made = 0;
    for (int i = 0; i < n; i++) { HANDLE th = CreateThread(NULL, 256 * 1024, va_thread, NULL, 0, NULL); if (!th) break; CloseHandle(th); made++; }
    s_va_n = made;
    if (made > 0) { extern void (*g_rsx_vtx_sync)(void); g_rsx_vtx_sync = rsx_vtx_sync; }
    fprintf(stderr, "[VTX_ASYNC] %d vertex conversion workers%c", made, 10);
    if (made > 0) va_lean_tick();   /* v242B */
    return s_va_n;
}
/* Millions of tiny draws were each paying for a plan copy, an interlocked ring
 * enqueue and a later cross-core handoff.  Their average batch is only about
 * 100 vertices, and the smallest are cheaper to convert on the submitting RSX
 * thread while its fetch plan and guest bytes are hot in cache. */
static u32 va_min(void)
{
    static int n = -1;
    if (n < 0) {
        const char* e = getenv("VTX_ASYNC_MIN");
        n = e ? atoi(e) : 64;
        if (n < 0) n = 0;
        fprintf(stderr, "[VTX_ASYNC] batches under %d vertices stay inline (VTX_ASYNC_MIN)%c", n, 10);
    }
    return (u32)n;
}
/* Drain: everything queued so far is converted when this returns. RSX thread only. */
static void vcw_drain(void);   /* v192 VC_ASYNC, defined with the VCACHE code */
void rsx_vtx_sync(void)
{
    s_ww_epoch++;   /* v219 VC_WW: labels / references / 2D + transfer methods (our own guest writes) */
    vcw_drain();   /* v192: the VCACHE worker's queued lookups/conversions land first (same guarantee) */
    if (s_va_n <= 0) return;
    if (s_va_done != s_va_sub) {
        s_va_syncs++;
        while (s_va_done != s_va_sub) { if (!va_take()) YieldProcessor(); }
    }
    MemoryBarrier();
    s_va_plan_n = 0; s_va_arena_used = 0;
}
/* Queue n vertices (list[j] or first + j) -> out[j * vpk] with the current plan. */
static int va_submit(const u32* list, u32 first, u32 n, VPSlot* out)
{
    if (va_on() <= 0 || !n) return 0;
    { static u32 s_k = 0; if (((++s_k) & 255u) == 0) va_lean_tick(); }   /* v242B */
    if (n < va_min()) return 0;
    if (s_va_plan_n >= VA_PLANS || (u32)(s_va_sub - s_va_done) + n / VA_CHUNK + 2 >= VA_JOBS ||
        (list && s_va_arena_used + n > VA_ARENA)) rsx_vtx_sync();
    if (list && s_va_arena_used + n > VA_ARENA) return 0;   /* too big even when empty */
    u32 pi = s_va_plan_n++;
    memcpy(s_va_plan[pi].vfa, s_vfa, sizeof s_vfa); s_va_plan[pi].vpk = s_vpk;
    const u32* L = NULL;
    if (list) { u32* a = s_va_arena + s_va_arena_used; memcpy(a, list, (size_t)n * sizeof(u32)); s_va_arena_used += n; L = a; }
    for (u32 lo = 0; lo < n; lo += VA_CHUNK) {
        VAJob* j = &s_va_job[(u32)s_va_sub % VA_JOBS];
        j->plan = pi; j->list = L; j->first = first; j->lo = lo; j->hi = (lo + VA_CHUNK < n) ? lo + VA_CHUNK : n; j->out = out;
        MemoryBarrier();
        InterlockedIncrement(&s_va_sub);
        s_va_jobs_total++;
    }
    if (s_va_sleep > 0) SetEvent(s_va_ev);
    return 1;
}
static u32 s_va_uniq[1u << 20];
/* v46 VIB_RANGE (default 1; 0 = always hash): the per-index dedup hash below was the RSX
 * thread's biggest cost in the world (~2.6M indices/frame -> "vtx" ~30 ms/frame in PERF).
 * Most index lists address a compact range of their vertex array; when [min, max] holds at
 * most n vertices, convert that whole range on the workers and rebase the indices instead:
 * one min/max pass + one subtract per index on the RSX thread. Unreferenced vertices inside
 * the range are converted too (harmless: same array, never indexed). */
static int vib_range_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VIB_RANGE"); v = (e && *e == '0') ? 0 : 1; }
    return v;
}
static u32 vib_build(const rsx_state* st, const u32* L, u32 n, VPSlot* out, u32* ib, u32 bias)
{
    if (vib_range_on() && n && va_on() > 0 && s_vf_ok && !st->imm_active) {   /* v46 */
        u32 mn = 0xFFFFFFFFu, mx = 0;
        for (u32 j = 0; j < n; j++) { u32 v = L[j]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
        u32 span = mx - mn + 1u;
        if (mx >= mn && span <= n && span <= (1u << 20)) {
            if (span < va_min()) {
                VFAttr vfa[16]; memcpy(vfa, s_vfa, sizeof vfa);
                for (u32 q = 0; q < span; q++)
                    read_vp_vertex_fast_p(st, vfa, mn + q, &out[(size_t)q * s_vpk]);
                s_va_inline_draws++; s_va_inline_verts += span;
                for (u32 j = 0; j < n; j++) ib[j] = bias + (L[j] - mn);
                s_perf_vconv += span; s_vib_range_hits++;
                return span;
            }
            if (va_submit(NULL, mn, span, out)) {
                for (u32 j = 0; j < n; j++) ib[j] = bias + (L[j] - mn);
                s_perf_vconv += span; s_vib_range_hits++;
                return span;
            }
        }
    }
    s_vib_hash_hits++;
    if (!s_vht_key) {
        s_vht_key = (u32*)malloc(VIB_HT_MAX * sizeof(u32));
        s_vht_val = (u32*)malloc(VIB_HT_MAX * sizeof(u32));
        s_vht_gen = (u32*)calloc(VIB_HT_MAX, sizeof(u32));
        if (!s_vht_key || !s_vht_val || !s_vht_gen) return 0;
    }
    u32 sz = 1024; while (sz < 2 * n && sz < VIB_HT_MAX) sz <<= 1;
    if (sz < 2 * n) return 0;                       /* too big for the table: old path */
    if (++s_vht_g == 0) { memset(s_vht_gen, 0, VIB_HT_MAX * sizeof(u32)); s_vht_g = 1; }
    const u32 g = s_vht_g, mask = sz - 1;
    VFAttr vfa[16]; memcpy(vfa, s_vfa, sizeof vfa);
    int async = (va_on() > 0 && s_vf_ok && !st->imm_active && n <= (1u << 20));
    u32 u = 0;
    for (u32 j = 0; j < n; j++) {
        u32 vi = L[j];
        u32 h = (vi * 0x9E3779B1u) & mask;
        for (;;) {
            if (s_vht_gen[h] != g) {                /* new vertex */
                s_vht_gen[h] = g; s_vht_key[h] = vi; s_vht_val[h] = u;
                if (async) s_va_uniq[u] = vi;       /* v33: converted by the workers */
                else read_vp_vertex_fast_p(st, vfa, vi, &out[(size_t)u * s_vpk]);
                ib[j] = bias + u; u++;
                break;
            }
            if (s_vht_key[h] == vi) { ib[j] = bias + s_vht_val[h]; break; }
            h = (h + 1) & mask;
        }
    }
    if (async && u && !va_submit(s_va_uniq, 0, u, out))   /* queue full/too big: convert here */
        for (u32 q = 0; q < u; q++) read_vp_vertex_fast_p(st, vfa, s_va_uniq[q], &out[(size_t)q * s_vpk]);
    s_perf_vconv += u;
    return u;
}


/* Same semantics as read_guest_index (base resolved from index_array_offset,
 * linear from there) with the resolve hoisted out of the per-index loop. */
#define VCS_N 32768u   /* v219i (VC_SEQ, defined with vc_lookup) */
static int vc_seq_on(void);
static inline void vc_pf_lines(const u8* p, u32 lines);
static void read_guest_indices(const rsx_state* st, u32 first, u32 n, u32* dst)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    const u8* p = vm_base + cellGcmResolveLocated(local, st->index_array_offset);
    {   /* v219i: index-buffer look-ahead by frame order (see VC_SEQ) */
        static const u8* s_rq[VCS_N]; static u32 s_rk = 0, s_rf = 0xFFFFFFFFu;
        static u8 s_rql[VCS_N];   /* v221C IDX_PF: lines the batch read */
        static int ipf = -1;
        if (__builtin_expect(ipf < 0, 0)) { const char* e = getenv("IDX_PF"); ipf = (e && *e == '1') ? 1 : 0;
            fprintf(stderr, "[IDX_PF] v221C index look-ahead prefetches the whole index list (<= 16 lines, was 4): %s (IDX_PF=1 = on; ABX 0x100000 = off)%c", ipf ? "ON" : "off", 10); }
        if (vc_seq_on()) {
            if (s_rf != s_frame_no) { s_rf = s_frame_no; s_rk = 0; }
            const u32 k = s_rk++ & (VCS_N - 1);
            const u8* nx = s_rq[(k + 2) & (VCS_N - 1)];
            const int full = ipf && !(g_abx & 0x100000);
            if (nx) vc_pf_lines(nx, full && s_rql[(k + 2) & (VCS_N - 1)] ? s_rql[(k + 2) & (VCS_N - 1)] : 4u);
            const u32 isz = (((st->index_array_dma >> 4) & 0xF) == 1) ? 2u : 4u;
            s_rq[k] = p + (u64)first * isz;
            { const u64 a = (u64)(uintptr_t)s_rq[k], b = a + (u64)n * isz;   /* lines touched, incl. a misaligned start */
              const u64 ln = (b - (a & ~63ull) + 63u) >> 6; s_rql[k] = (u8)(ln > 16u ? 16u : ln); }
        }
    }
    /* v219h: explicit SSSE3/AVX2 (was a plain loop the compiler only vectorised with a matching PGO profile -- after an
     * edit of d3d12_draw_indexed the stale profile left it scalar: ~15 % of the FIFO thread's exe samples in b007). */
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        const u8* q = p + (u64)first * 2;
        u32 j = 0;
        const __m128i sw16 = _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
        for (; j + 8 <= n; j += 8) {
            const __m128i v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(q + (size_t)j * 2)), sw16);
#if defined(__AVX2__)
            _mm256_storeu_si256((__m256i*)(dst + j), _mm256_cvtepu16_epi32(v));
#else   /* v242B PORT_CPU_COMPAT (no AVX2): same 8 words, two SSE4.1 halves */
            _mm_storeu_si128((__m128i*)(dst + j), _mm_cvtepu16_epi32(v));
            _mm_storeu_si128((__m128i*)(dst + j + 4), _mm_cvtepu16_epi32(_mm_srli_si128(v, 8)));
#endif
        }
        for (; j < n; j++) dst[j] = ((u32)q[(size_t)j * 2] << 8) | q[(size_t)j * 2 + 1];
    } else {
        const u8* q = p + (u64)first * 4;
        u32 j = 0;
#if defined(__AVX2__)
        const __m256i sw32 = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                                              3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        for (; j + 8 <= n; j += 8)
            _mm256_storeu_si256((__m256i*)(dst + j), _mm256_shuffle_epi8(_mm256_loadu_si256((const __m256i*)(q + (size_t)j * 4)), sw32));
#else   /* v242B PORT_CPU_COMPAT (no AVX2) */
        const __m128i sw32 = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
        for (; j + 8 <= n; j += 8) {
            _mm_storeu_si128((__m128i*)(dst + j), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(q + (size_t)j * 4)), sw32));
            _mm_storeu_si128((__m128i*)(dst + j + 4), _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)(q + (size_t)j * 4 + 16)), sw32));
        }
#endif
        for (; j < n; j++) { u32 w; memcpy(&w, q + (size_t)j * 4, 4); dst[j] = __builtin_bswap32(w); }
    }
}

/* VTX_POS=<N>: print the first fetched position of the first N draws. A guest
 * vertex array that resolves to the wrong memory reads as garbage/denormals, and
 * that is indistinguishable from "the shader is wrong" without seeing the input. */
void rsx_vtx_pos_dbg(const rsx_state* state, const float* v, u32 n)
{
    static int cap = -1, seen = 0;
    if (cap < 0) { const char* e = getenv("VTX_POS"); cap = e ? atoi(e) : 0; }
    if (!cap || seen >= cap) return;
    seen++;
    const rsx_vertex_attrib* a = &state->vertex_attribs[0];
    fprintf(stderr, "[VTXPOS] a0 off=0x%X stride=%u size=%u type=%u -> (%g, %g, %g, %g)\n",
            a->offset, a->stride, a->size, a->type,
            v[0], v[1], v[2], v[3]);   /* all four: masking by `size` hid whether w survived */
}

static void vp_attrs_dbg(const rsx_state* state)
{
    if (!getenv("VP_ATTRS")) return;
    /* VP_ATTRS_FP=<hex shader_program>: dump only the draws that use one
     * fragment program. Without it the first six draws are whatever the frame
     * happens to start with, which is never the mesh being investigated. */
    { static const char* e = (const char*)1; static u32 want = 0;
      if (e == (const char*)1) { e = getenv("VP_ATTRS_FP");
          want = e ? (u32)strtoul(e, NULL, 0) : 0; }
      if (want && (!state || state->shader_program != want)) return; }
    /* Dedupe by the set of enabled attributes rather than printing the first
     * few draws: one fragment program can be used by meshes with different
     * vertex layouts, and a mesh missing an attribute takes the constant
     * register instead -- which is exactly the case worth seeing. */
    { u32 mask = 0;
      for (int i = 0; i < 16; i++) if (state->vertex_attribs[i].enabled) mask |= 1u << i;
      static u32 seen[16]; static int ns = 0;
      for (int i = 0; i < ns; i++) if (seen[i] == mask) return;
      if (ns >= 16) return;
      seen[ns++] = mask; }
    fprintf(stderr, "[VPATTR] --- fp=0x%X ---%c", state->shader_program, 10);
    fprintf(stderr, "[VPATTR] divider_op=0x%08X\n", state->frequency_divider_op);
    for (int i = 0; i < 16; i++) {
        const rsx_vertex_attrib* a = &state->vertex_attribs[i];
        if (a->enabled) fprintf(stderr, "[VPATTR] a%d off=0x%X stride=%u size=%u type=%u freq=%u fmt=0x%08X\n",
                                i, a->offset, a->stride, a->size, a->type, a->frequency, a->format);
    }
    /* VP_ATTRS_DUMP=<attr>:<n>: read n entries of that attribute straight out of
     * guest memory. Answers "did the guest write these values, or did our fetch
     * mangle them" without inferring it from the rendered image. */
    { const char* e = getenv("VP_ATTRS_DUMP"); if (!e) return;
      int ai = 0, cnt = 8; sscanf(e, "%d:%d", &ai, &cnt);
      const rsx_vertex_attrib* a = &state->vertex_attribs[ai];
      if (!a->enabled || !a->stride) return;
      extern uint8_t* vm_base;
      extern u32 cellGcmResolveLocated(int, u32);
      u32 nbad = 0, nzero = 0;
      for (int v = 0; v < cnt; v++) {
          u32 aoff = (a->offset & 0x7FFFFFFFu) + (u32)v * a->stride;
          const u8* q = vm_base + ((a->offset & 0x80000000u)
                        ? cellGcmResolveLocated(0, aoff) : cellGcmResolveLocated(1, aoff));
          /* Also read the same offset through the OTHER context-DMA. An array
           * the guest put in main memory but whose offset we resolve as LOCAL
           * reads as untouched VRAM, i.e. all zeros -- indistinguishable from
           * "the producer never ran". */
          if (v == 0) {
              u32 la = cellGcmResolveLocated(1, aoff), ma = cellGcmResolveLocated(0, aoff);
              const u8* lp = vm_base + la; const u8* mp = vm_base + ma;
              const u8* rp = vm_base + (a->offset & 0x7FFFFFFFu);   /* raw, unresolved */
              fprintf(stderr, "[VPDUMP] a%d off=0x%X  LOCAL@0x%X=(%g,%g,%g)  MAIN@0x%X=(%g,%g,%g)"
                              "  RAW@0x%X=(%g,%g,%g)%c",
                      ai, aoff, la, rsx_rd_bef(lp), rsx_rd_bef(lp+4), rsx_rd_bef(lp+8),
                      ma, rsx_rd_bef(mp), rsx_rd_bef(mp+4), rsx_rd_bef(mp+8),
                      (a->offset & 0x7FFFFFFFu), rsx_rd_bef(rp), rsx_rd_bef(rp+4), rsx_rd_bef(rp+8), 10);
          }
          u32 nc = a->size ? a->size : 3; if (nc > 3) nc = 3;
          float f[3] = {0,0,0};
          for (u32 k = 0; k < nc; k++) f[k] = rsx_rd_bef(q + k * 4);
          if (v < 12)
              fprintf(stderr, "[VPDUMP] a%d[%d] = (%g, %g, %g)%c", ai, v, f[0], f[1], f[2], 10);
          int allz = 1; for (u32 k = 0; k < nc; k++) if (f[k] != 0.f) allz = 0;
          if (allz) nzero++;
          else if (f[2] < -0.9f && f[0] > -0.1f && f[0] < 0.1f) nbad++;
      }
      fprintf(stderr, "[VPDUMP] a%d over %d verts: %u zero, %u near (0,0,-1)%c",
              ai, cnt, nzero, nbad, 10);
      /* How many entries differ from the first? An all-identical position array
       * is a degenerate mesh: it rasterizes nothing, which looks the same as
       * "the draw never happened". */
      { float f0[3]; u32 o0 = (a->offset & 0x7FFFFFFFu);
        const u8* q0 = vm_base + ((a->offset & 0x80000000u)
                       ? cellGcmResolveLocated(0, o0) : cellGcmResolveLocated(1, o0));
        for (int k = 0; k < 3; k++) f0[k] = rsx_rd_bef(q0 + k * 4);
        u32 diff = 0;
        for (int v = 1; v < cnt; v++) {
            u32 ao = o0 + (u32)v * a->stride;
            const u8* q = vm_base + ((a->offset & 0x80000000u)
                          ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
            for (int k = 0; k < 3; k++)
                if (rsx_rd_bef(q + k * 4) != f0[k]) { diff++; break; }
        }
        fprintf(stderr, "[VPDUMP] a%d: %u/%d entries differ from entry 0 (%g,%g,%g)%c",
                ai, diff, cnt, f0[0], f0[1], f0[2], 10);
        /* Extent: an isosurface with little fluid is a speck, and a speck that
         * rasterizes nothing is correct, not a bug. */
        { float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (f < lo[k]) lo[k] = f; if (f > hi[k]) hi[k] = f; } }
          u32 nnan = 0, nfin = 0;
          for (int v = 0; v < cnt; v++) {
              u32 ao = o0 + (u32)v * a->stride;
              const u8* q = vm_base + ((a->offset & 0x80000000u)
                            ? cellGcmResolveLocated(0, ao) : cellGcmResolveLocated(1, ao));
              int bad = 0;
              for (int k = 0; k < 3; k++) { float f = rsx_rd_bef(q + k * 4);
                  if (!(f == f) || f > 1e30f || f < -1e30f) bad = 1; }
              if (bad) nnan++; else nfin++;
          }
          fprintf(stderr, "[VPDUMP] a%d extent x[%g..%g] y[%g..%g] z[%g..%g]"
                          "  finite=%u nan/inf=%u%c",
                  ai, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], nfin, nnan, 10); } } }
}

static u32 upload_quads_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    vp_attrs_dbg(state);
    u32 quads = count / 4;
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex(state, first + q*4 + k, c[k]);
        /* quad -> two triangles (perimeter winding) */
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*s_vpk], c[idx[t]], s_vpk * sizeof(VPSlot)); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if ((_n < 4 && _n++ < 4)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) { for (u32 k = 0; k < 4; k++)
                fprintf(f, "q%02d v%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", _n, k,
                    c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                    c[k][8].v[0],c[k][8].v[1]);
              fclose(f); } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Straight triangle-list upload through the VP path (gcm/cube's cube draws
 * TRIANGLES, prim 5 -- no expansion needed). */
static u32 upload_tris_vp(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    vp_attrs_dbg(state);
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && (_n < 12 && _n++ < 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      vp_convert_list(state, NULL, first, count, out);
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: vertex range for that program's draws. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if ((_n < 12 && _n++ < 12))
          fprintf(stderr, "[IDXDBG] tris first=%u count=%u a0off=0x%X stride=%u%c",
                  first, count, state->vertex_attribs[0].offset,
                  state->vertex_attribs[0].stride, 10); } }
    /* RSX_DBG_VTX=<hex tex0 offset>: dump attribute-0 positions for the draws that
     * bind that texture. The duck's texture resolves and its 9960 draws all
     * target the backbuffer, yet none of its texels reach the screen -- so the
     * question is whether its vertices are where they should be. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 6) { n++;
        fprintf(stderr, "[DUCKVTX] first=%u count=%u", first, count);
        for (u32 k = 0; k < count && k < 4; k++)
            fprintf(stderr, "  v%u=(%.2f,%.2f,%.2f)", k,
                    out[k*s_vpk].v[0], out[k*s_vpk].v[1], out[k*s_vpk].v[2]);
        { int lo = -1, hi = -1, nz = 0;
          for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
              const float* m = state->vertex_constants[r];
              if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
          }
          fprintf(stderr, "  | constants: %d non-zero, slots %d..%d", nz, lo, hi); }
        fprintf(stderr, "%c", 10);
      } }
    if (getenv("VTX_DUMP") && state->surface_color_offset[0] == 0xCC0000) {
        static int _n=0; if ((_n < 1 && _n++ < 1)) {
        FILE* f = fopen("vtx_dump.txt", "w");
        if (f) { fprintf(f, "G-buffer draw surf0=0x%X count=%u first=%u\n",
                         state->surface_color_offset[0], count, first);
          for (u32 k = 0; k < count && k < 30; k++)
            fprintf(f, "v%02u a0=(%.3f,%.3f,%.3f) a2=(%.3f,%.3f,%.3f) a9=(%.4f,%.4f,%.4f,%.4f)\n", k,
                out[k*s_vpk].v[0],out[k*s_vpk].v[1],out[k*s_vpk].v[2],
                out[k*s_vpk+2].v[0],out[k*s_vpk+2].v[1],out[k*s_vpk+2].v[2],
                out[k*s_vpk+9].v[0],out[k*s_vpk+9].v[1],out[k*s_vpk+9].v[2],out[k*s_vpk+9].v[3]);
          fclose(f); } } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Non-indexed TRIANGLE_STRIP (fan=0) / TRIANGLE_FAN (fan=1) through the VP
 * path, expanded to a triangle list. LBP's Bink movie draws its YUV quad as a
 * non-indexed 4-vertex strip (prim 6); without this it fell to the fixed-
 * function fallback with is_vp=0 (no vertex-program transform -> the quad
 * collapsed to a line) and textured=0 (the Y/U/V planes never sampled). */
static u32 upload_strip_vp(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    u32 o = 0;
    vp_memo_begin();
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        /* strip winding alternates per triangle to keep facing consistent */
        u32 i1 = t + 1, i2 = t + 2;
        if (!fan && (t & 1)) { u32 tmp = i1; i1 = i2; i2 = tmp; }
        read_vp_vertex_memo(state, first + i0, &out[o*s_vpk]); o++;
        read_vp_vertex_memo(state, first + i1, &out[o*s_vpk]); o++;
        read_vp_vertex_memo(state, first + i2, &out[o*s_vpk]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Fetch index k from the guest index array (SET_INDEX_ARRAY_ADDRESS/_DMA:
 * dma [3:0] = location (0 local, 1 main), [7:4] = type (0 u32, 1 u16)).
 * Indices are big-endian in guest memory. */
static u32 read_guest_index(const rsx_state* st, u32 k)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    int local = ((st->index_array_dma & 0xF) == 0);
    u32 base = cellGcmResolveLocated(local, st->index_array_offset);
    const u8* p = vm_base + base;
    if (((st->index_array_dma >> 4) & 0xF) == 1) {
        p += (u64)k * 2;
        return ((u32)p[0] << 8) | p[1];
    }
    p += (u64)k * 4;
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Indexed variants of the VP-path uploads. */
static u32 upload_quads_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    u32 quads = count / 4;
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (quads * 6 > maxv) quads = maxv / 6;
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    u32 o = 0;
    for (u32 q = 0; q < quads; q++) {
        VPSlot c[4][16];
        if (q == 0) vp_memo_begin();
        for (u32 k = 0; k < 4; k++)
            read_vp_vertex_memo(state, read_guest_index(state, first + q*4 + k), c[k]);
        static const int idx[6] = {0,1,2, 0,2,3};
        for (int t = 0; t < 6; t++) { memcpy(&out[o*s_vpk], c[idx[t]], s_vpk * sizeof(VPSlot)); o++; }
        if (getenv("VTX_DUMP")) { static int _n=0; if ((_n < 6 && _n++ < 6)) {
            FILE* f = fopen("vtx_dump.txt", _n==1 ? "w" : "a");
            if (f) {
                const float* vs_ = state->viewport_scale;
                const float* vo_ = state->viewport_offset;
                fprintf(f, "iq%02d vp_scale=(%.1f,%.1f,%.3f) vp_off=(%.1f,%.1f,%.3f)\n", _n,
                        vs_[0], vs_[1], vs_[2], vo_[0], vo_[1], vo_[2]);
                for (int _c = 464; _c <= 467; _c++)
                    fprintf(f, "  c[%d]=(%.3f,%.3f,%.3f,%.3f)\n", _c,
                            state->vertex_constants[_c][0], state->vertex_constants[_c][1],
                            state->vertex_constants[_c][2], state->vertex_constants[_c][3]);
                for (u32 k = 0; k < 4; k++)
                    fprintf(f, "  v%u i%u a0=(%.3f,%.3f,%.3f,%.3f) a8=(%.3f,%.3f)\n", k,
                        read_guest_index(state, first + q*4 + k),
                        c[k][0].v[0],c[k][0].v[1],c[k][0].v[2],c[k][0].v[3],
                        c[k][8].v[0],c[k][8].v[1]);
                fclose(f);
            } } }
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

static u32 upload_tris_vp_indexed(const rsx_state* state, u32 first, u32 count)
{
    extern uint8_t* vm_base;
    s_req_verts += count; s_req_draws++;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    vp_attrs_dbg(state);
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (count > maxv) { s_drop_draws++;
        { static int _n = 0; if (getenv("VBFULL") && (_n < 12 && _n++ < 12))
            fprintf(stderr, "[VBFULL] fp=0x%X wanted %u verts, room for %u%c",
                    state->shader_program, count, maxv, 10); }
        count = maxv - (maxv % 3); }
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    { double _tv = perf_on() ? perf_now() : 0.0;
      u32* L = vlist_get(count);
      if (L) { read_guest_indices(state, first, count, L); vp_convert_list(state, L, 0, count, out); }
      else {
      vp_memo_begin();
      for (u32 k = 0; k < count; k++)
          read_vp_vertex_memo(state, read_guest_index(state, first + k), &out[k*s_vpk]);
      }
      if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += count; } }
    /* IDXDBG=<hex shader_program>: index range for that program's draws. An
     * index past the vertex array reads unmapped guest memory as zero, which
     * shows up as a spike triangle with a zero texcoord, not as a missing draw. */
    { static const char* ie = (const char*)1; static u32 iw = 0;
      if (ie == (const char*)1) { ie = getenv("IDXDBG");
          iw = ie ? (u32)strtoul(ie, NULL, 16) : 0; }
      if (iw && state->shader_program == iw) { static int _n = 0; if ((_n < 12 && _n++ < 12)) {
          u32 lo = 0xFFFFFFFFu, hi = 0;
          for (u32 k = 0; k < count; k++) { u32 ix = read_guest_index(state, first + k);
              if (ix < lo) lo = ix; if (ix > hi) hi = ix; }
          fprintf(stderr, "[IDXDBG] first=%u count=%u idx=[%u..%u] dma=0x%X a0off=0x%X stride=%u%c",
                  first, count, lo, hi, state->index_array_dma,
                  state->vertex_attribs[0].offset, state->vertex_attribs[0].stride, 10); } } }
    /* DBG_LOCK tracking: projected centroid of this mesh, updated every draw. */
    { static const char* dl = (const char*)1; static u32 wantl = 0; static int mvpb = 256;
      if (dl == (const char*)1) { dl = getenv("RSX_DBG_VTX");
                                  wantl = dl ? (u32)strtoul(dl, NULL, 16) : 0;
                                  const char* mb = getenv("MVP_BASE");
                                  if (mb) mvpb = atoi(mb); }
      /* RSX_DBG_TRACK=1 tracks the content-identified duck without needing its
       * offset passed in (VRAM offsets move between runs). Re-read every call:
       * s_duck_raw is filled by the texture upload, which happens AFTER the
       * first draws, so caching it once left the tracker watching offset 0. */
      { static int dtrk = -1;
        if (dtrk < 0) { const char* e = getenv("RSX_DBG_TRACK"); dtrk = e ? atoi(e) : 0; }
        if (dtrk && s_duck_raw) wantl = s_duck_raw; }
      /* RSX_DBG_PICK=<first>: track ONLY the draw with that starting index. The
       * mesh is one big vertex buffer sliced into 256-index chunks, so averaging
       * across all of them converges on the centre of the whole field -- which is
       * where there is nothing in particular. A single slice is a stable target
       * and can be magnified without drifting off it. */
      static int pick = -1;
      if (pick < 0) { const char* e = getenv("RSX_DBG_PICK"); pick = e ? atoi(e) : -2; }
      if (wantl && s_d3d.cur_texs[0].raw == wantl && count
          && (pick == -2 || (int)first == pick)) {
          double sx = 0, sy = 0, sz = 0;
          for (u32 k = 0; k < count; k++) {
              sx += out[k*s_vpk].v[0]; sy += out[k*s_vpk].v[1]; sz += out[k*s_vpk].v[2];
          }
          float v[4] = { (float)(sx/count), (float)(sy/count), (float)(sz/count), 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[mvpb + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          if (clip[3] > 1e-6f) {
              float nx = clip[0]/clip[3], ny = clip[1]/clip[3];
              if (nx > -2.0f && nx < 2.0f && ny > -2.0f && ny < 2.0f) {
                  s_lock_sx += nx; s_lock_sy += ny; s_lock_n++;
                  if (!s_lock_valid) {        /* seed so frame 1 is usable */
                      s_lock_x = nx; s_lock_y = ny; s_lock_valid = 1;
                  }
              }
          }
      } }
    /* RSX_DBG_SCALE=<f>: multiply attribute-0 positions for the draws binding
     * RSX_DBG_VTX's texture. The duck's object-space bbox is ~0.2 units and its
     * draws rasterize only a few dozen pixels per frame -- sub-pixel. Growing it
     * about its own origin tests the rest of the chain (index buffer, texture,
     * per-draw transform): if a duck-shaped, duck-coloured object appears, only
     * the scale is wrong. Diagnostic. */
    { static const char* ds = (const char*)1; static float sc = 0.0f;
      if (ds == (const char*)1) { ds = getenv("RSX_DBG_SCALE");
                                  sc = ds ? (float)atof(ds) : 0.0f; }
      static const char* dv2 = (const char*)1; static u32 want2 = 0;
      if (dv2 == (const char*)1) { dv2 = getenv("RSX_DBG_VTX");
                                   want2 = dv2 ? (u32)strtoul(dv2, NULL, 16) : 0; }
      if (sc > 0.0f && want2 && s_d3d.cur_texs[0].raw == want2 && count) {
        /* Magnify about a FIXED point captured from the first tracked draw, not
         * about each chunk's own centre -- recentring per chunk would stack every
         * chunk of the mesh on top of the others. Combined with VP_BYPASS (which
         * maps attribute 0 straight to clip space) this renders the mesh's real
         * geometry and texture at a readable size. */
        /* RSX_DBG_RECENTER=1: recentre EVERY chunk on its own centroid, overlaying
         * them. If each chunk is one instance of the same mesh they align into a
         * single silhouette; if they are arbitrary slices of one big mesh they
         * smear. Either way the answer is visible at a glance. */
        static int recen = -1;
        if (recen < 0) { const char* e = getenv("RSX_DBG_RECENTER"); recen = e ? atoi(e) : 0; }
        static int have_c = 0; static float ox = 0, oy = 0, oz = 0;
        if (!have_c || recen) {
            double sx = 0, sy = 0, sz = 0;
            for (u32 k = 0; k < count; k++) {
                sx += out[k*s_vpk].v[0]; sy += out[k*s_vpk].v[1]; sz += out[k*s_vpk].v[2];
            }
            ox = (float)(sx/count); oy = (float)(sy/count); oz = (float)(sz/count);
            have_c = 1;
        }
        for (u32 k = 0; k < count; k++) {
            /* RSX_DBG_SHIFT="dx,dy": nudge after scaling. VP_BYPASS applies the
             * guest's posoffset, so a recentred mesh does not land at screen
             * centre -- without this the magnified geometry sits clipped against
             * the top edge. */
            static int shf = -1; static float dx = 0.0f, dy = 0.0f;
            if (shf < 0) { const char* e = getenv("RSX_DBG_SHIFT");
                           if (e) { double a=0,b=0; sscanf(e, "%lf,%lf", &a, &b);
                                    dx = (float)a; dy = (float)b; }
                           shf = 1; }
            out[k*s_vpk].v[0] = (out[k*s_vpk].v[0] - ox) * sc + dx;
            out[k*s_vpk].v[1] = (out[k*s_vpk].v[1] - oy) * sc + dy;
            /* Leave z alone: under VP_BYPASS the object z maps straight to clip
             * z, so scaling it walks the geometry out of the depth range and
             * everything clips away. */
            (void)oz;
        }
      } }
    /* RSX_DBG_VTX=<hex tex0 offset>: attribute-0 positions for the draws binding
     * that texture (see upload_tris_vp). The duck's meshes are indexed, so this
     * is the copy that actually fires for it. */
    { static const char* dv = (const char*)1; static u32 want = 0; static int n = 0;
      if (dv == (const char*)1) { dv = getenv("RSX_DBG_VTX");
                                  want = dv ? (u32)strtoul(dv, NULL, 16) : 0; }
      if (want && s_d3d.cur_texs[0].raw == want && n < 8) { n++;
        float mnx=1e30f,mny=1e30f,mnz=1e30f,mxx=-1e30f,mxy=-1e30f,mxz=-1e30f;
        for (u32 k = 0; k < count; k++) {
            float x=out[k*s_vpk].v[0], y=out[k*s_vpk].v[1], z=out[k*s_vpk].v[2];
            if (x<mnx)mnx=x; if (y<mny)mny=y; if (z<mnz)mnz=z;
            if (x>mxx)mxx=x; if (y>mxy)mxy=y; if (z>mxz)mxz=z;
        }
        const rsx_vertex_attrib* _a3 = &state->vertex_attribs[3];
        fprintf(stderr, "[DUCKVTX] a3(en=%d type=%u size=%u stride=%u off=0x%08X freq=%u)"
                        " -> col0 v0=(%.3f %.3f %.3f %.3f)%c",
                _a3->enabled, _a3->type, _a3->size, _a3->stride, _a3->offset,
                _a3->frequency,
                out[3].v[0], out[3].v[1], out[3].v[2], out[3].v[3], 10);
        const rsx_vertex_attrib* _a0 = &state->vertex_attribs[0];
        fprintf(stderr, "[DUCKVTX] first=%u count=%u a0(type=%u size=%u stride=%u off=0x%08X)"
                        " obj-bbox x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]%c",
                first, count, _a0->type, _a0->size, _a0->stride, _a0->offset,
                mnx,mxx, mny,mxy, mnz,mxz, 10);
        /* Project the object-space bbox corners through the first four transform
         * constants (the conventional MVP rows) and report NDC. Sub-pixel output
         * with correct vertices means the placement transform is the problem, and
         * the NDC extent says by how much. */
        { float cx=(mnx+mxx)*0.5f, cy=(mny+mxy)*0.5f, cz=(mnz+mxz)*0.5f;
          /* This title's Cg programs keep their MVP at c[256..259], not c[0..3]
           * (see the vp_uses_c03 note); projecting through c0 reported zeros. */
          static int MVP = -1;
          if (MVP < 0) { const char* e = getenv("MVP_BASE"); MVP = e ? atoi(e) : 256; }
          float v[4] = { cx, cy, cz, 1.0f };
          float clip[4];
          for (int r = 0; r < 4; r++) {
              const float* m = state->vertex_constants[MVP + r];
              clip[r] = m[0]*v[0] + m[1]*v[1] + m[2]*v[2] + m[3]*v[3];
          }
          float ex[4];
          { float e[4] = { mxx-cx, mxy-cy, mxz-cz, 0.0f };
            for (int r = 0; r < 4; r++) {
                const float* m = state->vertex_constants[MVP + r];
                ex[r] = m[0]*e[0] + m[1]*e[1] + m[2]*e[2];
            } }
          { int lo = -1, hi = -1, nz = 0;
            for (int r = 0; r < RSX_MAX_VERTEX_CONSTANTS; r++) {
                const float* m = state->vertex_constants[r];
                if (m[0] || m[1] || m[2] || m[3]) { if (lo < 0) lo = r; hi = r; nz++; }
            }
            fprintf(stderr, "[DUCKVTX]   constants: %d non-zero, slots %d..%d;", nz, lo, hi);
            for (int r = (lo < 0 ? 0 : lo); r < (lo < 0 ? 0 : lo) + 4 && r < RSX_MAX_VERTEX_CONSTANTS; r++)
                fprintf(stderr, " c%d=(%.3f %.3f %.3f %.3f)", r,
                        state->vertex_constants[r][0], state->vertex_constants[r][1],
                        state->vertex_constants[r][2], state->vertex_constants[r][3]);
            fprintf(stderr, "%c", 10); }
          fprintf(stderr, "[DUCKVTX]   mvp0=(%.3f %.3f %.3f %.3f) clip=(%.3f %.3f %.3f w=%.3f)",
                  state->vertex_constants[MVP][0], state->vertex_constants[MVP][1],
                  state->vertex_constants[MVP][2], state->vertex_constants[MVP][3],
                  clip[0], clip[1], clip[2], clip[3]);
          if (clip[3] > 1e-6f)
              fprintf(stderr, " ndc=(%.4f %.4f) half-extent-px=(%.2f %.2f)",
                      clip[0]/clip[3], clip[1]/clip[3],
                      ex[0]/clip[3]*640.0f, ex[1]/clip[3]*360.0f);
          fprintf(stderr, "%c", 10); }
      } }
    /* IDX_DBG=<N>: the first indices and the position they resolve to. An index
     * buffer read with the wrong element size or base yields huge indices and
     * degenerate triangles -- an INDEXED mesh vanishes while non-indexed
     * geometry in the same scene renders fine. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("IDX_DBG"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && count >= 3) { n++;
        extern u32 cellGcmResolveOffset(u32);
        const rsx_vertex_attrib* a0 = &state->vertex_attribs[0];
        u32 vbase = cellGcmResolveOffset(a0->offset & 0x7FFFFFFFu);
        u32 nz = 0; for (u32 q = 0; q < 0x1000u; q += 29) if (vm_base[vbase + q]) nz++;
        fprintf(stderr, "[IDX] idxoff=0x%X idx: %u %u %u | a0 off=0x%08X(->0x%08X nz=%u/142)"
                        " stride=%u size=%u type=%u  pos0=(%g %g %g)%c",
                state->index_array_offset,
                read_guest_index(state, first), read_guest_index(state, first+1),
                read_guest_index(state, first+2),
                a0->offset, vbase, nz, a0->stride, a0->size, a0->type,
                out[0].v[0], out[0].v[1], out[0].v[2], 10); } }
    s_d3d.vp_vb_offset += count * VP_VERT_STRIDE;
    return count;
}

/* Indexed TRIANGLE_STRIP / TRIANGLE_FAN, expanded CPU-side into a triangle
 * list (the whole VP path draws lists; CullMode is NONE so strip parity
 * winding doesn't matter). DeferredShading's spline tube meshes -- the
 * octopus tentacles -- are indexed strips. */
static u32 upload_strip_vp_indexed(const rsx_state* state, u32 first, u32 count, int fan)
{
    extern uint8_t* vm_base;
    if (!state || !vm_base || !s_d3d.vp_vb_mapped) return 0;
    if (!state->vertex_attribs[0].enabled && !state->imm_active) return 0;   /* immediate-mode draws (rsx_state.imm_verts) disable every array on purpose */
    vf_plan(state);   /* per-draw vertex fetch plan (VTX_FASTFETCH) */
    if (count < 3) return 0;
    u32 tris = count - 2;
    u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
    if (tris * 3 > maxv) tris = maxv / 3;
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    u32 o = 0;
    { u32* L = vlist_get(tris * 3 + 1); u32* G = L ? vlist_tmp_get(count) : NULL;
      if (L && G) {
          read_guest_indices(state, first, count, G);
          for (u32 t = 0; t < tris; t++) {
              L[o++] = G[fan ? 0 : t]; L[o++] = G[t + 1]; L[o++] = G[t + 2];
          }
          double _tv = perf_on() ? perf_now() : 0.0;
          vp_convert_list(state, L, 0, o, out);
          if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += o; }
          s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
          return o;
      } }
    vp_memo_begin();
    for (u32 t = 0; t < tris; t++) {
        u32 i0 = fan ? 0 : t;
        read_vp_vertex_memo(state, read_guest_index(state, first + i0),    &out[o*s_vpk]); o++;
        read_vp_vertex_memo(state, read_guest_index(state, first + t + 1), &out[o*s_vpk]); o++;
        read_vp_vertex_memo(state, read_guest_index(state, first + t + 2), &out[o*s_vpk]); o++;
    }
    s_d3d.vp_vb_offset += o * VP_VERT_STRIDE;
    return o;
}

/* Depth surface of the op being recorded: the RSX zeta offset and the size of the
 * colour surface it pairs with (display draws: the window). */
static void rec_zeta(D3D12DrawRecord* dr)
{
    const rsx_state* st = s_d3d.current_rsx_state;
    /* v37 CT_NONE (default 1): SET_SURFACE_COLOR_TARGET = 0 (CELL_GCM_SURFACE_TARGET_NONE) means the
     * draw writes depth/stencil only. current_rt_off() still picks surface A for it, so such a pass
     * painted its colour into whatever surface A last was. CT_NONE=0 -> old behaviour. */
    dr->ct = st ? st->color_target : 1u;
    { static int on = -1; if (on < 0) { const char* e = getenv("CT_NONE"); on = (e && *e == '0') ? 0 : 1; }
      if (on && st && st->color_target == 0 && !dr->is_clear) {
          dr->cmask = 0;
          static int n = 0; if ((n < 4 && n++ < 4))
              fprintf(stderr, "[CT] colour target NONE: draw writes no colour (fp 0x%X, surface A 0x%X)%c",
                      st->shader_program, st->surface_color_offset[0], 10); } }
    dr->zeta_off = st ? st->surface_zeta_offset : 0;
    /* Titles point the zeta at a COLOUR surface (or a display buffer) for passes
     * that do not use depth (inFamous: zeta = 0x1AB8000/0x1E3C000/0x21C0000/
     * 0x1734000). Treating that as a depth surface made later reads of the
     * colour target return converted depth -> neon G-buffer garbage. */
    if (st && dr->zeta_off) {
        extern int cellGcmOffsetIsDisplay(u32 offset);
        u32 z = dr->zeta_off;
        int sel = (st->color_target == 2) ? 1 : 0;
        int alias = (z == st->surface_color_offset[sel] && st->color_target != 0) || cellGcmOffsetIsDisplay(z);
        if (st->color_target >= 0x13 && z == st->surface_color_offset[1]) alias = 1;
        if (st->color_target >= 0x17 && z == st->surface_color_offset[2]) alias = 1;
        if (st->color_target >= 0x1F && z == st->surface_color_offset[3]) alias = 1;
        if (alias) dr->zeta_off = 0;
    }
    dr->zw = dr->rt_off ? dr->rt_w : s_d3d.width;
    dr->zh = dr->rt_off ? dr->rt_h : s_d3d.height;
    if (!dr->zw || !dr->zh) { dr->zw = s_d3d.width; dr->zh = s_d3d.height; }
    dr->zsamp_zs = -1; dr->zsamp_rt = -1;
    dr->shkey = 0; dr->shz[0] = dr->shz[1] = dr->shz[2] = dr->shz[3] = -1;   /* v34 */
    dr->disp_off = 0;
    if (st && dr->rt_off == 0) {
        int sel = (st->color_target == 2) ? 1 : 0;
        dr->disp_off = st->surface_color_offset[sel];
    }
}

static u32 phase_fix_prepare(u32 first, const char* path);   /* v172, below */
static void d3d12_draw_arrays(void* ud, u32 primitive, u32 first, u32 count)
{
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] arrays callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    /* Log the first 20 calls in detail, then every 1000th to show liveness
     * without flooding. */
    static u64 s_total = 0;
    static int s_fast = -1;
    if (s_fast < 0) { const char* e = getenv("PS3_FAST_RUNTIME"); s_fast = e && e[0] && e[0] != '0'; }
    if (!s_fast && (s_total < 20 || (s_total % 1000) == 0)) {
        printf("[D3D12] draw_arrays #%llu prim=%u first=%u count=%u\n",
               (unsigned long long)s_total, primitive, first, count);
    }
    if (!s_fast) s_total++;

    if (!s_d3d.pipeline_ready || !s_d3d.vb_mapped) return;
    if (count == 0 || count > s_vp_max_verts) return;
    if (shadow_skip_draw(s_d3d.current_rsx_state)) return;   /* v30e */
    frame_split_check();   /* v103A: before anything reserves vertex space or a record */

    /* QUADS (prim 8) have no D3D12 topology; expand to a triangle list.
     * dbgfont draws all text as quads. Prefer the real vertex-program path
     * (exact position + texcoord): upload raw float4 attrib0 into vp_vb and
     * mark the draw is_vp; render_frame compiles the VP and draws it. Fall
     * back to the frac-approximation textured path if VP resources are absent. */
    if (primitive == RSX_PRIMITIVE_QUADS) {
        /* Prefer the real vertex-program path whenever VP resources exist --
         * NOT only when a texture is bound. Requiring tex_bound routed vkcube's
         * UNtextured cube to the fixed-function fallback, which never applies the
         * VP's MVP transform, so the cube was drawn in object space and clipped
         * off-screen. Untextured VP draws now render via the colour PSO. */
        if (s_d3d.vp_vb_mapped && s_d3d.vp_root_sig) {
            vp_pack_begin(s_d3d.current_rsx_state);
    vp_reserve(vp_expanded(primitive, count));
            u32 rec = s_d3d.vp_vb_offset, rec_chunk = s_vpc_cur;
            u32 emitted = upload_quads_vp(s_d3d.current_rsx_state, first, count);
            if (emitted && s_d3d.draw_count < s_max_draws) {
                D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
                dr->vb_byte_offset = rec;
                dr->vb_chunk       = rec_chunk;
                dr->vb_stride      = VP_VERT_STRIDE;
                dr->vertex_count   = emitted;
                dr->topology       = RSX_TOPOLOGY_TRIANGLES;
                dr->textured       = s_d3d.tex_bound;
                dr->is_vp          = 1;
                dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
                dr->fp_exp32 = s_d3d.current_rsx_state ?
                    fp_ctrl_bits(s_d3d.current_rsx_state->shader_control) : 1;
                dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->sref = s_d3d.current_rsx_state ? (s_d3d.current_rsx_state->stencil_ref & 0xFFu) : 0; rec_zrange(dr);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
                dr->cmask = 0xF;
                if (s_d3d.current_rsx_state) {
                    u32 _cm = s_d3d.current_rsx_state->color_mask;
                    dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                              | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                              | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                              | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
                } dr->mrtk = rsx_mrt_key(s_d3d.current_rsx_state);   /* v90 */
                dr->alpha_ctl = 0;
                if (s_d3d.current_rsx_state)
                    dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                                  | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                                  | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
                for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++) {
                    dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                    dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                    dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                    dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                    dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].zf    = s_d3d.cur_texs[_u].zf;   /* v34 */
                    dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                    dr->tex_rt[_u]  = -1;
                }
                dr->tex_slot = -1; dr->samp_tbl = samp_pack_on() ? 0 : samp_table_for_cur();
                dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
                dr->is_clear = 0;
                dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
                dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend); if (dr->mrtk & (1u << 20)) dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, 1);   /* v91 */
                if (getenv("BLENDDBG")) { static u32 seen[32]; static int ns=0;
                    u32 k = (dr->blend ? 0x80000000u : 0u) | dr->blend_key;
                    int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
                    if (!f && ns<32) { seen[ns++]=k;
                        fprintf(stderr, "[BLEND] enable=%d key=0x%X%c",
                                dr->blend, dr->blend_key, 10); } }
                dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr);
                dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
                if (s_d3d.current_rsx_state) {
                    dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                    dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                    dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                    dr->vp_h = s_d3d.current_rsx_state->viewport_h;
                } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
                dr->cb_slot = s_d3d.draw_count; dr->vcb_slot = dr->cb_slot; dr->fcb_slot = dr->cb_slot;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                if (samp_pack_on()) dr->samp_tbl = samp_table_for_draw(dr);   /* v156 */
                s_d3d.draw_count++;
            }
            return;
        }
        u32 record_offset = s_d3d.vb_offset;
        u32 emitted = upload_quads_from_rsx(first, count);
        if (emitted == 0) return;
        if (s_d3d.draw_count < s_max_draws) {
            s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
            s_d3d.draws[s_d3d.draw_count].vertex_count   = emitted;
            s_d3d.draws[s_d3d.draw_count].topology       = RSX_TOPOLOGY_TRIANGLES;
            s_d3d.draws[s_d3d.draw_count].textured       = s_d3d.tex_bound;
            s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
            s_legacy_draws++; if (s_d3d.tex_bound) s_legacy_tex_draws++;   /* v187 */
            s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
            s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
            s_d3d.draw_count++;
        }
        return;
    }

    /* TRIANGLES (prim 5): same VP-path preference as quads -- the guest's
     * vertex program does the MVP transform (gcm/cube draws its cube this way);
     * the fixed-function fallback below applies no transform, so 3D geometry
     * ends up in object space (invisible/garbage). */
    /* Route TRIANGLES/STRIP/FAN through the VP path (sagemono: LBP draws
     * non-indexed strips/fans), but ONLY when the guest ACTUALLY HAS a vertex
     * program (flOw). The VP path transforms via the guest's VP microcode; with
     * no microcode loaded it transforms nothing and the draw silently produces
     * zero pixels (it also uploads to vp_vb, leaving the fixed-function vb empty).
     * Geometry already in clip space with no VP -- flOw's injected scene -- must
     * go down the fixed-function passthrough below instead. */
    if ((primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP
         || primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
        s_d3d.vp_vb_mapped && s_d3d.vp_root_sig &&
        s_d3d.current_rsx_state && s_d3d.current_rsx_state->vp_ucode_bytes >= 16) {
        const int ph_cont = (primitive == RSX_PRIMITIVE_TRIANGLES) && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
                            s_d3d.merge_first_end == first;   /* v172 */
        vp_pack_begin(s_d3d.current_rsx_state);
    vp_reserve(vp_expanded(primitive, count));
        u32 rec = s_d3d.vp_vb_offset, rec_chunk = s_vpc_cur;
        u32 emitted = (primitive == RSX_PRIMITIVE_TRIANGLES)
            ? upload_tris_vp(s_d3d.current_rsx_state, first, count)
            : upload_strip_vp(s_d3d.current_rsx_state, first, count, primitive == RSX_PRIMITIVE_TRIANGLE_FAN);
        /* RSX splits one primitive stream across several DRAW_ARRAYS entries
         * inside a single BEGIN/END (each carries at most 256 vertices), and the
         * hardware concatenates them before assembling primitives. Recording a
         * draw per batch instead regroups the triangles: this title's tub mesh
         * arrives as 128 then 256,256,..., neither a multiple of 3, so every
         * batch after the first was assembled one vertex out of phase -- which
         * rendered as spike triangles with a zero texcoord along the tub rim.
         * The batches land contiguously in the vertex buffer, so extending the
         * previous record is all that is needed to restore the stream. */
        if (emitted && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
            s_d3d.merge_first_end == first) {
            D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
            u32 fpnow = s_d3d.current_rsx_state
                        ? s_d3d.current_rsx_state->shader_program : 0;
            if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                pv->fp_addr == fpnow && primitive == RSX_PRIMITIVE_TRIANGLES &&
                pv->begin_epoch == (s_d3d.current_rsx_state
                                    ? s_d3d.current_rsx_state->begin_epoch : 0) &&
                pv->vb_chunk == rec_chunk && pv->vb_stride == VP_VERT_STRIDE && pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
                pv->vertex_count += emitted;
                s_d3d.merge_first_end = first + count;
                return;
            }
        }
        if (emitted && ph_cont && s_d3d.draw_count < s_max_draws) {   /* v172 PHASE_FIX */
            u32 c = phase_fix_prepare(first, "arrays");
            if (c) { d3d12_draw_arrays(ud, primitive, first - c, count + c); return; }
        }
        if (emitted && s_d3d.draw_count < s_max_draws) {
            D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
            dr->vb_byte_offset = rec;
            dr->vb_chunk       = rec_chunk;
            dr->vb_stride      = VP_VERT_STRIDE;
            dr->vertex_count   = emitted;
            dr->topology       = RSX_TOPOLOGY_TRIANGLES;
            dr->textured       = s_d3d.tex_bound;
            dr->is_vp          = 1;
            dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
            dr->fp_exp32 = s_d3d.current_rsx_state ?
                fp_ctrl_bits(s_d3d.current_rsx_state->shader_control) : 1;
            dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->sref = s_d3d.current_rsx_state ? (s_d3d.current_rsx_state->stencil_ref & 0xFFu) : 0; rec_zrange(dr);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
            dr->cmask = 0xF;
            if (s_d3d.current_rsx_state) {
                u32 _cm = s_d3d.current_rsx_state->color_mask;
                dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                          | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                          | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                          | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
            } dr->mrtk = rsx_mrt_key(s_d3d.current_rsx_state);   /* v90 */
            dr->alpha_ctl = 0;
            if (s_d3d.current_rsx_state)
                dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                              | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                              | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
            for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++) {
                dr->tex[_u].off = s_d3d.cur_texs[_u].off;
                dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
                dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
                dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
                dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].zf    = s_d3d.cur_texs[_u].zf;   /* v34 */
                dr->tex[_u].set = s_d3d.cur_texs[_u].set;
                dr->tex_rt[_u]  = -1;
            }
            dr->tex_slot = -1; dr->samp_tbl = samp_pack_on() ? 0 : samp_table_for_cur();
            dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
            dr->is_clear = 0;
            dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
            dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend); if (dr->mrtk & (1u << 20)) dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, 1);   /* v91 */
            dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr);
            dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
            if (s_d3d.current_rsx_state) {
                dr->vp_x = s_d3d.current_rsx_state->viewport_x;
                dr->vp_y = s_d3d.current_rsx_state->viewport_y;
                dr->vp_w = s_d3d.current_rsx_state->viewport_w;
                dr->vp_h = s_d3d.current_rsx_state->viewport_h;
            } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
            dr->cb_slot = s_d3d.draw_count; dr->vcb_slot = dr->cb_slot; dr->fcb_slot = dr->cb_slot;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                if (samp_pack_on()) dr->samp_tbl = samp_table_for_draw(dr);   /* v156 */
            s_d3d.draw_count++;
            /* Anchor for merging the next DRAW_ARRAYS batch of this stream. */
            s_d3d.merge_prev_draw  = (primitive == RSX_PRIMITIVE_TRIANGLES);
            s_d3d.merge_first_end  = first + count;
        }
        return;
    }
    s_d3d.merge_prev_draw = 0;   /* any other primitive breaks the stream */

    u32 topo = (u32)rsx_primitive_topology(primitive);
    if (topo == RSX_TOPOLOGY_UNSUPPORTED) {
        /* Other primitives still needing index-buffer conversion
         * (line loops, triangle fans) — skip rather than draw wrong. */
        static int s_skipped_nontri = 0;
        if (s_skipped_nontri < 3) {
            printf("[D3D12] draw_arrays: skipping prim=%u (needs index conversion)\n",
                   primitive);
            s_skipped_nontri++;
        }
        return;
    }

    u32 record_offset = s_d3d.vb_offset;
    u32 actual_count  = upload_vertices_from_rsx(first, count);
    if (actual_count == 0) return;

    if (s_d3d.draw_count < s_max_draws) {
        s_d3d.draws[s_d3d.draw_count].vb_byte_offset = record_offset;
        s_d3d.draws[s_d3d.draw_count].vertex_count   = actual_count;
        s_d3d.draws[s_d3d.draw_count].topology       = topo;
        s_d3d.draws[s_d3d.draw_count].textured       = 0;
        s_d3d.draws[s_d3d.draw_count].is_vp          = 0;
        s_legacy_draws++;   /* v187 */
        s_d3d.draws[s_d3d.draw_count].is_clear       = 0;
        s_d3d.draws[s_d3d.draw_count].rt_off         = 0;
        s_d3d.draw_count++;
    }
}

/* ---- v60 VCACHE: cross-frame cache of CONVERTED vertices, in VRAM ----------------------
 * The city frame converts ~2M guest vertices (bswap + format decode into VPSlot float4s,
 * ~80 B each = ~160 MB of write-combined upload writes), and the GPU then pulls ~300 MB per
 * frame of vertex data across PCIe from the upload heap. Measured on the Linux repro
 * (VCACHE_SIM): 86-89% of those vertices are byte-identical to last frame's (static city
 * geometry). Each indexed 256-index batch is now keyed by its fetch plan + index range +
 * a hash of the RAW guest bytes it reads; a hit binds the already-converted copy in a
 * DEFAULT-heap buffer (no conversion, no upload, VRAM reads); a miss converts into the
 * frame's upload chunk as before and queues a GPU copy into the cache, executed at the
 * start of that frame's command list. Records over the cache use absolute u32 indices
 * (entry data starts on a stride multiple), so batches of one draw still merge.
 * Eviction: per-size-class LRU, never an entry used by the frame being recorded (the
 * frame the render thread is executing finished its GPU work before our copies run).
 * VCACHE=0 -> v59 path. VCACHE_MB=<n> (default 768). Log: [VCACHE] every 600 frames. */
static int s_vc_xxh;                       /* v180, defined with vc_hash_bytes */
#define VC_CLASSES 9                       /* 1 KB .. 256 KB blocks */
#define VC_ENTS (1u << 19)
#define VC_HT (1u << 21)
#define VC_COPIES 65536
typedef struct { u64 key; u32 blk, data, last, fr; int prev, next; u8 cls, live; } VcEnt;   /* v188 fr = frame of its last LRU placement */
/* v188: hash-table slot carries the key and the entry's data offset, so a hit costs ONE random cache line
 * (was: the int slot, then the entry for the key compare, then 2-3 more lines for the LRU move-to-front). */
typedef struct { u64 key; int e; u32 data; } VcSlot;
typedef struct { u32 chunk, src, dst, bytes; } VcCopy;
static ID3D12Resource* s_vc_res = NULL;
static u64 s_vc_size = 0, s_vc_bump = 0;
static int s_vc_state = 0;                 /* 0 COMMON, 1 COPY_DEST, 2 VERTEX/CB */
static VcEnt* s_vc_ent = NULL; static u32 s_vc_nent = 0;
static VcSlot* s_vc_ht = NULL; static u32 s_vc_tomb = 0, s_vc_live = 0;   /* ht .e: -1 empty, -2 tomb, else entry */
static int s_vc_lru_head[VC_CLASSES], s_vc_lru_tail[VC_CLASSES];
static VcCopy s_vc_cp[3][VC_COPIES]; static u32 s_vc_ncp[3];
static u64 s_vc_hits, s_vc_miss, s_vc_hitv, s_vc_missv, s_vc_fail, s_vc_hashb, s_vc_evict;
static int vc_on(void)
{
    static int on = -1;
    if (on >= 0) return on;
    on = 0;
    { const char* e = getenv("VC_XXH"); s_vc_xxh = (e && *e == '0') ? 0 : 1;
      fprintf(stderr, "[VCACHE] v180 key hash: %s (VC_XXH)%c", s_vc_xxh ? "XXH3-64" : "old mix", 10); }
    { const char* e = getenv("VCACHE"); if (e && *e == '0') { fprintf(stderr, "[VCACHE] off%c", 10); return 0; } }
    if (!s_d3d.device) { on = -1; return 0; }
    long mb = 768; { const char* e = getenv("VCACHE_MB"); if (e) mb = atol(e); }
    for (; mb >= 128; mb /= 2) {
        D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof bd);
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (u64)mb << 20;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = NULL;
        if (SUCCEEDED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, (void**)&r)) && r) {
            s_vc_res = r; s_vc_size = (u64)mb << 20; break;
        }
    }
    s_vc_ent = (VcEnt*)calloc(VC_ENTS, sizeof(VcEnt));
    s_vc_ht = (VcSlot*)malloc(VC_HT * sizeof(VcSlot));
    if (!s_vc_res || !s_vc_ent || !s_vc_ht) { fprintf(stderr, "[VCACHE] could not allocate -- off%c", 10); return 0; }
    for (u32 i = 0; i < VC_HT; i++) { s_vc_ht[i].e = -1; s_vc_ht[i].key = 0; s_vc_ht[i].data = 0; }
    for (int c = 0; c < VC_CLASSES; c++) { s_vc_lru_head[c] = s_vc_lru_tail[c] = -1; }
    on = 1;
    fprintf(stderr, "[VCACHE] converted-vertex cache ON (v60): %u MB VRAM%c", (unsigned)(s_vc_size >> 20), 10);
    return 1;
}
static inline u32 vc_slot(u64 key) { u64 h = key * 0x9E3779B97F4A7C15ull; return (u32)(h >> 40) & (VC_HT - 1); }
static int vc_find(u64 key, u32* slot_out)
{
    u32 h = vc_slot(key);
    for (;;) {
        const VcSlot* q = &s_vc_ht[h];
        int e = q->e;
        if (e == -1) { if (slot_out) *slot_out = h; return -1; }
        if (e >= 0 && q->key == key) { if (slot_out) *slot_out = h; return e; }
        h = (h + 1) & (VC_HT - 1);
    }
}
/* v188 VC_LRU (default 0 = CLOCK): a hit only stamps entry.last; eviction walks the class's list from the tail
 * and gives every entry used since its last placement (last > fr) or by the frame being recorded a second chance
 * (back to the front) -- approximate LRU without touching 3-4 list nodes per hit. Which entry gets evicted can
 * differ; what is drawn cannot (entries are keyed by every source byte). VC_LRU=1 = exact LRU (v60). */
static int vc_lru_exact(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VC_LRU"); v = (e && *e == '1') ? 1 : 0;
                 fprintf(stderr, "[VCACHE] v188 eviction: %s, key+data in the hash slot (VC_LRU)%c", v ? "exact LRU (v60)" : "CLOCK second chance", 10); }
    return v;
}
static void vc_ht_insert(u64 key, int e)
{
    u32 h = vc_slot(key);
    for (;;) {
        int x = s_vc_ht[h].e;
        if (x < 0) { if (x == -2) s_vc_tomb--; s_vc_ht[h].e = e; s_vc_ht[h].key = key; s_vc_ht[h].data = s_vc_ent[e].data; s_vc_live++; return; }
        h = (h + 1) & (VC_HT - 1);
    }
}
static void vc_ht_rebuild(void)
{
    for (u32 i = 0; i < VC_HT; i++) s_vc_ht[i].e = -1;
    s_vc_tomb = 0; s_vc_live = 0;
    for (u32 i = 0; i < s_vc_nent; i++) if (s_vc_ent[i].live) vc_ht_insert(s_vc_ent[i].key, (int)i);
}
static void vc_lru_unlink(int e)
{
    VcEnt* x = &s_vc_ent[e]; int c = x->cls;
    if (x->prev >= 0) s_vc_ent[x->prev].next = x->next; else s_vc_lru_head[c] = x->next;
    if (x->next >= 0) s_vc_ent[x->next].prev = x->prev; else s_vc_lru_tail[c] = x->prev;
    x->prev = x->next = -1;
}
static void vc_lru_front(int e)
{
    VcEnt* x = &s_vc_ent[e]; int c = x->cls;
    x->prev = -1; x->next = s_vc_lru_head[c];
    if (s_vc_lru_head[c] >= 0) s_vc_ent[s_vc_lru_head[c]].prev = e; else s_vc_lru_tail[c] = e;
    s_vc_lru_head[c] = e;
}
/* A new entry of class c (block 1 KB << c). -1 = nothing evictable. */
static int vc_alloc(int c, u32* blk_out)
{
    const u32 B = 1024u << c;
    int e = -1; u32 blk = 0;
    int t = s_vc_lru_tail[c];
    if (s_vc_bump + B <= s_vc_size && s_vc_nent < VC_ENTS) {
        blk = (u32)s_vc_bump; s_vc_bump += B; e = (int)s_vc_nent++;
    } else if (t >= 0 && (vc_lru_exact() ? s_vc_ent[t].last != s_frame_no : 1)) {   /* evict LRU of this class */
        if (!vc_lru_exact()) {   /* v188 CLOCK: second chance for entries used since their last placement */
            int tries = 0;
            while (s_vc_ent[t].last == s_frame_no || s_vc_ent[t].last > s_vc_ent[t].fr) {
                if (++tries > 64) return -1;
                s_vc_ent[t].fr = s_frame_no;
                vc_lru_unlink(t); vc_lru_front(t);
                t = s_vc_lru_tail[c];
            }
        }
        VcEnt* x = &s_vc_ent[t];
        u32 sl; if (vc_find(x->key, &sl) == t) { s_vc_ht[sl].e = -2; s_vc_tomb++; s_vc_live--; }
        vc_lru_unlink(t);
        blk = x->blk; e = t; s_vc_evict++;
    } else return -1;
    memset(&s_vc_ent[e], 0, sizeof(VcEnt));
    s_vc_ent[e].prev = s_vc_ent[e].next = -1; s_vc_ent[e].blk = blk; s_vc_ent[e].cls = (u8)c;
    s_vc_ent[e].fr = s_frame_no;   /* v188 */
    *blk_out = blk;
    return e;
}
static inline u64 vc_mix(u64 h, u64 w) { h ^= w; h *= 0x9E3779B97F4A7C15ull; return h ^ (h >> 29); }
/* v180 VC_XXH (default 1): the raw-byte hash of every VCACHE lookup (~17 MB/frame in the city, 9 % of the FIFO
 * thread in perf180 = the old two-lane multiply chain, ~2.7 B/cycle) is XXH3-64 (vendored xxhash.h v0.8.2, BSD-2,
 * AVX2 path; ~2.5x faster at the typical ~1 KB per lookup). Same exactness: the key still covers every byte read.
 * VC_XXH=0 = old hash. Read once in vc_on(). */
#define XXH_INLINE_ALL
#define XXH_STATIC_LINKING_ONLY
#include "xxhash.h"
static int s_vc_xxh = 1;
extern volatile int g_abx;   /* v180: ABX 0x1000 = old VCACHE hash, 0x2000 = old FIFO generic path (cellGcmSys.c) */
static u64 vc_hash_bytes(const u8* p, size_t n, u64 h)
{
    if (s_vc_xxh && !(g_abx & 0x1000)) return (u64)XXH3_64bits_withSeed(p, n, (XXH64_hash_t)h);   /* ABX 0x1000 = old hash (A/B) */
    u64 h2 = h ^ 0x5bd1e995ull;
    size_t i = 0;
    for (; i + 16 <= n; i += 16) { u64 a, b; memcpy(&a, p + i, 8); memcpy(&b, p + i + 8, 8); h = vc_mix(h, a); h2 = vc_mix(h2, b); }
    for (; i + 8 <= n; i += 8) { u64 a; memcpy(&a, p + i, 8); h = vc_mix(h, a); }
    for (; i < n; i++) h = vc_mix(h, p[i]);
    return vc_mix(h, h2 ^ n);
}
/* Try the cache for the guest index list L[0..n) of the current plan. On success the
 * converted vertices for [mn, mn+span) live in the cache at absolute vertex *vbase (units
 * of VP_VERT_STRIDE) -- either already there or arriving via this frame's copy -- and the
 * caller writes indices vbase + (L[j] - mn). */
static u64 s_vc_uniq = 0;
static u32 vc_store_list(const rsx_state* st, const u32* L, u32 n, u32* ib);
/* v192 VC_ASYNC (see vcw_* below vc_store_list) */
#define VCW_MAXR 8
static int vcw_active(void);
static int vcw_push(const u8* const* lo, const u32* len, int nr, u64 key0, int keyed, const u32* L, u32 n,
                    u32 mn, u32 span, int c, u32 stride, u32* ib);
static u64 s_vcw_inline;
/* store_only: the batch continues a cached record (same primitive stream), so it must land
 * in the cache even when it cannot be keyed -- else the stream would split out of phase. */
/* v198 VC_FASTKEY (ABX 0x10000 = on; VC_FASTKEY=1 forces on, =0 forces off): the VCACHE key needs a hash of every
 * raw byte a batch reads (18 MB/frame in the city, mostly cache-cold guest memory, on the FIFO thread the main
 * thread waits for). A batch whose plan + index range + source regions are the same as last time, and whose bytes
 * at 16 spread probes per region are the same too, reuses its last FULL key -- but only for a cache HIT, and only
 * for VC_FK_VERIFY frames (default 4) after the last full hash. A miss always re-hashes before inserting, so the
 * cache is never keyed by an unverified hash. Worst case: a mesh that changed only between probes shows its old
 * shape for <= VC_FK_VERIFY frames. [VC_FASTKEY] line with the [VCACHE] line. */
typedef struct { u64 ik, smp, full; u32 vf; u32 pad; } VcFk;
#define VC_FK_N (1u << 16)
static VcFk* s_vc_fk = NULL; static int s_vc_fk_on = -1, s_vc_fk_ver = 4;
static u64 s_vc_fk_fast, s_vc_fk_slow, s_vc_fk_bytes_saved;
static inline int vc_fk_active(void)
{
    if (__builtin_expect(s_vc_fk_on < 0, 0)) {
        const char* e = getenv("VC_FASTKEY"); s_vc_fk_on = (e && *e == '1') ? 2 : (e && *e == '0') ? 0 : 1;
        { const char* v = getenv("VC_FK_VERIFY"); if (v && atoi(v) >= 1) s_vc_fk_ver = atoi(v); }
        if (s_vc_fk_on) s_vc_fk = (VcFk*)calloc(VC_FK_N, sizeof(VcFk));
        if (!s_vc_fk) s_vc_fk_on = 0;
        fprintf(stderr, "[VC_FASTKEY] v198 %s (verify every %d frames)%c", s_vc_fk_on == 2 ? "forced ON" : s_vc_fk_on ? "ABX 0x10000" : "off", s_vc_fk_ver, 10);
    }
    return s_vc_fk_on == 2 || (s_vc_fk_on == 1 && (g_abx & 0x10000));
}
static inline u64 vc_fk_probe(const u8* p, u32 len, u64 h)
{
    if (len < 16) { u64 w = 0; memcpy(&w, p, len < 8 ? len : 8); return vc_mix(h, w ^ len); }
    u64 a, b; memcpy(&a, p, 8); memcpy(&b, p + len - 8, 8);
    h = vc_mix(vc_mix(h, a), b);
    const u32 step = (len - 8) / 15u;
    for (u32 k = 1; k < 15; k++) { u64 w; memcpy(&w, p + (size_t)k * step, 8); h = vc_mix(h, w); }
    return h;
}
/* v219 VC_WW (VC_WW=1; needs the guest reservation made with MEM_WRITE_WATCH, see vm.h): EXACT change detection for
 * VC_FASTKEY. Windows tracks which 4 KB pages of the guest arena were written (by ANY thread: PPU, SPU DMA, HLE, our own
 * walker); GetWriteWatch(RESET) hands the dirty pages over. Each page carries the poll generation of its last write.
 * A batch identity re-uses its last full key only if NO page of its source regions was written since that key was
 * hashed (gen < vgen) -- no sampling, no stale window -- so the FIFO thread stops reading ~18 MB of cold guest memory
 * per frame for unchanged meshes. When the poll must happen: before the first fast-path use after an EPOCH change.
 * The epoch moves whenever the RSX may see newer guest data: a new `put` snapshot (cellGcmSys.c drain pass), every
 * NV406E semaphore ACQUIRE (SPU-skinned vertices are released through it), and rsx_vtx_sync() (labels, references,
 * 2D/transfer methods = our own writes into guest memory). A poll that overflows / fails marks EVERYTHING dirty.
 * [VC_WW] line every 600 frames. ABX 0x10000 still gates VC_FASTKEY itself. */
static volatile LONG s_ww_epoch = 1;
static volatile LONG s_ww_done = 0;
static int s_ww_on = -1;
static u32* s_ww_gen = NULL;                 /* per 4 KB guest page: generation of the poll that saw it written */
static volatile u32 s_ww_g = 1, s_ww_all = 0;        /* current generation; everything written as of generation s_ww_all */
static ULONG_PTR* s_ww_buf = NULL;
#define WW_BUFN (1u << 18)
static u64 s_ww_polls, s_ww_pages, s_ww_ovf, s_ww_clean, s_ww_dirty, s_ww_mb; static double s_ww_ms;
void rsx_ww_bump(void) { s_ww_epoch++; }
/* v219c: only the 1 MB chunks that hold a fast-path batch's source bytes are polled (a whole-arena poll walked 1M
 * page-table entries: 3.5 ms). A chunk enters the set when a batch identity is stored with it; its first poll then
 * reports everything written since the start -> the identities using it look dirty once (conservative). */
static u8 s_ww_used[4096];
/* v220 VC_WSRC=1 (DIAGNOSTIC, needs VC_WW=1 + VC_FASTKEY=1, phase 0):
 * WHO writes the pages that hold vertex-source bytes? Every dirty page the write-watch poll reports inside a page that
 * a fast-path batch reads is counted as "SPU" when an SPU DMA PUT landed in it during this or the previous poll
 * interval (gcm_note_spu_put -> g_vc_put_note), else "other" (PPU stores, RSX transfers/labels, HLE memcpy, SPU
 * PUTLLC). If "other" is ~0, a software dirty map fed by the SPU PUT path can replace the 18 MB/frame XXH3 hash with
 * no write-watch cost. The first poll of a 1 MB chunk reports everything since boot -> not counted.
 * [VC_WSRC] line with [VC_WW] every 600 frames, plus the top "other" 1 MB chunks (guest EA). */
static int s_ws_on = 0;
static volatile u32* s_ws_spu = NULL;    /* per 4 KB page: s_ww_g when an SPU PUT last landed in it */
static u8* s_ws_vpg = NULL;              /* per 4 KB page: a fast-path batch reads it */
static u8 s_ws_seen[4096];               /* per 1 MB chunk: polled at least once */
static u32 s_ws_other_mb[4096];          /* "other" dirty vertex pages per 1 MB chunk (since the last report) */
static u64 s_ws_dirty, s_ws_spu_n, s_ws_other, s_ws_vpages;
/* v220 VC_SWD (phase 1; OPT-IN VC_SWD=1 since v220c, ABX 0x100000 = off for an in-run A/B):
 * SOFTWARE page-dirty map. Phase 0 (b025) showed that ~100 % of the writes into vertex-source pages are SPU DMA PUTs,
 * so the PUT hook (after the copy) stamps every 4 KB page it touches with the FIFO frame number. A VC_FASTKEY batch
 * identity re-uses its last full key without reading a byte when none of its source pages was stamped in or after
 * the frame before the one that hashed it (gen + 1 < vgen). Writers the hook does not see (PPU stores, PUTLLC, HLE,
 * RSX transfers; b025: ~0.1 page/frame, EA 0x41B00000) are caught by re-hashing every identity at least every
 * VC_SWD_VERIFY frames (default 8); a verify hash that differs although the map said clean counts as MISSED
 * ([VC_SWD] line, with the 1 MB chunks it happened in). */
static u32* s_swd_gen = NULL;            /* per 4 KB page: FIFO frame number of the last SPU PUT into it (0 = never) */
static int s_swd_init = 0, s_swd_ver = 8;
static int s_swd_phase = -1;   /* v243C VC_SWD_PHASE=0|1: the map is kept always, the fast key is used only on that PS3_AB parity */
static u64 s_swd_clean, s_swd_dirty, s_swd_verify, s_swd_missed;
static u32 s_swd_missed_mb[4096];
static void rsx_vc_note_spu_put(u32 ea, u32 size)
{
    if (!size) return;
    const u32 e = (u32)(((u64)ea + size - 1u) >> 12);
    u32* sw = s_swd_gen;
    if (sw) {
        const u32 f = *(volatile u32*)&s_frame_no;
        for (u32 pg = ea >> 12; pg <= e && pg < (1u << 20); pg++) if (sw[pg] != f) sw[pg] = f;
    }
    volatile u32* m = s_ws_spu;
    if (m) {
        const u32 g = s_ww_g;
        for (u32 pg = ea >> 12; pg <= e && pg < (1u << 20); pg++) if (m[pg] != g) m[pg] = g;
    }
}
static inline int swd_on(void)
{
    if (__builtin_expect(!s_swd_init, 0)) {
        s_swd_init = 1;
        const char* e = getenv("VC_SWD");
        { const char* v = getenv("VC_SWD_VERIFY"); if (v && atoi(v) >= 1) s_swd_ver = atoi(v); }
        { const char* ph = getenv("VC_SWD_PHASE"); if (ph && *ph) s_swd_phase = atoi(ph) & 1; }   /* v243C */
        (void)vc_fk_active();   /* allocates the identity memo */
        if ((e && *e == '1') && s_vc_fk) {   /* v220c: opt-in (b026c / b027: neutral) */
            u32* g = (u32*)calloc(1u << 20, sizeof(u32));
            if (g) { extern void (*g_vc_put_note)(u32 ea, u32 size);
                     s_swd_gen = g; MemoryBarrier(); g_vc_put_note = rsx_vc_note_spu_put; }
        }
        fprintf(stderr, "[VC_SWD] v220 software page-dirty map (SPU PUT stamps) instead of hashing unchanged batches: %s, verify every %d frames (VC_SWD=1 = on; v221C: no ABX bit)%c",
                s_swd_gen ? "ON" : "off", s_swd_ver, 10);
    }
    return s_swd_gen != NULL;   /* v221C: ABX 0x100000 now belongs to DCOAL (cellGcmSys.c); VC_SWD=1 alone switches this on */
}
/* v243C: swd_on() restricted to the VC_SWD_PHASE parity (unset: == swd_on()). Off phases run the plain hashing path;
 * memo entries from an earlier on-phase are older than VC_SWD_VERIFY frames, so they are re-hashed before reuse. */
static inline int swd_use(void)
{
    if (!swd_on()) return 0;
    if (s_swd_phase < 0) return 1;
    extern volatile int g_ab_phase;
    return (g_ab_phase & 1) == s_swd_phase;
}
/* v220b: frame-order prefetch for the VC_SWD path (the VC_PF / VC_SEQ prefetches live in the non-FASTKEY branch).
 * b026c profile: hashing went 5.7 -> 3.7 % of the FIFO thread but vc_lookup itself 19.3 -> 21.0 % -- the memo line,
 * the hash-table slot and the dirty-map line are three random DRAM loads per batch. Batches come in nearly the same
 * order every frame, so ordinal k remembers what it touched and batch k prefetches ordinals k+1..k+3. */
typedef struct { u32 m, sl, pg, pad; } SwdSeq;
#define SWQ_N 32768u
static SwdSeq s_swq[SWQ_N]; static u32 s_swq_frame = 0, s_swq_k = 0;
static inline int swd_region_clean(const u8* p, u32 len, u32 vgen)
{
    extern uint8_t* vm_base;
    if (!len) return 1;
    const size_t a = (size_t)(p - vm_base) >> 12, b = (size_t)(p + len - 1 - vm_base) >> 12;
    if (b >= (1u << 20) || a > b) return 0;
    for (size_t k = a; k <= b; k++) if (s_swd_gen[k] + 1u >= vgen) return 0;
    return 1;
}
static inline void ws_count(size_t pg, u32 g)
{
    if (!s_ws_seen[pg >> 8] || !s_ws_vpg[pg]) return;
    s_ws_dirty++;
    const u32 sg = s_ws_spu[pg];
    if (sg && sg + 1u >= g) s_ws_spu_n++;
    else { s_ws_other++; s_ws_other_mb[pg >> 8]++; }
}
static inline void ww_mark_used(const u8* p, u32 len)
{
    extern uint8_t* vm_base;
    if (!len) return;
    const size_t a = (size_t)(p - vm_base) >> 20, b = (size_t)(p + len - 1 - vm_base) >> 20;
    for (size_t k = a; k <= b && k < 4096; k++) s_ww_used[k] = 1;
    if (s_ws_vpg) {   /* v220 VC_WSRC */
        const size_t pa = (size_t)(p - vm_base) >> 12, pb = (size_t)(p + len - 1 - vm_base) >> 12;
        for (size_t k = pa; k <= pb && k < (1u << 20); k++) if (!s_ws_vpg[k]) { s_ws_vpg[k] = 1; s_ws_vpages++; }
    }
}
/* v219d VC_WW_ASYNC (default 1): the poll (~1.2 ms for ~50 MB, mostly the kernel resetting dirty bits) runs on a helper
 * thread. The FIFO thread asks for a poll of the current epoch (one SetEvent) and keeps going on the full-hash path
 * until a poll that STARTED after the request has completed; identities hashed meanwhile get vgen = the generation of
 * the newest poll that has started (a poll in flight may still report a write made after the hash). */
static volatile LONG s_ww_req = 0; static HANDLE s_ww_ev = NULL; static int s_ww_async = 0;
static void ww_poll_ep(LONG ep);
static DWORD WINAPI ww_thread(LPVOID p)
{
    (void)p;
    if (getenv("PS3_FAST_HOST")) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;) { WaitForSingleObject(s_ww_ev, INFINITE); ww_poll_ep(s_ww_req); }
    return 0;
}
static void ww_poll(void) { ww_poll_ep(s_ww_epoch); }
static void ww_poll_ep(LONG ep)
{
    extern uint8_t* vm_base;
    const double t0 = perf_now();
    const u32 g = s_ww_g + 1; s_ww_g = g; MemoryBarrier();
    for (u32 c = 0; c < 4096; ) {
        if (!s_ww_used[c]) { c++; continue; }
        u32 e = c; while (e < 4096 && s_ww_used[e]) e++;
        ULONG_PTR cnt = WW_BUFN; ULONG gran = 0;
        const UINT r = GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base + ((size_t)c << 20), (SIZE_T)(e - c) << 20, (PVOID*)s_ww_buf, &cnt, &gran);
        if (r != 0 || cnt >= WW_BUFN || gran != 4096) { s_ww_all = g; s_ww_ovf++; }
        else for (ULONG_PTR i = 0; i < cnt; i++) {
            const ULONG_PTR pg = (s_ww_buf[i] - (ULONG_PTR)vm_base) >> 12;
            if (pg < (1u << 20)) { s_ww_gen[pg] = g; if (s_ws_on) ws_count(pg, g); }
        }
        if (s_ws_on) for (u32 k = c; k < e; k++) s_ws_seen[k] = 1;   /* v220 VC_WSRC */
        s_ww_pages += cnt; s_ww_mb += e - c;
        c = e;
    }
    s_ww_polls++; s_ww_ms += (perf_now() - t0) * 1000.0;
    MemoryBarrier();
    s_ww_done = ep;
}
static int ww_active(void)
{
    if (__builtin_expect(s_ww_on < 0, 0)) {
        s_ww_on = 0;
        const char* e = getenv("VC_WW");
        if (e && *e == '1') {
            extern uint8_t* vm_base;
            s_ww_gen = (u32*)calloc(1u << 20, sizeof(u32));
            s_ww_buf = (ULONG_PTR*)malloc(WW_BUFN * sizeof(ULONG_PTR));
            ULONG_PTR cnt = WW_BUFN; ULONG gran = 0;
            const UINT r = (s_ww_gen && s_ww_buf) ? GetWriteWatch(WRITE_WATCH_FLAG_RESET, vm_base, (SIZE_T)0x100000000ull, (PVOID*)s_ww_buf, &cnt, &gran) : 1;
            if (r == 0 && gran == 4096) {
                s_ww_on = 1; s_ww_all = s_ww_g = 2;   /* everything before now counts as written */
                { const char* a = getenv("VC_WW_ASYNC"); s_ww_async = (a && *a == '0') ? 0 : 1; }
                if (s_ww_async) { s_ww_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
                    if (!s_ww_ev || !CreateThread(NULL, 1u << 16, ww_thread, NULL, 0, NULL)) s_ww_async = 0; }
                fprintf(stderr, "[VC_WW] poll on %s%c", s_ww_async ? "a helper thread (VC_WW_ASYNC)" : "the FIFO thread", 10);
                extern void (*g_rsx_vtx_sync)(void); g_rsx_vtx_sync = rsx_vtx_sync;   /* labels / 2D methods bump the epoch */
                { const char* w = getenv("VC_WSRC");   /* v220 diagnostic */
                  if (w && *w == '1') {
                      s_ws_spu = (volatile u32*)calloc(1u << 20, sizeof(u32)); s_ws_vpg = (u8*)calloc(1u << 20, 1);
                      if (s_ws_spu && s_ws_vpg) { extern void (*g_vc_put_note)(u32 ea, u32 size);
                          s_ws_on = 1; g_vc_put_note = rsx_vc_note_spu_put; }
                      fprintf(stderr, "[VC_WSRC] v220 vertex-page writer attribution (SPU PUT vs other): %s%c", s_ws_on ? "ON" : "alloc failed", 10); } }
            }
            fprintf(stderr, "[VC_WW] v219 write-watch change detection: %s (GetWriteWatch r=%u gran=%lu, %lu pages dirty at start)%c",
                    s_ww_on ? "ON" : "UNAVAILABLE (reservation not write-watched?)", r, (unsigned long)gran, (unsigned long)cnt, 10);
        } else fprintf(stderr, "[VC_WW] off (VC_WW=1 = exact write-watch change detection for VC_FASTKEY)%c", 10);
    }
    return s_ww_on;
}
static inline int ww_region_clean(const u8* p, u32 len, u32 vgen)
{
    extern uint8_t* vm_base;
    if (!len) return 1;
    const size_t a = (size_t)(p - vm_base) >> 12, b = (size_t)(p + len - 1 - vm_base) >> 12;
    if (b >= (1u << 20) || a > b) return 0;
    for (size_t k = a; k <= b; k++) if (s_ww_gen[k] >= vgen) return 0;
    return 1;
}
/* v208 VC_PF (default on; VC_PF=0 or ABX 0x80000 = old code): VCACHE lookup latency on the FIFO thread.
 * perf207 FIFO profile: ~10 % of the walking time was ONE load -- the hash-table slot of vc_find (32 MB table, a DRAM
 * miss per batch, ~12k batches/frame) -- plus ~6 % the scalar hit-path index rewrite and ~5 % the masked min/max loop.
 * (1) A small memo keyed by the batch's identity (key prefix + source pointers + lengths, known before the XXH3 hash)
 *     remembers the slot and entry this batch hit last time; both lines are prefetched before hashing, so the miss
 *     overlaps the hash. A wrong prediction only costs a useless prefetch -- the lookup itself is unchanged.
 * (2) AVX2 min/max over the index list and (3) AVX2 ib[j] = L[j] + (vb - mn) (== vb + (L[j] - mn) mod 2^32).
 * All three are exact. */
#include <immintrin.h>
#define VC_PF_N (1u << 16)
typedef struct { u64 ik; u32 slot; int e; } VcPf;
static VcPf* s_vc_pf = NULL;
static int vc_pf_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VC_PF"); v = (e && *e == '0') ? 0 : 1;
        if (v) { s_vc_pf = (VcPf*)calloc(VC_PF_N, sizeof(VcPf)); if (!s_vc_pf) v = 0; }
        fprintf(stderr, "[VCACHE] v208 slot prefetch + AVX2 index loops: %s (VC_PF=0 / ABX 0x80000 = old)%c", v ? "ON" : "off", 10); }
    return v && !(g_abx & 0x80000);
}
static inline void vc_minmax_u32(const u32* L, u32 n, u32* pmn, u32* pmx)
{
    u32 mn = 0xFFFFFFFFu, mx = 0, j = 0;
    if (n >= 16) {
#if defined(__AVX2__)
        __m256i vmn = _mm256_set1_epi32(-1), vmx = _mm256_setzero_si256();
        for (; j + 8 <= n; j += 8) { const __m256i v = _mm256_loadu_si256((const __m256i*)(L + j));
            vmn = _mm256_min_epu32(vmn, v); vmx = _mm256_max_epu32(vmx, v); }
        u32 a[8], b[8]; _mm256_storeu_si256((__m256i*)a, vmn); _mm256_storeu_si256((__m256i*)b, vmx);
#else   /* v242B PORT_CPU_COMPAT (no AVX2): same min/max, SSE4.1 */
        __m128i vmn0 = _mm_set1_epi32(-1), vmx0 = _mm_setzero_si128(), vmn1 = vmn0, vmx1 = vmx0;
        for (; j + 8 <= n; j += 8) { const __m128i v0 = _mm_loadu_si128((const __m128i*)(L + j)), v1 = _mm_loadu_si128((const __m128i*)(L + j + 4));
            vmn0 = _mm_min_epu32(vmn0, v0); vmx0 = _mm_max_epu32(vmx0, v0); vmn1 = _mm_min_epu32(vmn1, v1); vmx1 = _mm_max_epu32(vmx1, v1); }
        u32 a[8], b[8]; _mm_storeu_si128((__m128i*)a, vmn0); _mm_storeu_si128((__m128i*)(a + 4), vmn1);
        _mm_storeu_si128((__m128i*)b, vmx0); _mm_storeu_si128((__m128i*)(b + 4), vmx1);
#endif
        for (int k = 0; k < 8; k++) { mn = a[k] < mn ? a[k] : mn; mx = b[k] > mx ? b[k] : mx; }
    }
    for (; j < n; j++) { const u32 v = L[j]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    *pmn = mn; *pmx = mx;
}
static inline void vc_ib_fill(u32* ib, const u32* L, u32 n, u32 add)
{
    u32 j = 0;
#if defined(__AVX2__)
    const __m256i va = _mm256_set1_epi32((int)add);
    for (; j + 8 <= n; j += 8) _mm256_storeu_si256((__m256i*)(ib + j), _mm256_add_epi32(_mm256_loadu_si256((const __m256i*)(L + j)), va));
#else   /* v242B PORT_CPU_COMPAT (no AVX2) */
    const __m128i va = _mm_set1_epi32((int)add);
    for (; j + 8 <= n; j += 8) {
        _mm_storeu_si128((__m128i*)(ib + j), _mm_add_epi32(_mm_loadu_si128((const __m128i*)(L + j)), va));
        _mm_storeu_si128((__m128i*)(ib + j + 4), _mm_add_epi32(_mm_loadu_si128((const __m128i*)(L + j + 4)), va));
    }
#endif
    for (; j < n; j++) ib[j] = L[j] + add;
}
/* v219i VC_SEQ (default 1; 0 = off; ABX 0x10000000 = off for in-run A/B): latency hiding by FRAME ORDER. The FIFO
 * thread's top stalls (b007 exe) were two cache misses per batch -- the v208 memo line (1 MB table, random) and the
 * hash-table slot (32 MB, DRAM) -- plus the first lines of each batch's cold vertex bytes. A frame issues its batches
 * in nearly the same order as the last one, so remember, per batch ordinal, what that batch touched (identity, slot,
 * entry, first source line) and prefetch the slots/entries of batches k+1..k+2 and the source bytes of batch k+2
 * while batch k is hashed. Only prefetches: the lookup itself is unchanged (exact). read_guest_indices does the same
 * for the index buffers. */
typedef struct { u64 ik; const u8* src; u32 slot; int e; } VcSeq;
#define VCS_N 32768u
static VcSeq s_vcs[VCS_N]; static u32 s_vcs_k = 0, s_vcs_frame = 0xFFFFFFFFu;
static int vc_seq_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("VC_SEQ"); v = (e && *e == '0') ? 0 : 1;
        if (v) for (u32 i = 0; i < VCS_N; i++) { s_vcs[i].e = -1; s_vcs[i].src = NULL; }
        fprintf(stderr, "[VCACHE] v219i frame-order look-ahead prefetch: %s (VC_SEQ=0 / ABX 0x10000000 = off)%c", v ? "ON" : "off", 10); }
    return v && !(g_abx & 0x10000000);
}
/* v219k: __builtin_prefetch(.., 0, 3) = prefetcht0. clang-cl + the MSVC header made _mm_prefetch(.., _MM_HINT_T0) a prefetcht2. */
#define VC_PFT0(p) __builtin_prefetch((const void*)(p), 0, 3)
static inline void vc_pf_lines(const u8* p, u32 lines) { for (u32 i = 0; i < lines; i++) VC_PFT0(p + 64u * i); }
/* v221C VC_SUCC (default 1; VC_SUCC=0 off; ABX 0x100000 = off): VC_SEQ predicts batch k by its ORDINAL in the frame,
 * so one batch more or less (culling changes while the camera pans) misaligns every later ordinal for the rest of the
 * frame -- then the random v208 memo line was the hottest single stall of the FIFO thread (b027: 2.7 % at one load).
 * Correlation prefetch instead: each batch identity's entry (32 B, 4 MB table) remembers which identities followed it
 * last time (n1, n2 = table indices). Batch X prefetches the hash-table slot + cache entry + first source lines of n1
 * (whose entry line was prefetched one batch earlier as the previous batch's n2) and the entry line of n2. A shifted
 * frame re-synchronises after one batch. Only prefetches + the table itself: the lookup is unchanged (exact).
 * [VC_SUCC] line with [VCACHE]. */
typedef struct { u64 ik; const u8* src; u32 slot; int e; u32 n1, n2; } VcSc;
#define VC_SC_N (1u << 17)
static VcSc* s_vc_sc = NULL; static VcSc* s_sc_cur = NULL;
static u32 s_sc_frame = 0xFFFFFFFFu, s_sc_p1 = 0xFFFFFFFFu, s_sc_p2 = 0xFFFFFFFFu;
static u64 s_sc_n, s_sc_known, s_sc_n1ok;
static int vc_succ_on(void)
{
    static int v = -1;
    if (__builtin_expect(v < 0, 0)) {
        const char* e = getenv("VC_SUCC"); v = (e && *e == '0') ? 0 : 1;
        if (v) { s_vc_sc = (VcSc*)_aligned_malloc((size_t)VC_SC_N * sizeof(VcSc), 64);
                 if (s_vc_sc) { memset(s_vc_sc, 0, (size_t)VC_SC_N * sizeof(VcSc)); for (u32 i = 0; i < VC_SC_N; i++) s_vc_sc[i].e = -1; }
                 else v = 0; }
        fprintf(stderr, "[VC_SUCC] v221C successor-chain prefetch for VCACHE lookups: %s (VC_SUCC=0 off; ABX 0x100000 = off)%c", v ? "ON" : "off", 10);
    }
    return v && !(g_abx & 0x100000);
}
static int vc_lookup(const rsx_state* st, const u32* L, u32 n, u32* ib, u32* span_out, int store_only)
{
    s_sc_cur = NULL;   /* v221C */
    if (!s_vf_ok || st->imm_active || !n) return 0;
    const u32 stride = VP_VERT_STRIDE;
    u32 mn = 0xFFFFFFFFu, mx = 0;
    const int pfon = vc_pf_on();   /* v208 */
    if (pfon) vc_minmax_u32(L, n, &mn, &mx);
    else for (u32 j = 0; j < n; j++) { u32 v = L[j]; mn = v < mn ? v : mn; mx = v > mx ? v : mx; }
    u32 span = mx - mn + 1u;
    if (mx < mn || span > n || span > 4096u) {
        if (!store_only) return 0;
        vcw_drain();   /* v192: vc_store_list touches the cache state the worker owns */
        u32 u = vc_store_list(st, L, n, ib);
        if (!u) return 0;
        *span_out = u; return 1;
    }
    u64 bytes = (u64)span * stride;
    int c = 0; while (c < VC_CLASSES && (1024ull << c) < bytes + stride) c++;
    if (c >= VC_CLASSES) return 0;
    int keyed = 1;
    /* key: plan + range + raw bytes */
    u64 key = vc_mix(0xC0FFEEull, ((u64)mn << 32) | mx);
    key = vc_mix(key, s_vpk);
    const u8* lo[48]; u32 len[48]; int nr = 0;
    for (u32 i = 0; i < s_vpk && i < 16; i++) {
        VFAttr* f = &s_vfa[i];
        if (f->mode == 2) return 0;
        if (!keyed) continue;
        u32 d[4]; memcpy(d, f->def, sizeof d);
        key = vc_mix(key, ((u64)f->mode << 56) | ((u64)f->type << 48) | ((u64)f->n << 40) | ((u64)f->local << 32) | i);
        if (f->mode != 1) { key = vc_mix(key, ((u64)d[0] << 32) | d[1]); key = vc_mix(key, ((u64)d[2] << 32) | d[3]); continue; }
        if (f->freq) { keyed = 0; continue; }
        if (f->n < 4) { key = vc_mix(key, ((u64)d[0] << 32) | d[1]); key = vc_mix(key, ((u64)d[2] << 32) | d[3]); }
        key = vc_mix(key, ((u64)f->off0 << 32) | f->stride);
        u32 es; switch (f->type) { case 3: case 1: case 5: es = 2u * f->n; break; case 4: case 7: es = f->n; break; case 6: es = 4; break; default: es = 4u * f->n; }
        u64 o0 = (u64)f->off0 + (u64)mn * f->stride, o1 = (u64)f->off0 + (u64)mx * f->stride + es;
        if (o1 > 0xFFFFFFFFull) return 0;
        if (!f->local && ((o0 >> 20) != ((o1 - 1) >> 20))) {
            /* v60b: IO pages need not be contiguous -- hash each 1 MB piece on its own
             * (was: uncacheable, which split skinned-character streams out of phase). */
            for (u64 a = o0; a < o1 && nr < 46; ) {
                u64 pe = ((a >> 20) + 1) << 20; if (pe > o1) pe = o1;
                lo[nr] = vf_ptr(f, (u32)a); len[nr] = (u32)(pe - a); nr++;
                a = pe;
            }
            if (nr >= 46) keyed = 0;
            continue;
        }
        const u8* p0 = vf_ptr(f, (u32)o0);
        u32 l = (u32)(o1 - o0);
        int merged = 0;
        for (int r = 0; r < nr; r++) {
            const u8* a = lo[r]; const u8* b = lo[r] + len[r];
            if (p0 <= b && p0 + l >= a) { const u8* na = p0 < a ? p0 : a; const u8* nb = (p0 + l) > b ? (p0 + l) : b;
                lo[r] = na; len[r] = (u32)(nb - na); merged = 1; break; }
        }
        if (!merged) { lo[nr] = p0; len[nr] = l; nr++; }
    }
    if (!keyed && !store_only) return 0;
    /* v192 VC_ASYNC: hand the hash + lookup + miss conversion to the VCACHE worker. The record only needs the
     * span (known here); the worker writes this batch's absolute cache indices into ib[] before the frame is
     * handed to the render thread (rsx_vtx_sync drains it at every point where the guest could reuse vertex
     * memory or observe RSX progress -- the same points that already drain the VTX_ASYNC workers). */
    if (vcw_active()) {
        if (nr <= VCW_MAXR && vcw_push(lo, len, nr, key, keyed, L, n, mn, span, c, stride, ib)) { *span_out = span; return 1; }
        s_vcw_inline++;
    }
    vcw_drain();   /* sync path below owns the cache state now */
    int e = -1; u32 hit_data = 0;
    if (keyed && (vc_fk_active() | swd_use())) {   /* v198 VC_FASTKEY: reuse the last full key when nothing visible changed */
        const int sw = swd_use();     /* v220 VC_SWD: software page-dirty map (no poll, no sampling); v243C phase-gated */
        const int ww = sw ? 0 : ww_active();   /* v219: exact page write tracking instead of the 16-probe sample */
        SwdSeq* swq = NULL;
        if (sw) {   /* v220b frame-order prefetch */
            if (s_swq_frame != s_frame_no) { s_swq_frame = s_frame_no; s_swq_k = 0; }
            const u32 qk = s_swq_k++ & (SWQ_N - 1);
            for (u32 a = 1; a <= 3; a++) { const SwdSeq* q = &s_swq[(qk + a) & (SWQ_N - 1)];
                VC_PFT0(&s_vc_fk[q->m & (VC_FK_N - 1)]); VC_PFT0(&s_vc_ht[q->sl & (VC_HT - 1)]); VC_PFT0(&s_swd_gen[q->pg & ((1u << 20) - 1)]); }
            swq = &s_swq[qk];
            if (nr) { extern uint8_t* vm_base; swq->pg = (u32)((size_t)(lo[0] - vm_base) >> 12); }
        }
        int wwok = 0;
        if (ww) { const LONG ep = s_ww_epoch;
            if (s_ww_done == ep) wwok = 1;
            else if (s_ww_async) { if (s_ww_req != ep) { s_ww_req = ep; SetEvent(s_ww_ev); } }
            else { ww_poll(); wwok = 1; }
            MemoryBarrier(); }
        u64 ik = vc_mix(key, (u64)nr), smp = 0x51ED27ull; u64 tot = 0;
        for (int r = 0; r < nr; r++) { ik = vc_mix(vc_mix(ik, (u64)(uintptr_t)lo[r]), len[r]); if (!ww && !sw) smp = vc_fk_probe(lo[r], len[r], smp); tot += len[r]; }
        VcFk* m = &s_vc_fk[vc_slot(ik) & (VC_FK_N - 1)];
        if (swq) swq->m = (u32)(m - s_vc_fk);
        int same = 0, mapclean = 0;
        if (m->ik == ik && (!ww || wwok)) {
            if (sw) { mapclean = 1; for (int r = 0; mapclean && r < nr; r++) mapclean = swd_region_clean(lo[r], len[r], m->pad);
                      if (mapclean) s_swd_clean++; else s_swd_dirty++;
                      same = mapclean && (u32)(s_frame_no - m->vf) < (u32)s_swd_ver;
                      if (mapclean && !same) s_swd_verify++; }
            else if (ww) { same = m->pad > s_ww_all; for (int r = 0; same && r < nr; r++) same = ww_region_clean(lo[r], len[r], m->pad);
                      if (same) s_ww_clean++; else s_ww_dirty++; }
            else same = (m->smp == smp && (u32)(s_frame_no - m->vf) < (u32)s_vc_fk_ver);
        }
        if (same) {
            u32 sl; const int e2 = vc_find(m->full, &sl);
            if (swq) swq->sl = sl;
            if (e2 >= 0) {
                s_vc_fk_fast++; s_vc_fk_bytes_saved += tot;
                s_vc_ent[e2].last = s_frame_no;
                if (vc_lru_exact()) { VcEnt* x = &s_vc_ent[e2]; if (s_vc_lru_head[x->cls] != e2) { vc_lru_unlink(e2); vc_lru_front(e2); } }
                const u32 vb = s_vc_ht[sl].data / stride;
                for (u32 j = 0; j < n; j++) ib[j] = vb + (L[j] - mn);
                *span_out = span;
                s_vc_hits++; s_vc_hitv += span;
                return 1;
            }
        }
        /* slow: full hash (below), then remember it for this batch identity */
        u64 fk = key;
        for (int r = 0; r < nr; r++) { fk = vc_hash_bytes(lo[r], len[r], fk); s_vc_hashb += len[r]; }
        if (!fk) fk = 1;
        if (sw && mapclean && fk != m->full) {   /* v220: the map said clean, the bytes changed -> a writer the hook misses */
            s_swd_missed++;
            extern uint8_t* vm_base;
            for (int r = 0; r < nr; r++) { const size_t k = (size_t)(lo[r] - vm_base) >> 20; if (k < 4096) s_swd_missed_mb[k]++; }
        }
        m->ik = ik; m->smp = smp; m->full = fk; m->vf = s_frame_no;
        m->pad = sw ? s_frame_no : s_ww_async ? s_ww_g : s_ww_g + 1;   /* v219 VC_WW: pages written in or after this generation invalidate this key; v220 VC_SWD: the frame it was hashed in */
        if (ww) for (int r = 0; r < nr; r++) ww_mark_used(lo[r], len[r]);
        s_vc_fk_slow++;
        key = fk;
        if ((s_vc_tomb > VC_HT / 8)) vc_ht_rebuild();
        { u32 sl; e = vc_find(key, &sl); if (e >= 0) hit_data = s_vc_ht[sl].data; if (swq) swq->sl = sl; }
    } else if (keyed) {
        VcPf* pf = NULL;   /* v208: prefetch the slot + entry this batch hit last time, then hash */
        VcSeq* sq = NULL; u64 sq_ik = 0;   /* v219i */
        VcSc* sc = NULL;   /* v221C VC_SUCC */
        if (pfon && vc_succ_on()) {   /* v221C: successor-chain prefetch instead of VC_SEQ + the v208 memo */
            u64 ik = vc_mix(key, (u64)nr);
            for (int r = 0; r < nr; r++) ik = vc_mix(vc_mix(ik, (u64)(uintptr_t)lo[r]), len[r]);
            const u32 ix = vc_slot(ik) & (VC_SC_N - 1);
            sc = &s_vc_sc[ix];
            if (s_sc_frame != s_frame_no) { s_sc_frame = s_frame_no; s_sc_p1 = s_sc_p2 = 0xFFFFFFFFu; }
            if (sc->ik == ik) {
                s_sc_known++;
                if ((u32)sc->e < VC_ENTS) { VC_PFT0(&s_vc_ht[sc->slot & (VC_HT - 1)]); VC_PFT0(&s_vc_ent[sc->e]); }
                const VcSc* s1 = &s_vc_sc[sc->n1 & (VC_SC_N - 1)];   /* prefetched one batch ago (as that batch's n2) */
                if ((u32)s1->e < VC_ENTS) { VC_PFT0(&s_vc_ht[s1->slot & (VC_HT - 1)]); VC_PFT0(&s_vc_ent[s1->e]); }
                if (s1->src) vc_pf_lines(s1->src, 4);
                VC_PFT0(&s_vc_sc[sc->n2 & (VC_SC_N - 1)]);
            } else { sc->ik = ik; sc->e = -1; sc->src = NULL; sc->n1 = sc->n2 = ix; }
            if (s_sc_p1 != 0xFFFFFFFFu) { if (s_vc_sc[s_sc_p1].n1 == ix) s_sc_n1ok++; s_vc_sc[s_sc_p1].n1 = ix; }
            if (s_sc_p2 != 0xFFFFFFFFu) s_vc_sc[s_sc_p2].n2 = ix;
            s_sc_p2 = s_sc_p1; s_sc_p1 = ix; s_sc_n++;
            sc->src = nr ? lo[0] : NULL;
        } else if (pfon) {
            u64 ik = vc_mix(key, (u64)nr);
            for (int r = 0; r < nr; r++) ik = vc_mix(vc_mix(ik, (u64)(uintptr_t)lo[r]), len[r]);
            if (vc_seq_on()) {
                if (s_vcs_frame != s_frame_no) { s_vcs_frame = s_frame_no; s_vcs_k = 0; }
                const u32 k = s_vcs_k++ & (VCS_N - 1);
                for (u32 a = 1; a <= 2; a++) {
                    const VcSeq* nx = &s_vcs[(k + a) & (VCS_N - 1)];
                    if ((u32)nx->e < VC_ENTS) { VC_PFT0(&s_vc_ht[nx->slot & (VC_HT - 1)]); VC_PFT0(&s_vc_ent[nx->e]); }
                }
                { const VcSeq* n2 = &s_vcs[(k + 2) & (VCS_N - 1)]; if (n2->src) vc_pf_lines(n2->src, 4); }
                sq = &s_vcs[k]; sq_ik = ik;
                if (sq->ik == ik && (u32)sq->e < VC_ENTS) VC_PFT0(&s_vc_ht[sq->slot & (VC_HT - 1)]);
            }
            /* v219k: the frame-order entry already predicted this batch -> skip the random 1 MB memo line (it was the
             * single hottest stall left in b009: 7.8 % of the FIFO thread's exe samples). */
            if (!(sq && sq->ik == ik && (u32)sq->e < VC_ENTS))
            pf = &s_vc_pf[vc_slot(ik) & (VC_PF_N - 1)];
            if (pf && pf->ik == ik) { VC_PFT0(&s_vc_ht[pf->slot & (VC_HT - 1)]);
                                if ((u32)pf->e < VC_ENTS) VC_PFT0(&s_vc_ent[pf->e]); }
            else if (pf) { pf->ik = ik; pf->e = -1; }
        }
        for (int r = 0; r < nr; r++) { key = vc_hash_bytes(lo[r], len[r], key); s_vc_hashb += len[r]; }
        if (!key) key = 1;
        if ((s_vc_tomb > VC_HT / 8)) vc_ht_rebuild();
        { u32 sl; e = vc_find(key, &sl); if (e >= 0) { hit_data = s_vc_ht[sl].data; if (pf) { pf->slot = sl; pf->e = e; } }
          if (sq) { sq->ik = sq_ik; sq->src = nr ? lo[0] : NULL; sq->slot = sl; sq->e = e; }   /* v219i (e = -1 on a miss) */
          if (sc) { sc->slot = sl; sc->e = e; } }   /* v221C (a miss fills e below) */
        s_sc_cur = sc;
    } else key = 0x8000000000000000ull | ++s_vc_uniq;   /* never matches */
    if (e >= 0) {
        s_vc_ent[e].last = s_frame_no;   /* v188: a store, no read of the entry on the hit path */
        if (vc_lru_exact()) { VcEnt* x = &s_vc_ent[e]; if (s_vc_lru_head[x->cls] != e) { vc_lru_unlink(e); vc_lru_front(e); } }
        u32 vb = hit_data / stride;
        if (pfon) vc_ib_fill(ib, L, n, vb - mn);
        else for (u32 j = 0; j < n; j++) ib[j] = vb + (L[j] - mn);
        *span_out = span;
        s_vc_hits++; s_vc_hitv += span;
        return 1;
    }
    /* miss: convert into this frame's upload chunk, copy into the cache */
    int par = s_d3d.vp_parity;
    if (s_vc_ncp[par] >= VC_COPIES || !s_vpc_res[par][s_vpc_cur]) { s_vc_fail++; return 0; }
    u32 blk = 0;
    e = vc_alloc(c, &blk);
    if (e < 0) { s_vc_fail++; return 0; }
    u32 data = (blk + stride - 1) / stride * stride;
    VcEnt* x = &s_vc_ent[e];
    x->key = key; x->data = data; x->last = s_frame_no; x->live = keyed;
    vc_lru_front(e);
    if (keyed) vc_ht_insert(key, e);
    if (s_sc_cur && keyed) { s_sc_cur->e = e; s_sc_cur->slot = vc_slot(key); }   /* v221C VC_SUCC: next frame prefetches this entry */
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    if (span < va_min() || !va_submit(NULL, mn, span, out)) {
        VFAttr vfa[16]; memcpy(vfa, s_vfa, sizeof vfa);
        for (u32 q = 0; q < span; q++) read_vp_vertex_fast_p(st, vfa, mn + q, &out[(size_t)q * s_vpk]);
        s_va_inline_draws++; s_va_inline_verts += span;
    }
    s_perf_vconv += span;
    VcCopy* cp = &s_vc_cp[par][s_vc_ncp[par]++];
    cp->chunk = s_vpc_cur; cp->src = s_d3d.vp_vb_offset; cp->dst = data; cp->bytes = (u32)bytes;
    s_d3d.vp_vb_offset += (u32)bytes;
    { u32 vb = data / stride; if (pfon) vc_ib_fill(ib, L, n, vb - mn); else for (u32 j = 0; j < n; j++) ib[j] = vb + (L[j] - mn); }
    *span_out = span;
    s_vc_miss++; s_vc_missv += span;
    return 1;
}
/* Sparse list (range wider than the list): unique vertices via vib_build into the upload
 * chunk, then one copy into a fresh, unkeyed cache entry. Returns the unique count. */
static u32 vc_store_list(const rsx_state* st, const u32* L, u32 n, u32* ib)
{
    const u32 stride = VP_VERT_STRIDE;
    int par = s_d3d.vp_parity;
    if (s_vc_ncp[par] >= VC_COPIES || !s_vpc_res[par][s_vpc_cur]) { s_vc_fail++; return 0; }
    u64 maxb = (u64)n * stride;
    int c = 0; while (c < VC_CLASSES && (1024ull << c) < maxb + stride) c++;
    if (c >= VC_CLASSES) { s_vc_fail++; return 0; }
    u32 blk = 0;
    int e = vc_alloc(c, &blk);
    if (e < 0) { s_vc_fail++; return 0; }
    u32 data = (blk + stride - 1) / stride * stride;
    VcEnt* x = &s_vc_ent[e];
    x->key = 0x8000000000000000ull | ++s_vc_uniq; x->data = data; x->last = s_frame_no; x->live = 0;
    vc_lru_front(e);
    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
    u32 u = vib_build(st, L, n, out, ib, data / stride);
    if (!u) return 0;   /* entry stays in the LRU, recycled later */
    VcCopy* cp = &s_vc_cp[par][s_vc_ncp[par]++];
    cp->chunk = s_vpc_cur; cp->src = s_d3d.vp_vb_offset; cp->dst = data; cp->bytes = u * stride;
    s_d3d.vp_vb_offset += u * stride;
    s_vc_miss++; s_vc_missv += u;
    return u;
}
/* ===========================================================================
 * v192 VC_ASYNC (default ON; VC_ASYNC=0 off; ABX 0x8000 = old synchronous path for in-run A/B).
 * perf190/192: VCACHE work was ~25 % of the RSX FIFO thread (XXH3 hashing of every batch's source bytes,
 * the hash-table lookup, the LRU, and converting misses), and the main PPU thread waits ~11 % of the frame
 * for that thread to publish its fence. Now the FIFO thread only computes the batch's span/key prefix/byte
 * regions and queues a job; ONE worker thread (it owns ht/entries/LRU/copy list while it runs) does the rest
 * in FIFO order: hash, lookup, on a hit write ib[], on a miss allocate an entry, convert the vertices into its
 * own per-parity UPLOAD staging buffer and queue the GPU copy into the cache (chunk VCW_STAGE_CHUNK).
 * vcw_drain() (FIFO thread) waits until every queued job is done; it runs from rsx_vtx_sync(), i.e. at
 * render_frame, VB resets, label/semaphore writes, set-reference and 2D methods -- after it the FIFO thread
 * may touch the cache state itself (sync path, vc_store_list, vc_reset_parity).
 * Staging full / copy list full / no evictable entry: the batch's indices become 0 (degenerate, not drawn)
 * and it is counted ([VC_ASYNC] degenerate) -- the old path would have fallen back to an uncached draw.
 * =========================================================================*/
#define VCW_JOBS  32768
#define VCW_PLANS 4096
#define VCW_ARENA (4u << 20)          /* u32 indices */
#define VCW_STAGE_CHUNK 0xFEu
typedef struct { const u8* lo[VCW_MAXR]; u32 len[VCW_MAXR]; u64 key0; u32* ib;
                 u32 nr, n, mn, span, c, stride, plan, larena; u8 keyed, par; } VcwJob;
static int s_vcw_on = -1;
static VcwJob* s_vcw_job = NULL; static VAPlan* s_vcw_plan = NULL; static u32* s_vcw_arena = NULL;
static volatile LONG s_vcw_sub = 0, s_vcw_done = 0, s_vcw_sleep = 0;
static u32 s_vcw_plan_n = 0, s_vcw_arena_used = 0; static int s_vcw_last_plan = -1;
static HANDLE s_vcw_ev = NULL;
static ID3D12Resource* s_vcw_stage_res[3]; static u8* s_vcw_stage_map[3];
static u32 s_vcw_stage_off[3], s_vcw_stage_size = 0;
static u64 s_vcw_jobs, s_vcw_waits, s_vcw_degen, s_vcw_full; static double s_vcw_wait_ms;
static void vcw_run(const VcwJob* j)
{
    const u32 stride = j->stride, n = j->n, mn = j->mn, span = j->span;
    const int par = j->par;
    const u32* L = s_vcw_arena + j->larena;
    u32* ib = j->ib;
    u64 key = j->key0; int e = -1; u32 data = 0;
    if (j->keyed) {
        for (u32 r = 0; r < j->nr; r++) { key = vc_hash_bytes(j->lo[r], j->len[r], key); s_vc_hashb += j->len[r]; }
        if (!key) key = 1;
        if ((s_vc_tomb > VC_HT / 8)) vc_ht_rebuild();
        u32 sl; e = vc_find(key, &sl); if (e >= 0) data = s_vc_ht[sl].data;
    } else key = 0x8000000000000000ull | ++s_vc_uniq;   /* never matches */
    if (e >= 0) {
        s_vc_ent[e].last = s_frame_no;
        if (vc_lru_exact()) { VcEnt* x = &s_vc_ent[e]; if (s_vc_lru_head[x->cls] != e) { vc_lru_unlink(e); vc_lru_front(e); } }
        const u32 vb = data / stride;
        for (u32 q = 0; q < n; q++) ib[q] = vb + (L[q] - mn);
        s_vc_hits++; s_vc_hitv += span;
        return;
    }
    {
        const u32 bytes = span * stride;
        if (s_vc_ncp[par] >= VC_COPIES || !s_vcw_stage_map[par] || s_vcw_stage_off[par] + bytes > s_vcw_stage_size) goto degen;
        u32 blk = 0;
        e = vc_alloc(j->c, &blk);
        if (e < 0) goto degen;
        data = (blk + stride - 1) / stride * stride;
        VcEnt* x = &s_vc_ent[e];
        x->key = key; x->data = data; x->last = s_frame_no; x->live = j->keyed;
        vc_lru_front(e);
        if (j->keyed) vc_ht_insert(key, e);
        const u32 so = s_vcw_stage_off[par];
        VPSlot* out = (VPSlot*)(s_vcw_stage_map[par] + so);
        VFAttr vfa[16]; memcpy(vfa, s_vcw_plan[j->plan].vfa, sizeof vfa);   /* private page-cache copy */
        const u32 k = s_vcw_plan[j->plan].vpk;
        for (u32 q = 0; q < span; q++) read_vp_vertex_fast_k(NULL, vfa, k, mn + q, out + (size_t)q * k);
        VcCopy* cp = &s_vc_cp[par][s_vc_ncp[par]++];
        cp->chunk = VCW_STAGE_CHUNK; cp->src = so; cp->dst = data; cp->bytes = bytes;
        s_vcw_stage_off[par] = so + bytes;
        const u32 vb = data / stride;
        for (u32 q = 0; q < n; q++) ib[q] = vb + (L[q] - mn);
        s_vc_miss++; s_vc_missv += span;
        return;
    }
degen:
    for (u32 q = 0; q < n; q++) ib[q] = 0;
    s_vc_fail++; s_vcw_degen++;
}
static DWORD WINAPI vcw_thread(LPVOID p)
{
    (void)p;
    fprintf(stderr, "[THREADS] VCACHE worker hosttid=%lu%c", (unsigned long)GetCurrentThreadId(), 10);
    { extern void ps3_thread_prefer_pcores(const char* who, int prio);
      ps3_thread_prefer_pcores("VCACHE worker", THREAD_PRIORITY_ABOVE_NORMAL); }
    for (;;) {
        const LONG d = s_vcw_done;
        if (d != s_vcw_sub) {
            MemoryBarrier();
            VcwJob jl = s_vcw_job[(u32)d % VCW_JOBS];
            vcw_run(&jl);
            MemoryBarrier();
            InterlockedIncrement(&s_vcw_done);
            continue;
        }
        int it = 0;
        while (s_vcw_done == s_vcw_sub && ++it < 20000) YieldProcessor();
        if (s_vcw_done != s_vcw_sub) continue;
        InterlockedIncrement(&s_vcw_sleep);
        if (s_vcw_done == s_vcw_sub) WaitForSingleObject(s_vcw_ev, 2);
        InterlockedDecrement(&s_vcw_sleep);
    }
    return 0;
}
static void vcw_init(void)
{
    s_vcw_on = 0;
    /* v193: default OFF -- perf195 A/B: -0.3 +- 0.9 fps (the game forces ~290 drains per frame: label/semaphore
     * writes, 2D methods), and 13-40 degenerate batches per 600 frames. VC_ASYNC=1 = on. */
    { const char* e = getenv("VC_ASYNC"); if (!e || *e != '1') { fprintf(stderr, "[VC_ASYNC] off (default; VC_ASYNC=1 = worker)%c", 10); return; } }
    if (!s_d3d.device || !s_vc_res) return;
    long mb = 96; { const char* e = getenv("VC_ASYNC_STAGE_MB"); if (e && atol(e) >= 16) mb = atol(e); }
    s_vcw_stage_size = (u32)mb << 20;
    for (int par = 0; par < rf_npar(); par++) {
        D3D12_HEAP_PROPERTIES hp; memset(&hp, 0, sizeof hp); hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd; memset(&bd, 0, sizeof bd);
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = s_vcw_stage_size;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* r = NULL;
        if (FAILED(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device, &hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&r)) || !r) return;
        void* m = NULL; D3D12_RANGE nr0 = {0, 0};
        if (FAILED(r->lpVtbl->Map(r, 0, &nr0, &m)) || !m) { r->lpVtbl->Release(r); return; }
        s_vcw_stage_res[par] = r; s_vcw_stage_map[par] = (u8*)m; s_vcw_stage_off[par] = 0;
    }
    s_vcw_job = (VcwJob*)calloc(VCW_JOBS, sizeof(VcwJob));
    s_vcw_plan = (VAPlan*)calloc(VCW_PLANS, sizeof(VAPlan));
    s_vcw_arena = (u32*)malloc((size_t)VCW_ARENA * sizeof(u32));
    s_vcw_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!s_vcw_job || !s_vcw_plan || !s_vcw_arena || !s_vcw_ev) return;
    HANDLE th = CreateThread(NULL, 256 * 1024, vcw_thread, NULL, 0, NULL);
    if (!th) return;
    CloseHandle(th);
    { extern void (*g_rsx_vtx_sync)(void); g_rsx_vtx_sync = rsx_vtx_sync; }
    s_vcw_on = 1;
    fprintf(stderr, "[VC_ASYNC] v192 VCACHE worker ON (staging %ld MB x 2; VC_ASYNC=0 off, ABX 0x8000 = synchronous)%c", mb, 10);
}
static int vcw_active(void)
{
    if (__builtin_expect(s_vcw_on < 0, 0)) vcw_init();
    return s_vcw_on > 0 && !(g_abx & 0x8000);
}
/* FIFO thread: wait until the worker has finished every queued job; afterwards the cache state, the copy list
 * and the staging offsets are the FIFO thread's again. Cheap when idle. */
static void vcw_drain(void)
{
    if (s_vcw_on <= 0) return;
    if (s_vcw_done != s_vcw_sub) {
        LARGE_INTEGER a, b, f; QueryPerformanceCounter(&a);
        s_vcw_waits++;
        if (s_vcw_sleep > 0) SetEvent(s_vcw_ev);
        while (s_vcw_done != s_vcw_sub) { if (!(s_va_n > 0 && va_take())) YieldProcessor(); }
        QueryPerformanceCounter(&b); QueryPerformanceFrequency(&f);
        s_vcw_wait_ms += (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    }
    MemoryBarrier();
    s_vcw_arena_used = 0; s_vcw_plan_n = 0; s_vcw_last_plan = -1;
}
static int vcw_push(const u8* const* lo, const u32* len, int nr, u64 key0, int keyed, const u32* L, u32 n,
                    u32 mn, u32 span, int c, u32 stride, u32* ib)
{
    if (s_vcw_arena_used + n > VCW_ARENA || s_vcw_plan_n >= VCW_PLANS) {
        vcw_drain();
        if (s_vcw_arena_used + n > VCW_ARENA) return 0;
    }
    while ((u32)(s_vcw_sub - s_vcw_done) >= VCW_JOBS - 2) { s_vcw_full++; if (s_vcw_sleep > 0) SetEvent(s_vcw_ev); YieldProcessor(); }
    int pi = s_vcw_last_plan;
    if (pi < 0 || s_vcw_plan[pi].vpk != s_vpk || memcmp(s_vcw_plan[pi].vfa, s_vfa, sizeof(VFAttr) * s_vpk) != 0) {
        pi = (int)s_vcw_plan_n++;
        memcpy(s_vcw_plan[pi].vfa, s_vfa, sizeof s_vfa); s_vcw_plan[pi].vpk = s_vpk;
        s_vcw_last_plan = pi;
    }
    const u32 at = s_vcw_arena_used;
    memcpy(s_vcw_arena + at, L, (size_t)n * sizeof(u32));
    s_vcw_arena_used = at + n;
    VcwJob* j = &s_vcw_job[(u32)s_vcw_sub % VCW_JOBS];
    for (int r = 0; r < nr; r++) { j->lo[r] = lo[r]; j->len[r] = len[r]; }
    j->nr = (u32)nr; j->key0 = key0; j->keyed = (u8)(keyed != 0); j->ib = ib;
    j->n = n; j->mn = mn; j->span = span; j->c = (u32)c; j->stride = stride; j->plan = (u32)pi; j->larena = at;
    j->par = (u8)s_d3d.vp_parity;
    MemoryBarrier();
    InterlockedIncrement(&s_vcw_sub);
    s_vcw_jobs++;
    if (s_vcw_sleep > 0) SetEvent(s_vcw_ev);
    return 1;
}
/* Render thread, right after the frame's command list is reset: land the misses. */
static void vc_exec_copies(int par)
{
    if (!s_vc_res) return;
    u32 n = s_vc_ncp[par];
    if (!n && s_vc_state == 2) return;
    D3D12_RESOURCE_BARRIER b; memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = s_vc_res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    if (n) {
        if (s_vc_state != 1) {
            b.Transition.StateBefore = s_vc_state == 2 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : D3D12_RESOURCE_STATE_COMMON;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
            s_vc_state = 1;
        }
        for (u32 i = 0; i < n; ) {
            VcCopy c0 = s_vc_cp[par][i++];
            while (i < n && s_vc_cp[par][i].chunk == c0.chunk && s_vc_cp[par][i].src == c0.src + c0.bytes &&
                   s_vc_cp[par][i].dst == c0.dst + c0.bytes) { c0.bytes += s_vc_cp[par][i].bytes; i++; }
            ID3D12Resource* src = c0.chunk == VCW_STAGE_CHUNK ? s_vcw_stage_res[par]   /* v192 worker staging */
                                : c0.chunk < VP_MAX_CHUNKS ? s_vpc_res[par][c0.chunk] : NULL;
            if (src) s_d3d.cmd_list->lpVtbl->CopyBufferRegion(s_d3d.cmd_list, s_vc_res, c0.dst, src, c0.src, c0.bytes);
        }
    }
    if (s_vc_state != 2) {
        b.Transition.StateBefore = s_vc_state == 1 ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COMMON;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
        s_d3d.cmd_list->lpVtbl->ResourceBarrier(s_d3d.cmd_list, 1, &b);
        s_vc_state = 2;
    }
}
static void dc_report(void);   /* v221C DCOAL, with d3d12_draw_indexed */
static void drw_report(void);   /* v223 */
static void vc_report(void)
{
    dc_report();
    drw_report();
    if (!s_vc_res) return;
    fprintf(stderr, "[VCACHE] last 600 frames: %llu hits (%llu verts), %llu misses (%llu verts) = %.1f%% verts cached, %llu fallbacks, %llu evictions, %.1f MB hashed/frame, %u MB in use%c",
            (unsigned long long)s_vc_hits, (unsigned long long)s_vc_hitv, (unsigned long long)s_vc_miss, (unsigned long long)s_vc_missv,
            100.0 * (double)s_vc_hitv / (double)(s_vc_hitv + s_vc_missv + 1), (unsigned long long)s_vc_fail, (unsigned long long)s_vc_evict,
            (double)s_vc_hashb / 600.0 / 1048576.0, (unsigned)(s_vc_bump >> 20), 10);
    s_vc_hits = s_vc_miss = s_vc_hitv = s_vc_missv = s_vc_fail = s_vc_hashb = s_vc_evict = 0;
    if (s_vcw_on > 0)
        fprintf(stderr, "[VC_ASYNC] last 600 frames: %llu jobs, %llu drains waited %.1f ms total (%.3f ms/frame), %llu not queued, %llu ring-full spins, %llu degenerate (abx 0x%X)%c",
                (unsigned long long)s_vcw_jobs, (unsigned long long)s_vcw_waits, s_vcw_wait_ms, s_vcw_wait_ms / 600.0,
                (unsigned long long)s_vcw_inline, (unsigned long long)s_vcw_full, (unsigned long long)s_vcw_degen, (unsigned)g_abx, 10);
    s_vcw_jobs = s_vcw_waits = s_vcw_inline = s_vcw_full = s_vcw_degen = 0; s_vcw_wait_ms = 0.0;
    if (s_vc_fk_on > 0)
        fprintf(stderr, "[VC_FASTKEY] last 600 frames: %llu fast hits, %llu full hashes, %.1f MB/frame not hashed (abx 0x%X)%c",
                (unsigned long long)s_vc_fk_fast, (unsigned long long)s_vc_fk_slow, (double)s_vc_fk_bytes_saved / 600.0 / 1048576.0, (unsigned)g_abx, 10);
    s_vc_fk_fast = s_vc_fk_slow = s_vc_fk_bytes_saved = 0;
    if (s_sc_n) {   /* v221C VC_SUCC */
        fprintf(stderr, "[VC_SUCC] last 600 frames: %llu lookups, identity known %.1f %%, successor predicted %.1f %% (abx 0x%X)%c",
                (unsigned long long)s_sc_n, 100.0 * (double)s_sc_known / (double)s_sc_n, 100.0 * (double)s_sc_n1ok / (double)s_sc_n, (unsigned)g_abx, 10);
        s_sc_n = s_sc_known = s_sc_n1ok = 0;
    }
    if (s_fpr_fast + s_fpr_slow) {   /* v221C FPR_FAST */
        fprintf(stderr, "[FPR_FAST] last 600 frames: %llu FP memo hits re-used by one memcmp, %llu full memo walks stored (abx 0x%X)%c",
                (unsigned long long)s_fpr_fast, (unsigned long long)s_fpr_slow, (unsigned)g_abx, 10);
        s_fpr_fast = s_fpr_slow = 0;
    }
    if (s_ww_on > 0)
        fprintf(stderr, "[VC_WW] last 600 frames: %llu polls (%.2f/frame, %.3f ms/frame, %.1f us each), %.0f dirty pages/poll, %llu overflows; identity clean %llu, dirty %llu%c",
                (unsigned long long)s_ww_polls, s_ww_polls / 600.0, s_ww_ms / 600.0, s_ww_polls ? 1000.0 * s_ww_ms / s_ww_polls : 0.0,
                s_ww_polls ? (double)s_ww_pages / s_ww_polls : 0.0, (unsigned long long)s_ww_ovf, (unsigned long long)s_ww_clean, (unsigned long long)s_ww_dirty, 10);
    if (s_ww_on > 0)
        fprintf(stderr, "[VC_WW] polled %.0f MB per poll%c", s_ww_polls ? (double)s_ww_mb / s_ww_polls : 0.0, 10);
    if (s_swd_gen) {   /* v220 VC_SWD */
        char b[512]; int o = 0; b[0] = 0;
        for (int top = 0; top < 8; top++) {
            u32 best = 0, bk = 0;
            for (u32 k = 0; k < 4096; k++) if (s_swd_missed_mb[k] > best) { best = s_swd_missed_mb[k]; bk = k; }
            if (!best) break;
            o += snprintf(b + o, sizeof b - (size_t)o, " 0x%08X:%u", bk << 20, best);
            s_swd_missed_mb[bk] = 0;
        }
        fprintf(stderr, "[VC_SWD] last 600 frames: identity map-clean %llu, map-dirty %llu, verify re-hashes %llu, MISSED %llu%s%s (abx 0x%X)%c",
                (unsigned long long)s_swd_clean, (unsigned long long)s_swd_dirty, (unsigned long long)s_swd_verify, (unsigned long long)s_swd_missed,
                o ? " in (EA:batches)" : "", b, (unsigned)g_abx, 10);
        memset(s_swd_missed_mb, 0, sizeof s_swd_missed_mb);
        s_swd_clean = s_swd_dirty = s_swd_verify = s_swd_missed = 0;
    }
    if (s_ws_on) {   /* v220 VC_WSRC */
        fprintf(stderr, "[VC_WSRC] last 600 frames: %.1f dirty vertex pages/frame (%.2f MB), SPU PUT %.1f %%, OTHER %.1f %% (%.1f pages/frame); %llu vertex pages known (%.1f MB)%c",
                s_ws_dirty / 600.0, s_ws_dirty * 4096.0 / 600.0 / 1048576.0,
                s_ws_dirty ? 100.0 * s_ws_spu_n / s_ws_dirty : 0.0, s_ws_dirty ? 100.0 * s_ws_other / s_ws_dirty : 0.0, s_ws_other / 600.0,
                (unsigned long long)s_ws_vpages, s_ws_vpages * 4096.0 / 1048576.0, 10);
        char b[512]; int o = 0;
        for (int top = 0; top < 8; top++) {
            u32 best = 0, bk = 0;
            for (u32 k = 0; k < 4096; k++) if (s_ws_other_mb[k] > best) { best = s_ws_other_mb[k]; bk = k; }
            if (!best) break;
            o += snprintf(b + o, sizeof b - (size_t)o, " 0x%08X:%u", bk << 20, best);
            s_ws_other_mb[bk] = 0;
        }
        if (o) fprintf(stderr, "[VC_WSRC] top OTHER chunks (EA:pages):%s%c", b, 10);
        memset(s_ws_other_mb, 0, sizeof s_ws_other_mb);
        s_ws_dirty = s_ws_spu_n = s_ws_other = 0;
    }
    s_ww_polls = s_ww_pages = s_ww_ovf = s_ww_clean = s_ww_dirty = s_ww_mb = 0; s_ww_ms = 0.0;
}
static ID3D12Resource* vc_resource(u64* size) { if (size) *size = s_vc_size; return s_vc_res; }
static void vc_reset_parity(int par) { vcw_drain(); s_vc_ncp[par] = 0; s_vcw_stage_off[par] = 0; }   /* v192: drained (no-op after rsx_vtx_sync) */

/* v172 PHASE_FIX (default 1; 0 = old): a TRIANGLES stream that the RSX splits into DRAW_INDEX_ARRAY /
 * DRAW_ARRAYS batches (<= 256 each, not multiples of 3) is merged back into one record per path; when a batch
 * cannot be merged (e.g. the previous batch went to the converted-vertex cache and this one to the upload chunk,
 * or the upload chunk rolled over between batches) the new record started one or two indices INTO a triangle,
 * so every triangle of that batch was assembled out of phase -- mostly back-facing (culled) slivers: the holes and
 * white/blue specks around Cole's eyes, nostrils and mouth (rsxcap/frame_100..102, stream ops 534-600). Repair:
 * trim the previous record to whole triangles and re-read this batch from the start of the cut triangle. */
/* v174 GEOM dump (F3, always armed; GEOM_DUMP=0 off): for ONE whole frame after F3, every indexed TRIANGLES-family
 * draw into a 3-MRT target (the character G-buffer pass) or with stencil ref 3/7 is written to
 * rsxcap/geom/g_<seq>.bin AT RECORD TIME, from guest memory, through the generic fetch: header, viewport, the VS
 * transform constants c256..c259 and c464..c467, the guest index list and the fetched attribute-0 (position) of
 * every index. Offline (tools/geom_raster.py) this re-rasterises Cole exactly as the guest asked for him, to tell
 * bad vertex data from a bad GPU path (the eye/mouth holes). */
volatile int g_geom_arm_req = 0;
static volatile int s_geom_arm = 0;   /* 1 = F3 seen, wait for a frame start; 2 = dumping frame s_geom_f0 */
static u32 s_geom_f0 = 0, s_geom_seq = 0;
static void geom_dump(const rsx_state* st, u32 primitive, u32 first, u32 count)
{
    if (__builtin_expect(g_geom_arm_req != 0, 0)) { g_geom_arm_req = 0; if (!s_geom_arm) s_geom_arm = 1; }
    if (__builtin_expect(s_geom_arm == 0, 1) || !st) return;
    extern unsigned cellGcm_flip_request_count(void);
    const u32 fl = cellGcm_flip_request_count();
    if (s_geom_arm == 1) { s_geom_f0 = fl + 1; s_geom_arm = 2; s_geom_seq = 0; }
    if (fl < s_geom_f0) return;
    if (fl > s_geom_f0 || s_geom_seq >= 600) {
        fprintf(stderr, "[GEOM] v174 dumped %u draws of frame %u to rsxcap/geom%c", s_geom_seq, s_geom_f0, 10);
        s_geom_arm = 0; return; }
    if (primitive != RSX_PRIMITIVE_TRIANGLES && primitive != RSX_PRIMITIVE_TRIANGLE_STRIP && primitive != RSX_PRIMITIVE_TRIANGLE_FAN) return;
    const u32 sref = st->stencil_ref & 0xFFu;
    const int mrt3 = st->surface_color_offset[2] != 0 && st->surface_color_offset[2] != st->surface_color_offset[0];
    if (!mrt3 && sref != 3 && sref != 7) return;
    if (!count || count > 200000u) return;
    if (s_geom_seq == 0) { CreateDirectoryA("rsxcap", NULL); CreateDirectoryA("rsxcap/geom", NULL); }
    char fn[96]; snprintf(fn, sizeof fn, "rsxcap/geom/g_%04u.bin", s_geom_seq);
    FILE* f = fopen(fn, "wb"); if (!f) return;
    u32 hd[16] = { 0x4D4F4547u, s_geom_seq, primitive, count, sref, st->shader_program, rsx_cull_key(st), first,
                   st->surface_color_offset[0], st->surface_color_offset[1], st->surface_color_offset[2],
                   st->index_array_offset, st->index_array_dma, st->vertex_attribs[0].offset, st->vertex_attribs[0].stride,
                   st->vertex_attribs[0].format };
    fwrite(hd, 4, 16, f);
    fwrite(st->viewport_scale, 4, 4, f); fwrite(st->viewport_offset, 4, 4, f);
    for (int r = 256; r < 260; r++) fwrite(st->vertex_constants[r], 4, 4, f);
    for (int r = 464; r < 468; r++) fwrite(st->vertex_constants[r], 4, 4, f);
    u32* L = (u32*)malloc((size_t)count * 4u);
    if (L) {
        read_guest_indices(st, first, count, L);
        fwrite(L, 4, count, f);
        VPSlot v16[16];
        for (u32 j = 0; j < count; j++) { memset(v16, 0, sizeof v16); read_vp_vertex(st, L[j], v16); fwrite(&v16[0], 16, 1, f); }
        free(L);
    }
    fclose(f);
    s_geom_seq++;
}

static u64 s_ph_fixes = 0;
static int phase_fix_on(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PHASE_FIX"); v = (e && *e == '0') ? 0 : 1;
        fprintf(stderr, "[PHASE] v172 split-triangle-stream repair %s (PHASE_FIX=0 off)%c", v ? "ON" : "off", 10); }
    return v;
}
static u32 phase_fix_prepare(u32 first, const char* path)
{
    if (!phase_fix_on() || s_d3d.draw_count == 0) return 0;
    D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
    if (pv->is_clear || !pv->is_vp || pv->topology != RSX_TOPOLOGY_TRIANGLES) return 0;
    const int pib = (pv->ib_stamp == s_frame_no);
    const u32 pn = pib ? pv->index_count : pv->vertex_count;
    const u32 c = pn % 3u;
    if (!c || first < c || pn <= c) return 0;
    if (pib) pv->index_count -= c; else pv->vertex_count -= c;
    s_d3d.merge_prev_draw = 0;
    s_ph_fixes++;
    if (s_ph_fixes <= 24 || (s_ph_fixes & 4095u) == 0)
        fprintf(stderr, "[PHASE] #%llu %s batch at %u continued a stream whose record (ib=%d chunk=%u n=%u fp=0x%X) ends"
                        " mid-triangle -> trimmed %u, re-read from %u%c", (unsigned long long)s_ph_fixes, path, first, pib,
                (unsigned)pv->vb_chunk, pn, pv->fp_addr, c, first - c, 10);
    return c;
}

/* v223 DRW. OPT-IN: DRW=1, and it needs VC_SWD=1 (the software page-dirty
 * map is what proves the vertex bytes unchanged) and VC_ASYNC off. ABX bit DRW_BIT (hex, default 0x800000 = free only
 * when ANIM_LOD=1, like V222T_BIT; every other bit is taken) SET = off (in-run A/B).
 * Per-DRAW replay memo in front of VCACHE. Each indexed TRIANGLES batch that reaches vc_lookup (a DCOAL chunk or a
 * plain <= 256-index batch) is remembered by its identity = fetch plan + resolved index pointer + first/count. Next
 * time the same identity comes, the batch is REPLAYED -- no index read/byte-swap, no min/max, no key build, no VcFk
 * memo, no hash-table probe, no index rewrite: the indices vc_lookup wrote last time are copied into this frame's IB
 * -- when ALL of these hold (otherwise the normal path runs and the memo is refreshed from it):
 *   - the raw guest index bytes hash (XXH3-64) to the same value (exact for PPU/SPU/RSX writers alike; for a DCOAL
 *     run the run's bytes are hashed once and every chunk derives its identity from that hash),
 *   - the VCACHE entry still holds the same key and data offset (not evicted / re-used),
 *   - every source region of the entry is clean in the VC_SWD map since the frame its bytes were last fully hashed,
 *     and that hash is younger than VC_SWD_VERIFY frames (default 8) -- the same rule (and the same exactness) as the
 *     VC_SWD fast key: a forced full path every 8 frames re-hashes the bytes,
 *   - the attribute regions still resolve to the same host pointers (IO map unchanged).
 * DCOAL runs additionally replay the run's chunk boundaries (no whole-run index read + per-block min/max).
 * DRW_CHECK=1 (diagnostic, slow): on every replay also re-read the indices and fully re-hash the vertex bytes and
 * compare key + indices with the replayed ones -> [DRW] "check" counts must stay 0.
 * [DRW] line with [VCACHE] every 600 frames. */
#define DRW_N (1u << 16)
#define DRW_MAXR 8
typedef struct {
    u64 id, ih, key, psig;
    u32 hgen, mn, span, data, n, cap;
    int e, nr;
    u32* ib;
    const u8* lo[DRW_MAXR]; u32 len[DRW_MAXR];
} DrwEnt;
#define DRWR_N (1u << 14)
typedef struct { u64 id, ih; u32 nch, cap; u32* ends; } DrwRun;
static DrwEnt* s_drw = NULL; static DrwRun* s_drwr = NULL;
static int s_drw_on = -1, s_drw_chk = 0, s_drw_bit = 0x800000;
static int s_drw_phase = -1;   /* v243C DRW_PHASE=0|1: DRW only on that PS3_AB phase parity (unset = every phase) */
static int s_drw_runact = 0; static u64 s_drw_runih = 0;
static u64 s_drw_try, s_drw_hit, s_drw_new, s_drw_ihm, s_drw_stale, s_drw_ver, s_drw_gone, s_drw_psm, s_drw_store, s_drw_nostore,
           s_drw_chk_n, s_drw_chk_bad, s_drw_run, s_drw_runhit;
static int drw_active(void)
{
    if (__builtin_expect(s_drw_on < 0, 0)) {
        s_drw_on = 0;
        const char* e = getenv("DRW");
        { const char* c = getenv("DRW_CHECK"); s_drw_chk = (c && *c == '1') ? 1 : 0; }
        { const char* b = getenv("DRW_BIT"); if (b && *b) s_drw_bit = (int)strtoul(b, NULL, 16); }
        { const char* ph = getenv("DRW_PHASE"); if (ph && *ph) s_drw_phase = atoi(ph) & 1; }   /* v243C */
        const int sw = swd_on();
        if (e && *e == '1' && sw && !vcw_active()) {
            s_drw = (DrwEnt*)calloc(DRW_N, sizeof(DrwEnt));
            s_drwr = (DrwRun*)calloc(DRWR_N, sizeof(DrwRun));
            if (s_drw && s_drwr) s_drw_on = 1;
        }
        fprintf(stderr, "[DRW] v223 per-draw replay memo: %s (DRW=1 + VC_SWD=1 = on, VC_ASYNC must be off; ABX 0x%X = off)%s%c",
                s_drw_on ? "ON" : (e && *e == '1') ? (sw ? "OFF (VC_ASYNC is on / alloc failed)" : "OFF (needs VC_SWD=1)") : "off",
                (unsigned)s_drw_bit, s_drw_chk ? " -- DRW_CHECK=1 verifies every replay (slow)" : "", 10);
    }
    if (s_drw_phase >= 0) { extern volatile int g_ab_phase; if ((g_ab_phase & 1) != s_drw_phase) return 0; }   /* v243C */
    return s_drw_on > 0 && !(g_abx & s_drw_bit);
}
static inline const u8* drw_idx_ptr(const rsx_state* st, u32 first, u32* isz)
{
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    const int local = ((st->index_array_dma & 0xF) == 0);
    *isz = (((st->index_array_dma >> 4) & 0xF) == 1) ? 2u : 4u;
    return vm_base + cellGcmResolveLocated(local, st->index_array_offset) + (u64)first * *isz;
}
/* The fetch-plan part of vc_lookup's key (everything but the index range and the bytes). */
static u64 drw_plan_id(void)
{
    u64 k = vc_mix(0xD3A70223ull, ((u64)s_vpk << 32) | VP_VERT_STRIDE);
    for (u32 i = 0; i < s_vpk && i < 16; i++) {
        const VFAttr* f = &s_vfa[i];
        u32 d[4]; memcpy(d, f->def, sizeof d);
        k = vc_mix(k, ((u64)f->mode << 56) | ((u64)f->type << 48) | ((u64)f->n << 40) | ((u64)f->local << 32) | i);
        if (f->mode != 1 || f->n < 4) { k = vc_mix(k, ((u64)d[0] << 32) | d[1]); k = vc_mix(k, ((u64)d[2] << 32) | d[3]); }
        if (f->mode == 1) { k = vc_mix(k, ((u64)f->off0 << 32) | f->stride); k = vc_mix(k, f->freq); }
    }
    return k;
}
/* Host pointers the plan's arrays resolve to for vertices [mn, mn + span) (every 1 MB piece for IO arrays). */
static u64 drw_psig(u32 mn, u32 span)
{
    u64 h = 0x95160223ull;
    for (u32 i = 0; i < s_vpk && i < 16; i++) {
        VFAttr* f = &s_vfa[i];
        if (f->mode != 1) continue;
        const u64 o0 = (u64)f->off0 + (u64)mn * f->stride, o1 = (u64)f->off0 + (u64)(mn + span - 1u) * f->stride + 1u;
        if (o1 > 0xFFFFFFFFull) return 0;
        h = vc_mix(h, (u64)(uintptr_t)vf_ptr(f, (u32)o0));
        if (!f->local) for (u64 a = ((o0 >> 20) + 1) << 20; a < o1; a += 1u << 20) h = vc_mix(h, (u64)(uintptr_t)vf_ptr(f, (u32)a));
    }
    return h;
}
/* Same min/max, key prefix and source regions vc_lookup builds for L (copy of its first half). 0 = vc_lookup would not
 * key this list (sparse, too big, immediate, frequency divider) -> not memoised. */
static int drw_regions(const u32* L, u32 n, u64* key_out, const u8** lo, u32* len, int* nr_out, u32* mn_out, u32* span_out)
{
    const u32 stride = VP_VERT_STRIDE;
    u32 mn, mx; vc_minmax_u32(L, n, &mn, &mx);
    const u32 span = mx - mn + 1u;
    if (mx < mn || span > n || span > 4096u) return 0;
    const u64 bytes = (u64)span * stride;
    int c = 0; while (c < VC_CLASSES && (1024ull << c) < bytes + stride) c++;
    if (c >= VC_CLASSES) return 0;
    u64 key = vc_mix(0xC0FFEEull, ((u64)mn << 32) | mx);
    key = vc_mix(key, s_vpk);
    int nr = 0;
    for (u32 i = 0; i < s_vpk && i < 16; i++) {
        VFAttr* f = &s_vfa[i];
        if (f->mode == 2) return 0;
        u32 d[4]; memcpy(d, f->def, sizeof d);
        key = vc_mix(key, ((u64)f->mode << 56) | ((u64)f->type << 48) | ((u64)f->n << 40) | ((u64)f->local << 32) | i);
        if (f->mode != 1) { key = vc_mix(key, ((u64)d[0] << 32) | d[1]); key = vc_mix(key, ((u64)d[2] << 32) | d[3]); continue; }
        if (f->freq) return 0;
        if (f->n < 4) { key = vc_mix(key, ((u64)d[0] << 32) | d[1]); key = vc_mix(key, ((u64)d[2] << 32) | d[3]); }
        key = vc_mix(key, ((u64)f->off0 << 32) | f->stride);
        u32 es; switch (f->type) { case 3: case 1: case 5: es = 2u * f->n; break; case 4: case 7: es = f->n; break; case 6: es = 4; break; default: es = 4u * f->n; }
        const u64 o0 = (u64)f->off0 + (u64)mn * f->stride, o1 = (u64)f->off0 + (u64)mx * f->stride + es;
        if (o1 > 0xFFFFFFFFull) return 0;
        if (!f->local && ((o0 >> 20) != ((o1 - 1) >> 20))) {
            for (u64 a = o0; a < o1 && nr < 46; ) {
                u64 pe = ((a >> 20) + 1) << 20; if (pe > o1) pe = o1;
                lo[nr] = vf_ptr(f, (u32)a); len[nr] = (u32)(pe - a); nr++;
                a = pe;
            }
            if (nr >= 46) return 0;
            continue;
        }
        const u8* p0 = vf_ptr(f, (u32)o0);
        const u32 l = (u32)(o1 - o0);
        int merged = 0;
        for (int r = 0; r < nr; r++) {
            const u8* a = lo[r]; const u8* b = lo[r] + len[r];
            if (p0 <= b && p0 + l >= a) { const u8* na = p0 < a ? p0 : a; const u8* nb = (p0 + l) > b ? (p0 + l) : b;
                lo[r] = na; len[r] = (u32)(nb - na); merged = 1; break; }
        }
        if (!merged) { lo[nr] = p0; len[nr] = l; nr++; }
    }
    *key_out = key; *nr_out = nr; *mn_out = mn; *span_out = span;
    return 1;
}
/* Before the batch's indices are read: the memo slot for this identity and whether it can be replayed. */
static DrwEnt* drw_probe(const rsx_state* st, u32 first, u32 n, u64* id_out, u64* ih_out, int* hit_out)
{
    u32 isz; const u8* p = drw_idx_ptr(st, first, &isz);
    const u64 id = vc_mix(vc_mix(vc_mix(drw_plan_id(), (u64)(uintptr_t)p), ((u64)first << 32) | n), isz);
    const u32 ix = vc_slot(id) & (DRW_N - 1);
    DrwEnt* d = &s_drw[ix];
    {   /* frame-order look-ahead (same idea as VC_SEQ): prefetch the memo slot of batch k+2, the entry + indices of k+1 */
        static u32 s_seq[VCS_N]; static u32 s_sk = 0, s_sf = 0xFFFFFFFFu;
        if (s_sf != s_frame_no) { s_sf = s_frame_no; s_sk = 0; }
        const u32 k = s_sk++ & (VCS_N - 1);
        const DrwEnt* n2 = &s_drw[s_seq[(k + 2) & (VCS_N - 1)] & (DRW_N - 1)];
        VC_PFT0(n2); VC_PFT0((const u8*)n2 + 64); VC_PFT0((const u8*)n2 + 128);
        const DrwEnt* n1 = &s_drw[s_seq[(k + 1) & (VCS_N - 1)] & (DRW_N - 1)];
        if ((u32)n1->e < VC_ENTS) VC_PFT0(&s_vc_ent[n1->e]);
        if (n1->ib) vc_pf_lines((const u8*)n1->ib, n1->n > 512u ? 32u : (n1->n * 4u + 63u) >> 6);
        s_seq[k] = ix;
    }
    const u64 ih = s_drw_runact ? vc_mix(s_drw_runih, ((u64)first << 32) | n)
                                : (u64)XXH3_64bits_withSeed(p, (size_t)n * isz, (XXH64_hash_t)0xD1D10223ull);
    *id_out = id; *ih_out = ih; *hit_out = 0;
    s_drw_try++;
    if (d->id != id || !d->ib) { s_drw_new++; return d; }
    if (d->ih != ih) { s_drw_ihm++; return d; }
    if ((u32)(s_frame_no - d->hgen) >= (u32)s_swd_ver) { s_drw_ver++; return d; }
    for (int r = 0; r < d->nr; r++) if (!swd_region_clean(d->lo[r], d->len[r], d->hgen)) { s_drw_stale++; return d; }
    if ((u32)d->e >= VC_ENTS) { s_drw_gone++; return d; }
    { const VcEnt* x = &s_vc_ent[d->e];
      if (!x->live || x->key != d->key || x->data != d->data) { s_drw_gone++; return d; } }
    if (drw_psig(d->mn, d->span) != d->psig) { s_drw_psm++; return d; }
    *hit_out = 1;
    return d;
}
/* Replay: same side effects as a vc_lookup hit (entry stamped, stats) + last frame's absolute indices. */
static void drw_emit(DrwEnt* d, u32* ib, u32* span_out)
{
    const int e = d->e;
    s_vc_ent[e].last = s_frame_no;
    if (vc_lru_exact()) { VcEnt* x = &s_vc_ent[e]; if (s_vc_lru_head[x->cls] != e) { vc_lru_unlink(e); vc_lru_front(e); } }
    memcpy(ib, d->ib, (size_t)d->n * 4u);
    *span_out = d->span;
    s_vc_hits++; s_vc_hitv += d->span; s_drw_hit++;
}
/* After a successful vc_lookup on the normal path: remember what it did (only for the VC_SWD keyed path). */
static void drw_store(DrwEnt* d, u64 id, u64 ih, const u32* L, u32 n)
{
    const u8* lo[48]; u32 len[48]; int nr = 0; u64 key0; u32 mn, span;
    d->id = 0;
    if (!drw_regions(L, n, &key0, lo, len, &nr, &mn, &span) || nr > DRW_MAXR) { s_drw_nostore++; return; }
    u64 ik = vc_mix(key0, (u64)nr);
    for (int r = 0; r < nr; r++) ik = vc_mix(vc_mix(ik, (u64)(uintptr_t)lo[r]), len[r]);
    const VcFk* m = &s_vc_fk[vc_slot(ik) & (VC_FK_N - 1)];
    if (m->ik != ik) { s_drw_nostore++; return; }
    u32 sl; const int e = vc_find(m->full, &sl);
    if (e < 0 || !s_vc_ent[e].live) { s_drw_nostore++; return; }
    if (d->cap < n) {
        const u32 nc = n < 256u ? 256u : n;
        u32* q = (u32*)realloc(d->ib, (size_t)nc * 4u);
        if (!q) { s_drw_nostore++; return; }
        d->ib = q; d->cap = nc;
    }
    const u32 data = s_vc_ht[sl].data;
    vc_ib_fill(d->ib, L, n, data / VP_VERT_STRIDE - mn);
    d->ih = ih; d->key = m->full; d->e = e; d->data = data; d->mn = mn; d->span = span; d->hgen = m->pad; d->n = n;
    d->nr = nr; memcpy(d->lo, lo, (size_t)nr * sizeof lo[0]); memcpy(d->len, len, (size_t)nr * sizeof len[0]);
    d->psig = drw_psig(mn, span);
    d->id = id;
    s_drw_store++;
}
/* DRW_CHECK=1: the replayed key / indices against a full recomputation (reads the indices into L). */
static void drw_check(const rsx_state* st, const DrwEnt* d, u32 first, u32 n, u32* L)
{
    read_guest_indices(st, first, n, L);
    const u8* lo[48]; u32 len[48]; int nr = 0; u64 key; u32 mn, span;
    int bad = 0;
    if (!drw_regions(L, n, &key, lo, len, &nr, &mn, &span)) bad = 1;
    else {
        for (int r = 0; r < nr; r++) key = vc_hash_bytes(lo[r], len[r], key);
        if (!key) key = 1;
        if (key != d->key || mn != d->mn || span != d->span) bad = 1;
        const u32 add = d->data / VP_VERT_STRIDE - mn;
        for (u32 j = 0; !bad && j < n; j++) if (d->ib[j] != L[j] + add) bad = 1;
    }
    s_drw_chk_n++;
    if (bad) { s_drw_chk_bad++;
        if (s_drw_chk_bad <= 16) fprintf(stderr, "[DRW] CHECK MISMATCH #%llu first=%u n=%u key %016llX vs %016llX mn %u/%u span %u/%u%c",
                (unsigned long long)s_drw_chk_bad, first, n, (unsigned long long)key, (unsigned long long)d->key, mn, d->mn, span, d->span, 10); }
}
static void drw_report(void)
{
    if (s_drw_on <= 0 || !(s_drw_try | s_drw_run)) return;
    const double t = (double)(s_drw_try ? s_drw_try : 1);
    fprintf(stderr, "[DRW] last 600 frames: %llu batches, replayed %.1f %% | new %.1f %% idx-changed %.1f %% verify %.1f %% dirty %.1f %% entry-gone %.1f %% ptr-changed %.1f %% | "
                    "%llu stores, %llu not memoisable | runs %llu, chunking replayed %.1f %% | check %llu, MISMATCH %llu (abx 0x%X)%c",
            (unsigned long long)s_drw_try, 100.0 * s_drw_hit / t, 100.0 * s_drw_new / t, 100.0 * s_drw_ihm / t, 100.0 * s_drw_ver / t,
            100.0 * s_drw_stale / t, 100.0 * s_drw_gone / t, 100.0 * s_drw_psm / t,
            (unsigned long long)s_drw_store, (unsigned long long)s_drw_nostore,
            (unsigned long long)s_drw_run, s_drw_run ? 100.0 * s_drw_runhit / (double)s_drw_run : 0.0,
            (unsigned long long)s_drw_chk_n, (unsigned long long)s_drw_chk_bad, (unsigned)g_abx, 10);
    s_drw_try = s_drw_hit = s_drw_new = s_drw_ihm = s_drw_stale = s_drw_ver = s_drw_gone = s_drw_psm = s_drw_store = s_drw_nostore = 0;
    s_drw_chk_n = s_drw_run = s_drw_runhit = 0;   /* s_drw_chk_bad stays cumulative */
}

/* v225 DCOAL_REUSE: d3d12_draw_indexed() already decodes every large run in
 * order to find dense chunk boundaries.  Let each triangle chunk consume its
 * exact slice of that immutable decoded run instead of decoding the same guest
 * indices again.  The range check deliberately excludes PHASE_FIX's recursive
 * first-c call, whose extra leading indices are outside the decoded run. */
static u32* s_dc_buf = NULL; static u32 s_dc_cap = 0;
static u32 s_dc_reuse_first, s_dc_reuse_count;
static int s_dc_reuse_active;
static u64 s_dc_reuse_chunks, s_dc_reuse_indices;
static int dc_reuse_on(void)
{
    static int v = -1;
    if (v < 0) {
        const char* e = getenv("DCOAL_REUSE");
        v = !(e && *e == '0');
        fprintf(stderr, "[DCOAL_REUSE] v225 decoded-run reuse %s (DCOAL_REUSE=0 disables)%c",
                v ? "on" : "off", 10);
    }
    return v;
}

static void d3d12_draw_indexed_one(void* ud, u32 primitive, u32 first, u32 count)
{
    /* RSX_LIVE_FEED_DBG=1: pair with the [feed] method counts so "the FIFO
     * carries no draw batches" can be checked against "the backend is told
     * to draw" in the same run. */
    { static int dbg = -1; if (dbg < 0) dbg = getenv("RSX_LIVE_FEED_DBG") ? 1 : 0;
      if (dbg) { static long long n = 0;
          if (((++n) % 20000) == 0)
              printf("[backend] indexed callbacks=%lld (prim=%u count=%u)\n", n, primitive, count), fflush(stdout); } }
    (void)ud;
    static int log_count = 0;
    if (log_count < 8) {
        printf("[D3D12] draw_indexed(prim=%u, first=%u, count=%u)\n",
               primitive, first, count);
        log_count++;
    }
    if (!s_d3d.pipeline_ready) return;
    if (count == 0 || count > s_vp_max_verts) return;
    if (!s_d3d.vp_vb_mapped || !s_d3d.vp_root_sig) return;
    if (shadow_skip_draw(s_d3d.current_rsx_state)) return;   /* v30e */
    geom_dump(s_d3d.current_rsx_state, primitive, first, count);   /* v174 (F3) */
    frame_split_check();   /* v103A: before anything reserves vertex space or a record */
    const int ph_cont = (primitive == RSX_PRIMITIVE_TRIANGLES) && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
                        s_d3d.merge_first_end == first;   /* v172 */

    /* Expand through the VP path (indices resolved CPU-side): QUADS -> two
     * triangles per quad, TRIANGLES straight through, STRIP/FAN -> triangle
     * list. Other primitives are skipped rather than drawn wrong. */
    u32 emitted = 0;
    vp_pack_begin(s_d3d.current_rsx_state);
    vp_reserve(vp_expanded(primitive, count));
    u32 rec = s_d3d.vp_vb_offset, rec_chunk = s_vpc_cur;
    /* v30d VTX_INDEXED (see vib_build). */
    u32 ib_rec = 0, ib_n = 0; int ib_mode = 0;
    {
        const rsx_state* st = s_d3d.current_rsx_state;
        int par = s_d3d.vp_parity;
        if (vtx_indexed_on() && st &&
            (primitive == RSX_PRIMITIVE_TRIANGLES || primitive == RSX_PRIMITIVE_TRIANGLE_STRIP ||
             primitive == RSX_PRIMITIVE_TRIANGLE_FAN) &&
            !st->imm_active && (st->vertex_attribs[0].enabled) && vib_ready(par)) {
            vf_plan(st);
            u32 nout = vp_expanded(primitive, count);
            u32 maxv = (s_vp_max_verts * 256u - s_d3d.vp_vb_offset) / VP_VERT_STRIDE;
            const int dc_hit = primitive == RSX_PRIMITIVE_TRIANGLES && dc_reuse_on() && s_dc_reuse_active &&
                               first >= s_dc_reuse_first &&
                               (u64)(first - s_dc_reuse_first) + count <= s_dc_reuse_count;
            u32* L = dc_hit ? s_dc_buf + (first - s_dc_reuse_first) : vlist_get(nout + 1);
            u32* G = (primitive != RSX_PRIMITIVE_TRIANGLES) ? vlist_tmp_get(count) : NULL;
            if (s_vf_ok && nout && nout <= maxv && L && (primitive == RSX_PRIMITIVE_TRIANGLES || G) &&
                (u64)s_ib_off + (u64)nout * 4u <= VIB_BYTES) {
                /* v223 DRW: probe the replay memo BEFORE reading the indices (a replay never needs them). Only where the
                 * VCACHE path below will run (vc_cont_old = the stream must stay on the old path). */
                DrwEnt* dw = NULL; u64 dw_id = 0, dw_ih = 0; int dw_hit = 0;
                if (primitive == RSX_PRIMITIVE_TRIANGLES && vc_on() && drw_active()) {
                    const D3D12DrawRecord* pq0 = (s_d3d.draw_count > 0) ? &s_d3d.draws[s_d3d.draw_count - 1] : NULL;
                    const int cont_old = pq0 && s_d3d.merge_prev_draw && s_d3d.merge_first_end == first && pq0->vb_chunk != VC_CHUNK_ID;
                    if (!cont_old) dw = drw_probe(st, first, count, &dw_id, &dw_ih, &dw_hit);
                }
                if (primitive == RSX_PRIMITIVE_TRIANGLES) {
                    s_req_verts += count; s_req_draws++;
                    if (!dw_hit && !dc_hit) read_guest_indices(st, first, count, L);
                    if (dc_hit) { s_dc_reuse_chunks++; s_dc_reuse_indices += count; }
                }
                else {
                    read_guest_indices(st, first, count, G);
                    u32 o = 0, tris = count - 2;
                    for (u32 t = 0; t < tris; t++) { L[o++] = G[primitive == RSX_PRIMITIVE_TRIANGLE_FAN ? 0 : t]; L[o++] = G[t + 1]; L[o++] = G[t + 2]; }
                }
                /* v60 VCACHE (see vc_lookup). */
                int vc_done = 0;
                if (vc_on()) {
                    u32 span = 0;
                    double _tv = perf_on() ? perf_now() : 0.0;
                    D3D12DrawRecord* pq = (s_d3d.draw_count > 0) ? &s_d3d.draws[s_d3d.draw_count - 1] : NULL;
                    int vc_merge = pq && s_d3d.merge_prev_draw && s_d3d.merge_first_end == first &&
                            primitive == RSX_PRIMITIVE_TRIANGLES && pq->is_vp && pq->topology == RSX_TOPOLOGY_TRIANGLES &&
                            pq->fp_addr == st->shader_program && pq->begin_epoch == st->begin_epoch &&
                            pq->vb_chunk == VC_CHUNK_ID && pq->vb_stride == VP_VERT_STRIDE &&
                            pq->ib_stamp == s_frame_no && pq->ib_off + pq->index_count * 4u == s_ib_off;
                    u32* ib = (u32*)(s_ib_map[par] + s_ib_off);
                    /* v60b: a batch continuing a primitive stream whose previous batch went down the
                     * old path must follow it there, or the stream splits out of phase (Cole's holes). */
                    int vc_cont_old = pq && s_d3d.merge_prev_draw && s_d3d.merge_first_end == first &&
                            primitive == RSX_PRIMITIVE_TRIANGLES && pq->vb_chunk != VC_CHUNK_ID;
                    int vc_ok;
                    if (dw_hit) { if (s_drw_chk) drw_check(st, dw, first, count, L); drw_emit(dw, ib, &span); vc_ok = 1; }   /* v223 */
                    else {
                        vc_ok = !vc_cont_old && vc_lookup(st, L, nout, ib, &span, vc_merge);
                        if (vc_ok && dw) drw_store(dw, dw_id, dw_ih, L, nout);
                    }
                    if (vc_ok) {
                        if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += nout; }
                        if (vc_merge) {
                            pq->vertex_count += span; pq->index_count += nout;
                            s_ib_off += nout * 4u;
                            s_d3d.merge_first_end = first + count;
                            return;
                        }
                        ib_mode = 1; ib_rec = s_ib_off; ib_n = nout; emitted = span;
                        s_ib_off += nout * 4u;
                        rec = 0; rec_chunk = VC_CHUNK_ID;
                        vc_done = 1;
                    }
                }
                if (!vc_done) {
                /* Same concatenation rule as the expanded path below, decided BEFORE
                 * writing so the indices can carry the merged record's bias. */
                D3D12DrawRecord* pv = (s_d3d.draw_count > 0) ? &s_d3d.draws[s_d3d.draw_count - 1] : NULL;
                int want_merge = pv && s_d3d.merge_prev_draw && s_d3d.merge_first_end == first &&
                    primitive == RSX_PRIMITIVE_TRIANGLES && pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
                    pv->fp_addr == st->shader_program && pv->begin_epoch == st->begin_epoch &&
                    pv->vb_chunk == rec_chunk && pv->vb_stride == VP_VERT_STRIDE &&
                    pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec;
                int pv_idx = want_merge && pv->ib_stamp == s_frame_no &&
                             pv->ib_off + pv->index_count * 4u == s_ib_off;
                if (!want_merge || pv_idx) {
                    double _tv = perf_on() ? perf_now() : 0.0;
                    VPSlot* out = (VPSlot*)(vp_cur_base() + s_d3d.vp_vb_offset);
                    u32* ib = (u32*)(s_ib_map[par] + s_ib_off);
                    u32 uq = vib_build(st, L, nout, out, ib, pv_idx ? pv->vertex_count : 0);
                    if (perf_on()) { s_perf_vtx += perf_now() - _tv; s_perf_nverts += nout; }
                    if (uq) {
                        s_d3d.vp_vb_offset += uq * VP_VERT_STRIDE;
                        if (pv_idx) {
                            pv->vertex_count += uq; pv->index_count += nout;
                            s_ib_off += nout * 4u;
                            s_d3d.merge_first_end = first + count;
                            return;
                        }
                        ib_mode = 1; ib_rec = s_ib_off; ib_n = nout; emitted = uq;
                        s_ib_off += nout * 4u;
                    }
                }
                }   /* !vc_done */
                /* want_merge into a non-indexed record, or the build failed: old path */
            }
        }
    }
    if (ib_mode) { /* record below */ }
    else if (primitive == RSX_PRIMITIVE_QUADS)             emitted = upload_quads_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLES)    emitted = upload_tris_vp_indexed(s_d3d.current_rsx_state, first, count);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_STRIP) emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 0);
    else if (primitive == RSX_PRIMITIVE_TRIANGLE_FAN)   emitted = upload_strip_vp_indexed(s_d3d.current_rsx_state, first, count, 1);
    else {
        static int _skip = 0;
        if ((_skip < 3 && _skip++ < 3))
            printf("[D3D12] draw_indexed: skipping prim=%u (not wired)\n", primitive);
        return;
    }
    /* Same BEGIN/END batch concatenation as the non-indexed path: this title's
     * fluid arrives as ~3000 DRAW_INDEX_ARRAY batches of 256, and 256 is not a
     * multiple of 3, so every batch after the first assembled out of phase. */
    if (emitted && !ib_mode && s_d3d.merge_prev_draw && s_d3d.draw_count > 0 &&
        s_d3d.merge_first_end == first && primitive == RSX_PRIMITIVE_TRIANGLES &&
        s_d3d.draws[s_d3d.draw_count - 1].ib_stamp != s_frame_no) {
        D3D12DrawRecord* pv = &s_d3d.draws[s_d3d.draw_count - 1];
        u32 fpnow = s_d3d.current_rsx_state
                    ? s_d3d.current_rsx_state->shader_program : 0;
        if (pv->is_vp && pv->topology == RSX_TOPOLOGY_TRIANGLES &&
            pv->fp_addr == fpnow &&
            pv->begin_epoch == (s_d3d.current_rsx_state
                                ? s_d3d.current_rsx_state->begin_epoch : 0) &&
            pv->vb_chunk == rec_chunk && pv->vb_stride == VP_VERT_STRIDE && pv->vb_byte_offset + pv->vertex_count * VP_VERT_STRIDE == rec) {
            pv->vertex_count += emitted;
            s_d3d.merge_first_end = first + count;
            return;
        }
    }
    if (emitted && ph_cont && s_d3d.draw_count < s_max_draws) {   /* v172 PHASE_FIX */
        u32 c = phase_fix_prepare(first, ib_mode ? (rec_chunk == VC_CHUNK_ID ? "indexed/vcache" : "indexed/upload") : "indexed/expanded");
        if (c) {   /* v223: the re-read starts before the DCOAL run -> its bytes are not covered by the run hash */
            const int ra = s_drw_runact; s_drw_runact = 0;
            d3d12_draw_indexed_one(ud, primitive, first - c, count + c);   /* v221C: _one (was the registered entry point) */
            s_drw_runact = ra;
            return;
        }
    }
    if (emitted && s_d3d.draw_count < s_max_draws) {
        D3D12DrawRecord* dr = &s_d3d.draws[s_d3d.draw_count];
        dr->vb_byte_offset = rec;
        dr->vb_chunk       = rec_chunk;
        dr->vb_stride      = VP_VERT_STRIDE;
        dr->vertex_count   = emitted;
        dr->topology       = RSX_TOPOLOGY_TRIANGLES;
        dr->textured       = s_d3d.tex_bound;
        dr->is_vp          = 1;
        dr->fp_addr = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->shader_program : 0;
        dr->fp_exp32 = s_d3d.current_rsx_state ?
            fp_ctrl_bits(s_d3d.current_rsx_state->shader_control) : 1;
        dr->cull = rsx_cull_key(s_d3d.current_rsx_state); dr->sref = s_d3d.current_rsx_state ? (s_d3d.current_rsx_state->stencil_ref & 0xFFu) : 0; rec_zrange(dr);
        dr->begin_epoch = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->begin_epoch : 0;
        dr->cmask = 0xF;
        if (s_d3d.current_rsx_state) {
            u32 _cm = s_d3d.current_rsx_state->color_mask;
            dr->cmask = ((_cm & 0x00010000) ? 1u : 0u)   /* R */
                      | ((_cm & 0x00000100) ? 2u : 0u)   /* G */
                      | ((_cm & 0x00000001) ? 4u : 0u)   /* B */
                      | ((_cm & 0x01000000) ? 8u : 0u);  /* A */
        } dr->mrtk = rsx_mrt_key(s_d3d.current_rsx_state);   /* v90 */
        dr->alpha_ctl = 0;
        if (s_d3d.current_rsx_state)
            dr->alpha_ctl = ((s_d3d.current_rsx_state->alpha_test_enable ? 1u : 0u) << 16)
                          | ((s_d3d.current_rsx_state->alpha_func & 0xFFu) << 8)
                          | (s_d3d.current_rsx_state->alpha_ref & 0xFFu);
        if (!(g_abx & 0x1000000)) {
            /* v219e: same copy, vectorised (perf218/b001: the scalar 10-field x 16-unit copy was ~4 % of the FIFO
             * thread, ~13k records/frame). cur_texs = off raw w h fmt ctrl1 mips cube set samp zf; the record's
             * tex[] = off raw w h fmt ctrl1 mips zf cube set -> words 0..6 straight, 7 = zf, 8 = cube, 9 = set.
             * ABX 0x1000000 = the old loop (A/B). */
            _Static_assert(sizeof(dr->tex[0]) == 40 && sizeof(s_d3d.cur_texs[0]) == 44, "tex layouts");
            const u32* src = (const u32*)&s_d3d.cur_texs[0];
            u32* dst = (u32*)&dr->tex[0];
#if defined(__AVX2__)
            const __m256i neg1 = _mm256_set1_epi32(-1);
            for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++, src += 11, dst += 10) {
                __m256i v = _mm256_loadu_si256((const __m256i*)src);
                v = _mm256_insert_epi32(v, (int)src[10], 7);
                _mm256_storeu_si256((__m256i*)dst, v);
                dst[8] = src[7]; dst[9] = src[8];
            }
            _mm256_storeu_si256((__m256i*)&dr->tex_rt[0], neg1);
            _mm256_storeu_si256((__m256i*)&dr->tex_rt[8], neg1);
#else       /* v242B PORT_CPU_COMPAT (no AVX2): the same words with SSE4.1 */
            const __m128i neg1 = _mm_set1_epi32(-1);
            for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++, src += 11, dst += 10) {
                const __m128i lo = _mm_loadu_si128((const __m128i*)src);
                const __m128i hi = _mm_insert_epi32(_mm_loadu_si128((const __m128i*)(src + 4)), (int)src[10], 3);
                _mm_storeu_si128((__m128i*)dst, lo);
                _mm_storeu_si128((__m128i*)(dst + 4), hi);
                dst[8] = src[7]; dst[9] = src[8];
            }
            for (int _k = 0; _k < 16; _k += 4) _mm_storeu_si128((__m128i*)&dr->tex_rt[_k], neg1);
#endif
        } else
        for (int _u = 0; _u < RSX_D3D_TEX_UNITS; _u++) {
            dr->tex[_u].off = s_d3d.cur_texs[_u].off;
            dr->tex[_u].raw = s_d3d.cur_texs[_u].raw;
            dr->tex[_u].w   = s_d3d.cur_texs[_u].w;
            dr->tex[_u].h   = s_d3d.cur_texs[_u].h;
            dr->tex[_u].fmt = s_d3d.cur_texs[_u].fmt;
                    dr->tex[_u].ctrl1 = s_d3d.cur_texs[_u].ctrl1;
                    dr->tex[_u].cube  = s_d3d.cur_texs[_u].cube;
                    dr->tex[_u].mips  = s_d3d.cur_texs[_u].mips;
                    dr->tex[_u].zf    = s_d3d.cur_texs[_u].zf;   /* v34 */
            dr->tex[_u].set = s_d3d.cur_texs[_u].set;
            dr->tex_rt[_u]  = -1;
        }
        dr->tex_slot = -1; dr->samp_tbl = samp_pack_on() ? 0 : samp_table_for_cur();
        dr->vs_idx = vp_get_vs(s_d3d.current_rsx_state);
        dr->is_clear = 0;
        dr->blend = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->blend_enable : 1;
        dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, dr->blend); if (dr->mrtk & (1u << 20)) dr->blend_key = rsx_blend_key(s_d3d.current_rsx_state, 1);   /* v91 */
        dr->rt_off = current_rt_off(&dr->rt_w, &dr->rt_h, dr->rt_mrt); rec_zeta(dr);
        dr->rt_fmt = s_d3d.current_rsx_state ? s_d3d.current_rsx_state->surface_format : 0;
        if (s_d3d.current_rsx_state) {
            dr->vp_x = s_d3d.current_rsx_state->viewport_x;
            dr->vp_y = s_d3d.current_rsx_state->viewport_y;
            dr->vp_w = s_d3d.current_rsx_state->viewport_w;
            dr->vp_h = s_d3d.current_rsx_state->viewport_h;
        } else { dr->vp_x = dr->vp_y = dr->vp_w = dr->vp_h = 0; }
        dr->cb_slot = s_d3d.draw_count; dr->vcb_slot = dr->cb_slot; dr->fcb_slot = dr->cb_slot;
        if (ib_mode) { dr->ib_off = ib_rec; dr->index_count = ib_n; dr->ib_stamp = s_frame_no; }
        else dr->ib_stamp = 0;
                vp_record_cb(s_d3d.draw_count, dr->vs_idx, dr);
                if (samp_pack_on()) dr->samp_tbl = samp_table_for_draw(dr);   /* v156 */
        s_d3d.draw_count++;
        /* Anchor for merging the next batch of this indexed stream. */
        s_d3d.merge_prev_draw = (primitive == RSX_PRIMITIVE_TRIANGLES);
        s_d3d.merge_first_end = first + count;
    }
}

/* v221C DCOAL: entry point for draw_indexed. cellGcmSys.c hands over a whole DRAW_INDEX_ARRAY RUN (the contiguous
 * 256-index words of one packet, TRIANGLES only) as one call with count > 256. Old behaviour = one d3d12_draw_indexed_one
 * per 256 indices, each with its own VCACHE lookup (key, hash, memo, slot, entry, index fill), merged into one record.
 * Now the run's indices are read once, each 256-block's [min, max] computed, and the run is cut GREEDILY into chunks:
 * from block b take the LARGEST b..e (<= DCOAL_MAXB blocks) whose union is still dense (span <= indices, the condition
 * vc_lookup needs to cache a list) and fits one cache class (span * stride + stride <= 256 KB, span <= 4096); no such
 * union -> block b alone (= old). Every chunk goes through the unchanged per-batch path (d3d12_draw_indexed_one), so the
 * draw is the same triangle list over the same guest vertices -- only the cache-entry granularity differs (entries are
 * still keyed by every source byte). Effects: fewer lookups (count), and 256-windows that are sparse on their own
 * (uncached: vib_build + conversion + copy every frame) become cacheable when their chunk is dense.
 * Anything not chunkable (other primitive, no VCACHE, immediate mode) is re-split into the original 256 pieces.
 * [DCOAL] line every 600 frames (with [VCACHE]). */
static u64 s_dc_runs, s_dc_idx, s_dc_split, s_dc_chunks, s_dc_multi, s_dc_mblocks, s_dc_sparse1, s_dc_blocks;
#define DC_MAXBLK 4096u
static int dc_maxb(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("DCOAL_MAXB"); v = e ? atoi(e) : 16; if (v < 1) v = 1; if (v > 256) v = 256;
        fprintf(stderr, "[DCOAL] v221C chunks of up to %d x 256 indices (DCOAL_MAXB)%c", v, 10); }
    return v;
}
static void d3d12_draw_indexed(void* ud, u32 primitive, u32 first, u32 count)
{
    if (count <= 256u) { d3d12_draw_indexed_one(ud, primitive, first, count); return; }
    const rsx_state* st = s_d3d.current_rsx_state;
    s_dc_runs++; s_dc_idx += count;
    const u32 nb = (count + 255u) >> 8;
    int ok = primitive == RSX_PRIMITIVE_TRIANGLES && st && !st->imm_active && st->vertex_attribs[0].enabled &&
             s_d3d.pipeline_ready && vtx_indexed_on() && vc_on() && nb <= DC_MAXBLK;
    if (ok && count > s_dc_cap) {
        u32 c = s_dc_cap ? s_dc_cap : 65536u; while (c < count) c *= 2u;
        u32* p = (u32*)realloc(s_dc_buf, (size_t)c * sizeof(u32));
        if (p) { s_dc_buf = p; s_dc_cap = c; } else ok = 0;
    }
    if (!ok) {   /* the original 256 pieces */
        s_dc_split++;
        for (u32 o = 0; o < count; o += 256u) d3d12_draw_indexed_one(ud, primitive, first + o, (count - o) < 256u ? (count - o) : 256u);
        return;
    }
    /* v223 DRW: the run's index bytes are hashed once; unchanged run -> replay last time's chunk boundaries (no index
     * read, no per-block min/max), and every chunk derives its index identity from the run hash (no second hash). */
    DrwRun* dr = NULL; u64 dr_id = 0, dr_ih = 0;
    if (drw_active()) {
        vp_pack_begin(st);
        u32 isz; const u8* ip = drw_idx_ptr(st, first, &isz);
        dr_ih = (u64)XXH3_64bits_withSeed(ip, (size_t)count * isz, (XXH64_hash_t)0xD1D20223ull);
        dr_id = vc_mix(vc_mix(vc_mix((u64)(uintptr_t)ip, ((u64)first << 32) | count), ((u64)isz << 32) | VP_VERT_STRIDE), (u64)dc_maxb());
        dr = &s_drwr[vc_slot(dr_id) & (DRWR_N - 1)];
        s_drw_run++;
        if (dr->id == dr_id && dr->ih == dr_ih && dr->ends) {
            s_drw_runhit++;
            s_dc_blocks += nb; s_dc_chunks += dr->nch;
            s_drw_runact = 1; s_drw_runih = dr_ih;
            u32 o = 0;
            for (u32 k = 0; k < dr->nch; k++) { const u32 end = dr->ends[k]; d3d12_draw_indexed_one(ud, primitive, first + o, end - o); o = end; }
            s_drw_runact = 0;
            return;
        }
        if (dr->cap < nb) {
            u32* q = (u32*)realloc(dr->ends, (size_t)nb * sizeof(u32));
            if (q) { dr->ends = q; dr->cap = nb; } else dr = NULL;
        }
        if (dr) { dr->id = 0; dr->nch = 0; }
    }
    read_guest_indices(st, first, count, s_dc_buf);
    if (dc_reuse_on()) {
        s_dc_reuse_first = first;
        s_dc_reuse_count = count;
        s_dc_reuse_active = 1;
    }
    vp_pack_begin(st);   /* VP_VERT_STRIDE of this draw (cached by microcode generation; _one recomputes the same) */
    const u32 stride = VP_VERT_STRIDE;
    const u32 cmax = 1024u << (VC_CLASSES - 1);
    u32 cap = stride ? (cmax - stride) / stride : 0; if (cap > 4096u) cap = 4096u;
    static u32 bmn[DC_MAXBLK], bmx[DC_MAXBLK];
    for (u32 b = 0; b < nb; b++) {
        const u32 o = b << 8, k = (count - o) < 256u ? (count - o) : 256u;
        vc_minmax_u32(s_dc_buf + o, k, &bmn[b], &bmx[b]);
    }
    const u32 maxb = (u32)dc_maxb();
    s_dc_blocks += nb;
    for (u32 b = 0; b < nb; ) {
        u32 mn = 0xFFFFFFFFu, mx = 0, ntot = 0; int best = -1;
        for (u32 e = b; e < nb && e < b + maxb; e++) {
            mn = bmn[e] < mn ? bmn[e] : mn; mx = bmx[e] > mx ? bmx[e] : mx;
            ntot += (count - (e << 8)) < 256u ? (count - (e << 8)) : 256u;
            const u32 span = mx - mn + 1u;
            if (mx < mn || span > cap) break;
            if (span <= ntot) best = (int)e;
        }
        const u32 eb = best >= 0 ? (u32)best : b;
        if (best < 0) s_dc_sparse1++;
        if (eb > b) { s_dc_multi++; s_dc_mblocks += eb - b + 1u; }
        s_dc_chunks++;
        const u32 o = b << 8, end = ((eb + 1u) << 8) < count ? ((eb + 1u) << 8) : count;
        if (dr) { dr->ends[dr->nch++] = end; s_drw_runact = 1; s_drw_runih = dr_ih; }   /* v223 DRW */
        d3d12_draw_indexed_one(ud, primitive, first + o, end - o);
        s_drw_runact = 0;
        b = eb + 1u;
    }
    s_dc_reuse_active = 0;
    if (dr) { dr->ih = dr_ih; dr->id = dr_id; }   /* v223 DRW: chunk boundaries of this run */
}
static void dc_report(void)
{
    extern unsigned long long g_gcm_dcoal_runs, g_gcm_dcoal_words;
    extern volatile int g_abx;
    if (!s_dc_runs && !g_gcm_dcoal_runs) return;
    fprintf(stderr, "[DCOAL] last 600 frames: %llu runs (%llu words -> %.1f/frame saved calls), %.0f indices/run; %llu re-split; "
                    "%llu blocks -> %llu chunks (%llu multi-block covering %llu blocks, %llu sparse single blocks); "
                    "decoded-run reuse %llu chunks / %.1f M indices (abx 0x%X)%c",
            (unsigned long long)g_gcm_dcoal_runs, (unsigned long long)g_gcm_dcoal_words,
            (double)(g_gcm_dcoal_words - g_gcm_dcoal_runs) / 600.0, s_dc_runs ? (double)s_dc_idx / (double)s_dc_runs : 0.0,
            (unsigned long long)s_dc_split, (unsigned long long)s_dc_blocks, (unsigned long long)s_dc_chunks,
            (unsigned long long)s_dc_multi, (unsigned long long)s_dc_mblocks, (unsigned long long)s_dc_sparse1,
            (unsigned long long)s_dc_reuse_chunks, (double)s_dc_reuse_indices / 1000000.0, (unsigned)g_abx, 10);
    g_gcm_dcoal_runs = g_gcm_dcoal_words = 0;
    s_dc_runs = s_dc_idx = s_dc_split = s_dc_chunks = s_dc_multi = s_dc_mblocks = s_dc_sparse1 = s_dc_blocks = 0;
    s_dc_reuse_chunks = s_dc_reuse_indices = 0;
}

static void d3d12_bind_texture(void* ud, u32 unit, const rsx_texture_state* tex)
{
    (void)ud;
    extern uint8_t* vm_base;
    extern u32 cellGcmResolveOffset(u32);

    u32 width  = (tex->image_rect >> 16) & 0xFFFF;
    u32 height = tex->image_rect & 0xFFFF;
    u32 format = (tex->format >> 8) & 0xFF;
    u32 offset = tex->offset;
    /* CUBEDBG=1: NV4097_SET_TEXTURE_FORMAT bit 2 is the cubemap flag, and the
     * line above throws it away with the rest of the low byte. Nothing in this
     * backend handles cube textures, so a cubemap bound for an environment
     * reflection is sampled as a plain 2D image -- which is what the chrome
     * faucet's black patches and banded escutcheons look like. */
    { static int cd = -1;
      if (cd < 0) { const char* e = getenv("CUBEDBG"); cd = e ? atoi(e) : 0; }
      if (cd && (tex->format & 4)) {
          static u32 seen[8]; static int ns = 0; int known = 0;
          for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
          if (!known && ns < 8) { seen[ns++] = offset;
              fprintf(stderr, "[CUBE] unit=%u offset=0x%08X %ux%u fmt=0x%02X"
                              " raw_format=0x%08X (cubemap bit SET)%c",
                      unit, offset, width, height, format, tex->format, 10); }
      } }

    static int log_count = 0;
    if (log_count < 10) {
        printf("[D3D12] bind_texture(unit=%u, offset=0x%X, fmt=0x%02X, %ux%u)\n",
               unit, offset, format, width, height);
        log_count++;
    }
    /* MOVIE_BIND=1: trace movie-plane binds (640x360 Y / 320x180 U/V) with the
     * resolved EA + a content probe -- used to diagnose the Bink frame-buffer
     * ring mismatch (the draw binds a cleared buffer 0x4D80 before the one the
     * decoder actually fills). */
    if (getenv("MOVIE_BIND") &&
        ((width == 640 && height == 360) || (width == 320 && height == 180))) {
        extern u32 cellGcmResolveLocated(int local, u32 offset);
        static int _mv = 0; if ((_mv < 24 && _mv++ < 24)) {
            /* Resolve the SAME raw offset both ways -- as LOCAL (VRAM) and as MAIN
             * (IO-mapped) -- and count nonzero bytes over the whole plane for each.
             * If the plane the game declares (loc bits fmt&3) is zero but the OTHER
             * pool has content, the decoder wrote to a different memory space than
             * our texture resolve picked (the local-vs-main mismatch). */
            u32 loc = tex->format & 3;               /* 1=LOCAL, 2=MAIN */
            u32 ea_loc  = cellGcmResolveLocated(1, offset);
            u32 ea_main = cellGcmResolveLocated(0, offset);
            u32 plane = width * height;
            u32 nz_loc = 0, nz_main = 0;
            if (vm_base) for (u32 i = 0; i < plane; i += 137) {
                if (vm_base[ea_loc  + i]) nz_loc++;
                if (vm_base[ea_main + i]) nz_main++;
            }
            fprintf(stderr, "[mv-bind] unit=%u fmt=0x%02X loc=%u %ux%u off=0x%X | LOCAL ea=0x%08X nz=%u | MAIN ea=0x%08X nz=%u\n",
                    unit, format, loc, width, height, offset,
                    ea_loc, nz_loc, ea_main, nz_main);
        }
    }

    if (!vm_base || width == 0 || height == 0) return;

    /* Record the currently-bound atlas so subsequent quad draws sample it. The
     * actual GPU upload happens in render_frame (we have no open command list
     * here). Only the 8-bit single-channel font atlas (B8, RSX fmt base 0x81 /
     * as-seen 0xA1 with the LN flag) is wired up so far; other formats fall
     * back to untextured. */
    u32 base_fmt = format & 0x9F;   /* strip LN(0x20)/UN(0x40) flags */
    if (unit < RSX_D3D_TEX_UNITS)   /* guest filter/wrap -> sampler key (SAMP_GUEST) */
        s_d3d.cur_texs[unit].samp = samp_key_from_regs(tex->filter, tex->address, tex->control0)
                                  | samp_border_bits(tex->border_color);   /* v154 */
    /* Formats the VP path does not take leave cur_texs[unit] holding the PREVIOUS
     * texture (stale binding). Log each such format once so a title's missing
     * formats are visible (2026-10-04 v27, in-game texture investigation). */
    if (unit < RSX_D3D_TEX_UNITS &&
        !(base_fmt == 0x81 || base_fmt == 0x85 || base_fmt == 0x8B || base_fmt == 0x95 /* v41 */ ||
          (base_fmt >= 0x86 && base_fmt <= 0x88) || base_fmt == 0x9A ||
          (base_fmt >= 0x90 && base_fmt <= 0x93 && (shadow_lit_on() || shadows_on())))) {
        static u32 useen[32]; static int un = 0; int known = 0;
        for (int k = 0; k < un; k++) if (useen[k] == base_fmt) known = 1;
        if (!known && un < 32) { useen[un++] = base_fmt;
            fprintf(stderr, "[TEXFMT-UNSUP] unit=%u fmt=0x%02X (raw 0x%02X) %ux%u off=0x%X filter=0x%08X addr=0x%08X -- unit keeps its previous texture%c",
                    unit, base_fmt, format, width, height, offset, tex->filter, tex->address, 10); }
    }
    { static int fl = 0; if (unit < RSX_D3D_TEX_UNITS && fl < 16) {
        static u32 fseen[16]; int known = 0; u32 k = samp_key_from_regs(tex->filter, tex->address, tex->control0);
        for (int i = 0; i < fl; i++) if (fseen[i] == k) known = 1;
        if (!known) { fseen[fl++] = k;
            fprintf(stderr, "[TEXFILT] unit=%u fmt=0x%02X %ux%u filter=0x%08X addr=0x%08X ctrl0=0x%08X -> key %05X%c",
                    unit, base_fmt, width, height, tex->filter, tex->address, tex->control0, k & 0xFFFFu, 10); } } }
    if (base_fmt >= 0x90 && base_fmt <= 0x93) {   /* v148: what the shadow units expect outside the map */
        static u32 bseen[8]; static int nb = 0; int f = 0;
        for (int i = 0; i < nb; i++) if (bseen[i] == tex->border_color) f = 1;
        if (!f && nb < 8) { bseen[nb++] = tex->border_color;
            fprintf(stderr, "[SHADOW] v148 depth unit %u %ux%u addr 0x%08X border colour 0x%08X (v154: sampler border honoured)%c",
                    unit, width, height, tex->address, tex->border_color, 10); } }
    /* VP path: record the latest bound texture (any supported format) so draws
     * can carry it per-draw. Location bits (format[1:0]): 1 = LOCAL, 2 = MAIN. */
    extern u32 cellGcmResolveLocated(int local, u32 offset);
    if (unit < RSX_D3D_TEX_UNITS &&
        (base_fmt == 0x81 /* B8 */ || base_fmt == 0x85 /* A8R8G8B8 */ ||
         base_fmt == 0x8B /* G8B8: LBP's font atlas */ ||
         base_fmt == 0x95 /* v41 Y16_X16: inFamous' 64x64 lighting lookup -- skipped, the unit kept a stale texture */ ||
         (base_fmt >= 0x86 && base_fmt <= 0x88) /* DXT1/23/45 */ ||
         base_fmt == 0x9A /* W16Z16Y16X16 half-float: RTT intermediates */ ||
         (base_fmt >= 0x90 && base_fmt <= 0x93 && (shadow_lit_on() || shadows_on())) /* depth: shadow maps (v28); v88: also with SHADOWS=1 SHADOW_LIT=0 -- the v77 launchers dropped every shadow-map binding, the sun shader then sampled the unit's stale 512x8 ramp = no sun at all */)) {
        /* TEX_RESOLVE_AUTO=1: resolve through the page tables (local-page map
         * then IO table) instead of trusting the format's location bits. A
         * texture the guest built in main memory but tagged local resolves to
         * untouched VRAM and samples as all-zero -- geometry renders, flat. */
        { static int _ra = -1; if (_ra < 0) _ra = getenv("TEX_RESOLVE_AUTO") ? 1 : 0;
          extern u32 cellGcmResolveIO(u32);
          u32 _r = 0;
          if (_ra) _r = cellGcmResolveIO(offset);          /* IO table first */
          if (!_r) _r = cellGcmResolveLocated((tex->format & 3) == 1, offset);
          /* TEX_PROBE=1: the location bits are not always right. This title
           * binds its menu textures with a location that resolves into main
           * memory, where the bytes are all zero, while the same raw offset
           * resolved as LOCAL lands on real data -- so 100k menu draws sample
           * black and the menu never appears. When the chosen resolution is
           * empty over a sample and the other one is not, take the other. */
          { static int _tp = -1;
            if (_tp < 0) _tp = getenv("TEX_PROBE") ? 1 : 0;
            if (_tp && vm_base && _r) {
                u32 alt = cellGcmResolveLocated((tex->format & 3) != 1, offset);
                if (alt && alt != _r && alt < 0xE0000000u && _r < 0xE0000000u) {
                    u32 span = width * height / 2;
                    if (span > 4096) span = 4096;
                    u32 nz_r = 0, nz_a = 0;
                    for (u32 i = 0; i < span; i += 7) {
                        if (vm_base[_r  + i]) nz_r++;
                        if (vm_base[alt + i]) nz_a++;
                    }
                    if (nz_r == 0 && nz_a > 0) {
                        static u32 seen[32]; static int ns = 0; int known = 0;
                        for (int k = 0; k < ns; k++) if (seen[k] == offset) known = 1;
                        if (!known && ns < 32) { seen[ns++] = offset;
                            fprintf(stderr, "[TEX_PROBE] 0x%08X: %s empty, using %s"
                                            " (%u non-zero)%c", offset,
                                    ((tex->format & 3) == 1) ? "local" : "main",
                                    ((tex->format & 3) == 1) ? "main" : "local",
                                    nz_a, 10); }
                        _r = alt;
                    }
                }
            } }
          s_d3d.cur_texs[unit].off = _r; }
        s_d3d.cur_texs[unit].raw = offset;
        s_d3d.cur_texs[unit].w = width; s_d3d.cur_texs[unit].h = height;
        s_d3d.cur_texs[unit].fmt = format;   /* full byte: LN(0x20)/UN(0x40) kept */
        s_d3d.cur_texs[unit].ctrl1 = tex->control1;
        s_d3d.cur_texs[unit].zf    = (tex->address >> 28) & 0xFu;   /* v34 depth-compare func */
        {   /* v89 TEX_XF: bits 8-9 = unit mode (1 = unsigned remap BIASED, 2 = gamma/sRGB on RGB) */
            const u32 ur = (tex->address >> 12) & 0xFu, ga = (tex->address >> 20) & 0xFu, sr = (tex->address >> 24) & 0xFu;
            u32 m = (ur == 1u ? 1u : 0u) | (ga ? 2u : 0u);
            if (base_fmt >= 0x90 && base_fmt <= 0x93) m = 0;   /* depth compares: never */
            s_d3d.cur_texs[unit].zf |= m << 8;
            if (ur || ga || sr) { static u32 seen[48]; static int ns = 0; u32 key = (unit << 24) | (base_fmt << 16) | (ur << 8) | (ga << 4) | sr; int k2 = 0;
                for (int i = 0; i < ns; i++) if (seen[i] == key) k2 = 1;
                if (!k2 && ns < 48) { seen[ns++] = key;
                    fprintf(stderr, "[TEX_XF] unit %u fmt 0x%02X %ux%u: unsigned_remap %u gamma 0x%X signed_remap 0x%X (address 0x%08X) -> mode %u%c",
                            unit, base_fmt, width, height, ur, ga, sr, tex->address, m, 10); } }
        }
        s_d3d.cur_texs[unit].cube  = (tex->format & 4) ? 1 : 0;
        s_d3d.cur_texs[unit].mips  = (tex->format >> 16) & 0xFFFFu;
        if (getenv("TEXFMTDBG")) { static u32 seen[64]; static int ns=0;
            u32 k = (unit<<24) | (((tex->format>>8) & 0xFFu)<<16) | (width & 0xFFFFu);
            int f=0; for (int i=0;i<ns;i++) if (seen[i]==k) f=1;
            if (!f && ns < 64) { seen[ns++]=k;
                fprintf(stderr, "[TEXFMT] unit=%u fmt=0x%02X %ux%u mips=%u cube=%d off=0x%X%c",
                        unit, (tex->format >> 8) & 0xFFu, width, height,
                        (tex->format>>16)&0xFFFFu, (tex->format&4)?1:0, offset, 10); } }
        s_d3d.cur_texs[unit].set = 1;
    }
    if (base_fmt == 0x81 /* B8 */) {
        s_d3d.tex_src_offset = cellGcmResolveLocated((tex->format & 3) == 1, offset);
        if (s_d3d.tex_w != width || s_d3d.tex_h != height) {
            /* dims changed -> resource must be (re)created in render_frame */
            s_d3d.tex_ready = 0;
        }
        s_d3d.tex_w     = width;
        s_d3d.tex_h     = height;
        s_d3d.tex_bound = 1;
        s_d3d.tex_dirty = 1;
    } else {
        s_d3d.tex_bound = 0;
    }
}

static void d3d12_set_vertex_attribs(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;
    s_vsattr_chg = 1;   /* v221C: the attribute layout may have changed (vp_record_cb re-writes s_vs_attr) */

    /* Log enabled vertex attributes for debugging. RSX_VTXDBG=<N> raises the
     * cap: the fixed 5 were all consumed by boot-time setup, so the layouts the
     * real draws use were never printed. */
    static int log_count = 0, log_cap = -1;
    if (log_cap < 0) { const char* e = getenv("RSX_VTXDBG"); log_cap = e ? atoi(e) : 5; }
    if (log_count < log_cap) {
        printf("[D3D12] set_vertex_attribs:\n");
        for (int i = 0; i < 16; i++) {
            const rsx_vertex_attrib* a = &state->vertex_attribs[i];
            if (a->enabled) {
                const char* type_name = "?";
                switch (a->type) {
                case 1: type_name = "snorm16"; break;
                case 2: type_name = "float32"; break;
                case 3: type_name = "float16"; break;
                case 4: type_name = "ubyte"; break;
                case 5: type_name = "s16"; break;
                case 7: type_name = "ubyte256"; break;
                }
                printf("  attrib[%d]: %s x%u, stride=%u, offset=0x%X\n",
                       i, type_name, a->size, a->stride, a->offset);
            }
        }
        log_count++;
    }
}

static void d3d12_set_shader(void* ud, const rsx_state* state)
{
    (void)ud;
    s_d3d.current_rsx_state = state;

    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_shader: fp_addr=0x%08X, vp_load=%u, output_mask=0x%08X\n",
               state->fragment_program_addr, state->transform_program_load,
               state->vertex_attrib_output_mask);
        log_count++;
    }

    /* TODO: Look up or compile a PSO matching this shader combination.
     * For now we use the basic vertex-colored PSO for everything. */
}

static void d3d12_set_blend(void* ud, const rsx_state* state)
{
    (void)ud;
    /* TODO: modify PSO blend state or use dynamic state.
     * D3D12 requires PSO recreation for blend state changes,
     * so we'd need a PSO cache keyed by blend configuration. */
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_blend(enable=%d, sfactor=0x%X, dfactor=0x%X)\n",
               state->blend_enable, state->blend_sfactor, state->blend_dfactor);
        log_count++;
    }
}

static void d3d12_set_depth_stencil(void* ud, const rsx_state* state)
{
    (void)ud;
    static int log_count = 0;
    if (log_count < 5) {
        printf("[D3D12] set_depth_stencil(depth=%d, stencil=%d, func=0x%X)\n",
               state->depth_test_enable, state->stencil_test_enable,
               state->depth_func);
        log_count++;
    }
}

/* ---------------------------------------------------------------------------
 * Backend registration
 * -----------------------------------------------------------------------*/

static rsx_backend s_d3d12_backend = {0};

/* ---------------------------------------------------------------------------
 * Public API
 * -----------------------------------------------------------------------*/

int rsx_d3d12_backend_init(u32 width, u32 height, const char* title)
{
    if (!s_d3d.draws) s_d3d.draws = s_draw_buf[0];   /* v59 */
    memset(&s_d3d, 0, sizeof(s_d3d));
    s_d3d.width = width;
    s_d3d.height = height;
    {   /* v154 RES_SCALE */
        const char* e = getenv("RES_SCALE");
        float sc = e ? (float)atof(e) : 1.0f;
        if (!(sc >= 1.0f)) sc = 1.0f;
        if (sc > 3.0f) sc = 3.0f;
        s_res_scale = sc;
        s_bbw = ((u32)((float)width * sc + 0.5f) + 1u) & ~1u;
        s_bbh = ((u32)((float)height * sc + 0.5f) + 1u) & ~1u;
        if (sc <= 1.0f) { s_bbw = width; s_bbh = height; }
        fprintf(stderr, "[RES] v154 internal resolution %ux%u (RES_SCALE %.2f, guest %ux%u)%c", s_bbw, s_bbh, sc, width, height, 10);
#ifdef PS3RECOMP_DLSS
        {   /* DLSS upscaling (DLSS_MODE quality / balanced / performance): the game renders at the mode's share of that
             * size, never below the PS3's own 720p, and DLSS rebuilds the image at the full size */
            float rs = sc * rsx_dlss_render_ratio();
            if (rs < 1.0f) rs = 1.0f;
            if (rs < sc - 0.001f) {
                s_outw = s_bbw; s_outh = s_bbh; s_out_scale = sc;
                s_res_scale = rs;
                s_bbw = ((u32)((float)width * rs + 0.5f) + 1u) & ~1u;
                s_bbh = ((u32)((float)height * rs + 0.5f) + 1u) & ~1u;
                if (rs <= 1.0f) { s_bbw = width; s_bbh = height; }
                fprintf(stderr, "[RES] DLSS upscaling: rendering %ux%u, output %ux%u%c", s_bbw, s_bbh, s_outw, s_outh, 10);
            }
        }
#endif
    }
    s_d3d.clear_color[0] = 0.0f;
    s_d3d.clear_color[1] = 0.0f;
    s_d3d.clear_color[2] = 0.1f;
    s_d3d.clear_color[3] = 1.0f;

    /* Debug: dump the first N presented frames to BMP if CELLMARK_DUMP is set
     * (its numeric value when > 1, else 24). */
    {
        const char* dv = getenv("CELLMARK_DUMP");
        int n = dv ? atoi(dv) : 0;
        s_d3d.dump_frames_left = dv ? (n > 1 ? n : 24) : 0;
        /* CELLMARK_DUMP_SKIP=N: ignore the first N presents before dumping.
         * The opening frames are the loading screen; the interesting content
         * only appears once the sim SPUs have advanced the scene. */
        { const char* sv = getenv("CELLMARK_DUMP_SKIP");
          s_d3d.dump_skip_left = sv ? atoi(sv) : 0; }
    }

    /* Create window */
    {
        extern char g_rsx_title_base[128];
        snprintf(g_rsx_title_base, sizeof(g_rsx_title_base), "%s",
                 title ? title : "ps3recomp");
    }
    s_d3d.hwnd = create_window(BBW, BBH, title);   /* v154: window at the internal resolution (clamped to the screen) */
    {   /* v153: raw mouse input for mouse-look (PAD_RAWINPUT=0 = cursor deltas as before) */
        const char* e = getenv("PAD_RAWINPUT");
        if (s_d3d.hwnd && !(e && e[0] == '0')) {
            RAWINPUTDEVICE rid;
            rid.usUsagePage = 0x01; rid.usUsage = 0x02; rid.dwFlags = 0; rid.hwndTarget = s_d3d.hwnd;
            BOOL ok = RegisterRawInputDevices(&rid, 1, sizeof(rid));
            fprintf(stderr, "[d3d12] v153 raw mouse input %s\n", ok ? "registered" : "FAILED (cursor deltas)");
        }
    }
    if (!s_d3d.hwnd) {
        printf("[D3D12] ERROR: Window creation failed\n");
        return -1;
    }

    /* Initialize D3D12 */
    if (init_d3d12(width, height) != 0) {
        printf("[D3D12] ERROR: D3D12 initialization failed\n");
        return -1;
    }

    /* Set up backend callbacks */
    s_d3d12_backend.userdata          = &s_d3d;
    s_d3d12_backend.init              = d3d12_init;
    s_d3d12_backend.shutdown          = d3d12_shutdown;
    s_d3d12_backend.begin_frame       = d3d12_begin_frame;
    s_d3d12_backend.end_frame         = d3d12_end_frame;
    s_d3d12_backend.present           = d3d12_present;
    s_d3d12_backend.clear             = d3d12_clear;
    s_d3d12_backend.set_render_target = d3d12_set_render_target;
    s_d3d12_backend.set_viewport      = d3d12_set_viewport;
    s_d3d12_backend.set_blend         = d3d12_set_blend;
    s_d3d12_backend.set_depth_stencil = d3d12_set_depth_stencil;
    s_d3d12_backend.set_shader        = d3d12_set_shader;
    s_d3d12_backend.set_vertex_attribs = d3d12_set_vertex_attribs;
    s_d3d12_backend.draw_arrays       = d3d12_draw_arrays;
    s_d3d12_backend.draw_indexed      = d3d12_draw_indexed;
    { extern volatile int g_rsx_dcoal_ok; g_rsx_dcoal_ok = 1; }   /* v221C DCOAL: d3d12_draw_indexed chunks counts > 256 itself */
    s_d3d12_backend.bind_texture      = d3d12_bind_texture;

    rsx_set_backend(&s_d3d12_backend);

    s_d3d.initialized = 1;
    s_d3d.last_fps_time = GetTickCount64();

    printf("[D3D12] Backend ready: %ux%u\n", width, height);
    return 0;
}

void rsx_d3d12_backend_shutdown(void)
{
    if (!s_d3d.initialized) return;
    { int g = 0; while (s_rf_busy && (g < 500 && g++ < 500)) Sleep(10); }   /* v59: let the render thread finish */

    wait_for_gpu();

    /* Release D3D12 resources */
    if (s_d3d.vertex_buffer) {
        s_d3d.vertex_buffer->lpVtbl->Unmap(s_d3d.vertex_buffer, 0, NULL);
        s_d3d.vertex_buffer->lpVtbl->Release(s_d3d.vertex_buffer);
    }
    if (s_d3d.pipeline_state)        s_d3d.pipeline_state->lpVtbl->Release(s_d3d.pipeline_state);
    if (s_d3d.pipeline_state_lines)  s_d3d.pipeline_state_lines->lpVtbl->Release(s_d3d.pipeline_state_lines);
    if (s_d3d.pipeline_state_points) s_d3d.pipeline_state_points->lpVtbl->Release(s_d3d.pipeline_state_points);
    if (s_d3d.depth_buffer) s_d3d.depth_buffer->lpVtbl->Release(s_d3d.depth_buffer);
    if (s_d3d.dsv_heap)     s_d3d.dsv_heap->lpVtbl->Release(s_d3d.dsv_heap);
    if (s_d3d.root_signature) s_d3d.root_signature->lpVtbl->Release(s_d3d.root_signature);
    if (s_d3d.fence) s_d3d.fence->lpVtbl->Release(s_d3d.fence);
    if (s_d3d.fence_event) CloseHandle(s_d3d.fence_event);
    if (s_d3d.cmd_list) s_d3d.cmd_list->lpVtbl->Release(s_d3d.cmd_list);
    for (u32 i = 0; i < FRAME_COUNT; i++) {
        if (s_d3d.cmd_allocators[i]) s_d3d.cmd_allocators[i]->lpVtbl->Release(s_d3d.cmd_allocators[i]);
        if (s_d3d.render_targets[i]) s_d3d.render_targets[i]->lpVtbl->Release(s_d3d.render_targets[i]);
    }
    if (s_d3d.rtv_heap) s_d3d.rtv_heap->lpVtbl->Release(s_d3d.rtv_heap);
    if (s_d3d.swap_chain) s_d3d.swap_chain->lpVtbl->Release(s_d3d.swap_chain);
    if (s_d3d.cmd_queue) s_d3d.cmd_queue->lpVtbl->Release(s_d3d.cmd_queue);
    if (s_d3d.device) s_d3d.device->lpVtbl->Release(s_d3d.device);

    if (s_d3d.hwnd) DestroyWindow(s_d3d.hwnd);

    rsx_set_backend(NULL);
    s_d3d.initialized = 0;

    printf("[D3D12] Backend shut down\n");
}

int rsx_d3d12_backend_pump_messages(void)
{
    MSG msg;
    rf_apply_title();   /* v59 */
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return -1;
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return s_d3d.window_closed ? -1 : 0;
}

/* v102c [FRAME-STALL]: rate-limited (first 60, then one per 5 s) snapshot of why no frame ended. */
static void frame_stall_report(const char* why)
{
    static u32 n = 0; static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    if (n >= 60 && now - last < 5000) return;
    n++; last = now;
    extern unsigned cellGcm_flip_request_count(void);
    extern int cellGcmOffsetIsDisplay(u32 offset);
    const unsigned fc = cellGcm_flip_request_count();
    u32 offs[10] = {0}, dr_n[10] = {0}, cl_n[10] = {0}, disp_draws = 0, disp_clears = 0; int nd = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
        const D3D12DrawRecord* r = &s_d3d.draws[i];
        if (r->rt_off == 0) { if (r->is_clear) disp_clears++; else disp_draws++; }
        int j = 0; while (j < nd && offs[j] != r->rt_off) j++;
        if (j == nd && nd < 10) { offs[nd] = r->rt_off; dr_n[nd] = cl_n[nd] = 0; nd++; }
        if (j < nd) { if (r->is_clear) cl_n[j]++; else dr_n[j]++; }
    }
    const rsx_state* st = s_d3d.current_rsx_state;
    fprintf(stderr, "[FRAME-STALL] #%u %s: %llu ms since the last frame (render_frame #%u), flips %u since it (count %u,"
                    " last clear-present flip %u), ticker presents skipped %u, forced %u, clear boundaries forced %u (v103A); batch %u records (cap %u), display draws %u"
                    " clears %u, frame max %u (clear needs >= %u); clears since: offscreen %u, display-but-no-display-draws %u,"
                    " same-flip %u, too-small %u, RSX_ACCUM %u; render thread %s; cur surface tgt=0x%X off=0x%X/0x%X clip=%ux%u"
                    " isdisp=%d/%d; batch targets (rt_off draws/clears):",
            n, why, (unsigned long long)(s_fs_rf_tick ? now - s_fs_rf_tick : 0), s_fs_rf_calls, fc - s_fs_flip_at_rf, fc,
            s_fs_last_present_flip_mirror, s_fs_ticker_skips, s_fs_forced, s_fs_clr_forced, s_d3d.draw_count, s_max_draws, disp_draws,
            disp_clears, s_fs_frame_max_mirror, s_fs_frame_max_mirror > 16 ? s_fs_frame_max_mirror / 4 : 0,
            s_fs_clr_off, s_fs_clr_nodisp, s_fs_clr_sameflip, s_fs_clr_small, s_fs_clr_accum,
            s_rf_busy ? "BUSY" : "idle",
            st ? st->color_target : 0, st ? st->surface_color_offset[0] : 0, st ? st->surface_color_offset[1] : 0,
            st ? st->surface_clip_w : 0, st ? st->surface_clip_h : 0,
            st ? cellGcmOffsetIsDisplay(st->surface_color_offset[0]) : -1,
            st ? cellGcmOffsetIsDisplay(st->surface_color_offset[1]) : -1);
    for (int j = 0; j < nd; j++) fprintf(stderr, " 0x%X %u/%u", offs[j], dr_n[j], cl_n[j]);
    fprintf(stderr, "%c", 10);
    fflush(stderr);
}
/* v102c [FRAMESIG]: the render-target structure of each executed frame (distinct colour targets in
 * order of first use with draw counts, display draws, clears). A line is printed whenever the
 * structure differs from the last 16 seen (max 400 lines): mode switches such as the Triangle
 * "look at" camera show up as new signatures next to [WKL]/[FRAME-STALL] lines. FRAMESIG=0 off. */
static void framesig_note(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("FRAMESIG"); on = (e && *e == '0') ? 0 : 1; }
    if (!on || s_d3d.draw_count == 0) return;
    u32 offs[12], dn[12], cn[12]; int nd = 0; u32 dd = 0;
    for (u32 i = 0; i < s_d3d.draw_count && i < MAX_DRAWS; i++) {
        const D3D12DrawRecord* r = &s_d3d.draws[i];
        if (r->rt_off == 0 && !r->is_clear) dd++;
        int j = 0; while (j < nd && offs[j] != r->rt_off) j++;
        if (j == nd) { if (nd >= 12) continue; offs[nd] = r->rt_off; dn[nd] = cn[nd] = 0; nd++; }
        if (r->is_clear) cn[j]++; else dn[j]++;
    }
    u64 h = 1469598103934665603ull;
    for (int j = 0; j < nd; j++) { h = (h ^ offs[j]) * 1099511628211ull; h = (h ^ (dn[j] ? 1u : 0u) ^ (cn[j] ? 2u : 0u)) * 1099511628211ull; }
    h = (h ^ (dd ? 7u : 0u)) * 1099511628211ull;
    static u64 seen[16]; static int ns = 0, nlog = 0;
    for (int k = 0; k < ns; k++) if (seen[k] == h) return;
    seen[ns < 16 ? ns++ : (nlog % 16)] = h;
    if (nlog >= 400) return;
    nlog++;
    fprintf(stderr, "[FRAMESIG] #%d t=%llu ms frame %u: %u records, display draws %u, targets (rt_off draws/clears):",
            nlog, (unsigned long long)GetTickCount64(), s_fs_rf_calls, s_d3d.draw_count, dd);
    for (int j = 0; j < nd; j++) fprintf(stderr, " 0x%X %u/%u", offs[j], dn[j], cn[j]);
    fprintf(stderr, "%c", 10);
}
void rsx_d3d12_backend_present(void)
{
    if (blink_dbg())
        printf("[PRESENT] draws=%u clears_since_last=%u clear_presents=%u\n",
               s_d3d.draw_count, s_dbg_clears_since_present, s_clear_presents);
    s_dbg_clears_since_present = 0;

    /* Once frame-boundary presents are active (d3d12_clear presents each
     * completed frame as the drain crosses into the next one), the ticker
     * present would only ever show the partially-accumulated NEXT frame --
     * that partial present right after the FIFO ring recycle was the visible
     * blink. Keep the ticker present solely as the boot-time fallback (before
     * the first framed clear arrives). */
    if (s_clear_presents > 0) {
        /* v102c FRAME_STALL_FLIPS (default 3; 0 = old: the ticker never executes once clear boundaries
         * are active). The ticker runs on every guest flip. Normally a clear boundary executes each frame
         * one flip later; if FRAME_STALL_FLIPS flips pass with no render_frame() at all (or the record
         * buffer is full), the clear-boundary heuristic has stopped matching this title's frames: fall
         * through and execute the batch here (Present only if it holds display draws), so a frame can
         * never accumulate for more than a few flips. [FRAME-STALL] says which clear test failed. */
        static int s_lim = -1;
        if (s_lim < 0) { const char* e = getenv("FRAME_STALL_FLIPS"); s_lim = e ? atoi(e) : 3; if (s_lim < 0) s_lim = 0;
            fprintf(stderr, "[FRAME-STALL] v102c ticker fallback after %d flips without a frame (FRAME_STALL_FLIPS, 0 = off)%c", s_lim, 10); }
        /* Once it had to step in, keep ending frames at EVERY flip (like the boot-time ticker path) until
         * a clear boundary presents again -- a mode the heuristic cannot follow would otherwise run at one
         * merged 3-frame batch per 3 flips. */
        static u32 s_cp_seen = 0; static int s_flip_mode = 0;
        if (s_clear_presents != s_cp_seen) { s_cp_seen = s_clear_presents;
            if (s_flip_mode) fprintf(stderr, "[FRAME-STALL] clear boundaries are back (clear present #%u) -> ticker fallback idle again%c", s_clear_presents, 10);
            s_flip_mode = 0; }
        extern unsigned cellGcm_flip_request_count(void);
        const unsigned fl = cellGcm_flip_request_count() - s_fs_flip_at_rf;
        const unsigned need = s_flip_mode ? 1u : (unsigned)s_lim;
        if (!s_lim || !s_d3d.initialized || s_d3d.draw_count == 0 ||
            (fl < need && s_d3d.draw_count < s_max_draws)) { s_fs_ticker_skips++; return; }
        s_fs_forced++;
        frame_stall_report(s_flip_mode ? "ticker fallback (per-flip mode): executing the batch" : "ticker fallback: executing the accumulated batch");
        s_flip_mode = 1;
    }

    /* Same display gate as d3d12_present: a batch of offscreen pass work only
     * (render-to-texture) keeps accumulating until its composite arrives.
     * Empty batches present only until the first real frame -- after that an
     * empty present is a flip/drain race and wipes the screen for a frame
     * (wave: black flashes and layout flicker between frames). */
    static int s_seen_content = 0;
    int has_display = (s_d3d.draw_count == 0 && !s_seen_content);
    if (!has_display && recs_have_display()) has_display = 1;   /* v219l */
    /* The guest-framebuffer present needs the pass to run at all. */
    { static int gfb = -1;
      if (gfb < 0) { const char* e = getenv("GCM_GUEST_FB"); gfb = e ? 1 : 0; }
      if (gfb) has_display = 1; }
    if (s_composite_src) has_display = 1;
    if (s_split_disp) has_display = 1;   /* v103A: an earlier split part of this frame drew into a display buffer */
    if (has_display && s_d3d.draw_count > 0) s_seen_content = 1;

    /* VP_SUBMIT=<N>: has_display gates render_frame() entirely, so a batch whose
     * records all target an OFFSCREEN rt (rt_off != 0) presents without ever
     * running the draw pass -- the backbuffer then shows only the clear, which
     * reads as "nothing rasterizes" from every downstream check. */
    { static int cap = -1, n = 0;
      if (cap < 0) { const char* e = getenv("VP_SUBMIT"); cap = e ? atoi(e) : 0; }
      if (cap && n < cap && s_d3d.draw_count) { n++;
        u32 onscreen = 0, offscreen = 0, clears = 0;
        for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
            if (s_d3d.draws[_i].is_clear) { clears++; continue; }
            if (s_d3d.draws[_i].rt_off) offscreen++; else onscreen++;
        }
        { /* Which surfaces the batch's draws actually target -- "offscreen=57"
           * does not say whether they all go to one RT or scatter. */
          u32 offs[8]; u32 cnt[8]; int nd = 0;
          for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
              if (s_d3d.draws[_i].is_clear) continue;
              u32 o = s_d3d.draws[_i].rt_off; int f = -1;
              for (int _j = 0; _j < nd; _j++) if (offs[_j] == o) { f = _j; break; }
              if (f < 0) { if (nd < 8) { offs[nd] = o; cnt[nd] = 1; nd++; } }
              else cnt[f]++;
          }
          { u32 vp = 0, novp = 0;
            for (u32 _i = 0; _i < s_d3d.draw_count && _i < MAX_DRAWS; _i++) {
                if (s_d3d.draws[_i].is_clear) continue;
                if (s_d3d.draws[_i].is_vp) vp++; else novp++;
            }
            /* The execution loop skips anything without is_vp, so a batch full
             * of !is_vp records is counted, gated and then never drawn. */
            fprintf(stderr, "[PRESENTGATE] is_vp=%u not_vp=%u%c", vp, novp, 10); }
          fprintf(stderr, "[PRESENTGATE] targets:");
          for (int _j = 0; _j < nd; _j++)
              fprintf(stderr, " 0x%08X x%u", offs[_j], cnt[_j]);
          fprintf(stderr, "%c", 10); }
        fprintf(stderr, "[PRESENTGATE] records=%u onscreen=%u offscreen=%u clears=%u"
                        " has_display=%d seen_content=%d -> render_frame=%s\n",
                s_d3d.draw_count, onscreen, offscreen, clears, has_display,
                s_seen_content, (s_d3d.initialized && has_display) ? "YES" : "SKIPPED"); } }

    /* Execute the batch whenever it has draws. Gating the whole call on
     * has_display meant a render-to-texture pass -- every draw targeting an
     * offscreen surface -- was DISCARDED rather than deferred, so the texture
     * it produces was never written and whatever sampled it later read an
     * empty resource. Only the Present needs onscreen content. */
    if (s_d3d.initialized && (has_display || s_d3d.draw_count > 0)) {
        { extern void rsx_reset_upload_claims(void);
          static int remap = -1;
          if (remap < 0) { const char* e = getenv("TEX_REMAP"); remap = e ? atoi(e) : 0; }
          if (remap) rsx_reset_upload_claims(); }
        s_present_this_frame = has_display;
        render_frame();
        s_present_this_frame = 1;
    }
}

static void d3d12_dred_report(void)
{
    if (!s_d3d.device) return;
    ID3D12DeviceRemovedExtendedData* dred = NULL;
    if (FAILED(s_d3d.device->lpVtbl->QueryInterface(s_d3d.device,
            &IID_ID3D12DeviceRemovedExtendedData, (void**)&dred)) || !dred) {
        printf("[DRED] no DRED data (run with D3D12_DRED=1)\n");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc = {0};
    if (SUCCEEDED(dred->lpVtbl->GetAutoBreadcrumbsOutput(dred, &bc))) {
        int nodes = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && nodes < 8; n = n->pNext, nodes++) {
            UINT32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            UINT32 draws_done = 0, draws_total = 0;
            for (UINT32 i = 0; i < n->BreadcrumbCount; i++) {
                int op = (int)n->pCommandHistory[i];
                if (op == D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED ||
                    op == D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED) {
                    draws_total++;
                    if (i < last) draws_done++;
                }
            }
            printf("[DRED] cmdlist %p: %u ops, last completed op index %u -> %u of %u draws completed%s\n",
                   (void*)n->pCommandList, n->BreadcrumbCount, last, draws_done, draws_total,
                   (last < n->BreadcrumbCount) ? "  <== this list did not finish" : "");
            if (last < n->BreadcrumbCount) {
                UINT32 lo = last > 8 ? last - 8 : 0, hi = last + 4 < n->BreadcrumbCount ? last + 4 : n->BreadcrumbCount;
                for (UINT32 i = lo; i < hi; i++)
                    printf("[DRED]   op[%u] = %d%s\n", i, (int)n->pCommandHistory[i],
                           i == last ? "   <== first NOT completed" : "");
            }
        }
        if (!nodes) printf("[DRED] no breadcrumb nodes\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {0};
    if (SUCCEEDED(dred->lpVtbl->GetPageFaultAllocationOutput(dred, &pf))) {
        printf("[DRED] page fault VA = 0x%016llX%s\n", (unsigned long long)pf.PageFaultVA,
               pf.PageFaultVA ? "" : " (none -- a real hang/timeout, not a bad address)");
        int k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, k++)
            printf("[DRED]   existing alloc at fault: type=%d name=%s\n", (int)a->AllocationType,
                   a->ObjectNameA ? a->ObjectNameA : "?");
        k = 0;
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, k++)
            printf("[DRED]   recently FREED alloc at fault: type=%d name=%s\n", (int)a->AllocationType,
                   a->ObjectNameA ? a->ObjectNameA : "?");
        #define DRED_VA(r, nm) if (r) printf("[DRED]   %-14s VA 0x%016llX\n", nm, \
            (unsigned long long)(r)->lpVtbl->GetGPUVirtualAddress(r));
        DRED_VA(s_d3d.vertex_buffer, "vertex_buffer");
        DRED_VA(s_d3d.vp_vb, "vp_vb");
        DRED_VA(s_d3d.vp_cb, "vp_cb");
        DRED_VA(s_d3d.vp_fpcb, "vp_fpcb");
        #undef DRED_VA
    }
    dred->lpVtbl->Release(dred);
    fflush(stdout);
}

#else /* !_WIN32 */

#include <ps3emu/ps3types.h>   /* u32 (header includes above are inside the _WIN32 guard) */
#include <stdio.h>

/* Stub for non-Windows — D3D12 is Windows-only */
int rsx_d3d12_backend_init(u32 w, u32 h, const char* t)
{
    (void)w; (void)h; (void)t;
    printf("[D3D12] Not available on this platform (use Vulkan backend)\n");
    return -1;
}
void rsx_d3d12_backend_shutdown(void) {}
int rsx_d3d12_backend_pump_messages(void) { return 0; }
void rsx_d3d12_backend_present(void) {}

#endif /* _WIN32 */
