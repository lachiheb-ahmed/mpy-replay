# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
#
# A bug that depends on the sequence of inputs AND on timing, and that therefore
# does not happen on every run: a frame parser for a noisy serial link.
#
# The bug: when the sender stalls in the middle of a frame, the parser times out
# and goes back to IDLE, but forgets to reset `self.idx`. If the next frame is
# shorter than the stale index, `self.buf[self.idx]` raises IndexError.
#
# FakeUart stands in for machine.UART on the PC: random frames, random chunk sizes,
# and occasional stalls. On a board you would use the real UART instead.
import os
import time

import replay
import replay_io

TIMEOUT_MS = 30
IDLE, LEN, PAY, SUM = 0, 1, 2, 3


class Parser:
    def __init__(self):
        self.state = IDLE
        self.idx = 0
        self.expect = 0
        self.buf = None
        self.last = 0
        self.frames = 0
        self.bad = 0

    def feed(self, b, now):
        if self.state != IDLE and time.ticks_diff(now, self.last) > TIMEOUT_MS:
            self.state = IDLE  # BUG: self.idx is not reset
        self.last = now
        if self.state == IDLE:
            if b == 0x7E:
                self.state = LEN
        elif self.state == LEN:
            self.expect = b
            self.buf = bytearray(b)
            self.state = PAY if b else SUM
        elif self.state == PAY:
            self.buf[self.idx] = b
            self.idx += 1
            if self.idx == self.expect:
                self.state = SUM
        else:
            if (sum(self.buf) & 0xFF) == b:
                self.frames += 1
            else:
                self.bad += 1
            self.idx = 0
            self.state = IDLE


class FakeUart:
    def __init__(self):
        self.pending = b""

    def _frame(self):
        n = 1 + os.urandom(1)[0] % 6
        payload = os.urandom(n)
        return bytes((0x7E, n)) + payload + bytes((sum(payload) & 0xFF,))

    def read(self, n):
        while len(self.pending) < 3:
            self.pending += self._frame()
        k = 1 + os.urandom(1)[0] % 5
        data, self.pending = self.pending[:k], self.pending[k:]
        if os.urandom(1)[0] < 20:  # about 8% of the reads: the sender stalls
            time.sleep_ms(50)
        return data


replay.start()
uart = replay_io.UART(FakeUart(), uid=1)
parser = Parser()


def main():
    reads = 0
    try:
        while reads < 3000:
            data = uart.read(16)
            now = time.ticks_ms()
            reads += 1
            for b in data:
                parser.feed(b, now)
        print("NO CRASH in %d reads (frames=%d bad=%d)" % (reads, parser.frames, parser.bad))
    except IndexError:
        print("PARSER CRASH after reads=%d frames=%d bad=%d idx=%d expect=%d"
              % (reads, parser.frames, parser.bad, parser.idx, parser.expect))
        raise


replay_io.run(main)
