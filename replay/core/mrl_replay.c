/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#include "mrl_internal.h"

static struct {
    mrl_reader_t     rd;
    mrl_event_t      next;
    bool             have;
    bool             strict_heap;
    mrl_divergence_t div;
    mrl_replay_stats_t st;
} rp;

static void advance(void) {
    rp.have = (mrl_reader_next(&rp.rd, &rp.next) == MRL_OK);
    rp.st.consumed++;
    rp.st.at_end = !rp.have;
}

static void set_div(mrl_div_reason_t why, uint8_t expected) {
    if (rp.div.reason != MRL_DIV_NONE) {
        return;
    }
    rp.div.reason = why;
    rp.div.lclk_now = mrl_lclk;
    rp.div.lclk_log = rp.have ? rp.next.lclk : 0;
    rp.div.type_expected = expected;
    rp.div.type_logged = rp.have ? rp.next.type : 0;
    rp.div.event_index = rp.st.consumed;
}

mrl_err_t mrl_replay_begin(const uint8_t *data, size_t size, bool strict_heap) {
    if (mrl_mode != MRL_MODE_OFF) {
        return MRL_ERR_STATE;
    }
    memset(&rp, 0, sizeof rp);
    mrl_err_t e = mrl_reader_open(&rp.rd, data, size);
    if (e != MRL_OK) {
        return e;
    }
    rp.strict_heap = strict_heap;
    advance();
    rp.st.consumed = 0;
    mrl_mode = MRL_MODE_REPLAY;
    return MRL_OK;
}

void mrl_replay_end(void) {
    if (mrl_mode == MRL_MODE_REPLAY) {
        mrl_mode = MRL_MODE_OFF;
    }
}

const mrl_divergence_t *mrl_replay_divergence(void) {
    return rp.div.reason != MRL_DIV_NONE ? &rp.div : NULL;
}

const mrl_header_t *mrl_replay_header(void) {
    return &rp.rd.hdr;
}

void mrl_replay_stats(mrl_replay_stats_t *s) {
    *s = rp.st;
}

/* Events delivered by the VM hook. An interrupt that hit a recorded read
 * (MRL_EXC_IN_IO) is NOT one of them: mrl_replay_take_io_exc() handles it. */
static bool is_async(const mrl_event_t *e) {
    if (e->type == MRL_EV_IRQ_SCHED) {
        return true;
    }
    return e->type == MRL_EV_EXC_ASYNC && !(e->len == 1 && e->payload[0] == MRL_EXC_IN_IO);
}

bool mrl_replay_take_io_exc(void) {
    if (mrl_mode != MRL_MODE_REPLAY || rp.div.reason != MRL_DIV_NONE || !rp.have) {
        return false;
    }
    if (rp.next.type == MRL_EV_EXC_ASYNC && rp.next.len == 1 && rp.next.payload[0] == MRL_EXC_IN_IO
        && rp.next.lclk == mrl_lclk) {
        advance();
        return true;
    }
    return false;
}

bool mrl_replay_poll(mrl_event_t *ev) {
    if (mrl_mode != MRL_MODE_REPLAY || rp.div.reason != MRL_DIV_NONE) {
        return false;
    }
    if (!rp.have || !is_async(&rp.next) || rp.next.lclk > mrl_lclk) {
        return false;
    }
    /* A callback recorded while a blocking C call was running is delivered at
     * the next VM branch; count it so the user can see it. */
    if (rp.next.lclk < mrl_lclk) {
        rp.st.late_async++;
    }
    *ev = rp.next;
    advance();
    return true;
}

/* Consume the next event, which must be `type` and sit at the current clock. */
static mrl_err_t pop_expect(uint8_t type, mrl_event_t *out) {
    if (rp.div.reason != MRL_DIV_NONE) {
        return MRL_ERR_DIVERGED;
    }
    if (!rp.have) {
        return MRL_ERR_END;
    }
    if (rp.next.type != type) {
        set_div(MRL_DIV_TYPE, type);
        return MRL_ERR_DIVERGED;
    }
    if (rp.next.lclk != mrl_lclk) {
        set_div(MRL_DIV_CLOCK, type);
        return MRL_ERR_DIVERGED;
    }
    *out = rp.next;
    advance();
    return MRL_OK;
}

mrl_err_t mrl_io(uint8_t type, void *buf, uint8_t *len, uint8_t cap) {
    if (len == NULL || (buf == NULL && (*len != 0 || mrl_mode == MRL_MODE_REPLAY))) {
        return MRL_ERR_ARG;
    }
    switch (mrl_mode) {
        case MRL_MODE_RECORD:
            return mrl_rec_event(type, buf, *len);
        case MRL_MODE_REPLAY: {
            mrl_event_t ev;
            mrl_err_t e = pop_expect(type, &ev);
            if (e != MRL_OK) {
                return e;
            }
            if (ev.len > cap) {
                return MRL_ERR_SIZE;
            }
            memcpy(buf, ev.payload, ev.len);
            *len = ev.len;
            return MRL_OK;
        }
        default:
            return MRL_OK;
    }
}

mrl_err_t mrl_checkpoint(uint8_t kind, uint32_t value) {
    uint8_t p[5];
    p[0] = kind;
    mrl_put32(p + 1, value);
    if (mrl_mode == MRL_MODE_RECORD) {
        return mrl_rec_event(MRL_EV_CHECKPOINT, p, sizeof p);
    }
    if (mrl_mode != MRL_MODE_REPLAY) {
        return MRL_OK;
    }
    mrl_event_t ev;
    mrl_err_t e = pop_expect(MRL_EV_CHECKPOINT, &ev);
    if (e != MRL_OK) {
        return e;
    }
    if (ev.len != 5 || ev.payload[0] != kind) {
        set_div(MRL_DIV_VALUE, MRL_EV_CHECKPOINT);
        return MRL_ERR_DIVERGED;
    }
    if (kind == MRL_CKPT_HEAP && !rp.strict_heap) {
        return MRL_OK; /* heap CRC is only comparable on a bit-exact target */
    }
    if (mrl_get32(ev.payload + 1) != value) {
        set_div(MRL_DIV_VALUE, MRL_EV_CHECKPOINT);
        return MRL_ERR_DIVERGED;
    }
    return MRL_OK;
}

mrl_err_t mrl_crash(const char *text, size_t len) {
    if (text == NULL && len) {
        return MRL_ERR_ARG;
    }
    if (len > MRL_MAX_PAYLOAD) {
        len = MRL_MAX_PAYLOAD;
    }
    if (mrl_mode == MRL_MODE_RECORD) {
        mrl_err_t e = mrl_rec_event(MRL_EV_CRASH, text, (uint8_t)len);
        mrl_err_t f = mrl_rec_flush();
        return e != MRL_OK ? e : f;
    }
    if (mrl_mode != MRL_MODE_REPLAY) {
        return MRL_OK;
    }
    mrl_event_t ev;
    mrl_err_t e = pop_expect(MRL_EV_CRASH, &ev);
    if (e != MRL_OK) {
        return e;
    }
    if (ev.len != len || (len && memcmp(ev.payload, text, len) != 0)) {
        set_div(MRL_DIV_VALUE, MRL_EV_CRASH);
        return MRL_ERR_DIVERGED;
    }
    return MRL_OK;
}

mrl_err_t mrl_mark(const char *text, size_t len) {
    if (text == NULL && len) {
        return MRL_ERR_ARG;
    }
    if (len > MRL_MAX_PAYLOAD) {
        len = MRL_MAX_PAYLOAD;
    }
    if (mrl_mode == MRL_MODE_RECORD) {
        return mrl_rec_event(MRL_EV_MARK, text, (uint8_t)len);
    }
    if (mrl_mode != MRL_MODE_REPLAY) {
        return MRL_OK;
    }
    mrl_event_t ev;
    mrl_err_t e = pop_expect(MRL_EV_MARK, &ev);
    if (e != MRL_OK) {
        return e;
    }
    if (ev.len != len || (len && memcmp(ev.payload, text, len) != 0)) {
        set_div(MRL_DIV_VALUE, MRL_EV_MARK);
        return MRL_ERR_DIVERGED;
    }
    return MRL_OK;
}

uint32_t mrl_replay_remaining(void) {
    if (mrl_mode != MRL_MODE_REPLAY || !rp.have) {
        return 0;
    }
    mrl_reader_t r = rp.rd;
    mrl_event_t e;
    uint32_t n = 1; /* rp.next */
    while (mrl_reader_next(&r, &e) == MRL_OK) {
        n++;
    }
    return n;
}
