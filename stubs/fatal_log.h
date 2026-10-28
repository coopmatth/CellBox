// fatal_log.h — 1.0.35: host fatal-file writers, implemented in
// host-stubs.cpp (linked into the UWP exe in place of the probe no-op
// report_fatal_error). All functions are no-throw and write only to
// LocalState\rpcs3-fatal.txt (never host-results.txt, whose exclusive
// share mode 0 would drop worker-thread writes during a main-thread
// flush). Included by host-stubs.cpp (same dir) and by main_uwp_1035.cpp
// via the relative path ..\xbox-core-lib\linktest\fatal_log.h.
#pragma once


// LocalState dir (backslash form). Must be set before Emu.Init; writes
// before it is set are dropped (documented, not silently misdirected).
void xbox_fatal_set_dir(const wchar_t* dir);

// Boot-return tick (GetTickCount64); fatal lines report ms relative to it
// (-1 if not yet set, e.g. a fatal during Load).
void xbox_fatal_set_boot_ms(unsigned long long ms);

// Append one fatal line: thread id, tick, ms since boot-return, text.
// Flushes + closes before returning, so the line survives whatever the
// caller does next (including the [[noreturn]] contract violation of the
// old stub: emergency_exit's null-TLS path calls report_fatal_error and
// then falls off the end — the evidence is already on disk).
void xbox_fatal_write(const char* text, size_t len, bool is_html, bool include_help_text);
