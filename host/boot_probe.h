// boot_probe.h — v1.0.22.0: ISO-boot probe (NullGSRender first).
// Host-only TU (no /ZW), compiled with the emucore include list, linked
// against emucore.lib + CMake deps like emu_host.cpp. NO RPCS3 core file
// is modified: all of this is host-side callback/config wiring.
// No firmware, keys, or game content is in any package: the ISO is read
// from LocalState (uploaded separately); absence logs BOOT-ISO-MISSING
// and the phase SKIPs cleanly — that is an expected, non-failure outcome.
#pragma once

#include <functional>
#include <string>

namespace emu_host
{

struct boot_callbacks
{
    std::function<void(const std::string&)> log;
    std::function<void(std::function<void()>)> pump;
    // MemoryManager figures from the C++/CX host (may be empty = unavailable).
    std::function<unsigned long long()> mem_usage;
    std::function<unsigned long long()> mem_limit;
};

struct boot_outcome
{
    bool attempted = false;   // BootGame was called (ISO present)
    bool skipped = false;     // ISO absent — expected, not a failure
    int result_code = -1;     // game_boot_result value (0 = no_errors)
    bool running_after = false;
};

// Boot probe, in order:
//  1. Re-apply g_cfg_vfs.emulator_dir = LocalState (magic-static lesson).
//  2. ISO presence check (LocalState/Call of Duty - Black Ops III.iso).
//  3. Config: renderer=null, llvm_threads=1, net=enabled + psn=psn_fake
//     (RPCS3 "Network Connected + PSN Simulated"; fixup_settings forces
//     psn to disabled when net is disabled, so both must be set),
//     audio renderer=null.
//  4. Replace the init/firmware logging no-op callbacks with boot
//     callbacks: init_gs_render -> NullGSRender, init_pad_handler ->
//     named_thread<pad_thread> with host_pad.cpp methods (see pad audit
//     in the report; Input/pad_thread.cpp is not part of emucore),
//     get_audio -> NullAudioBackend, get_gs_frame -> nullptr (GSRender
//     guards every frame use), safe defaults for every other callback
//     the core may call during boot (empty std::function would throw
//     bad_function_call).
//  5. Emu.BootGame(iso, "", direct=true, cfg_mode::custom).
// Returns the outcome; all milestones are logged with BOOT- lines.
boot_outcome boot_phase(const std::string& localStateUtf8,
                        const boot_callbacks& cb);

// Watch-loop state queries for the host (usable after boot_phase
// returns). boot_is_running()/boot_is_stopped() are the legacy
// wrappers; boot_emu_state() is the 1.0.30 fix for the 1.0.29
// misread: BootGame only calls Load(), Run() sets state=starting,
// and state=running arrives later via the RSX thread's RunPPU() ->
// FinalizeRunRequest(). IsRunning()==false at BootGame return is
// therefore a NORMAL race, not a failure — and IsStopped() is also
// wrong here (it returns true for loading as well as stopped).
// boot_emu_state() returns (int)Emu.GetStatus(): stopped=0,
// loading=1, running=3, paused=4, ready=6, starting=7 (stopping
// maps to stopped, frozen maps to paused under GetStatus fixup).
// The watch loop treats everything except stopped as resident.
bool boot_is_running();
bool boot_is_stopped();
int boot_emu_state();

} // namespace emu_host
