# Changelog

## 0.4.0

- Model the System 2 interrupt mask, control-pin latches, and mirrored acknowledgement writes.
- Accept eligible T‑11 interrupts in native code, save the return frame, and report the vector and handler PC.
- Test mask and latch semantics, acknowledgement mirrors, IRQ priority, and stack validation.
- Advance the read-only rev 7 scan to the autoincrement operand at PC 0xF03C.

## 0.3.0

- Lower System 2 watchdog strobes from native byte and word clears and word moves.
- Count host-supplied board ticks, latch expiry, and stop native entry for a pending reset.
- Verify the watchdog mirrors and reset behavior with synthetic native code.
- Advance the read-only rev 7 scan to the interrupt-enable write at 0xF034.

## 0.2.0

- Load System 2 T‑11 ROM maps from split byte lanes and diagnose missing or overlapping regions.
- Compile direct jumps and immediate bank selections ahead of time, including switches that fetch an operand from the other ROM window.
- Lower absolute RAM and ROM word operands, plus byte clears in low RAM.
- Inspect the verified rev 7 reset path and report its first unsupported watchdog access at 0xF030.
- Test banked execution and map diagnostics with hand-authored fixtures.

## 0.1.1

- Configure Vulkan headers without the upstream C++ module target so GCC 14 can build with Unix Makefiles.

## 0.1.0

- Establish the 64-bit C++23 and Vulkan 1.3 project and its build matrix.
- Verify all 36 ROMs in Atari's arcade rev 7 set against published sizes and digests.
- Translate a bounded subset of T‑11 code ahead of time and test native execution using a synthetic program.
- Clear and read back an offscreen Vulkan image through the runtime loader.
