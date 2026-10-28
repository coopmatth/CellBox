#pragma once
// Desktop MemSample provider (PC only; never compiled into the UWP host).
// PrivateCommit/WorkingSet from psapi GetProcessMemoryInfo; optional DXGI
// video-memory info from a caller-supplied IDXGIAdapter3 (may be nullptr).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include "memory_governor.h"

inline MemSample desktop_sample(IDXGIAdapter3* adapter3) {
    MemSample s;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        s.has_psapi = true;
        s.private_commit = pmc.PagefileUsage; // commit charge (private) - moves at MEM_COMMIT
        s.working_set = pmc.WorkingSetSize;   // resident - moves on touch
        s.peak_working_set = pmc.PeakWorkingSetSize;
    }
    if (adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO loc{}, non{};
        HRESULT h1 = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc);
        HRESULT h2 = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non);
        if (SUCCEEDED(h1) || SUCCEEDED(h2)) {
            s.has_vidmem = true;
            if (SUCCEEDED(h1)) { s.vid_local_budget = loc.Budget; s.vid_local_usage = loc.CurrentUsage; }
            if (SUCCEEDED(h2)) { s.vid_nonlocal_budget = non.Budget; s.vid_nonlocal_usage = non.CurrentUsage; }
        }
    }
    return s;
}
