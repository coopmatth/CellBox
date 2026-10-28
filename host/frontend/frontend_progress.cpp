// frontend_progress.cpp - see frontend_progress.h.
#include "frontend_progress.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cctype>
#include <cstring>

#ifdef FRONTEND_PROGRESS_CORE
#include "util/logs.hpp"
#include "Emu/system_progress.hpp"
static_assert(static_cast<int>(logs::level::notice) == frontend::kLevelNotice,
              "logs::level moved: frontend level ints must follow");
static_assert(static_cast<int>(logs::level::trace) == frontend::kLevelTrace,
              "logs::level moved: frontend level ints must follow");
#endif

namespace frontend
{
namespace
{

SRWLOCK g_lock = SRWLOCK_INIT;
std::uint64_t g_counts[8] = {};
std::string g_last_notice, g_last_trace, g_last_any, g_last_channel;
std::vector<std::string> g_mount_lines; // FRONTEND (1.1.3)
std::atomic<int> g_taskbar_value{0};
std::atomic<int> g_taskbar_limit{0};
bool g_installed = false;

std::string trunc240(std::string_view s)
{
    std::string out(s.substr(0, 240));
    for (char& c : out) if (c == '\r' || c == '\n') c = ' ';
    return out;
}

void count_message(int level, std::string_view prefix, std::string_view text)
{
    AcquireSRWLockExclusive(&g_lock);
    if (level >= 0 && level < 8) ++g_counts[level];
    g_last_any = trunc240(text);
    g_last_channel = trunc240(prefix);
    if (level == kLevelNotice) g_last_notice = g_last_any;
    if (level == kLevelTrace) g_last_trace = g_last_any;
    // FRONTEND (1.1.3): capture the core's disc-mount broadcasts
    // (VFS "Mounted path" for bdvd/app_home, SYS "Disc:"/"Elf
    // path:") so the host can log what Load actually mounted.
    {
        const bool isMount = text.rfind("Mounted path", 0) == 0 &&
            (text.find("dev_bdvd") != std::string_view::npos ||
             text.find("app_home") != std::string_view::npos);
        const bool isDisc = text.rfind("Disc:", 0) == 0 || text.rfind("Elf path:", 0) == 0;
        if ((isMount || isDisc) && g_mount_lines.size() < 12)
            g_mount_lines.push_back(trunc240(text));
    }
    ReleaseSRWLockExclusive(&g_lock);
}

#ifdef FRONTEND_PROGRESS_CORE
struct progress_listener : logs::listener
{
    void log(u64 /*stamp*/, const logs::message& msg, std::string_view prefix,
             std::string_view text) override
    {
        count_message(static_cast<int>(static_cast<logs::level>(msg)), prefix, text);
    }
};
#else
// Desktop-test stand-ins for the core progress globals.
std::uint32_t g_test_pdone = 0, g_test_ptotal = 0;
std::uint32_t g_test_fdone = 0, g_test_ftotal = 0;
std::string g_test_text;
#endif

} // namespace

void progress_install(const LogFn& log)
{
    if (g_installed)
    {
        if (log) log("FRONTEND-PROGRESS listener already installed");
        return;
    }
    g_installed = true;
#ifdef FRONTEND_PROGRESS_CORE
    static progress_listener* listener = new progress_listener();
    logs::listener::add(listener);
    if (log)
        log("FRONTEND-PROGRESS logs::listener installed (notice/trace counters + last-message; percent from g_progr_pdone/ptotal, taskbar sink, g_progr_fdone/ftotal, then text; trace counted only if the core broadcasts it)");
#else
    if (log)
        log("FRONTEND-PROGRESS no-core build: listener NOT installed (desktop test feeds only)");
#endif
}

void progress_taskbar(int type, int value)
{
    switch (type)
    {
    case 0: g_taskbar_value.store(0); break;              // reset
    case 1: g_taskbar_value.fetch_add(value); break;      // increment
    case 2: g_taskbar_limit.store(value); break;          // set_limit
    case 3: g_taskbar_value.store(value); break;          // set_value
    default: break;
    }
}

void progress_reset()
{
    AcquireSRWLockExclusive(&g_lock);
    memset(g_counts, 0, sizeof(g_counts));
    g_last_notice.clear(); g_last_trace.clear();
    g_last_any.clear(); g_last_channel.clear();
    g_mount_lines.clear(); // FRONTEND (1.1.3)
    ReleaseSRWLockExclusive(&g_lock);
    g_taskbar_value.store(0);
    g_taskbar_limit.store(0);
#ifndef FRONTEND_PROGRESS_CORE
    g_test_pdone = g_test_ptotal = g_test_fdone = g_test_ftotal = 0;
    g_test_text.clear();
#endif
}

std::vector<std::string> progress_mount_lines()
{
    AcquireSRWLockShared(&g_lock);
    std::vector<std::string> out = g_mount_lines;
    ReleaseSRWLockShared(&g_lock);
    return out;
}

int progress_parse_percent(const std::string& text)
{
    int found = -1;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%') continue;
        // Scan back over digits (and one dot) before the %.
        size_t j = i;
        bool dot = false;
        while (j > 0)
        {
            char c = text[j - 1];
            if (std::isdigit(static_cast<unsigned char>(c))) { --j; continue; }
            if (c == '.' && !dot) { dot = true; --j; continue; }
            break;
        }
        if (j == i) continue; // % with no digits
        std::string num = text.substr(j, i - j);
        if (num.empty() || num == ".") continue;
        char* end = nullptr;
        double v = strtod(num.c_str(), &end);
        if (end && *end == '\0' && v >= 0.0 && v <= 100.0)
            found = static_cast<int>(v + 0.5);
    }
    return found;
}

void progress_snapshot(ProgressSnapshot& out)
{
    out = ProgressSnapshot{};
    AcquireSRWLockShared(&g_lock);
    memcpy(out.counts, g_counts, sizeof(g_counts));
    out.last_notice = g_last_notice;
    out.last_trace = g_last_trace;
    out.last_any = g_last_any;
    out.last_channel = g_last_channel;
    ReleaseSRWLockShared(&g_lock);
    for (int i = 0; i < 8; ++i) out.total += out.counts[i];
    out.taskbar_value = g_taskbar_value.load();
    out.taskbar_limit = g_taskbar_limit.load();
#ifdef FRONTEND_PROGRESS_CORE
    out.core_text = trunc240(static_cast<std::string>(g_progr_text));
    out.core_pdone = g_progr_pdone.load();
    out.core_ptotal = g_progr_ptotal.load();
    out.core_fdone = g_progr_fdone.load();
    out.core_ftotal = g_progr_ftotal.load();
#else
    out.core_text = g_test_text;
    out.core_pdone = g_test_pdone;
    out.core_ptotal = g_test_ptotal;
    out.core_fdone = g_test_fdone;
    out.core_ftotal = g_test_ftotal;
#endif
    if (out.core_ptotal > 0)
    {
        out.percent = static_cast<int>(
            (static_cast<unsigned long long>(out.core_pdone) * 100ull) / out.core_ptotal);
        out.percent_source = "core-progress";
    }
    else if (out.taskbar_limit > 0)
    {
        out.percent = (out.taskbar_value * 100) / out.taskbar_limit;
        out.percent_source = "taskbar";
    }
    else if (out.core_ftotal > 0)
    {
        out.percent = static_cast<int>(
            (static_cast<unsigned long long>(out.core_fdone) * 100ull) / out.core_ftotal);
        out.percent_source = "core-files";
    }
    else
    {
        int p = progress_parse_percent(out.core_text);
        if (p < 0) p = progress_parse_percent(out.last_notice);
        if (p >= 0) { out.percent = p; out.percent_source = "text"; }
    }
    if (out.percent > 100) out.percent = 100;
}

#ifndef FRONTEND_PROGRESS_CORE
void progress_test_feed(int level, const std::string& channel, const std::string& text)
{
    count_message(level, channel, text);
}

void progress_test_set_core(std::uint32_t pdone, std::uint32_t ptotal,
                            std::uint32_t fdone, std::uint32_t ftotal,
                            const std::string& text)
{
    g_test_pdone = pdone; g_test_ptotal = ptotal;
    g_test_fdone = fdone; g_test_ftotal = ftotal;
    g_test_text = text;
}
#endif

} // namespace frontend
