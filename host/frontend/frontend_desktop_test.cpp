// frontend_desktop_test.cpp - PC pre-test for the CellBox frontend
// (built + run by build-frontend.ps1 BEFORE packaging; exit != 0
// blocks the package). Covers, on DESKTOP only:
//  1. PARAM.SFO parse: synthetic PSF built here from the on-disk
//     format (rpcs3/Loader/PSF.cpp structs) -> title/serial/category,
//     plus bad-magic and truncated negatives.
//  2. Library scans over a synthetic LocalState tree (disc folder,
//     games/*.iso, legacy root *.iso) and a synthetic USB root.
//  3. Menu pad edge/repeat/exit-combo state machine (synthetic WGI
//     readings through MenuPad::update_from_reading) + the
//     sync_to_current no-leak guarantee.
//  4. Screens state machine (library -> actions -> load/back).
//  5. Progress: counters, taskbar sink, percent precedence, "%" parse
//     (no-core build: test feeds stand in for the core listener; the
//     listener itself is link-verified in the UWP build only).
//  6. D3D12 UI smoke: HWND swapchain + 40 library/actions/loading/
//     compiling/stopping/delete-confirm frames.
//  7. F4 compile helpers: module enumeration over the synthetic
//     tree (disc-root + EBOOT-path shapes, ISO negative), the module
//     filename filter (incl. the core's libfs_utility_init.sprx
//     exclusion), PPU cache stats (per-title + whole-cache scopes),
//     and the notice -> current-module extraction.
//  8. 1.1.3: boot-target resolution (form names per entry shape),
//     F2 delete_entry (folder tree, EBOOT-path folder, legacy ISO +
//     staging parts, refusal set), the delete-confirm UI flow, and
//     the progress disc-mount capture.
//     Prints FRONTEND-DESKTOP-D3D UNAVAILABLE when DXGI has no output
//     (session 0) and the caller must run us in the interactive
//     session for that part (build-frontend.ps1 does, like 1044).
// No game, firmware or key content: every fixture is synthetic.
#include "frontend_library.h"
#include "frontend_compile.h"
#include "frontend_input.h"
#include "frontend_progress.h"
#include "frontend_ui.h"

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
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace fsx = std::filesystem;

static int g_failures = 0;
static void check(bool cond, const char* name)
{
    printf("FRONTEND-TEST %s: %s\n", name, cond ? "PASS" : "FAIL");
    if (!cond) ++g_failures;
}
static void tlog(const std::string& s) { printf("  %s\n", s.c_str()); }

// ---- minimal PSF writer (format per rpcs3/Loader/PSF.cpp) ----
static void put32(std::vector<std::uint8_t>& b, size_t off, std::uint32_t v)
{
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
    b[off + 2] = static_cast<std::uint8_t>(v >> 16);
    b[off + 3] = static_cast<std::uint8_t>(v >> 24);
}
static void put16(std::vector<std::uint8_t>& b, size_t off, std::uint16_t v)
{
    b[off] = static_cast<std::uint8_t>(v);
    b[off + 1] = static_cast<std::uint8_t>(v >> 8);
}
static std::vector<std::uint8_t> make_sfo(const std::string& title, const std::string& serial)
{
    struct E { std::string key, val; };
    const E entries[] = {{"CATEGORY", "DG"}, {"TITLE", title}, {"TITLE_ID", serial}};
    const std::uint32_t num = 3;
    const std::uint32_t offKeys = 20 + num * 16;
    std::string keyTab;
    std::uint32_t keyOff[3];
    for (int i = 0; i < 3; ++i)
    {
        keyOff[i] = static_cast<std::uint32_t>(keyTab.size());
        keyTab += entries[i].key;
        keyTab += '\0';
    }
    const std::uint32_t offData = offKeys + static_cast<std::uint32_t>(keyTab.size());
    std::string dataTab;
    std::uint32_t dataOff[3], dataLen[3];
    for (int i = 0; i < 3; ++i)
    {
        dataOff[i] = static_cast<std::uint32_t>(dataTab.size());
        dataLen[i] = static_cast<std::uint32_t>(entries[i].val.size()) + 1; // NTS
        dataTab += entries[i].val;
        dataTab += '\0';
    }
    std::vector<std::uint8_t> b(offData + dataTab.size(), 0);
    b[0] = 0; b[1] = 'P'; b[2] = 'S'; b[3] = 'F';
    put32(b, 4, 0x101);
    put32(b, 8, offKeys);
    put32(b, 12, offData);
    put32(b, 16, num);
    for (int i = 0; i < 3; ++i)
    {
        const size_t d = 20 + static_cast<size_t>(i) * 16;
        put16(b, d + 0, static_cast<std::uint16_t>(keyOff[i]));
        put16(b, d + 2, 0x0204);
        put32(b, d + 4, dataLen[i]);
        put32(b, d + 8, dataLen[i]);
        put32(b, d + 12, dataOff[i]);
    }
    memcpy(b.data() + offKeys, keyTab.data(), keyTab.size());
    memcpy(b.data() + offData, dataTab.data(), dataTab.size());
    return b;
}

static bool write_file(const fsx::path& p, const std::vector<std::uint8_t>& bytes)
{
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return o.good();
}

int main(int argc, char** argv)
{
    const bool d3dOnly = argc > 1 && std::string(argv[1]) == "--d3d-only";
    if (!d3dOnly)
    {
        // 1. SFO parse
        auto sfo = make_sfo("Synthetic Test Game", "TEST12345");
        std::string t, s, c;
        check(frontend::parse_param_sfo(sfo.data(), sfo.size(), t, s, c) &&
                  t == "Synthetic Test Game" && s == "TEST12345" && c == "DG",
              "sfo-parse");
        auto bad = sfo; bad[1] = 'X';
        check(!frontend::parse_param_sfo(bad.data(), bad.size(), t, s, c), "sfo-bad-magic");
        check(!frontend::parse_param_sfo(sfo.data(), 24, t, s, c), "sfo-truncated");

        // 2. Library scans over synthetic trees (under the exe dir arg or cwd).
        fsx::path base = (argc > 1 && std::string(argv[1]) != "--d3d-only")
            ? fsx::path(argv[1]) / "frontend-test-tree"
            : fsx::current_path() / "frontend-test-tree";
        std::error_code ec;
        fsx::remove_all(base, ec);
        fsx::create_directories(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "sub", ec);
        fsx::create_directories(base / "cache" / "TEST12345" / "ppu-deadbeef-EBOOT.BIN", ec);
        fsx::create_directories(base / "usbroot" / "UsbGame" / "PS3_GAME" / "USRDIR", ec);
        bool wrote = write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "PARAM.SFO", sfo);
        wrote &= write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN",
                            {0x7f, 'E', 'L', 'F'});
        // F4 module fixtures: two real modules, one excluded utility
        // SPRX (the core skips it too), one non-module file.
        wrote &= write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "libtest.sprx",
                            std::vector<std::uint8_t>(128, 0x53));
        wrote &= write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "sub" / "mod.prx",
                            std::vector<std::uint8_t>(64, 0x54));
        wrote &= write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "libfs_utility_init.sprx",
                            std::vector<std::uint8_t>(32, 0x55));
        wrote &= write_file(base / "games" / "SyntheticGame" / "PS3_GAME" / "USRDIR" / "readme.txt",
                            std::vector<std::uint8_t>(16, 0x56));
        wrote &= write_file(base / "cache" / "TEST12345" / "ppu-deadbeef-EBOOT.BIN" / "v1-ppu-cache.bin",
                            std::vector<std::uint8_t>(256, 0x57));
        wrote &= write_file(base / "games" / "some game.iso", std::vector<std::uint8_t>(1024, 0));
        wrote &= write_file(base / "legacy disc.iso", std::vector<std::uint8_t>(2048, 0));
        wrote &= write_file(base / "usbroot" / "UsbGame" / "PS3_GAME" / "PARAM.SFO",
                            make_sfo("USB Synthetic", "USBX99999"));
        wrote &= write_file(base / "usbroot" / "UsbGame" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN",
                            {0x7f, 'E', 'L', 'F'});
        check(wrote, "fixtures-written");

        const std::string lsUtf8 = base.u8string();
        auto entries = frontend::scan_localstate(lsUtf8, tlog);
        bool foundFolder = false, foundIso = false, foundLegacy = false;
        for (auto& e : entries)
        {
            if (e.sfo_ok && e.serial == "TEST12345" && e.title == "Synthetic Test Game" &&
                e.kind == frontend::GameKind::Folder && e.has_cache && e.size_bytes > 0)
                foundFolder = true;
            if (e.kind == frontend::GameKind::Iso && e.source == frontend::GameSource::LocalState)
                foundIso = true;
            if (e.kind == frontend::GameKind::Iso && e.source == frontend::GameSource::LocalIso &&
                e.size_bytes == 2048)
                foundLegacy = true;
        }
        check(entries.size() == 3 && foundFolder && foundIso && foundLegacy, "scan-localstate");
        auto usb = frontend::scan_usb_root((base / "usbroot").u8string(), lsUtf8, tlog);
        check(usb.size() == 1 && usb[0].source == frontend::GameSource::Usb &&
                  usb[0].serial == "USBX99999" && usb[0].sfo_ok,
              "scan-usb-root");
        auto none = frontend::scan_localstate((base / "does-not-exist").u8string(), tlog);
        check(none.empty(), "scan-missing-dir-empty");

        // 2b. F4 compile helpers: module enumeration (disc-root shape
        // from the scan + EBOOT-path shape built by hand), module
        // filename filter, PPU cache stats, notice -> module name.
        const frontend::GameEntry* folderEntry = nullptr;
        const frontend::GameEntry* isoEntry = nullptr;
        for (auto& e : entries)
        {
            if (e.kind == frontend::GameKind::Folder) folderEntry = &e;
            if (e.kind == frontend::GameKind::Iso && e.source == frontend::GameSource::LocalIso) isoEntry = &e;
        }
        check(folderEntry != nullptr && isoEntry != nullptr, "compile-fixtures-found");
        if (folderEntry)
        {
            auto men = frontend::enumerate_modules(*folderEntry, tlog);
            bool enumOk = men.ok && men.eboot_present && men.modules.size() == 3 &&
                          men.modules[0].rel == "USRDIR/EBOOT.BIN" &&
                          men.compile_dir.size() >= 8 &&
                          men.compile_dir.substr(men.compile_dir.size() - 8) == "PS3_GAME" &&
                          men.total_bytes == 4 + 128 + 64;
            check(enumOk, "compile-enumerate-disc-root");
            frontend::GameEntry shapeB = *folderEntry;
            shapeB.path = men.compile_dir + "/USRDIR/EBOOT.BIN";
            auto menB = frontend::enumerate_modules(shapeB, tlog);
            check(menB.ok && menB.compile_dir == men.compile_dir && menB.modules.size() == 3,
                  "compile-enumerate-eboot-path");
        }
        if (isoEntry)
        {
            auto menIso = frontend::enumerate_modules(*isoEntry, tlog);
            check(!menIso.ok && menIso.modules.empty(), "compile-enumerate-iso-none");
        }
        check(frontend::is_module_filename("libx.SPRX") &&
                  frontend::is_module_filename("a.prx") &&
                  frontend::is_module_filename("b.elf") &&
                  frontend::is_module_filename("c.self") &&
                  frontend::is_module_filename("d.mself") &&
                  !frontend::is_module_filename("libfs_utility_init.sprx") &&
                  !frontend::is_module_filename("readme.txt") &&
                  !frontend::is_module_filename("noext"),
              "compile-module-filter");
        auto csTitle = frontend::ppu_cache_stats(lsUtf8, "TEST12345", tlog);
        check(csTitle.dir_exists && csTitle.files == 1 && csTitle.bytes == 256,
              "compile-cache-stats-title");
        auto csWhole = frontend::ppu_cache_stats(lsUtf8, "ISO", tlog);
        check(csWhole.dir_exists && csWhole.files == 1, "compile-cache-stats-whole");
        auto csNone = frontend::ppu_cache_stats(lsUtf8, "MISSING999", tlog);
        check(!csNone.dir_exists && csNone.files == 0, "compile-cache-stats-missing");
        check(frontend::current_module_from_notice("Trying to load: D:/g/USRDIR/libfoo.sprx") == "libfoo.sprx" &&
                  frontend::current_module_from_notice("Trying to load binary: /a/b/EBOOT.BIN") == "EBOOT.BIN" &&
                  frontend::current_module_from_notice("Compiling PPU module...") == "" &&
                  frontend::current_module_from_notice("") == "",
              "compile-current-module");

        // 2c. FRONTEND (1.1.3): boot-target resolution - the path is
        // never rewritten (the desktop core experiment proved all
        // folder forms mount /dev_bdvd at the disc root); only the
        // form name differs per entry shape.
        if (folderEntry && isoEntry)
        {
            auto btRoot = frontend::resolve_boot_target(*folderEntry);
            frontend::GameEntry shapeB2 = *folderEntry;
            shapeB2.path = btRoot.path + "/PS3_GAME/USRDIR/EBOOT.BIN";
            auto btEboot = frontend::resolve_boot_target(shapeB2);
            frontend::GameEntry ps3dir = *folderEntry;
            ps3dir.path = btRoot.path + "/PS3_GAME";
            auto btDir = frontend::resolve_boot_target(ps3dir);
            auto btIso = frontend::resolve_boot_target(*isoEntry);
            check(btRoot.form == "disc-root" && btRoot.path == folderEntry->path &&
                      btEboot.form == "eboot-path" && btEboot.path == shapeB2.path &&
                      btDir.form == "ps3_game-dir" && btDir.path == ps3dir.path &&
                      btIso.form == "iso-file" && btIso.path == isoEntry->path,
                  "boot-target-forms");
        }

        // 2d. FRONTEND (1.1.3, F2): delete_entry over synthetic
        // trees - folder tree, PS3_GAME-shaped folder (EBOOT-path
        // entry), legacy ISO + its staging parts, and the refusal
        // set (outside games/, the games root itself, a .. escape,
        // a USB entry). The config/keep.txt sentinel must survive
        // every refusal.
        {
            fsx::path delBase = base;
            delBase += "-delete";
            fsx::remove_all(delBase, ec);
            const fsx::path dls = delBase / "ls";
            fsx::create_directories(dls / "games" / "FolderGame" / "PS3_GAME" / "USRDIR" / "sub", ec);
            fsx::create_directories(dls / "games" / "ShapeB" / "USRDIR", ec);
            fsx::create_directories(dls / "config", ec);
            bool dw = write_file(dls / "games" / "FolderGame" / "PS3_GAME" / "PARAM.SFO", sfo);
            dw &= write_file(dls / "games" / "FolderGame" / "PS3_GAME" / "USRDIR" / "EBOOT.BIN",
                             std::vector<std::uint8_t>(111, 0x61));
            dw &= write_file(dls / "games" / "FolderGame" / "PS3_GAME" / "USRDIR" / "sub" / "mod.prx",
                             std::vector<std::uint8_t>(222, 0x62));
            dw &= write_file(dls / "games" / "ShapeB" / "USRDIR" / "EBOOT.BIN",
                             std::vector<std::uint8_t>(33, 0x63));
            dw &= write_file(dls / "config" / "keep.txt", std::vector<std::uint8_t>(7, 0x64));
            dw &= write_file(dls / frontend::kStagingIsoName, std::vector<std::uint8_t>(4096, 0x65));
            for (int i = 0; i < frontend::kStagingPartCount; ++i)
                dw &= write_file(dls / (std::string(frontend::kStagingPartBase) + ".part" + std::to_string(i)),
                                 std::vector<std::uint8_t>(100, 0x66));
            check(dw, "delete-fixtures-written");
            const std::string dlsUtf8 = dls.u8string();

            frontend::GameEntry fe;
            fe.title = "Folder Game"; fe.serial = "FOLD00001";
            fe.source = frontend::GameSource::LocalState;
            fe.kind = frontend::GameKind::Folder;
            fe.path = (dls / "games" / "FolderGame").u8string();
            auto dres = frontend::delete_entry(fe, dlsUtf8, tlog);
            check(dres.ok && !dres.refused && dres.files_removed == 3 &&
                      dres.bytes_removed == sfo.size() + 111 + 222 &&
                      !fsx::exists(dls / "games" / "FolderGame"),
                  "delete-folder-tree");

            frontend::GameEntry sb = fe;
            sb.title = "Shape B"; sb.serial = "SHAP00002";
            sb.path = (dls / "games" / "ShapeB" / "USRDIR" / "EBOOT.BIN").u8string();
            dres = frontend::delete_entry(sb, dlsUtf8, tlog);
            check(dres.ok && dres.files_removed == 1 &&
                      !fsx::exists(dls / "games" / "ShapeB"),
                  "delete-folder-shapeb");

            frontend::GameEntry le;
            le.title = "Legacy"; le.serial = "ISO";
            le.source = frontend::GameSource::LocalIso;
            le.kind = frontend::GameKind::Iso;
            le.path = (dls / frontend::kStagingIsoName).u8string();
            dres = frontend::delete_entry(le, dlsUtf8, tlog);
            bool partsGone = !fsx::exists(dls / frontend::kStagingIsoName);
            for (int i = 0; i < frontend::kStagingPartCount; ++i)
                partsGone = partsGone &&
                    !fsx::exists(dls / (std::string(frontend::kStagingPartBase) + ".part" + std::to_string(i)));
            check(dres.ok && dres.files_removed == 1 + (std::uint32_t)frontend::kStagingPartCount &&
                      dres.bytes_removed == 4096 + 400 && partsGone,
                  "delete-iso-parts");

            frontend::GameEntry outside = fe;
            outside.path = (dls / "config").u8string();
            dres = frontend::delete_entry(outside, dlsUtf8, tlog);
            bool refusedOk = dres.refused && !dres.ok && fsx::exists(dls / "config" / "keep.txt");
            frontend::GameEntry gamesRoot = fe;
            gamesRoot.path = (dls / "games").u8string();
            dres = frontend::delete_entry(gamesRoot, dlsUtf8, tlog);
            refusedOk = refusedOk && dres.refused && fsx::exists(dls / "games");
            frontend::GameEntry escape = fe;
            escape.path = dlsUtf8 + "/games/../config";
            dres = frontend::delete_entry(escape, dlsUtf8, tlog);
            refusedOk = refusedOk && dres.refused && fsx::exists(dls / "config" / "keep.txt");
            frontend::GameEntry usbE = fe;
            usbE.source = frontend::GameSource::Usb;
            usbE.path = (dls / "games").u8string();
            dres = frontend::delete_entry(usbE, dlsUtf8, tlog);
            refusedOk = refusedOk && dres.refused && fsx::exists(dls / "games");
            check(refusedOk, "delete-refusals");
            fsx::remove_all(delBase, ec);
        }

        // 3. Menu pad state machine. Bits: A=4 B=8 DUp=0x40 DDown=0x80
        // Menu=1 View=2.
        frontend::MenuPad pad;
        auto in0 = pad.update_from_reading(0x80, 0, 0, 1000, true);
        auto in1 = pad.update_from_reading(0x80, 0, 0, 1100, true);  // held, no repeat yet
        auto in2 = pad.update_from_reading(0x80, 0, 0, 1400, true);  // repeat after 350 ms
        auto in3 = pad.update_from_reading(0x84, 0, 0, 1500, true);  // +A edge
        auto in4 = pad.update_from_reading(0x84, 0, 0, 1600, true);  // A held: no new edge
        check(in0.down && !in1.down && in2.down && in3.accept && !in4.accept, "pad-edges-repeat");
        pad.sync_to_current();
        auto in5 = pad.update_from_reading(0x84, 0, 0, 1700, true);  // still held after sync
        check(!in5.accept && !in5.down, "pad-sync-no-leak");
        auto in6 = pad.update_from_reading(0, 0, -0.9, 1800, true);  // stick down
        check(in6.down, "pad-stick-down");
        frontend::MenuPad pad2;
        auto c0 = pad2.update_from_reading(0x3, 0, 0, 5000, true);   // View+Menu down
        auto c1 = pad2.update_from_reading(0x3, 0, 0, 5900, true);   // >=800 ms
        auto c2 = pad2.update_from_reading(0x3, 0, 0, 6000, true);   // fired once
        check(!c0.exit_combo && c1.exit_combo && !c2.exit_combo, "pad-exit-combo");
        auto d0 = pad2.update_from_reading(0x3, 0, 0, 6100, false);  // disconnect
        check(!d0.connected && !d0.exit_combo, "pad-disconnect-clean");

        // 4. Screens state machine.
        frontend::UiState st;
        frontend::MenuInput mi{};
        mi.down = true;
        frontend::ui_apply_input(st, mi, 3);
        frontend::ui_apply_input(st, mi, 3); // selected 2
        mi = {}; mi.up = true;
        frontend::ui_apply_input(st, mi, 3); // selected 1
        mi = {}; mi.accept = true;
        auto ev = frontend::ui_apply_input(st, mi, 3);
        bool actionsOk = st.screen == frontend::Screen::Actions && st.selected == 1 &&
                         ev == frontend::UiEvent::None;
        ev = frontend::ui_apply_input(st, mi, 3); // Load
        check(actionsOk && ev == frontend::UiEvent::LoadSelected, "ui-actions-load");
        frontend::UiState st2;
        st2.screen = frontend::Screen::Actions;
        mi = {}; mi.down = true;
        frontend::ui_apply_input(st2, mi, 1); // Compile
        mi = {}; mi.accept = true;
        ev = frontend::ui_apply_input(st2, mi, 1);
        bool f4Ok = ev == frontend::UiEvent::CompileSelected;
        mi = {}; mi.back = true;
        frontend::ui_apply_input(st2, mi, 1);
        check(f4Ok && st2.screen == frontend::Screen::Library, "ui-actions-compile-and-back");
        frontend::UiState st3;
        st3.screen = frontend::Screen::Compiling;
        mi = {}; mi.accept = true;
        check(frontend::ui_apply_input(st3, mi, 1) == frontend::UiEvent::None &&
                  st3.screen == frontend::Screen::Compiling,
              "ui-compiling-input-ignored");
        // 1.1.2: the Stopping screen (teardown wait) must also swallow
        // all input - no pad press may escape the wait back into the
        // library state machine while the core is still joining.
        frontend::UiState st4;
        st4.screen = frontend::Screen::Stopping;
        mi = {}; mi.accept = true;
        bool stopIgnored = frontend::ui_apply_input(st4, mi, 1) == frontend::UiEvent::None &&
                           st4.screen == frontend::Screen::Stopping;
        mi = {}; mi.back = true;
        stopIgnored = stopIgnored && frontend::ui_apply_input(st4, mi, 1) == frontend::UiEvent::None &&
                      st4.screen == frontend::Screen::Stopping;
        mi = {}; mi.down = true;
        stopIgnored = stopIgnored && frontend::ui_apply_input(st4, mi, 1) == frontend::UiEvent::None &&
                      st4.screen == frontend::Screen::Stopping;
        check(stopIgnored, "ui-stopping-input-ignored");
        // 1.1.3 (F2): Actions -> Delete opens the confirm screen
        // (DeleteSelected); B backs out to Actions; A on the
        // confirm fires DeleteConfirmed; navigation is ignored
        // while the confirm is up.
        frontend::UiState st5;
        st5.screen = frontend::Screen::Actions;
        mi = {}; mi.down = true;
        frontend::ui_apply_input(st5, mi, 1); // action 1 (Compile)
        frontend::ui_apply_input(st5, mi, 1); // action 2 (Delete)
        mi = {}; mi.accept = true;
        ev = frontend::ui_apply_input(st5, mi, 1);
        bool delFlow = ev == frontend::UiEvent::DeleteSelected &&
                       st5.screen == frontend::Screen::DeleteConfirm;
        mi = {}; mi.down = true;
        delFlow = delFlow && frontend::ui_apply_input(st5, mi, 1) == frontend::UiEvent::None &&
                  st5.screen == frontend::Screen::DeleteConfirm;
        mi = {}; mi.back = true;
        delFlow = delFlow && frontend::ui_apply_input(st5, mi, 1) == frontend::UiEvent::None &&
                  st5.screen == frontend::Screen::Actions;
        mi = {}; mi.accept = true;
        delFlow = delFlow && frontend::ui_apply_input(st5, mi, 1) == frontend::UiEvent::DeleteSelected &&
                  st5.screen == frontend::Screen::DeleteConfirm;
        mi = {}; mi.accept = true;
        delFlow = delFlow && frontend::ui_apply_input(st5, mi, 1) == frontend::UiEvent::DeleteConfirmed;
        check(delFlow, "ui-delete-confirm-flow");
        mi = {}; mi.menu = true;
        check(frontend::ui_apply_input(st2, mi, 0) == frontend::UiEvent::RescanRequested,
              "ui-library-rescan");

        // 5. Progress plumbing (no-core feeds).
        frontend::progress_install(tlog);
        frontend::progress_reset();
        frontend::progress_test_feed(frontend::kLevelNotice, "PPU", "Compiling module x");
        frontend::progress_test_feed(frontend::kLevelNotice, "PPU", "Progress: 42%");
        frontend::progress_test_feed(frontend::kLevelTrace, "RSX", "trace line");
        frontend::progress_test_feed(frontend::kLevelError, "SYS", "some error");
        frontend::ProgressSnapshot snap;
        frontend::progress_snapshot(snap);
        check(snap.counts[frontend::kLevelNotice] == 2 &&
                  snap.counts[frontend::kLevelTrace] == 1 &&
                  snap.counts[frontend::kLevelError] == 1 && snap.total == 4 &&
                  snap.last_notice == "Progress: 42%" &&
                  snap.percent == 42 && snap.percent_source == "text",
              "progress-counters-text-percent");
        frontend::progress_taskbar(2, 200); // set_limit
        frontend::progress_taskbar(3, 50);  // set_value
        frontend::progress_taskbar(1, 10);  // increment -> 60/200 = 30
        frontend::progress_snapshot(snap);
        check(snap.percent == 30 && snap.percent_source == "taskbar", "progress-taskbar-percent");
        frontend::progress_test_set_core(3, 4, 0, 0, "Compiling PPU module...");
        frontend::progress_snapshot(snap);
        check(snap.percent == 75 && snap.percent_source == "core-progress" &&
                  snap.core_text == "Compiling PPU module...",
              "progress-core-percent-wins");
        // 1.1.3: disc-mount capture - VFS mount lines for bdvd /
        // app_home and SYS Disc:/Elf lines are recorded in order;
        // unrelated lines are not; reset clears the capture.
        frontend::progress_reset();
        frontend::progress_test_feed(frontend::kLevelNotice, "VFS",
                                     "Mounted path \"/dev_bdvd\" to \"X:/games/TestGame/\"");
        frontend::progress_test_feed(frontend::kLevelNotice, "VFS",
                                     "Mounted path \"/dev_hdd0\" to \"X:/dev_hdd0/\"");
        frontend::progress_test_feed(frontend::kLevelNotice, "SYS", "Disc: X:/games/TestGame/");
        frontend::progress_test_feed(frontend::kLevelNotice, "SYS",
                                     "Elf path: /dev_bdvd/PS3_GAME/USRDIR/EBOOT.BIN");
        frontend::progress_test_feed(frontend::kLevelNotice, "PPU", "unrelated notice");
        auto mountLines = frontend::progress_mount_lines();
        bool mountOk = mountLines.size() == 3 &&
            mountLines[0].find("dev_bdvd") != std::string::npos &&
            mountLines[1].find("Disc:") == 0 &&
            mountLines[2].find("Elf path:") == 0;
        frontend::progress_reset();
        mountOk = mountOk && frontend::progress_mount_lines().empty();
        check(mountOk, "progress-mount-capture");
        check(frontend::progress_parse_percent("nothing here") == -1 &&
                  frontend::progress_parse_percent("at 99.6% done") == 100 &&
                  frontend::progress_parse_percent("100% legit") == 100,
              "progress-parse-percent");
        fsx::remove_all(base, ec);
    }

    // 6. D3D12 UI smoke.
    {
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"FrontendDesktopTest";
        RegisterClassW(&wc);
        HWND hwnd = CreateWindowW(wc.lpszClassName, L"CellBox frontend test",
                                  WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
                                  nullptr, nullptr, wc.hInstance, nullptr);
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGISwapChain1> swap;
        HRESULT hrDev = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        HRESULT hrSwap = E_FAIL;
        if (SUCCEEDED(hrDev) && hwnd)
        {
            D3D12_COMMAND_QUEUE_DESC q{};
            q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            dev->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
            CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 1280; sd.Height = 720;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.SampleDesc.Count = 1;
            if (factory && queue)
                hrSwap = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &swap);
        }
        if (FAILED(hrDev) || FAILED(hrSwap) || !swap)
        {
            printf("FRONTEND-DESKTOP-D3D: UNAVAILABLE device=0x%08lX swapchain=0x%08lX (run in the interactive session for the D3D smoke)\n",
                   (unsigned long)hrDev, (unsigned long)hrSwap);
            if (d3dOnly) ++g_failures; // in d3d-only mode unavailable IS a failure
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
            frontend::FrontendUi ui;
            bool ok = false;
            std::string log = ui.init(dev.Get(), queue.Get(), swap.Get(), &ok);
            printf("%s", log.c_str());
            check(ok, "d3d-ui-init");
            if (ok)
            {
                frontend::GameEntry e1;
                e1.title = "Synthetic Test Game"; e1.serial = "TEST12345";
                e1.path = "X:/synthetic"; e1.size_bytes = 4599644160ull;
                e1.has_cache = true; e1.sfo_ok = true;
                frontend::GameEntry e2 = e1;
                e2.title = "USB Synthetic"; e2.serial = "USBX99999";
                e2.source = frontend::GameSource::Usb;
                std::vector<frontend::GameEntry> entries = {e1, e2};
                frontend::UiState st;
                frontend::ProgressSnapshot snap;
                snap.percent = 42; snap.percent_source = "test";
                snap.counts[frontend::kLevelNotice] = 7; snap.total = 7;
                snap.last_notice = "Compiling module x";
                snap.core_fdone = 1; snap.core_ftotal = 3;
                frontend::CompileView cview;
                cview.route = "Route: core directory-scan precompile (PPU cache pre-bake)";
                cview.modules_total = 3;
                cview.cache_files = 1;
                cview.cache_bytes = 256;
                cview.current_module = "libtest.sprx";
                cview.elapsed_ms = 42000;
                HRESULT last = S_OK;
                for (int f = 0; f < 40; ++f)
                {
                    MSG msg{};
                    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                    if (f < 10) frontend::ui_render_library(ui, st, entries, "desktop smoke");
                    else if (f < 20) frontend::ui_render_actions(ui, st, e1);
                    else if (f < 30) frontend::ui_render_loading(ui, e1.title, snap, (unsigned long long)f);
                    else if (f < 35) frontend::ui_render_compiling(ui, e1.title, cview, snap, (unsigned long long)f);
                    else if (f < 38) frontend::ui_render_stopping(ui, e1.title, "stage: awaiting-on-stop - waiting for the emulation join to finish", 4200, (unsigned long long)f);
                    else frontend::ui_render_delete_confirm(ui, st, e1);
                    last = ui.present_frame();
                    if (FAILED(last)) break;
                }
                printf("FRONTEND-DESKTOP-D3D presented=%llu last=0x%08lX deviceRemoved=0x%08lX\n",
                       ui.frames_presented(), (unsigned long)last,
                       (unsigned long)dev->GetDeviceRemovedReason());
                check(SUCCEEDED(last) && ui.frames_presented() == 40 &&
                          SUCCEEDED(dev->GetDeviceRemovedReason()),
                      "d3d-ui-40-frames");
            }
        }
        if (hwnd) DestroyWindow(hwnd);
    }

    printf("FRONTEND-DESKTOP-TEST: %s failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
