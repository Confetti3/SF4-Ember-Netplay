# Check that a built setup.exe refuses a folder with files in it, installs into an empty
# temporary folder, and that uninstalling removes the product while keeping the player's files.
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
    # What an update leaves behind, and a file the player put there.
    New-Item -ItemType Directory -Path (Join-Path $target '.ember-update-backups\old') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $target '.ember-update-backups\old\Launcher.exe') -Value 'backup'
    Set-Content -LiteralPath (Join-Path $target 'assets\selection\added-by-update.png') -Value 'art'
    Set-Content -LiteralPath (Join-Path $target 'my-notes.txt') -Value 'mine'
} finally {
    $uninstaller = (Get-ItemProperty -LiteralPath $uninstallKey).UninstallString.Trim('"')
    $removal = Start-Process -FilePath $uninstaller -ArgumentList $silent -Wait -PassThru
}
if ($removal.ExitCode -ne 0) { throw "Uninstaller exited with $($removal.ExitCode)" }
# The uninstaller finishes from a temporary copy after the first process exits.
for ($i = 0; $i -lt 50 -and (Test-Path -LiteralPath $uninstallKey); $i++) { Start-Sleep -Milliseconds 200 }
$left = @(Get-ChildItem -LiteralPath $target -Recurse -File -Force | ForEach-Object { $_.FullName.Substring($target.Length + 1) })
Remove-Item -LiteralPath $target -Recurse -Force
if (($left -join ',') -ne 'my-notes.txt') { throw "Unexpected files after uninstall: $($left -join ', ')" }
Write-Host 'Installer test passed: a folder with files is refused, the installed folder passes preflight, uninstall removed the product and kept the player file.'
