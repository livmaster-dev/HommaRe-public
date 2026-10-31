// vulkan_device.h - gpu::Device over Vulkan. One queue; a reaper thread
// retires fences in submission order so command buffers complete in commit
// order. Every image the backend owns stays in VK_IMAGE_LAYOUT_GENERAL; a full
// barrier precedes each pass and transfer. Included only under gpu/vulkan/.
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "../../../third_party/volk/volk.h"
#include "../gpu.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace gpu {

bool vulkan_load();                    // volk initialised; false when no loader exists
const char *vulkan_loader_path_impl(); // path dlopen'ed, or nullptr for the default

class VulkanDevice final : public Device {
  public:
    static std::unique_ptr<VulkanDevice> create();
    ~VulkanDevice() override;

    Texture create_texture(const TextureDesc &desc) override;
    bool upload(Texture t, Region region, const void *bytes, int pitch, int level = 0) override;
    bool readback(Texture t, Region region, void *bytes, int pitch) override;
    void destroy(Texture t) override;
    TextureDesc describe(Texture t) override;
    uint64_t allocated_bytes(Texture t) override;
    Buffer create_buffer(uint64_t bytes, const void *contents) override;
    void update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) override;
    const void *map_read(Buffer b) override;
    uint64_t buffer_bytes(Buffer b) override;
    void destroy(Buffer b) override;
    Pipeline render_pipeline(const std::string &shader, const RenderState &state) override;
    Pipeline compute_pipeline(const std::string &shader) override;
    int thread_execution_width(Pipeline p) override;
    CommandBuffer begin() override;
    void begin_render_pass(CommandBuffer cb, const RenderPass &pass) override;
    void set_pipeline(CommandBuffer cb, Pipeline p) override;
    void set_depth(CommandBuffer cb, const DepthState &d) override;
    void set_cull(CommandBuffer cb, Cull c) override;
    void set_viewport(CommandBuffer cb, const Viewport &v) override;
    void set_vertex_buffer(CommandBuffer cb, int slot, Buffer b, uint64_t offset) override;
    void set_bytes(CommandBuffer cb, Stage stage, int slot, const void *bytes,
                   uint64_t count) override;
    void set_buffer(CommandBuffer cb, Stage stage, int slot, Buffer b, uint64_t offset) override;
    void set_texture(CommandBuffer cb, Stage stage, int slot, Texture t) override;
    void set_sampler(CommandBuffer cb, Stage stage, int slot, const SamplerState &s) override;
    void draw(CommandBuffer cb, Primitive primitive, int first, int count) override;
    void end_render_pass(CommandBuffer cb) override;
    void begin_compute_pass(CommandBuffer cb) override;
    void dispatch_threads(CommandBuffer cb, int tx, int ty, int gx, int gy) override;
    void dispatch_groups(CommandBuffer cb, int groups_x, int groups_y, int gx, int gy) override;
    void end_compute_pass(CommandBuffer cb) override;
    void blit(CommandBuffer cb, Texture src, Region src_region, Texture dst, int dst_x,
              int dst_y) override;
    void copy_buffer_to_texture(CommandBuffer cb, Buffer src, uint64_t offset, int pitch,
                                Texture dst, Region dst_region) override;
    void copy_texture_to_buffer(CommandBuffer cb, Texture src, Region src_region, Buffer dst,
                                uint64_t offset, int pitch) override;
    void generate_mipmaps(CommandBuffer cb, Texture t) override;
    void on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) override;
    void commit(CommandBuffer cb) override;
    void wait(CommandBuffer cb) override;
    CommandStatus status(CommandBuffer cb) override;
    Swapchain create_swapchain(void *native_surface, int width, int height) override;
    void resize(Swapchain s, int width, int height) override;
    Format swapchain_format(Swapchain s) override;
    Texture acquire(Swapchain s) override;
    void release_drawable(Swapchain s, Texture t) override;
    void present(CommandBuffer cb, Swapchain s, Texture t, double min_duration_seconds,
                 std::function<void(double)> presented) override;
    double refresh_period(Swapchain s) override;
    void destroy(Swapchain s) override;
    double now_seconds() override;

    // --- for renderers that record their own Vulkan (d3d9_vulkan.cpp) ---
    VkDevice native_device() const {
        return device_;
    }
    VkPhysicalDevice native_physical() const {
        return physical_;
    }
    uint32_t native_queue_family() const {
        return queue_family_;
    }
    const VkPhysicalDeviceFeatures &native_features() const {
        return enabled_features_;
    }
    bool native_core13() const {
        return core13_;
    }
    // Submits `cb` on the device's queue, behind the transfers the device has
    // pending, so it runs before any command buffer committed after it.
    bool submit_native(VkCommandBuffer cb, VkFence fence);
    // A handle the presenter can sample for an image the caller owns and
    // keeps in VK_IMAGE_LAYOUT_GENERAL. destroy() on it forgets the handle
    // and leaves the image alone.
    Texture import_image(VkImage image, VkImageView view, const TextureDesc &desc);

    // --- shared with vulkan_swapchain.cpp ---
    struct Tex {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        TextureDesc desc;
        uint64_t bytes = 0;
        VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL; // swapchain images start UNDEFINED
        bool external = false;                          // swapchain image: not ours to free
        uint64_t swapchain = 0;                         // owning swapchain id for external
        uint32_t image_index = 0;
        // UsageCpu: a persistently mapped staging buffer for level 0.
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void *staging_map = nullptr;
        uint64_t staging_bytes = 0;
    };
    struct Buf {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *map = nullptr;
        uint64_t bytes = 0;
    };
    struct Binding {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64_t offset = 0, range = 0;
    };
    struct TexBinding {
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
    };
    struct Cmd {
        VkCommandBuffer buffer = VK_NULL_HANDLE;
        VkCommandPool pool_of = VK_NULL_HANDLE; // this Cmd's own pool (see below)
        VkFence fence = VK_NULL_HANDLE;
        VkQueryPool queries = VK_NULL_HANDLE; // 2 timestamps
        VkDescriptorPool pool = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> extra_pools; // exhausted pools, reset with `pool`
        // The bytes ring: host-visible, reset when the Cmd is recycled.
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceMemory ring_memory = VK_NULL_HANDLE;
        uint8_t *ring_map = nullptr;
        uint64_t ring_bytes = 0, ring_used = 0;
        std::vector<std::function<void(CommandStatus, double)>> callbacks;
        // Recording state.
        bool in_render = false, in_compute = false;
        uint64_t pipeline = 0; // family id
        Primitive topology = Primitive::Triangles;
        Cull cull = Cull::None;
        DepthState depth;
        Viewport viewport{};
        bool viewport_set = false;
        int pass_width = 0, pass_height = 0;
        Format pass_formats[2] = {Format::BGRA8, Format::R8};
        int pass_color_count = 1;
        bool pass_depth = false;
        Binding buffers[8];
        TexBinding textures[4];
        bool bindings_dirty = true;
        VkPipeline bound = VK_NULL_HANDLE;
        // Presentation.
        uint64_t present_swapchain = 0;
        uint32_t present_image = 0;
        bool presents = false;
        VkSemaphore wait_semaphore = VK_NULL_HANDLE, signal_semaphore = VK_NULL_HANDLE;
        std::function<void(double)> presented;
        uint32_t draws = 0, dispatches = 0, binds = 0; // trace counters
    };
    struct Submission {
        uint64_t id = 0;
        std::unique_ptr<Cmd> cmd;
    };
    // A destroyed resource a recording or in-flight command buffer may still
    // reference: freed once every command buffer in `waits` has retired, the
    // retention Metal's encoders gave for free.
    struct Grave {
        Tex tex;
        Buf buf;
        std::vector<uint64_t> waits;
    };
    struct Chain {
        void *window = nullptr; // SDL_Window*
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
        int width = 0, height = 0;
        std::vector<VkImage> images;
        std::vector<VkImageView> views;
        std::vector<VkSemaphore> acquire_semaphores;  // one per image slot plus one, rotated
        std::vector<VkSemaphore> finished_semaphores; // one per image
        uint32_t next_acquire = 0;
        std::unordered_map<uint64_t, uint32_t> acquired; // texture id -> image index
        std::unordered_map<uint64_t, VkSemaphore> acquired_wait;
        bool needs_recreate = false;
        bool release = false; // release_drawable may use vkReleaseSwapchainImagesEXT
        double refresh = 1.0 / 60;
    };

    Cmd *cmd(CommandBuffer cb); // mutex held; nullptr when unknown or committed
    void full_barrier(VkCommandBuffer cb);
    Binding ring_alloc(Cmd &c, const void *bytes, uint64_t count); // mutex held
    VkCommandBuffer one_shot_begin();
    void one_shot_end_wait(VkCommandBuffer cb);
    void end_completing(uint64_t id);        // with mutex_ held
    void wait_all_submitted();               // every fence so far; shutdown and swapchain teardown
    void queue_idle(); // vkQueueWaitIdle under queue_mutex_; never with mutex_ held
    void wait_submitted_before(uint64_t id); // submissions older than `id`; bounded under load
    uint64_t submission_watermark();         // next_id_ now: everything submitted so far is older
    void fail(const char *what);
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want);
    bool allocate(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkDeviceMemory *out);
    Texture register_external_image(VkImage image, VkImageView view, const TextureDesc &desc,
                                    uint64_t swapchain, uint32_t index);
    void transition_if_external(Cmd &c, Tex &t, VkImageLayout to); // swapchain images only
    bool init_pipeline_layout();
    void reap_loop();
    void create_descriptor_pool(Cmd &c);
    void end_passes(Cmd &c);
    void queue_present(Cmd &c);
    VkShaderModule module(const std::string &name);
    VkPipeline variant_for(Cmd &c);
    VkSampler sampler_for(const SamplerState &s);
    void bind_descriptors(Cmd &c, VkPipelineBindPoint point);
    void destroy_tex(Tex &t);
    void destroy_buf(Buf &b);
    void bury(Tex tex, Buf buf);         // mutex held
    void retire_graves(uint64_t cmd_id); // mutex held

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    // A command pool may be used by one thread at a time and the host records a
    // command buffer from whichever thread holds it, so every Cmd owns its own
    // pool: the host's per-command-buffer discipline is then all the pool needs.
    std::vector<std::unique_ptr<Cmd>> free_cmds_;  // guarded by mutex_
    VkCommandPool transfer_pool_ = VK_NULL_HANDLE; // guarded by transfer_mutex_
    // Transfers. Uploads and texture creation record into one pending command
    // buffer (a normal Cmd, registered in recording_ so the graveyard waits on
    // it) that every commit submits ahead of its own work; nothing on the
    // upload path waits for the GPU. Readbacks still use the synchronous
    // one-shot buffer below, after flushing the pending transfers, so the bytes
    // are there on return. transfer_mutex_ serialises all of it and is taken
    // before mutex_.
    std::mutex transfer_mutex_;
    uint64_t pending_id_ = 0;                      // the pending transfer Cmd's id, 0 for none
    bool pending_dirty_ = false;                   // it has recorded something
    VkCommandBuffer transfer_cb_ = VK_NULL_HANDLE; // readback one-shot
    VkFence transfer_fence_ = VK_NULL_HANDLE;
    Cmd *pending_locked();       // transfer_mutex_ held; nullptr when the device failed
    void flush_pending_locked(); // transfer_mutex_ held
    void flush_pending();
    void submit_cmd(uint64_t id, std::unique_ptr<Cmd> c); // the tail of commit()
    VkPhysicalDeviceProperties props_{};
    VkPhysicalDeviceMemoryProperties memory_props_{};
    uint32_t subgroup_size_ = 32;
    bool subgroup_arithmetic_ = false;
    bool anisotropy_ = false;
    VkPhysicalDeviceFeatures enabled_features_{};
    bool core13_ = false;
    bool full_subgroups_ = false; // computeFullSubgroups enabled
    // VK_EXT_swapchain_maintenance1: an acquired image goes back unpresented (release_drawable).
    bool release_images_ = false;
    bool failed_ = false;
    bool trace_ = false;     // RECOMP_GPU_TRACE=1
    uint32_t one_shots_ = 0; // trace: synchronous transfers since the last commit
    // RECOMP_GPU_TRACE per-second summary: where the CPU waited on the GPU.
    struct TraceSecond {
        double started = 0;
        uint32_t commits = 0, one_shots = 0, waits = 0, acquires = 0, sets = 0, draws = 0;
        double one_shot_ms = 0, wait_ms = 0, acquire_ms = 0, submit_ms = 0;
    } trace_second_;
    void trace_tick(); // mutex held; prints and resets once a second

    std::mutex mutex_; // guards every table below
    std::mutex queue_mutex_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, Tex> textures_;
    std::unordered_map<uint64_t, Buf> buffers_;
    std::unordered_map<uint64_t, std::unique_ptr<Cmd>> recording_;
    // Submitted and not yet retired, in submission order (the reaper's queue).
    std::deque<Submission> submitted_;
    std::vector<Grave> graves_;
    std::unordered_map<uint64_t, CommandStatus> retired_; // bounded: last 1024
    std::deque<uint64_t> retired_order_;
    // Retired buffers whose on_complete callbacks are still running, unlocked
    // (the reaper's, or a failed submit's): wait() returns only after them, as
    // gpu.h's "blocks until complete" means.
    std::vector<uint64_t> completing_;
    std::condition_variable reaper_cv_, retired_cv_;
    std::thread reaper_;
    bool stopping_ = false;

    struct Family {
        std::string shader;
        RenderState state;
        std::map<uint64_t, VkPipeline> variants; // key: topology | cull<<2 | compare<<4 | write<<8
    };
    std::unordered_map<uint64_t, Family> families_;
    std::unordered_map<uint64_t, uint64_t> family_by_key_; // hash(shader, state.key()) -> id
    struct ComputePipe {
        VkPipeline pipeline = VK_NULL_HANDLE;
        int width = 1;
    };
    std::unordered_map<uint64_t, ComputePipe> computes_;
    std::unordered_map<std::string, uint64_t> compute_by_name_;
    std::unordered_map<std::string, VkShaderModule> modules_;
    std::map<uint64_t, VkSampler> samplers_; // key from SamplerState fields
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    Buf dummy_buffer_;
    Texture dummy_texture_;
    VkSampler dummy_sampler_ = VK_NULL_HANDLE;

    std::unordered_map<uint64_t, std::unique_ptr<Chain>> swapchains_;
};

VkFormat vk_format(Format f);
Format format_of(VkFormat f);

} // namespace gpu
