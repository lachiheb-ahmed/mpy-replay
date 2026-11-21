---
name: Hardware test report
about: Report the result of docs/BOARD_TEST.md on a real board (success or failure)
title: "Hardware report: <board>"
labels: hardware
---

**Board and chip:** (for example NUCLEO-F429ZI, STM32F429)
**MicroPython commit:** (`git -C micropython rev-parse HEAD`)
**mpy-replay version:** (`replay.about()` output)
**Host OS and toolchain:** (for example Ubuntu 24.04, arm-none-eabi-gcc 13.2)

**Result per step** (pass / fail / not run):

| Step | Result | Notes |
|---|---|---|
| Stock firmware builds and boots | | |
| Firmware with the module builds and flashes | | |
| `replay.about()` works, `replay.has_log()` is `None` on a fresh board | | |
| `main.py` records and crashes on purpose | | |
| After reset, `boot.py` saves `crash0.mrl` | | |
| PC replay prints the same `acc` and `crash reproduced` | | |
| `main_full.py` (extended test) | | |
| Negative tests (hash, edited program, corrupted log) | | |

**Output that matters** (paste, without secrets): the `RESULT` line from the board, the `RESULT` line from the PC replay, `replay.has_log()` after the crash.

**Anything that went wrong:** (error messages, what you tried)

Please do not attach `.mrl` logs from real devices to a public issue if they may contain sensitive data.
