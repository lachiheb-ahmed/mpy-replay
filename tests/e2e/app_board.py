# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Mimics the board test: hardware ADC + crash after 300 samples.
# PRE_BOOT_WORK simulates boot-time code that differs between board and PC.
import sys
import time
import replay
import replay_io
import machine

adc_hw = machine.ADC("A0")
for _i in range(int(sys.argv[1]) if len(sys.argv) > 1 else 0):   # differs per run
    pass

replay.start()
adc = replay_io.ADC(adc_hw, uid=0)


def main():
    n = 0
    acc = 0
    while True:
        acc = (acc * 31 + adc.read_u16()) & 0xFFFFFF
        n += 1
        if n % 100 == 0:
            replay.checkpoint(acc)
        time.sleep_ms(1)
        if n == 300:
            print("RESULT n=%d acc=%d" % (n, acc))
            raise ValueError("boom")


replay_io.run(main)
