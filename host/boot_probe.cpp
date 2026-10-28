// boot_probe.cpp — v1.0.22.0 ISO-boot probe (NullGSRender first).
// Host-only: no RPCS3 core file modified. See boot_probe.h + the
// 1.0.22 report for the pad audit verdict and the config rationale.
//
// PAD AUDIT (read-only source inspection, 2026-10-27; full text in the
// report): cellPad does NOT tolerate a null g_pad_thread. It calls the
// STRICT pad::get_pad_thread() (Input/pad_thread.h:87-94, ensure() =
// fatal) at 21 call sites in Emu/Cell/Modules/cellPad.cpp and
// dereferences handler->GetPads() immediately. The core creates the
// pad_thread ONLY through the host's init_pad_handler callback
// (Emu/System.cpp init_fxo_for_exec; Qt: main_application.cpp:215
// g_fxo->init<named_thread<pad_thread>>(thread, window, title_id),
// then waits for pad::g_started). AND: Input/pad_thread.cpp is NOT
// part of emucore (linktest host-stubs.cpp says so; LNK2019 proved
// it), so this probe's init_pad_handler creates that object with null
// thread/window args and the pad_thread METHODS THEMSELVES are
// supplied by host_pad.cpp (host-only TU): 7 null-handler,
// DISCONNECTED pads, no config load, no Qt, no window — so cellPad
// cannot fatal and no keyboard-handler fallback can occur. WGI button
// delivery into cellPad is NOT wired here: that needs a custom
// PadHandlerBase subclass + handler registration, which is separate
// build work. In this probe pads are present (so cellPad cannot fatal)
// but disconnected; the host's Step 7 WGI polling keeps proving
// controller presence to the host, not to the game.

#include "boot_probe.h"
#include "callback_queue.h"
#include "crash_capture.h"
#include "early_config_dir.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <windows.h>

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/emu_callbacks.h"
#include "Emu/RSX/GSFrameBase.h"
#include "d3d12_rsx.h"
#include "pad_wgi.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/Io/music_handler_base.h"
#include "Emu/Audio/Null/NullAudioBackend.h"
#include "Emu/vfs_config.h"
#include "Emu/VFS.h"
#include "Input/pad_thread.h"
#include "util/video_source.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Utilities/Thread.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"

namespace emu_host
{

namespace
{

std::string slash_path(const std::string& p)
{
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

const char* boot_result_name(int code)
{
    switch (static_cast<game_boot_result>(code))
    {
    case game_boot_result::no_errors: return "no_errors";
    case game_boot_result::generic_error: return "generic_error";
    case game_boot_result::nothing_to_boot: return "nothing_to_boot";
    case game_boot_result::wrong_disc_location: return "wrong_disc_location";
    case game_boot_result::invalid_file_or_folder: return "invalid_file_or_folder";
    case game_boot_result::invalid_bdvd_folder: return "invalid_bdvd_folder";
    case game_boot_result::install_failed: return "install_failed";
    case game_boot_result::decryption_error: return "decryption_error";
    case game_boot_result::file_creation_error: return "file_creation_error";
    case game_boot_result::firmware_missing: return "firmware_missing";
    case game_boot_result::firmware_version: return "firmware_version";
    case game_boot_result::unsupported_disc_type: return "unsupported_disc_type";
    case game_boot_result::disc_key_missing: return "disc_key_missing";
    case game_boot_result::disc_key_invalid: return "disc_key_invalid";
    case game_boot_result::savestate_corrupted: return "savestate_corrupted";
    case game_boot_result::savestate_version_unsupported: return "savestate_version_unsupported";
    case game_boot_result::still_running: return "still_running";
    case game_boot_result::already_added: return "already_added";
    case game_boot_result::currently_restricted: return "currently_restricted";
    case game_boot_result::database_config_missing: return "database_config_missing";
    default: return "unknown";
    }
}


// Win32 write probe: returns GetLastError() (0 = success) for mkdir and
// for create+write of a small file. Wide conversion via UTF-8.
bool wide_from_utf8_str(const std::string& s, std::wstring& out)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0)
        return false;
    out.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0')
        out.pop_back();
    return true;
}

void log_write_test(std::function<void(const std::string&)> log, const char* tag,
                    const std::string& dir_slash)
{
    std::wstring wdir;
    if (!wide_from_utf8_str(dir_slash, wdir))
    {
        log(std::string(tag) + " dir=" + dir_slash + " CONVERT-FAILED");
        return;
    }
    std::wstring sub = wdir;
    if (!sub.empty() && sub.back() != L'/')
        sub += L'/';
    // Direct file write in the dir itself (no mkdir: CreateDirectoryW
    // is not proven in the app partition, and the dir already exists
    // in both probe cases). CreateFile2 is the console-proven API
    // (stage_diag.cpp pattern).
    std::wstring file = sub + L"xbox-probe-1023-write-test.txt";
    HANDLE h = CreateFile2(file.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr);
    DWORD wr_err = 0;
    if (h == INVALID_HANDLE_VALUE)
    {
        wr_err = GetLastError();
    }
    else
    {
        const char* data = "xbox-probe-1023-write-test";
        DWORD written = 0;
        if (!WriteFile(h, data, 26, &written, nullptr) || written != 26)
            wr_err = GetLastError();
        FlushFileBuffers(h);
        CloseHandle(h);
    }
    log(std::string(tag) + " dir=" + dir_slash +
        " write_err=" + std::to_string(wr_err) +
        " (0 = dir writable; 5=ACCESS_DENIED proves the read-only-exe-dir suspect; 3=PATH_NOT_FOUND means the dir itself is absent)");
}

std::string env_utf8(const wchar_t* name)
{
    wchar_t buf[4096] = {};
    DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    if (n == 0)
        return "UNSET";
    int m = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (m <= 0)
        return "CONVERT-FAILED";
    std::string out(static_cast<size_t>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), m, nullptr, nullptr);
    if (!out.empty() && out.back() == '\0')
        out.pop_back();
    return out;
}

// No-op music handler (v1.0.28.0): satisfies music_handler_base's
// pure virtuals so cellMusic's music_state ctor never sees a null
// handler. All operations are no-ops; get_volume returns 1.0 (full)
// so any volume read during boot is sane.
class null_music_handler : public music_handler_base
{
public:
    void stop() override {}
    void pause() override {}
    void play(const std::string&, bool = false) override {}
    void fast_forward(const std::string&) override {}
    void fast_reverse(const std::string&) override {}
    void set_volume(f32) override {}
    f32 get_volume() const override { return 1.0f; }
};

void install_boot_callbacks(std::function<void(const std::string&)> log,
                            std::function<void(std::function<void()>)> pump)
{
    // call_from_main_thread: host dispatcher pump (same as init_phase).
// 1.0.35: queue replaces the dispatcher pump (Argon fix 1).
// The pump param is kept for signature compatibility but
// intentionally unused: WinRT must never see callbacks.
g_emu_callbacks.call_from_main_thread = [pump](std::function<void()> f, atomic_t<u32>* wake_up)
{
    (void)pump;
    xbox_cb_enqueue_and_wait(f, 2000);
    if (wake_up) *wake_up = 1;
};

    // Renderer: D3D12GSRender (v1.0.29 Stage 2, first promotion step).
    // A real GSRender subclass: the shared rsx::thread base runs the
    // real RSX command stream; draws still execute as nop draws
    // (see d3d12_rsx.h — draw translation is pending, NOT faked).
    // Flip/draw/init counters are host-visible for BOOT-RSX lines.
    g_emu_callbacks.init_gs_render = [log](utils::serial* ar)
    {
        log("BOOT-RENDERER D3D12GSRender (g_fxo init rsx::thread/named_thread<D3D12GSRender>; draws=nop, counted; flip counted)");
        g_fxo->init<rsx::thread, named_thread<D3D12GSRender>>(ar);
    };

    // Frame: GSRender guards every m_frame use (GSRender.cpp ctor,
    // on_init_thread, on_exit, flip, refresh-rate), so nullptr is safe
    // for the D3D12 thread too. No host window is handed to the core.
    g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return nullptr; };

    // Pad: the core's own mechanism (see header comment). Create the
    // pad_thread the core expects; wait briefly for g_started so the
    // log proves Init() ran before Load proceeds.
    g_emu_callbacks.init_pad_handler = [log](std::string_view title_id)
    {
        log(std::string("BOOT-PAD: init_pad_handler title_id=") + std::string(title_id) +
            " creating named_thread<pad_thread> (host_pad.cpp methods, null thread/window args; v1.0.29 Stage 3: WGI poll drives player 0)");
        bool ok = false;
        try
        {
            ok = g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id);
        }
        catch (const std::exception& ex)
        {
            log(std::string("BOOT-PAD-EXCEPTION: ") + ex.what());
        }
        catch (...)
        {
            log("BOOT-PAD-EXCEPTION: unknown");
        }
        log(std::string("BOOT-PAD: g_fxo init named_thread<pad_thread>=") + (ok ? "1" : "0") +
            " g_pad_thread=" + (pad::g_pad_thread.load() ? "nonnull" : "null"));
        for (int i = 0; i < 50 && !pad::g_started.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        log(std::string("BOOT-PAD: g_started=") + (pad::g_started.load() ? "1" : "0") +
            " (1 = pad_thread::Init() ran; 0 = Init still pending or failed — see BOOT result)");
        log("BOOT-PAD-AUDIT: cellPad NOT null-tolerant (strict get_pad_thread x21, ensure=fatal); "
            "host_pad.cpp pad_thread created above is the fallback (player 0 = WGI-driven, players 1-6 DISCONNECTED, no config load, no Qt); "
            "BOOT-PAD-WGI counters in the watch loop prove polling (polls/connected/buttons)");
    };

    // 1.0.30: shader-cache dir handoff for D3D12GSRender (LocalState;
    // set here so on_init_thread can read the cache file during Load).
    // The cfg path is built just below; this lambda runs during Load,
    // after boot_phase has set the dir — see the set call before
    // BootGame.
    g_emu_callbacks.init_kb_handler = []() {};
    g_emu_callbacks.init_mouse_handler = []() {};

    // Audio: null backend — no host audio device is handed to the core
    // in this probe (host XAudio2 tone runs separately, as before).
    g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
    {
        return std::make_shared<NullAudioBackend>();
    };
    g_emu_callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator> { return nullptr; };
    g_emu_callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base> { return nullptr; };
    // v1.0.28.0: music handler must NOT be null. music_state's ctor
    // (cellMusic.cpp) does handler = get_music_handler() then
    // handler->set_event_status_callback(...) with NO null check, so
    // the 1.0.27 null return faulted at handler+0x48 (fault=0x48,
    // RVA 0xBCFF31 inside music_state::music_state()). A no-op handler
    // is the host-side fix: callbacks register on a valid object and
    // play/stop/etc. simply do nothing (no music device on Xbox).
    g_emu_callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base>
    {
        return std::make_shared<null_music_handler>();
    };

    // Dialogs / OS services the core may call during boot: safe
    // null/empty defaults instead of empty std::functions (which would
    // throw bad_function_call and kill the boot opaquely).
    g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return nullptr; };
    g_emu_callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return nullptr; };
    g_emu_callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return nullptr; };
    g_emu_callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return nullptr; };
    g_emu_callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return nullptr; };
    g_emu_callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return nullptr; };
    g_emu_callbacks.get_localized_string = [](localized_string_id, const char*) -> std::string { return std::string{}; };
    g_emu_callbacks.get_localized_u32string = [](localized_string_id, const char*) -> std::u32string { return std::u32string{}; };
    g_emu_callbacks.get_localized_setting = [](const cfg::_base*, u32) -> std::string { return std::string{}; };
    g_emu_callbacks.get_photo_path = [](std::string_view) -> std::string { return std::string{}; };
    g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
    g_emu_callbacks.get_image_info = [](const std::string&, std::string&, s32&, s32&, s32&) { return false; };
    g_emu_callbacks.get_scaled_image = [](const std::string&, s32, s32, s32&, s32&, u8*, bool) { return false; };
    g_emu_callbacks.get_font_dirs = []() -> std::vector<std::string> { return {}; };
    g_emu_callbacks.on_install_pkgs = [](const std::vector<std::string>&, bool) { return false; };
    g_emu_callbacks.add_breakpoint = [](u32) {};
    g_emu_callbacks.display_sleep_control_supported = []() { return false; };
    g_emu_callbacks.enable_display_sleep = [](bool) {};
    g_emu_callbacks.check_microphone_permissions = []() {};
    g_emu_callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return nullptr; };
    g_emu_callbacks.enable_gamemode = [](bool) {};
    g_emu_callbacks.enable_disc_eject = [](bool) {};
    g_emu_callbacks.enable_disc_insert = [](bool) {};
    g_emu_callbacks.handle_taskbar_progress = [](s32, s32) {};
    g_emu_callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>>, int) {};
    g_emu_callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
    g_emu_callbacks.get_database_config = [](const std::string&) -> std::string { return std::string{}; };

    g_emu_callbacks.on_run = [log](bool) { log("EMU-CB: on_run (boot lifecycle)"); };
    g_emu_callbacks.on_pause = [log]() { log("EMU-CB: on_pause"); };
    g_emu_callbacks.on_resume = [log]() { log("EMU-CB: on_resume"); };
    g_emu_callbacks.on_stop = [log]() { log("EMU-CB: on_stop"); };
    g_emu_callbacks.on_ready = [log]() { log("EMU-CB: on_ready"); };
    g_emu_callbacks.on_missing_fw = [log]() { log("EMU-CB: on_missing_fw (firmware missing at boot)"); };
    g_emu_callbacks.close_gs_frame = []() {};
    g_emu_callbacks.try_to_quit = [](bool, std::function<void()>) { return false; };
    g_emu_callbacks.update_emu_settings = []() {};
    g_emu_callbacks.save_emu_settings = []() {};
    // resolve_path keeps its identity default (paths already resolve
    // under the LocalState config dir).
}

// 1.0.24 address-space / commit probe (host-only, Win32 APIs already
// proven in this host: VirtualAlloc/VirtualFree are used by the Step 8
// governor; MapViewOfFileEx/CreateFileMappingW are used by the core
// itself, so they are present in this image). Distinguishes three
// mechanisms for the 1.0.23 vm.cpp:2348 failure:
//   (a) VA ceiling: VirtualAlloc MEM_RESERVE of 32 GiB at nullptr fails.
//   (b) Commit/section ceiling: reserve succeeds but a 32 GiB
//       pagefile-backed MapViewOfFileEx fails with err 8.
//   (c) Fixed-address fragmentation: both succeed at nullptr, so the
//       failure is specific to the fixed g_hook_addr range (released
//       by memory_release just before s_hook.map, then re-occupied).
// All probes are freed/unmapped immediately; nothing persists to boot.
void log_addrspace_probe(std::function<void(const std::string&)> log)
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms))
    {
        log(std::string("BOOT-ADDRSPACE commit-limit-pagefile=") + std::to_string(ms.ullTotalPageFile) +
            " commit-avail=" + std::to_string(ms.ullAvailPageFile) +
            " phys-total=" + std::to_string(ms.ullTotalPhys) +
            " phys-avail=" + std::to_string(ms.ullAvailPhys) +
            " (pagefile total is the commit ceiling; a 32 GiB COW view charges against it)");
    }
    else
    {
        log(std::string("BOOT-ADDRSPACE GlobalMemoryStatusEx FAILED err=") + std::to_string(GetLastError()));
    }
    static const unsigned long long sizes[] = {
        68719476736ull, 34359738368ull, 17179869184ull, 8589934592ull, 4294967296ull};
    for (unsigned long long sz : sizes)
    {
        void* p = VirtualAllocFromApp(nullptr, static_cast<SIZE_T>(sz), MEM_RESERVE, PAGE_NOACCESS);
        DWORD err = p ? 0 : GetLastError();
        log(std::string("BOOT-ADDRSPACE reserve size=") + std::to_string(sz) +
            " ok=" + (p ? "1" : "0") + " err=" + std::to_string(err));
        if (p)
            VirtualFree(p, 0, MEM_RELEASE);  // VirtualFree is NOT SDK-wrapped (never in the LNK2005 list); only VirtualAlloc/CreateFileMappingW are;
    }
    // Pagefile-backed section view at nullptr, 32 GiB — the shape of
    // s_hook.map (COW view) minus the fixed address and minus the
    // sparse file. FILE_MAP_WRITE|FILE_MAP_COPY matches shm::map
    // with prot=rw, cow=true.
    HANDLE hmap = CreateFileMappingFromApp(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 34359738368ull, nullptr);  // 5-arg FromApp form: handle, attributes, protect, ULONG64 size, name
    if (!hmap)
    {
        log(std::string("BOOT-ADDRSPACE section-create FAILED err=") + std::to_string(GetLastError()));
        return;
    }
    void* view = MapViewOfFileFromApp(hmap, FILE_MAP_WRITE | FILE_MAP_COPY, 0, static_cast<SIZE_T>(34359738368ull));  // 4-arg form (memoryapi.h:522): handle, access, ULONG64 offset, size. MapViewOfFileEx is desktop-only/undeclared under WINAPI_FAMILY_APP, and the MapViewOfFile inline wrapper duplicates the windowsapp.lib export at link (LNK2005), so neither is usable here. Base-address choice is unavailable in this form, which is fine — this probe measures the nullptr-address ceiling
    DWORD verr = view ? 0 : GetLastError();
    log(std::string("BOOT-ADDRSPACE mapview-32gib-pagefile ok=") + (view ? "1" : "0") + " err=" + std::to_string(verr) +
        " (err=8 here + reserve-32gib ok above = commit/section ceiling, mechanism (b); err=8 + reserve fail = VA ceiling (a); ok here = fixed-address issue (c))");
    if (view)
        UnmapViewOfFile(view);
    CloseHandle(hmap);
}

} // namespace

boot_outcome boot_phase(const std::string& localStateUtf8,
                        const boot_callbacks& cb)
{
    auto log = cb.log;
    boot_outcome out;
    log("BOOT-PHASE start: comprehensive v1.0.29, renderer=D3D12GSRender (nop draws, counted), llvm_threads=1, PSN=Simulated, config dir=LocalState");

    // Operative config-dir override, re-applied (Init/firmware re-init
    // may have reset config to defaults since init_phase ran).
    const std::string cfg = slash_path(localStateUtf8) + "/";
    g_cfg_vfs.emulator_dir.set(cfg);
    log(std::string("BOOT-CONFIG-DIR override set to ") + cfg);
    log(std::string("BOOT-CACHE-DIR fs::get_cache_dir=") + fs::get_cache_dir() +
        " (PPU/SPU cache target; exe-dir on console = read-only, cache-write failure is a named probe outcome, not assumed fatal)");
    // 1.0.23 diagnostics: what did the early (compiler-segment) ctor
    // record, and what does the env hold NOW? If the ctor won, the
    // BOOT-CACHE-DIR above already equals LocalState; if it lost, the
    // reason code below says why (see early_config_dir.cpp).
    log(std::string("BOOT-ENV-RPCS3_CONFIG_DIR=") + env_utf8(L"RPCS3_CONFIG_DIR"));
    {
        std::string early_val;
        if (g_early_config_dir_set)
        {
            wchar_t* v = g_early_config_dir_value;
            int m = WideCharToMultiByte(CP_UTF8, 0, v, -1, nullptr, 0, nullptr, nullptr);
            if (m > 0)
            {
                early_val.resize(static_cast<size_t>(m));
                WideCharToMultiByte(CP_UTF8, 0, v, -1, early_val.data(), m, nullptr, nullptr);
                if (!early_val.empty() && early_val.back() == '\0')
                    early_val.pop_back();
            }
        }
        log(std::string("BOOT-EARLY-CONFIG-DIR set=") + (g_early_config_dir_set ? "1" : "0") +
            " reason=" + std::to_string(g_early_config_dir_reason) +
            " value=" + (early_val.empty() ? "(none)" : early_val) +
            " (reason: 0=not packaged/no force env, 1=force env, 2=LOCALAPPDATA AC->LocalState, 3=LOCALAPPDATA missing, 4=no \\AC suffix, 5=module unreadable)");
    }
    // Cache-write proof, independent of the crash: if the cache dir is
    // read-only, mkdir/write fail here with ACCESS_DENIED (5) BEFORE
    // BootGame runs, killing or confirming suspect (1) on their own.
    log_write_test(log, "BOOT-CACHE-WRITE-TEST", fs::get_cache_dir());
    log_write_test(log, "BOOT-LOCALSTATE-WRITE-TEST", cfg);
    // 1.0.24: pre-create the directories the core writes during boot.
    // 1.0.23 core log: config.yml save failed "failed to create temporary
    // file (Not found)", games.yml save failed, BDVD location save failed
    // — all under LocalState/config/. fs::create_path creates parents.
    // These errors did NOT stop boot at 1.0.23 (it reached vm init), so
    // they are not yet proven fatal; this block tests whether they
    // disappear on the next run, host-side, with no core edit.
    for (const char* sub : {"config/", "config/input_configs/", "captures/", "sounds/"})
    {
        const std::string dir = cfg + sub;
        const bool created = fs::create_path(dir);
        const bool isdir = fs::is_dir(dir);
        log(std::string("BOOT-DIRS dir=") + dir + " create_path=" + (created ? "1" : "0") +
            " is_dir=" + (isdir ? "1" : "0") +
            " (1/1 = dir present before BootGame; 0 in is_dir after create_path=1 means the app-partition create failed silently)");
    }

    // ISO presence: the file is uploaded separately by the parent; the
    // package must never contain it.
    const std::string iso_path = slash_path(localStateUtf8) + "/Call of Duty - Black Ops III.iso";
    fs::file iso_f(iso_path);
    if (!iso_f)
    {
        log("BOOT-ISO: MISSING path=LocalState/Call of Duty - Black Ops III.iso size=0 (upload the 4 parts + relaunch; REASSEMBLE phase runs before BOOT)");
        log("BOOT-VERDICT: SKIPPED (ISO absent from LocalState; expected on an empty LocalState, not a package failure)");
        if (cb.mem_usage && cb.mem_limit)
            log("BOOT-MEMORY AppMemoryUsage=" + std::to_string(cb.mem_usage()) +
                " AppMemoryUsageLimit=" + std::to_string(cb.mem_limit()) + " (3.0 GiB abort line=3221225472)");
        out.skipped = true;
        return out;
    }
    const u64 iso_size = iso_f.size();
    iso_f.close();
    log("BOOT-ISO: found path=LocalState/Call of Duty - Black Ops III.iso size=" + std::to_string(iso_size) +
        " expected=4599644160 match=" + (iso_size == 4599644160ull ? "1" : "0"));

    // Config for the probe. NOTE on Load() ordering (Emu/System.cpp):
    // BootGame -> Load reloads the global config.yml from the exe dir
    // (fs::get_config_dir) and re-runs fixup_settings. On console the
    // exe dir is read-only, so the reload cannot overwrite these
    // in-memory values; on desktop the same file may exist, which is
    // why the desktop pre-test below must NOT call boot_phase.
    // fixup_settings forces psn_status=disabled when net_active is
    // disabled, so Simulated requires net=enabled AND psn=psn_fake.
    g_cfg.video.renderer.set(video_renderer::null);
    g_cfg.core.llvm_threads.set(1);
    g_cfg.net.net_active.set(np_internet_status::enabled);
    g_cfg.net.psn_status.set(np_psn_status::psn_fake);
    g_cfg.audio.renderer.set(audio_renderer::null);
    g_cfg.io.camera.set(camera_handler::null);
    g_cfg.core.ppu_decoder.set(ppu_decoder_type::_static);
    g_cfg.core.spu_decoder.set(spu_decoder_type::_static);
    log("BOOT-CONFIG: renderer=null llvm_threads=1 net_active=enabled psn_status=psn_fake(Simulated) audio=null camera=null ppu_decoder=static spu_decoder=static (1.0.35 DIAGNOSTIC ONLY)");
    log(std::string("BOOT-DECODER ppu=") + (g_cfg.core.ppu_decoder.get() == ppu_decoder_type::_static ? "static" : "llvm-or-other") + " spu=" + (g_cfg.core.spu_decoder.get() == spu_decoder_type::_static ? "static" : "llvm-or-other") + " (read back immediately before BootGame; see BOOT-DECODER-AFTER for post-Load values)");
    log("BOOT-CONFIG-NOTE: never real PSN sign-in; Simulated=psn_fake, fallback RPCN; stubbed WS2_32 returns fail-closed, so any real-socket NP call during boot is a named probe outcome");

    install_boot_callbacks(log, cb.pump);
    log("BOOT-CALLBACKS installed (init_gs_render=D3D12GSRender, init_pad_handler=pad_thread(WGI player 0), get_audio=NullAudioBackend, get_gs_frame=nullptr, dialogs safe-defaulted)");
    log_addrspace_probe(log);

    if (cb.mem_usage && cb.mem_limit)
        log("BOOT-MEMORY AppMemoryUsage=" + std::to_string(cb.mem_usage()) +
            " AppMemoryUsageLimit=" + std::to_string(cb.mem_limit()) + " (3.0 GiB abort line=3221225472) milestone=boot-start");

    // 1.0.23 capture: core fatal/error listener + process-wide VEH +
    // terminate handler, installed before BootGame. Stale crash logs
    // are cleared here so any BOOT-VEH/BOOT-CORE-LOG line found after
    // a run belongs to THAT run.
    crash_capture_install(cfg.substr(0, cfg.size() - 1), log);
    log("BOOT-CAPTURE-READY: listener+VEH installed; crash logs are LocalState/boot-crash-log.txt and LocalState/boot-veh-log.txt (fetch separately — the host-results fetch does not include them)");

    // 1.0.30: hand the LocalState cache dir to the RSX thread BEFORE
    // BootGame so D3D12GSRender::on_init_thread can load
    // d3d12_shader_cache.bin during Load.
    d3d12_rsx_set_cache_dir(cfg);
    game_boot_result result = game_boot_result::generic_error;
    bool seh_ok = false;
    bool running_after_call = false;
    {
        // Arguments constructed HERE (caller) so the SEH leaf holds no
        // C++ object with a destructor (see crash_capture.h).
        const std::string empty_title;
        const std::string empty_config_path;
        const std::optional<std::string> empty_db_config;
        try
        {
            seh_ok = call_boot_game_seh(iso_path, empty_title, true, cfg_mode::custom,
                                        empty_config_path, empty_db_config,
                                        result, running_after_call);
        }
        catch (const std::exception& ex)
        {
            log(std::string("BOOT-EXCEPTION: ") + ex.what());
            out.attempted = true;
            out.result_code = static_cast<int>(game_boot_result::generic_error);
            out.running_after = false;
            log("BOOT-VERDICT: FAIL (BootGame threw; see BOOT-EXCEPTION line)");
            return out;
        }
        catch (...)
        {
            log("BOOT-EXCEPTION: unknown (non-std exception from BootGame)");
            out.attempted = true;
            out.result_code = static_cast<int>(game_boot_result::generic_error);
            out.running_after = false;
            log("BOOT-VERDICT: FAIL (BootGame threw unknown exception)");
            return out;
        }
    }
    if (!seh_ok)
    {
        log("BOOT-VERDICT: FAIL (main-thread hardware SEH in BootGame; see LocalState/boot-veh-log.txt BOOT-SEH line for the code)");
        log("BOOT-VM-FAIL: if boot-veh-log.txt shows code=0x80000003, the named candidate is vm::ps3_::init() Emu/Memory/vm.cpp:2348 ensure(s_hook.map(g_hook_addr, protection::rw, cow=true)) — a 32 GiB copy-on-write MapViewOfFileEx of the sparse-file section (util/vm_native.cpp:818 shm::map, section from shm ctor util/vm_native.cpp:572 temp-dir rpcs3_vm_sparse.tmp, CreateFileMappingW) at the fixed g_hook_addr (vm.cpp:52 memory_reserve_4GiB(g_exec_addr, 0x800000000)), released by utils::memory_release(vm.cpp:2346) immediately before the map; 1.0.23 core fatal quoted error 0x8 ERROR_NOT_ENOUGH_MEMORY. Correlate with the BOOT-ADDRSPACE lines above: reserve-fail=(a) VA ceiling, mapview-fail-only=(b) commit/section ceiling, both-ok=(c) fixed-address fragmentation.");
        out.attempted = true;
        out.result_code = static_cast<int>(game_boot_result::generic_error);
        out.running_after = false;
        return out;
    }

    out.attempted = true;
    out.result_code = static_cast<int>(result);
    out.running_after = running_after_call;
    log(std::string("BOOT-RESULT: code=") + std::to_string(out.result_code) +
        " name=" + boot_result_name(out.result_code) +
        " IsStopped=" + (Emu.IsStopped() ? "1" : "0") +
        " IsRunning=" + (out.running_after ? "1" : "0") +
        " title_id=" + Emu.GetTitleID());
        log(std::string("BOOT-DECODER-AFTER ppu=") + (g_cfg.core.ppu_decoder.get() == ppu_decoder_type::_static ? "static" : "llvm-or-other") + " spu=" + (g_cfg.core.spu_decoder.get() == spu_decoder_type::_static ? "static" : "llvm-or-other") + " (read back after BootGame returns; differs from BOOT-DECODER only if Load() reloaded config)");
    if (cb.mem_usage && cb.mem_limit)
        log("BOOT-MEMORY AppMemoryUsage=" + std::to_string(cb.mem_usage()) +
            " AppMemoryUsageLimit=" + std::to_string(cb.mem_limit()) + " (3.0 GiB abort line=3221225472) milestone=boot-returned");
    if (result == game_boot_result::no_errors)
    {
        // 1.0.30 wording fix: BootGame only calls Load(); Run() sets
        // state=starting and state=running arrives later via the RSX
        // thread's RunPPU() -> FinalizeRunRequest(). Do NOT claim
        // "running" here — the host watch loop's BOOT-STATE lines
        // are the progress proof. running_after==false at this point
        // is the normal race, not a failure.
        log(std::string("BOOT-PPU: Load returned no_errors; state=starting expected (Run() called, running arrives via RSX RunPPU); running_after=") + (out.running_after ? "1" : "0") + " — see BOOT-STATE watch lines");
    }
    else if (result != game_boot_result::no_errors)
    {
        log(std::string("BOOT-VERDICT: FAIL (BootGame result=") + boot_result_name(out.result_code) + "; see BOOT-RESULT line)");
    }
    return out;
}

bool boot_is_running()
{
    return Emu.IsRunning();
}

bool boot_is_stopped()
{
    return Emu.IsStopped();
}

int boot_emu_state()
{
    return static_cast<int>(Emu.GetStatus());
}

} // namespace emu_host
