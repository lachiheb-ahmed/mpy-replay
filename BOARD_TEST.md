# Testing mpy-replay on an STM32 board, step by step

This guide takes you from nothing to a verified result: build the firmware, flash it, record a run on the board, rescue the log after a reset, and replay the same run on your PC.

**Honest status.** The firmware has been built and linked for NUCLEO-F429ZI, PYBv1.1 (F405) and NUCLEO-F767ZI, and everything except the board part has been validated on the MicroPython unix port. It has **not been run on real hardware yet**. If you run this guide you are the first, and your report (pass or fail) is valuable: see section 12.

**Time:** about 1 to 2 hours the first time, mostly toolchain and build time.

## What you will do

```
build stock firmware -> flash -> check REPL          (section 3)
patch + build with mpy-replay -> flash -> check      (sections 4 and 5)
copy test files -> board records a run and crashes   (section 6)
press reset -> boot.py saves the log -> copy to PC   (section 7)
build the PC replay binary -> replay -> compare      (section 8)
extended test and negative tests                     (sections 9 and 10)
```

## 1. What you need

**Hardware**
- A board with an **STM32F4 or F7** that has backup SRAM (BKPSRAM, 4 KB). The adapter stops the build with an `#error` on other families.
- A USB cable (and the programmer interface your board uses: DFU, ST-Link, or OpenOCD).
- Optional: a coin cell on VBAT, only needed to keep the log across a *power loss*. A plain reset keeps backup SRAM without it, and a plain reset is what this guide tests.

**Boards the build has been verified on (build only)**

| Board | Chip | Analog pin for `ADC_PIN` | Usual way to flash |
|---|---|---|---|
| NUCLEO_F429ZI | STM32F429 | `"A0"` | `deploy-stlink`, or drag `firmware.bin` onto the NUCLEO drive |
| NUCLEO_F767ZI | STM32F767 | `"A0"` | same |
| PYBV11 | STM32F405 | `"X1"` | `deploy` (DFU) |

For another board, pick any analog-capable pin from its pinout and set `ADC_PIN` in the example files.

**Software (Linux, Debian/Ubuntu)**

```bash
sudo apt install build-essential git python3 python3-pip gcc-arm-none-eabi \
                 dfu-util stlink-tools openocd pkg-config libffi-dev
pip install mpremote
```

macOS: `brew install arm-none-eabi-gcc dfu-util stlink openocd` and `pip install mpremote`. Windows: use WSL2 for building; for NUCLEO boards the simplest flashing path is to copy `firmware.bin` to the board's USB drive from Windows. On Linux you may need to add yourself to the `dialout` group to open the serial port (`sudo usermod -aG dialout $USER`, then log out and in).

## 2. Get the sources

```bash
mkdir ~/replay-test && cd ~/replay-test
git clone https://github.com/lachiheb-ahmed/mpy-replay
git clone https://github.com/micropython/micropython
cd micropython
git checkout 9e10c3e                       # the commit this was tested against
make -C mpy-cross -j4
```

Set variables that you will reuse (keep this terminal open, or export them again later):

```bash
export BOARD=NUCLEO_F429ZI                 # your board name from ports/stm32/boards/
export MPR=$HOME/replay-test/mpy-replay
export HASH=$(git rev-parse HEAD)          # identifies this source revision; the PC build must use the same
echo $HASH                                 # 40 hex characters
```

## 3. Baseline: stock MicroPython first

This proves that your toolchain and board work before anything of ours is involved.

```bash
make -C ports/stm32 BOARD=$BOARD submodules
make -C ports/stm32 BOARD=$BOARD -j4 MICROPY_USE_COMPILER_PLUGIN=none
```

The build ends with a size line and creates `ports/stm32/build-$BOARD/firmware.bin` (and `firmware.dfu`). Note the `text` size; you will compare it later.

Flash it (use the one that matches your board):

```bash
make -C ports/stm32 BOARD=$BOARD deploy MICROPY_USE_COMPILER_PLUGIN=none           # DFU: hold BOOT0 and reset first (PYBv1.1: DFU jumper)
make -C ports/stm32 BOARD=$BOARD deploy-stlink MICROPY_USE_COMPILER_PLUGIN=none    # ST-Link
make -C ports/stm32 BOARD=$BOARD deploy-openocd MICROPY_USE_COMPILER_PLUGIN=none   # OpenOCD
```

Check the REPL:

```bash
mpremote connect list                       # shows the serial port, for example /dev/ttyACM0
mpremote repl                               # Ctrl+] exits
>>> import sys; print(sys.implementation)
```

Expected: a tuple that starts with `(name='micropython', ...)`. If this does not work, stop and fix it first (see Troubleshooting).

## 4. Build the firmware with mpy-replay

```bash
python3 $MPR/scripts/apply_time_hook.py .        # routes time.ticks_* through the recorder (revert later with --revert)
make -C ports/stm32 BOARD=$BOARD -j4 MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=$MPR MPY_REPLAY_PORT=stm32 MPY_REPLAY_FW_HASH=$HASH
```

Expected: no `error` or `warning` lines, and a size line whose `text` is about **4.8 KB larger** than the baseline. If you see `replay_port_stm32.c: ... F4/F7 only`, your chip family is not supported yet.

## 5. Flash it and check the module

Flash exactly as in section 3, adding the same variables to the deploy command:

```bash
make -C ports/stm32 BOARD=$BOARD deploy-stlink MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=$MPR MPY_REPLAY_PORT=stm32 MPY_REPLAY_FW_HASH=$HASH       # or deploy / deploy-openocd
```

Then in the REPL:

```python
>>> import replay
>>> replay.about()
'mpy-replay 0.1.0 - Copyright (c) 2026 Ahmed (lachiheb-ahmed) - MIT - https://github.com/lachiheb-ahmed'
>>> replay.mode()          # 0 = OFF
0
>>> replay.has_log()       # nothing valid in backup SRAM yet
```

`replay.has_log()` prints nothing (it returns `None`) on a fresh board. **If `import replay` fails, the module is not in this firmware**: check that you passed `USER_C_MODULES` in the *deploy* command as well, and that the build in section 4 completed.

**Optional now: measure the hook cost on your board.** Run `mpremote run $MPR/bench/bench_mcu.py` on the stock firmware (section 3) and again on this one (recording is not started, so this measures the idle hook), and note both lines.

## 6. Test 1: record a run on the board

Edit `ADC_PIN` in `$MPR/examples/board/main.py` if your board is not a NUCLEO, then copy the files to the board:

```bash
mpremote cp $MPR/usermod/replay_io.py :replay_io.py
mpremote cp $MPR/examples/board/boot.py :boot.py
mpremote cp $MPR/examples/board/main.py :main.py
mpremote reset
mpremote repl
```

What `main.py` does: it reads the ADC 300 times (one read every 10 ms), logs every read with a logical clock, writes 3 checkpoints, prints a `RESULT` line and raises `ValueError('boom')` on purpose.

Expected on the console after the reset (the numbers will differ):

```
RESULT n=300 acc=7112604
Traceback (most recent call last):
  ...
ValueError: boom
```

**Write down the `acc` value.** Now look at the log that is in backup SRAM:

```python
>>> import replay
>>> replay.has_log()
(2518, 304, 1500, "ValueError('boom',)")
```

The tuple is `(bytes, events, last clock, crash text)`. Expect **304 events** (300 ADC reads, 3 checkpoints, 1 crash) and about 2.5 KB. The clock value will differ.

## 7. Rescue the log after a reset

Press the board's **reset button** (a plain reset, not a power cycle unless VBAT is powered). `boot.py` runs before `main.py`, finds the log of the previous run and saves it. Watch the console:

```
previous log: (2518, 304, 1500, "ValueError('boom',)")
saved /flash/crash0.mrl
```

`main.py` then runs again and records a new log (it crashes again, which is intended). Copy the saved file to your PC:

```bash
mpremote fs ls                              # crash0.mrl is listed
mpremote fs cp :crash0.mrl .
```

Later resets create `crash1.mrl`, `crash2.mrl`... (up to 10). To start clean, delete them with `mpremote fs rm :crash0.mrl`.

If `has_log()` printed nothing after the reset, see Troubleshooting.

## 8. Replay on the PC

Build the unix replay binary **with the same hash** and with the board's float precision (STM32 uses single-precision floats, the unix port defaults to double):

```bash
cd ~/replay-test/micropython
python3 $MPR/scripts/apply_time_hook.py .        # already applied if you did section 4 in this tree
make -C ports/unix submodules
make -C ports/unix -j4 VARIANT=standard MICROPY_PY_SSL=0 MICROPY_PY_BTREE=0 MICROPY_PY_FFI=0 \
     USER_C_MODULES=$MPR MPY_REPLAY_PORT=unix MPY_REPLAY_MODE=1 MPY_REPLAY_FW_HASH=$HASH \
     CFLAGS_EXTRA='-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT' BUILD=build-replay
```

Prepare a work folder with the **same `main.py` that ran on the board**, the proxies and the PC stand-in for `machine`:

```bash
mkdir -p ~/replay-test/run && cd ~/replay-test/run
cp $MPR/usermod/replay_io.py $MPR/examples/pc/machine.py .
cp $MPR/examples/board/main.py .
cp ../crash0.mrl .                          # the file you copied from the board
export MICROPYPATH=.
MRL_REPLAY_LOG=crash0.mrl ~/replay-test/micropython/ports/unix/build-replay/micropython main.py
```

**Expected: the same result as the board.**

```
RESULT n=300 acc=7112604            <- must equal the acc you wrote down
replay: crash reproduced: ValueError('boom',)
Traceback (most recent call last):
  ...
ValueError: boom
```

The ADC is never read during the replay: the values come from the log. Inspect the log if you want:

```bash
make -C $MPR tools/mrl_dump && $MPR/tools/mrl_dump crash0.mrl | head -20
```

## 9. Test 2: the extended test

`main_full.py` records more kinds of inputs: ADC, the clock (`time.ticks_ms`), recorded random numbers, 4 marks and 4 checkpoints, then crashes.

```bash
mpremote cp $MPR/examples/board/main_full.py :main.py     # replaces main.py on the board; edit ADC_PIN first if needed
mpremote reset ; mpremote repl                            # note the RESULT line, then press reset once
mpremote fs ls                                            # find the newest crashN.mrl
mpremote fs cp :crash1.mrl .                              # the name depends on how many logs exist
```

Expected log (`replay.has_log()` or `mrl_dump -q`): **210 events**: 100 clock reads, 100 ADC reads, 1 random seed, 4 checkpoints, 4 marks, 1 crash, about 2.3 KB. Replay on the PC with `main_full.py` (copy it as `main.py` into the run folder) and check that `RESULT n=100 acc=...` matches the board.

## 10. Negative tests (prove that the checks work)

Each of these must **fail loudly**. A replay that "succeeds" in any of them is a bug; please report it.

| Test | How | Expected |
|---|---|---|
| Wrong source revision | Build the replay binary with a different `MPY_REPLAY_FW_HASH` (change one digit) and replay | `RuntimeError: replay: firmware hash mismatch` (override: `MRL_IGNORE_FW_HASH=1`) |
| Edited program | In the PC copy of `main.py` change `n % 100` to `n % 50` and replay | `RuntimeError: replay diverged ...` |
| Different mark | In `main_full.py` change `"n=%d"` to `"m=%d"` and replay | `RuntimeError: replay diverged ...` |
| Corrupted log | Change one byte in the middle of the `.mrl` with a hex editor and replay | The log ends at the damage: `replay: end of log` or a divergence |
| Early end | Make `main.py` stop after 100 iterations on the PC | `replay: N recorded events were not consumed` |

## 11. Clean up and restore

```bash
python3 $MPR/scripts/apply_time_hook.py ~/replay-test/micropython --revert
```

To go back to stock MicroPython, rebuild and flash without the `USER_C_MODULES` options, as in section 3.

## 12. Report your result

Open an issue with the template **Hardware test report** (`.github/ISSUE_TEMPLATE/hardware_report.md`), pass or fail. Include: board, MicroPython commit, the board's `RESULT` line, the PC replay's `RESULT` line, `replay.has_log()` after the crash, and the output of `bench_mcu.py` on both firmwares if you ran it. Please do not attach `.mrl` logs from real devices to a public issue.

## Pass criteria

| Check | Expected |
|---|---|
| Stock firmware boots | REPL answers |
| Firmware with the module builds | no errors or warnings, about +4.8 KB |
| `replay.about()` | prints the copyright string |
| `replay.has_log()` on a fresh board | `None` |
| After `main.py` | `RESULT` line, `ValueError: boom`, `has_log()` shows 304 events |
| After reset | `boot.py` prints `saved /flash/crash0.mrl` |
| PC replay | same `acc` as the board and `crash reproduced` |
| `main_full.py` | 210 events and a matching `acc` |
| Negative tests | all five fail loudly |

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `mpremote connect list` shows nothing | Cable carries power only, wrong USB port (NUCLEO: use the ST-Link USB port), or missing permission (`dialout` group on Linux). |
| DFU deploy says "No DFU capable USB device" | The board is not in DFU mode: hold BOOT0 while pressing reset (PYBv1.1: DFU jumper), then retry. |
| Build stops at `ffi.h: No such file` / `qstr.i.last Error 1` (PC build) | Add `MICROPY_PY_FFI=0` (already in the commands above) or install `libffi-dev pkg-config`. |
| `replay_port_stm32.c: ... F4/F7 only` | Your chip family is not supported by the adapter yet. |
| `STM32_HAL_H` or `BKPSRAM_BASE` undeclared | A board or family without backup SRAM; the adapter needs one. |
| `MICROPY_VM_HOOK_LOOP` redefined | The board configuration already defines it; combine both calls in one macro. |
| `import replay` fails on the board | The module is not in the flashed firmware: pass `USER_C_MODULES` etc. to the *deploy* command too. |
| `has_log()` prints nothing after reset | `boot.py` or `replay_io.py` not copied, the board was power-cycled without VBAT, or `main.py` crashed before `replay.start()`. Run `import replay; replay.has_log()` immediately after the crash, before any reset, to check. |
| `replay.start()` raises `storage error` | The backup regulator did not become ready. Build with `-DMRL_STM32_REQUIRE_BKUP_REG=0` (add it to `CFLAGS_EXTRA`) to accept reset-only retention, and report it. |
| `firmware hash mismatch` on the PC | The replay binary was built with a different `MPY_REPLAY_FW_HASH` than the board firmware. |
| `replay diverged` on the PC with an unmodified program | A real finding. Check that the PC `main.py` is byte-identical to the board's, that float precision matches (section 8), and report it with the details. |
| `RuntimeError: replay: end of log` | The log ended before the program did: the log was truncated (4 KB full) or the program differs. |
| `acc` differs between board and PC | Do not trust the replay; report it. First check the float flag and the hash. |
