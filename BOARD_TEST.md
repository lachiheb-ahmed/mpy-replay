# Testing mpy-replay on an STM32 board, step by step

This guide takes you from a stock MicroPython firmware to a full cycle:

1. build a firmware that contains the new `replay` module,
2. record a run on the board and let it crash,
3. rescue the log after a reset,
4. build the **unix** replay binary and reproduce the crash on your PC.

> **Status (v0.1.0-alpha).** The STM32 adapter **builds and links** for NUCLEO_F429ZI, PYBV11 (F405) and
> NUCLEO_F767ZI (about 4.8 KB flash, 1 KB RAM) but it has **never run on real hardware**. Record and replay
> are validated on the unix port only. Your run is the first one: please report exactly what you see (section 11).

---

## 0. The idea in one picture

```
 BOARD (STM32)                                        PC (unix port)
 ---------------------------------------------        ------------------------------------
 main.py  --> replay_io.ADC.read_u16()                 same main.py
               |                                        |
               v                                        v
        replay._io (C)  --records-->  backup SRAM       replay._io (C) --reads--> crash0.mrl
               |        (4 KB, survives reset)          (never touches hardware)
   crash: ValueError('boom')                           replay: same values, same clock
               |                                        -> "crash reproduced"
   reset --> boot.py: replay.has_log()/read()
               \--> /flash/crash0.mrl --mpremote--> PC
```

`Execution = f(firmware, Python sources, input log)`. The board stores only the **inputs** (ADC values, clock
reads, ...) and the **logical clock** (a counter of VM branches) at which each input was delivered. The PC
re-runs the same program and feeds it the logged inputs at the same logical clock.

Two things make this work and you will set both up below:

| Piece | What it does | Where |
|---|---|---|
| `replay` C module | recorder, replay engine, VM hook (`MICROPY_VM_HOOK_LOOP`) | `usermod/modreplay.c`, `replay/core/` |
| Clock patch | routes `time.ticks_ms/us/cpu` through the recorder | `scripts/apply_time_hook.py` (edits `extmod/modtime.c`) |
| STM32 adapter | stores the log in backup SRAM (BKPSRAM) | `replay/ports/stm32/replay_port_stm32.c` |
| `replay_io.py` | Python proxies (ADC, Pin, UART, I2C, SPI, random) | `usermod/replay_io.py` (copied to the board) |

---

## 1. Requirements

- A board with an **STM32F4 or F7** that has backup SRAM (BKPSRAM, 4 KB). Examples: PYBv1.1,
  NUCLEO-F429ZI, NUCLEO-F446RE, NUCLEO-F767ZI. Check the datasheet of your chip.
- On the PC: `arm-none-eabi-gcc`, `make`, `git`, `gcc`, `python3`, and `mpremote` (`pip install mpremote`).
- A way to flash: DFU, ST-Link or OpenOCD (whatever your board uses).
- Optional: a coin cell on VBAT. It is only needed to keep the log across a **power loss**.
  A normal reset keeps backup SRAM without it, and this test only uses a reset.

Set up a few shell variables once (use the same terminal for the whole guide):

```bash
export REPLAY=/path/to/mpy-replay          # the unzipped project
export MPY=/path/to/micropython            # the MicroPython clone (section 3)
export BOARD=NUCLEO_F429ZI                 # your board name under ports/stm32/boards/
```

---

## 2. Step 0: check the project on your PC (2 minutes, no board)

This proves the C core and the log tool work on your machine before you touch the firmware.

```bash
cd $REPLAY
make test              # core unit tests under AddressSanitizer + UBSan
make tools/mrl_dump    # builds the log inspection tool
```

Expected: the last line of `make test` is `all core tests passed`.

---

## 3. Step 1: build and flash stock MicroPython (baseline)

```bash
git clone https://github.com/micropython/micropython $MPY && cd $MPY
git submodule update --init lib/stm32lib lib/micropython-lib
make -C mpy-cross
make -C ports/stm32 BOARD=$BOARD submodules
make -C ports/stm32 BOARD=$BOARD -j8
make -C ports/stm32 BOARD=$BOARD deploy          # DFU.  ST-Link: deploy-stlink   OpenOCD: deploy-openocd
```

Open the REPL (`mpremote` or any serial terminal) and confirm `print("hello")` works.
**Keep this firmware working as a fallback.** If something fails later you know the board and cable are fine.

> The project was tested against MicroPython commit `9e10c3e`. If your `master` has moved and the build breaks,
> check out that commit: `git checkout 9e10c3e` (then repeat the submodule and `mpy-cross` steps).

---

## 4. Step 2: patch the clock hooks

`time.ticks_ms()` is an input that changes on every run, so it must go through the recorder. The script edits
`extmod/modtime.c` so that `ticks_ms/us/cpu` call `MPY_REPLAY_TICKS_*` (these macros are provided by the module).

```bash
cd $MPY
python3 $REPLAY/scripts/apply_time_hook.py .
```

Expected: `patched ./extmod/modtime.c`. Check the change:

```bash
git diff extmod/modtime.c      # three MP_OBJ_NEW_SMALL_INT(...) calls now wrapped in MPY_REPLAY_TICKS_*
```

- Running it twice prints `nothing to do (already applied)`: it is safe.
- Undo at any time with `python3 $REPLAY/scripts/apply_time_hook.py . --revert`.

---

## 5. Step 3: build the firmware with the new `replay` module

Remember the **firmware hash**. The recording build and the PC replay build must use the same value, otherwise the
replay refuses the log (`firmware hash mismatch`). It must be exactly 40 hexadecimal characters.

```bash
cd $MPY
export HASH=$(git rev-parse HEAD)
echo $HASH | tee ~/fw_hash.txt        # keep it somewhere: you need it again in section 9
```

Build:

```bash
make -C ports/stm32 BOARD=$BOARD -j8 MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=$REPLAY MPY_REPLAY_PORT=stm32 \
     MPY_REPLAY_FW_HASH=$HASH
```

What these options do:

| Option | Meaning |
|---|---|
| `USER_C_MODULES=$REPLAY` | MicroPython finds `usermod/micropython.mk` and compiles the `replay` module into the firmware |
| `MPY_REPLAY_PORT=stm32` | selects `replay/ports/stm32/replay_port_stm32.c` (backup SRAM storage) |
| `MPY_REPLAY_FW_HASH=$HASH` | written into every log header, checked at replay time |
| `MICROPY_USE_COMPILER_PLUGIN=none` | avoids a compiler-plugin requirement on some toolchains |

What `usermod/micropython.mk` does behind the scenes (you do not need to edit anything):

- adds `modreplay.c`, the four core files and the STM32 adapter to the build,
- force-includes `usermod/mpy_replay_hooks.h` in every file, so `py/vm.c` sees
  `MICROPY_VM_HOOK_LOOP = mpy_replay_hook_loop();` without editing any MicroPython file
  (one 64-bit increment and one test per VM branch),
- sets `-DMRL_FLUSH_EVERY_EVENT=1` for STM32, so each event is written straight to backup SRAM and survives a hard fault.

Flash it:

```bash
make -C ports/stm32 BOARD=$BOARD deploy MICROPY_USE_COMPILER_PLUGIN=none \
     USER_C_MODULES=$REPLAY MPY_REPLAY_PORT=stm32 MPY_REPLAY_FW_HASH=$HASH
# ST-Link: deploy-stlink   OpenOCD: deploy-openocd
```

### 5.1 Check the module is in the firmware

In the REPL:

```python
>>> import replay
>>> replay.about()
'mpy-replay 0.1.0 - Copyright (c) 2026 Ahmed (lachiheb-ahmed) - MIT - ...'
>>> replay.mode() == replay.OFF         # nothing is recording yet
True
>>> replay.has_log()                    # None on a fresh board: backup SRAM holds no valid log
```

### 5.2 Smoke-test the functions from the REPL

This checks start, mark, checkpoint, stop, has_log and read on the real backup SRAM, before using the full test.
(Do it **now**, not later: `replay.start()` erases whatever log is stored.)

```python
>>> import replay
>>> replay.start()
>>> replay.mode() == replay.RECORD
True
>>> replay.mark("hello")
>>> replay.checkpoint(42)
>>> replay.clock()                      # logical clock: grows with every VM branch
>>> replay.stop()
>>> replay.has_log()                    # (bytes, events, last_lclk, None): events should be at least 2
>>> replay.read(0, 4)
b'MRL1'
```

`b'MRL1'` is the log magic number. If you see it, the board wrote to backup SRAM and read it back.

If `replay.start()` raises `RuntimeError` about storage: the adapter could not enable backup SRAM or the backup
regulator. See the troubleshooting table; this is exactly the kind of result we want you to report.

---

## 6. Step 4: copy the test files to the board

```bash
cd $REPLAY
mpremote cp usermod/replay_io.py :replay_io.py          # the Python proxies
mpremote cp examples/board/boot.py :boot.py             # rescues the previous log at every boot
mpremote cp examples/board/main.py :main.py             # the test program
```

**Edit the ADC pin first.** In `examples/board/main.py` change `machine.ADC("A0")` to an analog pin of **your**
board (for example `"X1"` on PYBv1.1), then copy it. Leave the pin floating or connect a potentiometer; any value works,
because the log keeps whatever was read.

What the two files do:

`main.py` (the program under test)

```python
adc_hw = machine.ADC("A0")
replay.start()                       # logical clock starts at 0 here; log is erased and a new one begins
adc = replay_io.ADC(adc_hw, uid=0)   # proxy: every read is recorded

def main():
    ...
        acc = (acc * 31 + adc.read_u16()) & 0xFFFFFF     # state that depends on the inputs
        if n % 100 == 0:
            replay.checkpoint(acc)                       # compared during replay
        time.sleep_ms(10)
        if n == 300:
            print("RESULT n=%d acc=%d" % (n, acc))
            raise ValueError("boom")                     # the "bug"

replay_io.run(main)                  # logs the crash text, then re-raises
```

`boot.py` (runs before `main.py` at every boot): asks `replay.has_log()` whether a log from the previous run is
still in backup SRAM and, if so, copies it with `replay.read()` to `/flash/crash0.mrl` (then `crash1.mrl`, ... up to 10 files).
It runs **before** `main.py`, which is why the old log is saved before `replay.start()` erases it.

---

## 7. Step 5: record and crash

Reset the board (`mpremote reset`, or the reset button) and watch the serial output:

```bash
mpremote            # or any serial terminal; it opens the REPL
```

Within about 3 seconds (300 samples x 10 ms) you should see:

```
RESULT n=300 acc=<number>
Traceback (most recent call last):
  ...
ValueError: boom
```

**Write down the `acc` value.** This is the number the PC replay must reproduce.

The log is about 2.5 KB (64-byte header + 304 events), which fits in the 4 KB backup SRAM:
300 ADC reads + 3 checkpoints + 1 crash record = 304 events.

---

## 8. Step 6: rescue the log

1. **Wait for the crash first** (section 7), so the `ValueError` is already in the log.
2. Press the **reset button** (not a power cycle, unless VBAT is powered).
3. `boot.py` runs, finds the log, prints `previous log: (...)` and `saved /flash/crash0.mrl`.
   After that, `main.py` starts again and begins a **new** recording, which will crash 3 seconds later.
4. Wait about 4 seconds until the new run has crashed too, so the board is idle, then copy the file to the PC:

```bash
mpremote fs cp :crash0.mrl .
```

> **Important.** `crash0.mrl` belongs to the **first** run (the one whose `acc` you wrote down), not to the run
> that is happening right after the reset. A second reset would save the second run as `crash1.mrl`.

Check it without leaving the REPL, right after a crash and a reset, before `main.py` overwrites it:

```python
>>> import replay
>>> replay.has_log()
(2500, 304, 12345, "ValueError('boom',)")      # (bytes, events, last logical clock, crash text); numbers vary
```

Now look at the file on the PC:

```bash
$REPLAY/tools/mrl_dump crash0.mrl          # header, every event, the crash text
$REPLAY/tools/mrl_dump -q crash0.mrl       # summary only
```

You should see the firmware hash you exported in section 5, about 300 ADC events, 3 checkpoints, and a final CRASH
record with `ValueError('boom',)`.

---

## 9. Step 7: build the unix replay binary

The replay runs on MicroPython's **unix port**, built with the **same MicroPython tree** (already patched in section 4)
and the **same hash**.

```bash
cd $MPY
make -C mpy-cross                      # skip if already built
make -C ports/unix submodules
export HASH=$(cat ~/fw_hash.txt)     # same value as the board firmware

make -C ports/unix VARIANT=standard USER_C_MODULES=$REPLAY \
     MPY_REPLAY_PORT=unix MPY_REPLAY_MODE=1 MPY_REPLAY_FW_HASH=$HASH \
     MICROPY_PY_SSL=0 MICROPY_PY_BTREE=0 BUILD=build-replay \
     CFLAGS_EXTRA='-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT'
```

| Option | Meaning |
|---|---|
| `MPY_REPLAY_PORT=unix` | adapter that reads the log from a file |
| `MPY_REPLAY_MODE=1` | **replay build**: `replay.start()` loads `$MRL_REPLAY_LOG` instead of recording |
| `MPY_REPLAY_FW_HASH=$HASH` | must equal the board's hash, or the replay is refused |
| `MICROPY_PY_SSL=0 MICROPY_PY_BTREE=0` | much faster build; the module does not need them |
| `CFLAGS_EXTRA=...FLOAT` | STM32 uses single-precision floats, the PC defaults to double. Matching them avoids numeric differences (this test only uses integers, but do it anyway) |

The binary is `$MPY/ports/unix/build-replay/micropython`.

> `examples/pc/machine.py` is a tiny stand-in for the board's `machine` module: it only provides a `machine.ADC`
> constructor. During replay the hardware is never read; the logged values are returned instead.

---

## 10. Step 8: replay the crash on the PC

```bash
cd $REPLAY
export MICROPYPATH=$REPLAY/examples/pc:$REPLAY/usermod       # machine.py stand-in + replay_io.py
cp examples/board/main.py main.py                            # the SAME main.py you put on the board
cp /path/to/crash0.mrl .

MRL_REPLAY_LOG=crash0.mrl $MPY/ports/unix/build-replay/micropython main.py
```

(The pin name in `main.py` does not matter on the PC; `examples/pc/machine.py` accepts anything.)

Expected output:

```
RESULT n=300 acc=<same value as on the board>
replay: crash reproduced: ValueError('boom',)
```

What happened: every `adc.read_u16()` returned the logged value, `time.sleep_ms()` waited again, the 3 checkpoints were
compared with the recording, and the final `ValueError('boom')` matched the crash record. If anything had differed
(a different input kind, a different logical clock, a different checkpoint value), the replay would have stopped with
a **divergence** report instead.

Now you can debug the crash as often as you want on the PC, for example with `gdb` or with extra `print()` calls in
a copy of `main.py` (adding code **after** the logged inputs does not change what was recorded, but code that
changes the number of VM branches before an input can cause a divergence; see the rules in the README, section 7).

### 10.1 Optional rehearsal without a board

If you want to see the whole mechanism before flashing anything, build a **record** binary for the PC (same command
as section 9 but without `MPY_REPLAY_MODE=1` and with `BUILD=build-rec`) and use the test program that mimics the board test:

```bash
export MICROPYPATH=$REPLAY/examples/pc:$REPLAY/usermod
MRL_LOG=rehearsal.mrl $MPY/ports/unix/build-rec/micropython $REPLAY/tests/e2e/app_board.py 100
MRL_REPLAY_LOG=rehearsal.mrl $MPY/ports/unix/build-replay/micropython $REPLAY/tests/e2e/app_board.py 50000
```

The number after the script name only changes how long some code runs **before** `replay.start()`. The replay still
gives the same `acc`, which shows that code before `start()` is not part of the log.

The project's automatic version of this is `tests/e2e/run_e2e.sh` and `tests/e2e/run_regress.sh` (see `.github/workflows/ci.yml`).

---

## 11. Pass criteria and what to report

| Check | Expected |
|---|---|
| `replay.about()` on the board | prints the copyright string |
| REPL smoke test (section 5.2) | `replay.read(0, 4)` returns `b'MRL1'` |
| `has_log()` after the crash and a reset | `(about 2500, 304, <clock>, "ValueError('boom',)")` |
| `crash0.mrl` on the PC | `mrl_dump` shows your firmware hash and a CRASH record |
| PC replay | same `acc` as the board and `crash reproduced` |
| Replay build with a different `MPY_REPLAY_FW_HASH` | refuses with `firmware hash mismatch` (override with `MRL_IGNORE_FW_HASH=1`) |
| Change one input byte in the `.mrl` (hex editor) and replay | the CRC check ends the log early; the replay reports a divergence or end of log |

To test the hash check, build a second replay binary into another folder (`BUILD=build-badhash`) with
`MPY_REPLAY_FW_HASH=0123456789abcdef0123456789abcdef01234567` and replay `crash0.mrl` with it.

When you report, please include: board name, MicroPython commit, the `acc` on the board and on the PC,
the output of `replay.has_log()`, and anything that failed (copy the exact error text).
Please do **not** attach `.mrl` logs from real devices to public issues, they contain your sensor data.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `replay_port_stm32.c: ... F4/F7 only` | Your MCU family is not covered. Port the clock and power enable sequence for BKPSRAM. |
| `STM32_HAL_H` or `BKPSRAM_BASE` undeclared | Different MicroPython version or family. Include the HAL header of your family. |
| Undefined `_heap_start` / `_heap_end` | Your linker script uses other names; return a constant or the GC heap size in `replay_port_heap_size()`. It is informational only. |
| `MICROPY_VM_HOOK_LOOP` redefined | The board config already defines it. Chain both calls in one macro. |
| `replay.start()` fails on the board / `has_log()` always `None` | The backup regulator did not become ready. Build with `-DMRL_STM32_REQUIRE_BKUP_REG=0` (add it to `CFLAGS_EXTRA`) to accept reset-only retention, and report it. |
| `RuntimeError: replay: log storage full` at `stop()` | More than 4 KB of events. Record fewer or smaller events. The log up to that point is valid. |
| `RuntimeError: replay diverged (reason 1/2...)` | The PC run asked for a different input or at a different clock. Check that `main.py` is identical, that code before `replay.start()` has no recorded inputs, and that nothing branches on `replay.mode()`. |
| `firmware hash mismatch` on the PC | The replay build was made from a different `MPY_REPLAY_FW_HASH` than the board firmware. |
| `has_log()` returns `None` | The board was power-cycled without VBAT, or `replay.start()` ran before `boot.py` read the log, or you ran the REPL smoke test (section 5.2) after the crash, which erased it. |
| `crash0.mrl` is missing | `boot.py` did not find a log at the reset. Make sure the crash happened first and that you pressed reset (not power off). |
| `mpremote` interrupts the program | `mpremote` sends Ctrl+C when it connects. Wait for the 3-second run to end (crash) before copying files. |
| Replay prints a different `acc` | Different `main.py`, a log from another run (remember `crash0.mrl` is the **first** run), or a different float/word configuration; rebuild with the FLOAT flag from section 9. |
