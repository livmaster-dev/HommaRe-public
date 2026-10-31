#include "fake_device.h"

#include <algorithm>
#include <cstring>

namespace gpu {

int FakeDevice::bytes_per_pixel(Format f) {
    switch (f) {
    case Format::R8:
        return 1;
    case Format::Depth32F:
        return 4;
    default:
        return 4;
    }
}
FakeDevice::Tex *FakeDevice::tex(Texture t) {
    auto it = textures_.find(t.id);
    return it == textures_.end() ? nullptr : &it->second;
}

void FakeDevice::hold_allocations_of(std::thread::id who) {
    std::lock_guard lock(gate_mutex_);
    held_thread_ = who;
    gate_cv_.notify_all();
}
int FakeDevice::allocations_held() const {
    std::lock_guard lock(gate_mutex_);
    return held_;
}
Texture FakeDevice::create_texture(const TextureDesc &desc) {
    {
        std::unique_lock gate(gate_mutex_);
        ++held_;
        gate_cv_.wait(gate, [&] { return held_thread_ != std::this_thread::get_id(); });
        --held_;
    }
    std::lock_guard lock(mutex_);
    if (desc.width <= 0 || desc.height <= 0)
        return {};
    if (fail_allocations_ > 0) {
        --fail_allocations_;
        return {};
    }
    Tex t;
    t.desc = desc;
    int w = desc.width, h = desc.height;
    for (int level = 0; level < std::max(1, desc.mip_levels); ++level) {
        t.levels.emplace_back(size_t(w) * h * bytes_per_pixel(desc.format), 0);
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
    }
    uint64_t id = next_id_++;
    textures_[id] = std::move(t);
    return {id};
}
bool FakeDevice::upload(Texture tex_handle, Region r, const void *bytes, int pitch, int level) {
    std::lock_guard lock(mutex_);
    Tex *t = tex(tex_handle);
    if (!t || level < 0 || level >= int(t->levels.size()) || !bytes)
        return false;
    int w = std::max(1, t->desc.width >> level), h = std::max(1, t->desc.height >> level);
    if (r.x < 0 || r.y < 0 || r.x + r.w > w || r.y + r.h > h)
        return false;
    const int bpp = bytes_per_pixel(t->desc.format);
    for (int y = 0; y < r.h; ++y)
        memcpy(&t->levels[level][(size_t(r.y + y) * w + r.x) * bpp],
               static_cast<const uint8_t *>(bytes) + size_t(y) * pitch, size_t(r.w) * bpp);
    return true;
}
bool FakeDevice::readback(Texture tex_handle, Region r, void *bytes, int pitch) {
    std::lock_guard lock(mutex_);
    Tex *t = tex(tex_handle);
    if (!t || !bytes || r.x < 0 || r.y < 0 || r.x + r.w > t->desc.width ||
        r.y + r.h > t->desc.height)
        return false;
    const int bpp = bytes_per_pixel(t->desc.format);
    for (int y = 0; y < r.h; ++y)
        memcpy(static_cast<uint8_t *>(bytes) + size_t(y) * pitch,
               &t->levels[0][(size_t(r.y + y) * t->desc.width + r.x) * bpp], size_t(r.w) * bpp);
    return true;
}
void FakeDevice::destroy(Texture t) {
    std::lock_guard lock(mutex_);
    textures_.erase(t.id);
}
TextureDesc FakeDevice::describe(Texture t) {
    std::lock_guard lock(mutex_);
    Tex *p = tex(t);
    return p ? p->desc : TextureDesc{};
}
uint64_t FakeDevice::allocated_bytes(Texture t) {
    std::lock_guard lock(mutex_);
    Tex *p = tex(t);
    uint64_t total = 0;
    if (p)
        for (auto &level : p->levels)
            total += level.size();
    return total;
}

Buffer FakeDevice::create_buffer(uint64_t bytes, const void *contents) {
    std::lock_guard lock(mutex_);
    Buf b;
    b.bytes.assign(bytes, 0);
    if (contents)
        memcpy(b.bytes.data(), contents, bytes);
    uint64_t id = next_id_++;
    buffers_[id] = std::move(b);
    return {id};
}
void FakeDevice::update(Buffer b, uint64_t offset, const void *bytes, uint64_t count) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    if (it != buffers_.end() && offset + count <= it->second.bytes.size())
        memcpy(it->second.bytes.data() + offset, bytes, count);
}
const void *FakeDevice::map_read(Buffer b) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    return it == buffers_.end() ? nullptr : it->second.bytes.data();
}
uint64_t FakeDevice::buffer_bytes(Buffer b) {
    std::lock_guard lock(mutex_);
    auto it = buffers_.find(b.id);
    return it == buffers_.end() ? 0 : it->second.bytes.size();
}
void FakeDevice::destroy(Buffer b) {
    std::lock_guard lock(mutex_);
    buffers_.erase(b.id);
}

Pipeline FakeDevice::render_pipeline(const std::string &shader, const RenderState &state) {
    std::lock_guard lock(mutex_);
    auto &by_key = render_pipelines_[shader];
    auto it = by_key.find(state.key());
    if (it != by_key.end())
        return it->second;
    Pipeline p{next_id_++};
    by_key[state.key()] = p;
    return p;
}
Pipeline FakeDevice::compute_pipeline(const std::string &shader) {
    std::lock_guard lock(mutex_);
    auto it = compute_pipelines_.find(shader);
    if (it != compute_pipelines_.end())
        return it->second;
    Pipeline p{next_id_++};
    compute_pipelines_[shader] = p;
    return p;
}
int FakeDevice::thread_execution_width(Pipeline) {
    return 32;
}

CommandBuffer FakeDevice::begin() {
    std::lock_guard lock(mutex_);
    uint64_t id = next_id_++;
    commands_[id] = Cmd{};
    return {id};
}
void FakeDevice::begin_render_pass(CommandBuffer cb, const RenderPass &pass) {
    std::lock_guard lock(mutex_);
    auto &c = commands_[cb.id];
    c.pass = pass;
    c.in_pass = true;
    // A Clear load action is the one thing a pass does that a test can see.
    for (int i = 0; i < pass.color_count; ++i) {
        Tex *t = tex(pass.color[i].texture);
        if (!t || pass.color[i].load != Load::Clear)
            continue;
        const int bpp = bytes_per_pixel(t->desc.format);
        uint8_t px[4] = {0, 0, 0, 0};
        if (t->desc.format == Format::BGRA8) {
            px[0] = uint8_t(pass.color[i].clear[2] * 255 + 0.5f);
            px[1] = uint8_t(pass.color[i].clear[1] * 255 + 0.5f);
            px[2] = uint8_t(pass.color[i].clear[0] * 255 + 0.5f);
            px[3] = uint8_t(pass.color[i].clear[3] * 255 + 0.5f);
        } else {
            for (int k = 0; k < 4; ++k)
                px[k] = uint8_t(pass.color[i].clear[k] * 255 + 0.5f);
        }
        for (size_t at = 0; at + bpp <= t->levels[0].size(); at += bpp)
            memcpy(&t->levels[0][at], px, bpp);
    }
}
void FakeDevice::set_pipeline(CommandBuffer, Pipeline) {}
void FakeDevice::set_depth(CommandBuffer, const DepthState &) {}
void FakeDevice::set_cull(CommandBuffer, Cull) {}
void FakeDevice::set_viewport(CommandBuffer, const Viewport &) {}
void FakeDevice::set_vertex_buffer(CommandBuffer, int, Buffer, uint64_t) {}
void FakeDevice::set_bytes(CommandBuffer, Stage, int, const void *, uint64_t) {}
void FakeDevice::set_buffer(CommandBuffer, Stage, int, Buffer, uint64_t) {}
void FakeDevice::set_texture(CommandBuffer, Stage, int, Texture) {}
void FakeDevice::set_sampler(CommandBuffer, Stage, int, const SamplerState &) {}
void FakeDevice::draw(CommandBuffer cb, Primitive, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].draws;
}
void FakeDevice::end_render_pass(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    commands_[cb.id].in_pass = false;
}
void FakeDevice::begin_compute_pass(CommandBuffer) {}
void FakeDevice::dispatch_threads(CommandBuffer cb, int, int, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].dispatches;
}
void FakeDevice::dispatch_groups(CommandBuffer cb, int, int, int, int) {
    std::lock_guard lock(mutex_);
    ++commands_[cb.id].dispatches;
}
void FakeDevice::end_compute_pass(CommandBuffer) {}
void FakeDevice::blit(CommandBuffer, Texture src, Region r, Texture dst, int dx, int dy) {
    std::lock_guard lock(mutex_);
    Tex *s = tex(src), *d = tex(dst);
    if (!s || !d || s->desc.format != d->desc.format)
        return;
    const int bpp = bytes_per_pixel(s->desc.format);
    for (int y = 0; y < r.h; ++y) {
        if (r.y + y >= s->desc.height || dy + y >= d->desc.height)
            break;
        int w = std::min({r.w, s->desc.width - r.x, d->desc.width - dx});
        if (w <= 0)
            break;
        memcpy(&d->levels[0][(size_t(dy + y) * d->desc.width + dx) * bpp],
               &s->levels[0][(size_t(r.y + y) * s->desc.width + r.x) * bpp], size_t(w) * bpp);
    }
}
void FakeDevice::copy_buffer_to_texture(CommandBuffer, Buffer src, uint64_t offset, int pitch,
                                        Texture dst, Region r) {
    std::lock_guard lock(mutex_);
    Tex *d = tex(dst);
    auto b = buffers_.find(src.id);
    if (!d || b == buffers_.end() || r.x < 0 || r.y < 0 || r.x + r.w > d->desc.width ||
        r.y + r.h > d->desc.height)
        return;
    const int bpp = bytes_per_pixel(d->desc.format);
    for (int y = 0; y < r.h; ++y) {
        const uint64_t at = offset + uint64_t(y) * pitch;
        if (at + uint64_t(r.w) * bpp > b->second.bytes.size())
            break;
        memcpy(&d->levels[0][(size_t(r.y + y) * d->desc.width + r.x) * bpp], &b->second.bytes[at],
               size_t(r.w) * bpp);
    }
}
void FakeDevice::copy_texture_to_buffer(CommandBuffer, Texture src, Region r, Buffer dst,
                                        uint64_t offset, int pitch) {
    std::lock_guard lock(mutex_);
    Tex *s = tex(src);
    auto b = buffers_.find(dst.id);
    if (!s || b == buffers_.end() || r.x < 0 || r.y < 0 || r.x + r.w > s->desc.width ||
        r.y + r.h > s->desc.height)
        return;
    const int bpp = bytes_per_pixel(s->desc.format);
    for (int y = 0; y < r.h; ++y) {
        const uint64_t at = offset + uint64_t(y) * pitch;
        if (at + uint64_t(r.w) * bpp > b->second.bytes.size())
            break;
        memcpy(&b->second.bytes[at], &s->levels[0][(size_t(r.y + y) * s->desc.width + r.x) * bpp],
               size_t(r.w) * bpp);
    }
}
void FakeDevice::generate_mipmaps(CommandBuffer, Texture) {}
void FakeDevice::on_complete(CommandBuffer cb, std::function<void(CommandStatus, double)> fn) {
    std::lock_guard lock(mutex_);
    commands_[cb.id].on_complete.push_back(std::move(fn));
}
void FakeDevice::commit(CommandBuffer cb) {
    std::vector<std::function<void(CommandStatus, double)>> run;
    std::function<void()> present;
    if (commit_hook_)
        commit_hook_();
    {
        std::lock_guard lock(mutex_);
        auto &c = commands_[cb.id];
        c.committed = true;
        committed_order_.push_back(cb.id);
        if (manual_completion_)
            return;
        run.swap(c.on_complete);
        present = std::move(c.deferred_present);
    }
    if (present)
        present();
    for (auto &fn : run)
        fn(CommandStatus::Completed, 0.0);
}
void FakeDevice::complete_all() {
    std::vector<std::function<void(CommandStatus, double)>> run;
    std::vector<std::function<void()>> presents;
    CommandStatus status;
    {
        std::lock_guard lock(mutex_);
        status = completion_status_;
        for (uint64_t id : committed_order_) {
            auto &c = commands_[id];
            for (auto &fn : c.on_complete)
                run.push_back(std::move(fn));
            c.on_complete.clear();
            if (c.deferred_present)
                presents.push_back(std::move(c.deferred_present));
        }
    }
    for (auto &p : presents)
        p();
    for (auto &fn : run)
        fn(status, 0.0);
}
void FakeDevice::wait(CommandBuffer) {}
CommandStatus FakeDevice::status(CommandBuffer cb) {
    std::lock_guard lock(mutex_);
    auto it = commands_.find(cb.id);
    if (it == commands_.end())
        return CommandStatus::Completed;
    return it->second.committed && !manual_completion_ ? CommandStatus::Completed
                                                       : CommandStatus::Pending;
}

Swapchain FakeDevice::create_swapchain(void *, int width, int height) {
    std::lock_guard lock(mutex_);
    if (width <= 0 || height <= 0)
        return {};
    uint64_t id = next_id_++;
    swapchains_[id] = Chain{width, height, Format::BGRA8, {}};
    return {id};
}
void FakeDevice::resize(Swapchain s, int width, int height) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    if (it != swapchains_.end()) {
        it->second.w = width;
        it->second.h = height;
    }
}
Format FakeDevice::swapchain_format(Swapchain s) {
    std::lock_guard lock(mutex_);
    auto it = swapchains_.find(s.id);
    return it == swapchains_.end() ? Format::BGRA8 : it->second.format;
}
Texture FakeDevice::acquire(Swapchain s) {
    int w, h;
    Format f;
    {
        std::lock_guard lock(mutex_);
        auto it = swapchains_.find(s.id);
        if (it == swapchains_.end())
            return {};
        w = it->second.w;
        h = it->second.h;
        f = it->second.format;
    }
    Texture t = create_texture({w, h, f, UsageRenderTarget | UsageCpu});
    std::lock_guard lock(mutex_);
    swapchains_[s.id].acquired.push_back(t);
    return t;
}
void FakeDevice::release_drawable(Swapchain, Texture t) {
    destroy(t);
}
void FakeDevice::present(CommandBuffer cb, Swapchain, Texture t, double min_duration,
                         std::function<void(double)> presented) {
    std::lock_guard lock(mutex_);
    last_min_duration_ = min_duration;
    last_presented_ = presented;
    auto self = this;
    commands_[cb.id].deferred_present = [self, presented, t] {
        double now;
        {
            std::lock_guard lock(self->mutex_);
            now = self->clock_;
        }
        if (presented)
            presented(now);
        self->destroy(t);
    };
}
void FakeDevice::fire_presented(double presented_seconds) {
    std::function<void(double)> fn;
    {
        std::lock_guard lock(mutex_);
        fn = last_presented_;
    }
    if (fn)
        fn(presented_seconds);
}
double FakeDevice::refresh_period(Swapchain) {
    return 1.0 / 60;
}
void FakeDevice::destroy(Swapchain s) {
    std::lock_guard lock(mutex_);
    swapchains_.erase(s.id);
}
double FakeDevice::now_seconds() {
    std::lock_guard lock(mutex_);
    return clock_;
}
void FakeDevice::advance_clock(double seconds) {
    std::lock_guard lock(mutex_);
    clock_ += seconds;
}
void FakeDevice::set_clock(double seconds) {
    std::lock_guard lock(mutex_);
    clock_ = seconds;
}
int FakeDevice::draws_recorded(CommandBuffer cb) const {
    std::lock_guard lock(mutex_);
    auto it = commands_.find(cb.id);
    return it == commands_.end() ? 0 : it->second.draws;
}
int FakeDevice::dispatches_recorded(CommandBuffer cb) const {
    std::lock_guard lock(mutex_);
    auto it = commands_.find(cb.id);
    return it == commands_.end() ? 0 : it->second.dispatches;
}
int FakeDevice::live_textures() const {
    std::lock_guard lock(mutex_);
    return int(textures_.size());
}

} // namespace gpu
