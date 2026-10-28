# v1.0.43.0: DIAGNOSTIC-ONLY 5-point instrumentation (Argon green-light 2026-10-28).
# ZERO behavioral changes: the prior version's execution paths 100% intact. No caches, no
# timeout tweaks, no fake label-1, no host-GPU labels, NO SPU/PPU writer trace.
# (i) end() inlined_array guard: atomic inlined_skips + [INLINE-SKIP] first-16 +
# new-VP log; exported to BOOT-RSX-SUSTAINED + BOOT-WD SUSTAINED lines.
# (ii) Un-sampled unique VP tracker (max 32) after analyse, all paths: [VP-NEW].
# (iii) Edge PAD trace: [PAD-CNT] transitions + [PAD-EDGE] on raw_btn change (host
# WGI poll) + [PAD-DELIVERED] inside core cellPadGetData (nonzero d1/d2).
# (iv) Host-loop breadcrumb in the BOOT-LOOP-ENTERED sustained loop (try/catch
# with rethrow; SEH __try not applicable: function requires C++ unwinding):
# hlTick/hlStage (1=sleep_done, 2=wgi_poll, 3=file_open, 4=file_write_done)
# exported to BOOT-WD; [HOST-LOOP-EXC] on caught exception. Main changed ONLY
# for (iv); all other main behavior byte-identical to the prior version.
# (v) Gate snapshot: [MSG-DIALOG] in core cellMsgDialogOpen2/ErrorCode (error
# level: crash listener captures error/fatal only); [XBOX-AUDIO-LAYOUT] at the
# AudioBackend default_layout_channel_count throw site (layout 0 = unresolved
# automatic); BOOT-PPU-SUMMARY in the watchdog every 500 ticks (core
# xbox_ppu_thread_summary: counts by RUN/WAIT/PAUSED/STOP + id/name/cia).
# Core rebuilt FIRST (prior-version lib preserved); new strings verified in lib + exe.
# Gate evolved vs the prior version: six new positives ([INLINE-SKIP] [VP-NEW] [PAD-EDGE]
# [PAD-DELIVERED] [HOST-LOOP-EXC] [MSG-DIALOG]); poisons unchanged.

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
$buildStart = Get-Date # 1.0.43 gate: build-start timestamp for fresh-object/exe checks

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

# Slice shaders (pixel->NDC dual-guard since 1.0.43) (build-time dxc SM6.0, same route as the control
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
$hdr = "// slice_shaders.h - GENERATED by build-1043.ps1 from dxc output. Do not hand-edit.`n#pragma once`n"
$hdr += ConvertTo-CHeader "$out\slice_vs.cso" "slice_vs"
$hdr += ConvertTo-CHeader "$out\slice_ps.cso" "slice_ps"
$hdr += ConvertTo-CHeader "$out\blit_vs.cso" "blit_vs"
$hdr += ConvertTo-CHeader "$out\blit_ps.cso" "blit_ps"
Set-Content -Path "$root\slice_shaders.h" -Value $hdr -Encoding ascii
Write-Host "SLICE-SHADERS-OK slice_vs/slice_ps/blit_vs/blit_ps compiled + slice_shaders.h generated"

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

Invoke-Vc "cl /nologo /EHsc /O2 /MD /std:c++latest /utf-8 /Zc:throwingNew- /constexpr:steps16777216 $emuDefs $emuIncArgs /c `"$root\boot_probe.cpp`" /Fo:`"$out\boot_probe.obj`""

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

# 1.0.43 OBJECT HYGIENE: never link a stale main object (the 1.0.40 lesson).
Remove-Item "$out\main_uwp*.obj" -Force -ErrorAction SilentlyContinue
$mainCompileCmd = "cl /nologo /EHsc /O2 /MD /ZW /DUNICODE /D_UNICODE /DWINAPI_FAMILY=WINAPI_FAMILY_APP /wd28204 /FU`"$platformWinmd`" /c `"$root\main_uwp_1043.cpp`" /Fo:`"$out\main_uwp.obj`""
Invoke-Vc $mainCompileCmd
if (-not (Test-Path "$out\main_uwp.obj")) { throw "MAIN OBJECT GATE FAILED: main_uwp.obj not produced" }
if ((Get-Item "$out\main_uwp.obj").LastWriteTime -lt $buildStart) { throw "MAIN OBJECT GATE FAILED: main_uwp.obj predates build start (stale)" }

$libsu = "C:\rpcs3\xbox-core-lib\linktest\libsu.rsp"

if (-not (Test-Path $libsu)) { throw "libsu.rsp missing - run xbox-uwp-link-test2.ps1 first to regenerate it (emucore-md + CMake deps minus SDL3-static)" }

Invoke-Vc "link /nologo `"$out\ws2_stub.obj`" `"$out\stage_diag.obj`" `"$out\pso_probe.obj`" `"$out\present_probe.obj`" `"$out\draw_probe.obj`" `"$out\audio_pad_probe.obj`" `"$out\memory_governor.obj`" `"$out\d3d12_gsrender.obj`" `"$out\emu_link.obj`" `"$out\reassemble.obj`" `"$out\emu_host.obj`" `"$out\boot_probe.obj`" `"$out\host_pad.obj`" `"$out\\d3d12_rsx.obj`" `"$out\crash_capture.obj`" `"$out\watchdog.obj`" `"$out\callback_queue.obj`" `"$out\iat_hooks.obj`" `"$out\early_config_dir.obj`" `"$out\host-stubs.obj`" `"$out\uwp-stubs.obj`" `"$out\main_uwp.obj`" @$libsu /MAP:`"$out\XboxUwpHost-1043.map`" /OUT:`"$out\XboxUwpHost.exe`" /SUBSYSTEM:WINDOWS /APPCONTAINER /WINMD /MANIFEST:NO /NODEFAULTLIB /NODEFAULTLIB:ws2_32.lib /LIBPATH:`"$uwpLib`" windowsapp.lib vccorlib.lib msvcprt.lib vcruntime.lib ucrt.lib msvcrt.lib d3d12.lib dxgi.lib"
Write-Host "== 1.0.43 EXE STRING GATE (linked exe, before packaging) =="
$gateExe = "$out\XboxUwpHost.exe"
if (-not (Test-Path $gateExe)) { throw "STRING GATE FAILED: linked exe missing" }
if ((Get-Item $gateExe).LastWriteTime -lt $buildStart) { throw "STRING GATE FAILED: linked exe predates build start (stale)" }
$gateBytes = [System.IO.File]::ReadAllBytes($gateExe)
$gateText = [System.Text.Encoding]::ASCII.GetString($gateBytes)
$gatePositive = @('BOOT-BRIDGE','BOOT-LOOP-ENTERED','BOOT-RSX-SUSTAINED','BOOT-CALLBACK-EXCEPTION','BOOT-STATE','BOOT-LIST-RESET','BOOT-VP-DIAG','BOOT-DIAG-MAGENTA-REMOVED','BOOT-DIAG-PIXEL-NDC','BOOT-DIAG-FORCE-SCRATCH','XBOX-SEMA-BE','XBOX-SEMA-ACQ','XBOX-SEMA-RELEASE seq=','prev=0x%08X','[INLINE-SKIP]','[VP-NEW]','[PAD-EDGE]','[PAD-DELIVERED]','[HOST-LOOP-EXC]','[MSG-DIALOG]')
$gateMissing = @($gatePositive | Where-Object { -not $gateText.Contains($_) })
if ($gateMissing) { throw "STRING GATE FAILED missing: $($gateMissing -join ', ')" }
# NOTE: BOOT-CALLBACK-EXCEPTION is a callback-queue-path marker, not a main-TU
# string (it is present in 1040 too, so it is non-discriminating for the
# wrong-main failure; the discriminating positives are BOOT-BRIDGE,
# BOOT-LOOP-ENTERED, BOOT-RSX-SUSTAINED and BOOT-STATE).
$gatePoison = @('boot NOT attempted in this build','EMU-HOST-PUMP: TIMEOUT','BOOT-DIAG-MAGENTA-CLEAR active')
$gateFoundPoison = @($gatePoison | Where-Object { $gateText.Contains($_) })
if ($gateFoundPoison) { throw "POISON GATE FAILED present: $($gateFoundPoison -join ', ')" }
# 1.0.43 GATE EVOLUTION: BOOT-DIAG-MAGENTA-CLEAR active is a POISON here (clear removed by design);
Write-Host "STRING GATE PASS (positive + poison + sema sanity)"
$gateMainHash = (Get-FileHash "$root\main_uwp_1043.cpp" -Algorithm SHA256).Hash
$gateObjHash = (Get-FileHash "$out\main_uwp.obj" -Algorithm SHA256).Hash
$gateExeHash = (Get-FileHash $gateExe -Algorithm SHA256).Hash
$gateLibsuHash = (Get-FileHash $libsu -Algorithm SHA256).Hash
$gateLinkObjs = @('ws2_stub.obj','stage_diag.obj','pso_probe.obj','present_probe.obj','draw_probe.obj','audio_pad_probe.obj','memory_governor.obj','d3d12_gsrender.obj','emu_link.obj','reassemble.obj','emu_host.obj','boot_probe.obj','host_pad.obj','d3d12_rsx.obj','crash_capture.obj','watchdog.obj','callback_queue.obj','iat_hooks.obj','early_config_dir.obj','host-stubs.obj','uwp-stubs.obj','main_uwp.obj')
$gateRecord = @()
$gateRecord += "main_source main_uwp_1043.cpp $gateMainHash"
$gateRecord += "main_compile_command $mainCompileCmd"
$gateRecord += "main_object main_uwp.obj $gateObjHash"
$gateRecord += "libsu_rsp $gateLibsuHash"
foreach ($o in $gateLinkObjs) { $gateRecord += ("{0} {1}" -f (Get-FileHash (Join-Path $out $o) -Algorithm SHA256).Hash, $o) }
$gateRecord += "linked_exe XboxUwpHost.exe $gateExeHash"
$gateRecord | Set-Content -Path (Join-Path $out "gate-1043.txt") -Encoding ascii
Write-Host "GATE RECORD: $out\gate-1043.txt (main $gateMainHash) (exe $gateExeHash)"


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

$manifest = (Get-Content "$root\AppxManifest.xml" -Raw) -replace '\$targetnametoken\$', 'XboxUwpHost' -replace 'Version="1\.0\.\d+\.0"', 'Version="1.0.43.0"'

if ($manifest -notmatch 'Version="1\.0\.43\.0"') { throw "manifest version injection failed (expected 1.0.43.0)" }

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
# 1.0.43 PACKAGED-EXE HASH COMPARE: the exe inside the final signed package
# must be byte-identical to the gated link output.
$unpackDir = Join-Path $out "unpack-check-1043"
if (Test-Path $unpackDir) { Remove-Item $unpackDir -Recurse -Force }
& "$sdk\makeappx.exe" unpack /o /d $unpackDir /p $msix
if ($LASTEXITCODE -ne 0) { throw "makeappx unpack (gate check) failed" }
$pkgExeHash = (Get-FileHash (Join-Path $unpackDir "XboxUwpHost.exe") -Algorithm SHA256).Hash
if ($pkgExeHash -ne $gateExeHash) { throw "PACKAGED-EXE HASH MISMATCH gated=$gateExeHash packaged=$pkgExeHash" }
Write-Host "PACKAGED-EXE HASH MATCH: $pkgExeHash"
Remove-Item $unpackDir -Recurse -Force
"packaged_exe XboxUwpHost.exe $pkgExeHash" | Add-Content -Path (Join-Path $out "gate-1043.txt") -Encoding ascii


Write-Host "PACKAGE OK: $msix ($((Get-Item $msix).Length) bytes)"

Write-Host "SHA256 msix: $((Get-FileHash $msix -Algorithm SHA256).Hash)"

Write-Host "SHA256 exe: $((Get-FileHash "$out\XboxUwpHost.exe" -Algorithm SHA256).Hash)"

Copy-Item $msix (Join-Path $out "XboxUwpHost-1043.msix") -Force

Copy-Item "$out\XboxUwpHost.exe" (Join-Path $out "XboxUwpHost-1043.exe") -Force

Copy-Item (Join-Path $out "imports.txt") (Join-Path $out "imports-1043.txt") -Force

if (Test-Path "$out\desktop-pretest.log") { Copy-Item "$out\desktop-pretest.log" (Join-Path $out "desktop-pretest-1043.log") -Force }

Write-Host "PRESERVED: $out\XboxUwpHost-1043.msix ($((Get-Item (Join-Path $out "XboxUwpHost-1043.msix")).Length) bytes)"

Write-Host "SHA256 msix-1043: $((Get-FileHash (Join-Path $out "XboxUwpHost-1043.msix") -Algorithm SHA256).Hash)"

Write-Host "SHA256 exe-1043: $((Get-FileHash (Join-Path $out "XboxUwpHost-1043.exe") -Algorithm SHA256).Hash)"





















