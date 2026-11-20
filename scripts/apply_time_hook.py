#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
"""Route time.ticks_ms/us/cpu through the recorder.

    python3 scripts/apply_time_hook.py /path/to/micropython          # apply
    python3 scripts/apply_time_hook.py /path/to/micropython --revert # undo

Edits extmod/modtime.c only. Idempotent. Build with the mpy-replay user
module, which force-includes the MPY_REPLAY_TICKS_* macros.
"""
import re
import sys

PAIRS = [
    ("mp_hal_ticks_ms()", "MPY_REPLAY_TICKS_MS(mp_hal_ticks_ms())"),
    ("mp_hal_ticks_us()", "MPY_REPLAY_TICKS_US(mp_hal_ticks_us())"),
    ("mp_hal_ticks_cpu()", "MPY_REPLAY_TICKS_CPU(mp_hal_ticks_cpu())"),
]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1].rstrip("/") + "/extmod/modtime.c"
    revert = "--revert" in sys.argv
    src = open(path).read()
    out = src
    for plain, hooked in PAIRS:
        if revert:
            out = out.replace(hooked, plain)
        else:
            # only the "return MP_OBJ_NEW_SMALL_INT(<call> & ...)" sites
            out = re.sub(r"MP_OBJ_NEW_SMALL_INT\(" + re.escape(plain),
                         "MP_OBJ_NEW_SMALL_INT(" + hooked.replace("\\", "\\\\"), out)
    if out == src:
        print("nothing to do (already %s)" % ("reverted" if revert else "applied"))
        return
    open(path, "w").write(out)
    print("%s %s" % ("reverted" if revert else "patched", path))


main()
