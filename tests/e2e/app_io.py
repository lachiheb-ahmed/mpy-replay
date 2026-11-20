# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Mostly-I/O workload: Ctrl+C almost always arrives while a recorded read runs.
import time
import replay
import replay_io

replay.start()


class SlowUart:  # a "device" that takes time to answer
    def read(self, n):
        import os
        time.sleep_us(300)
        return os.urandom(3)


uart = replay_io.UART(SlowUart(), uid=2)


def main():
    n = 0
    acc = 0
    try:
        while True:
            n += 1
            acc = (acc * 33 + sum(uart.read(8))) & 0xFFFFFF
    except KeyboardInterrupt:
        print("RESULT n=%d acc=%d lclk=%d" % (n, acc, replay.clock()))
        raise


replay_io.run(main)
