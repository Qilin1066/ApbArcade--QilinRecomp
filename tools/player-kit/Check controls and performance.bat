@echo off
setlocal
cd /d "%~dp0"
if not exist "kit\play.ps1" (
    echo APB's files aren't all here. If you opened the zip, unpack it first:
    echo right-click it, Extract All, then run this from the folder.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "kit\play.ps1" -HardwareCheck
if not "%errorlevel%"=="0" pause
