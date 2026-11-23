# Example: a bug that depends on inputs and timing

`app.py` is a frame parser for a noisy serial link with a real bug: when the sender stalls in the middle of a frame, the parser times out and returns to IDLE but forgets to reset `self.idx`. If the next frame is shorter than the stale index, it raises `IndexError`. Whether and when that happens depends on the random frames and on timing, so it does **not** happen the same way on every run.

`FakeUart` simulates the link on the PC (random frames, random chunk sizes, occasional stalls). On a board you would wrap the real UART with `replay_io.UART` instead.

## Run it

```bash
export MICROPYPATH=<repo>/usermod:.
./run_demo.sh <record-micropython> <replay-micropython>
```

It records until the bug appears, then replays the recording three times. Example output:

```
== recorded run:
PARSER CRASH after reads=46 frames=14 bad=1 idx=5 expect=2
== replay x3 (must be identical):
PARSER CRASH after reads=46 frames=14 bad=1 idx=5 expect=2
PARSER CRASH after reads=46 frames=14 bad=1 idx=5 expect=2
PARSER CRASH after reads=46 frames=14 bad=1 idx=5 expect=2
```

`idx=5 expect=2` is the root cause: a stale index larger than the frame length.

## What the example shows

- **The bug is random across runs.** Five independent recordings crashed after 107, 6, 229, 3 and 117 reads. The recording of one of them reproduces the same state on every replay.
- **You can inspect the reproduced failure.** Add a `print()` inside the parser (for example before `self.buf[self.idx] = b`) and replay the same log: the replay still succeeds and shows how `idx` and `expect` evolve until the crash. This works because a `print()` adds no VM branch.
- **The log is small but not tiny.** This run needed 93 events and 1156 bytes for 46 reads (about 25 bytes per read, one UART read and one clock read each). A 4 KB backup SRAM would hold roughly 160 reads of this program, so a crash after that point would not be captured.

## What it does not show (yet)

- **Replaying fixed code.** If you fix the parser (add `self.idx = 0` after the timeout) and replay the same log, the replay stops with `replay diverged ... at lclk ...`, because the fix changes which branches run and the replay checks the logical clock. An input-only replay mode, to re-test a fix against recorded inputs, does not exist yet.
