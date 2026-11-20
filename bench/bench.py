# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Micro-benchmarks for the cost of the VM hook.  usage: micropython bench.py [rec]
import sys
import time


def loop(n):                 # tight loop: one VM branch per iteration
    x = 0
    for i in range(n):
        x += i & 3
    return x


def fib(n):                  # call-heavy, conditional expression
    return n if n < 2 else fib(n - 1) + fib(n - 2)


def mix(n):                  # list access, branches, arithmetic
    a = [0] * 64
    s = 0
    for i in range(n):
        j = i & 63
        a[j] = a[j] + i
        if a[j] & 1:
            s += 1
    return s


def best(f, *a):
    # time_ns is not routed through the recorder, so it measures real time in every mode
    ts = []
    for _ in range(9):
        t = time.time_ns()
        f(*a)
        ts.append((time.time_ns() - t) // 1000)
    return min(ts)           # minimum of 9 runs, microseconds (least noisy estimator)


rec = len(sys.argv) > 1 and sys.argv[1] == "rec"
if rec:
    import replay
    replay.start()

r = (best(loop, 300000), best(fib, 22), best(mix, 200000))
print("loop %d fib %d mix %d" % r)
if rec:
    replay.stop(True)
