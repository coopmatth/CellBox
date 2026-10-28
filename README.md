# CellBox

A UWP host that embeds the RPCS3 PlayStation 3 emulator core on Xbox Series S in
Dev Mode, packaged as a **Game**. Xbox Dev Mode has no Vulkan, so the RSX
graphics backend is DirectX 12, and shaders reach the console as pre-baked,
signed DXIL — the console never compiles shaders itself.

This is the first commit: the host scaffolding and build tooling. The remaining
host sources and the core patch set follow in a later commit.

## Why a Game package, and why D3D12

- **Memory tier.** In Xbox Dev Mode, a UWP App is capped at 1 GiB of foreground
  memory; a Game is capped at 5 GiB. RPCS3's minimum is 8 GiB, so even the Game
  tier is below minimum — the host therefore runs under a memory governor and
  targets 720p. Light titles first; large titles are expected to struggle.
- **No Vulkan on Xbox Dev Mode.** Dev Mode supports DirectX 11/12 only. RPCS3's
  renderers are Vulkan and OpenGL, so the RSX backend here is a D3D12 backend
  written against the RSX command stream.
- **Signed DXIL.** Runtime-compiled or unsigned DXIL is not accepted for
  pipeline state on the console. Shaders are translated offline
  (GLSL → SPIR-V → DXIL), hashed into a DXBC container, and baked into a cache
  the host loads. See `dxil/dxil_sign_candidate.h` for the container-hash
  signer.
- **CPU.** Series S is 8-core Zen 2 with AVX2; Dev Mode Games get 4 exclusive +
  2 shared cores. PPU/SPU use the LLVM recompilers, not interpreters.

## Repository layout

| Path | What it is |
|---|---|
| `host/main_uwp_1042.cpp` | UWP entry point: activation, the host watch loop, boot phase, and the main pump. Versioned mains are immutable history — each build gets its own. |
| `host/emu_host.h`, `host/emu_host.cpp` | Host-side emulator bring-up: config directory, VFS, firmware install from a user-supplied PUP in app storage, and core init. |
| `host/boot_probe.h`, `host/boot_probe.cpp` | Boot probes: firmware presence, game file presence, reassembly of chunked uploads, and boot-result reporting. |
| `host/pad_wgi.h` | Windows.Gaming.Input pad mapping declarations (gamepad → Cell pad). |
| `host/rsx_counters.h` | RSX diagnostic counters shared by the host and the backend. |
| `host/crash_capture.cpp` | Crash/VEH logging to the app's LocalState logs. |
| `host/early_config_dir.cpp` | Early config-directory setup, before the core reads its config. |
| `host/callback_queue.h`, `host/callback_queue.cpp` | Callback queue replacing the UWP dispatcher pump for core callbacks. |
| `host/iat_hooks.h`, `host/iat_hooks.cpp` | Import-address-table hooks used to observe process-exit and allocation calls. |
| `host/slice_vs.hlsl` | HLSL source for the display-slice vertex shader (2D blit path). |
| `host/build-1042.ps1` | Host build script: compiles the host objects, links against the prebuilt core library, injects the manifest version, packs and signs the MSIX, and runs the shipped-binary gate (string checks, hash checks). |
| `host/deploy.ps1` | Device Portal operations: certificate install, package install, package list, state, launch, fetch. Takes the Portal address as a `-Portal` runtime parameter — never committed. |
| `host/AppxManifest.xml` | Package manifest template. The build script injects the real package name and version at stage time. |
| `dxil/dxil_sign_candidate.h` | DXBC container-hash signer: wraps a DXIL blob so the console accepts it as signed. |
| `docs/xbox-tuning.yaml` | Starting tuning profile for Series S (720p, LLVM recompilers, governor budgets). Starting values to test, not proven values. |
| `docs/deploy-README.md` | Manual install steps for a built package. |

Not in this commit (follow later): the D3D12 RSX backend sources, the pad
implementation, the watchdog, and the core patch set (guest-memory mapping,
semaphore logging, and related core changes).

## Build intentions

The build is designed to run on a Windows PC and sideload to a Dev Mode
console. The intended pipeline:

1. Clone upstream `RPCS3/rpcs3` (not a website repo) with its submodules, and
   build the core as a library (`emucore.lib`) from that tree.
2. Build the host objects with the host build script, link against
   `emucore.lib`, and gate the shipped binary (fresh object directory,
   explicit compiler/linker exit codes, string checks, packaged-exe hash
   match) before calling the build complete.
3. Pack and sign with a local developer certificate, install via
   `deploy.ps1 -Phase Install`, then **manually flip the package to Game**
   before launching. An un-flipped package runs at the 1 GiB App tier, not
   the 5 GiB Game tier.
4. Firmware and games are supplied by the user into the app's LocalState
   storage (firmware PUP, game files). Nothing of that kind is in this repo.

## Prerequisites

- Windows 10/11 PC, Visual Studio 2022 (C++ workload), Windows 10/11 SDK
- Qt 6.10.3 (MSVC 2022, 64-bit)
- Vulkan SDK 1.4.363.0
- Upstream `RPCS3/rpcs3` at commit `e8979d31b3a22c8bc9a6eae6bdc31756eac8ee89`
  (the commit this host tree was developed against), plus its submodules
- A Windows baseline build of that tree, from which `emucore.lib` is produced
- Xbox Series S in Dev Mode, with the Device Portal enabled

## Build steps (intended)

```powershell
# 1. Build the core library first (Release/x64), then the host:
#    MSBuild emucore.vcxproj /p:SolutionDir=C:\rpcs3\ /m:8
#    (core edits always require the library rebuild before the host build)

# 2. Run the host build script from the host directory:
powershell -ExecutionPolicy Bypass -File build-1042.ps1

# 3. Install the built package (Portal address is a runtime parameter):
Copy-Item -Force out\XboxUwpHost-1042.msix out\XboxUwpHost.msix
powershell -ExecutionPolicy Bypass -File deploy.ps1 -Portal <YOUR-XBOX-DEV-MODE-PORTAL> -Phase Install
powershell -ExecutionPolicy Bypass -File deploy.ps1 -Portal <YOUR-XBOX-DEV-MODE-PORTAL> -Phase List

# 4. In Dev Home, flip the package to Game, then launch it from Dev Home.
```

The `build-1042.ps1` script is versioned: each release gets its own script
derived from the previous one, with the version number updated in every
spelling (plain, dotted, and escaped-dot) before running.

## Legal

- This host layer is released under **GPL-2.0** (`LICENSE`), matching RPCS3's
  license. Any distributed derivative must keep its source available.
- **No Sony firmware, no games, no keys, and no Sony code are included in
  this repo, and none ever will be.** Users supply their own legally dumped
  firmware and games. The host only reads files the user places in the app's
  own storage.
- This project is not affiliated with Sony, Microsoft, or the RPCS3 team.
  PlayStation is a trademark of Sony Interactive Entertainment; Xbox is a
  trademark of Microsoft. No trademarked names are used as the project name.
- Distribution is Dev Mode sideload only. Retail-mode distribution of
  emulators on Xbox is blocked by Microsoft; do not expect a Store listing.
