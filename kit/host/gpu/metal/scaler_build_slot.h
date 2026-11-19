// Scheduling half of building a MetalFX scaler off the render path (d3d9_metal.mm, metalfx_upscale
// and scene_end), kept free of Metal so its rules can be tested without a device
// (tests/scaler_build_slot_tests.cpp).
//
// Creating an MTLFXSpatialScaler takes about 70 ms (MacNativeGames' measure), and a new one is
// needed whenever the frame's size, the window's size or the render quality changes - including
// each step of the thermal auto-quality. The render thread no longer waits for it:
//  - at a frame boundary it asks for the configuration this frame needs (request);
//  - the first request starts one background build; while it runs, a newer request only replaces
//    the wanted configuration, and when the build ends the builder goes straight on to the latest
//    wanted one (complete returns it) - configurations asked for in between are never built;
//  - a finished scaler is handed over once, and only for the configuration still wanted: one made
//    for a size the renderer has since left is dropped;
//  - until then the caller keeps its current scaler (when it still fits the textures) or the
//    plain path, so no frame is encoded with a scaler of another size;
//  - settle() says the current scaler fits again: a build still running is dropped when it ends.
// Nothing is held locked while a scaler is created.
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

// A scaler's fixed configuration: input and output size and the colour format.
struct ScalerKey {
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0, format = 0;
    bool operator==(const ScalerKey &o) const {
        return in_w == o.in_w && in_h == o.in_h && out_w == o.out_w && out_h == o.out_h && format == o.format;
    }
    bool operator!=(const ScalerKey &o) const { return !(*this == o); }
};

template <class Scaler> class ScalerBuildSlot {
  public:
    struct Request {
        bool ready = false;      // `scaler` was built for exactly the key asked for (it may be null: failed)
        Scaler scaler{};
        bool start = false;      // the caller starts the background build of the key it asked for
    };
    // Render thread, at a frame boundary: the configuration this frame needs, which the caller's
    // current scaler does not have.
    Request request(const ScalerKey &want) {
        std::lock_guard<std::mutex> lock(mutex_);
        Request r;
        if (ready_ && ready_key_ == want) {
            r.ready = true;
            r.scaler = ready_scaler_;
            ready_ = false;
            ready_scaler_ = Scaler{};
            wanted_.reset();
            ++handed_;
            return r;
        }
        if (ready_) {   // built for a configuration left since: never handed over
            ready_ = false;
            ready_scaler_ = Scaler{};
            ++dropped_;
        }
        wanted_ = want;
        if (!building_) {
            building_ = true;
            r.start = true;
            ++started_;
        }
        return r;
    }
    // Render thread: the current scaler fits this frame; nothing more is wanted.
    void settle() {
        std::lock_guard<std::mutex> lock(mutex_);
        wanted_.reset();
        if (ready_) {
            ready_ = false;
            ready_scaler_ = Scaler{};
            ++dropped_;
        }
    }
    // Builder: the scaler for `built` is made (null on failure). Returns the next key to build,
    // the latest one wanted, when that is another; otherwise the builder stops.
    std::optional<ScalerKey> complete(const ScalerKey &built, Scaler s) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (wanted_ && *wanted_ == built) {
            ready_ = true;
            ready_key_ = built;
            ready_scaler_ = s;
            building_ = false;
            return std::nullopt;
        }
        ++dropped_;
        if (wanted_) {
            ++started_;
            return *wanted_;   // still building: go on to the latest wanted configuration
        }
        building_ = false;
        return std::nullopt;
    }
    bool building() {
        std::lock_guard<std::mutex> lock(mutex_);
        return building_;
    }
    uint32_t started() {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_;
    }
    uint32_t handed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return handed_;
    }
    uint32_t dropped() {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

  private:
    std::mutex mutex_;
    std::optional<ScalerKey> wanted_;
    bool building_ = false, ready_ = false;
    ScalerKey ready_key_;
    Scaler ready_scaler_{};
    uint32_t started_ = 0, handed_ = 0, dropped_ = 0;
};
