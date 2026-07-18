@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "PS1=%ROOT%scripts\preflight.ps1"
if not exist "%PS1%" set "PS1=%ROOT%preflight.ps1"
if not exist "%PS1%" set "PS1=%ROOT%scripts\tester-preflight.ps1"
if not exist "%PS1%" (
    echo(
    echo ERROR: Cannot find preflight.ps1 (expected scripts\preflight.ps1^).
    echo        Re-extract the full release zip, or run from the package folder.
    echo(
    pause
    exit /b 1
)
set "PKG=%ROOT%"
if "%PKG:~-1%"=="\" set "PKG=%PKG:~0,-1%"

rem Optional: preflight.cmd -NoPause   or   set SF4E_PREFLIGHT_NOPAUSE=1
set "NOPAUSE=0"
if /I "%~1"=="-NoPause" set "NOPAUSE=1"
if /I "%SF4E_PREFLIGHT_NOPAUSE%"=="1" set "NOPAUSE=1"

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -PackageDir "%PKG%"
set "EC=%ERRORLEVEL%"
echo(
if "%NOPAUSE%"=="1" goto :done
echo Press any key to close...
pause >nul
:done
exit /b %EC%
