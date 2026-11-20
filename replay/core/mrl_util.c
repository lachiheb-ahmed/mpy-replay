/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#include "mrl_internal.h"

uint64_t mrl_lclk;
uint8_t  mrl_mode;

const char *mrl_strerror(mrl_err_t e) {
    switch (e) {
        case MRL_OK:           return "ok";
        case MRL_ERR_ARG:      return "bad argument";
        case MRL_ERR_STATE:    return "wrong state";
        case MRL_ERR_STORAGE:  return "storage error";
        case MRL_ERR_FULL:     return "log storage full";
        case MRL_ERR_BAD_HEADER: return "bad log header";
        case MRL_ERR_VERSION:  return "unsupported log version";
        case MRL_ERR_END:      return "end of log";
        case MRL_ERR_CORRUPT:  return "corrupt record";
        case MRL_ERR_SIZE:     return "buffer too small";
        case MRL_ERR_DIVERGED: return "replay diverged";
    }
    return "unknown error";
}

/* ---- CRC32 (IEEE 802.3, nibble table) ------------------------------------ */
static const uint32_t crc32_tab[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
};

uint32_t mrl_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ crc32_tab[crc & 15];
        crc = (crc >> 4) ^ crc32_tab[crc & 15];
    }
    return ~crc;
}

/* ---- CRC16 CCITT-FALSE (poly 0x1021) ------------------------------------- */
uint16_t mrl_crc16(uint16_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    while (len--) {
        crc ^= (uint16_t)((uint16_t)(*p++) << 8);
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* ---- header --------------------------------------------------------------- */
void mrl_header_encode(const mrl_header_t *h, uint8_t out[MRL_HEADER_SIZE]) {
    memset(out, 0, MRL_HEADER_SIZE);
    mrl_put32(out + 0, MRL_MAGIC);
    mrl_put16(out + 4, MRL_VERSION);
    mrl_put16(out + 6, h->flags);
    memcpy(out + 8, h->fw_hash, 20);
    mrl_put32(out + 28, h->heap_size);
    mrl_put64(out + 32, h->rng_seed);
    mrl_put32(out + 40, h->board_id);
    /* 44..59 reserved (zero) */
    mrl_put32(out + 60, mrl_crc32(0, out, 60));
}

mrl_err_t mrl_header_decode(const uint8_t in[MRL_HEADER_SIZE], mrl_header_t *h, uint16_t *seed) {
    if (mrl_get32(in) != MRL_MAGIC) {
        return MRL_ERR_BAD_HEADER;
    }
    uint32_t crc = mrl_get32(in + 60);
    if (crc != mrl_crc32(0, in, 60)) {
        return MRL_ERR_BAD_HEADER;
    }
    h->version = mrl_get16(in + 4);
    if (h->version != MRL_VERSION) {
        return MRL_ERR_VERSION;
    }
    h->flags = mrl_get16(in + 6);
    memcpy(h->fw_hash, in + 8, 20);
    h->heap_size = mrl_get32(in + 28);
    h->rng_seed = mrl_get64(in + 32);
    h->board_id = mrl_get32(in + 40);
    /* Records are CRC-seeded with the header CRC so that stale records left
     * over from an older, longer log can never validate. */
    *seed = (uint16_t)(crc & 0xFFFF);
    return MRL_OK;
}

/* ---- varint (LEB128) ------------------------------------------------------ */
size_t mrl_varint_put(uint8_t *out, uint64_t v) {
    size_t n = 0;
    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

mrl_err_t mrl_varint_get(const uint8_t *in, size_t avail, uint64_t *v, size_t *used) {
    uint64_t r = 0;
    for (size_t i = 0; i < 10; i++) {
        if (i >= avail) {
            return MRL_ERR_END;
        }
        uint8_t b = in[i];
        if (i == 9 && b > 1) {
            return MRL_ERR_CORRUPT;
        }
        r |= (uint64_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) {
            *v = r;
            *used = i + 1;
            return MRL_OK;
        }
    }
    return MRL_ERR_CORRUPT;
}

/* ---- record: varint(delta) type len payload crc16 ------------------------- */
size_t mrl_record_encode(uint8_t *out, uint16_t seed, uint64_t delta,
                         uint8_t type, const void *payload, uint8_t len) {
    size_t n = mrl_varint_put(out, delta);
    out[n++] = type;
    out[n++] = len;
    if (len) {
        memcpy(out + n, payload, len);
        n += len;
    }
    mrl_put16(out + n, mrl_crc16(seed, out, n));
    return n + 2;
}

mrl_err_t mrl_record_decode(const uint8_t *in, size_t avail, uint16_t seed,
                            uint64_t *delta, mrl_event_t *ev, size_t *consumed) {
    size_t n;
    mrl_err_t e = mrl_varint_get(in, avail, delta, &n);
    if (e != MRL_OK) {
        return e;
    }
    if (avail < n + 2) {
        return MRL_ERR_END;
    }
    uint8_t type = in[n];
    uint8_t len = in[n + 1];
    if (type == 0) {
        return MRL_ERR_CORRUPT; /* zero-filled memory is never a record */
    }
    size_t total = n + 2 + (size_t)len + 2;
    if (avail < total) {
        return MRL_ERR_END;
    }
    if (mrl_get16(in + total - 2) != mrl_crc16(seed, in, total - 2)) {
        return MRL_ERR_CORRUPT;
    }
    ev->type = type;
    ev->len = len;
    memcpy(ev->payload, in + n + 2, len);
    *consumed = total;
    return MRL_OK;
}
