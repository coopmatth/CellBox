#pragma once
// Step 4 shared present-loop core (same code on desktop pre-test and console).
// Hosts create the swapchain their own way (HWND on desktop, CoreWindow on
// UWP) and drive the loop; this class owns RTVs, per-frame clears of an
// advancing colour-bar test pattern, fence synchronisation and metrics.
// No shaders/PSO are used to present: ClearRenderTargetView with per-bar
// rects only, so the present result is independent of the DXIL signing gate.
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

struct PresentMetrics {
    unsigned long long frames_attempted = 0;
    unsigned long long frames_presented = 0;   // Present() returned S_OK
    unsigned long long present_failures = 0;
    long first_present_fail_hr = 0;
    long last_present_hr = 0;
    unsigned long long close_failures = 0;
    long last_close_hr = 0;
    unsigned long long intervals_over_25ms = 0;  // dropped/stall indicators
    unsigned long long intervals_over_50ms = 0;
    double min_interval_ms = 0.0;
    double max_interval_ms = 0.0;
    double last_interval_ms = 0.0;
    double elapsed_s = 0.0;
};

class PresentLoop {
public:
    // Takes the host-created swapchain (any IDXGISwapChain1); records every
    // bring-up HRESULT into the returned log. ok=false means do not present.
    std::string init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain1* swap1, bool* ok);
    // Renders + presents one pattern frame (vsync Present(1,0)), fully
    // fence-serialised like the proven Step 1/3 loops. Returns Present hr.
    HRESULT render_frame();
    // One-line metrics summary + device-removed reason at call time.
    std::string summary(const char* tag) const;
    const PresentMetrics& metrics() const { return m_metrics; }
    unsigned width() const { return m_width; }
    unsigned height() const { return m_height; }
    unsigned buffer_count() const { return m_bufferCount; }

private:
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
    long long m_qpf = 0, m_startQpc = 0, m_lastQpc = 0;
    PresentMetrics m_metrics;
};
