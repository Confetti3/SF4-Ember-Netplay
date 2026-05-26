# SF4 Enhanced relay diagnostics - broker health, room create/resolve, local port check.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\relay-diag.ps1 [-BrokerUrl URL] [-PackageDir path]

param(
    [string]$BrokerUrl = "",
    [string]$PackageDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BrokerUrl) {
    $BrokerUrl = $env:SF4E_BROKER_URL
}
if (-not $BrokerUrl) {
    $BrokerUrl = "http://74.208.200.95:8787"
}

if (-not $PackageDir) {
    $PackageDir = Join-Path (Split-Path $PSScriptRoot -Parent) "msvc-out\relwithdebinfo"
}

Write-Host "SF4 Enhanced relay diagnostics"
Write-Host "Broker: $BrokerUrl"
Write-Host "Package: $PackageDir"
Write-Host ""

function Fail($msg) {
    Write-Host "[FAIL] $msg"
    exit 1
}

function Ok($msg) {
    Write-Host "[OK]   $msg"
}

$relayHostPath = Join-Path $PackageDir "RelayHost.exe"
if (-not (Test-Path $relayHostPath)) {
    Fail "RelayHost.exe not found at $relayHostPath"
}
Ok "RelayHost.exe present"

try {
    $health = Invoke-RestMethod -Uri "$BrokerUrl/v1/health" -TimeoutSec 10
    if (-not $health.ok) { Fail "Broker health returned ok=false" }
    Ok "Broker health (rooms=$($health.rooms)/$($health.maxRooms))"
} catch {
    Fail "Broker health request failed: $_"
}

try {
    $publicIp = Invoke-RestMethod -Uri "https://api.ipify.org" -TimeoutSec 10
    Ok "Public IP: $publicIp"
} catch {
    Fail "Could not detect public IP: $_"
}

$createBody = @{ displayName = "RelayDiag"; relayHost = $publicIp } | ConvertTo-Json
try {
    $created = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms" -Method POST -ContentType "application/json" -Body $createBody -TimeoutSec 10
} catch {
    Fail "Room create failed: $_"
}

if (-not $created.code -or -not $created.port) {
    Fail "Room create returned incomplete data"
}
Ok "Created room $($created.code) on port $($created.port)"

try {
    $resolved = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms/$($created.code)" -TimeoutSec 10
} catch {
    Fail "Room resolve failed: $_"
}

if ($resolved.host -ne $publicIp -or [int]$resolved.port -ne [int]$created.port) {
    Fail "Resolve mismatch: expected ${publicIp}:$($created.port), got $($resolved.host):$($resolved.port)"
}
Ok "Resolved room to $($resolved.host):$($resolved.port)"

try {
    $heartbeat = Invoke-RestMethod -Uri "$BrokerUrl/v1/rooms/$($created.code)/heartbeat" -Method POST -ContentType "application/json" -Body "{}" -TimeoutSec 10
    if (-not $heartbeat.ok) { Fail "Heartbeat returned ok=false" }
    Ok "Heartbeat accepted"
} catch {
    if ($_.Exception.Response.StatusCode.value__ -eq 404) {
        Write-Host "[WARN] Heartbeat endpoint missing on broker - deploy updated services/room-broker/server.js"
    } else {
        Fail "Heartbeat failed: $_"
    }
}

$relayProc = Start-Process -FilePath $relayHostPath -ArgumentList @("--port", $created.port) -WorkingDirectory $PackageDir -PassThru
Start-Sleep -Seconds 2

if ($relayProc.HasExited) {
    Fail "RelayHost exited immediately with code $($relayProc.ExitCode)"
}
Ok "RelayHost running (pid $($relayProc.Id)) on port $($created.port)"

$listeners = netstat -ano | Select-String ":$($created.port)\s"
if (-not $listeners) {
    Stop-Process -Id $relayProc.Id -Force -ErrorAction SilentlyContinue
    Fail "Nothing listening on port $($created.port)"
}
Ok "netstat shows listener on port $($created.port)"

Stop-Process -Id $relayProc.Id -Force -ErrorAction SilentlyContinue
Ok "RelayHost stopped cleanly"

Write-Host ""
Write-Host "RESULT: PASS"
Write-Host "Next: run Launcher, create relay room, Start game, then have joiner use $($created.code)."
Write-Host "Ensure router forwards TCP+UDP $($created.port) if testing across the internet."
