// frontend_ui.h - CellBox F1: D3D12 library UI on the existing
// CoreWindow FLIP_DISCARD swapchain (NOT XAML, per frontend-design.md).
// Quads + pre-baked font-atlas text (atlas baked at build time into
// font_atlas.h; shaders dxc-signed at build time into ui_shaders.h -
// the Step 3 control-shader route, console-proven). Frame pacing and
// fence discipline mirror present_probe.cpp (fully serialised).
// Also home of the screens state machine (design doc: frontend_ui =
// "quads, font atlas, screens state machine"): UiState +
// ui_apply_input are pure logic, exercised by the desktop pre-test.
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

#include <string>
#include <vector>

#include "frontend_library.h"
#include "frontend_input.h"
#include "frontend_progress.h"

namespace frontend
{

struct UiColor { float r, g, b, a; };

class FrontendUi
{
public:
    // Bring-up on the host swapchain; multi-line log returned like
    // PresentLoop::init. ok=false means do not present.
    std::string init(ID3D12Device* device, ID3D12CommandQueue* queue,
                     IDXGISwapChain1* swap1, bool* ok);
    void begin_frame(float clearR, float clearG, float clearB);
    // Pixel-space drawing (origin top-left), accumulated until present.
    void quad(float x, float y, float w, float h, UiColor c);
    void text(float x, float y, float scale, UiColor c, const std::string& s);
    float text_width(float scale, const std::string& s) const;
    void progress_bar(float x, float y, float w, float h, int percent,
                      UiColor back, UiColor fill);
    HRESULT present_frame(); // vsync Present(1,0), fence-serialised
    unsigned width() const { return m_width; }
    unsigned height() const { return m_height; }
    unsigned long long frames_presented() const { return m_presented; }

private:
    struct Vert { float x, y, u, v, r, g, b, a; };
    void push_quad(float x, float y, float w, float h,
                   float u0, float v0, float u1, float v1, UiColor c);
    void to_ndc(float px, float py, float& nx, float& ny) const;

    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtv;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srv;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_back;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rs;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_fontTex;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vb;
    HANDLE m_fenceEv = nullptr;
    unsigned m_rtvSize = 0;
    unsigned long long m_fv = 0;
    unsigned long long m_presented = 0;
    unsigned m_width = 0, m_height = 0, m_bufferCount = 0;
    std::vector<Vert> m_verts;
    float m_clear[4] = {};
    bool m_ok = false;
};

// ---- screens state machine ----
enum class Screen { Library, Actions, DeleteConfirm, Loading, Compiling, Stopping };

struct UiState
{
    Screen screen = Screen::Library;
    int selected = 0;     // library index
    int action = 0;       // actions index: 0 Load, 1 Compile(F4), 2 Delete(F2, live in 1.1.3)
    std::string status;   // one-line status shown in the footer area
    // F4 compile details for the selected entry (filled by the host
    // when Actions opens; -1 = not computed / not applicable). Plain
    // data only: the state machine never computes these itself.
    int detail_modules = -1;        // enumerated code modules (folder entries)
    int detail_ppu_files = -1;      // PPU cache files counted for the entry
    std::uint64_t detail_ppu_bytes = 0;
    std::string detail_shader;      // honest one-line shader-cache state
};

enum class UiEvent { None, LoadSelected, CompileSelected, RescanRequested, DeleteSelected, DeleteConfirmed };

// Pure navigation logic. Library: up/down move, A opens actions,
// Menu asks for rescan. Actions: up/down move, A on Load fires
// LoadSelected, A on Compile fires CompileSelected (F4), A on
// Delete moves to the DeleteConfirm screen and fires DeleteSelected
// (the host logs the request; USB entries are bounced back here by
// the host with a status line). DeleteConfirm: A fires
// DeleteConfirmed (the host performs the delete), B returns to
// Actions. Loading/Compiling: input ignored here (the game/compile
// loops own the pad there).
UiEvent ui_apply_input(UiState& st, const MenuInput& in, int entry_count);

// F4 compiling-screen view data (host-computed per frame).
struct CompileView
{
    std::string route;         // route description line
    int modules_total = -1;    // host enumeration (folder route), -1 = n/a
    int cache_files = -1;      // PPU cache files counted live
    std::uint64_t cache_bytes = 0;
    std::string current_module; // from the core's "Trying to load:" notice
    unsigned long long elapsed_ms = 0;
    bool cancelling = false;   // stop requested; takes effect between modules
};

// Renderers (draw only; caller presents).
void ui_render_library(FrontendUi& ui, const UiState& st,
                       const std::vector<GameEntry>& entries,
                       const std::string& statusLine);
void ui_render_actions(FrontendUi& ui, const UiState& st, const GameEntry& e);
// FRONTEND (1.1.3, F2): delete confirmation - names the entry's
// title + size and spells out that the delete is permanent and the
// PPU cache is kept. A = delete permanently, B = back to Actions
// (handled in ui_apply_input; this only draws).
void ui_render_delete_confirm(FrontendUi& ui, const UiState& st, const GameEntry& e);
void ui_render_loading(FrontendUi& ui, const std::string& gameTitle,
                       const ProgressSnapshot& prog, unsigned long long frameTick);
void ui_render_compiling(FrontendUi& ui, const std::string& gameTitle,
                         const CompileView& view, const ProgressSnapshot& prog,
                         unsigned long long frameTick);
// FRONTEND (1.1.2): stopping screen - shown while the frontend waits
// for the core's emulation teardown (Emulation Join Thread) to
// complete before returning to the library. stageLine names the
// current wait stage (FRONTEND-STOP-JOIN stages); the bar sweeps
// (no honest percent exists for a join).
void ui_render_stopping(FrontendUi& ui, const std::string& gameTitle,
                        const std::string& stageLine,
                        unsigned long long elapsedMs,
                        unsigned long long frameTick);

} // namespace frontend
