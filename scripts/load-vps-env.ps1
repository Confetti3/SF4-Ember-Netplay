# Dot-source in PowerShell before deploy:  . .\scripts\load-vps-env.ps1
$secretsFile = Join-Path $PSScriptRoot ".vps-env.ps1"
if (Test-Path -LiteralPath $secretsFile) {
    . $secretsFile
    return
}
$userPass = [Environment]::GetEnvironmentVariable("SF4E_VPS_PASSWORD", "User")
if ($userPass) {
    $env:SF4E_VPS_PASSWORD = $userPass
    $env:SF4E_VPS_HOST = [Environment]::GetEnvironmentVariable("SF4E_VPS_HOST", "User")
    $env:SF4E_VPS_USER = [Environment]::GetEnvironmentVariable("SF4E_VPS_USER", "User")
    return
}
Write-Warning "No scripts\.vps-env.ps1 and no User SF4E_VPS_PASSWORD. Run .\scripts\setup-vps-env.ps1"
