#!/bin/sh
# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# Build-time checks that need no compilation: a malformed MPY_REPLAY_FW_HASH must stop the build.
# usage: build_checks.sh /path/to/micropython
MP=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FAIL=0
try() { # name, hash, expect-error(1/0)
    out=$(make -n -C "$MP/ports/unix" USER_C_MODULES="$ROOT" MPY_REPLAY_PORT=unix MPY_REPLAY_FW_HASH="$2" BUILD=/tmp/mrl_bc 2>&1)
    case "$out" in *"must be exactly 40 hexadecimal"*) got=1 ;; *) got=0 ;; esac
    if [ "$got" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1"; FAIL=1; fi
}
try "valid 40-hex hash is accepted"          0123456789abcdef0123456789abcdef01234567 0
try "uppercase hex is accepted"              0123456789ABCDEF0123456789ABCDEF01234567 0
try "39 characters is rejected"              0123456789abcdef0123456789abcdef0123456 1
try "41 characters is rejected"              0123456789abcdef0123456789abcdef012345678 1
try "40 non-hex characters are rejected"     zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz 1
try "one bad character is rejected"          0123456789abcdef0123456789abcdef0123456g 1
exit $FAIL
