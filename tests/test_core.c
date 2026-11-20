#define _POSIX_C_SOURCE 200809L
/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mrl_internal.h"
#include "replay_port.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    CHECK(f);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    CHECK(fread(b, 1, (size_t)sz, f) == (size_t)sz);
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void test_crc_vectors(void) {
    CHECK(mrl_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(mrl_crc16(0xFFFF, "123456789", 9) == 0x29B1);
    /* incremental CRC32 equals one-shot */
    uint32_t c = mrl_crc32(0, "1234", 4);
    CHECK(mrl_crc32(c, "56789", 5) == 0xCBF43926u);
}

static void test_varint(void) {
    uint64_t vals[] = {0, 1, 127, 128, 300, 0xFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull};
    for (size_t i = 0; i < sizeof vals / sizeof *vals; i++) {
        uint8_t b[10]; uint64_t out; size_t used;
        size_t n = mrl_varint_put(b, vals[i]);
        CHECK(mrl_varint_get(b, n, &out, &used) == MRL_OK && out == vals[i] && used == n);
        if (n > 1) CHECK(mrl_varint_get(b, n - 1, &out, &used) == MRL_ERR_END);
    }
}

static void test_header(void) {
    mrl_header_t h = {.flags = 0, .heap_size = 98304, .rng_seed = 7, .board_id = 0xAABBCCDD};
    for (int i = 0; i < 20; i++) h.fw_hash[i] = (uint8_t)i;
    uint8_t b[MRL_HEADER_SIZE]; mrl_header_t o; uint16_t seed;
    mrl_header_encode(&h, b);
    CHECK(memcmp(b, "MRL1", 4) == 0);
    CHECK(mrl_header_decode(b, &o, &seed) == MRL_OK);
    CHECK(o.heap_size == 98304 && o.rng_seed == 7 && o.board_id == 0xAABBCCDD && o.fw_hash[19] == 19);
    b[10] ^= 1;
    CHECK(mrl_header_decode(b, &o, &seed) == MRL_ERR_BAD_HEADER);
}

static const char *LOG = "/tmp/mrl_test.mrl";

/* A tiny "program": reads 3 ticks, a uart chunk, one async IRQ at lclk 50, marks, checkpoints. */
static uint32_t prog_ticks(void) {
    uint8_t p[4]; uint8_t len = 4;
    mrl_put32(p, (uint32_t)(1000 + mrl_lclk * 3)); /* "hardware" value */
    CHECK(mrl_io(MRL_EV_TICKS_MS, p, &len, sizeof p) == MRL_OK);
    return mrl_get32(p);
}

static void test_record_replay(void) {
    setenv("MRL_LOG", LOG, 1);
    mrl_header_t h = {.heap_size = 4096, .board_id = 1};
    mrl_lclk = 0; mrl_mode = MRL_MODE_OFF;
    CHECK(mrl_rec_begin(&h) == MRL_OK);

    uint32_t rec_ticks[3];
    for (int i = 0; i < 3; i++) {
        mrl_lclk += 10; rec_ticks[i] = prog_ticks();
    }
    uint8_t u[] = {1, 'h', 'e', 'l', 'l', 'o'}; uint8_t ul = sizeof u;
    CHECK(mrl_io(MRL_EV_UART_RX, u, &ul, sizeof u) == MRL_OK);
    mrl_lclk = 50;
    uint8_t hid = 3;
    CHECK(mrl_rec_event(MRL_EV_IRQ_SCHED, &hid, 1) == MRL_OK);
    mrl_lclk = 60;
    CHECK(mrl_io(MRL_EV_MARK, "phase2", &(uint8_t){6}, 6) == MRL_OK);
    CHECK(mrl_checkpoint(MRL_CKPT_USER, 0xDEADBEEF) == MRL_OK);
    CHECK(mrl_checkpoint(MRL_CKPT_HEAP, 0x11111111) == MRL_OK);
    mrl_lclk = 70;
    CHECK(mrl_crash("ValueError: boom", 16) == MRL_OK);
    CHECK(mrl_rec_end() == MRL_OK);

    /* the log is inspectable straight from storage (flight recorder) */
    mrl_scan_info_t si;
    CHECK(mrl_storage_scan(&si) == MRL_OK);
    CHECK(si.events == 9 && si.has_crash && si.crash_len == 16 && memcmp(si.crash, "ValueError: boom", 16) == 0);
    CHECK(si.last_lclk == 70);

    /* ---- faithful replay ---- */
    size_t n; uint8_t *img = slurp(LOG, &n);
    CHECK(n == si.valid_bytes);
    mrl_lclk = 0;
    CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    CHECK(mrl_replay_header()->board_id == 1 && mrl_replay_header()->heap_size == 4096);
    for (int i = 0; i < 3; i++) {
        mrl_lclk += 10;
        CHECK(prog_ticks() != 0); /* value comes from the log, not the formula */
    }
    /* replaying must return the recorded values even if "hardware" differs */
    mrl_replay_end();
    mrl_lclk = 0;
    CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    for (int i = 0; i < 3; i++) {
        mrl_lclk += 10;
        uint8_t p[4] = {0xFF, 0xFF, 0xFF, 0xFF}, l = 4;
        CHECK(mrl_io(MRL_EV_TICKS_MS, p, &l, sizeof p) == MRL_OK);
        CHECK(mrl_get32(p) == rec_ticks[i]);
    }
    uint8_t rb[16], rl = 0;
    CHECK(mrl_io(MRL_EV_UART_RX, rb, &rl, sizeof rb) == MRL_OK);
    CHECK(rl == 6 && memcmp(rb + 1, "hello", 5) == 0);
    mrl_event_t ev;
    mrl_lclk = 49; CHECK(!mrl_replay_poll(&ev));            /* not due yet */
    mrl_lclk = 50; CHECK(mrl_replay_poll(&ev) && ev.type == MRL_EV_IRQ_SCHED && ev.payload[0] == 3);
    CHECK(!mrl_replay_poll(&ev));
    mrl_lclk = 60;
    rl = 6; memcpy(rb, "XXXXXX", 6);
    CHECK(mrl_io(MRL_EV_MARK, rb, &rl, sizeof rb) == MRL_OK && memcmp(rb, "phase2", 6) == 0);
    CHECK(mrl_checkpoint(MRL_CKPT_USER, 0xDEADBEEF) == MRL_OK);
    CHECK(mrl_checkpoint(MRL_CKPT_HEAP, 0x22222222) == MRL_OK);   /* ignored: not strict */
    mrl_lclk = 70;
    CHECK(mrl_crash("ValueError: boom", 16) == MRL_OK);
    CHECK(mrl_replay_divergence() == NULL);
    mrl_replay_stats_t st; mrl_replay_stats(&st);
    CHECK(st.at_end && st.late_async == 0);
    mrl_replay_end();

    /* ---- divergence: wrong logical clock ---- */
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    mrl_lclk = 11;
    { uint8_t p[4], l = 4; CHECK(mrl_io(MRL_EV_TICKS_MS, p, &l, 4) == MRL_ERR_DIVERGED); }
    const mrl_divergence_t *d = mrl_replay_divergence();
    CHECK(d && d->reason == MRL_DIV_CLOCK && d->lclk_now == 11 && d->lclk_log == 10);
    mrl_replay_end();

    /* ---- divergence: wrong kind of input ---- */
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    mrl_lclk = 10;
    { uint8_t p[4], l = 4; CHECK(mrl_io(MRL_EV_TICKS_US, p, &l, 4) == MRL_ERR_DIVERGED); }
    d = mrl_replay_divergence();
    CHECK(d && d->reason == MRL_DIV_TYPE && d->type_expected == MRL_EV_TICKS_US && d->type_logged == MRL_EV_TICKS_MS);
    mrl_replay_end();

    /* ---- divergence: strict heap checkpoint mismatch ---- */
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, true) == MRL_OK);
    for (int i = 0; i < 3; i++) { mrl_lclk += 10; uint8_t p[4], l = 4; CHECK(mrl_io(MRL_EV_TICKS_MS, p, &l, 4) == MRL_OK); }
    { uint8_t p[16], l = 0; CHECK(mrl_io(MRL_EV_UART_RX, p, &l, 16) == MRL_OK); }
    mrl_lclk = 50; CHECK(mrl_replay_poll(&ev));
    mrl_lclk = 60; { uint8_t p[16], l = 0; CHECK(mrl_io(MRL_EV_MARK, p, &l, 16) == MRL_OK); }
    CHECK(mrl_checkpoint(MRL_CKPT_USER, 0xDEADBEEF) == MRL_OK);
    CHECK(mrl_checkpoint(MRL_CKPT_HEAP, 0x22222222) == MRL_ERR_DIVERGED);
    CHECK(mrl_replay_divergence()->reason == MRL_DIV_VALUE);
    mrl_replay_end();

    /* ---- late async delivery is counted, not fatal ---- */
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    for (int i = 0; i < 3; i++) { mrl_lclk += 10; uint8_t p[4], l = 4; mrl_io(MRL_EV_TICKS_MS, p, &l, 4); }
    { uint8_t p[16], l = 0; mrl_io(MRL_EV_UART_RX, p, &l, 16); }
    mrl_lclk = 55; CHECK(mrl_replay_poll(&ev));
    mrl_replay_stats(&st); CHECK(st.late_async == 1);
    mrl_replay_end();

    /* ---- truncated / corrupted tail is detected ---- */
    uint8_t *cut = malloc(n);
    memcpy(cut, img, n);
    cut[n - 3] ^= 0xFF; /* corrupt the CRC of the CRASH record */
    mrl_reader_t rd; CHECK(mrl_reader_open(&rd, cut, n) == MRL_OK);
    int cnt = 0; while (mrl_reader_next(&rd, &ev) == MRL_OK) cnt++;
    CHECK(cnt == 8);
    CHECK(mrl_reader_open(&rd, cut, n - 40) == MRL_OK);
    cnt = 0; while (mrl_reader_next(&rd, &ev) == MRL_OK) cnt++;
    CHECK(cnt < 8);
    free(cut); free(img);
}

static void test_io_exc(void) {
    mrl_header_t h = {0};
    unsetenv("MRL_CAPACITY");
    mrl_mode = MRL_MODE_OFF; mrl_lclk = 0;
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    mrl_lclk = 100;
    uint8_t kind = MRL_EXC_IN_IO;
    CHECK(mrl_rec_event(MRL_EV_EXC_ASYNC, &kind, 1) == MRL_OK);
    kind = MRL_EXC_KBD; mrl_lclk = 200;
    CHECK(mrl_rec_event(MRL_EV_EXC_ASYNC, &kind, 1) == MRL_OK);
    CHECK(mrl_rec_end() == MRL_OK);
    size_t n; uint8_t *img = slurp(LOG, &n);
    mrl_event_t ev;
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    mrl_lclk = 100;
    CHECK(!mrl_replay_poll(&ev));            /* in-I/O interrupts are not delivered by the hook */
    mrl_lclk = 99;  CHECK(!mrl_replay_take_io_exc());
    mrl_lclk = 100; CHECK(mrl_replay_take_io_exc());
    CHECK(!mrl_replay_take_io_exc());
    mrl_lclk = 199; CHECK(!mrl_replay_poll(&ev));
    mrl_lclk = 200; CHECK(mrl_replay_poll(&ev) && ev.type == MRL_EV_EXC_ASYNC && ev.payload[0] == MRL_EXC_KBD);
    mrl_replay_end();
    free(img);
}

static void test_review_regressions(void) {
    mrl_header_t h = {0};
    unsetenv("MRL_CAPACITY");
    mrl_mode = MRL_MODE_OFF; mrl_lclk = 0;

    /* NULL / bad arguments are rejected, not dereferenced */
    CHECK(mrl_rec_begin(NULL) == MRL_ERR_ARG);
    uint8_t l = 0;
    CHECK(mrl_io(MRL_EV_ADC, NULL, NULL, 0) == MRL_ERR_ARG);
    CHECK(mrl_io(MRL_EV_ADC, NULL, &l, 0) == MRL_OK);
    CHECK(mrl_mark(NULL, 3) == MRL_ERR_ARG);
    CHECK(mrl_crash(NULL, 3) == MRL_ERR_ARG);
    CHECK(mrl_storage_read(0, NULL, 4) == MRL_ERR_ARG);

    /* mark: the replayed text must match the recorded one */
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    mrl_lclk = 5;  CHECK(mrl_mark("alpha", 5) == MRL_OK);
    mrl_lclk = 9;  CHECK(mrl_mark("beta", 4) == MRL_OK);
    CHECK(mrl_rec_end() == MRL_OK);
    size_t n; uint8_t *img = slurp(LOG, &n);
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    mrl_lclk = 5;  CHECK(mrl_mark("alpha", 5) == MRL_OK);
    CHECK(mrl_replay_remaining() == 1);
    mrl_lclk = 9;  CHECK(mrl_mark("gamma", 5) == MRL_ERR_DIVERGED);  /* different text and length */
    CHECK(mrl_replay_divergence() && mrl_replay_divergence()->reason == MRL_DIV_VALUE);
    mrl_replay_end();
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    mrl_lclk = 5;  CHECK(mrl_mark("alpha", 5) == MRL_OK);
    mrl_lclk = 9;  CHECK(mrl_mark("betX", 4) == MRL_ERR_DIVERGED);   /* same length, one byte differs */
    mrl_replay_end();

    /* remaining events are reported */
    mrl_lclk = 0; CHECK(mrl_replay_begin(img, n, false) == MRL_OK);
    CHECK(mrl_replay_remaining() == 2);
    mrl_lclk = 5;  CHECK(mrl_mark("alpha", 5) == MRL_OK);
    CHECK(mrl_replay_remaining() == 1);
    mrl_lclk = 9;  CHECK(mrl_mark("beta", 4) == MRL_OK);
    CHECK(mrl_replay_remaining() == 0);
    mrl_replay_end();
    free(img);

    /* too small a storage must NOT erase the previous log */
    mrl_lclk = 0;
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    mrl_lclk = 1; CHECK(mrl_mark("keep", 4) == MRL_OK);
    CHECK(mrl_rec_end() == MRL_OK);
    setenv("MRL_CAPACITY", "100", 1);
    CHECK(mrl_rec_begin(&h) == MRL_ERR_FULL);
    unsetenv("MRL_CAPACITY");
    mrl_scan_info_t si; CHECK(mrl_storage_scan(&si) == MRL_OK && si.events == 1);
}

static void test_storage_full(void) {
    mrl_mode = MRL_MODE_OFF; mrl_lclk = 0;
    mrl_header_t h = {0};
    uint8_t payload[200] = {0};

    /* plenty of room: 1000 events, nothing lost */
    unsetenv("MRL_CAPACITY");
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    for (int i = 0; i < 1000; i++) { mrl_lclk++; CHECK(mrl_rec_event(MRL_EV_UART_RX, payload, 200) == MRL_OK); }
    CHECK(!mrl_rec_overflowed() && mrl_rec_end() == MRL_OK);
    mrl_scan_info_t si; CHECK(mrl_storage_scan(&si) == MRL_OK && si.events == 1000);
    CHECK(mrl_rec_event(1, NULL, 0) == MRL_ERR_STATE);   /* not recording any more */

    /* tiny storage: recording stops cleanly, the log stays valid */
    setenv("MRL_CAPACITY", "3000", 1);
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    int ok = 0, full = 0;
    for (int i = 0; i < 100; i++) {
        mrl_lclk++;
        mrl_err_t e = mrl_rec_event(MRL_EV_UART_RX, payload, 200);
        if (e == MRL_OK) ok++; else { CHECK(e == MRL_ERR_FULL); full++; }
    }
    CHECK(full > 0 && ok > 5 && ok < 100);
    CHECK(mrl_rec_overflowed());
    CHECK(mrl_rec_end() == MRL_ERR_FULL);
    CHECK(mrl_storage_scan(&si) == MRL_OK);
    CHECK(si.events > 5 && si.events <= (uint32_t)ok);       /* no garbage, nothing past capacity */
    CHECK(si.valid_bytes <= 3000);

    /* restarting erases the previous log */
    unsetenv("MRL_CAPACITY");
    CHECK(mrl_rec_begin(&h) == MRL_OK);
    CHECK(mrl_rec_end() == MRL_OK);
    CHECK(mrl_storage_scan(&si) == MRL_OK && si.events == 0);
}

int main(void) {
    test_crc_vectors();
    test_varint();
    test_header();
    test_record_replay();
    test_io_exc();
    test_review_regressions();
    test_storage_full();
    puts("all core tests passed");
    return 0;
}
