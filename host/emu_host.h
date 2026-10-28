// emu_host.h — v1.0.10.0: real Emulator init + firmware install phases.
// Native TU (no /ZW): compiled with the emucore include list and linked
// against emucore.lib + the CMake dependency libs (same line as the
// proven UWP link test). No Qt, no GUI exe linkage, no firmware/game
// file inside any package: the PUP is read from LocalState (uploaded
// by Matt via the Portal, SHA-256 verified) and dev_flash lands in
// LocalState under the config dir.
//
// log: one line at a time, no trailing newline (wraps main_uwp log_line).
// pump: posts f to the host main thread and blocks until it runs (the
// 1.0.9 RunAsync + ProcessEvents pattern) - used for call_from_main_thread.
// localStateUtf8: LocalState dir with '/' separators, NO trailing slash.
#pragma once

#include <functional>
#include <string>

namespace emu_host
{

// Sets RPCS3_CONFIG_DIR to LocalState (trailing slash, per the
// get_config_dir truncation rule) BEFORE any fs call, installs the
// host callbacks (dispatcher pump + logging no-ops), calls Emu.Init(),
// logs IsStopped/IsRunning + the resolved config dir. Returns true iff
// Init completed without throwing.
bool init_phase(const std::string& localStateUtf8,
                std::function<void(const std::string&)> log,
                std::function<void(std::function<void()>)> pump);

// Core firmware-install sequence, mirroring main_window.cpp (Qt parts
// skipped: no prompt dialogs - the host has no window; install proceeds
// and logs). Reads <LocalState>/PS3UPDAT.PUP, extracts each dev_flash_*
// package through SCEDecrypter into the /dev_flash mount, re-inits,
// then verifies by file count + total bytes + vsh.self presence.
// Returns true iff FIRMWARE-VERDICT is PASS. Logs FIRMWARE-SKIP (and
// returns false) when the PUP is absent - not a package failure.
bool firmware_phase(const std::string& localStateUtf8,
                    std::function<void(const std::string&)> log);

} // namespace emu_host
