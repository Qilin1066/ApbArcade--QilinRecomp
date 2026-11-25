@echo off
rem Play APB: double-click. The first time it builds the game from your ROMs
rem (a few minutes); after that it just starts it.
setlocal
cd /d "%~dp0"
if not exist "kit\play.ps1" (
    echo APB's files aren't all here. If you opened the zip, unpack it first:
    echo right-click it, Extract All, then run Play APB.bat from the folder.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "kit\play.ps1" %*
if not "%errorlevel%"=="0" (
    echo.
    echo Something stopped the build: the red line above says what.
    pause
)
