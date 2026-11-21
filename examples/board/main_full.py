# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Extended board test: records several kinds of inputs (ADC, clock, recorded random
# numbers), uses marks and checkpoints, then crashes on purpose.
# About 2.5 KB of log, so it fits the 4 KB backup SRAM.
import random
import time
import machine
import replay
import replay_io

ADC_PIN = "A0"                       # analog-capable pin of YOUR board (PYBv1.1: "X1")

adc_hw = machine.ADC(machine.Pin(ADC_PIN))
replay.start()                       # the logical clock starts here
replay_io.seed_random()              # `random` is seeded from a recorded value
adc = replay_io.ADC(adc_hw, uid=0)


def main():
    acc = 0
    for n in range(1, 101):
        t = time.ticks_ms()          # recorded clock
        acc = (acc * 31 + adc.read_u16() + random.getrandbits(8) + (t & 0xFF)) & 0xFFFFFF
        if n % 25 == 0:
            replay.mark("n=%d" % n)  # a replay with different marks diverges
            replay.checkpoint(acc)   # a replay with a different state diverges
        time.sleep_ms(5)
    print("RESULT n=100 acc=%d" % acc)
    raise ValueError("boom2")


replay_io.run(main)
