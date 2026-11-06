// fake_device.h - gpu::Device with CPU storage and immediate completion, for
// tests that need the presenter, compositor or renderer logic and not a GPU.
// Passes execute nothing; a Clear load action fills the attachment, a blit
// copies bytes, draws and dispatches are counted. Time is whatever the test
// says it is.
#pragma once
#include "../gpu.h"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gpu {

class FakeDevice final : public Device {
  public:
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

    // --- test controls ---
    void advance_clock(double seconds);
    void set_clock(double seconds);
    int draws_recorded(CommandBuffer cb) const;
    int dispatches_recorded(CommandBuffer cb) const;
    // Whether the next N texture allocations fail, for the presenter's halving test.
    void fail_next_allocations(int n) {
        fail_allocations_ = n;
    }
    // Texture allocations made on thread `who` wait until this is called again
    // with another thread (a default id lets them all go): a device whose
    // allocation is slow, for the presenter's lock test.
    void hold_allocations_of(std::thread::id who);
    int allocations_held() const; // allocations waiting at the gate now
    // Hold completion callbacks instead of running them at commit; `complete_all` runs them.
    void set_manual_completion(bool manual) {
        manual_completion_ = manual;
    }
    void complete_all();
    // Runs inside every commit, unlocked, before the commit counts as made: time
    // passing there is a commit that takes long (a synchronous present).
    void set_commit_hook(std::function<void()> hook) {
        commit_hook_ = std::move(hook);
    }
    // The status complete_all() reports for every pending command buffer.
    void set_completion_status(CommandStatus status) {
        completion_status_ = status;
    }
    // The last present's minimum duration, for the pacing selector test.
    double last_present_min_duration() const {
        return last_min_duration_;
    }
    // Fire the presented callback of the most recent present with this time.
    void fire_presented(double presented_seconds);
    int live_textures() const;

  private:
    struct Tex {
        TextureDesc desc;
        std::vector<std::vector<uint8_t>> levels;
    };
    struct Buf {
        std::vector<uint8_t> bytes;
    };
    struct Cmd {
        bool committed = false;
        int draws = 0, dispatches = 0;
        std::vector<std::function<void(CommandStatus, double)>> on_complete;
        std::function<void()> deferred_present;
        RenderPass pass;
        bool in_pass = false;
    };
    struct Chain {
        int w, h;
        Format format = Format::BGRA8;
        std::vector<Texture> acquired;
    };
    static int bytes_per_pixel(Format f);
    Tex *tex(Texture t);
    mutable std::mutex mutex_;
    uint64_t next_id_ = 1;
    std::map<uint64_t, Tex> textures_;
    std::map<uint64_t, Buf> buffers_;
    std::map<uint64_t, Cmd> commands_;
    std::map<uint64_t, Chain> swapchains_;
    std::map<std::string, std::map<uint64_t, Pipeline>> render_pipelines_;
    std::map<std::string, Pipeline> compute_pipelines_;
    std::vector<uint64_t> committed_order_;
    double clock_ = 0;
    int fail_allocations_ = 0;
    bool manual_completion_ = false;
    CommandStatus completion_status_ = CommandStatus::Completed;
    double last_min_duration_ = -1;
    mutable std::mutex gate_mutex_;
    std::condition_variable gate_cv_;
    std::thread::id held_thread_{};
    int held_ = 0;
    std::function<void(double)> last_presented_;
    std::function<void()> commit_hook_;
};

} // namespace gpu
