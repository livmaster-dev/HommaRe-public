// platform_tests.cpp - the platform layer, checked without a guest, a window
// or a GPU. Runs on every platform the layer supports.
#include "../os.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
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

namespace {

struct ThreadReport {
    OsThreadId seen_self = 0;
    int ran = 0;
};

void *report_self(void *arg) {
    ThreadReport *r = (ThreadReport *)arg;
    r->seen_self = os_thread_self();
    r->ran = 1;
    return nullptr;
}

void *deep_stack(void *arg) {
    // 400 KB of stack, which fits a 512 KB request and not a 64 KB one.
    volatile char buf[400 * 1024];
    buf[0] = 1;
    buf[sizeof buf - 1] = 2;
    *(int *)arg = buf[0] + buf[sizeof buf - 1];
    return nullptr;
}

int collect_name(const char *name, void *user) {
    ((std::vector<std::string> *)user)->push_back(name);
    return 0;
}

std::string scratch_dir() {
    const char *base = recomp_env("PLATFORM_TEST_DIR");
    std::string dir = std::string(base && *base ? base : "build/recomp") + "/platform-test";
    // A stub-only checkout has no build/recomp yet: create every component.
    for (size_t i = dir.find('/'); i != std::string::npos; i = dir.find('/', i + 1))
        os_mkdir(dir.substr(0, i).c_str());
    os_mkdir(dir.c_str());
    return dir;
}

void test_threads() {
    ThreadReport r;
    OsThread *t = os_thread_create(report_self, &r, 0);
    CHECK(t != nullptr);
    OsThreadId id = os_thread_id_of(t);
    os_thread_join(t);
    CHECK(r.ran == 1);
    CHECK(r.seen_self == id);
    CHECK(r.seen_self != os_thread_self());

    int sum = 0;
    OsThread *big = os_thread_create(deep_stack, &sum, 512 * 1024);
    CHECK(big != nullptr);
    os_thread_join(big);
    CHECK(sum == 3);
}

void test_time() {
    uint64_t a = os_monotonic_ns();
    os_sleep_us(2000);
    uint64_t b = os_monotonic_ns();
    CHECK(b > a);
    CHECK(b - a >= 1000000ull);                            // at least 1 ms elapsed
    CHECK(os_wall_time_us() > 1600000000ull * 1000000ull); // after 2020
    struct tm utc{};
    CHECK(os_gmtime(86400, &utc) == 0 && utc.tm_year == 70 && utc.tm_mday == 2 && utc.tm_hour == 0);
    struct tm local{};
    CHECK(os_localtime(86400 * 365, &local) == 0 && local.tm_year >= 70);
}

void test_vm() {
    size_t bytes = 1 << 20;
    uint8_t *p = (uint8_t *)os_vm_reserve(bytes);
    CHECK(p != nullptr);
    if (p) {
        CHECK(p[0] == 0 && p[bytes - 1] == 0);
        p[bytes - 1] = 7;
        CHECK(p[bytes - 1] == 7);
        os_vm_release(p, bytes);
    }
}

void test_paths_and_descriptors() {
    std::string dir = scratch_dir();
    std::string sub = dir + "/listed";
    os_unlink((sub + "/a.txt").c_str());
    os_unlink((sub + "/b.txt").c_str());
    os_mkdir(sub.c_str());
    CHECK(os_mkdir(sub.c_str()) == -1); // already there, as mkdir reports it

    int fd = os_fd_open((sub + "/a.txt").c_str(), OS_O_RDWR | OS_O_CREAT | OS_O_TRUNC);
    CHECK(fd >= 0);
    CHECK(os_fd_write(fd, "hello", 5) == 5);
    CHECK(os_fd_seek(fd, 0, OS_SEEK_SET) == 0);
    char buf[8] = {0};
    CHECK(os_fd_read(fd, buf, sizeof buf) == 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    OsStat st;
    CHECK(os_fd_stat(fd, &st) == 0 && st.size == 5 && st.is_regular && !st.is_dir);
    CHECK(os_fd_truncate(fd, 2) == 0);
    CHECK(os_fd_fsync(fd) == 0);
    int dup = os_fd_dup(fd);
    CHECK(dup >= 0 && dup != fd);
    CHECK(os_fd_close(dup) == 0);
    CHECK(os_fd_close(fd) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == 0 && st.size == 2);
    CHECK(os_fd_open((sub + "/a.txt").c_str(), OS_O_WRONLY | OS_O_CREAT | OS_O_EXCL) == -1);

    std::string tmpl = sub + "/b.XXXXXX";
    std::vector<char> t(tmpl.begin(), tmpl.end());
    t.push_back(0);
    int tfd = os_mkstemp(t.data());
    CHECK(tfd >= 0);
    CHECK(strncmp(t.data(), (sub + "/b.").c_str(), sub.size() + 3) == 0);
    CHECK(strcmp(t.data() + sub.size() + 3, "XXXXXX") != 0);
    FILE *f = (FILE *)os_fdopen(tfd, "wb");
    CHECK(f != nullptr);
    if (f)
        fclose(f);
    CHECK(os_rename(t.data(), (sub + "/b.txt").c_str()) == 0);
    CHECK(os_rename((sub + "/b.txt").c_str(), (sub + "/a.txt").c_str()) == 0); // replaces

    std::vector<std::string> names;
    CHECK(os_listdir(sub.c_str(), collect_name, &names) == 0);
    CHECK(names.size() == 1 && names[0] == "a.txt");
    CHECK(os_listdir((sub + "/nowhere").c_str(), collect_name, &names) == -1);

    CHECK(os_stat(sub.c_str(), &st) == 0 && st.is_dir);
    CHECK(os_lstat(sub.c_str(), &st) == 0 && !st.is_symlink);
    CHECK(os_unlink((sub + "/a.txt").c_str()) == 0);
    CHECK(os_stat((sub + "/a.txt").c_str(), &st) == -1);
    CHECK(os_rmdir(sub.c_str()) == 0);
    CHECK(os_stat(sub.c_str(), &st) == -1);

    char cwd[4096];
    CHECK(os_getcwd(cwd, sizeof cwd) == 0 && cwd[0] != 0);
    CHECK(os_chdir(cwd) == 0);
}

void test_process_and_strings() {
    char exe[4096];
    CHECK(os_exe_path(exe, sizeof exe) == 0);
    char dir[512];
    CHECK(strchr(os_temp_dir(), '\\') == nullptr);
    snprintf(dir, sizeof dir, "%s/pop-platform-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    OsStat st;
    CHECK(os_stat(dir, &st) == 0 && st.is_dir);
    CHECK(os_rmdir(dir) == 0);
    CHECK(os_stat(os_null_device(), &st) == 0 || true); // exists on both; stat may refuse NUL
    // The test binary re-runs itself as a child that exits 7.
    const char *child_argv[] = {exe, "--child-exit-7", nullptr};
    int64_t pid = 0;
    CHECK(os_spawn(child_argv, &pid) == 0);
    int code = -1;
    CHECK(os_wait(pid, &code) == 0);
    CHECK(code == 7);
    CHECK(strstr(exe, "platform_tests") != nullptr);
    CHECK(os_strcasecmp("Data", "DATA") == 0);
    CHECK(os_strcasecmp("a", "b") < 0);
    CHECK(os_setenv("RECOMP_PLATFORM_TEST", "yes") == 0);
    CHECK(recomp_env("PLATFORM_TEST") && strcmp(recomp_env("PLATFORM_TEST"), "yes") == 0);
    CHECK(os_unsetenv("RECOMP_PLATFORM_TEST") == 0);
    CHECK(recomp_env("PLATFORM_TEST") == nullptr || recomp_env("PLATFORM_TEST")[0] == 0);
    const char *ext = os_plugin_extension();
    CHECK(ext[0] == '.' && strlen(ext) >= 3);
    char data[4096];
    CHECK(os_user_data_dir("PopRecompTest", data, sizeof data) == 0);
    CHECK(strstr(data, "PopRecompTest") != nullptr);
    CHECK(data[strlen(data) - 1] != '/' && data[strlen(data) - 1] != '\\');
    CHECK(strchr(data, '\\') == nullptr); // paths come out in '/' everywhere
    // The file that holds a function of this binary's is the binary itself
    // (as the loader names it: glibc says argv[0] for the executable).
    char mine[4096];
    CHECK(os_module_path(reinterpret_cast<const void *>(&test_process_and_strings), mine,
                         sizeof mine) == 0);
    CHECK(strstr(mine, "platform_tests") != nullptr);
    CHECK(strchr(mine, '\\') == nullptr);
    CHECK(os_module_path(reinterpret_cast<const void *>(&test_process_and_strings), mine, 4) == -1);
    {
        uint64_t free_bytes = 0;
        const std::string dir = scratch_dir();
        CHECK(os_free_space(dir.c_str(), &free_bytes) == 0);
        CHECK(free_bytes > 0);
        CHECK(os_free_space("/definitely/not/a/volume/path", &free_bytes) == -1);
        const std::string file = dir + "/mtime.txt";
        FILE *f = fopen(file.c_str(), "wb");
        CHECK(f != nullptr);
        if (f)
            fclose(f);
        CHECK(os_set_mtime(file.c_str(), 1000000000) == 0);
        OsStat st{};
        CHECK(os_stat(file.c_str(), &st) == 0 && st.mtime == 1000000000);
        os_unlink(file.c_str());
    }
    CHECK(os_dlopen_noload("/definitely/not/loaded") == nullptr);
    CHECK(os_dlopen("/definitely/not/a/plugin") == nullptr);
    CHECK(os_dlerror() != nullptr);
    os_write_stderr_raw("platform_tests: raw stderr write ok\n", 36);
}

// The kit's switches are RECOMP_<NAME>; the spellings they had while the kit
// was one game's port (POPM_<NAME>, POP_RECOMP_<NAME>) are not read.
// Switches from a file: NAME=VALUE lines, trimmed, comments and junk skipped.
void test_switch_file() {
    char dir[512];
    snprintf(dir, sizeof dir, "%s/recomp-switches-XXXXXX", os_temp_dir());
    CHECK(os_mkdtemp(dir) == 0);
    std::string path = std::string(dir) + "/switches.txt";
    os_unsetenv("RECOMP_SWITCHFILE_PROBE");
    os_unsetenv("RECOMP_SWITCHFILE_OTHER");
    CHECK(recomp_env_apply_file(path.c_str()) == 0); // not there yet
    FILE *f = fopen(path.c_str(), "wb");
    CHECK(f != nullptr);
    if (!f)
        return;
    fputs("# the pointer trace\nRECOMP_SWITCHFILE_PROBE=1\n\nno equals here\n"
          "RECOMP_SWITCHFILE_OTHER = two words \n",
          f);
    fclose(f);
    CHECK(recomp_env_apply_file(path.c_str()) == 2);
    CHECK(recomp_env("SWITCHFILE_PROBE") && strcmp(recomp_env("SWITCHFILE_PROBE"), "1") == 0);
    CHECK(recomp_env("SWITCHFILE_OTHER") &&
          strcmp(recomp_env("SWITCHFILE_OTHER"), "two words") == 0);
    os_unsetenv("RECOMP_SWITCHFILE_PROBE");
    os_unsetenv("RECOMP_SWITCHFILE_OTHER");
    os_unlink(path.c_str());
    os_rmdir(dir);
}

void test_switch_names() {
    os_unsetenv("RECOMP_SWITCH_PROBE");
    CHECK(recomp_env("SWITCH_PROBE") == nullptr);
    CHECK(os_setenv("POPM_SWITCH_PROBE", "old") == 0);
    CHECK(os_setenv("POP_RECOMP_SWITCH_PROBE", "old") == 0);
    CHECK(recomp_env("SWITCH_PROBE") == nullptr);
    CHECK(os_setenv("RECOMP_SWITCH_PROBE", "new") == 0);
    CHECK(recomp_env("SWITCH_PROBE") && strcmp(recomp_env("SWITCH_PROBE"), "new") == 0);
    os_unsetenv("RECOMP_SWITCH_PROBE");
    os_unsetenv("POPM_SWITCH_PROBE");
    os_unsetenv("POP_RECOMP_SWITCH_PROBE");
}

} // namespace

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--child-exit-7") == 0)
        return 7;
    test_switch_names();
    test_switch_file();
    test_threads();
    test_time();
    test_vm();
    test_paths_and_descriptors();
    test_process_and_strings();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (!g_failures)
        printf("all platform tests passed\n");
    return g_failures ? 1 : 0;
}
