# Exercise real Inno Setup with an isolated identity and dummy game files.
# Only the built native Updater.exe executes. Nothing starts SF4 or changes the
# player's installation, uninstall registration or runtime.
param(
    [Parameter(Mandatory=$true)][string]$Updater,
    [Parameter(Mandatory=$true)][string]$InnoCompiler
)
$ErrorActionPreference = 'Stop'
$Updater = (Resolve-Path -LiteralPath $Updater).Path
$InnoCompiler = (Resolve-Path -LiteralPath $InnoCompiler).Path
$repo = Split-Path $PSScriptRoot -Parent
$guid = '7AB2CDF6-520F-47EC-AB29-549A38DE3821'
$name = 'SF4 Ember Updater Test'
$key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{$guid}_is1"
if (Test-Path -LiteralPath $key) { throw 'An earlier isolated installer test still owns its uninstall entry. Finish its cleanup first.' }
if (Get-Process -Name SSFIV -ErrorAction SilentlyContinue) { throw 'Close SF4 before running this test; the real updater refuses updates while it is running.' }
$root = Join-Path ([IO.Path]::GetTempPath()) ('ember-setup-test-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $root 'package'
$target = Join-Path $root 'installed'
$other = Join-Path $root 'unrecognized'
$null = New-Item -ItemType Directory -Path $stage,$other -Force
function Remove-TestRoot {
    $resolved = [IO.Path]::GetFullPath($root)
    $temp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (!$resolved.StartsWith($temp, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($resolved) -notmatch '^ember-setup-test-[a-f0-9]{32}$') { throw 'Unsafe test cleanup path' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
function Write-Manifest([string]$folder) {
    $lines = @(Get-ChildItem -LiteralPath $folder -Recurse -File | Where-Object {
        $_.Name -ne 'MANIFEST.txt'
    } | ForEach-Object { (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash + '  ' + $_.FullName.Substring($folder.Length + 1) })
    [IO.File]::WriteAllLines((Join-Path $folder 'MANIFEST.txt'), $lines)
}
function Setup([string]$destination, [string]$label) {
    $args = @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="' + $destination + '"'), ('/LOG="' + (Join-Path $root "$label.log") + '"'))
    return (Start-Process -FilePath $installer -ArgumentList $args -WindowStyle Hidden -Wait -PassThru).ExitCode
}
function Check([bool]$condition, [string]$message) { if (!$condition) { throw $message } }
try {
    $files = @('Launcher.exe','Sidecar.dll','Updater.exe','LICENSE','PackageInventory.inc','MANIFEST.txt',
        'next-helper.dll','docs\NEXT_GUIDE.md')
    foreach ($file in $files) {
        $path = Join-Path $stage $file
        $null = New-Item -ItemType Directory -Path (Split-Path $path -Parent) -Force
        [IO.File]::WriteAllText($path, 'new-release')
    }
    Copy-Item -LiteralPath $Updater -Destination (Join-Path $stage 'Updater.exe') -Force
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $stage 'LICENSE') -Force
    [IO.File]::WriteAllLines((Join-Path $stage 'PackageInventory.inc'), @($files | ForEach-Object {
        'SF4E_PACKAGE_REQUIRED("' + $_.Replace('\','\\') + '")'
    }))
    Write-Manifest $stage
    # RedistVersion=0 means this fake redist is never run.
    $redist = Join-Path $root 'unused-redist.exe'
    Copy-Item -LiteralPath $Updater -Destination $redist
    & $InnoCompiler /Q "/DPackageDir=$stage" '/DAppVersion=0.0.0' "/DAppName=$name" "/DAppGuid=$guid" "/DRedist=$redist" '/DRedistVersion=0.0.0.0' "/O$root" (Join-Path $PSScriptRoot 'installer\ember-setup.iss')
    Check ($LASTEXITCODE -eq 0) 'Test installer compilation failed'
    $installer = Join-Path $root 'sf4-ember-netplay-0.0.0-setup.exe'
    [IO.File]::WriteAllText((Join-Path $other 'keep.txt'), 'mine')
    Check ((Setup $other 'unrecognized') -ne 0) 'Accepted an unrelated occupied folder'
    Check ((Get-ChildItem -LiteralPath $other -Force).Count -eq 1) 'Changed an unrelated folder'
    Check ((Setup $target 'fresh') -eq 0) 'Fresh installation failed'
    Check (Test-Path -LiteralPath $key) 'Missing isolated uninstall registration'
    [IO.File]::WriteAllText((Join-Path $target 'Launcher.exe'), 'old-release')
    [IO.File]::WriteAllText((Join-Path $target 'retired-helper.dll'), 'old-owned')
    Remove-Item -LiteralPath (Join-Path $target 'next-helper.dll')
    Write-Manifest $target
    [IO.File]::WriteAllText((Join-Path $target 'my-settings.json'), 'mine')
    [IO.File]::WriteAllText((Join-Path $target 'docs\my-notes.txt'), 'mine')
    Check ((Setup $target 'upgrade') -eq 0) 'Setup did not upgrade the recognized installation'
    Check ([IO.File]::ReadAllText((Join-Path $target 'Launcher.exe')) -eq 'new-release') 'Launcher was not replaced'
    Check ([IO.File]::ReadAllText((Join-Path $target 'next-helper.dll')) -eq 'new-release') 'Future product file missing'
    Check ([IO.File]::ReadAllText((Join-Path $target 'docs\NEXT_GUIDE.md')) -eq 'new-release') 'Nested file missing'
    Check (!(Test-Path -LiteralPath (Join-Path $target 'retired-helper.dll'))) 'Retired product file remains'
    Check ([IO.File]::ReadAllText((Join-Path $target 'my-settings.json')) -eq 'mine') 'Player settings changed'
    Check ([IO.File]::ReadAllText((Join-Path $target 'docs\my-notes.txt')) -eq 'mine') 'Player notes changed'
    [IO.File]::WriteAllText((Join-Path $target 'PRIVATE_TEST_BUILD.txt'), 'private')
    Check ((Setup $target 'private') -ne 0) 'Replaced a private test package'
    Remove-Item -LiteralPath (Join-Path $target 'PRIVATE_TEST_BUILD.txt')
    $lock = [IO.File]::Open((Join-Path $target '.ember-update.lock'),'OpenOrCreate','ReadWrite','None')
    try { Check ((Setup $target 'busy') -ne 0) 'Bypassed the native update lock' } finally { $lock.Dispose() }
    Check ([IO.File]::ReadAllText((Join-Path $target 'Launcher.exe')) -eq 'new-release') 'Busy update changed the launcher'
    Check ((Setup $target 'retry') -eq 0) 'Retry after releasing update lock failed'
    Check ((Get-ChildItem -LiteralPath (Join-Path $target '.ember-update-backups') -Directory).Count -le 1) 'Updates accumulated backup sets'
    Write-Host 'Setup passed: fresh install, occupied-folder refusal, transactional upgrade, future/nested files, old product removal, player-file preservation, private-build refusal, update-lock refusal and retry.'
} catch {
    Get-ChildItem -LiteralPath $root -Filter '*.log' | ForEach-Object {
        Write-Host $_.Name
        Get-Content -LiteralPath $_.FullName -Tail 12 | Write-Host
    }
    throw
} finally {
    if (Test-Path -LiteralPath $key) {
        $uninstaller = (Get-ItemProperty -LiteralPath $key).UninstallString.Trim('"')
        $process = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -WindowStyle Hidden -Wait -PassThru
        for ($i=0; $i -lt 100 -and (Test-Path -LiteralPath $key); $i++) { Start-Sleep -Milliseconds 100 }
        if (Test-Path -LiteralPath $key) { throw "Isolated test uninstall did not finish. Evidence retained in $root" }
    }
    Remove-TestRoot
}
