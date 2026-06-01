# Fix Qt plugin folder layout beside Launcher.exe (for existing packages or build output).
param(
    [string]$Dir = ""
)

$ErrorActionPreference = "Stop"
if (-not $Dir) {
    $Dir = if ($PSScriptRoot) { Split-Path $PSScriptRoot -Parent } else { (Get-Location).Path }
}
$pluginRoot = Join-Path $Dir "plugins"
New-Item -ItemType Directory -Path $pluginRoot -Force | Out-Null
foreach ($name in @("platforms", "generic", "imageformats", "networkinformation", "styles", "tls")) {
    $src = Join-Path $Dir $name
    if (-not (Test-Path $src)) { continue }
    $dest = Join-Path $pluginRoot $name
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Move-Item $src $dest -Force
}
@'
[Paths]
Prefix=.
Plugins=plugins
'@ | Set-Content (Join-Path $Dir "qt.conf") -Encoding Ascii
$d3d = Join-Path ${env:WINDIR} "SysWOW64\d3dcompiler_47.dll"
if (Test-Path $d3d) {
    Copy-Item $d3d $Dir -Force
}
Write-Host "Qt layout fixed in: $Dir"
