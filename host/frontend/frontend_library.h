// frontend_library.h - CellBox F1: game library scan (LocalState + USB).
// Native TU (no /ZW, no core headers): directory walks use the FromApp
// file APIs so the same code runs in the UWP app partition and in the
// desktop pre-test. PARAM.SFO parsing follows rpcs3/Loader/PSF.cpp
// (header_t/def_table_t, formats in PSF.h) - read-only ground truth,
// no core file modified and no core code linked for scanning.
#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace frontend
{

using LogFn = std::function<void(const std::string&)>;

enum class GameSource
{
    LocalState, // LocalState/games/<entry>
    LocalIso,   // *.iso directly in LocalState root (legacy 1.0.x layout)
    Usb         // removable-storage root (KnownFolders::RemovableDevices)
};

enum class GameKind
{
    Folder, // extracted disc folder (PS3_GAME structure / EBOOT.BIN)
    Iso     // single .iso file
};

struct GameEntry
{
    std::string title;   // PARAM.SFO TITLE, else folder/file name
    std::string serial;  // PARAM.SFO TITLE_ID, else "ISO" / "-"
    std::string path;    // UTF-8 '/' path handed to Emu.BootGame
    GameSource source = GameSource::LocalState;
    GameKind kind = GameKind::Folder;
    std::uint64_t size_bytes = 0;
    bool has_cache = false; // LocalState/cache/<serial> exists
    bool sfo_ok = false;    // PARAM.SFO parsed (title/serial are real)
};

const char* game_source_name(GameSource s);
const char* game_kind_name(GameKind k);

// FRONTEND (1.1.3): boot-target resolution. The Step-1-style desktop
// experiment (2026-10-28, desktop rpcs3.exe vs the extracted BO3
// folder, three target forms) proved the core normalises EVERY
// folder form to the disc root: root, PS3_GAME dir and EBOOT.BIN
// path all mount /dev_bdvd at the disc root and boot identically.
// So resolution never rewrites the path - it NAMES the form the
// entry already carries, for the FRONTEND-BOOT-TARGET log line.
struct BootTarget
{
    std::string path; // handed to Emu.BootGame (== entry.path today)
    std::string form; // iso-file | disc-root | ps3_game-dir | eboot-path
};

BootTarget resolve_boot_target(const GameEntry& e);

// FRONTEND (1.1.3, slice F2): the app-start staging block in
// main_frontend reassembles the legacy ISO from Portal-uploaded
// parts on every launch (reassemble.h). These constants are the
// single source both the staging call and delete_entry use, so
// deleting the legacy entry removes exactly what staging would
// rebuild from: the ISO plus its part files.
inline constexpr const char kStagingPartBase[] = "bo3.iso";
inline constexpr int kStagingPartCount = 4;
inline constexpr const char kStagingIsoName[] = "Call of Duty - Black Ops III.iso";
inline constexpr std::uint64_t kStagingIsoBytes = 4599644160ull;
// 1.1.5: SHA-256 of the full ISO (== concatenation of the four parts),
// computed on the PC from the staged parts. The staging block deletes the
// parts only after the assembled ISO matches this and kStagingIsoBytes.
inline constexpr const char kStagingIsoSha256[] =
    "67CC16FBB7ECDAA329F5DCCB8DBA4587134F4E1EB782381997213165FE89CE9C";

struct DeleteResult
{
    bool ok = false;      // all planned removals done (absent counts as done)
    bool refused = false; // validation refusal - nothing was touched
    std::uint32_t files_removed = 0;
    std::uint64_t bytes_removed = 0;
    std::string fail_path; // first failing path when ok=false
    unsigned long fail_err = 0;
};

// Delete a library entry's files. Folder entries (LocalState only):
// recursive delete of the entry tree, validated to sit strictly
// under <ls>/games/. Legacy LocalIso entry: the ISO file plus the
// staging part files (see constants above) - deleting only the ISO
// would resurrect the entry at the next launch's reassembly. An
// .iso under games/ deletes as a single file. USB entries are
// refused (read-only source). The per-title PPU cache under
// <ls>/cache is deliberately LEFT in place (it is keyed by module
// hash, shared across entries, and expensive to rebuild).
// Logs FRONTEND-DELETE lines (requested / refused / per-target
// result / fail / done).
DeleteResult delete_entry(const GameEntry& e, const std::string& localStateUtf8,
                          const LogFn& log);

// In-memory PARAM.SFO (PSF) parse. Returns false on bad magic/version/
// bounds (title/serial/category left untouched). Mirrors PSF.cpp checks:
// magic "\0PSF", version 0x101, LE fields, string fmt 0x0204 / array
// 0x0004 read as text, values cut at the first NUL for string fmt.
bool parse_param_sfo(const std::uint8_t* data, std::size_t size,
                     std::string& title, std::string& serial,
                     std::string& category);

// LocalState scan: <ls>/games/* (folders + .iso files) plus *.iso in
// <ls> itself (legacy route the 1.0.x diagnostic line used). Creates
// nothing. Logs FRONTEND-LIB-SCAN / FRONTEND-LIB-ENTRY lines.
std::vector<GameEntry> scan_localstate(const std::string& localStateUtf8,
                                       const LogFn& log);

// Removable-storage scan of one brokered root path: the root itself as
// a disc root, root/PS3_GAME, root/<folder> entries and root/*.iso.
// lsForCache is only used for the cache/<serial> existence flag.
// Logs FRONTEND-USB-SCAN / FRONTEND-LIB-ENTRY lines with GetLastError
// values; path-based access under a brokered removable root is
// UNVERIFIED on console (see results) - failures log, never fake.
std::vector<GameEntry> scan_usb_root(const std::string& rootUtf8,
                                     const std::string& localStateUtf8,
                                     const LogFn& log);

} // namespace frontend
