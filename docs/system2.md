# Atari System 2: the board as the T-11 sees it

Everything here is taken from MAME (`src/mame/atari/atarisy2.cpp`, `atarisy2_v.cpp`, `slapstic.cpp`, `src/devices/cpu/t11/`), which is this project's reference. MAME writes the map in octal; addresses here are hex.

## Main CPU (T-11, 10 MHz)
MAME's `T11(config, m_maincpu, MASTER_CLOCK/2)` with initial mode `0x36FF`: the reset PC is `0x8000`, SP `0x00FE`, PSW `0340`. Word accesses ignore address bit 0 (MAME notes that this "fixes APB").

| Address | Read | Write |
|---|---|---|
| `0000-0FFF` | RAM | RAM |
| `1000-11FF`, mirrored `1200-13FF` | palette RAM | palette RAM (256 words) |
| `1400-147F` | ADC data (even byte) | ROM bank select: `1400` window 0, `1402` window 1 |
| `1480-14FF` | — | ADC channel select and start (low byte) |
| `1580-159F` | — | acknowledge CP0 (sound command read) |
| `15A0-15BF` | — | sound reset (bit 0 holds the 6502 in reset; clears the YM2151 reset, coin counters and LEDs) |
| `15C0-15DF` | — | acknowledge CP2 (32V scanline) |
| `15E0-15FF` | — | acknowledge CP3 (VBLANK) |
| `1600-167F` | — | interrupt enable (applied at the end of MAME's timeslice) |
| `1680-16FF` | — | sound command latch (asserts the 6502's NMI) |
| `1700-177F` | — | X scroll |
| `1780-17FF` | — | Y scroll |
| `1800-1BFF` | IN0 | watchdog (even byte) |
| `1C00-1FFF` | sound reply latch, `0xFF00` ORed in; acknowledges CP1 | — |
| `2000-3FFF` | video RAM view selected by the Slapstic | same |
| `4000-5FFF` | ROM window 0 (bank 0-63) | — |
| `6000-7FFF` | ROM window 1 (bank 0-63) | — |
| `8000-FFFF` | fixed ROM | — |

Unmapped reads return `0xFFFF` (`unmap_value_high`).

**ROM banks.** Bank *n* of either window is `maincpu` region offset `0x10000 + 0x2000 n`. The selector is bits 10-15 of the word written, XORed with 3, then bit-swapped (`bitswap<6>(5,4,1,0,3,2)`). Banks 32-47 (`0x50000-0x6FFFF`) have no ROM on APB; MAME zero-fills them.

**Watchdog.** `set_time(MASTER_CLOCK/2/16/16/16/256)`: 1,048,576 main-CPU clocks (104.8576 ms). A write to the even byte of any word in `1800-1BFF` restarts it.

**Interrupts.** System 2 drives the T-11's CP0-CP3 lines directly (no vectored interrupts). Each device event clocks its enable bit into a latch: CP0 sound command read by the 6502 (enable bit 0), CP1 sound reply written (bit 1), CP2 every 64 scanlines (bit 2), CP3 VBLANK (bit 3). The T-11 takes the highest code above its PSW priority through MAME's fixed vector table (CP code 1 → `070`, …, 15 → `0140`), reading the new PC and PSW before pushing PSW and PC, for 114 cycles. MAME checks interrupts at the start of each timeslice and after instructions that change the PSW.

**Video RAM and the Slapstic.** The Slapstic 137412-110 watches every bus cycle and selects the view at `2000-3FFF`: view 0 alphanumerics (`2000-37FF`) and motion objects (`3800-3FFF`), view 2 playfield top, view 3 playfield bottom (the reset view); view 1 maps nothing. Its decode range is `8000-81FF`, but the alternate sequence starts on any address whose low byte is `0x5A`. `recomp/tests` checks the model against a transcription of MAME's state machine on random bus traffic.

**Palette.** Each word is `RRRRGGGGBBBBIIII`; MAME's intensity DAC table and colour table are reproduced exactly (checked on all 65,536 values).

## Video timing
`screen.set_raw(VIDEO_CLOCK/2, 640, 0, 512, 416, 0, 384)`: a 16 MHz pixel clock, 640 × 416 total, 512 × 384 visible, 60.096 Hz. The cabinet is ROT270.

## Sound CPU (6502, 1.79 MHz)
RAM `0000-0FFF`, EEPROM `1000-11FF`, POKEYs at `1800` and `1830`, YM2151 at `1850`, the main-to-sound latch at `1860` (read clocks CP0 on the main CPU), TMS5220 at `1870`, the sound-to-main latch at `1874` (write clocks CP1), ROM `4000-FFFF`. IRQ every `MASTER_CLOCK/2/16/16/16/10` (244 Hz). Recompilation of the 6502 program follows the T-11's.
