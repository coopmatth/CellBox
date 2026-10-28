#pragma once
// Step 8 memory governor prototype (host-side, synthetic allocations only).
// No RPCS3 core code. Backing is VirtualAlloc MEM_COMMIT pages, touched
// page-by-page so commit charge is real. Platform memory figures come from
// a caller-supplied sampler so this file never conflates them:
//  - AppMemoryUsage/AppMemoryUsageLimit: Windows::System::MemoryManager
//    (UWP/console only; the figure the 5 GiB Game-mode limit is enforced
//    against). UNAVAILABLE on desktop.
//  - PrivateCommit (PROCESS_MEMORY_COUNTERS_EX.PagefileUsage via psapi):
//    desktop commit charge for this process. Moves at MEM_COMMIT.
//  - WorkingSet (psapi): resident pages. Moves when pages are touched,
//    may lag/trim. NOT the same as commit.
//  - ReservedBytes (VirtualQuery over our own region): address-space
//    reservation only. A reservation never touched is not commit.
#include <cstdint>
#include <functional>
#include <string>

struct MemSample {
    bool has_app = false;
    unsigned long long app_usage = 0;   // MemoryManager::AppMemoryUsage
    unsigned long long app_limit = 0;   // MemoryManager::AppMemoryUsageLimit
    int app_level = -1;                 // AppMemoryUsageLevel (UWP), else -1
    bool has_psapi = false;
    unsigned long long private_commit = 0; // PagefileUsage (desktop)
    unsigned long long working_set = 0;
    unsigned long long peak_working_set = 0;
    bool has_vidmem = false;
    unsigned long long vid_local_budget = 0, vid_local_usage = 0;
    unsigned long long vid_nonlocal_budget = 0, vid_nonlocal_usage = 0;
};

// The figure the abort line is judged on, per platform (never mixed):
// console/UWP -> app_usage; desktop -> private_commit.
inline unsigned long long sample_governed_figure(const MemSample& s, bool* ok) {
    if (s.has_app) { *ok = true; return s.app_usage; }
    if (s.has_psapi) { *ok = true; return s.private_commit; }
    *ok = false; return 0;
}

using GovLogFn = std::function<void(const std::string& line)>;
using GovSamplerFn = std::function<MemSample()>;
// Renders/polls one regression frame. Returns false (+reason) on present
// failure or device removal -> stress abort condition.
using GovFrameFn = std::function<bool(std::string& failReason)>;

struct GovernorConfig {
    // Hard caps (bytes). Texture checkpoint "~1.2 GB" implemented as
    // 1200 MiB and labelled as such in every log line.
    unsigned long long cap_texture = 1200ull << 20;
    unsigned long long cap_shader_soft = 128ull << 20;
    unsigned long long cap_shader_hard = 256ull << 20;
    unsigned long long cap_upload = 256ull << 20;
    unsigned long long cap_guest = 512ull << 20; // 256 MiB main + 256 MiB RSX analogue (synthetic)
    // Pre-declared abort line: 3.0 GiB on the governed figure, below the
    // 5.00 GiB console limit. Declared before the stress runs (logged).
    unsigned long long abort_line = 3ull << 30;
    unsigned long long global_ceiling_ref = 5ull << 30; // console Game-mode limit, for % reporting
    int frames_per_step = 10;
};

struct GovernorResult {
    bool reserve_commit_ok = false;
    bool denial_ok = false;
    bool eviction_ok = false;
    bool stress_completed = false; // reached all caps + hold + free
    bool abort_fired = false;
    std::string abort_reason;
    unsigned long long denials = 0;
    unsigned long long evictions = 0;
    unsigned long long evicted_bytes = 0;
    unsigned long long threshold_crossings = 0;
    unsigned long long alloc_failures_outside_denial = 0;
    unsigned long long peak_governed_figure = 0;
    unsigned long long peak_total_pool_bytes = 0;
    bool pass() const {
        return reserve_commit_ok && denial_ok && eviction_ok && stress_completed &&
               !abort_fired && alloc_failures_outside_denial == 0;
    }
};

// Runs all Step 8 phases in order: reserve-vs-commit, baseline seeding
// (Step 6 pool bytes, accounting-only), denial proof, eviction proof,
// interleaved stress to caps with frames + 1 Hz samples, hold, free-all.
// frameFn may be null (PC standalone): steps then sleep briefly and the
// log says NO-PRESENT so it can never be read as a present regression.
GovernorResult run_step8_governor(const GovernorConfig& cfg, GovLogFn log,
                                  GovSamplerFn sampler, GovFrameFn frameFn);
