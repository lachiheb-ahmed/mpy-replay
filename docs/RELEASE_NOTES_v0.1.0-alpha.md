# mpy-replay v0.1.0-alpha

First public release. Deterministic record and replay for MicroPython: record the inputs of a running program, then re-run the same execution on a PC.

## What works (validated)

- Portable C core: logical clock, log format with CRC, recorder, reader, replay engine. Unit tests under AddressSanitizer and UBSan; the log parser was fuzzed with 300 000 corrupted logs without a crash.
- MicroPython integration (`replay` module and `replay_io` proxies) with end-to-end tests on the unix port: CPU-bound program, I/O-bound program, simulated hardware interrupts, Ctrl+C at a random moment, including inside a recorded read. Replays reproduced the same final state and logical clock.
- Firmware build for STM32 (NUCLEO-F429ZI, PYBv1.1, NUCLEO-F767ZI): compiles and links, about 4.8 KB flash and 1 KB RAM.
- Safety checks: firmware hash, `mark()` verification, unconsumed events, request sizes of UART/I2C/SPI reads, hardware outputs skipped during replay.

## Known limits (please read)

- **Never run on real hardware.** The STM32 backup-SRAM adapter is untested. [BOARD_TEST.md](BOARD_TEST.md) is the procedure; reports are welcome.
- **The log starts at `replay.start()` and stops when full.** On STM32 backup SRAM (4 KB) that is a few hundred events. A crash after that point is not captured. A circular mode is planned but it will not be able to replay a run, only to show the last events.
- **Only part of the inputs are recorded:** clock, ADC `read_u16`, Pin value, UART/I2C/SPI reads, `urandom`, seeding of `random`. Not yet: `UART.any/readline/readinto`, sockets, `time.time`, RTC. `asyncio` is untested.
- **Recording is not free:** the hook costs roughly 1% to 20% on the unix port (not measured on hardware), and the scheduler is locked while a recorded read runs.
- **The replay host must match the board's numbers** (single-precision floats on STM32).
- Tested against one MicroPython commit (`9e10c3e`).
- The log format and the proxy payloads may change before v1.0.

## Try it

Unix port: README, section 5. Board: `docs/BOARD_TEST.md`. Questions and answers: `docs/FAQ.md`. Measurements: `docs/BENCHMARK.md`.

## Feedback wanted

Real-board results, inputs you need that are not covered, and cases where a replay diverges. Please do not attach `.mrl` logs from real devices to public issues.
