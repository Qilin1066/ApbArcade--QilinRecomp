@echo off
rem Play APB: double-click. Starts the game; the first time (or if it isn't
rem built yet) it builds it first, which takes a while.
setlocal
cd /d "%~dp0"
if exist "build-apb\Release\apb.exe" (
    start "" "build-apb\Release\apb.exe"
    exit /b 0
)
echo APB isn't built yet: building it now. The first build takes 15 to 40 minutes.
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File "tools\build-apb.ps1" -Run
if not "%errorlevel%"=="0" (
    echo.
    echo The build stopped. The message above says where; copy it for Claude.
    pause
)
