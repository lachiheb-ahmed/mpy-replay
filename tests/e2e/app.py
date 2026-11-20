# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# End-to-end sample: non-deterministic inputs (clock, "UART", RNG) drive the
# program state; it ends with Ctrl+C (SIGINT) at an unpredictable moment.
import time
import replay
import replay_io

replay.start()


class FakeUart:  # stands in for machine.UART on the unix port
    def read(self, n):
        import os
        return os.urandom(min(n, 4))


uart = replay_io.UART(FakeUart(), uid=1)


def main():
    n = 0
    acc = 0
    try:
        while True:
            n += 1
            if n % 997 == 0:
                acc = (acc * 31 + (time.ticks_us() & 0xFFFF)) & 0xFFFFFF
            if n % 1999 == 0:
                acc ^= sum(uart.read(8))
                acc ^= replay_io.urandom(2)[0]
            if n % 50000 == 0:
                replay.checkpoint(acc)
    except KeyboardInterrupt:
        replay.mark("stopped")
        print("RESULT n=%d acc=%d lclk=%d" % (n, acc, replay.clock()))
        raise


replay_io.run(main)
