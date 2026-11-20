/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * Port adapter for hosted systems: the log lives in a file.
 *   MRL_LOG       path of the log file   (default: mpy-replay.mrl)
 *   MRL_CAPACITY  max log size in bytes  (default: 16 MiB)
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "replay_port.h"

static int fd = -1;
static size_t capacity = 16u * 1024 * 1024;

bool replay_port_storage_init(size_t *cap) {
    if (fd < 0) {
        const char *path = getenv("MRL_LOG");
        fd = open(path ? path : "mpy-replay.mrl", O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            return false;
        }
    }
    const char *c = getenv("MRL_CAPACITY");
    capacity = c ? (size_t)strtoul(c, NULL, 0) : 16u * 1024 * 1024;
    *cap = capacity;
    return true;
}

bool replay_port_storage_reset(void) {
    return fd >= 0 && ftruncate(fd, 0) == 0;
}

bool replay_port_storage_write(size_t off, const void *src, size_t n) {
    const uint8_t *p = (const uint8_t *)src;
    while (n) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);
        if (w <= 0) {
            return false;
        }
        p += w;
        off += (size_t)w;
        n -= (size_t)w;
    }
    return true;
}

bool replay_port_storage_read(size_t off, void *dst, size_t n) {
    uint8_t *p = (uint8_t *)dst;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            memset(p, 0, n); /* past the end of the file: zeros */
            break;
        }
        p += r;
        off += (size_t)r;
        n -= (size_t)r;
    }
    return true;
}

bool replay_port_storage_flush(void) {
    return fd >= 0 && fsync(fd) == 0;
}

uint32_t replay_port_board_id(void) { return 0x554E4958u; /* "UNIX" */ }
uint32_t replay_port_heap_size(void) { return 0; }
