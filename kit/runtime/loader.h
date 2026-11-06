// loader.h - maps the original PE into the guest arena and starts it.
#pragma once
#include "game_config.h"
#include "guest.h"
#include <string>
#include <vector>

struct SectionInfo {
    std::string name;
    uint32_t va;       // guest virtual address
    uint32_t vsize;    // Misc_VirtualSize
    uint32_t raw_size; // SizeOfRawData
    uint32_t raw_ptr;  // PointerToRawData
    uint32_t characteristics;
};

// Default image and its expected content hash.
extern const char *const LOADER_DEFAULT_EXE; // RECOMP_DEVELOPER_EXE from game.toml
extern const char *const LOADER_EXPECTED_SHA256;
// The SHA-256 of the image loader_load actually mapped, as lowercase hex, or
// "" before a load. Equal to LOADER_EXPECTED_SHA256 on a successful load; kept
// separate because it is a fact about this run rather than a build constant.
const char *loader_exe_sha256();
// The SHA-256 of the file at `path` as lowercase hex, or "" when unreadable.
std::string loader_hash_file(const char *path);
static const uint32_t LOADER_EXPECTED_ENTRY = RECOMP_ENTRY_POINT;

// Maps the PE at `exe_path` (nullptr => LOADER_DEFAULT_EXE) into a freshly
// initialised guest arena: sections at their virtual addresses, tail of each
// section zero filled (.bss), PE headers at the image base, IAT patched with
// import trampolines, TEB/TLS/stack prepared. Refuses any image whose SHA-256
// does not match LOADER_EXPECTED_SHA256. There is no hash bypass.
// Returns false and leaves loader_error() set on failure.
bool loader_load(const char *exe_path = nullptr);

const char *loader_error();
uint32_t loader_image_base();
uint32_t loader_image_size();
// IMAGE_BASE + SizeOfImage, i.e. one past the last image byte. 0 before a load.
uint32_t loader_image_limit();
uint32_t loader_entry_point();
// SizeOfStackReserve from the image's optional header: the stack Windows gives a
// thread whose creator asks for less (or for 0). 0 before a load.
uint32_t loader_stack_reserve();
const std::vector<SectionInfo> &loader_sections();

// A mapped PE module. Auxiliary modules come from game.toml [modules.aux.*].
// The main executable is already attached and also serves code/data exports.
// Auxiliary load references own DllMain attach/detach. The verified, IAT-patched
// image is retained so a later load starts with fresh globals and CRT state.
struct LoaderModule {
    std::string name, path;
    uint32_t base = 0, size = 0, entry = 0, export_rva = 0, export_size = 0;
    bool attached = false;
    // Named in some image's import table (not only reached by LoadLibrary):
    // run_entry attaches it before the game's entry point, as Windows does.
    bool bound = false;
    uint32_t load_refs = 0;
    std::vector<uint8_t> initial_image;
    std::vector<SectionInfo> sections;
};
uint32_t loader_module_count();
const LoaderModule *loader_module(uint32_t i);
// Count/index enumerate auxiliary modules; name/address lookups include the EXE.
LoaderModule *loader_module_named(const char *name); // case-insensitive, nullptr when unknown
const LoaderModule *loader_module_containing(uint32_t addr);
// The guest address of a named export, 0 when the module has none by that name.
uint32_t loader_module_export(const LoaderModule &m, const char *name);
uint32_t loader_module_export_ordinal(const LoaderModule &m, uint32_t ordinal);
// True inside the main image or any auxiliary module.
bool loader_in_image(uint32_t addr);
const std::string &loader_exe_path();
// IAT slots patched: trampolines for code imports, guest storage for data ones.
uint32_t loader_iat_patched();
uint32_t loader_iat_data_imports();

// The PE TLS directory, if the image has one. index is the slot the loader
// reserved in every thread's TLS array; 0xffffffff when there is none.
struct LoaderTls {
    uint32_t raw_start, raw_end, index_addr, callbacks, zero_fill, index;
};
const LoaderTls &loader_tls();
// Allocates and initialises one thread's TLS block (raw data plus zero fill)
// from the guest heap and stores its address in slot `index` of `tls_array`.
// Returns the block, 0 when the image has no TLS directory or the heap is full.
uint32_t loader_tls_block_for_thread(uint32_t tls_array);

// Resets `c` to the process-start state: zeroed registers, ESP just below
// STACK_TOP with a sentinel return address pushed, FS base at the TEB, x87
// control word 0x027f.
void loader_init_context(X86 *c);

// Process-wide context used by run_entry() and by hosts that do not keep their
// own X86 instance.
X86 *loader_context();

// Calls the PE entry point through recomp_call using loader_context().
void run_entry();
void run_entry(X86 *c);
