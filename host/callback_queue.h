// callback_queue.h — 1.0.35 (Argon fix 1): host-owned main-thread callback
// queue replacing CoreDispatcher::RunAsync for Emu.CallFromMainThread.
// WinRT never sees emulator callbacks. Plain C++ (no /ZW), usable from
// both native host TUs (boot_probe, emu_host) and the /ZW main TU.
#pragma once

#include <functional>
#include <memory>
#include <atomic>
#include <string>

struct xbox_cb_item
{
    std::function<void()> f;
    std::atomic<bool> done{false};
};

// Enqueue f for the main thread; returns the item (caller waits on done).
std::shared_ptr<xbox_cb_item> xbox_cb_enqueue(std::function<void()> f);

// Pop one queued item (null if empty). The drainer runs f, then MUST set
// done=true (even on exception) so the waiting worker unblocks.
std::shared_ptr<xbox_cb_item> xbox_cb_pop();

// Enqueue + wait up to timeout_ms for main-thread execution. If called on
// the main thread itself, runs f directly (no deadlock). On timeout logs
// BOOT-CALLBACK-TIMEOUT via the registered logger and returns; the item
// stays queued and may still run later when drained (same as the old
// pump's timeout behaviour, where the RunAsync handler stayed pending).
// Never throws.
void xbox_cb_enqueue_and_wait(std::function<void()> f, unsigned timeout_ms);

// Main-thread id (set by App::Run); used by enqueue_and_wait's direct-run path.
void xbox_cb_set_main_thread(unsigned long tid);

// Logger for timeout lines (set by App::Run to log_line). Default writes
// to OutputDebugStringA only. Never throws out of the queue.
void xbox_cb_set_logger(std::function<void(const std::string&)> fn);
