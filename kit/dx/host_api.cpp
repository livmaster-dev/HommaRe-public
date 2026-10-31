// host_api.cpp - weak no-op defaults for every host callback.
//
// Task 7 links strong definitions over these. Building with
// -DRECOMP_NULL_HOST makes them strong, which pins a headless run to doing
// nothing graphical or audible even if a host object file is on the link line.
#include "host_api.h"
#include <string.h>
#include "../platform/os.h"

#ifdef RECOMP_NULL_HOST
#define HOST_DEFAULT
#else
#define HOST_DEFAULT __attribute__((weak))
#endif

extern "C" {

HOST_DEFAULT void host_set_gamma_ramp(const uint16_t *) {}

// Shutdown remains reachable even with RECOMP_NULL_HOST: only graphical and
// audible callbacks are pinned to no-ops in that build.
__attribute__((weak)) int host_close_requested(void) {
    return 0;
}

// The access counters are the DirectDraw shim's, and a binary that links this
// file without that shim still has to be able to print the stats line: the
// host unit tests are exactly that binary. Zeros are the truth for a run with
// no shim in it, and the real definition in ddraw.cpp overrides this one
// wherever the shim IS linked. Task 1's stub lived in display_stubs.cpp until
// mod-t1's T2 replaced it with the real thing, at which point every binary
// that did not link ddraw.cpp lost the symbol.
// ALWAYS weak, and deliberately not HOST_DEFAULT.
//
// The defaults around this one are host callbacks, and RECOMP_NULL_HOST makes
// them strong on purpose: that pins a headless run to doing nothing graphical
// even if a host object file is on the link line. This is not a host callback.
// It reads counters that live in the DirectDraw shim, the null host links that
// shim, and it wants the shim's real numbers. Built with HOST_DEFAULT it was a
// strong definition beside ddraw.cpp's strong definition, and the parity link
// failed on a duplicate symbol - which is how the gates found it.
__attribute__((weak)) void host_access_counts(HostAccessCounts *out) {
    if (out)
        memset(out, 0, sizeof *out);
}

// Frames the host has presented (host/present.h). A game's native code may
// pace itself by it (HoMM5's animation LOD), and the binaries that link the
// generated library without a host - pop_fixture, profile_tests - need it
// defined: none presented. Always weak for the same reason as above: the
// headless host defines it as well as the real one.
__attribute__((weak)) uint32_t host_present_count(void) {
    return 0;
}

HOST_DEFAULT void host_present(const void *, int, int, int, const uint32_t *, int) {}
HOST_DEFAULT void host_set_display_mode(int, int, int) {}
HOST_DEFAULT void host_set_render_resolution(int, int) {}

HOST_DEFAULT void host_d3d_begin_scene() {}
HOST_DEFAULT void host_d3d_end_scene() {}
HOST_DEFAULT void host_d3d_draw(const HostD3DDrawSnapshot *) {}
HOST_DEFAULT void host_d3d_clear(uint32_t, const int32_t *, uint32_t, uint32_t, float) {}
HOST_DEFAULT void host_d3d_set_render_target(const HostD3DSurface *) {}
HOST_DEFAULT void host_d3d_flush_surface(const HostD3DSurface *, const char *) {}
HOST_DEFAULT void host_d3d_discard(void) {}
HOST_DEFAULT void host_d3d_texture(const HostD3DTexture *) {}
HOST_DEFAULT void host_d3d_texture_destroyed(uint32_t) {}
HOST_DEFAULT int host_d3d_texture_retain(uint32_t, uint32_t) {
    return 1;
}
HOST_DEFAULT void host_d3d_texture_release(uint32_t, uint32_t) {}

HOST_DEFAULT void host_audio_play(const HostAudioPlay *) {}
HOST_DEFAULT int32_t host_audio_stream(int32_t) {
    return -1;
}
HOST_DEFAULT uint32_t host_audio_played_bytes(int32_t) {
    return 0;
}
HOST_DEFAULT int32_t host_audio_queue(int32_t, const void *, uint32_t) {
    return 0;
}
HOST_DEFAULT int32_t host_audio_write(int32_t, const void *, uint32_t, uint32_t) {
    return 0;
}
HOST_DEFAULT uint32_t host_audio_queued_bytes(int32_t) {
    return 0;
}
HOST_DEFAULT uint32_t host_audio_voice_remaining_bytes(int32_t) {
    return 0;
}
HOST_DEFAULT void host_audio_stop(int32_t) {}
HOST_DEFAULT void host_audio_set_volume(int32_t, int32_t) {}
HOST_DEFAULT void host_audio_set_pan(int32_t, int32_t) {}
HOST_DEFAULT void host_audio_set_frequency(int32_t, uint32_t) {}
HOST_DEFAULT uint32_t host_audio_position(int32_t) {
    return 0;
}
HOST_DEFAULT int32_t host_audio_is_playing(int32_t) {
    return 0;
}

// No host means no input: every key up, the pointer at the origin. The guest
// then simply sees nothing happening, which is what a headless run wants.
HOST_DEFAULT void host_input_pointer_correction(int32_t *, int32_t *) {}

HOST_DEFAULT void host_input_state(HostInputState *out) {
    if (out)
        memset(out, 0, sizeof(*out));
}

// No host means no pad either: off, no packet, nothing queued, no rumble
// sink. The axis/button order strings still answer with the spec order, since
// dx/dinput_joystick.cpp's ABI test reads them even with host_pad_mode() 0.
HOST_DEFAULT int host_pad_mode(void) {
    return 0;
}
HOST_DEFAULT int host_pad_native_apis(void) {
    return 0;
}
HOST_DEFAULT uint32_t host_pad_state(HostPadState *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    return 0;
}
HOST_DEFAULT int host_pad_next_event(uint32_t, HostPadEvent *) {
    return 0;
}
HOST_DEFAULT void host_pad_rumble(uint16_t, uint16_t) {}
HOST_DEFAULT int host_pad_rumble_pulse(uint32_t, uint16_t, uint16_t, uint32_t) {
    return 0;
}
HOST_DEFAULT int host_audio_set_output(uint32_t, uint32_t) {
    return 0;
}
HOST_DEFAULT void host_audio_get_output(uint32_t *mode, uint32_t *flags) {
    if (mode)
        *mode = 0;
    if (flags)
        *flags = 0;
}
HOST_DEFAULT const char *host_pad_native_axes(void) {
    return "x,y,z,rz,rx,ry";
}
HOST_DEFAULT const char *host_pad_native_buttons(void) {
    return "square,cross,circle,triangle,l1,r1,l2,r2,select,start,l3,r3,ps";
}

} // extern "C"

// Rectangles are disjoint. Subtracting a read leaves dirty islands intact,
// unlike a bounding box which would read a clean hole a second time.
#include "coherence.h"
#include <algorithm>
#include <map>
#include <vector>
#include <stdlib.h>
namespace {
using DirtyKey = std::pair<uint32_t, uint32_t>;
using DirtyRects = std::vector<HostDirtyRect>;
std::map<DirtyKey, DirtyRects> dirty_regions;
uint64_t readbacks[HOST_READ_REASON_COUNT] = {};
uint64_t clean_reads = 0;
bool nonempty(HostDirtyRect r) {
    return r.x0 < r.x1 && r.y0 < r.y1;
}
HostDirtyRect intersect(HostDirtyRect a, HostDirtyRect b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}
DirtyRects subtract(const DirtyRects &from, HostDirtyRect cut) {
    DirtyRects out;
    for (auto r : from) {
        auto i = intersect(r, cut);
        if (!nonempty(i)) {
            out.push_back(r);
            continue;
        }
        for (auto q :
             {HostDirtyRect{r.x0, r.y0, r.x1, i.y0}, HostDirtyRect{r.x0, i.y1, r.x1, r.y1},
              HostDirtyRect{r.x0, i.y0, i.x0, i.y1}, HostDirtyRect{i.x1, i.y0, r.x1, i.y1}})
            if (nonempty(q))
                out.push_back(q);
    }
    return out;
}
} // namespace
extern "C" {
int host_d3d_legacy_writeback(void) {
    const char *v = recomp_env("LEGACY_WRITEBACK");
    return v && !strcmp(v, "1");
}
void host_d3d_mark_dirty(uint32_t s, uint32_t g, HostDirtyRect r) {
    if (!s || !nonempty(r))
        return;
    auto &regions = dirty_regions[{s, g}];
    // A draw inside an already-dirty island adds no pixels to the union.
    // Retain that island instead of splitting it around every overlapping
    // draw: otherwise a full clear fragments into hundreds of GPU readbacks.
    for (auto q : regions)
        if (q.x0 <= r.x0 && q.y0 <= r.y0 && q.x1 >= r.x1 && q.y1 >= r.y1)
            return;
    regions = subtract(regions, r);
    regions.push_back(r);
}
int host_d3d_dirty_rect(uint32_t s, uint32_t g, HostDirtyRect *out) {
    auto it = dirty_regions.find({s, g});
    if (out)
        *out = {};
    if (it == dirty_regions.end() || it->second.empty())
        return 0;
    auto r = it->second.front();
    for (auto q : it->second) {
        r.x0 = std::min(r.x0, q.x0);
        r.y0 = std::min(r.y0, q.y0);
        r.x1 = std::max(r.x1, q.x1);
        r.y1 = std::max(r.y1, q.y1);
    }
    if (out)
        *out = r;
    return 1;
}
void host_d3d_clean_pixels(uint32_t s, uint32_t g, HostDirtyRect r) {
    auto it = dirty_regions.find({s, g});
    if (it == dirty_regions.end())
        return;
    it->second = subtract(it->second, r);
    if (it->second.empty())
        dirty_regions.erase(it);
}
int host_d3d_make_coherent(const HostD3DSurface *s, uint32_t g, const HostDirtyRect *rect,
                           HostReadReason why) {
    if (!s || !s->pixels || why < 0 || why >= HOST_READ_REASON_COUNT)
        return -1;
    HostDirtyRect bounds{0, 0, s->width, s->height};
    auto request = rect ? intersect(*rect, bounds) : bounds;
    DirtyRects read;
    auto it = dirty_regions.find({s->id, g});
    if (it != dirty_regions.end())
        for (auto r : it->second) {
            auto q = intersect(r, request);
            if (nonempty(q))
                read.push_back(q);
        }
    if (read.empty()) {
        ++clean_reads;
        return 0;
    }
    if (!host_d3d_readback_rects(s, g, read.data(), (uint32_t)read.size()))
        return -1;
    host_d3d_clean_pixels(s->id, g, request);
    ++readbacks[why];
    host_d3d_note_readback(why);
    return 1;
}
void host_d3d_transfer_dirty(uint32_t a, uint32_t ag, uint32_t b, uint32_t bg) {
    DirtyRects ar, br;
    auto ai = dirty_regions.find({a, ag}), bi = dirty_regions.find({b, bg});
    if (ai != dirty_regions.end())
        ar = std::move(ai->second);
    if (bi != dirty_regions.end())
        br = std::move(bi->second);
    dirty_regions.erase({a, ag});
    dirty_regions.erase({b, bg});
    if (!br.empty())
        dirty_regions[{a, ag + 1}] = std::move(br);
    if (!ar.empty())
        dirty_regions[{b, bg + 1}] = std::move(ar);
    host_d3d_swap_generations(a, ag, b, bg);
}
void host_d3d_forget_generation(uint32_t s, uint32_t g) {
    dirty_regions.erase({s, g});
}
void host_d3d_reset_coherence(void) {
    dirty_regions.clear();
    memset(readbacks, 0, sizeof readbacks);
    clean_reads = 0;
    host_d3d_reset_readback_metrics();
}
uint64_t host_readback_reason_count(HostReadReason why) {
    return why >= 0 && why < HOST_READ_REASON_COUNT ? readbacks[why] : 0;
}
uint64_t host_readback_count_for_test(void) {
    uint64_t n = 0;
    for (auto r : readbacks)
        n += r;
    return n;
}
uint64_t host_d3d_clean_read_count(void) {
    return clean_reads;
}
// Null host has no GPU writes. Its read callback intentionally leaves the
// guest bytes unchanged, as did the pre-coherence null host.
HOST_DEFAULT void host_d3d_reset_readback_metrics(void) {}
HOST_DEFAULT int host_d3d_readback_rects(const HostD3DSurface *, uint32_t, const HostDirtyRect *,
                                         uint32_t) {
    return 1;
}
HOST_DEFAULT void host_d3d_bind_generation(const HostD3DSurface *s, uint32_t, uint64_t) {
    host_d3d_set_render_target(s);
}
HOST_DEFAULT void host_d3d_swap_generations(uint32_t, uint32_t, uint32_t, uint32_t) {}
HOST_DEFAULT void host_d3d_apply_cpu(const HostD3DSurface *, const HostBlitRecord *) {}
HOST_DEFAULT void host_d3d_seal_frame(uint64_t) {}
HOST_DEFAULT void host_d3d_retire_frame(uint64_t) {}
}
#include <atomic>
namespace {
std::atomic<int> coherence_appkit_pending{0};
}
extern "C" void host_d3d_set_appkit_pending(int p) {
    coherence_appkit_pending.store(p);
}
extern "C" int host_d3d_appkit_pending(void) {
    return coherence_appkit_pending.load();
}
extern "C" HOST_DEFAULT int host_d3d_claim_sealed_frame(uint64_t) {
    return 0;
}

extern "C" HOST_DEFAULT void host_d3d_note_readback(HostReadReason) {}

extern "C" __attribute__((weak)) void host_d3d_release_unclaimed_frame(uint64_t) {}

extern "C" HOST_DEFAULT void host_d3d_prepare_cpu_write(const HostD3DSurface *, uint32_t,
                                                        uint64_t) {}

// Recorder queries stay weak even in a null-host build.
#include "passes.h"
#include "../platform/os.h"
extern "C" {
__attribute__((weak)) int host_frame_legacy(HostFrameHandle) {
    return 0;
}
__attribute__((weak)) uint64_t host_legacy_fallback_count(void) {
    return 0;
}
__attribute__((weak)) HostDrawMapping host_frame_draw_mapping(HostFrameHandle, uint32_t) {
    return HOST_MAPPING_SCENE;
}
const char *host_overlay_mapping_for_test(HostFrameHandle f, uint32_t seq) {
    return host_frame_draw_mapping(f, seq) == HOST_MAPPING_UI ? "ui" : "scene";
}
HOST_DEFAULT void host_d3d_replay_barrier(const HostD3DSurface *, uint32_t, uint32_t) {}
HOST_DEFAULT HostDrawMapping host_render_draw_mapping_for_test(HostFrameHandle, uint32_t) {
    return HOST_MAPPING_SCENE;
}
HOST_DEFAULT int host_render_legacy_frame_for_test(HostFrameHandle) {
    return 0;
}
}

extern "C" HOST_DEFAULT int host_d3d_accepts_draw(void) {
    return 1;
}
