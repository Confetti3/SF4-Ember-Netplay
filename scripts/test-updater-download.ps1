# Test GitHub release zip download paths (same URLs the launcher uses).
# Usage: powershell -NoProfile -File scripts\test-updater-download.ps1

param(
    [string]$Repo = "Confetti3/SF4-Netplay-Launcher"
)

$ErrorActionPreference = "Stop"

function Test-DownloadMethod {
    param(
        [string]$Label,
        [string]$Url,
        [scriptblock]$Download
    )
    $dest = Join-Path $env:TEMP ("sf4e-updater-test-" + $Label + ".zip")
    if (Test-Path $dest) { Remove-Item -Force $dest }
    try {
        & $Download $Url $dest
        if (-not (Test-Path $dest)) { throw "no output file" }
        $size = (Get-Item $dest).Length
        if ($size -le 0) { throw "empty file" }
        Write-Host ("[OK]   {0} ({1} bytes)" -f $Label, $size)
        return $true
    }
    catch {
        Write-Host ("[FAIL] {0}: {1}" -f $Label, $_.Exception.Message)
        return $false
    }
    finally {
        if (Test-Path $dest) { Remove-Item -Force $dest -ErrorAction SilentlyContinue }
    }
}

Write-Host "Fetching latest release for $Repo..."
$release = Invoke-RestMethod -Uri "https://api.github.com/repos/$Repo/releases/latest" -Headers @{
    Accept = "application/vnd.github+json"
    "User-Agent" = "sf4e-updater-test"
}

$asset = $release.assets | Where-Object { $_.name -like "sf4-netplay-launcher-*.zip" -or $_.name -like "sf4-enhanced-team-*.zip" } | Select-Object -First 1
if (-not $asset) {
    throw "No sf4-netplay-launcher (or legacy sf4-enhanced-team) zip asset on latest release."
}

Write-Host ("Latest: {0}" -f $release.tag_name)
Write-Host ("Browser URL: {0}" -f $asset.browser_download_url)
Write-Host ("API URL:     {0}" -f $asset.url)
Write-Host ""

$results = @()
$results += Test-DownloadMethod "browser-iwr" $asset.browser_download_url {
    param($Url, $Dest)
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing -MaximumRedirection 10
}
$results += Test-DownloadMethod "curl" $asset.browser_download_url {
    param($Url, $Dest)
    & curl.exe -fL --retry 2 --connect-timeout 30 --max-time 300 -o $Dest $Url
}
$results += Test-DownloadMethod "api-iwr" $asset.url {
    param($Url, $Dest)
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $Dest -Headers @{ Accept = "application/octet-stream" } -UseBasicParsing -MaximumRedirection 10
}

Write-Host ""
if ($results -contains $true) {
    Write-Host "At least one download method succeeded."
    exit 0
}
Write-Host "All download methods failed."
exit 1
