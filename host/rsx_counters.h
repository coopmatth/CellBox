// rsx_counters.h — lightweight accessor declarations for the
// D3D12GSRender counters (v1.0.36.0). Deliberately free of Emu
// includes so the /ZW main_uwp TU (which has no emucore include
// paths) can read them. Defined in d3d12_rsx.cpp.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <cstdint>
#include <string>

std::uint64_t d3d12_rsx_init_count();
std::uint64_t d3d12_rsx_begin_count();
std::uint64_t d3d12_rsx_end_count();
std::uint64_t d3d12_rsx_flip_count();
std::uint64_t d3d12_rsx_shader_distinct();
std::uint64_t d3d12_rsx_shader_hits();
std::uint64_t d3d12_rsx_shader_misses();
std::uint64_t d3d12_rsx_shader_cache_entries();
std::uint64_t d3d12_rsx_rtt_bytes();
std::uint64_t d3d12_rsx_texture_bytes();
std::uint64_t d3d12_rsx_upload_bytes();
std::uint64_t d3d12_rsx_pso_bytes();
// Miss-ring drain: ordinal = distinct-miss index (0-based). Returns
// false when the ordinal was overwritten (ring holds the last 64) or
// is beyond the total — the host logs an overflow line in that case.
bool d3d12_rsx_shader_miss_at(std::uint64_t ordinal, std::uint64_t* vp, std::uint64_t* fp);
std::string d3d12_rsx_shader_cache_path();
// Cache-dir handoff (called by boot_phase before BootGame).
void d3d12_rsx_set_cache_dir(const std::string& dir);

// ---- 1.0.36 device handoff (plain C++ POD, no Emu types) ----
// The host /ZW TU fills this BEFORE Emu.BootGame() and passes it to
// SetD3D12HostBridge(). Width/height are the RSX buffer size
// (1280x720, Series S target) — NOT the swapchain size; the Slice 1
// blit scales to the actual backbuffer at present time. Pointers are
// non-owning: the host device/queue/swapchain outlive Run().
// D3D12GSRender's constructor is NOT changed (g_fxo->init inside
// Emulator::Load()).
struct D3D12HostBridge
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain3* swapchain = nullptr;
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::atomic<bool>* rsx_owns_swapchain = nullptr;
};
void SetD3D12HostBridge(const D3D12HostBridge& bridge);
// True once the RSX backend has presented its first valid flip
// (bridge atomic, release semantics, set in D3D12GSRender::flip()).
// The host pump loop uses this — NOT the flip count — as the
// host-present-stop gate.
bool d3d12_bridge_owns();
// Pointer to the backend-owned owns flag, for the host to hand back
// inside D3D12HostBridge::rsx_owns_swapchain.
std::atomic<bool>* d3d12_bridge_owns_ptr();
// Slice 2 real-draw accounting (see d3d12_rsx.h): real draws executed
// vs end() calls that fell back to the counted nop (per-reason counts
// live in boot-watchdog.txt as BOOT-DRAW-SKIP first occurrences).
std::uint64_t d3d12_rsx_real_draws();
std::uint64_t d3d12_rsx_real_skips();
