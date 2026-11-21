# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Hook-cost benchmark for a real board. Run it on the STOCK firmware and on the
# firmware built with the module (recording NOT started), then compare.
#   mpremote run bench/bench_mcu.py
# Result: minimum of 5 runs per workload, in microseconds.
import time


def loop(n):                 # one VM branch per iteration
    x = 0
    for i in range(n):
        x += i & 3
    return x


def fib(n):
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def mix(n):
    a = [0] * 64
    s = 0
    for i in range(n):
        j = i & 63
        a[j] = a[j] + i
        if a[j] & 1:
            s += 1
    return s


def best(f, *args):
    ts = []
    for _ in range(5):
        t = time.ticks_us()
        f(*args)
        ts.append(time.ticks_diff(time.ticks_us(), t))
    return min(ts)


print("loop(20000) %d us  fib(18) %d us  mix(10000) %d us" % (best(loop, 20000), best(fib, 18), best(mix, 10000)))
