/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#include "mrl_internal.h"

mrl_err_t mrl_reader_open(mrl_reader_t *r, const uint8_t *data, size_t size) {
    if (data == NULL || size < MRL_HEADER_SIZE) {
        return MRL_ERR_BAD_HEADER;
    }
    mrl_err_t e = mrl_header_decode(data, &r->hdr, &r->seed);
    if (e != MRL_OK) {
        return e;
    }
    r->data = data;
    r->size = size;
    r->pos = MRL_HEADER_SIZE;
    r->lclk = 0;
    return MRL_OK;
}

mrl_err_t mrl_reader_next(mrl_reader_t *r, mrl_event_t *ev) {
    if (r->pos >= r->size) {
        return MRL_ERR_END;
    }
    uint64_t delta;
    size_t used;
    mrl_err_t e = mrl_record_decode(r->data + r->pos, r->size - r->pos, r->seed, &delta, ev, &used);
    if (e == MRL_ERR_CORRUPT) {
        return MRL_ERR_END; /* first invalid record = end of the valid log */
    }
    if (e != MRL_OK) {
        return e;
    }
    r->pos += used;
    r->lclk += delta;
    ev->lclk = r->lclk;
    return MRL_OK;
}
