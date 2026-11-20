/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * mrl_dump: print the header and every valid event of a .mrl log.
 *   usage: mrl_dump [-q] file.mrl      (-q: summary only)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mrl_core.h"

static const char *name(uint8_t t) {
    switch (t) {
        case MRL_EV_TICKS_MS:   return "TICKS_MS";
        case MRL_EV_ADC:        return "ADC";
        case MRL_EV_GPIO:       return "GPIO";
        case MRL_EV_UART_RX:    return "UART_RX";
        case MRL_EV_I2C_RD:     return "I2C_RD";
        case MRL_EV_SPI_RD:     return "SPI_RD";
        case MRL_EV_RNG:        return "RNG";
        case MRL_EV_IRQ_SCHED:  return "IRQ_SCHED";
        case MRL_EV_EXC_ASYNC:  return "EXC_ASYNC";
        case MRL_EV_CHECKPOINT: return "CHECKPOINT";
        case MRL_EV_MARK:       return "MARK";
        case MRL_EV_TICKS_US:   return "TICKS_US";
        case MRL_EV_TICKS_CPU:  return "TICKS_CPU";
        case MRL_EV_CRASH:      return "CRASH";
        default:                return "?";
    }
}

int main(int argc, char **argv) {
    int quiet = argc > 2 && strcmp(argv[1], "-q") == 0;
    const char *path = argc > 1 ? argv[argc - 1] : NULL;
    if (path == NULL) {
        fprintf(stderr, "usage: %s [-q] file.mrl\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return 1;
    }
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
    }
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "%s: cannot determine file size\n", path);
        fclose(f);
        return 1;
    }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: read error\n", path);
        free(buf);
        fclose(f);
        return 1;
    }
    fclose(f);

    mrl_reader_t r;
    mrl_err_t e = mrl_reader_open(&r, buf, (size_t)sz);
    if (e != MRL_OK) {
        fprintf(stderr, "%s: %s\n", path, mrl_strerror(e));
        free(buf);
        return 1;
    }
    printf("version %u  heap %u  board 0x%08x  fw ", r.hdr.version, r.hdr.heap_size, r.hdr.board_id);
    for (int i = 0; i < 20; i++) {
        printf("%02x", r.hdr.fw_hash[i]);
    }
    printf("\n");

    mrl_event_t ev;
    unsigned n = 0, by_type[256] = {0};
    while (mrl_reader_next(&r, &ev) == MRL_OK) {
        n++;
        by_type[ev.type]++;
        if (quiet) {
            continue;
        }
        printf("%10llu  %-10s %3u  ", (unsigned long long)ev.lclk, name(ev.type), ev.len);
        int text = (ev.type == MRL_EV_MARK || ev.type == MRL_EV_CRASH);
        for (unsigned i = 0; i < ev.len && i < 24; i++) {
            if (text) {
                putchar(ev.payload[i] >= 32 && ev.payload[i] < 127 ? ev.payload[i] : '.');
            } else {
                printf("%02x", ev.payload[i]);
            }
        }
        printf("%s\n", ev.len > 24 ? "..." : "");
    }
    printf("-- %u events, %zu of %ld bytes valid\n", n, r.pos, sz);
    for (int t = 0; t < 256; t++) {
        if (by_type[t]) {
            printf("   %-10s %u\n", name((uint8_t)t), by_type[t]);
        }
    }
    free(buf);
    return r.pos == (size_t)sz ? 0 : 3; /* 3 = trailing bytes that are not valid records */
}
