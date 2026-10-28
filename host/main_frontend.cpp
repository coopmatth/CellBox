// main_frontend.cpp - CellBox frontend line 1.1.0 (slice F1 + USB
// source + notice/trace progress). Derived from main_uwp_1044.cpp's
// proven shape - CoreWindow + IFrameworkView C++/CX, D3D12
// FLIP_DISCARD swapchain, log-to-LocalState per line, BackRequested
// Handled=true, callback-queue drain, RSX device bridge handoff,
// EMU-INIT + FIRMWARE phases, REASSEMBLE phase - with the 1.0.x
// diagnostic probe phases (PSO, Step 6 draw/null-flip, 60 s Step 7,
// Step 8 governor, INT synthetic, EMU-LINK wiring probe) REPLACED by
// the D3D12 game library. Each skipped phase logs one FRONTEND-SKIP
// line naming the 1.0.x line as its home; nothing is silently
// dropped. main_uwp_1044.cpp is NOT modified.
//
// Flow: bring-up -> library (LocalState/games + legacy root .iso +
// USB removable roots) -> A opens actions -> Load boots the selected
// entry through frontend_boot (the 1044 boot_probe BootGame path) ->
// loading screen fed by frontend_progress until the RSX backend owns
// the swapchain -> on emulation stop (or View+Menu hold) stop and
// return to the library. The app never exits on emulation stop.
#include "../xbox-emu-link/reassemble.h"
#include "emu_host.h"
#include "frontend/frontend_boot.h"
#include "frontend/frontend_compile.h"
#include "frontend/frontend_ui.h"
#include "frontend/frontend_input.h"
#include "frontend/frontend_library.h"
#include "frontend/frontend_progress.h"
#include "rsx_counters.h"
#include "watchdog.h"
#include "callback_queue.h"
#include "iat_hooks.h"
#include "..\xbox-core-lib\linktest\fatal_log.h"
#include "pad_wgi.h"
#include "stage_diag.h"
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
#include <vector>
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
    size_t i = 0;
    while (i < s.size()) { size_t j = s.find("\r\n", i); if (j == std::string::npos) j = s.size();
        log_line("%s", s.substr(i, j - i).c_str()); i = j + 2; if (j == s.size()) break; }
}
static std::string utf8_from_wide_main(const std::wstring& s) {
    std::string out;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize(static_cast<size_t>(n));
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0') out.pop_back();
    for (char& c : out) if (c == '\\') c = '/';
    return out;
}

// FRONTEND (1.1.3): log the core's disc-mount broadcasts captured
// by frontend_progress during the Load that just returned (VFS
// "Mounted path" lines for /dev_bdvd + /app_home, SYS "Disc:" /
// "Elf path:"). An empty capture is logged as such - the mount
// table is evidence from the core's own log channel, never
// synthesised host-side.
static void dump_mount_lines(const char* context) {
    const std::vector<std::string> mounts = frontend::progress_mount_lines();
    if (mounts.empty()) {
        log_line("FRONTEND-BOOT-MOUNT none-captured context=%s (no VFS mount lines were broadcast on the log channel during Load)", context);
        return;
    }
    for (const std::string& m : mounts)
        log_line("FRONTEND-BOOT-MOUNT context=%s %s", context, m.c_str());
}

namespace XboxUwpHost {
ref class App sealed : public Windows::ApplicationModel::Core::IFrameworkView {
public:
    virtual void Initialize(CoreApplicationView^ v) { (void)v; }
    virtual void SetWindow(CoreWindow^ w) {
        m_window = w;
        m_window->Closed += ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &App::OnClosed);
        try {
            auto nav = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();
            nav->BackRequested += ref new Windows::Foundation::EventHandler<Windows::UI::Core::BackRequestedEventArgs^>(this, &App::OnBackRequested);
            m_backRegistered = true;
        } catch (Platform::Exception^) { m_backRegistered = false; }
    }
    void OnBackRequested(Platform::Object^ sender, Windows::UI::Core::BackRequestedEventArgs^ args) {
        (void)sender;
        args->Handled = true;
        // Menu B is consumed by frontend_input (WGI), game B by the
        // pad thread; the OS back action must never suspend the app
        // (console-proven 1.0.6 INT-BACK verdict).
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
        log_line("CellBox frontend 1.1.3 (F1 library + F4 compile/pre-bake + F2 delete + boot-target/mount logging + USB source + notice/trace progress; D3D12 UI, NOT XAML)");
        log_line("AppMemoryUsageLimit=%llu bytes (%.2f GiB) ModeGuess=%s", limit, limit / 1073741824.0,
            limit > (unsigned long long(3) << 30) ? "GAME" : "APP");
        {
            auto pkg = Windows::ApplicationModel::Package::Current;
            auto ver = pkg->Id->Version;
            log_line("FRONTEND-TIER pkg=%S version=%u.%u.%u.%u limit=%llu level=%d backRegistered=%d",
                pkg->Id->FamilyName->Data(), (unsigned)ver.Major, (unsigned)ver.Minor,
                (unsigned)ver.Build, (unsigned)ver.Revision,
                limit, (int)Windows::System::MemoryManager::AppMemoryUsageLevel, m_backRegistered ? 1 : 0);
        }
        stage_write(3, "RUN_HEADER");
        log_line("STAGE:3:RUN_HEADER");
        { auto iat_log = [](const char* s) { log_line("%s", s); }; iat_sweep(iat_log); }
        log_line("FRONTEND-SKIP PSO/Step6-draw/Step7-tone-60s/Step8-governor/INT-synthetic/EMU-LINK-probe phases: diagnostic line 1.0.x only (main_uwp_1044); frontend goes bring-up -> EMU-INIT/FIRMWARE -> library");
        log_line("--- D3D12 device + queue (headless bring-up) ---");
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
        log_line("D3D12CreateDevice FL11_0: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
        bool deviceOk = SUCCEEDED(hr);
        if (deviceOk) {
            D3D12_FEATURE_DATA_FEATURE_LEVELS fl{};
            const D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
            fl.NumFeatureLevels = 5; fl.pFeatureLevelsRequested = lv;
            if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof(fl))))
                log_line("MaxFeatureLevel=0x%04X", (unsigned)fl.MaxSupportedFeatureLevel);
            D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            hr = m_device->CreateCommandQueue(&q, IID_PPV_ARGS(&m_queue));
            log_line("CreateCommandQueue DIRECT: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
            deviceOk = SUCCEEDED(hr);
        }
        if (!deviceOk) {
            log_line("FRONTEND-VERDICT: BLOCKED (no device/queue - library not attempted, no present claimed)");
            while (!m_closed) m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
            return;
        }
        run_frontend();
    }
private:
    CoreWindow^ m_window = nullptr; bool m_closed = false; bool m_backRegistered = false;
    ComPtr<ID3D12Device> m_device; ComPtr<ID3D12CommandQueue> m_queue;

    void OnClosed(CoreWindow^ s, CoreWindowEventArgs^ a) { (void)s; (void)a; m_closed = true; }
    template <typename TOp> bool wait_op(TOp^ op) {
        for (int i = 0; i < 100; ++i) {
            if (op->Status != Windows::Foundation::AsyncStatus::Started) return true;
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            Sleep(50);
        }
        return op->Status != Windows::Foundation::AsyncStatus::Started;
    }
    // Removable-storage roots via the broker (removableStorage
    // capability in AppxManifest.xml). The returned paths are handed
    // to the native scanner; raw path access under them is the
    // UNVERIFIED-on-console part and logs its own err values.
    std::vector<std::string> collect_usb_roots() {
        std::vector<std::string> roots;
        try {
            auto op = Windows::Storage::KnownFolders::RemovableDevices->GetFoldersAsync();
            if (!wait_op(op)) { log_line("FRONTEND-USB-ROOTS: TIMEOUT"); return roots; }
            if (op->Status != Windows::Foundation::AsyncStatus::Completed) {
                log_line("FRONTEND-USB-ROOTS: FAIL status=%d hr=0x%08lX", (int)op->Status, (unsigned long)op->ErrorCode.Value);
                return roots;
            }
            auto folders = op->GetResults();
            log_line("FRONTEND-USB-ROOTS: removable folders=%u", folders->Size);
            for (unsigned i = 0; i < folders->Size; ++i) {
                auto f = folders->GetAt(i);
                std::wstring p(f->Path->Data(), f->Path->Length());
                std::string u8 = utf8_from_wide_main(p);
                log_line("FRONTEND-USB-ROOT[%u] path=%s", i, u8.c_str());
                if (!u8.empty()) roots.push_back(u8);
            }
        } catch (Platform::Exception^ ex) {
            log_line("FRONTEND-USB-ROOTS: EXCEPTION hr=0x%08lX", (unsigned long)ex->HResult);
        }
        return roots;
    }
    void run_frontend();
    void game_loop(const frontend::GameEntry& entry,
                   const std::function<void()>& drain_callbacks,
                   frontend::FrontendUi& ui, frontend::MenuPad& pad);
    // F4: Compile (pre-bake) for the selected entry. Returns the
    // one-line status shown in the library footer afterwards.
    std::string compile_loop(const frontend::GameEntry& entry,
                             const std::function<void()>& drain_callbacks,
                             frontend::FrontendUi& ui, frontend::MenuPad& pad,
                             const emu_host::boot_callbacks& bcb,
                             const std::string& lsdir);
    // FRONTEND (1.1.2): teardown wait between a game/compile loop and
    // the return to the library - the library does NOT rescan or
    // present again until the core's Emulation Join Thread has
    // finished (on_stop delivered). See the definition for the full
    // mechanism note.
    void wait_teardown(const char* reason, const frontend::GameEntry& entry,
                       const std::function<void()>& drain_callbacks,
                       frontend::FrontendUi& ui, frontend::UiState& uist);
    void hold_after_failure(const char* verdictLine) {
        if (verdictLine) log_line("%s", verdictLine);
        while (!m_closed) m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
    }
};

// ---- library <-> game outer loop (see file header for the flow) ----
void App::run_frontend() {
    auto hostLog = [](const std::string& l) { log_line("%s", l.c_str()); };
    // Swapchain (1044-proven CoreWindow FLIP_DISCARD shape).
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    log_line("CreateDXGIFactory2: %s hr=0x%08lX", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr);
    if (FAILED(hr)) { hold_after_failure("FRONTEND-VERDICT: BLOCKED (DXGI factory failed)"); return; }
    auto b = m_window->Bounds;
    const bool boundsUsable = b.Width >= 16 && b.Height >= 16;
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = boundsUsable ? (UINT)b.Width : 1280;
    sd.Height = boundsUsable ? (UINT)b.Height : 720;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.SampleDesc.Count = 1;
    ComPtr<IDXGISwapChain1> s1;
    hr = factory->CreateSwapChainForCoreWindow(m_queue.Get(), reinterpret_cast<IUnknown*>(m_window), &sd, nullptr, &s1);
    log_line("CreateSwapChainForCoreWindow: %s hr=0x%08lX (%ux%u%s)", SUCCEEDED(hr) ? "PASS" : "FAIL", (unsigned long)hr,
        sd.Width, sd.Height, boundsUsable ? "" : " FALLBACK size");
    if (FAILED(hr)) { hold_after_failure("FRONTEND-VERDICT: BLOCKED (swapchain creation failed)"); return; }
    frontend::FrontendUi ui;
    bool uiOk = false;
    log_block(ui.init(m_device.Get(), m_queue.Get(), s1.Get(), &uiOk));
    if (!uiOk) { hold_after_failure("FRONTEND-VERDICT: BLOCKED (D3D12 UI init failed - see FRONTEND-UI lines)"); return; }
    frontend::MenuPad pad;
    pad.init(hostLog);
    // LocalState dir (UTF-8, '/' separators, no trailing slash).
    std::string lsdir;
    if (!g_results_path.empty()) {
        size_t slashPos = g_results_path.find_last_of(L"\\/");
        if (slashPos != std::wstring::npos)
            lsdir = utf8_from_wide_main(g_results_path.substr(0, slashPos));
    }
    log_line("FRONTEND-LOCALSTATE dir=%s", lsdir.c_str());
    // REASSEMBLE (kept from 1044): Portal ISO parts -> full ISO in
    // LocalState root, which the library then lists as a legacy entry.
    {
        uint64_t outBytes = 0; std::string fail;
        // Staging constants live in frontend_library.h (1.1.3) so
        // Delete of the legacy entry removes exactly this set.
        bool ok = reassemble::reassemble(lsdir, frontend::kStagingPartBase,
            frontend::kStagingPartCount, frontend::kStagingIsoName,
            frontend::kStagingIsoBytes, outBytes, fail);
        log_line("REASSEMBLE-RESULT: %s bytes=%llu expected=4599644160 fail=%s", ok ? "PASS" : "SKIPPED-or-FAIL", outBytes, fail.c_str());
        if (!lsdir.empty() && !ok && fail.find("missing part") == std::string::npos)
            log_line("REASSEMBLE-VERDICT: FAIL (parts present but size mismatch; parts kept for retry)");
        else if (ok)
            log_line("REASSEMBLE-VERDICT: PASS (full ISO present in LocalState)");
        else
            log_line("REASSEMBLE-VERDICT: SKIPPED (parts not all present yet)");
    }
    // EMU-INIT + FIRMWARE + RSX bridge (1044 order and machinery).
    watchdog_start(lsdir);
    {
        std::wstring wls = std::wstring(lsdir.begin(), lsdir.end());
        for (auto& c : wls) if (c == L'/') c = L'\\';
        xbox_fatal_set_dir(wls.c_str());
        xbox_hooks_install(wls);
    }
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
    // 1.0.35 callback queue: WinRT never sees emulator callbacks;
    // drained on this (main) thread in every loop below.
    auto hostPump = [&](std::function<void()> f) { xbox_cb_enqueue_and_wait(f, 2000); };
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
    frontend::progress_install(hostLog); // counts firmware-phase notices too
    const bool initOk = emu_host::init_phase(lsdir, hostLog, hostPump);
    log_line("EMU-INIT-VERDICT: %s (real Emulator object in host; library follows%s)", initOk ? "PASS" : "FAIL",
        initOk ? "" : " - boots will fail until this passes");
    if (initOk) {
        const bool fwOk = emu_host::firmware_phase(lsdir, hostLog);
        log_line("EMU-FIRMWARE-PHASE-VERDICT: %s", fwOk ? "PASS" : "FAIL-or-SKIPPED (see FIRMWARE lines above)");
    } else {
        log_line("EMU-FIRMWARE-PHASE-VERDICT: SKIPPED (EMU-INIT failed)");
    }
    // ---- library <-> game loop ----
    frontend::UiState uist;
    std::string statusLine = initOk ? "" : "EMU-INIT FAILED - Load will fail; see host-results.txt";
    unsigned bootCount = 0;
    try {
    while (!m_closed) {
        // (Re)scan: LocalState + every brokered USB root.
        std::vector<frontend::GameEntry> entries = frontend::scan_localstate(lsdir, hostLog);
        for (const std::string& root : collect_usb_roots()) {
            std::vector<frontend::GameEntry> usb = frontend::scan_usb_root(root, lsdir, hostLog);
            entries.insert(entries.end(), usb.begin(), usb.end());
        }
        log_line("FRONTEND-LIB-TOTAL entries=%u (LocalState+USB)", (unsigned)entries.size());
        if (uist.selected >= (int)entries.size()) uist.selected = entries.empty() ? 0 : (int)entries.size() - 1;
        uist.screen = frontend::Screen::Library;
        pad.sync_to_current(); // handoff guard: no leaked edges into the menu
        bool doRescan = false;
        bool doBoot = false;
        bool doCompile = false;
        bool doDelete = false; // F2 (1.1.3)
        int detailsSel = -1; // F4: Actions details computed once per selection
        while (!m_closed && !doRescan && !doBoot && !doCompile && !doDelete) {
            drain_callbacks();
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            if (m_closed) break;
            if (FAILED(m_device->GetDeviceRemovedReason())) {
                log_line("FRONTEND-DEVICE-LOST hr=0x%08lX - pump-only hold (app stays resident)", (unsigned long)m_device->GetDeviceRemovedReason());
                hold_after_failure(nullptr);
                return;
            }
            frontend::MenuInput in = pad.poll();
            frontend::UiEvent ev = frontend::ui_apply_input(uist, in, (int)entries.size());
            if (ev == frontend::UiEvent::RescanRequested) {
                log_line("FRONTEND-RESCAN requested from library (Menu)");
                doRescan = true;
                break;
            }
            if (ev == frontend::UiEvent::LoadSelected) { doBoot = true; break; }
            if (ev == frontend::UiEvent::CompileSelected) { doCompile = true; break; }
            if (ev == frontend::UiEvent::DeleteSelected && !entries.empty()) {
                // F2 (1.1.3): the confirm screen is up (state machine).
                // USB is a read-only source: bounce straight back to
                // Actions with the explanation as the status line.
                const frontend::GameEntry& de = entries[(size_t)uist.selected];
                if (de.source == frontend::GameSource::Usb) {
                    uist.screen = frontend::Screen::Actions;
                    uist.status = "USB games can't be deleted from here";
                    log_line("FRONTEND-DELETE refused reason=usb-read-only-source title=%s path=%s (explained on the Actions screen)",
                        de.title.c_str(), de.path.c_str());
                } else {
                    log_line("FRONTEND-DELETE confirm-open title=%s size=%llu path=%s (nothing deleted yet - A on the confirm screen deletes)",
                        de.title.c_str(), (unsigned long long)de.size_bytes, de.path.c_str());
                }
            }
            if (ev == frontend::UiEvent::DeleteConfirmed) { doDelete = true; break; }
            if (uist.screen == frontend::Screen::DeleteConfirm && !entries.empty())
                frontend::ui_render_delete_confirm(ui, uist, entries[(size_t)uist.selected]);
            else if (uist.screen == frontend::Screen::Actions && !entries.empty()) {
                if (detailsSel != uist.selected) {
                    // F4 compile details for the selected entry:
                    // enumerated modules + PPU cache count + one
                    // honest shader-cache line (backend counters when
                    // a game has run this session, file stat before).
                    detailsSel = uist.selected;
                    const frontend::GameEntry& de = entries[(size_t)uist.selected];
                    uist.detail_modules = -1;
                    uist.detail_ppu_files = -1;
                    uist.detail_ppu_bytes = 0;
                    uist.detail_shader.clear();
                    if (de.kind == frontend::GameKind::Folder) {
                        frontend::ModuleEnum men = frontend::enumerate_modules(de, hostLog);
                        if (men.ok) uist.detail_modules = (int)men.modules.size();
                    }
                    frontend::CacheStats cs = frontend::ppu_cache_stats(lsdir, de.serial, hostLog);
                    uist.detail_ppu_files = (int)cs.files;
                    uist.detail_ppu_bytes = cs.bytes;
                    if (d3d12_rsx_shader_distinct() > 0) {
                        uist.detail_shader =
                            "Shaders this session: " + std::to_string(d3d12_rsx_shader_distinct()) +
                            " requested, " + std::to_string(d3d12_rsx_shader_hits()) + " from cache, " +
                            std::to_string(d3d12_rsx_shader_misses()) + " not cached (need PC bake)";
                    } else {
                        frontend::ShaderCacheFile scf = frontend::shader_cache_file(lsdir);
                        uist.detail_shader = scf.present
                            ? "Shader cache file: present, " + std::to_string(scf.bytes) +
                              " B (entries counted when a game loads)"
                            : "Shader cache: none yet - built as games run; full bake needs a PC capture (not F4)";
                    }
                }
                frontend::ui_render_actions(ui, uist, entries[(size_t)uist.selected]);
            }
            else
                frontend::ui_render_library(ui, uist, entries, statusLine);
            HRESULT phr = ui.present_frame();
            if (FAILED(phr)) {
                log_line("FRONTEND-PRESENT-FAIL hr=0x%08lX deviceRemovedReason=0x%08lX", (unsigned long)phr, (unsigned long)m_device->GetDeviceRemovedReason());
                if (FAILED(m_device->GetDeviceRemovedReason())) { hold_after_failure(nullptr); return; }
                Sleep(100);
            }
        }
        if (m_closed) continue;
        if (doDelete) {
            // ---- F2 (1.1.3): delete the confirmed entry ----
            const frontend::GameEntry entry = entries[(size_t)uist.selected];
            uist.screen = frontend::Screen::Library;
            pad.sync_to_current(); // the confirm A press must not leak
            frontend::DeleteResult dres = frontend::delete_entry(entry, lsdir, hostLog);
            if (dres.ok)
                statusLine = "Deleted: " + entry.title + " (" +
                    std::to_string(dres.files_removed) + " files, cache kept)";
            else if (dres.refused)
                statusLine = "Delete refused - see host-results.txt";
            else
                statusLine = "Delete INCOMPLETE (" + entry.title + ") - see host-results.txt";
            log_line("FRONTEND-RETURN-TO-LIBRARY (rescan follows delete)");
            continue;
        }
        if (doCompile) {
            // ---- F4: compile (pre-bake) the selected entry ----
            const frontend::GameEntry entry = entries[(size_t)uist.selected];
            log_line("FRONTEND-COMPILE-SELECTED source=%s serial=%s title=%s path=%s",
                frontend::game_source_name(entry.source), entry.serial.c_str(), entry.title.c_str(), entry.path.c_str());
            uist.screen = frontend::Screen::Compiling;
            pad.sync_to_current(); // menu presses must not leak into the pass
            emu_host::boot_callbacks bcb;
            bcb.log = hostLog;
            bcb.pump = hostPump;
            bcb.mem_usage = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage; };
            bcb.mem_limit = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit; };
            statusLine = compile_loop(entry, drain_callbacks, ui, pad, bcb, lsdir);
            wait_teardown("compile", entry, drain_callbacks, ui, uist); // 1.1.2: library re-entry gated on core teardown
            log_line("FRONTEND-RETURN-TO-LIBRARY owns=%d state=%d frames_presented=%llu (rescan follows compile)",
                d3d12_bridge_owns() ? 1 : 0, emu_host::boot_emu_state(), ui.frames_presented());
            continue;
        }
        if (!doBoot) continue;
        // ---- boot the selected entry ----
        const frontend::GameEntry entry = entries[(size_t)uist.selected];
        ++bootCount;
        log_line("FRONTEND-BOOT-SELECTED count=%u source=%s serial=%s title=%s path=%s",
            bootCount, frontend::game_source_name(entry.source), entry.serial.c_str(), entry.title.c_str(), entry.path.c_str());
        uist.screen = frontend::Screen::Loading;
        {
            frontend::ProgressSnapshot snap;
            frontend::progress_snapshot(snap);
            frontend::ui_render_loading(ui, entry.title, snap, 0);
            ui.present_frame(); // one Loading frame up before the synchronous Load
        }
        pad.sync_to_current(); // menu presses must not leak into the game
        emu_host::boot_callbacks bcb;
        bcb.log = hostLog;
        bcb.pump = hostPump;
        bcb.mem_usage = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage; };
        bcb.mem_limit = []() { return (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit; };
        // FRONTEND (1.1.3): resolve (name) the boot-target form. The
        // desktop experiment proved the core normalises every folder
        // form to the disc root, so the path is passed unchanged; the
        // form rides along for the FRONTEND-BOOT-TARGET log line.
        const frontend::BootTarget bt = frontend::resolve_boot_target(entry);
        emu_host::boot_outcome bout = emu_host::boot_phase_for_path(
            lsdir, bt.path, frontend::game_source_name(entry.source), bcb, bt.form);
        watchdog_boot_returned();
        xbox_fatal_set_boot_ms(GetTickCount64());
        drain_callbacks();
        dump_mount_lines("game boot");
        if (bout.attempted && bout.result_code == 0) {
            game_loop(entry, drain_callbacks, ui, pad);
            statusLine = "Returned from: " + entry.title;
        } else if (bout.skipped) {
            statusLine = "Boot skipped: target missing (see log)";
            log_line("FRONTEND-BOOT-SKIPPED path=%s", entry.path.c_str());
        } else {
            statusLine = "Boot failed (code " + std::to_string(bout.result_code) + ") - see host-results.txt";
            log_line("FRONTEND-BOOT-FAILED code=%d path=%s", bout.result_code, entry.path.c_str());
        }
        wait_teardown("game", entry, drain_callbacks, ui, uist); // 1.1.2: library re-entry gated on core teardown
        log_line("FRONTEND-RETURN-TO-LIBRARY owns=%d state=%d frames_presented=%llu (rescan follows)",
            d3d12_bridge_owns() ? 1 : 0, emu_host::boot_emu_state(), ui.frames_presented());
    }
    } catch (Platform::Exception^ ex) {
        log_line("FRONTEND-RUN-TAIL-EXCEPTION winrt hr=0x%08lX (pump-only hold, app STAYS RESIDENT)", (unsigned long)ex->HResult);
        while (!m_closed) { drain_callbacks(); m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending); }
    } catch (...) {
        log_line("FRONTEND-RUN-TAIL-EXCEPTION std (pump-only hold, app STAYS RESIDENT)");
        while (!m_closed) { drain_callbacks(); m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending); }
    }
    log_line("FRONTEND-LOOP-EXIT reason=window-closed (Run() returns; the only legitimate exit)");
}
// FRONTEND (1.1.2): teardown wait. In 1.1.1 the frontend treated
// boot_emu_state()==0 as "stopped" and resumed library presenting
// (rescan + present) immediately - but Emu.Kill only SIGNALS the
// emulation threads; the actual joining runs afterwards on the
// core's Emulation Join Thread (measured: "Emulation Join Thread is
// too sleepy" waits doubling out to 7.2 s after state had already
// read 0), thread objects are destroyed during that join, and
// g_emu_callbacks.on_stop fires only at the END of the join lambda
// (System.cpp: reset_emu_state, m_state=stopped, then on_stop) -
// delivered to the host through the CallFromMainThread queue, so
// this loop MUST keep draining it. The 1.1.1 console crash itself
// was an unhandled AV in the host watchdog's PPU thread summary
// enumerating those half-destroyed objects (see watchdog.cpp gate);
// this wait removes the sibling race on the frontend side: no
// library presenting, no rescan, until teardown is proven complete.
// Completion signal: emu_host::stop_on_stop_seen() (strict when the
// boot returned no_errors). A boot that never reached running
// (result != no_errors) gets a quiescence fallback after 10 s at
// state=0 with frozen flips - no emulation threads exist to join in
// that case. The Stopping screen is presented only once the RSX
// flip counter has frozen for 500 ms (the RSX thread no longer
// presents) and the bridge owns flag is clear - before that, two
// presenters on one swapchain would fight, the exact race this wait
// exists to remove.
void App::wait_teardown(const char* reason, const frontend::GameEntry& entry,
                        const std::function<void()>& drain_callbacks,
                        frontend::FrontendUi& ui, frontend::UiState& uist) {
    if (!emu_host::stop_cycle_open()) {
        log_line("FRONTEND-STOP-JOIN skipped reason=%s (no boot cycle opened - nothing was booted, nothing to tear down)", reason);
        return;
    }
    // Natural stops (guest exit, folder-scan self-Kill) never pass
    // through boot_request_stop: latch the watchdog PPU-summary gate
    // here too, before anything else can enumerate core threads.
    watchdog_note_stop_requested();
    watchdog_marker("FRONTEND-STOPPING");
    const bool strict = emu_host::stop_cycle_strict();
    const unsigned long long t0 = GetTickCount64();
    log_line("FRONTEND-STOP-JOIN requested reason=%s strict=%d state=%d (library re-entry held: no rescan, no presenting until the core Emulation Join Thread completes; on_stop is the completion signal)",
        reason, strict ? 1 : 0, emu_host::boot_emu_state());
    uist.screen = frontend::Screen::Stopping;
    std::string stage;
    unsigned long long lastFlips = d3d12_rsx_flip_count();
    unsigned long long flipsChangedAt = t0;
    unsigned long long lastWaitingLog = t0;
    unsigned long long iter = 0;
    unsigned long long frameTick = 0;
    bool rsxExitedLogged = false;
    while (!m_closed) {
        drain_callbacks();
        m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        if (m_closed) break;
        watchdog_host_loop_mark(++iter, 9); // stage 9: teardown wait
        const unsigned long long now = GetTickCount64();
        const unsigned long long elapsed = now - t0;
        const int st = emu_host::boot_emu_state();
        const unsigned long long flips = d3d12_rsx_flip_count();
        if (flips != lastFlips) { lastFlips = flips; flipsChangedAt = now; }
        const bool flipsFrozen = (now - flipsChangedAt) >= 500;
        const bool onStop = emu_host::stop_on_stop_seen();
        const char* stageNow = (st != 0) ? "emu-stopping" : (!flipsFrozen ? "rsx-draining" : "awaiting-on-stop");
        if (stage != stageNow) {
            stage = stageNow;
            log_line("FRONTEND-STOP-JOIN stage=%s state=%d flips=%llu elapsed_ms=%llu", stage.c_str(), st, flips, elapsed);
        }
        if (flipsFrozen && st == 0 && !rsxExitedLogged) {
            rsxExitedLogged = true;
            log_line("FRONTEND-STOP-JOIN rsx-exited flips=%llu (flip counter frozen 500 ms: the RSX thread is no longer presenting)", flips);
        }
        if (onStop) {
            log_line("FRONTEND-STOP-JOIN join-complete on_stop received (Emulation Join Thread finished: emulation threads joined, emu state reset)");
            log_line("FRONTEND-STOP-JOIN complete elapsed_ms=%llu reason=%s via=on-stop", elapsed, reason);
            emu_host::stop_cycle_close();
            uist.screen = frontend::Screen::Library;
            return;
        }
        if (!strict && st == 0 && flipsFrozen && elapsed >= 10000) {
            log_line("FRONTEND-STOP-JOIN complete elapsed_ms=%llu reason=%s via=quiescence-fallback (boot never reached running - result!=no_errors - and on_stop never arrived; 10 s at state=0 with frozen flips: no emulation threads exist to join)", elapsed, reason);
            emu_host::stop_cycle_close();
            uist.screen = frontend::Screen::Library;
            return;
        }
        if (now - lastWaitingLog >= 2000) {
            lastWaitingLog = now;
            log_line("FRONTEND-STOP-JOIN waiting stage=%s elapsed_ms=%llu state=%d flips=%llu on_stop=%d", stage.c_str(), elapsed, st, flips, onStop ? 1 : 0);
        }
        if (flipsFrozen && st == 0 && !d3d12_bridge_owns()) {
            char stageBuf[160];
            std::snprintf(stageBuf, sizeof(stageBuf), "stage: %s - waiting for the emulation join to finish", stage.c_str());
            frontend::ui_render_stopping(ui, entry.title, stageBuf, elapsed, frameTick++);
            ui.present_frame();
        } else {
            Sleep(10);
        }
    }
    log_line("FRONTEND-STOP-JOIN aborted reason=%s (window closing; teardown wait cut short)", reason);
    emu_host::stop_cycle_close();
    uist.screen = frontend::Screen::Library;
}

// Resident game loop: never exits on emulation stop. While the RSX
// backend does not own the swapchain, the frontend Loading screen
// (frontend_progress snapshot) is presented; once the bridge owns
// flag is set the host stops presenting (1044 rule: two presenters
// on one swapchain fight). The menu pad is polled ONLY for the
// View+Menu exit combo - no menu event reaches the game, and the
// game pad (host_pad pad_thread) is a separate state entirely.
void App::game_loop(const frontend::GameEntry& entry,
                    const std::function<void()>& drain_callbacks,
                    frontend::FrontendUi& ui, frontend::MenuPad& pad) {
    auto hostLog = [](const std::string& l) { log_line("%s", l.c_str()); };
    watchdog_marker("FRONTEND-GAME");
    int st0 = emu_host::boot_emu_state();
    log_line("FRONTEND-GAME-ENTERED title=%s state=%d owns=%d (state: 0 stopped 1 loading 3 running 4 paused 6 ready 7 starting)",
        entry.title.c_str(), st0, d3d12_bridge_owns() ? 1 : 0);
    unsigned long long tick = 0;
    unsigned long long lastInit = ~0ull, lastFlip = ~0ull, lastPolls = ~0ull;
    bool ownsLogged = false;
    bool stopRequested = false;
    unsigned long long stopTickMs = 0;
    bool stopTimeoutLogged = false;
    while (!m_closed) {
        drain_callbacks();
        try {
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        } catch (Platform::Exception^ ex) {
            log_line("FRONTEND-GAME-PUMP-EXCEPTION winrt hr=0x%08lX (pump continues)", (unsigned long)ex->HResult);
        } catch (...) {
            log_line("FRONTEND-GAME-PUMP-EXCEPTION unknown (pump continues)");
        }
        if (m_closed) break;
        const int st = emu_host::boot_emu_state();
        if (st == 0) {
            log_line("FRONTEND-GAME-STOPPED state=0 stopRequested=%d (returning to library; app stays resident)", stopRequested ? 1 : 0);
            break;
        }
        if (FAILED(m_device->GetDeviceRemovedReason())) {
            log_line("FRONTEND-GAME-DEVICE-LOST hr=0x%08lX (stop requested, then library hold)", (unsigned long)m_device->GetDeviceRemovedReason());
            if (!stopRequested) { stopRequested = true; emu_host::boot_request_stop(hostLog); }
            Sleep(100);
            continue;
        }
        frontend::MenuInput in = pad.poll();
        if (in.exit_combo && !stopRequested) {
            stopRequested = true;
            stopTickMs = GetTickCount64();
            log_line("FRONTEND-EXIT-COMBO View+Menu held - stopping emulation for return to library");
            emu_host::boot_request_stop(hostLog);
        }
        if (stopRequested && !stopTimeoutLogged && GetTickCount64() - stopTickMs > 30000) {
            stopTimeoutLogged = true;
            log_line("FRONTEND-STOP-TIMEOUT state=%d after 30 s (still waiting in game loop; close the app if it never stops)", st);
        }
        const bool owns = d3d12_bridge_owns();
        if (owns && !ownsLogged) {
            ownsLogged = true;
            log_line("FRONTEND-RSX-OWNS-SWAPCHAIN first valid RSX flip blitted; frontend stops presenting (host frames are NOT game frames)");
        }
        if (!owns) {
            frontend::ProgressSnapshot snap;
            frontend::progress_snapshot(snap);
            frontend::ui_render_loading(ui, entry.title, snap, tick);
            HRESULT phr = ui.present_frame();
            if (FAILED(phr))
                log_line("FRONTEND-LOADING-PRESENT-FAIL hr=0x%08lX", (unsigned long)phr);
        } else {
            Sleep(16);
        }
        ++tick;
        const unsigned long long ci = d3d12_rsx_init_count(), cf = d3d12_rsx_flip_count();
        const unsigned long long po = host_pad_wgi_polls();
        if (ci != lastInit || cf != lastFlip || po != lastPolls) {
            lastInit = ci; lastFlip = cf; lastPolls = po;
            log_line("FRONTEND-GAME-RSX init=%llu flips=%llu draws(real)=%llu skips=%llu pad_polls=%llu pad_buttons=0x%llX state=%d owns=%d",
                ci, cf, d3d12_rsx_real_draws(), d3d12_rsx_real_skips(),
                po, host_pad_wgi_buttons(), st, owns ? 1 : 0);
        }
        if (tick % 300 == 0) { // ~5 s at 60 fps loading / ~5 s sleep-paced
            frontend::ProgressSnapshot snap;
            frontend::progress_snapshot(snap);
            log_line("FRONTEND-PROGRESS percent=%d src=%s notice=%llu trace=%llu error=%llu pdone=%u ptotal=%u taskbar=%d/%d text=%s last=%s",
                snap.percent, snap.percent_source.c_str(),
                snap.counts[frontend::kLevelNotice], snap.counts[frontend::kLevelTrace],
                snap.counts[frontend::kLevelError],
                snap.core_pdone, snap.core_ptotal, snap.taskbar_value, snap.taskbar_limit,
                snap.core_text.c_str(), snap.last_notice.c_str());
            log_line("FRONTEND-GAME-MEMORY AppMemoryUsage=%llu AppMemoryUsageLimit=%llu state=%d",
                (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage,
                (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit, st);
        }
    }
    log_line("FRONTEND-GAME-EXIT closed=%d state=%d owns=%d", m_closed ? 1 : 0,
        emu_host::boot_emu_state(), d3d12_bridge_owns() ? 1 : 0);
}

// F4 Compile (pre-bake) loop. Folder entries take the core's own
// directory-scan precompile (compile_phase_for_folder: the GUI
// CreateCPUCaches recipe - the core compiles EBOOT + every module in
// the install dir into the PPU cache, then self-Kills to stopped).
// ISO entries cannot be scanned host-side (their modules live inside
// the image), so they take a compile-pass boot: the normal boot path,
// stopped cleanly back to the library once the game has been running
// for a 20 s dwell - the pass caches the main module, firmware
// modules and whatever the boot itself loads, and NO more than that
// is claimed. Cancel (B or View+Menu) requests Emu.Kill; the core
// checks IsStopped between modules, so a cache file write always
// finishes before the pass unwinds. Completion requires state ==
// stopped; the compile working set is the emulator's own and is
// released by the Kill before this returns to the library.
std::string App::compile_loop(const frontend::GameEntry& entry,
                              const std::function<void()>& drain_callbacks,
                              frontend::FrontendUi& ui, frontend::MenuPad& pad,
                              const emu_host::boot_callbacks& bcb,
                              const std::string& lsdir) {
    auto hostLog = [](const std::string& l) { log_line("%s", l.c_str()); };
    const bool folder = entry.kind == frontend::GameKind::Folder;
    log_line("FRONTEND-COMPILE-START title=%s serial=%s kind=%s route=%s",
        entry.title.c_str(), entry.serial.c_str(), frontend::game_kind_name(entry.kind),
        folder ? "core directory-scan precompile (full module pre-bake)"
               : "compile-pass boot (ISO: caches what the boot loads only)");
    frontend::ModuleEnum men;
    if (folder) {
        men = frontend::enumerate_modules(entry, hostLog);
        if (!men.ok) {
            log_line("FRONTEND-COMPILE-RESULT outcome=skipped-no-eboot (enumeration found no EBOOT.BIN; nothing compiled)");
            return "Compile skipped: no EBOOT.BIN found for this entry";
        }
    }
    frontend::CacheStats before = frontend::ppu_cache_stats(lsdir, entry.serial, hostLog);
    {
        frontend::ShaderCacheFile scf = frontend::shader_cache_file(lsdir);
        log_line("FRONTEND-COMPILE-SHADERS when=before distinct=%llu hits=%llu misses=%llu cache_entries=%llu file_present=%d file_bytes=%llu (backend session counters; entries load from the cache file at RSX init)",
            d3d12_rsx_shader_distinct(), d3d12_rsx_shader_hits(), d3d12_rsx_shader_misses(),
            d3d12_rsx_shader_cache_entries(), scf.present ? 1 : 0, scf.bytes);
    }
    // One Compiling frame up before the synchronous phase call.
    {
        frontend::CompileView v;
        v.route = folder ? "Route: core directory-scan precompile (PPU cache pre-bake)"
                         : "Route: compile-pass boot (ISO contents cannot be scanned host-side)";
        v.modules_total = folder ? (int)men.modules.size() : -1;
        v.cache_files = (int)before.files;
        v.cache_bytes = before.bytes;
        frontend::ProgressSnapshot snap;
        frontend::progress_snapshot(snap);
        frontend::ui_render_compiling(ui, entry.title, v, snap, 0);
        ui.present_frame();
    }
    emu_host::boot_outcome bout;
    if (folder)
        bout = emu_host::compile_phase_for_folder(lsdir, men.compile_dir,
            entry.sfo_ok ? entry.serial : "", frontend::game_source_name(entry.source), bcb);
    else {
        const frontend::BootTarget bt = frontend::resolve_boot_target(entry);
        bout = emu_host::boot_phase_for_path(lsdir, bt.path,
            frontend::game_source_name(entry.source), bcb, bt.form);
    }
    watchdog_boot_returned();
    xbox_fatal_set_boot_ms(GetTickCount64());
    drain_callbacks();
    dump_mount_lines(folder ? "compile scan" : "compile pass");
    if (!bout.attempted || bout.result_code != 0) {
        log_line("FRONTEND-COMPILE-RESULT outcome=phase-failed attempted=%d code=%d skipped=%d",
            bout.attempted ? 1 : 0, bout.result_code, bout.skipped ? 1 : 0);
        return bout.skipped ? "Compile skipped: target missing (see log)"
                            : "Compile failed (code " + std::to_string(bout.result_code) + ") - see host-results.txt";
    }
    // ---- poll until the pass settles ----
    const unsigned long long t0 = GetTickCount64();
    const unsigned long long hardCapMs = folder ? 60ull * 60 * 1000 : 45ull * 60 * 1000;
    bool stopRequested = false;
    unsigned long long stopReqMs = 0;
    bool stopTimeoutLogged = false;
    bool sawActive = false, sawRunning = false;
    unsigned long long runningSince = 0;
    bool dwellDone = false;
    unsigned long long lastChangeMs = t0;
    frontend::ProgressSnapshot prevSnap;
    bool havePrev = false;
    int prevState = -1;
    std::string lastModule;
    frontend::CacheStats liveCache = before;
    unsigned long long tick = 0;
    const char* outcome = "loop-exit";
    while (!m_closed) {
        drain_callbacks();
        try {
            m_window->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
        } catch (Platform::Exception^ ex) {
            log_line("FRONTEND-COMPILE-PUMP-EXCEPTION winrt hr=0x%08lX (pump continues)", (unsigned long)ex->HResult);
        } catch (...) {
            log_line("FRONTEND-COMPILE-PUMP-EXCEPTION unknown (pump continues)");
        }
        if (m_closed) { outcome = "window-closed"; break; }
        const unsigned long long now = GetTickCount64();
        if (FAILED(m_device->GetDeviceRemovedReason()) && !stopRequested) {
            log_line("FRONTEND-COMPILE-DEVICE-LOST hr=0x%08lX (stop requested)", (unsigned long)m_device->GetDeviceRemovedReason());
            stopRequested = true; stopReqMs = now;
            emu_host::boot_request_stop(hostLog);
        }
        frontend::MenuInput in = pad.poll();
        if ((in.back || in.exit_combo) && !stopRequested) {
            stopRequested = true; stopReqMs = now;
            log_line("FRONTEND-COMPILE-CANCEL requested by pad (core checks IsStopped between modules; the in-flight cache write finishes first)");
            emu_host::boot_request_stop(hostLog);
        }
        const int st = emu_host::boot_emu_state();
        if (st != 0) sawActive = true;
        if (st == 3 && !sawRunning) {
            sawRunning = true; runningSince = now;
            log_line("FRONTEND-COMPILE-RUNNING state=running reached (ISO pass dwell starts; folder passes normally never run the game)");
        }
        frontend::ProgressSnapshot snap;
        frontend::progress_snapshot(snap);
        std::string cur = frontend::current_module_from_notice(snap.last_notice);
        if (!cur.empty()) lastModule = cur;
        if (tick % 150 == 0) liveCache = frontend::ppu_cache_stats(lsdir, entry.serial, nullptr);
        // Activity evidence beyond the state int: the counters were
        // reset by the phase call, so any core log/file progress or a
        // growing cache means the pass is alive even if a poll never
        // catches a non-stopped state.
        if (snap.total > 0 || snap.core_ftotal > 0 || liveCache.files != before.files)
            sawActive = true;
        const bool changed = !havePrev || snap.core_fdone != prevSnap.core_fdone ||
            snap.core_ftotal != prevSnap.core_ftotal || snap.total != prevSnap.total ||
            st != prevState;
        if (changed) { lastChangeMs = now; prevSnap = snap; havePrev = true; prevState = st; }
        // Completion rules.
        bool done = false;
        if (stopRequested && st == 0) { done = true; outcome = dwellDone ? "dwell-complete-stopped" : "cancelled-stopped"; }
        else if (folder) {
            if (st == 0 && sawActive && now - lastChangeMs > 3000) { done = true; outcome = "stopped-quiescent"; }
            else if (st == 0 && !sawActive && now - t0 > 120000 && now - lastChangeMs > 5000) { done = true; outcome = "never-activated"; }
        } else {
            if (st == 0 && sawActive) { done = true; outcome = "boot-ended-on-its-own"; }
            else if (st == 0 && !sawActive && now - t0 > 180000) { done = true; outcome = "never-activated"; }
            else if (sawRunning && !stopRequested && now - runningSince >= 20000) {
                dwellDone = true;
                stopRequested = true; stopReqMs = now;
                log_line("FRONTEND-COMPILE-DWELL running dwell 20 s complete - stopping the pass back to the library (main module + boot-loaded modules are cached by now)");
                emu_host::boot_request_stop(hostLog);
            }
        }
        if (!stopRequested && now - t0 > hardCapMs) {
            stopRequested = true; stopReqMs = now;
            log_line("FRONTEND-COMPILE-TIMEOUT hard cap %llu ms reached - stop requested", hardCapMs);
            emu_host::boot_request_stop(hostLog);
        }
        if (stopRequested && st != 0 && now - stopReqMs > 30000) {
            if (!stopTimeoutLogged) {
                stopTimeoutLogged = true;
                log_line("FRONTEND-COMPILE-STOP-TIMEOUT state=%d 30 s after stop request (leaving the loop; the library rescan follows, emulation state may still be settling)", st);
            }
            outcome = "stop-timeout";
            break;
        }
        if (done) break;
        // Render (never fight the RSX backend for the swapchain).
        if (!d3d12_bridge_owns()) {
            frontend::CompileView v;
            v.route = folder ? "Route: core directory-scan precompile (PPU cache pre-bake)"
                             : "Route: compile-pass boot (ISO contents cannot be scanned host-side)";
            v.modules_total = folder ? (int)men.modules.size() : -1;
            v.cache_files = (int)liveCache.files;
            v.cache_bytes = liveCache.bytes;
            v.current_module = lastModule;
            v.elapsed_ms = now - t0;
            v.cancelling = stopRequested;
            frontend::ui_render_compiling(ui, entry.title, v, snap, tick);
            HRESULT phr = ui.present_frame();
            if (FAILED(phr))
                log_line("FRONTEND-COMPILING-PRESENT-FAIL hr=0x%08lX", (unsigned long)phr);
        } else {
            Sleep(16);
        }
        if (tick % 300 == 0) {
            log_line("FRONTEND-COMPILE-PROGRESS percent=%d src=%s fdone=%u ftotal=%u module=%s cache_files=%u state=%d elapsed_s=%llu",
                snap.percent, snap.percent_source.c_str(), snap.core_fdone, snap.core_ftotal,
                lastModule.empty() ? "-" : lastModule.c_str(), liveCache.files, st,
                (now - t0) / 1000ull);
            log_line("FRONTEND-COMPILE-MEMORY AppMemoryUsage=%llu AppMemoryUsageLimit=%llu state=%d",
                (unsigned long long)Windows::System::MemoryManager::AppMemoryUsage,
                (unsigned long long)Windows::System::MemoryManager::AppMemoryUsageLimit, st);
        }
        ++tick;
    }
    const unsigned long long elapsedMs = GetTickCount64() - t0;
    frontend::CacheStats after = frontend::ppu_cache_stats(lsdir, entry.serial, hostLog);
    frontend::ProgressSnapshot snap;
    frontend::progress_snapshot(snap);
    log_line("FRONTEND-COMPILE-RESULT outcome=%s route=%s boot_code=%d modules_enumerated=%d core_fdone=%u core_ftotal=%u cache_files=%u->%u cache_bytes=%llu->%llu elapsed_s=%llu cancelled=%d state=%d",
        outcome, folder ? "scan" : "bootpass", bout.result_code,
        folder ? (int)men.modules.size() : -1, snap.core_fdone, snap.core_ftotal,
        before.files, after.files, before.bytes, after.bytes, elapsedMs / 1000ull,
        (stopRequested && !dwellDone) ? 1 : 0, emu_host::boot_emu_state());
    {
        frontend::ShaderCacheFile scf = frontend::shader_cache_file(lsdir);
        log_line("FRONTEND-COMPILE-SHADERS when=after distinct=%llu hits=%llu misses=%llu cache_entries=%llu file_present=%d file_bytes=%llu (requested = distinct pairs the game asked for this session; cached = served from the cache file; misses are NOT baked by F4 - they need a PC capture playthrough + offline bake)",
            d3d12_rsx_shader_distinct(), d3d12_rsx_shader_hits(), d3d12_rsx_shader_misses(),
            d3d12_rsx_shader_cache_entries(), scf.present ? 1 : 0, scf.bytes);
    }
    if (std::string(outcome) == "cancelled-stopped")
        return "Compile cancelled: " + entry.title + " (partial cache kept)";
    if (std::string(outcome) == "stopped-quiescent" || std::string(outcome) == "dwell-complete-stopped" ||
        std::string(outcome) == "boot-ended-on-its-own")
        return "Compiled: " + entry.title + " - PPU cache " + std::to_string(after.files) +
               " files (" + std::to_string((after.bytes + 524288) / 1048576) + " MiB)";
    return "Compile ended (" + std::string(outcome) + "): " + entry.title + " - see host-results.txt";
}

ref class AppSource sealed : Windows::ApplicationModel::Core::IFrameworkViewSource {
public: virtual IFrameworkView^ CreateView() { return ref new App(); }
};
} // namespace XboxUwpHost
[Platform::MTAThread]
int main(Platform::Array<Platform::String^>^ args) {
    (void)args; stage_write(2, "POST_CTORS_MAIN_ENTERED"); CoreApplication::Run(ref new XboxUwpHost::AppSource()); return 0;
}
