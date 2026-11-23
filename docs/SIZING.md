# How much fits in the log (sizing)

Every event costs a fixed overhead plus its payload:

```
overhead = 1 (clock delta, more if more than 127 VM branches passed since the previous event)
         + 1 (type) + 1 (length) + 2 (CRC16)            = 5 bytes
event    = 5 + payload
```

The header takes 64 bytes, so a 4 KB backup SRAM leaves 4032 bytes. Payloads are the proxy's header (device id, size, address...) plus the data bytes:

| Input | Payload (bytes) | Bytes per event | Events in 4 KB |
|---|---|---|---|
| GPIO `Pin.value()` read | 2 | 7 | about 576 |
| ADC `read_u16()` | 3 | 8 | about 504 |
| `time.ticks_ms()` / `ticks_us()` | 8 | 13 | about 310 |
| I2C read of 6 bytes | 13 | 18 | about 224 |
| SPI read of 12 bytes | 15 | 20 | about 201 |
| UART read of 16 bytes | 18 | 23 | about 175 |

**Recording time = events in the log / how often your program reads that input.** Examples at typical poll rates: an ADC read at 100 Hz fills the log in about 5 seconds, at 1 kHz in about 0.5 seconds; a 6-byte I2C sensor at 1 Hz lasts about 3.7 minutes. Several inputs share the same bytes, so the time is shorter. Clock reads (`time.ticks_*`) count as events too.

Larger storage (flash, SD, FRAM) and a streaming backend are planned; see the roadmap in the README.

## What this is not for

`mpy-replay` records what the *Python program* reads, in the order it reads it, on a logical clock. It does not record real time, and it cannot capture signals faster than Python can poll:

- **Timing between reads is logical, not real.** Two pins read one after the other keep their order, but the real interval between the reads (skew) is not stored and is not reproduced on replay.
- **Waveforms and fast protocols** (DMX, PIO or RMT decoding, bit-banged buses) are out of scope. At that level a logic analyzer or scope capture replayed in an emulator is the right tool. If the decoding happens in hardware (PIO, UART), `mpy-replay` can record the *decoded* data the program receives, for example the bytes of a frame, but not the timing or framing information that the hardware consumed.
- **Hardware side effects.** While recording, reads happen on the real hardware as usual. While replaying, reads are not performed and outputs routed through `Pin.value(x)` or `replay_io.output()` are skipped; any output you do not route through them is executed.
