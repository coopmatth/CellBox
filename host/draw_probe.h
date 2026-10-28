#pragma once
// Step 6 draw + null-flip probe (shared PC pre-test / UWP console host).
// Real D3D12 indexed textured draw through the Step 5 pipeline:
// SPIR-V -> pre-screen (Step 2 rules) -> Mesa spirv_to_dxil (out-of-process
// on PC; packaged unsigned DXIL fallback on UWP) -> in-memory container
// hash -> Step 5 cache -> graphics PSO. Root signature per Step 5 budget:
// [0] root CBV cb0/space30 (push block, windowed into an upload ring),
// [1] SRV descriptor table t4/space1, [2] sampler table s4/space1 (4 DWORDs
// of 64). Null-flip: synthetic 1280x720 A8R8G8B8 display buffer, CPU-swizzled
// staging copy via GetCopyableFootprints, CopyTextureRegion to a persistent
// texture then the back buffer, Present(1,0) + fence. No game data anywhere.
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

// Per-mode metrics (draw and null-flip are measured separately).
struct Step6Metrics {
    unsigned long long frames_attempted = 0;
    unsigned long long frames_presented = 0;
    unsigned long long present_failures = 0;
    long first_present_fail_hr = 0;
    long last_present_hr = 0;
    unsigned long long close_failures = 0;
    long last_close_hr = 0;
    unsigned long long draw_calls = 0;      // DrawIndexedInstanced issues (draw mode)
    unsigned long long cpu_stage_us_total = 0; // null-flip staging copy time
    unsigned long long intervals_over_25ms = 0;
    unsigned long long intervals_over_50ms = 0;
    double min_interval_ms = 0.0, max_interval_ms = 0.0, last_interval_ms = 0.0;
    double elapsed_s = 0.0;
    unsigned last_pixel_rgba[4] = {0, 0, 0, 0}; // readback sample
    long last_readback_hr = -1;
};

// Step 2 pre-screen over a SPIR-V module: rejects FragmentBarycentricKHR
// (5284) and InputAttachment (40) capabilities before any translator call.
// Returns true if the module may be translated; log receives the verdict.
bool prescreen_spirv(const std::vector<unsigned char>& spv, const char* label, std::string& log);

// Step 5 cache key: SHA256 over length-prefixed (SPIR-V || domain ||
// translator version || root-sig version || binding-remap), hex out.
// Pure-C++ SHA-256 (no BCrypt import); verified against Step 5 keys.
std::string step5_cache_key(const std::vector<unsigned char>& spv, const char* domain);

// Resolve one shader's signed DXIL via the Step 5 cache:
// lookup cacheDir\dxil\<key>_<domain>.dxil -> hit: load + hash-idempotence
// verify. Miss: if translatorExe is non-empty and runnable, translate
// spvPath out-of-process; else use packagedUnsigned (logged as fallback).
// Then sign in memory, store to cache, return signed bytes. Empty on failure.
struct DrawShaderInputs {
    std::wstring cacheDir;        // writable cache root (LocalState on console)
    std::wstring translatorExe;   // spirv2dxil.exe path, PC only (empty = skip)
    std::vector<unsigned char> spvVs, spvFs;
    std::vector<unsigned char> packagedVs, packagedFs; // unsigned Mesa DXIL
};
struct DrawShaderOutputs {
    std::vector<unsigned char> signedVs, signedFs;
    std::string keyVs, keyFs;
    std::string log;
    bool ok = false;
};
DrawShaderOutputs resolve_draw_dxil(const DrawShaderInputs& in);

class Step6Loop {
public:
    // Creates the draw root signature + PSO (records HRESULTs; ok=false if
    // the PSO failed) and all draw/null-flip resources. swap1 is the
    // host-created Step 4 swapchain (same queue). Two Step6Loop phases and
    // the Step 4 PresentLoop may share the swapchain sequentially because
    // every frame is fence-serialised.
    std::string init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain1* swap1,
                     const std::vector<unsigned char>& signedVs, const std::vector<unsigned char>& signedFs,
                     long* psoHr, bool* ok);
    HRESULT render_draw_frame();     // one indexed textured draw + present
    HRESULT render_nullflip_frame(); // one synthetic-buffer copy + present
    std::string draw_summary(const char* tag) const;
    std::string nullflip_summary(const char* tag) const;
    const Step6Metrics& draw_metrics() const { return m_draw; }
    const Step6Metrics& nullflip_metrics() const { return m_flip; }
    unsigned width() const { return m_width; }
    unsigned height() const { return m_height; }

private:
    std::string summary_of(const Step6Metrics& m, const char* tag) const;
    void note_present(Step6Metrics& m, HRESULT hr, long long qpcBefore);
    HRESULT wait_fence();
    Microsoft::WRL::ComPtr<ID3D12Device> m_device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtv;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> m_back;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEv = nullptr;
    unsigned m_rtvSize = 0;
    unsigned long long m_fv = 0;
    unsigned m_width = 0, m_height = 0, m_bufferCount = 0;
    long long m_qpf = 0, m_drawStartQpc = 0, m_flipStartQpc = 0;
    long long m_drawLastQpc = 0, m_flipLastQpc = 0;
    // draw resources
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSig;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pso;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_vb, m_ib, m_tex, m_texUpload;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_pushRing, m_readback;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvHeap, m_samplerHeap;
    unsigned char* m_pushMapped = nullptr;
    D3D12_VERTEX_BUFFER_VIEW m_vbView{};
    D3D12_INDEX_BUFFER_VIEW m_ibView{};
    // null-flip resources (synthetic 1280x720 guest A8R8G8B8 buffer)
    static constexpr unsigned kGuestW = 1280, kGuestH = 720;
    std::vector<unsigned> m_guest; // A8R8G8B8 pixels, generated in code
    Microsoft::WRL::ComPtr<ID3D12Resource> m_flipTex, m_flipStaging;
    unsigned char* m_flipMapped = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_flipFootprint{};
    unsigned long long m_flipStagingBytes = 0;
    Step6Metrics m_draw, m_flip;
};
