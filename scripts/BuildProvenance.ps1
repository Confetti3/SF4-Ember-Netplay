$ErrorActionPreference = 'Stop'
# Public-network and live Discord checks require a separate explicit run. The build receipt names them
# (testsExcluded), so testsPassed never implies they ran (ledger A-015). build-current.ps1 and
# run-package-tests.ps1 both exclude exactly these.
$EmberExcludedTests = 'Iroh(Room|Game|Authorized|Recovery)|CustomRoom(FourTables|Spectators)|DiscordSmoke'
function Get-SourceFingerprint([string]$SourceRoot) {
    $paths = [string[]]@(& git -C $SourceRoot ls-files --cached --others --exclude-standard)
    if ($LASTEXITCODE) { throw 'Source inventory failed' }
    # Fingerprints serialize paths in ordinal order, with exact duplicate removal.
    $paths = [Collections.Generic.SortedSet[string]]::new($paths, [StringComparer]::Ordinal)
    $lines = foreach ($relative in $paths) {
        $path = Join-Path $SourceRoot $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { "$relative $((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash)" }
        else { "$relative DELETED" }
    }
    $designation = Join-Path (Split-Path $SourceRoot -Parent) 'build-target.json'
    if (Test-Path -LiteralPath $designation) { $lines += "WORKSPACE_TARGET $((Get-FileHash -LiteralPath $designation).Hash)" }
    $sha = [Security.Cryptography.SHA256]::Create()
    try { [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes(($lines -join "`n")))).Replace('-','') }
    finally { $sha.Dispose() }
}
function Assert-CMakeSource([string]$SourceRoot, [string]$BuildRoot) {
    $cache = Get-Content -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt')
    $homeLine = @($cache | Where-Object { $_ -like 'CMAKE_HOME_DIRECTORY:INTERNAL=*' })
    if ($homeLine.Count -ne 1) { throw 'CMake source identity missing' }
    $homePath = [IO.Path]::GetFullPath($homeLine[0].Substring($homeLine[0].IndexOf('=')+1))
    if ($homePath -ne [IO.Path]::GetFullPath($SourceRoot)) { throw "CMake source mismatch: $homePath" }
}
function Get-DependencyReceipt([string]$SourceRoot, [string]$BuildRoot) {
    $root = Join-Path $BuildRoot 'dependencies/x86-windows-wchar-filenames'
    $abiPath = Join-Path $root 'share/ggpo/vcpkg_abi_info.txt'
    $abi = Get-Content -LiteralPath $abiPath
    foreach ($file in Get-ChildItem -LiteralPath (Join-Path $SourceRoot 'vcpkg-overlays/ports/ggpo') -File) {
        $expected = "$($file.Name) $((Get-FileHash -LiteralPath $file.FullName).Hash.ToLowerInvariant())"
        if ($abi -cnotcontains $expected) { throw "GGPO artifact was not built from current port input: $($file.Name)" }
    }
    $artifacts = foreach ($relative in @('bin/GGPO.dll','lib/GGPO.lib','include/ggponet.h','share/ggpo/vcpkg_abi_info.txt')) {
        $path = Join-Path $root $relative
        [pscustomobject]@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash}
    }
    [pscustomobject]@{root=$root;artifacts=@($artifacts);rustLockSha256=(Get-FileHash -LiteralPath (Join-Path $SourceRoot 'rust/sf4-net/Cargo.lock')).Hash}
}
# Runs the default CTest set and returns the tests that passed only when run once
# more. With -RerunFailed (Nightly), what failed runs once more, one test at a time:
# tests that time their work can fail on a busy publishing PC. Without it (Stable)
# every test must pass the first time. A test that still fails throws, so the build
# writes no receipt. CTest's output goes to the host, and so into a transcript.
function Invoke-DefaultTests([object]$Ctest, [string]$BuildRoot, [string]$Excluded, [int]$Parallel, [switch]$RerunFailed) {
    $junit = Join-Path $BuildRoot 'ctest-junit.xml'
    $rerunJunit = Join-Path $BuildRoot 'ctest-rerun-junit.xml'
    Remove-Item -LiteralPath $junit, $rerunJunit -ErrorAction SilentlyContinue
    $arguments = @('--test-dir', $BuildRoot, '--output-on-failure', '--no-tests=error', '-E', $Excluded)
    $passedOnRerun = @()
    & $Ctest @arguments --parallel $Parallel --output-junit $junit | Out-Host
    if ($LASTEXITCODE) {
        if (!$RerunFailed) { throw 'Tests failed' }
        $passedOnRerun = @(Get-Content -LiteralPath (Join-Path $BuildRoot 'Testing/Temporary/LastTestsFailed.log') | ForEach-Object { ($_ -split ':', 2)[1] })
        Write-Host "Running the failed tests once more, one at a time: $($passedOnRerun -join ', ')"
        & $Ctest @arguments --rerun-failed --parallel 1 --output-junit $rerunJunit | Out-Host
        if ($LASTEXITCODE) { throw 'Tests failed, and again when run once more' }
        Write-Warning "Passed only when run once more: $($passedOnRerun -join ', ')"
    }
    # The receipt names these components, so the runs themselves must show them.
    $ran = @(foreach ($file in @($junit, $rerunJunit) | Where-Object { Test-Path -LiteralPath $_ }) {
        ([xml](Get-Content -Raw -LiteralPath $file)).testsuite.testcase | Where-Object { $_.status -eq 'run' -and !$_.failure } | ForEach-Object { $_.name }
    })
    # The helper and Ember Rust suites, the SDK and the publisher's PowerShell suites (cmake/sf4e_test_targets.cmake;
    # build-current.ps1 no longer runs those scripts itself). A CMake edit cannot drop one silently, and
    # scripts/test-build-provenance.ps1 names the same set in its fake CTest.
    $missing = @('HelperRust','EmberRust','BridgeSdk','BuildProvenance','NightlyPublication','NightlyPaths','PackageFiles') | Where-Object { $_ -notin $ran }
    if ($missing) { throw "Default CTest did not run: $($missing -join ', ')" }
    $passedOnRerun
}
# Only a Nightly build trusts the staging bridge. A receipt without the field predates
# the check, so it is refused like a mismatch; rebuild the checkout to record it.
function Assert-ReceiptChannel([object]$Receipt, [string]$Channel) {
    $expected = $Channel -ceq 'nightly'
    if ($Receipt.stagingBridge -isnot [bool] -or $Receipt.stagingBridge -ne $expected) {
        throw "The build receipt's staging bridge setting does not match the $Channel channel; rebuild with scripts/build-current.ps1."
    }
    # The channel alone does not prove a Stable build: a checkout whose parent folder has no
    # build-target.json reads its own file, which names only the top-level (stable) entry, so a
    # Nightly checkout set up that way reads as stable. A Stable build comes only from release.
    if ($Channel -ceq 'stable' -and [string]$Receipt.branch -cne 'release') {
        throw "The build receipt is from branch '$($Receipt.branch)'; Stable builds come only from the release branch."
    }
}

# Breakpad symbols for a published build (build-current.ps1 -Symbols): the modules of the game process that
# have PDBs, Sidecar.dll and the vcpkg DLLs it loads, in the store layout the report server reads
# (server/ember-reports/deploy/README.md, Symbols). Mozilla dump_syms runs in WSL Ubuntu from ~/.cargo/bin
# (cargo install dump_syms --locked). Get-EmberDumpSyms checks for it, so a -Symbols build stops before it
# compiles anything, and returns a runner that takes dump_syms's arguments with Windows paths.
function Get-EmberDumpSyms {
    $linuxHome = "$(& wsl -d Ubuntu --exec printenv HOME)".Trim()
    if ($LASTEXITCODE -or !$linuxHome.StartsWith('/')) { throw 'Symbols need WSL Ubuntu, which did not start.' }
    $tool = "$linuxHome/.cargo/bin/dump_syms"
    & wsl -d Ubuntu --exec test -x $tool
    if ($LASTEXITCODE) { throw "dump_syms is not installed in WSL Ubuntu at $tool (cargo install dump_syms --locked)." }
    {
        $arguments = @(foreach ($argument in $args) {
            if ($argument -match '^[A-Za-z]:[\\/]') { "$(& wsl -d Ubuntu --exec wslpath -a $argument)".Trim() } else { $argument }
        })
        & wsl -d Ubuntu --exec $tool @arguments
        if ($LASTEXITCODE) { throw "dump_syms failed: $($args -join ' ')" }
    }.GetNewClosure()
}
# Writes the store to <build>\symbols, replacing an earlier one, and returns what the receipt records: the
# store, the tool's version and each module's <debug file>/<DEBUGID>. Each DLL is read from the copy beside its
# PDB, which must be the staged binary in $Binaries (the receipt's) byte for byte: a staged copy has no PDB
# beside it, and dump_syms then writes exported names only. A symbol file without functions means the PDB was
# not read, and stops the build.
function New-EmberSymbols([string]$BuildRoot, [object[]]$Binaries, [scriptblock]$DumpSyms) {
    $vcpkg = Join-Path $BuildRoot 'dependencies/x86-windows-wchar-filenames/bin'
    $sources = [ordered]@{'Sidecar.dll'=(Join-Path $BuildRoot 'Sidecar.dll')}
    foreach ($name in 'GGPO.dll', 'spdlog.dll', 'fmt.dll', 'zlib1.dll') { $sources[$name] = Join-Path $vcpkg $name }
    $store = Join-Path $BuildRoot 'symbols'
    if (Test-Path -LiteralPath $store) { Remove-Item -LiteralPath $store -Recurse -Force }
    New-Item -ItemType Directory -Path $store | Out-Null
    $written = { @(Get-ChildItem -LiteralPath $store -Recurse -File -Filter '*.sym' | ForEach-Object FullName) }
    $modules = foreach ($source in $sources.GetEnumerator()) {
        $staged = @($Binaries | Where-Object { $_.path -ceq $source.Key })
        if ($staged.Count -ne 1 -or (Get-FileHash -LiteralPath $source.Value).Hash -ne $staged[0].sha256) {
            throw "$($source.Value) is not the staged $($source.Key), so its symbols would not match the release."
        }
        $before = & $written
        & $DumpSyms '-s' $store $source.Value
        $made = @(& $written | Where-Object { $_ -notin $before })
        if ($made.Count -ne 1) { throw "dump_syms wrote no symbol file for $($source.Key)." }
        if (!(Select-String -LiteralPath $made[0] -Pattern '^FUNC ' -Quiet)) { throw "dump_syms wrote no functions for $($source.Key): its PDB was not read." }
        $id = Split-Path $made[0] -Parent
        "$(Split-Path (Split-Path $id -Parent) -Leaf)/$(Split-Path $id -Leaf)"
    }
    [ordered]@{store=$store;generator="$(& $DumpSyms '--version')".Trim();modules=@($modules)}
}
# A published build carries its symbols. Returns the store, refusing a receipt without Sidecar's symbols
# (a build without -Symbols records none) or a store that lost a module since.
function Assert-ReceiptSymbols([object]$Receipt) {
    $symbols = $Receipt.symbols
    if (!$symbols -or !@($symbols.modules | Where-Object { $_ -like 'Sidecar.pdb/*' }).Count) {
        throw 'The build receipt has no Sidecar symbols; rebuild with scripts/build-current.ps1 -Symbols.'
    }
    foreach ($module in $symbols.modules) {
        if (!(Test-Path -LiteralPath (Join-Path $symbols.store $module) -PathType Container)) {
            throw "The symbol store no longer holds $module; rebuild with scripts/build-current.ps1 -Symbols."
        }
    }
    return $symbols.store
}

# -SkipFingerprint leaves out only the fresh fingerprint of the tree (about 2-3 s); the receipt's own paths,
# the self-test flags, the staged binary hashes and the dependency checks all still run. A caller uses it only
# when it re-establishes freshness itself shortly after (Invoke-NightlyPublish does, with Assert-NightlySource).
function Assert-BuildReceipt([string]$SourceRoot, [string]$BuildRoot, [string]$StageRoot, [switch]$SkipFingerprint) {
    Assert-CMakeSource $SourceRoot $BuildRoot
    $receipt = Get-Content -LiteralPath (Join-Path $BuildRoot 'build-provenance.json') -Raw | ConvertFrom-Json
    if ($receipt.sourceRoot -ne $SourceRoot -or $receipt.buildRoot -ne $BuildRoot -or $receipt.stageRoot -ne $StageRoot) { throw 'Build receipt path mismatch' }
    # The in-game self-tests let environment variables drive the game and move the helper's Ember folder.
    # A receipt without the field predates the check, and a cache left ON by hand is as unsafe as one the script wrote.
    if ($receipt.selfTest -ne $false -or (Select-String -LiteralPath (Join-Path $BuildRoot 'CMakeCache.txt') -Pattern '^SF4E_SELFTEST:BOOL=ON' -Quiet)) {
        throw 'The build has the in-game self-test; rebuild with scripts/build-current.ps1 without -SelfTest'
    }
    if (!$SkipFingerprint -and $receipt.sourceFingerprint -ne (Get-SourceFingerprint $SourceRoot)) { throw 'Source changed since verified build; rebuild with scripts/build-current.ps1' }
    foreach ($binary in $receipt.binaries) {
        if ((Get-FileHash -LiteralPath (Join-Path $StageRoot $binary.path)).Hash -ne $binary.sha256) { throw "Staged binary changed: $($binary.path)" }
    }
    if (Test-Path -LiteralPath (Join-Path $SourceRoot 'vcpkg-overlays/ports/ggpo')) {
        if (!$receipt.dependencies) { throw 'Dependency provenance missing; rebuild current target' }
        $current = Get-DependencyReceipt $SourceRoot $BuildRoot
        if ($current.root -ne $receipt.dependencies.root -or $current.rustLockSha256 -ne $receipt.dependencies.rustLockSha256) { throw 'Dependency provenance changed' }
        foreach ($artifact in $receipt.dependencies.artifacts) {
            if ((Get-FileHash -LiteralPath $artifact.path).Hash -ne $artifact.sha256) { throw "Dependency artifact changed: $($artifact.path)" }
        }
        if ((Get-FileHash -LiteralPath (Join-Path $StageRoot 'GGPO.dll')).Hash -ne $current.artifacts[0].sha256) { throw 'Staged GGPO does not match designated dependency' }
    }
    return $receipt
}
