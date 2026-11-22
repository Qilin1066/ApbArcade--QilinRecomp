# Naming

As in GBTGhoulies--QilinRecomp: **Qilin** is the author's name. It goes on the *tools*, the things that do the recompiling. It doesn't go on the game.

| Thing | Name | Why |
|---|---|---|
| The repo / project | ApbArcade--QilinRecomp | The game plus the toolchain that ports it (the owner's form for every project: `<Game>--QilinRecomp`) |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces of the recompiler and runtime | `qilin`, `qilin::t11`, `qilin::sys2`, `qilin::rom`, `qilin::gfx` | Project namespace; avoids collisions with MAME or system names |
| Build options, macros, CMake targets | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `apb` | Named after the game. Players launch APB, not a tool |
| The window's title, SDL's app name | APB | Same reason |
| The game's data folder | `<per-user data>/qilin/apb` | As the other recomps keep theirs (`qilin/<game>`): the EEPROM (high scores), the settings, controller reports. `Qilin/apb` in SDL's preferences folder up to v0.21.0; the first run moves it across |
| The settings file | `apb.ini`, in the data folder | After the game |
| The settings menu | F1 or Esc (controller: Back + Start); Controls, Display and Game pages; pauses the game; saves on close | The other recomps' menu (Dear ImGui), kept in step with them |
| Lifted functions | `fn_8288`, `fn_4A20_b12` | The CPU address, plus the bank for code in a window, so code in different banks never shares a name |

## Recovery after conflicting or unreadable saves

Migration keeps the current folder's files in use and leaves old files with
conflicting names in the old folder. They may contain different high scores or
settings; compare and copy them manually with APB closed if needed.

If both `eeprom.bin` and `eeprom.bin.bak` are unreadable or incomplete, APB runs
with the board's initial state and disables EEPROM writes for that session.
Keep copies before repairing them or moving them aside, then restart. A valid
backup still recovers automatically when only the primary is damaged.

Windows kit updates stage `APB.exe.new` and retain `APB.exe.previous` after a
successful replacement. The build stamp records kit contents, so an updated kit
with the same version is rebuilt automatically.
