# CellBox 1.1.4 and the PS3 title-update tool

## What 1.1.4 changes

Host (`host/`):
- `d3d12_rsx.cpp`: `kForceScratchDiag` is now `false`, so the slice-2 path draws the guest's own geometry instead of the scratch diagnostic triangle. Quad, quad-strip, polygon, triangle-fan and line-loop primitives are expanded to triangle/line lists (`map_topo`). The vertex shader receives a vertex colour attribute and the pixel shader outputs it, replacing the solid magenta. Texturing is **not** implemented yet; textured draws render with vertex colour only.
- `slice_vs.hlsl` / `slice_ps.hlsl`: vertex-colour pass-through.
- `boot_probe.cpp`: one-shot listing of `/dev_bdvd` and the `/dev_hdd0/game/<id>/USRDIR` zone files at boot, so the log shows which files the guest can see.
- `build-frontend-114.ps1`: manifest `1.1.4.0`, outputs `*-frontend-114.*`. Signing needs your own dev certificate; the password in the repo copy is a placeholder (`CHANGE-ME-DEV-ONLY`).

Core (`patches/1.1.4/`): see `patches/README.md`.

## "Fake PSN" / updating from servers

There is no PSN login and no emulation of Sony account authentication here. PSN stays in RPCS3's *Simulated* mode (offline), which is what lets BO3 get past its network checks without an account. Game updates do not need PSN at all: Sony publishes title-update metadata at a public, unauthenticated URL (the same one RPCS3 uses):

    https://a0.ww.np.dl.playstation.net/tpl/np/BLUS31527/BLUS31527-ver.xml

`tools/Install-PS3Update.ps1` downloads each listed PKG (resumable), verifies size and SHA1 against the XML, extracts it into `dev_hdd0\game\BLUS31527` in version order, and can mirror the tree into a staging folder for upload to the Xbox app's `LocalState\dev_hdd0\game\BLUS31527`. It does not touch the Xbox.

```powershell
.\tools\Install-PS3Update.ps1 -TitleId BLUS31527 -CacheDir C:\cellbox\ps3-updates\BLUS31527 `
    -Hdd0Dir C:\cellbox\build\bin\dev_hdd0 -StageDir C:\cellbox\ps3-updates\xbox-stage\dev_hdd0
```

BLUS31527 updates published: v01.02 (SHA1 b06e7dcc3462181117fd1a2910a93dd5e6e10ed9) and v01.07 (SHA1 97865098e73a981b407d939962308199fe1e6c67). PKG extraction uses the public retail PKG key, as RPCS3's own `unpkg.cpp` does.

## Known gaps

- No texturing in the D3D12 slice path.
- The label force-write after the `0x50300F40` semaphore timeout is a workaround, not a faithful RSX semaphore implementation.
- No in-app "check for update" yet; the tool is run from the PC.
- The Xbox install of the 1.1.4 package and the upload of the staged update tree are manual steps.
