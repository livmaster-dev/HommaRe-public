// gpu_contract_tests.cpp - a backend against the gpu.h contract: bytes
// round-trip, a clear and a draw land where expected, commands complete in
// commit order, wait() returns after their handlers, a completion handler may
// own what it frees, a compute kernel reads what a texture holds, and a native
// surface makes a swapchain. Compiled once per backend with
// POP_GPU_TEST_BACKEND naming it; the factory override selects it.
#include "../../../platform/os.h"
#include "../gpu_factory.h"

#include <atomic>
#include <chrono>
#include <math.h>
#include <memory>
#include <mutex>
#include <stdlib.h>
#include <thread>
#include <stdio.h>
#include <string.h>
#include <vector>

static int g_checks = 0, g_failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(x)) {                                                                                \
            ++g_failures;                                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);                           \
        }                                                                                          \
    } while (0)

using namespace gpu;

static void test_upload_readback() {
    auto d = create_default_device();
    CHECK(d != nullptr);
    if (!d)
        return;
    Texture t = d->create_texture({4, 2, Format::RGBA8, UsageSampled | UsageCpu});
    CHECK(t);
    uint8_t in[32], out[32] = {0};
    for (int i = 0; i < 32; ++i)
        in[i] = uint8_t(i * 3);
    CHECK(d->upload(t, {0, 0, 4, 2}, in, 16));
    CHECK(d->readback(t, {0, 0, 4, 2}, out, 16));
    CHECK(memcmp(in, out, 32) == 0);
    CHECK(d->describe(t).width == 4 && d->allocated_bytes(t) >= 32);
    // A private render target round-trips through staging.
    Texture p = d->create_texture({4, 2, Format::RGBA8, UsageSampled | UsageRenderTarget});
    CHECK(d->upload(p, {0, 0, 4, 2}, in, 16));
    memset(out, 0, sizeof out);
    CHECK(d->readback(p, {0, 0, 4, 2}, out, 16));
    CHECK(memcmp(in, out, 32) == 0);
    d->destroy(t);
    d->destroy(p);
    CHECK(d->describe(t).width == 0);
}

static void test_clear_and_compositor_draw() {
    auto d = create_default_device();
    if (!d)
        return;
    Texture target = d->create_texture({8, 8, Format::BGRA8, UsageRenderTarget | UsageCpu});
    Texture src = d->create_texture({2, 2, Format::BGRA8, UsageSampled | UsageCpu});
    uint8_t green[16];
    for (int i = 0; i < 4; ++i) {
        green[i * 4] = 0;
        green[i * 4 + 1] = 255;
        green[i * 4 + 2] = 0;
        green[i * 4 + 3] = 255;
    }
    d->upload(src, {0, 0, 2, 2}, green, 8);
    RenderPass pass;
    pass.color[0].texture = target;
    pass.color[0].load = Load::Clear;
    pass.color[0].clear[0] = 1;
    pass.color[0].clear[1] = 0;
    pass.color[0].clear[2] = 0;
    RenderState state;
    state.color_format[0] = Format::BGRA8;
    struct Quad {
        float rect[4];
        float uv[4];
        float drawable[2];
        uint32_t opaque, pad;
    } q = {{0, 0, 4, 8}, {0, 0, 1, 1}, {8, 8}, 1, 0}; // the left half
    CommandBuffer cb = d->begin();
    d->begin_render_pass(cb, pass);
    Pipeline pipeline = d->render_pipeline("compositor", state);
    CHECK(pipeline);
    d->set_pipeline(cb, pipeline);
    d->set_bytes(cb, Stage::Vertex, 0, &q, sizeof q);
    d->set_bytes(cb, Stage::Fragment, 0, &q, sizeof q);
    d->set_texture(cb, Stage::Fragment, 0, src);
    d->set_sampler(cb, Stage::Fragment, 0, SamplerState{});
    d->draw(cb, Primitive::TriangleStrip, 0, 4);
    d->end_render_pass(cb);
    bool completed = false;
    double gpu_ms = -1;
    d->on_complete(cb, [&](CommandStatus s, double ms) {
        completed = s == CommandStatus::Completed;
        gpu_ms = ms;
    });
    d->commit(cb);
    d->wait(cb);
    CHECK(completed);
    CHECK(gpu_ms >= 0);
    CHECK(d->status(cb) == CommandStatus::Completed);
    uint8_t left[4], right[4];
    CHECK(d->readback(target, {1, 4, 1, 1}, left, 4));
    CHECK(d->readback(target, {6, 4, 1, 1}, right, 4));
    CHECK(left[1] == 255 && left[2] == 0);   // the green quad
    CHECK(right[2] == 255 && right[1] == 0); // the red clear
    // The same state returns the cached pipeline.
    CHECK(d->render_pipeline("compositor", state).id == pipeline.id);
}

static void test_commit_order() {
    auto d = create_default_device();
    if (!d)
        return;
    std::vector<int> order;
    std::mutex m;
    for (int round = 0; round < 20; ++round) {
        CommandBuffer a = d->begin(), b = d->begin();
        d->on_complete(a, [&](CommandStatus, double) {
            std::lock_guard l(m);
            order.push_back(1);
        });
        d->on_complete(b, [&](CommandStatus, double) {
            std::lock_guard l(m);
            order.push_back(2);
        });
        d->commit(a);
        d->commit(b);
        d->wait(b);
    }
    std::lock_guard l(m);
    CHECK(order.size() == 40);
    bool ordered = true;
    for (size_t i = 0; i + 1 < order.size(); i += 2)
        ordered = ordered && order[i] == 1 && order[i + 1] == 2;
    CHECK(ordered);
}

// wait() returns after the buffer's completion handlers have run, not only its
// work: the draw and commit-order checks above read what the handlers wrote
// right after wait(). The Vulkan reaper marked a buffer retired and woke
// wait() before it ran the handlers, so those checks failed about one run in
// forty on lavapipe; a handler slower than the wakeup shows it every time.
// Vulkan only: the ordering is the reaper's; Metal's is waitUntilCompleted's.
static void test_wait_covers_completion_handlers() {
    if (strcmp(POP_GPU_TEST_BACKEND, "vulkan") != 0)
        return;
    auto d = create_default_device();
    if (!d)
        return;
    // Shared with the handlers: one wait() returned early leaves its handler
    // running past this round, and past this function.
    auto handled = std::make_shared<std::atomic<int>>(0);
    for (int round = 1; round <= 3; ++round) {
        CommandBuffer cb = d->begin();
        d->on_complete(cb, [handled](CommandStatus, double) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            ++*handled;
        });
        d->commit(cb);
        d->wait(cb);
        CHECK(handled->load() == round);
    }
}

// A completion handler can hold the last reference to GPU resources: the
// presenter's handler holds its frame, and a frame dropped while in flight has
// no other owner. Releasing the handler then destroys textures through the
// device, so the backend must have let go of its own lock by then. The Vulkan
// reaper had not; it waited on itself and no command completed again.
static void test_completion_handler_owns_resources() {
    auto d = create_default_device();
    if (!d)
        return;
    struct Owner {
        Device *device = nullptr;
        Texture texture;
        std::atomic<bool> *released = nullptr;
        ~Owner() {
            device->destroy(texture);
            released->store(true);
        }
    };
    std::atomic<bool> released{false};
    auto owner = std::make_shared<Owner>();
    owner->device = d.get();
    owner->texture = d->create_texture({4, 4, Format::BGRA8, UsageSampled});
    owner->released = &released;
    CommandBuffer cb = d->begin();
    d->on_complete(cb, [owner](CommandStatus, double) {});
    owner.reset();
    d->commit(cb);
    // Not d->wait(cb): with the lock still held it would wait forever too.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!released.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(released.load());
    if (!released.load()) {
        // A device cannot be torn down past a stuck completion thread.
        fprintf(stderr, "the completion thread deadlocked releasing its handler\n");
        fflush(stderr);
        _Exit(1);
    }
    CommandBuffer next = d->begin();
    d->commit(next);
    d->wait(next);
    CHECK(d->status(next) == CommandStatus::Completed);
}

static void test_compute_readback_kernel() {
    auto d = create_default_device();
    if (!d)
        return;
    Texture color =
        d->create_texture({16, 16, Format::BGRA8, UsageSampled | UsageCpu | UsageRenderTarget});
    Texture cover =
        d->create_texture({16, 16, Format::R8, UsageSampled | UsageCpu | UsageRenderTarget});
    std::vector<uint8_t> px(16 * 16 * 4, 0x40), cv(16 * 16, 0xff);
    d->upload(color, {0, 0, 16, 16}, px.data(), 64);
    d->upload(cover, {0, 0, 16, 16}, cv.data(), 16);
    Buffer out = d->create_buffer(16 * 16 * 8, nullptr);
    CHECK(d->buffer_bytes(out) == 16 * 16 * 8);
    uint32_t p[12] = {0, 0, 16, 16, 16, 16, 16, 16, 0, 16, 16, 0};
    CommandBuffer cb = d->begin();
    d->begin_compute_pass(cb);
    Pipeline kernel = d->compute_pipeline("guest_readback");
    CHECK(kernel);
    CHECK(d->thread_execution_width(kernel) >= 1);
    d->set_pipeline(cb, kernel);
    d->set_texture(cb, Stage::Compute, 0, color);
    d->set_texture(cb, Stage::Compute, 1, cover);
    d->set_buffer(cb, Stage::Compute, 0, out, 0);
    d->set_bytes(cb, Stage::Compute, 1, p, sizeof p);
    d->dispatch_threads(cb, 16, 16, 8, 8);
    d->end_compute_pass(cb);
    d->commit(cb);
    d->wait(cb);
    const uint32_t *words = static_cast<const uint32_t *>(d->map_read(out));
    CHECK(words != nullptr);
    if (words) {
        CHECK((words[0] & 0xffffff) == 0x404040);
        CHECK((words[0] >> 24) == 1);
        CHECK((words[2 * 255] & 0xffffff) == 0x404040);
    }
}

static void test_swapchain_from_surface() {
    auto d = create_default_device();
    if (!d)
        return;
    void *surface = test_native_surface(32, 16);
    if (!surface) {
        printf("no native surface here: swapchain test skipped\n");
        return;
    }
    Swapchain s = d->create_swapchain(surface, 32, 16);
    CHECK(s && d->swapchain_format(s) == Format::BGRA8);
    Texture t = d->acquire(s);
    CHECK(t && d->describe(t).width == 32 && d->describe(t).height == 16);
    d->release_drawable(s, t);
    CHECK(d->describe(t).width == 0);
    // As a window host does: the window first, then the swapchain. X11 pins
    // a swapchain to its window, so resizing only the swapchain kept 32x16.
    test_resize_native_surface(surface, 64, 32);
    d->resize(s, 64, 32);
    Texture t2 = d->acquire(s);
    CHECK(t2 && d->describe(t2).width == 64);
    d->release_drawable(s, t2);
    CHECK(d->refresh_period(s) > 0.0 && d->refresh_period(s) < 0.1);
    CHECK(d->now_seconds() > 0.0);
    d->destroy(s);
}

// A drawable acquired and then not drawn (the presenter's composite could not
// be allocated) goes back to the swapchain with release_drawable, however often
// that happens. The Vulkan backend once only forgot it: the swapchain ran out
// of images after a few, and the next acquire waited forever. Both of its ways
// back are covered: VK_EXT_swapchain_maintenance1's release, and presenting
// the image (RECOMP_VULKAN_RELEASE_IMAGES=0, what a device without it does).
static void test_released_drawables_return() {
    for (const char *release : {"1", "0"}) {
        os_setenv("RECOMP_VULKAN_RELEASE_IMAGES", release);
        auto d = create_default_device();
        if (!d)
            break;
        void *surface = test_native_surface(32, 16);
        if (!surface) {
            printf("no native surface here: drawable release test skipped\n");
            break;
        }
        Swapchain s = d->create_swapchain(surface, 32, 16);
        CHECK(s);
        if (!s)
            break;
        constexpr int kRounds = 24; // several times any swapchain's image count
        std::atomic<int> rounds{0};
        std::atomic<bool> finished{false};
        std::thread worker([&] {
            for (int i = 0; i < kRounds; ++i) {
                Texture t = d->acquire(s);
                if (!t)
                    break;
                d->release_drawable(s, t);
                ++rounds;
            }
            finished = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!finished && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(rounds.load() == kRounds);
        if (!finished) {
            fprintf(stderr, "acquire never returned after %d released drawables (release %s)\n",
                    rounds.load(), release);
            fflush(stderr);
            _Exit(1);
        }
        worker.join();
        d->destroy(s);
    }
    os_unsetenv("RECOMP_VULKAN_RELEASE_IMAGES");
}

// The presenter makes, resizes and destroys swapchains while other threads
// (the game's renderer, transfers) keep submitting. The wait for a swapchain's
// last use must hold the queue as those submits do - vkDeviceWaitIdle did not -
// without waiting on the device's own lock in the wrong order.
static void test_swapchain_churn_beside_submits() {
    auto d = create_default_device();
    if (!d)
        return;
    void *surface = test_native_surface(32, 16);
    Swapchain probe = surface ? d->create_swapchain(surface, 32, 16) : Swapchain{};
    if (!probe) {
        printf("no swapchain on a test surface here: churn test skipped\n");
        return;
    }
    d->destroy(probe);
    std::atomic<bool> stop{false}, finished{false};
    std::atomic<int> submits{0}, rounds{0};
    std::thread submitter([&] {
        while (!stop) {
            CommandBuffer cb = d->begin();
            d->commit(cb);
            d->wait(cb);
            ++submits;
        }
    });
    std::thread churn([&] {
        for (int i = 0; i < 20; ++i) {
            Swapchain s = d->create_swapchain(surface, 32, 16);
            if (!s)
                break;
            if (Texture t = d->acquire(s))
                d->release_drawable(s, t);
            test_resize_native_surface(surface, 48, 24);
            d->resize(s, 48, 24);
            if (Texture t = d->acquire(s))
                d->release_drawable(s, t);
            test_resize_native_surface(surface, 32, 16);
            d->destroy(s);
            ++rounds;
        }
        finished = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!finished && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!finished) {
        fprintf(stderr, "swapchain churn stuck after %d rounds beside %d submits\n", rounds.load(),
                submits.load());
        fflush(stderr);
        _Exit(1);
    }
    stop = true;
    churn.join();
    submitter.join();
    CHECK(rounds.load() == 20);
    CHECK(submits.load() > 0);
}

int main() {
    os_setenv("RECOMP_GPU_BACKEND", POP_GPU_TEST_BACKEND);
    if (strcmp(default_backend_name(), POP_GPU_TEST_BACKEND) != 0) {
        fprintf(stderr, "backend %s is not available here (got %s)\n", POP_GPU_TEST_BACKEND,
                default_backend_name());
        return 1;
    }
    test_upload_readback();
    test_clear_and_compositor_draw();
    test_commit_order();
    test_wait_covers_completion_handlers();
    test_completion_handler_owns_resources();
    test_compute_readback_kernel();
    test_swapchain_from_surface();
    test_released_drawables_return();
    test_swapchain_churn_beside_submits();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all gpu %s tests passed\n", POP_GPU_TEST_BACKEND);
    return g_failures ? 1 : 0;
}
