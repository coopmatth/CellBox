# manual-ppu.ps1 -- compile ONLY Emu\Cell\PPUThread.cpp with the vcxproj flags
# PLUS the LLVM source-tree include dir the vcxproj omits (Verifier.h lives
# there, not under build\3rdparty\llvm\llvm_build\include). Output object goes
# to xbox-core-lib-md\tmp\PPUThread.obj (the exact name MSBuild expects), so a
# follow-up MSBuild run skips it as up-to-date and just links emucore.lib.
# No other file is touched. Fails loudly on any error.
$ErrorActionPreference = "Stop"
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$pch = "C:\rpcs3\xbox-core-lib-md\tmp\emucore.pch"
if (-not (Test-Path $pch)) { throw "PCH missing: $pch" }
Set-Location "C:\rpcs3\rpcs3"
# Include set = the HOST build's proven $emuIncs (build-1042.ps1), which the
# host TUs (emu_host, boot_probe, host_pad, d3d12_rsx) already compile against.
# The vcxproj's own set omits the repo roots + asmjit + LLVM source dirs.
$incs = @("C:\rpcs3", "C:\rpcs3\rpcs3", "C:\rpcs3\3rdparty\asmjit\asmjit\src", "C:\rpcs3\3rdparty\yaml-cpp\yaml-cpp\include", "C:\rpcs3\3rdparty\ffmpeg\include", "C:\rpcs3\3rdparty\libpng\libpng", "C:\rpcs3\3rdparty\GL", "C:\rpcs3\3rdparty\stblib\stb", "C:\rpcs3\3rdparty\openal\openal-soft\include\AL", "C:\rpcs3\3rdparty\pugixml\src", "C:\rpcs3\3rdparty\hidapi\hidapi", "C:\rpcs3\3rdparty\hidapi\hidapi\hidapi", "C:\rpcs3\3rdparty\llvm\llvm\llvm\include", "C:\rpcs3\build\3rdparty\llvm\llvm_build\include", "C:\rpcs3\3rdparty\protobuf\protobuf\src", "C:\rpcs3\build\_deps\absl-src", "C:\rpcs3\3rdparty\protobuf\protobuf\third_party\utf8_range", "C:\VulkanSDK\1.4.363.0\Include", "C:\rpcs3\3rdparty\zstd\zstd\lib", "C:\rpcs3\3rdparty\fusion\fusion\Fusion", "C:\rpcs3\3rdparty\wolfssl\extra\win32", "C:\rpcs3\3rdparty\libsdl-org\SDL\include", "C:\rpcs3\3rdparty\glslang\glslang", "C:\rpcs3\3rdparty\curl\curl\include", "C:\rpcs3\3rdparty\miniupnp\miniupnp\miniupnpc\include", "C:\rpcs3\3rdparty\wolfssl\wolfssl", "C:\rpcs3\3rdparty\libusb\libusb\libusb", "C:\rpcs3\3rdparty\SoundTouch\soundtouch\include", "C:\rpcs3\3rdparty\rtmidi\rtmidi", "C:\rpcs3\3rdparty\zlib\zlib")
$incArgs = ($incs | ForEach-Object { "/I$_" }) -join " "
$cmd = "cl /nologo /c $incArgs /Zi /W3 /WX- /diagnostics:column /sdl- /O2 /Oi /Ot /D LIBUSB_WINDOWS_HOTPLUG /D AL_LIBTYPE_STATIC /D MINIUPNP_STATICLIB /D HAVE_VULKAN /D HAVE_SDL3 /D ZLIB_CONST /D WOLFSSL_USER_SETTINGS /D CURL_STATICLIB /D LLVM_AVAILABLE /D NOMINMAX /D PUGIXML_HEADER_ONLY /D YAML_CPP_STATIC_DEFINE /D _ENABLE_EXTENDED_ALIGNED_STORAGE /D _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING /D _HAS_EXCEPTIONS=0 /D _UNICODE /D UNICODE /D _SILENCE_CXX17_ITERATOR_BASE_CLASS_DEPRECATION_WARNING=1 /Gm- /MT /GS /Gy /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /GR /std:c++23preview /Yu`"stdafx.h`" /Fp`"C:\rpcs3\xbox-core-lib-md\tmp\emucore.pch`" /Fo`"C:\rpcs3\xbox-core-lib-md\tmp\PPUThread.obj`" /Fd`"C:\rpcs3\xbox-core-lib-md\out\emucore.pdb`" /external:W3 /Gd /TP /FC /errorReport:queue /utf-8 /Zc:throwingNew- /constexpr:steps16777216 -d2FH4- Emu\Cell\PPUThread.cpp"
cmd.exe /c "`"$vcvars`" >nul 2>&1 && $cmd"
if ($LASTEXITCODE -ne 0) { throw "manual PPUThread compile failed ($LASTEXITCODE)" }
$obj = "C:\rpcs3\xbox-core-lib-md\tmp\PPUThread.obj"
if (-not (Test-Path $obj)) { throw "PPUThread.obj not produced" }
Write-Output "MANUAL-PPU-OK $obj"
