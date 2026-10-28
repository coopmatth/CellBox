# lib-surgery-1043.ps1 -- swap the 4 fresh 1043 objects into a COPY of the 1042
# emucore.lib, verify markers, then (and only then) replace out\emucore.lib.
# Never re-runs MSBuild (MSBuild deletes + fails to rebuild PPUThread.obj due
# to the vcxproj's missing LLVM/roots/asmjit include dirs -- a latent defect
# documented in the 1043 report, out of scope for this diagnostic build).
$ErrorActionPreference = "Stop"
$out = "C:\rpcs3\xbox-core-lib-md\out"
$tmp = "C:\rpcs3\xbox-core-lib-md\tmp"
$lib = Join-Path $out "emucore.lib"
$bak = Join-Path $out "emucore-1042.lib"
$work = Join-Path $out "emucore-1043-work.lib"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$msvcTools = Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | Sort-Object Name -Descending | Select-Object -First 1
$libexe = Join-Path $msvcTools.FullName "bin\Hostx64\x64\lib.exe"
if (-not (Test-Path $libexe)) { throw "lib.exe not found" }

$objs = @(
    @{ obj = "PPUThread.obj"; marker = "xbox_ppu_thread_summary" },
    @{ obj = "cellPad.obj"; marker = "PAD-DELIVERED" },
    @{ obj = "cellMsgDialog.obj"; marker = "MSG-DIALOG" },
    @{ obj = "AudioBackend.obj"; marker = "XBOX-AUDIO-LAYOUT" }
)
# 1) Preconditions: original lib intact vs backup; fresh objects carry markers
$libHash = (Get-FileHash $lib -Algorithm SHA256).Hash
$bakHash = (Get-FileHash $bak -Algorithm SHA256).Hash
if ($libHash -ne $bakHash) { throw "emucore.lib already differs from emucore-1042.lib backup -- refusing surgery" }
Write-Output "PRE-OK emucore.lib still 1042 ($libHash)"
foreach ($o in $objs) {
    $p = Join-Path $tmp $o.obj
    if (-not (Test-Path $p)) { throw "missing fresh object: $p" }
    $hit = Select-String -Path $p -Pattern $o.marker -SimpleMatch -Quiet
    if (-not $hit) { throw "marker $($o.marker) NOT in fresh object $p" }
    Write-Output "PRE-OK $($o.obj) carries $($o.marker)"
}
# 2) Work copy + member-name discovery
Copy-Item $lib $work -Force
$members = & $libexe /LIST $work
foreach ($o in $objs) {
    $m = @($members | Where-Object { $_ -match ([regex]::Escape($o.obj)) })
    if ($m.Count -ne 1) { throw "member lookup for $($o.obj): found $($m.Count) (need exactly 1)" }
    $o.member = $m[0]
    Write-Output "MEMBER $($o.obj) -> $($o.member)"
}
# 3) Remove old members (in place on the work copy)
foreach ($o in $objs) {
    & $libexe /REMOVE:$($o.member) $work | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "lib /REMOVE failed for $($o.member)" }
    Write-Output "REMOVED $($o.member)"
}
# 4) Add fresh objects (in place on the work copy)
$objArr = @($objs | ForEach-Object { Join-Path $tmp $_.obj })
$addOut = & $libexe $work $objArr 2>&1
if ($LASTEXITCODE -ne 0) { throw "lib add failed: $addOut" }
Write-Output "ADDED 4 fresh objects"
# 5) Verify work lib: all 4 new markers + 1042 semaphore marker still present
foreach ($o in $objs) {
    $hit = Select-String -Path $work -Pattern $o.marker -SimpleMatch -Quiet
    if (-not $hit) { throw "marker $($o.marker) NOT in work lib after surgery" }
    Write-Output "WORK-OK marker $($o.marker)"
}
$old = Select-String -Path $work -Pattern "XBOX-SEMA-BE" -SimpleMatch -Quiet
if (-not $old) { throw "1042 semaphore marker lost in work lib" }
Write-Output "WORK-OK 1042 semaphore marker retained"
# 6) Swap in + record
$workHash = (Get-FileHash $work -Algorithm SHA256).Hash
Copy-Item $work $lib -Force
Write-Output "SWAPPED emucore.lib now 1043 ($workHash)"
Write-Output "LIB-SURGERY-OK"
