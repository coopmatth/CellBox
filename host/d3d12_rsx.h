// d3d12_rsx.h — v1.0.36.0: Slice 1 (display blit) + Slice 2 (scratch RTV
// + passthrough PSO real draws) on top of the 1.0.35 counting backend.
// Host-only: no RPCS3 core file modified.
//
// SCOPE, read before claiming anything (1.0.36 additions):
//  - Device handoff: the host /ZW TU calls SetD3D12HostBridge() (see
//    rsx_counters.h) before Emu.BootGame() with the host D3D12 device,
//    DIRECT queue, IDXGISwapChain3, and 1280x720 (the RSX buffer size
//    for Series S — NOT the swapchain size; the blit scales to the
//    backbuffer at present time). D3D12GSRender's constructor is
//    UNCHANGED (g_fxo->init inside Emulator::Load()).
//  - Nop fallback preserved: bridge device null -> begin()/end()/flip()
//    stay on the 1.0.35 counted-nop path, exactly as before.
//  - Slice 1 (flip): display_buffers[info.buffer] (thread member via
//    GCM_context; RsxDisplayInfo { offset, pitch, width, height }) ->
//    guest address = rsx::get_address(offset, CELL_GCM_LOCATION_LOCAL)
//    (GLPresent.cpp precedent, linear read, no tile check). If the mode
//    is exactly 1280x720 and the guest buffer holds non-zero pixels,
//    upload it pitch-aware via the upload ring into a 1280x720
//    R8G8B8A8_UNORM texture (guest bytes copied as-is; the BGRA channel
//    order is fixed in hardware by the SRV component mapping (1,2,3,0)
//    — NO CPU byte-swap of colors, per Argon's do-not list). Otherwise
//    the scratch RTV (cleared with the RSX clear color in begin()) is
//    blitted. Blit PSO: fullscreen triangle, Y-orientation handled by
//    uv.y = 1-p.y (guest row 0 -> screen top), 720p->backbuffer scaling
//    via the viewport. Non-720p modes fall back to scratch + a
//    BOOT-SLICE-DISPLAY line naming the real mode.
//  - Slice 2 (end): analyse_current_rsx_pipeline() (tree-correct name),
//    then GRAPH_frontend().analyse_inputs_interleaved() (the same
//    backend-agnostic analyse VK/GL use) to find the block holding
//    vertex attribute 0 (POS). POS is converted to float3 and uploaded
//    into the vertex upload ring; a build-time dxc HLSL passthrough VS
//    + solid-magenta PS draws it into the persistent 1280x720 scratch
//    RTV, which flip() blits to the swapchain. Real draws are counted
//    separately (s_real_draws); skipped draws are counted per reason
//    (s_skip_reasons) with a first-occurrence watchdog line each.
//  - Slice 2 APPROXIMATIONS (named, not hidden):
//    * POS is treated as clip space (x,y) by the passthrough VS. Correct
//      for pre-transformed 2D/menu geometry; 3D object-space geometry
//      renders distorted until a real vertex program runs (Slice 3+).
//    * Indexed draws upload vertices [0, max_index] and draw indexed
//      as-is (correct indexing; POS-only, no other attributes).
//    * Non-indexed draws use the draw clause subranges (first,count).
//    * Primitive mapping: triangles/strip exact; fan->list, line_loop->
//      strip, quads/quad_strip/polygon->list (visible but wrong shape).
//    * POS types handled: float, SNORM16, UNORM8. Other types, immediate
//      (push-buffer) draws, single-vertex blocks, and >64K-vertex draws
//      are skipped with a counted reason, never faked.
//    * Guest multi-byte values are big-endian (Step 6 color-swizzle
//      precedent): vertex floats and index words are BE->LE swapped on
//      upload. If console geometry looks scrambled, the swap is the
//      first suspect — but the swap is NOT skipped silently.
//    * No surface tracking, no per-surface formats, no textures, no
//      depth, no real vertex/fragment programs yet (Slice 3+).
//  - get_report / flip_handler: NOT touched here. They live in core
//    (nv4097.cpp:644 get_report; RSXThread.cpp flip orchestration) and
//    are backend-independent — see slice12-1036-report.md for the proof.
//    No fake semaphore signalling, no timeout-cap change (the acquire
//    already breaks on driver_recovery_timeout and the game progresses).

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
#include <wrl/client.h>

#include "Emu/RSX/GSRender.h"
#include "rsx_counters.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>

class D3D12GSRender : public GSRender
{
public:
    D3D12GSRender(utils::serial* ar) noexcept;
    D3D12GSRender() noexcept : D3D12GSRender(nullptr) {}

    u64 get_cycles() final;

    // Host-visible counters (RSX thread writes, host watch loop reads).
    static std::atomic<u64> s_init_count;  // on_init_thread calls
    static std::atomic<u64> s_begin_count; // begin() calls
    static std::atomic<u64> s_end_count;   // end() calls = draw completions
    static std::atomic<u64> s_flip_count;  // flip() calls from the guest

    // Shader-cache instrumentation (RSX thread writes, host reads).
    static std::atomic<u64> s_shader_distinct; // distinct (vp,fp) pairs seen
    static std::atomic<u64> s_shader_hits;     // end() calls whose pair is in the cache file
    static std::atomic<u64> s_shader_misses;   // distinct pairs NOT in the cache file
    static std::atomic<u64> s_cache_entries;   // entries loaded from the cache file (0 = missing)

    // Budget usage, bytes.
    static std::atomic<u64> s_rtt_bytes;
    static std::atomic<u64> s_texture_bytes;
    static std::atomic<u64> s_upload_bytes;
    static std::atomic<u64> s_pso_bytes;

    // 1.0.36 device handoff state (public: the free functions in
    // rsx_counters.h — SetD3D12HostBridge, d3d12_bridge_owns,
    // d3d12_bridge_owns_ptr — are NOT members and must see these).
    // Bridge as handed by the host (non-owning pointers; the host's
    // device/queue/swapchain outlive Run()).
    static D3D12HostBridge s_bridge; // struct declared in rsx_counters.h
    static std::atomic<bool> s_bridge_owns_static; // d3d12_bridge_owns_ptr()

    // Slice 2 real-draw counters (1.0.36). s_end_count still counts ALL
    // end() calls (comparability with 1.0.35); s_real_draws counts only
    // end() calls that executed a real D3D12 draw. Once s_real_draws > 0
    // it governs "draws executed"; draws(nop) stays as the denominator.
    static std::atomic<u64> s_real_draws;
    static std::atomic<u64> s_real_skips; // total skipped real-draw attempts
    // Skip reasons (index into s_skip_reasons, first occurrence each
    // logged to boot-watchdog.txt as BOOT-DRAW-SKIP):
    enum SkipReason : u32
    {
        kSkipLayout = 0,   // layout.validate() false / no usable block
        kSkipImmediate,    // immediate (push-buffer) draw, not yet handled
        kSkipNoPos,        // attribute 0 size == 0
        kSkipType,         // POS type not float/SNORM16/UNORM8
        kSkipOffset,       // POS offset outside its interleaved block stride
        kSkipOverflow,     // vertex/index upload would exceed the 8 MiB ring
        kSkipUnmapped,     // guest vertex/index range failed vm::check_addr
        kSkipSingleVertex, // single-vertex block (constant POS, no triangle)
        kSkipEmptyClause,  // 1.0.37 guard: current_draw_clause.empty() at end()
        kSkipInlinedArray, // 1.0.37 guard: command == inlined_array (get_range N/A)
        kSkipNoCommand,    // 1.0.37 guard: command == none (or other non-array/indexed)
        kSkipCount
    };
    static std::atomic<u64> s_skip_reasons[kSkipCount];

    void on_init_thread() override;
    void on_exit() override;
    void begin() override;
    void flip(const rsx::display_flip_info_t& info) override;

private:
    void end() override;

    // Cache-file lookup. Called from on_init_thread (load) and end().
    void load_shader_cache();
    bool cache_lookup(u64 vp_hash, u64 fp_hash) const;

    struct PairHash
    {
        usz operator()(const std::pair<u64, u64>& p) const noexcept
        {
            u64 h = p.first + 0x9e3779b97f4a7c15ull;
            h ^= p.second + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return static_cast<usz>(h);
        }
    };

    // Static: single RSX thread; host free functions read these.
    static std::unordered_set<std::pair<u64, u64>, PairHash> s_seen_pairs;
    static std::unordered_set<std::pair<u64, u64>, PairHash> s_cache_pairs;

    // Miss ring: last 64 distinct misses, oldest overwritten. The
    // host watch loop drains it by ordinal (see rsx_counters.h).
    static constexpr u64 kMissRingSize = 64;
    static std::array<std::pair<u64, u64>, kMissRingSize> s_miss_ring;
    static std::atomic<u64> s_miss_next; // total distinct misses ever (= s_shader_misses)

    static std::string s_cache_dir; // set by boot_phase before BootGame
    static std::string s_cache_path; // resolved cache file path (for host log)

    // ---- Slice 1+2 device state (RSX thread only) ----
    // D3D12 resources, created lazily on first bridge-ready begin().
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_slice_alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_slice_list;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_slice_rs;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_blit_rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_slice_pso_tri;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_slice_pso_line;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_slice_pso_point;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_blit_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_scratch_tex;   // 1280x720 R8G8B8A8 RTV
    Microsoft::WRL::ComPtr<ID3D12Resource> m_display_tex;   // 1280x720 R8G8B8A8 texture
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vtx_upload;    // 8 MiB upload ring (vertices+indices)
    Microsoft::WRL::ComPtr<ID3D12Resource> m_disp_upload;   // 4 MiB upload (display buffer)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtv_heap; // 3: scratch + 2 swapchain
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srv_heap; // 2, shader-visible: display(swizzle) + scratch(identity)
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fence_event = nullptr;
    UINT64 m_fence_value = 0;
    bool m_slice_ok = false;       // resources + PSOs created
    bool m_slice_pso_logged = false;
    bool m_list_open = false;      // command list open for this frame
    bool m_owns_logged = false;    // BOOT-RSX-OWNS-SWAPCHAIN written
    bool m_swap_rtvs_made = false;
    D3D12_RESOURCE_STATES m_scratch_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    D3D12_RESOURCE_STATES m_display_state = D3D12_RESOURCE_STATE_COPY_DEST;
    UINT64 m_up_cursor = 0;        // vertex/index upload ring cursor (bytes)
    UINT64 m_draws_at_execute = 0; // 1.0.40: s_real_draws at last Execute (draws_since_execute base)
    std::unordered_set<u64> m_vp_logged; // 1.0.40: VP hashes already BOOT-VP-DIAG-logged
    u64 m_vp_log_draws = 0;        // 1.0.40: draws BOOT-VP-DIAG-logged (first few overall)
    UINT8* m_vtx_cpu = nullptr;    // persistently mapped upload pointers
    UINT8* m_disp_cpu = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS m_vtx_gpu = 0;
    bool m_skip_logged[kSkipCount] = {};

    bool slice_ensure_resources(); // create all Slice resources + PSOs (logs failures once)
    void slice_begin_frame();      // fence-wait, reset list, bind + clear scratch RTV
    void slice_open_list_if_needed(); // defensive open (flip/end without begin)
    void slice_note_skip(SkipReason r); // count + first-occurrence watchdog line
    bool slice_try_real_draw();    // analyse + upload + draw; true = real draw executed
    void slice_present(const rsx::display_flip_info_t& info); // blit + Present + owns gate

public:
    // Host free-function access (rsx_counters.h) goes through these.
    static bool miss_at(u64 ordinal, u64* vp, u64* fp);
    static const std::string& cache_path();
    static void set_cache_dir(const std::string& dir) { s_cache_dir = dir; }
};

// Cache-dir handoff: boot_phase (which owns the LocalState path)
// calls this before BootGame; on_init_thread reads it. The /ZW host
// TU cannot include this header, so the setter is also declared in
// rsx_counters.h as a free function.
void d3d12_rsx_set_cache_dir(const std::string& dir);
