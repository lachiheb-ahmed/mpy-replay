# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# PC stand-in for the board's `machine` module, used ONLY to replay a log on the
# unix port. Hardware reads are never executed during replay; the constructors
# just have to exist. (When RECORDING on the PC for a demo, read_u16 returns
# random numbers.)
import os


class Pin:
    OUT = 1
    IN = 0

    def __init__(self, *args, **kwargs):
        self._v = 0

    def value(self, *args):
        if args:
            self._v = 1 if args[0] else 0
            return None
        return self._v


class ADC:
    def __init__(self, *args, **kwargs):
        pass

    def read_u16(self):
        return int.from_bytes(os.urandom(2), "little")
