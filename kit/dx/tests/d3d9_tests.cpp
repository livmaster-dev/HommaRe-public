// Small, synthetic effects exercise the real guest ABI and CTAB binding.
// No game files, window, physical controller or sound device are involved.
#ifndef _WIN32
#include <sys/resource.h>
#endif
#include "../dx.h"
#include "../d3d9_ffp.h"
#include "../d3d9_glsl.h"
#include "../d3d9_msl.h"
#include "../d3d9_pipeline.h"
#include "../d3d9_raster.h"
#include "../d3d9_shader.h"
#include "../d3d9_wgsl.h"
#include "../host_d9.h"
#include "../dds_volume.h"
#include "../../runtime/memory.h"
#include "guest_abi.h"

#include <array>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <memory>

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
        }                                                                                          \
    } while (0)

static HostD9Draw captured;
static void test_volume_dds() {
    std::vector<uint8_t> b(128 + (4 * 2 * 8 + 2 * 1 * 4 + 1 * 1 * 2 + 1) * 4, 0);
    auto put = [&](unsigned word, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i)
            b[word * 4 + i] = uint8_t(value >> (8 * i));
    };
    const uint32_t header[32] = {0x20534444, 124,  0x821007,   2,        4,        0, 8,  4,
                                 0,          0,    0,          0,        0,        0, 0,  0,
                                 0,          0,    0,          32,       0x41,     0, 32, 0xff0000,
                                 0xff00,     0xff, 0xff000000, 0x40100a, 0x200000, 0, 0,  0};
    for (unsigned i = 0; i < 32; ++i)
        put(i, header[i]);
    for (size_t i = 128; i < b.size(); ++i)
        b[i] = uint8_t(i * 17);
    ddsvolume::View v;
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    CHECK(v.width == 4 && v.height == 2 && v.depth == 8 && v.levels == 4 && v.format == 21);
    CHECK(v.mip[1].width == 2 && v.mip[1].height == 1 && v.mip[1].depth == 4);
    CHECK(v.mip[3].offset == b.size() - 4 && v.mip[3].bytes == 4);
    CHECK(v.mip[0].row_pitch == 16 && v.mip[0].slice_pitch == 32);
    CHECK(!ddsvolume::parse(nullptr, b.size(), v));
    for (size_t n = 0; n < b.size(); ++n) {
        CHECK(!ddsvolume::parse(b.data(), n, v));
        CHECK(v.levels == 0 && v.mip[0].bytes == 0);
    }
    b.push_back(0);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    b.pop_back();
    for (auto [word, value] : std::initializer_list<std::pair<unsigned, uint32_t>>{
             {0, 0},   {1, 123},     {2, 0x21007},   {2, 0x821007 | 0x80000},
             {3, 0},   {4, 513},     {6, 0},         {7, 0},
             {7, 5},   {19, 31},     {20, 4},        {21, 0x30315844},
             {22, 24}, {23, 0xff},   {24, 0xff},     {25, 0xff0000},
             {26, 0},  {27, 0x1008}, {27, 0x400008}, {28, 0x200200},
             {29, 1},  {30, 1},      {31, 1},        {5, 16}}) {
        put(word, value);
        CHECK(!ddsvolume::parse(b.data(), b.size(), v));
        CHECK(v.width == 0 && v.mip[0].bytes == 0);
        put(word, header[word]);
    }
    put(2, header[2] | 8);
    put(5, 16);
    CHECK(ddsvolume::parse(b.data(), b.size(), v));
    put(5, 20); // unsupported padded rows cannot be silently read as packed
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    put(2, header[2]);
    put(5, 0);
    put(20, 0x40);
    put(26, 0);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.format == 22);
    put(3, 512);
    put(4, 512);
    put(6, 512);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v)); // reject budget before pointer arithmetic
    put(3, 0xffffffff);
    put(4, 0xffffffff);
    put(6, 0xffffffff);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
    // A valid single-mip non-cubic texture without mip-count metadata.
    b.resize(128 + 3 * 1 * 5 * 4);
    put(2, 0x801007);
    put(3, 1);
    put(4, 3);
    put(6, 5);
    put(7, 0);
    put(27, 0x1008);
    CHECK(ddsvolume::parse(b.data(), b.size(), v) && v.levels == 1);
    put(7, 2);
    CHECK(!ddsvolume::parse(b.data(), b.size(), v));
}
static unsigned draws = 0, rejected_draws = 0;
static bool admission_enabled = false;
static HostD9Draw rejected;
extern "C" void host_d9_admission_entry(uint64_t) {}
extern "C" int host_d9_admission_enabled(void) {
    return admission_enabled;
}
extern "C" void host_d9_rejected_draw(const HostD9Draw *d) {
    rejected = *d;
    ++rejected_draws;
}
extern "C" int host_d9_active(void) {
    return 1;
}
extern "C" void host_d9_draw(const HostD9Draw *d) {
    captured = *d;
    ++draws;
}

struct VolumeUpload {
    uint32_t id, slice, level, pitch;
    std::vector<uint8_t> data;
};
static std::unordered_map<uint32_t, HostD9TextureDesc> defined_textures;
static std::vector<VolumeUpload> volume_uploads;
static std::vector<uint32_t> dropped_textures;
extern "C" void host_d9_texture_define(const HostD9TextureDesc *d) {
    defined_textures[d->id] = *d;
}
extern "C" void host_d9_texture_upload(uint32_t id, uint32_t slice, uint32_t level,
                                       const uint8_t *bytes, uint32_t pitch) {
    const auto it = defined_textures.find(id);
    if (it == defined_textures.end() || it->second.kind != HOST_D9_TEX_VOLUME)
        return;
    const uint32_t h = std::max(it->second.height >> level, 1u);
    const auto *p = static_cast<const uint8_t *>(bytes);
    volume_uploads.push_back({id, slice, level, pitch, {p, p + pitch * h}});
}
extern "C" void host_d9_texture_drop(uint32_t id) {
    dropped_textures.push_back(id);
    defined_textures.erase(id);
}
static uint32_t make_volume(uint32_t dev, uint32_t pool = 1, uint32_t fmt = 21, uint32_t usage = 0,
                            uint32_t w = 2, uint32_t h = 2, uint32_t d = 2, uint32_t levels = 0) {
    CHECK(call_method(dev, 24, {w, h, d, levels, usage, fmt, pool, sc(0x600), 0}) == 0);
    CHECK(rd32(sc(0x600)) != 0);
    return rd32(sc(0x600));
}
static void test_volume(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c;
    const uint32_t tex = make_volume(dev);
    const uint32_t id = com_this(tex)->id;
    CHECK(call_method(tex, 10) == 4); // D3DRTYPE_VOLUMETEXTURE
    CHECK(call_method(tex, 13) == 2);
    CHECK(call_method(tex, 17, {0, sc(0x620)}) == 0);
    const uint32_t expected[] = {21, 2, 0, 1, 2, 2, 2};
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    CHECK(call_method(tex, 17, {1, sc(0x620)}) == 0);
    CHECK(rd32(sc(0x630)) == 1 && rd32(sc(0x634)) == 1 && rd32(sc(0x638)) == 1);
    CHECK(call_method(tex, 19, {0, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    const uint32_t data = rd32(sc(0x668));
    for (uint32_t i = 0; i < 32; ++i)
        gm_ptr(data)[i] = uint8_t(i);
    CHECK(call_method(tex, 19, {0, sc(0x680), 0, 0}) == invalid); // no nested lock
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 20, {0}) == invalid);
    volume_uploads.clear();
    call_method(tex, 9); // PreLoad uploads canonical storage
    CHECK(defined_textures.at(id).depth == 2 && defined_textures.at(id).levels == 2);
    CHECK(volume_uploads.size() == 3);
    for (uint32_t z = 0; z < 2; ++z) {
        const auto &u = volume_uploads[z];
        CHECK(u.id == id && u.slice == z && u.level == 0 && u.pitch == 8 && u.data.size() == 16);
        for (uint32_t i = 0; i < 16; ++i)
            CHECK(u.data[i] == uint8_t(z * 16 + i));
    }
    volume_uploads.clear();
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // no duplicate unchanged upload
    // A non-origin sub-box returns full pitches and an offset into the same mip.
    const uint32_t box[] = {1, 1, 2, 2, 1, 2};
    memcpy(gm_ptr(sc(0x700)), box, sizeof box);
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x8000}) == 0);
    CHECK(rd32(sc(0x660)) == 8 && rd32(sc(0x664)) == 16);
    CHECK(rd32(rd32(sc(0x668))) == 0x1f1e1d1c);
    wr32(rd32(sc(0x668)), 0xabcdef01);
    CHECK(call_method(tex, 20, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.empty()); // NO_DIRTY_UPDATE defers publication
    CHECK(call_method(tex, 21, {sc(0x700)}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads.size() == 3);
    CHECK(volume_uploads[1].data[12] == 1 && volume_uploads[1].data[15] == 0xab);
    volume_uploads.clear();
    CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0x10}) == 0);
    wr32(rd32(sc(0x668)), 0); // illegal guest write through READONLY cannot publish
    CHECK(call_method(tex, 20, {0}) == 0);
    CHECK(call_method(tex, 21, {0}) == 0);
    call_method(tex, 9);
    CHECK(volume_uploads[1].data[15] == 0xab);
    // Fail before writing outputs or mutating storage for bad extents/pointers/flags.
    for (uint32_t level : {2u, 0xffffffffu}) {
        wr32(sc(0x660), 0xdeadbeef);
        CHECK(call_method(tex, 17, {level, sc(0x660)}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
        CHECK(call_method(tex, 19, {level, sc(0x660), 0, 0}) == invalid);
        CHECK(rd32(sc(0x660)) == 0xdeadbeef);
    }
    for (uint32_t off : {0u, 4u, 8u, 12u, 16u, 20u}) {
        memcpy(gm_ptr(sc(0x700)), box, sizeof box);
        wr32(sc(0x700) + off, 0xffffffffu);
        CHECK(call_method(tex, 19, {0, sc(0x660), sc(0x700), 0}) == invalid);
    }
    for (uint32_t ptr : {0u, 0xfffffffcu}) {
        CHECK(call_method(tex, 19, {0, ptr, 0, 0}) == invalid);
        CHECK(call_method(tex, 17, {0, ptr}) == invalid);
        CHECK(call_method(tex, 18, {0, ptr}) == invalid);
    }
    CHECK(call_method(tex, 19, {0, sc(0x660), 0xfffffffcu, 0}) == invalid);
    for (uint32_t flags : {0x1000u, 0x2000u, 0x2010u, 0xffffffffu})
        CHECK(call_method(tex, 19, {0, sc(0x660), 0, flags}) == invalid);
    CHECK(call_method(tex, 11, {99}) == 0);
    CHECK(call_method(tex, 12) == 1);
    CHECK(call_method(tex, 11, {0}) == 1);
    CHECK(call_method(tex, 7, {19}) == 0 && call_method(tex, 8) == 19);
    // Prefix interface identity, a real mip interface and parent lifetime.
    const uint32_t base_iid[] = {0x580ca87e, 0x4d541d3c, 0xd3b71d99, 0xce98c2e3};
    const uint32_t tex_iid[] = {0x2518526c, 0x4111e789, 0xef47b9a7, 0xe6138d32};
    const uint32_t vol_iid[] = {0x24f416e6, 0x4aa71f67, 0x3fd38eb8, 0xa128316f};
    memcpy(gm_ptr(sc(0x740)), base_iid, 16);
    CHECK(call_method(tex, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 1);
    CHECK(call_method(tex, 18, {0, sc(0x600)}) == 0);
    const uint32_t mip = rd32(sc(0x600)), mip_id = com_this(mip)->id;
    CHECK(com_this(tex)->refs == 2);
    CHECK(call_method(mip, 8, {sc(0x620)}) == 0);
    CHECK(!memcmp(gm_ptr(sc(0x620)), expected, sizeof expected));
    memcpy(gm_ptr(sc(0x740)), vol_iid, 16);
    CHECK(call_method(mip, 0, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == mip);
    CHECK(call_method(mip, 2) == 1);
    memcpy(gm_ptr(sc(0x740)), tex_iid, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == tex);
    CHECK(call_method(tex, 2) == 2);
    memset(gm_ptr(sc(0x740)), 0xaa, 16);
    CHECK(call_method(mip, 7, {sc(0x740), sc(0x600)}) == 0x80004002u);
    CHECK(rd32(sc(0x600)) == 0);
    CHECK(call_method(tex, 2) == 1); // external mip alone now keeps the resource alive
    CHECK(call_method(mip, 9, {sc(0x660), 0, 0x10}) == 0);
    CHECK(gm_ptr(rd32(sc(0x668)))[31] == 0xab);
    CHECK(call_method(mip, 10) == 0);
    CHECK(call_method(mip, 2) == 0);
    CHECK(!com_get(id) && !com_get(mip_id));
    CHECK(!defined_textures.count(id));
    // The longest dimension determines mip count, even when it is depth.
    const uint32_t deep = make_volume(dev, 1, 28, 0, 1, 1, 4);
    CHECK(call_method(deep, 13) == 3);
    CHECK(call_method(deep, 19, {2, sc(0x660), 0, 0}) == 0);
    CHECK(rd32(sc(0x660)) == 1 && rd32(sc(0x664)) == 1);
    CHECK(call_method(deep, 20, {2}) == 0);
    call_method(deep, 2);
    for (const auto dims : {std::array<uint32_t, 3>{0, 2, 2},
                            {2, 0, 2},
                            {2, 2, 0},
                            {513, 2, 2},
                            {2, 513, 2},
                            {2, 2, 513},
                            {512, 512, 512},
                            {0xffffffffu, 0xffffffffu, 0xffffffffu}}) {
        wr32(sc(0x600), 0xdeadbeef);
        CHECK(call_method(dev, 24, {dims[0], dims[1], dims[2], 0, 0, 21, 1, sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    for (const auto args : {std::array<uint32_t, 4>{3, 0, 21, 1},
                            {0, 1, 21, 1},
                            {0, 2, 21, 1},
                            {0, 0x400, 21, 1},
                            {0, 0x200, 21, 1},
                            {0, 0, 75, 1},
                            {0, 0, 0x31545844, 1},
                            {0, 0, 21, 4}}) {
        CHECK(call_method(dev, 24, {2, 2, 2, args[0], args[1], args[2], args[3], sc(0x600), 0}) ==
              invalid);
        CHECK(rd32(sc(0x600)) == 0);
    }
    // Default-pool dynamic locks and system-memory to GPU updates are distinct paths.
    const uint32_t dyn = make_volume(dev, 0, 22, 0x200);
    CHECK(call_method(dyn, 19, {0, sc(0x660), 0, 0x2000}) == 0);
    CHECK(call_method(dyn, 20, {0}) == 0);
    call_method(dyn, 2);
    const uint32_t src = make_volume(dev, 2), dst = make_volume(dev, 0);
    const uint32_t dst_id = com_this(dst)->id;
    CHECK(call_method(dst, 19, {0, sc(0x660), 0, 0}) == invalid);
    CHECK(call_method(dev, 65, {0, src}) == invalid); // system-memory resources cannot be sampled
    CHECK(call_method(src, 19, {0, sc(0x660), 0, 0}) == 0);
    memset(gm_ptr(rd32(sc(0x668))), 0x71, 32);
    CHECK(call_method(dev, 31, {src, dst}) == invalid); // locked resource, no partial update
    CHECK(call_method(src, 20, {0}) == 0);
    CHECK(call_method(dev, 31, {src, dst}) == 0);
    volume_uploads.clear();
    call_method(dst, 9);
    CHECK(volume_uploads.size() == 3 && volume_uploads[0].data[0] == 0x71);
    CHECK(call_method(dev, 65, {0, dst}) == 0);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x600)}) == 0);
    const uint32_t declaration = rd32(sc(0x600));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float tri[] = {-1, 1, .5f, 3, 1, .5f, -1, -3, .5f};
    memcpy(gm_ptr(sc(0x400)), tri, sizeof tri);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(captured.sampler_texture[0] == dst_id);
    call_method(declaration, 2);
    CHECK(call_method(dev, 64, {0, sc(0x600)}) == 0);
    CHECK(rd32(sc(0x600)) == dst);
    call_method(dst, 2);             // returned GetTexture reference
    CHECK(call_method(dst, 2) == 1); // device binding alone retains it
    CHECK(com_get(dst_id));
    CHECK(call_method(dev, 65, {0, 0}) == 0);
    CHECK(!com_get(dst_id));
    call_method(src, 2);
}

struct Bytes {
    std::vector<uint8_t> data;
    uint32_t words(std::initializer_list<uint32_t> list) {
        const uint32_t start = uint32_t(data.size());
        for (uint32_t x : list)
            for (unsigned k = 0; k < 4; ++k)
                data.push_back(uint8_t(x >> (k * 8)));
        return start;
    }
    uint32_t raw(const std::vector<uint8_t> &b) {
        const uint32_t start = uint32_t(data.size());
        data.insert(data.end(), b.begin(), b.end());
        while (data.size() % 4)
            data.push_back(0);
        return start;
    }
    uint32_t text(const char *s) {
        uint32_t start = words({uint32_t(strlen(s) + 1)});
        raw(std::vector<uint8_t>(s, s + strlen(s) + 1));
        return start;
    }
};

// Hand-written CTAB: one BOOL parameter, independent register index in each
// shader. These bytes are a test fixture, not extracted proprietary assets.
static std::vector<uint8_t> shader(bool vertex, const char *name, uint32_t index, uint32_t count) {
    const uint32_t version = vertex ? 0xfffe0200u : 0xffff0200u;
    Bytes table;
    table.words({28, 0, version, 1, 28, 0, 0});
    table.words({64, index << 16, count, 48, 0});
    table.words({1u << 16, 0x00010001u, count, 0});
    table.raw(std::vector<uint8_t>(name, name + strlen(name) + 1));
    Bytes out;
    out.words({version, 0xfffeu | uint32_t((table.data.size() + 4) / 4) << 16, 0x42415443u});
    out.raw(table.data);
    if (vertex)
        out.words({0x0200001fu, 0x80000000u, 0x900f0000u, 0x02000001u, 0xc00f0000u, 0x90e40000u});
    else
        out.words({0x02000001u, 0x800f0800u, 0xa0e40000u});
    out.words({0xffffu});
    return out.data;
}

static std::vector<uint8_t> effect() {
    Bytes b;
    b.words({0}); // offset 0 is the null name
    const uint32_t sn = b.text("Switch"), mn = b.text("Many"), tn = b.text("Main"),
                   pn = b.text("P0");
    const uint32_t st = b.words({1, 0, sn, 0, 0, 1, 1});
    const uint32_t mt = b.words({1, 0, mn, 0, 2, 1, 1});
    const uint32_t sv = b.words({0}), mv = b.words({0, 1});
    const uint32_t vt = b.words({16, 4, 0, 0, 0}), pt = b.words({15, 4, 0, 0, 0});
    const uint32_t vi = b.words({1}), pi = b.words({2});
    const uint32_t start = b.words({2, 1, 0, 2});
    b.words({st, sv, 0, 0, mt, mv, 0, 0});
    b.words({tn, 0, 1, pn, 0, 2, 146, 0, vt, vi, 147, 0, pt, pi});
    b.words({2, 0}); // two shader objects, no extra resources
    for (bool vertex : {true, false}) {
        auto code = shader(vertex, vertex ? "Switch" : "Many", vertex ? 15 : 0, vertex ? 1 : 2);
        b.words({vertex ? 1u : 2u, uint32_t(code.size())});
        b.raw(code);
    }
    Bytes out;
    out.words({0xfeff0901u, start});
    out.raw(b.data);
    return out.data;
}

// Bounded readback (GetRenderTargetData + LockRect DONOTWAIT) against a scripted renderer.
static int rb_ready = 0, rb_sync_reads = 0, rb_forgets = 0, rb_requests = 0;
static uint32_t rb_ticket = 0, rb_source = 0;
extern "C" void host_d9_texture_read_async(uint32_t id, uint32_t, uint32_t, uint32_t ticket) {
    rb_source = id;
    rb_ticket = ticket;
    rb_ready = 0;
    ++rb_requests;
}
extern "C" int host_d9_texture_read_poll(uint32_t ticket, uint8_t *b, uint32_t pitch, uint32_t w,
                                         uint32_t h) {
    if (ticket != rb_ticket)
        return -1;
    if (!rb_ready)
        return 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t v = 0x11000000u + y * 16 + x;
            memcpy(b + (size_t)y * pitch + x * 4, &v, 4);
        }
    return 1;
}
extern "C" void host_d9_texture_read_forget(uint32_t) {
    ++rb_forgets;
}
extern "C" int host_d9_texture_read(uint32_t, uint32_t, uint32_t, uint8_t *b, uint32_t pitch) {
    ++rb_sync_reads;
    memset(b, 0x55, (size_t)pitch * 4);
    return 1;
}
static void test_state_getters_and_readback(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086c, still_drawing = 0x8876021c, not_found = 0x88760866;
    // Render and sampler states read back what was set, and the documented defaults otherwise.
    CHECK(call_method(dev, 57, {27, 1}) == 0);
    CHECK(call_method(dev, 58, {27, sc(0x10)}) == 0 && rd32(sc(0x10)) == 1);
    CHECK(call_method(dev, 58, {22, sc(0x10)}) == 0 && rd32(sc(0x10)) == 3); // CULLMODE ccw
    CHECK(call_method(dev, 58, {300, sc(0x10)}) == invalid);
    CHECK(call_method(dev, 69, {0, 5, 2}) == 0);
    CHECK(call_method(dev, 68, {0, 5, sc(0x10)}) == 0 && rd32(sc(0x10)) == 2);
    CHECK(call_method(dev, 68, {16, 5, sc(0x10)}) == invalid);
    // Render targets: a referenced surface for 0, NOTFOUND for an unset one.
    CHECK(call_method(dev, 38, {0, sc(0x1c)}) == 0);
    const uint32_t original_target = rd32(sc(0x1c));
    CHECK(original_target != 0);
    CHECK(call_method(dev, 28, {4, 4, 21, 0, 0, 0, sc(0x20), 0}) == 0);
    const uint32_t rt = rd32(sc(0x20));
    CHECK(rt != 0);
    CHECK(call_method(dev, 37, {0, rt}) == 0);
    CHECK(call_method(dev, 38, {0, sc(0x24)}) == 0 && com_this(rd32(sc(0x24))) == com_this(rt));
    call_method(rd32(sc(0x24)), 2);
    wr32(sc(0x24), 0x1234);
    CHECK(call_method(dev, 38, {1, sc(0x24)}) == not_found && rd32(sc(0x24)) == 0);
    // The vertex declaration: the one set, null after clearing it.
    const uint8_t decl[] = {0, 0, 0, 0, 3, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0x28)}) == 0);
    const uint32_t d = rd32(sc(0x28));
    CHECK(call_method(dev, 87, {d}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && com_this(rd32(sc(0x2c))) == com_this(d));
    call_method(rd32(sc(0x2c)), 2);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 88, {sc(0x2c)}) == 0 && rd32(sc(0x2c)) == 0);
    call_method(d, 2);
    // GetRenderTargetData queues the copy; a DONOTWAIT lock answers WASSTILLDRAWING until the
    // renderer has it, then locks the delivered image. A lock without the flag waits.
    CHECK(call_method(dev, 36, {4, 4, 21, 2, sc(0x30), 0}) == 0);
    const uint32_t sys = rd32(sc(0x30));
    CHECK(call_method(dev, 32, {rt, sys}) == 0);
    CHECK(rb_requests == 1 && rb_ticket == com_this(sys)->id && rb_source == com_this(rt)->id);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == still_drawing);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == still_drawing);
    rb_ready = 1;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4010}) == 0);
    const uint32_t pitch = rd32(sc(0x40)), bits = rd32(sc(0x44));
    CHECK(pitch >= 16 && rd32(bits) == 0x11000000u && rd32(bits + pitch + 4) == 0x11000011u);
    CHECK(call_method(sys, 14) == 0);
    const int forgets = rb_forgets;
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0x4000}) == 0); // nothing pending: plain lock
    CHECK(call_method(sys, 14) == 0 && rb_forgets == forgets);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 2);
    CHECK(call_method(sys, 13, {sc(0x40), 0, 0}) == 0 && rb_sync_reads == 1);
    CHECK(rd32(rd32(sc(0x44))) == 0x55555555u);
    CHECK(call_method(sys, 14) == 0);
    // Sizes must match.
    CHECK(call_method(dev, 36, {8, 4, 21, 2, sc(0x34), 0}) == 0);
    const uint32_t wide = rd32(sc(0x34));
    CHECK(call_method(dev, 32, {rt, wide}) == invalid && rb_requests == 2);
    call_method(wide, 2);
    CHECK(call_method(dev, 32, {rt, sys}) == 0 && rb_requests == 3);
    call_method(sys, 2); // destroyed while pending: the ticket is forgotten
    CHECK(rb_forgets > forgets);
    CHECK(call_method(dev, 37, {0, original_target}) == 0);
    call_method(original_target, 2);
    call_method(rt, 2);
}

// The process's peak resident memory, in bytes. Not measured on Windows,
// whose <windows.h> cannot share a file with the kit's own Win32 types.
static uint64_t peak_memory_bytes() {
#ifdef _WIN32
    return 0;
#else
    struct rusage r{};
    getrusage(RUSAGE_SELF, &r);
#ifdef __APPLE__
    return (uint64_t)r.ru_maxrss; // bytes on macOS
#else
    return (uint64_t)r.ru_maxrss * 1024; // kilobytes on Linux
#endif
#endif
}

// Test155: the four draw calls refuse a primitive count above the MaxPrimitiveCount this device reports in its caps
// (and indexed draws a vertex count above MaxVertexIndex + 1) with D3DERR_INVALIDCALL, before any size arithmetic,
// for every topology, and without growing the process. At the limit they are accepted.
static void test_draw_limits(uint32_t dev) {
    const uint32_t invalid = 0x8876086cu;
    CHECK(call_method(dev, 7, {sc(0x800)}) == 0); // GetDeviceCaps
    const uint32_t max_prims = rd32(sc(0x800) + 180), max_index = rd32(sc(0x800) + 184);
    CHECK(max_prims == 1048575 && max_index == 1048575);
    memset(gm_ptr(sc(0x1000)), 0, 0x1000); // degenerate vertices and indices
    const uint64_t before = peak_memory_bytes();
    for (uint32_t prim = 1; prim <= 6; ++prim) {
        for (uint32_t count : {max_prims + 1, 0x7fffffffu, 0x80000000u, 0x80000000u + 350u, 0xffffffffu}) {
            CHECK(call_method(dev, 83, {prim, count, sc(0x1000), 16}) == invalid);          // DrawPrimitiveUP
            CHECK(call_method(dev, 84, {prim, 0, 3, count, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
            CHECK(call_method(dev, 81, {prim, 0, count}) == invalid);                        // DrawPrimitive
            CHECK(call_method(dev, 82, {prim, 0, 0, 3, 0, count}) == invalid);              // DrawIndexedPrimitive
        }
        // vertex count over the index limit, primitive count fine
        CHECK(call_method(dev, 84, {prim, 0, max_index + 2, 1, sc(0x1000), 101, sc(0x1000), 16}) == invalid);
        CHECK(call_method(dev, 82, {prim, 0, 0, max_index + 2, 0, 1}) == invalid);
        // small valid draws are accepted
        CHECK(call_method(dev, 83, {prim, 1, sc(0x1000), 16}) == 0);
        CHECK(call_method(dev, 84, {prim, 0, 3, 1, sc(0x1000), 101, sc(0x1000), 16}) == 0);
    }
    // at and just below the limit: accepted (point lists, so the vertex count equals the primitive count; the
    // degenerate data lives anywhere in the arena)
    for (uint32_t count : {max_prims - 1, max_prims}) {
        CHECK(call_method(dev, 81, {1, 0, count}) == 0);
        CHECK(call_method(dev, 83, {1, count, 0x00100000u, 16}) == 0);
    }
    // Far below a runaway allocation.
    CHECK(peak_memory_bytes() - before < (uint64_t)512 * 1024 * 1024);
}

// Test155: with no depth-stencil surface set, GetDepthStencilSurface returns D3DERR_NOTFOUND and a null pointer
// (Direct3D 9), every time, and makes nothing; with one set it returns that one.
static void test_depth_stencil_get(uint32_t dev) {
    const uint32_t notfound = 0x88760866u, out = sc(0x900), made = sc(0x904);
    CHECK(call_method(dev, 39, {0}) == 0); // SetDepthStencilSurface(NULL)
    for (int i = 0; i < 64; ++i) {
        wr32(out, 0xdeadbeefu);
        CHECK(call_method(dev, 40, {out}) == notfound);
        CHECK(rd32(out) == 0);
    }
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0); // CreateDepthStencilSurface D24S8
    const uint32_t surface = rd32(made);
    CHECK(surface != 0);
    CHECK(call_method(dev, 39, {surface}) == 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) != 0);
    if (rd32(out))
        call_method(rd32(out), 2); // Release the reference GetDepthStencilSurface added
    CHECK(call_method(dev, 39, {0}) == 0);
    CHECK(call_method(dev, 40, {out}) == notfound);
    if (surface)
        call_method(surface, 2);
}

// Test157c: depth-stencil reference rules of Direct3D 9 (the audit found the kit diverged in all three):
// SetDepthStencilSurface AddRefs the bound surface (the device keeps it alive after the app releases it), and
// Reset drops only the device's own references, never the app's.
static void test_depth_stencil_refs(uint32_t dev) {
    const uint32_t made = sc(0x910), out = sc(0x914), pp = sc(0x940);
    auto addref = [](uint32_t o) { return call_method(o, 1); };
    auto release = [](uint32_t o) { return call_method(o, 2); };
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t s = rd32(made), s_id = com_this(s)->id;
    CHECK(call_method(dev, 39, {s}) == 0);
    CHECK(addref(s) == 3); // app + device + this
    release(s);
    CHECK(release(s) == 1); // the app lets go; the device still holds it
    CHECK(com_get(s_id) != nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0);
    CHECK(rd32(out) && com_this(rd32(out)) && com_this(rd32(out))->id == s_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(call_method(dev, 39, {0}) == 0); // unbinding drops the last reference
    CHECK(com_get(s_id) == nullptr);
    // Reset with the automatic depth buffer on, while an app-owned surface is bound
    CHECK(call_method(dev, 29, {16, 16, 75, 0, 0, 0, made, 0}) == 0);
    const uint32_t e = rd32(made), e_id = com_this(e)->id;
    CHECK(call_method(dev, 39, {e}) == 0);
    memset(gm_ptr(pp), 0, 56);
    wr32(pp, 8);
    wr32(pp + 4, 8);
    wr32(pp + 36, 1);
    wr32(pp + 40, 75);
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(e_id) != nullptr); // the app's surface survives Reset
    CHECK(addref(e) == 2);           // app + this: the device let go of its binding only
    release(e);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0); // the new automatic depth buffer is bound
    const uint32_t auto1 = rd32(out) && com_this(rd32(out)) ? com_this(rd32(out))->id : 0;
    CHECK(auto1 != 0 && auto1 != e_id);
    if (rd32(out))
        release(rd32(out));
    CHECK(release(e) == 0); // the app's last reference
    CHECK(com_get(e_id) == nullptr);
    // a second Reset replaces the automatic depth buffer; the old one goes once nothing holds it
    CHECK(call_method(dev, 16, {pp}) == 0);
    CHECK(com_get(auto1) == nullptr);
    wr32(out, 0);
    CHECK(call_method(dev, 40, {out}) == 0 && rd32(out));
    if (rd32(out))
        release(rd32(out));
}

// Test159: a texture level shares its texture's lifetime (Direct3D 9): releasing the level gives back the texture
// reference it took, in either release order, and a level obtained twice needs two releases.
static void test_surface_container_refs(uint32_t dev) {
    const uint32_t texp = sc(0x980), surfp = sc(0x984), surf2p = sc(0x988);
    auto release = [](uint32_t o) { return call_method(o, 2); };
    for (int order = 0; order < 2; ++order) {
        CHECK(call_method(dev, 23, {16, 16, 1, 1 /* RENDERTARGET */, 21, 0, texp, 0}) == 0); // CreateTexture
        const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
        CHECK(call_method(tex, 18, {0, surfp}) == 0); // GetSurfaceLevel
        const uint32_t surf = rd32(surfp), surf_id = com_this(surf)->id;
        if (order == 0) {
            release(tex);                              // the app lets the texture go first...
            CHECK(com_get(tex_id) != nullptr);         // ...the level keeps it alive
            release(surf);
        } else {
            release(surf);
            CHECK(com_get(tex_id) != nullptr);         // the app still holds the texture
            release(tex);
        }
        CHECK(com_get(tex_id) == nullptr && com_get(surf_id) == nullptr);
    }
    // a level obtained twice, and through GetRenderTarget after SetRenderTarget
    CHECK(call_method(dev, 23, {16, 16, 1, 1, 21, 0, texp, 0}) == 0);
    const uint32_t tex = rd32(texp), tex_id = com_this(tex)->id;
    CHECK(call_method(tex, 18, {0, surfp}) == 0);
    CHECK(call_method(tex, 18, {0, surf2p}) == 0);
    const uint32_t s1 = rd32(surfp), s2 = rd32(surf2p);
    CHECK(call_method(dev, 37, {0, s1}) == 0);    // SetRenderTarget(0, level)
    CHECK(call_method(dev, 38, {0, surfp}) == 0); // GetRenderTarget(0)
    const uint32_t s3 = rd32(surfp);
    CHECK(call_method(dev, 38, {1, surfp}) != 0 || rd32(surfp) == 0); // nothing on slot 1
    CHECK(call_method(dev, 18, {0, 0, 0, surf2p + 4}) == 0); // GetBackBuffer: put the back buffer back on slot 0
    const uint32_t bb = rd32(surf2p + 4);
    CHECK(call_method(dev, 37, {0, bb}) == 0);
    release(bb);
    release(tex);
    release(s1);
    release(s2);
    CHECK(com_get(tex_id) != nullptr); // GetRenderTarget's reference is still outstanding
    release(s3);
    CHECK(com_get(tex_id) == nullptr);
}

static uint32_t make_device() {
    const uint32_t api = call_shim(tramp("d3d9.dll", "Direct3DCreate9"), {32});
    CHECK(api != 0);
    wr32(sc(0x100), 8);
    wr32(sc(0x104), 8);
    CHECK(call_method(api, 16, {0, 1, 0, 0, sc(0x100), sc(0)}) == 0);
    const uint32_t dev = rd32(sc(0));
    CHECK(dev != 0);
    call_method(api, 2);
    return dev;
}

static void test_device_booleans(uint32_t dev) {
    // D3D9 vtable Set/Get VS B=98/99, PS B=113/114. The actual guest
    // stack and vtable calls catch ABI errors as well as bounds mistakes.
    for (uint32_t set : {98u, 113u}) {
        for (uint32_t i = 0; i < 16; ++i)
            wr32(sc(0x200) + i * 4, i & 1 ? 0xfffffff9u : 0);
        CHECK(call_method(dev, set, {0, sc(0x200), 16}) == 0);
        CHECK(call_method(dev, set + 1, {0, sc(0x300), 16}) == 0);
        CHECK(memcmp(gm_ptr(sc(0x200)), gm_ptr(sc(0x300)), 64) == 0);
        wr32(sc(0x200), 3);
        for (const auto range :
             {std::array<uint32_t, 2>{15, 2}, {16, 1}, {0xffffffffu, 1}, {0, 0xffffffffu}}) {
            CHECK(call_method(dev, set, {range[0], sc(0x200), range[1]}) == 0x8876086cu);
            wr32(sc(0x300), 0xdeadbeefu);
            CHECK(call_method(dev, set + 1, {range[0], sc(0x300), range[1]}) == 0x8876086cu);
            CHECK(rd32(sc(0x300)) == 0xdeadbeefu);
        }
        CHECK(call_method(dev, set + 1, {15, sc(0x300), 1}) == 0);
        CHECK(rd32(sc(0x300)) == 0xfffffff9u); // invalid writes were atomic
        CHECK(call_method(dev, set, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0, 1}) == 0x8876086cu);
        CHECK(call_method(dev, set, {0, 0xfffffffcu, 2}) == 0x8876086cu);
        CHECK(call_method(dev, set + 1, {0, 0xfffffffcu, 2}) == 0x8876086cu);
    }
}

static void test_effect_booleans(uint32_t dev) {
    const auto fx = effect();
    // Minimal loaded PE resource directory. This exercises the same export
    // used by a game, with an allocated guest module instead of a disk image.
    const uint32_t module = heap_alloc(0x1000 + uint32_t(fx.size()), true, 16);
    CHECK(module != 0);
    wr32(module, 0x5a4d);
    wr32(module + 0x3c, 0x40);
    wr32(module + 0x40, 0x4550);
    wr32(module + 0x40 + 0x18 + 0x60 + 16, 0x200);
    const uint32_t root = module + 0x200;
    wr32(root + 12, 1u << 16);
    wr32(root + 16, 10);
    wr32(root + 20, 0x80000020u);
    wr32(root + 0x20 + 12, 1u << 16);
    wr32(root + 0x20 + 16, 1);
    wr32(root + 0x20 + 20, 0x80000040u);
    wr32(root + 0x40 + 12, 1u << 16);
    wr32(root + 0x40 + 16, 1033);
    wr32(root + 0x40 + 20, 0x60);
    wr32(root + 0x60, 0x1000);
    wr32(root + 0x64, uint32_t(fx.size()));
    memcpy(gm_ptr(module + 0x1000), fx.data(), fx.size());
    CHECK(call_shim(tramp("d3dx9_26.dll", "D3DXCreateEffectFromResourceA"),
                    {dev, module, 1, 0, 0, 0, 0, sc(0), 0}) == 0);
    const uint32_t e = rd32(sc(0));
    CHECK(e != 0);
    memcpy(gm_ptr(sc(0x200)), "Switch", 7);
    memcpy(gm_ptr(sc(0x220)), "Many", 5);
    const uint32_t s = call_method(e, 9, {0, sc(0x200)}), m = call_method(e, 9, {0, sc(0x220)});
    CHECK(s != 0 && m != 0 && s != m);
    CHECK(call_method(e, 63, {sc(0x20), 0}) == 0);
    CHECK(rd32(sc(0x20)) == 1);
    CHECK(call_method(e, 64, {0}) == 0);
    D9Pipeline &pl = d9_pipeline(com_this(dev)->id);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 0 && pl.pbool[1] == 1);
    CHECK(!pl.vs.empty() && !pl.ps.empty());
    for (uint32_t value : {1u, 0u, 1u}) {
        CHECK(call_method(e, 22, {s, value}) == 0);
        wr32(sc(0x240), value);
        wr32(sc(0x244), !value);
        CHECK(call_method(e, 24, {m, sc(0x240), 2}) == 0);
        CHECK(call_method(e, 65) == 0);
        CHECK(pl.vbool[15] == int32_t(value));
        CHECK(pl.pbool[0] == int32_t(value) && pl.pbool[1] == int32_t(!value));
    }
    // Test75: incremental CommitChanges (d3dx9.cpp bind_constants). The registers remember which
    // effect and shader wrote them; an unchanged commit keeps the values, a parameter written between
    // two commits reaches the registers, and a device constant setter in between makes the next
    // commit write every constant again (the shim's behaviour before Test75 too).
    CHECK(pl.vbinder.fx == com_this(e)->id && pl.pbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.pbool[0] == 1 && pl.pbool[1] == 0);
    wr32(sc(0x260), 0);
    CHECK(call_method(dev, 98, {15, sc(0x260), 1}) == 0); // SetVertexShaderConstantB
    CHECK(pl.vbool[15] == 0 && pl.vbinder.fx == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1 && pl.vbinder.fx == com_this(e)->id);
    CHECK(call_method(e, 22, {s, 0}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 0 && pl.pbool[0] == 1);
    CHECK(call_method(e, 22, {s, 1}) == 0);
    CHECK(call_method(e, 65) == 0);
    CHECK(pl.vbool[15] == 1);
    const uint8_t decl[] = {0, 0, 0, 0, 2, 0, 0, 0, 0xff, 0, 0, 0, 17, 0, 0, 0};
    memcpy(gm_ptr(sc(0x300)), decl, sizeof decl);
    CHECK(call_method(dev, 86, {sc(0x300), sc(0)}) == 0);
    const uint32_t declaration = rd32(sc(0));
    CHECK(call_method(dev, 87, {declaration}) == 0);
    const float triangle[] = {-1, 1, 0.5f, 3, 1, 0.5f, -1, -3, 0.5f};
    memcpy(gm_ptr(sc(0x400)), triangle, sizeof triangle);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 1);
    CHECK(captured.vbool[15] == 1 && captured.pbool[0] == 1 && captured.pbool[1] == 0);
    CHECK(captured.admission.serial == 0 && rejected_draws == 0);
    // Finish the legacy first-eight draw descriptions using harmless no-IB
    // calls before supplying deliberately invalid UP pointers below.
    for (int i = 0; i < 3; ++i)
        CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 0 && draws == 1);
    admission_enabled = true;
    CHECK(call_method(dev, 26, {36, 0, 0, 0, sc(0x50), 0}) == 0);
    uint32_t vb = rd32(sc(0x50));
    CHECK(call_method(dev, 100, {0, vb, 0, 12}) == 0);
    CHECK(call_method(dev, 27, {6, 0, 101, 0, sc(0x54), 0}) == 0);
    uint32_t ib = rd32(sc(0x54));
    CHECK(call_method(dev, 104, {ib}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(draws == 2 && captured.admission.serial != 0);
    CHECK(captured.admission.api == 82 && captured.admission.device == dev);
    CHECK(captured.admission.caller != 0);
    CHECK(captured.admission.resource_identity[0] == vb);
    CHECK(captured.admission.resource_identity[8] == ib);
    uint64_t serial = captured.admission.serial;
    CHECK(call_method(dev, 104, {0}) == 0);
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 1 && draws == 2 && rejected.admission.reason == 3);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 82);
    CHECK(rejected.stream[0].buffer == com_this(vb)->id);
    CHECK(rejected.admission.resource_identity[0] == vb);
    CHECK(rejected.target.color[0].id == captured.target.color[0].id);
    CHECK(call_method(dev, 87, {0}) == 0);
    CHECK(call_method(dev, 81, {4, 0, 1}) == 0);
    CHECK(rejected_draws == 2 && draws == 2 && rejected.admission.reason == 2);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 81);
    CHECK(call_method(dev, 87, {declaration}) == 0);
    auto saved_vs = std::move(pl.vs);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    // No vertex shader bound: the fixed-function program draws it, admitted like any other.
    CHECK(rejected_draws == 2 && draws == 3 && captured.admission.reason == 0);
    CHECK(captured.admission.serial == ++serial && captured.admission.api == 83);
    CHECK(captured.vs && captured.vs_size && captured.vconst_count == d9ffp::VS_CONSTANTS);
    pl.vs = std::move(saved_vs);
    CHECK(call_method(dev, 83, {4, 1, 0xfffffff0u, 12}) == 0);
    CHECK(rejected_draws == 3 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial);
    CHECK(call_method(dev, 84, {4, 0, 3, 1, 0xfffffff0u, 101, sc(0x400), 12}) == 0);
    CHECK(rejected_draws == 4 && draws == 3 && rejected.admission.reason == 4);
    CHECK(rejected.admission.serial == ++serial && rejected.admission.api == 84);
    CHECK(!rejected.inline_vertices && !rejected.inline_indices);
    CHECK(rejected.admission.inline_indices == 0xfffffff0u);
    CHECK(rejected.admission.inline_vertices == sc(0x400));
    CHECK(rejected.admission.resource_identity[0] == 0);
    CHECK(rejected.stream[0].buffer == 0 && rejected.index_buffer == 0);
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 4 && captured.admission.reason == 0);
    CHECK(captured.admission.serial == ++serial && captured.admission.api == 83);
    admission_enabled = false;
    CHECK(call_method(dev, 82, {4, 0, 0, 3, 0, 1}) == 0);
    CHECK(rejected_draws == 4 && draws == 4); // opt-out retains old behavior
    CHECK(call_method(dev, 83, {4, 1, sc(0x400), 12}) == 0);
    CHECK(draws == 5 && captured.admission.serial == 0);
    call_method(dev, 100, {0, 0, 0, 0});
    call_method(vb, 2);
    call_method(ib, 2);
    call_method(e, 66);
    call_method(e, 67);
    call_method(e, 2);
    call_method(declaration, 2);
}

// ---- Shaders from bytecode, FVF codes and the fixed-function pipeline --------------------------
static void putf(uint32_t at, std::initializer_list<float> v) {
    uint32_t i = 0;
    for (float f : v)
        memcpy(gm_ptr(at + 4 * i++), &f, 4);
}
static uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// A generated program decodes as the shader model it claims and translates to every language.
static bool ffp_translates(const d9ffp::Program &p, bool pixel) {
    const d9sh::Program &prog = d9sh::program_for(p.bytes);
    if (!prog.ok || prog.major != 2 || prog.pixel != pixel ||
        p.key != d9sh::code_key(p.bytes.data(), p.bytes.size())) {
        fprintf(stderr, "ffp %s program does not decode: %s\n", pixel ? "pixel" : "vertex",
                prog.why.c_str());
        return false;
    }
    std::string out, why;
    const d9msl::PixelVariant v;
    const bool ok = pixel ? d9msl::pixel_source(prog, v, &out, &why) &&
                                d9glsl::pixel_source(prog, v, &out, &why) &&
                                d9wgsl::pixel_source(prog, v, &out, &why)
                          : d9msl::vertex_source(prog, &out, &why) &&
                                d9glsl::vertex_source(prog, &out, &why) &&
                                d9wgsl::vertex_source(prog, &out, &why);
    if (!ok)
        fprintf(stderr, "ffp %s program does not translate: %s\n", pixel ? "pixel" : "vertex",
                why.c_str());
    return ok;
}

static void test_ffp_programs() {
    std::vector<uint8_t> decl;
    // FVF codes become the declarations Direct3D 9 lays the vertex out by.
    CHECK(d9ffp::fvf_declaration(0x144, &decl)); // XYZRHW | DIFFUSE | TEX1
    const std::vector<uint8_t> rhw = {0, 0, 0,  0, 3, 0, 9, 0, 0,    0, 16, 0, 4,  0, 10, 0,
                                      0, 0, 20, 0, 1, 0, 5, 0, 0xff, 0, 0,  0, 17, 0, 0,  0};
    CHECK(decl == rhw);
    // XYZ | NORMAL | DIFFUSE | SPECULAR | TEX2, the second set three floats.
    CHECK(d9ffp::fvf_declaration(0x002 | 0x010 | 0x040 | 0x080 | 0x200 | 1u << 18, &decl));
    CHECK(decl.size() == 7 * 8);
    const uint8_t want[6][4] = {{0, 2, 0, 0},  {12, 2, 3, 0},  {24, 4, 10, 0},
                                {28, 4, 10, 1}, {32, 1, 5, 0}, {40, 2, 5, 1}};
    for (int i = 0; i < 6 && decl.size() == 56; ++i)
        CHECK(decl[i * 8 + 2] == want[i][0] && decl[i * 8 + 4] == want[i][1] &&
              decl[i * 8 + 6] == want[i][2] && decl[i * 8 + 7] == want[i][3]);
    // XYZB2 with the last beta as UBYTE4 indices: one weight, then the indices.
    CHECK(d9ffp::fvf_declaration(0x008 | 0x1000, &decl) && decl.size() == 4 * 8);
    CHECK(decl.size() == 32 && decl[8 + 2] == 12 && decl[8 + 4] == 0 && decl[8 + 6] == 1 &&
          decl[16 + 2] == 16 && decl[16 + 4] == 5 && decl[16 + 6] == 2);
    CHECK(!d9ffp::fvf_declaration(0x040, &decl)); // no position

    // Every program the states below describe decodes and translates.
    auto pl = std::make_unique<D9Pipeline>();
    static float vc[d9ffp::VS_CONSTANTS][4], pc[d9ffp::PS_CONSTANTS][4];
    const int32_t vp[4] = {0, 0, 640, 480};
    const float depth[2] = {0.0f, 1.0f};
    for (uint32_t type = 1; type <= 3; ++type) {
        D9Pipeline::Light &l = pl->lights[type];
        l.type = type;
        l.f[1] = l.f[2] = l.f[3] = 1.0f;
        l.f[18] = 1.0f;
        l.f[19] = 100.0f;
        l.f[21] = 1.0f;
        l.f[24] = 0.5f;
        l.f[25] = 1.0f;
        l.enabled = true;
        pl->active_lights[pl->active_light_count++] = type;
    }
    const uint32_t fvfs[] = {0x144, 0x2d2 | 1u << 18, 0x112, 0x002, 0x4002 | 0x040, 0x00c | 0x110};
    uint32_t programs = 0;
    for (uint32_t fvf : fvfs) {
        CHECK(d9ffp::fvf_declaration(fvf, &decl));
        for (uint32_t bits = 0; bits < 64; ++bits) {
            pl->rs[137] = bits & 1;             // LIGHTING
            pl->rs[29] = bits >> 1 & 1;         // SPECULARENABLE
            pl->rs[142] = bits >> 2 & 1;        // LOCALVIEWER
            pl->rs[143] = bits >> 2 & 1;        // NORMALIZENORMALS
            pl->rs[28] = 1;                     // FOGENABLE
            pl->rs[140] = bits >> 3 & 3;        // FOGVERTEXMODE
            pl->rs[48] = bits >> 5 & 1;         // RANGEFOGENABLE
            pl->rs[147] = bits & 2 ? 1 : 0;     // AMBIENTMATERIALSOURCE
            pl->tss[0][11] = (bits % 5) << 16;  // TEXCOORDINDEX generation
            pl->tss[0][24] = (bits % 5) | (bits % 3 == 0 ? 0x100 : 0); // TEXTURETRANSFORMFLAGS
            pl->tss[1][1] = bits & 4 ? 4 : 1;   // a second stage
            const d9ffp::Program &p = d9ffp::vertex_program(*pl, decl.data(), (uint32_t)decl.size(), vp,
                                                            depth, bits & 8, vc);
            CHECK(ffp_translates(p, false));
            ++programs;
        }
    }
    // The same state finds the same program.
    CHECK(&d9ffp::vertex_program(*pl, decl.data(), (uint32_t)decl.size(), vp, depth, false, vc) ==
          &d9ffp::vertex_program(*pl, decl.data(), (uint32_t)decl.size(), vp, depth, false, vc));
    const uint8_t args[] = {0, 1, 2, 3, 4, 5, 6, 0x12, 0x21, 0x33};
    d9ffp::Textures textures;
    for (uint32_t op = 1; op <= 26; ++op)
        for (uint32_t kind : {0u, 2u, 3u, 4u}) {
            textures.kind[0] = textures.kind[1] = (uint8_t)kind;
            for (uint32_t s = 0; s < 2; ++s) {
                pl->tss[s][1] = s ? (op % 2 ? 4 : 1) : op;
                pl->tss[s][2] = args[(op + s) % 10];
                pl->tss[s][3] = args[(op + 3) % 10];
                pl->tss[s][26] = args[(op + 5) % 10];
                pl->tss[s][4] = op;
                pl->tss[s][5] = args[(op + 7) % 10];
                pl->tss[s][6] = args[op % 10];
                pl->tss[s][27] = args[(op + 1) % 10];
                pl->tss[s][28] = op % 3 == 0 ? 5 : 1; // RESULTARG temp
                pl->tss[s][24] = op % 4 == 0 ? 0x103 : 0;
            }
            pl->rs[29] = op & 1;
            CHECK(ffp_translates(d9ffp::pixel_program(*pl, textures, pc), true));
            ++programs;
        }
    CHECK(programs == 6 * 64 + 26 * 4);
}

// ps_1_1 texture-matrix lookups decode and translate: texm3x3pad/texm3x3spec into a cube map and
// texm3x2pad/texm3x2tex into a 2D texture, each from a normal fetched with _bx2.
static void test_ps1_texture_matrix() {
    const std::vector<uint32_t> spec = {0xffff0101u, 0x42u, 0xb00f0000u, 0x49u, 0xb00f0001u, 0xb4e40000u,
                                        0x49u, 0xb00f0002u, 0xb4e40000u, 0x4cu, 0xb00f0003u, 0xb4e40000u,
                                        0xa0e40000u, 0x01u, 0x800f0000u, 0xb0e40003u, 0x0000ffffu};
    const std::vector<uint32_t> two = {0xffff0101u, 0x42u, 0xb00f0000u, 0x47u, 0xb00f0001u, 0xb0e40000u,
                                       0x48u, 0xb00f0002u, 0xb0e40000u, 0x01u, 0x800f0000u, 0xb0e40002u,
                                       0x0000ffffu};
    for (const auto *tokens : {&spec, &two}) {
        std::vector<uint8_t> bytes(tokens->size() * 4);
        memcpy(bytes.data(), tokens->data(), bytes.size());
        const d9sh::Program &p = d9sh::program_for(bytes);
        CHECK(p.ok && p.pixel && p.major == 1);
        d9msl::PixelVariant v;
        v.cube_mask = tokens == &spec ? 8 : 0;
        std::string out, why;
        CHECK(d9msl::pixel_source(p, v, &out, &why) && out.find(tokens == &spec ? "d9m.z" : "d9m.y") != std::string::npos);
        // The stage the last row samples must be bound: t0 and t3 (spec), t0 and t2 (two).
        CHECK(d9msl::pixel_sampler_mask(p) == (tokens == &spec ? 9u : 5u));
        CHECK(d9glsl::pixel_source(p, v, &out, &why));
        CHECK(d9wgsl::pixel_source(p, v, &out, &why));
        if (!why.empty())
            fprintf(stderr, "texture matrix: %s\n", why.c_str());
    }
}

static uint32_t target_pixel(uint32_t surface, uint32_t x, uint32_t y) {
    D9SurfaceInfo info;
    uint32_t v = 0;
    if (d9_surface_info(com_this(surface)->id, &info) && info.data)
        memcpy(&v, info.data + (size_t)y * info.pitch + x * 4, 4);
    return v;
}
static bool argb_near(uint32_t got, uint32_t want) {
    for (int k = 0; k < 32; k += 8)
        if (std::abs((int)((got >> k) & 255) - (int)((want >> k) & 255)) > 2) {
            fprintf(stderr, "pixel %08x, expected %08x\n", got, want);
            return false;
        }
    return true;
}
static void clear_target(uint32_t surface) {
    D9SurfaceInfo info;
    if (d9_surface_info(com_this(surface)->id, &info) && info.data)
        memset(info.data, 0, (size_t)info.pitch * info.height);
}
// Through the CPU renderer, which runs the same generated programs as the GPU.
static void raster_draw(uint32_t dev, uint32_t target, uint32_t prim, uint32_t count,
                        uint32_t vertices, uint32_t stride) {
    ComObj *d = com_this(dev);
    ComObj *decl = com_get(d9_pipeline(d->id).declaration);
    CHECK(decl != nullptr);
    if (!decl)
        return;
    D9DrawCall call;
    call.prim = prim;
    call.prim_count = count;
    call.vertices = vertices;
    call.stride = stride;
    d9_raster_draw(d, com_this(target), decl->blob, call);
}

static void test_ffp_device(uint32_t dev) {
    constexpr uint32_t invalid = 0x8876086cu;
    CHECK(call_method(dev, 28, {8, 8, 21, 0, 0, 0, sc(0x2000), 0}) == 0); // CreateRenderTarget
    const uint32_t rt = rd32(sc(0x2000));
    CHECK(rt != 0);
    for (uint32_t i = 0; i < 4; ++i)
        wr32(sc(0x2010) + 4 * i, i < 2 ? 0 : 8);
    putf(sc(0x2020), {0.0f, 1.0f});
    CHECK(call_method(dev, 47, {sc(0x2010)}) == 0); // SetViewport
    for (auto rs : {std::array<uint32_t, 2>{27, 0}, {22, 1}, {7, 0}, {137, 0}, {29, 0}, {28, 0}})
        CHECK(call_method(dev, 57, {rs[0], rs[1]}) == 0);
    CHECK(call_method(dev, 92, {0}) == 0 && call_method(dev, 107, {0}) == 0);

    // SetFVF binds a declaration made from the code; GetFVF and GetVertexDeclaration see it.
    CHECK(call_method(dev, 89, {0x144}) == 0);
    CHECK(call_method(dev, 90, {sc(0x2030)}) == 0 && rd32(sc(0x2030)) == 0x144);
    CHECK(call_method(dev, 88, {sc(0x2034)}) == 0 && rd32(sc(0x2034)) != 0);
    if (const uint32_t d = rd32(sc(0x2034))) {
        CHECK(com_this(d)->blob.size() == 32);
        call_method(d, 2);
    }
    CHECK(call_method(dev, 89, {0x040}) == invalid); // no position

    // 1. Pretransformed vertices, diffuse x texture factor: a full-target strip.
    CHECK(call_method(dev, 89, {0x044}) == 0); // XYZRHW | DIFFUSE
    const uint32_t quad = sc(0x2100);
    const float corner[4][2] = {{0, 0}, {8, 0}, {0, 8}, {8, 8}};
    for (int i = 0; i < 4; ++i) {
        putf(quad + 20 * i, {corner[i][0], corner[i][1], 0.5f, 1.0f});
        wr32(quad + 20 * i + 16, 0xff804020u);
    }
    for (auto ts : {std::array<uint32_t, 3>{0, 1, 4}, {0, 2, 0}, {0, 3, 3}, {0, 4, 2}, {0, 5, 0},
                    {1, 1, 1}, {1, 4, 1}})
        CHECK(call_method(dev, 67, {ts[0], ts[1], ts[2]}) == 0);
    CHECK(call_method(dev, 57, {60, 0xff808080u}) == 0); // TEXTUREFACTOR
    CHECK(call_method(dev, 66, {0, 3, sc(0x2038)}) == 0 && rd32(sc(0x2038)) == 3);
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, quad, 20);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xff402010u));
    CHECK(argb_near(target_pixel(rt, 0, 7), 0xff402010u));
    // The GPU path gets the same programs, decodable, with their constants.
    const unsigned before = draws;
    CHECK(call_method(dev, 83, {5, 2, quad, 20}) == 0);
    CHECK(draws == before + 1 && captured.vs && captured.ps);
    if (captured.vs && captured.ps) {
        const d9sh::Program &vs = d9sh::program_for(
            std::vector<uint8_t>(captured.vs, captured.vs + captured.vs_size));
        CHECK(vs.ok && !vs.pixel && captured.vs_key == d9sh::code_key(captured.vs, captured.vs_size));
        CHECK(vs.ok && !vs.inputs.empty() && vs.inputs[0].usage == 9); // POSITIONT
        CHECK(captured.vconst_count == d9ffp::VS_CONSTANTS && captured.pconst_count == d9ffp::PS_CONSTANTS);
        CHECK(captured.decl_size == 24);
        // TFACTOR reaches the pixel constants as colour.
        CHECK(captured.pconst && std::fabs(captured.pconst[0] - 128.0f / 255.0f) < 1e-6f);
    }

    // 2. Lighting: one directional light straight at a quad facing the eye, plus ambient.
    CHECK(call_method(dev, 89, {0x012}) == 0); // XYZ | NORMAL
    const float clip[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};
    const uint32_t lit = sc(0x2180);
    for (int i = 0; i < 4; ++i)
        putf(lit + 24 * i, {clip[i][0], clip[i][1], 0.5f, 0.0f, 0.0f, -1.0f});
    putf(sc(0x2040), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    for (uint32_t state : {256u, 2u, 3u})
        CHECK(call_method(dev, 44, {state, sc(0x2040)}) == 0);
    putf(sc(0x2200), {0.5f, 0.25f, 0.125f, 1.0f, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0});
    CHECK(call_method(dev, 49, {sc(0x2200)}) == 0); // SetMaterial
    CHECK(call_method(dev, 50, {sc(0x2280)}) == 0 && memcmp(gm_ptr(sc(0x2200)), gm_ptr(sc(0x2280)), 68) == 0);
    memset(gm_ptr(sc(0x2300)), 0, 104);
    wr32(sc(0x2300), 3); // directional
    putf(sc(0x2304), {1, 1, 1, 1});
    putf(sc(0x2300) + 64, {0, 0, 1});
    CHECK(call_method(dev, 52, {7, sc(0x2380)}) == invalid); // GetLight of one never set
    CHECK(call_method(dev, 51, {7, sc(0x2300)}) == 0);
    CHECK(call_method(dev, 52, {7, sc(0x2380)}) == 0 && memcmp(gm_ptr(sc(0x2300)), gm_ptr(sc(0x2380)), 104) == 0);
    CHECK(call_method(dev, 53, {7, 1}) == 0);
    CHECK(call_method(dev, 54, {7, sc(0x2038)}) == 0 && rd32(sc(0x2038)) != 0);
    CHECK(call_method(dev, 57, {137, 1}) == 0);           // LIGHTING
    CHECK(call_method(dev, 57, {139, 0xff404040u}) == 0); // AMBIENT
    CHECK(call_method(dev, 67, {0, 1, 2}) == 0);          // COLOROP selectarg1
    CHECK(call_method(dev, 67, {0, 2, 0}) == 0);          // of the lit diffuse
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, lit, 24);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xffbf8060u)); // ambient 0.25 + diffuse (0.5, 0.25, 0.125)
    CHECK(call_method(dev, 53, {7, 0}) == 0);
    CHECK(call_method(dev, 54, {7, sc(0x2038)}) == 0 && rd32(sc(0x2038)) == 0);
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, lit, 24);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xff404040u)); // ambient alone
    // Enabling a light never set turns on Direct3D's default: white, along +z.
    CHECK(call_method(dev, 53, {3, 1}) == 0);
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, lit, 24);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xffbf8060u));
    CHECK(call_method(dev, 53, {3, 0}) == 0);
    CHECK(call_method(dev, 57, {137, 0}) == 0);

    // 3. A texture through the cascade, then modulated by the complement of the texture factor.
    CHECK(call_method(dev, 23, {2, 2, 1, 0, 21, 1, sc(0x2400), 0}) == 0);
    const uint32_t tex = rd32(sc(0x2400));
    CHECK(tex && call_method(tex, 19, {0, sc(0x2404), 0, 0}) == 0);
    for (uint32_t y = 0; y < 2; ++y)
        for (uint32_t x = 0; x < 2; ++x)
            wr32(rd32(sc(0x2408)) + y * rd32(sc(0x2404)) + 4 * x, 0xff00ff00u);
    CHECK(call_method(tex, 20, {0}) == 0);
    // LockRect with a rectangle points at its top-left texel; one outside the level is refused.
    const uint32_t whole = rd32(sc(0x2408));
    for (uint32_t i = 0; i < 4; ++i)
        wr32(sc(0x2410) + 4 * i, i < 2 ? 1 : 2);
    CHECK(call_method(tex, 19, {0, sc(0x2404), sc(0x2410), 0}) == 0);
    CHECK(rd32(sc(0x2408)) == whole + rd32(sc(0x2404)) + 4);
    CHECK(call_method(tex, 20, {0}) == 0);
    wr32(sc(0x2418), 3);
    CHECK(call_method(tex, 19, {0, sc(0x2404), sc(0x2410), 0}) == invalid);
    CHECK(call_method(tex, 21, {0}) == 0); // AddDirtyRect
    // GetLevelDesc reports the creating call's type, usage and pool: D3DPOOL_MANAGED here.
    CHECK(call_method(tex, 17, {0, sc(0x2420)}) == 0);
    CHECK(rd32(sc(0x2420)) == 21 && rd32(sc(0x2424)) == 1 && rd32(sc(0x2428)) == 0 &&
          rd32(sc(0x242c)) == 1 && rd32(sc(0x2438)) == 2 && rd32(sc(0x243c)) == 2);
    CHECK(call_method(dev, 65, {0, tex}) == 0);
    CHECK(call_method(dev, 89, {0x104}) == 0); // XYZRHW | TEX1
    const uint32_t tq = sc(0x2480);
    for (int i = 0; i < 4; ++i)
        putf(tq + 24 * i, {corner[i][0], corner[i][1], 0.5f, 1.0f, corner[i][0] / 8, corner[i][1] / 8});
    for (auto ts : {std::array<uint32_t, 3>{0, 1, 2}, {0, 2, 2}, {0, 4, 2}, {0, 5, 2}})
        CHECK(call_method(dev, 67, {ts[0], ts[1], ts[2]}) == 0);
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, tq, 24);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xff00ff00u));
    CHECK(call_method(dev, 67, {0, 1, 4}) == 0);    // MODULATE
    CHECK(call_method(dev, 67, {0, 3, 0x13}) == 0); // by 1 - TFACTOR
    clear_target(rt);
    raster_draw(dev, rt, 5, 2, tq, 24);
    CHECK(argb_near(target_pixel(rt, 4, 4), 0xff007f00u));
    CHECK(call_method(dev, 65, {0, 0}) == 0);
    call_method(tex, 2);

    // 4. Shaders from bytecode: a vs_2_0 that passes its position through.
    const uint32_t vs_tokens[] = {0xfffe0200u, 0x0200001fu, 0x80000000u, 0x900f0000u,
                                  0x02000001u, 0xc00f0000u, 0x90e40000u, 0x0000ffffu};
    const uint32_t code = sc(0x2500);
    for (uint32_t i = 0; i < 8; ++i)
        wr32(code + 4 * i, vs_tokens[i]);
    CHECK(call_method(dev, 106, {code, sc(0x2540)}) == invalid); // not a pixel shader
    CHECK(call_method(dev, 91, {0, sc(0x2540)}) == invalid);
    CHECK(call_method(dev, 91, {code, sc(0x2540)}) == 0);
    const uint32_t vs = rd32(sc(0x2540));
    CHECK(vs != 0);
    CHECK(call_method(vs, 4, {0, sc(0x2544)}) == 0 && rd32(sc(0x2544)) == 32); // GetFunction size
    wr32(sc(0x2544), 31);
    CHECK(call_method(vs, 4, {sc(0x2560), sc(0x2544)}) == invalid);
    wr32(sc(0x2544), 32);
    CHECK(call_method(vs, 4, {sc(0x2560), sc(0x2544)}) == 0 && memcmp(gm_ptr(sc(0x2560)), gm_ptr(code), 32) == 0);
    CHECK(call_method(vs, 3, {sc(0x2548)}) == 0 && com_this(rd32(sc(0x2548))) == com_this(dev));
    call_method(rd32(sc(0x2548)), 2);
    CHECK(call_method(dev, 92, {vs}) == 0);
    CHECK(call_method(dev, 93, {sc(0x254c)}) == 0 && com_this(rd32(sc(0x254c))) == com_this(vs));
    call_method(rd32(sc(0x254c)), 2);
    const uint32_t vs_id = com_this(vs)->id;
    CHECK(call_method(vs, 2) == 1); // the device still holds it
    CHECK(call_method(dev, 89, {0x002}) == 0);
    CHECK(call_method(dev, 83, {4, 1, quad, 12}) == 0);
    CHECK(captured.vs_size == 32 && captured.vs && memcmp(captured.vs, gm_ptr(code), 32) == 0);
    CHECK(captured.vconst == &d9_pipeline(com_this(dev)->id).vconst[0][0]);
    CHECK(call_method(dev, 92, {0}) == 0);
    CHECK(!com_get(vs_id)); // the last reference went with the binding
    CHECK(call_method(dev, 93, {sc(0x254c)}) == 0 && rd32(sc(0x254c)) == 0);
    CHECK(call_method(dev, 83, {4, 1, quad, 12}) == 0);
    CHECK(captured.vs_size != 32 && captured.vconst_count == d9ffp::VS_CONSTANTS);

    // Transforms read back, and MultiplyTransform post-multiplies.
    putf(sc(0x2040), {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 1, 2, 3, 1});
    CHECK(call_method(dev, 44, {256, sc(0x2040)}) == 0);
    putf(sc(0x2080), {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1});
    CHECK(call_method(dev, 46, {256, sc(0x2080)}) == 0);
    CHECK(call_method(dev, 45, {256, sc(0x20c0)}) == 0);
    CHECK(rd32(sc(0x20c0)) == fbits(2.0f) && rd32(sc(0x20c0) + 48) == fbits(11.0f) &&
          rd32(sc(0x20c0) + 56) == fbits(33.0f));
    CHECK(call_method(dev, 45, {300, sc(0x20c0)}) == invalid);
    // Integer and float registers read back; a range past the end is refused whole.
    for (uint32_t i = 0; i < 8; ++i)
        wr32(sc(0x2600) + 4 * i, i + 1);
    CHECK(call_method(dev, 96, {14, sc(0x2600), 2}) == 0);
    CHECK(call_method(dev, 97, {14, sc(0x2620), 2}) == 0 && memcmp(gm_ptr(sc(0x2600)), gm_ptr(sc(0x2620)), 32) == 0);
    CHECK(call_method(dev, 111, {15, sc(0x2600), 2}) == invalid);
    putf(sc(0x2600), {1.5f, 2.5f, 3.5f, 4.5f});
    CHECK(call_method(dev, 109, {200, sc(0x2600), 1}) == 0); // ps_3_0 has 224 float registers
    CHECK(call_method(dev, 110, {200, sc(0x2620), 1}) == 0 && rd32(sc(0x2620) + 12) == fbits(4.5f));
    CHECK(call_method(dev, 110, {224, sc(0x2620), 1}) == invalid);
    call_method(rt, 2);
}

int main() {
    test_volume_dds();
    mem_init();
    imports_init();
    dx_register_shims();
    g_stack_top = STACK_TOP - 0x1000;
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch)
        return 1;
    cpu_reset();
    const uint32_t dev = make_device();
    if (!dev)
        return 1;
    test_device_booleans(dev);
    test_draw_limits(dev);
    test_depth_stencil_get(dev);
    test_surface_container_refs(dev);
    test_effect_booleans(dev);
    test_volume(dev);
    test_state_getters_and_readback(dev);
    test_ffp_programs();
    test_ps1_texture_matrix();
    test_ffp_device(dev);
    const uint32_t bound = make_volume(dev);
    const uint32_t bound_id = com_this(bound)->id;
    const uint32_t child_id = com_this(bound)->surfaces[0];
    CHECK(call_method(dev, 65, {0, bound}) == 0);
    CHECK(call_method(bound, 2) == 1);
    call_method(dev, 2);
    CHECK(!com_get(bound_id) && !com_get(child_id));
    if (const uint32_t refs_dev = make_device()) // Test157c, on a device of its own (it Resets)
        test_depth_stencil_refs(refs_dev);
    printf("d3d9 guest/effect: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
