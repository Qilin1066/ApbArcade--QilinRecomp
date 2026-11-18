# Changelog

## 0.1.1

- Configure Vulkan headers without the upstream C++ module target so GCC 14 can build with Unix Makefiles.

## 0.1.0

- Establish the 64-bit C++23 and Vulkan 1.3 project and its build matrix.
- Verify all 36 ROMs in Atari's arcade rev 7 set against published sizes and digests.
- Translate a bounded subset of T‑11 code ahead of time and test native execution using a synthetic program.
- Clear and read back an offscreen Vulkan image through the runtime loader.
