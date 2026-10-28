$ErrorActionPreference = "Stop"
$hostOut = "C:\rpcs3\xbox-uwp-host\out"
$work = "C:\rpcs3\xbox-core-lib\linktest"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$msvcTools = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$uwpLib = Join-Path $msvcTools.FullName "lib\x64\uwp"
$dumpbin = Join-Path $msvcTools.FullName "bin\Hostx64\x64\dumpbin.exe"
$incs = @("C:\rpcs3", "C:\rpcs3\rpcs3", "C:\rpcs3\3rdparty\libsdl-org\SDL\include")
$incArgs = ($incs | ForEach-Object { "/I$_" }) -join " "
Write-Output "== compile UWP stubs (/MD) =="
$cl = "cl /nologo /EHsc /MD /O2 /std:c++latest /utf-8 /Zc:throwingNew- /D NOMINMAX $incArgs /c $work\uwp-stubs.cpp /Fo:$work\uwp-stubs.obj"
cmd.exe /c "`"$vcvars`" >nul 2>&1 && $cl"
if ($LASTEXITCODE -ne 0) { throw "uwp-stubs compile failed $LASTEXITCODE" }
Write-Output "== assemble UWP response file (libs4 minus SDL3-static) =="
$libs = Get-Content "$work\libs4.rsp" | ForEach-Object { $_.Trim('"') } | Where-Object { $_ -ne "" -and $_ -notmatch 'SDL3-static' }
$libs | ForEach-Object { "`"$_`"" } | Set-Content -Path "$work\libsu.rsp" -Encoding ascii
Write-Output "libs in UWP response file: $($libs.Count)"
$hostObjs = @("pso_probe.obj", "present_probe.obj", "draw_probe.obj", "audio_pad_probe.obj", "memory_governor.obj", "d3d12_gsrender.obj", "emu_link.obj", "reassemble.obj", "main_uwp.obj") | ForEach-Object { Join-Path $hostOut $_ }
$objs = ($hostObjs | ForEach-Object { "`"$_`"" }) -join " "
$linkCmd = "link /nologo $objs `"$work\emu_test_md.obj`" `"$work\host-stubs.obj`" `"$work\uwp-stubs.obj`" @$work\libsu.rsp /OUT:`"$work\uwp-link-test.exe`" /SUBSYSTEM:WINDOWS /APPCONTAINER /MANIFEST:NO /NODEFAULTLIB /LIBPATH:`"$uwpLib`" windowsapp.lib vccorlib.lib msvcprt.lib vcruntime.lib ucrt.lib msvcrt.lib d3d12.lib dxgi.lib"
Write-Output "== UWP link attempt 2 =="
cmd.exe /c "`"$vcvars`" >nul 2>&1 && $linkCmd" 2>&1 | Tee-Object -FilePath "$work\uwp-link2.log"
if ($LASTEXITCODE -ne 0) { Write-Output "UWP LINK 2 FAILED (see uwp-link2.log)"; exit 0 }
Write-Output "UWP LINK 2 OK: $work\uwp-link-test.exe ($((Get-Item "$work\uwp-link-test.exe").Length) bytes)"
& $dumpbin /imports "$work\uwp-link-test.exe" | Out-File -FilePath "$work\imports-uwp2.txt" -Encoding ascii
$importDlls = Get-Content "$work\imports-uwp2.txt" | Select-String -Pattern '^\s{4}\S+\.dll\s*$' | ForEach-Object { $_.Line.Trim() } | Sort-Object -Unique
Write-Output "== imported DLLs =="
$importDlls | ForEach-Object { Write-Output "  $_" }
$forbidden = $importDlls | Where-Object { $_ -match '^(KERNEL32|USER32|GDI32|OLE32|ADVAPI32)\.dll$' }
Write-Output "FORBIDDEN: $($forbidden -join ', ')"
$disallowed = $importDlls | Where-Object {
    $_ -notmatch '^(api-ms-win-|ext-ms-win-)' -and $_ -notmatch '^(d3d12|dxgi)\.dll$' -and
    $_ -notmatch '^(msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vccorlib140|vcruntime140(_1|_threads)?|concrt140)(_app)?\.dll$'
}
Write-Output "NON-ALLOWLISTED: $($disallowed -join ', ')"
if (-not $forbidden -and -not $disallowed) { Write-Output "IMPORT GATE PASS" } else { Write-Output "IMPORT GATE FAIL" }
