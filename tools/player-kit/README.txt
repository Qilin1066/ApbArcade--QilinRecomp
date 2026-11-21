APB QilinRecomp for Windows (64-bit)
==================================

APB (Atari Games, 1987), recompiled to run natively: the arcade board's two
processors' programs translated into C++ and compiled for your PC, checked
against MAME's processor cores on the shared board model, and against
standalone MAME 0.264: taking turns between the processors as MAME does, the
pictures and sound commands come out the same.

No game data is included. You need your own copy of the APB rev 7 ROM set
(MAME's "apb": 36 files).


Playing
-------
1. Unpack this folder anywhere (it needs about 1 GB once built).
2. Double-click "Play APB.bat".
   - The first time, it asks for the folder that holds your ROMs, then builds
     the game from them: a few minutes. Your ROMs never leave your PC.
   - After that, it starts the game straight away. APB.exe in this folder can
     be started directly too.

If Windows says "Windows protected your PC": More info, then Run anyway.
(The download isn't signed.) Or before unpacking, right-click the zip,
Properties, tick Unblock, OK.

Needs: Windows 10 or 11, 64-bit, and a graphics driver with Vulkan 1.3
(any recent NVIDIA, AMD or Intel driver).


Controls
--------
Keyboard: arrows steer and accelerate; Left Ctrl, Left Alt, Space are the
siren/start, fire and alternate siren; 5 inserts a coin. Gamepad: left stick
or d-pad steers, right trigger or d-pad up accelerates, A or Start
starts/sirens, B fires, X is alternate siren, Back (View) inserts a coin.

F1 or Esc (gamepad: Back + Start together): the settings menu: wheels,
pedals, buttons, the picture (presets, size, a CRT look, smoothed pixel art),
full screen, the volume. F3: frame times. F11 or Alt+Enter: full screen.

Settings and high scores are kept in %LOCALAPPDATA%\qilin\apb (the Game page
of the menu shows the folder). Versions up to 0.21.0 kept them in
%APPDATA%\Qilin\apb; the game moves them across the first time it runs.


What's in here
--------------
Play APB.bat        builds (once) and starts the game
kit\qilin.exe       the recompiler: turns the ROMs' programs into C++
kit\lib, include    the prebuilt rest of the game: the board, video, sound,
                    the window and controls (SDL3), the menu (Dear ImGui)
kit\toolchain       LLVM-MinGW 20261104, a C++ compiler for Windows (Clang)
kit\ninja.exe       Ninja, which drives the compiler
work\               made when building: the recompiled code (from your ROMs)
LICENSES\           the licences of everything included

Source, changes and issues: https://github.com/Qilin1066/ApbArcade--QilinRecomp


Checking a wheel and performance
-------------------------------
Double-click "Check controls and performance.bat". It builds the game if needed,
then starts a normal session with input recording, performance timings and an
automatic controller report. Follow the checklist printed in the window: use
one wheel setup, check pedals/buttons, full turns or steering endpoints,
pause/Alt-Tab/reconnection, then play for two minutes and quit normally.

A new folder under "Hardware checks" opens with performance.txt, controllers.txt,
inputs.qir, logs and checklist.txt. Fill in the observations and driver/firmware
versions. No files are uploaded automatically. The controller report includes
serials/device paths; the recording includes starting EEPROM (high scores and
game settings), but no ROMs. Physical wheel behaviour and display latency need
this actual-machine check; software tests alone cannot certify them.
