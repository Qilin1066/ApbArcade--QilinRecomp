@echo off
rem Update APB: double-click. Gets the latest code from GitHub, rebuilds what
rem changed, and starts the game.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-apb.ps1" -Pull -Run
if not "%errorlevel%"=="0" (
    echo.
    echo The update stopped. The message above says where; copy it for Claude.
    pause
)
