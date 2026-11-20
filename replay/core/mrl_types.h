/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#ifndef MRL_TYPES_H
#define MRL_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MRL_MAGIC          0x314C524Du   /* bytes "MRL1" (little endian) */
#define MRL_VERSION        1
#define MRL_HEADER_SIZE    64
#define MRL_MAX_PAYLOAD    255
/* varint(10) + type(1) + len(1) + payload(255) + crc16(2) */
#define MRL_MAX_RECORD     (10 + 1 + 1 + MRL_MAX_PAYLOAD + 2)

typedef enum {
    MRL_EV_TICKS_MS   = 0x01, /* u64 value */
    MRL_EV_ADC        = 0x02, /* user-defined (see replay_io.py) */
    MRL_EV_GPIO       = 0x03,
    MRL_EV_UART_RX    = 0x04,
    MRL_EV_I2C_RD     = 0x05,
    MRL_EV_SPI_RD     = 0x06,
    MRL_EV_RNG        = 0x07,
    MRL_EV_IRQ_SCHED  = 0x08, /* u8 handler id */
    MRL_EV_EXC_ASYNC  = 0x09, /* u8 kind: 0 = KeyboardInterrupt */
    MRL_EV_CHECKPOINT = 0x0A, /* u8 kind, u32 value */
    MRL_EV_MARK       = 0x0B, /* text */
    MRL_EV_TICKS_US   = 0x0C, /* u64 value */
    MRL_EV_TICKS_CPU  = 0x0D, /* u64 value */
    MRL_EV_CRASH      = 0xFF, /* text */
} mrl_ev_type_t;

/* EXC_ASYNC payload[0] */
typedef enum {
    MRL_EXC_KBD   = 0, /* KeyboardInterrupt delivered at a VM branch */
    MRL_EXC_OTHER = 1,
    MRL_EXC_IN_IO = 2, /* KeyboardInterrupt that interrupted a recorded read */
} mrl_exc_kind_t;

typedef enum {
    MRL_CKPT_HEAP = 0, /* CRC32 of the GC heap (meaningful for bit-exact replay only) */
    MRL_CKPT_USER = 1, /* application-defined value */
} mrl_ckpt_kind_t;

typedef enum {
    MRL_OK = 0,
    MRL_ERR_ARG,
    MRL_ERR_STATE,
    MRL_ERR_STORAGE,
    MRL_ERR_FULL,
    MRL_ERR_BAD_HEADER,
    MRL_ERR_VERSION,
    MRL_ERR_END,      /* no more (valid) data */
    MRL_ERR_CORRUPT,
    MRL_ERR_SIZE,     /* destination buffer too small */
    MRL_ERR_DIVERGED, /* replay diverged from the recording */
} mrl_err_t;

typedef struct {
    uint16_t version;
    uint16_t flags;       /* reserved, 0 */
    uint8_t  fw_hash[20]; /* firmware git hash */
    uint32_t heap_size;   /* GC heap size in bytes */
    uint64_t rng_seed;    /* reserved, 0 in v0.1 */
    uint32_t board_id;
} mrl_header_t;

typedef struct {
    uint64_t lclk;        /* absolute logical clock */
    uint8_t  type;
    uint8_t  len;
    uint8_t  payload[MRL_MAX_PAYLOAD];
} mrl_event_t;

#endif /* MRL_TYPES_H */
