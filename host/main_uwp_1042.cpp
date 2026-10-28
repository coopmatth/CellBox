#include "pso_probe.h"
#include "present_probe.h"
#include "draw_probe.h"
#include "audio_pad_probe.h"
#include "../xbox-memory/memory_governor.h"
#include "../xbox-dxil/dxil_sign_candidate.h"
#include "../xbox-d3d12-backend/d3d12_gsrender.h"
#include "../xbox-emu-link/emu_link.h"
#include "../xbox-emu-link/reassemble.h"
#include "emu_host.h"
#include "boot_probe.h"
#include "rsx_counters.h"
#include "watchdog.h"
#include "callback_queue.h"
#include "iat_hooks.h"
#include "..\xbox-core-lib\linktest\fatal_log.h"
#include "pad_wgi.h"
#include "stage_diag.h"
// Step 4 UWP host (extends the Step 3 headless host; Step 3 probes kept as a
// regression). C++/CX activation copied from xbox-spikes pattern. D3D12
// FL11_0 device + DIRECT queue, no RPCS3 core. Runs: storage probe,
// signer-loadability probe, signed-control PSO, Mesa unsigned-DXIL PSO
// (Step 3 regression), then Step 4: CoreWindow FLIP_DISCARD swapchain +
// sustained vsync present loop of an advancing 8-bar test pattern.
// Writes LocalState\host-results.txt after every line. There is no solid
// verdict colour any more: moving bars = frames turning over.
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
#include <cstdarg>
#include <cstdio>
#include <string>
using Microsoft::WRL::ComPtr;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;

static std::wstring g_results_path;
static HANDLE g_log_handle = INVALID_HANDLE_VALUE;
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static void log_open() {
    if (g_results_path.empty() || g_log_handle != INVALID_HANDLE_VALUE) return;
    CREATEFILE2_EXTENDED_PARAMETERS ex = {};
    ex.dwSize = sizeof(ex);
    ex.dwFileFlags = FILE_FLAG_WRITE_THROUGH;
    g_log_handle = CreateFile2(g_results_path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, CREATE_ALWAYS, &ex);
}
static void log_line(const char* fmt, ...) {
    char buf[2048]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
    if (g_log_handle == INVALID_HANDLE_VALUE) return;
    char line[2110]; int n = snprintf(line, sizeof(line), "%s\r\n", buf);
    if (n <= 0) return;
    AcquireSRWLockExclusive(&g_log_lock);
    DWORD w = 0; WriteFile(g_log_handle, line, (DWORD)n, &w, nullptr);
    ReleaseSRWLockExclusive(&g_log_lock);
}
static void log_block(const std::string& s) {
    // append multi-line probe log line by line so file is flushed incrementally
    size_t i = 0;
    while (i < s.size()) { size_t j = s.find("\r\n", i); if (j == std::string::npos) j = s.size();
        log_line("%s", s.substr(i, j - i).c_str()); i = j + 2; if (j == s.size()) break; }
}

namespace XboxUwpHost {
ref class App sealed : public Windows::ApplicationModel::Core::IFrameworkView {
public:
    virtual void Initialize(CoreApplicationView^ v) { (void)v; }
    virtual void SetWindow(CoreWindow^ w) {
        m_window = w;
        m_window->Closed += ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &App::OnClosed);
        // INT trap 1: B can raise OS BackRequested and suspend/minimise.
        // Register a handler that marks it Handled so the OS never kills
        // or suspends on B. Whether it fires in the CoreApplication
        // (non-XAML) host shape is proven on console, not assumed here.
        try {
            auto nav = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
            nav->BackRequested += ref new Windows::Foundation::EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(this, &App::OnBackRequested);
            m_backRegistered = true;
        } catch (Platform::Exception^) { m_backRegistered = false; }
    }
    void OnBackRequested(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ args) {
        (void)sender;
        args->Handled = true;
        log_line("INT-BACK: BackRequested fired, Handled=true (OS will not suspend/minimise on B; prototype game_running=0, so no Circle delivered; overlay is View+Menu / L3+R3 only)");
    }
    virtual void Load(Platform::String^ e) { (void)e; }
    virtual void Uninitialize() {}
    virtual void Run() {
        m_window->Activate();
        m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        auto local = Windows::Storage::ApplicationData::Current->LocalFolder;
        g_results_path = std::wstring(local->Path->Data(), local->Path->Length()) + L"\\host-results.txt";
        log_open();
        xbox_cb_set_main_thread(GetCurrentThreadId());
        xbox_cb_set_logger([](const std::string& s) { log_line("%s", s.c_str()); });
        const unsigned long long limit = Windows::System::MemoryManager::AppMemoryUsageLimit;
        log_line("RPCS3 Xbox Integration UWP host (Steps 3-8 regressions, then INT D3D12 backend prototype)");
        log_line("AppMemoryUsageLimit=%llu bytes (%.2f GiB) ModeGuess=%s", limit, limit / 1073741824.0,
            limit > (unsigned long long(3) << 30) ? "GAME" : "APP");
        // 1.0.30 tier diagnostics: manifest diff 1027-vs-1029 proved
        // the manifests identical except version, so the 1 GiB tier
        // in 1.0.29 is NOT manifest-controlled. Log the package
        // identity + level here so the 1.0.30 console header proves
        // which package actually ran and which tier the OS gave it.
        {
            auto pkg = Windows::ApplicationModel::Package::Current;
            auto ver = pkg->Id->Version;
            log_line("BOOT-TIER pkg=%S version=%u.%u.%u.%u limit=%llu level=%d",
                pkg->Id->FamilyName->Data(), (unsigned)ver.Major, (unsigned)ver.Minor,
                (unsigned)ver.Build, (unsigned)ver.Revision,
                limit, (int)Windows::System::MemoryManager::AppMemoryUsageLevel);
        }
        stage_write(3, "RUN_HEADER");
        log_line("STAGE:3:RUN_HEADER");
        { auto iat_log = [](const char* s) { log_line("%s", s); }; iat_sweep(iat_log); }
        log_line("--- D3D12 device + queue (headless bring-up) ---");
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        log_line("D3D12CreateDevice FL11_0: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        bool deviceOk = SUCCEEDED(hr);
        long ctrlHr = -1, mesaHr = -1, hashedHr = -1;
        if (deviceOk) {
            D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
            const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            fl.NumFeatureLevels = 5; fl.pFeatureLevelsRequested = lv;
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
                log_line("MaxFeatureLevel=0x%04X", (unsigned)fl.MaxSupportedFeatureLevel);
            D3D12_FEATURE_DATA_SHADER_MODEL sm{}; sm.HighestShaderModel = D3D_SHADER_MODEL_6_2;
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))))
                log_line("HighestShaderModel=0x%04X", (unsigned)sm.HighestShaderModel);
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue));
            log_line("CreateCommandQueue DIRECT: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
            deviceOk = SUCCEEDED(hr);
        }
        storage_probe();
        signer_probe();
        if (deviceOk) {
            auto inst = Windows::ApplicationModel::Package::Current->InstalledLocation;
            std::wstring base(inst->Path->Data(), inst->Path->Length());
            long e1=0,e2=0,e3=0,e4=0;
            auto vs = load_file_bytes(base + L"\\dxil\\16_vs_simple.dxil", &e1);
            auto ps = load_file_bytes(base + L"\\dxil\\01_fs_simple_color.dxil", &e2);
            auto cvs = load_file_bytes(base + L"\\dxil\\control_vs.cso", &e3);
            auto cps = load_file_bytes(base + L"\\dxil\\control_ps.cso", &e4);
            log_line("bytecode load: mesaVS err=%ld mesaPS err=%ld ctrlVS err=%ld ctrlPS err=%ld", e1, e2, e3, e4);
            log_block(run_pso_probe(m_device.Get(), cvs, cps, "CONTROL-signed", &ctrlHr));
            log_block(run_pso_probe(m_device.Get(), vs, ps, "MESA-unsigned-DXIL", &mesaHr));
            // Step 5 hashed-PSO probe: same Mesa blobs, container hash applied
            // in memory by the gate-1-verified candidate; originals untouched.
            auto hvs = vs, hps = ps;
            bool s1 = sign_dxil_container(hvs.data(), hvs.size());
            bool s2 = sign_dxil_container(hps.data(), hps.size());
            log_line("sign_dxil_container(Mesa VS)=%d (Mesa PS)=%d", s1 ? 1 : 0, s2 ? 1 : 0);
            if (s1 && s2) log_block(run_pso_probe(m_device.Get(), hvs, hps, "MESA-hashed-DXIL", &hashedHr));
            log_line("PSO-SUMMARY control-hr=0x%08lX mesa-unsigned-hr=0x%08lX mesa-hashed-hr=0x%08lX",
                (unsigned long)ctrlHr, (unsigned long)mesaHr, (unsigned long)hashedHr);
            if (mesaHr == 0) log_line("PSO-VERDICT: UNSIGNED-DXIL-ACCEPTED (runtime translation + in-app PSO viable)");
            else if (ctrlHr == 0) log_line("PSO-VERDICT: UNSIGNED-DXIL-REJECTED-WITH-VALID-CONTROL (signing route required; desc/layout proven by control)");
            else log_line("PSO-VERDICT: INCONCLUSIVE (control also failed - desc/driver issue, not a signing verdict)");
            if (hashedHr == 0) log_line("STEP5-VERDICT: HASHED-DXIL-ACCEPTED (runtime translation + in-memory signing viable; Step 3 orange retired)");
            else if (ctrlHr == 0) log_line("STEP5-VERDICT: HASHED-DXIL-REJECTED-WITH-VALID-CONTROL hr=0x%08lX (console enforces more than container hash)", (unsigned long)hashedHr);
            else log_line("STEP5-VERDICT: INCONCLUSIVE (control also failed)");
        }
        // Step 3 regression gate: expected on console is control PASS hr=0 and
        // Mesa unsigned FAIL hr=0x80070057 (Step 3 measured verdict). Anything
        // else is a regression and is named as such in the results.
        const bool step3RegressionOk = deviceOk && ctrlHr == 0 && mesaHr == (long)0x80070057;
        log_line("STEP3-REGRESSION: %s (device=%s control-hr=0x%08lX mesa-hr=0x%08lX; expected control=0x00000000 mesa=0x80070057)",
            step3RegressionOk ? "PASS" : "REGRESSED-OR-CHANGED",
            deviceOk ? "PASS" : "FAIL", (unsigned long)ctrlHr, (unsigned long)mesaHr);
        log_line("OVERALL-PRE-PRESENT: device=%s step3-regression=%s",
            deviceOk ? "PASS" : "FAIL", step3RegressionOk ? "PASS" : "REGRESSED-OR-CHANGED");
        // Step 7 bring-up (independent of GPU): audio engine + tone first so
        // it runs during every later phase, then pad enumeration. Failures
        // are recorded here with exact HRESULTs and gated at the verdicts.
        log_block(m_audio.init_and_start());
        log_block(m_pad.init_and_enumerate());
        log_line("STEP7-BRINGUP: audio-running=%d xaudio-ok=%d wasapi-attempted=%d wasapi-ok=%d pad-statics=%s pad-count=%u AppMemoryUsage=%llu AppMemoryUsageLimit=%llu",
            m_audio.running() ? 1 : 0, m_audio.metrics().xaudio_ok ? 1 : 0,
            m_audio.metrics().wasapi_attempted ? 1 : 0, m_audio.metrics().wasapi_ok ? 1 : 0,
            m_pad.metrics().statics_ok ? "PASS" : "FAIL", m_pad.metrics().count,
            (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage,
            (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit);
        // Step 6 shaders: packaged SPIR-V + unsigned Mesa DXIL, resolved via
        // the Step 5 cache (LocalState\cache) + in-memory hash. No translator
        // exists in the sandbox (Step 3 measured); first run is a cache MISS
        // -> packaged+hash+store, later runs HIT.
        DrawShaderOutputs drawShaders;
        if (deviceOk) {
            auto inst = Windows::ApplicationModel::Package::Current->InstalledLocation;
            std::wstring base(inst->Path->Data(), inst->Path->Length());
            auto localF = Windows::Storage::ApplicationData::Current->LocalFolder;
            std::wstring localPath(localF->Path->Data(), localF->Path->Length());
            long da=0,db=0,dc=0,dd=0;
            DrawShaderInputs din;
            din.cacheDir = localPath + L"\\cache";
            din.spvVs = load_file_bytes(base + L"\\dxil\\draw_vs.spv", &da);
            din.spvFs = load_file_bytes(base + L"\\dxil\\draw_fs.spv", &db);
            din.packagedVs = load_file_bytes(base + L"\\dxil\\draw_vs.dxil", &dc);
            din.packagedFs = load_file_bytes(base + L"\\dxil\\draw_fs.dxil", &dd);
            log_line("Step6 shader inputs: spvVS err=%ld spvFS err=%ld dxilVS err=%ld dxilFS err=%ld", da, db, dc, dd);
            drawShaders = resolve_draw_dxil(din);
            log_block(drawShaders.log);
        }
        run_present_step4(deviceOk, drawShaders);
    }
private:
    CoreWindow^ m_window = nullptr; bool m_closed = false; bool m_backRegistered = false;
    ComPtr<ID3D12Device> m_device; ComPtr<ID3D12CommandQueue> m_queue;
    AudioProbe m_audio; PadProbe m_pad;
    unsigned long long m_pollTick = 0;
    // Step 7: one pad poll (re-enumerate ~every 60 ticks); transitions logged.
    void step7_poll() {
        std::vector<PadTransition> tr;
        m_pad.poll_once((m_pollTick++ % 60) == 0, tr);
        for (auto& x : tr) log_line("%s", x.text.c_str());
    }
    void step7_heartbeat(const char* tag) {
        log_line("%s", m_audio.state_line(tag).c_str());
        log_line("%s", m_pad.summary(tag).c_str());
        log_line("MEMORY-%s AppMemoryUsage=%llu AppMemoryUsageLimit=%llu deviceRemovedReason=0x%08lX", tag,
            (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage,
            (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit,
            m_device ? (unsigned long)m_device->GetDeviceRemovedReason() : 0ul);
    }
    void OnClosed(CoreWindow^ s, CoreWindowEventArgs^ a) { (void)s; (void)a; m_closed = true; }
    // Poll an async op to completion while pumping the dispatcher (5s cap).
    template <typename TOp> bool wait_op(TOp^ op) {
        for (int i = 0; i < 100; ++i) {
            if (op->Status != Windows::Foundation::AsyncStatus::Started) return true;
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(50);
        }
        return op->Status != Windows::Foundation::AsyncStatus::Started;
    }
    void storage_probe() {
        log_line("--- Storage seam probe (no firmware/game file touched) ---");
        try {
            auto op = Windows::Storage::KnownFolders::RemovableDevices->GetFoldersAsync();
            if (!wait_op(op)) { log_line("RemovableDevices.GetFoldersAsync: TIMEOUT"); }
            else if (op->Status == Windows::Foundation::AsyncStatus::Completed) {
                auto folders = op->GetResults();
                log_line("RemovableDevices enumeration: PASS count=%u", folders->Size);
                for (unsigned i = 0; i < folders->Size && i < 4; ++i) {
                    auto f = folders->GetAt(i);
                    log_line("  removable[%u] name-len=%u path-len=%u", i, f->Name->Length(), f->Path->Length());
                }
            } else log_line("RemovableDevices.GetFoldersAsync: FAIL status=%d hr=0x%08lX", (int)op->Status, (unsigned long)op->ErrorCode.Value);
        } catch (Platform::Exception^ ex) { log_line("RemovableDevices: EXCEPTION hr=0x%08lX", (unsigned long)ex->HResult); }
        try {
            auto op2 = Windows::Storage::StorageFolder::GetFolderFromPathAsync(L"C:\\");
            if (!wait_op(op2)) log_line("broadFileSystemAccess path probe C:\\\\: TIMEOUT");
            else if (op2->Status == Windows::Foundation::AsyncStatus::Completed)
                log_line("broadFileSystemAccess path probe C:\\\\: GRANTED (folder obtained)");
            else log_line("broadFileSystemAccess path probe C:\\\\: DENIED status=%d hr=0x%08lX (file-picker / removableStorage route required)", (int)op2->Status, (unsigned long)op2->ErrorCode.Value);
        } catch (Platform::Exception^ ex) { log_line("broadFileSystemAccess path probe: EXCEPTION hr=0x%08lX", (unsigned long)ex->HResult); }
    }
    void signer_probe() {
        log_line("--- DXIL signer loadability probe (informational, before PSO) ---");
        const wchar_t* names[] = {L"dxil.dll", L"dxcompiler.dll"};
        for (auto n : names) {
            SetLastError(0); HMODULE h = LoadPackagedLibrary(n, 0); DWORD e = GetLastError();
            log_line("LoadPackagedLibrary(%ls): handle=%p GetLastError=%lu", n, (void*)h, e);
            if (h) FreeLibrary(h);
            wchar_t syspath[MAX_PATH]; unsigned len = GetSystemDirectoryW(syspath, MAX_PATH);
            if (len && len < MAX_PATH - 32) {
                std::wstring p(syspath, len); p += L"\\"; p += n;
                SetLastError(0); HMODULE h2 = LoadLibraryExW(p.c_str(), nullptr, 0); DWORD e2 = GetLastError();
                log_line("LoadLibraryExW(System32\\%ls): handle=%p GetLastError=%lu", n, (void*)h2, e2);
                if (h2) FreeLibrary(h2);
            }
        }
    }
    // Step 4: CoreWindow swapchain + sustained present loop. Bounded phase
    // is 600 presented frames (metrics verdict written at that point); the
    // loop then keeps presenting until the window closes, logging a
    // heartbeat every 300 frames so a fetched results file always shows
    // the latest sustained state. Every failure is logged with its HRESULT.
    void run_present_step4(bool deviceOk, const DrawShaderOutputs& drawShaders) {
        log_line("--- Step 4 swapchain + present, then Step 6 draw + null-flip ---");
        if (!deviceOk || !m_device || !m_queue) {
            log_line("STEP4-VERDICT: BLOCKED (no device/queue - swapchain not attempted, no present claimed)");
            while (!m_closed) m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            return;
        }
        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        log_line("CreateDXGIFactory2: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        if (FAILED(hr)) { hold_after_failure("STEP4-VERDICT: BLOCKED (DXGI factory failed)"); return; }
        auto b = m_window->Bounds;
        log_line("CoreWindow Bounds: x=%.1f y=%.1f width=%.1f height=%.1f (DIPs, as reported by console)", b.X, b.Y, b.Width, b.Height);
        const bool boundsUsable = b.Width >= 16 && b.Height >= 16;
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = boundsUsable ? (UINT)b.Width : 1280;
        sd.Height = boundsUsable ? (UINT)b.Height : 720;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.SampleDesc.Count = 1;
        log_line("SwapChain requested: width=%u height=%u bufferCount=%u format=R8G8B8A8_UNORM swapEffect=FLIP_DISCARD sampleCount=1%s",
            sd.Width, sd.Height, sd.BufferCount, boundsUsable ? "" : " (FALLBACK size: window bounds unusable)");
        ComPtr<IDXGISwapChain1> s1;
        hr = factory->CreateSwapChainForCoreWindow(m_queue.Get(), reinterpret_cast<IUnknown*>(m_window), &sd, nullptr, &s1);
        log_line("CreateSwapChainForCoreWindow: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        if (FAILED(hr)) { hold_after_failure("STEP4-VERDICT: BLOCKED (swapchain creation failed - see hr above)"); return; }
        PresentLoop loop; bool ok = false;
        log_block(loop.init(m_device.Get(), m_queue.Get(), s1.Get(), &ok));
        if (!ok) { hold_after_failure("STEP4-VERDICT: BLOCKED (present-loop init failed - see lines above)"); return; }
        // Step 6 init on the same swapchain/queue (fence-serialised, so the
        // two loops can take turns). A failed draw PSO stops Step 6 at its
        // gate; the Step 4 bars keep running as the visible regression.
        Step6Loop s6; bool s6ok = false; long s6psoHr = -1;
        if (drawShaders.ok) {
            log_block(s6.init(m_device.Get(), m_queue.Get(), s1.Get(), drawShaders.signedVs, drawShaders.signedFs, &s6psoHr, &s6ok));
        } else {
            log_line("STEP6-INIT: skipped (shader resolve failed - see STEP6-SHADERS line above)");
        }
        log_line("STEP6-DRAW-PSO: %s hr=0x%08lX init-ok=%d", (s6ok && s6psoHr == 0) ? "PASS" : "FAIL/BLOCKED", (unsigned long)s6psoHr, s6ok ? 1 : 0);
        const unsigned long long kBounded = 600;
        log_line("Present loop start: pattern=8 colour bars advancing every 15 frames, vsync Present(1,0), bounded-phase=%llu frames then sustained until close", kBounded);
        bool boundedLogged = false;
        unsigned long long failLinesLogged = 0;
        while (!m_closed) {
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            if (m_closed) break;
            HRESULT phr = loop.render_frame();
            step7_poll();
            const auto& m = loop.metrics();
            if (FAILED(phr)) {
                if (failLinesLogged < 5) { failLinesLogged++; log_line("frame %llu FAILED hr=0x%08lX", m.frames_attempted, (unsigned long)phr); }
                if (m.close_failures > 0) {
                    log_line("%s", loop.summary("PRESENT-ABORT").c_str());
                    log_line("STEP4-VERDICT: BLOCKED (command list Close failed hr=0x%08lX - loop aborted, not spun)", (unsigned long)m.last_close_hr);
                    hold_after_failure(nullptr);
                    return;
                }
                HRESULT removed = m_device->GetDeviceRemovedReason();
                if (FAILED(removed) || phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET || phr == DXGI_ERROR_DEVICE_HUNG) {
                    log_line("%s", loop.summary("PRESENT-ABORT").c_str());
                    log_line("STEP4-VERDICT: BLOCKED (device removed/reset during present, GetDeviceRemovedReason=0x%08lX)", (unsigned long)removed);
                    hold_after_failure(nullptr);
                    return;
                }
            }
            if (!boundedLogged && m.frames_attempted % 120 == 0 && m.frames_attempted < kBounded)
                log_line("%s", loop.summary("PRESENT-PROGRESS").c_str());
            if (!boundedLogged && m.frames_attempted >= kBounded) {
                boundedLogged = true;
                log_line("%s", loop.summary("PRESENT-BOUNDED-600").c_str());
                HRESULT removed = m_device->GetDeviceRemovedReason();
                const bool pass = m.frames_presented >= kBounded && m.present_failures == 0 && m.close_failures == 0 && SUCCEEDED(removed);
                log_line("STEP4-BOUNDED-VERDICT: %s (presented=%llu/%llu present-failures=%llu deviceRemovedReason=0x%08lX; loop continues sustained)",
                    pass ? "PASS" : "FAIL", m.frames_presented, kBounded, m.present_failures, (unsigned long)removed);
            }
            if (boundedLogged && m.frames_attempted % 300 == 0)
                log_line("%s", loop.summary("PRESENT-HEARTBEAT").c_str());
            if (boundedLogged) break; // Step 4 bounded verdict recorded; Step 6 phases follow
        }
        // ---- Step 6 bounded phases (300 draw + 300 null-flip), then sustained ----
        auto mem = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage; };
        if (!m_closed && s6ok) {
            const unsigned long long kDraw = 300, kFlip = 300;
            log_line("STEP6-DRAW-PHASE start: %llu frames, 1 DrawIndexedInstanced(6) per frame, AppMemoryUsage=%llu", kDraw, mem());
            unsigned pixA[4] = {}, pixB[4] = {};
            while (!m_closed && s6.draw_metrics().frames_attempted < kDraw) {
                m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (m_closed) break;
                HRESULT phr = s6.render_draw_frame();
                step7_poll();
                const auto& dm = s6.draw_metrics();
                if (FAILED(phr) && dm.present_failures <= 5) log_line("draw frame %llu FAILED hr=0x%08lX", dm.frames_attempted, (unsigned long)phr);
                if (dm.frames_attempted == 30) memcpy(pixA, dm.last_pixel_rgba, sizeof(pixA));
                if (dm.frames_attempted == 90) memcpy(pixB, dm.last_pixel_rgba, sizeof(pixB));
                if (dm.frames_attempted % 60 == 0) log_line("%s", s6.draw_summary("DRAW-PROGRESS").c_str());
                if (FAILED(m_device->GetDeviceRemovedReason())) break;
            }
            log_line("%s", s6.draw_summary("DRAW-BOUNDED-300").c_str());
            log_line("DRAW-PIXEL frame30 RGBA=(%u,%u,%u,%u) frame90 RGBA=(%u,%u,%u,%u) AppMemoryUsage=%llu",
                pixA[0], pixA[1], pixA[2], pixA[3], pixB[0], pixB[1], pixB[2], pixB[3], mem());
            auto nv = [](unsigned v, unsigned e) { return v + 6 >= e && e + 6 >= v; };
            const bool pixOk = nv(pixA[0],40) && nv(pixA[1],40) && nv(pixA[2],50) && nv(pixB[0],10) && nv(pixB[1],40) && nv(pixB[2],200);
            const auto& dm = s6.draw_metrics();
            const bool drawPass = dm.frames_presented >= kDraw && dm.present_failures == 0 && dm.close_failures == 0 &&
                                  dm.draw_calls >= kDraw && SUCCEEDED(m_device->GetDeviceRemovedReason());
            log_line("STEP6-DRAW-VERDICT: %s (presented=%llu/%llu draw-calls=%llu pixel-tint-proof=%s deviceRemovedReason=0x%08lX)",
                drawPass ? "PASS" : "FAIL", dm.frames_presented, kDraw, dm.draw_calls, pixOk ? "PASS" : "FAIL",
                (unsigned long)m_device->GetDeviceRemovedReason());
            log_line("STEP6-NULLFLIP-PHASE start: %llu frames, synthetic 1280x720 A8R8G8B8 staging-copy, AppMemoryUsage=%llu", kFlip, mem());
            while (!m_closed && s6.nullflip_metrics().frames_attempted < kFlip) {
                m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (m_closed) break;
                HRESULT phr = s6.render_nullflip_frame();
                step7_poll();
                const auto& fm = s6.nullflip_metrics();
                if (FAILED(phr) && fm.present_failures <= 5) log_line("nullflip frame %llu FAILED hr=0x%08lX", fm.frames_attempted, (unsigned long)phr);
                if (fm.frames_attempted % 60 == 0) log_line("%s", s6.nullflip_summary("NULLFLIP-PROGRESS").c_str());
                if (FAILED(m_device->GetDeviceRemovedReason())) break;
            }
            log_line("%s", s6.nullflip_summary("NULLFLIP-BOUNDED-300").c_str());
            const auto& fm = s6.nullflip_metrics();
            const bool flipPixOk = fm.last_pixel_rgba[0] == 255 && fm.last_pixel_rgba[1] == 0 && fm.last_pixel_rgba[2] == 0 && fm.last_pixel_rgba[3] == 255;
            const bool flipPass = fm.frames_presented >= kFlip && fm.present_failures == 0 && fm.close_failures == 0 &&
                                  SUCCEEDED(m_device->GetDeviceRemovedReason());
            log_line("STEP6-NULLFLIP-VERDICT: %s (presented=%llu/%llu red-pixel-proof=%s FORMAT-DECISION=cpu-swizzle guest-A8R8G8B8->R8G8B8A8_UNORM AppMemoryUsage=%llu)",
                flipPass ? "PASS" : "FAIL", fm.frames_presented, kFlip, flipPixOk ? "PASS" : "FAIL", mem());
            log_line("STEP6-VERDICT: %s (draw=%s nullflip=%s; synthetic buffers only - no title/RSX stream ran; sustained alternating loop follows)",
                (drawPass && flipPass) ? "PASS" : "FAIL", drawPass ? "PASS" : "FAIL", flipPass ? "PASS" : "FAIL");
            // ---- Step 7 bounded phase: 60 s of tone + pad polling while presenting ----
            {
                log_line("STEP7-PHASE start: 60s bounded (looped tone running, pad polled per frame, re-enumerate ~1Hz, draw/null-flip alternating) - press/move the controller now");
                step7_heartbeat("STEP7-START");
                LARGE_INTEGER fq, a, bq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&a);
                unsigned long long startPolls = m_pad.metrics().polls;
                bool blk = true; unsigned inBlock = 0; unsigned hb = 0;
                double elapsed = 0;
                while (!m_closed) {
                    QueryPerformanceCounter(&bq);
                    elapsed = (double)(bq.QuadPart - a.QuadPart) / (double)fq.QuadPart;
                    if (elapsed >= 60.0) break;
                    m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (m_closed) break;
                    if (blk) s6.render_draw_frame(); else s6.render_nullflip_frame();
                    step7_poll();
                    if (++inBlock >= 300) { inBlock = 0; blk = !blk; }
                    if (++hb >= 300) { hb = 0; step7_heartbeat("STEP7-HEARTBEAT"); }
                    if (FAILED(m_device->GetDeviceRemovedReason())) break;
                }
                log_line("STEP7-PHASE elapsed=%.1fs polls-in-phase=%llu window-closed=%d", elapsed, m_pad.metrics().polls - startPolls, m_closed ? 1 : 0);
                step7_heartbeat("STEP7-BOUNDED-END");
                const auto& am = m_audio.metrics(); const auto& pm = m_pad.metrics();
                log_line("STEP7-AUDIO-VERDICT: %s (xaudio-create=0x%08lX mastering=0x%08lX source=0x%08lX submit=0x%08lX start=0x%08lX mastering-negotiated=%uch/%uHz source-negotiated=%uch/%uHz wasapi-attempted=%d wasapi-ok=%d wasapi-negotiated=%uch/%uHz/%ubit; audible output NOT proven - user report required)",
                    m_audio.running() ? "PASS" : "BLOCKED",
                    (unsigned long)am.xaudio_create_hr, (unsigned long)am.mastering_hr, (unsigned long)am.source_hr,
                    (unsigned long)am.submit_hr, (unsigned long)am.start_hr,
                    am.mastering_channels, am.mastering_rate, am.source_channels, am.source_rate,
                    am.wasapi_attempted ? 1 : 0, am.wasapi_ok ? 1 : 0, am.wasapi_channels, am.wasapi_rate, am.wasapi_bits);
                if (!pm.statics_ok) {
                    log_line("STEP7-PAD-VERDICT: BLOCKED (IGamepadStatics failed hr=0x%08lX - enumeration not possible with current manifest/API surface; no capability invented)", (unsigned long)pm.statics_hr);
                } else if (pm.transitions > 0) {
                    log_line("STEP7-PAD-VERDICT: PASS (enumeration count=%u + %llu transitions measured with timestamps; see PAD-TRANSITION lines)", pm.count, pm.transitions);
                } else if (pm.count > 0) {
                    log_line("STEP7-PAD-VERDICT: ENUMERATION-PROVEN-TRANSITIONS-NOT-YET-MEASURED (count=%u polls=%llu transitions=0 in bounded phase; sustained loop below keeps polling - press buttons/move sticks while it runs)", pm.count, pm.polls);
                } else {
                    log_line("STEP7-PAD-VERDICT: ENUMERATION-PROVEN-TRANSITIONS-NOT-YET-MEASURED (count=0 at probe time, polls=%llu, no input faked; sustained loop below keeps polling)", pm.polls);
                }
                log_line("STEP7-BOUNDED-VERDICT: audio=%s pad-statics=%s pad-count=%u transitions=%llu count-changes=%u disconnect-reconnect=%s",
                    m_audio.running() ? "PASS" : "BLOCKED", pm.statics_ok ? "PASS" : "BLOCKED", pm.count, pm.transitions, pm.count_changes,
                    pm.count_changes > 0 ? "OBSERVED-BY-REENUMERATION (see PAD-COUNT-CHANGE lines)" : "UNOBSERVED (no plug/unplug during run)");
            }
            // ---- Step 8: memory governor (audio still running; draw frames
            // + pad polls run between stress steps; sampler is the real
            // MemoryManager AppMemoryUsage + DXGI VidMem where available) ----
            if (!m_closed && SUCCEEDED(m_device->GetDeviceRemovedReason())) {
                log_line("STEP8-PHASE start: memory governor (synthetic pools, pre-declared abort line 3.0 GiB on AppMemoryUsage, limit=%llu)",
                    (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit);
                ComPtr<IDXGIAdapter3> adapter3; ComPtr<IDXGIAdapter1> a1;
                if (factory && SUCCEEDED(factory->EnumAdapters1(0, &a1)) && a1) a1.As(&adapter3);
                log_line("STEP8-SAMPLER adapter3-for-VidMem=%s (AppMemoryUsage is the governed figure on console; psapi PrivateCommit/WorkingSet do not exist here and are reported UNAVAILABLE)",
                    adapter3 ? "present" : "unavailable");
                const unsigned long long drawBefore8 = s6.draw_metrics().frames_presented;
                const unsigned long long drawFailsBefore8 = s6.draw_metrics().present_failures;
                GovernorConfig gcfg;
                auto gres = run_step8_governor(gcfg,
                    [](const std::string& l) { log_line("%s", l.c_str()); },
                    [&]() {
                        MemSample s; s.has_app = true;
                        s.app_usage = (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage;
                        s.app_limit = (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit;
                        s.app_level = (int)Windows::System::MemoryManager::AppMemoryUsageLevel;
                        if (adapter3) {
                            DXGI_QUERY_VIDEO_MEMORY_INFO loc{}, non{};
                            HRESULT h1 = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc);
                            HRESULT h2 = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non);
                            if (SUCCEEDED(h1) || SUCCEEDED(h2)) {
                                s.has_vidmem = true;
                                if (SUCCEEDED(h1)) { s.vid_local_budget = loc.Budget; s.vid_local_usage = loc.CurrentUsage; }
                                if (SUCCEEDED(h2)) { s.vid_nonlocal_budget = non.Budget; s.vid_nonlocal_usage = non.CurrentUsage; }
                            }
                        }
                        return s;
                    },
                    [&](std::string& why) -> bool {
                        m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                        if (m_closed) { why = "window closed"; return false; }
                        HRESULT fhr = s6.render_draw_frame();
                        step7_poll();
                        HRESULT rem = m_device->GetDeviceRemovedReason();
                        if (FAILED(fhr) || FAILED(rem) || fhr == DXGI_ERROR_DEVICE_REMOVED || fhr == DXGI_ERROR_DEVICE_RESET || fhr == DXGI_ERROR_DEVICE_HUNG) {
                            char b[192]; snprintf(b, sizeof(b), "draw frame hr=0x%08lX deviceRemovedReason=0x%08lX", (unsigned long)fhr, (unsigned long)rem);
                            why = b; return false;
                        }
                        return true;
                    });
                const auto& dm8 = s6.draw_metrics();
                log_line("STEP8-DURING-STRESS draw-presented-delta=%llu present-failures-delta=%llu deviceRemovedReason=0x%08lX",
                    dm8.frames_presented - drawBefore8, dm8.present_failures - drawFailsBefore8,
                    (unsigned long)m_device->GetDeviceRemovedReason());
                step7_heartbeat("STEP8-END");
                log_line("STEP8-VERDICT: %s (reserve-commit=%s denial=%s eviction=%s stress-to-caps=%s abort=%s peak-AppMemoryUsage=%llu pool-peak=%llu)",
                    gres.pass() ? "PASS" : "FAIL/ABORTED",
                    gres.reserve_commit_ok ? "PASS" : "FAIL", gres.denial_ok ? "PASS" : "FAIL",
                    gres.eviction_ok ? "PASS" : "FAIL", gres.stress_completed ? "COMPLETED" : "NOT-COMPLETED",
                    gres.abort_fired ? gres.abort_reason.c_str() : "none",
                    gres.peak_governed_figure, gres.peak_total_pool_bytes);
                if (!gres.pass())
                    log_line("STEP8-VERDICT-NOTE: governor did NOT pass on console (see STEP8-STRESS-ABORT / verdict lines above - recorded, not hidden); sustained regression loop still follows if device alive");
            }
            // ---- Integration: D3D12GSRender prototype (synthetic commands only) ----
            // Qt-free, callback-driven. No title, no real RSX stream, no
            // firmware/game, no core change. Real PPU/SPU execution is NOT
            // linked here (see INT-CPU lines); it is exercised by the PC
            // RPCS3 build in the PC title test, not faked here.
            {
                log_line("INT-PHASE start: D3D12GSRender prototype (FIFO -> begin/end -> draw -> flip shape of VKGSRender::end)");
                log_line("INT-BACK-HANDLER: %s (BackRequested in CoreApplication host; console proof pending)", m_backRegistered ? "REGISTERED" : "NOT-AVAILABLE");
                d3d12_backend::D3D12GSRender backend(
                    [](const std::string& l) { log_line("%s", l.c_str()); },
                    [&]() { return s6.render_draw_frame(); },
                    [&]() { return s6.render_nullflip_frame(); });
                backend.set_render_size(1280, 720);
                backend.set_y_flip(true); // viewport-negative-height ONLY; translator flip_yz stays OFF
                d3d12_backend::CpuModel cpu;
                cpu.start_ppu([](const std::string& l) { log_line("%s", l.c_str()); });
                cpu.start_spu([](const std::string& l) { log_line("%s", l.c_str()); });
                log_line("INT-CPU: llvm_threads=%d (pinned at 1; per-module LLVMContext/Module destroyed after emission â€” design, not yet measured on a real module)", cpu.llvm_threads);
                d3d12_backend::BackButtonPolicy backPol; backPol.game_running = false;
                log_line("INT-BACK: policy game_running=0 action=%d (0=ignore,1=Circle,2=overlay-combo View+Menu/L3+R3)", backPol.on_back_requested(false, false));
                bool guestOk = backend.guest_reserve() && backend.guest_commit(0, 0xA5) &&
                               backend.guest_translate(0, 0) != nullptr && backend.guest_translate(0, 1) != nullptr;
                log_line("INT-GUEST-MAP: translate main=%p rsx=%p committed=%llu", backend.guest_translate(0, 0), backend.guest_translate(0, 1), backend.guest_translate(0, 0) ? 67108864ull : 0ull);
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
                log_line("INT-FIFO: %s fail=%s", fifoOk ? "PASS" : "FAIL", fifoFail.c_str());
                log_line("%s", backend.summary().c_str());
                bool intOk = guestOk && fifoOk && backend.verdict_ok();
                log_line("INT-VERDICT: %s (guest=%s fifo=%s backend=%s; synthetic only â€” no title, no RSX stream, no PPU/SPU execution linked)", intOk ? "PASS" : "FAIL", guestOk ? "PASS" : "FAIL", fifoOk ? "PASS" : "FAIL", backend.verdict_ok() ? "PASS" : "FAIL");
                backend.guest_release();
                log_line("INT-GUEST-MAP: released after INT phase");
            }
            // ---- Emu-link: fill the real emu_callbacks surface with host
            // callbacks (EMU-LINK phase, v1.0.7.0). No core header is
            // included and the real Emulator object is NOT instantiated
            // here â€” see emu_link.h for the measured reason. This phase
            // proves the wiring shape and its boot gate, nothing more.
            {
                log_line("EMU-LINK-PHASE start: emu_callbacks surface, host callbacks, PSN Simulated, governor-owned, llvm_threads=1");
                emu_link::EmuLink link([](const std::string& l) { log_line("%s", l.c_str()); });
                link.set_psn(emu_link::PsnStatus::kSimulated);
                emu_link::HostCallbacks cb;
                // FIX 2026-10-27: the old code posted with RunAsync and
                // returned at once, so probe() counted main_thread=0 before
                // the dispatcher ever ran it (desktop passed only because
                // its callback invoked f() synchronously). Pump the
                // dispatcher for up to 2000 ms so this proves delivery.
                cb.call_from_main_thread = [&](std::function<void()> f) {
                    bool done = false;
                    auto h = ref new Windows::UI::Core::DispatchedHandler([f, &done]() { try { f(); } catch (Platform::Exception^ ex) { log_line("BOOT-DISPATCH-EXCEPTION winrt hr=0x%08lX (swallowed; callback marked done)", (unsigned long)ex->HResult); } catch (...) { log_line("BOOT-DISPATCH-EXCEPTION std (swallowed; callback marked done)"); } done = true; });
                    m_window->Dispatcher->RunAsync(Windows::UI::Core::CoreDispatcherPriority::Normal, h);
                    unsigned long long start = GetTickCount64();
                    while (!done && GetTickCount64() - start < 2000) {
                        m_window->Dispatcher->ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
                    }
                    log_line("EMU-LINK-MAINTHREAD-DISPATCH: %s (delivered within pump window)", done ? "PASS" : "TIMEOUT");
                };
                cb.init_gs_render = []() {};
                cb.get_gs_frame = [&]() { return m_window->Bounds.Width > 0 && m_window->Bounds.Height > 0; };
                cb.init_pad_handler = [](const std::string& t) { log_line("EMU-LINK-PAD: init_pad_handler title_id=%s (WGI; B->Circle when game_running)", t.c_str()); };
                cb.get_audio = [&]() { return m_audio.running(); };
                cb.on_run = []() { log_line("EMU-LINK-LIFECYCLE: on_run"); };
                cb.on_pause = []() { log_line("EMU-LINK-LIFECYCLE: on_pause"); };
                cb.on_resume = []() { log_line("EMU-LINK-LIFECYCLE: on_resume"); };
                cb.on_stop = []() { log_line("EMU-LINK-LIFECYCLE: on_stop"); };
                bool bound = link.bind(cb);
                bool probed = link.probe();
                log_line("%s", link.summary().c_str());
                std::string gateReason; bool gate = link.boot_gate(gateReason);
                log_line("EMU-LINK-BOOT-GATE: %s reason=%s", gate ? "OPEN" : "BLOCKED-real-Emu-not-linked", gateReason.c_str());
                log_line("EMU-LINK-PSN: status=Simulated (fallback RPCN, never real PSN) Y-flip=viewport-negative-only flip_yz=OFF llvm_threads=1 governor-caps=tex1200/shader256/upload256/guest512MiB abort3.0GiB");
                log_line("EMU-LINK-VERDICT: %s (bound=%d probed=%d psn=Simulated; boot gate BLOCKED is the next work, not a verdict failure)", link.verdict_ok() ? "PASS" : "FAIL", (int)bound, (int)probed);
            }
            // ---- Reassemble: Portal ISO fallback. If bo3.iso.part0..3 are
            // in LocalState, reassemble them into the full ISO. Parts are
            // NOT deleted here, so a failed size check can retry on a
            // later launch. Firmware (PS3UPDAT.PUP) is a single upload and
            // needs no reassembly.
            {
                std::string dir;
                if (!g_results_path.empty()) {
                    size_t slash = g_results_path.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) {
                        std::wstring wdir = g_results_path.substr(0, slash);
                        dir.assign(wdir.begin(), wdir.end());
                    }
                }
                uint64_t outBytes = 0; std::string fail;
                bool ok = reassemble::reassemble(dir, "bo3.iso", 4, "Call of Duty - Black Ops III.iso", 4599644160ull, outBytes, fail);
                log_line("REASSEMBLE-RESULT: %s bytes=%llu expected=4599644160 fail=%s", ok ? "PASS" : "SKIPPED-or-FAIL", outBytes, fail.c_str());
                if (!dir.empty() && !ok && fail.find("missing part") == std::string::npos)
                    log_line("REASSEMBLE-VERDICT: FAIL (parts present but size mismatch; parts kept for retry)");
                else if (ok)
                    log_line("REASSEMBLE-VERDICT: PASS (full ISO present in LocalState)");
                else
                    log_line("REASSEMBLE-VERDICT: SKIPPED (parts not all present yet; upload them via Portal, then relaunch)");
            }
            // ---- EMU-INIT + FIRMWARE (v1.0.10.0) + BOOT (v1.0.22.0):
            // real Emulator object linked in (emucore.lib), config dir =
            // LocalState, firmware install from the Portal-uploaded PUP,
            // then the ISO-boot probe (NullGSRender first). A failed
            // INIT skips firmware and boot; a missing PUP logs
            // FIRMWARE-SKIP and a missing ISO logs BOOT-ISO-MISSING,
            // neither a package failure.
            {
                std::string lsdir;
                if (!g_results_path.empty()) {
                    size_t slash = g_results_path.find_last_of(L"\\/");
                    if (slash != std::wstring::npos) {
                        std::wstring wdir = g_results_path.substr(0, slash);
                        lsdir.assign(wdir.begin(), wdir.end());
                        for (char& c : lsdir) if (c == '\\') c = '/';
                    }
                }
                watchdog_start(lsdir);
                {
                    std::wstring wls(lsdir.begin(), lsdir.end());
                    for (auto& c : wls) if (c == L'/') c = L'\\';
                    xbox_fatal_set_dir(wls.c_str());
                    xbox_hooks_install(wls);
                }
                // 1.0.36 device handoff: host D3D12 device/queue/swapchain
                // to the RSX backend BEFORE BootGame (see rsx_counters.h).
                // 1280x720 is the RSX buffer size (Series S target), NOT
                // the swapchain size - the Slice 1 blit scales to the
                // backbuffer at present time. s1 outlives Run(), so the
                // non-owning bridge pointers stay valid.
                {
                    ComPtr<IDXGISwapChain3> s3;
                    HRESULT scHr = s1.As(&s3);
                    D3D12HostBridge bridge{};
                    bridge.device = m_device.Get();
                    bridge.queue = m_queue.Get();
                    bridge.swapchain = SUCCEEDED(scHr) ? s3.Get() : nullptr;
                    bridge.width = 1280;
                    bridge.height = 720;
                    bridge.rsx_owns_swapchain = d3d12_bridge_owns_ptr();
                    SetD3D12HostBridge(bridge);
                    log_line("BOOT-BRIDGE device=%p queue=%p swapchain3=%p (qi hr=0x%08lX) size=1280x720 owns_ptr=%p",
                        (void*)bridge.device, (void*)bridge.queue, (void*)bridge.swapchain, (unsigned long)scHr, (void*)bridge.rsx_owns_swapchain);
                }
                auto hostLog = [](const std::string& l) { log_line("%s", l.c_str()); };
// 1.0.35 callback queue (Argon fix 1): WinRT never sees
// emulator callbacks. RunAsync is REMOVED entirely; the
// queue covers all call_from_main_thread sites (boot +
// init phases), drained by the main loop below. The
// 1.0.32 dispatch wrap is gone with it (no RunAsync
// use remains); callback exceptions are logged as
// BOOT-CALLBACK-EXCEPTION by the drain loop.
auto hostPump = [&](std::function<void()> f) {
    xbox_cb_enqueue_and_wait(f, 2000);
};
// Drain queued emulator callbacks on the main thread.
// Exceptions are logged and swallowed; done is ALWAYS
// set so the waiting worker unblocks.
auto drain_callbacks = [&]() {
    while (auto item = xbox_cb_pop()) {
        try { item->f(); }
        catch (Platform::Exception^ ex) { log_line("BOOT-CALLBACK-EXCEPTION winrt hr=0x%08lX (callback dropped, app stays resident)", (unsigned long)ex->HResult); }
        catch (...) { log_line("BOOT-CALLBACK-EXCEPTION std (callback dropped, app stays resident)"); }
        item->done.store(true);
    }
};
                stage_write(4, "PRE_EMU_INIT");
                log_line("STAGE:4:PRE_EMU_INIT");
                const bool initOk = emu_host::init_phase(lsdir, hostLog, hostPump);
                log_line("EMU-INIT-VERDICT: %s (real Emulator object in host; BOOT phase follows in this build)", initOk ? "PASS" : "FAIL");
                if (initOk) {
                    const bool fwOk = emu_host::firmware_phase(lsdir, hostLog);
                    log_line("EMU-FIRMWARE-PHASE-VERDICT: %s", fwOk ? "PASS" : "FAIL-or-SKIPPED (see FIRMWARE lines above)");
                    // ---- BOOT (v1.0.22.0): ISO-boot probe, NullGSRender
                    // first. Gated on initOk (same scope): a failed INIT
                    // must never reach BootGame. BOOT-ISO-MISSING on an
                    // empty LocalState is SKIPPED, not a failure; the
                    // sustained loop below still runs afterwards.
                    {
                        emu_host::boot_callbacks bcb;
                        bcb.log = hostLog;
                        bcb.pump = hostPump;
                        bcb.mem_usage = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage; };
                        bcb.mem_limit = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit; };
                        emu_host::boot_outcome bout = emu_host::boot_phase(lsdir, bcb);
                        watchdog_boot_returned();
                        xbox_fatal_set_boot_ms(GetTickCount64());
                        drain_callbacks();
                        if (bout.attempted && bout.result_code == 0) {
                            // 1.0.31 residency fix: the 1.0.30 console log ends at BOOT-PPU with no
                            // watch line, so entry itself was unprovable. Log entry + state NOW,
                            // before the first 5 s pump, so the next run cannot be ambiguous
                            // about whether the loop was entered.
                            watchdog_marker("LOOP-ENTERED");
                            log_line("BOOT-LOOP-ENTERED watch=1 result=%d closed=%d state=%d", bout.result_code, m_closed ? 1 : 0, emu_host::boot_emu_state());
                            watchdog_marker("STATE-ENTRY");
                            { int st0 = emu_host::boot_emu_state(); const char* st0Name = st0 == 0 ? "stopped" : st0 == 1 ? "loading" : st0 == 3 ? "running" : st0 == 4 ? "paused" : st0 == 6 ? "ready" : st0 == 7 ? "starting" : "unknown"; log_line("BOOT-STATE state=%d(%s) alive=%d (entry, before first watch tick)", st0, st0Name, st0 != 0 ? 1 : 0); }
                            watchdog_marker("ACTIVATION");
                            log_line("BOOT-LOOP-ACTIVATION visible=%d closed=%d", m_window->Visible ? 1 : 0, m_closed ? 1 : 0);
                            // 1.0.33 gap close: 1.0.32 died between BOOT-LOOP-ACTIVATION and the
                            // first BOOT-TICK-PRE with no handler line. The statements in that gap
                            // (below) are scalar inits + loop headers + Emu.GetStatus()/atomic
                            // counter reads already proven at lines 665-666, so the gap itself
                            // contains no plausible thrower â€” but it was unlogged, so prove
                            // every step of iteration 0 instead of assuming it.
                            watchdog_marker("BODY-ENTERED");
                            log_line("BOOT-LOOP-BODY-ENTERED watch=1 (loop prologue starts; gap statements follow)");
                            // 60 s watch, 5 s cadence, dispatcher pumped
                            // in 100 ms slices so window-close and
                            // call_from_main_thread still deliver.
                            // BOOT-MEMORY every 5 s vs the 5.00 GiB
                            // limit and the 3.0 GiB abort line; Step 8
                            // caps are NOT changed in this build.
                            // 1.0.30 watch rewrite (the 1.0.29 exit fix, part 1):
                            // boot_is_running() is the WRONG liveness test
                            // (BootGame returns with state=starting; running
                            // arrives via the RSX thread). Use
                            // boot_emu_state(): stopped=0, loading=1,
                            // running=3, paused=4, ready=6, starting=7.
                            // Everything except stopped counts as resident.
                            // A stopped emulation logs FAIL but does NOT
                            // exit the app â€” the sustained loop follows
                            // in all cases (1.0.29 died silently here:
                            // log_line flushes every line, so the
                            // missing watch lines were never written â€”
                            // the process ended before the first tick).
                            bool watchRunning = true;
                            unsigned long long peakMem = 0;
                            unsigned long long lastLoggedMiss = 0;
                            bool shaderCacheLogged = false, ownsLogged = false, budgetLogged = false;
                            unsigned long long lastRtt = 0, lastTex = 0, lastUp = 0, lastPso = 0;
                            watchdog_marker("GAP-1");
                            log_line("BOOT-LOOP-GAP-1 decls-done (watchRunning/peakMem/miss/budget locals initialised)");
                            watchdog_marker("W-HEADER");
                            log_line("BOOT-LOOP-STMT w-loop-header-about-to-run");
                            bool watchAborted = false;
                            try {
                            for (int w = 0; w < 12 && !m_closed && watchRunning; ++w) {
                                watchdog_marker("W0");
                                if (w == 0) log_line("BOOT-LOOP-STMT w0-entered");
                                for (int s = 0; s < 50 && !m_closed; ++s) {
                                    watchdog_marker("S0");
                                    if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-entered");
                                    watchdog_marker("S0-READ");
                                    { if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-state-read-about-to-run");
                                      int pst = emu_host::boot_emu_state(); unsigned long long pci = d3d12_rsx_init_count(), pcf = d3d12_rsx_flip_count();
                                      watchdog_marker("S0-DONE");
                                      if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-state-and-counters-read-done state=%d rsx_init=%llu flips=%llu", pst, pci, pcf);
                                      watchdog_marker("TICK-PRE");
                                      log_line("BOOT-TICK-PRE w=%d s=%d state=%d rsx_init=%llu flips=%llu visible=%d closed=%d", w, s, pst, pci, pcf, m_window->Visible ? 1 : 0, m_closed ? 1 : 0); }
                                    watchdog_marker("PUMP-ENTERED");
                                    if (w == 0 && s == 0) log_line("BOOT-PUMP-ENTERED w=0 s=0 (first ProcessEvents about to run)");
                                    drain_callbacks();
                                    try {
                                        m_window->Dispatcher->ProcessEvents(Windows::UI::Core::CoreProcessEventsOption::ProcessAllIfPresent);
                                    } catch (Platform::Exception^ ex) {
                                        log_line("BOOT-LOOP-PUMP-EXCEPTION winrt hr=0x%08lX (pump continues, app stays resident)", (unsigned long)ex->HResult);
                                    } catch (...) {
                                        log_line("BOOT-LOOP-PUMP-EXCEPTION unknown (pump continues, app stays resident)");
                                    }
                                    if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-pump-done");
                                    if (m_closed) break;
                                    { int pst = emu_host::boot_emu_state(); unsigned long long pci = d3d12_rsx_init_count(), pcf = d3d12_rsx_flip_count();
                                      watchdog_marker("TICK-POST");
                                      log_line("BOOT-TICK-POST w=%d s=%d state=%d rsx_init=%llu flips=%llu visible=%d closed=%d", w, s, pst, pci, pcf, m_window->Visible ? 1 : 0, m_closed ? 1 : 0); }
                                    if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-post-done");
                                    Sleep(100);
                                    if (w == 0 && s == 0) log_line("BOOT-LOOP-STMT s0-sleep-done (iteration 0 slice complete)");
                                }
                                if (m_closed) break;
                                unsigned long long mu = (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage;
                                if (mu > peakMem) peakMem = mu;
                                int st = emu_host::boot_emu_state();
                                const char* stName = st == 0 ? "stopped" : st == 1 ? "loading" : st == 3 ? "running" : st == 4 ? "paused" : st == 6 ? "ready" : st == 7 ? "starting" : "unknown";
                                bool alive = (st != 0);
                                log_line("BOOT-MEMORY AppMemoryUsage=%llu AppMemoryUsageLimit=%llu (3.0 GiB abort line=3221225472) milestone=watch+%ds state=%d(%s) alive=%d",
                                    mu, (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit, (w + 1) * 5, st, stName, alive ? 1 : 0);
                                {
                                    unsigned long long ci = d3d12_rsx_init_count();
                                    unsigned long long cb = d3d12_rsx_begin_count();
                                    unsigned long long ce = d3d12_rsx_end_count();
                                    unsigned long long cf = d3d12_rsx_flip_count();
                                    unsigned long long po = host_pad_wgi_polls();
                                    unsigned long long pc = host_pad_wgi_connected();
                                    unsigned long long pb = host_pad_wgi_buttons();
                                    unsigned long long cr = d3d12_rsx_real_draws();
                                    unsigned long long cs = d3d12_rsx_real_skips();
                                    static unsigned long long lastInit = 0, lastBegin = 0, lastEnd = 0, lastFlip = 0, lastPolls = 0, lastConn = 0, lastBtns = 0, lastReal = 0, lastSkips = 0;
                                    if (ci != lastInit || cb != lastBegin || ce != lastEnd || cf != lastFlip || po != lastPolls || pc != lastConn || pb != lastBtns || cr != lastReal || cs != lastSkips) {
                                        log_line("BOOT-RSX init=%llu begins=%llu draws(nop)=%llu draws(real)=%llu skips=%llu flips=%llu | BOOT-PAD-WGI polls=%llu connected=%llu buttons=0x%llX",
                                            ci, cb, ce, cr, cs, cf, po, pc, pb);
                                        lastInit = ci; lastBegin = cb; lastEnd = ce; lastFlip = cf; lastPolls = po; lastConn = pc; lastBtns = pb; lastReal = cr; lastSkips = cs;
                                    }
                                    // 1.0.36: the gate is the bridge atomic (set by
                                    // D3D12GSRender::flip on the first VALID blitted
                                    // flip), not the flip count.
                                    if (d3d12_bridge_owns() && !ownsLogged) {
                                        log_line("BOOT-RSX-OWNS-SWAPCHAIN: bridge owns flag set (first valid RSX flip blitted); host Step6 present loop stops from here (same queue, no cross-queue fences; host frames are NOT game frames)");
                                        ownsLogged = true;
                                    }
                                    if (!shaderCacheLogged) {
                                        log_line("BOOT-SHADER-CACHE path=%s entries=%llu distinct=%llu hits=%llu misses=%llu",
                                            d3d12_rsx_shader_cache_path().c_str(), d3d12_rsx_shader_cache_entries(),
                                            d3d12_rsx_shader_distinct(), d3d12_rsx_shader_hits(), d3d12_rsx_shader_misses());
                                        shaderCacheLogged = true;
                                    }
                                    unsigned long long totalMiss = d3d12_rsx_shader_misses();
                                    if (totalMiss > lastLoggedMiss) {
                                        if (totalMiss - lastLoggedMiss > 64) {
                                            log_line("BOOT-SHADER-MISS-OVERFLOW total=%llu logged=%llu (ring holds last 64; oldest misses lost â€” drain cadence too slow)", totalMiss, lastLoggedMiss);
                                            lastLoggedMiss = totalMiss > 64 ? totalMiss - 64 : 0;
                                        }
                                        while (lastLoggedMiss < totalMiss) {
                                            unsigned long long vp = 0, fp = 0;
                                            if (!d3d12_rsx_shader_miss_at(lastLoggedMiss, &vp, &fp)) break;
                                            log_line("BOOT-SHADER-MISS idx=%llu vp=0x%llX fp=0x%llX distinct=%llu (PC bake input: hash->hashed-DXBC blob; console never compiles)",
                                                lastLoggedMiss, vp, fp, d3d12_rsx_shader_distinct());
                                            ++lastLoggedMiss;
                                        }
                                    }
                                    unsigned long long rt = d3d12_rsx_rtt_bytes(), tx = d3d12_rsx_texture_bytes(), up = d3d12_rsx_upload_bytes(), ps = d3d12_rsx_pso_bytes();
                                    if (!budgetLogged || rt != lastRtt || tx != lastTex || up != lastUp || ps != lastPso) {
                                        log_line("BOOT-BUDGET rtt=%llu/536870912 tex=%llu/805306368 upload=%llu/134217728 pso_ram=%llu/67108864 gov_cap_texture=1258291200 gov_cap_shader_soft=134217728 gov_cap_shader_hard=268435456 gov_cap_upload=268435456 gov_cap_guest=536870912 abort_line=3221225472",
                                            rt, tx, up, ps);
                                        budgetLogged = true; lastRtt = rt; lastTex = tx; lastUp = up; lastPso = ps;
                                    }
                                }
                                if (!alive) {
                                    log_line("BOOT-VERDICT: FAIL (emulation stopped during +60s watch; app STAYS RESIDENT â€” sustained loop follows, no exit)");
                                    watchRunning = false;
                                }
                            }
                            } catch (Platform::Exception^ ex) {
                                log_line("BOOT-LOOP-BODY-EXCEPTION winrt hr=0x%08lX (watch aborted, app STAYS RESIDENT â€” sustained loop follows)", (unsigned long)ex->HResult);
                                watchAborted = true;
                            } catch (...) {
                                log_line("BOOT-LOOP-BODY-EXCEPTION std (watch aborted, app STAYS RESIDENT â€” sustained loop follows)");
                                watchAborted = true;
                            }
                            watchdog_marker("VERDICT");
                            if (watchAborted)
                                log_line("BOOT-VERDICT: ABORTED (watch loop threw; see BOOT-LOOP-BODY-EXCEPTION; app STAYS RESIDENT â€” sustained loop follows)");
                            else if (!m_closed && watchRunning)
                                log_line("BOOT-VERDICT: PASS (emulation resident at +60s watch, peak-AppMemoryUsage=%llu; sustained loop follows)", peakMem);
                            else if (m_closed)
                                log_line("BOOT-VERDICT: INCONCLUSIVE (window closed during watch)");
                            log_line("BOOT-LOOP-EXIT watch=1 closed=%d watchRunning=%d (sustained loop follows unless window closed)", m_closed ? 1 : 0, watchRunning ? 1 : 0);
                        }
                        // bout.attempted && result != 0: FAIL already
                        // logged by boot_phase (BOOT-RESULT line).
                        // bout.skipped: SKIPPED already logged there.
                    }
                } else {
                    log_line("EMU-FIRMWARE-PHASE-VERDICT: SKIPPED (EMU-INIT failed)");
                    log_line("BOOT-VERDICT: SKIPPED (EMU-INIT failed; boot never attempted)");
                }
            }
            // 1.0.30 sustained loop (the 1.0.29 exit fix, part 2):
            // This loop NEVER exits on emulation stop and NEVER exits
            // on device removal â€” the only exits are m_closed
            // (BackRequested/system termination). RSX-owns-swapchain
            // gate: once RSX flips start, the host stops presenting
            // its own Step 6 frames (they are NOT game frames, and
            // two presenters on one swapchain fight); it pumps the
            // dispatcher and logs counters instead. Before the first
            // flip, Step 6 frames keep the screen alive.
            bool drawBlock = true;
            bool deviceLostHold = false;
            unsigned long long sustTicks = 0;
            unsigned long long lastSustInit = 0, lastSustEnd = 0, lastSustFlip = 0, lastSustPolls = 0, lastSustConn = 0, lastSustBtns = 0, lastSustMiss = 0, lastSustReal = 0, lastSustSkips = 0;
            watchdog_marker("SUSTAINED");
            log_line("BOOT-LOOP-ENTERED sustained=1 closed=%d", m_closed ? 1 : 0);
            auto drain_callbacks_sust = [&]() {
                while (auto item = xbox_cb_pop()) {
                    try { item->f(); }
                    catch (Platform::Exception^ ex) { log_line("BOOT-CALLBACK-EXCEPTION winrt hr=0x%08lX (callback dropped, app stays resident)", (unsigned long)ex->HResult); }
                    catch (...) { log_line("BOOT-CALLBACK-EXCEPTION std (callback dropped, app stays resident)"); }
                    item->done.store(true);
                }
            };
            // 1.0.33 Run()-tail wrap: nothing in the sustained loop may escape Run()
            // unlogged either. On a throw, log and fall into a pump-only hold so the
            // app still stays resident instead of exiting.
            try {
            while (!m_closed) {
                drain_callbacks_sust();
                if (deviceLostHold) {
                    m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (m_closed) break;
                    Sleep(100);
                    continue;
                }
                if (FAILED(m_device->GetDeviceRemovedReason())) {
                    log_line("STEP6-SUSTAINED: BLOCKED device removed hr=0x%08lX â€” entering pump-only hold (app stays resident, no present attempts)", (unsigned long)m_device->GetDeviceRemovedReason());
                    deviceLostHold = true;
                    continue;
                }
                if (d3d12_bridge_owns()) {
                    // RSX owns the swapchain (bridge atomic, 1.0.36): pump + counters only.
                    m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (m_closed) break;
                    step7_poll();
                    Sleep(16);
                    ++sustTicks;
                    unsigned long long ci = d3d12_rsx_init_count(), ce = d3d12_rsx_end_count(), cf = d3d12_rsx_flip_count();
                    unsigned long long po = host_pad_wgi_polls(), pc = host_pad_wgi_connected(), pb = host_pad_wgi_buttons();
                    unsigned long long sm = d3d12_rsx_shader_misses();
                    unsigned long long sr = d3d12_rsx_real_draws(), ss = d3d12_rsx_real_skips();
                    if (ci != lastSustInit || ce != lastSustEnd || cf != lastSustFlip || po != lastSustPolls || pc != lastSustConn || pb != lastSustBtns || sm != lastSustMiss || sr != lastSustReal || ss != lastSustSkips) {
                        log_line("BOOT-RSX-SUSTAINED init=%llu draws(nop)=%llu draws(real)=%llu skips=%llu flips=%llu | BOOT-PAD-WGI polls=%llu connected=%llu buttons=0x%llX shader_misses=%llu",
                            ci, ce, sr, ss, cf, po, pc, pb, sm);
                        lastSustInit = ci; lastSustEnd = ce; lastSustFlip = cf; lastSustPolls = po; lastSustConn = pc; lastSustBtns = pb; lastSustMiss = sm; lastSustReal = sr; lastSustSkips = ss;
                    }
                    if (sustTicks % 3750 == 0) {
                        log_line("MEMORY-SUSTAINED AppMemoryUsage=%llu AppMemoryUsageLimit=%llu (RSX owns swapchain; host presenting stopped)",
                            mem(), (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit);
                    }
                    continue;
                }
                const unsigned long long target = drawBlock ? s6.draw_metrics().frames_attempted + 300
                                                             : s6.nullflip_metrics().frames_attempted + 300;
                while (!m_closed && (drawBlock ? s6.draw_metrics().frames_attempted : s6.nullflip_metrics().frames_attempted) < target) {
                    if (d3d12_bridge_owns()) break; // RSX took over mid-block: stop presenting now.
                    m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                    if (m_closed) break;
                    drain_callbacks_sust();
                    if (drawBlock) s6.render_draw_frame(); else s6.render_nullflip_frame();
                    step7_poll();
                    if (FAILED(m_device->GetDeviceRemovedReason())) break;
                }
                if (FAILED(m_device->GetDeviceRemovedReason())) {
                    log_line("STEP6-SUSTAINED: BLOCKED device removed hr=0x%08lX â€” entering pump-only hold (app stays resident, no present attempts)", (unsigned long)m_device->GetDeviceRemovedReason());
                    deviceLostHold = true;
                    continue;
                }
                log_line("%s", s6.draw_summary("DRAW-HEARTBEAT").c_str());
                log_line("%s", s6.nullflip_summary("NULLFLIP-HEARTBEAT").c_str());
                step7_heartbeat("STEP7-SUSTAINED");
                log_line("MEMORY AppMemoryUsage=%llu AppMemoryUsageLimit=%llu", mem(),
                    (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit);
                drawBlock = !drawBlock;
            }
            } catch (Platform::Exception^ ex) {
                log_line("BOOT-RUN-TAIL-EXCEPTION winrt hr=0x%08lX (sustained loop threw; entering pump-only hold, app STAYS RESIDENT)", (unsigned long)ex->HResult);
                while (!m_closed) { drain_callbacks_sust(); m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending); }
            } catch (...) {
                log_line("BOOT-RUN-TAIL-EXCEPTION std (sustained loop threw; entering pump-only hold, app STAYS RESIDENT)");
                while (!m_closed) { drain_callbacks_sust(); m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending); }
            }
            log_line("BOOT-LOOP-EXIT sustained=1 reason=window-closed (Run() will now return; this is the only legitimate resident-loop exit)");
        } else if (!m_closed) {
            log_line("STEP6-VERDICT: BLOCKED (draw init/PSO failed hr=0x%08lX - Step 6 stops at that gate; Step 4 bars continue sustained)", (unsigned long)s6psoHr);
            bool barsLostHold = false;
            while (!m_closed) {
                m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
                if (m_closed) break;
                if (barsLostHold) { Sleep(100); continue; }
                if (FAILED(m_device->GetDeviceRemovedReason())) {
                    log_line("STEP4-SUSTAINED: BLOCKED device removed hr=0x%08lX â€” entering pump-only hold (app stays resident)", (unsigned long)m_device->GetDeviceRemovedReason());
                    barsLostHold = true;
                    continue;
                }
                loop.render_frame();
                step7_poll();
                if (loop.metrics().frames_attempted % 300 == 0)
                    log_line("%s", loop.summary("PRESENT-HEARTBEAT").c_str());
            }
        }
        log_line("%s", loop.summary("PRESENT-FINAL").c_str());
        if (s6ok) { log_line("%s", s6.draw_summary("DRAW-FINAL").c_str()); log_line("%s", s6.nullflip_summary("NULLFLIP-FINAL").c_str()); }
        log_line("present loop ended (window closed)");
    }
    void hold_after_failure(const char* verdictLine) {
        if (verdictLine) log_line("%s", verdictLine);
        while (!m_closed) m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
    }
};
ref class AppSource sealed : Windows::ApplicationModel::Core::IFrameworkViewSource {
public: virtual IFrameworkView^ CreateView() { return ref new App(); }
};
} // namespace XboxUwpHost
[Platform::MTAThread]
int main(Platform::Array<Platform::String^>^ args) {
    (void)args; stage_write(2, "POST_CTORS_MAIN_ENTERED"); CoreApplication::Run(ref new XboxUwpHost::AppSource()); return 0;
}
