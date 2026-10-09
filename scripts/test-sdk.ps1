param([string]$BuildDir = '', [string]$Node = $env:SF4E_NODE)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
Enter-EmberLocalTools $repo
if (!$BuildDir) { $BuildDir = Join-Path $repo 'build/sdk-tests' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)
if (!$Node) { $Node = (Get-Command node -ErrorAction Stop).Source }
& $Node -e 'const [a,b]=process.versions.node.split(".").map(Number); if(a<22 || (a===22 && b<18)) process.exit(1)'
if ($LASTEXITCODE) { throw 'SDK tests require Node 22.18 or later; set SF4E_NODE to its executable.' }
$npm = (Get-Command npm -ErrorAction Stop).Source
$npmCli = @(
    (Join-Path (Split-Path $npm -Parent) 'node_modules/npm/bin/npm-cli.js'),
    (Join-Path (Split-Path $npm -Parent) '../lib/node_modules/npm/bin/npm-cli.js')
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (!$npmCli) { throw 'Install npm alongside Node (npm-cli.js was not found).' }
$toolchain = (Select-String -LiteralPath (Join-Path $repo 'rust/sf4-net/rust-toolchain.toml') -Pattern '^channel = "([^"]+)"').Matches.Groups[1].Value
$target = Join-Path $BuildDir 'rust-ember-target'
$previousIncremental = $env:CARGO_INCREMENTAL
try {
    $env:CARGO_INCREMENTAL = '0'
    & cargo "+$toolchain" build --locked --manifest-path (Join-Path $repo 'server/ember/Cargo.toml') -p ember-bridge --target-dir $target
    if ($LASTEXITCODE) { throw 'SDK test bridge build failed' }
} finally {
    $env:CARGO_INCREMENTAL = $previousIncremental
}
$previousBinary = $env:EMBER_BRIDGE_TEST_BINARY
$previousRequired = $env:EMBER_REQUIRE_BRIDGE_TESTS
$env:EMBER_BRIDGE_TEST_BINARY = Join-Path $target ('debug/ember-bridge' + $(if ($IsWindows) { '.exe' } else { '' }))
$env:EMBER_REQUIRE_BRIDGE_TESTS = '1'
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
    $env:EMBER_REQUIRE_BRIDGE_TESTS = $previousRequired
}
