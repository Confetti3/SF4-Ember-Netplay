param([string]$CMake = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'TestResults.ps1')
$repo = Split-Path $PSScriptRoot -Parent
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $temporaryRoot ('ember-build-validation-' + [guid]::NewGuid().ToString('N'))
$originalPath = $env:PATH
$originalPython = $env:SF4E_PYTHON
$originalToolchain = $env:RUSTUP_TOOLCHAIN
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Reject([scriptblock]$Action, [string]$Message) {
    $rejected = $false
    try { & $Action | Out-Null } catch { $rejected = $true }
    Require $rejected $Message
}
try {
    New-Item -ItemType Directory -Path $work | Out-Null
    $report = Join-Path $work 'tests.xml'
    $valid = '<testsuite tests="2" failures="0" skipped="0"><testcase name="A" status="run"/><testcase name="B" status="run"/></testsuite>'
    [IO.File]::WriteAllText($report,$valid)
    $result = Read-EmberTestResults $report @('A','B')
    Require ($result.passed -and $result.total -eq 2) 'A complete passing report was rejected.'
    Reject { Read-EmberTestResults $report @('Missing') } 'A missing required suite passed.'
    Reject { Read-EmberTestResults (Join-Path $work 'absent.xml') } 'A missing report passed.'
    foreach ($bad in @(
        '<testsuite tests="0"/>',
        $valid.Replace('tests="2"','tests="3"'),
        $valid.Replace('name="B"','name="A"'),
        $valid.Replace('status="run"','status="notrun"'),
        $valid.Replace('failures="0"','failures="1"'),
        $valid.Replace('skipped="0"','skipped="1"'),
        $valid.Replace('<testcase name="A" status="run"/>','<testcase name="A" status="run"><error/></testcase>'),
        $valid.Replace('<testcase name="A" status="run"/>','<testcase name="A" status="run"><failure/></testcase>'),
        $valid.Replace('<testcase name="A" status="run"/>','<testcase name="A" status="run"><skipped/></testcase>'),
        '<not-xml'
    )) {
        [IO.File]::WriteAllText($report,$bad)
        Reject { Read-EmberTestResults $report @('A','B') } "An invalid test report passed: $bad"
    }
    foreach ($version in @('22.18.0','22.19.0','24.0.0','25.0.0')) {
        Require (Test-EmberNodeVersion $version) "Supported Node version rejected: $version"
    }
    foreach ($version in @('20.20.0','22.17.9','23.0.0','23.5.0','23.6.0','invalid')) {
        Require (!(Test-EmberNodeVersion $version)) "Unsupported Node version accepted: $version"
    }
    $early = Join-Path $work 'earlier'
    $chosen = Join-Path $work 'chosen'
    New-Item -ItemType Directory -Path $early,$chosen | Out-Null
    $pythonName = if ($IsWindows) { 'python.exe' } else { 'python' }
    $selectedPython = Join-Path $chosen $pythonName
    [IO.File]::WriteAllText($selectedPython,'fixture only; never executed')
    [IO.File]::WriteAllText((Join-Path $early $pythonName),'fixture only; never executed')
    $env:PATH = (@($early,$chosen,$originalPath) -join [IO.Path]::PathSeparator)
    $env:SF4E_PYTHON = $selectedPython
    Enter-EmberLocalTools $work
    Require (($env:PATH -split [IO.Path]::PathSeparator)[0] -eq $chosen) 'An existing later override directory was not moved first.'
    Require ((Resolve-EmberExecutable 'python' $env:SF4E_PYTHON) -eq $selectedPython) 'The checked interpreter differs from the selected file.'
    $env:SF4E_PYTHON = Join-Path $work 'missing-python'
    Reject { Enter-EmberLocalTools $work } 'Invalid SF4E_PYTHON was silently ignored.'
    $env:SF4E_PYTHON = $originalPython
    $env:PATH = $originalPath

    if ($CMake) {
        # Configure only: no compiler, SDK, real service builds, or network required.
        $fixture = Join-Path $work 'registration'
        foreach ($relative in @('cmake','rust/sf4-net','server/ember','server/ember-short','server/ember-rooms')) {
            New-Item -ItemType Directory -Path (Join-Path $fixture $relative) -Force | Out-Null
        }
        Copy-Item -LiteralPath (Join-Path $repo 'cmake/sf4e_rust_toolchain.cmake') -Destination (Join-Path $fixture 'cmake')
        Copy-Item -LiteralPath (Join-Path $repo 'rust/sf4-net/rust-toolchain.toml') -Destination (Join-Path $fixture 'rust/sf4-net')
        $pin = Get-EmberRustChannel $repo
        $fakeCargo = Join-Path $fixture $(if ($IsWindows) { 'cargo.cmd' } else { 'cargo' })
        $fakeText = if ($IsWindows) {
            @('@echo off', ('if not "%RUSTUP_TOOLCHAIN%"=="' + $pin + '" exit /b 9'), 'exit /b 0') -join [Environment]::NewLine
        } else {
            @('#!/bin/sh', ('test "$RUSTUP_TOOLCHAIN" = "' + $pin + '"')) -join [char]10
        }
        [IO.File]::WriteAllText($fakeCargo,$fakeText + [Environment]::NewLine)
        if (!$IsWindows) { & chmod '+x' $fakeCargo; if ($LASTEXITCODE) { throw 'Fixture chmod failed' } }
        $repoForward = $repo.Replace('\','/')
        $cmakeText = @"
cmake_minimum_required(VERSION 3.26)
project(Registration NONE)
include(CTest)
set(SF4E_BUILD_IROH_HELPER OFF)
if(FORCE_NONWINDOWS)
    set(WIN32 FALSE)
endif()
include("$repoForward/cmake/sf4e_tests.cmake")
include("$repoForward/cmake/sf4e_component_tests.cmake")
"@
        [IO.File]::WriteAllText((Join-Path $fixture 'CMakeLists.txt'),$cmakeText)
        $pwsh = (Get-Process -Id $PID).Path
        $ctest = Join-Path (Split-Path $CMake -Parent) $(if ($IsWindows) { 'ctest.exe' } else { 'ctest' })
        # A wrong machine default must not override the pin on any Rust suite.
        $env:RUSTUP_TOOLCHAIN = 'deliberately-unavailable-default'
        foreach ($nonWindows in @('OFF','ON')) {
            $out = Join-Path $fixture "out-$nonWindows"
            & $CMake -S $fixture -B $out "-DFORCE_NONWINDOWS=$nonWindows" "-DSF4E_CARGO_EXECUTABLE=$fakeCargo" "-DSF4E_PWSH_EXECUTABLE=$pwsh" "-DSF4E_NODE_EXECUTABLE=$pwsh" | Out-Null
            if ($LASTEXITCODE) { throw 'Registration fixture configure failed.' }
            $listing = & $ctest --test-dir $out -C Debug --show-only=json-v1 | ConvertFrom-Json
            if ($LASTEXITCODE) { throw 'Registration fixture listing failed.' }
            foreach ($name in @('EmberRust','ember-shortRust','ember-roomsRust','BridgeSdk','BuildValidation')) {
                Require ($name -cin @($listing.tests.name)) "Suite vanished with helper disabled: $name; registered: $($listing.tests.name -join ', ')"
            }
            & $ctest --test-dir $out -C Debug --no-tests=error --output-on-failure -R '^(EmberRust|ember-shortRust|ember-roomsRust)$' | Out-Null
            if ($LASTEXITCODE) { throw 'A registered service did not receive the pinned toolchain.' }
        }
    }
    Write-Output 'Build validation passed: JUnit failures/omissions, Node boundaries, tool overrides, and requested CMake registration fixtures.'
} finally {
    $env:PATH = $originalPath
    $env:SF4E_PYTHON = $originalPython
    $env:RUSTUP_TOOLCHAIN = $originalToolchain
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if (!$resolvedWork.StartsWith($temporaryRoot, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolvedWork) -notlike 'ember-build-validation-*') { throw 'Unexpected fixture cleanup path.' }
    if (Test-Path -LiteralPath $resolvedWork) { Remove-Item -LiteralPath $resolvedWork -Recurse -Force }
}
