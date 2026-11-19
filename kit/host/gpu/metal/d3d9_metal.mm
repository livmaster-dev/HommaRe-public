// d3d9_metal.mm - the Direct3D 9 device's renderer on Metal: a D9Backend
// (host/gpu/d3d9_backend.h), driven by host/gpu/d3d9_host.cpp. It is
// Objective-C++, so the hosts compile it into their present sources.
//
// Shaders are translated to MSL (dx/d3d9_msl.h) and compiled once per
// program. Textures, surfaces and buffers are mirrored under the shim's ids.
// Draws are encoded on the guest thread into one command buffer per frame,
// which Present commits before handing the back buffer to the presenter as a
// GPU texture: a frame never comes back to the CPU unless the game reads a
// render target or a frame dump asks for it.
#import <Metal/Metal.h>
// The iOS Simulator SDK has no MetalFX: there the frame is stretched as before.
#if __has_include(<MetalFX/MetalFX.h>)
#import <MetalFX/MetalFX.h>
#define RECOMP_HAVE_METALFX 1
#else
#define RECOMP_HAVE_METALFX 0
#endif
#import <QuartzCore/QuartzCore.h>
#include <atomic>

#include "../../../dx/d3d9_msl.h"
#include "../../../dx/d3d9_shader.h"
#include "../../../dx/d3d9_volume.h"
#include "../../../dx/host_d9.h"
#include "../../../platform/os.h"
#include "../../../runtime/guest.h"
#include "../../../runtime/display_seam.h"
#include "../../../runtime/layout.h"
#include "../../d3d_render.h"
#include "metal_device.h"
#include "shader_warm_queue.h"
#include "pipeline_warm_spec.h"
#include "scaler_build_slot.h"
#include "../../present.h"
#include "../../gpu_time.h"
#include "../d3d9_backend.h"
#include "../d3d9_common.h"
#include "../d3d9_trace.h"
#include "../d3d9_capture.h"
#include <future>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <TargetConditionals.h>
#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>
#include <cctype>
#include <cerrno>
#if TARGET_OS_OSX
#include <libproc.h> // not in the iOS SDK
#endif
#include <thread>
#include <deque>
#include <condition_variable>
#include <pthread.h>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace d9gpu;

// How a D3DFORMAT is stored in Metal.
struct MtlFormat {
    MTLPixelFormat pixel = MTLPixelFormatInvalid;
    Conv conv = Conv::Unsupported;
    uint32_t bytes = 4; // bytes per source pixel, or per 4x4 block for DXT
    bool block = false;
};

MtlFormat format_info(uint32_t fmt, bool bc) {
    const FormatInfo f = d9gpu::format_info(fmt, bc);
    MtlFormat m;
    m.conv = f.conv;
    m.bytes = f.bytes;
    m.block = f.block;
    switch (f.store) {
    case Store::BGRA8:
        m.pixel = MTLPixelFormatBGRA8Unorm;
        break;
    case Store::A8:
        m.pixel = MTLPixelFormatA8Unorm;
        break;
    case Store::RGBA16:
        m.pixel = MTLPixelFormatRGBA16Unorm;
        break;
    case Store::R16F:
        m.pixel = MTLPixelFormatR16Float;
        break;
    case Store::RGBA16F:
        m.pixel = MTLPixelFormatRGBA16Float;
        break;
    case Store::R32F:
        m.pixel = MTLPixelFormatR32Float;
        break;
    case Store::RGBA32F:
        m.pixel = MTLPixelFormatRGBA32Float;
        break;
    case Store::BC1:
        m.pixel = MTLPixelFormatBC1_RGBA;
        break;
    case Store::BC2:
        m.pixel = MTLPixelFormatBC2_RGBA;
        break;
    case Store::BC3:
        m.pixel = MTLPixelFormatBC3_RGBA;
        break;
    case Store::Depth:
        m.pixel = MTLPixelFormatDepth32Float_Stencil8;
        break;
    }
    return m;
}

extern "C" int host_thermal_level(void); // host/energy.mm: 0 nominal .. 3 critical

struct Tex {
    HostD9TextureDesc desc{};
    MtlFormat info;
    id<MTLTexture> texture = nil;
    gpu::Texture imported;
    uint64_t used = 0; // the last frame serial that read or wrote it
    uint64_t written = 0;
    uint32_t generation = 0, revision = 0; // opt-in metadata trace identity
    float scale = 1.0f; // physical pixels per guest pixel: render targets are rendered larger
    bool cube = false, depth = false; // what `texture` is, without asking it
    // A multisampled target draws into `msaa` and resolves into `texture`,
    // which is what everything else reads.
    id<MTLTexture> msaa = nil;
    uint32_t samples = 1;
    // Test121: for a depth buffer, the Metal texture its last pass drew depth into (its own
    // texture, its multisampled one, or a fitted one), so a StretchRect from it copies that.
    id<MTLTexture> drawn_depth = nil;
};
// A buffer replaced while a frame still read it, kept for reuse once that
// frame is done. [lo, hi) is what has been written since it was replaced.
struct SpareBuf {
    id<MTLBuffer> buffer = nil;
    uint64_t retired = 0;
    uint32_t lo = 0, hi = 0;
};
struct Buf {
    id<MTLBuffer> buffer = nil;
    std::vector<uint8_t> shadow; // the contents, for fans and for copy-on-write
    uint64_t used = 0;
    uint32_t generation = 0, revision = 0;
    std::vector<SpareBuf> spares;
};

MTLBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
    case 1:
        return MTLBlendFactorZero;
    case 2:
        return MTLBlendFactorOne;
    case 3:
        return alpha ? MTLBlendFactorSourceAlpha : MTLBlendFactorSourceColor;
    case 4:
        return alpha ? MTLBlendFactorOneMinusSourceAlpha : MTLBlendFactorOneMinusSourceColor;
    case 5:
    case 12:
        return MTLBlendFactorSourceAlpha;
    case 6:
    case 13:
        return MTLBlendFactorOneMinusSourceAlpha;
    case 7:
        return MTLBlendFactorDestinationAlpha;
    case 8:
        return MTLBlendFactorOneMinusDestinationAlpha;
    case 9:
        return alpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorDestinationColor;
    case 10:
        return alpha ? MTLBlendFactorOneMinusDestinationAlpha
                     : MTLBlendFactorOneMinusDestinationColor;
    case 11:
        return MTLBlendFactorSourceAlphaSaturated;
    case 14:
        return alpha ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15:
        return alpha ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default:
        return MTLBlendFactorOne;
    }
}
MTLBlendOperation blend_op(uint32_t op) {
    switch (op) {
    case 2:
        return MTLBlendOperationSubtract;
    case 3:
        return MTLBlendOperationReverseSubtract;
    case 4:
        return MTLBlendOperationMin;
    case 5:
        return MTLBlendOperationMax;
    default:
        return MTLBlendOperationAdd;
    }
}
MTLCompareFunction compare_fn(uint32_t f) {
    switch (f) {
    case 1:
        return MTLCompareFunctionNever;
    case 2:
        return MTLCompareFunctionLess;
    case 3:
        return MTLCompareFunctionEqual;
    case 4:
        return MTLCompareFunctionLessEqual;
    case 5:
        return MTLCompareFunctionGreater;
    case 6:
        return MTLCompareFunctionNotEqual;
    case 7:
        return MTLCompareFunctionGreaterEqual;
    default:
        return MTLCompareFunctionAlways;
    }
}
MTLStencilOperation stencil_op(uint32_t op) {
    switch (op) {
    case 2:
        return MTLStencilOperationZero;
    case 3:
        return MTLStencilOperationReplace;
    case 4:
        return MTLStencilOperationIncrementClamp;
    case 5:
        return MTLStencilOperationDecrementClamp;
    case 6:
        return MTLStencilOperationInvert;
    case 7:
        return MTLStencilOperationIncrementWrap;
    case 8:
        return MTLStencilOperationDecrementWrap;
    default:
        return MTLStencilOperationKeep;
    }
}
MTLSamplerAddressMode address_mode(uint32_t a) {
    switch (a) {
    case 2:
        return MTLSamplerAddressModeMirrorRepeat;
    case 3:
        return MTLSamplerAddressModeClampToEdge;
    case 4:
        return MTLSamplerAddressModeClampToBorderColor;
    case 5:
        return MTLSamplerAddressModeMirrorClampToEdge;
    default:
        return MTLSamplerAddressModeRepeat;
    }
}

// A D3DDECLTYPE as a Metal vertex format, and the scale that turns what the
// shader receives back into the value Direct3D would have handed it.
bool vertex_format(uint32_t type, MTLVertexFormat *fmt, float scale[4]) {
    for (int k = 0; k < 4; ++k)
        scale[k] = 1.0f;
    switch (type) {
    case 0:
        *fmt = MTLVertexFormatFloat;
        return true;
    case 1:
        *fmt = MTLVertexFormatFloat2;
        return true;
    case 2:
        *fmt = MTLVertexFormatFloat3;
        return true;
    case 3:
        *fmt = MTLVertexFormatFloat4;
        return true;
    case 4:
        *fmt = MTLVertexFormatUChar4Normalized_BGRA;
        return true;
    case 5:
        *fmt = MTLVertexFormatUChar4Normalized;
        for (int k = 0; k < 4; ++k)
            scale[k] = 255.0f;
        return true;
    case 6:
        *fmt = MTLVertexFormatShort2Normalized;
        scale[0] = scale[1] = 32767.0f;
        return true;
    case 7:
        *fmt = MTLVertexFormatShort4Normalized;
        for (int k = 0; k < 4; ++k)
            scale[k] = 32767.0f;
        return true;
    case 8:
        *fmt = MTLVertexFormatUChar4Normalized;
        return true;
    case 9:
        *fmt = MTLVertexFormatShort2Normalized;
        return true;
    case 10:
        *fmt = MTLVertexFormatShort4Normalized;
        return true;
    case 11:
        *fmt = MTLVertexFormatUShort2Normalized;
        return true;
    case 12:
        *fmt = MTLVertexFormatUShort4Normalized;
        return true;
    case 13:
        *fmt = MTLVertexFormatUInt1010102Normalized;
        scale[0] = scale[1] = scale[2] = 1023.0f;
        return true;
    case 14:
        *fmt = MTLVertexFormatInt1010102Normalized;
        return true;
    case 15:
        *fmt = MTLVertexFormatHalf2;
        return true;
    case 16:
        *fmt = MTLVertexFormatHalf4;
        return true;
    default:
        return false;
    }
}

const char *kUtilitySource = R"MSL(#include <metal_stdlib>
using namespace metal;
struct QuadOut { float4 pos [[position]]; float2 uv; };
struct QuadIn { float4 rect; float4 uv; float z; float pad0; float pad1; float pad2; float4 color; };
vertex QuadOut quad_vs(uint vid [[vertex_id]], constant QuadIn &q [[buffer(0)]]) {
    float2 c = float2(float(vid & 1), float((vid >> 1) & 1));
    QuadOut o;
    o.pos = float4(mix(q.rect.xy, q.rect.zw, c), q.z, 1.0);
    o.uv = mix(q.uv.xy, q.uv.zw, c);
    return o;
}
fragment float4 copy_fs(QuadOut i [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    return t.sample(s, i.uv);
}
fragment float4 clear_fs(QuadOut i [[stage_in]], constant QuadIn &q [[buffer(0)]]) {
    return q.color;
}
// Test121: a depth buffer copied into a colour target (StretchRect from a depth surface), the
// stored depth as written; a multisampled depth buffer gives the nearest of its samples (the
// smallest depth), so a foreground edge keeps its own depth (Metal's MIN resolve choice;
// review 2 Oct, Ledger L1).
fragment float4 depth_copy_fs(QuadOut i [[stage_in]], depth2d<float> t [[texture(0)]]) {
    uint2 size = uint2(t.get_width(), t.get_height());
    uint2 p = min(uint2(i.uv * float2(size)), size - uint2(1));
    return float4(t.read(p));
}
fragment float4 depth_copy_ms_fs(QuadOut i [[stage_in]], depth2d_ms<float> t [[texture(0)]]) {
    uint2 size = uint2(t.get_width(), t.get_height());
    uint2 p = min(uint2(i.uv * float2(size)), size - uint2(1));
    float d = t.read(p, 0);
    for (uint k = 1; k < t.get_num_samples(); ++k)
        d = min(d, t.read(p, k));
    return float4(d);
}
)MSL";

struct QuadIn {
    float rect[4];
    float uv[4];
    float z, pad0, pad1, pad2;
    float color[4];
};

// ---- shader warm-up ------------------------------------------------------------------------
// Compiling MSL is synchronous at first use. With a cold system Metal cache (the first launch of a
// new bundle, or after the system purged it) the first main-menu frame held 1.1-1.3 s compiling
// 15-17 libraries (Test65; pipelines took under 2 ms). When RECOMP_D3D9_SHADER_WARM_DIR names a
// directory (the launcher puts it in the profile, outside the signed app), every MSL source that
// compiles is recorded there once (content-addressed by SHA-256 of the options identity + source,
// first use order kept in order.txt), and at start-up a background thread compiles the recorded
// sources in that order. RECOMP_D3D9_SHADER_WARM_SEED may name a read-only list in the same format
// (shipped inside the application, compiled first). The draw path takes a pre-compiled library when
// its key matches and
// otherwise compiles as before, so a missing, stale or tampered entry only costs the normal compile.
// Bounded: at most 8192 entries of at most 1 MiB each; only hex-named files are read.
// Requests are coalesced (Test66): a draw that needs the library the worker is compiling right now
// waits for that one compile instead of starting a second, and a queued entry the draw path compiles
// itself is struck from the worker's list. No lock is held while any library compiles.
// Test70: the pixel and vertex formats a recorded pipeline may name (the ones this renderer creates).
bool warm_formats_ok(const PipeSpec &p) {
    static const uint32_t color[] = {
        (uint32_t)MTLPixelFormatBGRA8Unorm,  (uint32_t)MTLPixelFormatBGRA8Unorm_sRGB, (uint32_t)MTLPixelFormatRGBA8Unorm,
        (uint32_t)MTLPixelFormatA8Unorm,     (uint32_t)MTLPixelFormatRGBA16Unorm,     (uint32_t)MTLPixelFormatR16Float,
        (uint32_t)MTLPixelFormatRGBA16Float, (uint32_t)MTLPixelFormatR32Float,        (uint32_t)MTLPixelFormatRGBA32Float,
        (uint32_t)MTLPixelFormatBGR10A2Unorm, (uint32_t)MTLPixelFormatRGB10A2Unorm};
    static const uint32_t vertex[] = {
        (uint32_t)MTLVertexFormatFloat, (uint32_t)MTLVertexFormatFloat2, (uint32_t)MTLVertexFormatFloat3,
        (uint32_t)MTLVertexFormatFloat4, (uint32_t)MTLVertexFormatUChar4Normalized_BGRA,
        (uint32_t)MTLVertexFormatUChar4Normalized, (uint32_t)MTLVertexFormatShort2Normalized,
        (uint32_t)MTLVertexFormatShort4Normalized, (uint32_t)MTLVertexFormatUShort2Normalized,
        (uint32_t)MTLVertexFormatUShort4Normalized, (uint32_t)MTLVertexFormatUInt1010102Normalized,
        (uint32_t)MTLVertexFormatInt1010102Normalized, (uint32_t)MTLVertexFormatHalf2, (uint32_t)MTLVertexFormatHalf4};
    auto in = [](uint32_t v, const uint32_t *set, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (set[i] == v)
                return true;
        return false;
    };
    if (p.depth && p.depth != (uint32_t)MTLPixelFormatDepth32Float_Stencil8)
        return false;
    for (const PipeSpec::Color &c : p.color)
        if (c.format && !in(c.format, color, sizeof color / sizeof *color))
            return false;
    for (const PipeSpec::Attr &a : p.attr)
        if (a.format && !in(a.format, vertex, sizeof vertex / sizeof *vertex))
            return false;
    return true;
}

// Test70: the recorded identity of a pipeline descriptor (every field the draw path sets).
PipeSpec warm_spec_of(MTLRenderPipelineDescriptor *pd, const std::string &vkey, const std::string &fkey) {
    PipeSpec p;
    p.vkey = vkey;
    p.fkey = fkey;
    p.samples = (uint32_t)pd.rasterSampleCount;
    p.depth = (uint32_t)pd.depthAttachmentPixelFormat;
    if (pd.stencilAttachmentPixelFormat != pd.depthAttachmentPixelFormat)
        p.samples = 0;   // not a combination the renderer records (refused by valid())
    for (int i = 0; i < 4; ++i) {
        MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
        if (a.pixelFormat == MTLPixelFormatInvalid)
            continue;
        PipeSpec::Color &c = p.color[i];
        c.format = (uint32_t)a.pixelFormat;
        c.mask = (uint32_t)a.writeMask;
        if (a.blendingEnabled) {
            c.blend = 1;
            c.src_rgb = (uint32_t)a.sourceRGBBlendFactor;
            c.dst_rgb = (uint32_t)a.destinationRGBBlendFactor;
            c.rgb_op = (uint32_t)a.rgbBlendOperation;
            c.src_a = (uint32_t)a.sourceAlphaBlendFactor;
            c.dst_a = (uint32_t)a.destinationAlphaBlendFactor;
            c.a_op = (uint32_t)a.alphaBlendOperation;
        }
    }
    if (MTLVertexDescriptor *vd = pd.vertexDescriptor) {
        for (int i = 0; i < 16; ++i) {
            MTLVertexAttributeDescriptor *at = vd.attributes[i];
            if (at.format == MTLVertexFormatInvalid)
                continue;
            p.attr[i] = {(uint32_t)at.format, (uint32_t)at.offset, (uint32_t)at.bufferIndex};
        }
        for (int i = 0; i < 9; ++i) {
            MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            if (!l.stride)
                continue;
            p.layout[i] = {(uint32_t)l.stride, (uint32_t)l.stepFunction, (uint32_t)l.stepRate};
        }
    }
    return p;
}

// Test70: the descriptor a recorded identity describes (the inverse of warm_spec_of).
MTLRenderPipelineDescriptor *warm_descriptor(const PipeSpec &p, id<MTLFunction> vfn, id<MTLFunction> ffn) {
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.rasterSampleCount = p.samples;
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    for (int i = 0; i < 16; ++i)
        if (const PipeSpec::Attr &a = p.attr[i]; a.format) {
            vd.attributes[i].format = (MTLVertexFormat)a.format;
            vd.attributes[i].offset = a.offset;
            vd.attributes[i].bufferIndex = a.buffer;
        }
    for (int i = 0; i < 9; ++i)
        if (const PipeSpec::Layout &l = p.layout[i]; l.stride) {
            vd.layouts[i].stride = l.stride;
            vd.layouts[i].stepFunction = (MTLVertexStepFunction)l.step;
            vd.layouts[i].stepRate = l.rate;
        }
    pd.vertexDescriptor = vd;
    for (int i = 0; i < 4; ++i)
        if (const PipeSpec::Color &c = p.color[i]; c.format) {
            MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
            a.pixelFormat = (MTLPixelFormat)c.format;
            a.writeMask = (MTLColorWriteMask)c.mask;
            if (c.blend) {
                a.blendingEnabled = YES;
                a.sourceRGBBlendFactor = (MTLBlendFactor)c.src_rgb;
                a.destinationRGBBlendFactor = (MTLBlendFactor)c.dst_rgb;
                a.rgbBlendOperation = (MTLBlendOperation)c.rgb_op;
                a.sourceAlphaBlendFactor = (MTLBlendFactor)c.src_a;
                a.destinationAlphaBlendFactor = (MTLBlendFactor)c.dst_a;
                a.alphaBlendOperation = (MTLBlendOperation)c.a_op;
            }
        }
    if (p.depth) {
        pd.depthAttachmentPixelFormat = (MTLPixelFormat)p.depth;
        pd.stencilAttachmentPixelFormat = (MTLPixelFormat)p.depth;
    }
    return pd;
}

class ShaderWarm {
  public:
    ShaderWarm(id<MTLDevice> dev, std::string dir, std::string seed, std::string identity)
        : dev_(dev), dir_(std::move(dir)), identity_(std::move(identity)) {
        if (!seed.empty())
            load_order(seed);   // read-only list shipped with the application
        if (!dir_.empty()) {
            mkdir(dir_.c_str(), 0700);
            load_order(dir_);
        }
        // Test70: recorded pipelines (seed first). RECOMP_D3D9_PIPELINE_WARM=0 (TEST ONLY) turns the
        // pipeline half off for comparisons; libraries are unaffected.
        const char *pw = recomp_env("D3D9_PIPELINE_WARM");
        pipes_on_ = !(pw && !strcmp(pw, "0"));
        if (pipes_on_) {
            if (!seed.empty())
                load_pipes(seed);
            if (!dir_.empty())
                load_pipes(dir_);
        }
        open_archive();
        worker_ = std::thread([this] { run(); });
    }
    ~ShaderWarm() {
        stop_.store(true);
        if (worker_.joinable())
            worker_.join();
    }
    static MTLCompileOptions *options() {
        MTLCompileOptions *opts = [MTLCompileOptions new];
        if (@available(macOS 15.0, iOS 18.0, *))
            opts.mathMode = MTLMathModeSafe;
        // TEST ONLY: a per-run salt makes every compile miss the system Metal cache (a controlled
        // cold start without deleting any system cache).
        if (const char *salt = recomp_env("D3D9_SHADER_SALT"); salt && *salt)
            opts.preprocessorMacros = @{@"RECOMP_SHADER_SALT" : [NSString stringWithUTF8String:salt]};
        return opts;
    }
    std::string key(const std::string &src) const {
        CC_SHA256_CTX c;
        CC_SHA256_Init(&c);
        CC_SHA256_Update(&c, identity_.data(), (CC_LONG)identity_.size());
        CC_SHA256_Update(&c, "\n", 1);
        CC_SHA256_Update(&c, src.data(), (CC_LONG)src.size());
        unsigned char d[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256_Final(d, &c);
        char hex[65];
        for (int i = 0; i < 32; ++i)
            snprintf(hex + 2 * i, 3, "%02x", d[i]);
        return std::string(hex, 64);
    }
    // The finished library for this source, if the warm-up has it (waiting, bounded, when the
    // worker is compiling exactly this source now); nil means compile it here.
    id<MTLLibrary> take(const std::string &k) {
        id<MTLLibrary> lib = nil;
        return queue_.take(k, &lib) ? lib : nil;
    }
    // Called after the draw path compiled a source itself. A new source is recorded; a known one
    // reaches here only when its recorded copy was not usable in time (claimed first, or missing,
    // damaged or altered), so a damaged profile copy is rewritten once rather than kept forever.
    // Entries shipped in the read-only seed are never written.
    void record(const std::string &k, const std::string &src) {
        std::lock_guard<std::mutex> lock(record_mutex_);
        if (dir_.empty() || src.size() > (1u << 20))
            return;
        const std::string path = dir_ + "/" + k + ".msl", tmp = path + ".tmp";
        const bool known = queue_.known(k);
        if (known && (!in_profile(k) || same_file(path, src)))
            return;
        if (!known && queue_.known_count() >= 8192)
            return;
        FILE *f = fopen(tmp.c_str(), "wb");
        if (!f) {
            note_write_failure();
            return;
        }
        const bool ok = fwrite(src.data(), 1, src.size(), f) == src.size();
        if (fclose(f) != 0 || !ok || rename(tmp.c_str(), path.c_str()) != 0) {
            remove(tmp.c_str());
            note_write_failure();
            return;
        }
        if (known) {
            if (++repaired_ == 1)
                fprintf(stderr, "d3d9 metal: shader warm-up rewrote a recorded entry that was missing or "
                                "damaged (%s)\n", k.c_str());
            return;   // already listed in order.txt
        }
        if (FILE *o = fopen((dir_ + "/order.txt").c_str(), "a")) {
            fprintf(o, "%s\n", k.c_str());
            fclose(o);
        }
        queue_.add_known(k);
        profile_keys_.insert(k);
    }
    uint32_t hits() { return queue_.hits(); }
    uint32_t coalesced() { return queue_.coalesced(); }

    // Test70: the finished pipeline recorded with exactly this identity, if the warm-up has it
    // (handed over once); nil means create it here. A recorded identity the worker has not reached
    // is struck from its list, as for libraries. Never waits.
    id<MTLRenderPipelineState> take_pipeline(const std::string &spec) {
        std::lock_guard<std::mutex> lock(pipe_mutex_);
        auto it = pipe_ready_.find(spec);
        if (it == pipe_ready_.end()) {
            if (pipe_known_.count(spec))
                pipe_claimed_.insert(spec);
            return nil;
        }
        id<MTLRenderPipelineState> pso = it->second;
        pipe_ready_.erase(it);
        if (++pipe_hits_ == 1 || pipe_hits_ == 10 || pipe_hits_ == 100 || pipe_hits_ == 1000)
            fprintf(stderr, "d3d9 metal: pipeline warm-up: %u pre-built pipelines used so far (at %.3f s monotonic)\n",
                    pipe_hits_, os_monotonic_ns() / 1e9);
        return pso;
    }
    // Test70: called after the draw path created a pipeline itself; a new identity is appended to
    // the profile's pipelines.txt (at most 8192; the read-only seed is never written).
    void record_pipeline(const std::string &spec) {
        if (dir_.empty() || !pipes_on_)
            return;
        {
            std::lock_guard<std::mutex> lock(pipe_mutex_);
            if (pipe_known_.count(spec) || pipe_known_.size() >= 8192)
                return;
            pipe_known_.insert(spec);
        }
        std::lock_guard<std::mutex> lock(record_mutex_);
        FILE *o = fopen((dir_ + "/pipelines.txt").c_str(), "a");
        if (!o) {
            note_write_failure();
            return;
        }
        const bool ok = fprintf(o, "%s\n", spec.c_str()) > 0;
        if (fclose(o) != 0 || !ok)
            note_write_failure();
    }
    uint32_t pipeline_hits() {
        std::lock_guard<std::mutex> lock(pipe_mutex_);
        return pipe_hits_;
    }
    bool pipelines_on() const { return pipes_on_; }

  private:
    bool in_profile(const std::string &k) const { return profile_keys_.count(k) != 0; }
    static bool same_file(const std::string &path, const std::string &src) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || (uint64_t)st.st_size != src.size())
            return false;
        std::string have;
        if (FILE *f = fopen(path.c_str(), "rb")) {
            have.resize(src.size());
            const size_t n = fread(have.data(), 1, have.size(), f);
            fclose(f);
            have.resize(n);
        }
        return have == src;
    }
    void note_write_failure() {
        // The game is unaffected (the library was compiled and is in use); only the next launch
        // loses this entry's head start. Said once, not per shader.
        if (!write_failure_logged_) {
            write_failure_logged_ = true;
            fprintf(stderr, "d3d9 metal: shader warm-up could not record into %s (%s); continuing "
                            "without recording\n", dir_.c_str(), strerror(errno));
        }
    }
    static bool hex_key(const char *s) {
        for (int i = 0; i < 64; ++i)
            if (!isxdigit((unsigned char)s[i]))
                return false;
        return s[64] == 0 || s[64] == '\n' || s[64] == '\r';
    }
    void load_order(const std::string &from) {
        FILE *o = fopen((from + "/order.txt").c_str(), "r");
        if (!o)
            return;
        char line[128];
        while (fgets(line, sizeof line, o) && order_.size() < 8192) {
            if (!hex_key(line))
                continue;
            std::string k(line, 64);
            if (queue_.add_known(k)) {
                order_.push_back({k, from});
                lib_dir_[k] = from;
                if (from == dir_)
                    profile_keys_.insert(k);
            }
        }
        fclose(o);
    }
    void load_pipes(const std::string &from) {
        FILE *o = fopen((from + "/pipelines.txt").c_str(), "r");
        if (!o)
            return;
        char line[2100];
        while (fgets(line, sizeof line, o) && pipe_order_.size() < 8192) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\n')
                line[--n] = 0;
            PipeSpec p;
            if (!PipeSpec::parse(line, &p) || !warm_formats_ok(p))
                continue;   // damaged, partial or altered: the draw path creates that pipeline itself
            if (pipe_known_.insert(line).second)
                pipe_order_.push_back(line);
        }
        fclose(o);
    }
    static std::string read_source(const std::string &dir, const std::string &k) {
        std::string src;
        if (FILE *f = fopen((dir + "/" + k + ".msl").c_str(), "rb")) {
            char buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0 && src.size() <= (1u << 20))
                src.append(buf, n);
            fclose(f);
        }
        return src;
    }
    // Worker only: a recorded library for the pipeline half (the one compiled above, or compiled
    // again from its verified recorded source when the draw path took it first).
    id<MTLLibrary> worker_library(const std::string &k, MTLCompileOptions *opts) {
        auto it = libs_.find(k);
        if (it != libs_.end())
            return it->second;
        id<MTLLibrary> lib = nil;
        if (auto d = lib_dir_.find(k); d != lib_dir_.end()) {
            const std::string src = read_source(d->second, k);
            if (!src.empty() && src.size() <= (1u << 20) && key(src) == k) {
                NSError *error = nil;
                lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()] options:opts error:&error];
            }
        }
        libs_[k] = lib;
        return lib;
    }
    void run_pipelines(MTLCompileOptions *opts) {
        const uint64_t t0 = os_monotonic_ns();
        uint32_t made = 0, skipped = 0, taken_over = 0, mismatched = 0;
        for (const std::string &spec : pipe_order_) {
            if (stop_.load())
                break;
            {
                std::lock_guard<std::mutex> lock(pipe_mutex_);
                if (pipe_claimed_.count(spec)) {
                    ++taken_over;
                    continue;
                }
            }
            PipeSpec p;
            if (!PipeSpec::parse(spec, &p)) {
                ++skipped;
                continue;
            }
            @autoreleasepool {
                id<MTLLibrary> vl = worker_library(p.vkey, opts), fl = worker_library(p.fkey, opts);
                id<MTLFunction> vfn = [vl newFunctionWithName:@"vs_main"], ffn = [fl newFunctionWithName:@"ps_main"];
                if (!vfn || !ffn) {
                    ++skipped;
                    continue;
                }
                MTLRenderPipelineDescriptor *pd = warm_descriptor(p, vfn, ffn);
                if (warm_spec_of(pd, p.vkey, p.fkey).text() != spec) {
                    ++mismatched;   // would not be the identity the draw path asks for
                    continue;
                }
                NSError *error = nil;
                id<MTLRenderPipelineState> pso = archived_pipeline(pd);
                if (!pso) {
                    pso = [dev_ newRenderPipelineStateWithDescriptor:pd error:&error];
                    if (pso)
                        archive_add(pd);
                }
                if (!pso) {
                    ++skipped;
                    continue;
                }
                std::lock_guard<std::mutex> lock(pipe_mutex_);
                if (pipe_claimed_.count(spec)) {
                    ++taken_over;
                } else {
                    pipe_ready_[spec] = pso;
                    ++made;
                }
            }
        }
        libs_.clear();
        fprintf(stderr,
                "d3d9 metal: pipeline warm-up created %u recorded pipelines in the background in %.1f ms "
                "(%u skipped, %u mismatched, %u created first by the draw path, %zu recorded%s; "
                "binary archive %u hits, %u added)\n",
                made, (os_monotonic_ns() - t0) / 1e6, skipped, mismatched, taken_over, pipe_order_.size(),
                stop_.load() ? ", stopped early" : "", archive_hits_, archive_added_);
        archive_save();
    }
    void run() {
        const uint64_t t0 = os_monotonic_ns();
        uint32_t compiled = 0, skipped = 0, taken_over = 0;
        MTLCompileOptions *opts = options();
        for (const auto &entry : order_) {
            const std::string &k = entry.first;
            if (stop_.load())
                break;
            const std::string src = read_source(entry.second, k);
            if (src.empty() || src.size() > (1u << 20) || key(src) != k) {
                ++skipped;   // missing, oversized, stale or altered: the draw path compiles it itself
                continue;
            }
            if (!queue_.begin(k)) {
                ++taken_over;   // first use got there first and compiled it
                continue;
            }
            @autoreleasepool {
                NSError *error = nil;
                id<MTLLibrary> lib = [dev_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                                        options:opts
                                                          error:&error];
                queue_.finish(k, lib, lib != nil);
                ++(lib ? compiled : skipped);
                if (lib && !pipe_order_.empty())
                    libs_[k] = lib;   // kept for the pipeline half below
            }
        }
        fprintf(stderr,
                "d3d9 metal: shader warm-up compiled %u recorded libraries in the background in %.1f ms "
                "(%u skipped, %u compiled first by the draw path, %zu recorded%s)\n",
                compiled, (os_monotonic_ns() - t0) / 1e6, skipped, taken_over, order_.size(),
                stop_.load() ? ", stopped early" : "");
        if (!pipe_order_.empty() && !stop_.load())
            run_pipelines(opts);
    }

    // The compiled GPU code of the recorded pipelines (<profile>/metal-shader-warm/
    // pipelines.binarchive): the warm-up worker creates a pipeline from it without a backend
    // compile, and adds the ones it had to compile. Worker only, so no locking; the draw path
    // never reads it. A file the system no longer accepts (an OS or GPU driver update) is
    // started again. RECOMP_METAL_ARCHIVE=0 leaves it out. The iOS simulator's GPU has no
    // architecture to key an archive by: loading one it wrote aborts in MTLLoader
    // ("Target device architecture is nil") on the next start, so it never gets one.
    void open_archive() {
        if (dir_.empty() || !pipes_on_)
            return;
#if TARGET_OS_SIMULATOR
        return;
#endif
        if (const char *v = recomp_env("METAL_ARCHIVE"); v && v[0] == '0')
            return;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            archive_path_ = dir_ + "/pipelines.binarchive";
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:archive_path_.c_str()]];
            MTLBinaryArchiveDescriptor *d = [MTLBinaryArchiveDescriptor new];
            NSError *error = nil;
            if (access(archive_path_.c_str(), R_OK) == 0) {
                d.url = url;
                archive_ = [dev_ newBinaryArchiveWithDescriptor:d error:&error];
                if (!archive_) {
                    fprintf(stderr, "d3d9 metal: pipeline archive not usable (%s); starting it again\n",
                            error.localizedDescription.UTF8String);
                    remove(archive_path_.c_str());
                    d.url = nil;
                }
            }
            if (!archive_)
                archive_ = [dev_ newBinaryArchiveWithDescriptor:d error:&error];
        }
    }
    id<MTLRenderPipelineState> archived_pipeline(MTLRenderPipelineDescriptor *pd) {
        if (!archive_)
            return nil;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            pd.binaryArchives = @[ archive_ ];
            NSError *error = nil;
            id<MTLRenderPipelineState> pso =
                [dev_ newRenderPipelineStateWithDescriptor:pd
                                                   options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                reflection:nil
                                                     error:&error];
            pd.binaryArchives = nil;
            if (pso)
                ++archive_hits_;
            return pso;
        }
        return nil;
    }
    void archive_add(MTLRenderPipelineDescriptor *pd) {
        if (!archive_)
            return;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            NSError *error = nil;
            if ([archive_ addRenderPipelineFunctionsWithDescriptor:pd error:&error])
                ++archive_added_;
        }
    }
    void archive_save() {
        if (!archive_ || !archive_added_)
            return;
        if (@available(macOS 11.0, iOS 14.0, *)) {
            const std::string tmp = archive_path_ + ".tmp";
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp.c_str()]];
            NSError *error = nil;
            if ([archive_ serializeToURL:url error:&error] && rename(tmp.c_str(), archive_path_.c_str()) == 0)
                return;
            remove(tmp.c_str());
            fprintf(stderr, "d3d9 metal: pipeline archive not saved (%s)\n",
                    error ? error.localizedDescription.UTF8String : strerror(errno));
        }
    }

    id<MTLDevice> dev_;
    std::string dir_, identity_;
    id<MTLBinaryArchive> archive_ = nil;
    std::string archive_path_;
    uint32_t archive_hits_ = 0, archive_added_ = 0;
    std::vector<std::pair<std::string, std::string>> order_;   // key, directory (worker only)
    ShaderWarmQueue<id<MTLLibrary>> queue_;
    std::set<std::string> profile_keys_;   // keys listed in the profile's order.txt (writable copies)
    std::mutex record_mutex_;
    uint32_t repaired_ = 0;
    bool write_failure_logged_ = false;
    std::atomic<bool> stop_{false};
    // Test70 pipeline half. pipe_order_, lib_dir_ and libs_ are filled before the worker starts or
    // used by the worker only; the rest is shared under pipe_mutex_.
    bool pipes_on_ = true;
    std::vector<std::string> pipe_order_;
    std::unordered_map<std::string, std::string> lib_dir_;
    std::unordered_map<std::string, id<MTLLibrary>> libs_;
    std::mutex pipe_mutex_;
    std::set<std::string> pipe_known_, pipe_claimed_;
    std::unordered_map<std::string, id<MTLRenderPipelineState>> pipe_ready_;
    uint32_t pipe_hits_ = 0;
    std::thread worker_;
};

#if RECOMP_HAVE_METALFX
// A MetalFX spatial scaler is made off the render path (scaler_build_slot.h): creating one takes
// about 70 ms, and the render thread keeps presenting with what it has meanwhile. One build per
// slot runs at a time; when it ends, the latest configuration wanted since is built next.
using FxSlot = ScalerBuildSlot<id<MTLFXSpatialScaler>>;
void fx_build_async(id<MTLDevice> dev, std::shared_ptr<FxSlot> slot, ScalerKey first, const char *what) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        ScalerKey k = first;
        for (;;) {
            const uint64_t t0 = os_monotonic_ns();
            id<MTLFXSpatialScaler> fx = nil;
            @autoreleasepool {
                MTLFXSpatialScalerDescriptor *sd = [MTLFXSpatialScalerDescriptor new];
                sd.inputWidth = k.in_w;
                sd.inputHeight = k.in_h;
                sd.outputWidth = k.out_w;
                sd.outputHeight = k.out_h;
                sd.colorTextureFormat = (MTLPixelFormat)k.format;
                sd.outputTextureFormat = (MTLPixelFormat)k.format;
                sd.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
                fx = [sd newSpatialScalerWithDevice:dev];
            }
            fprintf(stderr, "d3d9 metal: MetalFX %s scaler %ux%u -> %ux%u %s in the background in %.1f ms\n", what,
                    k.in_w, k.in_h, k.out_w, k.out_h, fx ? "made" : "failed", (os_monotonic_ns() - t0) / 1e6);
            const std::optional<ScalerKey> next = slot->complete(k, fx);
            if (!next)
                break;
            k = *next;
        }
    });
}
#endif

// Test157b: the number of cached fitted depth buffers, for host_tests (SIZE_MAX until a renderer exists).
std::atomic<size_t> g_fitted_depth_entries{SIZE_MAX};

class Renderer final : public D9Backend {
  public:
    const char *name() const override {
        return "Metal";
    }
    int vfetch() const override {
        return HOST_D9_VFETCH_MSL;
    }
    // The vertex fetch check buffer (HOST_D9_VFETCH_BUFFER(4)), handed over once the frames
    // that wrote it have finished.
    void vfetch_check(HostD9VFetchVisit visit, void *user) override {
        auto it = buffers_.find(HOST_D9_VFETCH_BUFFER(4));
        if (it == buffers_.end() || !it->second.buffer || vfetch_tags_.empty())
            return;
        const uint64_t done = completed_.load();
        uint32_t done_tag = 0;
        while (!vfetch_tags_.empty() && vfetch_tags_.front().first <= done) {
            done_tag = vfetch_tags_.front().second;
            vfetch_tags_.erase(vfetch_tags_.begin());
        }
        if (!done_tag)
            return;
        visit((const uint32_t *)it->second.buffer.contents, (uint32_t)(it->second.buffer.length / 4), done_tag,
              user);
    }
    explicit Renderer(gpu::MetalDevice *device)
        : device_(device), mtl_(device->native()), queue_(device->native_queue()) {
        bc_ = false;
        if (@available(macOS 11.0, iOS 16.4, *))
            bc_ = mtl_.supportsBCTextureCompression;
#if TARGET_OS_OSX
        bc_ = true;
#endif
        // RECOMP_D3D9_BC=0/1 forces CPU-decoded or native DXT textures.
        if (const char *bc = recomp_env("D3D9_BC"); bc && *bc)
            bc_ = *bc != '0';
        fprintf(stderr, "d3d9 metal: %s, BC textures %s\n", mtl_.name.UTF8String,
                bc_ ? "native" : "decoded on the CPU");
        NSError *error = nil;
        utility_ = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:kUtilitySource]
                                      options:nil
                                        error:&error];
        if (!utility_)
            fprintf(stderr, "d3d9 metal: utility shaders failed: %s\n",
                    error.localizedDescription.UTF8String);
        frames_ = dispatch_semaphore_create(3);
        const char *warm = recomp_env("D3D9_SHADER_WARM_DIR"), *seed = recomp_env("D3D9_SHADER_WARM_SEED");
        // On by default, in the player's profile: what a run compiled is compiled ahead in the
        // background next time, so an effect seen once never stalls a frame again (Heroes V's
        // battles held frames for up to 1.7 s on first sight of a spell). "0" turns it off.
        static std::string warm_default;
        if (!warm) {
            warm_default = host_layout().profile_dir + "/metal-shader-warm";
            warm = warm_default.c_str();
        } else if (!strcmp(warm, "0")) {
            warm = "";
        }
        if ((warm && *warm) || (seed && *seed)) {
            // Namespace of the recorded keys. The compile options themselves are always the current
            // run's (the background and draw paths share ShaderWarm::options()).
            std::string identity = "msl-v1";
            if (@available(macOS 15.0, iOS 18.0, *))
                identity += "-safe";
            warm_ = std::make_unique<ShaderWarm>(mtl_, warm ? warm : "", seed ? seed : "", identity);
        }
        MTLVertexDescriptor *vd = nil;
        (void)vd;
        zero_ = [mtl_ newBufferWithLength:64 options:MTLResourceStorageModeShared];
        memset(zero_.contents, 0, 64);
        const char *trace = recomp_env("D3D9_TRACE");
        if (trace && !strcmp(trace, "1")) {
            capture_mode_ = recomp_env("D3D9_CAPTURE_SESSION") != nullptr;
            if (capture_mode_) {
                trace_ = capture_ring();
                capture_spare_ = capture_ring();
                trace_->retain = false;
            } else
                trace_ = std::make_unique<d9trace::Ring>();
            fprintf(stderr,
                    "d3d9 trace: ten-frame metadata history enabled (%zu bytes); "
                    "probe requests flush history, no texture readback\n",
                    trace_->allocated_bytes());
            if (capture_mode_)
                fprintf(stderr,
                        "capture: DISARMED; F8 arms, wait 2s, F8 triggers; 6144 draws, 1024 "
                        "events, 2Hz full samples, 6s post, 3 captures; buffers=%zu bytes\n",
                        trace_->allocated_bytes() * 2);
        }
    }

    ~Renderer() override {
        if (capture_mode_ && capture_control_.state == capture_state::Capturing)
            capture_save();
        if (capture_job_.valid()) {
            capture_job_.wait();
            capture_poll();
        }
    }

    bool ok() const {
        return utility_ != nil;
    }

    // ---- textures ------------------------------------------------------
    void define(const HostD9TextureDesc &d) override {
        end_pass();
        split_forget(d.id);
        Tex &t = textures_[d.id];
        if (t.imported)
            device_->destroy(t.imported);
        forget_fitted_depth(t); // a redefinition replaces the textures
        t = Tex{};
        if (trace_)
            t.generation = ++resource_generation_;
        t.desc = d;
        const bool volume = d.kind == HOST_D9_TEX_VOLUME;
        // This path admits uncompressed formats only. Volumes are sampled
        // resources, never D3D9 render targets.
        t.info = format_info(d.format, bc_ && !volume);
        if (volume && (!d9volume::extent(d.width, d.height, d.depth, d.levels) ||
                       !d9volume::pixel_bytes(d.format) || d.samples > 1 || d.usage ||
                       t.info.conv == Conv::Unsupported ||
                       t.info.pixel == MTLPixelFormatDepth32Float_Stencil8)) {
            index_texture(d.id, &t);
            fprintf(stderr, "d3d9 metal: invalid/unsupported volume %u\n", d.id);
            return;
        }
        if (!t.info.block && is_dxt(d.format)) {
            // No BC support (the iOS simulator): decoded to BGRA8 on upload.
            t.info.pixel = MTLPixelFormatBGRA8Unorm;
            t.info.conv = Conv::Direct;
            t.info.bytes = 4;
        }
        if (t.info.conv == Conv::Unsupported) {
            fprintf(stderr, "d3d9 metal: texture format %08x is drawn as BGRA8\n", d.format);
            t.info.conv = Conv::Direct;
        }
        bool depth =
            (d.usage & HOST_D9_USAGE_DEPTH) || t.info.pixel == MTLPixelFormatDepth32Float_Stencil8;
        if (depth)
            t.info.pixel = MTLPixelFormatDepth32Float_Stencil8;
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = volume                       ? MTLTextureType3D
                         : d.kind == HOST_D9_TEX_CUBE ? MTLTextureTypeCube
                                                      : MTLTextureType2D;
        td.pixelFormat = t.info.pixel;
        td.width = d.width ? d.width : 1;
        td.height = d.kind == HOST_D9_TEX_CUBE ? td.width : (d.height ? d.height : 1);
        td.depth = volume ? d.depth : 1;
        // Render targets and depth buffers render at the chosen scale; the
        // guest keeps seeing its own sizes.
        bool target = (d.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) || depth;
        if (target && !scale_chosen_ && d.kind == HOST_D9_TEX_2D) {
            // The first target is the back buffer; its height is what the
            // scale is measured against from then on.
            scale_chosen_ = true;
            base_rows_ = d.height;
            base_cols_ = d.width;
            const float want = wanted_scale();
            if (want > 0)
                scale_ = want;
            fprintf(stderr, "d3d9: render targets at %.2fx the game's size\n", scale_);
        }
        if (target && d.kind == HOST_D9_TEX_2D && scale_ != 1.0f) {
            float s = scale_;
            // Under 1 (a touch device's render quality, upscaled by MetalFX) only
            // the targets the screen's size - the back buffer, its depth, the
            // game's copies of the screen - draw smaller; a shadow map, a glow
            // or a reflection keeps the size the game's own settings gave it.
            if (s < 1.0f && (d.width < base_cols_ || d.height < base_rows_))
                s = 1.0f;
            // A target taller than the back buffer the scale was chosen for is not
            // supersampled past the drawable's height. Heroes V makes a 640x480
            // device first and its real one (3440x1440, say) a few frames later;
            // until follow_drawable catches up, 3x of that was 10320x4320 and a
            // gigabyte of targets nobody saw.
            int dw = 0, dh = 0;
            if (base_rows_ && d.height > base_rows_ && host_present_drawable(&dw, &dh) && dh > 0)
                s = std::min(s, std::max(1.0f, std::floor(8.0f * dh / d.height) / 8.0f));
            NSUInteger limit = 16384;
            while (s > 1.0f && (td.width * s > limit || td.height * s > limit))
                s -= 0.25f;
            t.scale = scale_ < 1.0f ? s : std::max(1.0f, s);
            td.width = (NSUInteger)std::lround(td.width * t.scale);
            td.height = (NSUInteger)std::lround(td.height * t.scale);
        }
        uint32_t max_levels = 1;
        for (uint32_t s = (uint32_t)std::max({td.width, td.height, td.depth}); s > 1; s >>= 1)
            ++max_levels;
        td.mipmapLevelCount = std::max<uint32_t>(1, std::min(d.levels ? d.levels : 1, max_levels));
        td.usage = MTLTextureUsageShaderRead;
        if ((d.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) || depth)
            td.usage |= MTLTextureUsageRenderTarget;
        td.storageMode = depth ? MTLStorageModePrivate : MTLStorageModeShared;
        if (t.info.block)
            td.usage = MTLTextureUsageShaderRead;
        t.texture = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_TEXTURE, t.texture, td.width, td.height, 1, d.format);
        t.cube = td.textureType == MTLTextureTypeCube;
        t.depth = td.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        {
            // Diagnostic (RECOMP_D3D9_STATS, Test73): every render target at least 1280 wide, with the
            // samples the game asked for, so an anti-aliasing change through a reset can be seen.
            static const bool stats = recomp_env("D3D9_STATS") != nullptr;
            if (stats && target && td.width >= 1280)
                fprintf(stderr, "d3d9 metal: render target %u defined %lux%lu, %u samples requested\n", d.id,
                        (unsigned long)td.width, (unsigned long)td.height, d.samples);
        }
        // Saving mode draws without multisampling: a target made while it is on resolves
        // nothing (the game's choice returns with the next target it makes).
        if (d.samples > 1 && target && d.kind == HOST_D9_TEX_2D && !t.info.block && t.texture &&
            !mods_energy_saving()) {
            NSUInteger n = d.samples >= 4 ? 4 : 2;
            while (n > 1 && ![mtl_ supportsTextureSampleCount:n])
                n /= 2;
            if (n > 1) {
                MTLTextureDescriptor *md = [MTLTextureDescriptor new];
                md.textureType = MTLTextureType2DMultisample;
                md.pixelFormat = td.pixelFormat;
                md.width = td.width;
                md.height = td.height;
                md.sampleCount = n;
                md.usage = MTLTextureUsageRenderTarget;
                if (depth)
                    md.usage |= MTLTextureUsageShaderRead; // Test121: copied by StretchRect
                md.storageMode = MTLStorageModePrivate;
                t.msaa = [mtl_ newTextureWithDescriptor:md];
                note_alloc(AS_MSAA, t.msaa, md.width, md.height, n, d.format);
                t.samples = t.msaa ? (uint32_t)n : 1;
                // Test70 (stable-bar evidence): which targets are really multisampled, once per size.
                if (t.msaa && msaa_logged_.size() < 16 && msaa_logged_.insert((uint64_t)td.width << 32 | td.height << 4 | n).second)
                    fprintf(stderr, "d3d9 metal: render target %lux%lu is multisampled x%lu (%u requested), resolved into its texture\n",
                            (unsigned long)td.width, (unsigned long)td.height, (unsigned long)n, d.samples);
            }
        }
        index_texture(d.id, &t);
        if (trace_)
            if (auto *e = trace_->event("texture_define", trace_context(), trace_texture(d.id))) {
                e->values[0] = d.width;
                e->values[1] = d.height;
                e->values[2] = d.format;
                e->values[3] = d.usage;
                e->values[4] = d.kind;
                e->values[5] = d.levels;
                e->values[6] = d.samples;
                e->values[7] = t.texture != nil;
                e->values[8] = (uint32_t)t.texture.width;
                e->values[9] = (uint32_t)t.texture.height;
                e->values[10] = d.depth;
            }
        if (!t.texture)
            fprintf(stderr, "d3d9 metal: texture %u (%ux%u fmt %08x) was not created\n", d.id,
                    d.width, d.height, d.format);
    }

    void drop(uint32_t id) override {
        auto it = textures_.find(id);
        if (it == textures_.end())
            return;
        end_pass();
        split_forget(id);
        if (trace_)
            trace_->event("texture_drop", trace_context(), trace_texture(id));
        if (it->second.imported)
            device_->destroy(it->second.imported);
        forget_fitted_depth(it->second);
        index_texture(id, nullptr);
        textures_.erase(it);
    }

    void upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                uint32_t pitch) override {
        auto it = textures_.find(id);
        if (it == textures_.end() || !it->second.texture || !bytes)
            return;
        Tex &t = it->second;
        if (level >= t.texture.mipmapLevelCount)
            return;
        const bool volume = t.desc.kind == HOST_D9_TEX_VOLUME;
        const uint32_t slices = volume ? std::max(t.desc.depth >> level, 1u) : t.cube ? 6u : 1u;
        if (face >= slices)
            return;
        if (t.info.pixel == MTLPixelFormatDepth32Float_Stencil8)
            return;     // the game's view of a depth buffer is not uploaded
        settle(t.used); // draws already encoded read the old contents
        if (trace_) {
            ++t.revision;
            if (auto *e = trace_->event("texture_upload", trace_context(), trace_texture(id))) {
                e->values[0] = face;
                e->values[1] = level;
                e->values[2] = pitch;
            }
        }
        uint32_t w = std::max<uint32_t>((uint32_t)t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(
            (uint32_t)(t.desc.kind == HOST_D9_TEX_CUBE ? t.desc.width : t.desc.height) >> level, 1);
        if (volume) {
            // The API supplies one independently pitched Z slice. Conversion
            // and lifetime follow the same settled-resource rules as 2D uploads.
            const uint32_t source_pitch = w * t.info.bytes;
            if (pitch < source_pitch)
                return;
            const uint8_t *src = bytes;
            uint32_t row = pitch;
            if (t.info.conv != Conv::Direct) {
                convert(t.info.conv, bytes, w, h, pitch, scratch_);
                src = scratch_.data();
                row = w * 4;
            }
            [t.texture replaceRegion:MTLRegionMake3D(0, 0, face, w, h, 1)
                         mipmapLevel:level
                               slice:0
                           withBytes:src
                         bytesPerRow:row
                       bytesPerImage:(NSUInteger)row * h];
            return;
        }
        MTLRegion region = MTLRegionMake2D(0, 0, w, h);
        if (t.scale != 1.0f) {
            // The game wrote a scaled target: stretch its pixels to the target's size.
            std::vector<uint8_t> packed;
            const uint8_t *src = bytes;
            uint32_t src_pitch = pitch;
            if (t.info.conv != Conv::Direct) {
                convert(t.info.conv, bytes, w, h, pitch, scratch_);
                src = scratch_.data();
                src_pitch = w * 4;
            }
            uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
            uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
            uint32_t bpp = t.info.conv == Conv::Direct ? t.info.bytes : 4;
            packed.resize((size_t)pw * ph * bpp);
            for (uint32_t y = 0; y < ph; ++y) {
                uint32_t sy = std::min(h - 1, (uint32_t)(y * (uint64_t)h / ph));
                for (uint32_t x = 0; x < pw; ++x) {
                    uint32_t sx = std::min(w - 1, (uint32_t)(x * (uint64_t)w / pw));
                    memcpy(&packed[((size_t)y * pw + x) * bpp],
                           src + (size_t)sy * src_pitch + (size_t)sx * bpp, bpp);
                }
            }
            [t.texture replaceRegion:MTLRegionMake2D(0, 0, pw, ph)
                         mipmapLevel:level
                               slice:face
                           withBytes:packed.data()
                         bytesPerRow:pw * bpp
                       bytesPerImage:0];
            return;
        }
        if (t.info.block) {
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:bytes
                         bytesPerRow:pitch
                       bytesPerImage:0];
        } else if (is_dxt(t.desc.format)) {
            decode_dxt(bytes, w, h, t.desc.format, scratch_);
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:scratch_.data()
                         bytesPerRow:w * 4
                       bytesPerImage:0];
        } else if (t.info.conv == Conv::Direct) {
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:bytes
                         bytesPerRow:pitch
                       bytesPerImage:0];
        } else {
            convert(t.info.conv, bytes, w, h, pitch, scratch_);
            [t.texture replaceRegion:region
                         mipmapLevel:level
                               slice:face
                           withBytes:scratch_.data()
                         bytesPerRow:w * 4
                       bytesPerImage:0];
        }
    }

    bool read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes, uint32_t pitch) override {
        auto it = textures_.find(id);
        if (it == textures_.end() || !it->second.texture)
            return false;
        Tex &t = it->second;
        if (!bytes || level >= t.texture.mipmapLevelCount)
            return false;
        const bool volume = t.desc.kind == HOST_D9_TEX_VOLUME;
        if (face >= (volume ? std::max(t.desc.depth >> level, 1u) : t.cube ? 6u : 1u))
            return false;
        if (t.texture.storageMode == MTLStorageModePrivate || t.info.block)
            return false;
        settle(t.used);
        uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
        uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
        if (volume) {
            // Only direct 32-bit formats have the existing host read contract.
            if (t.info.conv != Conv::Direct || t.info.bytes != 4 || pitch < pw * 4)
                return false;
            [t.texture getBytes:bytes
                    bytesPerRow:pitch
                  bytesPerImage:(NSUInteger)pitch * ph
                     fromRegion:MTLRegionMake3D(0, 0, face, pw, ph, 1)
                    mipmapLevel:level
                          slice:0];
            return true;
        }
        if (t.scale == 1.0f) {
            [t.texture getBytes:bytes
                    bytesPerRow:pitch
                  bytesPerImage:0
                     fromRegion:MTLRegionMake2D(0, 0, pw, ph)
                    mipmapLevel:level
                          slice:face];
            return true;
        }
        // A scaled target reads back at the game's size, one sample per pixel.
        uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(t.desc.height >> level, 1);
        std::vector<uint8_t> full((size_t)pw * ph * 4);
        [t.texture getBytes:full.data()
                bytesPerRow:pw * 4
              bytesPerImage:0
                 fromRegion:MTLRegionMake2D(0, 0, pw, ph)
                mipmapLevel:level
                      slice:face];
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t sx = std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                memcpy(bytes + (size_t)y * pitch + (size_t)x * 4, &full[((size_t)sy * pw + sx) * 4],
                       4);
            }
        }
        return true;
    }

    // GetRenderTargetData without the wait: a blit into a shared buffer at the end of the work
    // encoded so far; the command buffer's completion hands the image over (one sample per guest
    // pixel, as read() does). Nothing here waits for the GPU.
    void read_async(uint32_t tex_id, uint32_t face, uint32_t level, uint32_t ticket, uint64_t request,
                    ReadDeliver deliver) override {
        auto it = textures_.find(tex_id);
        if (it == textures_.end() || !it->second.texture) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        Tex &t = it->second;
        if (t.info.block || is_dxt(t.desc.format) || t.info.conv != Conv::Direct || t.info.bytes != 4 ||
            t.desc.kind == HOST_D9_TEX_VOLUME || level >= t.texture.mipmapLevelCount ||
            face >= (t.cube ? 6u : 1u) || t.texture.sampleCount > 1) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        const uint32_t pw = std::max<uint32_t>((uint32_t)t.texture.width >> level, 1);
        const uint32_t ph = std::max<uint32_t>((uint32_t)t.texture.height >> level, 1);
        const uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        const uint32_t h = std::max<uint32_t>(t.desc.height >> level, 1);
        id<MTLBuffer> buf = [mtl_ newBufferWithLength:(NSUInteger)pw * ph * 4
                                              options:MTLResourceStorageModeShared];
        note_alloc(AS_READBACK, buf, pw, ph, 1, 0);
        if (!buf) {
            deliver(ticket, request, nullptr, 0, 0);
            return;
        }
        end_pass();
        begin_frame();
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:t.texture
                         sourceSlice:face
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(pw, ph, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:(NSUInteger)pw * 4
            destinationBytesPerImage:(NSUInteger)pw * ph * 4];
        [blit endEncoding];
        t.used = serial_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
          if (cb.error) {
              deliver(ticket, request, nullptr, 0, 0);
              return;
          }
          const uint8_t *src = (const uint8_t *)buf.contents;
          if (pw == w && ph == h) {
              deliver(ticket, request, src, w, h);
              return;
          }
          std::vector<uint8_t> out((size_t)w * h * 4);
          for (uint32_t y = 0; y < h; ++y) {
              const uint32_t sy = std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
              for (uint32_t x = 0; x < w; ++x) {
                  const uint32_t sx = std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                  memcpy(&out[((size_t)y * w + x) * 4], src + ((size_t)sy * pw + sx) * 4, 4);
              }
          }
          deliver(ticket, request, out.data(), w, h);
        }];
    }

    // ---- buffers --------------------------------------------------------
    void buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                       uint32_t size) override {
        Buf &b = buffers_[id];
        if (offset + size > total || !bytes)
            return;
        if (trace_)
            trace_->event("buffer_before_upload", trace_context(), trace_buffer(id));
        if (b.shadow.size() < total)
            b.shadow.resize(total, 0);
        memcpy(b.shadow.data() + offset, bytes, size);
        const uint32_t end = offset + size;
        for (SpareBuf &s : b.spares) {
            s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
            s.hi = std::max(s.hi, end);
        }
        if (!b.buffer || b.buffer.length < total) {
            b.buffer = [mtl_ newBufferWithBytes:b.shadow.data()
                                         length:std::max<size_t>(b.shadow.size(), 4)
                                        options:MTLResourceStorageModeShared];
            note_alloc(AS_BUFFER, b.buffer, b.shadow.size(), id, 1, 0);
            b.used = 0;
            b.spares.clear();
            trace_buffer_upload(id, b, total, offset, size, true);
            return;
        }
        // A buffer an unfinished frame reads is replaced rather than written:
        // those draws keep the old one, and nothing waits. The replacement is
        // a spare whose frame has finished, brought up to date by copying
        // only what changed since it was set aside.
        const uint64_t done = completed_.load();
        if (b.used > done) {
            SpareBuf retired{b.buffer, b.used, offset, end};
            size_t pick = b.spares.size();
            for (size_t i = 0; i < b.spares.size(); ++i)
                if (b.spares[i].retired <= done && b.spares[i].buffer.length >= total) {
                    pick = i;
                    break;
                }
            if (pick < b.spares.size()) {
                SpareBuf s = b.spares[pick];
                b.spares.erase(b.spares.begin() + (ptrdiff_t)pick);
                if (s.hi > s.lo)
                    memcpy((uint8_t *)s.buffer.contents + s.lo, b.shadow.data() + s.lo,
                           s.hi - s.lo);
                b.buffer = s.buffer;
            } else {
                b.buffer = [mtl_ newBufferWithBytes:b.shadow.data()
                                             length:std::max<size_t>(b.shadow.size(), 4)
                                            options:MTLResourceStorageModeShared];
                note_alloc(AS_BUFFER_SPARE, b.buffer, b.shadow.size(), id, 1, 0);
            }
            b.spares.push_back(retired);
            if (b.spares.size() > 4)
                b.spares.erase(b.spares.begin());
            b.used = 0;
            trace_buffer_upload(id, b, total, offset, size, true);
            return;
        }
        memcpy((uint8_t *)b.buffer.contents + offset, bytes, size);
        trace_buffer_upload(id, b, total, offset, size, false);
    }
    void buffer_upload_inplace(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                               uint32_t size) override {
        auto it = buffers_.find(id);
        if (it == buffers_.end() || !it->second.buffer || it->second.buffer.length < total ||
            offset + size > total || !bytes) {
            buffer_upload(id, total, offset, bytes, size);
            return;
        }
        // A dynamic vertex buffer filled batch by batch (Heroes V appends into
        // 26 MB under NOOVERWRITE): replacing the whole buffer for every batch
        // a frame already drew from cost a 26 MB copy each time.
        Buf &b = it->second;
        if (b.shadow.size() < total)
            b.shadow.resize(total, 0);
        memcpy(b.shadow.data() + offset, bytes, size);
        const uint32_t end = offset + size;
        for (SpareBuf &s : b.spares) {
            s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
            s.hi = std::max(s.hi, end);
        }
        memcpy((uint8_t *)b.buffer.contents + offset, bytes, size);
        trace_buffer_upload(id, b, total, offset, size, false);
    }
    void buffer_drop(uint32_t id) override {
        end_pass();
        if (trace_)
            trace_->event("buffer_drop", trace_context(), trace_buffer(id));
        buffers_.erase(id);
    }

    void admission_accounting(const HostD9Accounting &a) override {
        if (trace_)
            trace_->accounting(a);
    }
    void rejected_draw(const HostD9Draw &d) override {
        if (auto *r = trace_draw(d))
            r->result = d9trace::Result::ShimRejected;
    }

    // ---- drawing --------------------------------------------------------
    void draw(const HostD9Draw &d) override {
        ++stat_calls_;
        d9trace::Draw *tr = trace_draw(d);
        if (!ensure_pass(d.target)) {
            if (tr)
                tr->result = d9trace::Result::NoPass;
            skip("no render pass");
            return;
        }
        if (tr) {
            tr->context = trace_context();
            tr->fitted_depth = pass_fitted_depth_;
            tr->samples = pass_samples_;
        }
        count_query();
        uint64_t vs_key = d.vs_key ? d.vs_key : d9sh::code_key(d.vs, d.vs_size);
        uint64_t ps_key = d.ps_key ? d.ps_key : d9sh::code_key(d.ps, d.ps_size);
        if (skip_by_shader(vs_key, ps_key)) {
            skip("RECOMP_D3D9_SKIP");
            return;
        }
        const d9sh::Program &vp = d9sh::program_for_key(vs_key, d.vs, d.vs_size);
        const d9sh::Program &pp = d9sh::program_for_key(ps_key, d.ps, d.ps_size);
        if (!vp.ok || !pp.ok) {
            if (tr)
                tr->result = d9trace::Result::UndecodedShader;
            skip("undecoded shader");
            const std::string why =
                std::string(vp.ok ? "pixel shader: " + pp.why : "vertex shader: " + vp.why);
            if (undecoded_.insert(why + (d.label ? d.label : "")).second)
                fprintf(stderr, "d3d9 metal: skipping draws: %s (%s)\n", why.c_str(),
                        d.label ? d.label : "no label");
            return;
        }
        d9msl::PixelVariant variant;
        Tex *bound[16];
        for (uint32_t s = 0; s < 16; ++s) {
            Tex *t = d.sampler_texture[s] ? tex_ptr(d.sampler_texture[s]) : nullptr;
            bound[s] = t;
            auto decl = pp.samplers.find(s);
            const bool declared2d = decl == pp.samplers.end() || decl->second == d9sh::S_2D;
            if (declared2d && t && t->texture && t->depth && t->desc.kind == HOST_D9_TEX_2D)
                variant.depth_mask |= (uint16_t)(1u << s);
        }
        if (pp.major < 2) {
            for (int s = 0; s < 8; ++s)
                if (bound[s] && bound[s]->desc.kind == HOST_D9_TEX_CUBE)
                    variant.cube_mask |= (uint16_t)(1u << s);
            variant.projected_mask = (uint16_t)d.projected_mask;
        }
        d9msl::VFetchLayout vfl;
        const d9msl::VFetchLayout *vf = d.vfetch ? vfetch_layout(d, vp, &vfl) : nullptr;
        if (d.vfetch && !vf && vfetch_unfit_++ == 0)
            fprintf(stderr, "d3d9 metal: a draw from the vertex fetch's buffer has an element type it "
                            "cannot decode; such draws read the buffer as it is\n");
        id<MTLFunction> vfn = function(vs_key, vp, variant, false, vf);
        id<MTLFunction> ffn = function(ps_key, pp, variant, true);
        if (!vfn || !ffn) {
            if (tr)
                tr->result = d9trace::Result::UntranslatedShader;
            skip("untranslated shader");
            return;
        }
        const uint32_t *rs = d.render_state;
        // Test86 HDR: after the core mod's pre-HUD scene marker (private render state 250 = 1; the mod has
        // set the backbuffer's alpha to 1 over the scene), every draw into the backbuffer - the HUD, the
        // mirror, menus - also clears alpha where it covers the scene (alpha blend ZERO, INVSRCALPHA) with its
        // colour unchanged, so the presenter expands highlights only where the scene still shows. Test109: it was
        // ZERO, ZERO, which cleared the whole quad - the minimap's square backing quad (transparent corners) left
        // a square of unexpanded road around the map in bright HDR scenes (an RC2 test report).
        if (rs[250] == 1 && pass_color_id_ && pass_color_id_ == last_presented_ && (rs[168] & 7)) {
            memcpy(hud_rs_, rs, sizeof hud_rs_);
            if (!hud_rs_[27]) { // no blending: colour ONE, ZERO, ADD
                hud_rs_[27] = 1;
                hud_rs_[19] = 2;
                hud_rs_[20] = 1;
                hud_rs_[171] = 1;
            }
            hud_rs_[206] = 1; // separate alpha: ZERO, INVSRCALPHA, ADD (dest alpha x (1 - HUD coverage))
            hud_rs_[207] = 1;
            hud_rs_[208] = 6;
            hud_rs_[209] = 1;
            // Test133 (test report: police lights in the mirror faint in HDR, right with HDR off): a draw of
            // the texture the mod marked as scene content (the rear-view mirror) leaves the scene marker as it is
            // (ZERO, ONE), so the presenter expands its highlights like the main view's.
            if (d.hdr_scene_texture) {
                hud_rs_[208] = 2;
                if (++hdr_scene_draws_ == 1 || hdr_scene_draws_ % 36000 == 0) {
                    fprintf(stderr, "d3d9 metal: HDR scene texture under the HUD (the mirror) kept its highlight expansion: %llu draws (Test133)\n",
                            (unsigned long long)hdr_scene_draws_);
                }
            }
            hud_rs_[168] |= 8;
            rs = hud_rs_;
            hdr_mask_frame_ = true;
        }

        // Vertex layout.
        float ascale[16][4];
        for (auto &a : ascale)
            for (float &x : a)
                x = 1.0f;
        uint64_t vkey =
            mix(mix((uint64_t)d.decl_id << 32 | d.decl_size, vs_key), fnv(d.decl, d.decl_size));
        {
            uint64_t strides = 0;
            for (int i = 0; i < 8; ++i)
                strides = strides * 1099511628211ull + d.stream[i].stride;
            vkey = mix(vkey, strides);
        }
        MTLVertexDescriptor *vdesc = vertex_descriptor(d, vp, vkey, ascale);

        // Pipeline.
        uint64_t key = mix(mix(vs_key, ps_key), variant.key());
        key = mix(key, vkey);
        if (vf)
            key = mix(key, vf->key());
        MTLPixelFormat cf[4] = {MTLPixelFormatInvalid, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        for (int i = 0; i < 4; ++i)
            if (id<MTLTexture> t = pass_color_[i]) {
                cf[i] = t.pixelFormat;
                key = mix(key, (uint64_t)cf[i] + 1000 * (uint64_t)i);
            }
        MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        key = mix(key, df);
        bool blend = rs[27] != 0;
        uint32_t cw[4] = {rs[168], rs[190], rs[191], rs[192]};
        uint64_t bkey =
            blend ? (1ull | (uint64_t)rs[19] << 1 | (uint64_t)rs[20] << 5 | (uint64_t)rs[171] << 9 |
                     (uint64_t)(rs[206] != 0) << 13 | (uint64_t)rs[207] << 14 |
                     (uint64_t)rs[208] << 18 | (uint64_t)rs[209] << 22)
                  : 0;
        key = mix(key, bkey);
        key = mix(key, (uint64_t)cw[0] | (uint64_t)cw[1] << 4 | (uint64_t)cw[2] << 8 |
                           (uint64_t)cw[3] << 12);
        id<MTLRenderPipelineState> pso = pipeline(key, vfn, ffn, vdesc, cf, df, blend, rs, cw);
        if (!pso) {
            if (tr)
                tr->result = d9trace::Result::NoPipeline;
            skip("no pipeline");
            return;
        }
        if (pso != es_.pipeline) {
            [enc_ setRenderPipelineState:pso];
            es_.pipeline = pso;
        }

        // Depth and stencil.
        bool zon = rs[7] != 0 && pass_depth_;
        uint64_t dkey = (uint64_t)zon | (uint64_t)(rs[14] != 0) << 1 | (uint64_t)rs[23] << 2 |
                        (uint64_t)(rs[52] != 0) << 6;
        bool stencil = rs[52] != 0 && pass_depth_;
        if (stencil)
            dkey = mix(dkey, fnv(&rs[53], 7 * sizeof(uint32_t)) ^ ((uint64_t)rs[185] << 1) ^
                                 fnv(&rs[186], 4 * sizeof(uint32_t)));
        {
            id<MTLDepthStencilState> dss = depth_state(dkey, zon, rs, stencil);
            if (dss != es_.depth) {
                [enc_ setDepthStencilState:dss];
                es_.depth = dss;
            }
        }
        if (stencil && es_.stencil_ref != (rs[57] & 0xff)) {
            es_.stencil_ref = rs[57] & 0xff;
            [enc_ setStencilReferenceValue:es_.stencil_ref];
        }

        // Rasterizer.
        {
            int cull = rs[22] == 2   ? (int)MTLCullModeFront
                       : rs[22] == 3 ? (int)MTLCullModeBack
                                     : (int)MTLCullModeNone;
            if (cull != es_.cull) {
                [enc_ setCullMode:(MTLCullMode)cull];
                es_.cull = cull;
            }
            int fill = rs[8] == 2 ? (int)MTLTriangleFillModeLines : (int)MTLTriangleFillModeFill;
            if (fill != es_.fill) {
                [enc_ setTriangleFillMode:(MTLTriangleFillMode)fill];
                es_.fill = fill;
            }
        }
        float bias, slope;
        memcpy(&bias, &rs[195], 4);
        memcpy(&slope, &rs[175], 4);
        // D3D's DEPTHBIAS is in depth units; Metal's in units of the minimum resolvable
        // difference, which for a float depth buffer is tiny, so scale it up.
        if (!(bias == es_.bias && slope == es_.slope)) {
            [enc_ setDepthBias:bias * 16777215.0f slopeScale:slope clamp:0.0f];
            es_.bias = bias;
            es_.slope = slope;
        }
        const double s = pass_scale_;
        MTLViewport vpm = {d.viewport[0] * s, d.viewport[1] * s, d.viewport[2] * s,
                           d.viewport[3] * s, d.depth_range[0],  d.depth_range[1]};
        if (!es_.viewport_valid || memcmp(&vpm, &es_.viewport, sizeof vpm) != 0) {
            [enc_ setViewport:vpm];
            es_.viewport = vpm;
            es_.viewport_valid = true;
        }
        set_scissor(rs[174] != 0, d.scissor);
        if (blend && (rs[19] == 14 || rs[19] == 15 || rs[20] == 14 || rs[20] == 15 ||
                      rs[207] == 14 || rs[208] == 14)) {
            uint32_t f = rs[193];
            [enc_ setBlendColorRed:((f >> 16) & 255) / 255.0f
                             green:((f >> 8) & 255) / 255.0f
                              blue:(f & 255) / 255.0f
                             alpha:((f >> 24) & 255) / 255.0f];
        }

        // Constants and parameters.
        D9VSParams vparams{};
        memcpy(vparams.bconst, d.vbool, sizeof vparams.bconst);
        // Direct3D 9 samples a pixel at its integer corner, here at its centre:
        // geometry moves half a pixel right and down to match. 63/64 of it, as
        // Wine does, so an edge never lands exactly on a pixel centre.
        vparams.halfpix[0] = d.viewport[2] ? (63.0f / 64.0f) / d.viewport[2] : 0.0f;
        vparams.halfpix[1] = d.viewport[3] ? -(63.0f / 64.0f) / d.viewport[3] : 0.0f;
        memcpy(vparams.ascale, ascale, sizeof ascale);
        [enc_ setVertexBytes:&vparams length:sizeof vparams atIndex:17];
        if (vf) {
            for (uint32_t k = 0; k < (vf->check ? 5u : 4u); ++k) {
                auto b = buffers_.find(HOST_D9_VFETCH_BUFFER(k));
                id<MTLBuffer> mb = b != buffers_.end() ? b->second.buffer : nil;
                if (!mb) {
                    skip("vertex fetch buffer missing");
                    return;
                }
                b->second.used = serial_;
                [enc_ setVertexBuffer:mb offset:0 atIndex:18 + k];
            }
            if (vf->check) {
                const uint32_t tag = d.vfetch_tag;
                [enc_ setVertexBytes:&tag length:sizeof tag atIndex:23];
                if (vfetch_tags_.empty() || vfetch_tags_.back().first != serial_)
                    vfetch_tags_.push_back({serial_, tag});
                else
                    vfetch_tags_.back().second = tag;
                if (vfetch_tags_.size() > 64)
                    vfetch_tags_.erase(vfetch_tags_.begin());
            }
        }
        set_constants(true, d.vconst, d.vconst_count, vp);
        // RECOMP_D3D9_TRACK_VCONST=<vs key>:<first reg>:<count>: one line per frame with those
        // registers of the shader's first draw in the frame, to see how they move over time.
        if (static const char *track = recomp_env("D3D9_TRACK_VCONST"); track && d.vconst) {
            static uint64_t key = 0;
            static uint32_t first = 0, count = 0;
            static uint64_t last_frame = ~0ull;
            if (!key) {
                key = strtoull(track, nullptr, 16);
                const char *a = strchr(track, ':');
                first = a ? (uint32_t)atoi(a + 1) : 0;
                const char *b = a ? strchr(a + 1, ':') : nullptr;
                count = b ? (uint32_t)atoi(b + 1) : 4;
            }
            if (d.vs_key == key && presents_ != last_frame) {
                last_frame = presents_;
                fprintf(stderr, "vtrack %llu", (unsigned long long)presents_);
                for (uint32_t i = first; i < first + count && i < d.vconst_count; ++i)
                    fprintf(stderr, " c%u=%.7g,%.7g,%.7g,%.7g", i, d.vconst[i * 4], d.vconst[i * 4 + 1],
                            d.vconst[i * 4 + 2], d.vconst[i * 4 + 3]);
                fprintf(stderr, "\n");
            }
        }
        // RECOMP_D3D9_DUMP_VCONST=<vs key in hex>: print that shader's vertex constants once.
        if (static const char *want = recomp_env("D3D9_DUMP_VCONST"); want && d.vconst) {
            // The first matching draw and then every 600th, five dumps in all.
            // RECOMP_D3D9_DUMP_VCONST_FRAME=N instead dumps every matching draw of frame N, so
            // the passes of one frame can be compared (zfunc and colour mask tell them apart).
            static uint32_t seen = 0, dumps = 0;
            static const uint64_t at_frame = recomp_env("D3D9_DUMP_VCONST_FRAME")
                                                 ? strtoull(recomp_env("D3D9_DUMP_VCONST_FRAME"), nullptr, 10)
                                                 : 0;
            const bool match = strtoull(want, nullptr, 16) == d.vs_key;
            const bool take = at_frame ? (match && presents_ == at_frame && dumps < 64)
                                       : (match && seen++ % 600 == 0 && dumps < 5);
            if (take) {
                ++dumps;
                fprintf(stderr, "d3d9 vconst dump %u (frame %llu, zfunc %u zwrite %u colourmask %x)\n",
                        dumps, (unsigned long long)presents_, rs[23], rs[14], rs[168]);
                for (uint32_t i = 0; i < d.vconst_count; ++i)
                    fprintf(stderr, "d3d9 vconst c%u = %g %g %g %g\n", i, d.vconst[i * 4],
                            d.vconst[i * 4 + 1], d.vconst[i * 4 + 2], d.vconst[i * 4 + 3]);
                // D3DVERTEXELEMENT9: stream, offset (16-bit), type, method, usage, index.
                for (uint32_t o = 0; d.decl && o + 8 <= d.decl_size; o += 8) {
                    const uint8_t *e = (const uint8_t *)d.decl + o;
                    if (e[0] == 0xff)
                        break;
                    fprintf(stderr, "d3d9 decl stream %u offset %u type %u usage %u index %u\n",
                            e[0] | e[1] << 8, e[2] | e[3] << 8, e[4], e[6], e[7]);
                }
                for (int i = 0; i < 8; ++i)
                    if (d.stream[i].buffer)
                        fprintf(stderr, "d3d9 stream %d buffer %u offset %u stride %u\n", i,
                                d.stream[i].buffer, d.stream[i].offset, d.stream[i].stride);
                fprintf(stderr, "d3d9 draw base %d start %u count %u fog %u table %u colour %08x\n",
                        d.base_vertex, d.start, d.primitive_count, rs[28], rs[35], rs[34]);
                if (auto b = buffers_.find(d.stream[0].buffer); b != buffers_.end()) {
                    const auto &sh = b->second.shadow;
                    const uint32_t stride = d.stream[0].stride ? d.stream[0].stride : 32;
                    for (uint32_t v = 0; v < 6; ++v) {
                        size_t at = d.stream[0].offset + (size_t)(int64_t(d.base_vertex) + v * 7) * stride;
                        if (at + stride > sh.size())
                            break;
                        char line[256];
                        int n = 0;
                        for (uint32_t k = 0; k < stride && n < 240; ++k)
                            n += snprintf(line + n, sizeof line - n, "%02x%s", sh[at + k], (k & 3) == 3 ? " " : "");
                        fprintf(stderr, "d3d9 vertex %u: %s\n", v * 7, line);
                    }
                }
            }
        }
        D9PSParams pparams{};
        memcpy(pparams.bconst, d.pbool, sizeof pparams.bconst);
        pparams.alpha_func = rs[15] ? (int32_t)rs[25] : 8;
        pparams.alpha_ref = (rs[24] & 0xff) / 255.0f;
        if (rs[28]) {
            uint32_t table = rs[35];
            pparams.fog_mode = table == 3 ? 2 : table == 1 ? 3 : table == 2 ? 4 : 1;
            uint32_t fc = rs[34];
            pparams.fog_color[0] = ((fc >> 16) & 255) / 255.0f;
            pparams.fog_color[1] = ((fc >> 8) & 255) / 255.0f;
            pparams.fog_color[2] = (fc & 255) / 255.0f;
            pparams.fog_color[3] = 1.0f;
            memcpy(&pparams.fog_start, &rs[36], 4);
            memcpy(&pparams.fog_end, &rs[37], 4);
            memcpy(&pparams.fog_density, &rs[38], 4);
        }
        [enc_ setFragmentBytes:&pparams length:sizeof pparams atIndex:1];
        set_constants(false, d.pconst, d.pconst_count, pp);

        // Textures and samplers.
        if (pp.sampler_mask < 0) {
            uint32_t cubes = 0;
            for (const auto &kv : pp.samplers)
                if (kv.first < 16 && kv.second == d9sh::S_CUBE)
                    cubes |= 1u << kv.first;
            pp.cube_samplers = cubes;
            pp.sampler_mask = d9msl::pixel_sampler_mask(pp);
        }
        uint32_t used = (uint32_t)pp.sampler_mask;
        if (tr)
            tr->used_samplers = used;
        for (uint32_t s = 0; s < 16; ++s) {
            if (!(used >> s & 1))
                continue;
            Tex *t = bound[s];
            id<MTLTexture> tex = t ? t->texture : nil;
            if (tr && !tex)
                tr->missing_textures |= 1u << s;
            if (!tex) {
                // RECOMP_D3D9_MISSING_TRACE: say once per shader and stage why a sampler the
                // shader reads has no texture - nothing set, or one the backend never created.
                static const bool why = recomp_env("D3D9_MISSING_TRACE") != nullptr;
                static std::set<std::pair<uint64_t, uint32_t>> told;
                if (why && told.insert({d.ps_key, s}).second)
                    fprintf(stderr,
                            "d3d9 missing: ps %016llx stage %u: id %u %s (fmt %08x %ux%u kind %u)\n",
                            (unsigned long long)d.ps_key, s, d.sampler_texture[s],
                            !d.sampler_texture[s] ? "nothing set"
                            : !t                  ? "unknown to the backend"
                                                  : "has no Metal texture",
                            t ? t->desc.format : 0u, t ? t->desc.width : 0u,
                            t ? t->desc.height : 0u, t ? (unsigned)t->desc.kind : 0u);
            }
            bool cube = pp.major >= 2 ? (pp.cube_samplers >> s & 1) : (variant.cube_mask >> s & 1);
            auto declaration = pp.samplers.find(s);
            const bool volume = pp.major >= 2 && declaration != pp.samplers.end() &&
                                declaration->second == d9sh::S_VOLUME;
            if (tex && (t->cube != cube || (t->desc.kind == HOST_D9_TEX_VOLUME) != volume)) {
                if (tr)
                    tr->incompatible_textures |= 1u << s;
                tex = nil;
            }
            const bool depth = variant.depth_mask >> s & 1;
            if (tex && t->depth != depth) {
                if (tr)
                    tr->incompatible_textures |= 1u << s;
                tex = nil;
            }
            // Nor can the depth buffer this pass writes.
            if (tex && depth && tex == pass_depth_) {
                if (tr)
                    tr->feedback_textures |= 1u << s;
                tex = nil;
            }
            // A texture that is also this pass's target cannot be read from.
            for (int i = 0; i < 4 && tex; ++i)
                if (tex == pass_color_[i]) {
                    if (tr)
                        tr->feedback_textures |= 1u << s;
                    tex = nil;
                }
            if (tex)
                t->used = serial_;
            id<MTLTexture> use = tex     ? tex
                                 : depth ? placeholder_depth()
                                         : placeholder(cube, volume);
            if (use != es_.texture[s]) {
                [enc_ setFragmentTexture:use atIndex:s];
                es_.texture[s] = use;
            }
            id<MTLSamplerState> smp = sampler(d.sampler_state + s * 14);
            if (smp != es_.sampler[s]) {
                [enc_ setFragmentSamplerState:smp atIndex:s];
                es_.sampler[s] = smp;
            }
        }

        // Geometry.
        if (tr)
            tr->vertex_stream_mask = stream_mask_;
        for (int i = 0; i < 8; ++i) {
            if (!(stream_mask_ >> i & 1))
                continue;
            const HostD9Stream &st = d.stream[i];
            if (st.buffer) {
                auto b = buffers_.find(st.buffer);
                if (b == buffers_.end() || !b->second.buffer) {
                    if (tr)
                        tr->result = d9trace::Result::MissingVertexBuffer;
                    skip("missing vertex buffer");
                    return;
                }
                b->second.used = serial_;
                [enc_ setVertexBuffer:b->second.buffer offset:st.offset atIndex:i];
            } else if (d.inline_vertices) {
                bind_inline(i, d.inline_vertices, d.inline_bytes);
            } else {
                [enc_ setVertexBuffer:zero_ offset:0 atIndex:i];
            }
        }
        if (vdesc_needs_zero_)
            [enc_ setVertexBuffer:zero_ offset:0 atIndex:8];
        const d9trace::Result encoded = encode_primitives(d);
        if (tr)
            tr->result = encoded;
        if (trace_ && encoded == d9trace::Result::Encoded) {
            for (int i = 0; i < 4; ++i)
                if (pass_color_[i] && cw[i])
                    textures_[d.target.color[i].id].written = serial_;
            if (pass_depth_ && !pass_fitted_depth_ && ((zon && rs[14]) || stencil))
                textures_[d.target.depth.id].written = serial_;
        }
        ++draws_;
        if (probing()) {
            char vs_hash[20], ps_hash[20];
            snprintf(vs_hash, sizeof vs_hash, "%016llx", (unsigned long long)vs_key);
            snprintf(ps_hash, sizeof ps_hash, "%016llx", (unsigned long long)ps_key);
            float c0[4] = {0, 0, 0, 0};
            if (d.vconst && d.vconst_count)
                memcpy(c0, d.vconst, sizeof c0);
            fprintf(
                stderr,
                "probe draw %llu: rt %u/%u/%u depth %u vp %d,%d %dx%d prim %u x%u start %u base %d "
                "ib %u stream0 %u+%u/%u inline %u vs %s ps %s z %u/%u/%u cull %u blend %u %u/%u "
                "cw %x atest %u/%u fog %u tex0 %u c0 %g %g %g %g %s\n",
                (unsigned long long)draws_, d.target.color[0].id, d.target.color[0].face,
                d.target.color[0].level, d.target.depth.id, d.viewport[0], d.viewport[1],
                d.viewport[2], d.viewport[3], d.primitive, d.primitive_count, d.start,
                d.base_vertex, d.index_buffer, d.stream[0].buffer, d.stream[0].offset,
                d.stream[0].stride, d.inline_bytes, vs_hash, ps_hash, rs[7], rs[14], rs[23], rs[22],
                rs[27], rs[19], rs[20], rs[168], rs[15], rs[25], rs[28], d.sampler_texture[0],
                c0[0], c0[1], c0[2], c0[3], d.label ? d.label : "-");
        }
    }

    // ---- occlusion queries ----------------------------------------------
    // Every command buffer with a query in it has a buffer of 64-bit sample
    // counts. Each render pass drawn under a query gets a slot of its own; the
    // query's count is the sum of its slots once the command buffer has run.
    static constexpr NSUInteger kVisSlots = 1024;
    struct Query {
        struct Slot {
            id<MTLBuffer> buffer;
            uint32_t index;
            uint64_t serial;
        };
        std::vector<Slot> slots; // not yet counted
        uint64_t counted = 0;
        bool overflow = false;
        bool ended = false;
    };
    void query_begin(uint32_t id) override {
        if (trace_)
            trace_->event("query_begin", trace_context(), {id});
        Query &q = queries_[id];
        q = Query{};
        query_ = id;
        pass_slot_ = -1;
        if (enc_ && !pass_vis_)
            end_pass(); // the next draw opens a pass that can count
    }
    void query_end(uint32_t id) override {
        if (trace_)
            trace_->event("query_end", trace_context(), {id});
        if (query_ != id)
            return;
        if (enc_ && pass_slot_ >= 0)
            [enc_ setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
        pass_slot_ = -1;
        query_ = 0;
        queries_[id].ended = true;
    }
    int query_result(uint32_t id, uint32_t *count) override {
        auto it = queries_.find(id);
        if (it == queries_.end() || !it->second.ended)
            return -1;
        Query &q = it->second;
        if (q.overflow) {
            *count = 1;
            return 1;
        }
        if (!fold(q))
            return 0; // its frame has not finished on the GPU
        *count = (uint32_t)std::min<uint64_t>(q.counted, 0xffffffffu);
        if (trace_)
            if (auto *e = trace_->event("query_result", trace_context(), {id}))
                e->values[0] = *count;
        return 1;
    }
    // Adds the slots whose command buffers have run; true when none is left.
    bool fold(Query &q) {
        uint64_t done = completed_.load();
        size_t keep = 0;
        for (auto &s : q.slots) {
            if (s.serial <= done)
                q.counted += ((const uint64_t *)s.buffer.contents)[s.index];
            else
                q.slots[keep++] = s;
        }
        q.slots.resize(keep);
        return keep == 0;
    }
    void query_drop(uint32_t id) override {
        if (query_ == id)
            query_end(id);
        queries_.erase(id);
    }
    void count_query() {
        if (!query_ || pass_slot_ >= 0)
            return;
        Query &q = queries_[query_];
        if (!pass_vis_ || vis_used_ >= kVisSlots) {
            q.overflow = true;
            return;
        }
        pass_slot_ = (int)vis_used_++;
        ((uint64_t *)vis_.contents)[pass_slot_] = 0;
        [enc_ setVisibilityResultMode:MTLVisibilityResultModeCounting
                               offset:(NSUInteger)pass_slot_ * 8];
        q.slots.push_back({vis_, (uint32_t)pass_slot_, serial_});
    }
    id<MTLBuffer> vis_buffer() {
        if (vis_)
            return vis_;
        if (!vis_pool_.empty() && vis_pool_.front().second <= completed_.load())
            for (auto &[id, q] : queries_)
                fold(q);
        for (size_t i = 0; i < vis_pool_.size(); ++i) {
            if (vis_pool_[i].second <= completed_.load()) {
                vis_ = vis_pool_[i].first;
                vis_pool_.erase(vis_pool_.begin() + i);
                break;
            }
        }
        if (!vis_)
            vis_ = [mtl_ newBufferWithLength:kVisSlots * 8 options:MTLResourceStorageModeShared];
        vis_used_ = 0;
        return vis_;
    }

    bool probing() const {
        return probe_frame_ && presents_ + 1 == probe_frame_;
    }
    d9trace::Context trace_context() const {
        return {serial_, submitted_, completed_.load(), pass_serial_};
    }
    d9trace::Resource trace_texture(uint32_t id) {
        const Tex *t = tex_ptr(id);
        return t ? d9trace::Resource{id, t->generation, t->revision, t->used, t->written}
                 : d9trace::Resource{id};
    }
    void trace_texture_desc(uint32_t id, uint32_t *desc) {
        const Tex *t = tex_ptr(id);
        if (!t)
            return;
        desc[0] = t->desc.width;
        desc[1] = t->desc.height;
        desc[2] = t->desc.format;
        desc[3] = t->desc.kind;
    }
    d9trace::Resource trace_buffer(uint32_t id) const {
        const auto it = buffers_.find(id);
        if (it == buffers_.end())
            return {id};
        const Buf &b = it->second;
        return {id, b.generation, b.revision, b.used, 0};
    }
    void trace_buffer_upload(uint32_t id, Buf &b, uint32_t total, uint32_t offset, uint32_t size,
                             bool replaced) {
        if (!trace_)
            return;
        if (replaced)
            b.generation = ++resource_generation_;
        ++b.revision;
        if (auto *e = trace_->event("buffer_upload", trace_context(), trace_buffer(id))) {
            e->values[0] = total;
            e->values[1] = offset;
            e->values[2] = size;
            e->values[3] = replaced;
            e->values[4] = b.buffer != nil;
        }
    }
    d9trace::Draw *trace_draw(const HostD9Draw &d) {
        if (!trace_)
            return nullptr;
        auto *r = trace_->record(d);
        if (!r)
            return nullptr;
        r->context = trace_context();
        r->vs = d.vs_key ? d.vs_key : (d.vs ? d9sh::code_key(d.vs, d.vs_size) : 0);
        r->ps = d.ps_key ? d.ps_key : (d.ps ? d9sh::code_key(d.ps, d.ps_size) : 0);
        for (int i = 0; i < 16; ++i) {
            r->textures[i] = trace_texture(d.sampler_texture[i]);
            trace_texture_desc(d.sampler_texture[i], r->texture_desc[i]);
        }
        for (int i = 0; i < 8; ++i)
            r->buffers[i] = trace_buffer(d.stream[i].buffer);
        r->buffers[8] = trace_buffer(d.index_buffer);
        for (int i = 0; i < 4; ++i) {
            r->attachments[i] = trace_texture(d.target.color[i].id);
            trace_texture_desc(d.target.color[i].id, r->attachment_desc[i]);
        }
        r->attachments[4] = trace_texture(d.target.depth.id);
        trace_texture_desc(d.target.depth.id, r->attachment_desc[4]);
        return r;
    }
    // The render queue orders this request after preceding commands. It copies
    // no live resource bytes and does not wait for the GPU. File I/O is confined
    // to this explicit trigger; normal frames only overwrite bounded metadata.
    bool trace_flush(const char *tag) {
        char path[1024];
        os_mkdir(host_dump_dir());
        snprintf(path, sizeof path, "%s/trace-%llu-%u.jsonl", host_dump_dir(),
                 (unsigned long long)os_wall_time_us(), ++trace_flushes_);
        int fd = os_fd_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL);
        FILE *file = fd < 0 ? nullptr : static_cast<FILE *>(os_fdopen(fd, "wb"));
        if (!file) {
            if (fd >= 0)
                os_fd_close(fd);
            fprintf(stderr, "d3d9 trace: could not create %s\n", path);
            return false;
        }
        bool ok = trace_->write(file, tag, trace_context());
        long bytes = ftell(file);
        if (bytes > 0)
            trace_bytes_ += (uint64_t)bytes;
        ok = fclose(file) == 0 && bytes >= 0 && ok;
        fprintf(stderr, "d3d9 trace: %s %s\n", ok ? "wrote" : "write failed", path);
        return ok;
    }
    struct CaptureSave {
        std::unique_ptr<d9trace::Ring> ring;
        bool ok = false, complete = false;
    };
    static std::unique_ptr<d9trace::Ring> capture_ring() {
        return std::make_unique<d9trace::Ring>(d9trace::CaptureControl::draw_capacity,
                                               d9trace::CaptureControl::event_capacity,
                                               d9trace::CaptureControl::history);
    }
    void capture_poll() {
        if (capture_job_.valid() &&
            capture_job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto saved = capture_job_.get();
            capture_spare_ = std::move(saved.ring);
            capture_control_.saved(saved.ok, saved.complete);
            fprintf(stderr, "capture: C%llu %s reason=%s\n",
                    (unsigned long long)capture_control_.id,
                    capture_state::name(capture_control_.state),
                    capture_state::reason(capture_control_.reason));
        }
    }
    void capture_save() {
        // Move preallocated storage, never copy draw arrays or wait for disk/GPU.
        // The worker owns all frozen values; it cannot touch live renderer state.
        auto frozen = std::move(trace_);
        trace_ = std::move(capture_spare_);
        trace_->reset(frozen->current_frame_number());
        trace_->retain = false;
        const auto control = capture_control_;
        const auto summaries = capture_summaries_;
        const auto summary_count = capture_summary_count_;
        const std::string dir = host_dump_dir();
        const auto epoch = capture_epoch_;
        capture_job_ = std::async(std::launch::async, [frozen = std::move(frozen), control,
                                                       summaries, summary_count, dir,
                                                       epoch]() mutable {
            CaptureSave result;
            bool records_complete = true;
            uint64_t first = 0, last = 0, first_ns = 0, last_ns = 0;
            uint32_t samples = 0;
            frozen->visit([&](const d9trace::Frame &f) {
                if (!first) {
                    first = f.number;
                    first_ns = f.monotonic_ns;
                }
                last = f.number;
                last_ns = f.monotonic_ns;
                ++samples;
                records_complete &= f.complete && !f.draw_overflow && !f.event_overflow;
            });
            char path[1024];
            snprintf(path, sizeof path, "%s/capture-%llu-%03llu.jsonl", dir.c_str(),
                     (unsigned long long)epoch, (unsigned long long)control.id);
            os_mkdir(dir.c_str());
            int fd = os_fd_open(path, OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL);
            FILE *file = fd < 0 ? nullptr : static_cast<FILE *>(os_fdopen(fd, "wb"));
            if (!file && fd >= 0)
                os_fd_close(fd);
            bool ok = file != nullptr;
            uint64_t timeline_first = 0, timeline_last = 0, rows = 0;
            if (file) {
                setvbuf(file, nullptr, _IOFBF, 65536);
                ok = frozen->write(file, "sampled-capture", {});
                const size_t count = std::min<uint64_t>(summary_count, summaries.size());
                for (size_t i = 0; i < count; ++i) {
                    const auto &r = summaries[(summary_count - count + i) % summaries.size()];
                    if (r.frame < first || r.frame > last)
                        continue;
                    if (!timeline_first)
                        timeline_first = r.frame;
                    timeline_last = r.frame;
                    ++rows;
                    fprintf(
                        file,
                        "{\"kind\":\"frame_counts\",\"frame\":%llu,\"monotonic_ns\":%llu,\"wall_"
                        "us\":%llu,\"detailed\":%s,\"draws_attempted\":%llu,\"draws_retained\":%"
                        "llu,\"draws_dropped\":%llu,\"events_attempted\":%llu,\"events_retained\":%"
                        "llu,\"events_dropped\":%llu,\"context\":",
                        (unsigned long long)r.frame, (unsigned long long)r.ns,
                        (unsigned long long)r.wall_us, r.detailed ? "true" : "false",
                        (unsigned long long)r.draws_attempted, (unsigned long long)r.draws_retained,
                        (unsigned long long)r.draws_dropped, (unsigned long long)r.events_attempted,
                        (unsigned long long)r.events_retained,
                        (unsigned long long)r.events_dropped);
                    d9trace::context_json(file, r.context);
                    d9trace::accounting_json(file, r.accounting);
                    fprintf(file, "}\n");
                }
                result.complete = records_complete && samples > 1 &&
                                  first_ns < control.trigger_ns && last_ns >= control.deadline &&
                                  timeline_first == first && timeline_last == last &&
                                  rows == last - first + 1;
                fprintf(file,
                        "{\"kind\":\"capture_manifest\",\"capture_id\":%llu,\"status\":\"%s\","
                        "\"scope\":\"unfiltered full frames sampled at 2Hz; other frames counts "
                        "only; no "
                        "pixels\",\"history_samples\":16,\"post_ns\":6000000000,\"trigger_ns\":%"
                        "llu,\"trigger_render_frame\":%llu,\"first_ns\":%llu,\"last_ns\":%llu,"
                        "\"first_frame\":%llu,\"last_frame\":%llu,\"samples\":%u,\"timeline_rows\":"
                        "%llu,\"display_identity\":\"join frame_timings render_frame; presenter "
                        "frame_id differs; display_ack records actual acknowledgement\"}\n",
                        (unsigned long long)control.id, result.complete ? "COMPLETE" : "INCOMPLETE",
                        (unsigned long long)control.trigger_ns,
                        (unsigned long long)control.trigger_frame, (unsigned long long)first_ns,
                        (unsigned long long)last_ns, (unsigned long long)first,
                        (unsigned long long)last, samples, (unsigned long long)rows);
                ok = !ferror(file) && ok;
                ok = fclose(file) == 0 && ok;
            }
            result.ok = ok;
            fprintf(stderr, "capture: saved C%llu %s %s\n", (unsigned long long)control.id,
                    ok && result.complete ? "COMPLETE" : "INCOMPLETE", path);
            result.ring = std::move(frozen);
            return result;
        });
    }
    CaptureMarker capture_present(uint64_t frame, uint64_t ns, uint64_t wall,
                                  d9trace::Context context) {
        capture_poll();
        const auto &f = trace_->current();
        const bool detail = trace_->retain;
        auto &r = capture_summaries_[capture_summary_count_++ % capture_summaries_.size()];
        r = {f.accounting,
             frame,
             ns,
             wall,
             f.draws_attempted,
             f.events_attempted,
             f.draws,
             f.events,
             f.draw_overflow,
             f.event_overflow,
             context,
             detail};
        trace_->present(context, wall, ns);
        bool save = capture_control_.frame(ns, detail);
        const auto marker = capture_control_.marker(frame, ns, detail);
        if (save)
            capture_save();
        trace_->retain = capture_control_.next_detail;
        return marker;
    }
    void probe_next(const char *tag) override {
        if (capture_mode_) {
            capture_poll();
            const uint64_t now = os_monotonic_ns();
            bool accepted = capture_control_.request(now, trace_->current_frame_number());
            fprintf(stderr, "capture: request %s C%llu %s reason=%s mono_ns=%llu R=%llu\n",
                    accepted ? "accepted" : "REJECTED", (unsigned long long)capture_control_.id,
                    capture_state::name(capture_control_.state),
                    capture_state::reason(capture_control_.reason), (unsigned long long)now,
                    (unsigned long long)trace_->current_frame_number());
            return;
        }
        if (trace_) {
            // One explicitly armed, bounded forward capture. The ordinary ten-frame
            // ring cannot retain a fleeting fault until a human reacts to it.
            if (const char *burst = recomp_env("D3D9_TRACE_BURST")) {
                char *end = nullptr;
                unsigned long count = strtoul(burst, &end, 10);
                if (!*burst || *end || count < 1 || count > 240 || burst_used_) {
                    fprintf(stderr, "d3d9 burst: rejected (invalid count or already used)\n");
                    return;
                }
                burst_used_ = true;
                burst_start_ = presents_ + 2; // omit the partially drawn frame
                burst_left_ = (uint32_t)count;
                burst_trace_start_ = trace_bytes_;
                fprintf(stderr, "d3d9 burst: armed %lu complete frames; no pre-trigger pixels\n",
                        count);
            }
            if (!trace_flush(tag))
                burst_left_ = 0;
            return;
        }
        probe_frame_ = presents_ + 2; // the frame after the one in progress, whole
        probe_tag_ = tag ? tag : "";
        fprintf(stderr, "probe %s: frame %llu\n", probe_tag_.c_str(),
                (unsigned long long)probe_frame_);
    }
    void probe_dump() {
        settle(serial_);
        for (auto &kv : textures_) {
            Tex &t = kv.second;
            if (!t.texture || t.info.pixel != MTLPixelFormatBGRA8Unorm ||
                t.desc.kind != HOST_D9_TEX_2D)
                continue;
            uint32_t w = (uint32_t)t.texture.width, h = (uint32_t)t.texture.height;
            std::vector<uint8_t> bgra((size_t)w * h * 4), rgb((size_t)w * h * 3);
            [t.texture getBytes:bgra.data()
                    bytesPerRow:w * 4
                     fromRegion:MTLRegionMake2D(0, 0, w, h)
                    mipmapLevel:0];
            size_t lit = 0;
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
                rgb[i * 3] = bgra[i * 4 + 2];
                rgb[i * 3 + 1] = bgra[i * 4 + 1];
                rgb[i * 3 + 2] = bgra[i * 4];
                lit += (bgra[i * 4] | bgra[i * 4 + 1] | bgra[i * 4 + 2]) > 8;
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/probe%s%s_%u_%ux%u_%s.ppm", host_dump_dir(),
                     probe_tag_.empty() ? "" : "_", probe_tag_.c_str(), kv.first, w, h,
                     (t.desc.usage & HOST_D9_USAGE_RENDERTARGET) ? "rt" : "tex");
            host_write_ppm(path, rgb.data(), (int)w, (int)h);
            // The alpha channel as grey: games keep masks there (Heroes V's shadow pass writes
            // only alpha into its 512x512 light-space target).
            for (size_t i = 0, n = (size_t)w * h; i < n; ++i)
                rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = bgra[i * 4 + 3];
            snprintf(path, sizeof path, "%s/probe%s%s_%u_%ux%u_%s_alpha.ppm", host_dump_dir(),
                     probe_tag_.empty() ? "" : "_", probe_tag_.c_str(), kv.first, w, h,
                     (t.desc.usage & HOST_D9_USAGE_RENDERTARGET) ? "rt" : "tex");
            host_write_ppm(path, rgb.data(), (int)w, (int)h);
            fprintf(stderr, "probe texture %u %ux%u fmt %08x usage %u: %zu of %zu lit\n", kv.first,
                    w, h, t.desc.format, t.desc.usage, lit, (size_t)w * h);
        }
    }

    // RECOMP_D3D9_SKIP=hex,hex,... (diagnostic): draws whose vertex or pixel shader code key
    // starts with one of these hex prefixes (as the D3D9 trace prints them) are not drawn,
    // to find which draw makes a visible artefact.
    static bool skip_by_shader(uint64_t vs_key, uint64_t ps_key) {
        static const std::vector<std::string> prefixes = [] {
            std::vector<std::string> out;
            if (const char *v = recomp_env("D3D9_SKIP")) {
                std::string s(v), cur;
                for (char ch : s + ",") {
                    if (ch == ',') {
                        if (!cur.empty())
                            out.push_back(cur);
                        cur.clear();
                    } else
                        cur.push_back((char)tolower((unsigned char)ch));
                }
            }
            return out;
        }();
        if (prefixes.empty())
            return false;
        char a[17], b[17];
        snprintf(a, sizeof a, "%016llx", (unsigned long long)vs_key);
        snprintf(b, sizeof b, "%016llx", (unsigned long long)ps_key);
        for (const std::string &p : prefixes)
            if (!strncmp(a, p.c_str(), p.size()) || !strncmp(b, p.c_str(), p.size()))
                return true;
        return false;
    }
    void skip(const char *why) {
        ++skips_[why];
    }
    void report() {
        if (!recomp_env("D3D9_STATS"))
            return;
        double now = CACurrentMediaTime();
        double fps = report_time_ > 0 ? (presents_ - report_frames_) / (now - report_time_) : 0.0;
        double dpf = presents_ > report_frames_
                         ? double(draws_ - report_draws_) / double(presents_ - report_frames_)
                         : 0.0;
        const uint64_t frames = presents_ > report_frames_ ? presents_ - report_frames_ : 1;
        const uint64_t gpu_us = gpu_us_.exchange(0);
        // The thread presenting is the game's: its CPU time is the frame's cost.
        const uint64_t cpu_ns = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
        const double cpu_ms = report_cpu_ns_ ? (cpu_ns - report_cpu_ns_) / 1e6 / frames : 0.0;
        report_cpu_ns_ = cpu_ns;
        // Instructions are what a frame costs whichever core ran it.
        double minstr = 0.0;
#if TARGET_OS_OSX
        rusage_info_v4 ru{};
        if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ru) == 0) {
            if (report_instructions_)
                minstr = (ru.ri_instructions - report_instructions_) / 1e6 / frames;
            report_instructions_ = ru.ri_instructions;
        }
#endif
        const double waited = gpu_wait_s_;
        gpu_wait_s_ = 0;
        report_time_ = now;
        report_frames_ = presents_;
        report_draws_ = draws_;
        fprintf(stderr,
                "d3d9 metal: frame %llu: %.1f fps, %.0f draws/frame, %llu draw calls, %llu "
                "encoded, %zu textures, %zu pipelines, %.1fM instructions/frame, game thread %.2f "
                "ms/frame, gpu %.2f ms/frame, waited %.2f ms/frame",
                (unsigned long long)presents_, fps, dpf, (unsigned long long)stat_calls_,
                (unsigned long long)draws_, textures_.size(), pipelines_.size(), minstr, cpu_ms,
                gpu_us / 1000.0 / frames, waited * 1000.0 / frames);
        for (auto &s : skips_)
            fprintf(stderr, ", %s %llu", s.first.c_str(), (unsigned long long)s.second);
        fprintf(stderr, "\n");
    }

    void clear(const HostD9Target &target, const int32_t vp[4], uint32_t count,
               const int32_t *rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        if (trace_)
            if (auto *e = trace_->event("clear_request", trace_context(),
                                        trace_texture(target.color[0].id))) {
                e->values[0] = target.depth.id;
                e->values[1] = flags;
                e->values[2] = color;
                memcpy(&e->values[3], &z, 4);
                e->values[4] = stencil;
                e->values[5] = count;
                memcpy(&e->values[6], vp, 16);
            }
        if (probing())
            fprintf(
                stderr,
                "probe clear: rt %u depth %u flags %x colour %08x z %g rects %u vp %d,%d %dx%d\n",
                target.color[0].id, target.depth.id, flags, color, z, count, vp[0], vp[1], vp[2],
                vp[3]);
        id<MTLTexture> c0 = texture_for(target.color[0]);
        id<MTLTexture> dz = texture_for(target.depth);
        if (!c0)
            return;
        const Tex &ct = textures_[target.color[0].id];
        int32_t lw = (int32_t)std::max<uint32_t>(ct.desc.width >> target.color[0].level, 1);
        int32_t lh = (int32_t)std::max<uint32_t>(ct.desc.height >> target.color[0].level, 1);
        bool whole = count == 0 && vp[0] <= 0 && vp[1] <= 0 && vp[2] >= lw && vp[3] >= lh;
        bool want_color = flags & 1, want_depth = (flags & 2) && dz,
             want_stencil = (flags & 4) && dz;
        float rgba[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                         (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f};
        if (whole) {
            // Folded into the pass's load actions: a new pass on this target.
            end_pass();
            begin_pass(target, want_color, rgba, want_depth, z, want_stencil, stencil);
            return;
        }
        if (!ensure_pass(target))
            return;
        // A partial clear is a quad per rectangle, written unconditionally.
        MTLPixelFormat cf[4];
        for (int i = 0; i < 4; ++i)
            cf[i] = pass_color_[i] ? pass_color_[i].pixelFormat : MTLPixelFormatInvalid;
        MTLPixelFormat df = pass_depth_ ? pass_depth_.pixelFormat : MTLPixelFormatInvalid;
        uint64_t key = mix(mix(0xc1ea5ull, (uint64_t)want_color), df);
        for (int i = 0; i < 4; ++i)
            key = mix(key, cf[i]);
        id<MTLRenderPipelineState> pso = utility_pipeline(key, @"clear_fs", cf, df, want_color);
        if (!pso)
            return;
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        uint64_t dkey = 0xc1ea5000ull | (uint64_t)want_depth | (uint64_t)want_stencil << 1;
        id<MTLDepthStencilState> ds = clear_depth_state(want_depth, want_stencil, dkey);
        [enc_ setDepthStencilState:ds];
        [enc_ setStencilReferenceValue:stencil & 0xff];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setTriangleFillMode:MTLTriangleFillModeFill];
        [enc_ setDepthBias:0 slopeScale:0 clamp:0];
        int32_t tw = lw, th = lh; // quads are placed in guest pixels; the viewport scales them
        NSUInteger vw = std::max<NSUInteger>(c0.width >> target.color[0].level, 1),
                   vh = std::max<NSUInteger>(c0.height >> target.color[0].level, 1);
        if (const float ls = live_scale(target.color[0].id, ct); !target.color[0].level && ls != ct.scale) {
            vw = std::max<NSUInteger>((NSUInteger)std::lround(lw * ls), 1); // the drawn part only
            vh = std::max<NSUInteger>((NSUInteger)std::lround(lh * ls), 1);
        }
        [enc_ setViewport:(MTLViewport){0, 0, (double)vw, (double)vh, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, vw, vh}];
        std::vector<int32_t> list;
        if (count && rects)
            list.assign(rects, rects + 4 * count);
        else
            list = {vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3]};
        for (size_t i = 0; i + 4 <= list.size(); i += 4) {
            int32_t x0 = std::max(list[i], std::max(vp[0], 0));
            int32_t y0 = std::max(list[i + 1], std::max(vp[1], 0));
            int32_t x1 = std::min(list[i + 2], std::min(vp[0] + vp[2], tw));
            int32_t y1 = std::min(list[i + 3], std::min(vp[1] + vp[3], th));
            if (x1 <= x0 || y1 <= y0)
                continue;
            QuadIn q{};
            q.rect[0] = 2.0f * x0 / tw - 1.0f;
            q.rect[1] = 1.0f - 2.0f * y0 / th;
            q.rect[2] = 2.0f * x1 / tw - 1.0f;
            q.rect[3] = 1.0f - 2.0f * y1 / th;
            q.z = z;
            memcpy(q.color, rgba, sizeof rgba);
            [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
            [enc_ setFragmentBytes:&q length:sizeof q atIndex:0];
            [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
            if (trace_) {
                if (want_color)
                    textures_[target.color[0].id].written = serial_;
                if (pass_depth_ && !pass_fitted_depth_ && (want_depth || want_stencil))
                    textures_[target.depth.id].written = serial_;
            }
        }
    }

    void stretch(HostD9Surface src, const int32_t sr[4], HostD9Surface dst, const int32_t dr[4],
                 uint32_t filter) override {
        if (trace_)
            if (auto *e =
                    trace_->event("stretch_request", trace_context(), trace_texture(dst.id))) {
                e->values[0] = src.id;
                e->values[1] = src.face;
                e->values[2] = src.level;
                e->values[3] = dst.face;
                e->values[4] = dst.level;
                e->values[5] = filter;
                memcpy(&e->values[6], sr, 16);
                memcpy(&e->values[10], dr, 16);
            }
        if (probing())
            fprintf(stderr,
                    "probe stretch: %u/%u/%u (%d,%d,%d,%d) -> %u/%u/%u (%d,%d,%d,%d) filter %u\n",
                    src.id, src.face, src.level, sr[0], sr[1], sr[2], sr[3], dst.id, dst.face,
                    dst.level, dr[0], dr[1], dr[2], dr[3], filter);
        id<MTLTexture> s = view_for(src);
        id<MTLTexture> t = texture_for(dst);
        if (!s || !t)
            return;
        if (s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) {
            stretch_depth(src, dst, t);
            return;
        }
        end_pass();
        uint32_t tw = std::max<uint32_t>((uint32_t)t.width >> dst.level, 1);
        uint32_t th = std::max<uint32_t>((uint32_t)t.height >> dst.level, 1);
        const Tex &st_ = textures_[src.id];
        const Tex &dt_ = textures_[dst.id];
        // Rectangles are in each surface's guest pixels.
        float ssx =
            (float)s.width /
            std::max<uint32_t>(1, st_.desc.kind == HOST_D9_TEX_CUBE
                                      ? st_.desc.width >> src.level
                                      : std::max<uint32_t>(st_.desc.width >> src.level, 1));
        float ssy =
            (float)s.height /
            std::max<uint32_t>(1, st_.desc.kind == HOST_D9_TEX_CUBE
                                      ? st_.desc.width >> src.level
                                      : std::max<uint32_t>(st_.desc.height >> src.level, 1));
        float dsx = (float)tw / std::max<uint32_t>(dt_.desc.width >> dst.level, 1);
        float dsy = (float)th / std::max<uint32_t>(dt_.desc.height >> dst.level, 1);
        // The back buffer drawn in a part of its texture (the dynamic share): that part.
        if (const float ls = live_scale(src.id, st_); !src.level && ls != st_.scale)
            ssx = ssy = ls;
        if (const float ls = live_scale(dst.id, dt_); !dst.level && ls != dt_.scale)
            dsx = dsy = ls;
        uint32_t sw = (uint32_t)s.width, sh = (uint32_t)s.height;
        HostD9Target target{};
        target.color[0] = dst;
        begin_pass(target, false, nullptr, false, 0, false, 0);
        MTLPixelFormat cf[4] = {t.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso = utility_pipeline(mix(0xb117ull, t.pixelFormat), @"copy_fs",
                                                          cf, MTLPixelFormatInvalid, true);
        if (!pso)
            return;
        es_ = EncoderState{}; // what follows is not a draw's state
        [enc_ setRenderPipelineState:pso];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
        QuadIn q{};
        q.rect[0] = 2.0f * dr[0] * dsx / tw - 1.0f;
        q.rect[1] = 1.0f - 2.0f * dr[1] * dsy / th;
        q.rect[2] = 2.0f * dr[2] * dsx / tw - 1.0f;
        q.rect[3] = 1.0f - 2.0f * dr[3] * dsy / th;
        q.uv[0] = sr[0] * ssx / sw;
        q.uv[1] = sr[1] * ssy / sh;
        q.uv[2] = sr[2] * ssx / sw;
        q.uv[3] = sr[3] * ssy / sh;
        [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
        textures_[src.id].used = serial_;
        [enc_ setFragmentTexture:s atIndex:0];
        uint32_t st[14] = {};
        st[1] = st[2] = st[3] = 3; // clamp
        st[5] = st[6] = filter >= 2 ? 2 : 1;
        [enc_ setFragmentSamplerState:sampler(st) atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (trace_)
            textures_[dst.id].written = serial_;
        end_pass();
    }

    // Test121: StretchRect from a depth surface into a colour target (R32F for the core mod's
    // motion blur and depth of field): the whole of the depth its last pass drew, stretched to
    // the whole target, nearest texel; a multisampled depth buffer gives its smallest sample. Rectangles are
    // ignored (the one use copies whole surfaces). Not a D3D9 behaviour; the guest only gets it
    // by asking for it, and a colour source keeps the ordinary path.
    void stretch_depth(HostD9Surface src, HostD9Surface dst, id<MTLTexture> t) {
        Tex *st = tex_ptr(src.id);
        {   // Ledger L1: before Test121 this request was dropped; say when anything first asks for it
            static unsigned long long n = 0;
            if (n++ == 0)
                fprintf(stderr, "d3d9 metal: depth StretchRect into colour target %u (%ux%u px; first request; Test121)\n",
                        dst.id, (unsigned)t.width, (unsigned)t.height);
        }
        if (!st || src.level || t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8)
            return;
        id<MTLTexture> d = st->drawn_depth ? st->drawn_depth : st->texture;
        if (!d || !(d.usage & MTLTextureUsageShaderRead))
            return;
        const bool ms = d.textureType == MTLTextureType2DMultisample;
        end_pass();
        uint32_t tw = std::max<uint32_t>((uint32_t)t.width >> dst.level, 1);
        uint32_t th = std::max<uint32_t>((uint32_t)t.height >> dst.level, 1);
        HostD9Target target{};
        target.color[0] = dst;
        begin_pass(target, false, nullptr, false, 0, false, 0);
        if (!enc_)
            return;
        MTLPixelFormat cf[4] = {t.pixelFormat, MTLPixelFormatInvalid, MTLPixelFormatInvalid,
                                MTLPixelFormatInvalid};
        id<MTLRenderPipelineState> pso =
            utility_pipeline(mix(ms ? 0xd3c1ull : 0xd3c0ull, t.pixelFormat),
                             ms ? @"depth_copy_ms_fs" : @"depth_copy_fs", cf, MTLPixelFormatInvalid, true);
        if (!pso) {
            end_pass();
            return;
        }
        es_ = EncoderState{};
        [enc_ setRenderPipelineState:pso];
        [enc_ setCullMode:MTLCullModeNone];
        [enc_ setViewport:(MTLViewport){0, 0, (double)tw, (double)th, 0, 1}];
        [enc_ setScissorRect:(MTLScissorRect){0, 0, tw, th}];
        QuadIn q{};
        q.rect[0] = -1.0f;
        q.rect[1] = 1.0f;
        q.rect[2] = 1.0f;
        q.rect[3] = -1.0f;
        q.uv[0] = 0.0f;
        q.uv[1] = 0.0f;
        q.uv[2] = 1.0f;
        q.uv[3] = 1.0f;
        [enc_ setVertexBytes:&q length:sizeof q atIndex:0];
        st->used = serial_;
        [enc_ setFragmentTexture:d atIndex:0];
        [enc_ drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        if (trace_)
            textures_[dst.id].written = serial_;
        end_pass();
    }

    // Shader work done synchronously inside a frame (MSL compile, pipeline creation). A cold
    // system Metal cache (the first run of a new bundle) makes this long; the report says where a
    // no-new-frame gap came from. Bounded: frames holding 50 ms or more, at most 200 lines.
    void note_shader_work(uint64_t ns, bool library) {
        shader_frame_ns_ += ns;
        shader_total_ns_ += ns;
        (library ? shader_frame_lib_ns_ : shader_frame_pso_ns_) += ns;
        if (ns > shader_frame_max_ns_) shader_frame_max_ns_ = ns;
        ++(library ? shader_frame_libs_ : shader_frame_psos_);
    }
    void report_shader_work() {
        if (shader_frame_ns_ >= 50000000ull && shader_reports_ < 200) {
            ++shader_reports_;
            fprintf(stderr,
                    "d3d9 metal: shader compilation held this frame %.1f ms (%u libraries %.1f ms, %u pipelines %.1f ms, "
                    "longest %.1f ms; %.0f ms in all so far; %u pre-compiled used, %u waits on the warm-up, %u pre-built pipelines used; at %.3f s monotonic)\n",
                    shader_frame_ns_ / 1e6, shader_frame_libs_, shader_frame_lib_ns_ / 1e6, shader_frame_psos_,
                    shader_frame_pso_ns_ / 1e6, shader_frame_max_ns_ / 1e6,
                    shader_total_ns_ / 1e6, warm_ ? warm_->hits() : 0u, warm_ ? warm_->coalesced() : 0u,
                    warm_ ? warm_->pipeline_hits() : 0u,
                    os_monotonic_ns() / 1e9);
        }
        shader_frame_ns_ = shader_frame_max_ns_ = shader_frame_lib_ns_ = shader_frame_pso_ns_ = 0;
        shader_frame_libs_ = shader_frame_psos_ = 0;
    }
    void present(uint32_t backbuffer, uint32_t w, uint32_t h) override {
        const uint32_t guest_w = w, guest_h = h;
        alloc_watch();
        report_shader_work();
        end_pass();
        auto it = textures_.find(backbuffer);
        if (it == textures_.end() || !it->second.texture) {
            flush();
            if (trace_) {
                if (burst_left_) {
                    fprintf(stderr, "d3d9 burst: aborted: missing presented target\n");
                    burst_left_ = 0;
                }
                trace_->event("present_missing", trace_context(), {backbuffer});
                if (capture_mode_)
                    capture_present(trace_->current_frame_number(), os_monotonic_ns(),
                                    os_wall_time_us(), trace_context());
                else
                    trace_->present(trace_context(), os_wall_time_us());
            }
            return;
        }
        Tex &t = it->second;
        base_rows_ = t.desc.height;
        base_cols_ = t.desc.width;
        w = (uint32_t)t.texture.width;
        h = (uint32_t)t.texture.height;
        if (const float ls = live_scale(backbuffer, t); ls != t.scale) { // the drawn part
            w = std::max<uint32_t>(1, (uint32_t)std::lround(t.desc.width * ls));
            h = std::max<uint32_t>(1, (uint32_t)std::lround(t.desc.height * ls));
        }
        keep_presented(t, w, h);
        {
            // What a frame is, each time it changes: the game's size, the texture's part
            // drawn, the drawable it is fitted to (a foldable folding shows here).
            static uint32_t seen[5];
            int dw = 0, dh = 0;
            host_present_drawable(&dw, &dh);
            const uint32_t now[5] = {guest_w, guest_h, w, h, (uint32_t)dw << 16 | ((uint32_t)dh & 0xffff)};
            if (memcmp(now, seen, sizeof now)) {
                memcpy(seen, now, sizeof now);
                fprintf(stderr, "d3d9 metal: presenting %ux%u (drawn %ux%u) to a %dx%d drawable\n", guest_w,
                        guest_h, w, h, dw, dh);
            }
        }
        flush();
        uint64_t trace_frame = trace_ ? trace_->current_frame_number() : 0;
        uint64_t present_wall_us = os_wall_time_us();
        CaptureMarker capture_marker;
        if (trace_) {
            if (auto *e = trace_->event("present", trace_context(), trace_texture(backbuffer))) {
                e->values[0] = guest_w;
                e->values[1] = guest_h;
                e->values[2] = w;
                e->values[3] = h;
            }
            if (capture_mode_)
                capture_marker = capture_present(trace_frame, os_monotonic_ns(), present_wall_us,
                                                 trace_context());
            else
                trace_->present(trace_context(), present_wall_us);
        }
        if (burst_left_ && presents_ + 1 >= burst_start_) {
            // Reuse the existing exact backbuffer dump and metadata ring. I/O and
            // GPU waits perturb timing; this is evidence capture, not benchmarking.
            const uint64_t bytes = (uint64_t)w * h * 3;
            if (t.info.pixel != MTLPixelFormatBGRA8Unorm || !w || !h ||
                (uint64_t)w * h > 1920u * 1080u ||
                burst_pixel_bytes_ + trace_bytes_ - burst_trace_start_ + bytes > (2ull << 30)) {
                fprintf(stderr, "d3d9 burst: aborted: format/resolution/2GiB budget limit\n");
                trace_flush("burst-limit");
                burst_left_ = 0;
            } else {
                dump(t, w, h);
                burst_pixel_bytes_ += bytes;
                --burst_left_;
                ++burst_captured_;
                fprintf(stderr,
                        "d3d9 burst: present=%llu trace_frame=%llu wall_us=%llu remaining=%u\n",
                        (unsigned long long)(presents_ + 1), (unsigned long long)trace_frame,
                        (unsigned long long)present_wall_us, burst_left_);
                // Flush every ten completed frames, before the ring can overwrite
                // any burst frame. The final partial group is flushed as well.
                if (burst_captured_ % 10 == 0 || !burst_left_)
                    if (!trace_flush("burst"))
                        burst_left_ = 0;
                if (!burst_left_)
                    fprintf(stderr, "d3d9 burst: finished %u frames\n", burst_captured_);
            }
        }
        if (probing())
            probe_dump();
        uint32_t every = host_dump_every();
        if (every && ((presents_ + 1) % every) == 0)
            dump(t, w, h);
        if (!t.imported)
            t.imported = device_->import_texture(t.texture);
        g_host_hdr_scene_mask.store(hdr_mask_frame_, std::memory_order_relaxed); // Test86
        hdr_mask_frame_ = false;
        if (t.info.pixel == MTLPixelFormatBGRA8Unorm) {
            // MetalFX: a frame smaller than the window reaches the presenter already
            // upscaled to it, edge-aware, instead of stretched bilinearly.
            gpu::Texture staged = t.imported;
            uint32_t staged_w = w, staged_h = h;
            if (metalfx_upscale(t.texture, w, h, guest_w, guest_h)) {
                staged = fx_imported_;
                staged_w = fx_out_w_;
                staged_h = fx_out_h_;
                if (every && ((presents_ + 1) % every) == 0)
                    metalfx_dump();
            }
            gpu::CommandBuffer cb = device_->begin();
            if (host_present_stage_texture(staged, (int)staged_w, (int)staged_h, (int)guest_w,
                                           (int)guest_h, cb,
                                           capture_marker.enabled ? &capture_marker : nullptr))
                host_present_track_command(cb);
            device_->commit(cb);
        }
        ++presents_;
        g_host_game_gpu_frames.fetch_add(1, std::memory_order_relaxed);
        if (presents_ % 300 == 1)
            report();
        split_restore(); // the next frame's 3D draws small again
        update_dynamic_scale();
        follow_drawable();
        last_presented_ = backbuffer;
        last_w_ = w;
        last_h_ = h;
    }

    // The frame as presented, copied on the GPU when it was presented: the
    // back buffer itself already holds the start of the next frame.
    void keep_presented(Tex &t, uint32_t w, uint32_t h) {
        if (t.info.pixel != MTLPixelFormatBGRA8Unorm || !t.texture)
            return;
        std::lock_guard<std::mutex> lock(kept_mutex_);
        if (!kept_ || kept_.width != w || kept_.height != h) {
            MTLTextureDescriptor *td =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                   width:w
                                                                  height:h
                                                               mipmapped:NO];
            td.storageMode = MTLStorageModeShared;
            kept_ = [mtl_ newTextureWithDescriptor:td];
            note_alloc(AS_KEPT, kept_, w, h, 1, 0);
        }
        begin_frame();
        id<MTLBlitCommandEncoder> blit = [cmd_ blitCommandEncoder];
        [blit copyFromTexture:t.texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(w, h, 1)
                    toTexture:kept_
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        kept_cmd_ = cmd_;
        kept_w_ = w;
        kept_h_ = h;
    }

    // The scale that fills the drawable's height with the back buffer (at
    // least RECOMP_D3D9_MIN_ROWS rows, for supersampling), or 0 when it is
    // fixed by RECOMP_D3D9_SCALE or unknown. On a touch device with MetalFX,
    // the share of the drawable's rows the player's render quality asks for
    // (mods_render_fraction): under 1, upscaled by MetalFX at Present. A Mac
    // takes that share only when the player picked one under the native
    // size (render quality 2..4); automatic and native supersample as before.
    float wanted_scale() const {
        if (recomp_env("D3D9_SCALE") || !base_rows_)
            return 0.0f;
        int dw = 0, dh = 0;
        if (!host_present_drawable(&dw, &dh) || dh <= 0)
            return 0.0f;
#if TARGET_OS_IPHONE
        const bool share = true;
#else
        const bool share = mods_render_quality() > 1;
#endif
        if (share && metalfx_available()) {
            float s = std::round(32.0f * mods_render_fraction(dh) * dh / base_rows_) / 32.0f;
            if (const char *max = recomp_env("D3D9_MAX_SCALE"); max && *max)
                s = std::min(s, (float)atof(max));
            // Just under 1 MetalFX does nothing (metalfx_upscale) and the
            // presenter stretches the frame pixel by pixel: the screen's own.
            if (s > 0.9f)
                s = 1.0f;
            return std::clamp(s, 1.0f / 3.0f, 1.0f);
        }
        int rows = dh;
        if (const char *min = recomp_env("D3D9_MIN_ROWS"))
            rows = std::max(rows, atoi(min));
        float s = std::floor(8.0f * rows / base_rows_ + 0.5f) / 8.0f;
        // Energy: an iPad's panel is dense enough that filling it costs far more GPU
        // than it shows, so it stops at 1.25x; saving mode renders at the game's own
        // size. RECOMP_D3D9_MAX_SCALE overrides both.
#if TARGET_OS_IPHONE
        // With MetalFX the game's own size is upscaled to the panel; without it 1.25x.
        float ceiling = metalfx_available() ? 1.0f : 1.25f;
#else
        float ceiling = 8.0f;
#endif
        if (mods_energy_saving())
            ceiling = 1.0f;
        if (const char *max = recomp_env("D3D9_MAX_SCALE"); max && *max)
            ceiling = std::max(1.0f, (float)atof(max));
        return std::max(1.0f, std::min(ceiling, s));
    }
    // RECOMP_METALFX: 0 off, 1 always (even at a small enlargement), unset automatic -
    // on whenever the window is at least 1.1x the frame. Spatial only: the temporal
    // scaler needs motion vectors a Direct3D 9 game never makes.
    int metalfx_mode() const {
        static const int mode = [] {
            const char *v = recomp_env("METALFX");
            return v && *v ? (v[0] == '0' ? 0 : 1) : -1;
        }();
        return mode;
    }
    bool metalfx_available() const {
        if (metalfx_mode() == 0 || !RECOMP_HAVE_METALFX)
            return false;
#if RECOMP_HAVE_METALFX
        if (fx_supported_ < 0)
            fx_supported_ = [MTLFXSpatialScalerDescriptor supportsDevice:mtl_] ? 1 : 0;
#endif
        return fx_supported_ == 1;
    }
    // The frame is w x h at the top left of `src` (all of it, or the dynamic share's part);
    // the output is the game's own gw x gh fitted to the drawable, at most 3x the frame,
    // so a share that moves changes the scaler's content size only.
    bool metalfx_upscale(id<MTLTexture> src, uint32_t w, uint32_t h, uint32_t gw, uint32_t gh) {
        if (!src || !w || !h || !gw || !gh || !metalfx_available())
            return false;
#if RECOMP_HAVE_METALFX
        int dw = 0, dh = 0;
        if (!host_present_drawable(&dw, &dh) || dw <= 0 || dh <= 0)
            return false;
        double k = std::min((double)dw / gw, (double)dh / gh); // output pixels per guest pixel
        const double per = k * gw / w;                          // per frame pixel
        // A part of the texture is always upscaled: the presenter would stretch it pixel
        // by pixel, and make its frame anew at each size the share eases through.
        const bool part = w < src.width || h < src.height;
        if (per < (metalfx_mode() == 1 || part ? 1.0 : 1.1))
            return false;
        if (per > 3.0)
            k = 3.0 * w / gw;
        const uint32_t ow = (uint32_t)std::lround(gw * k), oh = (uint32_t)std::lround(gh * k);
        const uint32_t iw = (uint32_t)src.width, ih = (uint32_t)src.height;
        const ScalerKey want{iw, ih, ow, oh, (uint32_t)src.pixelFormat};
        const ScalerKey have{fx_in_w_, fx_in_h_, fx_out_w_, fx_out_h_, (uint32_t)fx_format_};
        if (!fx_ || have != want) {
            // A new configuration is made in the background (fx_build_async), between frames.
            // Until it is ready the current scaler goes on when it still takes this input (its
            // output, at the size it was made for, is fitted to the window by the presenter as
            // any frame is); otherwise the frame takes the plain path, stretched.
            const FxSlot::Request r = fx_slot_->request(want);
            if (r.start)
                fx_build_async(mtl_, fx_slot_, want, "present");
            if (r.ready) {
                fx_asked_ = false;
                if (!r.scaler) {
                    fx_supported_ = 0;
                    fprintf(stderr, "d3d9 metal: MetalFX spatial scaler unavailable; stretching instead\n");
                    return false;
                }
                fx_ = nil;
                fx_out_ = nil;
                if (fx_imported_) {
                    device_->destroy(fx_imported_);
                    fx_imported_ = {};
                }
                fx_ = r.scaler;
                MTLTextureDescriptor *od =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat
                                                                       width:ow
                                                                      height:oh
                                                                   mipmapped:NO];
                od.storageMode = MTLStorageModePrivate;
                od.usage = fx_.outputTextureUsage | MTLTextureUsageShaderRead;
                fx_out_ = [mtl_ newTextureWithDescriptor:od];
                fx_imported_ = fx_out_ ? device_->import_texture(fx_out_) : gpu::Texture{};
                fx_in_w_ = iw;
                fx_in_h_ = ih;
                fx_out_w_ = ow;
                fx_out_h_ = oh;
                fx_format_ = src.pixelFormat;
                fprintf(stderr, "d3d9 metal: MetalFX spatial upscale %ux%u -> %ux%u\n", iw, ih, ow, oh);
            } else {
                fx_asked_ = true;
                if (!fx_ || fx_in_w_ != iw || fx_in_h_ != ih || fx_format_ != src.pixelFormat)
                    return false;
            }
        } else if (fx_asked_) {
            fx_asked_ = false;
            fx_slot_->settle();   // back at the current scaler's size: a build still running is not wanted
        }
        if (!fx_out_ || !fx_imported_)
            return false;
        // Never encoded with a scaler of another size than the textures it is given.
        if (fx_.inputWidth != iw || fx_.inputHeight != ih || fx_.outputWidth != fx_out_.width ||
            fx_.outputHeight != fx_out_.height || fx_out_.width != fx_out_w_ || fx_out_.height != fx_out_h_)
            return false;
        if ((src.usage & fx_.colorTextureUsage) != fx_.colorTextureUsage) {
            static bool told = false;
            if (!told)
                fprintf(stderr, "d3d9 metal: MetalFX input lacks usage %#lx; stretching instead\n",
                        (unsigned long)fx_.colorTextureUsage);
            told = true;
            return false;
        }
        fx_.colorTexture = src;
        fx_.outputTexture = fx_out_;
        fx_.inputContentWidth = w;
        fx_.inputContentHeight = h;
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [fx_ encodeToCommandBuffer:cb];
        [cb commit];
        return true;
#else
        return false;
#endif
    }

    // The upscaled frame beside present_NNNNN.ppm, for comparing the two (dump frames only).
    void metalfx_dump() {
        const uint32_t w = fx_out_w_, h = fx_out_h_;
        id<MTLBuffer> buf = [mtl_ newBufferWithLength:(NSUInteger)w * h * 4
                                              options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:fx_out_
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:(NSUInteger)w * h * 4];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const uint8_t *bgra = (const uint8_t *)buf.contents;
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/metalfx_%05u.ppm", host_dump_dir(), (unsigned)(presents_ + 1));
        host_write_ppm(path, rgb.data(), (int)w, (int)h);
    }

    // The pixels per guest pixel target `id` is drawn at in this frame: its texture's scale,
    // or for the back buffer drawn in a part of its texture (the dynamic share) that part's;
    // the back buffer at its full size (a split frame's interface) as its texture is.
    float live_scale(uint32_t id, const Tex &t) const {
        if (id && id == last_presented_ && !split_bb_ && dyn_ < 1.0f && t.scale <= 1.0f)
            return t.scale * dyn_;
        return t.scale;
    }

    // The automatic render quality (mods_render_quality 0) draws the 3D smaller as the
    // device heats - by host_thermal_level, fair 0.9, serious 0.75, critical 0.6 of what the
    // quality itself asks for, never under a third of the game's size - in a part of the
    // back buffer's texture, which MetalFX takes as its content (inputContentWidth/Height):
    // nothing is made again as the share moves. It eases a step a frame (down 0.02, up
    // 0.01), between frames. Off with a quality the player chose, without MetalFX, after a
    // pass drew elsewhere with the back buffer's depth (dyn_unsafe_), or with
    // RECOMP_D3D9_DYNAMIC=0.
    void update_dynamic_scale() {
        static const bool allowed = [] {
            const char *v = recomp_env("D3D9_DYNAMIC");
            return !(v && v[0] == '0');
        }();
        const double now = CACurrentMediaTime();
        if (now - dyn_checked_ >= 1.0) {
            dyn_checked_ = now;
            const int level = std::clamp(host_thermal_level(), 0, 3);
            if (level != dyn_level_)
                fprintf(stderr, "d3d9 metal: thermal level %d\n", level);
            dyn_level_ = level;
        }
        float target = 1.0f;
#if TARGET_OS_IPHONE
        if (allowed && !dyn_unsafe_ && mods_render_quality() == 0 && metalfx_available() && scale_ <= 1.0f) {
            static const float by_level[4] = {1.0f, 0.9f, 0.75f, 0.6f};
            target = std::max(by_level[dyn_level_], (1.0f / 3.0f) / std::max(scale_, 1.0f / 3.0f));
        }
#else
        (void)allowed;
#endif
        const float was = dyn_;
        dyn_ = target < dyn_ ? std::max(target, dyn_ - 0.02f) : std::min(target, dyn_ + 0.01f);
        if (dyn_ == target && was != target)
            fprintf(stderr, "d3d9 metal: dynamic render scale %.2f of %.2f\n", dyn_, scale_);
    }

    // scene_end (host_d9_scene_end): the 3D of this frame is in the back buffer's own
    // texture at the render quality's scale; MetalFX upscales it into a texture of the
    // game's size, which the back buffer then is until Present (split_restore), with a
    // cleared depth buffer of that size beside it. Nothing when the back buffer is not
    // drawn smaller, the frame was split already (a battle's second 3D view draws at full
    // size), MetalFX is missing, or RECOMP_D3D9_SPLIT=0.
    void scene_end() override {
#if RECOMP_HAVE_METALFX
        static const bool allowed = [] {
            const char *v = recomp_env("D3D9_SPLIT");
            return !(v && v[0] == '0');
        }();
        if (!allowed || split_bb_ || !last_presented_ || !metalfx_available())
            return;
        auto it = textures_.find(last_presented_);
        if (it == textures_.end() || !it->second.texture)
            return;
        Tex &t = it->second;
        const float ls = live_scale(last_presented_, t);
        if (ls >= 1.0f || t.info.pixel != MTLPixelFormatBGRA8Unorm || t.desc.kind != HOST_D9_TEX_2D)
            return;
        const uint32_t w = t.desc.width, h = t.desc.height;
        const uint32_t iw = (uint32_t)t.texture.width, ih = (uint32_t)t.texture.height;
        // The drawn part: all of the texture, or the dynamic share's (update_dynamic_scale).
        const uint32_t cw = ls == t.scale ? iw : std::max<uint32_t>(1, (uint32_t)std::lround(w * ls));
        const uint32_t ch = ls == t.scale ? ih : std::max<uint32_t>(1, (uint32_t)std::lround(h * ls));
        if (!w || !h || !iw || !ih || cw > iw || ch > ih)
            return;
        // Made again when the game's size or the 3D's (the render quality) changes - in the
        // background (fx_build_async). Until the new scaler is ready the frame is not split: its
        // interface is drawn over the smaller 3D, and Present upscales the whole frame, as it
        // does whenever this pass is off. The old scaler never takes textures of another size.
        const ScalerKey want{iw, ih, w, h, (uint32_t)t.texture.pixelFormat};
        if (!split_hi_ || !split_fx_ || split_hi_.width != w || split_hi_.height != h || split_fx_.inputWidth != iw ||
            split_fx_.inputHeight != ih || split_fx_.colorTextureFormat != t.texture.pixelFormat) {
            if (split_failed_ && *split_failed_ == want)
                return;
            const FxSlot::Request r = split_slot_->request(want);
            if (r.start)
                fx_build_async(mtl_, split_slot_, want, "split");
            if (!r.ready) {
                split_asked_ = true;
                return;
            }
            split_asked_ = false;
            if (!r.scaler) {
                split_failed_ = want;
                return;
            }
            split_release();
            split_fx_ = r.scaler;
            MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.texture.pixelFormat
                                                                                         width:w
                                                                                        height:h
                                                                                     mipmapped:NO];
            od.storageMode = MTLStorageModePrivate;
            od.usage = split_fx_.outputTextureUsage | MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
            split_hi_ = [mtl_ newTextureWithDescriptor:od];
            note_alloc(AS_TEXTURE, split_hi_, w, h, 1, 0);
            split_hi_imported_ = split_hi_ ? device_->import_texture(split_hi_) : gpu::Texture{};
            fprintf(stderr, "d3d9 metal: split frame: 3D in %ux%u, upscaled to %ux%u for the interface\n", iw, ih,
                    w, h);
        } else if (split_asked_) {
            split_asked_ = false;
            split_slot_->settle();
        }
        if (!split_hi_ || !split_hi_imported_ ||
            (t.texture.usage & split_fx_.colorTextureUsage) != split_fx_.colorTextureUsage)
            return;
        end_pass();
        begin_frame();
        split_fx_.colorTexture = t.texture;
        split_fx_.outputTexture = split_hi_;
        split_fx_.inputContentWidth = cw;
        split_fx_.inputContentHeight = ch;
        [split_fx_ encodeToCommandBuffer:cmd_];
        split_lo_bb_ = {t.texture, t.msaa, t.imported, t.scale};
        t.texture = split_hi_;
        t.msaa = nil;
        t.imported = split_hi_imported_;
        t.scale = 1.0f;
        split_bb_ = last_presented_;
        // The depth buffer the 3D was drawn with, when it is the back buffer's size: a
        // cleared one of the full size beside the interface (its own 3D scene tests depth).
        auto dt = split_depth_seen_ ? textures_.find(split_depth_seen_) : textures_.end();
        if (dt != textures_.end() && dt->second.texture && dt->second.depth && dt->second.desc.width == w &&
            dt->second.desc.height == h && dt->second.scale == split_lo_bb_.scale) {
            if (!split_hi_depth_ || split_hi_depth_.width != w || split_hi_depth_.height != h) {
                MTLTextureDescriptor *dd =
                    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                                       width:w
                                                                      height:h
                                                                   mipmapped:NO];
                dd.storageMode = MTLStorageModePrivate;
                dd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
                split_hi_depth_ = [mtl_ newTextureWithDescriptor:dd];
                note_alloc(AS_TEXTURE, split_hi_depth_, w, h, 1, 0);
            }
            if (split_hi_depth_) {
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.depthAttachment.texture = split_hi_depth_;
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionStore;
                rp.stencilAttachment.texture = split_hi_depth_;
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = 0;
                rp.stencilAttachment.storeAction = MTLStoreActionStore;
                rp.renderTargetWidth = w;
                rp.renderTargetHeight = h;
                [[cmd_ renderCommandEncoderWithDescriptor:rp] endEncoding];
                Tex &d = dt->second;
                split_lo_depth_ = {d.texture, d.msaa, d.imported, d.scale};
                d.texture = split_hi_depth_;
                d.msaa = nil;
                d.scale = 1.0f;
                split_depth_ = split_depth_seen_;
            }
        }
#endif
    }
    // The back buffer and its depth buffer back at their own, smaller textures.
    void split_restore() {
        if (split_bb_) {
            auto it = textures_.find(split_bb_);
            if (it != textures_.end()) {
                Tex &t = it->second;
                t.texture = split_lo_bb_.texture;
                t.msaa = split_lo_bb_.msaa;
                t.imported = split_lo_bb_.imported;
                t.scale = split_lo_bb_.scale;
            }
            split_bb_ = 0;
            split_lo_bb_ = {};
        }
        if (split_depth_) {
            auto it = textures_.find(split_depth_);
            if (it != textures_.end()) {
                Tex &d = it->second;
                d.texture = split_lo_depth_.texture;
                d.msaa = split_lo_depth_.msaa;
                d.scale = split_lo_depth_.scale;
            }
            split_depth_ = 0;
            split_lo_depth_ = {};
        }
    }
    // A texture redefined or dropped: if it is swapped now, its own textures go back
    // first (define and drop release what the Tex holds); the full-size ones are made
    // again for the next size.
    void split_forget(uint32_t id) {
        if (id && (id == split_bb_ || id == split_depth_))
            split_restore();
        if (id && id == last_presented_)
            split_release();
    }
    void split_release() {
        split_restore();
        if (split_hi_imported_) {
            device_->destroy(split_hi_imported_);
            split_hi_imported_ = {};
        }
        split_hi_ = nil;
        split_hi_depth_ = nil;
#if RECOMP_HAVE_METALFX
        split_fx_ = nil;
#endif
    }

    // Follow the drawable: once a different scale has been wanted for a
    // quarter of a second's worth of frames, every scaled target is made
    // again at the new size, between frames. Their contents are redrawn by
    // the game; a target it only draws now and then shows black once.
    void follow_drawable() {
        const float want = wanted_scale();
        if (want <= 0 || std::fabs(want - scale_) < 0.01f) {
            rescale_frames_ = 0;
            return;
        }
        if (++rescale_frames_ < 15)
            return;
        rescale_frames_ = 0;
        end_pass();
        flush();
        wait();
        scale_ = want;
        fitted_depth_.clear();
        g_fitted_depth_entries = 0;
        std::vector<HostD9TextureDesc> again;
        for (auto &kv : textures_)
            if (kv.second.scale != 1.0f ||
                ((kv.second.desc.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) &&
                 kv.second.desc.kind == HOST_D9_TEX_2D))
                again.push_back(kv.second.desc);
        for (const HostD9TextureDesc &d : again)
            define(d);
        fprintf(stderr, "d3d9: render targets now at %.2fx the game's size\n", scale_);
    }

    bool read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) override {
        id<MTLTexture> kept;
        id<MTLCommandBuffer> cmd;
        {
            std::lock_guard<std::mutex> lock(kept_mutex_);
            kept = kept_;
            cmd = kept_cmd_;
            *w = kept_w_;
            *h = kept_h_;
        }
        if (!kept)
            return false;
        uint32_t fw = *w, fh = *h;
        if (!rgb || cap < fw * fh * 3)
            return false;
        [cmd waitUntilCompleted];
        std::vector<uint8_t> bgra((size_t)fw * fh * 4);
        std::lock_guard<std::mutex> lock(kept_mutex_);
        [kept getBytes:bgra.data()
            bytesPerRow:fw * 4
             fromRegion:MTLRegionMake2D(0, 0, fw, fh)
            mipmapLevel:0];
        size_t n = (size_t)fw * fh;
        for (size_t i = 0; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        return true;
    }

  private:
    // ---- passes ---------------------------------------------------------
    // Textures by id without hashing: ids are small, and map nodes stay put.
    void index_texture(uint32_t id, Tex *t) {
        if (id >= (1u << 20))
            return;
        if (tex_index_.size() <= id)
            tex_index_.resize(id + 1024, nullptr);
        tex_index_[id] = t;
    }
    Tex *tex_ptr(uint32_t id) {
        if (id < tex_index_.size())
            return tex_index_[id];
        if (!id)
            return nullptr;
        auto it = textures_.find(id);
        return it != textures_.end() ? &it->second : nullptr;
    }

    id<MTLTexture> texture_for(const HostD9Surface &s) {
        if (!s.id)
            return nil;
        Tex *t = tex_ptr(s.id);
        return t ? t->texture : nil;
    }
    // A 2D view of one face and level, for sampling a surface as a texture.
    id<MTLTexture> view_for(const HostD9Surface &s) {
        id<MTLTexture> t = texture_for(s);
        if (!t || t.textureType == MTLTextureType3D)
            return nil;
        if (t.textureType == MTLTextureType2D && s.level == 0 && t.mipmapLevelCount == 1)
            return t;
        return [t newTextureViewWithPixelFormat:t.pixelFormat
                                    textureType:MTLTextureType2D
                                         levels:NSMakeRange(s.level, 1)
                                         slices:NSMakeRange(s.face, 1)];
    }

    bool same_target(const HostD9Target &t) const {
        return enc_ && memcmp(&t, &pass_target_, sizeof t) == 0;
    }
    bool ensure_pass(const HostD9Target &t) {
        if (same_target(t))
            return true;
        end_pass();
        return begin_pass(t, false, nullptr, false, 0, false, 0);
    }
    bool begin_pass(const HostD9Target &t, bool clear_color, const float *rgba, bool clear_depth,
                    float z, bool clear_stencil, uint32_t stencil) {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        bool any = false;
        pass_samples_ = 0;
        pass_fitted_depth_ = false;
        for (int i = 0; i < 4; ++i) {
            pass_color_[i] = nil;
            Tex *ct = t.color[i].id ? tex_ptr(t.color[i].id) : nullptr;
            id<MTLTexture> tex = ct ? ct->texture : nil;
            if (!tex || ct->depth || ct->desc.kind == HOST_D9_TEX_VOLUME)
                continue;
            // Multisampled only as a whole surface; every attachment of a
            // pass has one sample count, the first one's.
            const bool ms = ct->msaa && t.color[i].level == 0 && t.color[i].face == 0;
            const uint32_t samples = ms ? ct->samples : 1;
            if (pass_samples_ && samples != pass_samples_)
                continue;
            pass_samples_ = samples;
            pass_color_[i] = tex;
            ct->used = serial_ + (cmd_ ? 0 : 1);
            if (ms) {
                rp.colorAttachments[i].texture = ct->msaa;
                rp.colorAttachments[i].resolveTexture = tex;
                rp.colorAttachments[i].storeAction = MTLStoreActionStoreAndMultisampleResolve;
            } else {
                rp.colorAttachments[i].texture = tex;
                rp.colorAttachments[i].level = t.color[i].level;
                rp.colorAttachments[i].slice = t.color[i].face;
                rp.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
            if (i == 0 && clear_color) {
                rp.colorAttachments[i].loadAction = MTLLoadActionClear;
                rp.colorAttachments[i].clearColor =
                    MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
            } else {
                rp.colorAttachments[i].loadAction = MTLLoadActionLoad;
            }
            any = true;
        }
        if (!pass_samples_)
            pass_samples_ = 1;
        pass_color_id_ = pass_color_[0] ? t.color[0].id : 0; // Test86
        pass_depth_ = nil;
        Tex *dt = t.depth.id ? tex_ptr(t.depth.id) : nullptr;
        id<MTLTexture> dz = dt ? dt->texture : nil;
        if (dz && pass_samples_ > 1)
            dz = (dt->msaa && dt->samples == pass_samples_) ? dt->msaa : nil;
        bool depth_unmatched = false;
        if (!dz && dt && dt->texture && pass_samples_ > 1 && pass_color_[0]) {
            pass_fitted_depth_ = true;
            // A single-sampled depth buffer under a multisampled target: the
            // pass gets a depth buffer of its own that matches.
            dz = depth_for_size(dt->texture, pass_color_[0], 0, pass_samples_);
            depth_unmatched = true;
        }
        if (dz && dz.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && pass_color_[0] &&
            dz.width >= pass_color_[0].width >> t.color[0].level &&
            dz.height >= pass_color_[0].height >> t.color[0].level) {
            pass_depth_ = dz;
            if (dt)
                dt->used = serial_ + (cmd_ ? 0 : 1);
            (void)depth_unmatched;
            rp.depthAttachment.texture = dz;
            rp.depthAttachment.loadAction = clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.depthAttachment.clearDepth = z;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.stencilAttachment.texture = dz;
            rp.stencilAttachment.loadAction =
                clear_stencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = stencil & 0xff;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        if (!any)
            return false;
        // Metal requires every attachment to be the same size.
        if (pass_depth_ &&
            (pass_depth_.width !=
                 std::max<NSUInteger>(pass_color_[0].width >> t.color[0].level, 1) ||
             pass_depth_.height !=
                 std::max<NSUInteger>(pass_color_[0].height >> t.color[0].level, 1))) {
            id<MTLTexture> fit =
                depth_for_size(pass_depth_, pass_color_[0], t.color[0].level, pass_samples_);
            pass_depth_ = fit;
            pass_fitted_depth_ = true;
            rp.depthAttachment.texture = fit;
            rp.stencilAttachment.texture = fit;
        }
        if (pass_depth_ && dt)
            dt->drawn_depth = pass_depth_; // Test121
        begin_frame();
        pass_vis_ = false;
        pass_slot_ = -1;
        if (query_) {
            rp.visibilityResultBuffer = vis_buffer();
            pass_vis_ = true;
        }
        enc_ = [cmd_ renderCommandEncoderWithDescriptor:rp];
        if (trace_) {
            ++pass_serial_;
            if (enc_) {
                if (clear_color && pass_color_[0])
                    textures_[t.color[0].id].written = serial_;
                if ((clear_depth || clear_stencil) && pass_depth_ && !pass_fitted_depth_)
                    textures_[t.depth.id].written = serial_;
            }
            if (auto *e = trace_->event("pass", trace_context())) {
                for (int i = 0; i < 4; ++i)
                    e->values[i] = t.color[i].id;
                e->values[4] = t.depth.id;
                e->values[5] = pass_fitted_depth_;
                e->values[6] = pass_samples_;
                e->values[7] = clear_color;
                e->values[8] = clear_depth;
                e->values[9] = clear_stencil;
                e->values[10] = enc_ != nil;
            }
        }
        es_ = EncoderState{};
        [enc_ setFrontFacingWinding:MTLWindingClockwise];
        pass_target_ = t;
        pass_scale_ = live_scale(t.color[0].id, textures_[t.color[0].id]);
        if (t.color[0].id && t.color[0].id == last_presented_ && t.depth.id)
            split_depth_seen_ = t.depth.id; // what scene_end swaps beside the back buffer
        else if (t.depth.id && t.depth.id == split_depth_seen_ && !dyn_unsafe_) {
            // The back buffer's depth under another target: its drawn part would not be
            // where that target's is, so the dynamic share stays off from now on.
            dyn_unsafe_ = true;
            fprintf(stderr, "d3d9 metal: the back buffer's depth drawn with target %u: no dynamic render scale\n",
                    t.color[0].id);
        }
        return enc_ != nil;
    }
    void end_pass() {
        if (enc_) {
            [enc_ endEncoding];
            enc_ = nil;
        }
        pass_vis_ = false;
        pass_slot_ = -1;
        memset(&pass_target_, 0, sizeof pass_target_);
    }
    // Direct3D allows a depth buffer larger than the colour target; Metal does
    // not. A draw into a smaller target gets a depth buffer of its own size.
    // Test155 (test sessions): at rain onset the game's footprint grew ~13 GB/s (about
    // 265 MB a frame at ~50 fps, 3840x2160 with 4x MSAA) until macOS killed it; all four draw calls were guarded and
    // refused nothing. Every Metal resource this backend creates is counted by call site; one of 64 MB or more is
    // logged, and when the device's allocated size grows by more than 1 GB within two seconds the log gets the
    // growth by site with each site's last descriptor and the pass it was made for. A counter read and an add per
    // allocation; one property read per presented frame.
    enum AllocSite { AS_TEXTURE, AS_MSAA, AS_READBACK, AS_BUFFER, AS_BUFFER_SPARE, AS_KEPT, AS_FITTED_DEPTH,
                     AS_TRANSIENT, AS_COUNT };
    struct AllocStat {
        uint64_t bytes = 0, count = 0, seen_bytes = 0, seen_count = 0;
        char last[160] = {};
    };
    static const char *alloc_site_name(int s) {
        static const char *n[] = {"texture",       "msaa colour", "readback buffer", "buffer",
                                  "buffer spare",  "kept frame",  "fitted depth",    "transient upload"};
        return s >= 0 && s < AS_COUNT ? n[s] : "?";
    }
    void note_alloc(AllocSite site, id<MTLResource> r, uint64_t w, uint64_t h, uint64_t samples, uint64_t format) {
        if (!r)
            return;
        const uint64_t n = (uint64_t)r.allocatedSize;
        AllocStat &a = alloc_[site];
        a.bytes += n;
        ++a.count;
        snprintf(a.last, sizeof a.last,
                 "%llux%llu format %llu x%llu samples, %llu MB, for pass target %u (frame %llu)",
                 (unsigned long long)w, (unsigned long long)h, (unsigned long long)format,
                 (unsigned long long)samples, (unsigned long long)(n >> 20), pass_color_id_,
                 (unsigned long long)serial_);
        if (n >= (64ull << 20)) {
            ++alloc_big_;
            if (alloc_big_ <= 16 || (alloc_big_ & (alloc_big_ - 1)) == 0)
                fprintf(stderr, "d3d9 metal: large allocation (%s): %s (%llu large so far)\n", alloc_site_name(site),
                        a.last, (unsigned long long)alloc_big_);
        }
    }
    void alloc_watch() {
        const uint64_t cur = (uint64_t)[mtl_ currentAllocatedSize];
        const double now = CACurrentMediaTime();
        // Test157c diagnostics (RECOMP_D3D9_STATS): every 5 s, the renderer's live resources, so a growing count can
        // be followed to its owner (the review's 20-reset discriminator).
        static const bool stats = recomp_env("D3D9_STATS") != nullptr;
        static double stats_t = 0;
        if (stats && now - stats_t >= 5.0) {
            stats_t = now;
            fprintf(stderr, "d3d9 metal stats: device allocated %llu MB, textures %zu, buffers %zu, fitted depth %zu "
                            "(frame %llu)\n",
                    (unsigned long long)(cur >> 20), textures_.size(), buffers_.size(), fitted_depth_.size(),
                    (unsigned long long)serial_);
        }
        if (alloc_t0_ == 0 || now - alloc_t0_ > 2.0) {
            if (alloc_t0_ == 0)
                fprintf(stderr, "d3d9 metal: allocation watch on (device allocated %llu MB)\n",
                        (unsigned long long)(cur >> 20));
            alloc_t0_ = now;
            alloc_base_ = cur;
            for (AllocStat &a : alloc_) {
                a.seen_bytes = a.bytes;
                a.seen_count = a.count;
            }
            return;
        }
        if (cur < alloc_base_ + (1ull << 30))
            return;
        if (++alloc_lines_ <= 48) {
            fprintf(stderr, "d3d9 metal: ALLOCATION GROWTH device allocated %llu MB, +%llu MB in %.2f s (frame %llu)\n",
                    (unsigned long long)(cur >> 20), (unsigned long long)((cur - alloc_base_) >> 20), now - alloc_t0_,
                    (unsigned long long)serial_);
            for (int i = 0; i < AS_COUNT; ++i) {
                const AllocStat &a = alloc_[i];
                if (a.count > a.seen_count)
                    fprintf(stderr, "d3d9 metal:   %s: +%llu MB in %llu allocations; last %s\n", alloc_site_name(i),
                            (unsigned long long)((a.bytes - a.seen_bytes) >> 20),
                            (unsigned long long)(a.count - a.seen_count), a.last);
            }
            fprintf(stderr, "d3d9 metal:   fitted depth cache %zu entries, textures %zu, buffers %zu\n",
                    fitted_depth_.size(), textures_.size(), buffers_.size());
        }
        alloc_t0_ = now;
        alloc_base_ = cur;
        for (AllocStat &a : alloc_) {
            a.seen_bytes = a.bytes;
            a.seen_count = a.count;
        }
    }

    // Test157b (review review after the rain incident): fitted depth buffers were never evicted, so a source depth
    // texture that was dropped or redefined left its fitted buffer behind for the rest of the session (and its
    // pointer could be reused by a new texture and hit the stale entry). They now go with their source.
    void forget_fitted_depth(const Tex &t) {
        if (fitted_depth_.empty())
            return;
        const uintptr_t a = (uintptr_t)(__bridge void *)t.texture, b = (uintptr_t)(__bridge void *)t.msaa;
        for (auto it = fitted_depth_.begin(); it != fitted_depth_.end();)
            if (it->second.source && (it->second.source == a || it->second.source == b))
                it = fitted_depth_.erase(it);
            else
                ++it;
        g_fitted_depth_entries = fitted_depth_.size();
    }

    id<MTLTexture> depth_for_size(id<MTLTexture> depth, id<MTLTexture> color, uint32_t level,
                                  uint32_t samples = 1) {
        NSUInteger w = std::max<NSUInteger>(color.width >> level, 1),
                   h = std::max<NSUInteger>(color.height >> level, 1);
        uint64_t key = mix(mix(mix((uint64_t)(__bridge void *)depth, w), h), samples);
        auto it = fitted_depth_.find(key);
        if (it != fitted_depth_.end())
            return it->second.texture;
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth.pixelFormat
                                                               width:w
                                                              height:h
                                                           mipmapped:NO];
        if (samples > 1) {
            td.textureType = MTLTextureType2DMultisample;
            td.sampleCount = samples;
        }
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; // Test121
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [mtl_ newTextureWithDescriptor:td];
        note_alloc(AS_FITTED_DEPTH, t, w, h, samples, (uint64_t)depth.pixelFormat);
        fitted_depth_[key] = FittedDepth{(uintptr_t)(__bridge void *)depth, t};
        g_fitted_depth_entries = fitted_depth_.size();
        return t;
    }

    void begin_frame() {
        if (cmd_)
            return;
        {
            const double t0 = CACurrentMediaTime();
            dispatch_semaphore_wait(frames_, DISPATCH_TIME_FOREVER);
            gpu_wait_s_ += CACurrentMediaTime() - t0;
        }
        cmd_ = [queue_ commandBuffer];
        ++serial_;
        dispatch_semaphore_t sem = frames_;
        uint64_t serial = serial_;
        std::atomic<uint64_t> *done = &completed_;
        std::atomic<uint64_t> *gpu_us = &gpu_us_;
        [cmd_ addCompletedHandler:^(id<MTLCommandBuffer> cb) {
          if (cb.GPUEndTime > cb.GPUStartTime) {
              const uint64_t us = (uint64_t)((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
              gpu_us->fetch_add(us);
              g_host_game_gpu_us.fetch_add(us, std::memory_order_relaxed); // Test83: the overlay's GPU
          }
          if (cb.error)
              fprintf(stderr, "d3d9 metal: command buffer failed: %s\n",
                      cb.error.localizedDescription.UTF8String);
          done->store(serial);
          dispatch_semaphore_signal(sem);
        }];
    }
    void flush() {
        if (!cmd_)
            return;
        end_pass();
        if (vis_) {
            vis_pool_.emplace_back(vis_, serial_);
            vis_ = nil;
        }
        [cmd_ commit];
        if (trace_) {
            submitted_ = serial_;
            trace_->event("submit", trace_context());
        }
        last_ = cmd_;
        cmd_ = nil;
        in_flight_ = true;
    }
    void wait() {
        if (last_) {
            const double t0 = CACurrentMediaTime();
            [last_ waitUntilCompleted];
            gpu_wait_s_ += CACurrentMediaTime() - t0;
            last_ = nil;
        }
        in_flight_ = false;
    }
    // Everything up to frame `used` has run, so the resource can be touched.
    void settle(uint64_t used) {
        if (!used || used <= completed_.load())
            return;
        end_pass();
        if (cmd_ && used >= serial_)
            flush();
        wait();
    }

    void set_scissor(bool on, const int32_t sc[4]) {
        NSUInteger tw =
            pass_color_[0]
                ? std::max<NSUInteger>(pass_color_[0].width >> pass_target_.color[0].level, 1)
                : 1;
        NSUInteger th =
            pass_color_[0]
                ? std::max<NSUInteger>(pass_color_[0].height >> pass_target_.color[0].level, 1)
                : 1;
        MTLScissorRect r = {0, 0, tw, th};
        if (on) {
            const float s = pass_scale_;
            int32_t x0 = std::max(0, (int32_t)std::lround(sc[0] * s)),
                    y0 = std::max(0, (int32_t)std::lround(sc[1] * s));
            int32_t x1 = std::min((int32_t)tw, (int32_t)std::lround(sc[2] * s));
            int32_t y1 = std::min((int32_t)th, (int32_t)std::lround(sc[3] * s));
            if (x1 <= x0 || y1 <= y0) {
                x0 = y0 = 0;
                x1 = y1 = 0;
            }
            r = {(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)std::max(0, x1 - x0),
                 (NSUInteger)std::max(0, y1 - y0)};
        }
        if (!es_.scissor_valid || memcmp(&r, &es_.scissor, sizeof r) != 0) {
            [enc_ setScissorRect:r];
            es_.scissor = r;
            es_.scissor_valid = true;
        }
    }

    // ---- shaders --------------------------------------------------------
    // Stream 0's elements feeding the shader's inputs, for the vertex fetch; null when one
    // of them has a type it cannot decode.
    const d9msl::VFetchLayout *vfetch_layout(const HostD9Draw &d, const d9sh::Program &vp,
                                             d9msl::VFetchLayout *out) {
        out->words = (uint8_t)std::min<uint32_t>(d.stream[0].stride / 4, 64);
        out->check = d.vfetch == 2;
        for (const d9sh::DclIn &in : vp.inputs) {
            if (in.type != d9sh::R_INPUT)
                continue;
            const uint32_t reg = in.index & 15;
            for (uint32_t i = 0; i + 8 <= d.decl_size; i += 8) {
                const uint8_t *e = d.decl + i;
                const uint16_t stream = (uint16_t)(e[0] | e[1] << 8);
                if (stream == 0xff)
                    break;
                const uint16_t offset = (uint16_t)(e[2] | e[3] << 8);
                if (e[6] != in.usage || e[7] != in.usage_index || stream >= 8)
                    continue;
                if (stream == 0) {
                    if (!d9msl::vfetch_type_ok(e[4]) || offset >= out->words * 4u || (offset & 3))
                        return nullptr;
                    out->type[reg] = e[4];
                    out->offset[reg] = (uint8_t)offset;
                    out->mask |= (uint16_t)(1u << reg);
                }
                break;
            }
        }
        return out->mask && out->words ? out : nullptr;
    }

    id<MTLFunction> function(uint64_t code_key, const d9sh::Program &p,
                             const d9msl::PixelVariant &v, bool pixel,
                             const d9msl::VFetchLayout *vf = nullptr) {
        uint64_t key = mix(code_key, pixel ? v.key() + 1 : vf ? vf->key() : 0);
        auto it = functions_.find(key);
        if (it != functions_.end())
            return it->second;
        std::string src, why;
        bool ok =
            pixel ? d9msl::pixel_source(p, v, &src, &why)
            : vf  ? d9msl::vertex_source(p, *vf, host_d9_vfetch_prelude(), &src, &why)
                  : d9msl::vertex_source(p, &src, &why);
        if (const char *dir = recomp_env("D3D9_SHADER_DUMP"); dir && ok) {
            char path[1024];
            // The code key leads, as the D3D9 trace names a draw's shaders by it.
            snprintf(path, sizeof path, "%s/%s-%016llx-%016llx.msl", dir, pixel ? "ps" : "vs",
                     (unsigned long long)code_key, (unsigned long long)key);
            if (FILE *f = fopen(path, "w")) {
                fputs(src.c_str(), f);
                fclose(f);
            }
        }
        id<MTLFunction> fn = nil;
        if (ok) {
            NSError *error = nil;
            std::string warm_key;
            id<MTLLibrary> lib = nil;
            if (warm_) {
                warm_key = warm_->key(src);
                const uint64_t take_t0 = os_monotonic_ns();
                lib = warm_->take(warm_key);
                // Only a wait on the warm-up's in-flight compile of this source takes measurable time.
                if (const uint64_t waited = os_monotonic_ns() - take_t0; waited > 500000ull)
                    note_shader_work(waited, true);
            }
            if (!lib) {
                MTLCompileOptions *opts = ShaderWarm::options();
                const uint64_t compile_t0 = os_monotonic_ns();
                lib = [mtl_ newLibraryWithSource:[NSString stringWithUTF8String:src.c_str()]
                                         options:opts
                                           error:&error];
                note_shader_work(os_monotonic_ns() - compile_t0, true);
                if (lib && warm_)
                    warm_->record(warm_key, src);
            }
            if (lib)
                fn = [lib newFunctionWithName:pixel ? @"ps_main" : @"vs_main"];
            if (fn && !warm_key.empty())
                fn_warm_keys_[(__bridge const void *)fn] = warm_key;   // Test70: names the pipeline identity
            else if (!fn) // Test73: a compiled shader without a warm-up cache is not a failure
                fprintf(stderr, "d3d9 metal: a %s shader failed to compile: %s\n",
                        pixel ? "pixel" : "vertex", error.localizedDescription.UTF8String);
        } else {
            char key_text[64];
            snprintf(key_text, sizeof key_text, "d3d9.metal.untranslated.%llx",
                     (unsigned long long)key);
            log_once(key_text, "d3d9 metal: a %s shader is not translated: %s",
                     pixel ? "pixel" : "vertex", why.c_str());
        }
        functions_[key] = fn;
        return fn;
    }

    MTLVertexDescriptor *vertex_descriptor(const HostD9Draw &d, const d9sh::Program &vp,
                                           uint64_t key, float ascale[16][4]) {
        auto it = vdescs_.find(key);
        if (it != vdescs_.end()) {
            memcpy(ascale, it->second.ascale, sizeof it->second.ascale);
            stream_mask_ = it->second.streams;
            vdesc_needs_zero_ = it->second.zero;
            return it->second.desc;
        }
        VDesc v;
        MTLVertexDescriptor *desc = [MTLVertexDescriptor vertexDescriptor];
        uint32_t streams = 0;
        uint32_t stream_end[8] = {};
        bool zero = false;
        for (const d9sh::DclIn &in : vp.inputs) {
            if (in.type != d9sh::R_INPUT)
                continue;
            uint32_t reg = in.index & 15;
            bool found = false;
            for (uint32_t i = 0; i + 8 <= d.decl_size; i += 8) {
                const uint8_t *e = d.decl + i;
                uint16_t stream = (uint16_t)(e[0] | e[1] << 8);
                if (stream == 0xff)
                    break;
                uint16_t offset = (uint16_t)(e[2] | e[3] << 8);
                uint8_t type = e[4], usage = e[6], index = e[7];
                if (usage != in.usage || index != in.usage_index || stream >= 8)
                    continue;
                MTLVertexFormat fmt;
                float scale[4];
                if (!vertex_format(type, &fmt, scale))
                    continue;
                desc.attributes[reg].format = fmt;
                desc.attributes[reg].offset = offset;
                desc.attributes[reg].bufferIndex = stream;
                memcpy(v.ascale[reg], scale, sizeof scale);
                streams |= 1u << stream;
                stream_end[stream] =
                    std::max<uint32_t>(stream_end[stream], offset + vertex_format_bytes(type));
                found = true;
                break;
            }
            if (!found) {
                static const bool why = recomp_env("D3D9_MISSING_TRACE") != nullptr;
                if (why)
                    fprintf(stderr, "d3d9 missing input: vs %016llx v%u usage %u index %u\n",
                            (unsigned long long)d.vs_key, reg, in.usage, in.usage_index);
                desc.attributes[reg].format = MTLVertexFormatFloat4;
                desc.attributes[reg].offset = 0;
                desc.attributes[reg].bufferIndex = 8;
                zero = true;
            }
        }
        for (uint32_t s = 0; s < 8; ++s) {
            if (!(streams >> s & 1))
                continue;
            uint32_t stride = d.stream[s].stride ? d.stream[s].stride : stream_end[s];
            desc.layouts[s].stride = std::max(stride, stream_end[s]);
            desc.layouts[s].stepFunction = MTLVertexStepFunctionPerVertex;
        }
        if (zero) {
            desc.layouts[8].stride = 16;
            desc.layouts[8].stepFunction = MTLVertexStepFunctionConstant;
            desc.layouts[8].stepRate = 0;
        }
        v.desc = desc;
        v.streams = streams;
        v.zero = zero;
        for (int r = 0; r < 16; ++r)
            for (int k = 0; k < 4; ++k)
                if (v.ascale[r][k] == 0.0f)
                    v.ascale[r][k] = 1.0f;
        memcpy(ascale, v.ascale, sizeof v.ascale);
        stream_mask_ = streams;
        vdesc_needs_zero_ = zero;
        vdescs_[key] = v;
        return desc;
    }

    id<MTLRenderPipelineState> pipeline(uint64_t key, id<MTLFunction> vfn, id<MTLFunction> ffn,
                                        MTLVertexDescriptor *vdesc, const MTLPixelFormat cf[4],
                                        MTLPixelFormat df, bool blend, const uint32_t *rs,
                                        const uint32_t cw[4]) {
        key = mix(key, 0x5a3f0000ull + pass_samples_);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(pass_samples_, 1);
        pd.vertexFunction = vfn;
        pd.fragmentFunction = ffn;
        pd.vertexDescriptor = vdesc;
        for (int i = 0; i < 4; ++i) {
            if (cf[i] == MTLPixelFormatInvalid)
                continue;
            MTLRenderPipelineColorAttachmentDescriptor *a = pd.colorAttachments[i];
            a.pixelFormat = cf[i];
            MTLColorWriteMask m = MTLColorWriteMaskNone;
            if (cw[i] & 1)
                m |= MTLColorWriteMaskRed;
            if (cw[i] & 2)
                m |= MTLColorWriteMaskGreen;
            if (cw[i] & 4)
                m |= MTLColorWriteMaskBlue;
            if (cw[i] & 8)
                m |= MTLColorWriteMaskAlpha;
            a.writeMask = m;
            if (blend) {
                a.blendingEnabled = YES;
                a.sourceRGBBlendFactor = blend_factor(rs[19], false);
                a.destinationRGBBlendFactor = blend_factor(rs[20], false);
                // BOTHSRCALPHA / BOTHINVSRCALPHA override the destination too.
                if (rs[19] == 12)
                    a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                if (rs[19] == 13)
                    a.destinationRGBBlendFactor = MTLBlendFactorSourceAlpha;
                a.rgbBlendOperation = blend_op(rs[171]);
                if (rs[206]) {
                    a.sourceAlphaBlendFactor = blend_factor(rs[207], true);
                    a.destinationAlphaBlendFactor = blend_factor(rs[208], true);
                    a.alphaBlendOperation = blend_op(rs[209]);
                } else {
                    a.sourceAlphaBlendFactor = blend_factor(rs[19], true);
                    a.destinationAlphaBlendFactor = blend_factor(rs[20], true);
                    if (rs[19] == 12)
                        a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
                    if (rs[19] == 13)
                        a.destinationAlphaBlendFactor = MTLBlendFactorSourceAlpha;
                    a.alphaBlendOperation = blend_op(rs[171]);
                }
            }
        }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            pd.stencilAttachmentPixelFormat = df;
        }
        // Test70: a pipeline the warm-up created from an identical recorded identity is used as is.
        std::string spec;
        if (warm_ && warm_->pipelines_on()) {
            auto vk = fn_warm_keys_.find((__bridge const void *)vfn), fk = fn_warm_keys_.find((__bridge const void *)ffn);
            if (vk != fn_warm_keys_.end() && fk != fn_warm_keys_.end()) {
                const PipeSpec ps = warm_spec_of(pd, vk->second, fk->second);
                if (ps.valid() && warm_formats_ok(ps))
                    spec = ps.text();
            }
            if (!spec.empty())
                if (id<MTLRenderPipelineState> ready = warm_->take_pipeline(spec)) {
                    pipelines_[key] = ready;
                    return ready;
                }
        }
        NSError *error = nil;
        const uint64_t pso_t0 = os_monotonic_ns();
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd
                                                                              error:&error];
        note_shader_work(os_monotonic_ns() - pso_t0, false);
        if (!pso)
            fprintf(stderr, "d3d9 metal: pipeline failed: %s\n",
                    error.localizedDescription.UTF8String);
        else if (!spec.empty())
            warm_->record_pipeline(spec);
        pipelines_[key] = pso;
        return pso;
    }

    id<MTLRenderPipelineState> utility_pipeline(uint64_t key, NSString *fragment,
                                                const MTLPixelFormat cf[4], MTLPixelFormat df,
                                                bool write_color) {
        key = mix(key, 0x5a3f0000ull + pass_samples_);
        auto it = pipelines_.find(key);
        if (it != pipelines_.end())
            return it->second;
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.rasterSampleCount = std::max<uint32_t>(pass_samples_, 1);
        pd.vertexFunction = [utility_ newFunctionWithName:@"quad_vs"];
        pd.fragmentFunction = [utility_ newFunctionWithName:fragment];
        for (int i = 0; i < 4; ++i)
            if (cf[i] != MTLPixelFormatInvalid) {
                pd.colorAttachments[i].pixelFormat = cf[i];
                pd.colorAttachments[i].writeMask =
                    (i == 0 && write_color) ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
            }
        if (df != MTLPixelFormatInvalid) {
            pd.depthAttachmentPixelFormat = df;
            pd.stencilAttachmentPixelFormat = df;
        }
        NSError *error = nil;
        id<MTLRenderPipelineState> pso = [mtl_ newRenderPipelineStateWithDescriptor:pd
                                                                              error:&error];
        if (!pso)
            fprintf(stderr, "d3d9 metal: utility pipeline failed: %s\n",
                    error.localizedDescription.UTF8String);
        pipelines_[key] = pso;
        return pso;
    }

    id<MTLDepthStencilState> depth_state(uint64_t key, bool zon, const uint32_t *rs, bool stencil) {
        auto it = depth_states_.find(key);
        if (it != depth_states_.end())
            return it->second;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = zon ? compare_fn(rs[23]) : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = zon && rs[14] != 0;
        if (stencil) {
            MTLStencilDescriptor *front = [MTLStencilDescriptor new];
            front.stencilFailureOperation = stencil_op(rs[53]);
            front.depthFailureOperation = stencil_op(rs[54]);
            front.depthStencilPassOperation = stencil_op(rs[55]);
            front.stencilCompareFunction = compare_fn(rs[56]);
            front.readMask = rs[58] & 0xff;
            front.writeMask = rs[59] & 0xff;
            dd.frontFaceStencil = front;
            if (rs[185]) {
                MTLStencilDescriptor *back = [MTLStencilDescriptor new];
                back.stencilFailureOperation = stencil_op(rs[186]);
                back.depthFailureOperation = stencil_op(rs[187]);
                back.depthStencilPassOperation = stencil_op(rs[188]);
                back.stencilCompareFunction = compare_fn(rs[189]);
                back.readMask = rs[58] & 0xff;
                back.writeMask = rs[59] & 0xff;
                dd.backFaceStencil = back;
            } else {
                dd.backFaceStencil = front;
            }
        }
        id<MTLDepthStencilState> s = [mtl_ newDepthStencilStateWithDescriptor:dd];
        depth_states_[key] = s;
        return s;
    }
    id<MTLDepthStencilState> clear_depth_state(bool depth, bool stencil, uint64_t key) {
        auto it = depth_states_.find(key);
        if (it != depth_states_.end())
            return it->second;
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = depth;
        if (stencil) {
            MTLStencilDescriptor *s = [MTLStencilDescriptor new];
            s.stencilCompareFunction = MTLCompareFunctionAlways;
            s.depthStencilPassOperation = MTLStencilOperationReplace;
            dd.frontFaceStencil = s;
            dd.backFaceStencil = s;
        }
        id<MTLDepthStencilState> st = [mtl_ newDepthStencilStateWithDescriptor:dd];
        depth_states_[key] = st;
        return st;
    }

    // World filtering (host setting, Test72/73): the anisotropy of the game's WORLD texture samplers.
    // Those are the mipmapped (linear mip), linearly or anisotropically filtered samplers with wrapped
    // U and V addressing - roads, walls, terrain. Clamped samplers (render targets, shadow maps,
    // post-process, fonts, HUD, movies), point and unmipmapped samplers keep the game's state.
    // Measured (Test73 mn1, the game's own "anisotropic" texture filtering): the game itself already
    // asks 16x on some world samplers and leaves others trilinear with MAXANISOTROPY 16.
    // 0 = the game's own state; 1 = trilinear (no anisotropy anywhere); 2/3/4 = 4x/8x/16x on every
    // world sampler. Read per lookup (an atomic), so a change takes effect at the next draw with no
    // rebuild; the cache holds at most five variants of each game sampler state.
    static bool world_sampler(const uint32_t *st) {
        const bool filtered = st[6] >= 2 && st[6] <= 3 && st[5] >= 2 && st[5] <= 3;
        return filtered && st[7] == 2 && st[1] == 1 && st[2] == 1;
    }
    static int world_anisotropy(const uint32_t *st, int policy) {
        const int game = (st[6] == 3 || st[5] == 3) ? int(std::max<uint32_t>(1, std::min<uint32_t>(16, st[10]))) : 1;
        if (!policy || !world_sampler(st))
            return game;
        switch (policy) {
        case 1:
            return 1;
        case 2:
            return 4;
        case 3:
            return 8;
        default:
            return 16;
        }
    }
    id<MTLSamplerState> sampler(const uint32_t *st) {
        const int policy = std::clamp(mods_display_filtering(), 0, 4);
        uint64_t key = fnv(st, 14 * sizeof(uint32_t));
        if (policy && world_sampler(st))
            key ^= 0x9e3779b97f4a7c15ull * uint64_t(policy);
        auto it = samplers_.find(key);
        if (it != samplers_.end())
            return it->second;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.sAddressMode = address_mode(st[1]);
        sd.tAddressMode = address_mode(st[2]);
        sd.rAddressMode = address_mode(st[3]);
        sd.magFilter = st[5] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.minFilter = st[6] >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = st[7] == 0   ? MTLSamplerMipFilterNotMipmapped
                       : st[7] == 1 ? MTLSamplerMipFilterNearest
                                    : MTLSamplerMipFilterLinear;
        const int anisotropy = world_anisotropy(st, policy);
        if (anisotropy > 1)
            sd.maxAnisotropy = NSUInteger(anisotropy);
        {
            // Diagnostic (RECOMP_D3D9_STATS): each new sampler state the game asked for, bounded.
            static const bool stats = recomp_env("D3D9_STATS") != nullptr;
            static int logged = 0;
            if (stats && logged < 64) {
                ++logged;
                fprintf(stderr, "d3d9 metal: sampler min %u mag %u mip %u maxaniso %u -> anisotropy %d (world filtering %d) address %u/%u/%u lodmin %u\n",
                        st[6], st[5], st[7], st[10], anisotropy, policy, st[1], st[2], st[3], st[9]);
            }
        }
        sd.lodMinClamp = (float)st[9];
        uint32_t border = st[4];
        sd.borderColor = (border >> 24) < 128  ? MTLSamplerBorderColorTransparentBlack
                         : (border & 0xffffff) ? MTLSamplerBorderColorOpaqueWhite
                                               : MTLSamplerBorderColorOpaqueBlack;
        id<MTLSamplerState> s = [mtl_ newSamplerStateWithDescriptor:sd];
        samplers_[key] = s;
        return s;
    }

    id<MTLTexture> placeholder(bool cube, bool volume = false) {
        __strong id<MTLTexture> &t = volume ? white_volume_ : cube ? white_cube_ : white_;
        if (t)
            return t;
        MTLTextureDescriptor *td =
            cube ? [MTLTextureDescriptor
                       textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                       size:1
                                                  mipmapped:NO]
                 : [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                      width:1
                                                                     height:1
                                                                  mipmapped:NO];
        if (volume)
            td.textureType = MTLTextureType3D;
        t = [mtl_ newTextureWithDescriptor:td];
        const uint8_t black[4] = {0, 0, 0, 255};
        for (NSUInteger s = 0; s < (cube ? 6u : 1u); ++s)
            [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                  mipmapLevel:0
                        slice:s
                    withBytes:black
                  bytesPerRow:4
                bytesPerImage:volume ? 4 : 0];
        return t;
    }

    // A depth texture at the far plane: every shadow lookup passes.
    id<MTLTexture> placeholder_depth() {
        if (white_depth_)
            return white_depth_;
        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                         width:1
                                        height:1
                                     mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        white_depth_ = [mtl_ newTextureWithDescriptor:td];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.depthAttachment.texture = white_depth_;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        [cb commit];
        return white_depth_;
    }

    void set_constants(bool vertex, const float *c, uint32_t count, const d9sh::Program &p) {
        // As many as the program reads, so its own defs always land and a
        // read past what the game set is zero rather than past the buffer.
        uint32_t n = std::max<uint32_t>(std::max<uint32_t>(count, p.max_const), 1);
        constants_.assign(n * 4, 0.0f);
        if (c && count)
            memcpy(constants_.data(), c, count * 4 * sizeof(float));
        for (const auto &def : p.defs)
            if (def.first < n)
                memcpy(&constants_[def.first * 4], def.second.data(), 4 * sizeof(float));
        if (vertex)
            [enc_ setVertexBytes:constants_.data()
                          length:constants_.size() * sizeof(float)
                         atIndex:16];
        else
            [enc_ setFragmentBytes:constants_.data()
                            length:constants_.size() * sizeof(float)
                           atIndex:0];
    }

    void bind_inline(int slot, const uint8_t *bytes, uint32_t size) {
        if (size <= 4000) {
            [enc_ setVertexBytes:bytes length:size atIndex:slot];
            return;
        }
        id<MTLBuffer> b = transient(size);
        memcpy((uint8_t *)b.contents + transient_used_ - size, bytes, size);
        [enc_ setVertexBuffer:b offset:transient_used_ - size atIndex:slot];
    }
    // Space in this frame's upload buffer; returns the buffer, with the space
    // ending at transient_used_.
    id<MTLBuffer> transient(uint32_t size) {
        uint32_t aligned = (size + 15) & ~15u;
        if (!transient_ || transient_used_ + aligned > transient_.length) {
            NSUInteger len = std::max<NSUInteger>(4u << 20, aligned);
            transient_ = [mtl_ newBufferWithLength:len options:MTLResourceStorageModeShared];
            note_alloc(AS_TRANSIENT, transient_, len, 1, 1, 0);
            transient_used_ = 0;
        }
        transient_used_ += aligned;
        return transient_;
    }

    d9trace::Result encode_primitives(const HostD9Draw &d) {
        MTLPrimitiveType type;
        uint32_t count = d.primitive_count;
        bool fan = false;
        switch (d.primitive) {
        case 1:
            type = MTLPrimitiveTypePoint;
            break;
        case 2:
            type = MTLPrimitiveTypeLine;
            count *= 2;
            break;
        case 3:
            type = MTLPrimitiveTypeLineStrip;
            count += 1;
            break;
        case 4:
            type = MTLPrimitiveTypeTriangle;
            count *= 3;
            break;
        case 5:
            type = MTLPrimitiveTypeTriangleStrip;
            count += 2;
            break;
        case 6:
            type = MTLPrimitiveTypeTriangle;
            fan = true;
            break;
        default:
            return d9trace::Result::UnsupportedPrimitive;
        }
        if (!d.primitive_count)
            return d9trace::Result::Empty;
        bool indexed = d.index_buffer || d.inline_indices;
        if (!indexed && !fan) {
            [enc_ drawPrimitives:type vertexStart:d.start vertexCount:count];
            return d9trace::Result::Encoded;
        }
        MTLIndexType itype = d.index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16;
        uint32_t isize = d.index_size == 4 ? 4 : 2;
        if (!fan) {
            if (d.index_buffer) {
                auto b = buffers_.find(d.index_buffer);
                if (b == buffers_.end() || !b->second.buffer)
                    return d9trace::Result::MissingIndexBuffer;
                if ((uint64_t)(d.start + count) * isize > b->second.buffer.length)
                    return d9trace::Result::IndexRange;
                b->second.used = serial_;
                [enc_ drawIndexedPrimitives:type
                                 indexCount:count
                                  indexType:itype
                                indexBuffer:b->second.buffer
                          indexBufferOffset:d.start * isize
                              instanceCount:1
                                 baseVertex:d.base_vertex
                               baseInstance:0];
            } else {
                id<MTLBuffer> b = transient(count * isize);
                memcpy((uint8_t *)b.contents + transient_used_ - ((count * isize + 15) & ~15u),
                       d.inline_indices, count * isize);
                [enc_ drawIndexedPrimitives:type
                                 indexCount:count
                                  indexType:itype
                                indexBuffer:b
                          indexBufferOffset:transient_used_ - ((count * isize + 15) & ~15u)];
            }
            return d9trace::Result::Encoded;
        }
        // A fan becomes a triangle list over the same vertices.
        std::vector<uint32_t> list;
        list.reserve(count * 3);
        auto source = [&](uint32_t i) -> uint32_t {
            if (!indexed)
                return d.start + i;
            const uint8_t *p = nullptr;
            if (d.inline_indices) {
                p = d.inline_indices + (size_t)i * isize;
            } else {
                auto b = buffers_.find(d.index_buffer);
                size_t at = (size_t)(d.start + i) * isize;
                if (b == buffers_.end() || at + isize > b->second.shadow.size())
                    return 0;
                p = b->second.shadow.data() + at;
            }
            uint32_t v = isize == 4
                             ? (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24)
                             : (uint32_t)(p[0] | p[1] << 8);
            return v + (uint32_t)(d.inline_indices ? 0 : d.base_vertex);
        };
        for (uint32_t i = 0; i < count; ++i) {
            list.push_back(source(0));
            list.push_back(source(i + 1));
            list.push_back(source(i + 2));
        }
        uint32_t bytes = (uint32_t)list.size() * 4;
        id<MTLBuffer> b = transient(bytes);
        NSUInteger off = transient_used_ - ((bytes + 15) & ~15u);
        memcpy((uint8_t *)b.contents + off, list.data(), bytes);
        [enc_ drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:list.size()
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:b
                  indexBufferOffset:off];
        return d9trace::Result::Encoded;
    }

    void dump(Tex &t, uint32_t w, uint32_t h) {
        if (t.info.pixel != MTLPixelFormatBGRA8Unorm)
            return;
        settle(t.used);
        std::vector<uint8_t> bgra((size_t)w * h * 4);
        [t.texture getBytes:bgra.data()
                bytesPerRow:w * 4
                 fromRegion:MTLRegionMake2D(0, 0, w, h)
                mipmapLevel:0];
        std::vector<uint8_t> rgb((size_t)w * h * 3);
        for (size_t i = 0, n = (size_t)w * h; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        char path[1024];
        snprintf(path, sizeof path, "%s/present_%05u.ppm", host_dump_dir(),
                 (unsigned)(presents_ + 1));
        host_write_ppm(path, rgb.data(), (int)w, (int)h);
    }

    struct VDesc {
        MTLVertexDescriptor *desc = nil;
        float ascale[16][4] = {};
        uint32_t streams = 0;
        bool zero = false;
    };

    gpu::MetalDevice *device_;
    id<MTLDevice> mtl_;
    // MetalFX spatial upscaling of the presented frame (metalfx_upscale).
#if RECOMP_HAVE_METALFX
    id<MTLFXSpatialScaler> fx_ = nil;
#endif
    id<MTLTexture> fx_out_ = nil;
    gpu::Texture fx_imported_{};
    uint32_t fx_in_w_ = 0, fx_in_h_ = 0, fx_out_w_ = 0, fx_out_h_ = 0;
    MTLPixelFormat fx_format_ = MTLPixelFormatInvalid;
    mutable int fx_supported_ = -1; // a cache filled by the const metalfx_available
#if RECOMP_HAVE_METALFX
    // The scalers being made in the background (fx_build_async); the build loop holds its own
    // reference, so a renderer gone first only drops the result.
    std::shared_ptr<FxSlot> fx_slot_ = std::make_shared<FxSlot>(), split_slot_ = std::make_shared<FxSlot>();
    std::optional<ScalerKey> split_failed_; // a split configuration MetalFX could not make: not asked again
#endif
    bool fx_asked_ = false, split_asked_ = false; // a configuration other than the current one was asked for
    // Split resolution (scene_end): the back buffer's own texture, at the render quality's
    // scale, holds the frame's 3D; at the scene's end MetalFX upscales it into split_hi_ at
    // the game's size, and the back buffer (and the depth buffer drawn with it) point at
    // full-size textures until Present, so the interface draws at the screen's own size.
    struct SplitSide {
        id<MTLTexture> texture = nil, msaa = nil;
        gpu::Texture imported{};
        float scale = 1.0f;
    };
#if RECOMP_HAVE_METALFX
    id<MTLFXSpatialScaler> split_fx_ = nil;
#endif
    id<MTLTexture> split_hi_ = nil, split_hi_depth_ = nil;
    gpu::Texture split_hi_imported_{};
    uint32_t split_bb_ = 0, split_depth_ = 0; // swapped now, 0 when not
    uint32_t split_depth_seen_ = 0;          // the depth buffer last drawn with the back buffer
    SplitSide split_lo_bb_, split_lo_depth_;
    // Dynamic share (update_dynamic_scale): the part of the back buffer's texture, across
    // and down, its 3D is drawn in this frame; 1 the whole texture.
    float dyn_ = 1.0f;
    bool dyn_unsafe_ = false; // a pass drew elsewhere with the back buffer's depth
    int dyn_level_ = 0;       // host_thermal_level, read once a second
    double dyn_checked_ = 0;
    id<MTLCommandQueue> queue_;
    id<MTLLibrary> utility_;
    bool bc_ = false;
    dispatch_semaphore_t frames_;
    id<MTLCommandBuffer> cmd_ = nil, last_ = nil;
    uint64_t serial_ = 0;
    std::vector<std::pair<uint64_t, uint32_t>> vfetch_tags_; // frames that wrote the vertex fetch check: serial, tag
    uint64_t vfetch_unfit_ = 0;
    uint64_t submitted_ = 0, pass_serial_ = 0;
    uint32_t resource_generation_ = 0, trace_flushes_ = 0;
    uint64_t trace_bytes_ = 0, burst_trace_start_ = 0, burst_pixel_bytes_ = 0, burst_start_ = 0;
    uint32_t burst_left_ = 0, burst_captured_ = 0;
    bool burst_used_ = false;
    std::unique_ptr<d9trace::Ring> trace_;
    bool capture_mode_ = false;
    d9trace::CaptureControl capture_control_;
    uint64_t capture_epoch_ = os_wall_time_us(), capture_summary_count_ = 0;
    std::array<d9trace::CaptureSummary, d9trace::CaptureControl::timeline_capacity>
        capture_summaries_{};
    std::unique_ptr<d9trace::Ring> capture_spare_;
    std::future<CaptureSave> capture_job_;
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> gpu_us_{0}; // GPU time of finished command buffers since the last report
    double gpu_wait_s_ = 0;           // time the game thread spent waiting for the GPU
    uint64_t report_cpu_ns_ = 0, report_instructions_ = 0;
    bool in_flight_ = false;
    id<MTLRenderCommandEncoder> enc_ = nil;
    std::unordered_map<uint32_t, Query> queries_;
    uint32_t query_ = 0;
    id<MTLBuffer> vis_ = nil;
    NSUInteger vis_used_ = 0;
    std::vector<std::pair<id<MTLBuffer>, uint64_t>> vis_pool_;
    bool pass_vis_ = false;
    int pass_slot_ = -1;
    HostD9Target pass_target_{};
    float pass_scale_ = 1.0f;
    uint32_t pass_samples_ = 1; // sample count of the open pass's attachments
    bool pass_fitted_depth_ = false;
    // Test155 allocation watch (see note_alloc / alloc_watch).
    AllocStat alloc_[AS_COUNT];
    uint64_t alloc_base_ = 0, alloc_lines_ = 0, alloc_big_ = 0;
    double alloc_t0_ = 0;
    bool scale_chosen_ = false;
    uint32_t base_rows_ = 0;
    uint32_t base_cols_ = 0; // the back buffer's width, beside base_rows_
    int rescale_frames_ = 0;
    // Render targets are this many times the guest's size (RECOMP_D3D9_SCALE).
    float scale_ = recomp_env("D3D9_SCALE")
                       ? std::max(1.0f, std::min(8.0f, (float)atof(recomp_env("D3D9_SCALE"))))
                       : 1.0f;
    id<MTLTexture> pass_color_[4] = {nil, nil, nil, nil};
    // Test86 HDR: the guest id of the pass's first colour target, and whether this frame had the core
    // mod's pre-HUD scene marker (private render state 250, reset by Present in d3d9.cpp).
    uint32_t pass_color_id_ = 0;
    bool hdr_mask_frame_ = false;
    uint32_t hud_rs_[256] = {};
    uint64_t hdr_scene_draws_ = 0; // Test133: mirror draws that kept the HDR expansion
    id<MTLTexture> pass_depth_ = nil;
    uint32_t stream_mask_ = 0;
    bool vdesc_needs_zero_ = false;
    id<MTLBuffer> zero_ = nil;
    id<MTLBuffer> transient_ = nil;
    NSUInteger transient_used_ = 0;
    id<MTLTexture> white_ = nil, white_cube_ = nil, white_depth_ = nil, white_volume_ = nil;
    std::vector<uint8_t> scratch_;
    std::vector<float> constants_;
    std::unordered_map<uint32_t, Tex> textures_;
    std::vector<Tex *> tex_index_;
    // What the encoder was last told in this pass, so a draw that repeats it
    // makes no call. Reset whenever a new encoder starts or a clear or blit
    // sets state of its own.
    struct EncoderState {
        __unsafe_unretained id<MTLRenderPipelineState> pipeline = nil;
        __unsafe_unretained id<MTLDepthStencilState> depth = nil;
        int cull = -1, fill = -1;
        uint32_t stencil_ref = 0xffffffffu;
        float bias = NAN, slope = NAN;
        bool viewport_valid = false, scissor_valid = false;
        MTLViewport viewport{};
        MTLScissorRect scissor{};
        __unsafe_unretained id<MTLTexture> texture[16] = {};
        __unsafe_unretained id<MTLSamplerState> sampler[16] = {};
    };
    EncoderState es_;
    std::unordered_map<uint32_t, Buf> buffers_;
    std::unordered_map<uint64_t, id<MTLFunction>> functions_;
    std::unordered_map<const void *, std::string> fn_warm_keys_;
    std::set<uint64_t> msaa_logged_;   // Test70: multisampled target sizes already reported   // Test70: function -> recorded library key
    std::unordered_map<uint64_t, VDesc> vdescs_;
    std::unordered_map<uint64_t, id<MTLRenderPipelineState>> pipelines_;
    uint64_t shader_frame_ns_ = 0, shader_frame_max_ns_ = 0, shader_total_ns_ = 0;
    uint64_t shader_frame_lib_ns_ = 0, shader_frame_pso_ns_ = 0;
    std::unique_ptr<ShaderWarm> warm_;
    unsigned shader_frame_libs_ = 0, shader_frame_psos_ = 0, shader_reports_ = 0;
    std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_states_;
    std::unordered_map<uint64_t, id<MTLSamplerState>> samplers_;
    // A depth buffer of a pass's own size/samples, cached per source depth texture (depth_for_size). Each entry
    // remembers its source, so it is dropped with that texture (forget_fitted_depth): Test157b.
    struct FittedDepth {
        uintptr_t source = 0;
        id<MTLTexture> texture = nil;
    };
    std::unordered_map<uint64_t, FittedDepth> fitted_depth_;
    uint64_t draws_ = 0, presents_ = 0, stat_calls_ = 0;
    double report_time_ = 0;
    uint64_t report_frames_ = 0, report_draws_ = 0;
    std::map<std::string, uint64_t> skips_;
    std::set<std::string> undecoded_;
    uint32_t last_presented_ = 0, last_w_ = 0, last_h_ = 0;
    std::mutex kept_mutex_;
    id<MTLTexture> kept_ = nil;
    id<MTLCommandBuffer> kept_cmd_ = nil;
    uint32_t kept_w_ = 0, kept_h_ = 0;
    std::string probe_tag_;
    uint64_t probe_frame_ =
        recomp_env("D3D9_PROBE") ? strtoull(recomp_env("D3D9_PROBE"), nullptr, 10) : 0;
};

} // namespace

size_t host_d9_metal_fitted_depth_entries() {
    return g_fitted_depth_entries.load();
}

D9Backend *d9_metal_create(gpu::Device *device) {
    auto *metal = dynamic_cast<gpu::MetalDevice *>(device);
    if (!metal)
        return nullptr;
    auto *r = new Renderer(metal);
    if (!r->ok()) {
        delete r;
        return nullptr;
    }
    g_fitted_depth_entries = 0;
    return r;
}

void d9_autorelease_run(void (*fn)(void *), void *arg) {
    @autoreleasepool {
        fn(arg);
    }
}
