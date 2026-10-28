#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "memory_governor.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
std::string fmt(const char* f, ...) {
    char b[2048]; va_list ap; va_start(ap, f); vsnprintf(b, sizeof(b), f, ap); va_end(ap); return b;
}
double qpc_now(LARGE_INTEGER freq) { LARGE_INTEGER c; QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)freq.QuadPart; }

std::string sample_text(const MemSample& s) {
    std::string t;
    if (s.has_app) t += fmt("AppMemoryUsage=%llu AppMemoryUsageLimit=%llu level=%d", s.app_usage, s.app_limit, s.app_level);
    else t += "AppMemoryUsage=UNAVAILABLE(not-UWP)";
    if (s.has_psapi) t += fmt(" PrivateCommit_PagefileUsage=%llu WorkingSet=%llu PeakWS=%llu", s.private_commit, s.working_set, s.peak_working_set);
    else t += " PrivateCommit/WorkingSet=UNAVAILABLE(no-psapi-sampler)";
    if (s.has_vidmem) t += fmt(" VidLocal usage=%llu budget=%llu VidNonLocal usage=%llu budget=%llu",
        s.vid_local_usage, s.vid_local_budget, s.vid_nonlocal_usage, s.vid_nonlocal_budget);
    return t;
}

void touch_pages(void* base, unsigned long long bytes) {
    volatile char* p = (volatile char*)base;
    for (unsigned long long o = 0; o < bytes; o += 4096) p[o] = (char)((o >> 12) & 0x7F);
}
bool verify_pages(void* base, unsigned long long bytes) {
    volatile char* p = (volatile char*)base;
    for (unsigned long long o = 0; o < bytes; o += 4096)
        if (p[o] != (char)((o >> 12) & 0x7F)) return false;
    return true;
}

enum Pool { P_TEXTURE = 0, P_SHADER, P_UPLOAD, P_GUEST, P_COUNT };
const char* kPoolName[P_COUNT] = {"texture/surface", "shader-cache/DXIL", "upload/staging", "guest-RAM(synthetic)"};

struct Entry {
    int pool = 0; unsigned long long id = 0; unsigned long long bytes = 0;
    void* ptr = nullptr; bool evictable = false; bool baseline = false;
    unsigned long long last_tick = 0; std::string tag;
};

struct Governor {
    GovernorConfig cfg; GovLogFn log; GovSamplerFn sample; GovFrameFn frame;
    LARGE_INTEGER freq{}; double t0 = 0; double last_hz = -10;
    GovernorResult res;
    std::vector<Entry> entries;
    unsigned long long next_id = 1, tick = 0;
    unsigned long long used[P_COUNT] = {};
    bool warned80[P_COUNT] = {}, warned90[P_COUNT] = {};
    bool gw80 = false, gw90 = false;

    double now() { return qpc_now(freq) - t0; }
    unsigned long long cap_of(int p) const {
        switch (p) {
            case P_TEXTURE: return cfg.cap_texture;
            case P_SHADER: return cfg.cap_shader_hard;
            case P_UPLOAD: return cfg.cap_upload;
            default: return cfg.cap_guest;
        }
    }
    unsigned long long total_used() const {
        unsigned long long t = 0; for (int i = 0; i < P_COUNT; ++i) t += used[i]; return t;
    }
    MemSample take_sample(const char* tag, bool log_it) {
        MemSample s = sample ? sample() : MemSample{};
        bool ok = false; unsigned long long fig = sample_governed_figure(s, &ok);
        if (ok && fig > res.peak_governed_figure) res.peak_governed_figure = fig;
        if (total_used() > res.peak_total_pool_bytes) res.peak_total_pool_bytes = total_used();
        if (log_it) log(fmt("STEP8-MEM %s t=%.2fs pools{tex=%llu shader=%llu upload=%llu guest=%llu total=%llu} %s",
            tag, now(), used[P_TEXTURE], used[P_SHADER], used[P_UPLOAD], used[P_GUEST], total_used(), sample_text(s).c_str()));
        return s;
    }
    void maybe_hz() {
        if (now() - last_hz >= 1.0) { last_hz = now(); take_sample("1HZ", true); }
    }
    void check_thresholds() {
        for (int p = 0; p < P_COUNT; ++p) {
            unsigned long long cap = cap_of(p); if (!cap) continue;
            double pct = 100.0 * (double)used[p] / (double)cap;
            if (pct >= 80.0 && !warned80[p]) { warned80[p] = true; res.threshold_crossings++;
                log(fmt("STEP8-THRESHOLD t=%.2fs pool=%s crossed 80%% of hard cap (used=%llu cap=%llu %.1f%%)", now(), kPoolName[p], used[p], cap, pct)); }
            if (pct >= 90.0 && !warned90[p]) { warned90[p] = true; res.threshold_crossings++;
                log(fmt("STEP8-THRESHOLD t=%.2fs pool=%s crossed 90%% of hard cap (used=%llu cap=%llu %.1f%%)", now(), kPoolName[p], used[p], cap, pct)); }
            if (pct < 75.0) { warned80[p] = warned90[p] = false; } // hysteresis: re-arm after drain
        }
        bool ok = false; MemSample s = sample ? sample() : MemSample{};
        unsigned long long fig = sample_governed_figure(s, &ok);
        if (ok && cfg.global_ceiling_ref) {
            double pct = 100.0 * (double)fig / (double)cfg.global_ceiling_ref;
            if (pct >= 80.0 && !gw80) { gw80 = true; res.threshold_crossings++;
                log(fmt("STEP8-THRESHOLD t=%.2fs GLOBAL governed-figure crossed 80%% of %llu-byte ceiling-ref (figure=%llu %.1f%%)", now(), cfg.global_ceiling_ref, fig, pct)); }
            if (pct >= 90.0 && !gw90) { gw90 = true; res.threshold_crossings++;
                log(fmt("STEP8-THRESHOLD t=%.2fs GLOBAL governed-figure crossed 90%% of %llu-byte ceiling-ref (figure=%llu %.1f%%)", now(), cfg.global_ceiling_ref, fig, pct)); }
        }
    }
    // Returns entry id >0 on success; 0 = governor DENIED (cap); -1 = OS alloc failure (outside denial path).
    long long alloc(int pool, unsigned long long bytes, bool evictable, const char* tag, bool allow_evict) {
        unsigned long long cap = cap_of(pool);
        if (bytes > cap) {
            res.denials++;
            log(fmt("STEP8-DENY pool=%s bytes=%llu reason=single-alloc-exceeds-hard-cap cap=%llu tag=%s (no OS alloc attempted)", kPoolName[pool], bytes, cap, tag));
            return 0;
        }
        if (allow_evict && pool == P_SHADER) {
            while (used[pool] + bytes > cfg.cap_shader_soft) {
                // evict LRU evictable, non-baseline entry
                size_t best = SIZE_MAX; unsigned long long bt = ~0ull;
                for (size_t i = 0; i < entries.size(); ++i) {
                    auto& e = entries[i];
                    if (e.pool == pool && e.evictable && !e.baseline && e.ptr && e.last_tick < bt) { bt = e.last_tick; best = i; }
                }
                if (best == SIZE_MAX) break;
                Entry e = entries[best]; entries.erase(entries.begin() + (long)best);
                VirtualFree(e.ptr, 0, MEM_RELEASE); used[pool] -= e.bytes;
                res.evictions++; res.evicted_bytes += e.bytes;
                log(fmt("STEP8-EVICT pool=%s evicted id=%llu bytes=%llu tag=%s lru-tick=%llu -> pool-used=%llu soft-cap=%llu", kPoolName[pool], e.id, e.bytes, e.tag.c_str(), e.last_tick, used[pool], cfg.cap_shader_soft));
            }
        }
        if (used[pool] + bytes > cap) {
            res.denials++;
            log(fmt("STEP8-DENY pool=%s bytes=%llu reason=hard-cap used=%llu cap=%llu tag=%s (denied, not crash; no OS alloc attempted)", kPoolName[pool], bytes, used[pool], cap, tag));
            return 0;
        }
        void* p = VirtualAlloc(nullptr, (SIZE_T)bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!p) {
            DWORD err = GetLastError(); res.alloc_failures_outside_denial++;
            log(fmt("STEP8-ALLOCFAIL-OUTSIDE-DENIAL pool=%s bytes=%llu VirtualAlloc err=%lu tag=%s", kPoolName[pool], bytes, err, tag));
            return -1;
        }
        touch_pages(p, bytes);
        Entry e; e.pool = pool; e.id = next_id++; e.bytes = bytes; e.ptr = p; e.evictable = evictable; e.tag = tag; e.last_tick = ++tick;
        entries.push_back(e); used[pool] += bytes;
        check_thresholds();
        return (long long)e.id;
    }
    void seed_baseline(int pool, unsigned long long bytes, const char* tag) {
        Entry e; e.pool = pool; e.id = next_id++; e.bytes = bytes; e.ptr = nullptr; e.baseline = true; e.tag = tag; e.last_tick = ++tick;
        entries.push_back(e); used[pool] += bytes;
        log(fmt("STEP8-BASELINE pool=%s bytes=%llu tag=%s (accounting-only, Step 6 measured pools; no new backing)", kPoolName[pool], bytes, tag));
    }
    void touch_entry(unsigned long long id) {
        for (auto& e : entries) if (e.id == id) { e.last_tick = ++tick; return; }
    }
    void free_all(bool keep_baseline) {
        size_t kept = 0;
        for (size_t i = 0; i < entries.size();) {
            Entry e = entries[i];
            if (e.baseline && keep_baseline) { ++i; ++kept; continue; }
            if (e.ptr) VirtualFree(e.ptr, 0, MEM_RELEASE);
            used[e.pool] -= e.bytes; entries.erase(entries.begin() + (long)i);
        }
        log(fmt("STEP8-FREEALL keep-baseline=%d kept-entries=%zu pools{tex=%llu shader=%llu upload=%llu guest=%llu}", keep_baseline ? 1 : 0, kept,
            used[P_TEXTURE], used[P_SHADER], used[P_UPLOAD], used[P_GUEST]));
    }
    bool run_frames(int n, std::string& why) {
        for (int i = 0; i < n; ++i) {
            if (frame) { if (!frame(why)) return false; }
            else Sleep(2);
            maybe_hz();
        }
        return true;
    }
    bool abort_check(const char* where) {
        MemSample s = take_sample("ABORTCHECK", false);
        bool ok = false; unsigned long long fig = sample_governed_figure(s, &ok);
        if (ok && fig > cfg.abort_line) {
            res.abort_fired = true;
            res.abort_reason = fmt("governed figure %llu crossed pre-declared abort line %llu at %s", fig, cfg.abort_line, where);
            log(fmt("STEP8-STRESS-ABORT t=%.2fs %s", now(), res.abort_reason.c_str()));
            return true;
        }
        return false;
    }
};
} // namespace

GovernorResult run_step8_governor(const GovernorConfig& cfg, GovLogFn logFn, GovSamplerFn sampler, GovFrameFn frameFn) {
    Governor g; g.cfg = cfg; g.log = logFn; g.sample = sampler; g.frame = frameFn;
    QueryPerformanceFrequency(&g.freq); g.t0 = qpc_now(g.freq);
    auto& L = g.log;
    L("=== STEP 8 MEMORY GOVERNOR (synthetic allocations only; no game data) ===");
    L(fmt("STEP8-CONFIG caps: texture/surface hard=%llu (1200 MiB, the '~1.2 GB' checkpoint) shader-cache soft=%llu hard=%llu upload/staging hard=%llu guest-RAM(synthetic) hard=%llu; cap-sum=%llu",
        cfg.cap_texture, cfg.cap_shader_soft, cfg.cap_shader_hard, cfg.cap_upload, cfg.cap_guest,
        cfg.cap_texture + cfg.cap_shader_hard + cfg.cap_upload + cfg.cap_guest));
    L(fmt("STEP8-ABORT-LINE PRE-DECLARED: governed-figure > %llu bytes (3.0 GiB) aborts the stress. Governed figure = AppMemoryUsage on UWP/console, psapi PrivateCommit on desktop. Console Game-mode limit ref=%llu. Other abort conditions: device-removed, present failure (frame callback), OS alloc failure outside governor denial.",
        cfg.abort_line, cfg.global_ceiling_ref));
    L(frameFn ? "STEP8-FRAMES: frame callback present (present/draw regression frames run between stress steps)"
              : "STEP8-FRAMES: NO-PRESENT (standalone PC governor test; no present/draw regression in this run)");
    L("STEP8-API-NOTES: ReservedBytes=VirtualQuery over our own MEM_RESERVE region (address space only). "
      "PrivateCommit=PROCESS_MEMORY_COUNTERS_EX.PagefileUsage via psapi (desktop commit charge; moves at MEM_COMMIT). "
      "WorkingSet=psapi resident pages (moves on touch, may trim). "
      "AppMemoryUsage/AppMemoryUsageLimit/level=Windows::System::MemoryManager (UWP/console; the 5 GiB limit figure). "
      "VidMem=IDXGIAdapter3::QueryVideoMemoryInfo (budget/usage as DXGI reports). UNAVAILABLE markers mean that figure does not exist on this platform - never substituted.");
    g.take_sample("BASELINE-ENTRY", true);

    // ---- Phase 1: RESERVE vs COMMIT ----
    L("--- STEP8 PHASE 1: reserve vs commit (4 GiB reserve, then commit+touch 8 x 64 MiB) ---");
    const unsigned long long kReserve = 4ull << 30, kInc = 64ull << 20; const int kSteps = 8;
    void* rbase = VirtualAlloc(nullptr, (SIZE_T)kReserve, MEM_RESERVE, PAGE_NOACCESS);
    if (!rbase) L(fmt("STEP8-RESERVE VirtualAlloc(MEM_RESERVE 4GiB) FAIL err=%lu - trying VirtualAllocFromApp", GetLastError()));
    const char* reserveApi = "VirtualAlloc";
    if (!rbase) { rbase = VirtualAllocFromApp(nullptr, (SIZE_T)kReserve, MEM_RESERVE, PAGE_NOACCESS); reserveApi = "VirtualAllocFromApp"; }
    if (!rbase) {
        L(fmt("STEP8-RESERVE: FAIL both APIs err=%lu (reserve-vs-commit NOT measured this run)", GetLastError()));
        g.res.reserve_commit_ok = false;
    } else {
        MEMORY_BASIC_INFORMATION mbi{};
        SIZE_T q = VirtualQuery(rbase, &mbi, sizeof(mbi));
        L(fmt("STEP8-RESERVE %s(4 GiB MEM_RESERVE,PAGE_NOACCESS) base=%p VirtualQuery ret=%zu State=0x%lX(MEM_RESERVE=0x2000) RegionSize=%zu Protect=0x%lX - nothing committed, nothing touched",
            reserveApi, rbase, q, mbi.State, mbi.RegionSize, mbi.Protect));
        MemSample s0 = g.take_sample("AFTER-RESERVE-4GIB-NOTOUCH", true);
        bool okF = false; unsigned long long fig0 = sample_governed_figure(s0, &okF);
        bool allOk = true;
        for (int i = 0; i < kSteps; ++i) {
            void* at = (char*)rbase + (unsigned long long)i * kInc;
            void* c = VirtualAlloc(at, (SIZE_T)kInc, MEM_COMMIT, PAGE_READWRITE);
            if (!c) { L(fmt("STEP8-COMMIT step=%d VirtualAlloc(MEM_COMMIT 64MiB) FAIL err=%lu", i, GetLastError())); allOk = false; break; }
            MemSample sc = g.take_sample(fmt("COMMIT-%d-NOTOUCH", i).c_str(), true);
            bool okC = false; unsigned long long figC = sample_governed_figure(sc, &okC);
            touch_pages(c, kInc);
            MemSample st = g.take_sample(fmt("COMMIT-%d-TOUCHED", i).c_str(), true);
            bool okT = false; unsigned long long figT = sample_governed_figure(st, &okT);
            L(fmt("STEP8-COMMIT-DELTA step=%d committed-total=%llu governed-figure: commit-notouch-delta-vs-reserve=%lld touched-delta-vs-notouch=%lld ws(commit-notouch)=%llu ws(touched)=%llu (figure-available=%d/%d)",
                i, (unsigned long long)(i + 1) * kInc,
                (okC && okF) ? (long long)(figC - fig0) : 0,
                (okT && okC) ? (long long)(figT - figC) : 0,
                sc.working_set, st.working_set, okC ? 1 : 0, okT ? 1 : 0));
            std::string why; if (!g.run_frames(2, why)) { L("STEP8-RESERVE-COMMIT frame fail: " + why); }
        }
        bool verifyOk = allOk && verify_pages(rbase, kInc);
        BOOL dec = VirtualFree(rbase, (SIZE_T)((unsigned long long)kSteps * kInc), MEM_DECOMMIT);
        L(fmt("STEP8-DECOMMIT VirtualFree(MEM_DECOMMIT %llu) -> %d err=%lu pattern-verify-first-block=%d", (unsigned long long)kSteps * kInc, (int)dec, dec ? 0ul : GetLastError(), verifyOk ? 1 : 0));
        g.take_sample("AFTER-DECOMMIT", true);
        BOOL rel = VirtualFree(rbase, 0, MEM_RELEASE);
        L(fmt("STEP8-RELEASE VirtualFree(MEM_RELEASE 4GiB reservation) -> %d err=%lu", (int)rel, rel ? 0ul : GetLastError()));
        g.take_sample("AFTER-RELEASE", true);
        g.res.reserve_commit_ok = allOk && verifyOk && dec && rel;
    }
    L(fmt("STEP8-RESERVE-COMMIT-VERDICT: %s (a never-touched reservation is address space, not committed memory - see AFTER-RESERVE sample vs COMMIT deltas above)", g.res.reserve_commit_ok ? "PASS" : "FAIL"));

    // ---- Baseline: Step 6 measured pool bytes (accounting-only) ----
    L("--- STEP8 BASELINE (Step 6 STEP6-POOLS bytes) ---");
    g.seed_baseline(P_TEXTURE, 65536ull + 3932160ull, "step6 texture-default 65536 + flip-persistent 3932160");
    g.seed_baseline(P_UPLOAD, 128ull + 12ull + 16384ull + 262144ull + 256ull + 3686400ull, "step6 vertex128+index12+tex-upload16384+push262144+readback256+flip-staging3686400");
    g.seed_baseline(P_GUEST, 3686400ull, "step6 synthetic guest 3686400");
    g.seed_baseline(P_SHADER, 2276ull + 2032ull + 1856ull + 1500ull, "step5/6 cache DXIL 2276+2032+1856+1500");
    g.take_sample("AFTER-BASELINE-SEED", true);

    // ---- Phase 2a: DENIAL proof ----
    L("--- STEP8 PHASE 2a: cap denial (denied, not crash) ---");
    long long deny1 = g.alloc(P_TEXTURE, cfg.cap_texture + (1ull << 20), false, "deny-proof-oversize-single", false);
    // Fill upload pool to cap in 64 MiB blocks, then one more must deny.
    bool fillOk = true;
    while (g.used[P_UPLOAD] + (64ull << 20) <= cfg.cap_upload) {
        if (g.alloc(P_UPLOAD, 64ull << 20, false, "deny-proof-upload-fill", false) < 0) { fillOk = false; break; }
        g.maybe_hz();
    }
    long long deny2 = g.alloc(P_UPLOAD, 64ull << 20, false, "deny-proof-upload-over-cap", false);
    g.res.denial_ok = (deny1 == 0 && deny2 == 0 && fillOk);
    L(fmt("STEP8-DENIAL-VERDICT: %s (oversize-single=%lld upload-over-cap=%lld, 0=governor-denied; host still running)", g.res.denial_ok ? "PASS" : "FAIL", deny1, deny2));
    // Drain upload back to baseline for phases below.
    {
        for (size_t i = 0; i < g.entries.size();) {
            auto e = g.entries[i];
            if (e.pool == P_UPLOAD && !e.baseline) { VirtualFree(e.ptr, 0, MEM_RELEASE); g.used[P_UPLOAD] -= e.bytes; g.entries.erase(g.entries.begin() + (long)i); }
            else ++i;
        }
        L(fmt("STEP8-DRAIN upload pool back to baseline used=%llu", g.used[P_UPLOAD]));
    }
    g.take_sample("AFTER-DENIAL-PROOF", true);

    // ---- Phase 2b: EVICTION / LRU proof (shader-cache pool) ----
    L("--- STEP8 PHASE 2b: eviction/LRU (shader-cache soft cap) ---");
    std::vector<long long> ids;
    bool evFillOk = true;
    for (int i = 0; i < 8; ++i) { // 8 x 16 MiB = 128 MiB = soft cap (plus tiny baseline -> last may evict; allow_evict off to fill deterministically to soft)
        long long id = g.alloc(P_SHADER, 16ull << 20, true, fmt("evict-proof-fill-%d", i).c_str(), false);
        if (id <= 0) { evFillOk = (id == 0); break; } ids.push_back(id);
    }
    unsigned long long evictionsBefore = g.res.evictions;
    if (!ids.empty()) g.touch_entry((unsigned long long)ids[0]); // make fill-0 most-recently-used; fill-1 becomes LRU
    long long pinned = g.alloc(P_SHADER, 16ull << 20, false, "evict-proof-pinned", false);
    long long need = g.alloc(P_SHADER, 32ull << 20, true, "evict-proof-new-alloc", true); // must evict LRU evictable (not fill-0, not pinned) to fit soft
    bool fill0Alive = false, pinnedAlive = false;
    for (auto& e : g.entries) { if (!ids.empty() && (long long)e.id == ids[0]) fill0Alive = true; if ((long long)e.id == pinned) pinnedAlive = true; }
    g.res.eviction_ok = evFillOk && need > 0 && g.res.evictions > evictionsBefore && fill0Alive && (pinned <= 0 || pinnedAlive);
    L(fmt("STEP8-EVICTION-VERDICT: %s (fill-ok=%d new-alloc-id=%lld evictions-in-proof=%llu evicted-bytes-total=%llu MRU-fill0-alive=%d pinned-alive=%d shader-used=%llu soft=%llu)",
        g.res.eviction_ok ? "PASS" : "FAIL", evFillOk ? 1 : 0, need, g.res.evictions - evictionsBefore, g.res.evicted_bytes,
        fill0Alive ? 1 : 0, pinnedAlive ? 1 : 0, g.used[P_SHADER], cfg.cap_shader_soft));
    g.take_sample("AFTER-EVICTION-PROOF", true);
    // Free non-baseline everywhere (back to Step 6 baseline) before stress.
    g.free_all(true);
    g.take_sample("PRE-STRESS", true);

    // ---- Phase 3: STRESS to caps in interleaved steps + frames ----
    L("--- STEP8 PHASE 3: stress (grow to caps in steps; abort line pre-declared above) ---");
    struct Step { int pool; unsigned long long bytes; const char* tag; };
    std::vector<Step> steps;
    // Interleave pools round-robin toward hard caps (accounting for baseline).
    {
        unsigned long long want[P_COUNT] = {
            cfg.cap_texture - g.used[P_TEXTURE], cfg.cap_shader_hard - g.used[P_SHADER],
            cfg.cap_upload - g.used[P_UPLOAD], cfg.cap_guest - g.used[P_GUEST]};
        unsigned long long chunk[P_COUNT] = {100ull << 20, 32ull << 20, 64ull << 20, 128ull << 20};
        bool more = true;
        while (more) { more = false;
            for (int p = 0; p < P_COUNT; ++p) if (want[p] > 0) {
                unsigned long long b = want[p] < chunk[p] ? want[p] : chunk[p];
                steps.push_back({p, b, "stress"}); want[p] -= b; more = more || (want[p] > 0);
            }
        }
    }
    L(fmt("STEP8-STRESS-PLAN steps=%zu frames-per-step=%d (guest 128MiB, texture 100MiB, upload 64MiB, shader 32MiB interleaved)", steps.size(), cfg.frames_per_step));
    bool aborted = false;
    for (size_t si = 0; si < steps.size() && !aborted; ++si) {
        auto& st = steps[si];
        long long id = g.alloc(st.pool, st.bytes, st.pool == P_SHADER, st.tag, false);
        if (id < 0) {
            g.res.abort_fired = true; g.res.abort_reason = "OS allocation failure outside governor denial path (see STEP8-ALLOCFAIL line)";
            L(fmt("STEP8-STRESS-ABORT step=%zu/%zu %s", si + 1, steps.size(), g.res.abort_reason.c_str())); aborted = true; break;
        }
        if (id == 0) { // governor denial inside stress = accounting bug vs plan; treat as abort-worthy surprise, recorded
            g.res.abort_fired = true; g.res.abort_reason = fmt("unexpected governor denial at stress step %zu pool=%s (plan should not exceed caps)", si + 1, kPoolName[st.pool]);
            L("STEP8-STRESS-ABORT " + g.res.abort_reason); aborted = true; break;
        }
        std::string why;
        if (!g.run_frames(cfg.frames_per_step, why)) {
            g.res.abort_fired = true; g.res.abort_reason = "present/draw frame failure or device-removed during stress: " + why;
            L(fmt("STEP8-STRESS-ABORT step=%zu/%zu %s", si + 1, steps.size(), g.res.abort_reason.c_str())); aborted = true; break;
        }
        MemSample s = g.take_sample(fmt("STRESS-STEP-%zu", si + 1).c_str(), true);
        bool okF = false; unsigned long long fig = sample_governed_figure(s, &okF);
        L(fmt("STEP8-STRESS-STEP %zu/%zu pool=%s +%llu pool-used=%llu/%llu total-pools=%llu governed-figure=%llu (%.1f%% of ceiling-ref %llu, abort-line %llu)",
            si + 1, steps.size(), kPoolName[st.pool], st.bytes, g.used[st.pool], g.cap_of(st.pool), g.total_used(),
            okF ? fig : 0ull, okF && cfg.global_ceiling_ref ? 100.0 * (double)fig / (double)cfg.global_ceiling_ref : 0.0,
            cfg.global_ceiling_ref, cfg.abort_line));
        if (g.abort_check(fmt("stress step %zu", si + 1).c_str())) aborted = true;
    }
    bool atCaps = !aborted && g.used[P_TEXTURE] == cfg.cap_texture && g.used[P_SHADER] == cfg.cap_shader_hard &&
                  g.used[P_UPLOAD] == cfg.cap_upload && g.used[P_GUEST] == cfg.cap_guest;
    L(fmt("STEP8-STRESS-AT-CAPS: %s pools{tex=%llu/%llu shader=%llu/%llu upload=%llu/%llu guest=%llu/%llu} aborted=%d",
        atCaps ? "REACHED" : "NOT-REACHED", g.used[P_TEXTURE], cfg.cap_texture, g.used[P_SHADER], cfg.cap_shader_hard,
        g.used[P_UPLOAD], cfg.cap_upload, g.used[P_GUEST], cfg.cap_guest, aborted ? 1 : 0));
    if (!aborted) {
        // Pattern-verify a sample entry per pool while at caps.
        for (int p = 0; p < P_COUNT; ++p) {
            bool any = false, okv = false;
            for (auto& e : g.entries) if (e.pool == p && e.ptr) { any = true; okv = verify_pages(e.ptr, e.bytes < (4ull << 20) ? e.bytes : (4ull << 20)); break; }
            L(fmt("STEP8-VERIFY-AT-CAP pool=%s sample-present=%d pattern-ok=%d", kPoolName[p], any ? 1 : 0, okv ? 1 : 0));
            if (any && !okv) { g.res.abort_fired = true; g.res.abort_reason = "backing pattern verify failed at caps"; aborted = true; }
        }
        if (!aborted) {
            L("STEP8-HOLD 5s at caps (frames + 1HZ samples)...");
            double h0 = g.now();
            while (g.now() - h0 < 5.0) {
                std::string why;
                if (!g.run_frames(10, why)) {
                    g.res.abort_fired = true; g.res.abort_reason = "present/draw failure during hold: " + why;
                    L("STEP8-STRESS-ABORT during hold: " + why); aborted = true; break;
                }
                if (g.abort_check("hold")) { aborted = true; break; }
            }
            g.take_sample("AFTER-HOLD", true);
        }
    }
    // Over-cap attempts at the end state (if at caps) must deny; harmless otherwise.
    if (atCaps && !aborted) {
        long long d = g.alloc(P_TEXTURE, 1ull << 20, false, "at-cap-over-cap-proof", false);
        L(fmt("STEP8-AT-CAP-DENY texture +1MiB at hard cap -> %lld (0=denied as required)", d));
        if (d != 0) { g.res.abort_fired = true; g.res.abort_reason = "over-cap allocation at cap was NOT denied"; aborted = true; }
    }
    g.res.stress_completed = atCaps && !aborted;
    g.free_all(false); // free everything incl. accounting baseline (baseline had no backing)
    g.take_sample("AFTER-FREE-ALL", true);
    L(fmt("STEP8-SUMMARY denials=%llu evictions=%llu evicted-bytes=%llu threshold-crossings=%llu alloc-failures-outside-denial=%llu peak-governed-figure=%llu peak-total-pool-bytes=%llu abort-fired=%d reason=%s",
        g.res.denials, g.res.evictions, g.res.evicted_bytes, g.res.threshold_crossings,
        g.res.alloc_failures_outside_denial, g.res.peak_governed_figure, g.res.peak_total_pool_bytes,
        g.res.abort_fired ? 1 : 0, g.res.abort_reason.empty() ? "(none)" : g.res.abort_reason.c_str()));
    L(fmt("STEP8-GOVERNOR-VERDICT: %s (reserve-commit=%s denial=%s eviction=%s stress-to-caps=%s abort=%s)",
        g.res.pass() ? "PASS" : "FAIL/ABORTED",
        g.res.reserve_commit_ok ? "PASS" : "FAIL", g.res.denial_ok ? "PASS" : "FAIL",
        g.res.eviction_ok ? "PASS" : "FAIL", g.res.stress_completed ? "COMPLETED" : "NOT-COMPLETED",
        g.res.abort_fired ? g.res.abort_reason.c_str() : "none"));
    L("STEP8-CAVEATS: pools are synthetic committed CPU pages standing in for texture/surface, shader-cache, upload and guest pools. "
      "D3D12 default-heap texture residency at 1200 MiB, real shader-cache volume, and real title memory behaviour are NOT proven here.");
    return g.res;
}

