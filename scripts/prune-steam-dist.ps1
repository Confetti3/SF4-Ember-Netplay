# Remove old sf4-netplay-p2p-steam-* folders/zips under dist/, keeping the newest N packages.
param(
    [int]$Keep = 2,
    [string]$DistDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $DistDir) { $DistDir = Join-Path $RepoRoot "dist" }
if (-not (Test-Path $DistDir)) {
    Write-Host "No dist folder at $DistDir"
    exit 0
}

$packages = Get-ChildItem $DistDir -Directory -Filter "sf4-netplay-p2p-steam-*" | Sort-Object LastWriteTime -Descending
if ($packages.Count -le $Keep) {
    Write-Host "Keeping all $($packages.Count) steam package folder(s)."
    exit 0
}

$remove = $packages | Select-Object -Skip $Keep
foreach ($dir in $remove) {
    $zip = "$($dir.FullName).zip"
    Write-Host "Removing old package: $($dir.Name)"
    Remove-Item $dir.FullName -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path $zip) { Remove-Item $zip -Force -ErrorAction SilentlyContinue }
}
Write-Host "Kept newest $Keep package(s)."
