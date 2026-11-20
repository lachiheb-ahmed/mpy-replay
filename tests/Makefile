CC ?= gcc
CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined
INC = -I../replay/core
SRC = ../replay/core/mrl_util.c ../replay/core/mrl_record.c ../replay/core/mrl_reader.c \
      ../replay/core/mrl_replay.c ../replay/ports/unix/replay_port_unix.c

test_core: test_core.c $(SRC)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

fuzz_reader: fuzz_reader.c $(SRC)
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined $(INC) -o $@ $^

.PHONY: test fuzz clean
fuzz: fuzz_reader
	./fuzz_reader
test: test_core
	./test_core

clean:
	rm -f test_core fuzz_reader
