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
    $entries = @([pscustomobject]@{channel='stable';target=$target})
    foreach ($property in $target.channels.PSObject.Properties) {
        $entries += [pscustomobject]@{channel=$property.Name;target=$property.Value}
    }
    $designated = @()
    $matching = @()
    foreach ($entry in $entries) {
        $expected = [IO.Path]::GetFullPath((Join-Path $workspace $entry.target.sourceDirectory)).TrimEnd('\','/')
        $designated += $expected
        if ($SourceRoot.TrimEnd('\','/') -eq $expected) { $matching += $entry }
    }
    if ($matching.Count -eq 0) {
        throw "Wrong build target: $SourceRoot. Designated source: $($designated -join ', ') ($targetPath)"
    }
    if ($matching.Count -ne 1) { throw "Ambiguous build target: $SourceRoot ($targetPath)" }
    $target = $matching[0].target
    $target | Add-Member -NotePropertyName channel -NotePropertyValue $matching[0].channel -Force
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

function Resolve-EmberExecutable([string]$Name, [string]$Override = '') {
    if ($Override) {
        if (!(Test-Path -LiteralPath $Override -PathType Leaf)) { throw "Tool override is not a file: $Override" }
        return [IO.Path]::GetFullPath($Override)
    }
    return (Get-Command $Name -CommandType Application -ErrorAction Stop).Source
}

function Add-EmberToolDirectory([string]$Directory) {
    $Directory = [IO.Path]::GetFullPath($Directory)
    # Move an existing entry to the front too; an earlier alias must not win.
    $remaining = @($env:PATH -split [IO.Path]::PathSeparator | Where-Object { $_ -and $_ -ne $Directory })
    $env:PATH = (@($Directory) + $remaining) -join [IO.Path]::PathSeparator
}

function Enter-EmberLocalTools([string]$SourceRoot) {
    foreach ($override in @($env:SF4E_NODE, $env:SF4E_PYTHON)) {
        if ($override) { Add-EmberToolDirectory (Split-Path (Resolve-EmberExecutable '' $override) -Parent) }
    }
    $localTools = Join-Path $SourceRoot 'build/tools'
    if (Test-Path -LiteralPath (Join-Path $localTools 'cargo/bin/rustup.exe')) {
        $env:CARGO_HOME = Join-Path $localTools 'cargo'
        $env:RUSTUP_HOME = Join-Path $localTools 'rustup'
        Add-EmberToolDirectory (Join-Path $env:CARGO_HOME 'bin')
    }
}

function Get-EmberRustChannel([string]$SourceRoot) {
    $pin = Get-Content -LiteralPath (Join-Path $SourceRoot 'rust/sf4-net/rust-toolchain.toml') -Raw
    if ($pin -notmatch '(?m)^\s*channel\s*=\s*"([^"]+)"') { throw 'Missing Rust toolchain pin' }
    return $Matches[1]
}

function Test-EmberNodeVersion([string]$Version) {
    $parsed = $null
    if (![version]::TryParse($Version.Trim().TrimStart('v'), [ref]$parsed)) { return $false }
    return ($parsed.Major -eq 22 -and $parsed.Minor -ge 18) -or $parsed.Major -ge 24
}

function Get-EmberValidationTools([string]$SourceRoot, [string]$Node = $env:SF4E_NODE, [string]$Cargo = $env:SF4E_CARGO, [switch]$SdkOnly) {
    Enter-EmberLocalTools $SourceRoot
    $nodePath = Resolve-EmberExecutable 'node' $Node
    $npm = Resolve-EmberExecutable $(if ($IsWindows) { 'npm.cmd' } else { 'npm' })
    $npmCli = @(
        (Join-Path (Split-Path $npm -Parent) 'node_modules/npm/bin/npm-cli.js'),
        (Join-Path (Split-Path $npm -Parent) '../lib/node_modules/npm/bin/npm-cli.js')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (!$npmCli) { throw 'Install npm alongside Node (npm-cli.js was not found).' }
    $pythonPath = if (!$SdkOnly) { Resolve-EmberExecutable 'python' $env:SF4E_PYTHON } else { '' }
    [pscustomobject]@{
        Node=$nodePath; NpmCli=[IO.Path]::GetFullPath($npmCli); Python=$pythonPath
        Cargo=(Resolve-EmberExecutable 'cargo' $Cargo); Rustup=(Resolve-EmberExecutable 'rustup')
        RustChannel=(Get-EmberRustChannel $SourceRoot)
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

# Git for Windows' bash.exe, never a WSL launcher that may be first on PATH. git.exe sits in
# <root>\cmd or <root>\mingw64\bin, so look for <directory>\bin\bash.exe up the path from it.
# SF4E_GIT_BASH overrides the search. Throws when no bash.exe is found.
function Get-EmberGitBash([string]$GitExecutable = '', [string]$Override = $env:SF4E_GIT_BASH) {
    if ($Override) {
        if (!(Test-Path -LiteralPath $Override -PathType Leaf)) { throw "SF4E_GIT_BASH is not a file: $Override" }
        return $Override
    }
    if (!$GitExecutable) {
        $command = Get-Command git -ErrorAction SilentlyContinue
        if (!$command) { throw 'git was not found; set SF4E_GIT_BASH to the path of Git for Windows'' bash.exe.' }
        $GitExecutable = $command.Source
    }
    $directory = Split-Path $GitExecutable -Parent
    for ($level = 0; $directory -and $level -lt 4; $level++) {
        $candidate = Join-Path $directory 'bin\bash.exe'
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        $directory = Split-Path $directory -Parent
    }
    throw "No bash.exe found for $GitExecutable; install Git for Windows or set SF4E_GIT_BASH."
}
