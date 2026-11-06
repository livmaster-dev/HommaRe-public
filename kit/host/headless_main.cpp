// headless_main.cpp - boots the recompiled game from its real PE entry point
// with no window, no device and no audio, and writes every presented frame to
// a file.
//
// This is the real path, not the parity fixture: it runs `entry` (0055d6c0),
// which runs the CRT startup, WinMain, the graphics initialisation and the
// game's own main loop. Everything the guest asks the operating system for is
// answered by runtime; everything it asks DirectDraw/Direct3D for
// is answered by dx.
//
// The boot sequence itself is not here. host/boot.cpp owns
// everything the windowed host in main.mm also needs - the load, the
// heartbeat on the guest's clock, the synthesised activation, the close, the
// unwind, the watchdog and the fault handler - and this file is what is left
// once that is taken out: the caps, a presenter that writes files, and the
// run report.
//
//   1. A presenter. `host_present` is a weak no-op by default; the strong
//      definition here converts the presented surface to RGB and writes
//      build/recomp/frames/frame_NNNN.ppm. Nothing is ever displayed.
//   2. A close. Reaching the frame or wall-clock cap posts WM_CLOSE, which is
//      what closing the window does, and lets the game shut itself down
//      through its own exit path.
//   3. The run report, printed on every path a run can end on.
//
// Environment:
//   RECOMP_MAX_FRAMES=N     stop after N presented frames (default 200)
//   RECOMP_MAX_SECONDS=S    stop after S wall-clock seconds (default 180)
//   RECOMP_FRAMES=DIR       frame directory (default build/recomp/frames)
//   RECOMP_FRAME_EVERY=N    write every Nth frame (default 1; 0 writes none)
// Static GDI windows refresh at the offscreen display's 60 Hz rate. Each
// refresh is a present and advances RECOMP_PIN_CLOCK just like DirectDraw.
//   RECOMP_EXE=PATH         image to load (default the loader's)
//   RECOMP_NO_ACTIVATE=1    do not synthesise activation (diagnostics)
//   RECOMP_LOG, RECOMP_IMPORT_STATS  as documented in runtime/README.md
#include "audio.h"
#include "audio_capture.h"
#include "../platform/os.h"
#include "boot.h"
#include "page_overlay.h"
#include "../runtime/guest.h"
#include "../runtime/loader.h"
#include "../runtime/win32.h"
#include "../runtime/gdi32_internal.h"
#include "../dx/host_api.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

namespace {

// --- configuration ---------------------------------------------------------
// 500, not 200. A run has three phases: about 10 blank startup flips, then
// about 190 frames of the intro logo video at 16 bpp, then a short blank gap,
// and only from about frame 215 does the front end appear. A 200-frame cap
// stopped inside that gap, so the default run reported "a frame with content
// was presented" on the strength of the video and never reached the menu at
// all. 500 lands well inside the settled menu and costs about 15 seconds.
uint32_t g_max_frames = 500;
double g_max_seconds = 180.0;
// Every tenth frame, not every frame. A default run presents about 500, and
// at 900 KB a frame writing all of them costs nearly half a gigabyte to say
// something a handful of frames already say. Ten still lands several frames in
// each phase, and RECOMP_FRAME_EVERY=1 is there for a run that needs them
// all.
uint32_t g_frame_every = 10;
std::string g_frames_dir = "build/recomp/frames";
bool g_activate = true;

// --- run state -------------------------------------------------------------
uint32_t g_present_count = 0; // host_present calls
uint32_t g_frames_written = 0;

// Presentation and renderer bookkeeping, for the run report.
char g_best_path[512] = {0};
char g_last_path[512] = {0};
uint32_t g_last_distinct = 0, g_last_nonbg = 0, g_last_src = 0, g_last_pal = 0;
uint32_t g_best_distinct = 0, g_best_nonbg = 0, g_best_src = 0, g_best_pal = 0;
// What the run saw, whether or not any frame was written. Writing frames is a
// convenience; whether the guest drew anything is a fact about the run, and
// RECOMP_FRAME_EVERY=0 must not turn a good run into a failed one.
uint32_t g_seen_max_distinct = 0, g_seen_max_nonbg = 0;
int g_mode_w = 0, g_mode_h = 0, g_mode_bpp = 0;
uint32_t g_mode_sets = 0;
uint32_t g_d3d_scenes = 0, g_d3d_draws = 0, g_d3d_clears = 0, g_d3d_textures = 0;
uint32_t g_present8 = 0, g_present16 = 0, g_present32 = 0;

void mkdir_p(const std::string &path) {
    std::string acc;
    size_t i = 0;
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                os_mkdir(acc.c_str());
        }
        if (i < path.size())
            acc.push_back(path[i]);
        ++i;
    }
}

// ---------------------------------------------------------------------------
// Frame files. A presented surface is 8-bit palettised or 5-6-5; both become
// binary PPM (P6), which needs no library to read and no library to write.
// ---------------------------------------------------------------------------
struct FrameStats {
    uint32_t index = 0;
    int w = 0, h = 0, bpp = 0;
    uint32_t distinct = 0;     // distinct 24-bit colours
    uint32_t distinct_src = 0; // distinct palette indices in the surface
    uint32_t distinct_pal = 0; // distinct colours in the attached palette
    uint32_t nonbg = 0;        // pixels differing from the top-left pixel
    uint64_t sum = 0;          // sum of luma*, for a coarse "is it dark" figure
    std::string path;
};
std::vector<FrameStats> g_frames;

// The run bookkeeping above is written here on the guest thread and read by
// the watchdog on its own, so every update is taken under the report lock.
// The lock is held for the bookkeeping only, never for the pixel work or the
// file write: a watchdog that had to wait for a PPM to reach the disk before
// it could report would be waiting on the very thing it exists to escape.
struct ReportLock {
    ReportLock() {
        boot_report_lock();
    }
    ~ReportLock() {
        boot_report_unlock();
    }
};

bool write_ppm(const std::string &path, const std::vector<uint8_t> &rgb, int w, int h) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f)
        return false;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    bool ok = fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    fclose(f);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Host callbacks. These are the strong definitions that displace the weak
// no-ops in dx/host_api.cpp. Nothing here opens a window, a device,
// an audio stream or an input device: every callback either writes a file or
// counts.
// ---------------------------------------------------------------------------
extern "C" void host_set_display_mode(int w, int h, int bpp) {
    {
        ReportLock held;
        g_mode_w = w;
        g_mode_h = h;
        g_mode_bpp = bpp;
        ++g_mode_sets;
    }
    printf("[host] display mode %dx%d %dbpp\n", w, h, bpp);
    fflush(stdout);
}

// Consume a guest present for headless capture and advance a pinned test clock.
// Draw diagnostics on owned copies so recording never changes guest surface contents.
extern "C" void host_present(const void *pixels, int w, int h, int bpp, const uint32_t *palette,
                             int pitch) {
    if (bpp == 8 || bpp == 16)
        boot_note_primary_present();
    // The frame boundary, and so the one thing that moves a pinned clock. See
    // boot.cpp; on an unpinned run this does nothing.
    boot_clock_advance();
    uint32_t index;
    {
        ReportLock held;
        index = g_present_count++;
        if (bpp == 8)
            ++g_present8;
        else if (bpp == 16)
            ++g_present16;
        else if (bpp == 32)
            ++g_present32;
    }

    if (!pixels || w <= 0 || h <= 0)
        return;

    // A DirectDraw present keeps its base pixels and overlays current window
    // surfaces. GDI-only snapshots arrive already composed through the seam.
    std::vector<uint32_t> composed;
    if (bpp == 8 || bpp == 16) {
        composed.resize(size_t(w) * h);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint8_t *row = static_cast<const uint8_t *>(pixels) + size_t(y) * pitch;
                uint32_t p;
                if (bpp == 8)
                    p = palette ? palette[row[x]] : row[x] * 0x010101u;
                else {
                    uint16_t v = uint16_t(row[x * 2] | row[x * 2 + 1] << 8);
                    p = (((v >> 11) & 31) * 255 / 31) << 16 | (((v >> 5) & 63) * 255 / 63) << 8 |
                        (v & 31) * 255 / 31;
                }
                composed[size_t(y) * w + x] = p | 0xff000000;
            }
        gdi_composite_windows(composed.data(), w, h);
    }
    // The page goes on a copy this host owns, never on the surface the guest
    // is still reading; everything below reads the copy.
    const uint8_t *src = composed.empty()
                             ? (const uint8_t *)host_page_overlay(pixels, w, h, bpp, pitch, palette)
                             : static_cast<const uint8_t *>(pixels);
    const uint32_t *composed_src =
        composed.empty() ? nullptr
                         : static_cast<const uint32_t *>(
                               host_page_overlay(composed.data(), w, h, 32, w * 4, nullptr));
    std::vector<uint8_t> rgb((size_t)w * (size_t)h * 3);
    // A coarse content summary, computed on every frame even when the frame
    // itself is not written, so the run can say which frame first had content.
    uint8_t seen[1 << 12] = {0}; // 4-4-4 buckets: enough to tell flat from not
    uint32_t distinct = 0, nonbg = 0;
    uint32_t first_r = 0, first_g = 0, first_b = 0;
    uint64_t sum = 0;
    // Counted separately from the colours so a frame that was drawn into but
    // has an all-black palette reads as "drawn, palette not applied" rather
    // than as "nothing drawn". They are different bugs.
    uint8_t seen_src[256] = {0};
    uint32_t distinct_src = 0, distinct_pal = 0;
    if (bpp == 8 && palette) {
        uint8_t seen_pal[1 << 12] = {0};
        for (int i = 0; i < 256; ++i) {
            uint32_t c = palette[i];
            uint32_t b = (((c >> 20) & 15) << 8) | (((c >> 12) & 15) << 4) | ((c >> 4) & 15);
            if (!seen_pal[b]) {
                seen_pal[b] = 1;
                ++distinct_pal;
            }
        }
    }

    for (int y = 0; y < h; ++y) {
        const uint8_t *row = src + (size_t)y * (size_t)pitch;
        uint8_t *out = rgb.data() + (size_t)y * (size_t)w * 3;
        for (int x = 0; x < w; ++x) {
            uint32_t r, g, b;
            if (bpp == 8) {
                uint8_t idx = row[x];
                if (!seen_src[idx]) {
                    seen_src[idx] = 1;
                    ++distinct_src;
                }
                uint32_t c = palette ? palette[idx] : (uint32_t)(idx * 0x010101u);
                r = (c >> 16) & 0xff;
                g = (c >> 8) & 0xff;
                b = c & 0xff;
            } else if (bpp == 32) {
                uint32_t p;
                memcpy(&p, row + 4 * x, 4);
                r = (p >> 16) & 255;
                g = (p >> 8) & 255;
                b = p & 255;
            } else {
                uint16_t p = (uint16_t)(row[2 * x] | (row[2 * x + 1] << 8));
                r = ((p >> 11) & 0x1f) * 255 / 31;
                g = ((p >> 5) & 0x3f) * 255 / 63;
                b = (p & 0x1f) * 255 / 31;
            }
            if (!composed.empty()) {
                uint32_t p = composed_src[size_t(y) * w + x];
                r = (p >> 16) & 255;
                g = (p >> 8) & 255;
                b = p & 255;
            }
            out[3 * x + 0] = (uint8_t)r;
            out[3 * x + 1] = (uint8_t)g;
            out[3 * x + 2] = (uint8_t)b;
            if (x == 0 && y == 0) {
                first_r = r;
                first_g = g;
                first_b = b;
            } else if (r != first_r || g != first_g || b != first_b)
                ++nonbg;
            uint32_t bucket = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
            if (!seen[bucket]) {
                seen[bucket] = 1;
                ++distinct;
            }
            sum += r + g + b;
        }
    }

    FrameStats fs;
    fs.index = index;
    fs.w = w;
    fs.h = h;
    fs.bpp = bpp;
    fs.distinct = distinct;
    fs.nonbg = nonbg;
    fs.sum = sum;
    fs.distinct_src = distinct_src;
    fs.distinct_pal = distinct_pal;
    bool write_ok = false;
    if (g_frame_every && (index % g_frame_every) == 0) {
        char name[64];
        snprintf(name, sizeof name, "/frame_%04u.ppm", index);
        fs.path = g_frames_dir + name;
        write_ok = write_ppm(fs.path, rgb, w, h);
        if (!write_ok) {
            fprintf(stderr, "[host] cannot write %s\n", fs.path.c_str());
            fs.path.clear();
        }
    }
    bool first_with_content;
    {
        // Recorded in plain scalars as well as in the vector, so the watchdog
        // can report the run without walking a container the presenting
        // thread owns - and under the lock, so it never reads one mid-update.
        ReportLock held;
        if (write_ok)
            ++g_frames_written;
        // The last frame with something in it, which is not the last frame: the
        // cap posts WM_CLOSE and the game blanks the screen on its way out, so the
        // final frames are the teardown.
        if (!fs.path.empty() && nonbg) {
            snprintf(g_last_path, sizeof g_last_path, "%s", fs.path.c_str());
            g_last_distinct = distinct;
            g_last_nonbg = nonbg;
            g_last_src = distinct_src;
            g_last_pal = distinct_pal;
        }
        if (distinct > g_seen_max_distinct)
            g_seen_max_distinct = distinct;
        if (nonbg > g_seen_max_nonbg)
            g_seen_max_nonbg = nonbg;
        if (!fs.path.empty() && distinct > g_best_distinct) {
            g_best_distinct = distinct;
            g_best_nonbg = nonbg;
            g_best_src = distinct_src;
            g_best_pal = distinct_pal;
            snprintf(g_best_path, sizeof g_best_path, "%s", fs.path.c_str());
        }
        first_with_content = nonbg && g_frames.size() >= 1 && g_frames.back().nonbg == 0;
        g_frames.push_back(fs);
    }

    if (index == 0 || (index % 25) == 0 || first_with_content) {
        printf("[host] frame %u: %dx%d %dbpp, %u distinct colours from %u surface "
               "values and a %u-colour palette, %u non-background pixels%s\n",
               index, w, h, bpp, distinct, distinct_src, distinct_pal, nonbg,
               fs.path.empty() ? "" : " (written)");
        fflush(stdout);
    }
}

// Same display seam as the windowed presenter, using the headless file sink.
// Presented frames, as the windowed host's present.cpp counts them: a game's
// native code reads it (HommaRe's camera.c paces its gestures by it).
extern "C" uint32_t host_present_count(void) {
    return g_present_count;
}
extern "C" void host_display_present_window(const uint32_t *argb, int w, int h) {
    host_present(argb, w, h, 32, nullptr, w * 4);
}

extern "C" void host_d3d_begin_scene() {
    ReportLock held;
    ++g_d3d_scenes;
}
extern "C" void host_d3d_end_scene() {}
extern "C" void host_d3d_draw(const HostD3DDrawSnapshot *cmd) {
    (void)cmd;
    ReportLock held;
    ++g_d3d_draws;
}
extern "C" void host_d3d_clear(uint32_t flags, const int32_t *rects, uint32_t count, uint32_t color,
                               float depth) {
    (void)flags;
    (void)rects;
    (void)count;
    (void)color;
    (void)depth;
    ReportLock held;
    ++g_d3d_clears;
}
extern "C" void host_d3d_texture(const HostD3DTexture *tex) {
    (void)tex;
    ReportLock held;
    ++g_d3d_textures;
}
extern "C" void host_d3d_texture_destroyed(uint32_t handle) {
    (void)handle;
}

// ---------------------------------------------------------------------------
// Audio. The real mixer, rendered offline.
//
// This host used to model the play cursor with arithmetic and stub out
// everything else, which answered the question the guest asks - has the cursor
// moved - and none of the questions a person asks after a run sounds wrong.
// It now links the same audio.mm the windowed host uses and runs its engine in
// manual rendering mode: the same player nodes, the same mixer, the same
// scheduling, rendered into a buffer instead of into a device. Nothing is
// heard, no hardware is opened, and what comes out is what would have.
//
// The render is pumped from the host's turn, in step with the wall clock the
// guest is reading. Rendering ahead would run every play cursor fast; not
// rendering at all would freeze them, which is the stall that used to wedge
// the intro movie.
//
// RECOMP_HOST_AUDIO_CAPTURE=<path.wav> writes it to a file. Without it the mix is
// still rendered and still measured, because the measurements are what say
// whether it was right.
// ---------------------------------------------------------------------------
namespace {

const double kAudioRate = 48000.0;
bool g_audio_offline = false;
uint64_t g_audio_frames_rendered = 0;

void audio_offline_begin() {
    if (!host_audio_offline_begin(kAudioRate, 4096)) {
        printf("[headless] no audio engine; the mix is not rendered\n");
        return;
    }
    g_audio_offline = true;
    const char *capture = recomp_env("HOST_AUDIO_CAPTURE");
    // Measured either way. A path only decides whether it is also written.
    host_audio_capture_begin(capture && *capture ? capture : nullptr);
    printf("[headless] audio rendered offline at %.0f Hz%s%s\n", kAudioRate,
           capture && *capture ? ", captured to " : " (measured, not written)",
           capture && *capture ? capture : "");
    fflush(stdout);
}

// Renders exactly as much as the wall clock has passed, and no more.
void audio_pump() {
    if (!g_audio_offline)
        return;
    double elapsed = boot_elapsed();
    if (elapsed < 0)
        return;
    uint64_t want = (uint64_t)(elapsed * kAudioRate);
    if (want <= g_audio_frames_rendered)
        return;
    uint64_t behind = want - g_audio_frames_rendered;
    // A quarter of a second is as far as this will catch up by rendering.
    // Beyond that the time is skipped rather than played: the loading pause
    // before the first turn is seconds long, and rendering it would produce a
    // burst of audio faster than real time whose cursors then disagree with
    // everything measured against the wall. The skip keeps the cursors moving
    // over time nobody could have heard anyway.
    const uint64_t catch_up = (uint64_t)(kAudioRate / 4);
    if (behind > catch_up) {
        host_audio_offline_skip((uint32_t)(behind - catch_up));
        g_audio_frames_rendered += behind - catch_up;
        behind = catch_up;
    }
    float peak = 0.0f;
    uint32_t got = host_audio_offline_render((uint32_t)behind, &peak);
    g_audio_frames_rendered += got ? got : behind;
}

// The runtime calls this on the run thread whenever the guest is about to
// block. It has to render, and that is not an optimisation: a play cursor
// counts rendered audio now, and the guest blocks waiting for one to move. If
// nothing rendered while it waited, the cursor would wait for the guest and
// the guest would wait for the cursor, which is a hang the video player finds
// within six frames.
int audio_idle_wait(double seconds) {
    (void)seconds;
    if (!g_audio_offline)
        return 0;
    uint64_t before = g_audio_frames_rendered;
    audio_pump();
    return g_audio_frames_rendered != before ? 1 : 0;
}

void audio_offline_end() {
    if (!g_audio_offline)
        return;
    host_audio_capture_end();
    host_audio_offline_end();
    g_audio_offline = false;
}

} // namespace

extern "C" void host_input_state(HostInputState *out) {
    // No mouse, no keyboard: a session where nobody touches anything. Zeroing
    // is what DirectInput reports for an idle device, and it is the truth here.
    if (out)
        memset(out, 0, sizeof *out);
}

namespace {

// ---------------------------------------------------------------------------
// The caps. boot.cpp calls this about once a millisecond from inside the
// guest's own clock reads, which is the point at which a real process yields
// to the system, and it is the only place this host gets to run.
// ---------------------------------------------------------------------------
void headless_tick() {
    // The mixer, rendered in step with the wall clock the guest is reading.
    // This is also the only thing that moves a play cursor here, so it runs
    // before the caps are looked at and on every turn.
    audio_pump();
    if (boot_close_requested())
        return;
    if (g_present_count < g_max_frames)
        boot_present_windows();
    bool over_frames = g_present_count >= g_max_frames;
    bool over_time = boot_elapsed() >= g_max_seconds;
    if (!over_frames && !over_time)
        return;

    const char *reason = over_frames ? "frame cap reached, WM_CLOSE posted"
                                     : "wall-clock cap reached, WM_CLOSE posted";
    boot_request_close(reason);
    printf("[host] %s (%u frames, %.1fs)\n", reason, g_present_count, boot_elapsed());
    fflush(stdout);
}

// ---------------------------------------------------------------------------
// The run report. Printed by main when the guest comes back, and by the
// watchdog when it does not, so a run always says what it did.
// ---------------------------------------------------------------------------
void print_report(FILE *out, bool abnormal) {
    (void)out;
    printf("\n== headless run ==\n");
    printf("stopped:            %s\n", boot_stop_reason());
    printf("elapsed:            %.1fs\n", boot_elapsed());
    printf("presented frames:   %u (%u written to %s)\n", g_present_count, g_frames_written,
           g_frames_dir.c_str());
    printf("present depth:      %u at 8bpp, %u at 16bpp, %u at 32bpp\n", g_present8, g_present16,
           g_present32);
    if (g_mode_sets)
        printf("display mode:       %dx%d %dbpp (set %u time%s)\n", g_mode_w, g_mode_h, g_mode_bpp,
               g_mode_sets, g_mode_sets == 1 ? "" : "s");
    else
        printf("display mode:       never set\n");
    printf("renderer path:      %s (scenes %u, draws %u, clears %u, textures %u)\n",
           g_d3d_draws ? "Direct3D device drew"
                       : (g_d3d_scenes ? "Direct3D device created, no draws"
                                       : "DirectDraw only, no Direct3D draws"),
           g_d3d_scenes, g_d3d_draws, g_d3d_clears, g_d3d_textures);
    // What the mixer actually produced, which is a different question from
    // what the shim was asked for and the only one a person can check by ear.
    host_capture_print(stdout);
    // What the clipper took off. The capture is written on the far side of it,
    // so the capture's own count of samples above full scale is zero by
    // construction now and this is the only place the overshoot is visible.
    if (host_audio_clipped_samples())
        printf("                    the clipper took the top off %llu samples "
               "(worst %.3f of full scale); nothing below it was touched\n",
               (unsigned long long)host_audio_clipped_samples(),
               (double)host_audio_worst_overshoot());
    host_audio_queue_report(stdout);
    // The COM inventory says which interfaces the game actually created, which
    // is the difference between "no Direct3D draws because it renders through
    // DirectDraw" and "no Direct3D draws because the device never existed".
    boot_print_dx_objects(stdout);
    boot_print_exit_code(stdout);
    printf("content:            %s\n",
           g_seen_max_nonbg
               ? "a frame with non-uniform content was presented"
               : (g_present_count ? "every presented frame was uniform" : "nothing was presented"));
    printf("front end reached:  not established by this run. A presented frame\n"
           "                    with content is what it can show; whether that\n"
           "                    frame is the front end is for a person to confirm\n"
           "                    by looking at it.\n");
    // The richest frame by colour count is usually a video frame, so the last
    // one written is named too: the run ends on whatever the game settled on,
    // which is the frame a person should be looking at.
    // This is the frame worth looking at. The richest by colour count is
    // almost always a video frame, and the literal last frame is the blank the
    // game leaves behind as it shuts down.
    if (g_last_path[0])
        printf("last drawn frame:   %s\n"
               "                    %u distinct colours, %u distinct surface values, "
               "%u-colour palette, %u non-background pixels\n",
               g_last_path, g_last_distinct, g_last_src, g_last_pal, g_last_nonbg);
    if (g_best_path[0])
        printf("richest frame:      %s\n"
               "                    %u distinct colours, %u distinct surface values, "
               "%u-colour palette, %u non-background pixels\n",
               g_best_path, g_best_distinct, g_best_src, g_best_pal, g_best_nonbg);

    boot_print_undeliverable(stdout);
    fflush(stdout);
    boot_print_import_stats(stdout, abnormal);
    // The lock belongs to whoever called: main takes it around a normal
    // report, and the watchdog takes it with a bound and refuses to report at
    // all if it cannot. Unlocking here would release a lock this function
    // never took.
}

} // namespace

// Configure and run the frame-writing host with bounded execution and offline audio.
// Exit status distinguishes missing frames or content from a completed capture.
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    if (const char *v = recomp_env("MAX_FRAMES"))
        g_max_frames = (uint32_t)strtoul(v, nullptr, 0);
    if (const char *v = recomp_env("MAX_SECONDS"))
        g_max_seconds = strtod(v, nullptr);
    if (const char *v = recomp_env("FRAME_EVERY"))
        g_frame_every = (uint32_t)strtoul(v, nullptr, 0);
    if (const char *v = recomp_env("FRAMES"))
        g_frames_dir = v;
    if (recomp_env("NO_ACTIVATE"))
        g_activate = false;

    mkdir_p(g_frames_dir);

    BootOptions opts;
    opts.name = "headless";
    opts.activate = g_activate;
    opts.tick = headless_tick;
    opts.report = print_report;
    opts.idle_wait = audio_idle_wait;
    // The watchdog's deadline is the wall-clock cap plus a grace period: past
    // that the guest has stopped calling into the runtime at all, so no cap
    // could otherwise fire.
    opts.deadline_seconds = g_max_seconds;
    opts.deadline_grace = 30.0;
    opts.close_unwind_grace = 15.0;

    if (!boot_load(opts)) {
        fprintf(stderr, "headless: loader_load: %s\n", loader_error());
        return 2;
    }

    printf("headless: %s, entry %08x, image %08x..%08x\n", loader_exe_path().c_str(),
           loader_entry_point(), loader_image_base(), loader_image_limit());
    printf("headless: caps %u frames / %.0fs, frames -> %s\n", g_max_frames, g_max_seconds,
           g_frames_dir.c_str());
    fflush(stdout);

    // The engine goes into manual rendering mode before the guest starts, so
    // no audio device is ever opened by this host.
    audio_offline_begin();

    boot_run();

    audio_offline_end();

    boot_report_lock();
    print_report(stdout, false);
    boot_report_unlock();

    // The exit status says how the run ended, so a script never reads a
    // rescued run as a good one.
    //   0  the guest ended the run itself and presented a frame with content
    //   1  nothing was presented at all
    //   3  the run did not end the way the host asked: the host had to unwind
    //      out of guest code, or the guest exited with a non-zero code
    //   4  frames were presented but every one of them was uniform
    // The watchdog and the fault handler exit on their own, with 4, 5 and 6.
    if (boot_abnormal_exit()) {
        fprintf(stderr, "headless: the run did not end normally (%s)\n", boot_stop_reason());
        return 3;
    }
    if (!g_present_count)
        return 1;
    if (!g_seen_max_nonbg)
        return 4;
    return 0;
}
