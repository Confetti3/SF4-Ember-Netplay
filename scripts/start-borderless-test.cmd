@echo off
setlocal
if not exist "%~dp0Launcher.exe" (
    echo Extract the entire test ZIP before using this shortcut.
    pause
    exit /b 1
)
powershell.exe -NoProfile -Command "if (Get-Process -Name SSFIV,Launcher -ErrorAction SilentlyContinue) { Write-Host 'Close the game and all Ember launchers normally, then run this shortcut again.'; exit 1 }"
if errorlevel 1 (
    pause
    exit /b 1
)
set "SF4E_BORDERLESS_TEST=1"
start "" /D "%~dp0" "%~dp0Launcher.exe"
endlocal
