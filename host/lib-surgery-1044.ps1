# lib-surgery-1044.ps1 -- swap the 4 freshly compiled 1.1.4 core objects into a
# COPY of the current emucore.lib, verify markers, then (and only then) replace
# out\emucore.lib. The pre-1.1.4 lib is kept as emucore-1044-pre114.lib (never
# overwritten). Does not run MSBuild (see patches/README.md on the LLVM include).
$ErrorActionPreference = "Stop"
$out = "C:\rpcs3\xbox-core-lib-md\out"
$tmp = "C:\rpcs3\xbox-core-lib-md\tmp"
$lib = Join-Path $out "emucore.lib"
$bak = Join-Path $out "emucore-1044-pre114.lib"
$work = Join-Path $out "emucore-114-work.lib"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$msvcTools = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$libexe = Join-Path $msvcTools.FullName "bin\Hostx64\x64\lib.exe"
if (-not (Test-Path $libexe)) { throw "lib.exe not found" }
if (-not (Test-Path $bak)) { throw "backup emucore-1044-pre114.lib missing" }
if (Test-Path $work) { throw "work lib already exists: $work" }
$objs = @(
    @{ obj = "PPUThread.obj"; marker = "total=%u run=%u wait=%u" },
    @{ obj = "AudioBackend.obj"; marker = "falling back to stereo" },
    @{ obj = "nv406e.obj"; marker = "XBOX-SEMA-FORCE" },
    @{ obj = "ISO.obj"; marker = $null }
)
if ((Get-FileHash $lib -Algorithm SHA256).Hash -ne (Get-FileHash $bak -Algorithm SHA256).Hash) { throw "emucore.lib differs from pre114 backup -- refusing" }
foreach ($o in $objs) {
    $p = Join-Path $tmp $o.obj
    if (-not (Test-Path $p)) { throw "missing fresh object: $p" }
    if ($o.marker -and -not (Select-String -Path $p -Pattern $o.marker -SimpleMatch -Quiet)) { throw "marker $($o.marker) NOT in $p" }
    Write-Output "PRE-OK $($o.obj)"
}
Copy-Item $lib $work
$members = & $libexe /LIST $work
foreach ($o in $objs) {
    $m = @($members | Where-Object { $_ -match ("[\\/]" + [regex]::Escape($o.obj) + "$") -or $_ -eq $o.obj })
    if ($m.Count -ne 1) { throw "member lookup for $($o.obj): found $($m.Count)" }
    & $libexe /REMOVE:$($m[0]) $work | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib /REMOVE failed for $($m[0])" }
    Write-Output "REMOVED $($m[0])"
}
$objArr = @($objs | ForEach-Object { Join-Path $tmp $_.obj })
$addOut = & $libexe $work $objArr 2>&1
if ($LASTEXITCODE -ne 0) { throw "lib add failed: $addOut" }
foreach ($o in $objs) { if ($o.marker -and -not (Select-String -Path $work -Pattern $o.marker -SimpleMatch -Quiet)) { throw "marker $($o.marker) NOT in work lib" } }
if (-not (Select-String -Path $work -Pattern "XBOX-SEMA-BE" -SimpleMatch -Quiet)) { throw "XBOX-SEMA-BE marker lost" }
Copy-Item $work $lib -Force
Write-Output "SWAPPED emucore.lib $((Get-FileHash $lib -Algorithm SHA256).Hash) size $((Get-Item $lib).Length)"
Write-Output "LIB-SURGERY-OK"
