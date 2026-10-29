// gpu_contract_tests.cpp - a backend against the gpu.h contract: bytes
// round-trip, a clear and a draw land where expected, commands complete in
// commit order, a compute kernel reads what a texture holds, and a native
// surface makes a swapchain. Compiled once per backend with
// POP_GPU_TEST_BACKEND naming it; the factory override selects it.
#include "../../../platform/os.h"
#include "../gpu_factory.h"

#include <math.h>
#include <mutex>
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
    d->resize(s, 64, 32);
    Texture t2 = d->acquire(s);
    CHECK(t2 && d->describe(t2).width == 64);
    d->release_drawable(s, t2);
    CHECK(d->refresh_period(s) > 0.0 && d->refresh_period(s) < 0.1);
    CHECK(d->now_seconds() > 0.0);
    d->destroy(s);
}

int main() {
    // No backend at all ("none"): the player is still told which graphics
    // the game needs, not "none" (host/sdl/main.cpp's no-GPU launcher).
    os_setenv("RECOMP_GPU_BACKEND", "none-such");
    CHECK(strcmp(default_backend_name(), "none") == 0);
    CHECK(strcmp(graphics_needed(), strcmp(POP_GPU_TEST_BACKEND, "metal") == 0 ? "Metal" : "Vulkan 1.1") == 0);
    os_setenv("RECOMP_GPU_BACKEND", POP_GPU_TEST_BACKEND);
    if (strcmp(default_backend_name(), POP_GPU_TEST_BACKEND) != 0) {
        fprintf(stderr, "backend %s is not available here (got %s)\n", POP_GPU_TEST_BACKEND,
                default_backend_name());
        return 1;
    }
    test_upload_readback();
    test_clear_and_compositor_draw();
    test_commit_order();
    test_compute_readback_kernel();
    test_swapchain_from_surface();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all gpu %s tests passed\n", POP_GPU_TEST_BACKEND);
    return g_failures ? 1 : 0;
}
