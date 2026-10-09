$ErrorActionPreference = 'Stop'

function Get-EmberBuildTarget([string]$SourceRoot, [switch]$WithoutDiscord) {
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
    if ($WithoutDiscord) {
        # Keep SDK-free development output separate from complete release staging.
        $target.buildDirectory = $target.buildDirectory.TrimEnd('\','/') + '-no-discord'
        $target.installDirectory = Join-Path $target.buildDirectory 'stage'
    }
    return $target
}

function Enter-EmberLocalTools([string]$SourceRoot) {
    if ($env:SF4E_NODE -and (Test-Path -LiteralPath $env:SF4E_NODE -PathType Leaf)) {
        $nodeBin = Split-Path $env:SF4E_NODE -Parent
        if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $nodeBin) {
            $env:PATH = $nodeBin + [IO.Path]::PathSeparator + $env:PATH
        }
    }
    $localTools = Join-Path $SourceRoot 'build/tools'
    if (Test-Path -LiteralPath (Join-Path $localTools 'cargo/bin/rustup.exe')) {
        $env:CARGO_HOME = Join-Path $localTools 'cargo'
        $env:RUSTUP_HOME = Join-Path $localTools 'rustup'
        $cargoBin = Join-Path $env:CARGO_HOME 'bin'
        if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $cargoBin) {
            $env:PATH = $cargoBin + [IO.Path]::PathSeparator + $env:PATH
        }
    }
    if ($env:SF4E_PYTHON -and (Test-Path -LiteralPath $env:SF4E_PYTHON -PathType Leaf)) {
        $pythonBin = Split-Path $env:SF4E_PYTHON -Parent
        if (($env:PATH -split [IO.Path]::PathSeparator) -notcontains $pythonBin) {
            $env:PATH = $pythonBin + [IO.Path]::PathSeparator + $env:PATH
        }
    }
}

function Get-EmberToolPaths([string]$SourceRoot, [string]$VisualStudioPath, [string]$VcpkgRoot) {
    Enter-EmberLocalTools $SourceRoot
    if (!$VisualStudioPath) { $VisualStudioPath = $env:SF4E_VISUAL_STUDIO_PATH }
    if (!$VisualStudioPath) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        }
    }
    if (!$VisualStudioPath -or !(Test-Path -LiteralPath (Join-Path $VisualStudioPath 'VC/Auxiliary/Build/vcvarsall.bat'))) {
        throw 'Install Visual Studio 2026 C++ Build Tools or set -VisualStudioPath.'
    }
    if (!$VcpkgRoot) { $VcpkgRoot = $env:VCPKG_ROOT }
    if (!$VcpkgRoot) {
        $VcpkgRoot = @((Join-Path $SourceRoot 'build/tools/vcpkg'),
            (Join-Path (Split-Path $SourceRoot -Parent) 'vcpkg'), (Join-Path $VisualStudioPath 'VC/vcpkg')) |
            Where-Object { Test-Path -LiteralPath (Join-Path $_ 'vcpkg.exe') } | Select-Object -First 1
    }
    if (!$VcpkgRoot) { throw 'Bootstrap vcpkg and set -VcpkgRoot or VCPKG_ROOT.' }
    if (!(Test-Path -LiteralPath (Join-Path $VcpkgRoot 'vcpkg.exe'))) {
        throw 'Bootstrap vcpkg and set -VcpkgRoot or VCPKG_ROOT.'
    }
    [pscustomobject]@{VisualStudioPath=[IO.Path]::GetFullPath($VisualStudioPath);VcpkgRoot=[IO.Path]::GetFullPath($VcpkgRoot)}
}

function Enter-EmberVcEnvironment([string]$VisualStudioPath, [string]$Architecture = 'x86') {
    $environment = & cmd.exe /d /s /c "`"$VisualStudioPath\VC\Auxiliary\Build\vcvarsall.bat`" $Architecture >nul && set"
    if ($LASTEXITCODE) { throw 'Compiler environment failed' }
    foreach ($entry in $environment) { if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1],$Matches[2],'Process') } }
}
