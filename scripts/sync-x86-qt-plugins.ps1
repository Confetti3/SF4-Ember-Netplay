# Copy x86 Qt plugins from vcpkg (windeployqt in this repo is x64-only and deploys wrong-arch plugins).
param(
    [string]$Dir = "",
    [string]$BuildDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $PSScriptRoot -Parent
if (-not $Dir) {
    if ($BuildDir) { $Dir = $BuildDir } else { $Dir = Join-Path $RepoRoot "msvc-build\steam-p2p" }
}
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "msvc-build\steam-p2p" }

$pluginSrc = Join-Path $BuildDir "vcpkg_installed\x86-windows-wchar-filenames\Qt6\plugins"
if (-not (Test-Path $pluginSrc)) {
    throw "x86 Qt plugins not found at $pluginSrc"
}

$pluginDest = Join-Path $Dir "plugins"
if (Test-Path $pluginDest) { Remove-Item $pluginDest -Recurse -Force }
Copy-Item -Recurse $pluginSrc $pluginDest -Force

@'
[Paths]
Prefix=.
Plugins=plugins
'@ | Set-Content (Join-Path $Dir "qt.conf") -Encoding Ascii

$d3d = Join-Path ${env:WINDIR} "SysWOW64\d3dcompiler_47.dll"
if (Test-Path $d3d) { Copy-Item $d3d $Dir -Force }

Write-Host "Synced x86 Qt plugins to: $pluginDest"
