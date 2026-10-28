#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include "pso_probe.h"
using Microsoft::WRL::ComPtr;

std::vector<unsigned char> load_file_bytes(const std::wstring& path, long* err) {
    std::vector<unsigned char> out;
    HANDLE h = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (h == INVALID_HANDLE_VALUE) { if (err) *err = (long)GetLastError(); return out; }
    LARGE_INTEGER sz{}; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0; ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h); if (err) *err = 0; return out;
}

std::string dxil_hash_summary(const std::vector<unsigned char>& d) {
    char b[256];
    if (d.size() < 32 || memcmp(d.data(), "DXBC", 4) != 0) { snprintf(b, sizeof(b), "not-DXBC size=%zu", d.size()); return b; }
    // DXBC header: FourCC@0 Hash[4]@4 Version@20 Size@24 NumChunks@28 Offsets@32
    unsigned count = 0; memcpy(&count, d.data() + 28, 4);
    std::string parts; bool hashZero = true; bool hasHash = false;
    for (unsigned i = 0; i < count && 32u + i * 4u + 4u <= d.size(); ++i) {
        unsigned off = 0; memcpy(&off, d.data() + 32 + i * 4, 4);
        if (off + 8 > d.size()) continue;
        char tag[5] = {}; memcpy(tag, d.data() + off, 4);
        unsigned sz = 0; memcpy(&sz, d.data() + off + 4, 4);
        char t[64]; snprintf(t, sizeof(t), "%s(%u) ", tag, sz); parts += t;
        if (memcmp(tag, "HASH", 4) == 0) { hasHash = true; for (unsigned k = 0; k < sz && off + 8 + k < d.size(); ++k) if (d[off + 8 + k]) hashZero = false; }
    }
    bool hdrZero = true; for (int k = 4; k < 20; ++k) if (d[k]) hdrZero = false;
    snprintf(b, sizeof(b), "chunks=[%s] headerHash@4=%s HASHchunk=%s", parts.c_str(),
        hdrZero ? "ALL-ZERO(unsigned)" : "non-zero(signed)", !hasHash ? "absent" : (hashZero ? "ALL-ZERO" : "non-zero"));
    return b;
}

std::string run_pso_probe(ID3D12Device* device, const std::vector<unsigned char>& vs,
    const std::vector<unsigned char>& ps, const char* label, long* pso_hr) {
    std::string log; char b[512];
    auto L = [&](const char* s) { log += s; log += "\r\n"; fprintf(stderr, "probe: %s\n", s); };
    snprintf(b, sizeof(b), "--- PSO probe [%s] VS=%zu bytes PS=%zu bytes ---", label, vs.size(), ps.size()); L(b);
    snprintf(b, sizeof(b), "VS container: %s", dxil_hash_summary(vs).c_str()); L(b);
    snprintf(b, sizeof(b), "PS container: %s", dxil_hash_summary(ps).c_str()); L(b);
    if (vs.empty() || ps.empty()) { if (pso_hr) *pso_hr = (long)E_INVALIDARG; L("PSO: FAIL empty bytecode"); return log; }
    // Empty root signature: DXIL sigs show no resource bindings.
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sigBlob, errBlob;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &sigBlob, &errBlob);
    snprintf(b, sizeof(b), "D3D12SerializeRootSignature(empty,ALLOW_IA_LAYOUT): hr=0x%08lX%s", (unsigned long)hr,
        (FAILED(hr) && errBlob) ? (const char*)errBlob->GetBufferPointer() : ""); L(b);
    if (FAILED(hr)) { if (pso_hr) *pso_hr = (long)hr; return log; }
    ComPtr<ID3D12RootSignature> rs;
    hr = device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&rs));
    snprintf(b, sizeof(b), "CreateRootSignature: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr); L(b);
    if (FAILED(hr)) { if (pso_hr) *pso_hr = (long)hr; return log; }
    // Input layout from VS ISG: TEXCOORD0 xyzw float reg0, TEXCOORD1 xyzw float reg1.
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = rs.Get();
    p.VS = {vs.data(), vs.size()};
    p.PS = {ps.data(), ps.size()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = 0xFFFFFFFFu;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = FALSE;
    p.DepthStencilState.StencilEnable = FALSE;
    p.InputLayout = {il, 2};
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1;
    p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.DSVFormat = DXGI_FORMAT_UNKNOWN;
    p.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    hr = device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&pso));
    if (pso_hr) *pso_hr = (long)hr;
    snprintf(b, sizeof(b), "CreateGraphicsPipelineState[%s]: %s hr=0x%08lX", label, SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr); L(b);
    if (device) { HRESULT dr = device->GetDeviceRemovedReason(); snprintf(b, sizeof(b), "GetDeviceRemovedReason after PSO: hr=0x%08lX", (unsigned long)dr); L(b); }
    return log;
}
