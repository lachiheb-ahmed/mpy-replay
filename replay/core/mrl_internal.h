/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#ifndef MRL_INTERNAL_H
#define MRL_INTERNAL_H

#include <string.h>
#include "mrl_core.h"

static inline void mrl_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void mrl_put32(uint8_t *p, uint32_t v) { mrl_put16(p, (uint16_t)v); mrl_put16(p + 2, (uint16_t)(v >> 16)); }
static inline void mrl_put64(uint8_t *p, uint64_t v) { mrl_put32(p, (uint32_t)v); mrl_put32(p + 4, (uint32_t)(v >> 32)); }
static inline uint16_t mrl_get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t mrl_get32(const uint8_t *p) { return (uint32_t)mrl_get16(p) | ((uint32_t)mrl_get16(p + 2) << 16); }
static inline uint64_t mrl_get64(const uint8_t *p) { return (uint64_t)mrl_get32(p) | ((uint64_t)mrl_get32(p + 4) << 32); }

size_t    mrl_varint_put(uint8_t *out, uint64_t v);
mrl_err_t mrl_varint_get(const uint8_t *in, size_t avail, uint64_t *v, size_t *used);

size_t    mrl_record_encode(uint8_t *out, uint16_t seed, uint64_t delta,
                            uint8_t type, const void *payload, uint8_t len);
mrl_err_t mrl_record_decode(const uint8_t *in, size_t avail, uint16_t seed,
                            uint64_t *delta, mrl_event_t *ev, size_t *consumed);

#endif /* MRL_INTERNAL_H */
