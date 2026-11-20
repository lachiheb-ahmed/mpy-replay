/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#include "mrl_internal.h"
#include "replay_port.h"

_Static_assert(MRL_STAGE_SIZE >= MRL_MAX_RECORD, "MRL_STAGE_SIZE too small");

static struct {
    uint8_t  stage[MRL_STAGE_SIZE];
    size_t   fill;      /* bytes staged in RAM */
    size_t   off;       /* next write offset in storage */
    size_t   cap;       /* storage capacity */
    uint64_t last;      /* logical clock of the previous event */
    uint16_t seed;
    bool     overflow;
} rec;

mrl_err_t mrl_rec_begin(const mrl_header_t *h) {
    if (h == NULL) {
        return MRL_ERR_ARG;
    }
    if (mrl_mode != MRL_MODE_OFF) {
        return MRL_ERR_STATE;
    }
    size_t cap = 0;
    if (!replay_port_storage_init(&cap)) {
        return MRL_ERR_STORAGE;
    }
    if (cap < MRL_HEADER_SIZE + MRL_MAX_RECORD) {
        return MRL_ERR_FULL; /* checked before erasing: the previous log survives */
    }
    if (!replay_port_storage_reset()) {
        return MRL_ERR_STORAGE;
    }
    uint8_t hdr[MRL_HEADER_SIZE];
    mrl_header_encode(h, hdr);
    if (!replay_port_storage_write(0, hdr, sizeof hdr)) {
        return MRL_ERR_STORAGE;
    }
    mrl_header_t tmp;
    mrl_header_decode(hdr, &tmp, &rec.seed);
    rec.cap = cap;
    rec.off = MRL_HEADER_SIZE;
    rec.fill = 0;
    rec.last = 0;
    rec.overflow = false;
    mrl_mode = MRL_MODE_RECORD;
    return MRL_OK;
}

mrl_err_t mrl_rec_flush(void) {
    if (rec.fill == 0) {
        return MRL_OK;
    }
    size_t n = rec.fill;
    mrl_err_t result = MRL_OK;
    if (rec.off + n > rec.cap) {
        n = rec.cap - rec.off; /* write what fits; a torn record is detected by CRC */
        rec.overflow = true;
        result = MRL_ERR_FULL;
    }
    if (n && !replay_port_storage_write(rec.off, rec.stage, n)) {
        return MRL_ERR_STORAGE;
    }
    rec.off += n;
    rec.fill = 0;
    if (!replay_port_storage_flush()) {
        return MRL_ERR_STORAGE;
    }
    return result;
}

mrl_err_t mrl_rec_event(uint8_t type, const void *payload, uint8_t len) {
    if (mrl_mode != MRL_MODE_RECORD) {
        return MRL_ERR_STATE;
    }
    if (rec.overflow) {
        return MRL_ERR_FULL;
    }
    if (len && payload == NULL) {
        return MRL_ERR_ARG;
    }
    if (sizeof rec.stage - rec.fill < MRL_MAX_RECORD) {
        mrl_err_t e = mrl_rec_flush();
        if (e != MRL_OK) {
            return e;
        }
    }
    uint64_t delta = mrl_lclk - rec.last;
    rec.last = mrl_lclk;
    rec.fill += mrl_record_encode(rec.stage + rec.fill, rec.seed, delta, type, payload, len);
    #if MRL_FLUSH_EVERY_EVENT
    return mrl_rec_flush();
    #else
    return MRL_OK;
    #endif
}

mrl_err_t mrl_rec_end(void) {
    if (mrl_mode != MRL_MODE_RECORD) {
        return MRL_ERR_STATE;
    }
    mrl_err_t e = mrl_rec_flush();
    mrl_mode = MRL_MODE_OFF;
    return (e == MRL_OK && rec.overflow) ? MRL_ERR_FULL : e; /* tells the caller the log was truncated */
}

size_t mrl_rec_bytes(void) {
    return rec.off + rec.fill;
}

bool mrl_rec_overflowed(void) {
    return rec.overflow;
}

/* ---- inspect a log that is already in storage ---------------------------- */

mrl_err_t mrl_storage_capacity(size_t *cap) {
    if (cap == NULL) {
        return MRL_ERR_ARG;
    }
    return replay_port_storage_init(cap) ? MRL_OK : MRL_ERR_STORAGE;
}

mrl_err_t mrl_storage_read(size_t off, void *dst, size_t n) {
    if (dst == NULL && n) {
        return MRL_ERR_ARG;
    }
    size_t cap = 0;
    if (!replay_port_storage_init(&cap)) {
        return MRL_ERR_STORAGE;
    }
    if (off > cap || n > cap - off) {
        return MRL_ERR_ARG;
    }
    return replay_port_storage_read(off, dst, n) ? MRL_OK : MRL_ERR_STORAGE;
}

mrl_err_t mrl_storage_scan(mrl_scan_info_t *info) {
    size_t cap = 0;
    if (info == NULL) {
        return MRL_ERR_ARG;
    }
    memset(info, 0, sizeof *info);
    if (!replay_port_storage_init(&cap)) {
        return MRL_ERR_STORAGE;
    }
    uint8_t hdr[MRL_HEADER_SIZE];
    uint16_t seed;
    if (cap < MRL_HEADER_SIZE || !replay_port_storage_read(0, hdr, sizeof hdr)) {
        return MRL_ERR_STORAGE;
    }
    mrl_err_t e = mrl_header_decode(hdr, &info->hdr, &seed);
    if (e != MRL_OK) {
        return e;
    }
    size_t pos = MRL_HEADER_SIZE;
    uint64_t lclk = 0;
    uint8_t buf[MRL_MAX_RECORD];
    mrl_event_t ev;
    while (pos < cap) {
        size_t chunk = cap - pos;
        if (chunk > sizeof buf) {
            chunk = sizeof buf;
        }
        if (!replay_port_storage_read(pos, buf, chunk)) {
            return MRL_ERR_STORAGE;
        }
        uint64_t delta;
        size_t used;
        if (mrl_record_decode(buf, chunk, seed, &delta, &ev, &used) != MRL_OK) {
            break;
        }
        lclk += delta;
        pos += used;
        info->events++;
        if (ev.type == MRL_EV_CRASH) {
            info->has_crash = true;
            info->crash_len = ev.len < sizeof info->crash ? ev.len : (uint8_t)sizeof info->crash;
            memcpy(info->crash, ev.payload, info->crash_len);
        }
    }
    info->valid_bytes = pos;
    info->last_lclk = lclk;
    return MRL_OK;
}
