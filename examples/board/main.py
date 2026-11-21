# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Board test: records 300 ADC samples, then crashes on purpose.
# Set ADC_PIN to an analog-capable pin of YOUR board:
#   NUCLEO_F429ZI / NUCLEO_F767ZI: "A0"      PYBv1.1: "X1"      others: see your board pinout
ADC_PIN = "A0"
import time
import machine
import replay
import replay_io

adc_hw = machine.ADC(machine.Pin(ADC_PIN))
replay.start()                       # logical clock starts here
adc = replay_io.ADC(adc_hw, uid=0)


def main():
    n = 0
    acc = 0
    while True:
        acc = (acc * 31 + adc.read_u16()) & 0xFFFFFF
        n += 1
        if n % 100 == 0:
            replay.checkpoint(acc)
        time.sleep_ms(10)
        if n == 300:
            print("RESULT n=%d acc=%d" % (n, acc))
            raise ValueError("boom")


replay_io.run(main)
