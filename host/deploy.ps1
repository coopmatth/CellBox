param(
    [Parameter(Mandatory=$true)][string]$Portal,
    [Parameter(Mandatory=$true)][ValidateSet("Cert","Install","State","List","Launch","Fetch","All")][string]$Phase
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root "out"
$jar  = Join-Path $out "wdp-cookies.txt"
$msix = Join-Path $out "XboxUwpHost.msix"
$cer  = Join-Path $out "host.cer"
function Get-Csrf {
    curl.exe -k -s -c $jar -o NUL --max-time 10 "$Portal/" | Out-Null
    $line = Get-Content $jar | Where-Object { $_ -match "CSRF-TOKEN" } | Select-Object -First 1
    if (-not $line) { throw "no CSRF-TOKEN cookie from portal" }
    return ($line -split "`t")[-1]
}
function Invoke-Wdp([string]$method, [string]$path, [string[]]$extra) {
    $csrf = Get-Csrf
    $a = @("-k","-s","-b",$jar,"-c",$jar,"-X",$method,"-H","X-CSRF-Token: $csrf","--max-time","120","-w","`nHTTP:%{http_code}`n") + $extra + @("$Portal$path")
    & curl.exe @a
}
function Get-Pkg {
    $json = Invoke-Wdp "GET" "/api/app/packagemanager/packages" @()
    $body = ($json -join "`n") -replace "HTTP:\d+",""
    $pkgs = ($body | ConvertFrom-Json).InstalledPackages | Where-Object { $_.PackageFullName -like "RPCS3XboxUwpHost*" }
    if (-not $pkgs) { throw "RPCS3XboxUwpHost package not installed" }
    return $pkgs[0]
}
switch ($Phase) {
    "Cert" { Invoke-Wdp "POST" "/api/app/packagemanager/certificate" @("-F","host.cer=@$cer") }
    "Install" { Invoke-Wdp "POST" "/api/app/packagemanager/package?package=XboxUwpHost.msix" @("-F","XboxUwpHost.msix=@$msix") }
    "State" { Invoke-Wdp "GET" "/api/app/packagemanager/state" @() }
    "List" { Invoke-Wdp "GET" "/api/app/packagemanager/packages" @() }
    "Launch" {
        $pkg = Get-Pkg; $pfn = $pkg.PackageFullName; $appid = $pkg.PackageRelativeId
        if ([string]::IsNullOrEmpty($appid)) {
            $fam = $pkg.PackageFamilyName; $parts = $pfn -split "_"
            if ($pfn -match "__" -and $parts.Count -ge 5) { $fam = "$($parts[0])_$($parts[-1])" }
            $appid = "$fam!App"
        }
        Write-Host "PFN=$pfn"; Write-Host "APPID=$appid"
        $b = { param($s) [uri]::EscapeDataString([Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($s))) }
        Invoke-Wdp "POST" "/api/taskmanager/app?appid=$(& $b $appid)&package=$(& $b $pfn)" @("-H","Content-Length: 0")
    }
    "Fetch" {
        $pkg = Get-Pkg; $pfn = [uri]::EscapeDataString($pkg.PackageFullName); $path = [uri]::EscapeDataString("\LocalState")
        # lowercase filename= is required (Step 1 fetch lesson).
        Invoke-Wdp "GET" "/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$pfn&path=$path&filename=host-results.txt" @("-o",(Join-Path $out "host-results-device.txt"))
        Get-Content (Join-Path $out "host-results-device.txt") -ErrorAction SilentlyContinue
    }
    "All" {
        & $PSCommandPath -Portal $Portal -Phase Cert
        & $PSCommandPath -Portal $Portal -Phase Install
        Start-Sleep -Seconds 3
        & $PSCommandPath -Portal $Portal -Phase State
    }
}
