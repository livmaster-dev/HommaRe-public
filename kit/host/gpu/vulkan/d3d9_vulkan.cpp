// d3d9_vulkan.cpp - the Direct3D 9 device's renderer on Vulkan: a D9Backend
// (host/gpu/d3d9_backend.h), driven by host/gpu/d3d9_host.cpp.
//
// It follows the Metal renderer (host/gpu/metal/d3d9_metal.mm) closely:
// shaders are translated to GLSL (dx/d3d9_glsl.h) and compiled to SPIR-V
// with glslang once per program; textures, surfaces and buffers are mirrored
// under the shim's ids; draws are recorded into one command buffer per frame,
// submitted at Present, and the back buffer is handed to the presenter as a
// GPU image. Every image stays in VK_IMAGE_LAYOUT_GENERAL, as the rest of the
// Vulkan backend's do, and passes use dynamic rendering.
//
// Resources a recorded frame still reads are destroyed only once that frame
// has finished: Vulkan, unlike Metal, does not keep them alive for us.
#include "vulkan_device.h"

#include "../../../dx/d3d9_glsl.h"
#include "../../../dx/d3d9_shader.h"
#include "../../../dx/host_d9.h"
#include "../../../platform/os.h"
#include "../../present.h"
#include "../d3d9_backend.h"
#include "../d3d9_common.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

bool log_once(const char *key, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

namespace {

using namespace d9gpu;
using gpu::VulkanDevice;

// ---- shaders -----------------------------------------------------------------

std::vector<uint32_t> compile_glsl(const std::string &source, bool pixel, std::string *error) {
    static std::once_flag once;
    std::call_once(once, [] { glslang_initialize_process(); });
    glslang_input_t in{};
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = pixel ? GLSLANG_STAGE_FRAGMENT : GLSLANG_STAGE_VERTEX;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_1;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_3;
    in.code = source.c_str();
    in.default_version = 450;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    std::vector<uint32_t> words;
    glslang_shader_t *shader = glslang_shader_create(&in);
    if (!glslang_shader_preprocess(shader, &in) || !glslang_shader_parse(shader, &in)) {
        *error = glslang_shader_get_info_log(shader);
        glslang_shader_delete(shader);
        return words;
    }
    glslang_program_t *program = glslang_program_create();
    glslang_program_add_shader(program, shader);
    if (!glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT)) {
        *error = glslang_program_get_info_log(program);
    } else {
        glslang_program_SPIRV_generate(program, in.stage);
        words.resize(glslang_program_SPIRV_get_size(program));
        glslang_program_SPIRV_get(program, words.data());
    }
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return words;
}

// ---- state translation ---------------------------------------------------------

VkBlendFactor blend_factor(uint32_t b, bool alpha) {
    switch (b) {
    case 1:
        return VK_BLEND_FACTOR_ZERO;
    case 2:
        return VK_BLEND_FACTOR_ONE;
    case 3:
        return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
    case 4:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:
    case 12:
        return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6:
    case 13:
        return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:
        return VK_BLEND_FACTOR_DST_ALPHA;
    case 8:
        return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:
        return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
    case 10:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11:
        return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 14:
        return alpha ? VK_BLEND_FACTOR_CONSTANT_ALPHA : VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 15:
        return alpha ? VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA
                     : VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default:
        return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp blend_op(uint32_t op) {
    switch (op) {
    case 2:
        return VK_BLEND_OP_SUBTRACT;
    case 3:
        return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4:
        return VK_BLEND_OP_MIN;
    case 5:
        return VK_BLEND_OP_MAX;
    default:
        return VK_BLEND_OP_ADD;
    }
}
VkCompareOp compare_op(uint32_t f) {
    switch (f) {
    case 1:
        return VK_COMPARE_OP_NEVER;
    case 2:
        return VK_COMPARE_OP_LESS;
    case 3:
        return VK_COMPARE_OP_EQUAL;
    case 4:
        return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5:
        return VK_COMPARE_OP_GREATER;
    case 6:
        return VK_COMPARE_OP_NOT_EQUAL;
    case 7:
        return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default:
        return VK_COMPARE_OP_ALWAYS;
    }
}
VkStencilOp stencil_op(uint32_t op) {
    switch (op) {
    case 2:
        return VK_STENCIL_OP_ZERO;
    case 3:
        return VK_STENCIL_OP_REPLACE;
    case 4:
        return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5:
        return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6:
        return VK_STENCIL_OP_INVERT;
    case 7:
        return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8:
        return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:
        return VK_STENCIL_OP_KEEP;
    }
}
VkSamplerAddressMode address_mode(uint32_t a, bool mirror_clamp) {
    switch (a) {
    case 2:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 5:
        return mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
                            : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// A D3DDECLTYPE as a Vulkan vertex format, and the scale that turns what the
// shader receives back into the value Direct3D would have handed it.
bool vertex_format(uint32_t type, bool bgra_ok, VkFormat *fmt, float scale[4], bool *swap) {
    for (int k = 0; k < 4; ++k)
        scale[k] = 1.0f;
    *swap = false;
    switch (type) {
    case 0:
        *fmt = VK_FORMAT_R32_SFLOAT;
        return true;
    case 1:
        *fmt = VK_FORMAT_R32G32_SFLOAT;
        return true;
    case 2:
        *fmt = VK_FORMAT_R32G32B32_SFLOAT;
        return true;
    case 3:
        *fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
        return true;
    case 4:
        if (bgra_ok) {
            *fmt = VK_FORMAT_B8G8R8A8_UNORM;
        } else {
            *fmt = VK_FORMAT_R8G8B8A8_UNORM;
            *swap = true;
        }
        return true;
    case 5:
        *fmt = VK_FORMAT_R8G8B8A8_UNORM;
        for (int k = 0; k < 4; ++k)
            scale[k] = 255.0f;
        return true;
    case 6:
        *fmt = VK_FORMAT_R16G16_SNORM;
        scale[0] = scale[1] = 32767.0f;
        return true;
    case 7:
        *fmt = VK_FORMAT_R16G16B16A16_SNORM;
        for (int k = 0; k < 4; ++k)
            scale[k] = 32767.0f;
        return true;
    case 8:
        *fmt = VK_FORMAT_R8G8B8A8_UNORM;
        return true;
    case 9:
        *fmt = VK_FORMAT_R16G16_SNORM;
        return true;
    case 10:
        *fmt = VK_FORMAT_R16G16B16A16_SNORM;
        return true;
    case 11:
        *fmt = VK_FORMAT_R16G16_UNORM;
        return true;
    case 12:
        *fmt = VK_FORMAT_R16G16B16A16_UNORM;
        return true;
    case 13:
        *fmt = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        scale[0] = scale[1] = scale[2] = 1023.0f;
        return true;
    case 14:
        *fmt = VK_FORMAT_A2B10G10R10_SNORM_PACK32;
        return true;
    case 15:
        *fmt = VK_FORMAT_R16G16_SFLOAT;
        return true;
    case 16:
        *fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
        return true;
    default:
        return false;
    }
}

// One of the vertex fetch's buffers (dx/host_d9.h), which its shaders read as storage.
bool vfetch_buffer(uint32_t id) {
    return id - HOST_D9_VFETCH_BUFFER(0) < 5u;
}

// ---- resources -----------------------------------------------------------------

struct VkFmt {
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    Conv conv = Conv::Unsupported;
    uint32_t bytes = 4;
    bool block = false;
    bool alpha_only = false; // stored in red, sampled as alpha
    bool depth = false;
    bool bgra8() const {
        return format == VK_FORMAT_B8G8R8A8_UNORM;
    }
};

struct Tex {
    HostD9TextureDesc desc{};
    VkFmt info;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView sample_view = VK_NULL_HANDLE;               // what shaders read
    uint32_t width = 0, height = 0, levels = 1, layers = 1; // physical size
    std::unordered_map<uint32_t, VkImageView> targets;      // level << 8 | face -> attachment view
    gpu::Texture imported;
    uint64_t used = 0;
    float scale = 1.0f;
    bool cube = false, depth = false;
    // A multisampled target draws into `msaa` and resolves into `image`.
    VkImage msaa = VK_NULL_HANDLE;
    VkDeviceMemory msaa_memory = VK_NULL_HANDLE;
    VkImageView msaa_view = VK_NULL_HANDLE;
    uint32_t samples = 1;
};

struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t *map = nullptr;
    uint64_t size = 0;
};

struct SpareBuf {
    HostBuffer buf;
    uint64_t retired = 0;
    uint32_t lo = 0, hi = 0;
};
struct Buf {
    HostBuffer buf;
    std::vector<uint8_t> shadow;
    uint64_t used = 0;
    std::vector<SpareBuf> spares;
};

// Space for one frame's transient data: uniforms, inline geometry, uploads.
struct Ring {
    std::vector<HostBuffer> chunks;
    size_t chunk = 0;
    uint64_t used = 0;
};
struct Slice {
    VkBuffer buffer = VK_NULL_HANDLE;
    uint64_t offset = 0;
    uint8_t *data = nullptr;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> pools;
    size_t pool_used = 0;
    std::unordered_map<uint64_t, VkDescriptorSet> sets;
    Ring ring;
    VkQueryPool queries = VK_NULL_HANDLE;
    uint32_t queries_used = 0;
    uint64_t serial = 0;
    double submitted_at = 0;
};

class VkRenderer final : public D9Backend {
  public:
    const char *name() const override {
        return "Vulkan";
    }
    int vfetch() const override {
        return vfetch_ ? HOST_D9_VFETCH_GLSL : 0;
    }
    // The vertex fetch check buffer (HOST_D9_VFETCH_BUFFER(4)), handed over once the frames
    // that wrote it have finished.
    void vfetch_check(HostD9VFetchVisit visit, void *user) override {
        auto it = buffers_.find(HOST_D9_VFETCH_BUFFER(4));
        if (it == buffers_.end() || !it->second.buf.map || vfetch_tags_.empty())
            return;
        poll();
        const uint64_t done = completed_;
        uint32_t done_tag = 0;
        while (!vfetch_tags_.empty() && vfetch_tags_.front().first <= done) {
            done_tag = vfetch_tags_.front().second;
            vfetch_tags_.erase(vfetch_tags_.begin());
        }
        if (!done_tag)
            return;
        visit((const uint32_t *)(const void *)it->second.buf.map,
              (uint32_t)(it->second.shadow.size() / 4), done_tag, user);
    }

    explicit VkRenderer(VulkanDevice *device) : dev_(device), vk_(device->native_device()) {
        VkPhysicalDevice phys = device->native_physical();
        vkGetPhysicalDeviceProperties(phys, &props_);
        const VkPhysicalDeviceFeatures &f = device->native_features();
        bc_ = f.textureCompressionBC;
        wireframe_ = f.fillModeNonSolid;
        precise_ = f.occlusionQueryPrecise;
        independent_blend_ = f.independentBlend;
        anisotropy_ = f.samplerAnisotropy;
        vertex_stores_ = f.vertexPipelineStoresAndAtomics;
        // The vertex fetch binds five storage buffers to the vertex stage.
        vfetch_ = props_.limits.maxPerStageDescriptorStorageBuffers >= 5;
        auto supports = [&](VkFormat fmt, VkFormatFeatureFlags want, bool vertex) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(phys, fmt, &fp);
            return ((vertex ? fp.bufferFeatures : fp.optimalTilingFeatures) & want) == want;
        };
        bgra_vertex_ =
            supports(VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT, true);
        const VkFormatFeatureFlags ds =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        depth_format_ = supports(VK_FORMAT_D32_SFLOAT_S8_UINT, ds, false)
                            ? VK_FORMAT_D32_SFLOAT_S8_UINT
                            : VK_FORMAT_D24_UNORM_S8_UINT;
        if (bc_)
            bc_ = supports(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
                           false) &&
                  supports(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, false);
        // What a phone's GPU may lack, turned off on one that has it, so the
        // other path can be seen on any machine: RECOMP_VULKAN_BC=0 decodes
        // DXT on upload (most Mali and PowerVR GPUs sample no BC formats),
        // RECOMP_VULKAN_VFETCH=0 leaves the vertex fetch out (fewer than five
        // storage buffers in the vertex stage), so the GPU skinning that
        // needs it hands its work back to the CPU.
        if (const char *v = recomp_env("VULKAN_BC"); v && v[0] == '0')
            bc_ = false;
        if (const char *v = recomp_env("VULKAN_VFETCH"); v && v[0] == '0')
            vfetch_ = false;
        fprintf(stderr,
                "d3d9 vulkan: %s; BC textures %s; vertex fetch %s (%u storage buffers a vertex shader, "
                "vertex stores %s)\n",
                props_.deviceName, bc_ ? "sampled" : "decoded to BGRA8 on upload", vfetch_ ? "on" : "off",
                props_.limits.maxPerStageDescriptorStorageBuffers, vertex_stores_ ? "yes" : "no");
        uniform_align_ = std::max<uint64_t>(props_.limits.minUniformBufferOffsetAlignment, 16);
        ok_ = init_layout();
        if (ok_) {
            zero_ = make_host_buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            ok_ = zero_.buffer != VK_NULL_HANDLE;
            if (ok_)
                memset(zero_.map, 0, 64);
        }
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = device->native_queue_family();
        if (ok_ && vkCreateCommandPool(vk_, &pci, nullptr, &oneshot_pool_) != VK_SUCCESS)
            ok_ = false;
        if (ok_) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = oneshot_pool_;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            ok_ = vkAllocateCommandBuffers(vk_, &ai, &oneshot_cb_) == VK_SUCCESS &&
                  vkCreateFence(vk_, &fci, nullptr, &oneshot_fence_) == VK_SUCCESS;
        }
        if (ok_) {
            // Made now, so a draw never has to leave its pass to make one.
            ok_ = placeholder(false) && placeholder(true) && placeholder_depth() &&
                  shadow_sampler() && sampler(nullptr);
            flush();
        }
        if (!ok_)
            fprintf(stderr, "d3d9 vulkan: the renderer could not start\n");
    }

    bool ok() const {
        return ok_;
    }

    // ---- textures ------------------------------------------------------
    void define(const HostD9TextureDesc &d) override {
        end_pass();
        Tex &t = textures_[d.id];
        retire_tex(t);
        t = Tex{};
        t.desc = d;
        if (d.kind == HOST_D9_TEX_VOLUME) {
            index_texture(d.id, &t);
            fprintf(stderr, "d3d9 vulkan: volume resources are not implemented\n");
            return;
        }
        t.info = vk_format_for(d.format);
        if (t.info.conv == Conv::Unsupported) {
            fprintf(stderr, "d3d9 vulkan: texture format %08x is drawn as BGRA8\n", d.format);
            t.info.conv = Conv::Direct;
        }
        bool depth = (d.usage & HOST_D9_USAGE_DEPTH) || t.info.depth;
        if (depth) {
            t.info.format = depth_format_;
            t.info.depth = true;
            t.info.block = false;
            t.info.alpha_only = false;
        }
        const bool cube = d.kind == HOST_D9_TEX_CUBE;
        uint32_t width = d.width ? d.width : 1;
        uint32_t height = cube ? width : (d.height ? d.height : 1);
        bool target = (d.usage & (HOST_D9_USAGE_RENDERTARGET | HOST_D9_USAGE_DEPTH)) || depth;
        if (target && !scale_chosen_ && d.kind == HOST_D9_TEX_2D) {
            scale_chosen_ = true;
            base_rows_ = d.height;
            const float want = wanted_scale();
            if (want > 0)
                scale_ = want;
            fprintf(stderr, "d3d9: render targets at %.2fx the game's size\n", scale_);
        }
        if (target && d.kind == HOST_D9_TEX_2D && scale_ != 1.0f) {
            float s = scale_;
            const uint32_t limit = props_.limits.maxImageDimension2D;
            while (s > 1.0f && (width * s > limit || height * s > limit))
                s -= 0.25f;
            t.scale = std::max(1.0f, s);
            width = (uint32_t)std::lround(width * t.scale);
            height = (uint32_t)std::lround(height * t.scale);
        }
        uint32_t max_levels = 1;
        for (uint32_t s = std::max(width, height); s > 1; s >>= 1)
            ++max_levels;
        uint32_t levels = std::max<uint32_t>(1, std::min(d.levels ? d.levels : 1, max_levels));
        t.width = width;
        t.height = height;
        t.levels = levels;
        t.layers = cube ? 6 : 1;
        t.cube = cube;
        t.depth = depth;
        VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (target && !t.info.block)
            usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (!make_image(width, height, levels, t.layers, t.info.format, usage, 1, cube, &t.image,
                        &t.memory)) {
            fprintf(stderr, "d3d9 vulkan: texture %u (%ux%u fmt %08x) was not created\n", d.id,
                    d.width, d.height, d.format);
            index_texture(d.id, &t);
            return;
        }
        t.sample_view = make_view(t, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, 0,
                                  levels, 0, t.layers, true);
        // Metal hands out zeroed textures and games rely on it; Vulkan's are undefined.
        if (!t.info.block)
            clear_image(t.image, depth, levels, t.layers);
        if (d.samples > 1 && target && d.kind == HOST_D9_TEX_2D && !t.info.block) {
            uint32_t n = d.samples >= 4 ? 4 : 2;
            const VkSampleCountFlags have = depth ? props_.limits.framebufferDepthSampleCounts
                                                  : props_.limits.framebufferColorSampleCounts;
            while (n > 1 && !(have & n))
                n /= 2;
            if (n > 1) {
                VkImageUsageFlags mu = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                              : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                if (make_image(width, height, 1, 1, t.info.format, mu, n, false, &t.msaa,
                               &t.msaa_memory)) {
                    clear_image(t.msaa, depth, 1, 1);
                    t.samples = n;
                    t.msaa_view = raw_view(t.msaa, VK_IMAGE_VIEW_TYPE_2D, t.info.format,
                                           aspect(t, false), 0, 1, 0, 1);
                }
            }
        }
        index_texture(d.id, &t);
    }

    void drop(uint32_t id) override {
        auto it = textures_.find(id);
        if (it == textures_.end())
            return;
        end_pass();
        retire_tex(it->second);
        index_texture(id, nullptr);
        textures_.erase(it);
    }

    void upload(uint32_t id, uint32_t face, uint32_t level, const uint8_t *bytes,
                uint32_t pitch) override {
        Tex *tp = tex_ptr(id);
        if (!tp || !tp->image || !bytes)
            return;
        Tex &t = *tp;
        if (level >= t.levels || t.depth)
            return;
        uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(
            (t.desc.kind == HOST_D9_TEX_CUBE ? t.desc.width : t.desc.height) >> level, 1);
        const uint8_t *src = bytes;
        uint32_t row = pitch; // bytes per source row as stored
        uint32_t bpp = t.info.bytes;
        uint32_t pw = std::max<uint32_t>(t.width >> level, 1),
                 ph = std::max<uint32_t>(t.height >> level, 1);
        std::vector<uint8_t> packed;
        if (t.info.block) {
            const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
            packed.resize((size_t)bw * bh * bpp);
            for (uint32_t y = 0; y < bh; ++y)
                memcpy(&packed[(size_t)y * bw * bpp], bytes + (size_t)y * pitch, (size_t)bw * bpp);
            copy_to_image(t, face, level, packed.data(), packed.size(), w, h);
            t.used = serial_ + 1;
            return;
        }
        if (is_dxt(t.desc.format)) {
            // No BC support: decoded here, from block rows `pitch` apart as
            // the path above copies them (the decoder reads them packed).
            const uint32_t block = t.desc.format == FMT_DXT1 ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
            const uint8_t *blocks = bytes;
            if (pitch != bw * block) {
                packed.resize((size_t)bw * bh * block);
                for (uint32_t y = 0; y < bh; ++y)
                    memcpy(&packed[(size_t)y * bw * block], bytes + (size_t)y * pitch, (size_t)bw * block);
                blocks = packed.data();
            }
            decode_dxt(blocks, w, h, t.desc.format, scratch_);
            src = scratch_.data();
            row = w * 4;
            bpp = 4;
        } else if (t.info.conv != Conv::Direct) {
            convert(t.info.conv, bytes, w, h, pitch, scratch_);
            src = scratch_.data();
            row = w * 4;
            bpp = 4;
        }
        // Tightly packed rows at the image's physical size.
        packed.resize((size_t)pw * ph * bpp);
        if (pw == w && ph == h) {
            for (uint32_t y = 0; y < h; ++y)
                memcpy(&packed[(size_t)y * w * bpp], src + (size_t)y * row, (size_t)w * bpp);
        } else {
            // The game wrote a scaled target: stretch its pixels to the target's size.
            for (uint32_t y = 0; y < ph; ++y) {
                uint32_t sy = std::min(h - 1, (uint32_t)(y * (uint64_t)h / ph));
                for (uint32_t x = 0; x < pw; ++x) {
                    uint32_t sx = std::min(w - 1, (uint32_t)(x * (uint64_t)w / pw));
                    memcpy(&packed[((size_t)y * pw + x) * bpp],
                           src + (size_t)sy * row + (size_t)sx * bpp, bpp);
                }
            }
        }
        copy_to_image(t, face, level, packed.data(), packed.size(), pw, ph);
        t.used = serial_ + 1;
    }

    bool read(uint32_t id, uint32_t face, uint32_t level, uint8_t *bytes, uint32_t pitch) override {
        Tex *tp = tex_ptr(id);
        if (!tp || !tp->image || tp->info.block || tp->depth || !tp->info.bgra8())
            return false;
        Tex &t = *tp;
        uint32_t pw = std::max<uint32_t>(t.width >> level, 1),
                 ph = std::max<uint32_t>(t.height >> level, 1);
        std::vector<uint8_t> full((size_t)pw * ph * 4);
        if (!read_image(t.image, VK_IMAGE_ASPECT_COLOR_BIT, level, face, pw, ph, 4, full.data()))
            return false;
        uint32_t w = std::max<uint32_t>(t.desc.width >> level, 1);
        uint32_t h = std::max<uint32_t>(
            (t.desc.kind == HOST_D9_TEX_CUBE ? t.desc.width : t.desc.height) >> level, 1);
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t sy = pw == w && ph == h ? y : std::min(ph - 1, (uint32_t)((y + 0.5) * ph / h));
            for (uint32_t x = 0; x < w; ++x) {
                uint32_t sx =
                    pw == w && ph == h ? x : std::min(pw - 1, (uint32_t)((x + 0.5) * pw / w));
                memcpy(bytes + (size_t)y * pitch + (size_t)x * 4, &full[((size_t)sy * pw + sx) * 4],
                       4);
            }
        }
        return true;
    }

    // ---- buffers --------------------------------------------------------
    void buffer_upload(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                       uint32_t size) override {
        Buf &b = buffers_[id];
        if (offset + size > total || !bytes)
            return;
        if (b.shadow.size() < total)
            b.shadow.resize(total, 0);
        memcpy(b.shadow.data() + offset, bytes, size);
        const uint32_t end = offset + size;
        for (SpareBuf &s : b.spares) {
            s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
            s.hi = std::max(s.hi, end);
        }
        const VkBufferUsageFlags usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            (vfetch_buffer(id) ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0);
        if (!b.buf.buffer || b.buf.size < total) {
            if (b.buf.buffer)
                retire_buffer(b.buf, b.used);
            for (SpareBuf &s : b.spares)
                retire_buffer(s.buf, s.retired);
            b.spares.clear();
            b.buf = make_host_buffer(std::max<size_t>(b.shadow.size(), 4), usage);
            if (b.buf.map)
                memcpy(b.buf.map, b.shadow.data(), b.shadow.size());
            b.used = 0;
            return;
        }
        // A buffer an unfinished frame reads is replaced rather than written;
        // see the Metal renderer for the scheme.
        poll();
        const uint64_t done = completed_;
        if (b.used > done) {
            SpareBuf retired{b.buf, b.used, offset, end};
            size_t pick = b.spares.size();
            for (size_t i = 0; i < b.spares.size(); ++i)
                if (b.spares[i].retired <= done && b.spares[i].buf.size >= total) {
                    pick = i;
                    break;
                }
            if (pick < b.spares.size()) {
                SpareBuf s = b.spares[pick];
                b.spares.erase(b.spares.begin() + (ptrdiff_t)pick);
                if (s.hi > s.lo)
                    memcpy(s.buf.map + s.lo, b.shadow.data() + s.lo, s.hi - s.lo);
                b.buf = s.buf;
            } else {
                b.buf = make_host_buffer(std::max<size_t>(b.shadow.size(), 4), usage);
                if (b.buf.map)
                    memcpy(b.buf.map, b.shadow.data(), b.shadow.size());
            }
            b.spares.push_back(retired);
            if (b.spares.size() > 4) {
                retire_buffer(b.spares.front().buf, b.spares.front().retired);
                b.spares.erase(b.spares.begin());
            }
            b.used = 0;
            return;
        }
        if (b.buf.map)
            memcpy(b.buf.map + offset, bytes, size);
    }
    void buffer_upload_inplace(uint32_t id, uint32_t total, uint32_t offset, const uint8_t *bytes,
                               uint32_t size) override {
        auto it = buffers_.find(id);
        if (it == buffers_.end() || !it->second.buf.map || it->second.buf.size < total ||
            offset + size > total || !bytes) {
            buffer_upload(id, total, offset, bytes, size);
            return;
        }
        // A dynamic vertex buffer filled batch by batch (Heroes V appends into
        // 26 MB under NOOVERWRITE), and the vertex fetch's buffers, whose map
        // changes with every part: replacing the whole buffer for every write
        // a frame already drew from cost a new allocation and a copy of
        // megabytes each time. The game writes only what no unfinished frame
        // reads (event queries tell it which: dx/d3d9.cpp). The Metal
        // renderer does the same.
        Buf &b = it->second;
        if (b.shadow.size() < total)
            b.shadow.resize(total, 0);
        memcpy(b.shadow.data() + offset, bytes, size);
        const uint32_t end = offset + size;
        for (SpareBuf &s : b.spares) {
            s.lo = s.hi > s.lo ? std::min(s.lo, offset) : offset;
            s.hi = std::max(s.hi, end);
        }
        memcpy(b.buf.map + offset, bytes, size);
    }
    void buffer_drop(uint32_t id) override {
        auto it = buffers_.find(id);
        if (it == buffers_.end())
            return;
        end_pass();
        retire_buffer(it->second.buf, it->second.used);
        for (SpareBuf &s : it->second.spares)
            retire_buffer(s.buf, s.retired);
        buffers_.erase(it);
    }

    // ---- drawing --------------------------------------------------------
    void draw(const HostD9Draw &d) override {
        ++stat_calls_;
        if (!ensure_pass(d.target)) {
            skip("no render pass");
            return;
        }
        uint64_t vs_key = d.vs_key ? d.vs_key : d9sh::code_key(d.vs, d.vs_size);
        uint64_t ps_key = d.ps_key ? d.ps_key : d9sh::code_key(d.ps, d.ps_size);
        const d9sh::Program &vp = d9sh::program_for_key(vs_key, d.vs, d.vs_size);
        const d9sh::Program &pp = d9sh::program_for_key(ps_key, d.ps, d.ps_size);
        for (const auto &binding : pp.samplers)
            if (binding.second == d9sh::S_VOLUME) {
                skip("volume samplers unsupported on this backend");
                return;
            }
        if (!vp.ok || !pp.ok) {
            skip("undecoded shader");
            const std::string why =
                std::string(vp.ok ? "pixel shader: " + pp.why : "vertex shader: " + vp.why);
            if (undecoded_.insert(why + (d.label ? d.label : "")).second)
                fprintf(stderr, "d3d9 vulkan: skipping draws: %s (%s)\n", why.c_str(),
                        d.label ? d.label : "no label");
            return;
        }
        d9msl::PixelVariant variant;
        Tex *bound[16];
        for (uint32_t s = 0; s < 16; ++s) {
            Tex *t = d.sampler_texture[s] ? tex_ptr(d.sampler_texture[s]) : nullptr;
            bound[s] = t;
            if (t && t->image && t->depth && t->desc.kind == HOST_D9_TEX_2D)
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
            fprintf(stderr, "d3d9 vulkan: a draw from the vertex fetch's buffer has an element "
                            "type it cannot decode; such draws read the buffer as it is\n");
        VkShaderModule vmod = module(vs_key, vp, variant, false, vf);
        VkShaderModule fmod = module(ps_key, pp, variant, true);
        if (!vmod || !fmod) {
            skip("untranslated shader");
            return;
        }
        count_query();
        const uint32_t *rs = d.render_state;

        // Vertex layout.
        uint64_t vkey =
            mix(mix((uint64_t)d.decl_id << 32 | d.decl_size, vs_key), fnv(d.decl, d.decl_size));
        {
            uint64_t strides = 0;
            for (int i = 0; i < 8; ++i)
                strides = strides * 1099511628211ull + d.stream[i].stride;
            vkey = mix(vkey, strides);
        }
        const VLayout &layout = vertex_layout(d, vp, vkey);

        // Pipeline.
        uint32_t topology;
        switch (d.primitive) {
        case 1:
            topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
            break;
        case 2:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
            break;
        case 3:
            topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
            break;
        case 5:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            break;
        case 4:
        case 6:
            topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            break;
        default:
            skip("primitive type");
            return;
        }
        PipeState ps{};
        ps.vmod = vmod;
        ps.fmod = fmod;
        ps.layout = &layout;
        ps.topology = topology;
        ps.blend = rs[RS_ALPHABLENDENABLE] != 0;
        ps.rs = rs;
        ps.cw[0] = rs[RS_COLORWRITEENABLE];
        ps.cw[1] = rs[RS_COLORWRITEENABLE1];
        ps.cw[2] = rs[RS_COLORWRITEENABLE2];
        ps.cw[3] = rs[RS_COLORWRITEENABLE3];
        ps.zon = rs[RS_ZENABLE] != 0 && pass_depth_view_;
        ps.stencil = rs[RS_STENCILENABLE] != 0 && pass_depth_view_;
        ps.cull = rs[RS_CULLMODE] == 2   ? VK_CULL_MODE_FRONT_BIT
                  : rs[RS_CULLMODE] == 3 ? VK_CULL_MODE_BACK_BIT
                                         : VK_CULL_MODE_NONE;
        ps.wire = wireframe_ && rs[RS_FILLMODE] == 2;
        VkPipeline pso = pipeline(ps, vkey);
        if (!pso) {
            skip("no pipeline");
            return;
        }
        VkCommandBuffer cb = cur_->cb;
        if (pso != es_.pipeline) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
            es_.pipeline = pso;
        }
        if (ps.stencil && es_.stencil_ref != (rs[RS_STENCILREF] & 0xff)) {
            es_.stencil_ref = rs[RS_STENCILREF] & 0xff;
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, es_.stencil_ref);
        }
        float bias, slope;
        memcpy(&bias, &rs[RS_DEPTHBIAS], 4);
        memcpy(&slope, &rs[RS_SLOPESCALEDEPTHBIAS], 4);
        if (!(bias == es_.bias && slope == es_.slope)) {
            // D3D's DEPTHBIAS is in depth units; Vulkan's in units of the
            // minimum resolvable difference, as Metal's are.
            vkCmdSetDepthBias(cb, bias * 16777215.0f, 0.0f, slope);
            es_.bias = bias;
            es_.slope = slope;
        }
        const float s = pass_scale_;
        {
            // Flipped, so clip space keeps Direct3D's (and Metal's) y up.
            VkViewport vpm{d.viewport[0] * s, (d.viewport[1] + d.viewport[3]) * s,
                           d.viewport[2] * s, -(float)d.viewport[3] * s,
                           d.depth_range[0],  d.depth_range[1]};
            if (vpm.height == 0.0f)
                vpm.height = -1.0f;
            if (!es_.viewport_valid || memcmp(&vpm, &es_.viewport, sizeof vpm) != 0) {
                vkCmdSetViewport(cb, 0, 1, &vpm);
                es_.viewport = vpm;
                es_.viewport_valid = true;
            }
        }
        set_scissor(rs[RS_SCISSORTESTENABLE] != 0, d.scissor);
        if (ps.blend &&
            (uses_blend_factor(rs[RS_SRCBLEND]) || uses_blend_factor(rs[RS_DESTBLEND]) ||
             uses_blend_factor(rs[RS_SRCBLENDALPHA]) || uses_blend_factor(rs[RS_DESTBLENDALPHA]))) {
            uint32_t f = rs[RS_BLENDFACTOR];
            const float c[4] = {((f >> 16) & 255) / 255.0f, ((f >> 8) & 255) / 255.0f,
                                (f & 255) / 255.0f, ((f >> 24) & 255) / 255.0f};
            vkCmdSetBlendConstants(cb, c);
        }

        // Constants and parameters, and the textures, as one descriptor set.
        D9VkVSParams vparams{};
        memcpy(vparams.bconst, d.vbool, sizeof vparams.bconst);
        // Direct3D 9 samples a pixel at its integer corner, here at its centre:
        // geometry moves half a pixel right and down to match. 63/64 of it, as
        // Wine does, so an edge never lands exactly on a pixel centre.
        vparams.halfpix[0] = d.viewport[2] ? (63.0f / 64.0f) / d.viewport[2] : 0.0f;
        vparams.halfpix[1] = d.viewport[3] ? -(63.0f / 64.0f) / d.viewport[3] : 0.0f;
        memcpy(vparams.ascale, layout.ascale, sizeof layout.ascale);
        vparams.bgra[0] = (int32_t)layout.bgra;
        SetKey sk{};
        if (vf) {
            for (uint32_t k = 0; k < (vf->check ? 5u : 4u); ++k) {
                auto b = buffers_.find(HOST_D9_VFETCH_BUFFER(k));
                if (b == buffers_.end() || !b->second.buf.buffer) {
                    skip("vertex fetch buffer missing");
                    return;
                }
                if (b->second.buf.size > props_.limits.maxStorageBufferRange) {
                    skip("vertex fetch buffer too large");
                    return;
                }
                b->second.used = serial_;
                sk.vfetch[k] = b->second.buf.buffer;
            }
            if (vf->check) {
                vparams.vtag[0] = d.vfetch_tag;
                if (vfetch_tags_.empty() || vfetch_tags_.back().first != serial_)
                    vfetch_tags_.push_back({serial_, d.vfetch_tag});
                else
                    vfetch_tags_.back().second = d.vfetch_tag;
                if (vfetch_tags_.size() > 64)
                    vfetch_tags_.erase(vfetch_tags_.begin());
            }
        }
        D9PSParams pparams{};
        memcpy(pparams.bconst, d.pbool, sizeof pparams.bconst);
        pparams.alpha_func = rs[RS_ALPHATESTENABLE] ? (int32_t)rs[RS_ALPHAFUNC] : 8;
        pparams.alpha_ref = (rs[RS_ALPHAREF] & 0xff) / 255.0f;
        if (rs[RS_FOGENABLE]) {
            uint32_t table = rs[RS_FOGTABLEMODE];
            pparams.fog_mode = table == 3 ? 2 : table == 1 ? 3 : table == 2 ? 4 : 1;
            uint32_t fc = rs[RS_FOGCOLOR];
            pparams.fog_color[0] = ((fc >> 16) & 255) / 255.0f;
            pparams.fog_color[1] = ((fc >> 8) & 255) / 255.0f;
            pparams.fog_color[2] = (fc & 255) / 255.0f;
            pparams.fog_color[3] = 1.0f;
            memcpy(&pparams.fog_start, &rs[RS_FOGSTART], 4);
            memcpy(&pparams.fog_end, &rs[RS_FOGEND], 4);
            memcpy(&pparams.fog_density, &rs[RS_FOGDENSITY], 4);
        }
        const uint32_t vn = d9glsl::constant_registers(vp), pn = d9glsl::constant_registers(pp);
        const uint64_t vsize = (uint64_t)vn * 16, psize = (uint64_t)pn * 16;
        const uint64_t a = uniform_align_;
        const uint64_t span = round_up(vsize, a) + round_up(sizeof vparams, a) +
                              round_up(psize, a) + round_up(sizeof pparams, a);
        Slice u = ring_alloc(span, a);
        uint32_t offsets[4];
        {
            uint64_t at = 0;
            offsets[0] = (uint32_t)(u.offset + at);
            fill_constants(u.data + at, d.vconst, d.vconst_count, vp, vn);
            at += round_up(vsize, a);
            offsets[1] = (uint32_t)(u.offset + at);
            memcpy(u.data + at, &vparams, sizeof vparams);
            at += round_up(sizeof vparams, a);
            offsets[2] = (uint32_t)(u.offset + at);
            fill_constants(u.data + at, d.pconst, d.pconst_count, pp, pn);
            at += round_up(psize, a);
            offsets[3] = (uint32_t)(u.offset + at);
            memcpy(u.data + at, &pparams, sizeof pparams);
        }
        if (pp.sampler_mask < 0) {
            uint32_t cubes = 0;
            for (const auto &kv : pp.samplers)
                if (kv.first < 16 && kv.second == d9sh::S_CUBE)
                    cubes |= 1u << kv.first;
            pp.cube_samplers = cubes;
            pp.sampler_mask = d9msl::pixel_sampler_mask(pp);
        }
        uint32_t used = (uint32_t)pp.sampler_mask;
        sk.uniforms = u.buffer;
        sk.ranges[0] = vsize;
        sk.ranges[1] = sizeof vparams;
        sk.ranges[2] = psize;
        sk.ranges[3] = sizeof pparams;
        for (uint32_t st = 0; st < 16; ++st) {
            const bool cube_slot =
                pp.major >= 2 ? (pp.cube_samplers >> st & 1) : (variant.cube_mask >> st & 1);
            const bool depth_slot = variant.depth_mask >> st & 1;
            VkImageView view = VK_NULL_HANDLE;
            VkSampler smp = VK_NULL_HANDLE;
            if (used >> st & 1) {
                Tex *t = bound[st];
                VkImageView tv = t ? t->sample_view : VK_NULL_HANDLE;
                if (tv && (t->cube != cube_slot || t->depth != depth_slot))
                    tv = VK_NULL_HANDLE;
                if (tv && pass_uses(t))
                    tv = VK_NULL_HANDLE; // a texture the pass writes cannot be read from
                if (tv)
                    t->used = serial_;
                view = tv ? tv : depth_slot ? placeholder_depth() : placeholder(cube_slot);
                smp = depth_slot ? shadow_sampler() : sampler(d.sampler_state + st * 14);
            } else {
                // Unused by the shader; any valid image of the declared kind will do.
                view = placeholder(false);
                smp = sampler(nullptr);
            }
            sk.views[st] = view;
            sk.samplers[st] = smp;
        }
        VkDescriptorSet set = descriptor_set(sk);
        if (!set) {
            skip("no descriptor set");
            return;
        }
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 4,
                                offsets);

        // Geometry.
        for (uint32_t i = 0; i < 8; ++i) {
            if (!(layout.streams >> i & 1))
                continue;
            const HostD9Stream &st = d.stream[i];
            VkBuffer vb = VK_NULL_HANDLE;
            VkDeviceSize off = 0;
            if (st.buffer) {
                auto b = buffers_.find(st.buffer);
                if (b == buffers_.end() || !b->second.buf.buffer) {
                    skip("missing vertex buffer");
                    return;
                }
                b->second.used = serial_;
                vb = b->second.buf.buffer;
                off = st.offset;
            } else if (d.inline_vertices) {
                Slice sl = ring_alloc(d.inline_bytes, 4);
                memcpy(sl.data, d.inline_vertices, d.inline_bytes);
                vb = sl.buffer;
                off = sl.offset;
            } else {
                vb = zero_.buffer;
            }
            if (es_.vb[i] != vb || es_.vb_offset[i] != off) {
                vkCmdBindVertexBuffers(cb, i, 1, &vb, &off);
                es_.vb[i] = vb;
                es_.vb_offset[i] = off;
            }
        }
        if (layout.zero && es_.vb[8] != zero_.buffer) {
            VkDeviceSize off = 0;
            vkCmdBindVertexBuffers(cb, 8, 1, &zero_.buffer, &off);
            es_.vb[8] = zero_.buffer;
            es_.vb_offset[8] = 0;
        }
        encode_primitives(d);
        ++draws_;
        if (probing()) {
            std::string texs;
            for (int s = 0; s < 16; ++s)
                if (d.sampler_texture[s])
                    texs += " s" + std::to_string(s) + "=" + std::to_string(d.sampler_texture[s]);
            fprintf(stderr, "probe samplers %llu:%s\n", (unsigned long long)draws_ + 1,
                    texs.c_str());
        }
        if (probing())
            fprintf(stderr,
                    "probe draw %llu: rt %u depth %u vp %d,%d %dx%d prim %u x%u start %u ib %u vs "
                    "%016llx ps %016llx "
                    "tex0 %u tex1 %u z %u/%u cull %u blend %u %s\n",
                    (unsigned long long)draws_, d.target.color[0].id, d.target.depth.id,
                    d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], d.primitive,
                    d.primitive_count, d.start, d.index_buffer, (unsigned long long)vs_key,
                    (unsigned long long)ps_key, d.sampler_texture[0], d.sampler_texture[1],
                    rs[RS_ZENABLE], rs[RS_ZWRITEENABLE], rs[RS_CULLMODE], rs[RS_ALPHABLENDENABLE],
                    d.label ? d.label : "-");
    }

    // ---- occlusion queries ----------------------------------------------
    static constexpr uint32_t kQuerySlots = 1024;
    struct Query {
        struct Slot {
            uint64_t serial;
            uint32_t index;
        };
        std::vector<Slot> slots; // not yet counted
        uint64_t counted = 0;
        bool overflow = false;
        bool ended = false;
    };
    void query_begin(uint32_t id) override {
        end_query_slot();
        queries_[id] = Query{};
        query_ = id;
    }
    void query_end(uint32_t id) override {
        if (query_ != id)
            return;
        end_query_slot();
        query_ = 0;
        queries_[id].ended = true;
    }
    bool events() const override {
        return true;
    }
    uint64_t event_mark() override {
        return cur_ ? cur_->serial : serial_;
    }
    bool event_done(uint64_t mark) override {
        poll();
        if (completed_ >= mark)
            return true;
        if (cur_ && cur_->serial <= mark)
            flush(); // D3DGETDATA_FLUSH: the frame the mark is in goes to the GPU
        return false;
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
        poll();
        if (!q.slots.empty())
            return 0; // its frame has not finished on the GPU
        *count = (uint32_t)std::min<uint64_t>(q.counted, 0xffffffffu);
        return 1;
    }
    void query_drop(uint32_t id) override {
        if (query_ == id)
            query_end(id);
        queries_.erase(id);
    }

    void probe_next(const char *tag) override {
        probe_frame_ = presents_ + 2;
        probe_tag_ = tag ? tag : "";
        fprintf(stderr, "probe %s: frame %llu\n", probe_tag_.c_str(),
                (unsigned long long)probe_frame_);
    }

    // ---- clears and blits -------------------------------------------------
    void clear(const HostD9Target &target, const int32_t vp[4], uint32_t count,
               const int32_t *rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        if (probing())
            fprintf(
                stderr,
                "probe clear: rt %u depth %u flags %x colour %08x z %g rects %u vp %d,%d %dx%d\n",
                target.color[0].id, target.depth.id, flags, color, z, count, vp[0], vp[1], vp[2],
                vp[3]);
        Tex *ct = target.color[0].id ? tex_ptr(target.color[0].id) : nullptr;
        if (!ct || !ct->image)
            return;
        Tex *dt = target.depth.id ? tex_ptr(target.depth.id) : nullptr;
        const bool has_depth = dt && dt->image;
        int32_t lw = (int32_t)std::max<uint32_t>(ct->desc.width >> target.color[0].level, 1);
        int32_t lh = (int32_t)std::max<uint32_t>(ct->desc.height >> target.color[0].level, 1);
        bool whole = count == 0 && vp[0] <= 0 && vp[1] <= 0 && vp[2] >= lw && vp[3] >= lh;
        bool want_color = flags & 1, want_depth = (flags & 2) && has_depth,
             want_stencil = (flags & 4) && has_depth;
        float rgba[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f,
                         (color & 255) / 255.0f, ((color >> 24) & 255) / 255.0f};
        if (whole) {
            end_pass();
            begin_pass(target, want_color, rgba, want_depth, z, want_stencil, stencil);
            return;
        }
        if (!ensure_pass(target))
            return;
        std::vector<int32_t> list;
        if (count && rects)
            list.assign(rects, rects + 4 * count);
        else
            list = {vp[0], vp[1], vp[0] + vp[2], vp[1] + vp[3]};
        std::vector<VkClearRect> out;
        const float s = pass_scale_;
        for (size_t i = 0; i + 4 <= list.size(); i += 4) {
            int32_t x0 = std::max(list[i], std::max(vp[0], 0));
            int32_t y0 = std::max(list[i + 1], std::max(vp[1], 0));
            int32_t x1 = std::min(list[i + 2], std::min(vp[0] + vp[2], lw));
            int32_t y1 = std::min(list[i + 3], std::min(vp[1] + vp[3], lh));
            if (x1 <= x0 || y1 <= y0)
                continue;
            int32_t px0 = std::lround(x0 * s), py0 = std::lround(y0 * s);
            int32_t px1 = std::min<int32_t>(std::lround(x1 * s), (int32_t)pass_width_);
            int32_t py1 = std::min<int32_t>(std::lround(y1 * s), (int32_t)pass_height_);
            if (px1 <= px0 || py1 <= py0)
                continue;
            VkClearRect r{};
            r.rect = {{px0, py0}, {(uint32_t)(px1 - px0), (uint32_t)(py1 - py0)}};
            r.baseArrayLayer = 0;
            r.layerCount = 1;
            out.push_back(r);
        }
        if (out.empty())
            return;
        VkClearAttachment atts[2];
        uint32_t n = 0;
        if (want_color && pass_color_[0]) {
            atts[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            atts[n].colorAttachment = 0;
            memcpy(atts[n].clearValue.color.float32, rgba, sizeof rgba);
            ++n;
        }
        if ((want_depth || want_stencil) && pass_depth_view_) {
            atts[n].aspectMask = (want_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                                 (want_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            atts[n].colorAttachment = 0;
            atts[n].clearValue.depthStencil = {z, stencil & 0xff};
            ++n;
        }
        if (n)
            vkCmdClearAttachments(cur_->cb, n, atts, (uint32_t)out.size(), out.data());
    }

    void stretch(HostD9Surface src, const int32_t sr[4], HostD9Surface dst, const int32_t dr[4],
                 uint32_t filter) override {
        if (probing())
            fprintf(stderr, "probe stretch: %u -> %u filter %u\n", src.id, dst.id, filter);
        Tex *st = tex_ptr(src.id);
        Tex *dt = tex_ptr(dst.id);
        if (!st || !dt || !st->image || !dt->image || st->depth || dt->depth || st->info.block ||
            dt->info.block)
            return;
        end_pass();
        begin_frame();
        const uint32_t sw = std::max<uint32_t>(st->width >> src.level, 1),
                       sh = std::max<uint32_t>(st->height >> src.level, 1);
        const uint32_t tw = std::max<uint32_t>(dt->width >> dst.level, 1),
                       th = std::max<uint32_t>(dt->height >> dst.level, 1);
        const uint32_t sgw = std::max<uint32_t>(st->desc.width >> src.level, 1);
        const uint32_t sgh =
            std::max<uint32_t>((st->cube ? st->desc.width : st->desc.height) >> src.level, 1);
        const uint32_t dgw = std::max<uint32_t>(dt->desc.width >> dst.level, 1);
        const uint32_t dgh =
            std::max<uint32_t>((dt->cube ? dt->desc.width : dt->desc.height) >> dst.level, 1);
        const float ssx = (float)sw / sgw, ssy = (float)sh / sgh, dsx = (float)tw / dgw,
                    dsy = (float)th / dgh;
        auto clampi = [](float v, uint32_t hi) {
            return (int32_t)std::max(0.0f, std::min((float)hi, v));
        };
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, src.level, src.face, 1};
        b.srcOffsets[0] = {clampi(sr[0] * ssx, sw), clampi(sr[1] * ssy, sh), 0};
        b.srcOffsets[1] = {clampi(sr[2] * ssx, sw), clampi(sr[3] * ssy, sh), 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dst.level, dst.face, 1};
        b.dstOffsets[0] = {clampi(dr[0] * dsx, tw), clampi(dr[1] * dsy, th), 0};
        b.dstOffsets[1] = {clampi(dr[2] * dsx, tw), clampi(dr[3] * dsy, th), 1};
        if (b.srcOffsets[0].x == b.srcOffsets[1].x || b.srcOffsets[0].y == b.srcOffsets[1].y ||
            b.dstOffsets[0].x == b.dstOffsets[1].x || b.dstOffsets[0].y == b.dstOffsets[1].y)
            return;
        st->used = serial_;
        dt->used = serial_;
        const bool linear = filter >= 2 && linear_blit(st->info.format);
        vkCmdBlitImage(cur_->cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }

    // ---- present -----------------------------------------------------------
    void present(uint32_t backbuffer, uint32_t w, uint32_t h) override {
        end_pass();
        Tex *t = tex_ptr(backbuffer);
        if (!t || !t->image) {
            flush();
            return;
        }
        base_rows_ = t->desc.height;
        const uint32_t pw = t->width, ph = t->height;
        const bool bgra = t->info.bgra8();
        if (bgra)
            keep_presented(*t, pw, ph);
        flush();
        if (probing())
            probe_dump();
        uint32_t every = host_dump_every();
        if (every && ((presents_ + 1) % every) == 0 && bgra)
            dump(*t, pw, ph);
        if (bgra) {
            if (!t->imported) {
                VkImageView v = raw_view(t->image, VK_IMAGE_VIEW_TYPE_2D, t->info.format,
                                         VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
                t->targets[0xffffffffu] = v; // freed with the texture
                gpu::TextureDesc gd;
                gd.width = (int)pw;
                gd.height = (int)ph;
                gd.format = gpu::Format::BGRA8;
                gd.usage = gpu::UsageSampled;
                t->imported = dev_->import_image(t->image, v, gd);
            }
            gpu::CommandBuffer cb = dev_->begin();
            if (host_present_stage_texture(t->imported, (int)pw, (int)ph, (int)w, (int)h, cb))
                host_present_track_command(cb);
            dev_->commit(cb);
        }
        ++presents_;
        if (presents_ % 300 == 1)
            report();
        follow_drawable();
    }

    bool read_presented(uint8_t *rgb, uint32_t cap, uint32_t *w, uint32_t *h) override {
        if (!kept_.image)
            return false;
        *w = kept_w_;
        *h = kept_h_;
        uint32_t fw = kept_w_, fh = kept_h_;
        if (!rgb || cap < fw * fh * 3)
            return false;
        std::vector<uint8_t> bgra((size_t)fw * fh * 4);
        if (!read_image(kept_.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, fw, fh, 4, bgra.data()))
            return false;
        for (size_t i = 0, n = (size_t)fw * fh; i < n; ++i) {
            rgb[i * 3] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4];
        }
        return true;
    }

  private:
    // ---- memory ------------------------------------------------------------------
    bool allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out) {
        return dev_->allocate(req, want, out);
    }
    HostBuffer make_host_buffer(uint64_t bytes, VkBufferUsageFlags usage) {
        HostBuffer b;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = std::max<uint64_t>(bytes, 16);
        bci.usage = usage;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(vk_, &bci, nullptr, &b.buffer) != VK_SUCCESS)
            return {};
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(vk_, b.buffer, &req);
        void *map = nullptr;
        if (!allocate(req,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &b.memory) ||
            vkBindBufferMemory(vk_, b.buffer, b.memory, 0) != VK_SUCCESS ||
            vkMapMemory(vk_, b.memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
            vkDestroyBuffer(vk_, b.buffer, nullptr);
            if (b.memory)
                vkFreeMemory(vk_, b.memory, nullptr);
            return {};
        }
        b.map = (uint8_t *)map;
        b.size = bci.size;
        return b;
    }
    void free_host_buffer(HostBuffer &b) {
        if (b.buffer)
            vkDestroyBuffer(vk_, b.buffer, nullptr);
        if (b.memory)
            vkFreeMemory(vk_, b.memory, nullptr);
        b = HostBuffer{};
    }
    bool make_image(uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, VkFormat fmt,
                    VkImageUsageFlags usage, uint32_t samples, bool cube, VkImage *image,
                    VkDeviceMemory *memory) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = fmt;
        ici.extent = {w, h, 1};
        ici.mipLevels = levels;
        ici.arrayLayers = layers;
        ici.samples = (VkSampleCountFlagBits)samples;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = usage;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(vk_, &ici, nullptr, image) != VK_SUCCESS) {
            *image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(vk_, *image, &req);
        if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, memory) ||
            vkBindImageMemory(vk_, *image, *memory, 0) != VK_SUCCESS) {
            vkDestroyImage(vk_, *image, nullptr);
            *image = VK_NULL_HANDLE;
            if (*memory)
                vkFreeMemory(vk_, *memory, nullptr);
            *memory = VK_NULL_HANDLE;
            return false;
        }
        // Into GENERAL once, in this frame, before anything else touches it.
        begin_frame();
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = *image;
        const bool depth = fmt == depth_format_;
        b.subresourceRange = {
            depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                  : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
            0, levels, 0, layers};
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cur_->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        return true;
    }
    void clear_image(VkImage image, bool depth, uint32_t levels, uint32_t layers) {
        begin_frame();
        if (depth) {
            VkClearDepthStencilValue v{1.0f, 0};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0,
                                      levels, 0, layers};
            vkCmdClearDepthStencilImage(cur_->cb, image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        } else {
            VkClearColorValue v{};
            VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
            vkCmdClearColorImage(cur_->cb, image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        }
    }
    VkImageAspectFlags aspect(const Tex &t, bool sampling) const {
        if (!t.depth)
            return VK_IMAGE_ASPECT_COLOR_BIT;
        return sampling ? VK_IMAGE_ASPECT_DEPTH_BIT
                        : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    VkImageView raw_view(VkImage image, VkImageViewType type, VkFormat fmt, VkImageAspectFlags asp,
                         uint32_t level, uint32_t levels, uint32_t layer, uint32_t layers,
                         bool alpha_only = false) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = image;
        vci.viewType = type;
        vci.format = fmt;
        if (alpha_only)
            vci.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                              VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
        vci.subresourceRange = {asp, level, levels, layer, layers};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(vk_, &vci, nullptr, &v) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return v;
    }
    VkImageView make_view(const Tex &t, VkImageViewType type, uint32_t level, uint32_t levels,
                          uint32_t layer, uint32_t layers, bool sampling) {
        return raw_view(t.image, type, t.info.format, aspect(t, sampling), level, levels, layer,
                        layers, sampling && t.info.alpha_only);
    }
    VkImageView target_view(Tex &t, uint32_t level, uint32_t face) {
        uint32_t key = level << 8 | face;
        auto it = t.targets.find(key);
        if (it != t.targets.end())
            return it->second;
        VkImageView v = make_view(t, VK_IMAGE_VIEW_TYPE_2D, level, 1, face, 1, false);
        t.targets[key] = v;
        return v;
    }

    VkFmt vk_format_for(uint32_t fmt) const {
        const FormatInfo f = d9gpu::format_info(fmt, bc_);
        VkFmt m;
        m.conv = f.conv;
        m.bytes = f.bytes;
        m.block = f.block;
        switch (f.store) {
        case Store::BGRA8:
            m.format = VK_FORMAT_B8G8R8A8_UNORM;
            break;
        case Store::A8:
            m.format = VK_FORMAT_R8_UNORM;
            m.alpha_only = true;
            break;
        case Store::RGBA16:
            m.format = VK_FORMAT_R16G16B16A16_UNORM;
            break;
        case Store::R16F:
            m.format = VK_FORMAT_R16_SFLOAT;
            break;
        case Store::RGBA16F:
            m.format = VK_FORMAT_R16G16B16A16_SFLOAT;
            break;
        case Store::R32F:
            m.format = VK_FORMAT_R32_SFLOAT;
            break;
        case Store::RGBA32F:
            m.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            break;
        case Store::BC1:
            m.format = VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            break;
        case Store::BC2:
            m.format = VK_FORMAT_BC2_UNORM_BLOCK;
            break;
        case Store::BC3:
            m.format = VK_FORMAT_BC3_UNORM_BLOCK;
            break;
        case Store::Depth:
            m.format = depth_format_;
            m.depth = true;
            break;
        }
        if (!f.block && is_dxt(fmt)) {
            // Decoded to BGRA8 on upload.
            m.format = VK_FORMAT_B8G8R8A8_UNORM;
            m.conv = Conv::Direct;
        }
        return m;
    }
    bool linear_blit(VkFormat fmt) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(dev_->native_physical(), fmt, &fp);
        return fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    }

    // Record a copy of tightly packed bytes into one face and level.
    void copy_to_image(Tex &t, uint32_t face, uint32_t level, const uint8_t *bytes, size_t size,
                       uint32_t w, uint32_t h) {
        end_pass();
        begin_frame();
        Slice s = ring_alloc(size, 16);
        memcpy(s.data, bytes, size);
        VkBufferImageCopy c{};
        c.bufferOffset = s.offset;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
        c.imageExtent = {w, h, 1};
        if (t.info.block) {
            c.imageExtent.width = std::max<uint32_t>(std::max<uint32_t>(t.width >> level, 1), 1);
            c.imageExtent.height = std::max<uint32_t>(std::max<uint32_t>(t.height >> level, 1), 1);
        }
        vkCmdCopyBufferToImage(cur_->cb, s.buffer, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }

    // Waits for everything recorded, then copies one face and level out.
    bool read_image(VkImage image, VkImageAspectFlags asp, uint32_t level, uint32_t face,
                    uint32_t w, uint32_t h, uint32_t bpp, uint8_t *out) {
        end_pass();
        flush();
        wait_all();
        const uint64_t bytes = (uint64_t)w * h * bpp;
        if (readback_.size < bytes) {
            free_host_buffer(readback_);
            readback_ = make_host_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            if (!readback_.buffer)
                return false;
        }
        vkResetCommandBuffer(oneshot_cb_, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(oneshot_cb_, &bi);
        VkBufferImageCopy c{};
        c.imageSubresource = {asp, level, face, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(oneshot_cb_, image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer, 1,
                               &c);
        vkEndCommandBuffer(oneshot_cb_);
        vkResetFences(vk_, 1, &oneshot_fence_);
        if (!dev_->submit_native(oneshot_cb_, oneshot_fence_))
            return false;
        vkWaitForFences(vk_, 1, &oneshot_fence_, VK_TRUE, UINT64_MAX);
        memcpy(out, readback_.map, bytes);
        return true;
    }

    // ---- deferred destruction ----------------------------------------------------
    struct Grave {
        uint64_t after = 0; // destroyed once this frame has completed
        std::function<void()> free;
    };
    void bury(uint64_t after, std::function<void()> free) {
        graves_.push_back({after, std::move(free)});
    }
    void retire_tex(Tex &t) {
        if (t.imported) {
            dev_->destroy(t.imported);
            t.imported = {};
        }
        if (!t.image && !t.msaa)
            return;
        std::vector<VkImageView> views;
        for (auto &kv : t.targets)
            views.push_back(kv.second);
        if (t.sample_view)
            views.push_back(t.sample_view);
        if (t.msaa_view)
            views.push_back(t.msaa_view);
        // A presented image may still be on the presenter's screen for a few
        // frames after the game lets it go.
        const uint64_t after = serial_ + 8;
        VkImage image = t.image, msaa = t.msaa;
        VkDeviceMemory mem = t.memory, msaa_mem = t.msaa_memory;
        VkDevice vk = vk_;
        bury(after, [vk, views, image, msaa, mem, msaa_mem] {
            for (VkImageView v : views)
                vkDestroyImageView(vk, v, nullptr);
            if (image)
                vkDestroyImage(vk, image, nullptr);
            if (msaa)
                vkDestroyImage(vk, msaa, nullptr);
            if (mem)
                vkFreeMemory(vk, mem, nullptr);
            if (msaa_mem)
                vkFreeMemory(vk, msaa_mem, nullptr);
        });
        t = Tex{};
    }
    void retire_buffer(HostBuffer &b, uint64_t used) {
        if (!b.buffer)
            return;
        HostBuffer copy = b;
        b = HostBuffer{};
        bury(std::max(used, serial_), [this, copy]() mutable { free_host_buffer(copy); });
    }

    // ---- frames ------------------------------------------------------------------
    Frame *new_frame() {
        auto f = std::make_unique<Frame>();
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.queueFamilyIndex = dev_->native_queue_family();
        if (vkCreateCommandPool(vk_, &pci, nullptr, &f->pool) != VK_SUCCESS)
            return nullptr;
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(vk_, &ai, &f->cb) != VK_SUCCESS ||
            vkCreateFence(vk_, &fci, nullptr, &f->fence) != VK_SUCCESS)
            return nullptr;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_OCCLUSION;
        qci.queryCount = kQuerySlots;
        vkCreateQueryPool(vk_, &qci, nullptr, &f->queries);
        all_frames_.push_back(std::move(f));
        return all_frames_.back().get();
    }
    void begin_frame() {
        if (cur_)
            return;
        poll();
        // At most three frames in flight, as with Metal.
        const double t0 = now();
        while (in_flight_.size() >= 3)
            wait_oldest();
        gpu_wait_s_ += now() - t0;
        Frame *f = nullptr;
        if (!free_frames_.empty()) {
            f = free_frames_.back();
            free_frames_.pop_back();
        } else {
            f = new_frame();
        }
        if (!f)
            return;
        vkResetCommandPool(vk_, f->pool, 0);
        for (size_t i = 0; i < f->pool_used && i < f->pools.size(); ++i)
            vkResetDescriptorPool(vk_, f->pools[i], 0);
        f->pool_used = 0;
        f->sets.clear();
        f->ring.chunk = 0;
        f->ring.used = 0;
        f->queries_used = 0;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f->cb, &bi);
        if (f->queries)
            vkCmdResetQueryPool(f->cb, f->queries, 0, kQuerySlots);
        f->serial = ++serial_;
        cur_ = f;
        es_ = EncoderState{};
    }
    void flush() {
        if (!cur_)
            return;
        end_pass();
        if (!vfetch_tags_.empty() && vfetch_tags_.back().first == cur_->serial) {
            // What the vertex fetch check wrote, for vfetch_check to read once the fence says so.
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(cur_->cb, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                                 VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        }
        vkEndCommandBuffer(cur_->cb);
        vkResetFences(vk_, 1, &cur_->fence);
        cur_->submitted_at = now();
        if (!dev_->submit_native(cur_->cb, cur_->fence)) {
            fprintf(stderr, "d3d9 vulkan: a frame could not be submitted\n");
            free_frames_.push_back(cur_);
            completed_ = cur_->serial;
            cur_ = nullptr;
            return;
        }
        in_flight_.push_back(cur_);
        cur_ = nullptr;
    }
    // Retires every frame the GPU has finished, oldest first.
    void poll() {
        while (!in_flight_.empty() &&
               vkGetFenceStatus(vk_, in_flight_.front()->fence) == VK_SUCCESS)
            retire_frame();
        collect();
    }
    void wait_oldest() {
        if (in_flight_.empty())
            return;
        vkWaitForFences(vk_, 1, &in_flight_.front()->fence, VK_TRUE, UINT64_MAX);
        retire_frame();
        collect();
    }
    void wait_all() {
        const double t0 = now();
        while (!in_flight_.empty())
            wait_oldest();
        gpu_wait_s_ += now() - t0;
    }
    void retire_frame() {
        Frame *f = in_flight_.front();
        in_flight_.pop_front();
        gpu_s_ += now() - f->submitted_at;
        // The frame's query slots are read before its pool is reused.
        for (auto &kv : queries_) {
            Query &q = kv.second;
            size_t keep = 0;
            for (auto &s : q.slots) {
                if (s.serial == f->serial) {
                    uint64_t v = 0;
                    if (vkGetQueryPoolResults(vk_, f->queries, s.index, 1, sizeof v, &v, sizeof v,
                                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                        q.counted += v;
                } else {
                    q.slots[keep++] = s;
                }
            }
            q.slots.resize(keep);
        }
        completed_ = f->serial;
        free_frames_.push_back(f);
    }
    void collect() {
        size_t keep = 0;
        for (size_t i = 0; i < graves_.size(); ++i) {
            Grave &g = graves_[i];
            if (g.after <= completed_ && (cur_ == nullptr || g.after < cur_->serial)) {
                g.free();
            } else {
                if (keep != i)
                    graves_[keep] = std::move(g);
                ++keep;
            }
        }
        graves_.resize(keep);
    }

    static uint64_t round_up(uint64_t v, uint64_t a) {
        return (v + a - 1) / a * a;
    }
    Slice ring_alloc(uint64_t size, uint64_t align) {
        begin_frame();
        Ring &r = cur_->ring;
        for (;;) {
            if (r.chunk < r.chunks.size()) {
                HostBuffer &c = r.chunks[r.chunk];
                uint64_t at = round_up(r.used, align);
                if (at + size <= c.size) {
                    r.used = at + size;
                    return {c.buffer, at, c.map + at};
                }
                ++r.chunk;
                r.used = 0;
                continue;
            }
            const uint64_t want = std::max<uint64_t>(8u << 20, round_up(size, 1 << 20));
            HostBuffer c = make_host_buffer(
                want, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
            if (!c.buffer) {
                fprintf(stderr, "d3d9 vulkan: out of memory for frame data\n");
                abort();
            }
            // A new chunk goes after the ones in use, so they keep their order.
            r.chunks.insert(r.chunks.begin() + (ptrdiff_t)r.chunk, c);
            r.used = 0;
        }
    }

    void fill_constants(uint8_t *out, const float *c, uint32_t count, const d9sh::Program &p,
                        uint32_t n) {
        memset(out, 0, (size_t)n * 16);
        if (c && count)
            memcpy(out, c, (size_t)std::min(count, n) * 16);
        for (const auto &def : p.defs)
            if (def.first < n)
                memcpy(out + (size_t)def.first * 16, def.second.data(), 16);
    }

    // ---- passes ------------------------------------------------------------------
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
    bool pass_uses(const Tex *t) const {
        for (int i = 0; i < 4; ++i)
            if (pass_color_[i] == t)
                return true;
        return pass_depth_tex_ == t;
    }

    bool same_target(const HostD9Target &t) const {
        return in_pass_ && memcmp(&t, &pass_target_, sizeof t) == 0;
    }
    bool ensure_pass(const HostD9Target &t) {
        if (same_target(t))
            return true;
        end_pass();
        return begin_pass(t, false, nullptr, false, 0, false, 0);
    }
    bool begin_pass(const HostD9Target &t, bool clear_color, const float *rgba, bool clear_depth,
                    float z, bool clear_stencil, uint32_t stencil) {
        begin_frame();
        if (!cur_)
            return false;
        VkRenderingAttachmentInfo colors[4];
        uint32_t ncolor = 0;
        pass_samples_ = 0;
        for (int i = 0; i < 4; ++i) {
            pass_color_[i] = nullptr;
            pass_formats_[i] = VK_FORMAT_UNDEFINED;
            colors[i] = VkRenderingAttachmentInfo{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            colors[i].imageView = VK_NULL_HANDLE;
            Tex *ct = t.color[i].id ? tex_ptr(t.color[i].id) : nullptr;
            if (!ct || !ct->image || ct->depth || ct->info.block)
                continue;
            const bool ms = ct->msaa && t.color[i].level == 0 && t.color[i].face == 0;
            const uint32_t samples = ms ? ct->samples : 1;
            if (pass_samples_ && samples != pass_samples_)
                continue;
            if (i > 0 && pass_color_[0] &&
                (std::max<uint32_t>(ct->width >> t.color[i].level, 1) < pass_width_ ||
                 std::max<uint32_t>(ct->height >> t.color[i].level, 1) < pass_height_))
                continue;
            if (i == 0) {
                pass_width_ = std::max<uint32_t>(ct->width >> t.color[0].level, 1);
                pass_height_ = std::max<uint32_t>(ct->height >> t.color[0].level, 1);
            } else if (!pass_color_[0]) {
                continue;
            }
            pass_samples_ = samples;
            pass_color_[i] = ct;
            pass_formats_[i] = ct->info.format;
            ct->used = serial_;
            VkRenderingAttachmentInfo &a = colors[i];
            if (ms) {
                a.imageView = ct->msaa_view;
                a.resolveImageView = target_view(*ct, 0, 0);
                a.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
                a.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            } else {
                a.imageView = target_view(*ct, t.color[i].level, t.color[i].face);
            }
            a.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (i == 0 && clear_color) {
                a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                memcpy(a.clearValue.color.float32, rgba, 4 * sizeof(float));
            } else {
                a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            }
            ncolor = (uint32_t)i + 1;
        }
        if (!pass_color_[0])
            return false;
        if (!pass_samples_)
            pass_samples_ = 1;
        pass_depth_view_ = VK_NULL_HANDLE;
        pass_depth_tex_ = nullptr;
        Tex *dt = t.depth.id ? tex_ptr(t.depth.id) : nullptr;
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (dt && dt->image && dt->depth) {
            VkImageView dv = VK_NULL_HANDLE;
            uint32_t dw = dt->width, dh = dt->height;
            if (pass_samples_ > 1) {
                if (dt->msaa && dt->samples == pass_samples_)
                    dv = dt->msaa_view;
                else
                    dv = fitted_depth(dt, pass_width_, pass_height_, pass_samples_, &dw, &dh);
            } else {
                dv = target_view(*dt, 0, 0);
            }
            // Vulkan takes a larger depth buffer; a smaller one gets one of its own.
            if (dv && (dw < pass_width_ || dh < pass_height_))
                dv = fitted_depth(dt, pass_width_, pass_height_, pass_samples_, &dw, &dh);
            if (dv) {
                pass_depth_view_ = dv;
                pass_depth_tex_ = dt;
                dt->used = serial_;
                depth.imageView = dv;
                depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                depth.loadOp =
                    clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
                depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                depth.clearValue.depthStencil = {z, stencil & 0xff};
            }
        }
        VkRenderingAttachmentInfo stencil_att = depth;
        stencil_att.loadOp =
            clear_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {pass_width_, pass_height_}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = ncolor;
        ri.pColorAttachments = colors;
        if (pass_depth_view_) {
            ri.pDepthAttachment = &depth;
            ri.pStencilAttachment = &stencil_att;
        }
        pass_color_count_ = ncolor;
        vkCmdBeginRendering(cur_->cb, &ri);
        in_pass_ = true;
        pass_slot_ = -1;
        es_ = EncoderState{};
        pass_target_ = t;
        pass_scale_ = pass_color_[0]->scale;
        return true;
    }
    void end_pass() {
        if (!in_pass_)
            return;
        end_query_slot();
        vkCmdEndRendering(cur_->cb);
        in_pass_ = false;
        memset(&pass_target_, 0, sizeof pass_target_);
        for (auto &c : pass_color_)
            c = nullptr;
        pass_depth_tex_ = nullptr;
        pass_depth_view_ = VK_NULL_HANDLE;
    }
    // A depth buffer of the pass's size and sample count, for a pass whose
    // depth buffer does not match.
    VkImageView fitted_depth(Tex *dt, uint32_t w, uint32_t h, uint32_t samples, uint32_t *ow,
                             uint32_t *oh) {
        uint64_t key = mix(mix(mix((uint64_t)dt->desc.id, w), h), samples);
        auto it = fitted_depth_.find(key);
        if (it == fitted_depth_.end()) {
            Fitted f;
            if (!make_image(w, h, 1, 1, depth_format_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            samples, false, &f.image, &f.memory))
                return VK_NULL_HANDLE;
            f.view = raw_view(f.image, VK_IMAGE_VIEW_TYPE_2D, depth_format_,
                              VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1);
            it = fitted_depth_.emplace(key, f).first;
        }
        *ow = w;
        *oh = h;
        return it->second.view;
    }

    void set_scissor(bool on, const int32_t sc[4]) {
        VkRect2D r{{0, 0}, {pass_width_, pass_height_}};
        if (on) {
            const float s = pass_scale_;
            int32_t x0 = std::max(0, (int32_t)std::lround(sc[0] * s)),
                    y0 = std::max(0, (int32_t)std::lround(sc[1] * s));
            int32_t x1 = std::min((int32_t)pass_width_, (int32_t)std::lround(sc[2] * s));
            int32_t y1 = std::min((int32_t)pass_height_, (int32_t)std::lround(sc[3] * s));
            if (x1 <= x0 || y1 <= y0)
                x0 = y0 = x1 = y1 = 0;
            r = {{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        }
        if (!es_.scissor_valid || memcmp(&r, &es_.scissor, sizeof r) != 0) {
            vkCmdSetScissor(cur_->cb, 0, 1, &r);
            es_.scissor = r;
            es_.scissor_valid = true;
        }
    }

    void count_query() {
        if (!query_ || pass_slot_ >= 0 || !in_pass_)
            return;
        Query &q = queries_[query_];
        if (!cur_->queries || cur_->queries_used >= kQuerySlots) {
            q.overflow = true;
            return;
        }
        pass_slot_ = (int)cur_->queries_used++;
        vkCmdBeginQuery(cur_->cb, cur_->queries, (uint32_t)pass_slot_,
                        precise_ ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
        q.slots.push_back({serial_, (uint32_t)pass_slot_});
    }
    void end_query_slot() {
        if (pass_slot_ < 0)
            return;
        vkCmdEndQuery(cur_->cb, cur_->queries, (uint32_t)pass_slot_);
        pass_slot_ = -1;
    }

    // ---- shaders and pipelines ---------------------------------------------------
    // Stream 0's elements feeding the shader's inputs, for the vertex fetch; null when one
    // of them has a type it cannot decode.
    const d9msl::VFetchLayout *vfetch_layout(const HostD9Draw &d, const d9sh::Program &vp,
                                             d9msl::VFetchLayout *out) {
        out->words = (uint8_t)std::min<uint32_t>(d.stream[0].stride / 4, 64);
        out->check = d.vfetch == 2;
        if (out->check && !vertex_stores_ && vfetch_unwritable_++ == 0)
            fprintf(stderr, "d3d9 vulkan: the device cannot write from vertex shaders; the vertex "
                            "fetch runs unchecked\n");
        out->check = out->check && vertex_stores_;
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

    VkShaderModule module(uint64_t code_key, const d9sh::Program &p, const d9msl::PixelVariant &v,
                          bool pixel, const d9msl::VFetchLayout *vf = nullptr) {
        uint64_t key = mix(code_key, pixel ? v.key() + 1 : vf ? vf->key() : 0);
        auto it = modules_.find(key);
        if (it != modules_.end())
            return it->second;
        std::string src, why;
        bool ok = pixel ? d9glsl::pixel_source(p, v, &src, &why)
                  : vf  ? d9glsl::vertex_source(p, *vf, host_d9_vfetch_prelude(), &src, &why)
                        : d9glsl::vertex_source(p, &src, &why);
        VkShaderModule m = VK_NULL_HANDLE;
        if (ok) {
            if (const char *dir = recomp_env("D3D9_SHADER_DUMP")) {
                char path[1024];
                snprintf(path, sizeof path, "%s/%016llx_%016llx.%s", dir,
                         (unsigned long long)code_key,
                         (unsigned long long)(vf ? vf->key() : v.key()), pixel ? "frag" : "vert");
                if (FILE *f = fopen(path, "w")) {
                    fputs(src.c_str(), f);
                    fclose(f);
                }
            }
            std::string error;
            std::vector<uint32_t> spirv = compile_glsl(src, pixel, &error);
            if (spirv.empty()) {
                fprintf(stderr, "d3d9 vulkan: a %s shader failed to compile: %s\n",
                        pixel ? "pixel" : "vertex", error.c_str());
            } else {
                VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                ci.codeSize = spirv.size() * sizeof(uint32_t);
                ci.pCode = spirv.data();
                if (vkCreateShaderModule(vk_, &ci, nullptr, &m) != VK_SUCCESS)
                    m = VK_NULL_HANDLE;
            }
        } else {
            char key_text[64];
            snprintf(key_text, sizeof key_text, "d3d9.vulkan.untranslated.%llx",
                     (unsigned long long)key);
            log_once(key_text, "d3d9 vulkan: a %s shader is not translated: %s",
                     pixel ? "pixel" : "vertex", why.c_str());
        }
        modules_[key] = m;
        return m;
    }

    struct VLayout {
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        float ascale[16][4] = {};
        uint32_t bgra = 0;
        uint32_t streams = 0;
        bool zero = false;
    };
    const VLayout &vertex_layout(const HostD9Draw &d, const d9sh::Program &vp, uint64_t key) {
        auto it = layouts_.find(key);
        if (it != layouts_.end())
            return it->second;
        VLayout v;
        uint32_t stream_end[8] = {};
        std::set<uint32_t> regs;
        for (const d9sh::DclIn &in : vp.inputs) {
            if (in.type != d9sh::R_INPUT)
                continue;
            uint32_t reg = in.index & 15;
            if (!regs.insert(reg).second)
                continue;
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
                VkFormat fmt;
                float scale[4];
                bool swap;
                if (!vertex_format(type, bgra_vertex_, &fmt, scale, &swap))
                    continue;
                v.attributes.push_back({reg, stream, fmt, offset});
                memcpy(v.ascale[reg], scale, sizeof scale);
                if (swap)
                    v.bgra |= 1u << reg;
                v.streams |= 1u << stream;
                stream_end[stream] =
                    std::max<uint32_t>(stream_end[stream], offset + vertex_format_bytes(type));
                found = true;
                break;
            }
            if (!found) {
                v.attributes.push_back({reg, 8, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
                v.zero = true;
            }
        }
        for (uint32_t s = 0; s < 8; ++s) {
            if (!(v.streams >> s & 1))
                continue;
            uint32_t stride = d.stream[s].stride ? d.stream[s].stride : stream_end[s];
            v.bindings.push_back({s, std::max(stride, stream_end[s]), VK_VERTEX_INPUT_RATE_VERTEX});
        }
        if (v.zero)
            v.bindings.push_back({8, 16, VK_VERTEX_INPUT_RATE_INSTANCE});
        for (int r = 0; r < 16; ++r)
            for (int k = 0; k < 4; ++k)
                if (v.ascale[r][k] == 0.0f)
                    v.ascale[r][k] = 1.0f;
        return layouts_.emplace(key, std::move(v)).first->second;
    }

    struct PipeState {
        VkShaderModule vmod, fmod;
        const VLayout *layout;
        uint32_t topology;
        bool blend;
        const uint32_t *rs;
        uint32_t cw[4];
        bool zon, stencil, wire;
        VkCullModeFlags cull;
    };
    VkPipeline pipeline(const PipeState &p, uint64_t vkey) {
        const uint32_t *rs = p.rs;
        uint64_t key = mix(mix((uint64_t)p.vmod, (uint64_t)p.fmod), vkey);
        key = mix(key, (uint64_t)p.topology | (uint64_t)p.cull << 8 | (uint64_t)p.wire << 12 |
                           (uint64_t)pass_samples_ << 16 | (uint64_t)pass_color_count_ << 24);
        for (int i = 0; i < 4; ++i)
            key = mix(key, (uint64_t)pass_formats_[i] + 1000 * (uint64_t)i);
        key = mix(key, pass_depth_view_ ? depth_format_ : 0);
        uint64_t bkey =
            p.blend ? (1ull | (uint64_t)rs[RS_SRCBLEND] << 1 | (uint64_t)rs[RS_DESTBLEND] << 5 |
                       (uint64_t)rs[RS_BLENDOP] << 9 |
                       (uint64_t)(rs[RS_SEPARATEALPHABLENDENABLE] != 0) << 13 |
                       (uint64_t)rs[RS_SRCBLENDALPHA] << 14 |
                       (uint64_t)rs[RS_DESTBLENDALPHA] << 18 | (uint64_t)rs[RS_BLENDOPALPHA] << 22)
                    : 0;
        key = mix(key, bkey);
        key = mix(key, (uint64_t)p.cw[0] | (uint64_t)p.cw[1] << 4 | (uint64_t)p.cw[2] << 8 |
                           (uint64_t)p.cw[3] << 12);
        uint64_t dkey = (uint64_t)p.zon | (uint64_t)(rs[RS_ZWRITEENABLE] != 0) << 1 |
                        (uint64_t)rs[RS_ZFUNC] << 2 | (uint64_t)p.stencil << 6;
        if (p.stencil)
            dkey = mix(dkey, fnv(&rs[RS_STENCILFAIL], 7 * sizeof(uint32_t)) ^
                                 ((uint64_t)rs[RS_TWOSIDEDSTENCILMODE] << 1) ^
                                 fnv(&rs[RS_CCW_STENCILFAIL], 4 * sizeof(uint32_t)));
        key = mix(key, dkey);
        if (key == last_pipeline_key_ && last_pipeline_)
            return last_pipeline_;
        auto it = pipelines_.find(key);
        if (it != pipelines_.end()) {
            last_pipeline_key_ = key;
            last_pipeline_ = it->second;
            return it->second;
        }
        VkPipelineShaderStageCreateInfo stages[2];
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = p.vmod;
        stages[0].pName = "main";
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = p.fmod;
        stages[1].pName = "main";
        VkPipelineVertexInputStateCreateInfo vi{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = (uint32_t)p.layout->bindings.size();
        vi.pVertexBindingDescriptions = p.layout->bindings.data();
        vi.vertexAttributeDescriptionCount = (uint32_t)p.layout->attributes.size();
        vi.pVertexAttributeDescriptions = p.layout->attributes.data();
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = (VkPrimitiveTopology)p.topology;
        VkPipelineViewportStateCreateInfo vs{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vs.viewportCount = 1;
        vs.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rast{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rast.polygonMode = p.wire ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rast.cullMode = p.cull;
        rast.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rast.depthBiasEnable = VK_TRUE;
        rast.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = (VkSampleCountFlagBits)std::max<uint32_t>(pass_samples_, 1);
        VkPipelineDepthStencilStateCreateInfo ds{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        ds.depthTestEnable = p.zon;
        ds.depthWriteEnable = p.zon && rs[RS_ZWRITEENABLE] != 0;
        ds.depthCompareOp = p.zon ? compare_op(rs[RS_ZFUNC]) : VK_COMPARE_OP_ALWAYS;
        ds.stencilTestEnable = p.stencil;
        if (p.stencil) {
            ds.front.failOp = stencil_op(rs[RS_STENCILFAIL]);
            ds.front.depthFailOp = stencil_op(rs[RS_STENCILZFAIL]);
            ds.front.passOp = stencil_op(rs[RS_STENCILPASS]);
            ds.front.compareOp = compare_op(rs[RS_STENCILFUNC]);
            ds.front.compareMask = rs[RS_STENCILMASK] & 0xff;
            ds.front.writeMask = rs[RS_STENCILWRITEMASK] & 0xff;
            ds.back = ds.front;
            if (rs[RS_TWOSIDEDSTENCILMODE]) {
                ds.back.failOp = stencil_op(rs[RS_CCW_STENCILFAIL]);
                ds.back.depthFailOp = stencil_op(rs[RS_CCW_STENCILZFAIL]);
                ds.back.passOp = stencil_op(rs[RS_CCW_STENCILPASS]);
                ds.back.compareOp = compare_op(rs[RS_CCW_STENCILFUNC]);
            }
        }
        VkPipelineColorBlendAttachmentState atts[4]{};
        for (uint32_t i = 0; i < pass_color_count_; ++i) {
            VkPipelineColorBlendAttachmentState &a = atts[i];
            uint32_t cw = independent_blend_ ? p.cw[i] : p.cw[0];
            a.colorWriteMask = cw & 0xf; // D3D's RED, GREEN, BLUE, ALPHA bits are Vulkan's
            if (p.blend) {
                a.blendEnable = VK_TRUE;
                a.srcColorBlendFactor = blend_factor(rs[RS_SRCBLEND], false);
                a.dstColorBlendFactor = blend_factor(rs[RS_DESTBLEND], false);
                if (rs[RS_SRCBLEND] == 12)
                    a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                if (rs[RS_SRCBLEND] == 13)
                    a.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                a.colorBlendOp = blend_op(rs[RS_BLENDOP]);
                if (rs[RS_SEPARATEALPHABLENDENABLE]) {
                    a.srcAlphaBlendFactor = blend_factor(rs[RS_SRCBLENDALPHA], true);
                    a.dstAlphaBlendFactor = blend_factor(rs[RS_DESTBLENDALPHA], true);
                    a.alphaBlendOp = blend_op(rs[RS_BLENDOPALPHA]);
                } else {
                    a.srcAlphaBlendFactor = blend_factor(rs[RS_SRCBLEND], true);
                    a.dstAlphaBlendFactor = blend_factor(rs[RS_DESTBLEND], true);
                    if (rs[RS_SRCBLEND] == 12)
                        a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                    if (rs[RS_SRCBLEND] == 13)
                        a.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                    a.alphaBlendOp = blend_op(rs[RS_BLENDOP]);
                }
            }
        }
        VkPipelineColorBlendStateCreateInfo cb{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = pass_color_count_;
        cb.pAttachments = atts;
        static const VkDynamicState dyn[] = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
        dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        pr.colorAttachmentCount = pass_color_count_;
        pr.pColorAttachmentFormats = pass_formats_;
        if (pass_depth_view_) {
            pr.depthAttachmentFormat = depth_format_;
            pr.stencilAttachmentFormat = depth_format_;
        }
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &pr;
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vs;
        gp.pRasterizationState = &rast;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &ds;
        gp.pColorBlendState = &cb;
        gp.pDynamicState = &dy;
        gp.layout = layout_;
        VkPipeline pso = VK_NULL_HANDLE;
        if (vkCreateGraphicsPipelines(vk_, cache_, 1, &gp, nullptr, &pso) != VK_SUCCESS) {
            fprintf(stderr, "d3d9 vulkan: pipeline failed\n");
            pso = VK_NULL_HANDLE;
        }
        pipelines_[key] = pso;
        last_pipeline_key_ = key;
        last_pipeline_ = pso;
        return pso;
    }

    bool init_layout() {
        VkDescriptorSetLayoutBinding b[4 + 16 + 5];
        for (uint32_t i = 0; i < 4 + 16 + 5; ++i) {
            b[i] = {};
            b[i].binding = i;
            b[i].descriptorCount = 1;
            if (i < 4) {
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                b[i].stageFlags = i < 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
            } else if (i < d9glsl::kVFetchBinding) {
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            } else {
                // Left unwritten by every draw but a vertex fetch's, whose shaders alone use them.
                b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
            }
        }
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4 + 16 + (vfetch_ ? 5 : 0);
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(vk_, &lci, nullptr, &set_layout_) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pci.setLayoutCount = 1;
        pci.pSetLayouts = &set_layout_;
        if (vkCreatePipelineLayout(vk_, &pci, nullptr, &layout_) != VK_SUCCESS)
            return false;
        VkPipelineCacheCreateInfo cci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        vkCreatePipelineCache(vk_, &cci, nullptr, &cache_);
        return true;
    }

    struct SetKey {
        VkBuffer uniforms;
        uint64_t ranges[4];
        VkImageView views[16];
        VkSampler samplers[16];
        VkBuffer vfetch[5]; // HOST_D9_VFETCH_BUFFER(k), for a vertex fetch's draw
    };
    VkDescriptorSet descriptor_set(const SetKey &k) {
        const uint64_t key = fnv(&k, sizeof k);
        auto it = cur_->sets.find(key);
        if (it != cur_->sets.end())
            return it->second;
        VkDescriptorSet set = VK_NULL_HANDLE;
        // The frame's pools in turn; a full one is left for the next.
        for (int attempt = 0;; ++attempt) {
            if (cur_->pool_used == 0 || attempt > 0) {
                if (attempt > 2)
                    return VK_NULL_HANDLE;
                if (cur_->pool_used >= cur_->pools.size()) {
                    VkDescriptorPool pool = make_descriptor_pool();
                    if (!pool)
                        return VK_NULL_HANDLE;
                    cur_->pools.push_back(pool);
                }
                ++cur_->pool_used;
            }
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = cur_->pools[cur_->pool_used - 1];
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &set_layout_;
            if (vkAllocateDescriptorSets(vk_, &ai, &set) == VK_SUCCESS)
                break;
        }
        VkDescriptorBufferInfo bufs[4 + 5];
        VkDescriptorImageInfo imgs[16];
        VkWriteDescriptorSet writes[4 + 16 + 5];
        for (uint32_t i = 0; i < 4; ++i) {
            bufs[i] = {k.uniforms, 0, k.ranges[i]};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[i].pBufferInfo = &bufs[i];
        }
        for (uint32_t i = 0; i < 16; ++i) {
            imgs[i] = {k.samplers[i], k.views[i], VK_IMAGE_LAYOUT_GENERAL};
            writes[4 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[4 + i].dstSet = set;
            writes[4 + i].dstBinding = 4 + i;
            writes[4 + i].descriptorCount = 1;
            writes[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[4 + i].pImageInfo = &imgs[i];
        }
        uint32_t n = 4 + 16;
        for (uint32_t j = 0; j < 5 && k.vfetch[j]; ++j) {
            bufs[4 + j] = {k.vfetch[j], 0, VK_WHOLE_SIZE};
            writes[n] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[n].dstSet = set;
            writes[n].dstBinding = d9glsl::kVFetchBinding + j;
            writes[n].descriptorCount = 1;
            writes[n].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[n].pBufferInfo = &bufs[4 + j];
            ++n;
        }
        vkUpdateDescriptorSets(vk_, n, writes, 0, nullptr);
        cur_->sets[key] = set;
        return set;
    }
    static constexpr uint32_t kSetsPerPool = 1024;
    VkDescriptorPool make_descriptor_pool() {
        VkDescriptorPoolSize sizes[3] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4 * kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 * kSetsPerPool},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * kSetsPerPool}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = kSetsPerPool;
        dpi.poolSizeCount = vfetch_ ? 3 : 2;
        dpi.pPoolSizes = sizes;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        if (vkCreateDescriptorPool(vk_, &dpi, nullptr, &pool) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        return pool;
    }

    VkSampler sampler(const uint32_t *st) {
        static const uint32_t none[14] = {0, 1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0};
        if (!st)
            st = none;
        uint64_t key = fnv(st, 14 * sizeof(uint32_t));
        auto it = samplers_.find(key);
        if (it != samplers_.end())
            return it->second;
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.addressModeU = address_mode(st[1], false);
        sc.addressModeV = address_mode(st[2], false);
        sc.addressModeW = address_mode(st[3], false);
        sc.magFilter = st[5] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.minFilter = st[6] >= 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.mipmapMode = st[7] == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.minLod = (float)st[9];
        sc.maxLod = st[7] == 0 ? sc.minLod + 0.25f
                               : VK_LOD_CLAMP_NONE; // no mip filter: level MAXMIPLEVEL only
        if (anisotropy_ && (st[6] == 3 || st[5] == 3)) {
            sc.anisotropyEnable = VK_TRUE;
            sc.maxAnisotropy =
                std::max(1.0f, std::min<float>(props_.limits.maxSamplerAnisotropy,
                                               (float)std::min<uint32_t>(16, st[10])));
        }
        uint32_t border = st[4];
        sc.borderColor = (border >> 24) < 128  ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                         : (border & 0xffffff) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                               : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        VkSampler s = VK_NULL_HANDLE;
        vkCreateSampler(vk_, &sc, nullptr, &s);
        samplers_[key] = s;
        return s;
    }
    VkSampler shadow_sampler() {
        if (shadow_sampler_)
            return shadow_sampler_;
        VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sc.magFilter = sc.minFilter = VK_FILTER_LINEAR;
        sc.compareEnable = VK_TRUE;
        sc.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        sc.maxLod = 0.25f;
        vkCreateSampler(vk_, &sc, nullptr, &shadow_sampler_);
        return shadow_sampler_;
    }
    VkImageView placeholder(bool cube) {
        Placeholder &p = cube ? black_cube_ : black_;
        if (p.view)
            return p.view;
        const uint32_t layers = cube ? 6 : 1;
        if (!make_image(1, 1, 1, layers, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, cube,
                        &p.image, &p.memory))
            return VK_NULL_HANDLE;
        p.view = raw_view(p.image, cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                          VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers);
        const uint8_t black[4] = {0, 0, 0, 255};
        for (uint32_t f = 0; f < layers; ++f) {
            Slice s = ring_alloc(4, 4);
            memcpy(s.data, black, 4);
            VkBufferImageCopy c{};
            c.bufferOffset = s.offset;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
            c.imageExtent = {1, 1, 1};
            bool pass = in_pass_;
            HostD9Target saved = pass_target_;
            if (pass)
                end_pass();
            vkCmdCopyBufferToImage(cur_->cb, s.buffer, p.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            if (pass)
                begin_pass(saved, false, nullptr, false, 0, false, 0);
        }
        return p.view;
    }
    // A depth texture at the far plane: every shadow lookup passes.
    VkImageView placeholder_depth() {
        Placeholder &p = far_depth_;
        if (p.view)
            return p.view;
        if (!make_image(1, 1, 1, 1, depth_format_,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, false,
                        &p.image, &p.memory))
            return VK_NULL_HANDLE;
        p.view = raw_view(p.image, VK_IMAGE_VIEW_TYPE_2D, depth_format_, VK_IMAGE_ASPECT_DEPTH_BIT,
                          0, 1, 0, 1);
        bool pass = in_pass_;
        HostD9Target saved = pass_target_;
        if (pass)
            end_pass();
        VkClearDepthStencilValue v{1.0f, 0};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0,
                                  1};
        vkCmdClearDepthStencilImage(cur_->cb, p.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
        if (pass)
            begin_pass(saved, false, nullptr, false, 0, false, 0);
        return p.view;
    }

    void encode_primitives(const HostD9Draw &d) {
        VkCommandBuffer cb = cur_->cb;
        uint32_t count = d.primitive_count;
        bool fan = false;
        switch (d.primitive) {
        case 1:
            break;
        case 2:
            count *= 2;
            break;
        case 3:
            count += 1;
            break;
        case 4:
            count *= 3;
            break;
        case 5:
            count += 2;
            break;
        case 6:
            fan = true;
            break;
        default:
            return;
        }
        if (!d.primitive_count)
            return;
        bool indexed = d.index_buffer || d.inline_indices;
        if (!indexed && !fan) {
            vkCmdDraw(cb, count, 1, d.start, 0);
            return;
        }
        const VkIndexType itype = d.index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        const uint32_t isize = d.index_size == 4 ? 4 : 2;
        if (!fan) {
            if (d.index_buffer) {
                auto b = buffers_.find(d.index_buffer);
                if (b == buffers_.end() || !b->second.buf.buffer)
                    return;
                if ((uint64_t)(d.start + count) * isize > b->second.shadow.size())
                    return;
                b->second.used = serial_;
                bind_index(b->second.buf.buffer, 0, itype);
                vkCmdDrawIndexed(cb, count, 1, d.start, d.base_vertex, 0);
            } else {
                Slice s = ring_alloc((uint64_t)count * isize, 4);
                memcpy(s.data, d.inline_indices, (size_t)count * isize);
                bind_index(s.buffer, s.offset, itype);
                vkCmdDrawIndexed(cb, count, 1, 0, 0, 0);
            }
            return;
        }
        // A fan becomes a triangle list over the same vertices.
        Slice s = ring_alloc((uint64_t)count * 3 * 4, 4);
        uint32_t *list = (uint32_t *)(void *)s.data;
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
            list[i * 3] = source(0);
            list[i * 3 + 1] = source(i + 1);
            list[i * 3 + 2] = source(i + 2);
        }
        bind_index(s.buffer, s.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, count * 3, 1, 0, 0, 0);
    }
    void bind_index(VkBuffer b, VkDeviceSize off, VkIndexType type) {
        if (es_.ib == b && es_.ib_offset == off && es_.ib_type == type)
            return;
        vkCmdBindIndexBuffer(cur_->cb, b, off, type);
        es_.ib = b;
        es_.ib_offset = off;
        es_.ib_type = type;
    }

    // ---- present helpers ---------------------------------------------------------
    void keep_presented(Tex &t, uint32_t w, uint32_t h) {
        if (!kept_.image || kept_w_ != w || kept_h_ != h) {
            if (kept_.image) {
                Placeholder old = kept_;
                VkDevice vk = vk_;
                bury(serial_ + 1, [vk, old] {
                    vkDestroyImage(vk, old.image, nullptr);
                    vkFreeMemory(vk, old.memory, nullptr);
                });
            }
            kept_ = Placeholder{};
            if (!make_image(w, h, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 1,
                            false, &kept_.image, &kept_.memory))
                return;
            kept_w_ = w;
            kept_h_ = h;
        }
        begin_frame();
        VkImageCopy c{};
        c.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.extent = {w, h, 1};
        vkCmdCopyImage(cur_->cb, t.image, VK_IMAGE_LAYOUT_GENERAL, kept_.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }

    void probe_dump() {
        for (auto &kv : textures_) {
            Tex &t = kv.second;
            if (!t.image || !t.info.bgra8() || t.desc.kind != HOST_D9_TEX_2D)
                continue;
            const uint32_t w = t.width, h = t.height;
            std::vector<uint8_t> bgra((size_t)w * h * 4), rgb((size_t)w * h * 3);
            if (!read_image(t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, w, h, 4, bgra.data()))
                continue;
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
            fprintf(stderr, "probe texture %u %ux%u fmt %08x usage %u: %zu of %zu lit\n", kv.first,
                    w, h, t.desc.format, t.desc.usage, lit, (size_t)w * h);
        }
    }
    void dump(Tex &t, uint32_t w, uint32_t h) {
        std::vector<uint8_t> bgra((size_t)w * h * 4);
        if (!read_image(t.image, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, w, h, 4, bgra.data()))
            return;
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

    float wanted_scale() const {
        if (recomp_env("D3D9_SCALE") || !base_rows_)
            return 0.0f;
        int dw = 0, dh = 0;
        if (!host_present_drawable(&dw, &dh) || dh <= 0)
            return 0.0f;
        int rows = dh;
        if (const char *min = recomp_env("D3D9_MIN_ROWS"))
            rows = std::max(rows, atoi(min));
        float s = std::floor(8.0f * rows / base_rows_ + 0.5f) / 8.0f;
        return std::max(1.0f, std::min(8.0f, s));
    }
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
        wait_all();
        scale_ = want;
        for (auto &kv : fitted_depth_) {
            Fitted f = kv.second;
            VkDevice vk = vk_;
            bury(serial_ + 1, [vk, f] {
                vkDestroyImageView(vk, f.view, nullptr);
                vkDestroyImage(vk, f.image, nullptr);
                vkFreeMemory(vk, f.memory, nullptr);
            });
        }
        fitted_depth_.clear();
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

    bool probing() const {
        return probe_frame_ && presents_ + 1 == probe_frame_;
    }
    void skip(const char *why) {
        ++skips_[why];
    }
    static double now() {
        return (double)os_monotonic_ns() * 1e-9;
    }
    static uint64_t thread_cpu_ns() {
#if defined(_WIN32)
        return 0;
#else
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
    }
    void report() {
        if (!recomp_env("D3D9_STATS"))
            return;
        double t = now();
        double fps = report_time_ > 0 ? (presents_ - report_frames_) / (t - report_time_) : 0.0;
        const uint64_t frames = presents_ > report_frames_ ? presents_ - report_frames_ : 1;
        double dpf = double(draws_ - report_draws_) / double(frames);
        const uint64_t cpu_ns = thread_cpu_ns();
        const double cpu_ms = report_cpu_ns_ ? (cpu_ns - report_cpu_ns_) / 1e6 / frames : 0.0;
        report_cpu_ns_ = cpu_ns;
        const double waited = gpu_wait_s_, gpu = gpu_s_;
        gpu_wait_s_ = gpu_s_ = 0;
        report_time_ = t;
        report_frames_ = presents_;
        report_draws_ = draws_;
        fprintf(stderr,
                "d3d9 vulkan: frame %llu: %.1f fps, %.0f draws/frame, %llu draw calls, %llu "
                "encoded, %zu textures, "
                "%zu pipelines, 0.0M instructions/frame, game thread %.2f ms/frame, gpu %.2f "
                "ms/frame, waited %.2f "
                "ms/frame",
                (unsigned long long)presents_, fps, dpf, (unsigned long long)stat_calls_,
                (unsigned long long)draws_, textures_.size(), pipelines_.size(), cpu_ms,
                gpu * 1000.0 / frames, waited * 1000.0 / frames);
        for (auto &s : skips_)
            fprintf(stderr, ", %s %llu", s.first.c_str(), (unsigned long long)s.second);
        fprintf(stderr, "\n");
    }

    // ---- state -------------------------------------------------------------------
    struct Placeholder {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Fitted {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct EncoderState {
        VkPipeline pipeline = VK_NULL_HANDLE;
        uint32_t stencil_ref = 0xffffffffu;
        float bias = NAN, slope = NAN;
        bool viewport_valid = false, scissor_valid = false;
        VkViewport viewport{};
        VkRect2D scissor{};
        VkBuffer vb[9] = {};
        VkDeviceSize vb_offset[9] = {};
        VkBuffer ib = VK_NULL_HANDLE;
        VkDeviceSize ib_offset = 0;
        VkIndexType ib_type = VK_INDEX_TYPE_UINT16;
    };

    VulkanDevice *dev_;
    VkDevice vk_;
    VkPhysicalDeviceProperties props_{};
    bool ok_ = false;
    bool bc_ = false, wireframe_ = false, precise_ = false, independent_blend_ = false,
         anisotropy_ = false;
    bool bgra_vertex_ = false;
    bool vfetch_ = false, vertex_stores_ = false;
    VkFormat depth_format_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    uint64_t uniform_align_ = 16;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    HostBuffer zero_, readback_;
    VkCommandPool oneshot_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer oneshot_cb_ = VK_NULL_HANDLE;
    VkFence oneshot_fence_ = VK_NULL_HANDLE;

    std::vector<std::unique_ptr<Frame>> all_frames_;
    std::vector<Frame *> free_frames_;
    std::deque<Frame *> in_flight_;
    Frame *cur_ = nullptr;
    uint64_t serial_ = 0, completed_ = 0;
    std::vector<Grave> graves_;

    bool in_pass_ = false;
    HostD9Target pass_target_{};
    Tex *pass_color_[4] = {};
    VkFormat pass_formats_[4] = {};
    uint32_t pass_color_count_ = 0;
    Tex *pass_depth_tex_ = nullptr;
    VkImageView pass_depth_view_ = VK_NULL_HANDLE;
    uint32_t pass_width_ = 0, pass_height_ = 0, pass_samples_ = 1;
    float pass_scale_ = 1.0f;
    int pass_slot_ = -1;
    EncoderState es_;

    bool scale_chosen_ = false;
    uint32_t base_rows_ = 0;
    int rescale_frames_ = 0;
    float scale_ = recomp_env("D3D9_SCALE")
                       ? std::max(1.0f, std::min(8.0f, (float)atof(recomp_env("D3D9_SCALE"))))
                       : 1.0f;

    std::unordered_map<uint32_t, Tex> textures_;
    std::vector<Tex *> tex_index_;
    std::unordered_map<uint32_t, Buf> buffers_;
    std::unordered_map<uint64_t, VkShaderModule> modules_;
    std::unordered_map<uint64_t, VLayout> layouts_;
    std::unordered_map<uint64_t, VkPipeline> pipelines_;
    uint64_t last_pipeline_key_ = 0;
    VkPipeline last_pipeline_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkSampler> samplers_;
    VkSampler shadow_sampler_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, Fitted> fitted_depth_;
    Placeholder black_, black_cube_, far_depth_, kept_;
    uint32_t kept_w_ = 0, kept_h_ = 0;
    std::vector<uint8_t> scratch_;
    std::unordered_map<uint32_t, Query> queries_;
    uint32_t query_ = 0;
    // Frames that wrote the vertex fetch check: serial, tag.
    std::vector<std::pair<uint64_t, uint32_t>> vfetch_tags_;
    uint64_t vfetch_unfit_ = 0, vfetch_unwritable_ = 0;

    uint64_t draws_ = 0, presents_ = 0, stat_calls_ = 0;
    double report_time_ = 0, gpu_wait_s_ = 0, gpu_s_ = 0;
    uint64_t report_frames_ = 0, report_draws_ = 0, report_cpu_ns_ = 0;
    std::map<std::string, uint64_t> skips_;
    std::set<std::string> undecoded_;
    std::string probe_tag_;
    uint64_t probe_frame_ =
        recomp_env("D3D9_PROBE") ? strtoull(recomp_env("D3D9_PROBE"), nullptr, 10) : 0;
};

} // namespace

D9Backend *d9_vulkan_create(gpu::Device *device) {
    auto *vulkan = dynamic_cast<VulkanDevice *>(device);
    if (!vulkan)
        return nullptr;
    auto *r = new VkRenderer(vulkan);
    if (!r->ok()) {
        delete r;
        return nullptr;
    }
    return r;
}
