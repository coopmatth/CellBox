# Deploying a built package (manual steps)

The host build script produces `out\XboxUwpHost-<version>.msix`. Installation
is manual: the builder does not install to the console.

## Steps

1. Copy the versioned package to the fixed name the deploy script expects:

   ```powershell
   Copy-Item -Force out\XboxUwpHost-<version>.msix out\XboxUwpHost.msix
   ```

2. Install and list (the Portal address is passed at runtime — it is never
   stored in this repo):

   ```powershell
   powershell -ExecutionPolicy Bypass -File deploy.ps1 -Portal <YOUR-XBOX-DEV-MODE-PORTAL> -Phase Install
   powershell -ExecutionPolicy Bypass -File deploy.ps1 -Portal <YOUR-XBOX-DEV-MODE-PORTAL> -Phase List
   ```

   `deploy.ps1` phases: `Cert`, `Install`, `State`, `List`, `Launch`,
   `Fetch`, `All`. The `Launch` phase uses the Device Portal app-launch
   endpoint, which is known to fail on some consoles; launching from Dev
   Home is the reliable route.

3. **Flip the package to Game before launching.** The tier is not manifest
   content — it is a manual setting in Dev Home. An un-flipped package runs
   at the 1 GiB App tier instead of the 5 GiB Game tier.

4. Launch from Dev Home.

## Notes

- Installing a lower version over a higher one is refused by the Portal.
  Install in ascending version order.
- Uninstalling a package wipes that package family's LocalState, including
  any firmware and game files stored there. Never uninstall to go down a
  version while user-supplied files live in LocalState.
- `deploy.ps1 -Phase Fetch` downloads `host-results.txt` from the app's
  LocalState. Per-version fetch scripts (not in this commit) download the
  full log set.
