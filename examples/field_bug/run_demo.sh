#!/bin/sh
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Record until the parser bug shows up (it is random), then replay it as often as you like.
# usage: run_demo.sh <record-micropython> <replay-micropython>
REC=$1; RPL=$2
cd "$(dirname "$0")"
export MICROPYPATH=../../usermod:.
for i in $(seq 40); do
    rm -f bug.mrl
    MRL_LOG=bug.mrl $REC app.py > rec.out 2>&1
    if grep -q "PARSER CRASH" rec.out; then break; fi
    echo "record attempt $i: no crash this time ($(grep -c . rec.out) lines)"
done
grep -q "PARSER CRASH" rec.out || { echo "no crash in 40 attempts"; exit 1; }
echo "== recorded run:";  grep "PARSER CRASH" rec.out
echo "== replay x3 (must be identical):"
for k in 1 2 3; do MRL_REPLAY_LOG=bug.mrl $RPL app.py 2>&1 | grep "PARSER CRASH"; done
