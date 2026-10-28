#include "present_probe.h"
#include <cstdio>
using Microsoft::WRL::ComPtr;

static void addf(std::string& log, const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    log += buf; log += "\r\n"; fprintf(stderr, "present: %s\n", buf);
}

// 8-bar palette. Bars advance by one position every 15 frames, so at any
// plausible refresh rate the pattern visibly moves several times per second
// and cannot be confused with a solid verdict colour (green/orange/red).
static const FLOAT kPalette[8][4] = {
    {0.85f, 0.10f, 0.10f, 1.0f}, // red
    {0.95f, 0.55f, 0.05f, 1.0f}, // orange (as one bar of 8, not a solid fill)
    {0.90f, 0.85f, 0.10f, 1.0f}, // yellow
    {0.10f, 0.75f, 0.15f, 1.0f}, // green (as one bar of 8)
    {0.05f, 0.65f, 0.85f, 1.0f}, // cyan-blue
    {0.10f, 0.20f, 0.90f, 1.0f}, // blue
    {0.60f, 0.15f, 0.85f, 1.0f}, // violet
    {0.95f, 0.95f, 0.95f, 1.0f}, // white
};

std::string PresentLoop::init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain1* swap1, bool* ok) {
    std::string log;
    *ok = false;
    m_device = device; m_queue = queue;
    HRESULT hr = swap1->QueryInterface(IID_PPV_ARGS(&m_swap));
    addf(log, "SwapChain As IDXGISwapChain3: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    hr = m_swap->GetDesc1(&desc);
    addf(log, "SwapChain GetDesc1: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    m_width = desc.Width; m_height = desc.Height; m_bufferCount = desc.BufferCount;
    addf(log, "SwapChain actual: width=%u height=%u bufferCount=%u format=%d (28=R8G8B8A8_UNORM) swapEffect=%d (4=FLIP_DISCARD) sampleCount=%u flags=0x%X",
        desc.Width, desc.Height, desc.BufferCount, (int)desc.Format, (int)desc.SwapEffect, desc.SampleDesc.Count, desc.Flags);
    if (m_bufferCount == 0 || m_bufferCount > 8) { addf(log, "SwapChain bufferCount out of supported range"); return log; }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = m_bufferCount;
    hr = m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtv));
    addf(log, "CreateDescriptorHeap RTV x%u: %s hr=0x%08lX", m_bufferCount, SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_back.resize(m_bufferCount);
    auto h = m_rtv->GetCPUDescriptorHandleForHeapStart();
    for (unsigned i = 0; i < m_bufferCount; ++i) {
        hr = m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i]));
        addf(log, "SwapChain GetBuffer[%u]: %s hr=0x%08lX", i, SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        if (FAILED(hr)) return log;
        m_device->CreateRenderTargetView(m_back[i].Get(), nullptr, h);
        h.ptr += m_rtvSize;
    }
    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc));
    addf(log, "CreateCommandAllocator: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), nullptr, IID_PPV_ARGS(&m_list));
    addf(log, "CreateCommandList: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    m_list->Close();
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    addf(log, "CreateFence: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    m_fenceEv = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    addf(log, "CreateEventExW fence event: handle=%p", (void*)m_fenceEv);
    if (!m_fenceEv) return log;
    LARGE_INTEGER f{}, t{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    m_qpf = f.QuadPart; m_startQpc = t.QuadPart; m_lastQpc = t.QuadPart;
    *ok = true;
    return log;
}

HRESULT PresentLoop::render_frame() {
    const unsigned long long frame = m_metrics.frames_attempted++;
    const unsigned idx = m_swap->GetCurrentBackBufferIndex();
    m_alloc->Reset();
    m_list->Reset(m_alloc.Get(), nullptr);
    D3D12_RESOURCE_BARRIER bar{}; bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = m_back[idx].Get();
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_list->ResourceBarrier(1, &bar);
    auto hh = m_rtv->GetCPUDescriptorHandleForHeapStart(); hh.ptr += (size_t)idx * m_rtvSize;
    const unsigned shift = (unsigned)((frame / 15) % 8);
    for (unsigned b = 0; b < 8; ++b) {
        D3D12_RECT r{};
        r.left = (LONG)(m_width * b / 8); r.right = (LONG)(m_width * (b + 1) / 8);
        r.top = 0; r.bottom = (LONG)m_height;
        m_list->ClearRenderTargetView(hh, kPalette[(b + shift) % 8], 1, &r);
    }
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_list->ResourceBarrier(1, &bar);
    HRESULT hr = m_list->Close();
    if (FAILED(hr)) { m_metrics.close_failures++; m_metrics.last_close_hr = (long)hr; return hr; }
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    const long long beforePresent = m_lastQpc;
    hr = m_swap->Present(1, 0); // vsync on
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
    const double intervalMs = m_qpf ? (double)(t.QuadPart - beforePresent) * 1000.0 / (double)m_qpf : 0.0;
    m_lastQpc = t.QuadPart;
    m_metrics.last_present_hr = (long)hr;
    m_metrics.last_interval_ms = intervalMs;
    if (m_metrics.frames_attempted > 1) { // first interval includes setup slack
        if (m_metrics.min_interval_ms == 0.0 || intervalMs < m_metrics.min_interval_ms) m_metrics.min_interval_ms = intervalMs;
        if (intervalMs > m_metrics.max_interval_ms) m_metrics.max_interval_ms = intervalMs;
        if (intervalMs > 25.0) m_metrics.intervals_over_25ms++;
        if (intervalMs > 50.0) m_metrics.intervals_over_50ms++;
    }
    m_metrics.elapsed_s = m_qpf ? (double)(t.QuadPart - m_startQpc) / (double)m_qpf : 0.0;
    if (SUCCEEDED(hr)) m_metrics.frames_presented++;
    else {
        m_metrics.present_failures++;
        if (m_metrics.present_failures == 1) m_metrics.first_present_fail_hr = (long)hr;
    }
    const unsigned long long fv = ++m_fv;
    m_queue->Signal(m_fence.Get(), fv);
    if (m_fence->GetCompletedValue() < fv) {
        m_fence->SetEventOnCompletion(fv, m_fenceEv);
        WaitForSingleObjectEx(m_fenceEv, INFINITE, FALSE);
    }
    return hr;
}

std::string PresentLoop::summary(const char* tag) const {
    char buf[1024];
    const double fps = m_metrics.elapsed_s > 0.0 ? (double)m_metrics.frames_presented / m_metrics.elapsed_s : 0.0;
    long removed = -1;
    if (m_device) removed = (long)m_device->GetDeviceRemovedReason();
    unsigned presentCount = 0; long statsHr = -1;
    if (m_swap) { DXGI_FRAME_STATISTICS st{}; HRESULT hr = m_swap->GetFrameStatistics(&st); statsHr = (long)hr; if (SUCCEEDED(hr)) presentCount = st.PresentCount; }
    snprintf(buf, sizeof(buf),
        "%s attempted=%llu presented=%llu present-failures=%llu first-fail-hr=0x%08lX last-present-hr=0x%08lX close-failures=%llu elapsed=%.2fs avg-fps=%.2f interval-ms min=%.2f max=%.2f last=%.2f over25ms=%llu over50ms=%llu frameStats-hr=0x%08lX dxgiPresentCount=%u deviceRemovedReason=0x%08lX",
        tag, m_metrics.frames_attempted, m_metrics.frames_presented, m_metrics.present_failures,
        (unsigned long)m_metrics.first_present_fail_hr, (unsigned long)m_metrics.last_present_hr,
        m_metrics.close_failures, m_metrics.elapsed_s, fps,
        m_metrics.min_interval_ms, m_metrics.max_interval_ms, m_metrics.last_interval_ms,
        m_metrics.intervals_over_25ms, m_metrics.intervals_over_50ms,
        (unsigned long)statsHr, presentCount, (unsigned long)removed);
    return buf;
}
