# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
CC ?= gcc
CORE = replay/core
SRC  = $(CORE)/mrl_util.c $(CORE)/mrl_record.c $(CORE)/mrl_reader.c $(CORE)/mrl_replay.c replay/ports/unix/replay_port_unix.c

all: tools/mrl_dump

tools/mrl_dump: tools/mrl_dump.c $(SRC)
	$(CC) -std=c99 -Wall -Wextra -O2 -I$(CORE) -o $@ $^

test:
	$(MAKE) -C tests test

fuzz:
	$(MAKE) -C tests fuzz

.PHONY: all test fuzz clean
clean:
	rm -f tools/mrl_dump
	$(MAKE) -C tests clean
