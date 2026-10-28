// emu_host.cpp — v1.0.10.0: real Emulator init + firmware install.
// Compiled native (no /ZW), /MD /std:c++latest, with the emucore
// include list. Mirrors rpcs3qt/main_window.cpp firmware install
// (lines 1485-1780) minus the Qt-only parts: no prompt dialogs (host
// has no window, so version/reinstall checks log and proceed), no
// progress dialog, no font-database step.

#include "emu_host.h"
#include "callback_queue.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <windows.h>

#include "Emu/System.h"
#include "Emu/emu_callbacks.h"
#include "Emu/RSX/GSFrameBase.h"
#include "Emu/Audio/AudioBackend.h"
#include "Emu/vfs_config.h"
#include "Emu/VFS.h"
#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Crypto/unself.h"
#include "Crypto/key_vault.h"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"

namespace emu_host
{

namespace
{

std::string to_utf8(const std::wstring& w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

std::string slash_path(const std::string& p)
{
    std::string s = p;
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

// Recursive file count under dir (fs::dir + fs::is_dir; '.'/'..' skipped).
u64 count_files(const std::string& dir, u64 depth = 0)
{
    if (depth > 64) return 0;
    u64 count = 0;
    fs::dir d(dir);
    if (!d) return 0;
    fs::dir_entry e;
    while (d.read(e))
    {
        if (e.name == "." || e.name == "..") continue;
        const std::string full = dir + "/" + e.name;
        if (fs::is_dir(full))
            count += count_files(full, depth + 1);
        else
            count += 1;
    }
    return count;
}

void install_callbacks(std::function<void(const std::string&)> log,
                       std::function<void(std::function<void()>)> pump)
{
    // call_from_main_thread: pump the host dispatcher until f runs.
    // wake_up is set after delivery (the Emu side waits on it).
// 1.0.35: queue replaces the dispatcher pump (Argon fix 1).
// The pump param is kept for signature compatibility but
// intentionally unused: WinRT must never see callbacks.
g_emu_callbacks.call_from_main_thread = [pump](std::function<void()> f, atomic_t<u32>* wake_up)
{
    (void)pump;
    xbox_cb_enqueue_and_wait(f, 2000);
    if (wake_up) *wake_up = 1;
};
    // Firmware install does not boot, so no renderer/pad/audio is needed.
    // Logging no-ops (instead of empty std::functions, which would throw
    // bad_function_call) make an unexpected call visible, not fatal.
    g_emu_callbacks.init_gs_render = [log](utils::serial*) { log("EMU-CB: init_gs_render called (unexpected during init/firmware, ignored)"); };
    g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase> { return nullptr; };
    g_emu_callbacks.init_pad_handler = [log](std::string_view t) { log(std::string("EMU-CB: init_pad_handler title_id=") + std::string(t) + " (ignored)"); };
    g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend> { return nullptr; };
    g_emu_callbacks.init_kb_handler = []() {};
    g_emu_callbacks.init_mouse_handler = []() {};
    g_emu_callbacks.close_gs_frame = []() {};
    g_emu_callbacks.on_run = [log](bool) { log("EMU-CB: on_run"); };
    g_emu_callbacks.on_pause = [log]() { log("EMU-CB: on_pause"); };
    g_emu_callbacks.on_resume = [log]() { log("EMU-CB: on_resume"); };
    g_emu_callbacks.on_stop = [log]() { log("EMU-CB: on_stop"); };
    g_emu_callbacks.on_ready = [log]() { log("EMU-CB: on_ready"); };
    g_emu_callbacks.on_missing_fw = [log]() { log("EMU-CB: on_missing_fw"); };
    g_emu_callbacks.try_to_quit = [](bool, std::function<void()>) { return false; }; // host owns the window lifecycle
    g_emu_callbacks.update_emu_settings = []() {};
    g_emu_callbacks.save_emu_settings = []() {};
    // resolve_path left at its identity default: paths already resolve
    // under the LocalState config dir.
}

} // namespace

bool init_phase(const std::string& localStateUtf8,
                std::function<void(const std::string&)> log,
                std::function<void(std::function<void()>)> pump)
{
    log("EMU-INIT-PHASE start: config dir = LocalState, callbacks = host dispatcher pump + logging no-ops");
    // RPCS3_CONFIG_DIR is set here, but it is NOT the operative override:
    // fs::get_config_dir() is a magic static, and desktop-proven
    // (2026-10-27) an emucore global constructor computes it BEFORE
    // main/Run ever runs, so a set here arrives too late and the exe
    // dir wins (InstalledLocation, read-only, on console). The operative
    // override is g_cfg_vfs.emulator_dir, set AFTER each Emu.Init()
    // below: get_emu_dir() prefers it over get_config_dir(), and every
    // VFS path (dev_flash, hdd0, ...) derives from it. Trailing slash
    // is the config-dir convention for both values.
    const std::string cfg = slash_path(localStateUtf8) + "/";
    const std::wstring wcfg(cfg.begin(), cfg.end());
    SetEnvironmentVariableW(L"RPCS3_CONFIG_DIR", wcfg.c_str());
    log(std::string("EMU-INIT-CONFIG-DIR env set to ") + cfg + " (advisory only: magic static already computed, see emulator_dir override below)");

    install_callbacks(log, pump);

    try
    {
        Emu.Init();
    }
    catch (const std::exception& ex)
    {
        log(std::string("EMU-INIT-EXCEPTION: ") + ex.what());
        return false;
    }
    catch (...)
    {
        log("EMU-INIT-EXCEPTION: unknown (non-std exception from Emu.Init)");
        return false;
    }

    log(std::string("EMU-INIT-RESULT: IsStopped=") + (Emu.IsStopped() ? "1" : "0") +
        " IsRunning=" + (Emu.IsRunning() ? "1" : "0"));
    log(std::string("EMU-INIT-CONFIG-DIR resolved: ") + fs::get_config_dir() + " (expected to be the exe dir: static computed pre-main)");
    // Operative override, AFTER Init (Init resets config to defaults when
    // no config.yml exists, so a pre-Init set would be wiped).
    g_cfg_vfs.emulator_dir.set(cfg);
    log(std::string("EMU-INIT-EMU-DIR override set to ") + cfg);
    log(std::string("EMU-INIT-DEV-FLASH: ") + g_cfg_vfs.get_dev_flash());
    return true;
}

bool firmware_phase(const std::string& localStateUtf8,
                    std::function<void(const std::string&)> log)
{
    log("FIRMWARE-PHASE start: core install sequence (PUP -> dev_flash_*, SCEDecrypter, TAR, /dev_flash mount)");
    const std::string pup_path = slash_path(localStateUtf8) + "/PS3UPDAT.PUP";
    fs::file pup_f(pup_path);
    if (!pup_f)
    {
        log("FIRMWARE-SKIP: PS3UPDAT.PUP absent from LocalState (upload it via Portal, then relaunch)");
        return false;
    }
    log("FIRMWARE-PUP: found, size=" + std::to_string(pup_f.size()));

    pup_object pup(std::move(pup_f));
    const pup_error perr = static_cast<pup_error>(pup);
    if (perr != pup_error::ok)
    {
        log("FIRMWARE-PUP-ERROR: code=" + std::to_string(static_cast<u32>(perr)) + " (not ok)");
        return false;
    }

    fs::file update_files_f = pup.get_file(0x300);
    if (!update_files_f)
    {
        log("FIRMWARE-ERROR: entry 0x300 (update files) empty - PUP corrupt");
        return false;
    }
    const u64 update_files_size = update_files_f.size();
    log("FIRMWARE-UPDATE-FILES: entry 0x300 size=" + std::to_string(update_files_size));

    // Belt-and-braces: firmware_phase is also correct standalone (the
    // host calls init_phase first, but the override must hold here even
    // if Init is ever called again between the phases).
    g_cfg_vfs.emulator_dir.set(slash_path(localStateUtf8) + "/");
    const std::string dev_flash = g_cfg_vfs.get_dev_flash();
    fs::create_path(dev_flash);
    fs::device_stat dev_stat{};
    if (!fs::statfs(dev_flash, dev_stat))
    {
        log("FIRMWARE-ERROR: statfs on dev_flash failed (" + dev_flash + ")");
        return false;
    }
    if (dev_stat.avail_free < update_files_size)
    {
        log("FIRMWARE-ERROR: not enough space (avail_free=" + std::to_string(dev_stat.avail_free) +
            " need=" + std::to_string(update_files_size) + ")");
        return false;
    }

    // Version string from entry 0x100, truncated at first newline.
    std::string version;
    {
        fs::file ver_f = pup.get_file(0x100);
        if (ver_f) version = ver_f.to_string();
        const size_t nl = version.find('\n');
        if (nl != std::string::npos) version.resize(nl);
    }
    if (version.empty())
    {
        log("FIRMWARE-ERROR: version string empty - PUP corrupt");
        return false;
    }
    log("FIRMWARE-VERSION: PUP version=" + version + " (host has no window: no prompt, install proceeds)");

    tar_object update_files(update_files_f);
    std::vector<std::string> update_filenames;
    for (const std::string& name : update_files.get_filenames())
    {
        if (name.find("dev_flash_") != std::string::npos)
            update_filenames.push_back(name);
    }
    if (update_filenames.empty())
    {
        log("FIRMWARE-ERROR: no dev_flash_* entries in update files - PUP corrupt");
        return false;
    }
    log("FIRMWARE-PACKAGES: count=" + std::to_string(update_filenames.size()));

    const bool mounted = vfs::mount("/dev_flash", dev_flash);
    log(std::string("FIRMWARE-MOUNT: /dev_flash -> ") + dev_flash + " mounted=" + (mounted ? "1" : "0"));

    u64 progress = 0;
    for (const std::string& name : update_filenames)
    {
        std::unique_ptr<utils::serial> stream = update_files.get_file(name);
        if (!stream)
        {
            log("FIRMWARE-ERROR: get_file failed for " + name);
            return false;
        }
        if (stream->m_file_handler)
            stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
        fs::file update_file = fs::make_stream(std::move(stream->data));

        SCEDecrypter self_dec(update_file);
        if (!self_dec.LoadHeaders())
        {
            log("FIRMWARE-ERROR: LoadHeaders failed for " + name);
            return false;
        }
        if (!self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV))
        {
            log("FIRMWARE-ERROR: LoadMetadata failed for " + name);
            return false;
        }
        if (!self_dec.DecryptData())
        {
            log("FIRMWARE-ERROR: DecryptData failed for " + name);
            return false;
        }
        std::vector<fs::file> decrypted = self_dec.MakeFile();
        if (decrypted.size() < 3)
        {
            log("FIRMWARE-ERROR: MakeFile size < 3 for " + name);
            return false;
        }
        tar_object dev_flash_tar(decrypted[2]);
        if (!dev_flash_tar.extract())
        {
            log("FIRMWARE-ERROR: extract failed for " + name);
            return false;
        }
        progress++;
        log("FIRMWARE-PROGRESS: " + std::to_string(progress) + "/" +
            std::to_string(update_filenames.size()) + " " + name);
    }

    update_files_f.close();
    log("FIRMWARE-EXTRACT: progress=" + std::to_string(progress) + "/" +
        std::to_string(update_filenames.size()) + " (re-init to refresh VFS, as main_window does)");
    try
    {
        Emu.Init();
    }
    catch (const std::exception& ex)
    {
        log(std::string("FIRMWARE-REINIT-EXCEPTION: ") + ex.what());
        return false;
    }
    catch (...)
    {
        log("FIRMWARE-REINIT-EXCEPTION: unknown");
        return false;
    }

    // Re-init (as main_window does) may reset config to defaults, so
    // re-apply the override before verifying, or the verify would read
    // the exe-dir dev_flash instead of the LocalState one just written.
    g_cfg_vfs.emulator_dir.set(slash_path(localStateUtf8) + "/");
    log(std::string("FIRMWARE-EMU-DIR re-applied after re-init: ") + slash_path(localStateUtf8) + "/");

    // Verification: file count + total bytes + vsh.self presence.
    const std::string df2 = g_cfg_vfs.get_dev_flash();
    const u64 file_count = count_files(df2);
    const u64 total_bytes = fs::get_dir_size(df2, 1);
    const bool vsh = fs::is_file(df2 + "/vsh/module/vsh.self");
    log("FIRMWARE-VERIFY: dev_flash files=" + std::to_string(file_count) +
        " bytes=" + std::to_string(total_bytes) +
        " vsh.self=" + (vsh ? "1" : "0") +
        " (PC 4.92 reference: 1075 files, 195070119 bytes)");
    const bool ok = (progress == update_filenames.size()) && vsh;
    log(std::string("FIRMWARE-VERDICT: ") + (ok ? "PASS" : "FAIL") +
        " (extracted all packages, vsh.self present; count/size vs PC reference in report)");
    return ok;
}

} // namespace emu_host
