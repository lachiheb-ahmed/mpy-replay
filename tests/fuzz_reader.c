/* SPDX-License-Identifier: MIT
 * mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 * Mutation fuzzing of the log parser: corrupted or truncated logs must never crash. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mrl_internal.h"
#include "replay_port.h"

static uint32_t rng_state = 12345;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

int main(void) {
    setenv("MRL_LOG", "/tmp/mrl_fuzz.mrl", 1);
    mrl_header_t h = {.heap_size = 4096};
    mrl_lclk = 0; mrl_mode = MRL_MODE_OFF;
    if (mrl_rec_begin(&h) != MRL_OK) return 1;
    for (int i = 0; i < 300; i++) {
        mrl_lclk += 1 + rnd() % 50;
        uint8_t p[40]; uint8_t n = (uint8_t)(rnd() % 40);
        for (int k = 0; k < n; k++) p[k] = (uint8_t)rnd();
        mrl_rec_event((uint8_t)(1 + rnd() % 13), p, n);
    }
    mrl_rec_end();
    mrl_scan_info_t si;
    if (mrl_storage_scan(&si) != MRL_OK) return 1;
    size_t n = si.valid_bytes;
    uint8_t *good = malloc(n), *buf = malloc(n + 16);
    FILE *f = fopen("/tmp/mrl_fuzz.mrl", "rb");
    if (!f || fread(good, 1, n, f) != n) return 1;
    fclose(f);

    long events = 0, opened = 0;
    for (int it = 0; it < 300000; it++) {
        memcpy(buf, good, n);
        size_t len = n;
        int flips = 1 + rnd() % 8;
        for (int k = 0; k < flips; k++) buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
        if (rnd() % 3 == 0) len = rnd() % (n + 1);             /* truncate */
        if (rnd() % 7 == 0 && len > 70) memset(buf + 64 + rnd() % (len - 64), (int)(rnd() % 3 ? 0xFF : 0), rnd() % 20);
        mrl_reader_t r;
        if (mrl_reader_open(&r, buf, len) != MRL_OK) continue;
        opened++;
        mrl_event_t ev;
        while (mrl_reader_next(&r, &ev) == MRL_OK) events++;
        mrl_lclk = 0; mrl_mode = MRL_MODE_OFF;
        if (mrl_replay_begin(buf, len, false) == MRL_OK) {      /* replay engine on a corrupted log */
            for (int k = 0; k < 20; k++) { uint8_t b[255], l = 0; mrl_lclk += rnd() % 5; mrl_io((uint8_t)(1 + rnd() % 13), b, &l, sizeof b); mrl_replay_poll(&ev); }
            (void)mrl_replay_remaining();
            mrl_replay_end();
        }
    }
    printf("fuzz ok: %ld logs parsed past the header, %ld events decoded, no crash\n", opened, events);
    fflush(stdout);
    free(good); free(buf);
    return 0;
}
