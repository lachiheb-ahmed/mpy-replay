# FAQ: questions you are likely to ask, with honest answers

**1. Why not use a debugger, logging, or a core dump?**
A debugger needs the device on your desk, and stopping it changes timing. Logging shows what you decided to print, not the state that led to the crash. A core dump shows the state at the crash but not how it got there. `mpy-replay` stores the inputs, so you can re-run the whole path to the crash as many times as you need and inspect anything.

**2. Is the idea new?**
No. Deterministic record and replay is an established technique (for example `rr` for Linux programs and many VM-level replay systems), and a quick search shows the same idea being applied to AI agents today. In one search I found no equivalent for MicroPython, but that was not exhaustive, so I do not claim novelty. The contribution here is a working design for a microcontroller VM, with its specific problems solved (logical clock, interrupt delivery, replaying hardware reads safely).

**3. What is the performance overhead?**
About 1% to 13% on realistic code with the module idle, up to about 20% on a loop made only of branches, and recording adds roughly 5% to 10% on top. On Cortex-M the fast path is 10 instructions per VM branch (about 12 to 13 cycles, estimated). Measured on the unix port only; see [BENCHMARK.md](BENCHMARK.md) for the method and its noise. I have not measured it on real hardware yet.

**4. What about the garbage collector? Isn't it non-deterministic?**
The GC is deterministic given the same allocations and heap layout. The replay does not depend on GC timing: it replays inputs, not memory. Differences appear only if the program's behavior depends on heap state (`gc.mem_free()`, a `MemoryError`) and the replay host has a different heap. The heap CRC checkpoint exists for bit-exact targets, which are not implemented yet.

**5. Why a logical clock instead of an instruction counter or real time?**
Real time cannot order events reproducibly. A hardware instruction counter is not available on most MCUs and would not align with Python semantics. Counting VM branches uses a hook MicroPython already provides, and asynchronous events are delivered exactly at those points anyway (callbacks and exceptions run in `pending_exception_check`), so the count is the natural unit.

**6. What does it not capture?**
Hard IRQ handlers (they run inside the interrupt and cannot be placed on the clock), reads that bypass the recorder, `time.time()` and other unrecorded sources, DMA writing to memory behind the VM, and anything on a second thread or core. See the Limitations section of the README. A replay that hits one of these reports a divergence instead of pretending to succeed.

**7. Can I replay an STM32 recording on my PC? What can differ?**
Yes, that is the main use. The host must match the board's numbers: STM32 uses single-precision floats and the unix port defaults to double, so build the replay binary with `CFLAGS_EXTRA='-DMICROPY_FLOAT_IMPL=MICROPY_FLOAT_IMPL_FLOAT'` (I verified it builds and prints `0.3` for `0.1+0.2`). Word size also differs (31-bit vs 63-bit small ints), so `sys.maxsize`, `id()` and `set` iteration order may differ. Avoid logic that depends on them. Bit-exact replay on an emulated Cortex-M is future work.

**8. Does it work with `asyncio`?**
`asyncio` uses `time.ticks_ms` and the scheduler, both of which are hooked, so it should work, but I have not tested it. It is on the roadmap. Treat it as unverified.

**9. What about network sockets (ESP32, WiFi)?**
Not supported in v0.1. Socket reads are inputs and would need a proxy like the UART one; none exists yet.

**10. Does it work on ports other than the unix port and STM32?**
The core and the MicroPython glue are port-independent; each port needs a small storage adapter. Only the unix port is validated end to end. STM32 builds and links for three boards but has not run on hardware. Other ports are planned, not tested.

**11. The log only holds 4 KB on STM32. Is that useful?**
It is a flight recorder: a few hundred events, enough for a short window from `replay.start()`. Replay needs the log from the start of the run, so a longer recording needs a bigger backend (flash, FRAM, SD). A ring buffer with periodic state snapshots would allow arbitrary length; it is the largest planned improvement and the hardest one.

**11b. How many events fit, and what is it not for?**
See [SIZING.md](SIZING.md): the byte cost of each input, the resulting recording time at typical poll rates, and the cases that are out of scope (waveform-level capture, signals faster than Python can poll, real-time skew between pins).

**12. Is the log sensitive data?**
Yes. It contains the raw inputs: sensor values, serial traffic, possibly credentials or personal data passing over UART. Treat `.mrl` files like logs containing secrets, and do not post them in public issues.

**13. How do I know a replay really reproduced the run?**
Every input is checked for kind and logical clock before it is returned; a mismatch stops the replay with a divergence report. Marks and checkpoints add explicit synchronization points, the crash text must match, `stop()` fails if recorded events were not consumed, and the firmware hash must match. In the tests, the final program state and logical clock were identical after millions of iterations.

**14. Why is it a user module plus a patch, not a pull request to MicroPython?**
The module needs no changes to MicroPython except one small patch to `extmod/modtime.c`. A large feature with a performance cost is unlikely to be accepted upstream as is. The sensible path is to let it prove itself outside, then propose only a minimal hook if maintainers are interested.

**15. Which MicroPython versions are supported?**
Tested only against master at commit `9e10c3e`. The hook (`MICROPY_VM_HOOK_LOOP`) and the scheduler API are long-standing, but names can change, so pin your version and rebuild.

**16. What is the license? Can I use it commercially?**
MIT. You may use and modify it commercially; the copyright notice must be kept.

**17. Has it run on real hardware?**
Not yet. The unix port is validated end to end; the STM32 firmware builds and links for NUCLEO-F429ZI, PYBv1.1 and NUCLEO-F767ZI, but backup-SRAM retention and the full flow on a board are untested. [BOARD_TEST.md](BOARD_TEST.md) is the procedure.
