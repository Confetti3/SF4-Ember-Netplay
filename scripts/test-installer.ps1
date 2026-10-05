# Install a built setup.exe silently over a stale temporary folder, add files the way
# Updater.exe and a player would, uninstall, and check what is left.
param([Parameter(Mandatory=$true)][string]$Installer)
$ErrorActionPreference = 'Stop'
$Installer = (Resolve-Path -LiteralPath $Installer).Path
# Same AppId as a real installation: running this would replace its uninstall entry.
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{791CAF8E-B4B5-45D5-AB5C-B4864DDB8866}_is1'
if (Test-Path -LiteralPath $uninstallKey) { throw 'SF4 Ember Netplay is installed for this user; uninstall it before running this test.' }
$target = Join-Path ([IO.Path]::GetTempPath()) ("ember-installer-test-" + [Guid]::NewGuid().ToString('N'))
# An older folder: a program file a past version shipped, and an unfinished update.
New-Item -ItemType Directory -Path $target | Out-Null
Set-Content -LiteralPath (Join-Path $target 'dxwrapper.dll') -Value 'obsolete'
Set-Content -LiteralPath (Join-Path $target '.ember-update-transaction-v1.json') -Value '{}'
$setup = Start-Process -FilePath $Installer -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/DIR=`"$target`"" -Wait -PassThru
if ($setup.ExitCode -ne 0) { throw "Installer exited with $($setup.ExitCode)" }
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $target 'preflight.ps1') -PackageDir $target
    if ($LASTEXITCODE -ne 0) { throw 'The installed folder fails preflight' }
    foreach ($stale in 'dxwrapper.dll', '.ember-update-transaction-v1.json') { if (Test-Path -LiteralPath (Join-Path $target $stale)) { throw "Installing left $stale in place" } }
    # What an update leaves behind, and a file the player put there.
    New-Item -ItemType Directory -Path (Join-Path $target '.ember-update-backups\old') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $target '.ember-update-backups\old\Launcher.exe') -Value 'backup'
    Set-Content -LiteralPath (Join-Path $target 'assets\selection\added-by-update.png') -Value 'art'
    Set-Content -LiteralPath (Join-Path $target 'my-notes.txt') -Value 'mine'
} finally {
    $uninstaller = (Get-ItemProperty -LiteralPath $uninstallKey).UninstallString.Trim('"')
    $removal = Start-Process -FilePath $uninstaller -ArgumentList '/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART' -Wait -PassThru
}
if ($removal.ExitCode -ne 0) { throw "Uninstaller exited with $($removal.ExitCode)" }
# The uninstaller finishes from a temporary copy after the first process exits.
for ($i = 0; $i -lt 50 -and (Test-Path -LiteralPath $uninstallKey); $i++) { Start-Sleep -Milliseconds 200 }
$left = @(Get-ChildItem -LiteralPath $target -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($target.Length + 1) })
Remove-Item -LiteralPath $target -Recurse -Force
if (($left -join ',') -ne 'my-notes.txt') { throw "Unexpected files after uninstall: $($left -join ', ')" }
Write-Host 'Installer test passed: stale files replaced, installed folder passes preflight; uninstall removed the product and kept the player file.'
