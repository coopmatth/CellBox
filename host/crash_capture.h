// crash_capture.h — v1.0.23.0 boot-crash diagnostics (host-only, no core edit).
//
// Why this exists: 1.0.22 died inside Emu.BootGame() with total silence —
// no BOOT-RESULT, no watch step, no exception line. The core's own fatal
// path explains the silence: fmt::ensure() -> raw_verify_error() ->
// thread_ctrl::emergency_exit() -> utils::trap() (int3), and the core
// log normally goes to the read-only exe dir, so the reason is lost
// with the process. A plain try/catch around BootGame cannot catch any
// of that (trap is not an exception; a worker-thread AV is not on the
// calling thread). So this TU installs, before BootGame runs:
//   1. A logs::listener that mirrors core fatal/error lines into
//      LocalState\boot-crash-log.txt (raw Win32 append, flushed by
//      close per line). This captures sig_log.fatal from
//      emergency_exit() and every Load() fatal, if the process lives
//      long enough to broadcast them.
//   2. A process-wide VEH (AddVectoredExceptionHandler, first handler)
//      that logs BOOT-VEH code/address for ANY thread's exception —
//      including EXCEPTION_BREAKPOINT 0x80000003 from utils::trap() and
//      access violations — to LocalState\boot-veh-log.txt, then returns
//      EXCEPTION_CONTINUE_SEARCH so the OS still terminates the app.
//      The log is flushed before the process dies.
//      v1.0.27.0: the VEH also tries commit-on-touch for access
//      violations inside known SEC_RESERVE views (one 4 KiB page,
//      VirtualAllocFromApp MEM_COMMIT, then CONTINUE_EXECUTION;
//      out-of-range/commit-fail/repeat faults fall through), and
//      the BOOT-VEH line gains fault=/base=/rva=/range= fields.
//   3. A std::set_terminate handler logging BOOT-TERMINATE.
//   4. A __try/__except leaf wrapper for the BootGame call itself
//      (main-thread SEH only; worker-thread faults are the VEH's job).
#pragma once

#include <string>

#include "Emu/System.h"

namespace emu_host
{

// Install listener + VEH + terminate handler (idempotent). Must be
// called from boot_phase before BootGame, with the LocalState dir
// (forward slashes, no trailing slash). The log function is the host's
// host-results logger (used only for BOOT-CAPTURE-INSTALLED lines).
void crash_capture_install(const std::string& localStateUtf8,
                           std::function<void(const std::string&)> log);

// SEH leaf wrapper for BootGame. All string/optional arguments must be
// constructed by the CALLER and passed by reference: this function
// itself holds no C++ object with a destructor, so __try/__except is
// legal here under /EHsc. The __except filter lets C++ exceptions
// (0xE06D7363) pass through untouched so the caller's std::exception
// catch still sees what(); it only swallows hardware SEH (access
// violation, breakpoint from utils::trap(), etc.). Returns false if
// the __except handler ran (main-thread hardware SEH); on false,
// result/running are left untouched and the caller must log
// BOOT-VERDICT: FAIL (SEH).
bool call_boot_game_seh(const std::string& iso_path,
                        const std::string& title_id,
                        bool direct,
                        cfg_mode mode,
                        const std::string& config_path,
                        const std::optional<std::string>& db_config,
                        game_boot_result& result_out,
                        bool& running_out);

} // namespace emu_host
