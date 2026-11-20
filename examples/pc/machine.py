# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# PC stand-in for the board's machine module, used ONLY to replay a log.
# Hardware reads are never executed during replay; the constructor just has to exist.
import os
class ADC:
    def __init__(self, *a, **k): pass
    def read_u16(self):
        return int.from_bytes(os.urandom(2), "little")
