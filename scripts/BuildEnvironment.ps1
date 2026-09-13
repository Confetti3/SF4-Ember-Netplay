$ErrorActionPreference = 'Stop'

function Get-EmberBuildTarget([string]$SourceRoot) {
    $SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
    $workspace = Split-Path $SourceRoot -Parent
    $targetPath = Join-Path $workspace 'build-target.json'
    if (!(Test-Path -LiteralPath $targetPath)) {
        $workspace = $SourceRoot
        $targetPath = Join-Path $SourceRoot 'build-target.json'
    }
    $target = Get-Content -LiteralPath $targetPath -Raw | ConvertFrom-Json
    $expected = [IO.Path]::GetFullPath((Join-Path $workspace $target.sourceDirectory)).TrimEnd('\','/')
    if ($SourceRoot.TrimEnd('\','/') -ne $expected) {
        throw "Wrong build target: $SourceRoot. Designated source: $expected ($targetPath)"
    }
    foreach ($relative in @($target.buildDirectory, $target.installDirectory)) {
        $resolved = [IO.Path]::GetFullPath((Join-Path $SourceRoot $relative))
        if (!$resolved.StartsWith($SourceRoot.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Build and stage paths must be inside the designated source: $resolved"
        }
    }
    return $target
}

function Get-EmberToolPaths([string]$SourceRoot, [string]$VisualStudioPath, [string]$VcpkgRoot) {
    if (!$VisualStudioPath) { $VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH }
    if (!$VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
    if (!$VisualStudioPath -or !(Test-Path -LiteralPath (Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvarsall.bat'))) {
        throw 'Install Visual Studio 2026 C++ Build Tools or set -VisualStudioPath.'
    }
    if (!$VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
    if (!$VcpkgRoot) { $VcpkgRoot = Join-Path (Split-Path $SourceRoot -Parent) 'vcpkg' }
    if (!(Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        throw 'Bootstrap vcpkg and set -VcpkgRoot or VCPKG_ROOT.'
    }
    [pscustomobject]@{VisualStudioPath=[IO.Path]::GetFullPath($VisualStudioPath);VcpkgRoot=[IO.Path]::GetFullPath($VcpkgRoot)}
}
