#Requires -Version 7
# Takes a build that scripts/build-current.ps1 made on the Linux build PC (the clang-cl cross build,
# docs/release/LAPTOP_BUILD.md) into this checkout's designated build directory, so packaging and publication
# (package-team.ps1, github-release.ps1, publish-nightly.ps1) run here unchanged and nothing compiles here.
#
#   pwsh scripts/import-build.ps1 -BuildHost laptop -RemoteSource /mnt/games/sf4-publish/sf4-current [-ReplaceBuildTree]
#
# Refuses unless the build PC's receipt is a cross build of exactly this checkout: the same commit and branch, the
# same source fingerprint (every file's bytes and the workspace's build-target.json), passing tests, no self-test.
# It copies the stage, the receipt, the symbol store, the dependency files the receipt and the notices name, and
# PackageInstallerTest.exe (packaging's inventory validator, with vcpkg's DLLs beside it). The receipt's paths are
# rewritten to this checkout's and the copy is recorded in it (import: the build PC, its paths and the SHA-256 of its
# receipt, kept beside as build-provenance.buildpc.json). The build PC's CMakeCache.txt is kept as
# CMakeCache.buildpc.txt; CMakeCache.txt then holds only the entries packaging reads, its paths this checkout's.
# Assert-BuildReceipt then checks the result as it checks a local build. An existing build tree that is not an
# earlier import is removed only with -ReplaceBuildTree.
param([Parameter(Mandatory=$true)][string]$BuildHost, [Parameter(Mandatory=$true)][string]$RemoteSource,
      [switch]$ReplaceBuildTree)
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$repo = Split-Path $PSScriptRoot -Parent
$target = Get-EmberBuildTarget $repo
$build = Join-Path $repo $target.buildDirectory
$stage = Join-Path $repo $target.installDirectory
if ($RemoteSource -cnotmatch '^/[A-Za-z0-9._/-]+$') { throw "-RemoteSource must be an absolute Linux path: $RemoteSource" }
if ($BuildHost -cnotmatch '^[A-Za-z0-9._-]+$') { throw "Invalid -BuildHost: $BuildHost" }
$RemoteSource = $RemoteSource.TrimEnd('/')
$buildRelative = ([string]$target.buildDirectory).Replace('\', '/').Trim('/')
$stageRelative = ([string]$target.installDirectory).Replace('\', '/').Trim('/')
if (!$stageRelative.StartsWith("$buildRelative/", [StringComparison]::Ordinal)) { throw 'The import expects the stage inside the build directory (build-target.json).' }
$remoteBuild = "$RemoteSource/$buildRelative"
$remoteStage = "$RemoteSource/$stageRelative"

function Invoke-Remote([string]$Command, [string]$Failure) {
    $output = & ssh -o BatchMode=yes $BuildHost $Command
    if ($LASTEXITCODE) { throw $Failure }
    $output
}

# The build PC's receipt, read before anything here changes.
$remoteText = (Invoke-Remote "cat '$remoteBuild/build-provenance.json'" "No build receipt at ${BuildHost}:$remoteBuild") -join "`n"
$receipt = $remoteText | ConvertFrom-Json
if ($receipt.sourceRoot -cne $RemoteSource -or $receipt.buildRoot -cne $remoteBuild -or $receipt.stageRoot -cne $remoteStage) {
    throw 'The build PC''s receipt names other paths than -RemoteSource and build-target.json give.'
}
if (!$receipt.PSObject.Properties['toolchain'] -or !$receipt.toolchain.splat) { throw 'The receipt is not a cross build''s (no toolchain splat); only the Linux build PC''s builds are imported.' }
if ($receipt.testsPassed -ne $true) { throw 'The build PC''s build has no passing tests.' }
if ($receipt.selfTest -ne $false) { throw 'The build PC''s build has the in-game self-test.' }
if (!$receipt.windowsHostTests -or $receipt.windowsHostTests.status -cne 'pending' -or $receipt.windowsHostTests.list -cne "$remoteBuild/windows-host-tests.json") {
    throw 'The receipt names no windows_host tests to run here.'
}
# The tests the build PC left for Windows (cmake/sf4e_test_targets.cmake, label windows_host), as its CTest listed them.
$testsText = (Invoke-Remote "cat '$remoteBuild/windows-host-tests.json'" 'Could not read the windows_host test list.') -join "`n"
if ([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($testsText))) -cne $receipt.windowsHostTests.sha256 -and
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($testsText + "`n"))) -cne $receipt.windowsHostTests.sha256) {
    throw 'The windows_host test list differs from the one the receipt names.'
}
$windowsTests = @(($testsText | ConvertFrom-Json).tests)
if (!$windowsTests.Count) { throw 'The windows_host test list is empty.' }
$missingRequired = @($EmberWindowsHostRequiredTests | Where-Object { $_ -notin $windowsTests.name })
if ($missingRequired) { throw "The windows_host tests do not include $($missingRequired -join ', ')." }
# The programs those tests run or hand to a script: files of the build PC's build folder.
$testPrograms = @($windowsTests | ForEach-Object { $_.command } | Where-Object { $_ -like "$remoteBuild/*.exe" } |
    ForEach-Object { $_.Substring($remoteBuild.Length + 1) } | Sort-Object -Unique)
$branch = "$(& git -C $repo branch --show-current)".Trim()
$revision = "$(& git -C $repo rev-parse HEAD)".Trim()
if ($receipt.branch -cne $branch -or $receipt.baseRevision -cne $revision) {
    throw "The build PC built $($receipt.branch) at $($receipt.baseRevision); this checkout is $branch at $revision."
}
if ("$(& git -C $repo status --porcelain --untracked-files=all)".Trim()) { throw 'This checkout has changes or untracked files; the import needs it clean.' }
if ((Get-SourceFingerprint $repo) -cne $receipt.sourceFingerprint) {
    throw 'This checkout''s source fingerprint differs from the build PC''s (line endings, build-target.json or a file).'
}

if (Test-Path -LiteralPath $build) {
    $earlier = Join-Path $build 'build-provenance.json'
    $wasImport = (Test-Path -LiteralPath $earlier) -and [bool]((Get-Content -LiteralPath $earlier -Raw | ConvertFrom-Json).PSObject.Properties['import'])
    if (!$wasImport -and !$ReplaceBuildTree) { throw "$build holds a local build; pass -ReplaceBuildTree to remove it for the import." }
    Remove-Item -LiteralPath $build -Recurse -Force
}
New-Item -ItemType Directory -Path $build | Out-Null

# What packaging and the receipt checks read, as one archive.
$dependencies = 'dependencies/x86-windows-wchar-filenames'
$items = @('build-provenance.json', 'CMakeCache.txt', 'PackageInstallerTest.exe', 'assets/input/README.md', 'assets/brand/README.md',
    "$dependencies/bin", "$dependencies/share", "$dependencies/lib/GGPO.lib", "$dependencies/include/ggponet.h",
    $stageRelative.Substring($buildRelative.Length + 1))
if ($receipt.symbols) { $items += 'symbols' }
$items += @('windows-host-tests.json') + @($testPrograms | Where-Object { $_ -cne 'PackageInstallerTest.exe' })
$archive = "/tmp/sf4e-import-$([Guid]::NewGuid().ToString('N')).tar"
$local = Join-Path ([IO.Path]::GetTempPath()) (Split-Path $archive -Leaf)
try {
    Invoke-Remote "tar -C '$remoteBuild' -cf '$archive' $(($items | ForEach-Object { "'$_'" }) -join ' ')" 'Could not pack the build on the build PC.' | Out-Null
    & scp -q -o BatchMode=yes "${BuildHost}:$archive" $local
    if ($LASTEXITCODE) { throw 'Could not copy the build from the build PC.' }
    # Windows' own tar (bsdtar): Git's tar on PATH would read the drive letter as a remote host.
    & (Join-Path $env:SystemRoot 'System32/tar.exe') -xf $local -C $build
    if ($LASTEXITCODE) { throw 'Could not unpack the build.' }
} finally {
    Invoke-Remote "rm -f '$archive'" 'Could not remove the archive on the build PC.' | Out-Null
    Remove-Item -LiteralPath $local -ErrorAction SilentlyContinue
}
$receiptPath = Join-Path $build 'build-provenance.json'
if ((Get-Content -LiteralPath $receiptPath -Raw).TrimEnd() -cne $remoteText.TrimEnd()) { throw 'The copied receipt differs from the one read before the copy.' }
Move-Item -LiteralPath $receiptPath -Destination (Join-Path $build 'build-provenance.buildpc.json')
$cachePath = Join-Path $build 'CMakeCache.txt'
Move-Item -LiteralPath $cachePath -Destination (Join-Path $build 'CMakeCache.buildpc.txt')

# The cache entries packaging reads: the source identity (Assert-CMakeSource), the self-test and Ember service
# options (Assert-BuildReceipt) and the dependency tree (collect-notices.py), copied from the build PC's cache with
# paths rewritten to this checkout's.
$remoteCache = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.buildpc.txt')
$kept = foreach ($name in 'SF4E_SELFTEST', 'SF4E_EMBER_SERVICE', 'VCPKG_TARGET_TRIPLET') {
    $line = @($remoteCache | Where-Object { $_ -match "^${name}:[A-Z]+=" })
    if ($line.Count -ne 1) { throw "The build PC's CMakeCache.txt has no single $name entry." }
    $line[0]
}
$homeLine = @($remoteCache | Where-Object { $_ -ceq "CMAKE_HOME_DIRECTORY:INTERNAL=$RemoteSource" })
if ($homeLine.Count -ne 1) { throw 'The build PC''s CMakeCache.txt was not configured from -RemoteSource.' }
Set-Content -LiteralPath $cachePath -Encoding UTF8 -Value (@(
    "# Imported build (scripts/import-build.ps1) from ${BuildHost}:$remoteBuild; the whole cache is CMakeCache.buildpc.txt.",
    "CMAKE_HOME_DIRECTORY:INTERNAL=$repo",
    "VCPKG_INSTALLED_DIR:PATH=$(Join-Path $build 'dependencies')") + $kept)

# The receipt with this checkout's paths, and where it came from.
function ConvertTo-LocalPath([string]$Path) {
    if (!$Path.StartsWith("$RemoteSource/", [StringComparison]::Ordinal)) { throw "Receipt path outside the build PC's checkout: $Path" }
    [IO.Path]::GetFullPath((Join-Path $repo $Path.Substring($RemoteSource.Length + 1)))
}
$receipt.sourceRoot = $repo
$receipt.buildRoot = $build
$receipt.stageRoot = $stage
if ($receipt.dependencies) {
    $receipt.dependencies.root = ConvertTo-LocalPath $receipt.dependencies.root
    foreach ($artifact in $receipt.dependencies.artifacts) { $artifact.path = ConvertTo-LocalPath $artifact.path }
}
if ($receipt.symbols) { $receipt.symbols.store = ConvertTo-LocalPath $receipt.symbols.store }
$sha = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($remoteText))
$receipt | Add-Member -NotePropertyName import -NotePropertyValue ([ordered]@{
    host=$BuildHost; sourceRoot=$RemoteSource; buildRoot=$remoteBuild; stageRoot=$remoteStage
    receiptSha256=[Convert]::ToHexString($sha); importedUtc=[DateTime]::UtcNow.ToString('o')})
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath

# Packaging runs the inventory validator here; vcpkg's DLLs go beside it, as the local build's applocal step does.
Copy-Item -Path (Join-Path $build "$dependencies/bin/*.dll") -Destination $build

# The windows_host tests, natively here, against the programs the build PC built: each test's command with the
# build PC's paths turned into this checkout's, its emulator left out, and its PowerShell and Bash the ones here.
# One at a time, each within its CTest timeout. The receipt records every result; packaging refuses a cross build
# whose windows_host tests did not all pass (Assert-BuildReceipt).
$localPwsh = (Get-Command pwsh -CommandType Application | Select-Object -First 1).Source
$localBash = Get-EmberGitBash
function ConvertTo-LocalArgument([string]$Argument) {
    if ($Argument -ceq $RemoteSource -or $Argument.StartsWith("$RemoteSource/", [StringComparison]::Ordinal)) {
        return [IO.Path]::GetFullPath((Join-Path $repo $Argument.Substring($RemoteSource.Length).TrimStart('/')))
    }
    if ($Argument -match '/pwsh$') { return $localPwsh }
    if ($Argument -match '^/(usr/)?bin/bash$') { return $localBash }
    $Argument
}
$log = Join-Path $build 'windows-host-tests.log'
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
# Tests that compile their source folder's path in (__FILE__) look here instead (package_installer_test.cxx).
$env:SF4E_SOURCE_ROOT = $repo
function Stop-BuildFolderProcesses {
    # A test's child that outlives it (HeapCorruptionCapture's frozen copy) holds its output open; end every
    # process still running from the build folder.
    Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith("$build\", [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}
function Invoke-WindowsHostTest([object]$Test) {
    $command = @($Test.command)
    # A Windows program's test runs through the build PC's emulator (Wine), and Windows PowerShell through env
    # (SF4E_WINDOWS_POWERSHELL): here the program runs itself.
    if ($command.Count -gt 1 -and $command[0] -cnotlike '*.exe' -and ($command[1] -like "$remoteBuild/*.exe" -or $command[1] -ceq 'powershell.exe')) {
        $command = $command[1..($command.Count - 1)]
    }
    $command = @($command | ForEach-Object { ConvertTo-LocalArgument $_ })
    $properties = @{}
    foreach ($property in @($Test.properties)) { $properties[$property.name] = $property.value }
    $directory = if ($properties['WORKING_DIRECTORY']) { ConvertTo-LocalArgument $properties['WORKING_DIRECTORY'] } else { $build }
    $timeout = if ($properties['TIMEOUT']) { [double]$properties['TIMEOUT'] } else { 1500 }
    $start = [Diagnostics.ProcessStartInfo]::new($command[0])
    if ($command.Count -gt 1) { foreach ($argument in $command[1..($command.Count - 1)]) { $start.ArgumentList.Add($argument) } }
    $start.WorkingDirectory = $directory
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $process = [Diagnostics.Process]::Start($start)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $finished = $process.WaitForExit([int]($timeout * 1000))
    if (!$finished) { try { $process.Kill($true) } catch {} }
    $process.WaitForExit(10000) | Out-Null
    Stop-BuildFolderProcesses
    $output = foreach ($task in $stdout, $stderr) { if ($task.Wait(15000)) { $task.Result } else { '(output not closed)' } }
    $code = if ($finished) { $process.ExitCode } else { $null }
    $skipped = $properties['SKIP_RETURN_CODE'] -and $finished -and $code -eq [int]$properties['SKIP_RETURN_CODE']
    $status = if (!$finished) { 'timeout' } elseif ($skipped) { 'skipped' } elseif ($code -eq 0) { 'passed' } else { 'failed' }
    $seconds = [math]::Round($clock.Elapsed.TotalSeconds, 1)
    Add-Content -LiteralPath $log -Value (@("==== $($Test.name): $status ($seconds s)", "$($command -join ' ')") + $output -join "`n")
    Write-Host ("{0,-28} {1} ({2:0.0} s)" -f $Test.name, $status, $seconds)
    [ordered]@{name=$Test.name;status=$status;exitCode=$code;seconds=$seconds}
}
Stop-BuildFolderProcesses
# As the build PC's run (Invoke-DefaultTests -RerunFailed): a test that fails runs once more, alone, and the
# receipt names it.
$results = foreach ($test in $windowsTests) {
    $result = Invoke-WindowsHostTest $test
    if ($result.status -cne 'passed') {
        $again = Invoke-WindowsHostTest $test
        if ($again.status -ceq 'passed') { $again.passedOnRerun = $true }
        $result = $again
    }
    $result
}
$failed = @($results | Where-Object { $_.status -cne 'passed' })
$receipt.import.windowsHostTests = [ordered]@{status=$(if ($failed) { 'failed' } else { 'passed' }); ranOn=[Environment]::MachineName
    ranUtc=[DateTime]::UtcNow.ToString('o'); tests=@($results)}
$receipt | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $receiptPath
if ($failed) { throw "windows_host tests did not pass here: $(($failed | ForEach-Object { "$($_.name) ($($_.status))" }) -join ', '); see $log." }

# The same checks as for a local build: paths, cache entries, fingerprint, every staged binary's hash, the
# dependency artifacts and the staged GGPO.dll; then the channel and the symbol store.
$checked = Assert-BuildReceipt $repo $build $stage -EmberService $target.emberService
Assert-ReceiptChannel $checked $target.channel
if ($checked.symbols) { Assert-ReceiptSymbols $checked | Out-Null }
Write-Host "Imported $($checked.binaries.Count) staged binaries built on $BuildHost ($($checked.toolchain.compiler); CRT $($checked.toolchain.crt)) at $revision into $build."
