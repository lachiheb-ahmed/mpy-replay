# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Runs at every boot BEFORE main.py: rescue the log left by the previous run.
import os
import replay


def _free_name():
    for i in range(10):
        p = "/flash/crash%d.mrl" % i
        try:
            os.stat(p)
        except OSError:
            return p
    return None


info = replay.has_log()          # (bytes, events, last_lclk, crash_text) or None
name = _free_name() if info else None
if info and name:
    print("previous log:", info)
    with open(name, "wb") as f:
        off = 0
        while off < info[0]:
            n = min(512, info[0] - off)
            f.write(replay.read(off, n))
            off += n
    print("saved", name)
