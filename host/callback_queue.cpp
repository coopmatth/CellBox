// callback_queue.cpp — 1.0.35 host-owned callback queue (see header).
// No WinRT, no allocation on the drain path beyond the std::function copy
// made at enqueue time. All queue operations are mutex-protected; the
// logger is only called from enqueue_and_wait (worker or main thread),
// never from inside the mutex.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <deque>
#include <mutex>
#include "callback_queue.h"

namespace
{
std::mutex g_cb_mutex;
std::deque<std::shared_ptr<xbox_cb_item>> g_cb_queue;
std::function<void(const std::string&)> g_cb_logger = [](const std::string& s)
{
    OutputDebugStringA(s.c_str());
    OutputDebugStringA("\n");
};
unsigned long g_cb_main_tid = 0;
} // namespace

std::shared_ptr<xbox_cb_item> xbox_cb_enqueue(std::function<void()> f)
{
    auto item = std::make_shared<xbox_cb_item>();
    item->f = std::move(f);
    std::lock_guard<std::mutex> lock(g_cb_mutex);
    g_cb_queue.push_back(item);
    return item;
}

std::shared_ptr<xbox_cb_item> xbox_cb_pop()
{
    std::lock_guard<std::mutex> lock(g_cb_mutex);
    if (g_cb_queue.empty())
        return nullptr;
    auto item = g_cb_queue.front();
    g_cb_queue.pop_front();
    return item;
}

void xbox_cb_enqueue_and_wait(std::function<void()> f, unsigned timeout_ms)
{
    // Direct-run path: call_from_main_thread invoked from the main thread
    // itself (e.g. a drained callback recursing). Waiting would deadlock.
    if (GetCurrentThreadId() == g_cb_main_tid && g_cb_main_tid != 0)
    {
        try
        {
            f();
        }
        catch (const std::exception& ex)
        {
            g_cb_logger(std::string("BOOT-CALLBACK-EXCEPTION std main-thread-direct: ") + ex.what());
        }
        catch (...)
        {
            g_cb_logger("BOOT-CALLBACK-EXCEPTION unknown main-thread-direct");
        }
        return;
    }

    auto item = xbox_cb_enqueue(std::move(f));
    unsigned waited = 0;
    while (!item->done.load() && waited < timeout_ms)
    {
        Sleep(1);
        ++waited;
    }
    if (!item->done.load())
    {
        g_cb_logger("BOOT-CALLBACK-TIMEOUT (callback not drained within 2000 ms; item stays queued and may still run later when drained)");
    }
}

void xbox_cb_set_main_thread(unsigned long tid)
{
    g_cb_main_tid = tid;
}

void xbox_cb_set_logger(std::function<void(const std::string&)> fn)
{
    if (fn)
        g_cb_logger = std::move(fn);
}
