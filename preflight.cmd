@echo off
setlocal
powershell -NoProfile -File "%~dp0preflight.ps1" %*
exit /b %ERRORLEVEL%
