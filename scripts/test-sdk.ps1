param([string]$BuildDir = '', [string]$Node = $env:SF4E_NODE, [int]$Jobs = 2, [string]$Cargo = $env:SF4E_CARGO)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
$tools = Get-EmberValidationTools $repo -Node $Node -Cargo $Cargo -SdkOnly
if (!$BuildDir) { $BuildDir = Join-Path $repo 'build/sdk-tests' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
$Node = $tools.Node
$nodeVersion = & $Node --version
if ($LASTEXITCODE -or !(Test-EmberNodeVersion $nodeVersion)) { throw 'SDK tests require Node 22.18.x+ within 22, or 24 and later.' }
if ($Jobs -lt 1) { throw 'Jobs must be positive.' }
$npmCli = $tools.NpmCli
$toolchain = $tools.RustChannel
$target = Join-Path $BuildDir 'rust-ember-target'
$previousIncremental = $env:CARGO_INCREMENTAL
try {
    $env:CARGO_INCREMENTAL = '0'
    & $tools.Cargo "+$toolchain" build --locked -j $Jobs --manifest-path (Join-Path $repo 'server/ember/Cargo.toml') -p ember-bridge --target-dir $target
    if ($LASTEXITCODE) { throw 'SDK test bridge build failed' }
} finally {
    $env:CARGO_INCREMENTAL = $previousIncremental
}
$previousBinary = $env:EMBER_BRIDGE_TEST_BINARY
$env:EMBER_BRIDGE_TEST_BINARY = Join-Path $target ('debug/ember-bridge' + $(if ($IsWindows) { '.exe' } else { '' }))
Push-Location (Join-Path $repo 'sdk/typescript')
try {
    & $Node $npmCli ci --ignore-scripts --no-audit --no-fund
    if ($LASTEXITCODE) { throw 'Locked SDK dependency installation failed' }
    & $Node node_modules/typescript/bin/tsc -p tsconfig.json
    if ($LASTEXITCODE) { throw 'SDK TypeScript build failed' }
    & $Node --test --test-reporter=tap 'test/*.test.ts'
    if ($LASTEXITCODE) { throw 'SDK tests failed' }
} finally {
    Pop-Location
    $env:EMBER_BRIDGE_TEST_BINARY = $previousBinary
}
