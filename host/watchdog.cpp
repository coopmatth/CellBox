// watchdog.cpp — 1.0.34.0. Host-only TU, no /ZW, compiled with the
// emucore include set (same flags as boot_probe.cpp) so it can read
// emu_host::boot_emu_state() and the D3D12 RSX counters. The watchdog
// thread makes NO WinRT calls: only atomics, GetTickCount64/Sleep, and
// raw Win32 file appends. It must never allocate on its hot path beyond
// one stack buffer per line.
#include "watchdog.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <thread>

#include "boot_probe.h"
#include "rsx_counters.h"

namespace
{
std::wstring g_wd_path;
SRWLOCK g_wd_lock = SRWLOCK_INIT;
std::atomic<const char*> g_marker{"(none)"};
std::atomic<unsigned long long> g_boot_tick{0};
std::atomic<unsigned long long> g_tick{0};
std::atomic<bool> g_started{false};
std::atomic<unsigned long long> g_host_loop_tick{0};
std::atomic<unsigned> g_host_loop_stage{0};
// FRONTEND (1.1.2): teardown latch - see watchdog.h. Set when a stop
// is requested (or teardown is otherwise pending), cleared at the
// next BootGame. Gates the PPU thread summary below.
std::atomic<bool> g_teardown_pending{false};
// Watchdog-thread-only: the summary-skip line is written once per
// gated period, not once per summary tick (the tick cadence is 1 s
// and a gated period can last the whole library session).
bool g_summary_skip_logged = false;

void wd_write_locked(const char* line, unsigned long len)
{
    HANDLE h = CreateFile2(g_wd_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD w = 0;
    WriteFile(h, line, len, &w, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
}

void watchdog_loop()
{
    for (;;)
    {
        unsigned long long t = ++g_tick;
        unsigned long long now = GetTickCount64();
        long long ms = -1;
        unsigned long long bt = g_boot_tick.load();
        if (bt != 0)
            ms = static_cast<long long>(now - bt);
        int emu = emu_host::boot_emu_state();
        unsigned long long ci = d3d12_rsx_init_count();
        unsigned long long cf = d3d12_rsx_flip_count();
        const char* m = g_marker.load();
        char buf[512];
        int n = std::snprintf(buf, sizeof(buf),
            "BOOT-WD tick=%llu ms=%lld marker=%s emu=%d rsx_init=%llu flips=%llu inlined=%llu hltick=%llu hlstage=%u\r\n",
            t, ms, m ? m : "(null)", emu, ci, cf, d3d12_rsx_inlined_skips(), watchdog_host_loop_tick(), watchdog_host_loop_stage());
        if (n > 0)
        {
            AcquireSRWLockExclusive(&g_wd_lock);
            wd_write_locked(buf, static_cast<unsigned long>(n));
            ReleaseSRWLockExclusive(&g_wd_lock);
        }
        // 1.0.43 (v): PPU thread summary every 500 ticks (~30 s at 1 s cadence).
        // FRONTEND (1.1.2) GATE: run it ONLY while the emulator is in an
        // active state AND no stop/teardown is pending.
        // xbox_ppu_thread_summary walks the core's PPU thread objects
        // via idm::select; after Emu.Kill, the Emulation Join Thread
        // destroys those objects on its own schedule, and Kill flips
        // the emu state to stopped=0 long before the join finishes -
        // so emu==0 is itself a teardown window, not a safe state.
        // The 1.1.1 console run crashed with an unhandled AV
        // (0xC0000005, rcx=0xCCCCCCCCCCCCCCCC) inside this call at
        // exactly tick=500 with emu=0, after the compile-pass stop
        // (boot-watchdog-111.txt ends at that tick; the AV is in
        // boot-veh-log-111.txt).
        if ((t % 500) == 0)
        {
            const bool emu_active = (emu == 1 || emu == 3 || emu == 4 || emu == 6 || emu == 7);
            if (emu_active && !g_teardown_pending.load())
            {
                char pbuf[1024];
                if (xbox_ppu_thread_summary(pbuf, sizeof(pbuf)))
                {
                    char pline[1152];
                    int pn = std::snprintf(pline, sizeof(pline), "BOOT-PPU-SUMMARY tick=%llu %s\r\n", t, pbuf);
                    if (pn > 0)
                    {
                        AcquireSRWLockExclusive(&g_wd_lock);
                        wd_write_locked(pline, static_cast<unsigned long>(pn));
                        ReleaseSRWLockExclusive(&g_wd_lock);
                    }
                }
                g_summary_skip_logged = false;
            }
            else if (!g_summary_skip_logged)
            {
                g_summary_skip_logged = true;
                char sbuf[512];
                int sn = std::snprintf(sbuf, sizeof(sbuf),
                    "BOOT-PPU-SUMMARY-SKIPPED tick=%llu emu=%d teardown_pending=%d (1.1.2 gate: never enumerate core PPU threads while stopped or tearing down)\r\n",
                    t, emu, g_teardown_pending.load() ? 1 : 0);
                if (sn > 0)
                {
                    AcquireSRWLockExclusive(&g_wd_lock);
                    wd_write_locked(sbuf, static_cast<unsigned long>(sn));
                    ReleaseSRWLockExclusive(&g_wd_lock);
                }
            }
        }
        // 100 ms cadence for the first 120 s (1200 ticks), then 1 s:
        // proves liveness without unbounded file growth on a long run.
        Sleep(t < 1200 ? 100 : 1000);
    }
}
} // namespace

void watchdog_append(const char* line)
{
    if (g_wd_path.empty())
        return;
    unsigned long len = 0;
    while (line[len])
        ++len;
    AcquireSRWLockExclusive(&g_wd_lock);
    wd_write_locked(line, len);
    ReleaseSRWLockExclusive(&g_wd_lock);
}

void watchdog_start(const std::string& localStateUtf8)
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true))
        return;
    if (localStateUtf8.empty())
        return;
    std::string p = localStateUtf8;
    if (p.back() != '/')
        p += '/';
    p += "boot-watchdog.txt";
    wchar_t wbuf[1024];
    if (p.size() + 1 > 1024)
        return;
    for (size_t i = 0; i < p.size(); ++i)
        wbuf[i] = static_cast<wchar_t>(static_cast<unsigned char>(p[i]));
    wbuf[p.size()] = 0;
    g_wd_path = wbuf;
    // Fresh file per run: truncate stale lines (CREATE_ALWAYS) so a repeat
    // run's watchdog is unambiguous.
    HANDLE h = CreateFile2(g_wd_path.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    watchdog_append("BOOT-WD-START watchdog thread starting (100 ms cadence, then 1 s after 120 s)\r\n");
    std::thread watchdog_thread(watchdog_loop);
    watchdog_thread.detach();
}

void watchdog_boot_returned()
{
    g_boot_tick.store(GetTickCount64());
}

// FRONTEND (1.1.2): teardown latch (see watchdog.h).
void watchdog_note_boot_started()
{
    g_teardown_pending.store(false);
}

void watchdog_note_stop_requested()
{
    g_teardown_pending.store(true);
}

bool watchdog_teardown_pending()
{
    return g_teardown_pending.load();
}

void watchdog_marker(const char* m)
{
    g_marker.store(m ? m : "(null)");
}

void watchdog_host_loop_mark(unsigned long long tick, unsigned stage)
{
    g_host_loop_tick.store(tick);
    g_host_loop_stage.store(stage);
}

unsigned long long watchdog_host_loop_tick()
{
    return g_host_loop_tick.load();
}

unsigned watchdog_host_loop_stage()
{
    return g_host_loop_stage.load();
}

long long watchdog_ms_since_boot()
{
    unsigned long long bt = g_boot_tick.load();
    if (bt == 0)
        return -1;
    return static_cast<long long>(GetTickCount64() - bt);
}

unsigned long long watchdog_tick64()
{
    return GetTickCount64();
}
