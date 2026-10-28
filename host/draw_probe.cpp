#include "draw_probe.h"
#include "pso_probe.h"
#include "../xbox-dxil/dxil_sign_candidate.h"
#include <cstdio>
#include <cstring>
using Microsoft::WRL::ComPtr;

static void addf(std::string& log, const char* fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    log += buf; log += "\r\n"; fprintf(stderr, "step6: %s\n", buf);
}

// ---------------- SHA-256 (public-domain style, FIPS 180-4) ----------------
namespace {
struct Sha256 {
    unsigned h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    unsigned char block[64]; unsigned blockLen = 0; unsigned long long total = 0;
    static unsigned rotr(unsigned x, unsigned n) { return (x >> n) | (x << (32 - n)); }
    void compress(const unsigned char* p) {
        static const unsigned k[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
        unsigned w[64];
        for (int i = 0; i < 16; ++i) w[i] = (unsigned(p[i*4])<<24)|(unsigned(p[i*4+1])<<16)|(unsigned(p[i*4+2])<<8)|unsigned(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            unsigned s0 = rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3);
            unsigned s1 = rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10);
            w[i] = w[i-16]+s0+w[i-7]+s1;
        }
        unsigned a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            unsigned S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            unsigned ch = (e&f)^((~e)&g);
            unsigned t1 = hh+S1+ch+k[i]+w[i];
            unsigned S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            unsigned mj = (a&b)^(a&c)^(b&c);
            unsigned t2 = S0+mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void update(const void* data, size_t len) {
        const unsigned char* p = (const unsigned char*)data; total += len;
        while (len) {
            unsigned take = 64 - blockLen; if (take > len) take = (unsigned)len;
            memcpy(block + blockLen, p, take); blockLen += take; p += take; len -= take;
            if (blockLen == 64) { compress(block); blockLen = 0; }
        }
    }
    void finish(unsigned char out[32]) {
        unsigned long long bits = total * 8;
        unsigned char pad = 0x80; update(&pad, 1); total -= 1; // undo bit count change
        unsigned char zero = 0;
        while (blockLen != 56) update(&zero, 1);
        unsigned char lb[8]; for (int i = 0; i < 8; ++i) lb[i] = (unsigned char)(bits >> (56 - i*8));
        // feed length without touching total
        for (int i = 0; i < 8; ++i) { block[blockLen++] = lb[i]; if (blockLen == 64) { compress(block); blockLen = 0; } }
        for (int i = 0; i < 8; ++i) { out[i*4]=(unsigned char)(h[i]>>24); out[i*4+1]=(unsigned char)(h[i]>>16); out[i*4+2]=(unsigned char)(h[i]>>8); out[i*4+3]=(unsigned char)h[i]; }
    }
};
} // namespace

std::string step5_cache_key(const std::vector<unsigned char>& spv, const char* domain) {
    // Identical key material to xbox-dxil\step5_cache_test.cpp (Step 5 cache,
    // not a second cache): length-prefixed fields, same version strings.
    static const char* kSpirvToDxilVer = "mesa-69e0bf9e5177 spirv2dxil sm6.2 pushcb=cb0/space30 rtdata=space31";
    static const char* kRootSigVer = "rs-v1 push<=8dword cbv+ srv-tbl sampler-tbl uav-tbl";
    static const char* kRemap = "remap-v1 identity set->space binding->reg (Step2 corpus defaults)";
    Sha256 s;
    auto feed = [&](const void* p, size_t n) {
        unsigned len = (unsigned)n; s.update(&len, 4); if (n) s.update(p, n);
    };
    feed(spv.data(), spv.size()); feed(domain, strlen(domain));
    feed(kSpirvToDxilVer, strlen(kSpirvToDxilVer));
    feed(kRootSigVer, strlen(kRootSigVer)); feed(kRemap, strlen(kRemap));
    unsigned char digest[32]; s.finish(digest);
    char hex[65]; for (int i = 0; i < 32; ++i) sprintf_s(hex + i*2, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

bool prescreen_spirv(const std::vector<unsigned char>& spv, const char* label, std::string& log) {
    if (spv.size() < 20 || (spv.size() % 4) != 0) { addf(log, "PRESCREEN[%s]: FAIL bad SPIR-V size %zu", label, spv.size()); return false; }
    unsigned magic = 0; memcpy(&magic, spv.data(), 4);
    if (magic != 0x07230203u) { addf(log, "PRESCREEN[%s]: FAIL bad magic 0x%08X", label, magic); return false; }
    const unsigned* w = (const unsigned*)spv.data();
    const size_t n = spv.size() / 4;
    size_t i = 5; // after header
    unsigned caps = 0;
    while (i < n) {
        const unsigned word0 = w[i];
        const unsigned wc = word0 >> 16, op = word0 & 0xFFFF;
        if (wc == 0 || i + wc > n) { addf(log, "PRESCREEN[%s]: FAIL truncated instruction at word %zu", label, i); return false; }
        if (op == 17 && wc >= 2) { // OpCapability
            caps++;
            const unsigned cap = w[i + 1];
            if (cap == 5284) { addf(log, "PRESCREEN[%s]: REJECT FragmentBarycentricKHR (5284) - Step 2 rule 1", label); return false; }
            if (cap == 40) { addf(log, "PRESCREEN[%s]: REJECT InputAttachment (40) - Step 2 rule 3 (no subpass input for DXIL)", label); return false; }
        }
        i += wc;
    }
    addf(log, "PRESCREEN[%s]: PASS words=%zu capabilities=%u (no barycentric, no input-attachment; uint-filtered-fetch excluded by shader construction - float texture() only)", label, n, caps);
    return true;
}

static bool write_bytes_atomic(const std::wstring& path, const std::vector<unsigned char>& d) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFile2(tmp.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0; BOOL ok = WriteFile(h, d.data(), (DWORD)d.size(), &w, nullptr); CloseHandle(h);
    if (!ok || w != d.size()) { DeleteFileW(tmp.c_str()); return false; }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

#ifndef STEP6_DESKTOP_TRANSLATOR
// UWP/console: no CreateProcess; packaged unsigned DXIL + in-memory hash.
static bool run_translator(const std::wstring&, const char*, const std::wstring&, const std::wstring&, std::string&) { return false; }
#else
static bool run_translator(const std::wstring& exe, const char* stage, const std::wstring& spvPath, const std::wstring& outPath, std::string& log) {
    // Out-of-process per Step 2 rule 4: a translator reject can abort
    // (0xC0000409 measured); it must never take the host process with it.
    std::wstring stageW = (strcmp(stage, "vs") == 0) ? L"vertex" : L"fragment";
    std::wstring cmd = L"\"" + exe + L"\" -s " + stageW + L" -o \"" + outPath + L"\" \"" + spvPath + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr; si.hStdOutput = nullptr; si.hStdError = nullptr;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        addf(log, "TRANSLATE[%s]: CreateProcess failed err=%lu", stage, GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    addf(log, "TRANSLATE[%s]: spirv2dxil exit=0x%08lX (%s)", stage, (unsigned long)code, code == 0 ? "ACCEPT" : "REJECT/ABORT - contained out-of-process");
    return code == 0;
}
#endif

static std::vector<unsigned char> resolve_one(const DrawShaderInputs& in, const char* domain,
    const std::vector<unsigned char>& spv, const std::vector<unsigned char>& packaged,
    const std::wstring& spvStagePath, std::string& keyOut, std::string& log) {
    keyOut = step5_cache_key(spv, domain);
    const std::wstring dxilDir = in.cacheDir + L"\\dxil";
    CreateDirectoryW(in.cacheDir.c_str(), nullptr);
    CreateDirectoryW(dxilDir.c_str(), nullptr);
    std::wstring file = dxilDir + L"\\" + std::wstring(keyOut.begin(), keyOut.end()) + L"_" +
        std::wstring(domain, domain + strlen(domain)) + L".dxil";
    long err = 0;
    auto cached = load_file_bytes(file, &err);
    if (!cached.empty()) {
        auto verify = cached;
        const bool idem = sign_dxil_container(verify.data(), verify.size()) && verify == cached;
        addf(log, "CACHE[%s]: HIT key=%s bytes=%zu hash-idempotent=%d", domain, keyOut.c_str(), cached.size(), idem ? 1 : 0);
        if (idem) return cached;
        addf(log, "CACHE[%s]: entry failed verify - treating as miss", domain);
    } else {
        addf(log, "CACHE[%s]: MISS key=%s (load err=%ld)", domain, keyOut.c_str(), err);
    }
    std::vector<unsigned char> rawDxil;
    const char* source = "PACKAGED-FALLBACK";
    if (!in.translatorExe.empty() && !spv.empty()) {
        // Stage SPIR-V to a temp file for the standalone translator.
        if (write_bytes_atomic(spvStagePath, spv)) {
            const std::wstring tmpDxil = spvStagePath + L".dxil";
            if (run_translator(in.translatorExe, domain, spvStagePath, tmpDxil, log)) {
                long e2 = 0; rawDxil = load_file_bytes(tmpDxil, &e2);
                if (!rawDxil.empty()) source = "TRANSLATED-OUT-OF-PROCESS";
            }
            DeleteFileW(tmpDxil.c_str());
        }
    }
    if (rawDxil.empty()) rawDxil = packaged;
    if (rawDxil.empty()) { addf(log, "CACHE[%s]: FAIL no DXIL source (packaged empty)", domain); return {}; }
    auto signedDxil = rawDxil;
    if (!sign_dxil_container(signedDxil.data(), signedDxil.size())) {
        addf(log, "CACHE[%s]: FAIL sign_dxil_container", domain); return {};
    }
    const bool stored = write_bytes_atomic(file, signedDxil);
    addf(log, "CACHE[%s]: MISS->%s bytes=%zu container=%s stored=%d", domain, source, signedDxil.size(),
        dxil_hash_summary(signedDxil).c_str(), stored ? 1 : 0);
    return signedDxil;
}

DrawShaderOutputs resolve_draw_dxil(const DrawShaderInputs& in) {
    DrawShaderOutputs out;
    if (!prescreen_spirv(in.spvVs, "draw_vs", out.log) || !prescreen_spirv(in.spvFs, "draw_fs", out.log)) {
        addf(out.log, "STEP6-SHADERS: FAIL pre-screen rejected a shader - no PSO attempted");
        return out;
    }
    out.signedVs = resolve_one(in, "vs", in.spvVs, in.packagedVs, in.cacheDir + L"\\draw_vs.spv", out.keyVs, out.log);
    out.signedFs = resolve_one(in, "fs", in.spvFs, in.packagedFs, in.cacheDir + L"\\draw_fs.spv", out.keyFs, out.log);
    out.ok = !out.signedVs.empty() && !out.signedFs.empty();
    addf(out.log, "STEP6-SHADERS: %s (VS=%zu FS=%zu via Step 5 cache + in-memory hash)", out.ok ? "PASS" : "FAIL",
        out.signedVs.size(), out.signedFs.size());
    return out;
}

// ---------------- Step6Loop ----------------
static D3D12_HEAP_PROPERTIES heap_props(D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES h{}; h.Type = t; return h;
}
static D3D12_RESOURCE_DESC buffer_desc(unsigned long long bytes) {
    D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = bytes;
    d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; return d;
}
static void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* res,
    D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res; b.Transition.StateBefore = before; b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &b);
}

HRESULT Step6Loop::wait_fence() {
    const unsigned long long fv = ++m_fv;
    HRESULT hr = m_queue->Signal(m_fence.Get(), fv);
    if (FAILED(hr)) return hr;
    if (m_fence->GetCompletedValue() < fv) {
        m_fence->SetEventOnCompletion(fv, m_fenceEv);
        WaitForSingleObjectEx(m_fenceEv, INFINITE, FALSE);
    }
    return S_OK;
}

std::string Step6Loop::init(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain1* swap1,
    const std::vector<unsigned char>& signedVs, const std::vector<unsigned char>& signedFs,
    long* psoHr, bool* ok) {
    std::string log; *ok = false; if (psoHr) *psoHr = -1;
    m_device = device; m_queue = queue;
    HRESULT hr = swap1->QueryInterface(IID_PPV_ARGS(&m_swap));
    addf(log, "Step6 SwapChain As IDXGISwapChain3: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) return log;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    hr = m_swap->GetDesc1(&desc);
    if (FAILED(hr)) { addf(log, "Step6 GetDesc1 FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    m_width = desc.Width; m_height = desc.Height; m_bufferCount = desc.BufferCount;
    addf(log, "Step6 swapchain: %ux%u buffers=%u format=%d", m_width, m_height, m_bufferCount, (int)desc.Format);

    // --- Root signature: [0] CBV cb0/space30, [1] SRV t4/space1, [2] sampler s4/space1 ---
    D3D12_DESCRIPTOR_RANGE srvRange{}; srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange.NumDescriptors = 1; srvRange.BaseShaderRegister = 4; srvRange.RegisterSpace = 1;
    D3D12_DESCRIPTOR_RANGE sampRange{}; sampRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    sampRange.NumDescriptors = 1; sampRange.BaseShaderRegister = 4; sampRange.RegisterSpace = 1;
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0; params[0].Descriptor.RegisterSpace = 30;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1; params[1].DescriptorTable.pDescriptorRanges = &srvRange;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 1; params[2].DescriptorTable.pDescriptorRanges = &sampRange;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd{}; rsd.NumParameters = 3; rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> sigBlob, errBlob;
    hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &sigBlob, &errBlob);
    addf(log, "SerializeRootSignature(draw: CBV cb0/space30 + SRV t4/space1 + sampler s4/space1, 4 DWORDs): hr=0x%08lX%s",
        (unsigned long)hr, (FAILED(hr) && errBlob) ? (const char*)errBlob->GetBufferPointer() : "");
    if (FAILED(hr)) { if (psoHr) *psoHr = (long)hr; return log; }
    hr = m_device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSig));
    addf(log, "CreateRootSignature(draw): %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) { if (psoHr) *psoHr = (long)hr; return log; }

    // --- PSO from Mesa-translated, in-memory-hashed DXIL (Step 5 route) ---
    addf(log, "Draw VS container: %s", dxil_hash_summary(signedVs).c_str());
    addf(log, "Draw FS container: %s", dxil_hash_summary(signedFs).c_str());
    D3D12_INPUT_ELEMENT_DESC il[2] = {
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = m_rootSig.Get();
    p.VS = {signedVs.data(), signedVs.size()};
    p.PS = {signedFs.data(), signedFs.size()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask = 0xFFFFFFFFu;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = FALSE; p.DepthStencilState.StencilEnable = FALSE;
    p.InputLayout = {il, 2};
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.NumRenderTargets = 1; p.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    p.DSVFormat = DXGI_FORMAT_UNKNOWN; p.SampleDesc.Count = 1;
    hr = m_device->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&m_pso));
    if (psoHr) *psoHr = (long)hr;
    addf(log, "CreateGraphicsPipelineState[STEP6-DRAW Mesa-hashed]: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    addf(log, "GetDeviceRemovedReason after draw PSO: hr=0x%08lX", (unsigned long)m_device->GetDeviceRemovedReason());
    if (FAILED(hr)) return log;

    // --- RTVs / command objects / fence ---
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = m_bufferCount;
    hr = m_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_rtv));
    if (FAILED(hr)) { addf(log, "Step6 RTV heap FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    m_rtvSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_back.resize(m_bufferCount);
    { auto h = m_rtv->GetCPUDescriptorHandleForHeapStart();
      for (unsigned i = 0; i < m_bufferCount; ++i) {
          hr = m_swap->GetBuffer(i, IID_PPV_ARGS(&m_back[i]));
          if (FAILED(hr)) { addf(log, "Step6 GetBuffer[%u] FAIL hr=0x%08lX", i, (unsigned long)hr); return log; }
          m_device->CreateRenderTargetView(m_back[i].Get(), nullptr, h); h.ptr += m_rtvSize;
      } }
    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc));
    if (FAILED(hr)) { addf(log, "Step6 allocator FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc.Get(), nullptr, IID_PPV_ARGS(&m_list));
    if (FAILED(hr)) { addf(log, "Step6 command list FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) { addf(log, "Step6 fence FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    m_fenceEv = CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    if (!m_fenceEv) { addf(log, "Step6 fence event FAIL"); return log; }

    // --- Geometry: indexed quad, 4 verts (pos xyzw + color rgba), 6 uint16 indices ---
    struct Vert { float pos[4]; float col[4]; };
    const Vert verts[4] = {
        {{-0.8f,-0.8f,0.f,1.f},{1.f,1.f,1.f,1.f}},
        {{ 0.8f,-0.8f,0.f,1.f},{1.f,1.f,1.f,1.f}},
        {{ 0.8f, 0.8f,0.f,1.f},{1.f,1.f,1.f,1.f}},
        {{-0.8f, 0.8f,0.f,1.f},{1.f,1.f,1.f,1.f}},
    };
    const unsigned short indices[6] = {0, 1, 2, 0, 2, 3};
    const unsigned long long vbBytes = sizeof(verts), ibBytes = sizeof(indices);
    auto uploadBuf = [&](unsigned long long bytes, const void* data, ComPtr<ID3D12Resource>& res) {
        auto hp = heap_props(D3D12_HEAP_TYPE_UPLOAD); auto bd = buffer_desc(bytes);
        HRESULT c = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res));
        if (FAILED(c)) return c;
        void* m = nullptr; D3D12_RANGE r{0, 0};
        c = res->Map(0, &r, &m); if (FAILED(c)) return c;
        memcpy(m, data, (size_t)bytes); res->Unmap(0, nullptr); return S_OK;
    };
    hr = uploadBuf(vbBytes, verts, m_vb);
    addf(log, "VertexBuffer upload %llu bytes: hr=0x%08lX", vbBytes, (unsigned long)hr); if (FAILED(hr)) return log;
    hr = uploadBuf(ibBytes, indices, m_ib);
    addf(log, "IndexBuffer upload %llu bytes: hr=0x%08lX", ibBytes, (unsigned long)hr); if (FAILED(hr)) return log;
    m_vbView.BufferLocation = m_vb->GetGPUVirtualAddress();
    m_vbView.SizeInBytes = (unsigned)vbBytes; m_vbView.StrideInBytes = sizeof(Vert);
    m_ibView.BufferLocation = m_ib->GetGPUVirtualAddress();
    m_ibView.SizeInBytes = (unsigned)ibBytes; m_ibView.Format = DXGI_FORMAT_R16_UINT;

    // --- Texture: 64x64 RGBA checker (R8G8B8A8 bytes), generated in code ---
    constexpr unsigned kTex = 64;
    std::vector<unsigned char> texels(kTex * kTex * 4);
    for (unsigned y = 0; y < kTex; ++y) for (unsigned x = 0; x < kTex; ++x) {
        const bool a = (((x / 8) + (y / 8)) % 2) == 0;
        unsigned char* px = &texels[(y * kTex + x) * 4];
        px[0] = a ? 200 : 40; px[1] = 40; px[2] = a ? 40 : 200; px[3] = 255;
    }
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = kTex; td.Height = kTex; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    { auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_tex)); }
    addf(log, "Texture 64x64 R8G8B8A8 default-heap: hr=0x%08lX", (unsigned long)hr); if (FAILED(hr)) return log;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; unsigned rows = 0; unsigned long long rowBytes = 0, texUploadBytes = 0;
    m_device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &rowBytes, &texUploadBytes);
    { auto hp = heap_props(D3D12_HEAP_TYPE_UPLOAD); auto bd = buffer_desc(texUploadBytes);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_texUpload)); }
    addf(log, "Texture upload heap %llu bytes (footprint RowPitch=%u rows=%u): hr=0x%08lX",
        texUploadBytes, fp.Footprint.RowPitch, rows, (unsigned long)hr); if (FAILED(hr)) return log;
    { void* m = nullptr; D3D12_RANGE r{0, 0}; m_texUpload->Map(0, &r, &m);
      for (unsigned y = 0; y < kTex; ++y)
          memcpy((unsigned char*)m + fp.Offset + (size_t)y * fp.Footprint.RowPitch, &texels[y * kTex * 4], kTex * 4);
      m_texUpload->Unmap(0, nullptr); }
    // one-time texture copy on the shared queue
    m_list->Close();
    m_alloc->Reset(); m_list->Reset(m_alloc.Get(), m_pso.Get());
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.pResource = m_tex.Get();
    D3D12_TEXTURE_COPY_LOCATION src{}; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.pResource = m_texUpload.Get(); src.PlacedFootprint = fp;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(m_list.Get(), m_tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    hr = m_list->Close();
    if (FAILED(hr)) { addf(log, "Texture upload Close FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    { ID3D12CommandList* lists[] = {m_list.Get()}; m_queue->ExecuteCommandLists(1, lists); }
    hr = wait_fence();
    addf(log, "Texture upload executed+fence: hr=0x%08lX", (unsigned long)hr); if (FAILED(hr)) return log;

    // --- SRV + sampler descriptor heaps (tables t4/s4 space1) ---
    D3D12_DESCRIPTOR_HEAP_DESC sh{}; sh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    sh.NumDescriptors = 1; sh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&m_srvHeap));
    if (FAILED(hr)) { addf(log, "SRV heap FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Texture2D.MipLevels = 1;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    m_device->CreateShaderResourceView(m_tex.Get(), &sv, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
    D3D12_DESCRIPTOR_HEAP_DESC smh{}; smh.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    smh.NumDescriptors = 1; smh.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_device->CreateDescriptorHeap(&smh, IID_PPV_ARGS(&m_samplerHeap));
    if (FAILED(hr)) { addf(log, "Sampler heap FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    D3D12_SAMPLER_DESC smp{}; smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = 3.402823466e+38f;
    m_device->CreateSampler(&smp, m_samplerHeap->GetCPUDescriptorHandleForHeapStart());
    addf(log, "SRV(t4/space1)+sampler(s4/space1) descriptor tables created");

    // --- Push-constant CBV ring: 1024 x 256B slots, windowed per draw ---
    const unsigned long long ringBytes = 256ull * 1024;
    { auto hp = heap_props(D3D12_HEAP_TYPE_UPLOAD); auto bd = buffer_desc(ringBytes);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_pushRing)); }
    if (FAILED(hr)) { addf(log, "Push ring FAIL hr=0x%08lX", (unsigned long)hr); return log; }
    { D3D12_RANGE r{0, 0}; hr = m_pushRing->Map(0, &r, (void**)&m_pushMapped); }
    if (FAILED(hr) || !m_pushMapped) { addf(log, "Push ring Map FAIL hr=0x%08lX", (unsigned long)hr); return log; }

    // --- Readback (1 pixel footprint, RowPitch 256) ---
    { auto hp = heap_props(D3D12_HEAP_TYPE_READBACK); auto bd = buffer_desc(256);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)); }
    if (FAILED(hr)) { addf(log, "Readback FAIL hr=0x%08lX", (unsigned long)hr); return log; }

    // --- Null-flip: synthetic guest buffer + persistent texture + staging ---
    m_guest.assign(kGuestW * kGuestH, 0);
    D3D12_RESOURCE_DESC fd{}; fd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    fd.Width = kGuestW; fd.Height = kGuestH; fd.DepthOrArraySize = 1; fd.MipLevels = 1;
    fd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; fd.SampleDesc.Count = 1;
    { auto hp = heap_props(D3D12_HEAP_TYPE_DEFAULT);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &fd,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_flipTex)); }
    addf(log, "NullFlip persistent texture %ux%u R8G8B8A8: hr=0x%08lX", kGuestW, kGuestH, (unsigned long)hr);
    if (FAILED(hr)) return log;
    unsigned frows = 0; unsigned long long frowBytes = 0;
    m_device->GetCopyableFootprints(&fd, 0, 1, 0, &m_flipFootprint, &frows, &frowBytes, &m_flipStagingBytes);
    { auto hp = heap_props(D3D12_HEAP_TYPE_UPLOAD); auto bd = buffer_desc(m_flipStagingBytes);
      hr = m_device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_flipStaging)); }
    addf(log, "NullFlip staging %llu bytes (guest pitch=%llu footprint RowPitch=%u rows=%u rowBytes=%llu): hr=0x%08lX",
        m_flipStagingBytes, (unsigned long long)kGuestW * 4, m_flipFootprint.Footprint.RowPitch, frows, frowBytes, (unsigned long)hr);
    if (FAILED(hr)) return log;
    { D3D12_RANGE r{0, 0}; hr = m_flipStaging->Map(0, &r, (void**)&m_flipMapped); }
    if (FAILED(hr) || !m_flipMapped) { addf(log, "NullFlip staging Map FAIL hr=0x%08lX", (unsigned long)hr); return log; }

    const D3D12_RESOURCE_ALLOCATION_INFO texInfo = m_device->GetResourceAllocationInfo(0, 1, &td);
    const D3D12_RESOURCE_ALLOCATION_INFO flipInfo = m_device->GetResourceAllocationInfo(0, 1, &fd);
    LARGE_INTEGER f{}, t{}; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    m_qpf = f.QuadPart; m_drawStartQpc = m_drawLastQpc = m_flipStartQpc = m_flipLastQpc = t.QuadPart;
    addf(log, "STEP6-POOLS vertex=%lluB index=%lluB texture-default=%lluB texture-upload=%lluB push-ring=%lluB readback=256B flip-tex-default=%lluB flip-staging=%lluB guest-synthetic=%zuB",
        vbBytes, ibBytes, (unsigned long long)texInfo.SizeInBytes, texUploadBytes, ringBytes,
        (unsigned long long)flipInfo.SizeInBytes, m_flipStagingBytes, m_guest.size() * sizeof(unsigned));
    *ok = true;
    return log;
}

void Step6Loop::note_present(Step6Metrics& m, HRESULT hr, long long qpcBefore) {
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t);
    const double intervalMs = m_qpf ? (double)(t.QuadPart - qpcBefore) * 1000.0 / (double)m_qpf : 0.0;
    m.last_present_hr = (long)hr; m.last_interval_ms = intervalMs;
    if (m.frames_attempted > 1) {
        if (m.min_interval_ms == 0.0 || intervalMs < m.min_interval_ms) m.min_interval_ms = intervalMs;
        if (intervalMs > m.max_interval_ms) m.max_interval_ms = intervalMs;
        if (intervalMs > 25.0) m.intervals_over_25ms++;
        if (intervalMs > 50.0) m.intervals_over_50ms++;
    }
    if (SUCCEEDED(hr)) m.frames_presented++;
    else { m.present_failures++; if (m.present_failures == 1) m.first_present_fail_hr = (long)hr; }
}

HRESULT Step6Loop::render_draw_frame() {
    Step6Metrics& m = m_draw;
    const unsigned long long frame = m.frames_attempted++;
    if (frame == 0) { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); m_drawStartQpc = m_drawLastQpc = t.QuadPart; }
    const unsigned idx = m_swap->GetCurrentBackBufferIndex();
    ID3D12Resource* back = m_back[idx].Get();
    // push block: tint alternates every 60 frames so CBV updates are visible
    // in both motion and readback; scale pulses the quad size.
    const bool phaseB = ((frame / 60) % 2) == 1;
    struct Push { float tint[4]; float scale, p0, p1, p2; } push{};
    push.tint[0] = phaseB ? 0.25f : 1.0f; push.tint[1] = 1.0f; push.tint[2] = phaseB ? 1.0f : 0.25f; push.tint[3] = 1.0f;
    push.scale = 0.85f + 0.15f * (float)((frame % 120) < 60 ? (frame % 60) : (119 - (frame % 120))) / 60.0f;
    const unsigned slot = (unsigned)(frame % 1024);
    memcpy(m_pushMapped + (size_t)slot * 256, &push, sizeof(push)); // 32B block in 256B window
    m_alloc->Reset(); m_list->Reset(m_alloc.Get(), m_pso.Get());
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto rtv = m_rtv->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += (size_t)idx * m_rtvSize;
    const FLOAT bg[4] = {0.05f, 0.05f, 0.12f, 1.0f};
    m_list->ClearRenderTargetView(rtv, bg, 0, nullptr);
    m_list->SetGraphicsRootSignature(m_rootSig.Get());
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get(), m_samplerHeap.Get()};
    m_list->SetDescriptorHeaps(2, heaps);
    m_list->SetGraphicsRootConstantBufferView(0, m_pushRing->GetGPUVirtualAddress() + (size_t)slot * 256);
    m_list->SetGraphicsRootDescriptorTable(1, m_srvHeap->GetGPUDescriptorHandleForHeapStart());
    m_list->SetGraphicsRootDescriptorTable(2, m_samplerHeap->GetGPUDescriptorHandleForHeapStart());
    D3D12_VIEWPORT vp{}; vp.Width = (float)m_width; vp.Height = (float)m_height; vp.MaxDepth = 1.0f;
    D3D12_RECT sc{}; sc.right = (LONG)m_width; sc.bottom = (LONG)m_height;
    m_list->RSSetViewports(1, &vp); m_list->RSSetScissorRects(1, &sc);
    m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    m_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_list->IASetVertexBuffers(0, 1, &m_vbView);
    m_list->IASetIndexBuffer(&m_ibView);
    m_list->DrawIndexedInstanced(6, 1, 0, 0, 0);
    m.draw_calls++;
    // center-pixel readback: honest proof the draw rasterized
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.pResource = m_readback.Get();
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 1; dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = back;
    D3D12_BOX box{}; box.left = m_width / 2; box.top = m_height / 2;
    box.right = box.left + 1; box.bottom = box.top + 1; box.back = 1;
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    HRESULT hr = m_list->Close();
    if (FAILED(hr)) { m.close_failures++; m.last_close_hr = (long)hr; return hr; }
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    const long long before = m_drawLastQpc;
    hr = m_swap->Present(1, 0);
    note_present(m, hr, before);
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t); m_drawLastQpc = t.QuadPart;
    m.elapsed_s = m_qpf ? (double)(t.QuadPart - m_drawStartQpc) / (double)m_qpf : 0.0;
    HRESULT w = wait_fence();
    if (FAILED(w)) return w;
    void* rb = nullptr; D3D12_RANGE rr{0, 4};
    HRESULT hr2 = m_readback->Map(0, &rr, &rb);
    m.last_readback_hr = (long)hr2;
    if (SUCCEEDED(hr2) && rb) {
        const unsigned char* px = (const unsigned char*)rb;
        m.last_pixel_rgba[0] = px[0]; m.last_pixel_rgba[1] = px[1]; m.last_pixel_rgba[2] = px[2]; m.last_pixel_rgba[3] = px[3];
        D3D12_RANGE nw{0, 0}; m_readback->Unmap(0, &nw);
    }
    return hr;
}

HRESULT Step6Loop::render_nullflip_frame() {
    Step6Metrics& m = m_flip;
    const unsigned long long frame = m.frames_attempted++;
    if (frame == 0) { LARGE_INTEGER t{}; QueryPerformanceCounter(&t); m_flipStartQpc = m_flipLastQpc = t.QuadPart; }
    // Synthetic guest display buffer (A8R8G8B8 u32s), generated in code:
    // horizontal bands + moving white square; test pixels row 0: R,G,B.
    LARGE_INTEGER c0{}; QueryPerformanceCounter(&c0);
    const unsigned sq = (unsigned)((frame * 4) % (kGuestW - 128));
    for (unsigned y = 0; y < kGuestH; ++y) {
        const unsigned band = (y * 8) / kGuestH;
        const unsigned r = (band * 32) & 0xFF, g = (y * 255 / kGuestH) & 0xFF, b = 255 - g;
        unsigned* row = &m_guest[y * kGuestW];
        for (unsigned x = 0; x < kGuestW; ++x) row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        if (y >= 300 && y < 428) for (unsigned x = sq; x < sq + 128; ++x) row[x] = 0xFFFFFFFFu;
    }
    m_guest[0] = 0xFFFF0000u; m_guest[1] = 0xFF00FF00u; m_guest[2] = 0xFF0000FFu;
    // Stage with CPU R<->B swizzle: guest A8R8G8B8 memory order is B,G,R,A;
    // the proven Step 4 swapchain is R8G8B8A8_UNORM. Verbatim copy would
    // swap red/blue (readback pixel (0,0) proves the choice below).
    const unsigned rowPitch = m_flipFootprint.Footprint.RowPitch;
    for (unsigned y = 0; y < kGuestH; ++y) {
        const unsigned* srow = &m_guest[y * kGuestW];
        unsigned char* drow = m_flipMapped + m_flipFootprint.Offset + (size_t)y * rowPitch;
        for (unsigned x = 0; x < kGuestW; ++x) {
            const unsigned px = srow[x]; // 0xAARRGGBB
            drow[x*4+0] = (unsigned char)(px >> 16); // R
            drow[x*4+1] = (unsigned char)(px >> 8);  // G
            drow[x*4+2] = (unsigned char)px;         // B
            drow[x*4+3] = (unsigned char)(px >> 24); // A
        }
    }
    LARGE_INTEGER c1{}; QueryPerformanceCounter(&c1);
    if (m_qpf) m.cpu_stage_us_total += (unsigned long long)((c1.QuadPart - c0.QuadPart) * 1000000ll / m_qpf);
    const unsigned idx = m_swap->GetCurrentBackBufferIndex();
    ID3D12Resource* back = m_back[idx].Get();
    m_alloc->Reset(); m_list->Reset(m_alloc.Get(), nullptr);
    auto rtv = m_rtv->GetCPUDescriptorHandleForHeapStart(); rtv.ptr += (size_t)idx * m_rtvSize;
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const FLOAT black[4] = {0.f, 0.f, 0.f, 1.f};
    m_list->ClearRenderTargetView(rtv, black, 0, nullptr); // area outside 720p rect
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION tdst{}; tdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; tdst.pResource = m_flipTex.Get();
    D3D12_TEXTURE_COPY_LOCATION tsrc{}; tsrc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    tsrc.pResource = m_flipStaging.Get(); tsrc.PlacedFootprint = m_flipFootprint;
    m_list->CopyTextureRegion(&tdst, 0, 0, 0, &tsrc, nullptr);
    barrier(m_list.Get(), m_flipTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION bdst{}; bdst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; bdst.pResource = back;
    D3D12_TEXTURE_COPY_LOCATION bsrc{}; bsrc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; bsrc.pResource = m_flipTex.Get();
    m_list->CopyTextureRegion(&bdst, 0, 0, 0, &bsrc, nullptr);
    barrier(m_list.Get(), m_flipTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{}; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.pResource = m_readback.Get();
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 1; dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
    D3D12_TEXTURE_COPY_LOCATION src{}; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.pResource = back;
    D3D12_BOX box{}; box.right = 1; box.bottom = 1; box.back = 1; // guest pixel (0,0) = red
    m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
    barrier(m_list.Get(), back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    HRESULT hr = m_list->Close();
    if (FAILED(hr)) { m.close_failures++; m.last_close_hr = (long)hr; return hr; }
    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    const long long before = m_flipLastQpc;
    hr = m_swap->Present(1, 0);
    note_present(m, hr, before);
    LARGE_INTEGER t{}; QueryPerformanceCounter(&t); m_flipLastQpc = t.QuadPart;
    m.elapsed_s = m_qpf ? (double)(t.QuadPart - m_flipStartQpc) / (double)m_qpf : 0.0;
    HRESULT w = wait_fence();
    if (FAILED(w)) return w;
    void* rb = nullptr; D3D12_RANGE rr{0, 4};
    HRESULT hr2 = m_readback->Map(0, &rr, &rb);
    m.last_readback_hr = (long)hr2;
    if (SUCCEEDED(hr2) && rb) {
        const unsigned char* px = (const unsigned char*)rb;
        m.last_pixel_rgba[0] = px[0]; m.last_pixel_rgba[1] = px[1]; m.last_pixel_rgba[2] = px[2]; m.last_pixel_rgba[3] = px[3];
        D3D12_RANGE nw{0, 0}; m_readback->Unmap(0, &nw);
    }
    return hr;
}

std::string Step6Loop::summary_of(const Step6Metrics& m, const char* tag) const {
    char buf[1024];
    const double fps = m.elapsed_s > 0.0 ? (double)m.frames_presented / m.elapsed_s : 0.0;
    const double avgStageMs = m.frames_attempted ? (double)m.cpu_stage_us_total / 1000.0 / (double)m.frames_attempted : 0.0;
    long removed = -1; if (m_device) removed = (long)m_device->GetDeviceRemovedReason();
    unsigned presentCount = 0; long statsHr = -1;
    if (m_swap) { DXGI_FRAME_STATISTICS st{}; HRESULT h = m_swap->GetFrameStatistics(&st); statsHr = (long)h; if (SUCCEEDED(h)) presentCount = st.PresentCount; }
    snprintf(buf, sizeof(buf),
        "%s attempted=%llu presented=%llu present-failures=%llu first-fail-hr=0x%08lX last-present-hr=0x%08lX close-failures=%llu draw-calls=%llu elapsed=%.2fs avg-fps=%.2f interval-ms min=%.2f max=%.2f last=%.2f over25ms=%llu over50ms=%llu cpu-stage-avg-ms=%.2f readback-hr=0x%08lX pixelRGBA=(%u,%u,%u,%u) frameStats-hr=0x%08lX dxgiPresentCount=%u deviceRemovedReason=0x%08lX",
        tag, m.frames_attempted, m.frames_presented, m.present_failures,
        (unsigned long)m.first_present_fail_hr, (unsigned long)m.last_present_hr, m.close_failures,
        m.draw_calls, m.elapsed_s, fps, m.min_interval_ms, m.max_interval_ms, m.last_interval_ms,
        m.intervals_over_25ms, m.intervals_over_50ms, avgStageMs, (unsigned long)m.last_readback_hr,
        m.last_pixel_rgba[0], m.last_pixel_rgba[1], m.last_pixel_rgba[2], m.last_pixel_rgba[3],
        (unsigned long)statsHr, presentCount, (unsigned long)removed);
    return buf;
}
std::string Step6Loop::draw_summary(const char* tag) const { return summary_of(m_draw, tag); }
std::string Step6Loop::nullflip_summary(const char* tag) const { return summary_of(m_flip, tag); }
