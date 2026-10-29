// native/gpu_skin.cpp - skinned parts drawn from the bind pose on the GPU
// (design notes gpu-skinning-plan.md). The game skins and lights every vertex of an
// animated model's part on the CPU and writes it into the one big vertex
// buffer of format 5 (0066e4d0 -> 0066f790 -> 0068dd70 in the base game). Here
// such a part writes no vertices: its record (palettes, the light worked out
// per part) goes into a GPU buffer, a map says which of the big buffer's
// vertices are its, and the kit's vertex fetch (runtime/native_seam.h)
// computes each vertex in the shader with the same integer arithmetic
// (native/gpu_skin_math.h), so it comes out byte for byte as the game's.
//
// RECOMP_HOMM5_GPU_SKIN: 0 off, 1 on (the launcher's homm5.perf/gpu_skin otherwise, 1 on, 2 off;
// on when never chosen, H5SKIN_DEFAULT_ON), check (the game draws as
// always; each part is also computed here and compared with what the game
// wrote, every 2 s a summary), gpucheck (the game writes its vertices and the
// GPU draws its own; what the GPU computed is compared with what the game
// wrote, every second).
//
// Replaced (Tribes of the East addresses in the game's native/addresses.h):
// the part fill (0066e4d0), the copy of a part's staged vertices into the big
// buffer (0066d950, gfx_low_ram 0) and the big buffer's allocation (00606af0),
// which clears the map of a reused range.
#pragma clang fp contract(off)
#include "guest.h"
#include "native_seam.h"

#ifdef RECOMP_ADDRESS_HEADER
#include RECOMP_ADDRESS_HEADER
#endif
#ifndef H5SKIN_FILL_ORIG /* the base game (games/toe/native/addresses.h names them) */
#define H5SKIN_FILL_ORIG fn_0066e4d0
#define H5SKIN_COPY_ORIG fn_0066d950
#define H5SKIN_ALLOC_ORIG fn_00606af0
#define H5SKIN_ALPHA_FN 0x0068fc20u
#define H5SKIN_ALPHA_A 0x01138ee8u
#define H5SKIN_ALPHA_B 0x01138ef4u
#define H5SKIN_RSQRT 0x01139088u
#define H5SKIN_CBRT 0x01142088u
#define H5SKIN_LOW_RAM 0x010cd1f9u
#define H5SKIN_POSITIONS_ORIG fn_0066f600
#endif
/* On in all three games: check and gpucheck come out clean on the base
 * game's map, the HoF menu and the ToE map (design notes perf-opt.md section 4). */
#ifndef H5SKIN_DEFAULT_ON
#define H5SKIN_DEFAULT_ON 1
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "gpu_skin_math.h"
#include "platform/os.h"

extern "C" {
void H5SKIN_FILL_ORIG(X86 *c);
void H5SKIN_COPY_ORIG(X86 *c);
void H5SKIN_ALLOC_ORIG(X86 *c);
void homm5_skin_part_fill(X86 *c);
void homm5_skin_copy(X86 *c);
void homm5_skin_alloc(X86 *c);
int mods_settings_file_get(const char *key, int64_t *out); // kit/mods/settings_file.h
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
static const char kMath[] = {
#embed "gpu_skin_math.h"
    , 0};
#pragma clang diagnostic pop

// gpucheck: the map word the vertex was made from in place of uv1 (a plain
// copy from the pool), so the check knows which part the GPU drew it for.
static const char kFetchCheck[] = R"MSL(
static inline bool d9_vfetch(uint vid, device const uint *b0, device const uint *b1,
                             device const uint *b2, constant uint *b3, thread uint *w) {
    const uint m = b0[vid];
    if (!m)
        return false;
    h5_skin_vertex(m, b1, b2, b3, w);
    w[5] = m;
    return true;
}
)MSL";

static const char kFetch[] = R"MSL(
static inline bool d9_vfetch(uint vid, device const uint *b0, device const uint *b1,
                             device const uint *b2, constant uint *b3, thread uint *w) {
    const uint m = b0[vid];
    if (!m)
        return false;
    h5_skin_vertex(m, b1, b2, b3, w);
    return true;
}
)MSL";

// The two in Vulkan GLSL, where the math reads d9_b1..d9_b3 itself: its
// pointers are indices into them, all from 0.
static const char kFetchCheckGlsl[] = R"GLSL(
bool d9_vfetch(uint vid, out uint w[8]) {
    const uint m = d9_b0[vid];
    if (m == 0u)
        return false;
    h5_skin_vertex(m, 0u, 0u, 0u, w);
    w[5] = m;
    return true;
}
)GLSL";

static const char kFetchGlsl[] = R"GLSL(
bool d9_vfetch(uint vid, out uint w[8]) {
    const uint m = d9_b0[vid];
    if (m == 0u)
        return false;
    h5_skin_vertex(m, 0u, 0u, 0u, w);
    return true;
}
)GLSL";

namespace {

enum Mode { OFF = 0, ON = 1, CHECK = 2, GPUCHECK = 3 };

int skin_mode() {
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_GPU_SKIN");
        int64_t chosen = 0; // the launcher's box: 1 on, 2 off
        mods_settings_file_get("homm5.perf/gpu_skin", &chosen);
        mode = !v || !*v ? (chosen == 2 ? OFF : chosen == 1 || H5SKIN_DEFAULT_ON ? ON : OFF)
               : v[0] == '0' ? OFF
               : !strcmp(v, "check") ? CHECK
               : !strcmp(v, "gpucheck") ? GPUCHECK
                                        : ON;
        fprintf(stderr, "[gpu-skin] mode %d\n", mode);
    }
    return mode;
}

int64_t now_ms() {
    return (int64_t)(os_monotonic_ns() / 1000000);
}

float rdf(uint32_t a) {
    const uint32_t w = rd32(a);
    float f;
    memcpy(&f, &w, 4);
    return f;
}
uint32_t fbits(float f) {
    uint32_t w;
    memcpy(&w, &f, 4);
    return w;
}
// FISTP of a float, rounded the way the guest's control word says (the game
// lights its parts under 0x0c7f: toward zero, not to nearest): 0x80000000
// when it does not fit.
int32_t fistp(const X86 *c, float x) {
    const double r = fround_cw(c, (double)x);
    if (!(r >= -2147483648.0 && r < 2147483648.0))
        return (int32_t)0x80000000u;
    return (int32_t)r;
}
uint32_t pack2(int32_t lo, int32_t hi) {
    return (uint32_t)(uint16_t)lo | (uint32_t)(uint16_t)hi << 16;
}

// ---- buffers ----------------------------------------------------------------

constexpr uint32_t POOL_VERTS = 1u << 19;  // 512k mesh vertices, 32 MB
constexpr uint32_t SLOTS = 16384;          // part records
constexpr uint32_t PAL_WORDS = 4u << 20;   // 16 MB of palettes
constexpr uint32_t HEAP_WORDS = SLOTS * H5_PART_WORDS + PAL_WORDS;
constexpr uint32_t TAB_WORDS = 8192 + 8192; // rsqrt 16384 u16, cube root 32768 u8
constexpr uint64_t QUARANTINE = 4;          // presents before a freed record is reused

// A cache key: guest addresses and versions side by side, never folded into
// fewer bits (two palettes once shared a folded key).
struct Key {
    uint64_t a = 0, b = 0;
    bool operator==(const Key &o) const { return a == o.a && b == o.b; }
};
struct KeyHash {
    size_t operator()(const Key &k) const { return (size_t)(k.a * 0x9e3779b97f4a7c15ull ^ k.b); }
};

struct Slot {
    uint32_t refs = 0, pal = 0, gen = 0;
    bool used = false;
};
struct Pal {
    uint32_t off = 0, cls = 0, refs = 0;
    Key key;
};
struct Freed {
    uint64_t at;
    uint32_t what; // a slot, or a palette index | 0x80000000
};

struct State {
    bool ready = false, failed = false, kit = false;
    uint32_t base = 0, nverts = 0; // the big buffer's guest memory and its vertices
    uint32_t vb = 0;               // the kit's buffer for it, 0 while there is none
    uint64_t looked = ~0ull;       // the present at which bound() last looked for one
    std::vector<uint32_t> map, pool, heap, tab;
    uint32_t pool_used = 0;
    std::unordered_map<Key, uint32_t, KeyHash> meshes; // mesh -> its first pool vertex, or ~0u: unusable
    std::vector<Slot> slots;
    std::vector<uint32_t> free_slots;
    std::vector<Pal> pals;
    std::vector<uint32_t> free_pals[4];
    uint32_t pal_top = SLOTS * H5_PART_WORDS;
    std::unordered_map<Key, uint32_t, KeyHash> pal_of;
    std::vector<Freed> freed;
    uint32_t gen = 0;
} S;

struct Stats {
    uint64_t parts, gpu_parts, cpu_parts, why[8], verts, bad_parts, bad_pos, bad_nrm, bad_t0, bad_t1,
        bad_uv, gpu_verts, gpu_bad, gpu_bad_ref, gpu_moved, staged, restaged;
    float max_pos;
    int64_t last;
} st;

const char *kWhy[8] = {"not skinned", "mesh", "part flags", "light cache", "lights > 8", "buffers full",
                       "no palette", "other"};

void release_frees() {
    const uint64_t now = d3d9_vfetch_frames();
    size_t k = 0;
    for (const Freed &f : S.freed) {
        if (f.at + QUARANTINE > now) {
            S.freed[k++] = f;
            continue;
        }
        if (f.what & 0x80000000u) {
            const uint32_t p = f.what & 0x7fffffffu;
            S.free_pals[S.pals[p].cls].push_back(p);
        } else {
            S.free_slots.push_back(f.what);
        }
    }
    S.freed.resize(k);
}

void pal_unref(uint32_t p) {
    if (!p || --S.pals[p].refs)
        return;
    S.pal_of.erase(S.pals[p].key);
    S.freed.push_back({d3d9_vfetch_frames(), p | 0x80000000u});
}

void slot_unref(uint32_t s, uint32_t n) {
    Slot &sl = S.slots[s];
    if (!sl.used || sl.refs < n) {
        sl.refs = 0;
    } else {
        sl.refs -= n;
    }
    if (sl.refs || !sl.used)
        return;
    sl.used = false;
    pal_unref(sl.pal);
    sl.pal = 0;
    S.freed.push_back({d3d9_vfetch_frames(), s});
}

void kwrite(uint32_t k, uint32_t offset, const void *bytes, uint32_t size) {
    if (S.kit)
        d3d9_vfetch_write(k, offset, bytes, size);
}

void map_write(uint32_t first, uint32_t count) {
    kwrite(0, first * 4u, S.map.data() + first, count * 4u);
}

// The big buffer's vertices [first, first + count) are written by the CPU.
void map_clear(uint32_t first, uint32_t count) {
    if (!S.ready || first >= S.nverts)
        return;
    count = std::min(count, S.nverts - first);
    bool any = false;
    uint32_t run_slot = 0, run = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t &m = S.map[first + i];
        if (!m)
            continue;
        const uint32_t s = m >> 16;
        if (s != run_slot) {
            if (run)
                slot_unref(run_slot, run);
            run_slot = s;
            run = 0;
        }
        ++run;
        m = 0;
        any = true;
    }
    if (run)
        slot_unref(run_slot, run);
    if (any)
        map_write(first, count);
}

void map_set(uint32_t first, uint32_t count, uint32_t slot) {
    if (first >= S.nverts || count > S.nverts - first)
        return;
    map_clear(first, count);
    for (uint32_t i = 0; i < count; ++i)
        S.map[first + i] = slot << 16 | i;
    S.slots[slot].refs += count;
    map_write(first, count);
}

// The fetch in the renderer's language (asked once vertex fetch is known to be there).
const std::string &fetch_source() {
    static const std::string source = [] {
        const bool check = skin_mode() == GPUCHECK;
        if (d3d9_vfetch_supported() == D3D9_VFETCH_GLSL)
            return std::string(kMath) + (check ? kFetchCheckGlsl : kFetchGlsl);
        return std::string(kMath) + (check ? kFetchCheck : kFetch);
    }();
    return source;
}

bool setup(uint32_t base) {
    if (S.ready || S.failed)
        return S.ready;
    if (!d3d9_vfetch_supported()) {
        S.failed = true;
        fprintf(stderr, "[gpu-skin] the renderer has no vertex fetch: the CPU draws as before\n");
        return false;
    }
    // check computes and compares on the CPU only: no vertex fetch.
    uint32_t n = 0x100000u;
    if (skin_mode() != CHECK) {
        n = d3d9_vfetch_pool(base, 0x20, fetch_source().c_str(), skin_mode() == GPUCHECK);
        if (!n) {
            S.failed = true;
            fprintf(stderr, "[gpu-skin] no vertex buffer at %08x: the CPU draws as before\n", base);
            return false;
        }
        S.kit = true;
        S.vb = d3d9_vfetch_buffer();
    }
    S.base = base;
    S.nverts = n;
    S.map.assign(n, 0);
    S.pool.assign((size_t)POOL_VERTS * 16, 0);
    S.heap.assign(HEAP_WORDS, 0);
    S.tab.assign(TAB_WORDS, 0);
    for (uint32_t i = 0; i < 16384; ++i)
        S.tab[H5_TAB_RSQRT + (i >> 1)] |= (uint32_t)rd16(H5SKIN_RSQRT + i * 2u) << ((i & 1u) * 16u);
    for (uint32_t i = 0; i < 32768; ++i)
        S.tab[H5_TAB_CBRT + (i >> 2)] |= (uint32_t)rd8(H5SKIN_CBRT + i) << ((i & 3u) * 8u);
    S.slots.assign(SLOTS, Slot{});
    for (uint32_t s = SLOTS - 1; s >= 1; --s)
        S.free_slots.push_back(s);
    S.pals.assign(1, Pal{});
    if (S.kit) {
        d3d9_vfetch_size(0, n * 4u);
        d3d9_vfetch_size(1, POOL_VERTS * 64u);
        d3d9_vfetch_size(2, HEAP_WORDS * 4u);
        d3d9_vfetch_size(3, TAB_WORDS * 4u);
    }
    kwrite(0, 0, S.map.data(), n * 4u);
    kwrite(1, 0, S.pool.data(), 64);
    kwrite(2, 0, S.heap.data(), H5_PART_WORDS * 4u);
    kwrite(3, 0, S.tab.data(), TAB_WORDS * 4u);
    if (skin_mode() == GPUCHECK) {
        d3d9_vfetch_size(4, n * 9u * 4u);
        const uint32_t zero = 0;
        d3d9_vfetch_write(4, 0, &zero, 4);
    }
    S.ready = true;
    // RECOMP_HOMM5_GPU_SKIN_LOW_RAM=0/1: gfx_low_ram set here, to test both fills.
    if (const char *lr = getenv("RECOMP_HOMM5_GPU_SKIN_LOW_RAM"))
        wr8(H5SKIN_LOW_RAM, lr[0] == '1');
    fprintf(stderr, "[gpu-skin] the big vertex buffer at %08x, %u vertices; gfx_low_ram %u\n", base, n,
            rd8(H5SKIN_LOW_RAM));
    return true;
}

// The kit's buffer has gone (released with its device, as a change of the
// graphics settings does).
bool kit_lost() {
    return S.kit && (!S.vb || d3d9_vfetch_buffer() != S.vb);
}

// After a new device: every range back to the CPU, then the vertex fetch on
// the big buffer now at base. False while there is none yet (one look a
// frame): the CPU fills, as without the GPU.
bool bound(uint32_t base) {
    if (!kit_lost())
        return true;
    if (S.vb) {
        map_clear(0, S.nverts);
        S.vb = 0;
        fprintf(stderr, "[gpu-skin] the big vertex buffer went with its device: the CPU fills meanwhile\n");
    }
    const uint64_t f = d3d9_vfetch_frames();
    if (f == S.looked)
        return false;
    S.looked = f;
    const uint32_t n = d3d9_vfetch_pool(base, 0x20, fetch_source().c_str(), skin_mode() == GPUCHECK);
    if (!n)
        return false;
    if (n != S.nverts) {
        S.map.assign(n, 0);
        S.nverts = n;
        d3d9_vfetch_size(0, n * 4u);
        if (skin_mode() == GPUCHECK)
            d3d9_vfetch_size(4, n * 9u * 4u);
    }
    kwrite(0, 0, S.map.data(), n * 4u);
    S.base = base;
    S.vb = d3d9_vfetch_buffer();
    fprintf(stderr, "[gpu-skin] the big vertex buffer again, at %08x, %u vertices\n", base, n);
    return true;
}

// ---- the mesh pool -------------------------------------------------------------

// 00a15a10: a mesh's channel `id` (mesh+0x54: 16-byte {id, vector}), or 0.
uint32_t mesh_channel(uint32_t mesh, uint32_t id) {
    const uint32_t b = rd32(mesh + 0x54u), e = rd32(mesh + 0x58u);
    for (uint32_t p = b; p + 16u <= e; p += 16u)
        if (rd32(p) == id)
            return p + 4u;
    return 0;
}

// The mesh's first vertex in the pool, made on first use; ~0u when it cannot be.
uint32_t mesh_pool(uint32_t mesh, uint32_t nv) {
    const uint32_t recs = rd32(mesh + 0x18u);
    const Key key{(uint64_t)mesh << 32 | recs, nv};
    auto it = S.meshes.find(key);
    if (it != S.meshes.end())
        return it->second;
    uint32_t base = ~0u;
    const uint32_t pos = rd32(mesh + 0x0cu), npos = (rd32(mesh + 0x10u) - pos) / 12u;
    const uint32_t idx = rd32(mesh + 0x30u), nidx = (rd32(mesh + 0x34u) - idx) / 2u;
    const uint32_t skin = rd32(mesh + 0x24u), nskin = (rd32(mesh + 0x28u) - skin) / 0x18u;
    const uint32_t ch = mesh_channel(mesh, 0xd);
    const uint32_t chb = ch ? rd32(ch) : 0, chn = ch ? (rd32(ch + 4u) - chb) / 4u : 0;
    bool ok = nidx >= nv && nv < 65536 && S.pool_used + nv <= POOL_VERTS && (!chn || chn >= nv);
    for (uint32_t i = 0; ok && i < nv; ++i) {
        const uint32_t pi = rd16(idx + i * 2u);
        ok = pi < npos && pi < nskin;
    }
    if (ok) {
        base = S.pool_used;
        S.pool_used += nv;
        for (uint32_t i = 0; i < nv; ++i) {
            uint32_t *v = S.pool.data() + (size_t)(base + i) * 16;
            const uint32_t pi = rd16(idx + i * 2u), r = recs + i * 0x14u, sk = skin + pi * 0x18u;
            v[0] = rd32(pos + pi * 12u);
            v[1] = rd32(pos + pi * 12u + 4u);
            v[2] = rd32(pos + pi * 12u + 8u);
            v[3] = rd32(r + 8u);
            v[4] = rd32(r);
            v[5] = rd32(r + 4u);
            v[6] = rd32(r + 0xcu);
            v[7] = rd32(r + 0x10u);
            for (uint32_t k = 0; k < 4; ++k)
                v[8 + k] = rd32(sk + k * 4u);
            v[12] = rd32(sk + 0x10u);
            v[13] = rd32(sk + 0x14u);
            v[14] = chn ? rd32(chb + i * 4u) : 0;
            v[15] = chn ? 1 : 0;
        }
        kwrite(1, base * 64u, S.pool.data() + (size_t)base * 16, nv * 64u);
    }
    S.meshes[key] = base;
    return base;
}

// ---- palettes ------------------------------------------------------------------

constexpr uint32_t kPalBones[4] = {32, 64, 128, 256};

// The palettes of a bind's current version (float rows, then i16), shared by
// the instance's parts; 0 when it cannot be had.
uint32_t palette(uint32_t palf, uint32_t pali, uint32_t *nbones) {
    const uint32_t fb = rd32(palf), nf = (rd32(palf + 4u) - fb) / 0x40u;
    const uint32_t ib = rd32(pali), ni = (rd32(pali + 4u) - ib) / 0x18u;
    if (!nf || nf != ni || nf > 256)
        return 0;
    *nbones = nf;
    // A node's value is at +0x14 and its version at +0x10.
    const Key key{(uint64_t)palf << 32 | rd32(palf - 4u), (uint64_t)pali << 32 | rd32(pali - 4u)};
    auto it = S.pal_of.find(key);
    if (it != S.pal_of.end())
        return it->second;
    uint32_t cls = 0;
    while (kPalBones[cls] < nf)
        ++cls;
    const uint32_t words = kPalBones[cls] * 18u;
    release_frees();
    uint32_t p;
    if (!S.free_pals[cls].empty()) {
        p = S.free_pals[cls].back();
        S.free_pals[cls].pop_back();
    } else {
        if (S.pal_top + words > HEAP_WORDS)
            return 0;
        p = (uint32_t)S.pals.size();
        S.pals.push_back(Pal{});
        S.pals[p].off = S.pal_top;
        S.pals[p].cls = cls;
        S.pal_top += words;
    }
    Pal &pl = S.pals[p];
    pl.key = key;
    pl.refs = 0;
    uint32_t *w = S.heap.data() + pl.off;
    for (uint32_t b = 0; b < nf; ++b)
        for (uint32_t k = 0; k < 12; ++k)
            w[b * 12u + k] = rd32(fb + b * 0x40u + k * 4u);
    uint32_t *wi = w + nf * 12u;
    for (uint32_t b = 0; b < nf; ++b)
        for (uint32_t k = 0; k < 6; ++k)
            wi[b * 6u + k] = rd32(ib + b * 0x18u + k * 4u);
    kwrite(2, pl.off * 4u, w, nf * 18u * 4u);
    S.pal_of[key] = p;
    return p;
}

// ---- the part record -----------------------------------------------------------

struct Box {
    float c[3], r, h[3];
};

// 0066e4d0's box of a part's AABB (part+0x7c..0x90).
Box part_box(uint32_t part) {
    Box b;
    float hh[3];
    for (int a = 0; a < 3; ++a) {
        const float mn = rdf(part + 0x7cu + a * 4u), mx = rdf(part + 0x88u + a * 4u);
        hh[a] = (mx - mn) * 0.5f;
        b.c[a] = (mn + mx) * 0.5f;
        b.h[a] = hh[a];
    }
    float s = hh[0] * hh[0];
    s = s + hh[1] * hh[1];
    s = s + hh[2] * hh[2];
    b.r = (float)sqrt((double)s);
    return b;
}

// 0068ef30's test of a point light against the part's box.
bool box_hit(const Box &b, uint32_t l) {
    const double r = (double)rdf(l + 0xcu);
    for (int a = 0; a < 3; ++a) {
        const double d = fabs((double)(b.c[a] - rdf(l + a * 4u))) - r;
        if (!((double)b.h[a] > d))
            return false;
    }
    return true;
}

// 0068e930 for a skinned part (one vector from the box's centre) and the
// colour words 0068f7b0 makes of the light's colour.
void light_words(const X86 *c, const Box &b, uint32_t l, uint32_t *out) {
    const float dx = rdf(l) - b.c[0], dy = rdf(l + 4u) - b.c[1], dz = rdf(l + 8u) - b.c[2];
    float d2 = dy * dy;
    const float dx2 = dx * dx;
    d2 = d2 + dx2;
    const float r4 = b.r * b.r * 0.25f;
    d2 = d2 + dz * dz;
    const float lr = rdf(l + 0xcu), r2 = lr * lr;
    const float d2e = r4 + d2;
    const float lim = r2 - d2e;
    const float f = 0.0f < lim ? lim : (lim == lim ? 0.0f : lim);
    const float k2 = 1.0f / r2;
    const float num = f * k2;
    float den = 64.0f / r2;
    den = den * d2e;
    den = den + 0.25f;
    const float att = num / den;
    const double dist = sqrt((double)d2);
    const float a = (float)((double)att / dist);
    const int32_t wx = fistp(c, a * dx * 8191.0f), wy = fistp(c, a * dy * 8191.0f), wz = fistp(c, a * dz * 8191.0f);
    // 0068f7b0 adds nothing when the vector is short.
    const float fz = (float)(int16_t)wz, fy = (float)(int16_t)wy, fx = (float)(int16_t)wx;
    float s = fy * fy;
    s = s + fx * fx;
    s = s + fz * fz;
    const bool weak = 200.0f > s;
    out[0] = weak ? 0 : pack2(wz, wy);
    out[1] = weak ? 0 : pack2(wx, 0);
    const float cx = rdf(l + 0x10u), cy = rdf(l + 0x14u), cz = rdf(l + 0x18u);
    out[2] = pack2(fistp(c, cz * 32767.0f), fistp(c, cy * 32767.0f));
    out[3] = pack2(fistp(c, cx * 32767.0f), 0);
}

// 0068fc20 at the box's centre, as 0068ef30 calls it for a skinned part: the
// two alpha bytes (TANGENT0's from layer B, TANGENT1's from layer A).
uint32_t part_alpha(X86 *c, uint32_t E, uint32_t L, uint32_t part, const Box &b) {
    if ((int32_t)rd32(L + 0x3cu) < 2)
        return 0xffffu;
    const uint32_t s = part + 0x24u;
    uint32_t A, B;
    if (!rd8(s + 1) && !rd8(s)) {
        A = s + 0x30u;
        B = s + 0x3cu;
        if (rd32(L + 0x58u) != rd32(s + 0x4cu) && rd32(L + 0x58u) == rd32(s + 0x48u)) {
            for (uint32_t k = 0; k < 12; k += 4) {
                const uint32_t t = rd32(A + k);
                wr32(A + k, rd32(B + k));
                wr32(B + k, t);
            }
            const uint32_t t = rd32(s + 0x4cu);
            wr32(s + 0x4cu, rd32(s + 0x48u));
            wr32(s + 0x48u, t);
        }
    } else {
        A = H5SKIN_ALPHA_A;
        B = H5SKIN_ALPHA_B;
    }
    // A one-position vector on the guest stack, below what this call uses.
    const uint32_t scratch = (E - 0x400u) & ~15u;
    wr32(scratch, scratch + 16u);
    wr32(scratch + 4u, scratch + 28u);
    wr32(scratch + 8u, scratch + 28u);
    for (int a = 0; a < 3; ++a)
        wr32(scratch + 16u + a * 4u, fbits(b.c[a]));
    c->r[R_ESP] = scratch - 16u;
    c->r[R_ECX] = scratch;
    c->r[R_EDX] = A;
    guest_call(c, H5SKIN_ALPHA_FN, rd32(L + 0x5cu), L + 0x34u, B, L + 0x44u);
    c->r[R_ESP] = E;
    return rd8(rd32(B)) | (uint32_t)rd8(rd32(A)) << 8;
}

// The part's record, or 0 with why[] counted; the palettes taken.
uint32_t part_record(X86 *c, uint32_t E, uint32_t L, uint32_t part, uint32_t mesh, uint32_t nv,
                     uint32_t palf, uint32_t pali, int *why) {
    const uint32_t s = part + 0x24u;
    if (rd8(s)) {
        *why = 2;
        return 0;
    }
    const bool baked = rd8(s + 2) != 0;
    if (!baked && rd32(part + 0x50u) == rd32(L + 0x30u)) {
        *why = 3; // the game would take its cached colours
        return 0;
    }
    const Box b = part_box(part);
    uint32_t lights[H5_PART_MAXLIGHTS];
    uint32_t ns = 0, nd = 0;
    if (!baked) {
        if (!rd8(s + 5)) {
            const uint32_t lb = rd32(L + 0x74u), le = rd32(L + 0x78u);
            for (uint32_t l = lb; l + 0x20u <= le; l += 0x20u)
                if (box_hit(b, l)) {
                    if (ns == 8) {
                        *why = 4;
                        return 0;
                    }
                    lights[ns++] = l;
                }
        }
        const uint32_t lb = rd32(L + 0x68u), le = rd32(L + 0x6cu);
        for (uint32_t l = lb; l + 0x20u <= le; l += 0x20u)
            if (box_hit(b, l)) {
                if (nd == 8) {
                    *why = 4;
                    return 0;
                }
                lights[ns + nd++] = l;
            }
    }
    const uint32_t pool = mesh_pool(mesh, nv);
    if (pool == ~0u) {
        *why = 1;
        return 0;
    }
    uint32_t nbones = 0;
    const uint32_t p = palette(palf, pali, &nbones);
    if (!p) {
        *why = 6;
        return 0;
    }
    release_frees();
    if (S.free_slots.empty()) {
        *why = 5;
        return 0;
    }
    const uint32_t slot = S.free_slots.back();
    S.free_slots.pop_back();
    Slot &sl = S.slots[slot];
    sl.used = true;
    sl.refs = 0;
    sl.pal = p;
    sl.gen = ++S.gen;
    S.pals[p].refs++;
    uint32_t *r = S.heap.data() + (size_t)slot * H5_PART_WORDS;
    memset(r, 0, H5_PART_WORDS * 4u);
    r[H5_PART_POOL] = pool;
    r[H5_PART_PALF] = S.pals[p].off;
    r[H5_PART_PALI] = S.pals[p].off + nbones * 12u;
    r[H5_PART_BONES] = nbones;
    const uint32_t ch = mesh_channel(mesh, 0xd);
    uint32_t flags = 0;
    if (baked)
        flags |= H5_FLAG_BAKED | (rd8(s + 3) ? H5_FLAG_GRAY : 0u);
    if (ch && rd32(ch) != rd32(ch + 4u))
        flags |= H5_FLAG_CH13;
    if (ns)
        flags |= H5_FLAG_STATIC;
    if (nd)
        flags |= H5_FLAG_DYN;
    r[H5_PART_FLAGS] = flags;
    r[H5_PART_NLIGHT] = ns | nd << 8;
    for (uint32_t k = 0; k < 2; ++k) {
        r[H5_PART_BASE + k] = rd32(L + k * 4u);
        r[H5_PART_C0 + k] = rd32(L + 0x08u + k * 4u);
        r[H5_PART_C1 + k] = rd32(L + 0x10u + k * 4u);
        r[H5_PART_BACK + k] = rd32(L + 0x18u + k * 4u);
        r[H5_PART_SEC + k] = rd32(L + 0x18u + k * 4u);
        r[H5_PART_DIR + k] = rd32(L + 0x20u + k * 4u);
        r[H5_PART_BIAS + k] = rd32(L + 0x28u + k * 4u);
    }
    if (rd8(s + 6)) { // 0068fb80 / 0068fb10: the back colour of output 0, C0 x the part's tint
        float t[3];
        for (int a = 0; a < 3; ++a)
            t[a] = rdf(L + 0x8cu + a * 4u) * rdf(s + 8u + a * 4u);
        r[H5_PART_SEC] = pack2(fistp(c, t[2] * 16384.0f), fistp(c, t[1] * 16384.0f));
        r[H5_PART_SEC + 1] = pack2(fistp(c, t[0] * 16384.0f), 0);
    }
    for (uint32_t j = 0; j < ns + nd; ++j)
        light_words(c, b, lights[j], r + H5_PART_LIGHTS + j * 4u);
    r[H5_PART_ALPHA] = part_alpha(c, E, L, part, b);
    kwrite(2, slot * H5_PART_WORDS * 4u, r, H5_PART_WORDS * 4u);
    return slot;
}

// ---- the part fill -------------------------------------------------------------

struct Part {
    uint32_t mesh = 0, nv = 0, palf = 0, pali = 0;
};

// What 0066e4d0 reads before it skins: the mesh, a skinned part's palettes.
// False (why[] counted) for a part this does not take.
bool part_probe(X86 *c, uint32_t part, Part *p, int *why) {
    const uint32_t loader = rd32(part + 0x1cu);
    const uint32_t mesh = loader ? rd32(loader + 0x14u) : 0;
    if (!mesh || (int32_t)rd32(mesh + 4u) < 0) {
        *why = 1;
        return false;
    }
    const uint32_t rb = rd32(mesh + 0x18u), re = rd32(mesh + 0x1cu);
    if (rb == re || rd32(mesh + 0x24u) == rd32(mesh + 0x28u)) {
        *why = 1;
        return false;
    }
    p->mesh = mesh;
    p->nv = (re - rb) / 0x14u;
    // A part class's type (vtable slot 7) is a constant of the class for the
    // skinned ones (CAnimatedPart, CStaticAnimatedPart: 2); the one class
    // whose type varies (CDynamicGeometryPart) is never 2.
    static std::unordered_map<uint32_t, bool> skinned;
    const uint32_t vt = rd32(part);
    auto it = skinned.find(vt);
    if (it == skinned.end()) {
        c->r[R_ECX] = part;
        it = skinned.emplace(vt, guest_call(c, rd32(vt + 0x1cu)) == 2).first;
    }
    if (!it->second) {
        *why = 0;
        return false;
    }
    c->r[R_ECX] = part;
    p->pali = guest_call(c, rd32(vt + 0x28u));
    c->r[R_ECX] = part;
    p->palf = guest_call(c, rd32(vt + 0x24u));
    if (!p->palf || rd32(p->palf) == rd32(p->palf + 4u) || !p->pali) {
        *why = 6;
        return false;
    }
    return true;
}

void ret_from(X86 *c, uint32_t E, uint32_t ret_exp, uint32_t eax, uint32_t pop) {
    c->r[R_EAX] = eax;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 4u + pop;
    if (c->eip != ret_exp)
        recomp_return(c);
}

// The game's 8 words of a vertex compared with this file's.
void compare(uint32_t dst, uint32_t nv, uint32_t slot) {
    bool bad = false;
    for (uint32_t i = 0; i < nv; ++i) {
        uint32_t w[8];
        h5_skin_vertex(slot << 16 | i, S.pool.data(), S.heap.data(), S.tab.data(), w);
        const uint32_t g = dst + i * 0x20u;
        for (int k = 0; k < 3; ++k) {
            const float a = h5_f(w[k]), o = rdf(g + k * 4u);
            const float e = fabsf(a - o);
            if (e > st.max_pos)
                st.max_pos = e;
            if (!(e <= 1e-4f * (1.0f + fabsf(o)))) {
                st.bad_pos++;
                bad = true;
            }
        }
        const uint32_t n = rd32(g + 0xcu), t0 = rd32(g + 0x18u), t1 = rd32(g + 0x1cu);
        if (n != w[3])
            st.bad_nrm++, bad = true;
        if (rd32(g + 0x10u) != w[4] || rd32(g + 0x14u) != w[5])
            st.bad_uv++, bad = true;
        if (t0 != w[6])
            st.bad_t0++, bad = true;
        if (t1 != w[7])
            st.bad_t1++, bad = true;
        if (bad && st.bad_parts < 12 && i < 2)
            fprintf(stderr,
                    "[gpu-skin]   vertex %u: normal %08x/%08x t0 %08x/%08x t1 %08x/%08x pos %.4f %.4f %.4f / "
                    "%.4f %.4f %.4f\n",
                    i, w[3], n, w[6], t0, w[7], t1, h5_f(w[0]), h5_f(w[1]), h5_f(w[2]), rdf(g), rdf(g + 4u),
                    rdf(g + 8u));
    }
    st.verts += nv;
    if (bad && st.bad_parts++ < 12) {
        const uint32_t *r = S.heap.data() + (size_t)slot * H5_PART_WORDS;
        fprintf(stderr, "[gpu-skin] part differs: flags %x lights %x alpha %04x bones %u\n", r[H5_PART_FLAGS],
                r[H5_PART_NLIGHT], r[H5_PART_ALPHA], r[H5_PART_BONES]);
    }
}

void report() {
    const int64_t t = now_ms();
    if (t - st.last < 2000)
        return;
    st.last = t;
    fprintf(stderr,
            "[gpu-skin] parts %llu: gpu %llu, cpu %llu (", (unsigned long long)st.parts,
            (unsigned long long)st.gpu_parts, (unsigned long long)st.cpu_parts);
    for (int k = 0; k < 8; ++k)
        if (st.why[k])
            fprintf(stderr, "%s %llu, ", kWhy[k], (unsigned long long)st.why[k]);
    fprintf(stderr,
            "); checked %llu vertices: parts differing %llu, pos %llu (max %.2g), normal %llu, uv %llu, t0 %llu, "
            "t1 %llu; gpu checked %llu, differing %llu (+%llu as the cpu, %llu moved); staged copies %llu, refilled %llu; pool %u of %u, records %zu free\n",
            (unsigned long long)st.verts, (unsigned long long)st.bad_parts, (unsigned long long)st.bad_pos,
            st.max_pos, (unsigned long long)st.bad_nrm, (unsigned long long)st.bad_uv,
            (unsigned long long)st.bad_t0, (unsigned long long)st.bad_t1, (unsigned long long)st.gpu_verts,
            (unsigned long long)st.gpu_bad, (unsigned long long)st.gpu_bad_ref, (unsigned long long)st.gpu_moved, (unsigned long long)st.staged, (unsigned long long)st.restaged, S.pool_used, POOL_VERTS, S.free_slots.size());
}

// gpucheck: what the GPU fetched, against what the game wrote.
void gpu_visit(const uint32_t *w, uint32_t count, uint32_t done_tag, void *) {
    const uint32_t n = std::min(count / 9u, S.nverts);
    for (uint32_t v = 0; v < n; ++v) {
        const uint32_t *e = w + v * 9u;
        // Only the newest finished frame's: older ranges may have been reused since.
        if (e[8] != done_tag || !S.map[v])
            continue;
        const uint32_t g = S.base + v * 0x20u;
        bool bad = false;
        for (int k = 0; k < 3; ++k) {
            const float a = h5_f(e[k]), o = rdf(g + k * 4u);
            if (!(fabsf(a - o) <= 1e-4f * (1.0f + fabsf(o))))
                bad = true;
        }
        if (e[5] != S.map[v]) { // drawn for the part that had the range before
            st.gpu_moved++;
            continue;
        }
        for (int k = 3; k < 8; ++k)
            if (k != 5 && e[k] != rd32(g + k * 4u))
                bad = true;
        st.gpu_verts++;
        if (!bad)
            continue;
        uint32_t r[8];
        h5_skin_vertex(S.map[v], S.pool.data(), S.heap.data(), S.tab.data(), r);
        const bool as_ref = r[3] == e[3] && r[4] == e[4] && r[6] == e[6] && r[7] == e[7];
        const bool ref_ok = r[3] == rd32(g + 12u) && r[6] == rd32(g + 24u) && r[7] == rd32(g + 28u);
        (as_ref ? st.gpu_bad_ref : st.gpu_bad)++;
        static bool dumped = false;
        if (!as_ref && !dumped) {
            // RECOMP_HOMM5_GPU_SKIN_DUMP=<dir>: the buffers as the CPU has them, to replay offline.
            dumped = true;
            if (const char *dir = getenv("RECOMP_HOMM5_GPU_SKIN_DUMP")) {
                auto save = [&](const char *name, const void *p, size_t n) {
                    char path[1024];
                    snprintf(path, sizeof path, "%s/%s", dir, name);
                    if (FILE *f = fopen(path, "wb")) {
                        fwrite(p, 1, n, f);
                        fclose(f);
                    }
                };
                save("map.bin", S.map.data(), S.map.size() * 4);
                save("pool.bin", S.pool.data(), (size_t)S.pool_used * 64);
                save("heap.bin", S.heap.data(), S.heap.size() * 4);
                save("tab.bin", S.tab.data(), S.tab.size() * 4);
                save("gpu.bin", w, (size_t)count * 4);
                const uint32_t meta[2] = {v, done_tag};
                save("meta.bin", meta, sizeof meta);
                fprintf(stderr, "[gpu-skin] buffers saved to %s\n", dir);
            }
        }
        if (st.gpu_bad + st.gpu_bad_ref < 16) {
            const uint32_t sl = S.map[v] >> 16, *rec = S.heap.data() + (size_t)sl * H5_PART_WORDS;
            fprintf(stderr,
                    "[gpu-skin] gpu vertex %u (slot %u gen %u refs %u, pal %u/%u bones %u) %s, cpu %s: %08x %08x %08x "
                    "/ game %08x %08x %08x / cpu %08x %08x %08x\n",
                    v, sl, S.slots[sl].gen, S.slots[sl].refs, rec[H5_PART_PALF], rec[H5_PART_PALI],
                    rec[H5_PART_BONES], as_ref ? "as the cpu" : "unlike the cpu", ref_ok ? "as the game" : "unlike it",
                    e[3], e[6], e[7], rd32(g + 12u), rd32(g + 24u), rd32(g + 28u), r[3], r[6], r[7]);
        }
    }
}

void gpu_check_tick() {
    static uint64_t last = 0;
    const uint64_t f = d3d9_vfetch_frames();
    if (f < last + 10)
        return;
    last = f;
    d3d9_vfetch_check(gpu_visit, nullptr);
}

constexpr uint32_t MAGIC = 0x534b494eu; // the staging buffer of a part drawn on the GPU

} // namespace

// 0066e4d0 (thiscall ECX the fill context {light context, geometry, its locked
// vertices, vertices so far}; part, destination, positions, &flag; RET 0x10):
// one part's vertices.
void homm5_skin_part_fill(X86 *c) {
    const int mode = skin_mode();
    if (mode == OFF) {
        H5SKIN_FILL_ORIG(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], ret_exp = c->ret_expect, ctx = c->r[R_ECX];
    const uint32_t part = rd32(E + 4u), dst = rd32(E + 8u), flag = rd32(E + 16u);
    const uint32_t vb = rd32(ctx + 8u) + rd32(ctx + 12u) * 0x20u;
    const uint32_t base = rd32(ctx + 8u) - rd32(rd32(ctx + 4u) + 0xcu) * 0x20u;
    st.parts++;
    int why = 7;
    Part p;
    uint32_t slot = 0;
    if (setup(base) && bound(base) && base == S.base && part_probe(c, part, &p, &why))
        slot = part_record(c, E, rd32(ctx), part, p.mesh, p.nv, p.palf, p.pali, &why);
    c->r[R_ECX] = ctx;
    c->r[R_ESP] = E;
    c->ret_expect = ret_exp;
    if (!slot) {
        st.cpu_parts++;
        st.why[why]++;
        if (mode != ON)
            report();
        H5SKIN_FILL_ORIG(c);
        return;
    }
    st.gpu_parts++;
    if (mode == CHECK || (mode == GPUCHECK && dst != vb)) {
        H5SKIN_FILL_ORIG(c);
        compare(dst, p.nv, slot);
        S.slots[slot].refs = 1; // freed at once
        slot_unref(slot, 1);
        report();
        return;
    }
    if (mode == GPUCHECK) {
        H5SKIN_FILL_ORIG(c); // the game's vertices too, to compare the GPU's with
        compare(dst, p.nv, slot);
        map_set((vb - S.base) / 0x20u, p.nv, slot);
        gpu_check_tick();
        report();
        return;
    }
    report();
    if (dst == vb) {
        map_set((vb - S.base) / 0x20u, p.nv, slot);
    } else {
        // gfx_low_ram 0: the part's staging buffer, copied by 0066d950 next.
        wr32(dst, MAGIC);
        wr32(dst + 4u, part);
        wr32(dst + 8u, slot);
        wr32(dst + 12u, S.slots[slot].gen);
        wr32(dst + 16u, 1); // holds a reference, dropped at its first copy
        S.slots[slot].refs++;
    }
    wr8(flag, 0);
    ret_from(c, E, ret_exp, p.nv, 0x10);
}

// 0066d950 (fastcall ECX the destination, EDX the source; the byte count; RET
// 4): a part's staged vertices into the big buffer.
void homm5_skin_copy(X86 *c) {
    const uint32_t E = c->r[R_ESP], ret_exp = c->ret_expect, dst = c->r[R_ECX], src = c->r[R_EDX];
    const uint32_t bytes = rd32(E + 4u);
    if (!S.ready || bytes < 16u || rd32(src) != MAGIC) {
        H5SKIN_COPY_ORIG(c);
        return;
    }
    const uint32_t part = rd32(src + 4u), slot = rd32(src + 8u), gen = rd32(src + 12u);
    const uint32_t n = bytes / 0x20u;
    if (slot && slot < SLOTS && S.slots[slot].used && S.slots[slot].gen == gen) {
        const bool here = !kit_lost() && dst >= S.base;
        if (here)
            map_set((dst - S.base) / 0x20u, n, slot);
        if (rd32(src + 16u)) {
            wr32(src + 16u, 0);
            slot_unref(slot, 1);
        }
        if (here) {
            st.staged++;
            ret_from(c, E, ret_exp, c->r[R_EAX], 4);
            return;
        }
    }
    // The record went with the range that held it, or the big buffer with
    // its device: the part again, through the game's own fill this time (EBX
    // holds the fill context here).
    const uint32_t ctx = c->r[R_EBX];
    const uint32_t pos = rd32(part + 0xcu) != rd32(part + 0x10u) ? part + 0xcu
                                                                 : rd32(rd32(part + 0x1cu) + 0x14u) + 0xcu;
    const uint32_t scratch = (E - 0x40u) & ~15u, sp = scratch - 0x20u;
    wr32(scratch, 0);
    wr32(sp, 0); // the return: matches ret_expect below, so the RET comes straight back here
    wr32(sp + 4u, part);
    wr32(sp + 8u, src);
    wr32(sp + 12u, pos);
    wr32(sp + 16u, scratch);
    c->r[R_ESP] = sp;
    c->r[R_ECX] = ctx;
    c->ret_expect = 0;
    H5SKIN_FILL_ORIG(c);
    c->r[R_ESP] = E;
    c->r[R_ECX] = dst;
    c->r[R_EDX] = src;
    c->ret_expect = ret_exp;
    st.restaged++;
    H5SKIN_COPY_ORIG(c);
}

// 00606af0 (fastcall ECX the vertex format, EDX the vertex count; a key; RET
// 4): a range of a format's big vertex buffer, as its CUserGeometry (+0xc the
// first vertex). A new owner writes it, so its map goes back to the CPU's.
void homm5_skin_alloc(X86 *c) {
    const uint32_t fmt = c->r[R_ECX], n = c->r[R_EDX];
    H5SKIN_ALLOC_ORIG(c);
    if (fmt == 5 && S.ready && c->r[R_EAX])
        map_clear(rd32(c->r[R_EAX] + 0xcu), n);
}

// ---- the bounds' positions -----------------------------------------------------
//
// 00557ef0's SSE skinning of a part's positions (the base game's 0066f600),
// which the bounds still need: fastcall ECX the bind positions, EDX the
// output, then the skin records (0x18 each), the palette's vector, the
// count; RET 0xc. Per position: bone 0's four rows, and when a second weight
// byte is set the rows 0-2 scaled by the weights and summed bone by bone
// (MULPS, ADDPS), transposed, then (x c0 + c3) + (y c1 + z c2). The same
// float operations in NEON, lane for lane, so the result is bit for bit the
// game's. RECOMP_HOMM5_NATIVE_BOUNDS: 1 (default), 0 the original, check both.

#include "neon_compat.h"

extern "C" {
void H5SKIN_POSITIONS_ORIG(X86 *c);
void homm5_skin_positions(X86 *c);
}

namespace {

int bounds_mode() {
    static int mode = -1;
    if (mode < 0) {
        const char *v = getenv("RECOMP_HOMM5_NATIVE_BOUNDS");
        mode = v && v[0] == '0' ? 0 : v && v[0] == 'c' ? 2 : 1;
        fprintf(stderr, "[gpu-skin] native bounds positions mode %d\n", mode);
    }
    return mode;
}

void positions(uint32_t in, uint32_t out, uint32_t skin, uint32_t pal, int32_t n) {
    for (int32_t j = 0; j < n; ++j, in += 0xcu, out += 0xcu, skin += 0x18u) {
        const uint8_t *rec = g_mem + skin;
        const float *m = (const float *)(g_mem + pal + (uint32_t)rec[0x14] * 0x40u);
        float32x4_t r0 = vld1q_f32(m), r1 = vld1q_f32(m + 4), r2 = vld1q_f32(m + 8);
        const float32x4_t r3 = vld1q_f32(m + 12);
        if (rec[0x11]) {
            float w;
            memcpy(&w, rec, 4);
            r0 = vmulq_n_f32(r0, w);
            r1 = vmulq_n_f32(r1, w);
            r2 = vmulq_n_f32(r2, w);
            for (int k = 1; k < 4; ++k) {
                if (k > 1 && !rec[0x10 + k])
                    break;
                const float *b = (const float *)(g_mem + pal + (uint32_t)rec[0x14 + k] * 0x40u);
                memcpy(&w, rec + 4 * k, 4);
                r0 = vaddq_f32(r0, vmulq_n_f32(vld1q_f32(b), w));
                r1 = vaddq_f32(r1, vmulq_n_f32(vld1q_f32(b + 4), w));
                r2 = vaddq_f32(r2, vmulq_n_f32(vld1q_f32(b + 8), w));
            }
        }
        // columns: c_i = (r0[i], r1[i], r2[i], r3[i])
        const float32x4x4_t t = {{r0, r1, r2, r3}};
        float rows[16];
        vst4q_f32(rows, t); // rows[4i + k] = r_k[i]: column i
        const float32x4_t c0 = vld1q_f32(rows), c1 = vld1q_f32(rows + 4), c2 = vld1q_f32(rows + 8),
                          c3 = vld1q_f32(rows + 12);
        float x, y, z;
        memcpy(&x, g_mem + in, 4);
        memcpy(&y, g_mem + in + 4, 4);
        memcpy(&z, g_mem + in + 8, 4);
        const float32x4_t a = vaddq_f32(vmulq_n_f32(c0, x), c3);
        const float32x4_t b = vaddq_f32(vmulq_n_f32(c1, y), vmulq_n_f32(c2, z));
        const float32x4_t p = vaddq_f32(a, b);
        uint32_t w3[3];
        memcpy(w3, &p, 12);
        wr32(out, w3[0]);
        wr32(out + 4u, w3[1]);
        wr32(out + 8u, w3[2]);
    }
}

} // namespace

void homm5_skin_positions(X86 *c) {
    const int mode = bounds_mode();
    if (mode == 0) {
        H5SKIN_POSITIONS_ORIG(c);
        return;
    }
    const uint32_t E = c->r[R_ESP], ret_exp = c->ret_expect;
    const uint32_t in = c->r[R_ECX], out = c->r[R_EDX], skin = rd32(E + 4u), palv = rd32(E + 8u);
    const int32_t n = (int32_t)rd32(E + 12u);
    const uint32_t pal = rd32(palv);
    if (mode == 2 && n > 0) {
        static uint64_t calls, bad;
        std::vector<uint32_t> mine((size_t)n * 3);
        // here first into a copy of the output, then the original over the real one
        std::vector<uint32_t> keep((size_t)n * 3);
        for (int32_t i = 0; i < n * 3; ++i)
            keep[i] = rd32(out + i * 4u);
        positions(in, out, skin, pal, n);
        for (int32_t i = 0; i < n * 3; ++i)
            mine[i] = rd32(out + i * 4u), wr32(out + i * 4u, keep[i]);
        H5SKIN_POSITIONS_ORIG(c);
        bool diff = false;
        for (int32_t i = 0; i < n * 3; ++i)
            diff |= mine[i] != rd32(out + i * 4u);
        ++calls;
        if (diff && bad++ < 10)
            fprintf(stderr, "[gpu-skin] bounds positions differ (call %llu, %d positions)\n",
                    (unsigned long long)calls, n);
        if (calls % 20000 == 0)
            fprintf(stderr, "[gpu-skin] bounds positions check: %llu calls, %llu differ\n",
                    (unsigned long long)calls, (unsigned long long)bad);
        return;
    }
    positions(in, out, skin, pal, n);
    // What the caller may see of the registers: EAX the records' end (the
    // palette when there were none), ECX and EDX the arrays' ends.
    c->r[R_EAX] = n > 0 ? skin + (uint32_t)n * 0x18u : pal;
    c->r[R_ECX] = n > 0 ? in + (uint32_t)n * 0xcu : in;
    c->r[R_EDX] = n > 0 ? out + (uint32_t)n * 0xcu : out;
    c->eip = rd32(E);
    c->r[R_ESP] = E + 16u;
    if (c->eip != ret_exp)
        recomp_return(c);
}
