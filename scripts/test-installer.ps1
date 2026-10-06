# Check that a built setup.exe refuses a folder with files in it, installs into an empty
# temporary folder, that uninstalling stops untouched while an update holds the folder, and
# that it then removes the product (including what Updater.exe added later, and whatever has
# happened to the folder's own Updater.exe) while keeping the player's files.
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
# Same AppId as a real installation: running this would replace its uninstall entry.
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{791CAF8E-B4B5-45D5-AB5C-B4864DDB8866}_is1'
if (Test-Path -LiteralPath $uninstallKey) { throw 'SF4 Ember Netplay is installed for this user; uninstall it before running this test.' }
$target = Join-Path ([IO.Path]::GetTempPath()) ("ember-installer-test-" + [Guid]::NewGuid().ToString('N'))
$silent = '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
# An existing copy, however old or broken, is the updater's business.
New-Item -ItemType Directory -Path $target | Out-Null
Set-Content -LiteralPath (Join-Path $target 'dxwrapper.dll') -Value 'obsolete'
$refused = Start-Process -FilePath $Installer -ArgumentList ($silent + "/DIR=`"$target`"") -Wait -PassThru
if ($refused.ExitCode -eq 0) { throw 'The installer accepted a folder that already had files in it' }
if ((Get-ChildItem -LiteralPath $target -Force).Count -ne 1) { throw 'The refused installation changed the folder' }
Remove-Item -LiteralPath $target -Recurse -Force
$setup = Start-Process -FilePath $Installer -ArgumentList ($silent + "/DIR=`"$target`"") -Wait -PassThru
if ($setup.ExitCode -ne 0) { throw "Installer exited with $($setup.ExitCode)" }
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $target 'preflight.ps1') -PackageDir $target
    if ($LASTEXITCODE -ne 0) { throw 'The installed folder fails preflight' }
    # What updates leave behind: a backup set, an added selection asset, a file a past version
    # shipped; and the player's own files, one of them inside the product's assets folder.
    New-Item -ItemType Directory -Path (Join-Path $target '.ember-update-backups\old') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $target '.ember-update-backups\old\Launcher.exe') -Value 'backup'
    New-Item -ItemType Directory -Path (Join-Path $target 'assets\selection') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $target 'assets\selection\sources.json') -Value 'added-by-update'
    Set-Content -LiteralPath (Join-Path $target 'dxwrapper.dll') -Value 'obsolete'
    Set-Content -LiteralPath (Join-Path $target 'assets\selection\my-mod.png') -Value 'mine'
    Set-Content -LiteralPath (Join-Path $target 'my-notes.txt') -Value 'mine'
    # Uninstall runs setup's own helper, not the folder's Updater.exe.
    Set-Content -LiteralPath (Join-Path $target 'Updater.exe') -Value 'not a working updater'
    # While an update holds the folder, uninstall stops before anything is removed. The
    # uninstaller finishes from a temporary copy, so wait for the helper's log line.
    $uninstaller = (Get-ItemProperty -LiteralPath $uninstallKey).UninstallString.Trim('"')
    $log = Join-Path ([IO.Path]::GetTempPath()) 'sf4-netplay-update.log'
    $logStart = if (Test-Path -LiteralPath $log) { (Get-Item -LiteralPath $log).Length } else { 0 }
    $held = [IO.File]::Open((Join-Path $target '.ember-update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    try {
        Start-Process -FilePath $uninstaller -ArgumentList $silent -Wait | Out-Null
        $failed = $false
        for ($i = 0; $i -lt 100 -and !$failed; $i++) {
            Start-Sleep -Milliseconds 200
            if ((Test-Path -LiteralPath $log) -and (Get-Item -LiteralPath $log).Length -gt $logStart) {
                $stream = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite')
                try { $null = $stream.Seek($logStart, 'Begin'); $failed = (New-Object IO.StreamReader($stream)).ReadToEnd() -match 'uninstall failed' } finally { $stream.Dispose() }
            }
        }
        if (!$failed) { throw 'The uninstall helper did not report the held folder' }
        Start-Sleep -Seconds 3
    } finally { $held.Dispose() }
    if (!(Test-Path -LiteralPath $uninstallKey)) { throw 'A stopped uninstall removed its uninstall entry' }
    if (!(Test-Path -LiteralPath (Join-Path $target 'Launcher.exe'))) { throw 'A stopped uninstall removed product files' }
} finally {
    $uninstaller = (Get-ItemProperty -LiteralPath $uninstallKey).UninstallString.Trim('"')
    $removal = Start-Process -FilePath $uninstaller -ArgumentList $silent -Wait -PassThru
}
if ($removal.ExitCode -ne 0) { throw "Uninstaller exited with $($removal.ExitCode)" }
# The uninstaller finishes from a temporary copy after the first process exits.
for ($i = 0; $i -lt 50 -and (Test-Path -LiteralPath $uninstallKey); $i++) { Start-Sleep -Milliseconds 200 }
$left = @(Get-ChildItem -LiteralPath $target -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($target.Length + 1) } | Sort-Object)
Remove-Item -LiteralPath $target -Recurse -Force
if (($left -join ',') -ne 'assets\selection\my-mod.png,my-notes.txt') { throw "Unexpected files after uninstall: $($left -join ', ')" }
Write-Host 'Installer test passed: a folder with files is refused, the installed folder passes preflight, uninstall stops while an update holds the folder, then removes the product (also after its Updater.exe changed) and keeps the player files.'
