// d3d9.cpp - Direct3D 9, for a game that renders through shaders.
//
// The kit's other Direct3D module (d3d.cpp) models version 2: a fixed-function
// recorder reached by QueryInterface on a DirectDraw object. Version 9 shares
// none of that. It is its own DLL with its own factory function, its own
// object model, and a device whose drawing is programmable, so it lives here.
//
// What works today: the factory object, everything a game asks it before it
// commits to a device (adapter count, identifier, display mode, device type
// and format checks, caps) and CreateDevice itself. The device's vtable is
// complete and correctly ordered - a guest calls these by slot index, so the
// order is the interface - but only the handful of methods below do anything;
// every other slot reports itself once and returns D3D_OK. That turns what
// was a crash into a list of exactly which device methods this game uses,
// which is what the drawing work needs next.
//
// Nothing here rasterizes. No pixels are produced by this file yet.
#include "com.h"
#include "dx.h"
#include "../runtime/guest.h"
#include "../runtime/memory.h"
#include "../runtime/display_seam.h"
#include "../runtime/win32.h"
#include "../platform/os.h"
#include "../platform/ab_phase.h"
#include "host_api.h"
#include "d3d9_ffp.h"
#include "d3d9_pipeline.h"
#include "d3d9_raster.h"
#include "d3d9_shader.h"
#include "d3d9_volume.h"
#include "host_d9.h"
#include <unordered_map>
#include <map>
#if defined(__APPLE__) || defined(__linux__)
#define D9_PAGE_PROTECT 1
#include <dlfcn.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#else
#define D9_PAGE_PROTECT 0
#endif
#include <tuple>

#include <string.h>
#include <iterator>
#include <algorithm>
#include <initializer_list>

#define IID_BYTES(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)                                         \
    {(uint8_t)((a) & 0xff),                                                                        \
     (uint8_t)(((a) >> 8) & 0xff),                                                                 \
     (uint8_t)(((a) >> 16) & 0xff),                                                                \
     (uint8_t)(((a) >> 24) & 0xff),                                                                \
     (uint8_t)((b) & 0xff),                                                                        \
     (uint8_t)(((b) >> 8) & 0xff),                                                                 \
     (uint8_t)((c) & 0xff),                                                                        \
     (uint8_t)(((c) >> 8) & 0xff),                                                                 \
     d0,                                                                                           \
     d1,                                                                                           \
     d2,                                                                                           \
     d3,                                                                                           \
     d4,                                                                                           \
     d5,                                                                                           \
     d6,                                                                                           \
     d7}

// {81BDCBCA-64D4-426d-AE8D-AD0147F4275C} and {D0223B96-BF7A-43fd-92BD-A43B0D82B9EB}
static const uint8_t IID_IDirect3D9_[16] =
    IID_BYTES(0x81bdcbca, 0x64d4, 0x426d, 0xae, 0x8d, 0xad, 0x01, 0x47, 0xf4, 0x27, 0x5c);
static const uint8_t IID_IDirect3DDevice9_[16] =
    IID_BYTES(0xd0223b96, 0xbf7a, 0x43fd, 0x92, 0xbd, 0xa4, 0x3b, 0x0d, 0x82, 0xb9, 0xeb);

#define MAKE_D3DHRESULT(code) (0x88760000u | (uint32_t)(code))
static const uint32_t D3D_OK9 = 0u;
static const uint32_t D3DERR_NOTAVAILABLE = MAKE_D3DHRESULT(2154);
static const uint32_t D3DERR_INVALIDCALL = MAKE_D3DHRESULT(2156);
// The draw limits this device reports in its caps (GetDeviceCaps) and enforces in the four draw calls (Test155):
// one definition, so what the guest is told and what is enforced cannot drift apart.
static const uint32_t D9_MAX_PRIMITIVE_COUNT = 1048575;
static const uint32_t D9_MAX_VERTEX_INDEX = 1048575;
static const uint32_t D3DERR_WASSTILLDRAWING = MAKE_D3DHRESULT(540);
static const uint32_t D3DERR_NOTFOUND9 = MAKE_D3DHRESULT(2150);
static const uint32_t D3DLOCK_DONOTWAIT = 0x4000u;

// Defined with the resources further down.
struct ComObj;
static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s);
static bool gpu_on();
static HostD9Target gpu_target(ComObj *dev);
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]);
static void gpu_target_drawn(ComObj *dev, bool color, bool depth);
static void gpu_sync(ComObj *o);
static HostD9Surface surface_ref(ComObj *s);
static void gpu_drawn_into(ComObj *s);

// Vertex fetch (runtime/native_seam.h, dx/host_d9.h): the one vertex buffer
// whose unwritten vertices the game supplies, and the buffers it fills.
static struct {
    uint32_t vb = 0, stride = 0, nverts = 0;
    uint32_t size[5] = {};
    bool written[5] = {};
    int check = 0;
    uint64_t presents = 0;
} g_vfetch;

// One scratch block for structures a method fills in for the guest.
static uint32_t g_scratch = 0, g_scratch_size = 0;
static uint32_t scratch9(uint32_t n) {
    if (g_scratch_size < n) {
        if (g_scratch)
            heap_free(g_scratch);
        g_scratch = heap_alloc(n, true, 16);
        g_scratch_size = g_scratch ? n : 0;
    }
    if (g_scratch)
        memset(gm_ptr(g_scratch), 0, g_scratch_size);
    return g_scratch;
}

static ComObj *this_d3d9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9) ? o : nullptr;
}
static ComObj *this_device9(X86 *c) {
    ComObj *o = com_this_arg(c);
    return (o && o->kind == K_D3D9DEVICE) ? o : nullptr;
}

// ---------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------
// One adapter, the host window's display. A game that enumerates further gets
// an honest "there is only one".
void D9_RegisterSoftwareDevice(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
void D9_GetAdapterCount(X86 *c) {
    set_eax(c, 1);
}

// D3DADAPTER_IDENTIFIER9: two 512-byte strings, a 32-byte device name, the
// driver version, four ids, the identifier GUID and the WHQL level.
void D9_GetAdapterIdentifier(X86 *c) {
    uint32_t out = arg(c, 3);
    if (!out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(out), 0, 1100);
    // Games of this era pick their feature set by vendor and device id, and
    // turn off whatever they do not recognise (Most Wanted offers shadows
    // only on NVIDIA and ATI). The renderer covers what a GeForce 7800 GTX
    // offered, hardware shadow maps included, so that is the card it names.
    gm_put_str(out, "nv4_disp.dll", 512);
    gm_put_str(out + 512, "NVIDIA GeForce 7800 GTX", 512);
    gm_put_str(out + 1024, "\\\\.\\DISPLAY1", 32);
    // Driver[512], Description[512], DeviceName[32], then:
    wr32(out + 1056, 0x000a249b); // DriverVersion 6.14.10.9371 (low, high)
    wr32(out + 1060, 0x0006000e);
    wr32(out + 1064, 0x10de); // VendorId
    wr32(out + 1068, 0x0091); // DeviceId
    wr32(out + 1076, 0xa1);   // Revision
    wr32(out + 1096, 1);      // WHQLLevel: certified
    com_ret(c, D3D_OK9);
}

extern "C" int host_display_fullscreen_size(int *w, int *h);

// The modes a settings screen lists: the usual 4:3, 16:10 and 16:9 sizes up to
// the display, and the display's own size. The game renders at the one it picks
// and the host scales the frame to the window, so any of them is honest.
struct D9Mode {
    uint32_t w, h;
};
static const std::vector<D9Mode> &display_modes() {
    static std::vector<D9Mode> modes;
    static int built_w = -1, built_h = -1;
    int dw = 0, dh = 0;
    if (!host_display_fullscreen_size(&dw, &dh) || dw <= 0 || dh <= 0)
        dw = dh = 0;
    if (dw == built_w && dh == built_h && !modes.empty())
        return modes;
    built_w = dw;
    built_h = dh;
    static const D9Mode common[] = {
        {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800},  {1280, 960},
        {1280, 1024}, {1366, 768},  {1440, 900},  {1600, 900},  {1600, 1200}, {1680, 1050},
        {1920, 1080}, {1920, 1200}, {2560, 1080}, {2560, 1440}, {2560, 1600}, {3440, 1440},
        {3840, 2160},
    };
    modes.clear();
    for (const D9Mode &m : common)
        if (!dw || (m.w <= (uint32_t)dw && m.h <= (uint32_t)dh) || (m.w == 1280 && m.h == 960))
            modes.push_back(m);
    if (dw) {
        // The display's own size, and its shape at 1200 rows: a phone's screen
        // is wider than any common mode and too many pixels to fill at full size.
        int32_t sw = 0, sh = 0;
        host_display_screen_mode(dw, dh, 1200, &sw, &sh);
        for (const D9Mode own : {D9Mode{(uint32_t)dw, (uint32_t)dh}, D9Mode{(uint32_t)sw, (uint32_t)sh}}) {
            bool have = false;
            for (const D9Mode &m : modes)
                have = have || (m.w == own.w && m.h == own.h);
            if (!have)
                modes.push_back(own);
        }
    }
    std::sort(modes.begin(), modes.end(), [](const D9Mode &a, const D9Mode &b) {
        return a.w != b.w ? a.w < b.w : a.h < b.h;
    });
    return modes;
}

// D3DDISPLAYMODE: width, height, refresh rate, format. X8R8G8B8 is 22.
static void put_display_mode(uint32_t p, uint32_t w = 1280, uint32_t h = 960) {
    if (!p)
        return;
    wr32(p + 0, w);
    wr32(p + 4, h);
    wr32(p + 8, 60);
    wr32(p + 12, 22);
}
// (this, Adapter, Format)
void D9_GetAdapterModeCount(X86 *c) {
    set_eax(c, (uint32_t)display_modes().size());
}
// (this, Adapter, Format, Mode, pMode)
void D9_EnumAdapterModes(X86 *c) {
    const auto &modes = display_modes();
    uint32_t i = arg(c, 3);
    if (i >= modes.size()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    put_display_mode(arg(c, 4), modes[i].w, modes[i].h);
    com_ret(c, D3D_OK9);
}
// The desktop's mode is the display's own size (what display_modes() ends
// with). A game that sizes a window from it gets the display's aspect: Heroes V
// clamps a windowed gfx_resolution to the desktop mode (005ffd80), so a fixed
// 1280x960 turned a windowed 3440x1440 into 4:3. 1280x960 is only the
// fallback before the host knows the display.
void D9_GetAdapterDisplayMode(X86 *c) {
    int dw = 0, dh = 0;
    if (host_display_fullscreen_size(&dw, &dh) && dw > 0 && dh > 0)
        put_display_mode(arg(c, 2), (uint32_t)dw, (uint32_t)dh);
    else
        put_display_mode(arg(c, 2));
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceType(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormat(X86 *c) {
    if (arg(c, 5) == 4 && (!d9volume::pixel_bytes(arg(c, 6)) || (arg(c, 4) & ~0x210u))) {
        com_ret(c, D3DERR_NOTAVAILABLE);
        return;
    }
    com_ret(c, D3D_OK9);
}
// D3DMULTISAMPLE_TYPE and quality as a sample count: 2x and 4x, which every
// Apple GPU renders. NONMASKABLE (1) offers them as quality levels 0 and 1.
static uint32_t multisample_count(uint32_t type, uint32_t quality) {
    if (type == 1)
        return quality == 0 ? 2 : 4;
    if (type >= 2)
        return type >= 4 ? 4 : 2;
    return 0;
}
// (this, Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType,
// pQualityLevels)
void D9_CheckDeviceMultiSampleType(X86 *c) {
    uint32_t type = arg(c, 5), quality = arg(c, 6);
    uint32_t levels = type == 0 ? 1 : type == 1 ? 2 : (type == 2 || type == 4) ? 1 : 0;
    if (!levels) {
        com_ret(c, 0x8876086au); // D3DERR_NOTAVAILABLE
        return;
    }
    if (quality && gm_valid(quality, 4))
        wr32(quality, levels);
    com_ret(c, D3D_OK9);
}
void D9_CheckDepthStencilMatch(X86 *c) {
    com_ret(c, D3D_OK9);
}
void D9_CheckDeviceFormatConversion(X86 *c) {
    com_ret(c, D3D_OK9);
}

// D3DCAPS9 (304 bytes), as a GeForce 7800 GTX reports it for what the
// renderer implements: shader model 3.0, trilinear and 16x anisotropic
// filtering, the usual blend, compare and stencil sets, non-power-of-two
// textures, four render targets.
static void put_caps9(uint32_t p) {
    if (!p)
        return;
    memset(gm_ptr(p), 0, 304);
    auto f = [&](uint32_t off, float v) { wrf32(p + off, v); };
    wr32(p + 0, 1);            // DeviceType: D3DDEVTYPE_HAL
    wr32(p + 8, 0x00020000u);  // Caps: READ_SCANLINE
    wr32(p + 12, 0x00020000u); // Caps2: FULLSCREENGAMMA
    wr32(
        p + 16,
        0x000003a0u); // Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM, LINEAR_TO_SRGB
    wr32(p + 20, 0x8000000fu); // PresentationIntervals: IMMEDIATE, ONE..FOUR
    wr32(p + 28, 0x001bbef0u); // DevCaps: HWTRANSFORMANDLIGHT, PUREDEVICE, HWRASTERIZATION, ...
    wr32(
        p + 32,
        0x0003ccf2u); // PrimitiveMiscCaps: masks, cull modes, blend op, separate alpha, independent write masks
    wr32(
        p + 36,
        0x0f732191u); // RasterCaps: dither, zbias, fog, anisotropy, scissor, slope-scale depth bias
    wr32(p + 40, 0x000000ffu); // ZCmpCaps: all
    wr32(p + 44, 0x00003fffu); // SrcBlendCaps: all, BLENDFACTOR
    wr32(p + 48, 0x00003fffu); // DestBlendCaps
    wr32(p + 52, 0x000000ffu); // AlphaCmpCaps
    wr32(p + 56, 0x00084208u); // ShadeCaps
    wr32(
        p + 60,
        0x0001ec45u); // TextureCaps: PERSPECTIVE, ALPHA, CUBEMAP, MIPMAP, MIPCUBEMAP, PROJECTED, VOLUMEMAP, ...
    wr32(
        p + 64,
        0x03030700u); // TextureFilterCaps: MIN point/linear/aniso, MIP point/linear, MAG point/linear
    wr32(p + 68, 0x03030300u);  // CubeTextureFilterCaps
    wr32(p + 72, 0x03030300u);  // VolumeTextureFilterCaps
    wr32(p + 76, 0x0000003fu);  // TextureAddressCaps
    wr32(p + 80, 0x0000003fu);  // VolumeTextureAddressCaps
    wr32(p + 84, 0x0000001fu);  // LineCaps
    wr32(p + 88, 4096);         // MaxTextureWidth
    wr32(p + 92, 4096);         // MaxTextureHeight
    wr32(p + 96, 512);          // MaxVolumeExtent
    wr32(p + 100, 8192);        // MaxTextureRepeat
    wr32(p + 104, 8192);        // MaxTextureAspectRatio
    wr32(p + 108, 16);          // MaxAnisotropy
    f(112, 1e10f);              // MaxVertexW
    f(116, -1e8f);              // GuardBandLeft
    f(120, -1e8f);              // GuardBandTop
    f(124, 1e8f);               // GuardBandRight
    f(128, 1e8f);               // GuardBandBottom
    wr32(p + 136, 0x000001ffu); // StencilCaps: all ops, TWOSIDED
    wr32(p + 140, 0x00080008u); // FVFCaps: 8 texture coordinates, PSIZE
    wr32(p + 144, 0x03feffffu); // TextureOpCaps
    wr32(p + 148, 8);           // MaxTextureBlendStages
    wr32(p + 152, 8);           // MaxSimultaneousTextures
    wr32(p + 156, 0x0000007bu); // VertexProcessingCaps
    wr32(p + 160, 8);           // MaxActiveLights
    wr32(p + 164, 6);           // MaxUserClipPlanes
    wr32(p + 168, 4);           // MaxVertexBlendMatrices
    f(176, 8192.0f);            // MaxPointSize
    wr32(p + 180, D9_MAX_PRIMITIVE_COUNT); // MaxPrimitiveCount (enforced by draw_refused)
    wr32(p + 184, D9_MAX_VERTEX_INDEX);    // MaxVertexIndex (enforced by draw_refused)
    wr32(p + 188, 16);          // MaxStreams
    wr32(p + 192, 255);         // MaxStreamStride
    wr32(p + 196, 0xfffe0300u); // VertexShaderVersion vs_3_0
    wr32(p + 200, 256);         // MaxVertexShaderConst
    wr32(p + 204, 0xffff0300u); // PixelShaderVersion ps_3_0
    f(208, 3.4e38f);            // PixelShaderMaxValue
    wr32(
        p + 212,
        0x00000051u); // DevCaps2: STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES, VERTEXELEMENTSCANSHARESTREAMOFFSET
    wr32(p + 232, 1);           // NumberOfAdaptersInGroup
    wr32(p + 236, 0x0000030fu); // DeclTypes
    wr32(p + 240, 4);           // NumSimultaneousRTs
    wr32(p + 244, 0x03000300u); // StretchRectFilterCaps: point, linear
    wr32(p + 248, 1);           // VS20Caps.Caps: PREDICATION
    wr32(p + 252, 24);          // VS20Caps.DynamicFlowControlDepth
    wr32(p + 256, 32);          // VS20Caps.NumTemps
    wr32(p + 260, 4);           // VS20Caps.StaticFlowControlDepth
    wr32(p + 264, 0x1f);        // PS20Caps.Caps
    wr32(p + 268, 24);          // PS20Caps.DynamicFlowControlDepth
    wr32(p + 272, 32);          // PS20Caps.NumTemps
    wr32(p + 276, 4);           // PS20Caps.StaticFlowControlDepth
    wr32(p + 280, 512);         // PS20Caps.NumInstructionSlots
    wr32(p + 284, 0x00000300u); // VertexTextureFilterCaps
    wr32(p + 288, 0xffffffffu); // MaxVShaderInstructionsExecuted
    wr32(p + 292, 0xffffffffu); // MaxPShaderInstructionsExecuted
    wr32(p + 296, 32768);       // MaxVertexShader30InstructionSlots
    wr32(p + 300, 32768);       // MaxPixelShader30InstructionSlots
}
void D9_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 3));
    com_ret(c, D3D_OK9);
}
void D9_GetAdapterMonitor(X86 *c) {
    set_eax(c, 1); // a non-null HMONITOR
}

// The host learns the back buffer's size as the display mode, as DXGI's
// swap chains tell it (dxgi.cpp): the window then takes the frame's aspect
// (window_size_for) and the guest window's client area its size. Without it a
// windowed 21:9 back buffer sat letterboxed in the startup window's 4:3.
static void announce_mode(const ComObj *dev) {
    if (dev && dev->width && dev->height)
        host_set_display_mode((int)dev->width, (int)dev->height, 32);
}

// D3DSWAPEFFECT_DISCARD (D3DPRESENT_PARAMETERS +24 == 1) leaves the back buffer
// undefined after Present; this device keeps it, so a game that draws only part
// of a frame shows the previous one around it. Heroes V's loading screens are
// one 1024x768 picture: centred on a wide screen (games/hof hof_screen.c) the
// menu stayed frozen beside them. RECOMP_D3D9_CLEAR_DISCARDED=1 clears such a
// back buffer to black after each Present instead.
static std::unordered_map<uint32_t, bool> &discard_swap() {
    static std::unordered_map<uint32_t, bool> m;
    return m;
}
static void note_swap_effect(const ComObj *dev, uint32_t present) {
    if (dev && present)
        discard_swap()[dev->id] = rd32(present + 24) == 1u;
}
static bool clear_discarded() {
    static const bool on = [] {
        const char *v = recomp_env("D3D9_CLEAR_DISCARDED");
        return v && v[0] == '1';
    }();
    return on;
}

void D9_CreateDevice(X86 *c) {
    ComObj *d3d = this_d3d9(c);
    uint32_t present = arg(c, 5), out = arg(c, 6);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_out_ptr(out, 0);
    // Direct3D 9 resets the calling thread's FPU at device creation unless the
    // behaviour flags (arg 4) hold D3DCREATE_FPU_PRESERVE (2): it masks every
    // floating-point exception (and selects single precision). NFS Carbon
    // unmasks invalid/zero-divide/overflow with _controlfp(3, 0x8001f) just
    // before its CreateDevice and relies on this reset; left unmasked, a CRT
    // maths call with an invalid argument raised STATUS_FLOAT_INVALID_OPERATION
    // and the CRT ended the game (first safehouse visit, 2026-10-10). Only the
    // masks are reset here; precision stays as the guest set it.
    if (!(arg(c, 4) & 2u))
        c->fpu_cw = uint16_t(c->fpu_cw | 0x3fu);
    ComObj *dev = com_new(K_D3D9DEVICE);
    if (!dev) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    dev->dev_d3d = d3d->id;
    // D3DPRESENT_PARAMETERS: back buffer size and format first, the device
    // window at 28 and Windowed at 32.
    if (present) {
        dev->width = rd32(present + 0);
        dev->height = rd32(present + 4);
        dev->bpp = 32;
        dev->hwnd = rd32(present + 28);
        dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    }
    uint32_t view = com_view(dev, IF_D3DDEVICE9);
    if (!view) {
        com_release(dev);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    {
        ComObj *bb = device_backbuffer(dev);
        D9Pipeline &pl = d9_pipeline(dev->id);
        bind_color_target(dev, 0, bb); // Test159: the device holds its bound target
        pl.scissor[2] = (int32_t)dev->width;
        pl.scissor[3] = (int32_t)dev->height;
        if (present && rd32(present + 36)) {
            device_depthbuffer(dev);
            pl.rs[7] = 1; // ZENABLE defaults on with an automatic depth buffer
            pl.states_changed();
        }
    }
    LOGW("d3d9: CreateDevice %ux%u, window %08x, %s renderer", dev->width, dev->height, dev->hwnd,
         host_d9_active() ? "GPU" : "CPU");
    announce_mode(dev);
    note_swap_effect(dev, present);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);

// ---------------------------------------------------------------------------
// IDirect3DDevice9
//
// Every slot is named and in its interface order, because a guest calls them
// by index. A stub reports itself once and returns D3D_OK: a game that is
// told "no" by a method it expects to work usually stops, and stopping tells
// us less than carrying on does.
// ---------------------------------------------------------------------------
#define D9_STUB(name, argc)                                                                        \
    void Dev_##name(X86 *c) {                                                                      \
        log_once("d3d9.dev." #name, "d3d9: IDirect3DDevice9::" #name " is not implemented");       \
        (void)argc;                                                                                \
        com_ret(c, D3D_OK9);                                                                       \
    }

D9_STUB(EvictManagedResources, 1)
D9_STUB(GetDisplayMode, 3)
D9_STUB(GetCreationParameters, 2)
D9_STUB(SetCursorProperties, 4)
D9_STUB(SetCursorPosition, 4)
D9_STUB(ShowCursor, 2)
D9_STUB(CreateAdditionalSwapChain, 3)
D9_STUB(GetSwapChain, 3)
D9_STUB(GetRasterStatus, 3)
D9_STUB(SetDialogBoxMode, 2)
// The game's brightness (Carbon's Brightness slider): SetGammaRamp(swapchain, flags,
// D3DGAMMARAMP *), 3 x 256 WORDs. Kept for GetGammaRamp; the presenter applies it.
uint16_t g_gamma_ramp[768];
bool g_gamma_set = false;
void Dev_SetGammaRamp(X86 *c) {
    const uint32_t ramp = arg(c, 3);
    if (ramp && gm_valid(ramp, sizeof g_gamma_ramp)) {
        memcpy(g_gamma_ramp, gm_ptr(ramp), sizeof g_gamma_ramp);
        g_gamma_set = true;
        host_set_gamma_ramp(g_gamma_ramp);
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetGammaRamp(X86 *c) {
    const uint32_t ramp = arg(c, 2);
    if (ramp && gm_valid(ramp, sizeof g_gamma_ramp)) {
        if (!g_gamma_set)
            for (int i = 0; i < 768; ++i)
                g_gamma_ramp[i] = uint16_t((i % 256) * 257);
        memcpy(gm_ptr(ramp), g_gamma_ramp, sizeof g_gamma_ramp);
    }
    com_ret(c, D3D_OK9);
}
void Dev_CreateVolumeTexture(X86 *c);
D9_STUB(UpdateSurface, 5)
void Dev_UpdateTexture(X86 *c);
void Dev_GetRenderTargetData(X86 *c);
D9_STUB(GetFrontBufferData, 3)
D9_STUB(ColorFill, 4)
void Dev_GetRenderTarget(X86 *c);
void Dev_GetTransform(X86 *c);
void Dev_MultiplyTransform(X86 *c);
void Dev_SetMaterial(X86 *c);
void Dev_GetMaterial(X86 *c);
void Dev_SetLight(X86 *c);
void Dev_GetLight(X86 *c);
void Dev_LightEnable(X86 *c);
void Dev_GetLightEnable(X86 *c);
D9_STUB(SetClipPlane, 3)
D9_STUB(GetClipPlane, 3)
void Dev_GetRenderState(X86 *c);
D9_STUB(CreateStateBlock, 3)
D9_STUB(BeginStateBlock, 1)
D9_STUB(EndStateBlock, 2)
D9_STUB(SetClipStatus, 2)
D9_STUB(GetClipStatus, 2)
void Dev_GetTexture(X86 *c);
void Dev_GetTextureStageState(X86 *c);
void Dev_GetSamplerState(X86 *c);
D9_STUB(ValidateDevice, 2)
D9_STUB(SetPaletteEntries, 3)
D9_STUB(GetPaletteEntries, 3)
D9_STUB(SetCurrentTexturePalette, 2)
D9_STUB(GetCurrentTexturePalette, 2)
D9_STUB(GetScissorRect, 2)
D9_STUB(SetSoftwareVertexProcessing, 2)
D9_STUB(GetSoftwareVertexProcessing, 1)
D9_STUB(SetNPatchMode, 2)
D9_STUB(GetNPatchMode, 1)
D9_STUB(ProcessVertices, 6)
void Dev_GetVertexDeclaration(X86 *c);
void Dev_SetFVF(X86 *c);
void Dev_GetFVF(X86 *c);
void Dev_CreateVertexShader(X86 *c);
void Dev_SetVertexShader(X86 *c);
void Dev_GetVertexShader(X86 *c);
void Dev_GetVertexShaderConstantF(X86 *c);
void Dev_SetVertexShaderConstantI(X86 *c);
void Dev_GetVertexShaderConstantI(X86 *c);
void Dev_GetStreamSource(X86 *c);
D9_STUB(SetStreamSourceFreq, 3)
D9_STUB(GetStreamSourceFreq, 3)
void Dev_GetIndices(X86 *c);
void Dev_CreatePixelShader(X86 *c);
void Dev_SetPixelShader(X86 *c);
void Dev_GetPixelShader(X86 *c);
void Dev_GetPixelShaderConstantF(X86 *c);
void Dev_SetPixelShaderConstantI(X86 *c);
void Dev_GetPixelShaderConstantI(X86 *c);
D9_STUB(DrawRectPatch, 4)
D9_STUB(DrawTriPatch, 4)
D9_STUB(DeletePatch, 2)

// (this, pRect)
void Dev_SetScissorRect(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t r = arg(c, 1);
    if (dev && r)
        for (int i = 0; i < 4; ++i)
            d9_pipeline(dev->id).scissor[i] = (int32_t)rd32(r + 4u * (uint32_t)i);
    com_ret(c, dev && r ? D3D_OK9 : D3DERR_INVALIDCALL);
}

static ComObj *device_backbuffer(ComObj *dev);
static ComObj *device_depthbuffer(ComObj *dev);
static void bind_depth_stencil(ComObj *dev, ComObj *s);
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s);
static std::unordered_map<uint32_t, uint32_t> &auto_depth_surfaces();
static std::unordered_map<uint32_t, uint32_t> &resource_usage();
static void device_forget_buffers(ComObj *dev);

// (this, pPresentationParameters): new back buffer and depth buffer at the
// new size; every resource the game made itself survives, as with
// D3DPOOL_MANAGED.
void Dev_Reset(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t present = arg(c, 1);
    if (!dev || !present) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    dev->width = rd32(present + 0);
    dev->height = rd32(present + 4);
    dev->samples = multisample_count(rd32(present + 16), rd32(present + 20));
    // Test159: Reset drops the device's bindings of render targets 1-3 and puts the new back buffer on 0.
    for (uint32_t i = 1; i < 4; ++i)
        bind_color_target(dev, i, nullptr);
    device_forget_buffers(dev);
    dev->present_deadline = FrameDeadline{};
    dev->present_fps = 0;
    ComObj *bb = device_backbuffer(dev);
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, 0, bb);
    pl.viewport_set = false;
    pl.scissor[0] = pl.scissor[1] = 0;
    pl.scissor[2] = (int32_t)dev->width;
    pl.scissor[3] = (int32_t)dev->height;
    if (rd32(present + 36)) { // EnableAutoDepthStencil
        device_depthbuffer(dev);
        pl.rs[7] = 1;
        pl.states_changed();
    } else
        bind_depth_stencil(dev, nullptr);
    LOGW("d3d9: Reset to %ux%u", dev->width, dev->height);
    announce_mode(dev);
    note_swap_effect(dev, present);
    // Test157c diagnostics (RECOMP_D3D9_STATS): live objects after each Reset, to tell a per-reset retention from
    // normal reloading (the review's 20-reset discriminator).
    static const bool reset_stats = recomp_env("D3D9_STATS") != nullptr;
    if (reset_stats && g_com_objects) {
        uint32_t surfaces = 0, textures = 0, vbs = 0, ibs = 0, all = 0;
        for (const auto &o : *g_com_objects)
            if (o && o->alive) {
                ++all;
                surfaces += o->kind == K_D3D9SURFACE;
                textures += o->kind == K_D3D9TEXTURE;
                vbs += o->kind == K_D3D9VB;
                ibs += o->kind == K_D3D9IB;
            }
        LOGW("d3d9 stats: after Reset: %u live objects (%u surfaces, %u textures, %u vertex / %u index buffers); "
             "bound depth %u, automatic depth %u",
             all, surfaces, textures, vbs, ibs, dev->zbuffer_obj, auto_depth_surfaces()[dev->id]);
        // Objects made since the previous Reset that are still alive, grouped by kind/size/format/usage/refs: what a
        // Reset leaves behind (default-pool render targets should not survive two Resets).
        static uint32_t prev_mark = 0;
        const uint32_t mark = (uint32_t)g_com_objects->size();
        std::map<std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, int32_t>, uint32_t> groups;
        for (uint32_t id = prev_mark + 1; id <= mark; ++id) {
            const ComObj *o = (*g_com_objects)[id - 1].get();
            if (!o || !o->alive)
                continue;
            auto u = resource_usage().find(o->id);
            ++groups[{(int)o->kind, o->width, o->height, o->rmask, u == resource_usage().end() ? 0u : u->second, o->refs}];
        }
        std::vector<std::pair<uint32_t, std::tuple<int, uint32_t, uint32_t, uint32_t, uint32_t, int32_t>>> top;
        for (const auto &g : groups)
            top.push_back({g.second, g.first});
        std::sort(top.begin(), top.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 8; ++i)
            LOGW("d3d9 stats:   survivors since the last Reset: %u x kind %d %ux%u format %u usage %u refs %d", top[i].first,
                 std::get<0>(top[i].second), std::get<1>(top[i].second), std::get<2>(top[i].second),
                 std::get<3>(top[i].second), std::get<4>(top[i].second), std::get<5>(top[i].second));
        prev_mark = mark;
    }
    com_ret(c, D3D_OK9);
}

void Dev_TestCooperativeLevel(X86 *c) {
    com_ret(c, this_device9(c) ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_GetAvailableTextureMem(X86 *c) {
    set_eax(c, 256u * 1024u * 1024u);
}
void Dev_GetNumberOfSwapChains(X86 *c) {
    set_eax(c, 1);
}
void Dev_GetDirect3D(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *d3d = dev ? com_get(dev->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!d3d || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(d3d, IF_D3D9);
    com_addref(d3d);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}
void Dev_GetDeviceCaps(X86 *c) {
    put_caps9(arg(c, 1));
    com_ret(c, D3D_OK9);
}
void Dev_BeginScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = true;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
void Dev_EndScene(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev)
        dev->in_scene = false;
    com_ret(c, dev ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// Defined with the resources below.
static ComObj *device_backbuffer(ComObj *dev);
static uint8_t *resource_bytes(ComObj *o);

// (this, Count, pRects, Flags, Color, Z, Stencil). Only the colour target is
// real so far: a 32-bit surface is filled with the colour, over the listed
// D3DRECTs or the whole surface.
void Dev_Clear(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t count = arg(c, 1), rects = arg(c, 2), flags = arg(c, 3), color = arg(c, 4);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        HostD9Target target = gpu_target(dev);
        int32_t vp[4];
        target_viewport(dev, target, vp);
        std::vector<int32_t> rs;
        if (count && rects && gm_fits(rects, count * 16u))
            for (uint32_t i = 0; i < count * 4; ++i)
                rs.push_back((int32_t)rd32(rects + 4 * i));
        float z;
        uint32_t zbits = arg(c, 5);
        memcpy(&z, &zbits, 4);
        host_d9_clear(&target, vp, (uint32_t)(rs.size() / 4), rs.empty() ? nullptr : rs.data(),
                      flags, color, z, arg(c, 6));
        gpu_target_drawn(dev, flags & 1u, flags & 6u);
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *s = com_get(dev->render_target);
    if (!s)
        s = device_backbuffer(dev);
    static uint32_t g_clears = 0;
    if (++g_clears <= 24)
        LOGW("d3d9: clear %u: flags %x colour %08x rects %u, target %u (%ux%u, %u bpp)%s", g_clears,
             flags, color, count, s ? s->id : 0, s ? s->width : 0, s ? s->height : 0,
             s ? s->bpp : 0, (s && s->id == dev->palette_obj) ? ", the back buffer" : "");
    uint8_t *bytes = resource_bytes(s);
    if ((flags & 1u) && bytes && s->bpp == 32) {
        uint32_t n = (count && rects) ? count : 1;
        for (uint32_t r = 0; r < n; ++r) {
            int32_t x1 = 0, y1 = 0, x2 = (int32_t)s->width, y2 = (int32_t)s->height;
            if (count && rects) {
                uint32_t q = rects + r * 16u;
                x1 = std::max(x1, (int32_t)rd32(q));
                y1 = std::max(y1, (int32_t)rd32(q + 4));
                x2 = std::min(x2, (int32_t)rd32(q + 8));
                y2 = std::min(y2, (int32_t)rd32(q + 12));
            }
            for (int32_t y = y1; y < y2; ++y) {
                uint8_t *row = bytes + (size_t)y * s->pitch;
                for (int32_t x = x1; x < x2; ++x)
                    memcpy(row + 4 * x, &color, 4);
            }
        }
        d9_raster_invalidate(s->id);
    }
    com_ret(c, D3D_OK9);
}

// (this, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion). The back
// buffer goes to the host as a 32-bit frame: the first thing this module puts
// on screen.
// RECOMP_D3D9_SYNC_STATS: what the lazy-buffer sync did, per second (Present logs it).
static struct {
    uint64_t calls, pages_stamped, pages_equal, pages_copied, locks_noover, locks_discard, locks_plain;
    uint64_t open_equal, open_copied; // of those, pages still writable this frame (synced per draw)
    uint64_t open_bytes, closed_bytes; // bytes compared on open / protected pages
    uint64_t open_skipped;             // open-page reads the synced-range cache answered
    uint64_t sync_ns, prot_frame_ns;   // time in gpu_sync_buffer_range / prot_frame
    uint64_t faults, faults_seq, faults_repeat, frames; // faults: next page after the last one / faulted last frame too
    uint64_t faults_big;               // in the biggest tracked buffer
    uint64_t changed_bytes, ib_bytes;  // of the bytes compared: changed / in index buffers
    uint64_t recloses, ahead_opened, sticky_kept, verify_bad; // the switches below prot_fault
    uint64_t prewritten;               // pages opened by d3d9_prewrite
} g_sync_stats;
static void sync_stats_faults();
static bool sync_stats_on() {
    static const bool on = recomp_env("D3D9_SYNC_STATS") != nullptr;
    return on;
}
// RECOMP_D3D9_FRAME_STATS=1 (or RECOMP_FPS_LOG): per-frame averages of what the
// game asked of the device, once a second - draws, primitives, buffer locks and
// the bytes they opened, state and shader calls. Counting is plain increments
// on the guest thread, always on; only the line is optional.
static struct {
    uint64_t frames, draws, prims, vb_locks, ib_locks, vb_lock_bytes, ib_lock_bytes, rs_calls,
        rs_changes, sampler_changes, tss_calls, tex_sets, shader_sets, vconst_vecs, pconst_vecs,
        stream_sets, decl_sets, prot_faults, prot_calls, uploads, upload_bytes, tex_uploads, tex_upload_bytes;
} g_frame_stats;
// RECOMP_D3D9_HITCH_MS=N: a frame whose game work (from the previous Present's return to
// this Present, so the frame limiter's wait is left out) took over N ms is logged with what
// it asked of the device and how long the previous Present took, to tell a stutter's cause
// (the walking hero's steps on a zoomed-out map, 2026-10-20) from a slow frame rate.
static uint64_t g_present_return_ns, g_present_call_ns;
static decltype(g_frame_stats) g_hitch_prev;
static void hitch_check() {
    static const double limit = [] {
        const char *v = recomp_env("D3D9_HITCH_MS");
        return v ? atof(v) : 0.0;
    }();
    if (limit <= 0 || !g_present_return_ns)
        return;
    const double work = (os_monotonic_ns() - g_present_return_ns) / 1e6;
    if (work <= limit)
        return;
    const auto &s = g_frame_stats, &p = g_hitch_prev;
    fprintf(stderr,
            "d3d9 hitch: %.1f ms of game work (previous present %.1f ms): %llu draws %llu prims, locks vb %llu "
            "(%llu KB) ib %llu (%llu KB), buffer uploads %llu (%llu KB), texture uploads %llu (%llu KB), "
            "page faults %llu, shaders %llu\n",
            work, g_present_call_ns / 1e6, (unsigned long long)(s.draws - p.draws),
            (unsigned long long)(s.prims - p.prims), (unsigned long long)(s.vb_locks - p.vb_locks),
            (unsigned long long)((s.vb_lock_bytes - p.vb_lock_bytes) >> 10),
            (unsigned long long)(s.ib_locks - p.ib_locks), (unsigned long long)((s.ib_lock_bytes - p.ib_lock_bytes) >> 10),
            (unsigned long long)(s.uploads - p.uploads), (unsigned long long)((s.upload_bytes - p.upload_bytes) >> 10),
            (unsigned long long)(s.tex_uploads - p.tex_uploads),
            (unsigned long long)((s.tex_upload_bytes - p.tex_upload_bytes) >> 10),
            (unsigned long long)(s.prot_faults - p.prot_faults), (unsigned long long)(s.shader_sets - p.shader_sets));
}
static void prot_frame();
// The 10-second phase the native replacements' alternate mode uses
// (native/native_check.h): CLOCK_MONOTONIC seconds / 10, odd or even.
static int ab_phase10() {
    return (int)((os_monotonic_ns() / 10000000000ull) & 1);
}
// RECOMP_PHASE_LOG=1 (TEST ONLY): at each change of that phase, the frames of
// the phase just ended and the game thread's CPU and wall time per frame, so
// an alternate-mode run gives an A/B of its switch on the same scene.
static void phase_log_tick() {
    static const bool on = recomp_env("PHASE_LOG") != nullptr;
    if (!on)
        return;
    static int phase = -1;
    static uint64_t frames = 0, cpu0 = 0, wall0 = 0;
    const int p = ab_phase10();
    const uint64_t cpu = os_thread_cpu_ns(), wall = os_monotonic_ns();
    if (p != phase) {
        if (phase >= 0 && frames)
            fprintf(stderr, "phase-ab: phase %d frames %llu cpu %.3f ms/frame wall %.3f ms/frame\n", phase,
                    (unsigned long long)frames, (cpu - cpu0) / 1e6 / frames, (wall - wall0) / 1e6 / frames);
        phase = p;
        frames = 0;
        cpu0 = cpu;
        wall0 = wall;
    }
    ++frames;
}
static void frame_stats_tick() {
    static const bool on = recomp_env("D3D9_FRAME_STATS") != nullptr || recomp_env("FPS_LOG") != nullptr;
    if (!on)
        return;
    g_frame_stats.frames++;
    static uint64_t t0 = os_monotonic_ns();
    const uint64_t now = os_monotonic_ns();
    if (now - t0 < 1000000000ull)
        return;
    const double f = g_frame_stats.frames ? (double)g_frame_stats.frames : 1.0;
    auto &s = g_frame_stats;
    fprintf(stderr,
            "d3d9 frame: %llu frames; per frame %.0f draws %.0f prims, locks vb %.0f (%.0f KB) ib %.0f "
            "(%.0f KB), render states %.0f (%.0f changed), samplers changed %.0f, tss %.0f, textures %.0f, "
            "shaders %.0f, constants vs %.0f ps %.0f, streams %.0f, decls %.0f, page faults %.1f "
            "protects %.1f, buffer uploads %.0f (%.0f KB)\n",
            (unsigned long long)s.frames, s.draws / f, s.prims / f, s.vb_locks / f, s.vb_lock_bytes / f / 1024,
            s.ib_locks / f, s.ib_lock_bytes / f / 1024, s.rs_calls / f, s.rs_changes / f,
            s.sampler_changes / f, s.tss_calls / f, s.tex_sets / f, s.shader_sets / f, s.vconst_vecs / f,
            s.pconst_vecs / f, s.stream_sets / f, s.decl_sets / f, s.prot_faults / f, s.prot_calls / f,
            s.uploads / f, s.upload_bytes / f / 1024);
    g_frame_stats = {};
    t0 = now;
}
void Dev_Present(X86 *c) {
    phase_log_tick();
    if (sync_stats_on()) {
        sync_stats_faults();
        const uint64_t t = os_monotonic_ns();
        prot_frame();
        g_sync_stats.prot_frame_ns += os_monotonic_ns() - t;
    } else {
        prot_frame();
    }
    hitch_check();
    frame_stats_tick();
    g_hitch_prev = g_frame_stats;
    if (sync_stats_on()) {
        static uint64_t t0 = os_monotonic_ns();
        const uint64_t now = os_monotonic_ns();
        if (now - t0 >= 1000000000ull) {
            fprintf(stderr,
                    "d3d9 sync: %llu draws, pages %llu checked %llu equal %llu copied (%.1f MB compared); "
                    "big-buffer locks noover %llu discard %llu plain %llu; open pages %llu equal %llu copied; "
                    "per frame: compared open %.0f KB closed %.0f KB, open cache hits %.0f, sync %.2f ms, "
                    "prot_frame %.2f ms, faults %.1f (%.0f%% next page, %.0f%% also last frame, %.0f%% big buffer), "
                    "changed %.0f KB, index buffers %.0f KB; recloses %.1f, opened ahead %.1f, kept open %.1f, "
                    "verify differences %llu, prewritten %.1f\n",
                    (unsigned long long)g_sync_stats.calls, (unsigned long long)g_sync_stats.pages_stamped,
                    (unsigned long long)g_sync_stats.pages_equal, (unsigned long long)g_sync_stats.pages_copied,
                    g_sync_stats.pages_stamped * 4096.0 / 1048576.0, (unsigned long long)g_sync_stats.locks_noover,
                    (unsigned long long)g_sync_stats.locks_discard, (unsigned long long)g_sync_stats.locks_plain,
                    (unsigned long long)g_sync_stats.open_equal, (unsigned long long)g_sync_stats.open_copied,
                    g_sync_stats.open_bytes / 1024.0 / std::max<uint64_t>(1, g_sync_stats.frames),
                    g_sync_stats.closed_bytes / 1024.0 / std::max<uint64_t>(1, g_sync_stats.frames),
                    (double)g_sync_stats.open_skipped / std::max<uint64_t>(1, g_sync_stats.frames),
                    g_sync_stats.sync_ns / 1e6 / std::max<uint64_t>(1, g_sync_stats.frames),
                    g_sync_stats.prot_frame_ns / 1e6 / std::max<uint64_t>(1, g_sync_stats.frames),
                    (double)g_sync_stats.faults / std::max<uint64_t>(1, g_sync_stats.frames),
                    100.0 * g_sync_stats.faults_seq / std::max<uint64_t>(1, g_sync_stats.faults),
                    100.0 * g_sync_stats.faults_repeat / std::max<uint64_t>(1, g_sync_stats.faults),
                    100.0 * g_sync_stats.faults_big / std::max<uint64_t>(1, g_sync_stats.faults),
                    g_sync_stats.changed_bytes / 1024.0 / std::max<uint64_t>(1, g_sync_stats.frames),
                    g_sync_stats.ib_bytes / 1024.0 / std::max<uint64_t>(1, g_sync_stats.frames),
                    (double)g_sync_stats.recloses / std::max<uint64_t>(1, g_sync_stats.frames),
                    (double)g_sync_stats.ahead_opened / std::max<uint64_t>(1, g_sync_stats.frames),
                    (double)g_sync_stats.sticky_kept / std::max<uint64_t>(1, g_sync_stats.frames),
                    (unsigned long long)g_sync_stats.verify_bad,
                    (double)g_sync_stats.prewritten / std::max<uint64_t>(1, g_sync_stats.frames));
            g_sync_stats = {};
            t0 = now;
        }
    }
    ComObj *dev = this_device9(c);
    if (dev) {
        // The limit in effect, under the energy policy (idle frames, saving mode).
        const int fps = recomp_env("PIN_CLOCK") ? 0 : mods_display_effective_fps();
        if (fps != dev->present_fps) {
            // A new limit takes effect with the next period; the deadline is not reset,
            // or every change (idle frames come and go) would let a frame through unpaced.
            static bool told = false;
            if (!told || mods_display_fps() != fps)
                LOGV("d3d9: completed-frame limit %d fps (0 = unlimited)", fps);
            if (!told)
                LOGW("d3d9: completed-frame limit %d fps (0 = unlimited)", fps);
            told = true;
            dev->present_fps = fps;
        }
        // Wait for the previous frame's deadline, then reserve the next
        // slot. Rendering work counts toward the period, the first frame is
        // immediate, and late frames never accumulate a catch-up backlog.
        // Yield the guest baton so audio/input workers continue to run.
        // Only presentation is paced; game clocks and simulation are intact.
        const uint32_t ms = dev->present_deadline.wait_ms(os_monotonic_ns());
        if (ms)
            guest_sleep_ms(ms);
        dev->present_deadline.begin(fps, os_monotonic_ns());
    }
    ComObj *bb = dev ? device_backbuffer(dev) : nullptr;
    if (bb && gpu_on()) {
        const uint64_t t_present = os_monotonic_ns();
        gpu_sync(bb);
        ddraw_external_present_begin();
        host_present(nullptr, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        host_d9_present(bb->id, bb->width, bb->height);
        ++g_vfetch.presents;
        ddraw_external_present_end();
        g_present_return_ns = os_monotonic_ns();
        g_present_call_ns = g_present_return_ns - t_present;
        if (dev && clear_discarded() && discard_swap()[dev->id]) {
            HostD9Target t{};
            t.color[0] = surface_ref(bb);
            const int32_t vp[4] = {0, 0, (int32_t)bb->width, (int32_t)bb->height};
            host_d9_clear(&t, vp, 0, nullptr, 1u, 0xff000000u, 1.0f, 0);
            gpu_drawn_into(bb);
        }
        // Test86 HDR: the core mod's pre-HUD scene marker (private render state 250) lasts one frame.
        if (dev) {
            D9Pipeline &p = d9_pipeline(dev->id);
            if (p.rs[250] || p.rs[251]) {
                p.rs[250] = p.rs[251] = 0;
                p.states_changed();
            }
        }
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *bbytes = resource_bytes(bb);
    if (bb && bbytes) {
        uint32_t lit = 0, n = bb->width * bb->height;
        for (uint32_t i = 0; i < n; i += 97) {
            uint32_t v;
            memcpy(&v, bbytes + (size_t)i * 4, 4);
            if (v & 0x00ffffffu)
                ++lit;
        }
        LOGW("d3d9: present back buffer %u (%ux%u, %u bpp), %u of %u sampled pixels lit, target "
             "now %u",
             bb->id, bb->width, bb->height, bb->bpp, lit, (n + 96) / 97, dev->render_target);
    }
    if (bb && bbytes && bb->bpp == 32) {
        ddraw_external_present_begin();
        host_present(bbytes, (int)bb->width, (int)bb->height, 32, nullptr, (int)bb->pitch);
        ddraw_external_present_end();
    }
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// Device resources
//
// A game creates textures, buffers and surfaces before it draws anything, and
// then writes its geometry and its images into them. None of that reaches a
// GPU yet, but the storage is real guest memory, so a Lock hands back a
// pointer the game can fill and the contents survive to be drawn later.
// ---------------------------------------------------------------------------
// A resource's contents live in host memory, as a driver's would: the guest
// heap is the game's, and a texture set would exhaust it. The guest sees the
// bytes only while the resource is locked, through a copy made on the first
// lock and written back on the last unlock.
static uint32_t resource_alloc(ComObj *o, uint32_t bytes) {
    if (!bytes)
        return 0;
    o->blob.assign(bytes, 0);
    o->pixels = 0;
    o->pixels_bytes = bytes;
    return 1;
}

// The bytes to read or write right now: the guest copy while locked.
static uint8_t *resource_bytes(ComObj *o) {
    if (!o)
        return nullptr;
    if (o->pixels)
        return gm_ptr(o->pixels);
    return o->blob.empty() ? nullptr : o->blob.data();
}

static void gpu_before_lock(ComObj *s);
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi);
static void gpu_forget(ComObj *o);
static void untrack_buffer(ComObj *o);

static uint32_t stage_lock(ComObj *o) {
    if (!o || o->blob.empty())
        return 0;
    if (o->lock_count == 0)
        gpu_before_lock(o);
    if (o->lock_count++ == 0) {
        o->pixels = heap_alloc((uint32_t)o->blob.size(), false, 16);
        if (!o->pixels) {
            o->lock_count = 0;
            return 0;
        }
        memcpy(gm_ptr(o->pixels), o->blob.data(), o->blob.size());
    }
    return o->pixels;
}

static void stage_unlock(ComObj *o) {
    if (!o || o->lock_count <= 0)
        return;
    if (--o->lock_count == 0 && o->pixels) {
        memcpy(o->blob.data(), gm_ptr(o->pixels), o->blob.size());
        heap_free(o->pixels);
        o->pixels = 0;
        d9_raster_invalidate(o->id);
        gpu_after_unlock(o, 0, (uint32_t)o->blob.size());
    }
}

static void readback_forget(ComObj *o);
static void d3d9_resource_destroy(ComObj *o) {
    readback_forget(o);
    untrack_buffer(o);
    if (o->pixels)
        heap_free(o->pixels);
    o->pixels = 0;
    o->lock_count = 0;
    o->pixels_bytes = 0;
    std::vector<uint8_t>().swap(o->blob);
    d9_raster_invalidate(o->id);
    gpu_forget(o);
    if (o->id == g_vfetch.vb)
        g_vfetch.vb = 0; // gone with its device: d3d9_vfetch_pool picks the next one
}

// Bytes per pixel for the formats this game creates. D3DFMT_A8R8G8B8 is 21,
// X8R8G8B8 22, R5G6B5 23, A8 28, D24S8 75, and the DXT block formats carry
// their four characters.
static uint32_t format_bytes(uint32_t fmt) {
    switch (fmt) {
    case 27:
    case 28:
    case 41:
    case 50:
    case 52: // R3G3B2, A8, P8, L8, A4L4
        return 1;
    case 23:
    case 24:
    case 25:
    case 26:
    case 29:
    case 30:
    case 40:
    case 51:
    case 60:
    case 61:
    case 70:
    case 73:
    case 80:
    case 81:
    case 111:
        return 2;
    case 36:
    case 113: // A16B16G16R16, A16B16G16R16F
        return 8;
    case 116: // A32B32G32R32F
        return 16;
    default:
        return 4;
    }
}

// The DXT formats, stored as blocks of 4x4 pixels.
static uint32_t dxt_block_bytes(uint32_t fmt) {
    switch (fmt) {
    case 0x31545844u:
        return 8; // DXT1
    case 0x32545844u:
    case 0x33545844u:
    case 0x34545844u:
    case 0x35545844u:
        return 16;
    default:
        return 0;
    }
}

static ComObj *make_surface(ComObj *dev, uint32_t w, uint32_t h, uint32_t fmt) {
    ComObj *s = com_new(K_D3D9SURFACE);
    if (!s)
        return nullptr;
    s->dev_d3d = dev ? dev->id : 0;
    s->width = w ? w : 1;
    s->height = h ? h : 1;
    s->rmask = fmt; // the D3DFORMAT, which a Direct3D 9 surface has no other field for
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // A compressed surface is rows of blocks; Pitch is the bytes in one.
        uint32_t bw = (s->width + 3) / 4, bh = (s->height + 3) / 4;
        s->bpp = 0;
        s->pitch = bw * block;
        resource_alloc(s, s->pitch * bh);
    } else {
        s->bpp = format_bytes(fmt) * 8;
        s->pitch = s->width * format_bytes(fmt);
        resource_alloc(s, s->pitch * s->height);
    }
    return s;
}

// ---- What the CPU renderer needs to know about surfaces and textures --------
bool d9_surface_info(uint32_t surface_id, D9SurfaceInfo *out) {
    ComObj *s = com_get(surface_id);
    if (!s || s->kind != K_D3D9SURFACE)
        return false;
    out->data = resource_bytes(s);
    out->size = s->blob.size();
    out->width = s->width;
    out->height = s->height;
    out->pitch = s->pitch;
    out->format = s->rmask;
    return true;
}

uint32_t d9_texture_level0(uint32_t texture_id) {
    ComObj *t = com_get(texture_id);
    if (!t)
        return 0;
    if (t->kind == K_D3D9SURFACE)
        return t->id;
    if (t->kind != K_D3D9TEXTURE)
        return 0;
    if (t->caps) // a cube: face zero, level zero, once it exists
        return t->surfaces.empty() ? 0 : t->surfaces[0];
    return t->back_obj;
}

// The surface behind a 2D texture's level, or null.
static ComObj *texture_level(ComObj *tex, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || tex->caps)
        return nullptr;
    if (level < tex->surfaces.size())
        return com_get(tex->surfaces[level]);
    return level == 0 ? com_get(tex->back_obj) : nullptr;
}

// ---- The GPU's copies ------------------------------------------------------
// With a GPU renderer running, every texture, surface and buffer has a mirror
// on the host under the object's id; a surface that belongs to a texture is a
// slot of its container's mirror. The shim's bytes stay the reference for what
// the game wrote, the GPU's for what was drawn: a slot the game unlocked is
// uploaded before the next draw that reads it, and a slot the GPU drew into is
// read back before the game locks it.
static const uint32_t USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2;

struct GpuMirror {
    bool defined = false;
    std::vector<uint8_t> cpu_dirty;      // by slot
    std::vector<uint8_t> gpu_newer;      // by slot
    uint32_t dirty_lo = 0, dirty_hi = 0; // buffers: the changed byte range
    // Buffers: a whole-buffer lock left the guest copy authoritative and the
    // host copy stale wherever the game wrote; each draw then syncs only the
    // bytes it reads (gpu_sync_buffer_range).
    bool lazy = false;
    // Every lock since the last upload was NOOVERWRITE (and none DISCARD);
    // the first upload, which creates the GPU buffer, never counts.
    bool noover_only = true;
    bool defined_uploaded = false;
    // Lazy buffers: pages a draw found (or made) equal on both copies,
    // stamped with lock_gen. The guest copy changes only under a lock, so a
    // stamped page is not compared again until the next Lock of the buffer -
    // whatever its flags: Heroes V rewrites vertices it drew earlier in the
    // frame under NOOVERWRITE. It saves the passes that redraw a range.
    std::vector<uint32_t> verified;
    uint32_t lock_gen = 1;
    // Lazy buffers whose guest copy is write-tracked (recomp_track): a page is
    // synced when the game wrote it, not after every Lock. `verified` is then
    // 1 for a page synced at least once.
    bool tracked = false;
    // Page protection (write_track_mode 3): for a page left writable this
    // frame, the lock generation and the byte range [lo, hi) within the page
    // last synced from it, packed gen << 32 | lo << 16 | hi. The game writes
    // only under a Lock, so a draw asking for bytes inside that range with no
    // Lock since (the shadow and reflection passes redrawing a mesh) skips
    // the compare; most compares were those, and equal.
    std::vector<uint64_t> open_sync;
};
static std::unordered_map<uint32_t, GpuMirror> &gpu_mirrors() {
    static auto *m = new std::unordered_map<uint32_t, GpuMirror>();
    return *m;
}
// Test75: a resource's mirror without the hash lookup (map nodes do not move; gpu_forget clears the
// cached pointer before it erases). RECOMP_AB_SWITCHES=mirrors (TEST ONLY) takes the lookup every time.
static GpuMirror &mirror_of(ComObj *o) {
    if (o->gpu_mirror && !recomp_ab::old(recomp_ab::MIRRORS))
        return *static_cast<GpuMirror *>(o->gpu_mirror);
    GpuMirror &m = gpu_mirrors()[o->id];
    o->gpu_mirror = &m;
    return m;
}
// The D3DUSAGE and D3DPOOL a texture or stand-alone surface was created with, as GetDesc
// reports them (a level reports its container's).
struct ResourceDesc {
    uint32_t usage = 0, pool = 0;
};
static std::unordered_map<uint32_t, ResourceDesc> &resource_desc() {
    static auto *m = new std::unordered_map<uint32_t, ResourceDesc>();
    return *m;
}
// D3DUSAGE of every texture and surface, by id.
static std::unordered_map<uint32_t, uint32_t> &resource_usage() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static bool gpu_on() {
    return host_d9_active() != 0;
}
static bool is_depth_format(uint32_t fmt) {
    return (fmt >= 70 && fmt <= 82) || fmt == 0x5a544e49u /* INTZ */ ||
           fmt == 0x34324644u /* DF24 */ || fmt == 0x36314644u /* DF16 */;
}

// Extra state for volume textures and their mip-level interfaces. The texture
// owns one reference to each mip. A mip with any external references holds one
// reference to the texture, released when its last external reference goes away.
static_assert(K_D3D9PSHADER < 64, "COM kind bitset capacity");
struct VolumeState {
    uint32_t depth = 0, pool = 0, lod = 0, priority = 0, lock_flags = 0;
};
static std::unordered_map<uint32_t, VolumeState> &volumes() {
    static auto *v = new std::unordered_map<uint32_t, VolumeState>();
    return *v;
}
static ComObj *volume_level(ComObj *o, uint32_t level) {
    return o && o->kind == K_D3D9VOLUMETEXTURE && level < o->surfaces.size()
               ? com_get(o->surfaces[level])
               : nullptr;
}
static void volume_destroy(ComObj *o) {
    for (uint32_t id : o->surfaces)
        com_destroy(com_get(id));
    o->surfaces.clear();
    volumes().erase(o->id);
    d3d9_resource_destroy(o);
}
// Test159: a 2D or cube texture takes its levels/faces with it, as a volume texture does (volume_destroy). Before,
// they were never destroyed: each texture left its level surfaces alive after its own final Release.
static void texture_destroy(ComObj *o) {
    for (uint32_t id : o->surfaces)
        com_destroy(com_get(id));
    o->surfaces.clear();
    o->back_obj = 0;
    d3d9_resource_destroy(o);
}
static void volume_addref(ComObj *o) {
    if (o && o->refs == 1)
        com_addref(com_get(o->front_obj));
    com_addref(o);
}
static void Vol_AddRef(X86 *c) {
    ComObj *o = com_this_arg(c);
    volume_addref(o);
    com_ret(c, o ? o->refs - 1 : 0);
}
static void Vol_Release(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o || o->refs <= 1) {
        com_ret(c, 0);
        return;
    }
    const uint32_t external = --o->refs - 1;
    if (!external)
        com_release(com_get(o->front_obj)); // may destroy this mip as well
    com_ret(c, external);
}
static void Vol_QueryInterface(X86 *c) {
    ComObj *o = com_this_arg(c);
    const bool internal_only = o && o->refs == 1;
    com_QueryInterface(c);
    if (internal_only && c->r[0] == S_OK)
        com_addref(com_get(o->front_obj));
}

// A surface's place in its mirror: the container's id and the slot, or the
// surface's own id and slot zero.
static uint32_t mirror_slot(ComObj *s, uint32_t *slot) {
    *slot = 0;
    if (!s)
        return 0;
    if ((s->kind == K_D3D9SURFACE || s->kind == K_D3D9VOLUME) && s->front_obj) {
        ComObj *tex = com_get(s->front_obj);
        if (tex && (tex->kind == K_D3D9TEXTURE || tex->kind == K_D3D9VOLUMETEXTURE)) {
            for (size_t i = 0; i < tex->surfaces.size(); ++i)
                if (tex->surfaces[i] == s->id) {
                    *slot = (uint32_t)i;
                    return tex->id;
                }
        }
    }
    return s->id;
}
static HostD9Surface surface_ref(ComObj *s) {
    HostD9Surface r = {0, 0, 0};
    if (!s)
        return r;
    uint32_t slot;
    r.id = mirror_slot(s, &slot);
    ComObj *owner = com_get(r.id);
    if (owner && owner->kind == K_D3D9TEXTURE && owner->caps) {
        r.face = slot % 6;
        r.level = slot / 6;
    } else {
        r.level = slot;
    }
    return r;
}
static uint32_t slot_count(ComObj *o) {
    if (o->kind == K_D3D9TEXTURE || o->kind == K_D3D9VOLUMETEXTURE)
        return (uint32_t)std::max<size_t>(o->surfaces.size(), 1);
    return 1;
}
static ComObj *slot_surface(ComObj *o, uint32_t slot) {
    if (o->kind != K_D3D9TEXTURE && o->kind != K_D3D9VOLUMETEXTURE)
        return o;
    return slot < o->surfaces.size() ? com_get(o->surfaces[slot]) : nullptr;
}

// Creates the mirror of a texture or stand-alone surface on first use.
static GpuMirror &gpu_define(ComObj *o) {
    GpuMirror &m = mirror_of(o);
    if (m.defined)
        return m;
    HostD9TextureDesc d{};
    d.id = o->id;
    d.format = o->rmask ? o->rmask : 22;
    d.width = o->width ? o->width : 1;
    d.height = o->height ? o->height : 1;
    uint32_t usage = resource_usage()[o->id];
    if (is_depth_format(d.format))
        usage |= USAGE_DEPTHSTENCIL;
    d.usage = (usage & USAGE_RENDERTARGET ? HOST_D9_USAGE_RENDERTARGET : 0) |
              (usage & USAGE_DEPTHSTENCIL ? HOST_D9_USAGE_DEPTH : 0);
    d.samples = o->kind == K_D3D9SURFACE ? o->samples : 0;
    if (o->kind == K_D3D9VOLUMETEXTURE) {
        d.kind = HOST_D9_TEX_VOLUME;
        d.depth = volumes().at(o->id).depth;
        d.levels = slot_count(o);
    } else if (o->kind == K_D3D9TEXTURE && o->caps) {
        d.kind = HOST_D9_TEX_CUBE;
        d.levels = o->pal_flags ? o->pal_flags : 1;
    } else {
        d.kind = HOST_D9_TEX_2D;
        d.levels = o->kind == K_D3D9TEXTURE ? slot_count(o) : 1;
    }
    host_d9_texture_define(&d);
    m.defined = true;
    uint32_t n = o->kind == K_D3D9TEXTURE && o->caps ? 6 * d.levels : slot_count(o);
    // A target's first contents are whatever is drawn; a texture's are what
    // the game wrote, so every slot starts out needing an upload.
    m.cpu_dirty.assign(n, d.usage ? 0 : 1);
    m.gpu_newer.assign(n, 0);
    return m;
}

// Uploads every slot the game changed since the GPU last saw it.
static void gpu_sync(ComObj *o) {
    if (!o)
        return;
    GpuMirror &m = gpu_define(o);
    for (uint32_t slot = 0; slot < m.cpu_dirty.size(); ++slot) {
        if (!m.cpu_dirty[slot])
            continue;
        m.cpu_dirty[slot] = 0;
        ComObj *s = slot_surface(o, slot);
        if (!s || s->blob.empty())
            continue;
        HostD9Surface r = surface_ref(s);
        if (o->kind == K_D3D9VOLUMETEXTURE) {
            const uint32_t depth = volumes().at(s->id).depth;
            for (uint32_t z = 0; z < depth; ++z)
                host_d9_texture_upload(o->id, z, slot, resource_bytes(s) + z * s->pitch * s->height,
                                       s->pitch);
            g_frame_stats.tex_upload_bytes += (uint64_t)depth * s->pitch * s->height;
        } else {
            host_d9_texture_upload_queued(o->id, r.face, r.level, resource_bytes(s), s->pitch,
                                          (uint32_t)s->blob.size());
            g_frame_stats.tex_upload_bytes += s->blob.size();
        }
        g_frame_stats.tex_uploads++;
    }
}

// The surface is about to be read or written by the game.
static void gpu_before_lock(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end() || slot >= it->second.gpu_newer.size() ||
        !it->second.gpu_newer[slot])
        return;
    it->second.gpu_newer[slot] = 0;
    if (s->bpp == 32 && !s->blob.empty()) {
        HostD9Surface r = surface_ref(s);
        host_d9_texture_read(id, r.face, r.level, s->blob.data(), s->pitch);
    }
}
// The game wrote the resource.
static void gpu_after_unlock(ComObj *o, uint32_t lo, uint32_t hi) {
    if (!o)
        return;
    if (o->kind == K_D3D9VB || o->kind == K_D3D9IB) {
        GpuMirror &m = mirror_of(o);
        if (m.dirty_hi <= m.dirty_lo) {
            m.dirty_lo = lo;
            m.dirty_hi = hi;
        } else {
            m.dirty_lo = std::min(m.dirty_lo, lo);
            m.dirty_hi = std::max(m.dirty_hi, hi);
        }
        m.defined = true;
        return;
    }
    uint32_t slot;
    uint32_t id = mirror_slot(o, &slot);
    auto it = gpu_mirrors().find(id);
    if (it == gpu_mirrors().end())
        return; // not mirrored yet: its first use uploads everything
    if (slot < it->second.cpu_dirty.size())
        it->second.cpu_dirty[slot] = 1;
}
static void gpu_drawn_into(ComObj *s) {
    if (!s)
        return;
    uint32_t slot;
    uint32_t id = mirror_slot(s, &slot);
    ComObj *owner = com_get(id);
    if (!owner)
        return;
    GpuMirror &m = gpu_define(owner);
    if (slot < m.gpu_newer.size()) {
        m.gpu_newer[slot] = 1;
        m.cpu_dirty[slot] = 0;
    }
}
static void gpu_sync_buffer(ComObj *b) {
    if (!b)
        return;
    GpuMirror &m = mirror_of(b);
    if (!m.defined) {
        m.defined = true;
        m.dirty_lo = 0;
        m.dirty_hi = (uint32_t)b->blob.size();
    }
    if (m.dirty_hi > m.dirty_lo && !b->blob.empty()) {
        uint32_t hi = std::min<uint32_t>(m.dirty_hi, (uint32_t)b->blob.size());
        uint32_t total = (uint32_t)b->blob.size();
        if (m.noover_only && m.defined_uploaded)
            total |= HOST_D9_UPLOAD_INPLACE;
        host_d9_buffer_upload(b->id, total, m.dirty_lo, b->blob.data() + m.dirty_lo,
                              hi - m.dirty_lo);
        g_frame_stats.uploads++;
        g_frame_stats.upload_bytes += hi - m.dirty_lo;
        m.defined_uploaded = true;
        m.noover_only = true;
    }
    m.dirty_lo = m.dirty_hi = 0;
}
// The bytes [lo, hi) of a buffer a draw is about to read. A lazy buffer
// (whole-buffer locks) compares that span of the guest copy with the host copy
// page by page and uploads what differs; anything else syncs as a whole.
// RECOMP_D3D9_WRITE_TRACK: unset or 1 tracks the guest's writes into lazy
// buffers (runtime/x86.h recomp_track); 0 compares every page a draw reads
// after each Lock instead; "verify" tracks but still compares the pages it
// calls unwritten, and logs any that differ - a writer the tracking misses.
// Heroes V locks its 26 MB batch buffer ~10000 times a second, and comparing
// after each Lock read up to 3.4 GB/s, 95% of it unchanged.
// 3 is page protection: the game's translated stores do not mark (game.toml
// [translate] write_track = "protect", runtime/x86.h), so the tracked pages
// are write-protected and the first store into one faults; the handler below
// marks the page and lets the store through. A build like that cannot use 1
// or 2 (nothing would mark), so only 0 overrides it. "protect" picks it in an
// ordinary build too, where the store marking just repeats it.
static int write_track_mode_init() {
    static const int mode = [] {
        const char *v = recomp_env("D3D9_WRITE_TRACK");
        if (v && v[0] == '0')
            return 0;
        if (recomp_write_track_protect || (v && !strcmp(v, "protect")))
            return D9_PAGE_PROTECT ? 3 : 0;
        if (!v || !*v)
            return 1;
        if (!strcmp(v, "verify"))
            return 2;
        return 1;
    }();
    return mode;
}
// Read in every draw's sync, so a plain global after the first call rather than
// a function-local static (a guard check per read, 0.7% of a ToE map frame).
static int g_write_track_mode = -1;
static inline int write_track_mode() {
    if (__builtin_expect(g_write_track_mode < 0, 0))
        g_write_track_mode = write_track_mode_init();
    return g_write_track_mode;
}

#if D9_PAGE_PROTECT
// Page protection (mode 3). The host page is the unit, whatever the kernel's
// size: 4 KiB on x86, 16 KiB on Apple silicon (macOS, iOS, Asahi Linux) and
// some other ARM Linux kernels, 64 KiB on some ARM servers. A tracked
// buffer's guest copy starts on one and is padded to whole ones
// (buffer_stage), so nothing else shares its pages. g_prot holds, per host
// page of the tracked span, 1 while this code has it write-protected; the
// fault handler acts only on those, so any other fault passes to the handler
// that was installed before (the runtime's crash report).
// RECOMP_D3D9_HOST_PAGE=N (TEST ONLY): N-byte pages, a power of two above the
// kernel's own, as a kernel with N-byte pages would have them. Protecting a
// run of smaller pages behaves the same, so the alignment, padding and fault
// arithmetic of 16 and 64 KiB pages run on a 4 KiB machine (d3d9_tests pages).
static uint32_t g_host_page = 0, g_host_page_shift = 0;
static inline uint32_t host_page() {
    if (__builtin_expect(!g_host_page, 0)) {
        uint32_t page = (uint32_t)sysconf(_SC_PAGESIZE);
        if (const char *v = recomp_env("D3D9_HOST_PAGE")) {
            const uint32_t want = (uint32_t)strtoul(v, nullptr, 0);
            if (want > page && want <= (1u << 20) && !(want & (want - 1)))
                page = want;
        }
        g_host_page_shift = (uint32_t)__builtin_ctz(page);
        g_host_page = page;
    }
    return g_host_page;
}
static uint8_t *g_prot = nullptr;      // per host page of [g_track_base, +g_track_len)
static uint8_t *g_prot_owned = nullptr; // 1 where the page is a tracked buffer's
static bool prot_page_tracked(uint32_t page) {
    return g_prot_owned && g_prot_owned[page];
}
static uint32_t g_prot_pages = 0;
static struct sigaction g_prev_segv, g_prev_bus;
// RECOMP_D3D9_SYNC_STATS: the host pages that faulted this frame, in order
// (sync_stats_faults reads them at Present).
static const uint32_t kFaultLog = 16384;
static uint32_t g_fault_log[kFaultLog], g_fault_log_n = 0;
static uintptr_t g_fault_pc[kFaultLog]; // the host instruction that stored
// Fewer faults and fewer compares (2026-10-21, perf-opt; on by default with
// the values in brackets, each off with 0, A/B in design notes perf-opt.md).
// Correctness never rests on them: a page left or made writable counts as
// written for every draw that reads it, and a page protected again faults on
// the next store, as any protected page does.
//   RECOMP_D3D9_RECLOSE=N  (2) a writable page that N draws in a row found
//                          equal to the host copy after a newer Lock is
//                          protected again at once, so the passes that redraw
//                          it (shadow, reflection) stop comparing it.
//   RECOMP_D3D9_STICKY=N   (2) a page the game wrote in N frames in a row stays
//                          writable at Present while it keeps changing (the
//                          rings rewritten every frame), so it does not fault
//                          again each frame.
//   RECOMP_D3D9_AHEAD=K    (4) a fault on the page after one opened this frame
//                          opens up to K more after it (as many as the run so
//                          far): the game appending across pages faults once
//                          per run rather than once per page.
struct HostPageState {
    uint32_t fault_frame; // the frame (g_prot_frame_no) of its last fault
    uint32_t open_frame;  // the frame it was last made writable in (fault or ahead)
    uint16_t run;         // consecutive pages made writable this frame, ending here
    uint8_t streak;       // frames in a row it was written (faulted or changed)
    uint8_t changed;      // a compare in it found a change this frame
    uint8_t equal;        // equal compares after a newer Lock since its last change
};
static HostPageState *g_hps = nullptr; // per host page, like g_prot
static uint32_t g_prot_frame_no = 1;
static int sync_switch(const char *name, int dflt) {
    const char *v = recomp_env(name);
    return v && *v ? atoi(v) : dflt;
}
// RECOMP_D3D9_SYNC_AB=1 (TEST ONLY): the three on only in the odd 10-second
// phases (the phase RECOMP_PHASE_LOG reports), so one run measures both.
static int g_reclose_set = -1, g_sticky_set = -1, g_ahead_set = -1;
static int g_reclose_n = 0, g_sticky_n = 0, g_ahead_k = 0;
static bool g_sync_ab = false;
// The value is a mask of which alternate: 1 reclose, 2 sticky, 4 ahead,
// 8 the prewrite hints (d3d9_prewrite); the others keep their setting.
static int g_sync_ab_mask = 0;
static bool g_prewrite_live = true;
static void sync_switches_init() {
    if (g_reclose_set >= 0)
        return;
    g_reclose_set = sync_switch("D3D9_RECLOSE", 2);
    g_sticky_set = sync_switch("D3D9_STICKY", 2);
    g_ahead_set = sync_switch("D3D9_AHEAD", 4);
    g_sync_ab_mask = sync_switch("D3D9_SYNC_AB", 0);
    g_sync_ab = g_sync_ab_mask != 0;
    g_reclose_n = g_reclose_set;
    g_sticky_n = g_sticky_set;
    g_ahead_k = g_ahead_set;
    LOGW("d3d9: lazy-buffer sync: reclose %d, sticky %d, ahead %d%s", g_reclose_n, g_sticky_n, g_ahead_k,
         g_sync_ab ? " (odd 10 s phases only)" : "");
}
static void sync_switches_phase(int phase) {
    if (!g_sync_ab)
        return;
    const int m = g_sync_ab_mask;
    g_reclose_n = phase || !(m & 1) ? g_reclose_set : 0;
    g_sticky_n = phase || !(m & 2) ? g_sticky_set : 0;
    g_ahead_k = phase || !(m & 4) ? g_ahead_set : 0;
    g_prewrite_live = phase || !(m & 8);
}
static void prot_set(uint32_t guest_lo, uint32_t len, bool ro) {
    const uint32_t hp = host_page();
    const uint32_t lo = guest_lo & ~(hp - 1), hi = (guest_lo + len + hp - 1) & ~(hp - 1);
    if (hi <= lo)
        return;
    mprotect(gm_ptr(lo), hi - lo, ro ? PROT_READ : PROT_READ | PROT_WRITE);
    g_frame_stats.prot_calls++;
    if (g_prot && lo >= g_track_base)
        for (uint32_t p = (lo - g_track_base) / hp; p < (hi - g_track_base) / hp && p < g_prot_pages; ++p)
            g_prot[p] = ro ? 1 : 0;
}
static void prot_fault(int sig, siginfo_t *si, void *uc) {
    const uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)gm_ptr(g_track_base);
    const uint32_t hp = host_page();
    if (g_prot && g_track_len && a >= base && a < base + g_track_len) {
        const uint32_t page = (uint32_t)((a - base) / hp);
        if (page < g_prot_pages && g_prot[page]) {
            g_prot[page] = 0;
            for (uint32_t k = 0; k < hp >> 12; ++k)
                g_track_pages[(page * hp >> 12) + k] = 1;
            uint32_t open = 1;
            if (g_hps) {
                HostPageState &st = g_hps[page];
                const uint32_t f = g_prot_frame_no;
                st.fault_frame = f;
                st.open_frame = f;
                st.run = page && g_hps[page - 1].open_frame == f ? (uint16_t)(g_hps[page - 1].run + 1) : 1;
                // Run-ahead: open the next pages with this one, one mprotect.
                if (g_ahead_k > 0 && st.run >= 2) {
                    const uint32_t want = std::min<uint32_t>((uint32_t)g_ahead_k, st.run);
                    while (open <= want && page + open < g_prot_pages && g_prot[page + open] &&
                           prot_page_tracked(page + open)) {
                        const uint32_t q = page + open;
                        g_prot[q] = 0;
                        for (uint32_t k = 0; k < hp >> 12; ++k)
                            g_track_pages[(q * hp >> 12) + k] = 1;
                        g_hps[q].open_frame = f;
                        g_hps[q].run = (uint16_t)(st.run + open);
                        ++open;
                    }
                    g_sync_stats.ahead_opened += open - 1;
                }
            }
            mprotect((void *)(base + (uintptr_t)page * hp), (size_t)hp * open, PROT_READ | PROT_WRITE);
            g_frame_stats.prot_faults++;
            if (g_fault_log_n < kFaultLog) {
#if defined(__APPLE__) && defined(__aarch64__)
                g_fault_pc[g_fault_log_n] = (uintptr_t)((ucontext_t *)uc)->uc_mcontext->__ss.__pc;
#elif defined(__linux__) && defined(__aarch64__)
                g_fault_pc[g_fault_log_n] = (uintptr_t)((ucontext_t *)uc)->uc_mcontext.pc;
#endif
                g_fault_log[g_fault_log_n++] = page;
            }
            return;
        }
    }
    // Not ours: put the previous handler back and let the instruction fault
    // again into it (or into the default action).
    const struct sigaction *prev = sig == SIGBUS ? &g_prev_bus : &g_prev_segv;
    if (prev->sa_flags & SA_SIGINFO) {
        prev->sa_sigaction(sig, si, uc);
        return;
    }
    sigaction(sig, prev, nullptr);
}
// Once a frame: every tracked page the game wrote (so made writable) is
// protected again. Its written mark stays until a draw syncs it.
// With RECOMP_D3D9_STICKY, a page written in enough frames in a row is left
// writable (it stays written for every draw, as it is now).
static int ab_phase10();
static void prot_frame() {
    if (write_track_mode() != 3 || !g_prot)
        return;
    sync_switches_phase(ab_phase10());
    const uint32_t hp = host_page();
    const uint32_t f = g_prot_frame_no;
    // The streaks: written this frame = faulted, or a compare found a change.
    if (g_hps)
        for (uint32_t p = 0; p < g_prot_pages; ++p) {
            HostPageState &st = g_hps[p];
            const bool active = st.fault_frame == f || st.changed;
            st.streak = active ? (uint8_t)std::min(st.streak + 1, 255) : 0;
            st.changed = 0;
        }
    auto keep = [&](uint32_t p) {
        return g_sticky_n > 0 && g_hps && g_hps[p].streak >= g_sticky_n;
    };
    for (uint32_t p = 0; p < g_prot_pages;) {
        if (g_prot[p] || !prot_page_tracked(p)) {
            ++p;
            continue;
        }
        if (keep(p)) {
            g_sync_stats.sticky_kept++;
            ++p;
            continue;
        }
        uint32_t q = p;
        while (q < g_prot_pages && !g_prot[q] && prot_page_tracked(q) && !keep(q))
            ++q;
        prot_set(g_track_base + p * hp, (q - p) * hp, true);
        p = q;
    }
    ++g_prot_frame_no;
}
static void prot_install() {
    static bool done = false;
    if (done)
        return;
    done = true;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = prot_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
    LOGW("d3d9: lazy buffers tracked by page protection (%u-byte pages)", host_page());
}
#else
static uint32_t host_page() {
    return 0x1000u;
}
static void prot_set(uint32_t, uint32_t, bool) {}
static void prot_install() {}
static void prot_frame() {}
#endif
static std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> &tracked_buffers() {
    static auto *m = new std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>>();
    return *m;
}
// RECOMP_D3D9_SYNC_STATS: this frame's faults, classified (see g_sync_stats).
static void sync_stats_faults() {
    g_sync_stats.frames++;
#if D9_PAGE_PROTECT
    static std::vector<uint8_t> last, cur;
    if (last.size() != g_prot_pages) {
        last.assign(g_prot_pages, 0);
        cur.assign(g_prot_pages, 0);
    }
    uint32_t big_lo = 0, big_hi = 0, big_size = 0;
    for (const auto &kv : tracked_buffers())
        if (kv.second.second > big_size) {
            big_size = kv.second.second;
            big_lo = kv.second.first;
            big_hi = kv.second.first + kv.second.second;
        }
    const uint32_t hp = host_page();
    // RECOMP_D3D9_FAULT_DUMP=N: every Nth frame's faults in order, as host
    // pages of the biggest buffer (others: B<buffer page>), to see the pattern.
    static const uint32_t dump_every = [] {
        const char *v = recomp_env("D3D9_FAULT_DUMP");
        return v ? (uint32_t)atoi(v) : 0u;
    }();
    static uint32_t dump_frame = 0;
    if (dump_every && ++dump_frame % dump_every == 0) {
        std::string line;
        char tmp[24];
        for (uint32_t i = 0; i < g_fault_log_n; ++i) {
            const uint32_t a = g_track_base + g_fault_log[i] * hp;
            if (a >= big_lo && a < big_hi)
                snprintf(tmp, sizeof tmp, " %u", (a - big_lo) / hp);
            else
                snprintf(tmp, sizeof tmp, " x%08x", a);
            line += tmp;
        }
        fprintf(stderr, "d3d9 faults (%u pages of %u):%s\n", g_fault_log_n, big_size / hp, line.c_str());
    }
    // RECOMP_D3D9_FAULT_WHO=N: every N frames, the functions whose stores
    // faulted most since the last report (the host symbol of the store).
    static const uint32_t who_every = [] {
        const char *v = recomp_env("D3D9_FAULT_WHO");
        return v ? (uint32_t)atoi(v) : 0u;
    }();
    if (who_every) {
        static std::unordered_map<uintptr_t, uint32_t> by_pc;
        static uint32_t who_frame = 0;
        for (uint32_t i = 0; i < g_fault_log_n; ++i)
            by_pc[g_fault_pc[i]]++;
        if (++who_frame % who_every == 0) {
            // As offsets in the image (atos -o <binary> -l 0x100000000 resolves
            // them; translated functions are not in the dynamic symbol table).
            std::map<std::string, uint32_t> by_fn;
            for (const auto &kv : by_pc) {
                Dl_info info{};
                char name[32];
                if (dladdr((void *)kv.first, &info) && info.dli_fbase)
                    snprintf(name, sizeof name, "0x%llx",
                             (unsigned long long)(kv.first - (uintptr_t)info.dli_fbase + 0x100000000ull));
                else
                    snprintf(name, sizeof name, "?");
                by_fn[name] += kv.second;
            }
            std::vector<std::pair<uint32_t, std::string>> top;
            for (const auto &kv : by_fn)
                top.push_back({kv.second, kv.first});
            std::sort(top.rbegin(), top.rend());
            std::string line;
            char tmp[160];
            for (size_t i = 0; i < top.size() && i < 12; ++i) {
                snprintf(tmp, sizeof tmp, " %s %.1f", top[i].second.c_str(), (double)top[i].first / who_every);
                line += tmp;
            }
            fprintf(stderr, "d3d9 fault writers per frame:%s\n", line.c_str());
            by_pc.clear();
        }
    }
    uint32_t prev = UINT32_MAX;
    for (uint32_t i = 0; i < g_fault_log_n; ++i) {
        const uint32_t page = g_fault_log[i];
        g_sync_stats.faults++;
        g_sync_stats.faults_seq += page == prev + 1;
        if (page < last.size()) {
            g_sync_stats.faults_repeat += last[page];
            cur[page] = 1;
        }
        const uint32_t a = g_track_base + page * hp;
        g_sync_stats.faults_big += a >= big_lo && a < big_hi;
        prev = page;
    }
    last.swap(cur);
    std::fill(cur.begin(), cur.end(), 0);
    g_fault_log_n = 0;
#endif
}
// The tracked range spans every tracked buffer's guest copy, from a page
// before the first (a store that starts there can reach into it); a new span
// counts every page as written once.
static void track_rebuild() {
    static std::vector<uint8_t> store;
    g_track_len = 0;
    auto &t = tracked_buffers();
    if (t.empty()) {
#if D9_PAGE_PROTECT
        g_prot = nullptr;
        g_prot_owned = nullptr;
        g_hps = nullptr;
        g_prot_pages = 0;
#endif
        g_track_base = 0;
        g_track_pages = nullptr;
        std::vector<uint8_t>().swap(store);
        return;
    }
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const auto &kv : t) {
        lo = std::min<uint64_t>(lo, kv.second.first);
        hi = std::max<uint64_t>(hi, (uint64_t)kv.second.first + kv.second.second);
    }
    const uint64_t hp = write_track_mode() == 3 ? host_page() : 0x1000;
    lo &= ~(hp - 1);
    lo = lo >= hp ? lo - hp : 0;
    hi = (hi + hp - 1) & ~(hp - 1);
    store.assign((size_t)((hi - lo) >> 12) + 2, 1);
#if D9_PAGE_PROTECT
    static std::vector<uint8_t> prot;
    if (write_track_mode() == 3) {
        // Everything counts as written once (store above), so nothing needs
        // the old protection state; the new span starts protected per buffer.
        prot.assign((size_t)((hi - lo) / hp) + 1, 0);
        static std::vector<uint8_t> owned;
        owned.assign(prot.size(), 0);
        for (const auto &kv : t)
            for (uint64_t a = kv.second.first & ~(hp - 1); a < (uint64_t)kv.second.first + kv.second.second; a += hp)
                owned[(size_t)((a - lo) / hp)] = 1;
        g_prot_pages = (uint32_t)prot.size();
        g_prot = prot.data();
        g_prot_owned = owned.data();
        static std::vector<HostPageState> hps;
        hps.assign(prot.size(), HostPageState{});
        g_hps = hps.data();
        sync_switches_init();
    }
#endif
    g_track_base = (uint32_t)lo;
    g_track_pages = store.data();
    g_track_len = (uint32_t)(hi - lo);
    if (write_track_mode() == 3) {
        prot_install();
        for (const auto &kv : t)
            prot_set(kv.second.first, kv.second.second, true);
    }
}
static void track_buffer(ComObj *o, GpuMirror &m) {
    if (m.tracked || !write_track_mode() || !o->pixels || (o->pixels & (host_page() - 1)))
        return;
    tracked_buffers()[o->id] = {o->pixels, (uint32_t)o->blob.size()};
    m.tracked = true;
    m.verified.clear();
    track_rebuild();
}
static void untrack_buffer(ComObj *o) {
    auto &t = tracked_buffers();
    auto it = t.find(o->id);
    if (it == t.end())
        return;
    // Writable again before the heap gets the block back.
    if (write_track_mode() == 3)
        prot_set(it->second.first, it->second.second, false);
    t.erase(it);
    track_rebuild();
}
// One run of changed bytes of a buffer whose GPU copy already exists. The
// first upload of a sync may make the renderer take a fresh copy (a DISCARD
// lock since the last one); the runs after it go into that copy in place.
static void gpu_upload_run(ComObj *b, GpuMirror &m, uint32_t lo, uint32_t hi) {
    uint32_t total = (uint32_t)b->blob.size();
    if (m.noover_only && m.defined_uploaded)
        total |= HOST_D9_UPLOAD_INPLACE;
    host_d9_buffer_upload(b->id, total, lo, b->blob.data() + lo, hi - lo);
    g_frame_stats.uploads++;
    g_frame_stats.upload_bytes += hi - lo;
    m.defined_uploaded = true;
    m.noover_only = true;
}
// Copies the 64-byte blocks of `src` that differ from `dst` and returns the
// changed byte range [*first, *last) (first == last: the two were equal). One
// pass over both copies where a compare and then a copy were two, and the
// range is what changed rather than the whole span compared - the per-draw
// sync of the pages the game is still appending to (2026-10-20: 113000 such
// copies a second on the zoomed-out ToE map).
static inline bool copy_changed(uint8_t *dst, const uint8_t *src, uint32_t n, uint32_t *first, uint32_t *last) {
    uint32_t f = n, l = 0, i = 0;
    for (; i + 64 <= n; i += 64) {
        uint64_t x = 0;
        for (int k = 0; k < 8; ++k) {
            uint64_t a, b;
            __builtin_memcpy(&a, src + i + k * 8, 8);
            __builtin_memcpy(&b, dst + i + k * 8, 8);
            x |= a ^ b;
        }
        if (x) {
            __builtin_memcpy(dst + i, src + i, 64);
            if (f == n)
                f = i;
            l = i + 64;
        }
    }
    if (i < n && memcmp(dst + i, src + i, n - i) != 0) {
        memcpy(dst + i, src + i, n - i);
        if (f == n)
            f = i;
        l = n;
    }
    *first = f == n ? 0 : f;
    *last = l;
    return l > 0;
}
static void gpu_sync_buffer_range_(ComObj *b, uint64_t lo, uint64_t hi);
static void gpu_sync_buffer_range(ComObj *b, uint64_t lo, uint64_t hi) {
    if (__builtin_expect(sync_stats_on(), 0)) {
        const uint64_t t = os_monotonic_ns();
        gpu_sync_buffer_range_(b, lo, hi);
        g_sync_stats.sync_ns += os_monotonic_ns() - t;
        return;
    }
    gpu_sync_buffer_range_(b, lo, hi);
}
static void gpu_sync_buffer_range_(ComObj *b, uint64_t lo, uint64_t hi) {
    if (!b)
        return;
    GpuMirror &m = mirror_of(b);
    if (!m.lazy || !b->pixels || !m.defined) {
        gpu_sync_buffer(b);
        return;
    }
    const uint32_t total = (uint32_t)b->blob.size();
    if (hi > total)
        hi = total;
    if (lo >= hi) {
        gpu_sync_buffer(b); // a pending ranged unlock still uploads
        return;
    }
    g_sync_stats.calls++;
    const uint8_t *guest = gm_ptr(b->pixels);
    uint8_t *host = b->blob.data();
    const uint32_t page = 0x1000u;
#if D9_PAGE_PROTECT
    // A page the game wrote this frame stays writable until Present protects
    // it again (prot_frame): its stores are not caught meanwhile, so it counts
    // as written for every draw that reads it until then. Protecting it again
    // here instead faulted once per draw on the page the game was appending to.
    const bool prot_mode = m.tracked && write_track_mode() == 3 && g_prot;
    host_page();
    const uint32_t hp_shift = g_host_page_shift;
#endif
    const bool verify = write_track_mode() == 2;
    if (m.verified.size() != (total + page - 1) / page)
        m.verified.assign((total + page - 1) / page, 0);
    // Changed pages go up as runs of contiguous bytes, not as one span from
    // the first to the last: a draw's vertex span reaches across the 26 MB
    // batch buffer, and the span between its changed pages was most of the
    // 30 MB a frame uploaded and copied into the command stream. Only when
    // the GPU copy exists and nothing else is pending (a ranged unlock).
    static const bool runs_on = [] {
        const char *v = recomp_env("D3D9_UPLOAD_RUNS");
        return !(v && v[0] == '0');
    }();
    const bool runs = runs_on && m.defined && m.dirty_hi <= m.dirty_lo;
    uint32_t run_lo = 0, run_hi = 0;
    for (uint32_t p = (uint32_t)lo & ~(page - 1); p < hi; p += page) {
        uint32_t n = std::min(page, total - p);
        uint32_t &stamp = m.verified[p / page];
        uint32_t s0 = p; // the bytes this draw syncs: the page, or less (below)
        bool open_page = false, reread = false;
        if (m.tracked) {
            uint8_t &written = g_track_pages[(b->pixels + p - g_track_base) >> 12];
#if D9_PAGE_PROTECT
            if (prot_mode && !g_prot[(b->pixels + p - g_track_base) >> hp_shift]) {
                written = 1; // writable: stores since this frame's fault are not seen
                open_page = true;
                // ...so every draw that reads it syncs, and only what it reads:
                // the index buffer the game appends to is one such page per
                // draw, and whole-page compares there were most of the sync.
                s0 = std::max(p, (uint32_t)lo);
                n = std::min(p + n, (uint32_t)hi) - s0;
                if (m.open_sync.size() != m.verified.size())
                    m.open_sync.assign(m.verified.size(), 0);
                uint64_t &os = m.open_sync[p / page];
                reread = os != 0 && (uint32_t)(os >> 32) != m.lock_gen;
                const uint32_t a = s0 - p, e = a + n;
                const uint32_t olo = (uint32_t)(os >> 16) & 0xffffu, ohi = (uint32_t)os & 0xffffu;
                if ((uint32_t)(os >> 32) == m.lock_gen && a >= olo && e <= ohi) {
                    g_sync_stats.open_skipped++;
                    continue; // synced since the last Lock
                }
                // Grow the range only while it stays contiguous; a disjoint
                // one replaces it, so the range never covers bytes not synced.
                const bool join = (uint32_t)(os >> 32) == m.lock_gen && a <= ohi && e >= olo;
                os = (uint64_t)m.lock_gen << 32 | (uint64_t)(join ? std::min(a, olo) : a) << 16 |
                     (join ? std::max(e, ohi) : e);
            }
#endif
            if (stamp && !written) {
                if (!verify || memcmp(guest + p, host + p, n) == 0)
                    continue;
                static int missed = 0;
                if (missed++ < 20)
                    fprintf(stderr, "d3d9: write tracking missed a write: buffer %u page %u (guest %08x)\n",
                            b->id, p / page, b->pixels + p);
            }
#if D9_PAGE_PROTECT
            if (!prot_mode || g_prot[(b->pixels + p - g_track_base) >> hp_shift])
#endif
                written = 0;
            stamp = 1;
        } else {
            if (stamp == m.lock_gen)
                continue;
            stamp = m.lock_gen;
        }
        g_sync_stats.pages_stamped++;
        // RECOMP_D3D9_SYNC_ONEPASS=0: compare, then copy the whole span (A/B).
        static const bool onepass = [] {
            const char *v = recomp_env("D3D9_SYNC_ONEPASS");
            return !(v && v[0] == '0');
        }();
        uint32_t cf = 0, cl = n;
        (open_page ? g_sync_stats.open_bytes : g_sync_stats.closed_bytes) += n;
        if (b->kind == K_D3D9IB)
            g_sync_stats.ib_bytes += n;
        if (!onepass ? memcmp(guest + s0, host + s0, n) == 0
                     : !copy_changed(host + s0, guest + s0, n, &cf, &cl)) {
            g_sync_stats.pages_equal++;
            g_sync_stats.open_equal += open_page;
#if D9_PAGE_PROTECT
            if (open_page && reread && g_reclose_n > 0 && g_hps) {
                const uint32_t h = (b->pixels + p - g_track_base) >> hp_shift;
                HostPageState &st = g_hps[h];
                if (++st.equal >= g_reclose_n && !(g_sticky_n > 0 && st.streak >= g_sticky_n)) {
                    // Every 4 KB page of it stays marked written, so the next
                    // draw that reads one compares it whole, once.
                    st.equal = 0;
                    prot_set(g_track_base + (h << hp_shift), 1u << hp_shift, true);
                    g_sync_stats.recloses++;
                }
            }
#endif
            continue;
        }
        g_sync_stats.pages_copied++;
        g_sync_stats.open_copied += open_page;
#if D9_PAGE_PROTECT
        if (open_page && g_hps) {
            HostPageState &st = g_hps[(b->pixels + p - g_track_base) >> hp_shift];
            st.changed = 1;
            st.equal = 0;
        }
#endif
        if (!onepass)
            memcpy(host + s0, guest + s0, n);
        n = cl - cf; // what changed: the host copy already matched the rest
        g_sync_stats.changed_bytes += n;
        s0 += cf;
        if (runs) {
            // A gap of up to 256 unchanged bytes joins the run: the host copy
            // there matches the GPU's, and one upload costs more than 256 bytes.
            if (run_hi > run_lo && s0 >= run_hi && s0 - run_hi <= (onepass ? 256u : 0u)) {
                run_hi = s0 + n;
            } else {
                if (run_hi > run_lo)
                    gpu_upload_run(b, m, run_lo, run_hi);
                run_lo = s0;
                run_hi = s0 + n;
            }
            continue;
        }
        if (m.dirty_hi <= m.dirty_lo) {
            m.dirty_lo = s0;
            m.dirty_hi = s0 + n;
        } else {
            m.dirty_lo = std::min(m.dirty_lo, s0);
            m.dirty_hi = std::max(m.dirty_hi, s0 + n);
        }
    }
    if (run_hi > run_lo)
        gpu_upload_run(b, m, run_lo, run_hi);
    gpu_sync_buffer(b);
    // RECOMP_D3D9_SYNC_VERIFY=1: the host copy (what the GPU's holds) must now
    // equal the guest's over every byte the draw reads.
    static const bool sync_verify = recomp_env("D3D9_SYNC_VERIFY") != nullptr;
    if (sync_verify && memcmp(guest + lo, host + lo, (size_t)(hi - lo)) != 0) {
        g_sync_stats.verify_bad++;
        static int told = 0;
        if (told++ < 20) {
            uint32_t at = (uint32_t)lo;
            while (at < hi && guest[at] == host[at])
                ++at;
            fprintf(stderr, "d3d9: sync verify: buffer %u (%s) [%llu, %llu) differs from byte %u (guest %08x)\n",
                    b->id, b->kind == K_D3D9IB ? "index" : "vertex", (unsigned long long)lo,
                    (unsigned long long)hi, at, b->pixels + at);
        }
    }
}
// The draw being sent, for the buffer ranges it reads: its first vertex and
// vertex count (indexed: MinVertexIndex + base and NumVertices), and its first
// index and index count. vcount 0: unknown, sync whole.
static uint32_t g_draw_vfirst = 0, g_draw_vcount = 0;
static int32_t g_draw_vbase = 0;
// Indexed draws: the index span, so a lazy vertex buffer syncs the vertices
// the indices really reach rather than the declared MinVertexIndex/NumVertices
// (games declare generously; Heroes V declares everything up to its append
// cursor). g_draw_ib null: not indexed.
static ComObj *g_draw_ib = nullptr;
static uint32_t g_draw_istart = 0, g_draw_icount = 0, g_draw_isize = 2;
static bool g_draw_iscanned = false;
static uint32_t g_draw_imin = 0, g_draw_imax = 0;
static bool draw_index_span(uint32_t *mn, uint32_t *mx) {
    if (!g_draw_ib || !g_draw_icount)
        return false;
    if (!g_draw_iscanned) {
        g_draw_iscanned = true;
        g_draw_imin = UINT32_MAX;
        g_draw_imax = 0;
        ComObj *ib = g_draw_ib;
        const uint64_t total = ib->blob.size();
        const uint64_t a = (uint64_t)g_draw_istart * g_draw_isize;
        const uint64_t b = a + (uint64_t)g_draw_icount * g_draw_isize;
        if (b > total)
            return false;
        // The guest copy is authoritative for a lazy buffer; otherwise the two agree.
        const uint8_t *src = ib->pixels ? gm_ptr(ib->pixels) : ib->blob.data();
        if (g_draw_isize == 4) {
            const uint32_t *ix = (const uint32_t *)(src + a);
            for (uint32_t i = 0; i < g_draw_icount; ++i) {
                g_draw_imin = std::min(g_draw_imin, ix[i]);
                g_draw_imax = std::max(g_draw_imax, ix[i]);
            }
        } else {
            const uint16_t *ix = (const uint16_t *)(src + a);
            uint16_t lo16 = 0xffff, hi16 = 0;
            for (uint32_t i = 0; i < g_draw_icount; ++i) {
                lo16 = std::min(lo16, ix[i]);
                hi16 = std::max(hi16, ix[i]);
            }
            g_draw_imin = lo16;
            g_draw_imax = hi16;
        }
    }
    if (g_draw_imin > g_draw_imax)
        return false;
    *mn = g_draw_imin;
    *mx = g_draw_imax;
    return true;
}
static uint32_t prim_vertex_count(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1: return count;          // point list
    case 2: return count * 2;      // line list
    case 3: return count + 1;      // line strip
    case 4: return count * 3;      // triangle list
    case 5: case 6: return count + 2; // strip, fan
    default: return 0;
    }
}
static void gpu_forget(ComObj *o) {
    o->gpu_mirror = nullptr;
    auto it = gpu_mirrors().find(o->id);
    if (it != gpu_mirrors().end()) {
        if (o->kind == K_D3D9VB || o->kind == K_D3D9IB)
            host_d9_buffer_drop(o->id);
        else if (it->second.defined)
            host_d9_texture_drop(o->id);
        gpu_mirrors().erase(it);
    }
    resource_usage().erase(o->id);
    resource_desc().erase(o->id);
}

// (this, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle)
void Dev_CreateVertexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *vb = com_new(K_D3D9VB);
    if (!vb || !resource_alloc(vb, bytes)) {
        if (vb)
            com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    vb->dev_d3d = dev->id;
    vb->buf_bytes = bytes;
    uint32_t view_ = com_view(vb, IF_D3DVERTEXBUFFER9);
    if (!view_) {
        com_release(vb);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle)
void Dev_CreateIndexBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t bytes = arg(c, 1), out = arg(c, 5);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *ib = com_new(K_D3D9IB);
    if (!ib || !resource_alloc(ib, bytes)) {
        if (ib)
            com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    ib->dev_d3d = dev->id;
    ib->buf_bytes = bytes;
    ib->rmask = arg(c, 3) == 102 ? 4 : 2; // D3DFMT_INDEX32 or D3DFMT_INDEX16
    uint32_t view_ = com_view(ib, IF_D3DINDEXBUFFER9);
    if (!view_) {
        com_release(ib);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle)
void Dev_CreateTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, 5), out = arg(c, 7);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = w;
    tex->height = h;
    tex->bpp = format_bytes(fmt) * 8;
    // Every level gets its own surface. Levels = 0 asks for the full chain;
    // a game uploads each level with its own LockRect.
    uint32_t levels = arg(c, 3);
    if (!levels) {
        levels = 1;
        for (uint32_t d = std::max(w, h); d > 1; d >>= 1)
            ++levels;
    }
    tex->rmask = fmt;
    uint32_t usage = arg(c, 4);
    resource_usage()[tex->id] = usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    resource_desc()[tex->id] = {usage, arg(c, 6)};
    for (uint32_t l = 0; l < levels && l < 16; ++l) {
        ComObj *s = make_surface(dev, std::max(w >> l, 1u), std::max(h >> l, 1u), fmt);
        if (!s)
            break;
        s->front_obj = tex->id;
        tex->surfaces.push_back(s->id);
    }
    tex->back_obj = tex->surfaces.empty() ? 0 : tex->surfaces[0];
    static uint32_t made = 0, unusual = 0;
    const bool common = fmt == 21 || dxt_block_bytes(fmt);
    if (++made <= 48 || (!common && ++unusual <= 32)) // what a game creates, for diagnosis
        LOGW("d3d9: texture %u: %ux%u, %u levels, usage 0x%x, format 0x%x, pool %u", tex->id, w, h,
             levels, usage, fmt, arg(c, 6));
    uint32_t view_ = com_view(tex, IF_D3DTEXTURE9);
    if (!view_) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle)
void Dev_CreateCubeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t edge = arg(c, 1), fmt = arg(c, 4), out = arg(c, 6);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    tex->dev_d3d = dev->id;
    tex->width = tex->height = edge;
    tex->bpp = format_bytes(fmt) * 8;
    tex->rmask = fmt;
    tex->caps = 1;                              // a cube map
    tex->pal_flags = arg(c, 2) ? arg(c, 2) : 1; // levels, reusing a field cubes do not use
    resource_usage()[tex->id] = arg(c, 3) & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL);
    resource_desc()[tex->id] = {arg(c, 3), arg(c, 5)};
    uint32_t view = com_view(tex, IF_D3DCUBETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    LOGV("d3d9: CreateCubeTexture %u -> %08x into %08x", edge, view, out);
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

static void create_surface_method(X86 *c, uint32_t out_index, uint32_t fmt_index, uint32_t usage) {
    ComObj *dev = this_device9(c);
    uint32_t w = arg(c, 1), h = arg(c, 2), fmt = arg(c, fmt_index), out = arg(c, out_index);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = make_surface(dev, w, h, fmt);
    if (!s) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    // CreateRenderTarget and CreateDepthStencilSurface: MultiSample, MultisampleQuality.
    if (out_index == 7)
        s->samples = multisample_count(arg(c, 4), arg(c, 5));
    resource_usage()[s->id] = usage;
    // CreateOffscreenPlainSurface names its pool; targets and depth buffers are D3DPOOL_DEFAULT.
    resource_desc()[s->id] = {usage, out_index == 5 ? arg(c, 4) : 0u};
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Discard,
//  ppSurface, pSharedHandle)
void Dev_CreateDepthStencilSurface(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_DEPTHSTENCIL);
}
// (this, Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
//  ppSurface, pSharedHandle)
void Dev_CreateRenderTarget(X86 *c) {
    create_surface_method(c, 7, 3, USAGE_RENDERTARGET);
}
// (this, Width, Height, Format, Pool, ppSurface, pSharedHandle)
void Dev_CreateOffscreenPlainSurface(X86 *c) {
    create_surface_method(c, 5, 3, 0);
}

// The device's own back buffer and depth buffer, made on first ask and kept.
// The back buffer lives in its own field. It used to share render_target,
// which SetRenderTarget overwrites, so after the game switched targets the
// "back buffer" was whatever it had last drawn into.
static ComObj *device_backbuffer(ComObj *dev) {
    ComObj *s = com_get(dev->palette_obj);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 22);
        dev->palette_obj = s ? s->id : 0;
        if (s)
            s->samples = dev->samples;
        if (s)
            resource_usage()[s->id] = USAGE_RENDERTARGET;
        if (s && !com_get(dev->render_target))
            dev->render_target = s->id;
    }
    return s;
}

// (this, pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter): a
// nearest-neighbour copy between 32-bit surfaces, rectangles honoured. This is
// how a game moves an off-screen target onto the back buffer.
void Dev_StretchRect(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 3));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        ComObj *sowner = com_get(mirror_slot(src, &slot));
        ComObj *downer = com_get(mirror_slot(dst, &slot));
        if (sowner)
            gpu_sync(sowner);
        if (downer)
            gpu_sync(downer);
        int32_t sr[4] = {0, 0, (int32_t)src->width, (int32_t)src->height};
        int32_t dr[4] = {0, 0, (int32_t)dst->width, (int32_t)dst->height};
        if (arg(c, 2))
            for (int i = 0; i < 4; ++i)
                sr[i] = (int32_t)rd32(arg(c, 2) + 4u * (uint32_t)i);
        if (arg(c, 4))
            for (int i = 0; i < 4; ++i)
                dr[i] = (int32_t)rd32(arg(c, 4) + 4u * (uint32_t)i);
        host_d9_stretch(surface_ref(src), sr, surface_ref(dst), dr, arg(c, 5));
        gpu_drawn_into(dst);
        com_ret(c, D3D_OK9);
        return;
    }
    uint8_t *sbytes = resource_bytes(src), *dbytes = resource_bytes(dst);
    if (!sbytes || !dbytes || src->bpp != 32 || dst->bpp != 32) {
        com_ret(c, D3D_OK9);
        return;
    }
    auto rect = [](uint32_t p, ComObj *s, int32_t r[4]) {
        r[0] = 0;
        r[1] = 0;
        r[2] = (int32_t)s->width;
        r[3] = (int32_t)s->height;
        if (p)
            for (int i = 0; i < 4; ++i)
                r[i] = (int32_t)rd32(p + 4u * (uint32_t)i);
        r[0] = std::max(r[0], 0);
        r[1] = std::max(r[1], 0);
        r[2] = std::min(r[2], (int32_t)s->width);
        r[3] = std::min(r[3], (int32_t)s->height);
    };
    ComObj *dev_ = com_get(dst->dev_d3d);
    static uint32_t g_stretches = 0;
    if (++g_stretches <= 24)
        LOGW("d3d9: StretchRect %u (%ux%u, %u bpp) -> %u (%ux%u, %u bpp)%s", src->id, src->width,
             src->height, src->bpp, dst->id, dst->width, dst->height, dst->bpp,
             (dev_ && dst->id == dev_->palette_obj) ? ", onto the back buffer" : "");
    int32_t sr[4], dr[4];
    rect(arg(c, 2), src, sr);
    rect(arg(c, 4), dst, dr);
    int32_t sw = sr[2] - sr[0], sh = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        com_ret(c, D3D_OK9);
        return;
    }
    for (int32_t y = 0; y < dh; ++y) {
        int32_t sy = sr[1] + y * sh / dh;
        uint8_t *drow = dbytes + (size_t)(dr[1] + y) * dst->pitch;
        const uint8_t *srow = sbytes + (size_t)sy * src->pitch;
        for (int32_t x = 0; x < dw; ++x)
            memcpy(drow + 4 * (dr[0] + x), srow + 4 * (sr[0] + x * sw / dw), 4);
    }
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}
// Test157c (audit after the rain incident): depth-stencil ownership as Direct3D 9. The device keeps two
// references of its own: one to its automatic depth buffer (CreateDevice/Reset with EnableAutoDepthStencil,
// tracked here per device) and one to the bound depth-stencil surface (dev->zbuffer_obj; SetDepthStencilSurface
// AddRefs the new one and releases the old one). Before, Set took no reference, so a surface the game released
// while bound was destroyed at once, and Reset released the bound surface even when it was the game's own,
// destroying a surface the game still held.
static std::unordered_map<uint32_t, uint32_t> &auto_depth_surfaces() {
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
// Test159 (20-reset measurement, 4 Oct): a level or face of a 2D/cube texture gets container-coupled lifetime that
// keeps Direct3D 9's documented observable behaviour (a getter returns a reference the caller releases; the texture
// does not outlive its final release). Not checked against Windows' internal counting. The surface object holds one internal reference (its container's); while anything outside holds
// it, it also holds one reference to the container, taken with the first outside reference and given back with the
// last. Before, GetSurfaceLevel / GetCubeMapSurface / GetRenderTarget took a texture reference on every call and
// releasing the surface never gave it back, so every texture whose level was ever asked for (render targets, the
// game's and the mods') outlived its own release: each renderer Reset left its old targets behind.
static ComObj *surface_container(ComObj *s) {
    if (!s || s->kind != K_D3D9SURFACE || !s->front_obj)
        return nullptr;
    ComObj *t = com_get(s->front_obj);
    if (!t || t->kind != K_D3D9TEXTURE)
        return nullptr;
    for (uint32_t id : t->surfaces)
        if (id == s->id)
            return t;
    return nullptr;
}
static void surface_addref(ComObj *s) {
    if (!s)
        return;
    if (ComObj *t = surface_container(s); t && s->refs == 1)
        com_addref(t);
    com_addref(s);
}
static int32_t surface_release(ComObj *s) {
    if (!s)
        return 0;
    ComObj *t = surface_container(s);
    if (!t)
        return com_release(s);
    if (s->refs <= 1)
        return 0; // only the container's own reference: nothing outside to drop
    const int32_t outside = --s->refs - 1;
    if (!outside)
        com_release(t); // may destroy this level together with its container
    return outside;
}
static void bind_depth_stencil(ComObj *dev, ComObj *s) {
    surface_addref(s);
    ComObj *old = com_get(dev->zbuffer_obj);
    dev->zbuffer_obj = s ? s->id : 0;
    if (old)
        surface_release(old);
}
// Test159: the device holds the surfaces bound as render targets, as Direct3D 9 does (SetRenderTarget AddRefs the
// new one and releases the old one), so a target the game releases while it is bound stays valid until unbound.
static void bind_color_target(ComObj *dev, uint32_t index, ComObj *s) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    surface_addref(s);
    ComObj *old = com_get(pl.color_target[index]);
    pl.color_target[index] = s ? s->id : 0;
    if (index == 0)
        dev->render_target = pl.color_target[0];
    if (old)
        surface_release(old);
}
// The device's automatic depth buffer, made on first use, bound.
static ComObj *device_depthbuffer(ComObj *dev) {
    auto &autos = auto_depth_surfaces();
    ComObj *s = com_get(autos[dev->id]);
    if (!s) {
        s = make_surface(dev, dev->width ? dev->width : 640, dev->height ? dev->height : 480, 75);
        if (!s) {
            autos.erase(dev->id);
            return nullptr;
        }
        s->samples = dev->samples;
        resource_usage()[s->id] = USAGE_DEPTHSTENCIL;
        autos[dev->id] = s->id; // the device's own reference (make_surface's)
    }
    if (dev->zbuffer_obj != s->id)
        bind_depth_stencil(dev, s);
    return s;
}

// Reset: drops the device's own back buffer, its binding of the depth-stencil surface and its automatic depth
// buffer; the game's references keep those objects alive, and the device makes new ones on next use.
static void device_forget_buffers(ComObj *dev) {
    if (ComObj *s = com_get(dev->palette_obj))
        com_release(s);
    dev->palette_obj = 0;
    bind_depth_stencil(dev, nullptr);
    auto it = auto_depth_surfaces().find(dev->id);
    if (it != auto_depth_surfaces().end()) {
        if (ComObj *a = com_get(it->second))
            com_release(a);
        auto_depth_surfaces().erase(it);
    }
}

// (this, iSwapChain, iBackBuffer, Type, ppBackBuffer)
void Dev_GetBackBuffer(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 4);
    ComObj *s = dev ? device_backbuffer(dev) : nullptr;
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    surface_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// Test155 (Metal allocation trace): GetDepthStencilSurface returns the depth-stencil
// surface that is set (SetDepthStencilSurface; the automatic one from CreateDevice/Reset until the game sets
// another). With none set, Direct3D 9 returns D3DERR_NOTFOUND. This used to make a new device-sized depth buffer
// instead (device_depthbuffer), which the game never released: at rain onset that was one 3840x2160 4x-MSAA
// D24S8 surface (~204 MB) per frame, ~13 GB/s, until macOS killed the game.
void Dev_GetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_get(dev->zbuffer_obj);
    if (!s) {
        static uint64_t none = 0;
        ++none;
        static const bool notfound_log = recomp_env("D3D9_STATS") != nullptr; // expected in rain: debug-only (review 4 Oct)
        if (notfound_log && (none <= 8 || (none & (none - 1)) == 0))
            LOGW("d3d9: GetDepthStencilSurface with no depth-stencil surface set: D3DERR_NOTFOUND, as Direct3D 9 "
                 "(caller %08x, %llu so far)",
                 rd32(c->r[R_ESP]), (unsigned long long)none);
        wr32(out, 0);
        com_ret(c, D3DERR_NOTFOUND9);
        return;
    }
    surface_addref(s);
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 2));
    uint32_t index = arg(c, 1);
    if (!dev || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (index == 0 && !(s && s->kind == K_D3D9SURFACE)) {
        com_ret(c, D3DERR_INVALIDCALL); // target 0 cannot be unset
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    bind_color_target(dev, index, (s && s->kind == K_D3D9SURFACE) ? s : nullptr); // Test159: held while bound
    if (index == 0) {
        // Setting render target 0 resets the viewport and scissor to the target.
        pl.viewport_set = false;
        pl.scissor[0] = pl.scissor[1] = 0;
        pl.scissor[2] = (int32_t)s->width;
        pl.scissor[3] = (int32_t)s->height;
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetDepthStencilSurface(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *s = com_this(arg(c, 1));
    if (dev)
        bind_depth_stencil(dev, (s && s->kind == K_D3D9SURFACE) ? s : nullptr); // the device holds the bound one
    com_ret(c, D3D_OK9);
}

// (this, pVertexElements, ppDecl): the element array is kept verbatim, since
// only the eventual renderer can interpret it.
void Dev_CreateVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t elems = arg(c, 1), out = arg(c, 2);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_new(K_D3D9DECL);
    if (!d) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    d->dev_d3d = dev->id;
    // D3DVERTEXELEMENT9 is 8 bytes and the array ends with D3DDECL_END, whose
    // Stream is 0xff. Copy until that, so the declaration survives the call.
    for (uint32_t i = 0; i < 64 && elems; ++i) {
        uint32_t e = elems + i * 8u;
        uint16_t stream = rd16(e);
        for (int b = 0; b < 8; ++b)
            d->blob.push_back(gm_ptr(e)[b]);
        if (stream == 0xff)
            break;
    }
    uint32_t view_ = com_view(d, IF_D3DVERTEXDECL9);
    if (!view_) {
        com_release(d);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

void Dev_SetVertexDeclaration(X86 *c) {
    g_frame_stats.decl_sets++;
    ComObj *dev = this_device9(c);
    ComObj *d = com_this(arg(c, 1));
    if (dev) {
        dev->current_viewport = (d && d->kind == K_D3D9DECL) ? d->id : 0;
        d9_pipeline(dev->id).declaration = dev->current_viewport;
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, ppQuery). A query with no GPU behind it answers immediately.
void Dev_CreateQuery(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t type = arg(c, 1), out = arg(c, 2);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (!out) {
        com_ret(c, D3D_OK9); // a null out pointer only asks whether the type exists
        return;
    }
    ComObj *q = com_new(K_D3D9QUERY);
    if (!q) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    q->dev_d3d = dev ? dev->id : 0;
    q->dev_type = type;
    uint32_t view_ = com_view(q, IF_D3DQUERY9);
    if (!view_) {
        com_release(q);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, pStreamData, OffsetInBytes, Stride)
void Dev_SetStreamSource(X86 *c) {
    g_frame_stats.stream_sets++;
    ComObj *dev = this_device9(c);
    ComObj *vb = com_this(arg(c, 2));
    uint32_t stream = arg(c, 1);
    if (dev && stream < 8) {
        D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
        st.vb = (vb && vb->kind == K_D3D9VB) ? vb->id : 0;
        st.offset = arg(c, 3);
        st.stride = arg(c, 4);
    }
    if (dev && stream == 0 && vb && vb->kind == K_D3D9VB) {
        dev->back_obj = vb->id;
        dev->lock_off = arg(c, 3);
        dev->rate = arg(c, 4); // the stride, reusing a spare field
    }
    com_ret(c, D3D_OK9);
}

void Dev_SetIndices(X86 *c) {
    ComObj *dev = this_device9(c);
    ComObj *ib = com_this(arg(c, 1));
    if (dev) {
        dev->front_obj = (ib && ib->kind == K_D3D9IB) ? ib->id : 0;
        d9_pipeline(dev->id).index_buffer = dev->front_obj;
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, pTexture): the texture behind a sampler, for the renderer.
void Dev_SetTexture(X86 *c) {
    g_frame_stats.tex_sets++;
    ComObj *dev = this_device9(c);
    ComObj *tex = com_this(arg(c, 2));
    uint32_t stage = arg(c, 1);
    if (!dev || stage >= 16 || (arg(c, 2) && !tex) ||
        (tex && tex->kind == K_D3D9VOLUMETEXTURE && volumes().at(tex->id).pool > 1)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *old = com_get(d9_pipeline(dev->id).sampler_tex[stage]);
    if (tex && tex->kind == K_D3D9VOLUMETEXTURE)
        com_addref(tex);
    dev->texture_handle = tex ? tex->id : 0;
    d9_pipeline(dev->id).sampler_tex[stage] = tex ? tex->id : 0;
    if (old && old->kind == K_D3D9VOLUMETEXTURE)
        com_release(old);
    com_ret(c, D3D_OK9);
}
void Dev_GetTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stage = arg(c, 1), out = arg(c, 2);
    if (!dev || stage >= 16 || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *o = com_get(d9_pipeline(dev->id).sampler_tex[stage]);
    wr32(out, o ? o->identity : 0);
    com_addref(o);
    com_ret(c, D3D_OK9);
}
static void device_volume_destroy(ComObj *dev) {
    for (uint32_t &id : d9_pipeline(dev->id).sampler_tex) {
        ComObj *o = com_get(id);
        id = 0;
        if (o && o->kind == K_D3D9VOLUMETEXTURE)
            com_release(o);
    }
}

// The draws. Nothing is rasterized yet, so each one reports what it was asked
// to draw: the geometry a renderer would need is all in these arguments, and
// a run that names them is the specification for the work that comes next.
// The first few are reported in full and then every 256th, because a game at
// speed issues thousands.
static uint32_t g_draws = 0;

static const char *prim_name(uint32_t type) {
    switch (type) {
    case 1:
        return "point list";
    case 2:
        return "line list";
    case 3:
        return "line strip";
    case 4:
        return "triangle list";
    case 5:
        return "triangle strip";
    case 6:
        return "triangle fan";
    default:
        return "unknown primitive";
    }
}

// The first few draws, then (with verbose logging) every 256th: a race makes
// thousands a frame, and writing even one in 256 cost a percent of the frame.
static bool draw_worth_reporting() {
    ++g_draws;
    return g_draws <= 8 || ((g_draws % 256) == 0 && log_level() >= 2);
}

static uint32_t primitive_vertices(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1:
        return count; // points
    case 2:
        return count * 2; // lines
    case 3:
        return count + 1; // line strip
    case 4:
        return count * 3; // triangles
    case 5:
    case 6:
        return count + 2;
    default:
        return 0;
    }
}

// The render target and depth buffer a draw or clear goes to, mirrored.
static HostD9Target gpu_target(ComObj *dev) {
    HostD9Target t{};
    D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (i == 0 && !s)
            s = com_get(dev->render_target) ? com_get(dev->render_target) : device_backbuffer(dev);
        if (!s)
            continue;
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(s, &slot));
        if (owner)
            gpu_sync(owner);
        t.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj)) {
        uint32_t slot;
        ComObj *owner = com_get(mirror_slot(z, &slot));
        if (owner)
            gpu_sync(owner);
        t.depth = surface_ref(z);
    }
    return t;
}
static void gpu_target_drawn(ComObj *dev, bool color, bool depth) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (color)
        for (int i = 0; i < 4; ++i) {
            ComObj *s = com_get(pl.color_target[i]);
            if (i == 0 && !s)
                s = com_get(dev->render_target);
            gpu_drawn_into(s);
        }
    if (depth)
        gpu_drawn_into(com_get(dev->zbuffer_obj));
}
static void target_viewport(ComObj *dev, const HostD9Target &t, int32_t vp[4]) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    if (pl.viewport_set) {
        for (int i = 0; i < 4; ++i)
            vp[i] = (int32_t)pl.viewport[i];
        return;
    }
    ComObj *rt =
        com_get(pl.color_target[0]) ? com_get(pl.color_target[0]) : com_get(dev->render_target);
    (void)t;
    vp[0] = vp[1] = 0;
    vp[2] = rt ? (int32_t)rt->width : (int32_t)dev->width;
    vp[3] = rt ? (int32_t)rt->height : (int32_t)dev->height;
}

// Only observes calls already routed to the GPU. Does not infer guest visibility
// decisions or model ownership. Correlate serial + target + resources within one run.
static void observe_draw(X86 *c, ComObj *dev, HostD9Draw &d, uint32_t api) {
    if (!host_d9_admission_enabled())
        return;
    static uint64_t serial = 0; // guest execution is serialized by the scheduler baton
    d.admission.serial = ++serial;
    host_d9_admission_entry(serial);
    d.admission.api = api;
    d.admission.caller = rd32(c->r[R_ESP]);
    d.admission.device = dev->identity;
    if (api >= 83) {
        d.admission.inline_vertices = arg(c, api == 83 ? 3 : 7);
        d.admission.inline_indices = api == 84 ? arg(c, 5) : 0;
        d.stream[0].stride = arg(c, api == 83 ? 4 : 8);
        return; // UP calls do not use the device's bound buffers
    }
    const D9Pipeline &pl = d9_pipeline(dev->id);
    for (int i = 0; i < 9; ++i)
        if (ComObj *o = com_get(i < 8 ? pl.stream[i].vb : pl.index_buffer))
            d.admission.resource_identity[i] = o->identity;
}
static void reject_draw(ComObj *dev, const HostD9Draw &attempt, uint32_t reason) {
    if (!attempt.admission.serial)
        return;
    // Metadata only: never gpu_target()/gpu_sync(), never touch invalid UP pointers.
    HostD9Draw d{};
    d.admission = attempt.admission;
    d.admission.reason = reason;
    d.primitive = attempt.primitive;
    d.primitive_count = attempt.primitive_count;
    d.start = attempt.start;
    d.base_vertex = attempt.base_vertex;
    const D9Pipeline &pl = d9_pipeline(dev->id);
    d.label = pl.label;
    d.decl_id = pl.declaration;
    d.vs_key = pl.vs_key;
    d.ps_key = pl.ps_key;
    for (int i = 0; i < 8; ++i)
        d.stream[i] = {pl.stream[i].vb, pl.stream[i].offset, pl.stream[i].stride};
    d.index_buffer = pl.index_buffer;
    if (d.admission.api >= 83) {
        memset(d.stream, 0, sizeof d.stream);
        d.stream[0].stride = attempt.stream[0].stride;
        d.index_buffer = 0;
    }
    for (int i = 0; i < 4; ++i) {
        ComObj *s = com_get(pl.color_target[i]);
        if (!s && i == 0)
            s = com_get(dev->render_target);
        if (s)
            d.target.color[i] = surface_ref(s);
    }
    if (ComObj *z = com_get(dev->zbuffer_obj))
        d.target.depth = surface_ref(z);
    target_viewport(dev, d.target, d.viewport);
    host_d9_rejected_draw(&d);
}

// The fixed-function programs' constant registers for the draw being built. The host copies a
// draw's constants when it is queued, so one set serves every draw.
static float g_ffp_vconst[d9ffp::VS_CONSTANTS][4], g_ffp_pconst[d9ffp::PS_CONSTANTS][4];

// What each of the first eight samplers has bound, as the fixed-function cascade declares it.
static d9ffp::Textures ffp_textures(const D9Pipeline &pl) {
    d9ffp::Textures t;
    for (int s = 0; s < 8; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex)
            continue;
        if (tex->kind == K_D3D9VOLUMETEXTURE)
            t.kind[s] = d9sh::S_VOLUME;
        else if (tex->kind == K_D3D9TEXTURE)
            t.kind[s] = tex->caps ? d9sh::S_CUBE : d9sh::S_2D;
        else if (tex->kind == K_D3D9SURFACE)
            t.kind[s] = d9sh::S_2D;
    }
    return t;
}

// Vertex fetch (runtime/native_seam.h).
extern "C" int d3d9_vfetch_supported(void) {
    return gpu_on() ? host_d9_vfetch_supported() : 0;
}
extern "C" uint32_t d3d9_vfetch_pool(uint32_t addr, uint32_t stride, const char *source,
                                     int check) {
    if (!d3d9_vfetch_supported() || !stride || !source || !g_com_objects)
        return 0;
    for (const auto &u : *g_com_objects) {
        ComObj *o = u.get();
        if (!o || !o->alive || o->kind != K_D3D9VB || !o->pixels)
            continue;
        if (addr < o->pixels || addr - o->pixels >= o->blob.size())
            continue;
        g_vfetch.vb = o->id;
        g_vfetch.stride = stride;
        g_vfetch.nverts = (uint32_t)(o->blob.size() / stride);
        g_vfetch.check = check;
        host_d9_vfetch_set_prelude(source);
        LOGW("d3d9: vertex fetch for buffer %u (%u vertices of %u bytes)%s", o->id, g_vfetch.nverts,
             stride, check ? ", checked" : "");
        return g_vfetch.nverts;
    }
    return 0;
}
// native_seam.h. RECOMP_D3D9_PREWRITE=0 ignores the hints (A/B).
extern "C" void d3d9_prewrite(uint32_t buffer, uint32_t offset, uint32_t bytes) {
    static const bool on = [] {
        const char *v = recomp_env("D3D9_PREWRITE");
        return !(v && v[0] == '0');
    }();
    if (!on || !bytes || !g_track_len)
        return;
#if D9_PAGE_PROTECT
    if (!g_prewrite_live)
        return;
#endif
    ComObj *o = com_this(buffer, IF_NONE);
    if (!o || (o->kind != K_D3D9VB && o->kind != K_D3D9IB) || !o->pixels || offset >= o->blob.size())
        return;
    GpuMirror &m = mirror_of(o);
    if (!m.tracked)
        return;
    bytes = (uint32_t)std::min<uint64_t>(bytes, o->blob.size() - offset);
    const uint32_t lo = o->pixels + offset, hi = lo + bytes;
#if D9_PAGE_PROTECT
    if (write_track_mode() == 3 && g_prot) {
        const uint32_t hp = host_page();
        uint32_t p = (lo - g_track_base) >> g_host_page_shift;
        const uint32_t e = std::min(((hi - 1 - g_track_base) >> g_host_page_shift) + 1, g_prot_pages);
        while (p < e) {
            if (!g_prot[p]) {
                ++p;
                continue;
            }
            uint32_t q = p;
            while (q < e && g_prot[q]) {
                for (uint32_t k = 0; k < hp >> 12; ++k)
                    g_track_pages[(q * hp >> 12) + k] = 1;
                if (g_hps)
                    g_hps[q].open_frame = g_prot_frame_no;
                ++q;
            }
            prot_set(g_track_base + p * hp, (q - p) * hp, false);
            g_sync_stats.prewritten += q - p;
            p = q;
        }
        return;
    }
#endif
    recomp_track_range(lo, bytes);
}
extern "C" void d3d9_vfetch_size(uint32_t k, uint32_t bytes) {
    if (k < 5)
        g_vfetch.size[k] = bytes;
}
extern "C" void d3d9_vfetch_write(uint32_t k, uint32_t offset, const void *bytes, uint32_t size) {
    if (k >= 5 || !bytes || !size || offset > g_vfetch.size[k] || size > g_vfetch.size[k] - offset)
        return;
    host_d9_buffer_upload(HOST_D9_VFETCH_BUFFER(k), g_vfetch.size[k] | HOST_D9_UPLOAD_INPLACE, offset,
                          (const uint8_t *)bytes, size);
    g_vfetch.written[k] = true;
}
extern "C" uint64_t d3d9_vfetch_frames(void) {
    return g_vfetch.presents;
}
extern "C" uint32_t d3d9_vfetch_buffer(void) {
    return g_vfetch.vb;
}
extern "C" void d3d9_vfetch_check(void (*visit)(const uint32_t *, uint32_t, uint32_t, void *), void *user) {
    host_d9_vfetch_check(visit, user);
}
// A draw from the vertex fetch's buffer (stream 0 at offset 0, with a vertex shader) runs it.
static void gpu_vfetch(const D9Pipeline &pl, HostD9Draw &d) {
    if (!g_vfetch.vb || pl.stream[0].vb != g_vfetch.vb || pl.stream[0].offset ||
        pl.stream[0].stride != g_vfetch.stride || pl.vs.empty())
        return;
    for (int k = 0; k < (g_vfetch.check ? 5 : 4); ++k)
        if (!g_vfetch.written[k])
            return;
    d.vfetch = g_vfetch.check ? 2 : 1;
    d.vfetch_tag = (uint32_t)g_vfetch.presents + 1;
}

// One draw, handed to the GPU renderer with everything the device has bound. A stage with no
// shader bound draws with the fixed-function program its state describes (d3d9_ffp.h).
static void gpu_draw(ComObj *dev, HostD9Draw &d) {
    D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *decl = com_get(pl.declaration);
    if (!decl || decl->kind != K_D3D9DECL) {
        reject_draw(dev, d, 2);
        log_once("d3d9.gpu.nodecl", "d3d9: a draw with no vertex declaration or FVF is skipped");
        return;
    }
    d.target = gpu_target(dev);
    target_viewport(dev, d.target, d.viewport);
    d.depth_range[0] = pl.viewport_z[0];
    d.depth_range[1] = pl.viewport_z[1];
    for (int i = 0; i < 4; ++i)
        d.scissor[i] = pl.scissor[i];
    const bool lookup_always = recomp_ab::old(recomp_ab::MIRRORS);
    if (!pl.vs.empty()) {
        if (!pl.vs_key)
            pl.vs_key = d9sh::code_key(pl.vs.data(), pl.vs.size());
        if (!pl.vs_prog || pl.vs_prog_key != pl.vs_key || lookup_always) {
            pl.vs_prog = &d9sh::program_for_key(pl.vs_key, pl.vs.data(), pl.vs.size());
            pl.vs_prog_key = pl.vs_key;
        }
        const d9sh::Program &vs = *static_cast<const d9sh::Program *>(pl.vs_prog);
        d.vs_key = pl.vs_key;
        d.vs = pl.vs.data();
        d.vs_size = (uint32_t)pl.vs.size();
        d.vconst = &pl.vconst[0][0];
        d.vconst_count = std::min<uint32_t>(vs.max_const, 256);
        memcpy(d.vbool, pl.vbool, sizeof d.vbool);
    } else {
        const d9ffp::Program &ffp =
            d9ffp::vertex_program(pl, decl->blob.data(), (uint32_t)decl->blob.size(), d.viewport,
                                  d.depth_range, !pl.ps.empty(), g_ffp_vconst);
        d.vs_key = ffp.key;
        d.vs = ffp.bytes.data();
        d.vs_size = (uint32_t)ffp.bytes.size();
        d.vconst = &g_ffp_vconst[0][0];
        d.vconst_count = d9ffp::VS_CONSTANTS;
    }
    if (!pl.ps.empty()) {
        if (!pl.ps_key)
            pl.ps_key = d9sh::code_key(pl.ps.data(), pl.ps.size());
        if (!pl.ps_prog || pl.ps_prog_key != pl.ps_key || lookup_always) {
            pl.ps_prog = &d9sh::program_for_key(pl.ps_key, pl.ps.data(), pl.ps.size());
            pl.ps_prog_key = pl.ps_key;
        }
        const d9sh::Program &ps = *static_cast<const d9sh::Program *>(pl.ps_prog);
        d.ps_key = pl.ps_key;
        d.ps = pl.ps.data();
        d.ps_size = (uint32_t)pl.ps.size();
        d.pconst = &pl.pconst[0][0];
        d.pconst_count = std::min<uint32_t>(ps.max_const, 224);
        memcpy(d.pbool, pl.pbool, sizeof d.pbool);
    } else {
        const d9ffp::Program &ffp = d9ffp::pixel_program(pl, ffp_textures(pl), g_ffp_pconst);
        d.ps_key = ffp.key;
        d.ps = ffp.bytes.data();
        d.ps_size = (uint32_t)ffp.bytes.size();
        d.pconst = &g_ffp_pconst[0][0];
        d.pconst_count = d9ffp::PS_CONSTANTS;
    }
    if (pl.vs.empty() || pl.ps.empty()) {
        static uint64_t ffp_draws = 0;
        if (++ffp_draws == 1 || (ffp_draws & (ffp_draws - 1)) == 0)
            LOGW("d3d9: %llu draws through the fixed-function pipeline so far (this one: %s vertex, "
                 "%s pixel)",
                 (unsigned long long)ffp_draws, pl.vs.empty() ? "fixed" : "shader",
                 pl.ps.empty() ? "fixed" : "shader");
    }
    d.decl = decl->blob.data();
    d.decl_size = (uint32_t)decl->blob.size();
    d.decl_id = decl->id;
    for (int i = 0; i < 8; ++i) {
        if (d.stream[i].stride && !d.stream[i].buffer)
            continue; // the draw's own inline vertices
        ComObj *vb = com_get(pl.stream[i].vb);
        if (!vb)
            continue;
        if (g_draw_vcount && pl.stream[i].stride) {
            int64_t first = (int64_t)g_draw_vbase + g_draw_vfirst;
            uint64_t count = g_draw_vcount;
            uint32_t imin, imax;
            if (mirror_of(vb).lazy && draw_index_span(&imin, &imax)) {
                first = (int64_t)g_draw_vbase + imin;
                count = (uint64_t)imax - imin + 1;
            }
            uint64_t lo = (uint64_t)std::max<int64_t>(0, (int64_t)pl.stream[i].offset +
                                                            first * pl.stream[i].stride);
            uint64_t hi = lo + count * pl.stream[i].stride;
            gpu_sync_buffer_range(vb, lo, hi);
        } else {
            gpu_sync_buffer(vb);
        }
        d.stream[i].buffer = vb->id;
        d.stream[i].offset = pl.stream[i].offset;
        d.stream[i].stride = pl.stream[i].stride;
    }
    uint32_t sampler_state[16][14];
    bool volume_lod = false;
    for (int s = 0; s < 16; ++s) {
        ComObj *tex = com_get(pl.sampler_tex[s]);
        if (!tex || (tex->kind != K_D3D9TEXTURE && tex->kind != K_D3D9SURFACE &&
                     tex->kind != K_D3D9VOLUMETEXTURE))
            continue;
        gpu_sync(tex);
        d.sampler_texture[s] = tex->id;
        if (tex->kind == K_D3D9VOLUMETEXTURE && volumes().at(tex->id).lod) {
            if (!volume_lod) {
                memcpy(sampler_state, pl.sampler_state, sizeof sampler_state);
                volume_lod = true;
            }
            sampler_state[s][9] = std::max(sampler_state[s][9], volumes().at(tex->id).lod);
        }
    }
    d.sampler_state = volume_lod ? &sampler_state[0][0] : &pl.sampler_state[0][0];
    d.render_state = pl.rs;
    d.render_state_set = (const uint8_t *)pl.rs_set;
    // A volume-LOD draw passes a local copy (sampler [s][9] raised), not the pipeline's tables: version 0 so the queue
    // neither reuses an earlier table for it nor lets the next draw reuse this one.
    d.state_version = volume_lod ? 0 : pl.state_version;
    // Test133: private render state 251 = a guest texture pointer the mod counts as scene content under the HUD.
    if (pl.rs[251] && pl.sampler_tex[0]) {
        ComObj *scene_tex = com_this(pl.rs[251]);
        d.hdr_scene_texture = scene_tex && scene_tex->id == pl.sampler_tex[0];
    }
    for (int st = 0; st < 8; ++st)
        if (pl.tss[st][24] & 0x100) // D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_PROJECTED
            d.projected_mask |= 1u << st;
    d.label = pl.label;
    gpu_vfetch(pl, d);
    host_d9_draw(&d);
    bool color_write = !pl.rs_set[168] || (pl.rs[168] & 0xf) != 0;
    bool z_write = (!pl.rs_set[14] || pl.rs[14]) && (!pl.rs_set[7] || pl.rs[7]);
    gpu_target_drawn(dev, color_write, z_write);
}

// Hands one draw to the CPU renderer with what the device has bound.
static void raster(X86 *c, const D9DrawCall &call) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *target = com_get(dev->render_target);
    if (!target)
        target = device_backbuffer(dev);
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    static const std::vector<uint8_t> none;
    d9_raster_draw(dev, target, (decl && decl->kind == K_D3D9DECL) ? decl->blob : none, call);
}

// (this, PrimitiveType, StartVertex, PrimitiveCount)
static bool draw_refused(X86 *c, const char *what, uint32_t prim, uint32_t count, uint32_t vertices, uint32_t stride);

void Dev_DrawPrimitive(X86 *c) {
    g_frame_stats.draws++, g_frame_stats.prims += arg(c, 3);
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives from vertex %u, from the bound stream", g_draws,
             prim_name(arg(c, 1)), arg(c, 3), arg(c, 2));
    if (draw_refused(c, "DrawPrimitive", arg(c, 1), arg(c, 3), 0, 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 81);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 3);
        d.start = arg(c, 2);
        g_draw_vbase = 0;
        g_draw_vfirst = d.start;
        g_draw_vcount = prim_vertex_count(d.primitive, d.primitive_count);
        gpu_draw(dev, d);
        g_draw_vcount = 0;
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    if (vb && resource_bytes(vb) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 3);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.first = arg(c, 2);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
void Dev_DrawIndexedPrimitive(X86 *c) {
    g_frame_stats.draws++, g_frame_stats.prims += arg(c, 6);
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices from index %u, indexed", g_draws,
             prim_name(arg(c, 1)), arg(c, 6), arg(c, 4), arg(c, 5));
    if (draw_refused(c, "DrawIndexedPrimitive", arg(c, 1), arg(c, 6), arg(c, 4), 0)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *dev = this_device9(c);
    if (dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 82);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 6);
        d.start = arg(c, 5);
        d.base_vertex = (int32_t)arg(c, 2);
        ComObj *ibo = com_get(d9_pipeline(dev->id).index_buffer);
        if (ibo) {
            d.index_buffer = ibo->id;
            d.index_size = ibo->rmask ? ibo->rmask : 2;
            uint32_t indices = prim_vertex_count(d.primitive, d.primitive_count);
            if (indices)
                gpu_sync_buffer_range(ibo, (uint64_t)d.start * d.index_size,
                                      ((uint64_t)d.start + indices) * d.index_size);
            else
                gpu_sync_buffer(ibo);
            // (Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount)
            g_draw_vbase = d.base_vertex;
            g_draw_vfirst = arg(c, 3);
            g_draw_vcount = arg(c, 4);
            g_draw_ib = ibo;
            g_draw_istart = d.start;
            g_draw_icount = indices;
            g_draw_isize = d.index_size;
            g_draw_iscanned = false;
            gpu_draw(dev, d);
            g_draw_vcount = 0;
            g_draw_ib = nullptr;
        } else {
            reject_draw(dev, d, 3);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *vb = dev ? com_get(dev->back_obj) : nullptr;
    ComObj *ib = dev ? com_get(dev->front_obj) : nullptr;
    if (vb && resource_bytes(vb) && ib && resource_bytes(ib) && dev->lock_off < vb->blob.size()) {
        D9DrawCall call;
        call.prim = arg(c, 1);
        call.prim_count = arg(c, 6);
        call.vertex_data = resource_bytes(vb) + dev->lock_off;
        call.vertex_bytes = vb->blob.size() - dev->lock_off;
        call.stride = dev->rate;
        call.index_data = resource_bytes(ib);
        call.index_bytes = ib->blob.size();
        call.index_size = ib->rmask ? ib->rmask : 2;
        call.base_vertex = (int32_t)arg(c, 2);
        call.first = arg(c, 5);
        raster(c, call);
    }
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, PrimitiveCount, pVertexStreamZeroData, Stride): the
// vertices travel with the call rather than in a buffer, which is how this
// game draws.
// What a rasterizer will need from a draw, spelled out for the first few: the
// bound vertex declaration decoded, the raw vertices as floats, the bound
// texture, and the surface being drawn into.
static void describe_draw_inputs(X86 *c, uint32_t vertices, uint32_t stride, uint32_t nverts) {
    ComObj *dev = this_device9(c);
    if (!dev)
        return;
    ComObj *decl = com_get(dev->current_viewport); // SetVertexDeclaration keeps it here
    if (decl && decl->kind == K_D3D9DECL) {
        static const char *types[] = {"FLOAT1",    "FLOAT2",   "FLOAT3",   "FLOAT4",  "D3DCOLOR",
                                      "UBYTE4",    "SHORT2",   "SHORT4",   "UBYTE4N", "SHORT2N",
                                      "SHORT4N",   "USHORT2N", "USHORT4N", "UDEC3",   "DEC3N",
                                      "FLOAT16_2", "FLOAT16_4"};
        static const char *usages[] = {
            "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
            "BINORMAL", "TESSFACTOR",  "POSITIONT",    "COLOR",  "FOG",   "DEPTH",    "SAMPLE"};
        char line[512];
        size_t used = 0;
        for (size_t i = 0; i + 8 <= decl->blob.size(); i += 8) {
            uint16_t stream = (uint16_t)(decl->blob[i] | (decl->blob[i + 1] << 8));
            if (stream == 0xff)
                break;
            uint16_t off = (uint16_t)(decl->blob[i + 2] | (decl->blob[i + 3] << 8));
            uint8_t type = decl->blob[i + 4], usage = decl->blob[i + 6], index = decl->blob[i + 7];
            used += (size_t)snprintf(line + used, sizeof line - used, " %s%u@%u:%s",
                                     usage < 14 ? usages[usage] : "?", index, off,
                                     type < 17 ? types[type] : "?");
            if (used >= sizeof line)
                break;
        }
        LOGW("d3d9:   declaration %u:%s", decl->id, line);
    } else {
        LOGW("d3d9:   no vertex declaration bound (FVF path)");
    }
    for (uint32_t v = 0; v < nverts && v < 4; ++v) {
        uint32_t base = vertices + v * stride;
        char line[256];
        size_t used = 0;
        for (uint32_t o = 0; o + 4 <= stride && o < 40; o += 4)
            used += (size_t)snprintf(
                line + used, sizeof line - used, " %g", (double)[&] {
                    float f;
                    uint32_t bits = rd32(base + o);
                    memcpy(&f, &bits, 4);
                    return f;
                }());
        LOGW("d3d9:   vertex %u:%s", v, line);
    }
    ComObj *tex = com_get(dev->texture_handle);
    ComObj *rt = com_get(dev->render_target);
    LOGW("d3d9:   texture %u (%ux%u), target %u (%ux%u)%s", tex ? tex->id : 0, tex ? tex->width : 0,
         tex ? tex->height : 0, rt ? rt->id : 0, rt ? rt->width : 0, rt ? rt->height : 0,
         (rt && rt->id == dev->palette_obj) ? ", the back buffer" : "");
}


// Test155 (test session: the game's memory footprint went from 3.1 GB to 361 GB within a minute in
// rain and macOS killed it for low swap; 361 GB / 2^31 = ~180 bytes per primitive). The UP draws computed their
// vertex bytes in 32 bits, so a garbage primitive count (e.g. 0x80000000 + n) wrapped to a size that still fit the
// guest arena and was accepted. Direct3D 9 limits every draw to the device's MaxPrimitiveCount (and indexed draws
// to MaxVertexIndex) and an invalid draw returns D3DERR_INVALIDCALL. All four draw calls check the caps values
// this device reports, before any size arithmetic or allocation, and log the raw parameters and the caller.
static uint64_t primitive_vertices64(uint32_t prim, uint32_t count) {
    switch (prim) {
    case 1: return count;
    case 2: return (uint64_t)count * 2;
    case 3: return (uint64_t)count + 1;
    case 4: return (uint64_t)count * 3;
    case 5:
    case 6: return (uint64_t)count + 2;
    default: return 0;
    }
}
static bool draw_refused(X86 *c, const char *what, uint32_t prim, uint32_t count, uint32_t vertices, uint32_t stride) {
    if (count <= D9_MAX_PRIMITIVE_COUNT && vertices <= (uint64_t)D9_MAX_VERTEX_INDEX + 1)
        return false;
    static uint64_t refused = 0;
    ++refused;
    if (refused <= 16 || (refused & (refused - 1)) == 0)
        LOGW("d3d9: %s refused (D3DERR_INVALIDCALL, as Direct3D 9): primitive count %u (0x%08x, signed %d), type %u, "
             "%u vertices, stride %u, caller %08x; device limits %u primitives / %u vertex index (%llu refused so far)",
             what, count, count, (int32_t)count, prim, vertices, stride, rd32(c->r[R_ESP]), D9_MAX_PRIMITIVE_COUNT,
             D9_MAX_VERTEX_INDEX, (unsigned long long)refused);
    return true;
}

void Dev_DrawPrimitiveUP(X86 *c) {
    g_frame_stats.draws++, g_frame_stats.prims += arg(c, 2);
    if (draw_worth_reporting()) {
        LOGW("d3d9: draw %u: %s, %u primitives, %u-byte vertices inline at %08x", g_draws,
             prim_name(arg(c, 1)), arg(c, 2), arg(c, 4), arg(c, 3));
        // A fan of n primitives has n + 2 vertices.
        if (g_draws <= 8)
            describe_draw_inputs(c, arg(c, 3), arg(c, 4), arg(c, 2) + 2);
    }
    if (draw_refused(c, "DrawPrimitiveUP", arg(c, 1), arg(c, 2), 0, arg(c, 4))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 83);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 2);
        const uint64_t bytes64 = primitive_vertices64(d.primitive, d.primitive_count) * (uint64_t)arg(c, 4);
        uint32_t bytes = bytes64 <= 0xffffffffull ? (uint32_t)bytes64 : 0;
        if (bytes && gm_fits(arg(c, 3), bytes)) {
            d.stream[0].stride = arg(c, 4);
            d.inline_vertices = gm_ptr(arg(c, 3));
            d.inline_bytes = bytes;
            gpu_draw(dev, d);
        } else {
            reject_draw(dev, d, 4);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 2);
    call.vertices = arg(c, 3);
    call.stride = arg(c, 4);
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// (this, PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount,
//  pIndexData, IndexDataFormat, pVertexStreamZeroData, Stride)
void Dev_DrawIndexedPrimitiveUP(X86 *c) {
    g_frame_stats.draws++, g_frame_stats.prims += arg(c, 4);
    if (draw_worth_reporting())
        LOGW("d3d9: draw %u: %s, %u primitives, %u vertices of %u bytes inline at %08x, "
             "indices at %08x",
             g_draws, prim_name(arg(c, 1)), arg(c, 4), arg(c, 3), arg(c, 8), arg(c, 7), arg(c, 5));
    if (draw_refused(c, "DrawIndexedPrimitiveUP", arg(c, 1), arg(c, 4), arg(c, 3), arg(c, 8))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (ComObj *dev = this_device9(c); dev && gpu_on()) {
        HostD9Draw d{};
        observe_draw(c, dev, d, 84);
        d.primitive = arg(c, 1);
        d.primitive_count = arg(c, 4);
        uint32_t isize = arg(c, 6) == 102 ? 4 : 2;
        const uint64_t ib64 = primitive_vertices64(d.primitive, d.primitive_count) * isize;
        const uint64_t vb64 = ((uint64_t)arg(c, 2) + arg(c, 3)) * (uint64_t)arg(c, 8);
        uint32_t ibytes = ib64 <= 0xffffffffull ? (uint32_t)ib64 : 0;
        uint32_t vbytes = vb64 <= 0xffffffffull ? (uint32_t)vb64 : 0;
        if (ibytes && vbytes && gm_fits(arg(c, 5), ibytes) && gm_fits(arg(c, 7), vbytes)) {
            d.stream[0].stride = arg(c, 8);
            d.inline_vertices = gm_ptr(arg(c, 7));
            d.inline_bytes = vbytes;
            d.inline_indices = gm_ptr(arg(c, 5));
            d.inline_index_bytes = ibytes;
            d.index_size = isize;
            gpu_draw(dev, d);
        } else {
            reject_draw(dev, d, 4);
        }
        com_ret(c, D3D_OK9);
        return;
    }
    D9DrawCall call;
    call.prim = arg(c, 1);
    call.prim_count = arg(c, 4);
    call.vertices = arg(c, 7);
    call.stride = arg(c, 8);
    call.indices = arg(c, 5);
    call.index_size = arg(c, 6) == 102 ? 4 : 2;
    raster(c, call);
    com_ret(c, D3D_OK9);
}

// ---------------------------------------------------------------------------
// The resource interfaces themselves
// ---------------------------------------------------------------------------
#define RES_STUB(iface, name)                                                                      \
    void iface##_##name(X86 *c) {                                                                  \
        log_once("d3d9." #iface "." #name, "d3d9: " #iface "::" #name " is not implemented");      \
        com_ret(c, D3D_OK9);                                                                       \
    }

RES_STUB(Res, SetPrivateData)
RES_STUB(Res, GetPrivateData)
RES_STUB(Res, FreePrivateData)
RES_STUB(Res, SetPriority)
RES_STUB(Res, GetPriority)
RES_STUB(Res, PreLoad)
RES_STUB(Res, GetType)
RES_STUB(Tex, SetLOD)
RES_STUB(Tex, GetLOD)
RES_STUB(Tex, SetAutoGenFilterType)
RES_STUB(Tex, GetAutoGenFilterType)
RES_STUB(Tex, GenerateMipSubLevels)
RES_STUB(Surf, GetDC)
RES_STUB(Surf, ReleaseDC)
RES_STUB(Decl, GetDeclaration)

// (this, riid, ppContainer): the texture a level or face belongs to, or the
// device when the surface stands alone.
void Surf_GetContainer(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *parent = com_get(s->front_obj);
    if (parent) {
        com_addref(parent);
        com_out_ptr(out, com_view(parent, parent->caps ? IF_D3DCUBETEXTURE9 : IF_D3DTEXTURE9));
        com_ret(c, D3D_OK9);
        return;
    }
    ComObj *dev = com_get(s->dev_d3d);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

void Res_GetDevice(X86 *c) {
    ComObj *o = com_this_arg(c);
    ComObj *dev = o ? com_get(o->dev_d3d) : nullptr;
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    com_addref(dev);
    com_out_ptr(out, com_view(dev, IF_D3DDEVICE9));
    com_ret(c, D3D_OK9);
}

// Vertex and index buffers keep one guest copy for their whole life, made on
// the first Lock, as a driver keeps a dynamic buffer mapped: Lock hands back a
// pointer into it and Unlock copies back and uploads only what the lock
// covered. Staging the whole buffer per lock, as textures do, cost Heroes V
// two full copies, a guest heap block and a full GPU upload of a 26 MB
// dynamic vertex buffer for every batch it wrote - under one frame a second.
// A lock of the whole buffer (SizeToLock 0, the common way to write a few
// vertices at an offset the game tracks itself) is narrowed on unlock to the
// 4 KB pages that changed.
static uint32_t buffer_stage(ComObj *o) {
    if (!o->pixels) {
        // A buffer that may turn lazy gets a page-aligned copy, so its pages
        // are the tracked guest pages (track_buffer).
        const bool big = o->blob.size() > 0x10000u && write_track_mode();
        // Under page protection the copy owns whole host pages (see prot_set).
        const uint32_t align = !big ? 16u : write_track_mode() == 3 ? host_page() : 0x1000u;
        const uint32_t bytes = big && write_track_mode() == 3
                                   ? ((uint32_t)o->blob.size() + align - 1) & ~(align - 1)
                                   : (uint32_t)o->blob.size();
        o->pixels = heap_alloc(bytes, false, align);
        if (!o->pixels)
            return 0;
        memcpy(gm_ptr(o->pixels), o->blob.data(), o->blob.size());
    }
    return o->pixels;
}
// (this, OffsetToLock, SizeToLock, ppbData, Flags)
static void buffer_lock(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset = arg(c, 1), size = arg(c, 2), out = arg(c, 3);
    if (!o || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t total = (uint32_t)o->blob.size();
    uint32_t staged = offset < total ? buffer_stage(o) : 0;
    if (!staged) {
        com_out_ptr(out, 0);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    uint32_t hi = size ? (uint32_t)std::min<uint64_t>((uint64_t)offset + size, total) : total;
    if (log_level() >= 2) {
        char key[96];
        snprintf(key, sizeof key, "d9 buffer lock %u %s %x", o->id, size ? "range" : "whole", arg(c, 4));
        log_once(key, "d3d9: buffer %u (%u bytes) first %s lock: offset %u size %u flags %#x, from %08x",
                 o->id, total, size ? "ranged" : "whole-buffer", offset, size, arg(c, 4),
                 rd32(c->r[R_ESP]));
    }
    {
        // D3DLOCK_NOOVERWRITE 0x1000, D3DLOCK_DISCARD 0x2000
        uint32_t flags = arg(c, 4);
        if (!(flags & 0x1000u) || (flags & 0x2000u))
            mirror_of(o).noover_only = false;
        (o->kind == K_D3D9IB ? g_frame_stats.ib_locks : g_frame_stats.vb_locks)++;
        (o->kind == K_D3D9IB ? g_frame_stats.ib_lock_bytes : g_frame_stats.vb_lock_bytes) +=
            (size ? std::min(size, total - std::min(offset, total)) : total);
        if (sync_stats_on() && total > 0x10000u)
            (flags & 0x2000u ? g_sync_stats.locks_discard
                             : flags & 0x1000u ? g_sync_stats.locks_noover : g_sync_stats.locks_plain)++;
        mirror_of(o).lock_gen++;
    }
    if (o->lock_count++ == 0) {
        o->lock_lo = offset;
        o->lock_hi = hi;
        o->lock_whole = !size;
    } else {
        o->lock_lo = std::min(o->lock_lo, offset);
        o->lock_hi = std::max(o->lock_hi, hi);
        o->lock_whole = o->lock_whole || !size;
    }
    com_out_ptr(out, staged + offset);
    com_ret(c, D3D_OK9);
}
void VB_Lock(X86 *c) {
    buffer_lock(c);
}
void IB_Lock(X86 *c) {
    buffer_lock(c);
}
void Buf_Unlock(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o || o->lock_count <= 0) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (--o->lock_count == 0 && o->pixels) {
        const uint32_t total = (uint32_t)o->blob.size();
        uint32_t lo = std::min(o->lock_lo, total), hi = std::min(o->lock_hi, total);
        const uint8_t *guest = gm_ptr(o->pixels);
        uint8_t *host = o->blob.data();
        if (o->lock_whole && total > 0x10000u) {
            // The game writes where it alone knows (NOOVERWRITE appends, as
            // Heroes V's 26 MB batch buffer does); the draws that read it say
            // where, and sync just that (gpu_sync_buffer_range).
            GpuMirror &m = mirror_of(o);
            if (!m.defined) {
                memcpy(host, guest, total);
                gpu_after_unlock(o, 0, total);
            }
            m.lazy = true;
            track_buffer(o, m);
            d9_raster_invalidate(o->id);
            com_ret(c, D3D_OK9);
            return;
        }
        if (hi > lo && hi - lo > 0x10000u) {
            const uint32_t page = 0x1000u;
            uint32_t first = hi, last = lo;
            for (uint32_t p = lo; p < hi; p += page) {
                uint32_t n = std::min(page, hi - p);
                if (memcmp(guest + p, host + p, n) != 0) {
                    if (first == hi)
                        first = p;
                    last = p + n;
                }
            }
            lo = first;
            hi = first == hi ? first : last;
        }
        if (hi > lo) {
            memcpy(host + lo, guest + lo, hi - lo);
            d9_raster_invalidate(o->id);
            gpu_after_unlock(o, lo, hi);
        }
    }
    com_ret(c, D3D_OK9);
}
// D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Format, Type, Usage, Pool, Size.
void Buf_GetDesc(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!o || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memset(gm_ptr(d), 0, 20);
    wr32(d + 16, o->buf_bytes);
    com_ret(c, D3D_OK9);
}

// D3DLOCKED_RECT is { INT Pitch; void *pBits; }.
static bool readback_settle(ComObj *s, uint32_t flags);
// The pointer LockRect hands back: the locked storage at the top-left corner of pRect (a guest
// RECT, or 0 for the whole surface), as Direct3D 9 returns it. A compressed surface is addressed
// in 4x4 blocks. A rectangle outside the surface is refused (0).
static uint32_t locked_bits(ComObj *s, uint32_t staged, uint32_t rect) {
    if (!staged || !rect)
        return staged;
    const int32_t left = (int32_t)rd32(rect), top = (int32_t)rd32(rect + 4),
                  right = (int32_t)rd32(rect + 8), bottom = (int32_t)rd32(rect + 12);
    if (left < 0 || top < 0 || right <= left || bottom <= top || (uint32_t)right > s->width ||
        (uint32_t)bottom > s->height)
        return 0;
    if (const uint32_t block = dxt_block_bytes(s->rmask))
        return staged + (uint32_t)top / 4 * s->pitch + (uint32_t)left / 4 * block;
    return staged + (uint32_t)top * s->pitch + (uint32_t)left * (s->bpp / 8);
}
void Surf_LockRect(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t out = arg(c, 1);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // (this, pLockedRect, pRect, Flags): a GetRenderTargetData copy still on the GPU answers
    // D3DERR_WASSTILLDRAWING to a D3DLOCK_DONOTWAIT lock, as a driver does.
    if (!readback_settle(s, arg(c, 3))) {
        com_ret(c, D3DERR_WASSTILLDRAWING);
        return;
    }
    uint32_t staged = stage_lock(s);
    const uint32_t bits = locked_bits(s, staged, arg(c, 2));
    if (staged && !bits) {
        stage_unlock(s);
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, s->pitch);
    wr32(out + 4, bits);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Surf_UnlockRect(X86 *c) {
    stage_unlock(com_this_arg(c));
    com_ret(c, D3D_OK9);
}
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, Quality,
// Width, Height.
// D3DSURFACE_DESC: Format, Type, Usage, Pool, MultiSampleType, MultiSampleQuality, Width,
// Height. Usage and pool are the creating call's (a level's are its texture's).
static void put_surface_desc(uint32_t d, ComObj *s, uint32_t container, uint32_t fmt) {
    memset(gm_ptr(d), 0, 32);
    auto it = resource_desc().find(container);
    wr32(d + 0, fmt ? fmt : 22);
    wr32(d + 4, 1); // D3DRTYPE_SURFACE
    if (it != resource_desc().end()) {
        wr32(d + 8, it->second.usage);
        wr32(d + 12, it->second.pool);
    }
    wr32(d + 16, s && s->samples > 1 ? s->samples : 0);
    wr32(d + 24, s ? s->width : 0);
    wr32(d + 28, s ? s->height : 0);
}
void Surf_GetDesc(X86 *c) {
    ComObj *s = com_this_arg(c);
    uint32_t d = arg(c, 1);
    if (!s || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    put_surface_desc(d, s, s->front_obj ? s->front_obj : s->id, s->rmask);
    com_ret(c, D3D_OK9);
}

// A texture level is the surface made with the texture.
// A cube face, made on first ask and kept, so the game gets the same surface
// back every time. Returning success without writing the pointer - which is
// what the stub did - had the game store nothing and throw the cube away.
// The surface for one face and level of a cube, made on first ask and kept on
// the texture, indexed face + 6 * level.
static ComObj *cube_face(ComObj *tex, uint32_t face, uint32_t level) {
    if (!tex || tex->kind != K_D3D9TEXTURE || !tex->caps || face > 5 || level > 15)
        return nullptr;
    size_t slot = face + 6u * level;
    if (tex->surfaces.size() <= slot)
        tex->surfaces.resize(slot + 1, 0);
    ComObj *s = com_get(tex->surfaces[slot]);
    if (!s) {
        uint32_t edge = std::max(tex->width >> level, 1u);
        s = make_surface(com_get(tex->dev_d3d), edge, edge, tex->rmask ? tex->rmask : 22);
        if (!s)
            return nullptr;
        s->front_obj = tex->id;
        tex->surfaces[slot] = s->id;
    }
    return s;
}

// (this, FaceType, Level, ppCubeMapSurface). A face is a view into the
// texture, so handing one out references the texture too: the game takes
// face zero, releases the cube, and goes on using it for the other five.
void Tex_GetCubeMapSurface(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 3);
    ComObj *s = cube_face(tex, arg(c, 1), arg(c, 2));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t view = com_view(s, IF_D3DSURFACE9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    surface_addref(s); // a face shares its cube texture's lifetime (Test159)
    com_out_ptr(out, view);
    com_ret(c, D3D_OK9);
}

// Returns a count, not an HRESULT. The stub returned zero, which told the
// game its textures had no levels at all.
// (this, FaceType, Level, pLockedRect, pRect, Flags)
void Tex_LockRectCube(X86 *c) {
    ComObj *s = cube_face(com_this_arg(c), arg(c, 1), arg(c, 2));
    uint32_t out = arg(c, 3);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    const uint32_t bits = locked_bits(s, staged, arg(c, 4));
    if (staged && !bits) {
        stage_unlock(s);
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, s->pitch);
    wr32(out + 4, bits);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
// (this, FaceType, Level)
void Tex_UnlockRectCube(X86 *c) {
    stage_unlock(cube_face(com_this_arg(c), arg(c, 1), arg(c, 2)));
    com_ret(c, D3D_OK9);
}

void Tex_GetLevelCount(X86 *c) {
    ComObj *tex = com_this_arg(c);
    set_eax(c, (tex && !tex->caps && !tex->surfaces.empty()) ? (uint32_t)tex->surfaces.size() : 1);
}

// (this, Level, pDesc): the same D3DSURFACE_DESC a surface describes itself
// with, for the level the texture was created at.
void Tex_GetLevelDesc(X86 *c) {
    ComObj *tex = com_this_arg(c);
    ComObj *s = texture_level(tex, arg(c, 1));
    uint32_t d = arg(c, 2);
    if (!tex || !d) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    put_surface_desc(d, s ? s : tex, tex->id, tex->rmask);
    com_ret(c, D3D_OK9);
}

void Tex_GetSurfaceLevel(X86 *c) {
    ComObj *tex = com_this_arg(c);
    uint32_t out = arg(c, 2);
    ComObj *s = texture_level(tex, arg(c, 1));
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    // A level is a view into the texture and references it, the same way a
    // cube face does.
    s->front_obj = tex->id;
    surface_addref(s); // Test159: the container reference goes with the last outside one
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        surface_release(s);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Tex_LockRect(X86 *c) {
    ComObj *s = texture_level(com_this_arg(c), arg(c, 1));
    uint32_t out = arg(c, 2);
    if (!s || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t staged = stage_lock(s);
    const uint32_t bits = locked_bits(s, staged, arg(c, 3));
    if (staged && !bits) {
        stage_unlock(s);
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, s->pitch);
    wr32(out + 4, bits);
    com_ret(c, staged ? D3D_OK9 : E_OUTOFMEMORY);
}
void Tex_UnlockRect(X86 *c) {
    stage_unlock(texture_level(com_this_arg(c), arg(c, 1)));
    com_ret(c, D3D_OK9);
}

// (this, pDirtyRect) and (this, FaceType, pDirtyRect): the game changed part of a managed texture
// without Direct3D seeing it, through a lock with D3DLOCK_NO_DIRTY_UPDATE. The shim uploads whole
// levels, so the top level is marked changed and reaches the GPU again before the next draw that
// samples it; the other levels follow their own unlocks, as Direct3D's dirty region does.
static void texture_dirty(X86 *c, ComObj *level0) {
    if (!level0) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    gpu_after_unlock(level0, 0, (uint32_t)level0->blob.size());
    d9_raster_invalidate(level0->id);
    com_ret(c, D3D_OK9);
}
void Tex_AddDirtyRect(X86 *c) {
    texture_dirty(c, texture_level(com_this_arg(c), 0));
}
void Tex_AddDirtyRectCube(X86 *c) {
    texture_dirty(c, cube_face(com_this_arg(c), arg(c, 1), 0));
}

// Volume textures use the same guest ABI and canonical host storage as 2D
// resources. Each mip owns a whole 3D box; uploads preserve the Z slices.
static const uint8_t IID_VolumeTexture[16] =
    IID_BYTES(0x2518526c, 0xe789, 0x4111, 0xa7, 0xb9, 0x47, 0xef, 0x32, 0x8d, 0x13, 0xe6);
static const uint8_t IID_Volume[16] =
    IID_BYTES(0x24f416e6, 0x1f67, 0x4aa7, 0xb8, 0x8e, 0xd3, 0x3f, 0x6f, 0x31, 0x28, 0xa1);
static const uint8_t IID_BaseTexture[16] =
    IID_BYTES(0x580ca87e, 0x1d3c, 0x4d54, 0x99, 0x1d, 0xb7, 0xd3, 0xe3, 0xc2, 0x98, 0xce);
static const uint8_t IID_Resource[16] =
    IID_BYTES(0x05eec05d, 0x8f7d, 0x4362, 0xb9, 0x99, 0xd1, 0xba, 0xf3, 0x57, 0xc7, 0x04);
static bool volume_base_iid(uint32_t p) {
    return p && gm_valid(p, 16) &&
           (!memcmp(gm_ptr(p), IID_BaseTexture, 16) || !memcmp(gm_ptr(p), IID_Resource, 16));
}
static void VolTex_QueryInterface(X86 *c) {
    ComObj *o = com_this_arg(c);
    const uint32_t out = arg(c, 2);
    if (o && out && gm_valid(out, 4) && volume_base_iid(arg(c, 1))) {
        wr32(out, o->identity); // prefix interfaces have the same controlling identity
        com_addref(o);
        com_ret(c, S_OK);
    } else {
        com_QueryInterface(c);
    }
}
// A volume texture and its levels, without the COM call around it: shared by
// CreateVolumeTexture and D3DXCreateVolumeTextureFromFileInMemory.
static ComObj *volume_create(ComObj *dev, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                             uint32_t usage, uint32_t fmt, uint32_t pool) {
    levels = levels ? levels : d9volume::levels(w, h, depth);
    ComObj *tex = com_new(K_D3D9VOLUMETEXTURE);
    if (!tex)
        return nullptr;
    volumes()[tex->id] = {depth, pool};
    tex->dev_d3d = dev->id;
    tex->width = w;
    tex->height = h;
    tex->bpp = d9volume::pixel_bytes(fmt) * 8;
    tex->rmask = fmt;
    resource_usage()[tex->id] = usage;
    bool complete = true;
    for (uint32_t l = 0; l < levels; ++l) {
        ComObj *v = com_new(K_D3D9VOLUME);
        if (!v) {
            complete = false;
            break;
        }
        tex->surfaces.push_back(v->id);
        v->dev_d3d = dev->id;
        v->front_obj = tex->id;
        v->width = std::max(w >> l, 1u);
        v->height = std::max(h >> l, 1u);
        v->rmask = fmt;
        v->bpp = tex->bpp;
        v->pitch = v->width * (v->bpp / 8);
        volumes()[v->id] = {std::max(depth >> l, 1u), pool};
        resource_usage()[v->id] = usage;
        resource_alloc(v, v->pitch * v->height * volumes().at(v->id).depth);
    }
    if (!complete) {
        com_release(tex);
        return nullptr;
    }
    return tex;
}
void Dev_CreateVolumeTexture(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t w = arg(c, 1), h = arg(c, 2), depth = arg(c, 3), usage = arg(c, 5),
                   fmt = arg(c, 6), pool = arg(c, 7), out = arg(c, 8);
    uint32_t levels = arg(c, 4);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    if (!dev || !d9volume::extent(w, h, depth, levels) || !d9volume::pixel_bytes(fmt) || pool > 3 ||
        (usage & ~0x210u) || ((usage & 0x200) && pool != 0) || arg(c, 9)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *tex = volume_create(dev, w, h, depth, levels, usage, fmt, pool);
    if (!tex) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    const uint32_t view = com_view(tex, IF_D3DVOLUMETEXTURE9);
    if (!view) {
        com_release(tex);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    wr32(out, view);
    com_ret(c, D3D_OK9);
}
static bool volume_box(ComObj *v, uint32_t box, uint32_t &offset) {
    offset = 0;
    if (!v)
        return false;
    if (!box)
        return true;
    if (!gm_valid(box, 24))
        return false;
    const uint32_t x = rd32(box), y = rd32(box + 4), r = rd32(box + 8), b = rd32(box + 12),
                   z = rd32(box + 16), back = rd32(box + 20);
    if (x >= r || r > v->width || y >= b || b > v->height || z >= back ||
        back > volumes().at(v->id).depth)
        return false;
    offset = z * v->pitch * v->height + y * v->pitch + x * (v->bpp / 8);
    return true;
}
static void volume_lock(X86 *c, ComObj *v, uint32_t out, uint32_t box, uint32_t flags) {
    uint32_t offset;
    if (!v || !out || !gm_valid(out, 12) || v->lock_count || !volume_box(v, box, offset) ||
        (flags & ~0xa810u) ||
        ((flags & 0x2000) && ((flags & 0x10) || box || !(resource_usage()[v->id] & 0x200))) ||
        (volumes().at(v->id).pool == 0 && !(resource_usage()[v->id] & 0x200))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t data = stage_lock(v);
    if (!data) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    volumes().at(v->id).lock_flags = flags;
    wr32(out, v->pitch);
    wr32(out + 4, v->pitch * v->height);
    wr32(out + 8, data + offset);
    com_ret(c, D3D_OK9);
}
static void volume_unlock(X86 *c, ComObj *v) {
    if (!v || !v->lock_count || !v->pixels) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t flags = volumes().at(v->id).lock_flags;
    if (!(flags & 0x10))
        memcpy(v->blob.data(), gm_ptr(v->pixels), v->blob.size());
    heap_free(v->pixels);
    v->pixels = 0;
    v->lock_count = 0;
    if (!(flags & (0x10 | 0x8000)))
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    com_ret(c, D3D_OK9);
}
// D3DXFillVolumeTexture's work, here where volume storage lives: for every
// texel of every level, the guest's LPD3DXFILL3D (out float4, texel-centre
// coordinate, texel size, user data) gives a colour, stored in the level's
// format. Need for Speed Carbon builds its colour-grading lookup this way
// (0073c1a0 -> 0073c100); left unfilled, the lookup is zero and every 3D
// pixel that passes through the visual treatment comes out black.
bool d3d9_fill_volume(X86 *c, uint32_t view, uint32_t fn, uint32_t data) {
    ComObj *tex = view ? com_this(view) : nullptr;
    if (!tex || tex->kind != K_D3D9VOLUMETEXTURE || !fn)
        return false;
    const uint32_t scratch = heap_alloc(48, true);
    if (!scratch)
        return false;
    const uint32_t out = scratch, coord = scratch + 16, size = scratch + 32;
    auto wrf = [](uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); wr32(a, u); };
    auto rdf = [](uint32_t a) { uint32_t u = rd32(a); float f; memcpy(&f, &u, 4); return f; };
    auto u8 = [](float f) {
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        return uint32_t(f * 255.f + 0.5f);
    };
    for (uint32_t l = 0; l < tex->surfaces.size(); ++l) {
        ComObj *v = volume_level(tex, l);
        if (!v || v->lock_count)
            continue;
        const uint32_t w = v->width, h = v->height, d = volumes().at(v->id).depth;
        const uint32_t bpp = v->bpp / 8, fmt = v->rmask;
        if (v->blob.size() < size_t(v->pitch) * h * d)
            continue;
        wrf(size, 1.f / w);
        wrf(size + 4, 1.f / h);
        wrf(size + 8, 1.f / d);
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    wrf(coord, (x + 0.5f) / w);
                    wrf(coord + 4, (y + 0.5f) / h);
                    wrf(coord + 8, (z + 0.5f) / d);
                    for (int i = 0; i < 4; ++i)
                        wrf(out + 4 * i, 0.f);
                    const uint32_t args[4] = {out, coord, size, data};
                    guest_call(c, fn, args, 4);
                    const uint32_t r = u8(rdf(out)), g = u8(rdf(out + 4)),
                                   b = u8(rdf(out + 8)), a = u8(rdf(out + 12));
                    uint8_t *px = v->blob.data() + size_t(z) * v->pitch * h + size_t(y) * v->pitch +
                                  size_t(x) * bpp;
                    if (bpp == 4) {
                        const uint32_t argb = (fmt == 22 ? 0xffu : a) << 24 | r << 16 | g << 8 | b;
                        memcpy(px, &argb, 4);
                    } else {
                        *px = uint8_t(fmt == 28 ? a : r); // A8 keeps alpha, L8 the first channel
                    }
                }
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    }
    heap_free(scratch);
    return true;
}
// ---- DDS files handed to D3DXCreate{Cube,Volume}TextureFromFileInMemory ----
// NFSC keeps both in GLOBALB.BUN chunks (0x30300201 cube, 0x30300200 volume):
// five 256^2 DXT1 environment cubes and a 32^3 A8R8G8B8 volume with 6 mips.
struct DdsInfo {
    uint32_t w = 0, h = 0, d = 1, levels = 1, fmt = 0, caps2 = 0;
    const uint8_t *data = nullptr;
    size_t size = 0;
};
static bool dds_parse(const uint8_t *p, size_t n, DdsInfo &o) {
    auto u32 = [&](size_t at) { uint32_t v; memcpy(&v, p + at, 4); return v; };
    if (n < 128 || memcmp(p, "DDS ", 4) || u32(4) != 124)
        return false;
    const uint32_t flags = u32(8), pf_flags = u32(80), fourcc = u32(84), bits = u32(88),
                   rmask = u32(92), amask = u32(104);
    o.h = u32(12);
    o.w = u32(16);
    o.d = (flags & 0x800000) && u32(24) ? u32(24) : 1; // DDSD_DEPTH
    o.levels = (flags & 0x20000) && u32(28) ? u32(28) : 1; // DDSD_MIPMAPCOUNT
    o.caps2 = u32(112);
    if (pf_flags & 0x4) { // DDPF_FOURCC
        if (!dxt_block_bytes(fourcc))
            return false;
        o.fmt = fourcc;
    } else if ((pf_flags & 0x40) && bits == 32 && rmask == 0x00ff0000u) { // DDPF_RGB
        o.fmt = (pf_flags & 0x1) && amask == 0xff000000u ? 21u : 22u;      // A8R8G8B8 : X8R8G8B8
    } else if (bits == 8 && (pf_flags & 0x2)) {                               // DDPF_ALPHA
        o.fmt = 28;                                                           // A8
    } else if (bits == 8 && (pf_flags & 0x20000)) {                           // DDPF_LUMINANCE
        o.fmt = 50;                                                           // L8
    } else {
        return false;
    }
    if (!o.w || !o.h || o.levels > 16)
        return false;
    o.data = p + 128;
    o.size = n - 128;
    return true;
}
// Bytes in one level's 2D slice, and in one row as the file packs it.
static size_t dds_row_bytes(uint32_t fmt, uint32_t w) {
    if (uint32_t b = dxt_block_bytes(fmt))
        return size_t(std::max((w + 3) / 4, 1u)) * b;
    return size_t(w) * format_bytes(fmt);
}
static uint32_t dds_rows(uint32_t fmt, uint32_t h) {
    return dxt_block_bytes(fmt) ? std::max((h + 3) / 4, 1u) : h;
}

// Returns the new cube texture's IDirect3DCubeTexture9 view, or 0.
uint32_t d3d9_cube_from_dds(uint32_t device_view, const uint8_t *p, size_t n) {
    ComObj *dev = device_view ? com_this(device_view) : nullptr;
    DdsInfo dds;
    if (!dev || !dds_parse(p, n, dds) || !(dds.caps2 & 0x200) || dds.w != dds.h) {
        LOGW("d3dx9: cube DDS not loaded (%zu bytes, unsupported or not a full cube)", n);
        return 0;
    }
    ComObj *tex = com_new(K_D3D9TEXTURE);
    if (!tex)
        return 0;
    tex->dev_d3d = dev->id;
    tex->width = tex->height = dds.w;
    tex->bpp = format_bytes(dds.fmt) * 8;
    tex->rmask = dds.fmt;
    tex->caps = 1;
    tex->pal_flags = dds.levels;
    resource_usage()[tex->id] = 0;
    size_t at = 0;
    for (uint32_t face = 0; face < 6; ++face) // file order: +X -X +Y -Y +Z -Z, each with its mips
        for (uint32_t l = 0; l < dds.levels; ++l) {
            const uint32_t edge = std::max(dds.w >> l, 1u);
            const size_t row = dds_row_bytes(dds.fmt, edge);
            const uint32_t rows = dds_rows(dds.fmt, edge);
            if (at + row * rows > dds.size) {
                LOGW("d3dx9: cube DDS truncated at face %u level %u", face, l);
                com_release(tex);
                return 0;
            }
            ComObj *s = cube_face(tex, face, l);
            const uint32_t staged = s ? stage_lock(s) : 0;
            if (!staged) {
                com_release(tex);
                return 0;
            }
            for (uint32_t r = 0; r < rows; ++r)
                memcpy(gm_ptr(staged + r * s->pitch), dds.data + at + r * row,
                       std::min<size_t>(row, s->pitch));
            stage_unlock(s);
            at += row * rows;
        }
    const uint32_t view = com_view(tex, IF_D3DCUBETEXTURE9);
    if (!view)
        com_release(tex);
    return view;
}

// Returns the new volume texture's IDirect3DVolumeTexture9 view, or 0.
uint32_t d3d9_volume_from_dds(uint32_t device_view, const uint8_t *p, size_t n) {
    ComObj *dev = device_view ? com_this(device_view) : nullptr;
    DdsInfo dds;
    if (!dev || !dds_parse(p, n, dds) || !(dds.caps2 & 0x200000) || !d9volume::pixel_bytes(dds.fmt) ||
        !d9volume::extent(dds.w, dds.h, dds.d, dds.levels)) {
        LOGW("d3dx9: volume DDS not loaded (%zu bytes, unsupported format or size)", n);
        return 0;
    }
    ComObj *tex = volume_create(dev, dds.w, dds.h, dds.d, dds.levels, 0, dds.fmt, 1);
    if (!tex)
        return 0;
    size_t at = 0;
    for (uint32_t l = 0; l < tex->surfaces.size(); ++l) {
        ComObj *v = volume_level(tex, l);
        const uint32_t d = volumes().at(v->id).depth;
        const size_t row = size_t(v->width) * (v->bpp / 8);
        const size_t bytes = row * v->height * d;
        if (at + bytes > dds.size || v->blob.size() < bytes) {
            LOGW("d3dx9: volume DDS truncated at level %u", l);
            com_release(tex);
            return 0;
        }
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < v->height; ++y)
                memcpy(v->blob.data() + (size_t(z) * v->height + y) * v->pitch,
                       dds.data + at + (size_t(z) * v->height + y) * row, row);
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
        at += bytes;
    }
    const uint32_t view = com_view(tex, IF_D3DVOLUMETEXTURE9);
    if (!view)
        com_release(tex);
    return view;
}
// ---- D3DXLoadSurfaceFromSurface: whole-surface copy with format conversion ----
// NFSC copies textures into one another at run time (007d7580 when formats
// differ, 00759040 per mip level, 00732ca0 once at boot). Pixels are read from
// the surface's canonical CPU copy, so a GPU-rendered source (a render target)
// is only as current as its last write-back.
static void px_to_rgba(uint32_t fmt, const uint8_t *p, uint8_t *o) {
    uint32_t v = 0;
    switch (fmt) {
    case 21: o[2] = p[0]; o[1] = p[1]; o[0] = p[2]; o[3] = p[3]; return;      // A8R8G8B8 (BGRA bytes)
    case 22: o[2] = p[0]; o[1] = p[1]; o[0] = p[2]; o[3] = 255; return;       // X8R8G8B8
    case 23: v = p[0] | p[1] << 8;                                             // R5G6B5
        o[0] = uint8_t((v >> 11 & 31) * 255 / 31); o[1] = uint8_t((v >> 5 & 63) * 255 / 63);
        o[2] = uint8_t((v & 31) * 255 / 31); o[3] = 255; return;
    case 24: case 25: v = p[0] | p[1] << 8;                                    // X1R5G5B5 / A1R5G5B5
        o[0] = uint8_t((v >> 10 & 31) * 255 / 31); o[1] = uint8_t((v >> 5 & 31) * 255 / 31);
        o[2] = uint8_t((v & 31) * 255 / 31); o[3] = fmt == 25 ? (v & 0x8000 ? 255 : 0) : 255; return;
    case 26: v = p[0] | p[1] << 8;                                             // A4R4G4B4
        o[0] = uint8_t((v >> 8 & 15) * 17); o[1] = uint8_t((v >> 4 & 15) * 17);
        o[2] = uint8_t((v & 15) * 17); o[3] = uint8_t((v >> 12 & 15) * 17); return;
    case 28: o[0] = o[1] = o[2] = 0; o[3] = p[0]; return;                      // A8
    case 50: o[0] = o[1] = o[2] = p[0]; o[3] = 255; return;                    // L8
    default: o[0] = o[1] = o[2] = 0; o[3] = 255; return;
    }
}
static void rgba_to_px(uint32_t fmt, const uint8_t *o, uint8_t *p) {
    uint32_t v;
    switch (fmt) {
    case 21: p[0] = o[2]; p[1] = o[1]; p[2] = o[0]; p[3] = o[3]; return;
    case 22: p[0] = o[2]; p[1] = o[1]; p[2] = o[0]; p[3] = 255; return;
    case 23: v = (o[0] >> 3) << 11 | (o[1] >> 2) << 5 | o[2] >> 3; p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 24: case 25: v = (o[0] >> 3) << 10 | (o[1] >> 3) << 5 | o[2] >> 3 | (fmt == 25 && o[3] < 128 ? 0 : 0x8000);
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 26: v = (o[3] >> 4) << 12 | (o[0] >> 4) << 8 | (o[1] >> 4) << 4 | o[2] >> 4;
        p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); return;
    case 28: p[0] = o[3]; return;
    case 50: p[0] = uint8_t((o[0] * 77 + o[1] * 150 + o[2] * 29) >> 8); return;
    default: return;
    }
}
static bool rgba_format_known(uint32_t fmt) {
    switch (fmt) {
    case 21: case 22: case 23: case 24: case 25: case 26: case 28: case 50:
        return true;
    default:
        return dxt_block_bytes(fmt) != 0;
    }
}
static void dxt_color_565(const uint8_t *o, uint16_t &c) {
    c = uint16_t((o[0] >> 3) << 11 | (o[1] >> 2) << 5 | o[2] >> 3);
}
static void unpack_565(uint16_t c, uint8_t *o) {
    o[0] = uint8_t((c >> 11 & 31) * 255 / 31); o[1] = uint8_t((c >> 5 & 63) * 255 / 63);
    o[2] = uint8_t((c & 31) * 255 / 31); o[3] = 255;
}
static void surface_to_rgba(ComObj *s, std::vector<uint8_t> &out) {
    const uint32_t w = s->width, h = s->height, fmt = s->rmask;
    out.assign(size_t(w) * h * 4, 0);
    const uint8_t *src = s->blob.data();
    if (uint32_t block = dxt_block_bytes(fmt)) {
        const uint32_t bw = std::max((w + 3) / 4, 1u), bh = std::max((h + 3) / 4, 1u);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                const uint8_t *b = src + size_t(by) * s->pitch + size_t(bx) * block;
                if (size_t(b - src) + block > s->blob.size())
                    return;
                const uint8_t *cb = block == 8 ? b : b + 8;
                const uint16_t c0 = uint16_t(cb[0] | cb[1] << 8), c1 = uint16_t(cb[2] | cb[3] << 8);
                uint8_t pal[4][4];
                unpack_565(c0, pal[0]);
                unpack_565(c1, pal[1]);
                const bool four = block == 16 || c0 > c1;
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = uint8_t(four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = uint8_t(four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0);
                }
                pal[2][3] = 255;
                pal[3][3] = four ? 255 : 0;
                const uint32_t bits = uint32_t(cb[4] | cb[5] << 8 | cb[6] << 16 | uint32_t(cb[7]) << 24);
                uint8_t alpha[8] = {};
                uint64_t abits = 0;
                if (fmt == 0x35545844u) { // DXT5
                    alpha[0] = b[0];
                    alpha[1] = b[1];
                    for (int k = 1; k < 7; ++k)
                        alpha[k + 1] = alpha[0] > alpha[1]
                                           ? uint8_t(((7 - k) * alpha[0] + k * alpha[1]) / 7)
                                           : k < 5 ? uint8_t(((5 - k) * alpha[0] + k * alpha[1]) / 5)
                                                   : uint8_t(k == 5 ? 0 : 255);
                    for (int k = 0; k < 6; ++k)
                        abits |= uint64_t(b[2 + k]) << (8 * k);
                }
                for (uint32_t k = 0; k < 16; ++k) {
                    const uint32_t x = bx * 4 + k % 4, y = by * 4 + k / 4;
                    if (x >= w || y >= h)
                        continue;
                    uint8_t *o = &out[(size_t(y) * w + x) * 4];
                    memcpy(o, pal[bits >> (2 * k) & 3], 4);
                    if (fmt == 0x33545844u) // DXT3
                        o[3] = uint8_t((b[k / 2] >> ((k & 1) * 4) & 15) * 17);
                    else if (fmt == 0x35545844u)
                        o[3] = alpha[abits >> (3 * k) & 7];
                }
            }
        return;
    }
    const uint32_t bpp = format_bytes(fmt);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t at = size_t(y) * s->pitch + size_t(x) * bpp;
            if (at + bpp <= s->blob.size())
                px_to_rgba(fmt, src + at, &out[(size_t(y) * w + x) * 4]);
        }
}
// Encode RGBA (w x h) into a staged copy of `d` (pitch d->pitch).
static void rgba_to_surface(const std::vector<uint8_t> &in, ComObj *d, uint8_t *dst) {
    const uint32_t w = d->width, h = d->height, fmt = d->rmask;
    if (uint32_t block = dxt_block_bytes(fmt)) {
        // Endpoints at the block's colour bounding box: plain, but never wrong
        // in kind, and these copies are textures the game already ships.
        const uint32_t bw = std::max((w + 3) / 4, 1u), bh = std::max((h + 3) / 4, 1u);
        for (uint32_t by = 0; by < bh; ++by)
            for (uint32_t bx = 0; bx < bw; ++bx) {
                uint8_t px[16][4], lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0};
                for (uint32_t k = 0; k < 16; ++k) {
                    const uint32_t x = std::min(bx * 4 + k % 4, w - 1), y = std::min(by * 4 + k / 4, h - 1);
                    memcpy(px[k], &in[(size_t(y) * w + x) * 4], 4);
                    for (int c = 0; c < 4; ++c) {
                        lo[c] = std::min(lo[c], px[k][c]);
                        hi[c] = std::max(hi[c], px[k][c]);
                    }
                }
                uint8_t *b = dst + size_t(by) * d->pitch + size_t(bx) * block;
                uint8_t *cb = block == 8 ? b : b + 8;
                uint16_t c0, c1;
                dxt_color_565(hi, c0);
                dxt_color_565(lo, c1);
                if (c0 < c1)
                    std::swap(c0, c1);
                uint8_t pal[4][4];
                unpack_565(c0, pal[0]);
                unpack_565(c1, pal[1]);
                for (int c = 0; c < 3; ++c) {
                    pal[2][c] = uint8_t((2 * pal[0][c] + pal[1][c]) / 3);
                    pal[3][c] = uint8_t((pal[0][c] + 2 * pal[1][c]) / 3);
                }
                uint32_t bits = 0;
                for (uint32_t k = 0; k < 16; ++k) {
                    int best = 0, bestd = 1 << 30;
                    for (int i = 0; i < 4; ++i) {
                        int dd = 0;
                        for (int c = 0; c < 3; ++c)
                            dd += (px[k][c] - pal[i][c]) * (px[k][c] - pal[i][c]);
                        if (dd < bestd) { bestd = dd; best = i; }
                    }
                    if (c0 == c1)
                        best = 0;
                    bits |= uint32_t(best) << (2 * k);
                }
                cb[0] = uint8_t(c0); cb[1] = uint8_t(c0 >> 8); cb[2] = uint8_t(c1); cb[3] = uint8_t(c1 >> 8);
                memcpy(cb + 4, &bits, 4);
                if (fmt == 0x33545844u) { // DXT3: explicit 4-bit alpha
                    for (uint32_t k = 0; k < 16; k += 2)
                        b[k / 2] = uint8_t((px[k][3] >> 4) | (px[k + 1][3] >> 4) << 4);
                } else if (block == 16) { // DXT5 (DXT2/4 treated alike): 8-step ramp
                    const uint8_t a0 = hi[3], a1 = lo[3];
                    b[0] = a0;
                    b[1] = a1;
                    uint64_t abits = 0;
                    for (uint32_t k = 0; k < 16; ++k) {
                        int idx = 0;
                        if (a0 != a1) {
                            const int t = ((a0 - px[k][3]) * 7 + (a0 - a1) / 2) / (a0 - a1); // 0..7 along a0->a1
                            idx = t <= 0 ? 0 : t >= 7 ? 1 : t + 1;
                        }
                        abits |= uint64_t(idx) << (3 * k);
                    }
                    for (int k = 0; k < 6; ++k)
                        b[2 + k] = uint8_t(abits >> (8 * k));
                }
            }
        return;
    }
    const uint32_t bpp = format_bytes(fmt);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            rgba_to_px(fmt, &in[(size_t(y) * w + x) * 4], dst + size_t(y) * d->pitch + size_t(x) * bpp);
}
// Returns 0 (D3D_OK) or an HRESULT.
uint32_t d3d9_load_surface(uint32_t dst_view, uint32_t src_view, uint32_t color_key) {
    ComObj *d = dst_view ? com_this(dst_view) : nullptr, *s = src_view ? com_this(src_view) : nullptr;
    if (!d || !s || d->kind != K_D3D9SURFACE || s->kind != K_D3D9SURFACE)
        return D3DERR_INVALIDCALL;
    if (!rgba_format_known(d->rmask) || !rgba_format_known(s->rmask) || s->blob.empty()) {
        log_once("d3dx9.loadsurface.fmt", "d3dx9: D3DXLoadSurfaceFromSurface: format %08x -> %08x not supported",
                 s->rmask, d->rmask);
        return 0x8876086au; // D3DERR_NOTAVAILABLE
    }
    const uint32_t staged = stage_lock(d);
    if (!staged)
        return E_OUTOFMEMORY;
    uint8_t *dst = gm_ptr(staged);
    if (s->rmask == d->rmask && s->width == d->width && s->height == d->height && !color_key) {
        const uint32_t rows = dds_rows(d->rmask, d->height);
        const size_t row = std::min<size_t>(dds_row_bytes(d->rmask, d->width), std::min(s->pitch, d->pitch));
        for (uint32_t r = 0; r < rows; ++r)
            if (size_t(r) * s->pitch + row <= s->blob.size())
                memcpy(dst + size_t(r) * d->pitch, s->blob.data() + size_t(r) * s->pitch, row);
    } else {
        std::vector<uint8_t> rgba, scaled;
        surface_to_rgba(s, rgba);
        if (color_key)
            for (size_t i = 0; i < rgba.size(); i += 4) {
                const uint32_t argb = uint32_t(rgba[i + 3]) << 24 | rgba[i] << 16 | rgba[i + 1] << 8 | rgba[i + 2];
                if (argb == color_key)
                    rgba[i] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0;
            }
        if (s->width != d->width || s->height != d->height) { // nearest neighbour
            scaled.resize(size_t(d->width) * d->height * 4);
            for (uint32_t y = 0; y < d->height; ++y)
                for (uint32_t x = 0; x < d->width; ++x) {
                    const uint32_t sx = uint32_t(uint64_t(x) * s->width / d->width),
                                   sy = uint32_t(uint64_t(y) * s->height / d->height);
                    memcpy(&scaled[(size_t(y) * d->width + x) * 4], &rgba[(size_t(sy) * s->width + sx) * 4], 4);
                }
            rgba.swap(scaled);
        }
        rgba_to_surface(rgba, d, dst);
    }
    stage_unlock(d);
    return 0;
}
static void VolTex_LockBox(X86 *c) {
    volume_lock(c, volume_level(com_this_arg(c), arg(c, 1)), arg(c, 2), arg(c, 3), arg(c, 4));
}
static void VolTex_UnlockBox(X86 *c) {
    volume_unlock(c, volume_level(com_this_arg(c), arg(c, 1)));
}
static void Vol_LockBox(X86 *c) {
    volume_lock(c, com_this_arg(c), arg(c, 1), arg(c, 2), arg(c, 3));
}
static void Vol_UnlockBox(X86 *c) {
    volume_unlock(c, com_this_arg(c));
}
static void volume_desc(X86 *c, ComObj *v, uint32_t out) {
    if (!v || !out || !gm_valid(out, 28)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, v->rmask);
    wr32(out + 4, 2); // D3DRTYPE_VOLUME
    wr32(out + 8, resource_usage()[v->id]);
    wr32(out + 12, volumes().at(v->id).pool);
    wr32(out + 16, v->width);
    wr32(out + 20, v->height);
    wr32(out + 24, volumes().at(v->id).depth);
    com_ret(c, D3D_OK9);
}
static void VolTex_GetLevelDesc(X86 *c) {
    volume_desc(c, volume_level(com_this_arg(c), arg(c, 1)), arg(c, 2));
}
static void Vol_GetDesc(X86 *c) {
    volume_desc(c, com_this_arg(c), arg(c, 1));
}
static void VolTex_GetVolumeLevel(X86 *c) {
    ComObj *v = volume_level(com_this_arg(c), arg(c, 1));
    const uint32_t out = arg(c, 2);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    if (!v) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t view = com_view(v, IF_D3DVOLUME9);
    if (!view) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    volume_addref(v);
    wr32(out, view);
    com_ret(c, D3D_OK9);
}
static void Vol_GetContainer(X86 *c) {
    ComObj *v = com_this_arg(c), *tex = v ? com_get(v->front_obj) : nullptr;
    const uint32_t iid = arg(c, 1), out = arg(c, 2);
    if (!tex || !iid || !gm_valid(iid, 16) || !out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, 0);
    static const uint8_t unknown[16] = IID_BYTES(0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46);
    if (!volume_base_iid(iid) && memcmp(gm_ptr(iid), IID_VolumeTexture, 16) &&
        memcmp(gm_ptr(iid), unknown, 16)) {
        com_ret(c, E_NOINTERFACE);
        return;
    }
    wr32(out, tex->identity);
    com_addref(tex);
    com_ret(c, S_OK);
}
static void Vol_GetDevice(X86 *c) {
    const uint32_t out = arg(c, 1);
    if (!out || !gm_valid(out, 4)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    Res_GetDevice(c);
}
static void VolTex_GetType(X86 *c) {
    com_ret(c, 4);
}
static void VolTex_GetLevelCount(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? uint32_t(o->surfaces.size()) : 0);
}
static void VolTex_PreLoad(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (o && volumes().at(o->id).pool <= 1 && gpu_on())
        gpu_sync(o);
    com_ret(c, 0);
}
static void VolTex_SetLOD(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o || volumes().at(o->id).pool != 1) {
        com_ret(c, 0);
        return;
    }
    auto &v = volumes().at(o->id);
    const uint32_t old = v.lod;
    v.lod = std::min(arg(c, 1), uint32_t(o->surfaces.size() - 1));
    com_ret(c, old);
}
static void VolTex_GetLOD(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? volumes().at(o->id).lod : 0);
}
static void VolTex_SetPriority(X86 *c) {
    ComObj *o = com_this_arg(c);
    if (!o) {
        com_ret(c, 0);
        return;
    }
    auto &v = volumes().at(o->id);
    const uint32_t old = v.priority;
    v.priority = arg(c, 1);
    com_ret(c, old);
}
static void VolTex_GetPriority(X86 *c) {
    ComObj *o = com_this_arg(c);
    com_ret(c, o ? volumes().at(o->id).priority : 0);
}
static void VolTex_AddDirtyBox(X86 *c) {
    ComObj *o = com_this_arg(c);
    uint32_t offset;
    if (!volume_box(volume_level(o, 0), arg(c, 1), offset)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    auto it = gpu_mirrors().find(o->id);
    if (it != gpu_mirrors().end())
        std::fill(it->second.cpu_dirty.begin(), it->second.cpu_dirty.end(), 1);
    com_ret(c, D3D_OK9);
}
static void Vol_Unavailable(X86 *c) {
    com_ret(c, D3DERR_NOTAVAILABLE);
}
static void Vol_NoAutoFilter(X86 *c) {
    com_ret(c, 0);
}
// No autogeneration is advertised or admitted at creation. This void method
// cannot fabricate a mip chain; each allocated level must be uploaded by the caller.
static void Vol_NoGenerate(X86 *c) {
    com_ret(c, 0);
}
void Dev_UpdateTexture(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 2));
    if ((!src || src->kind != K_D3D9VOLUMETEXTURE) && (!dst || dst->kind != K_D3D9VOLUMETEXTURE)) {
        log_once("d3d9.UpdateTexture", "d3d9: non-volume UpdateTexture is not implemented");
        com_ret(c, D3D_OK9);
        return;
    }
    if (!this_device9(c) || !src || !dst || src->kind != K_D3D9VOLUMETEXTURE ||
        dst->kind != K_D3D9VOLUMETEXTURE || src->dev_d3d != this_device9(c)->id ||
        dst->dev_d3d != src->dev_d3d || volumes().at(src->id).pool != 2 ||
        volumes().at(dst->id).pool != 0 || src->rmask != dst->rmask || src->width != dst->width ||
        src->height != dst->height || volumes().at(src->id).depth != volumes().at(dst->id).depth ||
        src->surfaces.size() != dst->surfaces.size()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    for (uint32_t l = 0; l < src->surfaces.size(); ++l)
        if (volume_level(src, l)->lock_count || volume_level(dst, l)->lock_count) {
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
    for (uint32_t l = 0; l < src->surfaces.size(); ++l) {
        ComObj *v = volume_level(dst, l);
        v->blob = volume_level(src, l)->blob;
        gpu_after_unlock(v, 0, uint32_t(v->blob.size()));
    }
    com_ret(c, D3D_OK9);
}

// D3DQUERYTYPE: 8 is EVENT, 9 OCCLUSION. An occlusion query with a GPU
// renderer counts real samples, and an event is done once the GPU has
// finished what came before it (Heroes V paces its NOOVERWRITE vertex
// buffer by events: done early, it overwrites vertices a frame in flight
// still reads). Without a GPU renderer, or for any other type, the query is
// finished at once, and an occlusion count is reported as one sample so that
// nothing the game gates on visibility disappears.
static const uint32_t D3DQUERYTYPE_EVENT = 8, D3DQUERYTYPE_OCCLUSION = 9;
static const uint32_t S_FALSE9 = 1;
static std::unordered_map<uint32_t, uint32_t> &query_state() { // id -> 1 begun, 2 ended
    static auto *m = new std::unordered_map<uint32_t, uint32_t>();
    return *m;
}
static void query_destroy(ComObj *o) {
    if (query_state().erase(o->id)) {
        if (o->dev_type == D3DQUERYTYPE_EVENT)
            host_d9_event_drop(o->id);
        else
            host_d9_query_drop(o->id);
    }
}
void Query_GetType9(X86 *c) {
    ComObj *q = com_this_arg(c);
    set_eax(c, q ? q->dev_type : 0);
}
void Query_GetDataSize(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t type = q ? q->dev_type : 0;
    set_eax(c, (type == D3DQUERYTYPE_OCCLUSION || type == D3DQUERYTYPE_EVENT) ? 4 : 0);
}
// RECOMP_D3D9_QUERY (diagnostic): "trace" logs, once a second, how many
// occlusion GetData calls found their result not ready yet, or zero samples;
// "visible" answers every occlusion query at once with one sample, as the
// renderer-less path does, so nothing the game gates on visibility is culled.
static int query_mode() {
    static const int mode = [] {
        const char *v = recomp_env("D3D9_QUERY");
        return !v ? 0 : !strcmp(v, "trace") ? 1 : !strcmp(v, "visible") ? 2 : 0;
    }();
    return mode;
}
static struct {
    uint64_t calls, pending, zero, issued;
} g_query_stats;
static void query_stats_tick() {
    static uint64_t t0 = os_monotonic_ns();
    const uint64_t now = os_monotonic_ns();
    if (now - t0 < 1000000000ull)
        return;
    fprintf(stderr, "d3d9 query: %llu issued, %llu GetData, %llu not ready, %llu zero samples\n",
            (unsigned long long)g_query_stats.issued, (unsigned long long)g_query_stats.calls,
            (unsigned long long)g_query_stats.pending, (unsigned long long)g_query_stats.zero);
    g_query_stats = {};
    t0 = now;
}
// (this, dwIssueFlags): D3DISSUE_END 1, D3DISSUE_BEGIN 2.
void Query_Issue(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t flags = arg(c, 1);
    if (q && q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on()) {
        if (flags & 2) {
            host_d9_query_begin(q->id);
            query_state()[q->id] = 1;
        } else if (flags & 1) {
            host_d9_query_end(q->id);
            query_state()[q->id] = 2;
            g_query_stats.issued += query_mode() == 1;
        }
    } else if (q && q->dev_type == D3DQUERYTYPE_EVENT && gpu_on() && (flags & 1)) {
        host_d9_event_end(q->id);
        query_state()[q->id] = 2;
    }
    com_ret(c, q ? D3D_OK9 : D3DERR_INVALIDCALL);
}
// (this, pData, dwSize, dwGetDataFlags)
void Query_GetData(X86 *c) {
    ComObj *q = com_this_arg(c);
    uint32_t data = arg(c, 1), size = arg(c, 2);
    if (!q) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint32_t value = 1;
    if (q->dev_type == D3DQUERYTYPE_EVENT && gpu_on() && query_state().count(q->id) &&
        !host_d9_event_done(q->id)) {
        com_ret(c, S_FALSE9); // not yet
        return;
    }
    if (q->dev_type == D3DQUERYTYPE_OCCLUSION && gpu_on() && query_mode() != 2) {
        auto it = query_state().find(q->id);
        if (it != query_state().end()) {
            uint32_t count = 0;
            int r = it->second == 2 ? host_d9_query_result(q->id, &count) : 0;
            if (query_mode() == 1) {
                g_query_stats.calls++;
                g_query_stats.pending += r == 0;
                g_query_stats.zero += r > 0 && count == 0;
                query_stats_tick();
            }
            if (r == 0) {
                com_ret(c, S_FALSE9); // not yet
                return;
            }
            value = r > 0 ? count : 1;
        }
    }
    if (data && size >= 4)
        wr32(data, value);
    com_ret(c, D3D_OK9);
}

#define RESOURCE_HEAD                                                                              \
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},                          \
        {"Release", 1, com_Release}, {"GetDevice", 2, Res_GetDevice},                              \
        {"SetPrivateData", 5, Res_SetPrivateData}, {"GetPrivateData", 4, Res_GetPrivateData},      \
        {"FreePrivateData", 2, Res_FreePrivateData}, {"SetPriority", 2, Res_SetPriority},          \
        {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad}, {                        \
        "GetType", 1, Res_GetType                                                                  \
    }

static const ComMethod g_vb9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, VB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static const ComMethod g_ib9[] = {
    RESOURCE_HEAD,
    {"Lock", 5, IB_Lock},
    {"Unlock", 1, Buf_Unlock},
    {"GetDesc", 2, Buf_GetDesc},
};

static void Surf_AddRef(X86 *c) {
    ComObj *s = com_this_arg(c);
    surface_addref(s);
    com_ret(c, s ? (uint32_t)(surface_container(s) ? s->refs - 1 : s->refs) : 0);
}
static void Surf_Release(X86 *c) {
    com_ret(c, (uint32_t)surface_release(com_this_arg(c)));
}
static void Surf_QueryInterface(X86 *c) {
    ComObj *s = com_this_arg(c);
    const bool first_outside = s && surface_container(s) && s->refs == 1;
    com_QueryInterface(c);
    if (first_outside && c->r[0] == S_OK)
        com_addref(com_get(s->front_obj));
}
static const ComMethod g_surface9[] = {
    {"QueryInterface", 3, Surf_QueryInterface}, {"AddRef", 1, Surf_AddRef}, {"Release", 1, Surf_Release},
    {"GetDevice", 2, Res_GetDevice}, {"SetPrivateData", 5, Res_SetPrivateData},
    {"GetPrivateData", 4, Res_GetPrivateData}, {"FreePrivateData", 2, Res_FreePrivateData},
    {"SetPriority", 2, Res_SetPriority}, {"GetPriority", 1, Res_GetPriority}, {"PreLoad", 1, Res_PreLoad},
    {"GetType", 1, Res_GetType},
    {"GetContainer", 3, Surf_GetContainer},
    {"GetDesc", 2, Surf_GetDesc},
    {"LockRect", 4, Surf_LockRect},
    {"UnlockRect", 1, Surf_UnlockRect},
    {"GetDC", 2, Surf_GetDC},
    {"ReleaseDC", 2, Surf_ReleaseDC},
};

static const ComMethod g_texture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetSurfaceLevel", 3, Tex_GetSurfaceLevel},
    {"LockRect", 5, Tex_LockRect},
    {"UnlockRect", 2, Tex_UnlockRect},
    {"AddDirtyRect", 2, Tex_AddDirtyRect},
};

static const ComMethod g_volume_texture9[] = {
    {"QueryInterface", 3, VolTex_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Vol_GetDevice},
    {"SetPrivateData", 5, Vol_Unavailable},
    {"GetPrivateData", 4, Vol_Unavailable},
    {"FreePrivateData", 2, Vol_Unavailable},
    {"SetPriority", 2, VolTex_SetPriority},
    {"GetPriority", 1, VolTex_GetPriority},
    {"PreLoad", 1, VolTex_PreLoad},
    {"GetType", 1, VolTex_GetType},
    {"SetLOD", 2, VolTex_SetLOD},
    {"GetLOD", 1, VolTex_GetLOD},
    {"GetLevelCount", 1, VolTex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Vol_Unavailable},
    {"GetAutoGenFilterType", 1, Vol_NoAutoFilter},
    {"GenerateMipSubLevels", 1, Vol_NoGenerate},
    {"GetLevelDesc", 3, VolTex_GetLevelDesc},
    {"GetVolumeLevel", 3, VolTex_GetVolumeLevel},
    {"LockBox", 5, VolTex_LockBox},
    {"UnlockBox", 2, VolTex_UnlockBox},
    {"AddDirtyBox", 2, VolTex_AddDirtyBox},
};
static const ComMethod g_volume9[] = {
    {"QueryInterface", 3, Vol_QueryInterface},
    {"AddRef", 1, Vol_AddRef},
    {"Release", 1, Vol_Release},
    {"GetDevice", 2, Vol_GetDevice},
    {"SetPrivateData", 5, Vol_Unavailable},
    {"GetPrivateData", 4, Vol_Unavailable},
    {"FreePrivateData", 2, Vol_Unavailable},
    {"GetContainer", 3, Vol_GetContainer},
    {"GetDesc", 2, Vol_GetDesc},
    {"LockBox", 4, Vol_LockBox},
    {"UnlockBox", 1, Vol_UnlockBox},
};

static const ComMethod g_cubetexture9[] = {
    RESOURCE_HEAD,
    {"SetLOD", 2, Tex_SetLOD},
    {"GetLOD", 1, Tex_GetLOD},
    {"GetLevelCount", 1, Tex_GetLevelCount},
    {"SetAutoGenFilterType", 2, Tex_SetAutoGenFilterType},
    {"GetAutoGenFilterType", 1, Tex_GetAutoGenFilterType},
    {"GenerateMipSubLevels", 1, Tex_GenerateMipSubLevels},
    {"GetLevelDesc", 3, Tex_GetLevelDesc},
    {"GetCubeMapSurface", 4, Tex_GetCubeMapSurface},
    {"LockRect", 6, Tex_LockRectCube},
    {"UnlockRect", 3, Tex_UnlockRectCube},
    {"AddDirtyRect", 3, Tex_AddDirtyRectCube},
};

static const ComMethod g_decl9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetDeclaration", 3, Decl_GetDeclaration},
};

static const ComMethod g_query9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"GetDevice", 2, Res_GetDevice},
    {"GetType", 1, Query_GetType9},
    {"GetDataSize", 1, Query_GetDataSize},
    {"Issue", 2, Query_Issue},
    {"GetData", 4, Query_GetData},
};

// ---------------------------------------------------------------------------
// The pipeline record, and the device setters that write it
// ---------------------------------------------------------------------------
D9Pipeline &d9_pipeline(uint32_t device_id) {
    // Records are never erased and map nodes do not move, so the last one
    // asked for can be handed out again without a lookup.
    static auto *records = new std::map<uint32_t, D9Pipeline>();
    static thread_local uint32_t last_id = 0;
    static thread_local D9Pipeline *last = nullptr;
    if (last && last_id == device_id)
        return *last;
    last = &(*records)[device_id];
    last_id = device_id;
    return *last;
}

// ---- Getters for state a caller saves and puts back (Direct3D 9 semantics) ----
// (this, State, pValue)
void Dev_GetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || state >= 256) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).rs[state]);
    com_ret(c, D3D_OK9);
}
// (this, Sampler, Type, pValue)
void Dev_GetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || !out || sampler >= 16 || type >= 14) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).sampler_state[sampler][type]);
    com_ret(c, D3D_OK9);
}
// (this, RenderTargetIndex, ppRenderTarget): referenced, as D3D hands it out; NOTFOUND for an
// unset target above zero.
void Dev_GetRenderTarget(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t index = arg(c, 1), out = arg(c, 2);
    if (!dev || !out || index > 3) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *s = com_get(index == 0 ? dev->render_target : d9_pipeline(dev->id).color_target[index]);
    if (index == 0 && !s)
        s = device_backbuffer(dev);
    if (!s) {
        wr32(out, 0);
        com_ret(c, D3DERR_NOTFOUND9);
        return;
    }
    uint32_t view_ = com_view(s, IF_D3DSURFACE9);
    if (!view_) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    surface_addref(s); // a level shares its texture's lifetime (Test159)
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
// (this, ppDecl): referenced; null when none is set.
void Dev_GetVertexDeclaration(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *d = com_get(d9_pipeline(dev->id).declaration);
    uint32_t view_ = d && d->kind == K_D3D9DECL ? com_view(d, IF_D3DVERTEXDECL9) : 0;
    if (view_)
        com_addref(d);
    wr32(out, view_);
    com_ret(c, D3D_OK9);
}

// GetRenderTargetData and its LockRect, bounded: the copy is queued behind the draws that made
// the image and delivered when the GPU has run it (host_d9_texture_read_async); a
// D3DLOCK_DONOTWAIT lock before then answers D3DERR_WASSTILLDRAWING instead of waiting. A lock
// without that flag waits, through the synchronous read, as Direct3D 9 does. The destination is
// the ticket.
static std::unordered_map<uint32_t, HostD9Surface> &readbacks_pending() {
    static std::unordered_map<uint32_t, HostD9Surface> r;
    return r;
}
static void readback_forget(ComObj *o) {
    if (o && readbacks_pending().erase(o->id))
        host_d9_texture_read_forget(o->id);
}
static bool readback_settle(ComObj *s, uint32_t flags) {
    if (!s)
        return true;
    auto it = readbacks_pending().find(s->id);
    if (it == readbacks_pending().end() || s->blob.empty())
        return true;
    int r = host_d9_texture_read_poll(s->id, s->blob.data(), s->pitch, s->width, s->height);
    if (r == 0 && (flags & D3DLOCK_DONOTWAIT))
        return false;
    if (r == 0) // wait: the synchronous read of the same image
        host_d9_texture_read(it->second.id, it->second.face, it->second.level, s->blob.data(),
                             s->pitch);
    host_d9_texture_read_forget(s->id);
    readbacks_pending().erase(it);
    d9_raster_invalidate(s->id);
    return true;
}
// (this, pRenderTarget, pDestSurface)
void Dev_GetRenderTargetData(X86 *c) {
    ComObj *src = com_this(arg(c, 1)), *dst = com_this(arg(c, 2));
    if (!src || !dst || src->kind != K_D3D9SURFACE || dst->kind != K_D3D9SURFACE ||
        src->width != dst->width || src->height != dst->height || dst->bpp != 32 ||
        dst->blob.empty()) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (gpu_on()) {
        uint32_t slot;
        if (ComObj *owner = com_get(mirror_slot(src, &slot)))
            gpu_sync(owner);
        const HostD9Surface r = surface_ref(src);
        readbacks_pending()[dst->id] = r;
        host_d9_texture_read_async(r.id, r.face, r.level, dst->id);
        com_ret(c, D3D_OK9);
        return;
    }
    const uint8_t *sb = resource_bytes(src);
    uint8_t *db = resource_bytes(dst);
    if (sb && db && src->bpp == 32)
        for (uint32_t y = 0; y < dst->height; ++y)
            memcpy(db + (size_t)y * dst->pitch, sb + (size_t)y * src->pitch, (size_t)dst->width * 4);
    d9_raster_invalidate(dst->id);
    com_ret(c, D3D_OK9);
}

// (this, State, Value)
// (this, pViewport): D3DVIEWPORT9 is X, Y, Width, Height, MinZ, MaxZ.
void Dev_SetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        for (int i = 0; i < 4; ++i)
            p.viewport[i] = rd32(v + 4u * (uint32_t)i);
        uint32_t b0 = rd32(v + 16), b1 = rd32(v + 20);
        memcpy(&p.viewport_z[0], &b0, 4);
        memcpy(&p.viewport_z[1], &b1, 4);
        p.viewport_set = true;
    }
    com_ret(c, D3D_OK9);
}
void Dev_GetViewport(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t v = arg(c, 1);
    if (dev && v) {
        D9Pipeline &p = d9_pipeline(dev->id);
        ComObj *rt = com_get(dev->render_target);
        uint32_t whole[4] = {0, 0, rt ? rt->width : 640, rt ? rt->height : 480};
        for (int i = 0; i < 4; ++i)
            wr32(v + 4u * (uint32_t)i, p.viewport_set ? p.viewport[i] : whole[i]);
        float z0 = p.viewport_set ? p.viewport_z[0] : 0.0f,
              z1 = p.viewport_set ? p.viewport_z[1] : 1.0f;
        uint32_t b0, b1;
        memcpy(&b0, &z0, 4);
        memcpy(&b1, &z1, 4);
        wr32(v + 16, b0);
        wr32(v + 20, b1);
    }
    com_ret(c, D3D_OK9);
}

// (this, State, pMatrix): world, view, projection and texture transforms,
// kept for the fixed-function path.
void Dev_SetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1), m = arg(c, 2);
    if (dev && m) {
        uint32_t slot =
            state < 256 ? state : (state >= 256 && state < 260 ? state - 256 + 24 : 0xffffffffu);
        if (slot < 32)
            for (int i = 0; i < 16; ++i) {
                uint32_t b = rd32(m + 4u * (uint32_t)i);
                memcpy(&d9_pipeline(dev->id).transform[slot][i], &b, 4);
            }
    }
    com_ret(c, D3D_OK9);
}

// (this, Stage, Type, Value)
void Dev_SetTextureStageState(X86 *c) {
    g_frame_stats.tss_calls++;
    ComObj *dev = this_device9(c);
    uint32_t stage = arg(c, 1), type = arg(c, 2);
    if (dev && stage < 8 && type < 33)
        d9_pipeline(dev->id).tss[stage][type] = arg(c, 3);
    com_ret(c, D3D_OK9);
}

void Dev_SetRenderState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t state = arg(c, 1);
    g_frame_stats.rs_calls++;
    if (dev && state < 256) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.rs[state] != arg(c, 2) || !p.rs_set[state]) {
            g_frame_stats.rs_changes++;
            p.rs[state] = arg(c, 2);
            p.rs_set[state] = true;
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// (this, Sampler, Type, Value)
void Dev_SetSamplerState(X86 *c) {
    ComObj *dev = this_device9(c);
    uint32_t sampler = arg(c, 1), type = arg(c, 2);
    if (dev && sampler < 16 && type < 14) {
        D9Pipeline &p = d9_pipeline(dev->id);
        if (p.sampler_state[sampler][type] != arg(c, 3)) {
            g_frame_stats.sampler_changes++;
            p.sampler_state[sampler][type] = arg(c, 3);
            p.states_changed();
        }
    }
    com_ret(c, D3D_OK9);
}

// Copies Count four-float vectors from guest memory into a register table,
// clipped to the table.
static void write_registers(float (*table)[4], uint32_t limit, uint32_t start, uint32_t data,
                            uint32_t count) {
    for (uint32_t i = 0; i < count && start + i < limit && data; ++i)
        for (uint32_t k = 0; k < 4; ++k) {
            uint32_t bits = rd32(data + (i * 4 + k) * 4);
            memcpy(&table[start + i][k], &bits, 4);
        }
}

// (this, StartRegister, pConstantData, Vector4fCount)
void Dev_SetVertexShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        g_frame_stats.vconst_vecs += arg(c, 3);
        pl.vbinder = {}; // the registers no longer hold only what an effect bound
        write_registers(pl.vconst, 256, arg(c, 1), arg(c, 2), arg(c, 3));
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetPixelShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    if (dev) {
        D9Pipeline &pl = d9_pipeline(dev->id);
        g_frame_stats.pconst_vecs += arg(c, 3);
        pl.pbinder = {};
        write_registers(pl.pconst, 224, arg(c, 1), arg(c, 2), arg(c, 3));
    }
    com_ret(c, D3D_OK9);
}

// BOOL is a 32-bit guest value, not a C++ bool. Validate the entire range
// before reading or writing anything so an invalid call cannot partly apply.
static void shader_booleans(X86 *c, bool pixel, bool get) {
    ComObj *dev = this_device9(c);
    const uint32_t start = arg(c, 1), data = arg(c, 2), count = arg(c, 3);
    if (!dev || start > 16 || count > 16 - start ||
        (count && (!data || !gm_fits(data, count * 4)))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    int32_t *table = pixel ? pl.pbool : pl.vbool;
    if (!get)
        (pixel ? pl.pbinder : pl.vbinder) = {};
    for (uint32_t i = 0; i < count; ++i) {
        if (get)
            wr32(data + i * 4, (uint32_t)table[start + i]);
        else
            table[start + i] = (int32_t)rd32(data + i * 4);
    }
    com_ret(c, D3D_OK9);
}
void Dev_SetVertexShaderConstantB(X86 *c) {
    shader_booleans(c, false, false);
}
void Dev_GetVertexShaderConstantB(X86 *c) {
    shader_booleans(c, false, true);
}
void Dev_SetPixelShaderConstantB(X86 *c) {
    shader_booleans(c, true, false);
}
void Dev_GetPixelShaderConstantB(X86 *c) {
    shader_booleans(c, true, true);
}

// ---------------------------------------------------------------------------
// Shaders made from bytecode, and the fixed-function state
// ---------------------------------------------------------------------------
// {EFC5557E-6265-4613-8A94-43857889EB36} and {6D3BDBDC-5B02-4415-B852-CE5E8BCCB289}
static const uint8_t IID_VertexShader9[16] =
    IID_BYTES(0xefc5557e, 0x6265, 0x4613, 0x8a, 0x94, 0x43, 0x85, 0x78, 0x89, 0xeb, 0x36);
static const uint8_t IID_PixelShader9[16] =
    IID_BYTES(0x6d3bdbdc, 0x5b02, 0x4415, 0xb8, 0x52, 0xce, 0x5e, 0x8b, 0xcc, 0xb2, 0x89);

// A shader object's bytecode, shared with every pipeline that binds it, by object id.
static std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>> &shader_code() {
    static auto *m = new std::unordered_map<uint32_t, std::shared_ptr<const std::vector<uint8_t>>>();
    return *m;
}
// Each shader's code key, hashed once when it is created: the game switches
// shaders some 700 times a frame, and hashing the bytecode at every switch
// was a draw path cost of its own.
static std::unordered_map<uint32_t, uint64_t> &shader_keys() {
    static auto *m = new std::unordered_map<uint32_t, uint64_t>();
    return *m;
}
static void shader_destroy(ComObj *o) {
    shader_keys().erase(o->id);
    shader_code().erase(o->id);
}

// The length in bytes of the shader at `p`, through its end token, or 0 when
// it is not one. The token stream carries no total size: comments say how long
// they are, shader model 2 and later instructions too, and a 1.x parameter
// token always has its top bit set, so the end token cannot be mistaken.
static uint32_t shader_length(uint32_t p) {
    if (!p || !gm_fits(p, 8))
        return 0;
    const uint32_t version = rd32(p);
    if ((version >> 16) != 0xfffe && (version >> 16) != 0xffff)
        return 0;
    const bool sm2 = ((version >> 8) & 0xff) >= 2;
    const uint32_t limit = 1u << 20; // tokens; a real program is a few thousand
    for (uint32_t i = 1; i < limit && gm_fits(p + 4 * i, 4);) {
        const uint32_t t = rd32(p + 4 * i);
        if (t == 0x0000ffffu)
            return 4 * (i + 1);
        if ((t & 0xffff) == 0xfffe) { // comment
            i += 1 + ((t >> 16) & 0x7fff);
            continue;
        }
        i += (sm2 && !(t & 0x80000000u)) ? 1 + ((t >> 24) & 0xf) : 1;
    }
    return 0;
}

// (this, pFunction, ppShader)
static void create_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t code = arg(c, 1), out = arg(c, 2);
    const uint32_t size = shader_length(code);
    if (!dev || !out || !size || (rd32(code) >> 16 == 0xffff) != pixel) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *o = com_new(pixel ? K_D3D9PSHADER : K_D3D9VSHADER);
    if (!o) {
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    o->dev_d3d = dev->id;
    auto bytes = std::make_shared<std::vector<uint8_t>>(gm_ptr(code), gm_ptr(code) + size);
    const d9sh::Program &p = d9sh::program_for(*bytes);
    static uint32_t made[2];
    if (++made[pixel] <= 4 || !p.ok)
        LOGW("d3d9: %s shader %u: %s_%u_%u, %u bytes%s%s", pixel ? "pixel" : "vertex", o->id,
             pixel ? "ps" : "vs", (rd32(code) >> 8) & 0xff, rd32(code) & 0xff, size,
             p.ok ? "" : ", not decoded: ", p.ok ? "" : p.why.c_str());
    shader_keys()[o->id] = bytes ? d9sh::code_key(bytes->data(), bytes->size()) : 0;
    shader_code()[o->id] = std::move(bytes);
    uint32_t view_ = com_view(o, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9);
    if (!view_) {
        com_release(o);
        com_ret(c, E_OUTOFMEMORY);
        return;
    }
    com_out_ptr(out, view_);
    com_ret(c, D3D_OK9);
}
void Dev_CreateVertexShader(X86 *c) {
    create_shader(c, false);
}
void Dev_CreatePixelShader(X86 *c) {
    create_shader(c, true);
}

// (this, pShader): binds the program, or the fixed-function stage for null. The device
// references the shader it holds, as Direct3D 9 does, so the game may release its own.
static void set_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    ComObj *o = com_this(arg(c, 1));
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    if (o && o->kind != (pixel ? K_D3D9PSHADER : K_D3D9VSHADER))
        o = nullptr;
    D9Pipeline &pl = d9_pipeline(dev->id);
    uint32_t &held = pixel ? pl.ps_object : pl.vs_object;
    if (o)
        com_addref(o);
    if (ComObj *old = com_get(held))
        com_release(old);
    held = o ? o->id : 0;
    D9ShaderBytes &bound = pixel ? pl.ps : pl.vs;
    uint64_t &key = pixel ? pl.ps_key : pl.vs_key;
    auto it = o ? shader_code().find(o->id) : shader_code().end();
    if (it != shader_code().end()) {
        if (bound.bytes != it->second) {
            bound.bytes = it->second;
            auto k = shader_keys().find(o->id);
            key = k != shader_keys().end() ? k->second : d9sh::code_key(bound.data(), bound.size());
        }
    } else {
        bound.bytes.reset();
        key = 0;
    }
    pl.label = nullptr; // no effect pass names these shaders
    com_ret(c, D3D_OK9);
}
void Dev_SetVertexShader(X86 *c) {
    g_frame_stats.shader_sets++;
    set_shader(c, false);
}
void Dev_SetPixelShader(X86 *c) {
    g_frame_stats.shader_sets++;
    set_shader(c, true);
}
// (this, ppShader): referenced; null when the fixed-function stage is bound.
static void get_shader(X86 *c, bool pixel) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const D9Pipeline &pl = d9_pipeline(dev->id);
    ComObj *o = com_get(pixel ? pl.ps_object : pl.vs_object);
    const uint32_t view_ = o ? com_view(o, pixel ? IF_D3DPIXELSHADER9 : IF_D3DVERTEXSHADER9) : 0;
    if (view_)
        com_addref(o);
    wr32(out, view_);
    com_ret(c, D3D_OK9);
}
void Dev_GetVertexShader(X86 *c) {
    get_shader(c, false);
}
void Dev_GetPixelShader(X86 *c) {
    get_shader(c, true);
}
// IDirect3D{Vertex,Pixel}Shader9::GetFunction(this, pData, pSizeOfData): the bytecode, or
// with no buffer just its size.
static void Shader_GetFunction(X86 *c) {
    ComObj *o = com_this_arg(c);
    const uint32_t data = arg(c, 1), size_at = arg(c, 2);
    auto it = o ? shader_code().find(o->id) : shader_code().end();
    if (it == shader_code().end() || !size_at) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const uint32_t size = (uint32_t)it->second->size();
    if (data) {
        if (rd32(size_at) < size || !gm_fits(data, size)) {
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
        memcpy(gm_ptr(data), it->second->data(), size);
    }
    wr32(size_at, size);
    com_ret(c, D3D_OK9);
}

// (this, FVF): the declaration an FVF code describes, made once per code and device and
// kept, bound in place of the game's declaration.
void Dev_SetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t fvf = arg(c, 1);
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    static auto *made = new std::map<std::pair<uint32_t, uint32_t>, uint32_t>();
    D9Pipeline &pl = d9_pipeline(dev->id);
    uint32_t &id = (*made)[{dev->id, fvf}];
    ComObj *d = com_get(id);
    if (!d) {
        std::vector<uint8_t> elements;
        if (!d9ffp::fvf_declaration(fvf, &elements)) {
            if (fvf)
                log_once("d3d9.fvf.noposition", "d3d9: SetFVF(0x%x) has no position; ignored", fvf);
            com_ret(c, fvf ? D3DERR_INVALIDCALL : D3D_OK9);
            return;
        }
        d = com_new(K_D3D9DECL);
        if (!d) {
            com_ret(c, E_OUTOFMEMORY);
            return;
        }
        d->dev_d3d = dev->id;
        d->blob = std::move(elements);
        id = d->id;
    }
    dev->current_viewport = d->id; // the CPU renderer's declaration, as SetVertexDeclaration
    pl.declaration = d->id;
    pl.fvf = fvf;
    com_ret(c, D3D_OK9);
}
void Dev_GetFVF(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).fvf);
    com_ret(c, D3D_OK9);
}

// (this, State, pMatrix)
static float *transform_slot(ComObj *dev, uint32_t state) {
    const uint32_t slot = state < 24 ? state : (state >= 256 && state < 260 ? state - 256 + 24 : 32);
    return slot < 32 ? d9_pipeline(dev->id).transform[slot] : nullptr;
}
void Dev_GetTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t m = arg(c, 2);
    float *t = dev ? transform_slot(dev, arg(c, 1)) : nullptr;
    if (!t || !m || !gm_fits(m, 64)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(m), t, 64);
    com_ret(c, D3D_OK9);
}
// The matrix becomes the current one times pMatrix (Wine's tested order; the documentation's
// wording reads the other way round).
void Dev_MultiplyTransform(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t m = arg(c, 2);
    float *t = dev ? transform_slot(dev, arg(c, 1)) : nullptr;
    if (!t || !m || !gm_fits(m, 64)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    float b[16], r[16];
    memcpy(b, gm_ptr(m), 64);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = t[i * 4] * b[j] + t[i * 4 + 1] * b[4 + j] + t[i * 4 + 2] * b[8 + j] +
                           t[i * 4 + 3] * b[12 + j];
    memcpy(t, r, 64);
    com_ret(c, D3D_OK9);
}

// (this, pMaterial): D3DMATERIAL9, 17 floats.
void Dev_SetMaterial(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t p = arg(c, 1);
    if (!dev || !p || !gm_fits(p, 68)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(d9_pipeline(dev->id).material, gm_ptr(p), 68);
    com_ret(c, D3D_OK9);
}
void Dev_GetMaterial(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t p = arg(c, 1);
    if (!dev || !p || !gm_fits(p, 68)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(p), d9_pipeline(dev->id).material, 68);
    com_ret(c, D3D_OK9);
}
// (this, Index, pLight): D3DLIGHT9, 104 bytes. Any index; only enabled lights shine.
void Dev_SetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), p = arg(c, 2);
    if (!dev || !p || !gm_fits(p, 104)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline::Light &l = d9_pipeline(dev->id).lights[index];
    l.type = rd32(p);
    memcpy(l.f, gm_ptr(p), 104);
    com_ret(c, D3D_OK9);
}
void Dev_GetLight(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), p = arg(c, 2);
    D9Pipeline *pl = dev ? &d9_pipeline(dev->id) : nullptr;
    auto it = pl ? pl->lights.find(index) : decltype(pl->lights.end()){};
    if (!pl || it == pl->lights.end() || !p || !gm_fits(p, 104)) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    memcpy(gm_ptr(p), it->second.f, 104);
    com_ret(c, D3D_OK9);
}
// (this, Index, Enable). Enabling a light never set makes Direct3D's default one: white,
// directional, shining along +z. At most eight shine at once; the ninth is refused.
void Dev_LightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1);
    const bool enable = arg(c, 2) != 0;
    if (!dev) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    D9Pipeline &pl = d9_pipeline(dev->id);
    auto it = pl.lights.find(index);
    if (it == pl.lights.end()) {
        D9Pipeline::Light l;
        l.f[1] = l.f[2] = l.f[3] = 1.0f; // diffuse
        l.f[18] = 1.0f;                  // direction
        memcpy(&l.f[0], &l.type, 4);
        it = pl.lights.emplace(index, l).first;
    }
    if (it->second.enabled == enable) {
        com_ret(c, D3D_OK9);
        return;
    }
    if (enable) {
        if (pl.active_light_count >= 8) {
            log_once("d3d9.ffp.lights", "d3d9: a ninth light enabled at once is refused");
            com_ret(c, D3DERR_INVALIDCALL);
            return;
        }
        pl.active_lights[pl.active_light_count++] = index;
    } else {
        uint32_t n = 0;
        for (uint32_t i = 0; i < pl.active_light_count; ++i)
            if (pl.active_lights[i] != index)
                pl.active_lights[n++] = pl.active_lights[i];
        pl.active_light_count = n;
    }
    it->second.enabled = enable;
    com_ret(c, D3D_OK9);
}
// (this, Index, pEnable): TRUE is any nonzero BOOL; Direct3D 9 writes 128.
void Dev_GetLightEnable(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t index = arg(c, 1), out = arg(c, 2);
    D9Pipeline *pl = dev ? &d9_pipeline(dev->id) : nullptr;
    auto it = pl ? pl->lights.find(index) : decltype(pl->lights.end()){};
    if (!pl || it == pl->lights.end() || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, it->second.enabled ? 128 : 0);
    com_ret(c, D3D_OK9);
}

// (this, Stage, Type, pValue)
void Dev_GetTextureStageState(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stage = arg(c, 1), type = arg(c, 2), out = arg(c, 3);
    if (!dev || stage >= 8 || type >= 33 || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    wr32(out, d9_pipeline(dev->id).tss[stage][type]);
    com_ret(c, D3D_OK9);
}

// (this, StreamNumber, ppStreamData, pOffsetInBytes, pStride)
void Dev_GetStreamSource(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t stream = arg(c, 1), out = arg(c, 2), offset = arg(c, 3), stride = arg(c, 4);
    if (!dev || stream >= 8 || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    const D9Pipeline::Stream &st = d9_pipeline(dev->id).stream[stream];
    ComObj *vb = com_get(st.vb);
    const uint32_t view_ = vb ? com_view(vb, IF_D3DVERTEXBUFFER9) : 0;
    if (view_)
        com_addref(vb);
    wr32(out, view_);
    if (offset)
        wr32(offset, st.offset);
    if (stride)
        wr32(stride, st.stride);
    com_ret(c, D3D_OK9);
}
void Dev_GetIndices(X86 *c) {
    ComObj *dev = this_device9(c);
    const uint32_t out = arg(c, 1);
    if (!dev || !out) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    ComObj *ib = com_get(d9_pipeline(dev->id).index_buffer);
    const uint32_t view_ = ib ? com_view(ib, IF_D3DINDEXBUFFER9) : 0;
    if (view_)
        com_addref(ib);
    wr32(out, view_);
    com_ret(c, D3D_OK9);
}

// Get*ShaderConstantF and Set/Get*ShaderConstantI: (this, StartRegister, pData, Count),
// refused whole when the range leaves the register file. The integer registers are kept
// for the getters; the programs read only their own defi values (d3d9_shadergen.cpp).
static void shader_registers(X86 *c, void *table, uint32_t limit, bool get) {
    ComObj *dev = this_device9(c);
    const uint32_t start = arg(c, 1), data = arg(c, 2), count = arg(c, 3);
    if (!dev || start > limit || count > limit - start || (count && (!data || !gm_fits_n(data, count, 16)))) {
        com_ret(c, D3DERR_INVALIDCALL);
        return;
    }
    uint8_t *rows = (uint8_t *)table + 16u * start;
    if (get)
        memcpy(gm_ptr(data), rows, 16u * count);
    else
        memcpy(rows, gm_ptr(data), 16u * count);
    com_ret(c, D3D_OK9);
}
void Dev_GetVertexShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).vconst : nullptr, 256, true);
}
void Dev_GetPixelShaderConstantF(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).pconst : nullptr, 224, true);
}
void Dev_SetVertexShaderConstantI(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).vint : nullptr, 16, false);
}
void Dev_GetVertexShaderConstantI(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).vint : nullptr, 16, true);
}
void Dev_SetPixelShaderConstantI(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).pint : nullptr, 16, false);
}
void Dev_GetPixelShaderConstantI(X86 *c) {
    ComObj *dev = this_device9(c);
    shader_registers(c, dev ? d9_pipeline(dev->id).pint : nullptr, 16, true);
}

static const ComMethod g_vshader9[] = {
    {"QueryInterface", 3, com_QueryInterface}, {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},               {"GetDevice", 2, Res_GetDevice},
    {"GetFunction", 3, Shader_GetFunction},
};

static const ComMethod g_d3d9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"RegisterSoftwareDevice", 2, D9_RegisterSoftwareDevice},
    {"GetAdapterCount", 1, D9_GetAdapterCount},
    {"GetAdapterIdentifier", 4, D9_GetAdapterIdentifier},
    {"GetAdapterModeCount", 3, D9_GetAdapterModeCount},
    {"EnumAdapterModes", 5, D9_EnumAdapterModes},
    {"GetAdapterDisplayMode", 3, D9_GetAdapterDisplayMode},
    {"CheckDeviceType", 6, D9_CheckDeviceType},
    {"CheckDeviceFormat", 7, D9_CheckDeviceFormat},
    {"CheckDeviceMultiSampleType", 7, D9_CheckDeviceMultiSampleType},
    {"CheckDepthStencilMatch", 6, D9_CheckDepthStencilMatch},
    {"CheckDeviceFormatConversion", 5, D9_CheckDeviceFormatConversion},
    {"GetDeviceCaps", 4, D9_GetDeviceCaps},
    {"GetAdapterMonitor", 2, D9_GetAdapterMonitor},
    {"CreateDevice", 7, D9_CreateDevice},
};

static const ComMethod g_device9[] = {
    {"QueryInterface", 3, com_QueryInterface},
    {"AddRef", 1, com_AddRef},
    {"Release", 1, com_Release},
    {"TestCooperativeLevel", 1, Dev_TestCooperativeLevel},
    {"GetAvailableTextureMem", 1, Dev_GetAvailableTextureMem},
    {"EvictManagedResources", 1, Dev_EvictManagedResources},
    {"GetDirect3D", 2, Dev_GetDirect3D},
    {"GetDeviceCaps", 2, Dev_GetDeviceCaps},
    {"GetDisplayMode", 3, Dev_GetDisplayMode},
    {"GetCreationParameters", 2, Dev_GetCreationParameters},
    {"SetCursorProperties", 4, Dev_SetCursorProperties},
    {"SetCursorPosition", 4, Dev_SetCursorPosition},
    {"ShowCursor", 2, Dev_ShowCursor},
    {"CreateAdditionalSwapChain", 3, Dev_CreateAdditionalSwapChain},
    {"GetSwapChain", 3, Dev_GetSwapChain},
    {"GetNumberOfSwapChains", 1, Dev_GetNumberOfSwapChains},
    {"Reset", 2, Dev_Reset},
    {"Present", 5, Dev_Present},
    {"GetBackBuffer", 5, Dev_GetBackBuffer},
    {"GetRasterStatus", 3, Dev_GetRasterStatus},
    {"SetDialogBoxMode", 2, Dev_SetDialogBoxMode},
    {"SetGammaRamp", 4, Dev_SetGammaRamp},
    {"GetGammaRamp", 3, Dev_GetGammaRamp},
    {"CreateTexture", 9, Dev_CreateTexture},
    {"CreateVolumeTexture", 10, Dev_CreateVolumeTexture},
    {"CreateCubeTexture", 8, Dev_CreateCubeTexture},
    {"CreateVertexBuffer", 7, Dev_CreateVertexBuffer},
    {"CreateIndexBuffer", 7, Dev_CreateIndexBuffer},
    {"CreateRenderTarget", 9, Dev_CreateRenderTarget},
    {"CreateDepthStencilSurface", 9, Dev_CreateDepthStencilSurface},
    {"UpdateSurface", 5, Dev_UpdateSurface},
    {"UpdateTexture", 3, Dev_UpdateTexture},
    {"GetRenderTargetData", 3, Dev_GetRenderTargetData},
    {"GetFrontBufferData", 3, Dev_GetFrontBufferData},
    {"StretchRect", 6, Dev_StretchRect},
    {"ColorFill", 4, Dev_ColorFill},
    {"CreateOffscreenPlainSurface", 7, Dev_CreateOffscreenPlainSurface},
    {"SetRenderTarget", 3, Dev_SetRenderTarget},
    {"GetRenderTarget", 3, Dev_GetRenderTarget},
    {"SetDepthStencilSurface", 2, Dev_SetDepthStencilSurface},
    {"GetDepthStencilSurface", 2, Dev_GetDepthStencilSurface},
    {"BeginScene", 1, Dev_BeginScene},
    {"EndScene", 1, Dev_EndScene},
    {"Clear", 7, Dev_Clear},
    {"SetTransform", 3, Dev_SetTransform},
    {"GetTransform", 3, Dev_GetTransform},
    {"MultiplyTransform", 3, Dev_MultiplyTransform},
    {"SetViewport", 2, Dev_SetViewport},
    {"GetViewport", 2, Dev_GetViewport},
    {"SetMaterial", 2, Dev_SetMaterial},
    {"GetMaterial", 2, Dev_GetMaterial},
    {"SetLight", 3, Dev_SetLight},
    {"GetLight", 3, Dev_GetLight},
    {"LightEnable", 3, Dev_LightEnable},
    {"GetLightEnable", 3, Dev_GetLightEnable},
    {"SetClipPlane", 3, Dev_SetClipPlane},
    {"GetClipPlane", 3, Dev_GetClipPlane},
    {"SetRenderState", 3, Dev_SetRenderState},
    {"GetRenderState", 3, Dev_GetRenderState},
    {"CreateStateBlock", 3, Dev_CreateStateBlock},
    {"BeginStateBlock", 1, Dev_BeginStateBlock},
    {"EndStateBlock", 2, Dev_EndStateBlock},
    {"SetClipStatus", 2, Dev_SetClipStatus},
    {"GetClipStatus", 2, Dev_GetClipStatus},
    {"GetTexture", 3, Dev_GetTexture},
    {"SetTexture", 3, Dev_SetTexture},
    {"GetTextureStageState", 4, Dev_GetTextureStageState},
    {"SetTextureStageState", 4, Dev_SetTextureStageState},
    {"GetSamplerState", 4, Dev_GetSamplerState},
    {"SetSamplerState", 4, Dev_SetSamplerState},
    {"ValidateDevice", 2, Dev_ValidateDevice},
    {"SetPaletteEntries", 3, Dev_SetPaletteEntries},
    {"GetPaletteEntries", 3, Dev_GetPaletteEntries},
    {"SetCurrentTexturePalette", 2, Dev_SetCurrentTexturePalette},
    {"GetCurrentTexturePalette", 2, Dev_GetCurrentTexturePalette},
    {"SetScissorRect", 2, Dev_SetScissorRect},
    {"GetScissorRect", 2, Dev_GetScissorRect},
    {"SetSoftwareVertexProcessing", 2, Dev_SetSoftwareVertexProcessing},
    {"GetSoftwareVertexProcessing", 1, Dev_GetSoftwareVertexProcessing},
    {"SetNPatchMode", 2, Dev_SetNPatchMode},
    {"GetNPatchMode", 1, Dev_GetNPatchMode},
    {"DrawPrimitive", 4, Dev_DrawPrimitive},
    {"DrawIndexedPrimitive", 7, Dev_DrawIndexedPrimitive},
    {"DrawPrimitiveUP", 5, Dev_DrawPrimitiveUP},
    {"DrawIndexedPrimitiveUP", 9, Dev_DrawIndexedPrimitiveUP},
    {"ProcessVertices", 7, Dev_ProcessVertices},
    {"CreateVertexDeclaration", 3, Dev_CreateVertexDeclaration},
    {"SetVertexDeclaration", 2, Dev_SetVertexDeclaration},
    {"GetVertexDeclaration", 2, Dev_GetVertexDeclaration},
    {"SetFVF", 2, Dev_SetFVF},
    {"GetFVF", 2, Dev_GetFVF},
    {"CreateVertexShader", 3, Dev_CreateVertexShader},
    {"SetVertexShader", 2, Dev_SetVertexShader},
    {"GetVertexShader", 2, Dev_GetVertexShader},
    {"SetVertexShaderConstantF", 4, Dev_SetVertexShaderConstantF},
    {"GetVertexShaderConstantF", 4, Dev_GetVertexShaderConstantF},
    {"SetVertexShaderConstantI", 4, Dev_SetVertexShaderConstantI},
    {"GetVertexShaderConstantI", 4, Dev_GetVertexShaderConstantI},
    {"SetVertexShaderConstantB", 4, Dev_SetVertexShaderConstantB},
    {"GetVertexShaderConstantB", 4, Dev_GetVertexShaderConstantB},
    {"SetStreamSource", 5, Dev_SetStreamSource},
    {"GetStreamSource", 5, Dev_GetStreamSource},
    {"SetStreamSourceFreq", 3, Dev_SetStreamSourceFreq},
    {"GetStreamSourceFreq", 3, Dev_GetStreamSourceFreq},
    {"SetIndices", 2, Dev_SetIndices},
    {"GetIndices", 2, Dev_GetIndices},
    {"CreatePixelShader", 3, Dev_CreatePixelShader},
    {"SetPixelShader", 2, Dev_SetPixelShader},
    {"GetPixelShader", 2, Dev_GetPixelShader},
    {"SetPixelShaderConstantF", 4, Dev_SetPixelShaderConstantF},
    {"GetPixelShaderConstantF", 4, Dev_GetPixelShaderConstantF},
    {"SetPixelShaderConstantI", 4, Dev_SetPixelShaderConstantI},
    {"GetPixelShaderConstantI", 4, Dev_GetPixelShaderConstantI},
    {"SetPixelShaderConstantB", 4, Dev_SetPixelShaderConstantB},
    {"GetPixelShaderConstantB", 4, Dev_GetPixelShaderConstantB},
    {"DrawRectPatch", 4, Dev_DrawRectPatch},
    {"DrawTriPatch", 4, Dev_DrawTriPatch},
    {"DeletePatch", 2, Dev_DeletePatch},
    {"CreateQuery", 3, Dev_CreateQuery},
};

// The DLL's one export. Not a COM method: the guest calls it directly.
void d3d9_Direct3DCreate9(X86 *c) {
    ComObj *o = com_new(K_D3D9);
    uint32_t view = o ? com_view(o, IF_D3D9) : 0;
    if (!view) {
        if (o)
            com_release(o);
        set_eax(c, 0);
        return;
    }
    LOGW("d3d9: Direct3DCreate9(SDK %u) -> %08x", arg(c, 0), view);
    set_eax(c, view);
}

static const ImportShim g_d3d9_exports[] = {
    {"d3d9.dll", "Direct3DCreate9", 1, d3d9_Direct3DCreate9},
};

void d3d9_reset() {
    g_scratch = 0;
    g_scratch_size = 0;
}

void d3d9_register() {
    static bool done = false;
    if (done)
        return;
    done = true;

    com_define(IF_D3D9, "d3d9.dll", "IDirect3D9", g_d3d9, std::size(g_d3d9));
    com_define(IF_D3DDEVICE9, "d3d9.dll", "IDirect3DDevice9", g_device9, std::size(g_device9));
    com_bind(IF_D3D9, K_D3D9);
    com_bind(IF_D3DDEVICE9, K_D3D9DEVICE);
    com_register_iid(IF_D3D9, IID_IDirect3D9_);
    com_register_iid(IF_D3DDEVICE9, IID_IDirect3DDevice9_);
    com_define(IF_D3DTEXTURE9, "d3d9.dll", "IDirect3DTexture9", g_texture9, std::size(g_texture9));
    com_define(IF_D3DCUBETEXTURE9, "d3d9.dll", "IDirect3DCubeTexture9", g_cubetexture9,
               std::size(g_cubetexture9));
    com_define(IF_D3DSURFACE9, "d3d9.dll", "IDirect3DSurface9", g_surface9, std::size(g_surface9));
    com_define(IF_D3DVERTEXBUFFER9, "d3d9.dll", "IDirect3DVertexBuffer9", g_vb9, std::size(g_vb9));
    com_define(IF_D3DINDEXBUFFER9, "d3d9.dll", "IDirect3DIndexBuffer9", g_ib9, std::size(g_ib9));
    com_define(IF_D3DVERTEXDECL9, "d3d9.dll", "IDirect3DVertexDeclaration9", g_decl9,
               std::size(g_decl9));
    com_define(IF_D3DQUERY9, "d3d9.dll", "IDirect3DQuery9", g_query9, std::size(g_query9));
    com_bind(IF_D3DTEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DCUBETEXTURE9, K_D3D9TEXTURE);
    com_bind(IF_D3DSURFACE9, K_D3D9SURFACE);
    com_bind(IF_D3DVERTEXBUFFER9, K_D3D9VB);
    com_bind(IF_D3DINDEXBUFFER9, K_D3D9IB);
    com_bind(IF_D3DVERTEXDECL9, K_D3D9DECL);
    com_bind(IF_D3DQUERY9, K_D3D9QUERY);
    for (ComKind k : {K_D3D9TEXTURE, K_D3D9SURFACE, K_D3D9VB, K_D3D9IB})
        com_set_destructor(k, d3d9_resource_destroy);
    com_define(IF_D3DVOLUMETEXTURE9, "d3d9.dll", "IDirect3DVolumeTexture9", g_volume_texture9,
               std::size(g_volume_texture9));
    com_define(IF_D3DVOLUME9, "d3d9.dll", "IDirect3DVolume9", g_volume9, std::size(g_volume9));
    com_bind(IF_D3DVOLUMETEXTURE9, K_D3D9VOLUMETEXTURE);
    com_bind(IF_D3DVOLUME9, K_D3D9VOLUME);
    com_register_iid(IF_D3DVOLUMETEXTURE9, IID_VolumeTexture);
    com_register_iid(IF_D3DVOLUME9, IID_Volume);
    com_set_destructor(K_D3D9TEXTURE, texture_destroy); // Test159: with its levels and faces
    com_set_destructor(K_D3D9VOLUMETEXTURE, volume_destroy);
    com_set_destructor(K_D3D9VOLUME, volume_destroy);
    com_set_destructor(K_D3D9DEVICE, device_volume_destroy);
    com_set_destructor(K_D3D9QUERY, query_destroy);
    com_define(IF_D3DVERTEXSHADER9, "d3d9.dll", "IDirect3DVertexShader9", g_vshader9,
               std::size(g_vshader9));
    com_define(IF_D3DPIXELSHADER9, "d3d9.dll", "IDirect3DPixelShader9", g_vshader9,
               std::size(g_vshader9));
    com_bind(IF_D3DVERTEXSHADER9, K_D3D9VSHADER);
    com_bind(IF_D3DPIXELSHADER9, K_D3D9PSHADER);
    com_register_iid(IF_D3DVERTEXSHADER9, IID_VertexShader9);
    com_register_iid(IF_D3DPIXELSHADER9, IID_PixelShader9);
    com_set_destructor(K_D3D9VSHADER, shader_destroy);
    com_set_destructor(K_D3D9PSHADER, shader_destroy);
    imports_register(g_d3d9_exports, std::size(g_d3d9_exports));
}
