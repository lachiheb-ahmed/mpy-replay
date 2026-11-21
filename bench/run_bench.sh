#!/bin/sh
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# usage: run_bench.sh <base-micropython> <module-micropython> <replay-micropython> [passes]
# Prints the minimum over all passes for each mode and benchmark (microseconds).
BASE=$1; MOD=$2; RPL=$3; N=${4:-6}
cd "$(dirname "$0")"
export MICROPYPATH=../usermod
rm -f /tmp/bench.mrl
MRL_LOG=/tmp/bench.mrl $MOD bench.py rec >/dev/null 2>&1       # log consumed by the replay runs
for pass in $(seq $N); do
    echo "base   $($BASE bench.py)"
    echo "off    $($MOD bench.py)"
    echo "record $(MRL_LOG=/tmp/bench_run.mrl $MOD bench.py rec)"
    echo "replay $(MRL_REPLAY_LOG=/tmp/bench.mrl $RPL bench.py rec 2>&1 | tail -1)"
done | python3 -c "
import sys
best={}
for l in sys.stdin:
    p=l.split()
    if len(p)!=7 or p[1]!='loop': print('unparsed:',l.strip()); continue
    m=p[0]; v=[int(p[2]),int(p[4]),int(p[6])]
    best[m]=[min(a,b) for a,b in zip(best[m],v)] if m in best else v
base=best['base']
print('%-8s %9s %9s %9s     overhead vs base' % ('mode','loop','fib(22)','mix'))
for m in ('base','off','record','replay'):
    b=best[m]; o=['%+.0f%%' % (100.0*(x-y)/y) for x,y in zip(b,base)]
    print('%-8s %9d %9d %9d     %s' % (m,b[0],b[1],b[2],' '.join('%6s'%x for x in o)))
"
