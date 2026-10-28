# Core patch set

Unified diffs against upstream `RPCS3/rpcs3` at commit
`e8979d31b3a22c8bc9a6eae6bdc31756eac8ee89`. Diffs only — no full upstream
files, no library binaries. Apply from the clone root:

```sh
git apply patches/vm.cpp.diff patches/vm_native.cpp.diff \
  patches/nv47_sync.hpp.diff patches/nv406e.cpp.diff patches/nv4097.cpp.diff \
  patches/cellPad.cpp.diff patches/cellMsgDialog.cpp.diff \
  patches/AudioBackend.cpp.diff patches/PPUThread.cpp.diff
```

## What each diff does

| Diff | Area |
|---|---|
| `vm.cpp.diff` | Guest-memory mapping: pagefile-backed SEC_RESERVE section for the hook region, preallocated guest blocks with commit-on-alloc and placeholder coalescing (fits the 5 GiB Game-tier commit limit). |
| `vm_native.cpp.diff` | Native VM backend changes backing the same mapping model. |
| `nv47_sync.hpp.diff` | Shared semaphore sequence counter and previous-label logging for the RSX label at `0x50300F40`. |
| `nv406e.cpp.diff` | NV406E semaphore-acquire timeout logging with sequence numbers. |
| `nv4097.cpp.diff` | NV4097 back-end semaphore-release logging with sequence numbers. |
| `cellPad.cpp.diff` | Pad-delivery logging inside the guest-facing pad read. |
| `cellMsgDialog.cpp.diff` | Message-dialog logging for PSN/system modal calls. |
| `AudioBackend.cpp.diff` | Audio-layout logging at the backend channel site. |
| `PPUThread.cpp.diff` | PPU thread summary helper for periodic wait-state reporting. |

All logging additions are diagnostic: they record values and sequence numbers
and change no guest-visible behavior, no semaphore values, and no timeouts.

## Building the core library with these patches

After applying, rebuild the core library before the host build
(`MSBuild emucore.vcxproj /p:SolutionDir=C:\rpcs3\ /m:8`, Release/x64).
Known build caveat, unrelated to these diffs: the stock `emucore.vcxproj`
include set lacks the LLVM source-tree directory, so compiling the touched
`PPUThread.cpp` through the project file fails on `llvm/IR/Verifier.h`.
Workaround used for the 1.0.43 build: compile the touched core objects with
the host build's full include set (`host/manual-ppu.ps1` shows the pattern)
and swap them into the library (`host/lib-surgery-1043.ps1`), verifying the
new log markers exist in the rebuilt library before linking the host.

## Link inputs the repo does not contain

The host link needs `libsu.rsp` — the core build's CMake dependency libs
minus SDL3-static. Generate it from your core build with
`tools/xbox-uwp-link-test2.ps1` (it assembles `libsu.rsp` from the build's
`libs4.rsp`); the response file itself is a build artifact and is not
stored here. The prebuilt `emucore.lib` is likewise a build output, not a
repo file.
