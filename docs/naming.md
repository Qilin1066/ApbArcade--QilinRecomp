# Naming

As in GBTGhoulies-Qilincomp: **Qilin** is the author's name. It goes on the *tools*, the things that do the recompiling. It doesn't go on the game.

| Thing | Name | Why |
|---|---|---|
| The repo / project | ApbArcade-QilinComp | The game plus the toolchain that ports it |
| The recompiler CLI | `qilin` | The author's tool: short, unique, easy to type |
| C++ namespaces of the recompiler and runtime | `qilin`, `qilin::t11`, `qilin::sys2`, `qilin::rom`, `qilin::gfx` | Project namespace; avoids collisions with MAME or system names |
| Build options, macros, CMake targets | `QILIN_*`, `qilin_*` | Same prefix as the namespace, so it's obvious what owns them |
| The game executable players run | `apb` | Named after the game. Players launch APB, not a tool |
| Settings file, save folder (EEPROM), window title | after the game | Same reason |
| Lifted functions | `fn_<physical address>` | Named by their offset in MAME's `maincpu` region (5 hex digits), so code in different banks never shares a name |
