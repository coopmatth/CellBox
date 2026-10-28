<#
.SYNOPSIS
  Fetch and install the official PS3 title update(s) for a game (default: Call of Duty Black Ops III, BLUS31527).

.DESCRIPTION
  This is the "update from servers" tool for the CellBox / RPCS3 Xbox port. It does NOT log in to PSN and does not use any
  account: Sony publishes title-update metadata at a public, unauthenticated URL, the same one RPCS3 itself queries:
      https://a0.ww.np.dl.playstation.net/tpl/np/<TITLEID>/<TITLEID>-ver.xml
  (TLS verification is skipped, exactly as RPCS3 does, because that host uses a Sony CA.)
  The script
    1. downloads <TITLEID>-ver.xml and lists every <package> (version, size, sha1sum, url),
    2. downloads each PKG with curl.exe (resumable, -C -) into -CacheDir,
    3. verifies size and SHA1 against the XML,
    4. extracts the retail PKG (AES-128-CTR with the public PS3 retail PKG key, the same algorithm RPCS3 uses in
       Crypto/unpkg.cpp) into <Hdd0Dir>\game\<TITLEID>, in version order, honouring the PKG "overwrite" bit like RPCS3,
    5. optionally mirrors the installed tree into -StageDir for upload to the Xbox app's LocalState (\LocalState\dev_hdd0\...).
  Nothing here talks to the Xbox; upload is a separate, manual step.

.EXAMPLE
  .\Install-PS3Update.ps1 -TitleId BLUS31527 -CacheDir C:\cellbox\ps3-updates\BLUS31527 `
      -Hdd0Dir C:\cellbox\build\bin\dev_hdd0 -StageDir C:\cellbox\ps3-updates\xbox-stage\dev_hdd0
#>
param(
  [string]$TitleId = 'BLUS31527',
  [Parameter(Mandatory=$true)][string]$CacheDir,
  [Parameter(Mandatory=$true)][string]$Hdd0Dir,
  [string]$StageDir = '',
  [switch]$SkipDownload,
  [switch]$NoInstall
)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
[Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

public static class Ps3Pkg
{
    // Public PS3 retail PKG key (identical to rpcs3/Crypto/key_vault.h PKG_AES_KEY).
    static readonly byte[] KEY = new byte[] { 0x2e,0x7b,0x71,0xd7,0xc9,0xc9,0xa1,0x4e,0xa3,0x22,0x1f,0x18,0x88,0x28,0xb8,0xf8 };

    static uint BE32(byte[] b, int o) { return ((uint)b[o] << 24) | ((uint)b[o+1] << 16) | ((uint)b[o+2] << 8) | b[o+3]; }
    static ulong BE64(byte[] b, int o) { return ((ulong)BE32(b, o) << 32) | BE32(b, o + 4); }
    static void PutBE64(byte[] b, int o, ulong v) { for (int i = 7; i >= 0; i--) { b[o + i] = (byte)(v & 0xff); v >>= 8; } }

    public class Result { public int Files; public int Dirs; public int Skipped; public int Raw9; public long Bytes; public string InstallDir; public string Log = ""; }

    sealed class Cipher : IDisposable
    {
        FileStream fs; ulong dataOff; ulong hi, lo; ICryptoTransform enc; Aes aes;
        public Cipher(FileStream f, ulong dataOffset, byte[] klic)
        {
            fs = f; dataOff = dataOffset;
            hi = BE64(klic, 0); lo = BE64(klic, 8);
            aes = Aes.Create(); aes.Mode = CipherMode.ECB; aes.Padding = PaddingMode.None; aes.Key = KEY;
            enc = aes.CreateEncryptor();
        }
        public void Dispose() { enc.Dispose(); aes.Dispose(); }

        // Read len bytes of decrypted data at "offset" within the PKG data section.
        public int Read(ulong offset, byte[] buf, int len)
        {
            fs.Seek((long)(dataOff + offset), SeekOrigin.Begin);
            int got = 0;
            while (got < len) { int n = fs.Read(buf, got, len - got); if (n <= 0) break; got += n; }
            if (got == 0) return 0;
            ulong firstBlock = offset / 16; int skip = (int)(offset % 16);
            int blocks = (skip + got + 15) / 16;
            byte[] ctr = new byte[blocks * 16];
            byte[] ks = new byte[blocks * 16];
            for (int i = 0; i < blocks; i++)
            {
                ulong l = lo + firstBlock + (ulong)i; ulong h = hi + (l < lo ? 1UL : 0UL);
                // carry detection when lo + idx wraps
                if (l < lo) h = hi + 1; else h = hi;
                PutBE64(ctr, i * 16, h); PutBE64(ctr, i * 16 + 8, l);
            }
            enc.TransformBlock(ctr, 0, ctr.Length, ks, 0);
            for (int i = 0; i < got; i++) buf[i] ^= ks[skip + i];
            return got;
        }
    }

    public static string Hex(byte[] b) { var sb = new StringBuilder(); foreach (byte x in b) sb.Append(x.ToString("x2")); return sb.ToString(); }

    // Returns {fullSha1, bodySha1 (all but last 32 bytes), trailer20 (hex of the 20 bytes before the final 12)}
    public static string[] Hashes(string path)
    {
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
        using (var full = new SHA1Managed()) using (var body = new SHA1Managed())
        {
            long len = fs.Length; long bodyLen = len - 32; long pos = 0; byte[] buf = new byte[4 << 20];
            while (pos < len)
            {
                int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, len - pos)); if (n <= 0) break;
                full.TransformBlock(buf, 0, n, null, 0);
                long b = Math.Min((long)n, Math.Max(0L, bodyLen - pos)); if (b > 0) body.TransformBlock(buf, 0, (int)b, null, 0);
                pos += n;
            }
            full.TransformFinalBlock(new byte[0], 0, 0); body.TransformFinalBlock(new byte[0], 0, 0);
            fs.Seek(len - 32, SeekOrigin.Begin); byte[] tr = new byte[32]; fs.Read(tr, 0, 32);
            byte[] t20 = new byte[20]; Array.Copy(tr, 0, t20, 0, 20);
            return new string[] { Hex(full.Hash), Hex(body.Hash), Hex(t20) };
        }
    }

    public static Result Extract(string pkgPath, string gameRoot /* ...\dev_hdd0\game */)
    {
        var r = new Result();
        using (var fs = new FileStream(pkgPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
        {
            byte[] h = new byte[0x80]; fs.Read(h, 0, h.Length);
            if (h[0] != 0x7f || h[1] != 0x50 || h[2] != 0x4b || h[3] != 0x47) throw new Exception("not a PKG (bad magic)");
            uint type = ((uint)h[4] << 8) | h[5]; uint plat = ((uint)h[6] << 8) | h[7];
            if (type != 0x8000 || plat != 1) throw new Exception("unsupported PKG (type=0x" + type.ToString("x") + " platform=" + plat + "); only PS3 retail supported");
            uint fileCount = BE32(h, 0x14); ulong dataOff = BE64(h, 0x20); ulong dataSize = BE64(h, 0x28);
            byte[] klic = new byte[16]; Array.Copy(h, 0x70, klic, 0, 16);
            string installDir = Encoding.ASCII.GetString(h, 55, 9);
            // metadata packet 0xA may carry an explicit install dir (DLC); patches use header title id.
            r.InstallDir = installDir;
            string root = Path.Combine(gameRoot, installDir);
            Directory.CreateDirectory(root);
            string rootFull = Path.GetFullPath(root).TrimEnd('\\') + "\\";

            using (var c = new Cipher(fs, dataOff, klic))
            {
                byte[] tbl = new byte[fileCount * 32];
                if (c.Read(0, tbl, tbl.Length) != tbl.Length) throw new Exception("entry table truncated");
                for (uint i = 0; i < fileCount; i++)
                {
                    int e = (int)(i * 32);
                    ulong nameOff = BE32(tbl, e), nameSize = BE32(tbl, e + 4), fOff = BE64(tbl, e + 8), fSize = BE64(tbl, e + 16); uint etype = BE32(tbl, e + 24);
                    if (nameSize == 0 || nameSize > 256 || nameOff + nameSize > dataSize) throw new Exception("bad entry " + i + " (decrypt failed?)");
                    byte[] nb = new byte[nameSize]; c.Read(nameOff, nb, nb.Length);
                    string name = Encoding.UTF8.GetString(nb).TrimEnd('\0').Replace('/', '\\').TrimStart('\\');
                    string dest = Path.GetFullPath(Path.Combine(root, name));
                    if (!(dest + "\\").StartsWith(rootFull, StringComparison.OrdinalIgnoreCase) && !dest.StartsWith(rootFull, StringComparison.OrdinalIgnoreCase)) throw new Exception("entry escapes install dir: " + name);
                    uint et = etype & 0xff; bool overwrite = (etype & 0x80000000u) != 0;
                    if (et == 4 || et == 0x12) { Directory.CreateDirectory(dest); r.Dirs++; continue; }
                    if (File.Exists(dest) && !overwrite) { r.Skipped++; r.Log += "skip-existing " + name + "\n"; continue; }
                    if (et == 9) { r.Raw9++; r.Log += "WARNING type-9 (SDAT) entry written still-encrypted: " + name + "\n"; }
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    string tmp = dest + ".part";
                    using (var o = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
                    {
                        byte[] buf = new byte[4 << 20]; ulong pos = 0;
                        while (pos < fSize)
                        {
                            int want = (int)Math.Min((ulong)buf.Length, fSize - pos);
                            int n = c.Read(fOff + pos, buf, want); if (n != want) throw new Exception("short read in " + name);
                            o.Write(buf, 0, n); pos += (ulong)n;
                        }
                        r.Bytes += (long)fSize;
                    }
                    if (File.Exists(dest)) File.Delete(dest);
                    File.Move(tmp, dest);
                    r.Files++;
                }
            }
        }
        return r;
    }
}
'@

function Say($m) { Write-Host ("[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m) }

New-Item -ItemType Directory -Force $CacheDir | Out-Null
$xmlUrl = "https://a0.ww.np.dl.playstation.net/tpl/np/$TitleId/$TitleId-ver.xml"
$xmlPath = Join-Path $CacheDir "$TitleId-ver.xml"
Say "GET $xmlUrl"
$wc = New-Object Net.WebClient
$wc.Headers['User-Agent'] = 'Mozilla/5.0 (PLAYSTATION 3; 4.90)'
$wc.DownloadFile($xmlUrl, $xmlPath)
[xml]$x = Get-Content -Raw $xmlPath
$pkgs = @($x.titlepatch.tag.package) | Sort-Object { [version]($_.version -replace '^0','0') }
Say ("ver.xml lists {0} package(s): {1}" -f $pkgs.Count, (($pkgs | % { "v$($_.version) $($_.size)B" }) -join ', '))

$gameRoot = Join-Path $Hdd0Dir 'game'
foreach ($p in $pkgs) {
  $ver = $p.version -replace '\.', ''
  $file = Join-Path $CacheDir ("{0}-A{1}.pkg" -f $TitleId, $ver)
  $size = [int64]$p.size
  if (-not $SkipDownload) {
    $have = 0; if (Test-Path $file) { $have = (Get-Item $file).Length }
    if ($have -ne $size) {
      Say "download v$($p.version) ($have / $size bytes present) <- $($p.url)"
      & curl.exe -L -C - --retry 20 --retry-delay 5 -A 'Mozilla/5.0 (PLAYSTATION 3; 4.90)' -o $file $p.url
      if ($LASTEXITCODE -ne 0) { throw "curl failed ($LASTEXITCODE) for v$($p.version)" }
    }
  }
  if (-not (Test-Path $file)) { throw "missing $file" }
  $got = (Get-Item $file).Length
  if ($got -ne $size) { throw "size mismatch v$($p.version): got $got expected $size" }
  $hs = [Ps3Pkg]::Hashes($file)
  $want = $p.sha1sum.ToLower()
  $how = if ($hs[0] -eq $want) { 'full-file SHA1' } elseif ($hs[1] -eq $want) { 'SHA1 of file minus 32-byte trailer' } elseif ($hs[2] -eq $want) { 'PKG trailer digest' } else { '' }
  if (-not $how) { throw "SHA1 mismatch v$($p.version): xml=$want full=$($hs[0]) body=$($hs[1]) trailer=$($hs[2])" }
  Say "v$($p.version) size OK ($got), SHA1 OK ($how) = $want"
  if (-not $NoInstall) {
    Say "extract v$($p.version) -> $gameRoot"
    $r = [Ps3Pkg]::Extract($file, $gameRoot)
    Say ("installed v{0}: {1} files, {2} dirs, {3} skipped, {4:N0} bytes -> {5}\{6}" -f $p.version, $r.Files, $r.Dirs, $r.Skipped, $r.Bytes, $gameRoot, $r.InstallDir)
    if ($r.Log) { Write-Host $r.Log }
  }
}
if (-not $NoInstall) {
  $sfo = Join-Path $gameRoot "$TitleId\PARAM.SFO"
  if (Test-Path $sfo) {
    $b = [IO.File]::ReadAllBytes($sfo); $s = [Text.Encoding]::ASCII.GetString($b)
    $i = $s.IndexOf('APP_VER'); Say ("installed PARAM.SFO present ({0} bytes); contains APP_VER key: {1}" -f $b.Length, ($i -ge 0))
  } else { Say "WARNING: PARAM.SFO not found in installed tree" }
  if ($StageDir) {
    $dst = Join-Path $StageDir "game\$TitleId"
    Say "stage -> $dst"
    New-Item -ItemType Directory -Force $dst | Out-Null
    & robocopy.exe (Join-Path $gameRoot $TitleId) $dst /E /NFL /NDL /NJH /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE)" }
    $n = (Get-ChildItem $dst -Recurse -File | Measure-Object Length -Sum)
    Say ("staged {0} files, {1:N0} bytes (upload to \LocalState\dev_hdd0\game\{2} via Device Portal)" -f $n.Count, $n.Sum, $TitleId)
  }
}
Say 'DONE'
