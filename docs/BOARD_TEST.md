# Testing mpy-replay on an STM32 board

Goal: record a run on the board, let it crash, rescue the log after a reset, and
reproduce the crash on a PC. Adapter status: it **builds and links** for NUCLEO_F429ZI, PYBV11 (F405) and
NUCLEO_F767ZI with MicroPython master (about 4.8 KB flash, 1 KB RAM), but it has **not been run on
hardware yet**. Your run is the first one: please report what you see.

## 0. Requirements

- A board with an **STM32F4 or F7** that has backup SRAM (BKPSRAM, 4 KB). Check the
  datasheet of your chip (examples: PYBv1.1, NUCLEO-F429ZI, NUCLEO-F446RE, NUCLEO-F767ZI).
- `arm-none-eabi-gcc`, `make`, `git`, `python3`, and `mpremote` (`pip install mpremote`).
- A programmer path: DFU, ST-Link or OpenOCD (whatever your board uses).
- Optional: a coin cell on VBAT, only needed to keep the log across a *power loss*.
  A normal reset keeps backup SRAM without it.

## 1. Baseline: build and flash stock MicroPython

```bash
git clone https://github.com/micropython/micropython && cd micropython
git submodule update --init lib/stm32lib lib/micropython-lib
make -C mpy-cross
make -C ports/stm32 BOARD=<YOUR_BOARD> submodules
make -C ports/stm32 BOARD=<YOUR_BOARD> -j8
make -C ports/stm32 BOARD=<YOUR_BOARD> deploy        # DFU. ST-Link: deploy-stlink, OpenOCD: deploy-openocd
```

Confirm the REPL works before touching anything else. Keep this firmware as a fallback.

## 2. Patch the clock hooks

```bash
python3 /path/to/mpy-replay/scripts/apply_time_hook.py .     # revert with --revert
```

## 3. Build with mpy-replay

```bash
export HASH=$(git rev-parse HEAD)        # keep it: the PC replay build must use the same value
make -C ports/stm32 BOARD=<YOUR_BOARD> -j8 MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=/path/to/mpy-replay MPY_REPLAY_PORT=stm32 \
     MPY_REPLAY_FW_HASH=$HASH
make -C ports/stm32 BOARD=<YOUR_BOARD> deploy MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=/path/to/mpy-replay MPY_REPLAY_PORT=stm32 MPY_REPLAY_FW_HASH=$HASH
```

Check in the REPL:

```python
>>> import replay
>>> replay.about()
'mpy-replay 0.1.0 - Copyright (c) 2026 Ahmed (lachiheb-ahmed) - MIT - ...'
>>> replay.has_log()        # None on a fresh board (backup SRAM holds no valid log)
```

## 4. Copy the test files to the board

```bash
mpremote cp usermod/replay_io.py :replay_io.py
mpremote cp examples/board/boot.py :boot.py
mpremote cp examples/board/main.py :main.py     # edit the ADC pin first
```

## 5. Record and crash

Reset the board (or `mpremote reset`). `main.py` records 300 ADC samples, prints
`RESULT n=300 acc=...` and raises `ValueError('boom')`. Write down the `acc` value.
The log is about 2.5 KB, which fits the 4 KB backup SRAM.

## 6. Rescue the log

Press the **reset button** (not a power cycle unless VBAT is powered). `boot.py`
finds the previous log with `replay.has_log()` and saves it as `/flash/crash0.mrl`.
Immediately copy it, because the next run of `main.py` starts a new recording:

```bash
mpremote fs cp :crash0.mrl .
```

You can also check it from the REPL right after a crash and a reset:
`replay.has_log()` returns `(bytes, events, last_lclk, 'ValueError(...')`.

## 7. Replay on the PC

Build the unix replay binary once with the **same hash** (see README, section 5):
`make -C ports/unix VARIANT=standard USER_C_MODULES=/path/to/mpy-replay MPY_REPLAY_PORT=unix MPY_REPLAY_MODE=1 MPY_REPLAY_FW_HASH=$HASH BUILD=build-replay`. Then:

```bash
export MICROPYPATH=/path/to/mpy-replay/examples/pc:/path/to/mpy-replay/usermod
MRL_REPLAY_LOG=crash0.mrl /path/to/micropython/ports/unix/build-replay/micropython main.py
```

`examples/pc/machine.py` only provides a constructor for `machine.ADC`; the replay
never reads hardware. Expected output:

```
RESULT n=300 acc=<same value as on the board>
replay: crash reproduced: ValueError('boom',)
```

Inspect the log: `tools/mrl_dump crash0.mrl`.

## 8. Pass criteria

| Check | Expected |
|---|---|
| `replay.about()` on the board | prints the copyright string |
| `has_log()` after reset | `(~2500, 304, ..., "ValueError('boom',)")` |
| PC replay | same `acc` as the board, "crash reproduced" |
| Replay build with a different `MPY_REPLAY_FW_HASH` | refuses with `firmware hash mismatch` (override: `MRL_IGNORE_FW_HASH=1`) |
| Change one input byte in the `.mrl` (hex editor) and replay | the CRC check ends the log early and the replay reports a divergence or end of log |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `replay_port_stm32.c: ... F4/F7 only` | Your MCU family is not covered. Port the clock and power enable sequence for BKPSRAM. |
| `STM32_HAL_H` or `BKPSRAM_BASE` undeclared | Different MicroPython version or family. Include the HAL header of your family. |
| Undefined `_heap_start` / `_heap_end` | Your linker script uses other names; return a constant or the GC heap size in `replay_port_heap_size()`. It is informational only. |
| `MICROPY_VM_HOOK_LOOP` redefined | The board config already defines it. Chain both calls in one macro. |
| `RuntimeError: replay: log storage full` at `stop()` | More than 4 KB of events. Record fewer or smaller events. The log up to that point is valid. |
| `RuntimeError: replay diverged (reason 1/2...)` | The PC run asked for a different input or at a different clock. Check that `main.py` is identical, that code before `replay.start()` has no recorded inputs, and that nothing branches on `replay.mode()`. |
| `firmware hash mismatch` on the PC | The replay build was made from a different `MPY_REPLAY_FW_HASH` than the board firmware. |
| `has_log()` returns `None` | The board was power-cycled without VBAT, or `replay.start()` ran before `boot.py` read the log. |
