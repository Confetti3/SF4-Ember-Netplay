# Deploy broker + tiered room idle to VPS (requires SF4E_VPS_PASSWORD).
# Setup once: copy scripts\.vps-env.ps1.example → scripts\.vps-env.ps1, edit password, then:
#   .\scripts\setup-vps-env.ps1 -FromSecretsFile
$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

. (Join-Path $PSScriptRoot "load-vps-env.ps1")
if (-not $env:SF4E_VPS_PASSWORD) {
    Write-Error "SF4E_VPS_PASSWORD not set. Run .\scripts\setup-vps-env.ps1 -FromSecretsFile first."
}

python scripts/deploy-broker-vps.py
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python scripts/configure-room-idle-vps.py
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ""
Write-Host "Verifying broker health..."
$health = Invoke-RestMethod -Uri "https://74-208-200-95.nip.io/v1/health" -TimeoutSec 20
Write-Host "roomLobbyIdleMs    = $($health.roomLobbyIdleMs)"
Write-Host "roomOccupiedIdleMs = $($health.roomOccupiedIdleMs)"
if ($health.roomLobbyIdleMs -eq 300000 -and $health.roomOccupiedIdleMs -eq 1800000) {
    Write-Host "Tiered idle is live."
} else {
    Write-Warning "Health response missing new idle fields - broker may need restart or deploy retry."
}
