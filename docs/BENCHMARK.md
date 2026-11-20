# Cost of the VM hook

**Method.** `bench/bench.py` runs three workloads and reports the minimum of 9 runs (microseconds), measured with `time.time_ns` (not routed through the recorder). `bench/run_bench.sh` repeats it 6 times per mode and keeps the minimum. Binaries: stock unix port (`base`), the same port with the module but recording not started (`off`), recording (`record`), replaying (`replay`). MicroPython master `9e10c3e`, unix port, `standard` variant, default optimization, x86-64.

**Limits of this measurement.** The machine has one shared core and CPU pinning was not available, so there is noise. Different executables also differ in code layout: in the table the `replay` binary appears *faster* than `off`, which is impossible by design (it does more work), so differences of about +/-15% between two different executables on the tight loop are layout noise, not hook cost. Only the same-binary comparison (below) is clean.

| Workload | VM branches executed |
|---|---|
| `loop` (300 000 iterations) | 300 002 |
| `fib(22)` | 57 313 |
| `mix` (200 000 iterations) | 400 002 |

**Results (microseconds, lower is better)**

| Mode | loop | fib(22) | mix | vs base |
|---|---|---|---|---|
| base | 10 502 | 4 208 | 16 822 | |
| off | 12 572 | 4 268 | 19 070 | +20%, +1%, +13% |
| record | 13 677 | 4 463 | 19 529 | +30%, +6%, +16% |
| replay | 10 691 | 4 204 | 17 627 | not meaningful, see above |

**Same executable, recording vs off** (10 interleaved passes): **+8%** (loop), **+10%** (fib), **+5%** (mix).

**Reading.** Realistic code (calls, conditionals, list access) pays about 1% to 13% with the module present but idle. A loop that is nothing but branches is the worst case, about 20% here. Starting a recording adds roughly 5% to 10% more. Removing the module removes the cost entirely, because `MICROPY_VM_HOOK_LOOP` is then not defined.

**Cortex-M4 fast path (from the compiled `vm.o` of NUCLEO_F429ZI, recording idle).** Ten instructions per VM branch:

```
ldr r3,[r9]      adds r3,#1      str r3,[r9]            ; low word of the 64-bit counter
ldr r3,[r9,#4]   adc  r3,r3,#0   str r3,[r9,#4]         ; high word
ldr r3,[pc,#..]  ldrb r3,[r3]    cmp r3,#0   bne slow   ; mode check
```

This is about 12 to 13 cycles on a Cortex-M4 (loads take 2 cycles), an **estimate from the instruction count, not a measurement on hardware**. A 32-bit counter would save 3 instructions; it is a candidate optimization.

**Not measured:** hook cost on a real STM32, the cost of `time.ticks_*` and proxy calls (they log an event each), and effects on `asyncio` workloads.
