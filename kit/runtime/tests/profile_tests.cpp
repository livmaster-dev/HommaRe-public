// Real generated dispatch and sampler, with a safe test hook at a generated
// entry. Executing an arbitrary game's entry point or first function can boot
// the game; the copy workload exercises profiling without those side effects.
#include "game_config.h"
#include "../profile.h"
extern "C" {
#include "funcs.h"
}
#include "../loader.h"
#include "../memory.h"
#include "../intrinsics.h"
#include "../win32.h"
#include "../imports.h"
#include <initializer_list>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <setjmp.h>
#include "../../platform/os.h"
static int failures;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (0)
static uint32_t target;
static uint32_t index_;
static uint32_t fixture_calls;
// Volatile stores keep the sampler's workload observable even under LTO.
// The hook follows the guest cdecl return convention, just like a translation.
static void copy_fixture(X86 *c, uint32_t) {
    uint32_t dst = rd32(c->r[R_ESP] + 4), src = rd32(c->r[R_ESP] + 8);
    volatile uint8_t *out = g_mem + dst;
    const uint8_t *in = g_mem + src;
    for (uint32_t n = 0; n < 65536; ++n)
        out[n] = in[n];
    ++fixture_calls;
    c->r[R_EAX] = dst;
    c->eip = rd32(c->r[R_ESP]);
    c->r[R_ESP] += 4;
}
static void direct_call(X86 *c) {
    CALL_FN(RECOMP_PROFILE_TARGET);
}
static bool nested_ok;
static void nested(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 2);
    CHECK(sched_current_holder_slot()->top.load() == i);
    recomp_hook_ptrs[i] = copy_fixture;
    recomp_call(c, target);
    CHECK(recomp_profile_depth() == 2);
    CHECK(sched_current_holder_slot()->top.load() == i);
    nested_ok = true;
}
static uint32_t import_call(X86 *c, const char *name, std::initializer_list<uint32_t> args) {
    uint32_t saved = c->r[R_ESP];
    uint32_t sp = saved - 4 * (args.size() + 1);
    uint32_t p = sp + 4;
    for (auto arg : args) {
        wr32(p, arg);
        p += 4;
    }
    wr32(sp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = sp;
    uint32_t address = imports_resolve("kernel32.dll", name);
    CHECK(address != 0);
    imports_dispatch(c, address);
    CHECK(c->r[R_ESP] == saved);
    return c->r[R_EAX];
}
static bool worker_ran;
static ProfileSlot *worker_slot;
static void worker(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 1);
    worker_slot = sched_current_holder_slot();
    CHECK(worker_slot && worker_slot->top.load() == i);
    import_call(c, "Sleep", {1});
    CHECK(sched_current_holder_slot() == worker_slot);
    CHECK(worker_slot->top.load() == i);
    worker_ran = true;
    // ExitThread abandons recomp_call's pop; scheduler cleanup must clear it.
    import_call(c, "ExitThread", {0});
    CHECK(false);
}
static bool sync_ran;
static void sync_worker(X86 *c, uint32_t i) {
    CHECK(recomp_profile_depth() == 2);
    recomp_profile_push(i); // abandoned by ExitThread's synchronous landing pad
    sync_ran = true;
    import_call(c, "ExitThread", {0});
    CHECK(false);
}
int main(int argc, char **argv) {
    bool enabled = argc > 1 && strcmp(argv[1], "enabled") == 0;
    CHECK(bool(recomp_profile_enabled) == enabled);
    mem_init();
    // Game-backed: without the developer's image (the stub game) there is
    // nothing to profile.
    if (FILE *image = fopen(RECOMP_DEVELOPER_EXE, "rb"))
        fclose(image);
    else {
        printf("profile_tests: no game image at %s; skipped\n", RECOMP_DEVELOPER_EXE);
        return 0;
    }
    if (!loader_load()) {
        fprintf(stderr, "loader: %s\n", loader_error());
        return 1;
    }
    X86 &c = *loader_context();
    loader_init_context(&c);
    sched_set_guest_thread(true);
    CHECK(recomp_func_count > 0);
    if (!recomp_func_count)
        return 1;
    target = recomp_func_addrs[0];
    CHECK(FIDX(RECOMP_PROFILE_TARGET) == 0);
    int32_t found = recomp_index_of(target);
    CHECK(found >= 0);
    if (found < 0)
        return 1;
    index_ = uint32_t(found);
    RecompHookFn saved_hook = recomp_hook_ptrs[index_];
    uint8_t saved_hooked = recomp_hooked[index_];
    recomp_hook_ptrs[index_] = copy_fixture;
    recomp_hooked[index_] = 1;
    constexpr uint32_t n = 65536;
    uint32_t src = heap_alloc(n), dst = heap_alloc(n);
    CHECK(src && dst);
    if (!src || !dst)
        return 1;
    memset(g_mem + src, 0x5a, n);
    g_mem[src + n - 1] = 0;
    uint32_t sp = c.r[R_ESP] - 32;
    auto setup = [&] {
        c.r[R_ESP] = sp;
        wr32(sp, GUEST_RETURN_SENTINEL);
        wr32(sp + 4, dst);
        wr32(sp + 8, src);
    };
    // Caller stays published across all the generated dispatches; gaps therefore
    // count against the >90% requirement instead of disappearing as idle time.
    // Batches of 10,000 repeat until 150 ms have passed: a fast machine copies
    // one batch in under 100 ms, fewer samples than the 1000 Hz sampler needs.
    recomp_profile_push(0xfffffffe);
    uint32_t dispatches = 0;
    const auto start = std::chrono::steady_clock::now();
    do {
        for (int i = 0; i < 10000; ++i) {
            setup();
            if (i & 1)
                direct_call(&c);
            else
                recomp_call(&c, target);
        }
        dispatches += 10000;
    } while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(150));
    CHECK(memcmp(g_mem + src, g_mem + dst, n) == 0);
    CHECK(fixture_calls == dispatches);
    CHECK(recomp_profile_depth() == (enabled ? 1u : 0u));
    if (enabled) {
        auto rows = profile_snapshot();
        uint64_t total = 0, hot = 0;
        for (auto r : rows) {
            total += r.samples;
            if (r.index == index_)
                hot = r.samples;
        }
        printf("fixture: %u generated dispatches at %08x with a copy hook, "
               "hot=%llu total=%llu share=%.2f%%\n",
               dispatches, target, (unsigned long long)hot, (unsigned long long)total,
               total ? 100.0 * hot / total : 0);
        CHECK(total >= 100);
        CHECK(hot * 100 > total * 90);
        setup();
        recomp_hook_ptrs[index_] = nested;
        recomp_hooked[index_] = 1;
        recomp_call(&c, target);
        CHECK(nested_ok);
        CHECK(recomp_profile_depth() == 1);
        CHECK(sched_current_holder_slot()->top.load() == 0xfffffffe);
        // Match the generated two-call intrinsic: the saved depth must survive
        // jumping over two published callees, and preserve the original caller.
        uint32_t buf = heap_alloc(64);
        c.r[R_ESP] = sp;
        wr32(sp, GUEST_RETURN_SENTINEL);
        wr32(sp + 4, buf);
        jmp_buf *env = recomp_setjmp_prepare(&c);
        int value = setjmp(*env);
        recomp_setjmp_return(&c, value);
        if (!value) {
            recomp_profile_push(index_);
            recomp_profile_push(123);
            c.r[R_ESP] = sp - 32;
            wr32(sp - 28, buf);
            wr32(sp - 24, 7);
            recomp_longjmp(&c);
            CHECK(false);
        }
        CHECK(value == 7);
        CHECK(recomp_profile_depth() == 1);
        CHECK(sched_current_holder_slot()->top.load() == 0xfffffffe);
        ProfileSlot *main_slot = sched_current_holder_slot();
        recomp_hook_ptrs[index_] = worker;
        recomp_hooked[index_] = 1;
        import_call(&c, "CreateThread", {0, 0, target, 0, 0, 0});
        for (int tries = 0; tries < 100 && !worker_ran; ++tries)
            import_call(&c, "Sleep", {2});
        CHECK(worker_ran);
        CHECK(worker_slot && worker_slot != main_slot);
        CHECK(worker_slot && worker_slot->top.load() == PROFILE_IDLE);
        CHECK(sched_current_holder_slot() == main_slot);
        CHECK(main_slot->top.load() == 0xfffffffe);
        recomp_hook_ptrs[index_] = sync_worker;
        os_setenv("RECOMP_CREATETHREAD", "sync");
        import_call(&c, "CreateThread", {0, 0, target, 0, 0, 0});
        os_unsetenv("RECOMP_CREATETHREAD");
        CHECK(sync_ran);
        CHECK(recomp_profile_depth() == 1);
        CHECK(main_slot->top.load() == 0xfffffffe);
        recomp_hooked[index_] = 0;
        recomp_profile_pop();
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
        recomp_profile_push(index_);
        sched_run_thread_unwind_frames();
        CHECK(recomp_profile_depth() == 0);
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
        recomp_profile_push(index_);
        sched_set_guest_thread(false);
        CHECK(recomp_profile_depth() == 0);
        CHECK(sched_current_holder_slot()->top.load() == PROFILE_IDLE);
    } else {
        CHECK(profile_snapshot().empty());
        CHECK(sched_current_holder_slot() == nullptr);
    }
    recomp_hook_ptrs[index_] = saved_hook;
    recomp_hooked[index_] = saved_hooked;
    profile_stop();
    printf("profile tests: %s (%d failures; %s)\n", failures ? "FAIL" : "PASS", failures,
           enabled ? "enabled" : "disabled");
    return failures ? 1 : 0;
}
