# Recording and replaying controls

Use the same build and rev 7 ROM set to reproduce a session:

```sh
apb /path/to/roms --record session.qir
apb /path/to/roms --replay session.qir
qilin_run /path/to/roms --frames 6000 --monkey 20261120 --record test.qir --frame-hashes recorded.hashes
qilin_run /path/to/roms --replay test.qir --frame-hashes replayed.hashes
```

Recording starts at power-on. The file contains the initial 512-byte EEPROM, initial cabinet controls and subsequent controls at frame boundaries: switches, coins/buttons, ADC pedals and wrapping LETA counters. It includes the originating build version. It contains no ROM data. The exclusive-create operation refuses an existing recording; choose a new filename.

Replay restores the recorded EEPROM in memory and takes cabinet inputs from the file. It does not load or save over your personal EEPROM/high scores. Live controls still operate the pause menu and window, but do not enter the emulated cabinet. Pause duration, display timing, driver settings, force feedback and raw USB events are not recorded. This reproduces board inputs, rather than the wheel driver's behaviour. Timing fixes in another build can change a replay; keep the version with the recording. The stored version is metadata, not an enforced compatibility check.

Recording and replay are mutually exclusive. Headless replay rejects script controls; use `--frames N` to replay a shorter prefix, otherwise it runs to the recorded duration. Both movie modes require the default 64-cycle scheduler quantum. Recording is bounded to 2,160,000 input states, approximately ten hours. A 6,000-frame seeded test compares every frame hash and the entire WAV while switching from synchronous audio recording to worker audio replay.

Each record has a CRC32 and frame ordering is checked. Completed corrupt records are rejected. If recording stops before its end marker or during a record, replay reports an interruption and uses only the complete, checksum-validated prefix. Records are flushed to the operating system each frame; this does not guarantee preservation through power loss.

## File format, version 1

All multi-byte values are little endian. CRC32 uses the reflected polynomial `0xEDB88320`, initially and finally inverted, over all preceding bytes of the header or individual record.

| Record | Fields before CRC32 |
| --- | --- |
| Header | 8-byte `APBMOV1\0`, 32-byte zero-padded build version, 512-byte EEPROM, 13-byte initial input state |
| Frame | Byte `F`, unsigned 64-bit frame number starting at 1, 13-byte input state |
| End | Byte `E`, unsigned 64-bit completed frame count |

An input state is IN0 (16 bits), IN1, DSW0, DSW1, four ADC bytes and four LETA bytes. A final input snapshot can be unused because stopping at a vblank does not execute the following frame. The end count therefore accepts the complete-state count or one less. Unknown records, reordered frames, trailing data after the end marker and excessive duration fail validation.
