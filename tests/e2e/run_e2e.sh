#!/bin/sh
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Record on one binary, replay on another, compare the results.
# usage: run_e2e.sh <record-micropython> <replay-micropython> [app.py|app_io.py]
set -e
REC=$1; RPL=$2
HERE=$(cd "$(dirname "$0")" && pwd)
APP=${3:-app.py}
export MICROPYPATH="$HERE/../../usermod"
LOG=/tmp/e2e.mrl; rm -f $LOG

MRL_LOG=$LOG $REC "$HERE/$APP" > /tmp/rec.out 2>&1 &
PID=$!
sleep 1.3
kill -INT $PID
wait $PID || true
REC_RESULT=$(grep '^RESULT' /tmp/rec.out)
echo "record : $REC_RESULT"

MRL_REPLAY_LOG=$LOG $RPL "$HERE/$APP" > /tmp/rpl.out 2>&1 || true
RPL_RESULT=$(grep '^RESULT' /tmp/rpl.out || true)
echo "replay : $RPL_RESULT"
grep -i "reproduced\|diverged\|Error" /tmp/rpl.out || true

if [ -n "$REC_RESULT" ] && [ "$REC_RESULT" = "$RPL_RESULT" ]; then
    echo "E2E OK: replay reproduced the recording"
else
    echo "E2E FAILED"; echo "--- record output"; cat /tmp/rec.out; echo "--- replay output"; cat /tmp/rpl.out; exit 1
fi
