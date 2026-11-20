# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Hardware-style IRQs (SIGUSR1 -> mp_sched_schedule, like an ISR) hit a running loop.
import replay
import replay_io

replay.start()
count = 0
mix = 0


@replay_io.irq_handler(7, arg=0)
def on_irq(src):
    global count, mix
    count += 1 + src
    mix = (mix * 7 + count) & 0xFFFF


replay._test_irq(on_irq)


def main():
    n = 0
    acc = 0
    try:
        while True:
            n += 1
            acc = (acc + n + mix) & 0xFFFFFF
    except KeyboardInterrupt:
        print("RESULT n=%d acc=%d count=%d mix=%d lclk=%d" % (n, acc, count, mix, replay.clock()))
        raise


replay_io.run(main)
