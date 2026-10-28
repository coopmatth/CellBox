#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>
#include "pso_probe.h"
#include "present_probe.h"
#include "draw_probe.h"
#include "audio_pad_probe.h"
#include "../xbox-memory/memory_governor.h"
#include "../xbox-memory/desktop_sampler.h"
#include "../xbox-d3d12-backend/d3d12_gsrender.h"
#include "../xbox-emu-link/emu_link.h"
#include "../xbox-emu-link/reassemble.h"
#include "../xbox-dxil/dxil_sign_candidate.h"
using Microsoft::WRL::ComPtr;
// Step 4 desktop pre-test. args: <dir with DXIL/control files> [frames]
// Exit 0 iff ALL of: device+queue created, CONTROL (signed) PSO succeeds
// (Step 3 regression gate), HWND swapchain (FLIP_DISCARD) created, and the
// bounded present loop presents every frame with no Present failure and no
// device removal. Mesa unsigned-DXIL hr is reported; on PC it is expected
// to remain 0x80070057 (same signing signal as the console Step 3 result).
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); setvbuf(stderr, nullptr, _IONBF, 0);
    std::wstring dir = L".";
    if (argc > 1) { std::string a = argv[1]; dir = std::wstring(a.begin(), a.end()); }
    unsigned long long targetFrames = 240;
    if (argc > 2) targetFrames = strtoull(argv[2], nullptr, 10);
    HRESULT ehr = D3D12EnableExperimentalFeatures(1, &D3D12ExperimentalShaderModels, nullptr, nullptr); (void)ehr;
    printf("D3D12EnableExperimentalFeatures(ShaderModels) hr=0x%08lX\n", (unsigned long)ehr);
    ComPtr<ID3D12Device> dev;
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
    printf("D3D12CreateDevice(hw,FL11_0) hr=0x%08lX\n", (unsigned long)hr);
    bool warp = false;
    if (FAILED(hr)) {
        ComPtr<IDXGIFactory4> f; CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
        ComPtr<IDXGIAdapter> wa; if (f) f->EnumWarpAdapter(IID_PPV_ARGS(&wa));
        hr = D3D12CreateDevice(wa.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        printf("D3D12CreateDevice(WARP,FL11_0) hr=0x%08lX\n", (unsigned long)hr);
        warp = true;
        if (FAILED(hr)) return 2;
    }
    printf("adapter=%s\n", warp ? "WARP" : "hardware-default");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    hr = dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    printf("CreateCommandQueue hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) return 2;
    D3D12_FEATURE_DATA_SHADER_MODEL sm{}; sm.HighestShaderModel = D3D_SHADER_MODEL_6_2;
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
        printf("HighestShaderModel=0x%04X\n", (unsigned)sm.HighestShaderModel);
    // --- Step 3 PSO regression (must stay: control PASS, Mesa hr reported) ---
    long e1 = 0, e2 = 0, e3 = 0, e4 = 0;
    auto vs = load_file_bytes(dir + L"\\16_vs_simple.dxil", &e1);
    auto ps = load_file_bytes(dir + L"\\01_fs_simple_color.dxil", &e2);
    auto cvs = load_file_bytes(dir + L"\\control_vs.cso", &e3);
    auto cps = load_file_bytes(dir + L"\\control_ps.cso", &e4);
    printf("load unsigned VS err=%ld PS err=%ld | control VS err=%ld PS err=%ld\n", e1, e2, e3, e4);
    long ctrlHr = -1, mesaHr = -1, hashedHr = -1;
    printf("%s", run_pso_probe(dev.Get(), cvs, cps, "CONTROL-signed", &ctrlHr).c_str());
    printf("%s", run_pso_probe(dev.Get(), vs, ps, "MESA-unsigned-DXIL", &mesaHr).c_str());
    // Step 5: hash Mesa pair in memory (gate-1-verified candidate) + retry.
    auto hvs = vs, hps = ps;
    bool s1 = sign_dxil_container(hvs.data(), hvs.size());
    bool s2 = sign_dxil_container(hps.data(), hps.size());
    printf("sign_dxil_container(Mesa VS)=%d (Mesa PS)=%d\n", s1 ? 1 : 0, s2 ? 1 : 0);
    if (s1 && s2) printf("%s", run_pso_probe(dev.Get(), hvs, hps, "MESA-hashed-DXIL", &hashedHr).c_str());
    printf("PSO-REGRESSION control-hr=0x%08lX mesa-unsigned-hr=0x%08lX mesa-hashed-hr=0x%08lX\n",
        (unsigned long)ctrlHr, (unsigned long)mesaHr, (unsigned long)hashedHr);
    printf("STEP5-HASHED-PSO-PC: %s hr=0x%08lX\n", hashedHr == 0 ? "PASS" : "FAIL", (unsigned long)hashedHr);
    if (ctrlHr != 0) { printf("DESKTOP-PRETEST: FAIL (control PSO failed - Step 3 regression, do not package)\n"); return 1; }
    if (hashedHr != 0) { printf("DESKTOP-PRETEST: FAIL (Step 5 hashed Mesa PSO failed on PC - do not package for console probe)\n"); return 1; }
    // --- Step 7: audio engine/voice bring-up + pad enumeration (pre-test gate) ---
    AudioProbe audio;
    printf("%s", audio.init_and_start().c_str());
    PadProbe pad;
    printf("%s", pad.init_and_enumerate().c_str());
    auto pollPad = [&](bool reenum) {
        std::vector<PadTransition> tr; pad.poll_once(reenum, tr);
        for (auto& x : tr) printf("%s\n", x.text.c_str());
    };
    if (!audio.running()) { printf("DESKTOP-PRETEST: FAIL (Step 7 audio: XAudio2 failed and WASAPI fallback failed - see hrs above)\n"); return 1; }
    if (!pad.metrics().statics_ok) { printf("DESKTOP-PRETEST: FAIL (Step 7 pad: IGamepadStatics unavailable hr=0x%08lX)\n", (unsigned long)pad.metrics().statics_hr); return 1; }
    printf("STEP7-BRINGUP-PC: PASS (audio-running=1 pad-enum count=%u)\n", pad.metrics().count);
    // --- Step 4: HWND swapchain + bounded present loop (same PresentLoop) ---
    WNDCLASSW wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"XboxUwpHostStep4PreTest";
    RegisterClassW(&wc);
    RECT wr = {0, 0, 1280, 720}; AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RPCS3 Xbox Step 4 desktop pre-test", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    printf("CreateWindowExW hwnd=%p err=%lu\n", (void*)hwnd, GetLastError());
    if (!hwnd) { printf("DESKTOP-PRETEST: FAIL (no window)\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);
    RECT cr{}; GetClientRect(hwnd, &cr);
    printf("window client rect=%ldx%ld\n", cr.right - cr.left, cr.bottom - cr.top);
    ComPtr<IDXGIFactory4> factory;
    hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    printf("CreateDXGIFactory2 hr=0x%08lX\n", (unsigned long)hr);
    if (FAILED(hr)) { printf("DESKTOP-PRETEST: FAIL (factory)\n"); return 1; }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)(cr.right - cr.left); sd.Height = (UINT)(cr.bottom - cr.top);
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.SampleDesc.Count = 1;
    printf("SwapChain requested: width=%u height=%u bufferCount=%u format=R8G8B8A8_UNORM swapEffect=FLIP_DISCARD\n", sd.Width, sd.Height, sd.BufferCount);
    ComPtr<IDXGISwapChain1> swap1;
    hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &swap1);
    printf("CreateSwapChainForHwnd: %s hr=0x%08lX\n", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) { printf("DESKTOP-PRETEST: FAIL (swapchain creation)\n"); return 1; }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    PresentLoop loop; bool ok = false;
    printf("%s", loop.init(dev.Get(), queue.Get(), swap1.Get(), &ok).c_str());
    if (!ok) { printf("DESKTOP-PRETEST: FAIL (present loop init)\n"); return 1; }
    printf("present loop: target=%llu frames, vsync Present(1,0), pattern=8 colour bars advancing every 15 frames\n", targetFrames);
    bool quit = false;
    while (!quit && loop.metrics().frames_attempted < targetFrames) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit) break;
        HRESULT phr = loop.render_frame();
        pollPad(loop.metrics().frames_attempted % 60 == 0);
        if (FAILED(phr)) printf("frame %llu FAILED hr=0x%08lX\n", loop.metrics().frames_attempted, (unsigned long)phr);
        if (loop.metrics().frames_attempted % 60 == 0)
            printf("%s\n", loop.summary("PRESENT-PROGRESS").c_str());
    }
    printf("%s\n", loop.summary("PRESENT-FINAL").c_str());
    const auto& m = loop.metrics();
    HRESULT removed = dev->GetDeviceRemovedReason();
    printf("GetDeviceRemovedReason after present loop: hr=0x%08lX\n", (unsigned long)removed);
    if (quit || m.frames_presented < targetFrames || m.present_failures != 0 || m.close_failures != 0 || FAILED(removed)) {
        DestroyWindow(hwnd);
        printf("DESKTOP-PRETEST: FAIL (present loop: presented=%llu/%llu failures=%llu removed=0x%08lX quit=%d)\n",
            m.frames_presented, targetFrames, m.present_failures, (unsigned long)removed, (int)quit);
        return 1;
    }
    // --- Step 6: draw + null-flip on the same swapchain (must pass to package) ---
    printf("--- Step 6 draw + null-flip (desktop pre-test gate) ---\n");
    long da = 0, db = 0, dc = 0, dd = 0;
    DrawShaderInputs din;
    din.cacheDir = dir + L"\\cache";
    din.translatorExe = L"C:\\rpcs3\\xbox-shader-corpus\\mesa-build\\src\\microsoft\\spirv_to_dxil\\spirv2dxil.exe";
    din.spvVs = load_file_bytes(dir + L"\\draw_vs.spv", &da);
    din.spvFs = load_file_bytes(dir + L"\\draw_fs.spv", &db);
    din.packagedVs = load_file_bytes(dir + L"\\draw_vs.dxil", &dc);
    din.packagedFs = load_file_bytes(dir + L"\\draw_fs.dxil", &dd);
    printf("step6 inputs spvVS=%zu(e%ld) spvFS=%zu(e%ld) dxilVS=%zu(e%ld) dxilFS=%zu(e%ld)\n",
        din.spvVs.size(), da, din.spvFs.size(), db, din.packagedVs.size(), dc, din.packagedFs.size(), dd);
    auto shaders = resolve_draw_dxil(din);
    printf("%s", shaders.log.c_str());
    if (!shaders.ok) { printf("DESKTOP-PRETEST: FAIL (Step 6 shader resolve)\n"); DestroyWindow(hwnd); return 1; }
    Step6Loop s6; bool s6ok = false; long s6psoHr = -1;
    printf("%s", s6.init(dev.Get(), queue.Get(), swap1.Get(), shaders.signedVs, shaders.signedFs, &s6psoHr, &s6ok).c_str());
    printf("STEP6-DRAW-PSO-PC: %s hr=0x%08lX init-ok=%d\n", s6psoHr == 0 ? "PASS" : "FAIL", (unsigned long)s6psoHr, (int)s6ok);
    if (!s6ok || s6psoHr != 0) { printf("DESKTOP-PRETEST: FAIL (Step 6 draw PSO/init)\n"); DestroyWindow(hwnd); return 1; }
    const unsigned long long kS6 = 240;
    unsigned pixA[4] = {}, pixB[4] = {};
    while (!quit && s6.draw_metrics().frames_attempted < kS6) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit) break;
        s6.render_draw_frame();
        const auto& dm = s6.draw_metrics();
        if (dm.frames_attempted == 30) memcpy(pixA, dm.last_pixel_rgba, sizeof(pixA));
        if (dm.frames_attempted == 90) memcpy(pixB, dm.last_pixel_rgba, sizeof(pixB));
        if (dm.frames_attempted % 60 == 0) printf("%s\n", s6.draw_summary("DRAW-PROGRESS").c_str());
    }
    printf("%s\n", s6.draw_summary("DRAW-FINAL").c_str());
    printf("DRAW-PIXEL frame30 RGBA=(%u,%u,%u,%u) frame90 RGBA=(%u,%u,%u,%u)\n", pixA[0], pixA[1], pixA[2], pixA[3], pixB[0], pixB[1], pixB[2], pixB[3]);
    auto nearv = [](unsigned v, unsigned e) { return v + 6 >= e && e + 6 >= v; };
    const bool pixOk = nearv(pixA[0],40) && nearv(pixA[1],40) && nearv(pixA[2],50) && nearv(pixB[0],10) && nearv(pixB[1],40) && nearv(pixB[2],200);
    const auto& dm = s6.draw_metrics();
    const bool drawOk = !quit && dm.frames_presented == kS6 && dm.present_failures == 0 && dm.close_failures == 0 && dm.draw_calls == kS6 && pixOk;
    printf("STEP6-DRAW-PC: %s presented=%llu/%llu draw-calls=%llu pixel=%s\n", drawOk ? "PASS" : "FAIL", dm.frames_presented, kS6, dm.draw_calls, pixOk ? "PASS" : "FAIL");
    while (!quit && s6.nullflip_metrics().frames_attempted < kS6) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit) break;
        s6.render_nullflip_frame();
        if (s6.nullflip_metrics().frames_attempted % 60 == 0) printf("%s\n", s6.nullflip_summary("NULLFLIP-PROGRESS").c_str());
    }
    printf("%s\n", s6.nullflip_summary("NULLFLIP-FINAL").c_str());
    const auto& fm = s6.nullflip_metrics();
    const bool flipPixOk = fm.last_pixel_rgba[0] == 255 && fm.last_pixel_rgba[1] == 0 && fm.last_pixel_rgba[2] == 0 && fm.last_pixel_rgba[3] == 255;
    const bool flipOk = !quit && fm.frames_presented == kS6 && fm.present_failures == 0 && fm.close_failures == 0 && flipPixOk;
    printf("STEP6-NULLFLIP-PC: %s presented=%llu/%llu pixel=%s\n", flipOk ? "PASS" : "FAIL", fm.frames_presented, kS6, flipPixOk ? "PASS" : "FAIL");
    removed = dev->GetDeviceRemovedReason();
    printf("GetDeviceRemovedReason final hr=0x%08lX\n", (unsigned long)removed);
    if (!drawOk || !flipOk || FAILED(removed)) { printf("DESKTOP-PRETEST: FAIL (Step 6 draw/null-flip)\n"); DestroyWindow(hwnd); return 1; }
    // --- Step 7 bounded poll (desktop-short 5s; full 60s probe is xbox-audio-pad pc_probe) ---
    printf("--- Step 7 bounded pad/audio poll: 5s desktop-short (console phase is 60s; 60s PC probe separate) ---\n");
    {
        LARGE_INTEGER fq, a, bq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
        double lastRe = -1;
        for (;;) {
            QueryPerformanceCounter(&bq);
            double t = (double)(bq.QuadPart - a.QuadPart) / (double)fq.QuadPart;
            if (t >= 5.0) break;
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            pollPad(t - lastRe >= 1.0);
            if (t - lastRe >= 1.0) lastRe = t;
            Sleep(16);
        }
    }
    printf("%s\n", audio.state_line("AUDIO-FINAL-PC").c_str());
    printf("%s\n", pad.summary("PAD-FINAL-PC").c_str());
    const auto& pm = pad.metrics();
    printf("STEP7-PC: %s audio-running=%d pad-count=%u transitions=%llu (transitions NOT required for pre-test; 0 with no pad pressed is honest)\n",
        (audio.running() && pm.statics_ok) ? "PASS" : "FAIL", audio.running() ? 1 : 0, pm.count, pm.transitions);
    // --- Step 8: memory governor (pre-test gate: verdict must PASS) ---
    printf("--- Step 8 memory governor (desktop pre-test; draw frames + pad poll during stress) ---\n");
    {
        ComPtr<IDXGIAdapter3> adapter3;
        ComPtr<IDXGIAdapter1> a1;
        if (factory && SUCCEEDED(factory->EnumAdapters1(0, &a1)) && a1) a1.As(&adapter3);
        const unsigned long long drawBefore = s6.draw_metrics().frames_presented;
        const unsigned long long drawFailsBefore = s6.draw_metrics().present_failures;
        GovernorConfig gcfg;
        auto gres = run_step8_governor(gcfg,
            [](const std::string& l) { printf("%s\n", l.c_str()); },
            [&]() { return desktop_sample(adapter3.Get()); },
            [&](std::string& why) -> bool {
                MSG msg{};
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
                HRESULT fhr = s6.render_draw_frame();
                pollPad(s6.draw_metrics().frames_attempted % 60 == 0);
                HRESULT rem = dev->GetDeviceRemovedReason();
                if (FAILED(fhr) || FAILED(rem) || fhr == DXGI_ERROR_DEVICE_REMOVED || fhr == DXGI_ERROR_DEVICE_RESET || fhr == DXGI_ERROR_DEVICE_HUNG) {
                    char b[192]; snprintf(b, sizeof(b), "draw frame hr=0x%08lX deviceRemovedReason=0x%08lX", (unsigned long)fhr, (unsigned long)rem);
                    why = b; return false;
                }
                return true;
            });
        const auto& sdm = s6.draw_metrics();
        printf("STEP8-DURING-STRESS-PC draw-presented-delta=%llu present-failures-delta=%llu audio=%s pad-polls=%llu deviceRemovedReason=0x%08lX\n",
            sdm.frames_presented - drawBefore, sdm.present_failures - drawFailsBefore,
            audio.state_line("AUDIO-DURING-STEP8-PC").c_str(), pad.metrics().polls, (unsigned long)dev->GetDeviceRemovedReason());
        printf("STEP8-PC: %s (reserve-commit=%d denial=%d eviction=%d stress=%d abort=%d peak-private-commit=%llu)\n",
            gres.pass() ? "PASS" : "FAIL", gres.reserve_commit_ok ? 1 : 0, gres.denial_ok ? 1 : 0,
            gres.eviction_ok ? 1 : 0, gres.stress_completed ? 1 : 0, gres.abort_fired ? 1 : 0, gres.peak_governed_figure);
        if (!gres.pass()) { printf("DESKTOP-PRETEST: FAIL (Step 8 governor: %s)\n", gres.abort_reason.c_str()); DestroyWindow(hwnd); audio.stop(); return 1; }
    }
    // --- Integration: D3D12GSRender prototype (desktop pre-test gate) ---
    printf("--- INT D3D12 backend prototype (synthetic commands only) ---\n");
    bool intOk = false;
    {
        d3d12_backend::D3D12GSRender backend(
            [](const std::string& l) { printf("%s\n", l.c_str()); },
            [&]() { return s6.render_draw_frame(); },
            [&]() { return s6.render_nullflip_frame(); });
        backend.set_render_size(1280, 720);
        backend.set_y_flip(true);
        d3d12_backend::CpuModel cpu;
        cpu.start_ppu([](const std::string& l) { printf("%s\n", l.c_str()); });
        cpu.start_spu([](const std::string& l) { printf("%s\n", l.c_str()); });
        printf("INT-CPU-PC: llvm_threads=%d (pinned at 1)\n", cpu.llvm_threads);
        d3d12_backend::BackButtonPolicy backPol; backPol.game_running = false;
        printf("INT-BACK-PC: policy action=%d (0=ignore,1=Circle,2=overlay-combo; BackRequested handler is UWP-only, console proof pending)\n", backPol.on_back_requested(false, false));
        bool guestOk = backend.guest_reserve() && backend.guest_commit(0, 0xA5) &&
                       backend.guest_translate(0, 0) != nullptr && backend.guest_translate(0, 1) != nullptr;
        backend.bind_texture(d3d12_backend::CacheKey{0x10000000ULL, 0, 64, 64}, 65536);
        backend.bind_surface(d3d12_backend::CacheKey{0x20000000ULL, 0, 1280, 720}, 3932160);
        backend.insert_shader("draw-vs", 2276);
        backend.insert_shader("draw-fs", 2032);
        for (int i = 0; i < 10; ++i) backend.insert_shader("shader-fill-" + std::to_string(i), 16ull << 20);
        std::vector<d3d12_backend::SyntheticCommand> cmds;
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kBegin, 0, 0, 0, 0, 0});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kBindTexture, 0x10000000ULL, 0, 64, 64, 65536});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kBindSurface, 0x20000000ULL, 0, 1280, 720, 3932160});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kSetRop, 0, 0, 0, 0, 3});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kSetMsaa, 0, 0, 0, 0, 4});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kDrawIndexed, 0, 0, 0, 0, 0});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kDrawIndexed, 0, 0, 0, 0, 0});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kDrawIndexed, 0, 0, 0, 0, 0});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kFlip, 0, 0, 0, 0, 0});
        cmds.push_back(d3d12_backend::SyntheticCommand{d3d12_backend::SyntheticCommand::kEnd, 0, 0, 0, 0, 0});
        std::string fifoFail;
        bool fifoOk = backend.run_fifo(cmds, fifoFail);
        printf("INT-FIFO-PC: %s fail=%s\n", fifoOk ? "PASS" : "FAIL", fifoFail.c_str());
        printf("%s\n", backend.summary().c_str());
        intOk = guestOk && fifoOk && backend.verdict_ok();
        printf("INT-VERDICT-PC: %s (guest=%s fifo=%s backend=%s; synthetic only)\n",
            intOk ? "PASS" : "FAIL", guestOk ? "PASS" : "FAIL", fifoOk ? "PASS" : "FAIL", backend.verdict_ok() ? "PASS" : "FAIL");
        backend.guest_release();
    }
    // EMU-LINK-PC phase: same wiring shape as the UWP host, with the
    // desktop dispatcher replaced by direct invocation (desktop probe).
    bool emuLinkOk = false;
    {
        printf("EMU-LINK-PHASE-PC start: emu_callbacks surface, PSN Simulated, llvm_threads=1\n");
        emu_link::EmuLink link([](const std::string& l) { printf("%s\n", l.c_str()); });
        link.set_psn(emu_link::PsnStatus::kSimulated);
        emu_link::HostCallbacks cb;
        cb.call_from_main_thread = [](std::function<void()> f) { f(); };
        cb.init_gs_render = []() {};
        cb.get_gs_frame = []() { return true; };
        cb.init_pad_handler = [](const std::string& t) { printf("EMU-LINK-PAD-PC: init_pad_handler title_id=%s\n", t.c_str()); };
        cb.get_audio = [&]() { return audio.running(); };
        cb.on_run = []() { printf("EMU-LINK-LIFECYCLE-PC: on_run\n"); };
        cb.on_pause = []() {};
        cb.on_resume = []() {};
        cb.on_stop = []() {};
        bool bound = link.bind(cb);
        bool probed = link.probe();
        printf("%s\n", link.summary().c_str());
        std::string gateReason; bool gate = link.boot_gate(gateReason);
        printf("EMU-LINK-BOOT-GATE-PC: %s reason=%s\n", gate ? "OPEN" : "BLOCKED-real-Emu-not-linked", gateReason.c_str());
        printf("EMU-LINK-PSN-PC: status=Simulated (fallback RPCN, never real PSN)\n");
        emuLinkOk = link.verdict_ok();
        printf("EMU-LINK-VERDICT-PC: %s (bound=%d probed=%d; boot gate BLOCKED is next work, not a verdict failure)\n", emuLinkOk ? "PASS" : "FAIL", (int)bound, (int)probed);
    }
    if (!intOk) { printf("DESKTOP-PRETEST: FAIL (INT backend)\n"); DestroyWindow(hwnd); audio.stop(); return 1; }
    if (!emuLinkOk) { printf("DESKTOP-PRETEST: FAIL (EMU-LINK)\n"); DestroyWindow(hwnd); audio.stop(); return 1; }
    // REASSEMBLE-TEST-PC: split a known string into 3 parts, reassemble,
    // compare. Proves the Portal ISO fallback logic on desktop.
    {
        const char* parts[3] = { "ABCD", "EFGH", "IJKL" };
        for (int i = 0; i < 3; ++i) {
            std::string p = std::string("re-test.part") + std::to_string(i);
            FILE* f = fopen(p.c_str(), "wb"); if (f) { fwrite(parts[i], 1, 4, f); fclose(f); }
        }
        uint64_t outBytes = 0; std::string fail;
        bool rok = reassemble::reassemble(".", "re-test", 3, "re-test.out", 12, outBytes, fail);
        char back[13] = { 0 };
        FILE* f = fopen("re-test.out", "rb"); if (f) { fread(back, 1, 12, f); fclose(f); }
        bool testOk = rok && outBytes == 12 && std::string(back) == "ABCDEFGHIJKL";
        printf("REASSEMBLE-TEST-PC: %s bytes=%llu back=%s fail=%s\n", testOk ? "PASS" : "FAIL", outBytes, back, fail.c_str());
        if (!testOk) { printf("DESKTOP-PRETEST: FAIL (REASSEMBLE)\n"); DestroyWindow(hwnd); audio.stop(); return 1; }
    }
    removed = dev->GetDeviceRemovedReason();
    DestroyWindow(hwnd);
    audio.stop();
    if (FAILED(removed)) { printf("DESKTOP-PRETEST: FAIL (device removed final)\n"); return 1; }
    printf("DESKTOP-PRETEST: PASS (device+queue+control PSO+hashed PSO+present+Step6 draw %llu+nullflip %llu+Step7 audio/pad-enum+Step8 governor reserve/commit+denial+eviction+stress+INT D3D12 backend, 0 failures outside governor denial)\n", dm.frames_presented, fm.frames_presented);
    return 0;
}
