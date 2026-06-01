@echo off
setlocal EnableExtensions
set "ROOT=%~dp0"
set "PS1=%ROOT%scripts\preflight.ps1"
if not exist "%PS1%" set "PS1=%ROOT%preflight.ps1"
if not exist "%PS1%" set "PS1=%ROOT%scripts\tester-preflight.ps1"
if not exist "%PS1%" (
    echo.
    echo ERROR: Cannot find preflight.ps1 (expected scripts\preflight.ps1).
    echo        Re-extract the full release zip, or run from the package folder.
    echo.
    pause
    exit /b 1
)
set "PKG=%ROOT%"
if "%PKG:~-1%"=="\" set "PKG=%PKG:~0,-1%"
powershell -NoProfile -File "%PS1%" -PackageDir "%PKG%" %*
set "EC=%ERRORLEVEL%"
if %EC% NEQ 0 (
    echo.
    pause
)
exit /b %EC%
