// watchdog.h — 1.0.34.0 discriminator: a separate thread writing liveness
// lines to a SEPARATE file (LocalState/boot-watchdog.txt) every 100 ms.
// Purpose: if the main thread dies but the process lives, watchdog lines
// KEEP coming (main-thread-only death); if they stop at the same instant
// as the main log, the kill is process-wide. No WinRT calls on the
// watchdog thread (raw Win32 + atomics only), so it cannot itself be the
// failfast victim it is watching for. Plain C++: usable from both the
// /ZW main TU and plain host TUs.
#pragma once

#include <string>

// Start the watchdog thread (idempotent). localStateUtf8 is the LocalState
// dir in slash form, as used by boot_phase. Call BEFORE init/boot so the
// watchdog covers the whole run. Truncates any stale watchdog file first.
void watchdog_start(const std::string& localStateUtf8);

// Record the boot-return instant (call immediately after boot_phase
// returns). Watchdog ms values are relative to this; -1 before it.
void watchdog_boot_returned();

// Set the main-thread progress marker the watchdog reports (a static
// string literal; the watchdog copies the pointer, never the string).
void watchdog_marker(const char* m);

// Append one line (with its own \r\n) to boot-watchdog.txt, locked
// against the watchdog thread. Safe from any thread, including fault
// paths (raw Win32 append, no allocation, no WinRT).
void watchdog_append(const char* line);

// Milliseconds since boot-return, or -1 if boot has not returned yet.
long long watchdog_ms_since_boot();

// Raw tick count (GetTickCount64) for thread start/stop lines.
unsigned long long watchdog_tick64();
// 1.0.43 (iv) host-loop breadcrumb: set by the main sustained loop each iteration,
// read by the watchdog thread for the SUSTAINED line. Stage: 1=sleep_done,
// 2=wgi_poll, 3=file_open, 4=file_write_done.
void watchdog_host_loop_mark(unsigned long long tick, unsigned stage);
unsigned long long watchdog_host_loop_tick();
unsigned watchdog_host_loop_stage();
// 1.0.43 (v) PPU thread summary (defined in core PPUThread.cpp, linked via
// emucore.lib). Fills buf with one summary line (no trailing newline).
bool xbox_ppu_thread_summary(char* buf, unsigned long len);
