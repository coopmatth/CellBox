# core-build-1044.ps1 - rebuild emucore.lib with the 1.1.4 core patches.
# Never overwrites an existing backup: the current emucore.lib is kept as
# emucore-1044-pre114.lib (created only if absent). Paths use the C:\rpcs3
# junction (-> C:\cellbox) like core-build-1043.ps1.
$ErrorActionPreference = 'Continue'
$out = 'C:\rpcs3\xbox-core-lib-md\out'
$tmp = 'C:\rpcs3\xbox-core-lib-md\tmp'
if (-not (Test-Path "$out\emucore-1044-pre114.lib")) { Copy-Item "$out\emucore.lib" "$out\emucore-1044-pre114.lib" }
$bakObj = "$tmp\bak114"
New-Item -ItemType Directory -Force -Path $bakObj | Out-Null
foreach ($o in 'PPUThread.obj','AudioBackend.obj','ISO.obj','nv406e.obj') { if ((Test-Path "$tmp\$o") -and -not (Test-Path "$bakObj\$o")) { Copy-Item "$tmp\$o" "$bakObj\$o" } }
Write-Output 'BACKUP-DONE'
Set-Location 'C:\rpcs3\rpcs3'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild emucore.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=C:\rpcs3\ /p:OutDir=C:\rpcs3\xbox-core-lib-md\out\ /p:IntDir=C:\rpcs3\xbox-core-lib-md\tmp\ /m:8 > 'C:\rpcs3\xbox-core-lib-md\build-1044-pre114.log' 2>&1
Write-Output "MSBUILD_EXIT:$LASTEXITCODE"
$hits = Select-String -Path 'C:\rpcs3\xbox-core-lib-md\build-1044-pre114.log' -Pattern 'error|Build succeeded' | Select-Object -Last 8
foreach ($h in $hits) { Write-Output $h.Line }
