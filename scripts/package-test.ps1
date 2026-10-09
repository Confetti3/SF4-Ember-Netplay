# Separate SDK-free private-test packaging; does not weaken release receipt gates.
param([string]$Label = ('delay-test-' + (Get-Date -Format 'yyyyMMdd-HHmmss')))
$ErrorActionPreference = 'Stop'
if ($Label -notmatch '^[a-zA-Z0-9._-]+$') { throw 'Invalid package label' }
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'BuildEnvironment.ps1')
. (Join-Path $PSScriptRoot 'BuildProvenance.ps1')
$target = Get-EmberBuildTarget $repo -WithoutDiscord
$build = Join-Path $repo $target.buildDirectory
Assert-CMakeSource $repo $build
if ((Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt')) -notcontains 'SF4E_BUILD_DISCORD:BOOL=OFF') { throw 'Test packaging requires the SDK-free build' }
$tools = Get-EmberToolPaths $repo
Enter-EmberVcEnvironment $tools.VisualStudioPath
$cmakeBin = Join-Path $tools.VisualStudioPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin'
$python = if ($env:SF4E_PYTHON) { $env:SF4E_PYTHON } else { 'python' }
$fingerprint = Get-SourceFingerprint $repo
$dependencies = Get-DependencyReceipt $repo $build
& (Join-Path $cmakeBin 'cmake.exe') --build $build --parallel 4
if ($LASTEXITCODE) { throw 'Build failed' }
$testPattern = 'Display|Borderless|PrivateTest|PrivateBuildRetention|InputDelay|AutoDelay|RoomAuthority|RoomPrivateGolden|Session|Settings|ShellJourney|RoomPanel|Localization|Ggpo|Rollback|SaveState|Memento|OverlayPresentation|ControllerNavigation|PackageDocs|StartupHandshake|GameCompatibility|PackageInstaller'
$testResult = Join-Path $build "$Label-tests.xml"
$env:SF4E_DISPLAY_INTEGRATION_TEST = '1'
# Windows PowerShell fixtures must discover their own built-in modules, not
# inherit PowerShell 7's module locations from the packaging host.
$env:PSModulePath = $null
& (Join-Path $cmakeBin 'ctest.exe') --test-dir $build -R $testPattern --output-on-failure --output-junit $testResult
if ($LASTEXITCODE) { throw 'Targeted test gate failed' }
if ((Get-SourceFingerprint $repo) -ne $fingerprint) { throw 'Source changed while building/testing' }
[xml]$junit = Get-Content -LiteralPath $testResult -Raw
if ([int]$junit.testsuite.skipped -ne 0) { throw 'Selected test gate skipped required checks' }
$receipt = @{
    kind = 'private-test-build'; releaseValidated = $false; discordEnabled = $false
    baseRevision = (& git -C $repo rev-parse HEAD); sourceFingerprint = $fingerprint
    localChanges = $true; createdUtc = [DateTime]::UtcNow.ToString('o')
    tests = @{ selected = $testPattern; total = [int]$junit.testsuite.tests; failures = [int]$junit.testsuite.failures; skipped = [int]$junit.testsuite.skipped }
    rustLockSha256 = $dependencies.rustLockSha256
    limitations = @('Match-start loading cover and menu-input suppression require native first-match/rematch/spectator/cancellation acceptance', 'Inline room chat passed automated interaction/render checks; native two-player acceptance pending', 'Prior borderless prototype accepted by user; expanded windowed/fullscreen/monitor controls need native acceptance', 'Prior round-transition candidate completed four native 0/0 matches; unequal-delay acceptance remains pending', 'Full release and upgrade-recovery gates not run by this test packager', 'Fresh-folder extraction only; public updates disabled')
}
$receiptPath = Join-Path $build "$Label-provenance.json"
$receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
$crt = Join-Path $env:VCToolsRedistDir 'x86/Microsoft.VC145.CRT'
if (!(Test-Path -LiteralPath $crt)) { throw "Redistributable runtime directory missing: $crt" }
& $python -X utf8 (Join-Path $PSScriptRoot 'package-test.py') --build-dir $build --label $Label --receipt $receiptPath --crt $crt
if ($LASTEXITCODE) { throw 'Test package assembly failed' }
