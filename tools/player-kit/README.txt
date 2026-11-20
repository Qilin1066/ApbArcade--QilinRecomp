APB QilinRecomp for Windows (64-bit)
==================================

APB (Atari Games, 1987), recompiled to run natively: the arcade board's two
processors' programs translated into C++ and compiled for your PC, played
exactly as on MAME's own processor cores, frame for frame.

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
cabinet's buttons 1-3; 5 inserts a coin. Gamepad: left stick steers, right
trigger accelerates, A/B/X are the buttons, View inserts a coin.

F1 or Esc (gamepad: Menu): the settings menu: wheels, pedals, buttons, the
picture (presets, size, a CRT look, smoothed pixel art), full screen.
Alt+Enter or F11: full screen.

Settings and high scores are kept in %APPDATA%\Qilin\apb.


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

Source, changes and issues: https://github.com/Qilin1066/ApbArcade-QilinRecomp
