@echo off
rem Check controls and performance: double-click, once Play APB.exe has built
rem the game. Starts APB (work\APB.exe, with the ROM folder it remembers)
rem recording the inputs, frame and sound timings and a controller report
rem into a new folder under "Hardware checks", with a checklist to fill in.
rem Nothing is uploaded.
setlocal
cd /d "%~dp0"
if not exist "work\APB.exe" goto notbuilt
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set stamp=%%t
if "%stamp%"=="" set stamp=%RANDOM%%RANDOM%
set "reports=Hardware checks\%stamp%"
mkdir "%reports%" || goto failed
set "list=%reports%\checklist.txt"
set /p version=<kit\VERSION
> "%list%" echo APB hardware check - version %version%
>> "%list%" echo.
>> "%list%" echo 1. Test one wheel setup per run. Open F1, Controls, and select your saved setup.
>> "%list%" echo 2. Turn several full turns each way. MOZA R5: check the physical-angle reading keeps
>> "%list%" echo    tracking beyond the ordinary joystick endpoints. A wrapping axis: check the
>> "%list%" echo    dial keeps turning as the reading rolls over. Note Pit House and firmware.
>> "%list%" echo 3. Fully press and release the pedal. Test the siren/start, fire and coin bindings.
>> "%list%" echo 4. Open F1, turn the wheel, then resume. Confirm no unwanted steering jump.
>> "%list%" echo    Repeat with Alt-Tab. Check reconnecting while the wheel is stationary.
>> "%list%" echo 5. Play for at least two minutes with your normal display settings. Note any
>> "%list%" echo    stutters, sound gaps or unexpected force feel, and about when they occur.
>> "%list%" echo 6. Quit APB normally. The reports and the input recording are saved in this folder.
>> "%list%" echo.
>> "%list%" echo Fill these in, then return this folder's files for analysis:
>> "%list%" echo Wheel model:
>> "%list%" echo Driver / Pit House version:
>> "%list%" echo Firmware:
>> "%list%" echo Display refresh / connection:
>> "%list%" echo Continuous turning correct:
>> "%list%" echo Pedal / buttons correct:
>> "%list%" echo Pause / Alt-Tab / reconnect correct:
>> "%list%" echo Force feel:
>> "%list%" echo Stutters or sound gaps, and about when:
>> "%list%" echo.
>> "%list%" echo controllers.txt includes device serials and paths: review it before sharing publicly.
>> "%list%" echo inputs.qir includes the starting high scores and settings and the board's inputs, but no ROMs.
>> "%list%" echo Recording is timed separately in performance.txt. This is an actual hardware run,
>> "%list%" echo not an automated certification of wheel support or input-to-photon latency.
type "%list%"
echo.
echo Starting APB. Reports go to: %reports%
"work\APB.exe" --record "%reports%\inputs.qir" --profile "%reports%\performance.txt" --controller-report "%reports%\controllers.txt" > "%reports%\stdout.txt" 2> "%reports%\stderr.txt"
set code=%errorlevel%
start "" explorer "%reports%"
if not "%code%"=="0" (
    echo APB stopped with code %code%: see stderr.txt in %reports%.
    pause
)
exit /b %code%

:notbuilt
echo APB isn't built yet. Double-click "Play APB.exe" first: choose your ROM folder
echo and let it build the game, then run this again.
pause
exit /b 1

:failed
echo Can't make the folder "%reports%".
pause
exit /b 1
