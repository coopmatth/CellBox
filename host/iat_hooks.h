// iat_hooks.h — 1.0.35 (Argon item A + item D): IAT hooks installed at
// App::Run start, before Emu.Init. Killer hooks (NtTerminateProcess,
// TerminateProcess, ExitProcess, exit, _exit, RoFailFastWithErrorContext)
// write synchronously to LocalState\killer.txt. Exec-allocation hooks
// (VirtualAlloc, VirtualProtect, exec protections only) write request +
// result lines to LocalState\jit-alloc.txt. All writers are raw Win32
// appends (share read+write, flush+close per line) that never touch
// host-results.txt. If a target is not imported by the exe, no hook is
// installed for it and the install summary says so plainly.
// Plain C++ (no /ZW). No new DLL imports: RtlCaptureStackBackTrace is
// resolved at runtime via GetProcAddress.
#pragma once

#include <string>

// Install all hooks. localStateDir is the LocalState dir (backslash form).
// Idempotent. Truncates killer.txt / jit-alloc.txt fresh for this run and
// writes an install summary (found/missing per target) to killer.txt.
void xbox_hooks_install(const std::wstring& localStateDir);
