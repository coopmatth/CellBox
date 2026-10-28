// D3D12GSRender prototype — Jarvis-direct integration, header-only.
// Design rules (from the 2026-10-26 traps, applied, not pasted):
//  * Qt-free: NO headless_application, NO main_application, NO Qt types.
//    The backend is pure C++ and is driven by callbacks the C++/CX host
//    supplies (draw/flip frame sinks, log sink). Emu itself is NOT linked
//    here; real PPU/SPU LLVM execution lives in the PC RPCS3 build and is
//    proven there by the PC title test. On Xbox this file is the backend
//    SHAPE (FIFO -> begin/end -> draw -> flip) with synthetic commands.
//  * Y-flip, single place only: viewport negative height, NEVER also
//    spirv_to_dxil flip_yz. Flipping Y inverts winding, so
//    FrontCounterClockwise is inverted alongside it. The flip is proven
//    by a position-sensitive pixel test (left/right split quad), not by
//    the uniform tint of Step 6 — a uniform tint cannot see a flip.
//  * LLVM JIT working set: llvm_threads is pinned to 1 in this prototype
//    (a single JIT worker slot). Per-module LLVMContext/Module temporaries
//    must be destroyed immediately after emission to the JIT arena —
//    MEM_RESERVE is address space only (Steps 1/8) and never counts as
//    the risk; the compile-time working set is. Existing RPCS3 cache
//    machinery (ppu_progs / spu_progs / shaderlog) is to be reused before
//    any new cache is invented, and large caches go to USB/removableStorage,
//    never LocalState.
//  * B button: see BackButtonPolicy below. The host must verify
//    BackRequested actually fires in the CoreApplication (non-XAML) shape
//    on console before relying on it; until that console proof exists,
//    B-as-Circle is a design, not a measured behaviour.
//  * Governor: every pool byte here is accounted against the Step 8 caps
//    (texture 1200 / shader 256 / upload 256 / guest 512 MiB, abort 3.0 GiB).
//    Texture/surface pools are NON-evictable (in-flight resources must not
//    vanish): a full texture cache DENIES, it never evicts. Shader cache
//    is evictable (LRU), as in Step 8.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

namespace d3d12_backend {

using LogFn = std::function<void(const std::string& line)>;
using FrameFn = std::function<HRESULT()>; // draw or flip frame sink

// ---- Y-flip, single place ----
// D3D12 viewport with negative height flips Y. This is the ONLY flip in
// the backend: the translator's flip_yz conf must stay OFF, or geometry
// flips twice and winding inverts twice (net correct picture, wrong
// culling state — the exact trap this flag exists to prevent).
struct ViewportFlip {
    bool y_flip = false; // true = Vulkan-origin shaders (gl_Position Y-up)
    float top_left_y(float render_h) const { return y_flip ? render_h : 0.0f; }
    float height(float render_h) const { return y_flip ? -render_h : render_h; }
    // D3D12_RASTERIZER_DESC::FrontCounterClockwise, as the backend must set it.
    bool front_counter_clockwise(bool base_ccw) const { return y_flip ? !base_ccw : base_ccw; }
};

// ---- ROP / depth state (decoded, not guessed) ----
struct RopState {
    bool depth_test_enable = true;
    bool depth_write_enable = true;
    D3D12_COMPARISON_FUNC depth_func = D3D12_COMPARISON_FUNC_LESS;
    bool blend_enable = false;
    D3D12_BLEND src_blend = D3D12_BLEND_SRC_ALPHA;
    D3D12_BLEND dst_blend = D3D12_BLEND_INV_SRC_ALPHA;
    unsigned sample_count = 1; // MSAA: prototype is 1 (off). >1 is requested,
                               // logged, and falls back to 1 — never claimed.
    float render_scale = 1.0f; // prototype renders 1280x720, scale 1.0
};

// ---- Texture / surface caches (guest-address keyed, as VK caches are) ----
struct CacheKey {
    uint64_t guest_addr = 0;
    uint32_t format = 0, width = 0, height = 0;
    bool operator==(const CacheKey& o) const {
        return guest_addr == o.guest_addr && format == o.format && width == o.width && height == o.height;
    }
};
struct CacheKeyHash {
    size_t operator()(const CacheKey& k) const {
        return std::hash<uint64_t>{}(k.guest_addr) ^ (std::hash<uint32_t>{}(k.format) << 1) ^
               (std::hash<uint32_t>{}(k.width) << 2) ^ (std::hash<uint32_t>{}(k.height) << 3);
    }
};
struct CacheEntry { uint64_t bytes = 0; uint64_t last_use = 0; bool evictable = false; };

// ---- Guest memory map (prototype shape of RSX local + main RAM) ----
// 512 MiB address space (256 main + 256 RSX analogue, Step 8 guest cap).
// Reserve is address space only; commit happens in 64 MiB chunks, touched
// page by page, pattern-verified — the Step 8 reserve/commit measurement,
// reused, not re-proven here.
// NOTE: GuestMemoryMap methods live in d3d12_gsrender.cpp, compiled
// WITHOUT /ZW like memory_governor.cpp. Inlining VirtualAlloc calls into
// the /ZW main_uwp.obj breaks the UWP link (LNK2005 VirtualAlloc vs
// windowsapp.lib), so they are deliberately out-of-line here.
class GuestMemoryMap {
public:
    static constexpr uint64_t kTotal = 512ull << 20;
    static constexpr uint64_t kChunk = 64ull << 20;
    bool reserve();
    bool commit_chunk(unsigned chunk_index, uint8_t pattern);
    void* translate(uint32_t offset, int location) const;
    void release();
    uint64_t committed() const { return committed_; }
    bool reserved() const { return base_ != nullptr; }
private:
    void* base_ = nullptr;
    uint64_t committed_ = 0;
};

// ---- B button policy (design, console proof pending) ----
// Xbox UWP: B can raise OS BackRequested and suspend/minimise the app
// (Step 7 observation: B closed the app). Policy: the host handles
// BackRequested with Handled=true so the OS never kills/suspends on B;
// while a game runs, B is delivered to the pad handler as PS3 Circle;
// the emulator back/pause overlay is a two-button combo only
// (View+Menu, or L3+R3), never B alone. Until BackRequested is proven
// to fire in the CoreApplication (non-XAML) host on console, this is a
// design to verify, not a measured behaviour — the host must log whether
// the handler fired on its first console run.
struct BackButtonPolicy {
    bool game_running = false;
    bool back_requested_handled = false; // set true by the host handler
    // Returns: 0 = ignore, 1 = deliver Circle to game, 2 = overlay request.
    int on_back_requested(bool view_and_menu, bool l3_and_r3) const {
        if (view_and_menu || l3_and_r3) return 2;
        if (game_running) return 1;
        return 0;
    }
};

// ---- CPU model (design enforcement, not a linked core) ----
// llvm_threads is pinned to 1. PPU/SPU execution is NOT linked in this
// prototype: start_ppu/start_spu log that fact plainly. Real PPU/SPU
// LLVM execution is exercised by the PC RPCS3 build in the PC title test,
// not faked here.
struct CpuModel {
    int llvm_threads = 1; // Step 8 note: start at 1, raise to 2 only on measurement
    bool ppu_linked = false, spu_linked = false;
    std::string start_ppu(LogFn log) {
        log("INT-CPU: PPU execution NOT linked in Xbox host prototype (llvm_threads=1 design; real PPU runs in PC RPCS3 build only)");
        return "PPU-NOT-LINKED";
    }
    std::string start_spu(LogFn log) {
        log("INT-CPU: SPU execution NOT linked in Xbox host prototype (llvm_threads=1 design; real SPU runs in PC RPCS3 build only)");
        return "SPU-NOT-LINKED";
    }
};

// ---- Synthetic RSX command stream (FIFO shape, no real title) ----
struct SyntheticCommand {
    enum Kind { kBegin, kEnd, kDrawIndexed, kFlip, kBindTexture, kBindSurface, kSetRop, kSetMsaa } kind;
    uint64_t guest_addr = 0;
    uint32_t format = 0, width = 0, height = 0, arg = 0;
};

// ---- The backend itself ----
class D3D12GSRender {
public:
    D3D12GSRender(LogFn log, FrameFn draw_sink, FrameFn flip_sink)
        : log_(log), draw_sink_(draw_sink), flip_sink_(flip_sink) {}
    void set_y_flip(bool v) { flip_.y_flip = v; log_("INT-YFLIP: mode=viewport-negative-height flip_yz=OFF frontCCW-inverted=" + std::string(v ? "1" : "0")); }
    void set_render_size(unsigned w, unsigned h) { render_w_ = w; render_h_ = h; }
    ViewportFlip viewport() const { return flip_; }
    float viewport_top_y() const { return flip_.top_left_y((float)render_h_); }
    float viewport_height() const { return flip_.height((float)render_h_); }
    bool front_ccw(bool base) const { return flip_.front_counter_clockwise(base); }

    bool begin() { ++begins_; log_("INT-BEGIN: rsx::thread::begin() mirror (backend-virtual, VKGSRender::begin shape)"); return true; }
    bool end() { ++ends_; log_("INT-END: rsx::thread::end() mirror (draw_calls++ + post_execute_cleanup shape)"); return true; }

    bool bind_texture(const CacheKey& k, uint64_t bytes) {
        if (texture_bytes_ + bytes > kCapTexture) {
            ++texture_denials_;
            log_("INT-TEXTURE-DENY: guest=0x" + hex(k.guest_addr) + " bytes=" + std::to_string(bytes) + " (texture pool non-evictable: deny, never evict in-flight)");
            return false;
        }
        auto it = textures_.find(k);
        if (it == textures_.end()) { textures_[k] = CacheEntry{bytes, ++tick_, false}; texture_bytes_ += bytes; }
        else it->second.last_use = ++tick_;
        return true;
    }
    bool bind_surface(const CacheKey& k, uint64_t bytes) {
        if (surface_bytes_ + bytes > kCapTexture) { ++surface_denials_; log_("INT-SURFACE-DENY: over texture cap"); return false; }
        auto it = surfaces_.find(k);
        if (it == surfaces_.end()) { surfaces_[k] = CacheEntry{bytes, ++tick_, false}; surface_bytes_ += bytes; }
        else it->second.last_use = ++tick_;
        return true;
    }
    // Shader cache IS evictable (LRU), Step 8 shape.
    bool insert_shader(const std::string& key, uint64_t bytes) {
        while (shader_bytes_ + bytes > kCapShaderSoft && !shader_lru_.empty()) {
            auto old = shader_lru_.front(); shader_lru_.pop_front();
            auto it = shaders_.find(old);
            if (it != shaders_.end()) { shader_bytes_ -= it->second.bytes; ++shader_evictions_; shaders_.erase(it); }
        }
        if (shader_bytes_ + bytes > kCapShaderHard) { ++shader_denials_; return false; }
        if (!shaders_.count(key)) { shaders_[key] = CacheEntry{bytes, ++tick_, true}; shader_bytes_ += bytes; shader_lru_.push_back(key); }
        return true;
    }
    void set_rop(const RopState& r) {
        rop_ = r;
        if (r.sample_count != 1) log_("INT-MSAA: requested samples=" + std::to_string(r.sample_count) + " -> fallback 1 (MSAA path not implemented in prototype; never claimed)");
        else log_("INT-ROP: depth_test=" + std::to_string(r.depth_test_enable) + " depth_write=" + std::to_string(r.depth_write_enable) + " blend=" + std::to_string(r.blend_enable) + " msaa=1 scale=" + std::to_string(r.render_scale));
    }
    bool guest_reserve() { bool ok = guest_.reserve(); log_(std::string("INT-GUEST-MAP: reserve 512MiB (256 main + 256 RSX) ") + (ok ? "PASS" : "FAIL")); return ok; }
    bool guest_commit(unsigned chunk, uint8_t pattern) {
        bool ok = guest_.commit_chunk(chunk, pattern);
        log_("INT-GUEST-MAP: commit chunk=" + std::to_string(chunk) + " pattern-verify=" + (ok ? "PASS" : "FAIL") + " committed=" + std::to_string(guest_.committed()));
        return ok;
    }
    void* guest_translate(uint32_t offset, int location) { return guest_.translate(offset, location); }
    void guest_release() { guest_.release(); }

    // FIFO dispatch, mirroring RSXFIFO -> draw_command_processor -> begin/end.
    bool run_fifo(const std::vector<SyntheticCommand>& cmds, std::string& fail_reason) {
        for (auto& c : cmds) {
            switch (c.kind) {
            case SyntheticCommand::kBegin: begin(); break;
            case SyntheticCommand::kEnd: end(); break;
            case SyntheticCommand::kDrawIndexed: {
                HRESULT hr = draw_sink_ ? draw_sink_() : E_FAIL;
                if (FAILED(hr)) { fail_reason = "draw frame failed"; return false; }
                ++draw_calls_; break; }
            case SyntheticCommand::kFlip: {
                HRESULT hr = flip_sink_ ? flip_sink_() : E_FAIL;
                if (FAILED(hr)) { fail_reason = "flip frame failed"; return false; }
                ++flips_; break; }
            case SyntheticCommand::kBindTexture: bind_texture(CacheKey{c.guest_addr, c.format, c.width, c.height}, c.arg ? c.arg : 65536); break;
            case SyntheticCommand::kBindSurface: bind_surface(CacheKey{c.guest_addr, c.format, c.width, c.height}, c.arg ? c.arg : 3932160); break;
            case SyntheticCommand::kSetRop: { RopState r; r.depth_test_enable = c.arg & 1; r.depth_write_enable = c.arg & 2; set_rop(r); break; }
            case SyntheticCommand::kSetMsaa: { RopState r = rop_; r.sample_count = c.arg; set_rop(r); break; }
            }
        }
        return true;
    }
    std::string summary() const {
        return "INT-BACKEND: begins=" + std::to_string(begins_) + " ends=" + std::to_string(ends_) +
               " draw_calls=" + std::to_string(draw_calls_) + " flips=" + std::to_string(flips_) +
               " textures=" + std::to_string(textures_.size()) + " tex_bytes=" + std::to_string(texture_bytes_) +
               " surfaces=" + std::to_string(surfaces_.size()) + " surf_bytes=" + std::to_string(surface_bytes_) +
               " shaders=" + std::to_string(shaders_.size()) + " shader_bytes=" + std::to_string(shader_bytes_) +
               " shader_evictions=" + std::to_string(shader_evictions_) +
               " tex_denials=" + std::to_string(texture_denials_) + " surf_denials=" + std::to_string(surface_denials_) +
               " shader_denials=" + std::to_string(shader_denials_) +
               " guest_committed=" + std::to_string(guest_.committed()) +
               " y_flip=" + std::to_string(flip_.y_flip ? 1 : 0) +
               " viewport_top_y=" + std::to_string(viewport_top_y()) + " viewport_height=" + std::to_string(viewport_height());
    }
    bool verdict_ok() const { return begins_ > 0 && ends_ > 0 && draw_calls_ > 0 && flips_ > 0; }

private:
    static std::string hex(uint64_t v) { char b[32]; snprintf(b, sizeof(b), "%llX", (unsigned long long)v); return b; }
    static constexpr uint64_t kCapTexture = 1200ull << 20;
    static constexpr uint64_t kCapShaderSoft = 128ull << 20;
    static constexpr uint64_t kCapShaderHard = 256ull << 20;
    LogFn log_; FrameFn draw_sink_, flip_sink_;
    ViewportFlip flip_;
    unsigned render_w_ = 1280, render_h_ = 720;
    RopState rop_;
    uint64_t tick_ = 0, begins_ = 0, ends_ = 0, draw_calls_ = 0, flips_ = 0;
    uint64_t texture_bytes_ = 0, surface_bytes_ = 0, shader_bytes_ = 0;
    uint64_t texture_denials_ = 0, surface_denials_ = 0, shader_denials_ = 0, shader_evictions_ = 0;
    std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> textures_, surfaces_;
    std::unordered_map<std::string, CacheEntry> shaders_;
    std::list<std::string> shader_lru_;
    GuestMemoryMap guest_;
};

} // namespace d3d12_backend
