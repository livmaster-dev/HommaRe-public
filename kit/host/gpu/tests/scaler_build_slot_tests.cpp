// Rules of building a MetalFX scaler in the background (metal/scaler_build_slot.h). No device, no game.
#include "../metal/scaler_build_slot.h"

#include <atomic>
#include <cstdio>
#include <thread>

static int failures = 0, checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(c)) {                                                                                \
            ++failures;                                                                            \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                       \
        }                                                                                          \
    } while (0)

static ScalerKey key(uint32_t in_w, uint32_t out_w) {
    ScalerKey k;
    k.in_w = in_w;
    k.in_h = in_w * 3 / 4;
    k.out_w = out_w;
    k.out_h = out_w * 3 / 4;
    k.format = 80;
    return k;
}

int main() {
    {   // first request starts one build; repeated requests while it runs start nothing more
        ScalerBuildSlot<int> s;
        auto r = s.request(key(800, 1600));
        CHECK(r.start && !r.ready);
        r = s.request(key(800, 1600));
        CHECK(!r.start && !r.ready);
        CHECK(s.building());
        CHECK(!s.complete(key(800, 1600), 7));   // the wanted one: nothing more to build
        CHECK(!s.building());
        r = s.request(key(800, 1600));
        CHECK(r.ready && r.scaler == 7 && !r.start);
        CHECK(s.handed() == 1 && s.started() == 1);
        r = s.request(key(800, 1600));            // handed over once only
        CHECK(!r.ready && r.start);
    }
    {   // newer requests while building: only the latest is built next, the ones between never
        ScalerBuildSlot<int> s;
        CHECK(s.request(key(800, 1600)).start);
        CHECK(!s.request(key(810, 1600)).start);
        CHECK(!s.request(key(820, 1600)).start);
        CHECK(!s.request(key(830, 1600)).start);
        auto next = s.complete(key(800, 1600), 1);
        CHECK(next && *next == key(830, 1600));
        CHECK(s.building());
        auto r = s.request(key(800, 1600));       // the stale result is never handed over
        CHECK(!r.ready && !r.start);
        next = s.complete(key(830, 1600), 2);       // wanted is now 800 again
        CHECK(next && *next == key(800, 1600));
        CHECK(!s.complete(key(800, 1600), 3));
        r = s.request(key(800, 1600));
        CHECK(r.ready && r.scaler == 3);
        CHECK(s.started() == 3 && s.dropped() == 2);
    }
    {   // a result for a configuration left after it finished is dropped, and the new one built
        ScalerBuildSlot<int> s;
        s.request(key(800, 1600));
        CHECK(!s.complete(key(800, 1600), 4));
        auto r = s.request(key(640, 1600));
        CHECK(!r.ready && r.start);
        CHECK(s.dropped() == 1);
    }
    {   // settle: the current scaler fits again; a running build is dropped when it ends
        ScalerBuildSlot<int> s;
        s.request(key(800, 1600));
        s.settle();
        CHECK(!s.complete(key(800, 1600), 5));
        CHECK(!s.building());
        auto r = s.request(key(800, 1600));
        CHECK(!r.ready && r.start);               // not the dropped one: built again
    }
    {   // a failed build is handed over as such (null), for the caller to fall back for good
        ScalerBuildSlot<int> s;
        s.request(key(800, 1600));
        s.complete(key(800, 1600), 0);
        auto r = s.request(key(800, 1600));
        CHECK(r.ready && r.scaler == 0);
    }
    {   // threads: a builder loop on its own thread, the render side moving the size every frame;
        // at most one build at a time, and whatever is handed over matches the key asked for
        ScalerBuildSlot<int> s;
        std::atomic<int> in_flight{0}, max_in_flight{0}, bad{0}, handed{0};
        std::thread render([&] {
            for (uint32_t frame = 0; frame < 20000; ++frame) {
                const ScalerKey want = key(600 + (frame / 50) % 8 * 10, 1600);
                auto r = s.request(want);
                if (r.ready) {
                    ++handed;
                    if ((uint32_t)r.scaler != want.in_w)
                        ++bad;
                }
                if (r.start) {
                    std::thread([&s, &in_flight, &max_in_flight, want] {
                        ScalerKey k = want;
                        for (;;) {
                            const int n = ++in_flight;
                            int m = max_in_flight.load();
                            while (n > m && !max_in_flight.compare_exchange_weak(m, n)) {
                            }
                            std::this_thread::sleep_for(std::chrono::microseconds(200));
                            --in_flight;
                            auto next = s.complete(k, (int)k.in_w);
                            if (!next)
                                break;
                            k = *next;
                        }
                    }).detach();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(20));
            }
        });
        render.join();
        while (s.building())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(bad == 0);
        CHECK(max_in_flight <= 1);
        CHECK(handed > 0);
    }
    std::printf("scaler_build_slot_tests: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
