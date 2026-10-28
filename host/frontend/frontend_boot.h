// frontend_boot.h - CellBox F1: handoff from the library to the
// EXISTING boot_probe BootGame path, with the hardcoded 1.0.x ISO
// replaced by the selected library entry (nothing game-specific left
// in the boot path). frontend_boot.cpp is a DERIVED copy of
// boot_probe.cpp (1.0.44): the 1044 file is NOT modified in place;
// the frontend build links frontend_boot.obj INSTEAD of
// boot_probe.obj. Deltas vs boot_probe.cpp, all marked FRONTEND:
//  - target path + source are parameters (folder / .iso / EBOOT.BIN,
//    LocalState or USB), existence+size logged, no expected size
//  - notice/trace progress listener installed + reset before BootGame
//  - handle_taskbar_progress wired to frontend_progress
//  - boot_request_stop() (Emu.Kill) for return-to-library
// Everything else - callbacks, config, crash capture, SEH leaf, RSX
// cache-dir handoff - is the proven 1044 code, unchanged.
#pragma once

#include <functional>
#include <string>

namespace emu_host
{

struct boot_callbacks
{
    std::function<void(const std::string&)> log;
    std::function<void(std::function<void()>)> pump;
    std::function<unsigned long long()> mem_usage;
    std::function<unsigned long long()> mem_limit;
};

struct boot_outcome
{
    bool attempted = false;   // BootGame was called (target present)
    bool skipped = false;     // target absent - expected, not a failure
    int result_code = -1;     // game_boot_result value (0 = no_errors)
    bool running_after = false;
};

// Boot the selected library entry through the 1044 boot_probe path.
// gamePathUtf8: UTF-8 '/' path (disc root folder, EBOOT.BIN, or .iso).
// sourceTag: "LocalState" / "LocalIso" / "USB" (logged, nothing else).
// formTag (1.1.3): the entry's boot-target form as resolved by
// frontend::resolve_boot_target (iso-file / disc-root / ps3_game-dir
// / eboot-path); logged on the FRONTEND-BOOT-TARGET line. The 2026
// -10-28 desktop experiment proved the core normalises all folder
// forms to the disc root, so the form is evidence, not a rewrite.
boot_outcome boot_phase_for_path(const std::string& localStateUtf8,
                                 const std::string& gamePathUtf8,
                                 const std::string& sourceTag,
                                 const boot_callbacks& cb,
                                 const std::string& formTag = "");

// F4 Compile (pre-bake), folder route: the desktop GUI's
// CreateCPUCaches recipe (rpcs3qt/game_list_actions.cpp) driven
// host-only through existing emucore APIs - NO core change:
//   Emu.SetForceBoot(true);
//   Emu.SetPrecompileCacheOption({.is_fast = false});
//   Emu.BootGame(compileDir, titleId, direct=true)
// Load() takes its "Special boot mode (directory scan)" branch for a
// directory target: it loads/decrypts the EBOOT, then the core's own
// SPRX Loader thread analyses + PPU-compiles the main module and
// every SPRX/PRX/ELF/SELF under compileDir into the PPU cache, and
// finally Kills itself back to stopped (via CallFromMainThread, so
// the caller MUST keep draining the callback queue). This function
// returns when BootGame returns; the caller polls boot_emu_state()
// + frontend_progress until the pass settles at stopped.
// compileDirUtf8: the entry's PS3_GAME dir (frontend_compile
// enumerate_modules resolves it). titleIdUtf8: PARAM.SFO TITLE_ID or
// "" (the core then reads the SFO itself).
boot_outcome compile_phase_for_folder(const std::string& localStateUtf8,
                                      const std::string& compileDirUtf8,
                                      const std::string& titleIdUtf8,
                                      const std::string& sourceTag,
                                      const boot_callbacks& cb);

// Request emulation stop (frontend exit combo). Logs and calls
// Emu.Kill(allow_autoexit=false): the app must stay resident and
// return to the library. State polling stays boot_emu_state()'s job.
void boot_request_stop(std::function<void(const std::string&)> log);

// FRONTEND (1.1.2): stop-cycle tracking for the return-to-library
// teardown wait (App::wait_teardown in main_frontend.cpp). A cycle
// opens at every BootGame (both phases note it internally just
// before invoking BootGame) and completes when the core's on_stop
// callback fires. In this tree on_stop is invoked at the END of
// Emulator::Kill's join lambda - after the Emulation Join Thread
// has joined every emulation thread and reset the emu state - and
// it reaches the host through the CallFromMainThread queue, so the
// waiter must keep draining that queue. boot_emu_state()==0 is NOT
// a teardown-complete signal: Kill flips the state to stopped
// synchronously, seconds before the join finishes (the 1.1.1
// console stop was still joining 14+ s after state hit 0).
bool stop_cycle_open();    // a BootGame cycle is awaiting its on_stop
bool stop_cycle_strict();  // the cycle's BootGame returned no_errors
bool stop_on_stop_seen();  // on_stop delivered for the open cycle
void stop_cycle_close();   // waiter consumed the completion

// Same contracts as boot_probe.h (1.0.30 state semantics):
// stopped=0, loading=1, running=3, paused=4, ready=6, starting=7.
bool boot_is_running();
bool boot_is_stopped();
int boot_emu_state();

} // namespace emu_host
