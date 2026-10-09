@echo off
setlocal
if not exist "%~dp0Launcher.exe" (
    echo Extract the entire package before using this shortcut.
    pause
    exit /b 1
)
start "" /D "%~dp0" "%~dp0Launcher.exe" --native-display --play
endlocal
