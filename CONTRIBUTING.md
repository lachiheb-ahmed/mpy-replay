# Contributing to mpy-replay

Bug reports and pull requests are welcome. This is an alpha project: the log format and APIs may still change.

## Reporting a problem

Please include: MicroPython commit, port and board, how you built (`MPY_REPLAY_*` options), a minimal program, and the output of `tools/mrl_dump -q` on the log if you have one. **Do not attach `.mrl` logs from real devices to public issues**: they contain raw inputs (sensor data, serial traffic) that may be sensitive.

## Before you open a pull request

```bash
make test                               # core unit tests with sanitizers
make fuzz                               # mutation fuzzing of the log parser
sh tests/build_checks.sh /path/to/micropython
cd tests/e2e && ./run_e2e.sh <record-binary> <replay-binary> app.py
./run_regress.sh <record-binary> <replay-binary>     # binaries built with MPY_REPLAY_TEST=1
```

A fix for a bug should come with a test that fails without it. The rules that keep a replay deterministic are in README section 7; please read them before changing the hooks or the proxies.

## Style

C99, 4 spaces, no tabs. Keep the core free of MicroPython headers. Every source file keeps its SPDX header and copyright line.

## Licensing

By contributing you agree that your contribution is released under the MIT license of this project.
