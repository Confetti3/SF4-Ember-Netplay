@echo off
rem Starts Ember Stream Host (EmberStreamHost.ps1 beside this file) without a console window.
start "" powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "%~dp0EmberStreamHost.ps1"
