/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * Port adapter interface. Implemented once per port (see replay/ports/).
 */
#ifndef REPLAY_PORT_H
#define REPLAY_PORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Prepare the storage and report its capacity in bytes.
 * MUST preserve existing contents (a previous log may still be there). */
bool replay_port_storage_init(size_t *capacity);

/* Erase the whole storage so a new recording can start. */
bool replay_port_storage_reset(void);

bool replay_port_storage_write(size_t off, const void *src, size_t n);

/* Reads past the end of valid data may return zeros but must not fail. */
bool replay_port_storage_read(size_t off, void *dst, size_t n);

bool replay_port_storage_flush(void);

uint32_t replay_port_board_id(void);
uint32_t replay_port_heap_size(void);

#endif /* REPLAY_PORT_H */
