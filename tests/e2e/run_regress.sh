#!/bin/sh
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Regression tests for the external review findings (+ IRQ end-to-end).
# usage: run_regress.sh <record-micropython> <replay-micropython>   (both built with MPY_REPLAY_TEST=1)
REC=$1; RPL=$2
HERE=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d); cd "$T"
export MICROPYPATH="$HERE/../../usermod"
FAIL=0
ok()   { echo "PASS  $1"; }
bad()  { echo "FAIL  $1"; FAIL=1; }
want() { # name, expected-substring, actual
    case "$3" in *"$2"*) ok "$1" ;; *) bad "$1 (got: $3)" ;; esac
}

cat > mk.py <<'PY'
import sys, replay
replay.start()
tag = sys.argv[1]; n = int(sys.argv[2])
for i in range(n):
    replay.mark(tag)
    replay.checkpoint(i)
replay.stop(len(sys.argv) > 3)
print("DONE")
PY
MRL_LOG=m.mrl $REC mk.py a 3 >/dev/null
want "mark: different text is a divergence"  "diverged"      "$(MRL_REPLAY_LOG=m.mrl $RPL mk.py b 3 2>&1 | tail -1)"
want "mark: same text replays"               "DONE"          "$(MRL_REPLAY_LOG=m.mrl $RPL mk.py a 3 2>&1 | tail -1)"
want "stop: unconsumed events are reported"  "not consumed"   "$(MRL_REPLAY_LOG=m.mrl $RPL mk.py a 1 2>&1 | tail -1)"
want "stop(True): partial replay allowed"    "DONE"          "$(MRL_REPLAY_LOG=m.mrl $RPL mk.py a 1 partial 2>&1 | tail -1)"

for args in "0, -1" "-1, 4" "0, 99999999" "5000000000, 1"; do
    r=$(MRL_LOG=m.mrl $REC -c "
import replay
try: replay.read($args); print('NO ERROR')
except ValueError: print('ValueError')
except Exception as e: print(type(e).__name__)" 2>&1 | tail -1)
    want "read($args) rejected cleanly" "ValueError" "$r"
done
want "read(0, 8) works" "8" "$(MRL_LOG=m.mrl $REC -c "import replay; print(len(replay.read(0, 8)))" 2>&1 | tail -1)"

mkdir shim; cat > shim/machine.py <<'PY'
class Pin:
    writes = 0
    def __init__(self, *a): pass
    def value(self, *a):
        if a:
            Pin.writes += 1
            return None
        return 0
PY
cat > gp.py <<'PY'
import replay, replay_io, machine
replay.start()
p = replay_io.Pin(machine.Pin("X1"), uid=1)
for i in range(3):
    p.value(1)
    p.value()
print("WRITES", machine.Pin.writes)
replay.stop()
PY
w1=$(MICROPYPATH=$T/shim:$MICROPYPATH MRL_LOG=g.mrl $REC gp.py | tail -1)
w2=$(MICROPYPATH=$T/shim:$MICROPYPATH MRL_REPLAY_LOG=g.mrl $RPL gp.py 2>&1 | tail -1)
want "gpio: outputs executed while recording" "WRITES 3" "$w1"
want "gpio: outputs NOT executed while replaying" "WRITES 0" "$w2"

# run(): an unconsumed-events error must reach the caller on a normal ending
cat > rn.py <<'PY'
import sys, replay, replay_io
replay.start()
k = int(sys.argv[1])
def main():
    for i in range(k):
        replay.mark("m")
replay_io.run(main)
PY
MRL_LOG=r.mrl $REC rn.py 5 >/dev/null 2>&1
MRL_REPLAY_LOG=r.mrl $RPL rn.py 5 >/dev/null 2>&1; rc=$?
[ $rc -eq 0 ] && ok "run(): full replay ends cleanly" || bad "run(): full replay ends cleanly (exit $rc)"
out=$(MRL_REPLAY_LOG=r.mrl $RPL rn.py 2 2>&1); rc=$?
[ $rc -ne 0 ] && want "run(): partial replay is an error, not a warning" "not consumed" "$out" || bad "run(): partial replay exited 0"


# ---- a full log must not kill the application, and must not hide its real crash ----
cat > full.py <<'PY'
import replay, replay_io
replay.start()
def main():
    for i in range(200):
        replay.mark("event %d" % i)
    print("APP FINISHED ITS WORK")
    raise ValueError("the real bug")
replay_io.run(main)
PY
out=$(MRL_CAPACITY=400 MRL_LOG=f.mrl $REC full.py 2>&1)
want "full log: the application keeps running" "APP FINISHED ITS WORK" "$out"
want "full log: the original exception is the one reported" "ValueError: the real bug" "$out"

# ---- I2C / SPI / UART: the whole request is logged and verified ----
cat > par.py <<'PY'
import sys, replay, replay_io
class FI2C:
    def readfrom(self, a, n): return bytes(range(n))
    def readfrom_mem(self, a, m, n): return bytes([m & 255]) * n
class FSPI:
    def read(self, n, w): return bytes([w]) * n
class FUART:
    def read(self, n): return b"abcdefgh"[:n]
replay.start()
what = sys.argv[1]; n = int(sys.argv[2])
i2c = replay_io.I2C(FI2C(), 1); spi = replay_io.SPI(FSPI(), 2); u = replay_io.UART(FUART(), 3)
if what == "i2c": r = i2c.readfrom(0x50, n)
elif what == "i2cm": r = i2c.readfrom_mem(0x50, n, 4)
elif what == "i2ca": r = i2c.readfrom(n, 4)
elif what == "spi": r = spi.read(n)
elif what == "spiw": r = spi.read(4, n)
else: r = u.read(n)
print("OK", len(r))
replay.stop()
PY
for w in i2c i2cm spi spiw uart; do
    MRL_LOG=p.mrl $REC par.py $w 4 >/dev/null 2>&1
    want "$w: identical request replays"   "OK"       "$(MRL_REPLAY_LOG=p.mrl $RPL par.py $w 4 2>&1 | tail -1)"
    want "$w: a different request is detected" "mismatch" "$(MRL_REPLAY_LOG=p.mrl $RPL par.py $w 5 2>&1 | tail -1)"
done
MRL_LOG=p.mrl $REC par.py i2ca 80 >/dev/null 2>&1
want "i2c: a different device address is detected" "I2C address mismatch" "$(MRL_REPLAY_LOG=p.mrl $RPL par.py i2ca 81 2>&1 | tail -1)"

# ---- run(): the logging layer must not hide the original exception ----
cat > cr.py <<'PY'
import sys, replay, replay_io
replay.start()
def main():
    raise ValueError(sys.argv[1])
replay_io.run(main)
PY
MRL_LOG=c.mrl $REC cr.py first >/dev/null 2>&1
out=$(MRL_REPLAY_LOG=c.mrl $RPL cr.py second 2>&1)
want "replay: a different crash is reported as a divergence" "diverged" "$out"
want "replay: the original exception is printed too"          "the original exception was: ValueError('second'" "$out"

# ---- replay can be started again after stop() (the log image is released and reloaded) ----
cat > again.py <<'PY'
import replay
for k in range(3):
    replay.start()
    replay.mark("x")
    replay.stop()
print("AGAIN OK")
PY
MRL_LOG=a.mrl $REC -c "
import replay
replay.start(); replay.mark('x'); replay.stop()" >/dev/null 2>&1
want "replay can be restarted three times" "AGAIN OK" "$(MRL_REPLAY_LOG=a.mrl $RPL again.py 2>&1 | tail -1)"

# ---- the random module: unseeded it breaks replay, seeded through replay_io it does not ----
cat > rnd.py <<'PY'
import sys, random, replay, replay_io
replay.start()
if len(sys.argv) > 1:
    replay_io.seed_random()
n = random.getrandbits(8) + 10
for i in range(n):
    replay.mark("tick")
print("RESULT n=%d" % n)
replay.stop()
PY
MRL_LOG=rn1.mrl $REC rnd.py seeded > rn1.out 2>&1
MRL_REPLAY_LOG=rn1.mrl $RPL rnd.py seeded > rn1.rpl 2>&1
[ "$(grep RESULT rn1.out)" = "$(grep RESULT rn1.rpl)" ] && [ -n "$(grep RESULT rn1.out)" ] && ok "random: seeded through replay_io.seed_random() replays" || bad "random: seeded replay differs ($(cat rn1.rpl | tail -1))"

# IRQ end to end: three SIGUSR1 "hardware interrupts", then Ctrl+C
for i in 1 2 3; do
    rm -f irq.mrl
    MRL_LOG=irq.mrl $REC "$HERE/app_irq.py" > irq.rec 2>&1 &
    PID=$!
    sleep 0.3; kill -USR1 $PID
    sleep 0.2; kill -USR1 $PID
    sleep 0.17; kill -USR1 $PID
    sleep 0.3; kill -INT $PID
    wait $PID 2>/dev/null
    a=$(grep '^RESULT' irq.rec)
    MRL_REPLAY_LOG=irq.mrl $RPL "$HERE/app_irq.py" > irq.rpl 2>&1
    b=$(grep '^RESULT' irq.rpl)
    if [ -n "$a" ] && [ "$a" = "$b" ] && grep -q "count=3" irq.rec && grep -q "crash reproduced" irq.rpl; then
        ok "irq e2e run $i: $a"
    else
        bad "irq e2e run $i (record: $a | replay: $b)"; cat irq.rpl | tail -3
    fi
done
rm -rf "$T"
exit $FAIL
