$ErrorActionPreference = 'Continue'
Copy-Item -Force 'C:\rpcs3\xbox-core-lib-md\out\emucore.lib' 'C:\rpcs3\xbox-core-lib-md\out\emucore-1042.lib'
Write-Output 'BACKUP-DONE'
Set-Location 'C:\rpcs3\rpcs3'
$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild emucore.vcxproj /p:Configuration=Release /p:Platform=x64 /p:SolutionDir=C:\rpcs3\ /p:OutDir=C:\rpcs3\xbox-core-lib-md\out\ /p:IntDir=C:\rpcs3\xbox-core-lib-md\tmp\ /m:8 > 'C:\rpcs3\xbox-core-lib-md\build-1043-md.log' 2>&1
Write-Output "MSBUILD_EXIT:$LASTEXITCODE"
$hits = Select-String -Path 'C:\rpcs3\xbox-core-lib-md\build-1043-md.log' -Pattern 'error|Build succeeded' | Select-Object -Last 8
foreach ($h in $hits) { Write-Output $h.Line }
