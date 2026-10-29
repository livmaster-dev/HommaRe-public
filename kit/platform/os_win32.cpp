// os_win32.cpp - the platform layer on Windows. Compiled by clang's GNU
// driver against the MSVC ABI and the Windows SDK; the only first-party file
// besides os_posix.cpp that includes a platform header. Paths are UTF-8 at
// the API and UTF-16 at the system calls.
#include "os.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <string>

// A Windows presentation-clock backend is not implemented yet. Do not report
// producer or buffer-consumption counts as an output-device clock.
void os_audio_output_set_provider(const OsAudioOutputProvider *) {}
int os_audio_outputs_open(void) {
    return 0; // no output backend here yet
}
void os_audio_output_set_idle_callback(void (*)(void)) {}
void os_audio_on_service_restart(void (*)(void)) {}
OsAudioOutput *os_audio_output_open(uint32_t, OsAudioLayout, int *native_error) {
    if (native_error)
        *native_error = -1;
    return nullptr;
}
int os_audio_output_write(OsAudioOutput *, const int16_t *, uint32_t) {
    return -1;
}
int os_audio_output_start(OsAudioOutput *) {
    return -1;
}
int os_audio_output_pause(OsAudioOutput *) {
    return -1;
}
int os_audio_output_set_gain(OsAudioOutput *, float) {
    return -1;
}
int os_audio_output_get_gain(OsAudioOutput *, float *) {
    return -1;
}
int os_audio_output_clock(OsAudioOutput *, OsAudioClock *clock) {
    if (clock) {
        *clock = {};
        clock->native_status = -1;
    }
    return -1;
}
int os_audio_output_reset(OsAudioOutput *) {
    return -1;
}
void os_audio_output_close(OsAudioOutput *) {}
int os_audio_output_route(OsAudioOutput *, OsAudioRoute *) {
    return -1;
}
int os_audio_render_probe_install(uint32_t) {
    return -1;
}
size_t os_audio_render_probe_read(OsAudioRenderRecord *, size_t) {
    return 0;
}
int os_audio_marker_install(uint32_t, uint32_t) {
    return -1;
}
size_t os_audio_marker_read(OsAudioMarkerEdge *, size_t) {
    return 0;
}

// MinGW's headers predate these RegGetValue flags.
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000
#endif
#ifndef RRF_SUBKEY_WOW6432KEY
#define RRF_SUBKEY_WOW6432KEY 0x00020000
#endif

namespace {

std::wstring widen(const char *utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

std::string narrow(const wchar_t *wide) {
    int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? (size_t)n - 1 : 0, '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// FILETIME counts 100 ns ticks since 1601; the API speaks Unix seconds.
int64_t filetime_seconds(const FILETIME &ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)(u.QuadPart / 10000000ull) - 11644473600ll;
}

int copy_out(const std::string &s, char *buf, size_t cap) {
    if (s.size() + 1 > cap)
        return -1;
    memcpy(buf, s.c_str(), s.size() + 1);
    for (char *c = buf; *c; ++c)
        if (*c == '\\')
            *c = '/';
    return 0;
}

// The path of a loaded executable or DLL (nullptr: the executable).
int module_file(HMODULE m, char *buf, size_t cap) {
    wchar_t w[32768];
    DWORD n = GetModuleFileNameW(m, w, 32768);
    if (!n || n >= 32768)
        return -1;
    return copy_out(narrow(w), buf, cap);
}

struct ThreadStart {
    void *(*fn)(void *);
    void *arg;
};

unsigned __stdcall trampoline(void *p) {
    ThreadStart start = *(ThreadStart *)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

char g_dlerror[256];
void remember_dlerror() {
    snprintf(g_dlerror, sizeof g_dlerror, "Windows error %lu", (unsigned long)GetLastError());
}

int native_flags(int flags) {
    int f = _O_BINARY;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f |= _O_WRONLY;
        break;
    case OS_O_RDWR:
        f |= _O_RDWR;
        break;
    default:
        f |= _O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= _O_CREAT;
    if (flags & OS_O_EXCL)
        f |= _O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= _O_TRUNC;
    return f;
}

int stat_attributes(const wchar_t *path, OsStat *out, bool follow) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d))
        return -1;
    bool reparse = (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (follow && reparse) {
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            return -1;
        BY_HANDLE_FILE_INFORMATION info;
        BOOL ok = GetFileInformationByHandle(h, &info);
        CloseHandle(h);
        if (!ok)
            return -1;
        d.dwFileAttributes = info.dwFileAttributes;
        d.ftCreationTime = info.ftCreationTime;
        d.ftLastAccessTime = info.ftLastAccessTime;
        d.ftLastWriteTime = info.ftLastWriteTime;
        d.nFileSizeHigh = info.nFileSizeHigh;
        d.nFileSizeLow = info.nFileSizeLow;
        reparse = false;
    }
    out->size = ((uint64_t)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    out->atime = filetime_seconds(d.ftLastAccessTime);
    out->mtime = filetime_seconds(d.ftLastWriteTime);
    out->ctime = filetime_seconds(d.ftCreationTime);
    out->is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    out->is_symlink = reparse ? 1 : 0;
    out->is_regular = (!out->is_dir && !reparse) ? 1 : 0;
    out->is_readonly = (d.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 1 : 0;
    return 0;
}

} // namespace

struct OsThread {
    HANDLE handle;
    DWORD id;
};

extern "C" {

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    ThreadStart *start = (ThreadStart *)malloc(sizeof *start);
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!start || !t) {
        free(start);
        free(t);
        return nullptr;
    }
    start->fn = fn;
    start->arg = arg;
    unsigned id = 0;
    uintptr_t h = _beginthreadex(nullptr, (unsigned)stack_bytes, trampoline, start, 0, &id);
    if (!h) {
        free(start);
        free(t);
        return nullptr;
    }
    t->handle = (HANDLE)h;
    t->id = id;
    return t;
}
void os_thread_join(OsThread *t) {
    WaitForSingleObject(t->handle, INFINITE);
    CloseHandle(t->handle);
    free(t);
}
void os_thread_detach(OsThread *t) {
    CloseHandle(t->handle);
    free(t);
}
uint64_t os_thread_cpu_ns(void) {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    const uint64_t k = (uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime;
    const uint64_t u = (uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime;
    return (k + u) * 100;
}
const char *os_thread_class_name(void) {
    return "unknown";
}
void os_thread_set_time_critical_wake(void) {}
void os_thread_prefer_performance(void) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
}
void os_thread_exit(void) {
    _endthreadex(0);
}
OsThreadId os_thread_self(void) {
    return GetCurrentThreadId();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return t->id;
}

void *os_vm_reserve(size_t bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void *os_vm_reserve_at(void *at, size_t bytes) {
    void *p = VirtualAlloc(at, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p && p != at) {
        VirtualFree(p, 0, MEM_RELEASE);
        return nullptr;
    }
    return p;
}
void os_vm_release(void *p, size_t bytes) {
    (void)bytes;
    VirtualFree(p, 0, MEM_RELEASE);
}

void *os_dlopen(const char *path) {
    // LoadLibrary wants backslashes; paths here come in '/'.
    std::wstring w = widen(path);
    for (wchar_t &c : w)
        if (c == L'/')
            c = L'\\';
    HMODULE m = LoadLibraryW(w.c_str());
    if (!m)
        remember_dlerror();
    return m;
}
void *os_dlopen_noload(const char *path) {
    return GetModuleHandleW(widen(path).c_str());
}
void *os_dlsym(void *handle, const char *name) {
    return (void *)GetProcAddress((HMODULE)handle, name);
}
int os_dlclose(void *handle) {
    return FreeLibrary((HMODULE)handle) ? 0 : -1;
}
const char *os_dlerror(void) {
    return g_dlerror[0] ? g_dlerror : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
    return ".dll";
}

int os_stat(const char *path, OsStat *out) {
    return stat_attributes(widen(path).c_str(), out, true);
}
int os_lstat(const char *path, OsStat *out) {
    return stat_attributes(widen(path).c_str(), out, false);
}
int os_mkdir(const char *path) {
    return CreateDirectoryW(widen(path).c_str(), nullptr) ? 0 : -1;
}
int os_rename(const char *from, const char *to) {
    return MoveFileExW(widen(from).c_str(), widen(to).c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
}
int os_unlink(const char *path) {
    return DeleteFileW(widen(path).c_str()) ? 0 : -1;
}
int os_rmdir(const char *path) {
    return RemoveDirectoryW(widen(path).c_str()) ? 0 : -1;
}
int os_getcwd(char *buf, size_t cap) {
    wchar_t w[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW(sizeof w / sizeof w[0], w);
    if (!n || n >= sizeof w / sizeof w[0])
        return -1;
    return copy_out(narrow(w), buf, cap);
}
int os_chdir(const char *path) {
    return SetCurrentDirectoryW(widen(path).c_str()) ? 0 : -1;
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    std::wstring pattern = widen(dir) + L"\\*";
    WIN32_FIND_DATAW f;
    HANDLE h = FindFirstFileW(pattern.c_str(), &f);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    do {
        if (wcscmp(f.cFileName, L".") == 0 || wcscmp(f.cFileName, L"..") == 0)
            continue;
        if (fn(narrow(f.cFileName).c_str(), user) != 0)
            break;
    } while (FindNextFileW(h, &f));
    FindClose(h);
    return 0;
}
int os_mkstemp(char *template_path) {
    size_t len = strlen(template_path);
    if (len < 6 || strcmp(template_path + len - 6, "XXXXXX") != 0)
        return -1;
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)GetTickCount() ^ (GetCurrentThreadId() * 2654435761u);
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (int i = 0; i < 6; ++i) {
            seed = seed * 1103515245u + 12345u;
            template_path[len - 6 + i] = alphabet[(seed >> 16) % 36];
        }
        int fd = _wopen(widen(template_path).c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                        _S_IREAD | _S_IWRITE);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

int os_mkdtemp(char *template_path) {
    size_t len = strlen(template_path);
    if (len < 6 || strcmp(template_path + len - 6, "XXXXXX") != 0)
        return -1;
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned seed = (unsigned)GetTickCount() ^ (GetCurrentThreadId() * 2654435761u);
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (size_t i = len - 6; i < len; ++i) {
            seed = seed * 1103515245u + 12345u;
            template_path[i] = alphabet[(seed >> 16) % (sizeof alphabet - 1)];
        }
        if (CreateDirectoryW(widen(template_path).c_str(), nullptr))
            return 0;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return -1;
    }
    return -1;
}

const char *os_temp_dir(void) {
    static char buf[MAX_PATH * 3];
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, w);
    if (!n || n > MAX_PATH)
        return ".";
    while (n > 1 && (w[n - 1] == L'\\' || w[n - 1] == L'/'))
        w[--n] = 0;
    if (copy_out(narrow(w), buf, sizeof buf) != 0)
        return ".";
    return buf;
}

const char *os_null_device(void) {
    return "NUL";
}

int os_free_space(const char *path, uint64_t *bytes_out) {
    ULARGE_INTEGER available;
    if (!bytes_out || !GetDiskFreeSpaceExW(widen(path).c_str(), &available, nullptr, nullptr))
        return -1;
    *bytes_out = available.QuadPart;
    return 0;
}
int os_set_mtime(const char *path, int64_t mtime) {
    HANDLE h = CreateFileW(widen(path).c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    ULARGE_INTEGER u;
    u.QuadPart = uint64_t(mtime) * 10000000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart};
    const BOOL ok = SetFileTime(h, nullptr, &ft, &ft);
    CloseHandle(h);
    return ok ? 0 : -1;
}
int os_registry_read(const char *key, const char *value, char *buf, size_t cap) {
    if (!key || !value || !buf || !cap)
        return -1;
    HKEY root = nullptr;
    const char *path = nullptr;
    if (strncmp(key, "HKLM\\", 5) == 0) {
        root = HKEY_LOCAL_MACHINE;
        path = key + 5;
    } else if (strncmp(key, "HKCU\\", 5) == 0) {
        root = HKEY_CURRENT_USER;
        path = key + 5;
    } else
        return -1;
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        wchar_t data[4096];
        DWORD size = sizeof data;
        LSTATUS st = RegGetValueW(
            root, widen(path).c_str(), widen(value).c_str(),
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ |
                (view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : RRF_SUBKEY_WOW6432KEY),
            nullptr, data, &size);
        if (st != ERROR_SUCCESS)
            continue;
        const std::string out = narrow(data);
        if (out.size() + 1 > cap)
            return -1;
        memcpy(buf, out.c_str(), out.size() + 1);
        return 0;
    }
    return -1;
}
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    wchar_t w[MAX_PATH + 1];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", w, MAX_PATH + 1);
    if (!n || n > MAX_PATH)
        return -1;
    // In '/', like every other path this layer hands out: callers split there.
    return copy_out(narrow(w) + "/" + app, buf, cap);
}

int os_spawn(const char *const argv[], int64_t *pid_out) {
    std::wstring cmd;
    for (int i = 0; argv[i]; ++i) {
        std::wstring a = widen(argv[i]);
        if (i)
            cmd += L' ';
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
            cmd += a;
        else {
            cmd += L'"';
            for (wchar_t ch : a)
                cmd += ch == L'"' ? std::wstring(L"\\\"") : std::wstring(1, ch);
            cmd += L'"';
        }
    }
    STARTUPINFOW si = {};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(widen(argv[0]).c_str(), cmd.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    *pid_out = (int64_t)(intptr_t)pi.hProcess;
    return 0;
}

int os_wait(int64_t pid, int *exit_code) {
    HANDLE h = (HANDLE)(intptr_t)pid;
    if (WaitForSingleObject(h, INFINITE) != WAIT_OBJECT_0)
        return -1;
    DWORD code = 0;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    // abort() exits with 3 under a debug C runtime and fails fast with
    // STATUS_STACK_BUFFER_OVERRUN (0xC0000409, FAST_FAIL_FATAL_APP_EXIT) under a
    // release one; report both as POSIX SIGABRT so tests agree.
    *exit_code = code == 3 || code == 0xC0000409u ? 134 : (int)code;
    return 0;
}

int os_fd_open(const char *path, int flags) {
    return _wopen(widen(path).c_str(), native_flags(flags), _S_IREAD | _S_IWRITE);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    // _read takes an unsigned count; loop so a large guest read still works.
    int64_t total = 0;
    char *p = (char *)buf;
    while (n) {
        unsigned chunk = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        int got = _read(fd, p, chunk);
        if (got < 0)
            return total ? total : -1;
        total += got;
        p += got;
        n -= (size_t)got;
        if ((unsigned)got < chunk)
            break;
    }
    return total;
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    int64_t total = 0;
    const char *p = (const char *)buf;
    while (n) {
        unsigned chunk = n > (1u << 30) ? (1u << 30) : (unsigned)n;
        int put = _write(fd, p, chunk);
        if (put < 0)
            return total ? total : -1;
        total += put;
        p += put;
        n -= (size_t)put;
        if ((unsigned)put < chunk)
            break;
    }
    return total;
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return _lseeki64(fd, off, whence);
}
int os_fd_close(int fd) {
    return _close(fd);
}
int os_fd_dup(int fd) {
    return _dup(fd);
}
int os_fd_fsync(int fd) {
    return _commit(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return _chsize_s(fd, length) == 0 ? 0 : -1;
}
int os_fd_stat(int fd, OsStat *out) {
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0)
        return -1;
    out->size = (uint64_t)st.st_size;
    out->atime = st.st_atime;
    out->mtime = st.st_mtime;
    out->ctime = st.st_ctime;
    out->is_dir = (st.st_mode & _S_IFDIR) ? 1 : 0;
    out->is_regular = (st.st_mode & _S_IFREG) ? 1 : 0;
    out->is_symlink = 0;
    out->is_readonly = (st.st_mode & _S_IWRITE) ? 0 : 1;
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return _fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
    return module_file(nullptr, buf, cap);
}
void os_attach_parent_console(void) {
    // Redirected (2> log.txt, a pipe, Wine's own terminal): keep that.
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return; // started from Explorer: nowhere to write
    // The C streams and the handles os_write_stderr_raw writes to.
    if (freopen("CONOUT$", "w", stdout))
        SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(stdout)));
    if (freopen("CONOUT$", "w", stderr))
        SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(stderr)));
}
int os_module_path(const void *address, char *buf, size_t cap) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)address, &m))
        return -1;
    return module_file(m, buf, cap);
}
// The unhandled-exception filter: the fault address relative to this
// executable's base (so a linker map resolves it), the top of the stack as
// candidate return addresses, and the registered callback for the guest EIP.
static OsFaultFn g_fault_fn;
static LONG WINAPI fault_filter(EXCEPTION_POINTERS *info) {
    char line[256];
    const EXCEPTION_RECORD *rec = info->ExceptionRecord;
    const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    const uintptr_t at = (uintptr_t)rec->ExceptionAddress;
    int n = snprintf(line, sizeof line,
                     "[host] exception %08lx at %016llx (exe base %016llx, rva %llx)\n",
                     (unsigned long)rec->ExceptionCode, (unsigned long long)at,
                     (unsigned long long)base, (unsigned long long)(at - base));
    os_write_stderr_raw(line, (size_t)n);
    const uintptr_t *sp = (const uintptr_t *)info->ContextRecord->Rsp;
    for (int i = 0; i < 24; ++i) {
        uintptr_t v = 0;
        if (IsBadReadPtr(sp + i, sizeof v))
            break;
        v = sp[i];
        if (v >= base && v < base + (256u << 20)) {
            n = snprintf(line, sizeof line, "[host]   stack[%d] rva %llx\n", i,
                         (unsigned long long)(v - base));
            os_write_stderr_raw(line, (size_t)n);
        }
    }
    if (g_fault_fn)
        g_fault_fn(rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? "access violation"
                                                                    : "exception");
    return EXCEPTION_EXECUTE_HANDLER;
}

int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    SetUnhandledExceptionFilter(fault_filter);
    return 1;
}
void os_write_stderr_raw(const char *s, size_t n) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), s, (DWORD)n, &written, nullptr);
}
void os_exit_immediately(int code) {
    _exit(code);
}

int os_localtime(int64_t seconds, struct tm *out) {
    __time64_t t = (__time64_t)seconds;
    return _localtime64_s(out, &t) == 0 ? 0 : -1;
}
int os_gmtime(int64_t seconds, struct tm *out) {
    __time64_t t = (__time64_t)seconds;
    return _gmtime64_s(out, &t) == 0 ? 0 : -1;
}

uint64_t os_monotonic_ns(void) {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)freq.QuadPart);
}
uint64_t os_wall_time_us(void) {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10ull - 11644473600000000ull;
}
void os_sleep_us(uint64_t us) {
    Sleep((DWORD)((us + 999) / 1000));
}

// ---------------------------------------------------------------------------
// Sockets on Winsock 2. FIONBIO is set at creation, so every call answers at
// once and the caller does the waiting.
// ---------------------------------------------------------------------------
static thread_local OsNetError t_net_error = OS_NET_OK;

static OsNetError net_error_of(int e) {
    switch (e) {
    case 0:
        return OS_NET_OK;
    case WSAEWOULDBLOCK:
        return OS_NET_WOULDBLOCK;
    case WSAEINPROGRESS:
        return OS_NET_INPROGRESS;
    case WSAEALREADY:
        return OS_NET_ALREADY;
    case WSAEISCONN:
        return OS_NET_ISCONN;
    case WSAENOTCONN:
    case WSAEDESTADDRREQ:
        return OS_NET_NOTCONN;
    case WSAEADDRINUSE:
        return OS_NET_ADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return OS_NET_ADDRNOTAVAIL;
    case WSAEACCES:
        return OS_NET_ACCES;
    case WSAECONNREFUSED:
        return OS_NET_CONNREFUSED;
    case WSAECONNRESET:
        return OS_NET_CONNRESET;
    case WSAECONNABORTED:
        return OS_NET_CONNABORTED;
    case WSAENETUNREACH:
        return OS_NET_NETUNREACH;
    case WSAEHOSTUNREACH:
        return OS_NET_HOSTUNREACH;
    case WSAENETDOWN:
        return OS_NET_NETDOWN;
    case WSAETIMEDOUT:
        return OS_NET_TIMEDOUT;
    case WSAEMSGSIZE:
        return OS_NET_MSGSIZE;
    case WSAENOBUFS:
        return OS_NET_NOBUFS;
    case WSAESHUTDOWN:
        return OS_NET_SHUTDOWN;
    case WSAEINVAL:
    case WSAEAFNOSUPPORT:
    case WSAEFAULT:
        return OS_NET_INVAL;
    case WSAENOTSOCK:
        return OS_NET_NOTSOCK;
    case WSAEMFILE:
        return OS_NET_MFILE;
    case WSAENOPROTOOPT:
        return OS_NET_NOPROTOOPT;
    default:
        return OS_NET_OTHER;
    }
}
static int net_fail(void) {
    t_net_error = net_error_of(WSAGetLastError());
    return -1;
}
static sockaddr_in net_sockaddr(uint32_t addr, uint16_t port) {
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr;
    sa.sin_port = port;
    return sa;
}
static void net_split(const sockaddr_in &sa, uint32_t *addr, uint16_t *port) {
    if (addr)
        *addr = sa.sin_addr.s_addr;
    if (port)
        *port = sa.sin_port;
}
static int net_msg_flags(int flags) {
    return ((flags & OS_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & OS_MSG_OOB) ? MSG_OOB : 0);
}

OsNetError os_net_error(void) {
    return t_net_error;
}
int os_net_startup(void) {
    static int started = -1;
    if (started < 0) {
        WSADATA data;
        started = WSAStartup(MAKEWORD(2, 2), &data) == 0 ? 1 : 0;
    }
    if (!started)
        t_net_error = OS_NET_NETDOWN;
    return started ? 0 : -1;
}
OsSocket os_sock_open(int type) {
    if (os_net_startup() != 0)
        return OS_SOCKET_INVALID;
    SOCKET s = socket(AF_INET, type == OS_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET)
        return net_fail();
    u_long on = 1;
    if (ioctlsocket(s, FIONBIO, &on) != 0) {
        net_fail();
        closesocket(s);
        return OS_SOCKET_INVALID;
    }
    if (type != OS_SOCK_STREAM) {
        // An ICMP port-unreachable would otherwise fail the next recvfrom
        // with WSAECONNRESET, which POSIX never reports on an unconnected socket.
        BOOL off = FALSE;
        DWORD bytes = 0;
        WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, nullptr, 0,
                 &bytes, nullptr, nullptr);
    }
    return (OsSocket)s;
}
int os_sock_close(OsSocket s) {
    return closesocket((SOCKET)s) == 0 ? 0 : net_fail();
}
int os_sock_bind(OsSocket s, uint32_t addr, uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    return bind((SOCKET)s, (sockaddr *)&sa, sizeof sa) == 0 ? 0 : net_fail();
}
int os_sock_listen(OsSocket s, int backlog) {
    return listen((SOCKET)s, backlog) == 0 ? 0 : net_fail();
}
OsSocket os_sock_accept(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    int len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    SOCKET a = accept((SOCKET)s, (sockaddr *)&sa, &len);
    if (a == INVALID_SOCKET)
        return net_fail();
    u_long on = 1; // inherited from the listener, but say so
    ioctlsocket(a, FIONBIO, &on);
    net_split(sa, addr, port);
    return (OsSocket)a;
}
int os_sock_connect(OsSocket s, uint32_t addr, uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    if (connect((SOCKET)s, (sockaddr *)&sa, sizeof sa) == 0)
        return 0;
    net_fail();
    // Winsock says WSAEWOULDBLOCK where POSIX says EINPROGRESS.
    if (t_net_error == OS_NET_WOULDBLOCK)
        t_net_error = OS_NET_INPROGRESS;
    return -1;
}
int64_t os_sock_send(OsSocket s, const void *buf, size_t n, int flags) {
    int r = send((SOCKET)s, (const char *)buf, (int)n, net_msg_flags(flags));
    return r != SOCKET_ERROR ? (int64_t)r : net_fail();
}
int64_t os_sock_sendto(OsSocket s, const void *buf, size_t n, int flags, uint32_t addr,
                       uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    int r = sendto((SOCKET)s, (const char *)buf, (int)n, net_msg_flags(flags), (sockaddr *)&sa,
                   sizeof sa);
    return r != SOCKET_ERROR ? (int64_t)r : net_fail();
}
int64_t os_sock_recv(OsSocket s, void *buf, size_t n, int flags) {
    int r = recv((SOCKET)s, (char *)buf, (int)n, net_msg_flags(flags));
    return r != SOCKET_ERROR ? (int64_t)r : net_fail();
}
// Winsock fills the buffer and fails with WSAEMSGSIZE when a datagram did not
// fit; this layer reports that as a truncated success, as POSIX does.
int64_t os_sock_recvfrom(OsSocket s, void *buf, size_t n, int flags, uint32_t *addr, uint16_t *port,
                         int *truncated) {
    sockaddr_in sa;
    int len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int r = recvfrom((SOCKET)s, (char *)buf, (int)n, net_msg_flags(flags), (sockaddr *)&sa, &len);
    int cut = 0;
    if (r == SOCKET_ERROR) {
        if (WSAGetLastError() != WSAEMSGSIZE)
            return net_fail();
        r = (int)n;
        cut = 1;
    }
    if (truncated)
        *truncated = cut;
    net_split(sa, addr, port);
    return (int64_t)r;
}
int os_sock_shutdown(OsSocket s, int how) {
    int h = how == 0 ? SD_RECEIVE : how == 1 ? SD_SEND : SD_BOTH;
    return shutdown((SOCKET)s, h) == 0 ? 0 : net_fail();
}
static bool net_option(OsSockOption option, int *level, int *name) {
    *level = SOL_SOCKET;
    switch (option) {
    case OS_SO_REUSEADDR:
        *name = SO_REUSEADDR;
        return true;
    case OS_SO_BROADCAST:
        *name = SO_BROADCAST;
        return true;
    case OS_SO_KEEPALIVE:
        *name = SO_KEEPALIVE;
        return true;
    case OS_SO_DONTROUTE:
        *name = SO_DONTROUTE;
        return true;
    case OS_SO_OOBINLINE:
        *name = SO_OOBINLINE;
        return true;
    case OS_SO_SNDBUF:
        *name = SO_SNDBUF;
        return true;
    case OS_SO_RCVBUF:
        *name = SO_RCVBUF;
        return true;
    case OS_TCP_NODELAY:
        *level = IPPROTO_TCP;
        *name = TCP_NODELAY;
        return true;
    case OS_IP_TTL:
        *level = IPPROTO_IP;
        *name = IP_TTL;
        return true;
    default:
        return false;
    }
}
int os_sock_set_option(OsSocket s, OsSockOption option, int value) {
    if (option == OS_SO_LINGER) {
        linger l;
        l.l_onoff = value >= 0;
        l.l_linger = (u_short)(value >= 0 ? value : 0);
        return setsockopt((SOCKET)s, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof l) == 0
                   ? 0
                   : net_fail();
    }
    int level = 0, name = 0;
    if (!net_option(option, &level, &name)) {
        t_net_error = OS_NET_NOPROTOOPT;
        return -1;
    }
    int v = value;
    if (option != OS_SO_SNDBUF && option != OS_SO_RCVBUF && option != OS_IP_TTL)
        v = value ? 1 : 0;
    return setsockopt((SOCKET)s, level, name, (const char *)&v, sizeof v) == 0 ? 0 : net_fail();
}
int os_sock_get_option(OsSocket s, OsSockOption option, int *value) {
    if (option == OS_SO_LINGER) {
        linger l;
        int len = sizeof l;
        if (getsockopt((SOCKET)s, SOL_SOCKET, SO_LINGER, (char *)&l, &len) != 0)
            return net_fail();
        *value = l.l_onoff ? l.l_linger : -1;
        return 0;
    }
    if (option == OS_SO_ERROR) {
        int v = 0, len = sizeof v;
        if (getsockopt((SOCKET)s, SOL_SOCKET, SO_ERROR, (char *)&v, &len) != 0)
            return net_fail();
        *value = (int)net_error_of(v);
        return 0;
    }
    int level = 0, name = 0;
    if (!net_option(option, &level, &name)) {
        t_net_error = OS_NET_NOPROTOOPT;
        return -1;
    }
    int v = 0, len = sizeof v;
    if (getsockopt((SOCKET)s, level, name, (char *)&v, &len) != 0)
        return net_fail();
    *value = v;
    return 0;
}
int os_sock_name(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    int len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getsockname((SOCKET)s, (sockaddr *)&sa, &len) != 0)
        return net_fail();
    net_split(sa, addr, port);
    return 0;
}
int os_sock_peer(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    int len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getpeername((SOCKET)s, (sockaddr *)&sa, &len) != 0)
        return net_fail();
    net_split(sa, addr, port);
    return 0;
}
int os_sock_pending(OsSocket s, uint32_t *bytes) {
    u_long n = 0;
    if (ioctlsocket((SOCKET)s, FIONREAD, &n) != 0)
        return net_fail();
    *bytes = (uint32_t)n;
    return 0;
}
int os_sock_poll(OsSockPoll *set, size_t count, int timeout_ms) {
    if (count == 0) {
        if (timeout_ms > 0)
            Sleep((DWORD)timeout_ms);
        return 0;
    }
    WSAPOLLFD stack[64];
    WSAPOLLFD *fds = count <= 64 ? stack : (WSAPOLLFD *)malloc(count * sizeof(WSAPOLLFD));
    if (!fds) {
        t_net_error = OS_NET_NOBUFS;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        fds[i].fd = (SOCKET)set[i].sock;
        // WSAPoll rejects POLLPRI; out-of-band data is not asked for here.
        fds[i].events = (SHORT)(((set[i].events & OS_POLL_IN) ? POLLRDNORM : 0) |
                                ((set[i].events & OS_POLL_OUT) ? POLLWRNORM : 0));
        fds[i].revents = 0;
    }
    int r = WSAPoll(fds, (ULONG)count, timeout_ms);
    if (r == SOCKET_ERROR) {
        net_fail();
        r = -1;
    }
    for (size_t i = 0; r >= 0 && i < count; ++i) {
        const SHORT e = fds[i].revents;
        set[i].revents =
            ((e & POLLRDNORM) ? OS_POLL_IN : 0) | ((e & POLLWRNORM) ? OS_POLL_OUT : 0) |
            ((e & (POLLERR | POLLNVAL)) ? OS_POLL_ERR : 0) | ((e & POLLHUP) ? OS_POLL_HUP : 0);
    }
    if (fds != stack)
        free(fds);
    return r;
}
int os_net_hostname(char *buf, size_t cap) {
    if (!buf || cap == 0 || os_net_startup() != 0 || gethostname(buf, (int)cap) != 0)
        return -1;
    buf[cap - 1] = 0;
    return 0;
}
int os_net_resolve(const char *name, uint32_t *addrs, int max) {
    if (!name || os_net_startup() != 0)
        return -1;
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *list = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &list) != 0)
        return -1;
    int n = 0;
    for (addrinfo *a = list; a && n < max; a = a->ai_next)
        if (a->ai_family == AF_INET && a->ai_addr)
            addrs[n++] = ((sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(list);
    return n;
}
int os_net_interfaces(OsNetInterface *out, int max) {
    OsSocket s = os_sock_open(OS_SOCK_DGRAM);
    if (s == OS_SOCKET_INVALID)
        return -1;
    INTERFACE_INFO list[64];
    DWORD bytes = 0;
    int r = WSAIoctl((SOCKET)s, SIO_GET_INTERFACE_LIST, nullptr, 0, list, sizeof list, &bytes,
                     nullptr, nullptr);
    if (r != 0)
        net_fail();
    closesocket((SOCKET)s);
    if (r != 0)
        return -1;
    int n = 0;
    for (DWORD k = 0; k < bytes / sizeof(INTERFACE_INFO) && n < max; ++k) {
        const INTERFACE_INFO &ii = list[k];
        OsNetInterface &i = out[n++];
        memset(&i, 0, sizeof i);
        i.addr = ii.iiAddress.AddressIn.sin_addr.s_addr;
        i.netmask = ii.iiNetmask.AddressIn.sin_addr.s_addr;
        const u_long f = ii.iiFlags;
        i.flags = ((f & IFF_UP) ? OS_IF_UP : 0) | ((f & IFF_BROADCAST) ? OS_IF_BROADCAST : 0) |
                  ((f & IFF_LOOPBACK) ? OS_IF_LOOPBACK : 0) |
                  ((f & IFF_POINTTOPOINT) ? OS_IF_POINTTOPOINT : 0) |
                  ((f & IFF_MULTICAST) ? OS_IF_MULTICAST : 0);
        // Winsock reports 255.255.255.255 here; the directed form is what
        // reaches one subnet, so work it out.
        if (f & IFF_BROADCAST)
            i.broadcast = i.addr | ~i.netmask;
    }
    return n;
}

int os_strcasecmp(const char *a, const char *b) {
    return _stricmp(a, b);
}

int os_setenv(const char *name, const char *value) {
    return _putenv_s(name, value) == 0 ? 0 : -1;
}
int os_unsetenv(const char *name) {
    return _putenv_s(name, "") == 0 ? 0 : -1;
}

} // extern "C"
