// frontend_progress.h - CellBox F1: notice/trace progress piping.
// Hooks the core's log broadcast (logs::listener, the crash_capture
// mechanism) plus the core's own progress globals (Emu/system_progress
// .hpp: g_progr_text, g_progr_pdone/ptotal, g_progr_fdone/ftotal) and
// the emu_callbacks handle_taskbar_progress sink into one pollable
// snapshot the D3D12 UI renders during boot/compile. Plumbing only:
// no new compile feature. Header carries NO core types so the /ZW
// main TU and the desktop pre-test can consume it; the .cpp touches
// core symbols only under FRONTEND_PROGRESS_CORE (UWP build defines
// it; the desktop pre-test builds without and uses test feeds).
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "frontend_library.h" // LogFn

namespace frontend
{

// Level ints mirror logs::level (rpcs3/util/logs.hpp):
// always=0 fatal=1 error=2 todo=3 success=4 warning=5 notice=6 trace=7.
inline constexpr int kLevelAlways = 0;
inline constexpr int kLevelFatal = 1;
inline constexpr int kLevelError = 2;
inline constexpr int kLevelTodo = 3;
inline constexpr int kLevelSuccess = 4;
inline constexpr int kLevelWarning = 5;
inline constexpr int kLevelNotice = 6;
inline constexpr int kLevelTrace = 7;

struct ProgressSnapshot
{
    std::uint64_t counts[8] = {}; // indexed by level int above
    std::uint64_t total = 0;
    std::string last_notice;
    std::string last_trace;
    std::string last_any;
    std::string last_channel;
    std::string core_text; // g_progr_text (empty in no-core builds)
    std::uint32_t core_pdone = 0, core_ptotal = 0;
    std::uint32_t core_fdone = 0, core_ftotal = 0;
    int taskbar_value = 0, taskbar_limit = 0;
    int percent = -1; // -1 = core provided no usable fraction
    std::string percent_source = "none"; // core-progress|taskbar|core-files|text|none
};

// Idempotent. Core build: installs the logs::listener (counts every
// level the core actually broadcasts - trace stays 0 unless a channel
// emits it; that is reported as observed, never synthesised).
void progress_install(const LogFn& log);

// emu_callbacks::handle_taskbar_progress sink:
// (type,value) 0=reset 1=increment 2=set_limit 3=set_value.
void progress_taskbar(int type, int value);

void progress_reset();
void progress_snapshot(ProgressSnapshot& out);

// FRONTEND (1.1.3): disc-mount capture. The listener records the
// core's own VFS mount lines broadcast during Load (capped, in
// arrival order): "Mounted path ..." for /dev_bdvd and /app_home,
// plus the SYS "Disc:" / "Elf path:" lines. The host dumps them as
// FRONTEND-BOOT-MOUNT after BootGame returns, so a console folder
// boot shows what the core actually mounted - the desktop
// experiment's mount table, from the same log channel. Empty when
// the core broadcast none (the host logs that honestly).
std::vector<std::string> progress_mount_lines();

// Last "NN%" or "NN.N%" in text as an int percent, else -1.
int progress_parse_percent(const std::string& text);

#ifndef FRONTEND_PROGRESS_CORE
// Desktop pre-test seam (not compiled into the UWP frontend build).
void progress_test_feed(int level, const std::string& channel, const std::string& text);
void progress_test_set_core(std::uint32_t pdone, std::uint32_t ptotal,
                            std::uint32_t fdone, std::uint32_t ftotal,
                            const std::string& text);
#endif

} // namespace frontend
