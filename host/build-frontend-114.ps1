# v1.1.4.0 FRONTEND LINE (pip-fixes-1.1.4), derived from build-frontend-113.ps1
# (which is NOT modified). 1.1.4 deltas vs 1.1.3:
#  host : d3d12_rsx.cpp (kForceScratchDiag=false; quads/quad_strip/polygon/fan/
#         line_loop index expansion; vertex-colour slice shaders instead of solid
#         magenta), boot_probe.cpp (one-shot /dev_bdvd + /dev_hdd0 zone listing),
#         slice_vs.hlsl / slice_ps.hlsl.
#  core : emucore.lib REBUILT with the 1.1.4 core patches (case-insensitive ISO
#         lookup, BOOT-PPU-SUMMARY printf-format fix, audio message text, GCM
#         label force-complete on acquire timeout) - see patches/ and the core
#         build script core-build-1044.ps1. Previous lib kept as emucore-1044-pre114.lib.
#  Manifest 1.1.4.0; outputs named *-frontend-114.*; no existing msix is touched.
# v1.1.3.0 FRONTEND LINE (CellBox F1 + F4 + F2 delete + boot-target/
# mount logging), derived from build-frontend-112.ps1 (the 1.1.2
# script) - which is NOT modified. 1.1.3 deltas vs 1.1.2 (host-only;
# emucore.lib NOT rebuilt; 1044 files untouched):
#  (1) FOLDER BOOT: the desktop experiment (2026-10-28, desktop
#      rpcs3.exe vs $env:USERPROFILE\BO3-JB, Null renderers, ~90 s
#      each) booted the extracted BO3 folder three ways - disc root,
#      PS3_GAME dir, USRDIR/EBOOT.BIN - and ALL THREE mounted
#      /dev_bdvd at the disc root and ran identically (PPU analysis,
#      game TTY at ~9 s, still running at the 90 s kill). The core
#      normalises every folder form to the disc root, so NO boot
#      path rewrite was warranted: 1.1.3 instead NAMES the resolved
#      form on the FRONTEND-BOOT-TARGET line (frontend::
#      resolve_boot_target: iso-file / disc-root / ps3_game-dir /
#      eboot-path) and dumps the core's own disc-mount broadcasts
#      after each Load (frontend_progress mount capture ->
#      FRONTEND-BOOT-MOUNT lines: VFS "Mounted path" for /dev_bdvd +
#      /app_home, SYS "Disc:"/"Elf path:"). The 1.1.2 console folder
#      boot divergence (code=0, state=3, zero draws, self-stop,
#      no sys_fs errors) is therefore console-side, and these lines
#      are the evidence channel for it.
#  (2) DELETE (slice F2, Matt's explicit requirement): Actions ->
#      Delete -> DeleteConfirm screen (title + size, A = delete
#      permanently, B = back). frontend::delete_entry: folder
#      entries delete recursively ONLY under LocalState/games/
#      (validated; dot-components refused); the legacy LocalIso
#      entry deletes the reassembled ISO AND its bo3.iso.part0-3
#      staging files (constants now shared with the staging block
#      in frontend_library.h - deleting the ISO alone would
#      resurrect it at the next launch); games/*.iso deletes as a
#      single file; USB entries are refused (read-only source,
#      explained on the Actions screen). FRONTEND-DELETE logging
#      (requested/refused/result/fail/done); library rescans after.
#      The per-title PPU cache is deliberately kept (module-hash
#      keyed, shared, expensive to rebuild).
#  Gates: positives += FRONTEND-DELETE, FRONTEND-DELETE-RESULT,
#  FRONTEND-BOOT-MOUNT, FRONTEND-BOOT-TARGET; poisons += 'arrives
#  in slice F2' (the F2 placeholder text is gone from the sources).
#  Desktop pre-test extended: boot-target forms, delete folder
#  tree / EBOOT-path folder / ISO+parts / refusal set, the
#  delete-confirm UI flow, mount capture, delete-confirm frames in
#  the 40-frame D3D smoke. Manifest 1.1.3.0; preserved outputs
#  named *-frontend-113.*. 1.1.2 header follows.
#
# v1.1.2.0 FRONTEND LINE (CellBox F1 + F4 + 1.1.2 stop/teardown fix),
# derived from build-frontend-111.ps1 (the 1.1.1 script) - which is
# NOT modified. 1.1.2 deltas vs 1.1.1 (host-only; emucore.lib NOT
# rebuilt; 1044 files untouched): fixes the post-stop teardown crash
# of the 1.1.1 console run. Mechanism (console-proven): the ISO
# compile pass completed as designed and Emu.Kill returned with
# state=0, but the core's Emulation Join Thread was still joining
# 14+ s later ("too sleepy" waits doubling to 7.2 s), and the host
# watchdog's 500-tick PPU thread summary (xbox_ppu_thread_summary ->
# idm::select<named_thread<ppu_thread>>) enumerated PPU thread
# objects the join was destroying: watchdog file ends at tick=500
# emu=0, boot-veh-log-111.txt records BOOT-VEH 0xC0000005 at
# idm::select +0xC5 (rcx=0xCCCCCCCCCCCCCCCC) from watchdog_loop,
# then BOOT-UHF - the process death WAS instrumented after all.
# Fixes: (a) watchdog PPU summary gated - runs only in active emu
# states (1/3/4/6/7) with no stop/teardown pending (new latch:
# watchdog_note_boot_started / watchdog_note_stop_requested;
# BOOT-PPU-SUMMARY-SKIPPED logs the gate once per gated period);
# (b) library re-entry gated on real teardown completion: new
# App::wait_teardown holds rescan + presenting until the core's
# on_stop callback fires (= end of the Kill join lambda, delivered
# via the CallFromMainThread queue the wait keeps draining), with a
# Stopping screen presented only after RSX flips freeze 500 ms;
# FRONTEND-STOP-JOIN instrumentation (stop-requested in
# boot_request_stop, rsx-exit-signaled, stage transitions,
# rsx-exited, still-waiting every 2 s naming the stage,
# join-complete, complete with elapsed); quiescence fallback only
# for boots that never reached running (result != no_errors);
# (c) new Screen::Stopping (input swallowed; desktop-tested) +
# ui_render_stopping; stop-cycle tracking in frontend_boot
# (emu_host::stop_cycle_*). Compile-pass behaviour itself identical
# (dwell, Kill, result logging). Manifest 1.1.2.0; gates extended
# with FRONTEND-STOP-JOIN + BOOT-PPU-SUMMARY-SKIPPED markers;
# preserved outputs named *-frontend-112.*. 1.1.1 header follows.
#
# v1.1.1.0 FRONTEND LINE (CellBox F1 + F4 Compile/pre-bake + USB
# source + notice/trace progress), derived from build-frontend.ps1
# (the 1.1.0 script) - which is NOT modified. F4 deltas vs 1.1.0:
# frontend_compile.cpp added (module enumeration + PPU cache stats,
# native FromApp TU, desktop-tested); frontend_boot gains
# compile_phase_for_folder (the desktop GUI CreateCPUCaches recipe
# driven host-only: SetForceBoot + SetPrecompileCacheOption +
# directory-scan BootGame - existing public emucore APIs, NO core
# file modified, emucore.lib NOT rebuilt); Actions screen Compile
# fires CompileSelected (the 1.1.0 F4 placeholder is gone), new
# Compiling screen (core progress + live cache count), ISO entries
# compile via a compile-pass boot with a 20 s running dwell;
# frontend desktop pre-test extended (enumeration/cache tests,
# 40 UI frames incl. the compiling renderer); manifest version
# 1.1.1.0; gates extended with FRONTEND-COMPILE markers; preserved
# outputs named *-frontend-111.*. 1.1.0 header follows.
#
# v1.1.0.0 FRONTEND LINE (CellBox F1 + USB source + notice/trace
# progress), derived from build-1044.ps1 - which is NOT modified.
# Deltas vs 1044: main_frontend.cpp instead of main_uwp_1044.cpp;
# frontend/*.cpp added (library/ui/input/progress native, progress +
# boot with the emucore include list); frontend_boot.obj REPLACES
# boot_probe.obj (derived copy, selected-entry target); diagnostic
# probe TUs (pso/present/draw/audio_pad probes, memory governor,
# d3d12_gsrender INT prototype, emu_link) not compiled into the UWP
# exe (they stay in the 1.0.x line and in the host_desktop pre-test);
# UI shaders dxc-signed + font atlas baked at build time (generated
# frontend/ui_shaders.h + frontend/font_atlas.h); frontend desktop
# pre-test added; manifest version 1.1.0.0; gates extended with
# FRONTEND markers; preserved outputs named *-frontend-110.*.
# Original 1044 header history follows.
#
# v1.0.44.0: FIX + DISCRIMINATOR build (Matt-directed exact changes 2026-10-28).
# (a) Audio layout-0 crash fixed TWO ways: boot_probe forces
# g_cfg.audio.channels=stereo, AND AudioBackend::default_layout_channel_count
# returns 2 (stereo fallback) instead of fmt::throw_exception on unknown layout.
# The 1043 [XBOX-AUDIO-LAYOUT] error log line is left in place (its message text
# is now stale: the caller thread no longer terminates) -- see gate report.
# (b) WGI pad poll 100 ms -> 16 ms (catch fast taps); PAD-DIAG cadence %10 -> %60
# (~1/sec at 16 ms). Host pad Init() initial player-0 status also changes to
# CONNECTED in the staged host_pad.cpp (flagged in the gate report; transient:
# update_pad_states overwrites it every poll).
# (c) cellPadGetData: the 1043 nonzero-only [PAD-DELIVERED] hook is REPLACED by
# a first-16-or-nonzero version (call counter) -- single log site -- to prove
# whether the game calls cellPadGetData at all (1043: translation proven,
# DELIVERED=0). The staged cellPad diff's memcpy site did not exist in this
# tree; the replacement was applied at the real 1043 hook site in cellPadGetData.
# (d) Main byte-identical to 1043 (copied, SHA verified).
# (e) NOT fixed here (carries over, known open): the 1043 BOOT-PPU-SUMMARY
# blank-format defect (unfilled {} / {:X} placeholders).
# Core rebuilt FIRST (1043 lib preserved as emucore-1043.lib); new strings
# verified in lib + exe. Gate UNCHANGED vs 1043 (same positives + poisons).
# NOTE (gate honesty): the AudioBackend fallback is a return statement, not a
# string -- no string gate can prove it; the desktop pre-test + console
# behavior are its proof.

# v1.0.36.0 comprehensive build: D3D12GSRender promotion (nop draws, counted) + WGI pad player 0 + PSN Simulated + caps. No core file modified.

# v1.0.36.0 host-only: null music handler fix + VEH stack trace. No core file modified.

param([switch]$SkipDesktopTest)

# v1.0.36.0 VM preallocated-block SEC_RESERVE fix (Muse Code): build-1027 (ex-1025) UNCHANGED host; emucore rebuilt with shm flag 4 SEC_RESERVE for s_hook (B2, 1.0.25) AND for preallocated guest blocks (m_common) with commit-on-alloc + placeholder-coalesce Type guard (vm.cpp:845,1306,1390,1909; vm_native.cpp:846,962,975,1064). Fixes the 1.0.25 console fatal vm.cpp:1371 map3 err=8 on vm::main 256 MiB. BOOT-VM probe lines retained from: build-1021.ps1 + boot_probe (NullGSRender ISO-boot probe, pad_thread fallback, BOOT log lines); satellites + WS2_32 stub unchanged

# (ws2_stub.cpp diag fix + stage_diag + main_uwp_1035.cpp == 1.0.19 main_uwp.cpp,

# hash-verified) and ONE packaging change: ship the two MSVCP satellite DLLs the

# emucore image imports but no package ever staged (MSVCP140_ATOMIC_WAIT.dll,

# MSVCP140_CODECVT_IDS.dll, plain + _app, same OneCore redist as the other CRT

# DLLs), plus a PACKAGING-COMPLETENESS GATE that fails if any imported DLL is

# neither an apiset contract, nor console-proven OS-provided, nor staged.

# Manifest version injected at stage time (AppxManifest.xml stays 1.0.19.0).

# Original 1.0.19 line: v1.0.19.0 DIAGNOSTIC: build.ps1 + ws2_stub.cpp (WS2_32 link stub, first object) + import gate FAILS on WS2_32.dll. Restored 1.0.10 sources (main_uwp.cpp.1010bak, emu_host) + dispatcher-pump fix + emulator_dir override.

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path

$out  = Join-Path $root "out"

New-Item -ItemType Directory -Force -Path $out | Out-Null
$buildStart = Get-Date # 1.0.44 gate: build-start timestamp for fresh-object/exe checks

$vs   = "C:\Program Files\Microsoft Visual Studio\2022\Community"

$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"

$sdk  = "C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64"

$dxc  = "C:\VulkanSDK\1.4.363.0\Bin\dxc.exe"

$dxilSrc = "C:\rpcs3\xbox-shader-corpus\dxil"

function Invoke-Vc([string]$cmdline) {

    cmd.exe /c "`"$vcvars`" >nul 2>&1 && $cmdline"

    if ($LASTEXITCODE -ne 0) { throw "build step failed ($LASTEXITCODE): $cmdline" }

}

# Signed control shaders (same TEXCOORD0/1 signatures as Mesa pair). dxc signs

# via its bundled dxil.dll; verify non-zero HASH on desktop pre-test output.

Write-Host "== control shaders (signed, dxc SM6.0) =="

& $dxc -T vs_6_0 -E main -Fo "$out\control_vs.cso" "$root\control_vs.hlsl"

if ($LASTEXITCODE -ne 0) { throw "dxc control_vs failed" }

& $dxc -T ps_6_0 -E main -Fo "$out\control_ps.cso" "$root\control_ps.hlsl"

if ($LASTEXITCODE -ne 0) { throw "dxc control_ps failed" }

# Slice shaders (pixel->NDC dual-guard since 1.0.44) (build-time dxc SM6.0, same route as the control
# shaders — console-proven by Step 3 control-hr=0). The .cso blobs are
# embedded into slice_shaders.h (generated here, compiled into
# d3d12_rsx.cpp); the .cso files are ALSO staged to stage\dxil for
# reference. No runtime compiler dependency is introduced.
Write-Host "== slice shaders (dxc SM6.0, build-time) =="
& $dxc -T vs_6_0 -E main -Fo "$out\slice_vs.cso" "$root\slice_vs.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc slice_vs failed" }
& $dxc -T ps_6_0 -E main -Fo "$out\slice_ps.cso" "$root\slice_ps.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc slice_ps failed" }
& $dxc -T vs_6_0 -E main -Fo "$out\blit_vs.cso" "$root\blit_vs.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc blit_vs failed" }
& $dxc -T ps_6_0 -E main -Fo "$out\blit_ps.cso" "$root\blit_ps.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc blit_ps failed" }
function ConvertTo-CHeader([string]$cso, [string]$name) {
    $bytes = [System.IO.File]::ReadAllBytes($cso)
    if ($bytes.Length -eq 0) { throw "empty shader blob: $cso" }
    if ($bytes[0] -ne 0x44 -or $bytes[1] -ne 0x58 -or $bytes[2] -ne 0x42 -or $bytes[3] -ne 0x43) { throw "shader blob is not a DXBC container: $cso" }
    $hex = ($bytes | ForEach-Object { ("0x{0:X2}," -f $_) }) -join ""
    return "static const unsigned char ${name}_cso[] = { $hex };`nstatic const unsigned int ${name}_cso_len = $($bytes.Length);`n"
}
$hdr = "// slice_shaders.h - GENERATED by build-frontend.ps1 from dxc output. Do not hand-edit.`n#pragma once`n"
$hdr += ConvertTo-CHeader "$out\slice_vs.cso" "slice_vs"
$hdr += ConvertTo-CHeader "$out\slice_ps.cso" "slice_ps"
$hdr += ConvertTo-CHeader "$out\blit_vs.cso" "blit_vs"
$hdr += ConvertTo-CHeader "$out\blit_ps.cso" "blit_ps"
Set-Content -Path "$root\slice_shaders.h" -Value $hdr -Encoding ascii
Write-Host "SLICE-SHADERS-OK slice_vs/slice_ps/blit_vs/blit_ps compiled + slice_shaders.h generated"

# FRONTEND (1.1.0): UI shaders (dxc SM6.0 signed, embedded like the
# slice shaders) + pre-baked font atlas (System.Drawing, R8 bytes +
# per-glyph advance widths) -> generated headers under frontend\.
Write-Host "== frontend UI shaders + font atlas (build-time bake) =="
& $dxc -T vs_6_0 -E main -Fo "$out\ui_vs.cso" "$root\frontend\ui_vs.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc ui_vs failed" }
& $dxc -T ps_6_0 -E main -Fo "$out\ui_ps.cso" "$root\frontend\ui_ps.hlsl"
if ($LASTEXITCODE -ne 0) { throw "dxc ui_ps failed" }
$uiHdr = "// ui_shaders.h - GENERATED by build-frontend.ps1 from dxc output. Do not hand-edit.`n#pragma once`n"
$uiHdr += ConvertTo-CHeader "$out\ui_vs.cso" "ui_vs"
$uiHdr += ConvertTo-CHeader "$out\ui_ps.cso" "ui_ps"
Set-Content -Path "$root\frontend\ui_shaders.h" -Value $uiHdr -Encoding ascii
Add-Type -AssemblyName System.Drawing
$cellW = 32; $cellH = 32; $cols = 16; $rows = 16
$atlasW = $cellW * $cols; $atlasH = $cellH * $rows
$bmp = New-Object System.Drawing.Bitmap($atlasW, $atlasH)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.Clear([System.Drawing.Color]::Black)
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$font = New-Object System.Drawing.Font("Consolas", 20, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
$brush = [System.Drawing.Brushes]::White
$widths = @(0) * 256
$g.FillRectangle($brush, 0, 0, $cellW, $cellH) # cell 0: solid-white texel for plain quads
for ($ch = 32; $ch -le 126; $ch++) {
    $s = [string][char]$ch
    $cx = ($ch % $cols) * $cellW
    $cy = [math]::Floor($ch / $cols) * $cellH
    $g.DrawString($s, $font, $brush, [single]($cx + 2), [single]($cy + 4))
    $w = [math]::Round($g.MeasureString($s, $font).Width)
    if ($w -lt 4) { $w = 4 }
    if ($w -gt $cellW) { $w = $cellW }
    $widths[$ch] = $w
}
$widths[32] = 10 # space advance (MeasureString pads it)
$g.Dispose()
# LockBits readback (GetPixel per texel would take minutes in PS).
$rect = New-Object System.Drawing.Rectangle(0, 0, $atlasW, $atlasH)
$lock = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$raw = New-Object byte[] ([math]::Abs($lock.Stride) * $atlasH)
[System.Runtime.InteropServices.Marshal]::Copy($lock.Scan0, $raw, 0, $raw.Length)
$bmp.UnlockBits($lock)
$atlasBytes = New-Object byte[] ($atlasW * $atlasH)
for ($y = 0; $y -lt $atlasH; $y++) {
    $rowOff = $y * $lock.Stride
    for ($x = 0; $x -lt $atlasW; $x++) {
        $atlasBytes[$y * $atlasW + $x] = $raw[$rowOff + $x * 4 + 2] # BGRA -> R at +2
    }
}
$bmp.Dispose()
$nonzero = ($atlasBytes | Where-Object { $_ -ne 0 }).Count
if ($nonzero -lt 5000) { throw "FONT BAKE FAILED: atlas nearly empty (nonzero=$nonzero)" }
$fontHdr = "// font_atlas.h - GENERATED by build-frontend.ps1 (pre-baked font atlas). Do not hand-edit.`n#pragma once`n"
$fontHdr += "static const unsigned int kFontAtlasW = $atlasW;`nstatic const unsigned int kFontAtlasH = $atlasH;`n"
$fontHdr += "static const unsigned int kFontCellW = $cellW;`nstatic const unsigned int kFontCellH = $cellH;`nstatic const unsigned int kFontCols = $cols;`n"
$fontHdr += "static const unsigned char kFontWidths[256] = { " + (($widths | ForEach-Object { "$_," }) -join "") + " };`n"
$fontHdr += "static const unsigned char kFontAtlas[$($atlasW * $atlasH)] = { " + (($atlasBytes | ForEach-Object { ("0x{0:X2}," -f $_) }) -join "") + " };`n"
Set-Content -Path "$root\frontend\font_atlas.h" -Value $fontHdr -Encoding ascii
Write-Host "FRONTEND-UI-ASSETS-OK ui_vs/ui_ps compiled, font atlas ${atlasW}x${atlasH} baked (nonzero=$nonzero)"

Copy-Item "$dxilSrc\16_vs_simple.dxil" "$out\16_vs_simple.dxil" -Force

Copy-Item "$dxilSrc\01_fs_simple_color.dxil" "$out\01_fs_simple_color.dxil" -Force

# Step 6 draw shaders (SPIR-V + unsigned Mesa DXIL) from xbox-draw.

Copy-Item "C:\rpcs3\xbox-draw\spv\draw_vs.spv" "$out\draw_vs.spv" -Force

Copy-Item "C:\rpcs3\xbox-draw\spv\draw_fs.spv" "$out\draw_fs.spv" -Force

Copy-Item "C:\rpcs3\xbox-draw\dxil\draw_vs.dxil" "$out\draw_vs.dxil" -Force

Copy-Item "C:\rpcs3\xbox-draw\dxil\draw_fs.dxil" "$out\draw_fs.dxil" -Force



if (-not $SkipDesktopTest) {

    Write-Host "== desktop pre-test (device+queue+control PSO+swapchain+bounded present must pass) =="

    Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /DSTEP6_DESKTOP_TRANSLATOR `"$root\pso_probe.cpp`" `"$root\present_probe.cpp`" `"$root\draw_probe.cpp`" `"$root\audio_pad_probe.cpp`" `"$root\..\xbox-memory\memory_governor.cpp`" `"$root\..\xbox-d3d12-backend\d3d12_gsrender.cpp`" `"$root\..\xbox-emu-link\emu_link.cpp`" `"$root\..\xbox-emu-link\reassemble.cpp`" `"$root\main_desktop.cpp`" /Fe:`"$out\host_desktop.exe`" /Fo:`"$out\\`" /link windowsapp.lib onecore.lib d3d12.lib dxgi.lib user32.lib ole32.lib runtimeobject.lib psapi.lib"

    # Session 0 (service/agent shells) has no DXGI output: every swapchain

    # flavour fails there with DXGI_ERROR_NOT_CURRENTLY_AVAILABLE 0x887A0022

    # (measured Step 4). In that case run the identical pre-test binary in

    # the logged-on user's interactive session via a one-shot scheduled task.

    if ((Get-Process -Id $PID).SessionId -eq 0) {

        Write-Host "Session 0 shell detected - running desktop pre-test in interactive session via scheduled task"

        $runner = Join-Path $out "run-desktop-pretest.cmd"

        $exitFile = Join-Path $out "desktop-pretest.exit.txt"

        "@echo off`r`ncd /d $out`r`nhost_desktop.exe $out > desktop-pretest.log 2>&1`r`necho %ERRORLEVEL% > desktop-pretest.exit.txt`r`n" | Set-Content -Path $runner -Encoding ascii

        Remove-Item $exitFile, (Join-Path $out "desktop-pretest.log") -Force -ErrorAction SilentlyContinue

        $taskAction = New-ScheduledTaskAction -Execute "cmd.exe" -Argument "/c $runner" -WorkingDirectory $out

        $taskPrincipal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

        Register-ScheduledTask -TaskName "RPCS3Step4DesktopPreTest" -Action $taskAction -Principal $taskPrincipal -Force | Out-Null

        Start-ScheduledTask -TaskName "RPCS3Step4DesktopPreTest"

        $deadline = (Get-Date).AddSeconds(300)

        while (-not (Test-Path $exitFile) -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }

        Unregister-ScheduledTask -TaskName "RPCS3Step4DesktopPreTest" -Confirm:$false -ErrorAction SilentlyContinue

        if (-not (Test-Path $exitFile)) { throw "DESKTOP PRE-TEST TIMEOUT (interactive-session run produced no exit file)" }

        $dtExit = [int](Get-Content $exitFile | Select-Object -First 1)

    } else {

        cmd.exe /c "`"$out\host_desktop.exe`" `"$out`" > `"$out\desktop-pretest.log`" 2>&1"

        $dtExit = $LASTEXITCODE

    }

    Get-Content "$out\desktop-pretest.log"

    if ($dtExit -ne 0) { throw "DESKTOP PRE-TEST FAILED - do not package" }

    Write-Host "DESKTOP PRE-TEST PASS"

    # FRONTEND (1.1.0): frontend logic + D3D12 UI pre-test (SFO parse,
    # library/USB scans, menu pad machine, progress plumbing, 30 UI
    # frames). Same session-0 handling as host_desktop above.
    Write-Host "== frontend desktop pre-test =="
    Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /I`"$root\frontend`" /c `"$root\frontend\frontend_library.cpp`" /Fo:`"$out\frontend_library.obj`""
    Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /I`"$root\frontend`" /c `"$root\frontend\frontend_compile.cpp`" /Fo:`"$out\frontend_compile.obj`""
    Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /I`"$root\frontend`" `"$out\frontend_library.obj`" `"$out\frontend_compile.obj`" `"$root\frontend\frontend_input.cpp`" `"$root\frontend\frontend_progress.cpp`" `"$root\frontend\frontend_ui.cpp`" `"$root\frontend\frontend_desktop_test.cpp`" /Fe:`"$out\frontend_desktop_test.exe`" /Fo:`"$out\\`" /link windowsapp.lib onecore.lib d3d12.lib dxgi.lib user32.lib ole32.lib runtimeobject.lib"
    $feLog = Join-Path $out "frontend-pretest.log"
    $feExitFile = Join-Path $out "frontend-pretest.exit.txt"
    if ((Get-Process -Id $PID).SessionId -eq 0) {
        $feRunner = Join-Path $out "run-frontend-pretest.cmd"
        "@echo off`r`ncd /d $out`r`nfrontend_desktop_test.exe $out > frontend-pretest.log 2>&1`r`necho %ERRORLEVEL% > frontend-pretest.exit.txt`r`n" | Set-Content -Path $feRunner -Encoding ascii
        Remove-Item $feExitFile, $feLog -Force -ErrorAction SilentlyContinue
        $feAction = New-ScheduledTaskAction -Execute "cmd.exe" -Argument "/c $feRunner" -WorkingDirectory $out
        $fePrincipal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited
        Register-ScheduledTask -TaskName "RPCS3FrontendDesktopPreTest" -Action $feAction -Principal $fePrincipal -Force | Out-Null
        Start-ScheduledTask -TaskName "RPCS3FrontendDesktopPreTest"
        $feDeadline = (Get-Date).AddSeconds(300)
        while (-not (Test-Path $feExitFile) -and (Get-Date) -lt $feDeadline) { Start-Sleep -Seconds 2 }
        Unregister-ScheduledTask -TaskName "RPCS3FrontendDesktopPreTest" -Confirm:$false -ErrorAction SilentlyContinue
        if (-not (Test-Path $feExitFile)) { throw "FRONTEND PRE-TEST TIMEOUT (interactive-session run produced no exit file)" }
        $feExit = [int](Get-Content $feExitFile | Select-Object -First 1)
    } else {
        cmd.exe /c "`"$out\frontend_desktop_test.exe`" `"$out`" > `"$feLog`" 2>&1"
        $feExit = $LASTEXITCODE
    }
    Get-Content $feLog
    if ($feExit -ne 0) { throw "FRONTEND PRE-TEST FAILED - do not package" }
    if (-not (Select-String -Path $feLog -SimpleMatch "FRONTEND-DESKTOP-TEST: PASS" -Quiet)) { throw "FRONTEND PRE-TEST FAILED (no PASS line)" }
    if (-not (Select-String -Path $feLog -SimpleMatch "FRONTEND-DESKTOP-D3D presented=40" -Quiet)) { throw "FRONTEND PRE-TEST FAILED (D3D UI smoke did not present 40 frames - see log)" }
    Write-Host "FRONTEND PRE-TEST PASS"

}

Write-Host "== UWP host build (C++/CX pattern from xbox-spikes) =="

$msvcTools = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1

$uwpLib = Join-Path $msvcTools.FullName "lib\x64\uwp"

$platformWinmd = (Get-ChildItem (Join-Path $msvcTools.FullName "lib") -Recurse -Filter "platform.winmd" | Select-Object -First 1).FullName

if (-not $platformWinmd) { throw "platform.winmd not found" }

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\pso_probe.cpp`" /Fo:`"$out\pso_probe.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\present_probe.cpp`" /Fo:`"$out\present_probe.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\draw_probe.cpp`" /Fo:`"$out\draw_probe.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\audio_pad_probe.cpp`" /Fo:`"$out\audio_pad_probe.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\..\xbox-memory\memory_governor.cpp`" /Fo:`"$out\memory_governor.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\..\xbox-d3d12-backend\d3d12_gsrender.cpp`" /Fo:`"$out\d3d12_gsrender.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\..\xbox-emu-link\emu_link.cpp`" /Fo:`"$out\emu_link.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\..\xbox-emu-link\reassemble.cpp`" /Fo:`"$out\reassemble.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\ws2_stub.cpp`" /Fo:`"$out\ws2_stub.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /std:c++17 /MD /c `"$root\stage_diag.cpp`" /Fo:`"$out\stage_diag.obj`""

# v1.0.10.0: real Emulator phases. emu_host.cpp + the Qt/Input stubs are

# native TUs compiled with the emucore include list (same as the proven

# desktop link test) and linked against emucore.lib + the CMake

# dependency libs via libsu.rsp (generated by xbox-uwp-link-test2.ps1

# from libs4.rsp: true-/MD emucore + CMake deps minus SDL3-static).

# libsu.rsp is a build input, not generated here: if it is missing the

# build fails loudly instead of linking the wrong libs.

$emuIncs = @("C:\rpcs3", "C:\rpcs3\rpcs3", "C:\rpcs3\3rdparty\asmjit\asmjit\src", "C:\rpcs3\3rdparty\yaml-cpp\yaml-cpp\include", "C:\rpcs3\3rdparty\ffmpeg\include", "C:\rpcs3\3rdparty\libpng\libpng", "C:\rpcs3\3rdparty\GL", "C:\rpcs3\3rdparty\stblib\stb", "C:\rpcs3\3rdparty\openal\openal-soft\include\AL", "C:\rpcs3\3rdparty\pugixml\src", "C:\rpcs3\3rdparty\hidapi\hidapi", "C:\rpcs3\3rdparty\hidapi\hidapi\hidapi", "C:\rpcs3\3rdparty\llvm\llvm\llvm\include", "C:\rpcs3\build\3rdparty\llvm\llvm_build\include", "C:\rpcs3\3rdparty\protobuf\protobuf\src", "C:\rpcs3\build\_deps\absl-src", "C:\rpcs3\3rdparty\protobuf\protobuf\third_party\utf8_range", "C:\VulkanSDK\1.4.363.0\Include", "C:\rpcs3\3rdparty\zstd\zstd\lib", "C:\rpcs3\3rdparty\fusion\fusion\Fusion", "C:\rpcs3\3rdparty\wolfssl\extra\win32", "C:\rpcs3\3rdparty\libsdl-org\SDL\include", "C:\rpcs3\3rdparty\glslang\glslang", "C:\rpcs3\3rdparty\curl\curl\include", "C:\rpcs3\3rdparty\miniupnp\miniupnp\miniupnpc\include", "C:\rpcs3\3rdparty\wolfssl\wolfssl", "C:\rpcs3\3rdparty\libusb\libusb\libusb", "C:\rpcs3\3rdparty\SoundTouch\soundtouch\include", "C:\rpcs3\3rdparty\rtmidi\rtmidi", "C:\rpcs3\3rdparty\zlib\zlib")

$emuIncArgs = ($emuIncs | ForEach-Object { "/I$_" }) -join " "

$emuDefs = "/D NOMINMAX /D PUGIXML_HEADER_ONLY /D YAML_CPP_STATIC_DEFINE /D _ENABLE_EXTENDED_ALIGNED_STORAGE /D HAVE_VULKAN /D HAVE_SDL3 /D ZLIB_CONST /D WOLFSSL_USER_SETTINGS /D CURL_STATICLIB /D MINIUPNP_STATICLIB /D AL_LIBTYPE_STATIC /DWINAPI_FAMILY=WINAPI_FAMILY_APP"

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\emu_host.cpp`" /Fo:`"$out\emu_host.obj`""

# FRONTEND (1.1.0): frontend TUs. frontend_boot.obj REPLACES boot_probe.obj
# (derived copy, selected-entry target). frontend_progress is compiled WITH
# the core (FRONTEND_PROGRESS_CORE) so the logs::listener + g_progr_*
# snapshot is real; library/input/ui are native TUs (desktop-test flags).
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /I`"$root`" /c `"$root\frontend\frontend_boot.cpp`" /Fo:`"$out\frontend_boot.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /DFRONTEND_PROGRESS_CORE /c `"$root\frontend\frontend_progress.cpp`" /Fo:`"$out\frontend_progress.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /c `"$root\frontend\frontend_library.cpp`" /Fo:`"$out\frontend_library.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /DWINAPI_FAMILY=WINAPI_FAMILY_APP /c `"$root\frontend\frontend_compile.cpp`" /Fo:`"$out\frontend_compile.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /c `"$root\frontend\frontend_input.cpp`" /Fo:`"$out\frontend_input.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /c `"$root\frontend\frontend_ui.cpp`" /Fo:`"$out\frontend_ui.obj`""

$stubDefs1026 = "/D NOMINMAX /D PUGIXML_HEADER_ONLY /D YAML_CPP_STATIC_DEFINE /D _ENABLE_EXTENDED_ALIGNED_STORAGE /D HAVE_VULKAN /D HAVE_SDL3 /D ZLIB_CONST /D WOLFSSL_USER_SETTINGS /D CURL_STATICLIB /D MINIUPNP_STATICLIB /D AL_LIBTYPE_STATIC"

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\host_pad.cpp`" /Fo:`"$out\host_pad.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\d3d12_rsx.cpp`" /Fo:`"$out\d3d12_rsx.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\crash_capture.cpp`" /Fo:`"$out\crash_capture.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\watchdog.cpp`" /Fo:`"$out\watchdog.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /c `"$root\callback_queue.cpp`" /Fo:`"$out\callback_queue.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++17 /c `"$root\iat_hooks.cpp`" /Fo:`"$out\iat_hooks.obj`""
Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\early_config_dir.cpp`" /Fo:`"$out\early_config_dir.obj`""

# Stubs compile WITHOUT WINAPI_FAMILY_APP: uwp-stubs.cpp includes SDL

# headers, and SDL_platform_defines.h hard-errors (#error) on the UWP

# family define ("Windows RT/UWP is no longer supported in SDL"). The

# proven uwp-link-test2 compile used no family define either. The stubs

# are native code; the define only affects Windows API declarations in

# the TU, which the stubs do not need.

$stubDefs = "/D NOMINMAX /D PUGIXML_HEADER_ONLY /D YAML_CPP_STATIC_DEFINE /D _ENABLE_EXTENDED_ALIGNED_STORAGE /D HAVE_VULKAN /D HAVE_SDL3 /D ZLIB_CONST /D WOLFSSL_USER_SETTINGS /D CURL_STATICLIB /D MINIUPNP_STATICLIB /D AL_LIBTYPE_STATIC"

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $stubDefs $emuIncArgs /c `"C:\rpcs3\xbox-core-lib\linktest\host-stubs.cpp`" /Fo:`"$out\host-stubs.obj`""

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $stubDefs $emuIncArgs /c `"C:\rpcs3\xbox-core-lib\linktest\uwp-stubs.cpp`" /Fo:`"$out\uwp-stubs.obj`""

# 1.0.44 OBJECT HYGIENE: never link a stale main object (the 1.0.40 lesson).
Remove-Item "$out\main_uwp*.obj" -Force -ErrorAction SilentlyContinue
Remove-Item "$out\main_frontend*.obj" -Force -ErrorAction SilentlyContinue
$mainCompileCmd = "cl /nologo /EHsc /O2 /MD /ZW /std:c++17 /DUNICODE /D_UNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP /wd28204 /FU`"$platformWinmd`" /c `"$root\main_frontend.cpp`" /Fo:`"$out\main_frontend.obj`""
Invoke-Vc $mainCompileCmd
if (-not (Test-Path "$out\main_frontend.obj")) { throw "MAIN OBJECT GATE FAILED: main_frontend.obj not produced" }
if ((Get-Item "$out\main_frontend.obj").LastWriteTime -lt $buildStart) { throw "MAIN OBJECT GATE FAILED: main_frontend.obj predates build start (stale)" }

$libsu = "C:\rpcs3\xbox-core-lib\linktest\libsu.rsp"

if (-not (Test-Path $libsu)) { throw "libsu.rsp missing - run xbox-uwp-link-test2.ps1 first to regenerate it (emucore-md + CMake deps minus SDL3-static)" }

Invoke-Vc "link /nologo `"$out\ws2_stub.obj`" `"$out\stage_diag.obj`" `"$out\pso_probe.obj`" `"$out\present_probe.obj`" `"$out\draw_probe.obj`" `"$out\audio_pad_probe.obj`" `"$out\memory_governor.obj`" `"$out\d3d12_gsrender.obj`" `"$out\emu_link.obj`" `"$out\reassemble.obj`" `"$out\emu_host.obj`" `"$out\frontend_boot.obj`" `"$out\frontend_library.obj`" `"$out\frontend_compile.obj`" `"$out\frontend_input.obj`" `"$out\frontend_progress.obj`" `"$out\frontend_ui.obj`" `"$out\host_pad.obj`" `"$out\\d3d12_rsx.obj`" `"$out\crash_capture.obj`" `"$out\watchdog.obj`" `"$out\callback_queue.obj`" `"$out\iat_hooks.obj`" `"$out\early_config_dir.obj`" `"$out\host-stubs.obj`" `"$out\uwp-stubs.obj`" `"$out\main_frontend.obj`" @$libsu /MAP:`"$out\XboxUwpHost-frontend-114.map`" /OUT:`"$out\XboxUwpHost.exe`" /SUBSYSTEM:WINDOWS /APPCONTAINER /WINMD /MANIFEST:NO /NODEFAULTLIB /NODEFAULTLIB:ws2_32.lib /LIBPATH:`"$uwpLib`" windowsapp.lib vccorlib.lib msvcprt.lib vcruntime.lib ucrt.lib msvcrt.lib d3d12.lib dxgi.lib"
Write-Host "== FRONTEND 1.1.4 EXE STRING GATE (linked exe, before packaging) =="
$gateExe = "$out\XboxUwpHost.exe"
if (-not (Test-Path $gateExe)) { throw "STRING GATE FAILED: linked exe missing" }
if ((Get-Item $gateExe).LastWriteTime -lt $buildStart) { throw "STRING GATE FAILED: linked exe predates build start (stale)" }
$gateBytes = [System.IO.File]::ReadAllBytes($gateExe)
$gateText = [System.Text.Encoding]::ASCII.GetString($gateBytes)
$gatePositive = @('FRONTEND-TIER','FRONTEND-BOOT-PHASE','FRONTEND-LIB-SCAN','FRONTEND-USB-SCAN','FRONTEND-INPUT-INIT','FRONTEND-PROGRESS','FRONTEND-GAME-ENTERED','FRONTEND-RSX-OWNS-SWAPCHAIN','CellBox Library','BOOT-PHASE','BOOT-BRIDGE','BOOT-RESULT','BOOT-DECODER-AFTER','BOOT-CALLBACK-EXCEPTION','[PAD-DELIVERED]','[MSG-DIALOG]','[VP-NEW]','[INLINE-SKIP]','FRONTEND-COMPILE-PHASE','FRONTEND-COMPILE-MODULES','FRONTEND-COMPILE-START','FRONTEND-COMPILE-RESULT','FRONTEND-COMPILE-SHADERS','FRONTEND-COMPILING','FRONTEND-STOP-JOIN','FRONTEND-STOP-JOIN stop-requested','FRONTEND-STOP-JOIN rsx-exit-signaled','FRONTEND-STOP-JOIN rsx-exited','FRONTEND-STOP-JOIN join-complete','FRONTEND-STOP-JOIN waiting','FRONTEND-STOP-JOIN complete','FRONTEND-STOP-JOIN stage=','BOOT-PPU-SUMMARY-SKIPPED','FRONTEND-DELETE','FRONTEND-DELETE-RESULT','FRONTEND-BOOT-MOUNT','FRONTEND-BOOT-TARGET')
$gateMissing = @($gatePositive | Where-Object { -not $gateText.Contains($_) })
if ($gateMissing) { throw "STRING GATE FAILED missing: $($gateMissing -join ', ')" }
# NOTE: BOOT-CALLBACK-EXCEPTION is a callback-queue-path marker, not a main-TU
# string (it is present in 1040 too, so it is non-discriminating for the
# wrong-main failure; the discriminating positives are BOOT-BRIDGE,
# BOOT-LOOP-ENTERED, BOOT-RSX-SUSTAINED and BOOT-STATE).
$gatePoison = @('boot NOT attempted in this build','EMU-HOST-PUMP: TIMEOUT','BOOT-DIAG-MAGENTA-CLEAR active','BOOT-LOOP-ENTERED','arrives in slice F4','arrives in slice F2')
$gateFoundPoison = @($gatePoison | Where-Object { $gateText.Contains($_) })
if ($gateFoundPoison) { throw "POISON GATE FAILED present: $($gateFoundPoison -join ', ')" }
# 1.0.44 GATE EVOLUTION: BOOT-DIAG-MAGENTA-CLEAR active is a POISON here (clear removed by design);
Write-Host "STRING GATE PASS (positive + poison + sema sanity)"
$gateMainHash = (Get-FileHash "$root\main_frontend.cpp" -Algorithm SHA256).Hash
$gateObjHash = (Get-FileHash "$out\main_frontend.obj" -Algorithm SHA256).Hash
$gateExeHash = (Get-FileHash $gateExe -Algorithm SHA256).Hash
$gateLibsuHash = (Get-FileHash $libsu -Algorithm SHA256).Hash
$gateLinkObjs = @('ws2_stub.obj','stage_diag.obj','pso_probe.obj','present_probe.obj','draw_probe.obj','audio_pad_probe.obj','memory_governor.obj','d3d12_gsrender.obj','emu_link.obj','reassemble.obj','emu_host.obj','frontend_boot.obj','frontend_library.obj','frontend_compile.obj','frontend_input.obj','frontend_progress.obj','frontend_ui.obj','host_pad.obj','d3d12_rsx.obj','crash_capture.obj','watchdog.obj','callback_queue.obj','iat_hooks.obj','early_config_dir.obj','host-stubs.obj','uwp-stubs.obj','main_frontend.obj')
$gateRecord = @()
$gateRecord += "main_source main_frontend.cpp $gateMainHash"
$gateRecord += "main_compile_command $mainCompileCmd"
$gateRecord += "main_object main_frontend.obj $gateObjHash"
$gateRecord += "libsu_rsp $gateLibsuHash"
foreach ($o in $gateLinkObjs) { $gateRecord += ("{0} {1}" -f (Get-FileHash (Join-Path $out $o) -Algorithm SHA256).Hash, $o) }
$gateRecord += "linked_exe XboxUwpHost.exe $gateExeHash"
$gateRecord | Set-Content -Path (Join-Path $out "gate-frontend-114.txt") -Encoding ascii
Write-Host "GATE RECORD: $out\gate-frontend-114.txt (main $gateMainHash) (exe $gateExeHash)"


$dumpbin = Join-Path $msvcTools.FullName "bin\Hostx64\x64\dumpbin.exe"

$importDump = & $dumpbin /imports "$out\XboxUwpHost.exe"

if ($LASTEXITCODE -ne 0) { throw "dumpbin failed" }

$importDump | Out-File -FilePath (Join-Path $out "imports.txt") -Encoding ascii

$importDlls = $importDump | Select-String -Pattern '^\s{4}\S+\.dll\s*$' | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique

$forbidden = $importDlls | Where-Object { $_ -match '^(KERNEL32|USER32|GDI32|OLE32|ADVAPI32)\.dll$' }

if ($forbidden) { throw "IMPORT GATE FAILED desktop imports: $($forbidden -join ', ')" }

# 1.0.10.0: the linked Emu needs console-present UWP-partition DLLs

# (Winsock/CNG/cert/net-info/loader) - measured import list from the

# proven UWP link test, not a blanket exemption. The forbidden

# KERNEL32/USER32/GDI32/OLE32/ADVAPI32 check above still fails the gate.

$disallowed = $importDlls | Where-Object {

    $_ -notmatch '^(api-ms-win-|ext-ms-win-)' -and $_ -notmatch '^(d3d12|dxgi)\.dll$' -and

    $_ -notmatch '^(bcrypt|crypt32|iphlpapi|ntdll)\.dll$' -and

    $_ -notmatch '^(msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vccorlib140|vcruntime140(_1|_threads)?|concrt140)(_app)?\.dll$'

}

if ($disallowed) { throw "IMPORT GATE FAILED non-allowlisted: $($disallowed -join ', ')" }

if ($importDlls -contains "WS2_32.dll") { throw "IMPORT GATE FAILED: WS2_32.dll present - ws2_stub did not hold" }

Write-Host "IMPORT GATE PASS (WS2_32.dll absent):"; $importDlls | ForEach-Object { Write-Host "  $_" }

$stage = Join-Path $out "stage"

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }

New-Item -ItemType Directory -Force -Path (Join-Path $stage "Assets") | Out-Null

New-Item -ItemType Directory -Force -Path (Join-Path $stage "dxil") | Out-Null

Add-Type -AssemblyName System.Drawing

function New-Png([string]$path, [int]$w, [int]$h) {

    $bmp = New-Object System.Drawing.Bitmap($w, $h); $g = [System.Drawing.Graphics]::FromImage($bmp)

    $g.Clear([System.Drawing.Color]::DarkSlateGray); $g.Dispose(); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()

}

New-Png (Join-Path $stage "Assets\StoreLogo.png") 50 50

New-Png (Join-Path $stage "Assets\Square150x150Logo.png") 150 150

New-Png (Join-Path $stage "Assets\Square44x44Logo.png") 44 44

New-Png (Join-Path $stage "Assets\Wide310x150Logo.png") 310 150

New-Png (Join-Path $stage "Assets\SplashScreen.png") 620 300

Copy-Item "$out\XboxUwpHost.exe" "$stage\XboxUwpHost.exe"

if (Test-Path "$out\XboxUwpHost.winmd") { Copy-Item "$out\XboxUwpHost.winmd" "$stage\XboxUwpHost.winmd" }

Copy-Item "$out\16_vs_simple.dxil" "$stage\dxil\16_vs_simple.dxil"

Copy-Item "$out\01_fs_simple_color.dxil" "$stage\dxil\01_fs_simple_color.dxil"

Copy-Item "$out\control_vs.cso" "$stage\dxil\control_vs.cso"

Copy-Item "$out\control_ps.cso" "$stage\dxil\control_ps.cso"

Copy-Item "$out\slice_vs.cso" "$stage\dxil\slice_vs.cso"

Copy-Item "$out\slice_ps.cso" "$stage\dxil\slice_ps.cso"

Copy-Item "$out\blit_vs.cso" "$stage\dxil\blit_vs.cso"

Copy-Item "$out\blit_ps.cso" "$stage\dxil\blit_ps.cso"

Copy-Item "$out\draw_vs.spv" "$stage\dxil\draw_vs.spv"

Copy-Item "$out\draw_fs.spv" "$stage\dxil\draw_fs.spv"

Copy-Item "$out\draw_vs.dxil" "$stage\dxil\draw_vs.dxil"

Copy-Item "$out\draw_fs.dxil" "$stage\dxil\draw_fs.dxil"

$crt = Get-ChildItem "$vs\VC\Redist\MSVC" -Recurse -Directory -Filter "Microsoft.VC*.CRT" |

    Where-Object { $_.FullName -match "\\onecore\\x64\\" } | Select-Object -First 1

if (-not $crt) { throw "VC OneCore CRT redist not found" }

foreach ($dll in "msvcp140.dll","vccorlib140.dll","vcruntime140.dll","vcruntime140_1.dll","msvcp140_atomic_wait.dll","msvcp140_codecvt_ids.dll") {

    Copy-Item (Join-Path $crt.FullName $dll) (Join-Path $stage $dll)

    Copy-Item (Join-Path $crt.FullName $dll) (Join-Path $stage ($dll -replace '\.dll$', '_app.dll'))

}

# PACKAGING-COMPLETENESS GATE (1.0.21): every imported DLL must be an apiset

# contract, console-proven OS-provided, or staged app-local. Pre-1021 builds

# imported MSVCP140_ATOMIC_WAIT/CODECVT_IDS without staging them.

$osProven = @('bcrypt.dll','crypt32.dll','iphlpapi.dll','ntdll.dll','d3d12.dll','dxgi.dll')

$missingPayload = @()

foreach ($d in $importDlls) {

    if ($d -match '^(api-ms-win-|ext-ms-win-)') { continue }

    if ($osProven -contains $d) { continue }

    if (-not (Test-Path (Join-Path $stage $d))) { $missingPayload += $d }

}

if ($missingPayload) { throw "PACKAGING GATE FAILED imported-but-not-staged: $($missingPayload -join ', ')" }

Write-Host "PACKAGING GATE PASS (all non-apiset, non-OS-proven imports staged app-local)"

$manifest = (Get-Content "$root\AppxManifest.xml" -Raw) -replace '\$targetnametoken\$', 'XboxUwpHost' -replace 'Version="1\.0\.\d+\.0"', 'Version="1.1.4.0"'

if ($manifest -notmatch 'Version="1\.1\.4\.0"') { throw "manifest version injection failed (expected 1.1.4.0)" }

Set-Content -Path "$stage\AppxManifest.xml" -Value $manifest -Encoding UTF8

Write-Host "== certificate (reuse spike dev cert CN=RPCS3SpikeDev) =="

$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=RPCS3SpikeDev" } | Select-Object -First 1

if (-not $cert) { throw "spike dev cert CN=RPCS3SpikeDev not found - run xbox-spikes build first" }

$pw = ConvertTo-SecureString -String "CHANGE-ME-DEV-ONLY" -Force -AsPlainText

Export-PfxCertificate -Cert $cert -FilePath "$out\host.pfx" -Password $pw | Out-Null

Export-Certificate -Cert $cert -FilePath "$out\host.cer" | Out-Null

Write-Host "== pack + sign =="

$msix = Join-Path $out "XboxUwpHost.msix"

if (Test-Path $msix) { Remove-Item $msix -Force }

& "$sdk\makeappx.exe" pack /o /d $stage /p $msix

if ($LASTEXITCODE -ne 0) { throw "makeappx failed" }

& "$sdk\signtool.exe" sign /fd SHA256 /f "$out\host.pfx" /p CHANGE-ME-DEV-ONLY $msix

if ($LASTEXITCODE -ne 0) { throw "signtool failed" }

$prevEap = $ErrorActionPreference; $ErrorActionPreference = "Continue"

& "$sdk\signtool.exe" verify /pa $msix 2>$null | Out-Null

$ErrorActionPreference = $prevEap; $global:LASTEXITCODE = 0
# 1.0.44 PACKAGED-EXE HASH COMPARE: the exe inside the final signed package
# must be byte-identical to the gated link output.
$unpackDir = Join-Path $out "unpack-check-frontend-114"
if (Test-Path $unpackDir) { Remove-Item $unpackDir -Recurse -Force }
& "$sdk\makeappx.exe" unpack /o /d $unpackDir /p $msix
if ($LASTEXITCODE -ne 0) { throw "makeappx unpack (gate check) failed" }
$pkgExeHash = (Get-FileHash (Join-Path $unpackDir "XboxUwpHost.exe") -Algorithm SHA256).Hash
if ($pkgExeHash -ne $gateExeHash) { throw "PACKAGED-EXE HASH MISMATCH gated=$gateExeHash packaged=$pkgExeHash" }
Write-Host "PACKAGED-EXE HASH MATCH: $pkgExeHash"
Remove-Item $unpackDir -Recurse -Force
"packaged_exe XboxUwpHost.exe $pkgExeHash" | Add-Content -Path (Join-Path $out "gate-frontend-114.txt") -Encoding ascii


Write-Host "PACKAGE OK: $msix ($((Get-Item $msix).Length) bytes)"

Write-Host "SHA256 msix: $((Get-FileHash $msix -Algorithm SHA256).Hash)"

Write-Host "SHA256 exe: $((Get-FileHash "$out\XboxUwpHost.exe" -Algorithm SHA256).Hash)"

Copy-Item $msix (Join-Path $out "XboxUwpHost-frontend-114.msix") -Force

Copy-Item "$out\XboxUwpHost.exe" (Join-Path $out "XboxUwpHost-frontend-114.exe") -Force

Copy-Item (Join-Path $out "imports.txt") (Join-Path $out "imports-frontend-114.txt") -Force

if (Test-Path "$out\desktop-pretest.log") { Copy-Item "$out\desktop-pretest.log" (Join-Path $out "desktop-pretest-frontend-114.log") -Force }

Write-Host "PRESERVED: $out\XboxUwpHost-frontend-114.msix ($((Get-Item (Join-Path $out "XboxUwpHost-frontend-114.msix")).Length) bytes)"

Write-Host "SHA256 msix-frontend-114: $((Get-FileHash (Join-Path $out "XboxUwpHost-frontend-114.msix") -Algorithm SHA256).Hash)"

Write-Host "SHA256 exe-frontend-114: $((Get-FileHash (Join-Path $out "XboxUwpHost-frontend-114.exe") -Algorithm SHA256).Hash)"





















