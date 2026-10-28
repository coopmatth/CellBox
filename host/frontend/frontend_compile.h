// frontend_compile.h - CellBox F4: Compile (pre-bake) support.
// Host-side, core-free helpers for the Compile action:
//  - module enumeration over a folder entry's install dir, mirroring
//    the file filter the core's ppu_precompile applies (PPUThread.cpp:
//    .SPRX/.PRX/.ELF/.SELF records, .MSELF containers, the boot
//    EBOOT.BIN compiled separately as the main module, and
//    libfs_utility_init.sprx excluded). The compile itself is done by
//    the core (frontend_boot compile_phase_for_folder: the desktop
//    GUI CreateCPUCaches recipe - SetPrecompileCacheOption +
//    directory-scan BootGame); this TU only counts and reports.
//  - PPU cache statistics (LocalState/cache[/<serial>]) for honest
//    before/after evidence.
//  - RSX shader-cache FILE stat. Program-level counts (distinct /
//    hits / misses / entries) live in the D3D12 backend counters
//    (rsx_counters.h) and are read by main_frontend; this TU never
//    invents program numbers it cannot measure.
// Same FromApp file-API pattern as frontend_library.cpp so the
// identical code runs in the UWP app partition and the desktop
// pre-test. No firmware, keys or game content is read - file names
// and sizes only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "frontend_library.h" // GameEntry, LogFn

namespace frontend
{

struct CompileModule
{
    std::string rel; // path relative to the compile dir, '/' separators
    std::uint64_t size = 0;
};

struct ModuleEnum
{
    bool ok = false; // a compile dir with an EBOOT was resolved
    std::string compile_dir; // PS3_GAME dir handed to the core scan boot
    bool eboot_present = false;
    std::vector<CompileModule> modules; // EBOOT first, then sorted by rel
    std::uint64_t total_bytes = 0;
};

// Folder entries only. Returns ok=false for ISO entries (nothing to
// enumerate host-side; the ISO compile route is a boot pass) and for
// folders whose EBOOT cannot be located. Logs FRONTEND-COMPILE-DIR /
// FRONTEND-COMPILE-MODULES / FRONTEND-COMPILE-MODULE lines.
ModuleEnum enumerate_modules(const GameEntry& e, const LogFn& log);

// True when `name` (single path component) is a code-module file the
// core's precompile scan would queue. Exposed for the desktop test.
bool is_module_filename(const std::string& name);

struct CacheStats
{
    bool dir_exists = false;
    std::uint32_t files = 0;
    std::uint64_t bytes = 0;
    std::string dir; // the directory actually counted
};

// Count cache files under LocalState/cache/<serial> when the serial
// is a real TITLE_ID; otherwise (ISO placeholder / unknown) count the
// whole LocalState/cache tree - the honest scope when the title is
// not known host-side. Logs FRONTEND-COMPILE-CACHE.
CacheStats ppu_cache_stats(const std::string& localStateUtf8,
                           const std::string& serial, const LogFn& log);

struct ShaderCacheFile
{
    bool present = false;
    std::uint64_t bytes = 0;
    std::string path;
};

// Stat of the D3D12 backend's shader cache file (LocalState root).
// Presence/size only: entry counts are the backend's own counters.
ShaderCacheFile shader_cache_file(const std::string& localStateUtf8);

// The core announces each module it compiles with a notice of the
// form "Trying to load: <path>" / "Trying to load binary: <path>"
// (PPUThread.cpp / System.cpp). Extract the display name (basename)
// from such a notice; "" for anything else. Pure - desktop-tested.
std::string current_module_from_notice(const std::string& notice);

} // namespace frontend
