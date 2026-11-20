@echo off
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "kit\play.ps1" -HardwareCheck
if errorlevel 1 pause
