#include "seh.h"
#include "loader.h"
#include "sha256.h"

// From the generated table (tools/recomp/translate.py); absent in hosts
// without one.
// Weak defaults, which the generated table replaces.
extern "C" {
extern const uint32_t recomp_operand_redirect_pairs[];
extern const uint32_t recomp_operand_redirect_count;
__attribute__((weak)) const uint32_t recomp_operand_redirect_pairs[2] = {0, 0};
__attribute__((weak)) const uint32_t recomp_operand_redirect_count = 0;
extern const uint32_t recomp_data_seed_pairs[];
extern const uint32_t recomp_data_seed_count;
__attribute__((weak)) const uint32_t recomp_data_seed_pairs[2] = {0, 0};
__attribute__((weak)) const uint32_t recomp_data_seed_count = 0;
}
#include "memory.h"
#include "imports.h"
#include "guest.h"
#include "win32.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <algorithm>
#include "../platform/os.h"

const char *const LOADER_DEFAULT_EXE = RECOMP_DEVELOPER_EXE;
// The digest of the image actually mapped, recorded by loader_load.
static std::string g_exe_sha;

const char *const LOADER_EXPECTED_SHA256 = RECOMP_EXE_SHA256;

namespace {

std::string g_error;
std::string g_exe_path;
uint32_t g_base = 0, g_size = 0, g_entry = 0, g_iat_patched = 0, g_iat_data = 0;
std::vector<SectionInfo> g_sections;
LoaderModule g_main;
X86 g_ctx;
LoaderTls g_tls = {0, 0, 0, 0, 0, 0xffffffffu};

std::string sha256_hex(const std::vector<uint8_t> &data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.hex();
}

bool read_file(const char *path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        fclose(f);
        return false;
    }
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

std::string dirname_of(const std::string &p) {
    size_t s = p.find_last_of("\\/");
    if (s == std::string::npos)
        return ".";
    if (s == 0)
        return "/";
    return p.substr(0, s);
}

template <typename T> T rd(const std::vector<uint8_t> &d, size_t off) {
    T v{};
    if (off + sizeof(T) <= d.size())
        memcpy(&v, d.data() + off, sizeof(T));
    return v;
}

// True when [rva, rva+len) lies inside an image of `size` bytes.
bool rva_in(uint32_t size, uint32_t rva, uint32_t len) {
    return rva < size && len <= size - rva;
}
// True when [rva, rva+len) lies inside the mapped main image.
bool rva_ok(uint32_t rva, uint32_t len) {
    return rva_in(g_size, rva, len);
}

// Resolve the PE import table of the image mapped at [base, base+size) to
// runtime trampolines or registered guest data storage. Bound every descriptor
// and RVA by the image before reading or patching it.
bool patch_iat(const std::vector<uint8_t> &file, size_t opt_off, uint16_t opt_magic,
               uint32_t g_base, uint32_t g_size) {
    auto rva_ok = [g_size](uint32_t rva, uint32_t len) { return rva_in(g_size, rva, len); };
    size_t dd_off = opt_off + (opt_magic == 0x20b ? 112 : 96);
    if (dd_off + 16 > file.size()) {
        g_error = "PE data directories are truncated";
        return false;
    }
    uint32_t imp_rva = rd<uint32_t>(file, dd_off + 8 * 1 + 0);
    uint32_t imp_size = rd<uint32_t>(file, dd_off + 8 * 1 + 4);
    if (!imp_rva || !imp_size) {
        // A self-contained auxiliary module (pure code, no imports) is fine;
        // the game's executable always imports at least kernel32.
        if (g_base != loader_image_base())
            return true;
        g_error = "image has no import directory";
        return false;
    }
    if (!rva_ok(imp_rva, imp_size)) {
        g_error = "import directory lies outside the image";
        return false;
    }

    // One descriptor per DLL, bounded by the directory size.
    uint32_t max_desc = imp_size / 20 + 1;
    uint32_t desc_rva = imp_rva;
    for (uint32_t d = 0; d < max_desc; ++d, desc_rva += 20) {
        if (!rva_ok(desc_rva, 20)) {
            g_error = "import descriptor runs past the image";
            return false;
        }
        uint32_t desc = g_base + desc_rva;
        uint32_t orig_thunk = rd32(desc + 0);
        uint32_t name_rva = rd32(desc + 12);
        uint32_t first_thunk = rd32(desc + 16);
        if (!name_rva && !first_thunk && !orig_thunk)
            break;
        if (!rva_ok(name_rva, 1) || !rva_ok(first_thunk, 4)) {
            g_error = "import descriptor points outside the image";
            return false;
        }
        std::string dll = gm_str(g_base + name_rva, 260);

        uint32_t lookup_rva = orig_thunk ? orig_thunk : first_thunk;
        uint32_t slot_rva = first_thunk;
        for (;; lookup_rva += 4, slot_rva += 4) {
            if (!rva_ok(lookup_rva, 4) || !rva_ok(slot_rva, 4)) {
                g_error = "import thunk array runs past the image";
                return false;
            }
            uint32_t lookup = g_base + lookup_rva;
            uint32_t slot = g_base + slot_rva;
            uint32_t t = rd32(lookup);
            if (!t)
                break;
            char namebuf[280];
            if (t & 0x80000000u) {
                snprintf(namebuf, sizeof namebuf, "ord%u", t & 0xffff);
            } else {
                if (!rva_ok(t, 3)) {
                    g_error = "import name lies outside the image";
                    return false;
                }
                std::string n = gm_str(g_base + t + 2, 260);
                snprintf(namebuf, sizeof namebuf, "%s", n.c_str());
            }
            // DLLs can import code AND data from the executable. Both slots
            // must hold the original guest address, not a host trampoline or
            // a separately allocated copy of an exported global.
            if (LoaderModule *module = loader_module_named(dll.c_str())) {
                if (module->base != g_base)
                    module->bound = true;
                uint32_t address = (t & 0x80000000u)
                                       ? loader_module_export_ordinal(*module, t & 0xffff)
                                       : loader_module_export(*module, namebuf);
                if (!address) {
                    g_error = "missing mapped export " + dll + "!" + namebuf;
                    return false;
                }
                wr32(slot, address);
                ++g_iat_patched;
                continue;
            }
            uint32_t data = imports_alloc_data(dll.c_str(), namebuf);
            if (data) {
                // An imported variable, not a function: the slot holds the
                // address of zeroed guest storage.
                wr32(slot, data);
                ++g_iat_patched;
                ++g_iat_data;
                continue;
            }
            uint32_t tramp = imports_alloc_trampoline(dll.c_str(), namebuf, nullptr, ARGC_UNKNOWN);
            if (!tramp) {
                g_error = "ran out of import trampolines";
                return false;
            }
            wr32(slot, tramp);
            ++g_iat_patched;
        }
    }
    return true;
}

// --------------------------------------------------------------------------
// Auxiliary modules: DLLs game.toml names, mapped beside the main image at
// their preferred base (no relocation support, as for the image itself) and
// verified by content hash like it. Preserve configured installation subfolders
// when the executable and its game directory have moved onto another device.
// --------------------------------------------------------------------------
struct AuxSpec {
    const char *name, *path, *sha256;
    uint32_t base, size;
};
const AuxSpec g_aux_specs[] = RECOMP_AUX_MODULES;
std::vector<LoaderModule> g_aux;

bool load_aux_module(const AuxSpec &spec) {
    LoaderModule m;
    m.name = spec.name;
    m.path = dirname_of(g_exe_path) + "/" + spec.name;
    // Candidates in order: beside the executable, the configured installation
    // subfolder beside it, then the developer path. The first whose content
    // hash matches wins, so an original DLL beside the game (e.g. a stock
    // msvcr71.dll) does not shadow the rebased copy game.toml names.
    std::vector<std::string> candidates{dirname_of(g_exe_path) + "/" + spec.name};
    {
        const std::string root = std::string(RECOMP_DEVELOPER_GAME_DIR) + "/";
        const std::string configured = spec.path;
        if (configured.compare(0, root.size(), root) == 0)
            candidates.push_back(dirname_of(g_exe_path) + "/" + configured.substr(root.size()));
        candidates.push_back(configured);
    }
    std::vector<uint8_t> file;
    std::string mismatch;
    bool found = false;
    for (const std::string &path : candidates) {
        std::vector<uint8_t> bytes;
        if (!read_file(path.c_str(), bytes))
            continue;
        std::string digest = sha256_hex(bytes);
        if (digest != spec.sha256) {
            if (mismatch.empty())
                mismatch = path + " has SHA-256 " + digest + ", expected " + spec.sha256;
            continue;
        }
        m.path = path;
        file.swap(bytes);
        found = true;
        break;
    }
    if (!found) {
        g_error = !mismatch.empty() ? mismatch
                                    : std::string("cannot read auxiliary module ") + spec.name +
                                          " beside " + g_exe_path;
        return false;
    }
    if (file.size() < 0x40 || rd<uint16_t>(file, 0) != 0x5a4d) {
        g_error = m.path + " is not an MZ image";
        return false;
    }
    uint32_t pe_off = rd<uint32_t>(file, 0x3c);
    if (pe_off + 24 > file.size() || rd<uint32_t>(file, pe_off) != 0x00004550) {
        g_error = m.path + " is not a PE image";
        return false;
    }
    size_t fh_off = pe_off + 4;
    uint16_t nsections = rd<uint16_t>(file, fh_off + 2);
    uint16_t opt_size = rd<uint16_t>(file, fh_off + 16);
    size_t opt_off = fh_off + 20;
    uint16_t opt_magic = rd<uint16_t>(file, opt_off);
    if (opt_magic != 0x10b || opt_size < 96) {
        g_error = m.path + " is not a PE32 image";
        return false;
    }
    size_t sh_off = opt_off + opt_size;
    if (nsections == 0 || sh_off + 40ull * nsections > file.size()) {
        g_error = m.path + " has a truncated section table";
        return false;
    }
    uint32_t entry_rva = rd<uint32_t>(file, opt_off + 16);
    uint32_t image_base = rd<uint32_t>(file, opt_off + 28);
    uint32_t size_image = rd<uint32_t>(file, opt_off + 56);
    uint32_t size_hdrs = rd<uint32_t>(file, opt_off + 60);
    if (image_base != spec.base) {
        char buf[200];
        snprintf(buf, sizeof buf,
                 "%s: image base %08x is not the configured %08x (no relocation support)",
                 spec.name, image_base, spec.base);
        g_error = buf;
        return false;
    }
    if (size_image > spec.size || image_base < GUEST_SHIM_END ||
        (uint64_t)image_base + spec.size > GUEST_SIZE) {
        char buf[200];
        snprintf(
            buf, sizeof buf,
            "%s: [%08x, %08x) does not fit the configured [%08x, %08x) above the arena's shims",
            spec.name, image_base, image_base + size_image, spec.base, spec.base + spec.size);
        g_error = buf;
        return false;
    }
    for (const LoaderModule &other : g_aux)
        if (image_base < other.base + other.size && other.base < image_base + spec.size) {
            g_error = std::string(spec.name) + " overlaps " + other.name;
            return false;
        }
    m.base = image_base;
    m.size = spec.size;
    m.entry = entry_rva ? image_base + entry_rva : 0;
    m.attached = false;
    m.export_rva = rd<uint32_t>(file, opt_off + 96);
    m.export_size = rd<uint32_t>(file, opt_off + 100);
    if (m.export_rva && !rva_in(size_image, m.export_rva, m.export_size)) {
        g_error = std::string(spec.name) + ": export directory lies outside the image";
        return false;
    }
    if (rd<uint32_t>(file, opt_off + 92) > 9 && rd<uint32_t>(file, opt_off + 96 + 9 * 8))
        LOGW("%s has a TLS directory, which auxiliary modules do not get", spec.name);

    size_t hdr_copy = size_hdrs < file.size() ? size_hdrs : file.size();
    memcpy(g_mem + image_base, file.data(), hdr_copy);
    for (uint16_t i = 0; i < nsections; ++i) {
        size_t s = sh_off + 40 * i;
        char nm[9] = {0};
        memcpy(nm, file.data() + s, 8);
        uint32_t vsize = rd<uint32_t>(file, s + 8);
        uint32_t va = image_base + rd<uint32_t>(file, s + 12);
        uint32_t raw_size = rd<uint32_t>(file, s + 16);
        uint32_t raw_ptr = rd<uint32_t>(file, s + 20);
        uint32_t span = vsize ? vsize : raw_size;
        m.sections.push_back({nm, va, vsize, raw_size, raw_ptr, rd<uint32_t>(file, s + 36)});
        if (va < image_base || (uint64_t)va + span > (uint64_t)image_base + size_image) {
            g_error = std::string(spec.name) + ": section " + nm + " lies outside the image";
            return false;
        }
        uint32_t copy = raw_size;
        if (vsize && copy > vsize)
            copy = vsize;
        if (raw_ptr && copy) {
            if ((uint64_t)raw_ptr + copy > file.size()) {
                g_error = std::string(spec.name) + ": section " + nm + " raw data is truncated";
                return false;
            }
            memcpy(g_mem + va, file.data() + raw_ptr, copy);
        }
    }
    if (!patch_iat(file, opt_off, opt_magic, image_base, size_image)) {
        g_error = std::string(spec.name) + ": " + g_error;
        return false;
    }
    m.initial_image.assign(g_mem + image_base, g_mem + image_base + m.size);
    g_aux.push_back(m);
    LOGV("mapped auxiliary module %s: base %08x size %08x entry %08x", spec.name, m.base,
         size_image, m.entry);
    return true;
}

} // namespace

uint32_t loader_module_count() {
    return (uint32_t)g_aux.size();
}
const LoaderModule *loader_module(uint32_t i) {
    return i < g_aux.size() ? &g_aux[i] : nullptr;
}
LoaderModule *loader_module_named(const char *name) {
    if (!name)
        return nullptr;
    if (g_main.base && os_strcasecmp(g_main.name.c_str(), name) == 0)
        return &g_main;
    for (LoaderModule &m : g_aux)
        if (os_strcasecmp(m.name.c_str(), name) == 0)
            return &m;
    return nullptr;
}
const LoaderModule *loader_module_containing(uint32_t addr) {
    if (g_main.base && addr >= g_main.base && addr - g_main.base < g_main.size)
        return &g_main;
    for (const LoaderModule &m : g_aux)
        if (addr >= m.base && addr < m.base + m.size)
            return &m;
    return nullptr;
}
bool loader_in_image(uint32_t addr) {
    return (addr >= g_base && addr < g_base + g_size) || loader_module_containing(addr) != nullptr;
}

// The PE export directory read from guest memory, so a lookup sees the mapped
// bytes. Names are compared case sensitively, as GetProcAddress does.
uint32_t loader_module_export(const LoaderModule &m, const char *name) {
    if (!m.export_rva || !name || !rva_in(m.size, m.export_rva, 40))
        return 0;
    auto in = [&](uint32_t rva, uint32_t len) { return rva_in(m.size, rva, len); };
    uint32_t dir = m.base + m.export_rva;
    uint32_t nfuncs = rd32(dir + 20), nnames = rd32(dir + 24);
    uint32_t funcs = rd32(dir + 28), names = rd32(dir + 32), ords = rd32(dir + 36);
    if (nfuncs > m.size / 4 || nnames > m.size / 4 || !in(funcs, 4 * nfuncs) ||
        !in(names, 4 * nnames) || !in(ords, 2 * nnames))
        return 0;
    for (uint32_t i = 0; i < nnames; ++i) {
        uint32_t name_rva = rd32(m.base + names + 4 * i);
        if (!in(name_rva, 1) ||
            gm_str(m.base + name_rva, std::min(260u, m.size - name_rva)) != name)
            continue;
        uint32_t ordinal = rd16(m.base + ords + 2 * i);
        if (ordinal >= nfuncs)
            return 0;
        uint32_t rva = rd32(m.base + funcs + 4 * ordinal);
        // A forwarder (an RVA inside the export directory) is not served.
        if (!rva || !in(rva, 1) || rva_in(m.export_size, rva - m.export_rva, 1))
            return 0;
        return m.base + rva;
    }
    return 0;
}

// Ordinals index the same address table as names, offset by the directory's
// ordinal base. Missing entries and forwarded exports are not executable.
uint32_t loader_module_export_ordinal(const LoaderModule &m, uint32_t ordinal) {
    if (!m.export_rva || !rva_in(m.size, m.export_rva, 40))
        return 0;
    uint32_t dir = m.base + m.export_rva;
    uint32_t first = rd32(dir + 16), count = rd32(dir + 20), table = rd32(dir + 28);
    if (ordinal < first || ordinal - first >= count || count > m.size / 4 ||
        !rva_in(m.size, table, 4 * count))
        return 0;
    uint32_t rva = rd32(m.base + table + 4 * (ordinal - first));
    if (!rva || !rva_in(m.size, rva, 1) || rva_in(m.export_size, rva - m.export_rva, 1))
        return 0;
    return m.base + rva;
}

const char *loader_error() {
    return g_error.c_str();
}
uint32_t loader_image_base() {
    return g_base;
}
uint32_t loader_image_size() {
    return g_size;
}
uint32_t loader_image_limit() {
    return g_base && g_size ? g_base + g_size : 0;
}
uint32_t loader_entry_point() {
    return g_entry;
}
const std::vector<SectionInfo> &loader_sections() {
    return g_sections;
}
const std::string &loader_exe_path() {
    return g_exe_path;
}
uint32_t loader_iat_patched() {
    return g_iat_patched;
}
uint32_t loader_iat_data_imports() {
    return g_iat_data;
}
X86 *loader_context() {
    return &g_ctx;
}
const LoaderTls &loader_tls() {
    return g_tls;
}

// Verify the supported executable hash, map PE sections and bind its imports.
// All later address-based translation assumes this exact image; a mismatch is a hard failure.
bool loader_load(const char *exe_path) {
    g_error.clear();
    g_sections.clear();
    g_aux.clear();
    g_main = LoaderModule{};
    g_iat_patched = 0;
    g_iat_data = 0;
    g_exe_path = exe_path && *exe_path ? exe_path : LOADER_DEFAULT_EXE;

    std::vector<uint8_t> file;
    if (!read_file(g_exe_path.c_str(), file)) {
        g_error = "cannot read " + g_exe_path;
        return false;
    }

    // The content hash is a hard gate: there is no override. Everything below
    // trusts the layout of this exact image.
    std::string digest = sha256_hex(file);
    // Kept so a mod can key its manifest to the exact image it was built
    // against. It is the digest of what was really mapped, not of what was
    // expected, which is why it is recorded here and not made a constant.
    g_exe_sha = digest;
    if (digest != LOADER_EXPECTED_SHA256) {
        g_error = g_exe_path + " has SHA-256 " + digest + ", expected " + LOADER_EXPECTED_SHA256;
        return false;
    }

    if (file.size() < 0x40 || rd<uint16_t>(file, 0) != 0x5a4d) {
        g_error = "not an MZ image";
        return false;
    }
    uint32_t pe_off = rd<uint32_t>(file, 0x3c);
    if (pe_off + 24 > file.size() || rd<uint32_t>(file, pe_off) != 0x00004550) {
        g_error = "not a PE image";
        return false;
    }

    size_t fh_off = pe_off + 4;
    uint16_t machine = rd<uint16_t>(file, fh_off + 0);
    uint16_t nsections = rd<uint16_t>(file, fh_off + 2);
    uint16_t opt_size = rd<uint16_t>(file, fh_off + 16);
    size_t opt_off = fh_off + 20;
    if (opt_off + opt_size > file.size() || opt_size < 96) {
        g_error = "PE optional header is truncated";
        return false;
    }
    uint16_t opt_magic = rd<uint16_t>(file, opt_off);
    if (machine != 0x014c || opt_magic != 0x10b) {
        g_error = "expected a 32-bit x86 PE image";
        return false;
    }
    size_t sh_off = opt_off + opt_size;
    if (nsections == 0 || sh_off + 40ull * nsections > file.size()) {
        g_error = "PE section headers are truncated";
        return false;
    }

    uint32_t entry_rva = rd<uint32_t>(file, opt_off + 16);
    uint32_t image_base = rd<uint32_t>(file, opt_off + 28);
    uint32_t size_image = rd<uint32_t>(file, opt_off + 56);
    uint32_t size_hdrs = rd<uint32_t>(file, opt_off + 60);

    if (image_base != IMAGE_BASE) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "image base %08x is not the expected %08x (no relocation support)", image_base,
                 IMAGE_BASE);
        g_error = buf;
        return false;
    }
    if ((uint64_t)image_base + size_image > HEAP_BASE) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "image ends at %08x, above the heap arena start %08x; raise [game] heap_base",
                 image_base + size_image, HEAP_BASE);
        g_error = buf;
        return false;
    }

    mem_init();
    g_base = image_base;
    g_size = size_image;
    g_entry = image_base + entry_rva;
    g_main.name = RECOMP_EXECUTABLE;
    g_main.path = g_exe_path;
    g_main.base = g_base;
    g_main.size = g_size;
    g_main.entry = g_entry;
    g_main.attached = true; // Its process entry must never run as DllMain.
    if (opt_size >= 104 && rd<uint32_t>(file, opt_off + 92) > 0) {
        g_main.export_rva = rd<uint32_t>(file, opt_off + 96);
        g_main.export_size = rd<uint32_t>(file, opt_off + 100);
        if (g_main.export_rva && !rva_in(g_size, g_main.export_rva, g_main.export_size)) {
            g_error = "main export directory lies outside the image";
            return false;
        }
    }

    // PE headers, then each section at its virtual address. The arena is zero
    // filled by mmap, so the tail of a section past SizeOfRawData (.bss) is
    // already zero.
    size_t hdr_copy = size_hdrs < file.size() ? size_hdrs : file.size();
    memcpy(g_mem + g_base, file.data(), hdr_copy);

    for (uint16_t i = 0; i < nsections; ++i) {
        size_t s = sh_off + 40 * i;
        char nm[9] = {0};
        memcpy(nm, file.data() + s, 8);
        SectionInfo si;
        si.name = nm;
        si.vsize = rd<uint32_t>(file, s + 8);
        si.va = image_base + rd<uint32_t>(file, s + 12);
        si.raw_size = rd<uint32_t>(file, s + 16);
        si.raw_ptr = rd<uint32_t>(file, s + 20);
        si.characteristics = rd<uint32_t>(file, s + 36);

        uint32_t span = si.vsize ? si.vsize : si.raw_size;
        if (si.va < image_base || (uint64_t)si.va + span > (uint64_t)image_base + size_image) {
            g_error = "section " + si.name + " lies outside the image";
            return false;
        }
        uint32_t copy = si.raw_size;
        if (si.vsize && copy > si.vsize)
            copy = si.vsize;
        if (si.raw_ptr && copy) {
            if ((uint64_t)si.raw_ptr + copy > file.size()) {
                g_error = "section " + si.name + " raw data is truncated";
                return false;
            }
            memcpy(g_mem + si.va, file.data() + si.raw_ptr, copy);
        }
        g_sections.push_back(si);
    }

    // game.toml operand_redirects: each new location starts with the value of
    // the one it replaces (8 bytes, enough for a double).
    for (uint32_t i = 0; i < recomp_operand_redirect_count; ++i) {
        uint32_t from = recomp_operand_redirect_pairs[2 * i],
                 to = recomp_operand_redirect_pairs[2 * i + 1];
        if ((uint64_t)from + 8 <= GUEST_SIZE && (uint64_t)to + 8 <= GUEST_SIZE)
            memcpy(g_mem + to, g_mem + from, 8);
    }

    // game.toml data_seeds: slots the translation's patches read.
    for (uint32_t i = 0; i < recomp_data_seed_count; ++i) {
        uint32_t addr = recomp_data_seed_pairs[2 * i];
        if ((uint64_t)addr + 4 <= GUEST_SIZE)
            memcpy(g_mem + addr, &recomp_data_seed_pairs[2 * i + 1], 4);
    }

    imports_init();
    // RECOMP_GAME_ROOT names an install root above the EXE's folder (a GOG
    // install's bin/ beside data/): the guest root maps onto it and the game
    // starts in the EXE's folder, as it would on Windows.
    {
        std::string dir = dirname_of(g_exe_path), sub;
        if (const char *root_env = recomp_env("GAME_ROOT"); root_env && *root_env) {
            std::string root = root_env;
            while (root.size() > 1 && root.back() == '/')
                root.pop_back();
            if (dir.size() > root.size() + 1 && dir.compare(0, root.size(), root) == 0 &&
                dir[root.size()] == '/') {
                sub = dir.substr(root.size() + 1);
                dir = root;
            }
        }
        win32_init(dir, sub);
    }

    // TLS directory (data directory 9). Reserve after win32_init, which clears
    // the slot map, and before IAT binding or any guest code can call TlsAlloc.
    g_tls = LoaderTls{0, 0, 0, 0, 0, 0xffffffffu};
    if (rd<uint32_t>(file, opt_off + 92) > 9) {
        size_t dd_tls = opt_off + 96 + 9 * 8;
        if (opt_size < 96 + 10 * 8) {
            g_error = "PE TLS data directory is truncated";
            return false;
        }
        uint32_t tls_rva = rd<uint32_t>(file, dd_tls);
        uint32_t tls_size = rd<uint32_t>(file, dd_tls + 4);
        if (tls_rva) {
            if (tls_size < 24 || !rva_ok(tls_rva, tls_size)) {
                g_error = "TLS directory is truncated or lies outside the image";
                return false;
            }
            uint32_t d = image_base + tls_rva;
            g_tls.raw_start = rd32(d + 0);
            g_tls.raw_end = rd32(d + 4);
            g_tls.index_addr = rd32(d + 8);
            g_tls.callbacks = rd32(d + 12);
            g_tls.zero_fill = rd32(d + 16);
            if (g_tls.raw_end < g_tls.raw_start ||
                (g_tls.raw_end > g_tls.raw_start &&
                 !rva_ok(g_tls.raw_start - image_base, g_tls.raw_end - g_tls.raw_start)) ||
                !rva_ok(g_tls.index_addr - image_base, 4) ||
                (g_tls.callbacks && !rva_ok(g_tls.callbacks - image_base, 4))) {
                g_error = "TLS directory points outside the image";
                return false;
            }
            g_tls.index = tls_reserve_slot();
            if (g_tls.index == 0xffffffffu) {
                g_error = "no TLS slot available for the image";
                return false;
            }
            wr32(g_tls.index_addr, g_tls.index);
        }
    }

    // RECOMP_IMPORT_STATS=1 prints the implemented / not-reached / logging-only
    // classification at exit, which is how the "not reached" column of the
    // coverage table gets filled in from a real run.
    if (recomp_env("IMPORT_STATS")) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            atexit([] { imports_dump_report(stderr); });
        }
    }

    // Auxiliary modules first: the executable may import from them statically
    // (Need for Speed Carbon imports msvcr71.dll and msvcp71.dll), and
    // patch_iat binds such imports to their exports only when they are mapped.
    // game.toml orders them by key, so list a DLL before the DLLs importing it.
    for (uint32_t i = 0; i < RECOMP_AUX_MODULE_COUNT; ++i)
        if (!load_aux_module(g_aux_specs[i]))
            return false;
    if (!patch_iat(file, opt_off, opt_magic, g_base, g_size))
        return false;

    if (g_entry != LOADER_EXPECTED_ENTRY)
        LOGW("entry point is %08x, expected %08x", g_entry, LOADER_EXPECTED_ENTRY);

    loader_init_context(&g_ctx);
    if (g_tls.index != 0xffffffffu && !rd32(TLS_BASE + 4 * g_tls.index)) {
        g_error = "cannot allocate the main thread TLS block";
        return false;
    }
    LOGV("loaded %s: base %08x size %08x entry %08x, %u IAT slots, %u trampolines",
         g_exe_path.c_str(), g_base, g_size, g_entry, g_iat_patched, imports_count());
    return true;
}

void loader_init_context(X86 *c) {
    recomp_seh_reset(c);
    memset(c, 0, sizeof(*c));
    // Process-start CPU state. The x87 values are the post-FINIT ones the CRT
    // assumes: control word 0x037f (round to nearest, 64-bit precision, all
    // exceptions masked), clear status word, all-empty tag word. __ftol,
    // __ctrlfp and __statfp diverge immediately if these are wrong.
    c->fpu_cw = 0x037f;
    c->fpu_sw = 0;
    c->fpu_tag = 0xffff;
    c->fpu_top = 0;
    // MXCSR as Windows starts a thread: round to nearest, every exception masked.
    c->mxcsr = RECOMP_MXCSR_DEFAULT;
    // EFLAGS with only the reserved bit 1 and IF set, so PUSHFD reads 0x202.
    c->eflags_misc = 0x00000202u;
    c->fs_base = TEB_BASE;

    // TEB: FS:[0] SEH chain head (empty = -1), FS:[4] stack base (high),
    // FS:[8] stack limit (low), FS:[0x18] TEB self pointer, FS:[0x2c] TLS array.
    memset(g_mem + TEB_BASE, 0, TEB_SIZE);
    wr32(TEB_BASE + 0x00, 0xffffffffu);
    wr32(TEB_BASE + 0x04, STACK_TOP);
    wr32(TEB_BASE + 0x08, STACK_LIMIT);
    wr32(TEB_BASE + 0x18, TEB_BASE);
    wr32(TEB_BASE + 0x2c, TLS_BASE);
    wr32(TEB_BASE + 0x30, TEB_BASE - 0x1000); // PEB placeholder (zeroed page)
    memset(g_mem + TLS_BASE, 0, TLS_SLOTS * 4);
    loader_tls_block_for_thread(TLS_BASE);

    // Stack: 16-byte aligned, sentinel return address on top so a RET from the
    // entry point lands somewhere recognisable.
    uint32_t esp = (STACK_TOP - 0x20) & ~0xfu;
    esp -= 4;
    wr32(esp, GUEST_RETURN_SENTINEL);
    c->r[R_ESP] = esp;
    c->r[R_EBP] = 0;
    c->eip = g_entry;
}

const char *loader_exe_sha256() {
    return g_exe_sha.c_str();
}

std::string loader_hash_file(const char *path) {
    std::vector<uint8_t> file;
    if (!path || !read_file(path, file))
        return "";
    return sha256_hex(file);
}

void run_entry(X86 *c) {
    if (!g_entry) {
        LOGW("run_entry: no image loaded");
        return;
    }
    c->eip = g_entry;
    // This is threads()[0], and it is a guest thread for as long as guest code
    // runs on it. Registered rather than inferred, for the reason in
    // kernel32.cpp: t_self defaults to 0, so inference would make every host
    // thread claim to be this one.
    sched_set_guest_thread(true);
    // The guest's rounding mode for the guest code that runs here, and the
    // host's own once it has ended (x86.h, SSE rounding control).
    recomp_fp_guest(c);
    // ExitProcess/TerminateProcess/ExitThread-outside-a-thread longjmp here.
    if (setjmp(*process_exit_jmp()) == 0) {
        // DLL_PROCESS_ATTACH for statically imported modules, in load order,
        // before TLS callbacks and the entry point, as the Windows loader does.
        for (LoaderModule &m : g_aux) {
            if (!m.bound || m.attached)
                continue;
            m.attached = true;
            m.load_refs = 1;
            if (!m.entry || recomp_module_lookup(m.entry) < 0) {
                LOGW("%s: DLL entry %08x is not translated, not run", m.name.c_str(), m.entry);
                continue;
            }
            const uint32_t args[3] = {m.base, 1u /* DLL_PROCESS_ATTACH */, 1u /* static load */};
            uint32_t ok = guest_call(c, m.entry, args, 3);
            LOGV("%s: DLL entry %08x attach returned %08x", m.name.c_str(), m.entry, ok);
            if (!ok) {
                g_error = m.name + ": DllMain failed process attach";
                LOGW("%s", g_error.c_str());
                sched_set_guest_thread(false);
                return;
            }
        }
        uint32_t count = 0;
        for (uint32_t p = g_tls.callbacks; p && rva_ok(p - g_base, 4); p += 4) {
            uint32_t cb = rd32(p);
            if (!cb)
                break;
            // The guest sees a pushed call frame, never host pointers. Restore
            // ESP explicitly so both caller and callee cleanup leave the entry
            // point's original frame intact.
            uint32_t esp = c->r[R_ESP];
            wr32(esp - 4, 0);
            wr32(esp - 8, 1); // DLL_PROCESS_ATTACH
            wr32(esp - 12, g_base);
            wr32(esp - 16, GUEST_RETURN_SENTINEL);
            c->r[R_ESP] = esp - 16;
            c->eip = cb;
            recomp_run(c, cb);
            c->r[R_ESP] = esp;
            ++count;
        }
        LOGV("ran %u TLS process-attach callbacks", count);
        c->eip = g_entry;
        recomp_run(c, g_entry);
    } else
        LOGW("guest process exited with code %u", process_exit_code());
    recomp_fp_host();
    sched_set_guest_thread(false);
}

void run_entry() {
    run_entry(&g_ctx);
}
