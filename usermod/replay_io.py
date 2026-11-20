# SPDX-License-Identifier: MIT
# mpy-replay - deterministic record & replay for MicroPython
# Copyright (c) 2026 Ahmed (lachiheb-ahmed)
#
# Portable, pure-Python proxies for the peripherals whose reads must be
# recorded. They work on every port: no port-specific C code is needed.
#
# Rule: the Python code of a proxy must run the same VM branches in record
# and in replay mode (the logical clock counts them). Only replay._io decides
# whether the hardware is touched, and it freezes the clock while it does.
#
#   adc  = replay_io.ADC(machine.ADC(26), uid=0)
#   uart = replay_io.UART(machine.UART(1, 115200), uid=1)
#   replay_io.run(main)          # logs the last words if main() crashes
import struct

import replay

_handlers = {}


def _check(got, want, what="device id"):
    if got != want:
        raise RuntimeError("replay_io: %s mismatch (log %r, program %r)" % (what, got, want))


def _need(raw, k):
    if len(raw) < k:
        raise RuntimeError("replay_io: short record in log")


def _n(n, hi):
    if n < 0:
        raise ValueError("n must be >= 0")
    return min(n, hi)


class ADC:
    def __init__(self, adc, uid=0):
        self._a = adc
        self._id = uid & 0xFF

    def _fetch(self):
        return struct.pack("<BH", self._id, self._a.read_u16())

    def read_u16(self):
        uid, val = struct.unpack("<BH", replay._io(replay.ADC, self._fetch))
        _check(uid, self._id)
        return val


class Pin:
    def __init__(self, pin, uid=0):
        self._p = pin
        self._id = uid & 0xFF

    def _fetch(self):
        return bytes((self._id, 1 if self._p.value() else 0))

    def value(self, *args):
        if args:
            # an output: executed while recording, never while replaying
            return replay._act(self._p.value, *args)
        raw = replay._io(replay.GPIO, self._fetch)
        _need(raw, 2)
        _check(raw[0], self._id)
        return raw[1]


class UART:
    """read() returns at most 253 bytes per call (one log record).
    The log keeps the requested size; a replay with a different request fails."""

    def __init__(self, uart, uid=0):
        self._u = uart
        self._id = uid & 0xFF
        self._n = 253

    def _fetch(self):
        return bytes((self._id, self._n)) + (self._u.read(self._n) or b"")

    def read(self, n=253):
        self._n = _n(n, 253)
        raw = replay._io(replay.UART, self._fetch)
        _need(raw, 2)
        _check(raw[0], self._id)
        _check(raw[1], self._n, "UART read size")
        if len(raw) - 2 > self._n:
            raise RuntimeError("replay_io: logged data longer than the request")
        return raw[2:] or None


_I2C_H = "<BBBi"  # device id, address, size, memory address (-1 = plain read)


class I2C:
    """readfrom()/readfrom_mem() return at most 248 bytes per call.
    Address, memory address and size are logged and verified on replay."""

    def __init__(self, i2c, uid=0):
        self._i = i2c
        self._id = uid & 0xFF
        self._a = (0, 0, 0)

    def _fetch(self):
        addr, memaddr, n = self._a
        if memaddr < 0:
            data = self._i.readfrom(addr, n)
        else:
            data = self._i.readfrom_mem(addr, memaddr, n)
        return struct.pack(_I2C_H, self._id, addr, n, memaddr) + data

    def _read(self, addr, memaddr, n):
        n = _n(n, 248)
        self._a = (addr, memaddr, n)
        raw = replay._io(replay.I2C, self._fetch)
        _need(raw, 7)
        uid, a, rn, m = struct.unpack(_I2C_H, raw[:7])
        _check(uid, self._id)
        _check(a, addr, "I2C address")
        _check(rn, n, "I2C size")
        _check(m, memaddr, "I2C memory address")
        if len(raw) - 7 > n:
            raise RuntimeError("replay_io: logged data longer than the request")
        return raw[7:]

    def readfrom(self, addr, n):
        return self._read(addr, -1, n)

    def readfrom_mem(self, addr, memaddr, n):
        return self._read(addr, memaddr, n)


class SPI:
    """read() returns at most 252 bytes per call.
    Size and the written byte are logged and verified on replay."""

    def __init__(self, spi, uid=0):
        self._s = spi
        self._id = uid & 0xFF
        self._a = (1, 0)

    def _fetch(self):
        n, write = self._a
        return bytes((self._id, n, write & 0xFF)) + self._s.read(n, write)

    def read(self, n, write=0x00):
        n = _n(n, 252)
        self._a = (n, write)
        raw = replay._io(replay.SPI, self._fetch)
        _need(raw, 3)
        _check(raw[0], self._id)
        _check(raw[1], n, "SPI size")
        _check(raw[2], write & 0xFF, "SPI write byte")
        if len(raw) - 3 > n:
            raise RuntimeError("replay_io: logged data longer than the request")
        return raw[3:]


_rng_n = 1


def _rng_fetch():
    import os

    return os.urandom(_rng_n)


def urandom(n):
    """Recorded replacement for os.urandom (n <= 255)."""
    global _rng_n
    _rng_n = _n(n, 255)
    return replay._io(replay.RNG, _rng_fetch)


def seed_random():
    """Seed the `random` module from a recorded value. Call it right after
    replay.start(): the module is otherwise seeded from the hardware RNG at
    import time, which is neither recorded nor replayable."""
    import random

    random.seed(struct.unpack("<I", urandom(4))[0])


def output(fn, *args):
    """Run a hardware side effect (UART write, relay, PWM...) while recording,
    skip it while replaying:  replay_io.output(uart.write, b"hello")"""
    return replay._act(fn, *args)


def irq_handler(hid, arg=None):
    """Decorator for callbacks run by the scheduler (Pin.irq, Timer, ...).

        @replay_io.irq_handler(1, arg=None)
        def on_edge(pin): ...
        pin.irq(handler=on_edge, trigger=Pin.IRQ_RISING)

    While recording, the call is logged at the clock where it is delivered.
    While replaying, the engine calls fn(arg) at that same clock, so `arg` must
    be what the real source passes to the handler (the Pin or Timer object; it
    exists on the replay host too). Use it ONLY for hardware-triggered
    callbacks; a callback you schedule yourself is ordinary deterministic code.
    """

    def deco(fn):
        _handlers[hid] = (fn, arg)

        def wrapper(*a):
            replay._irq(hid)
            fn(*a)

        return wrapper

    return deco


def _dispatch(hid):
    fn, arg = _handlers[hid]
    fn(arg)


replay._set_dispatch(_dispatch)


def run(fn):
    """Run fn(); record (or verify, when replaying) the crash that ends it.

    Normal ending: replay.stop() runs and its errors (log truncated, recorded
    events not consumed) propagate to the caller.
    Crash: the original exception is always the one that reaches the caller,
    except in a replay whose crash does not match the recording, where the
    divergence is raised (the original is printed first). Failures of the
    logging layer itself are reported but never replace the real crash.
    """
    try:
        fn()
    except BaseException as e:
        try:
            matched = replay.crash(repr(e))
        except Exception as d:
            print("replay: could not log or verify the crash:", d)
            print("replay: the original exception was:", repr(e))
            if replay.mode() == replay.REPLAY:
                raise
            raise e
        if matched:
            print("replay: crash reproduced:", repr(e))
        try:
            replay.stop()
        except Exception as s:
            print("replay: stop failed:", s)
        raise
    replay.stop()
