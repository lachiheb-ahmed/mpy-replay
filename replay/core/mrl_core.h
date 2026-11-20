/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * Public API of the portable core. Pure C99, no MicroPython dependency.
 *
 * Threading rule: every function below must be called from the VM thread
 * only (never from an ISR). Asynchronous events are logged when the VM
 * *delivers* them, not when the hardware raises them.
 */
#ifndef MRL_CORE_H
#define MRL_CORE_H

#include "mrl_config.h"
#include "mrl_types.h"

#define MRL_MODE_OFF    0
#define MRL_MODE_RECORD 1
#define MRL_MODE_REPLAY 2

/* Logical clock and current mode (single writer: the VM thread). */
extern uint64_t mrl_lclk;
extern uint8_t  mrl_mode;

/* Called by MICROPY_VM_HOOK_LOOP on every VM branch. */
static inline void mrl_tick(void) { mrl_lclk++; }

const char *mrl_strerror(mrl_err_t e);

/* ---- checksums / codecs ------------------------------------------------ */
uint32_t  mrl_crc32(uint32_t crc, const void *data, size_t len); /* start with 0 */
uint16_t  mrl_crc16(uint16_t crc, const void *data, size_t len); /* CCITT-FALSE */
void      mrl_header_encode(const mrl_header_t *h, uint8_t out[MRL_HEADER_SIZE]);
mrl_err_t mrl_header_decode(const uint8_t in[MRL_HEADER_SIZE], mrl_header_t *h, uint16_t *seed);

/* ---- recorder ---------------------------------------------------------- */
mrl_err_t mrl_rec_begin(const mrl_header_t *h);  /* erases storage, mode = RECORD */
mrl_err_t mrl_rec_event(uint8_t type, const void *payload, uint8_t len);
mrl_err_t mrl_rec_flush(void);
mrl_err_t mrl_rec_end(void);                     /* flush, mode = OFF */
size_t    mrl_rec_bytes(void);                   /* bytes written + staged */
bool      mrl_rec_overflowed(void);

/* ---- existing log in storage (flight recorder after a reset) ------------ */
typedef struct {
    mrl_header_t hdr;
    size_t       valid_bytes;  /* header + all valid records */
    uint32_t     events;
    uint64_t     last_lclk;
    bool         has_crash;
    uint8_t      crash_len;
    uint8_t      crash[64];    /* first bytes of the last CRASH text */
} mrl_scan_info_t;

mrl_err_t mrl_storage_scan(mrl_scan_info_t *info);
mrl_err_t mrl_storage_read(size_t off, void *dst, size_t n);
mrl_err_t mrl_storage_capacity(size_t *cap);

/* ---- reader (memory image of a log) ------------------------------------ */
typedef struct {
    const uint8_t *data;
    size_t         size, pos;
    uint64_t       lclk;
    uint16_t       seed;
    mrl_header_t   hdr;
} mrl_reader_t;

mrl_err_t mrl_reader_open(mrl_reader_t *r, const uint8_t *data, size_t size);
mrl_err_t mrl_reader_next(mrl_reader_t *r, mrl_event_t *ev); /* MRL_ERR_END at the end */

/* ---- replay ------------------------------------------------------------ */
typedef enum {
    MRL_DIV_NONE = 0,
    MRL_DIV_TYPE,    /* a different kind of input was requested */
    MRL_DIV_CLOCK,   /* right input, wrong logical clock */
    MRL_DIV_VALUE,   /* checkpoint / crash text mismatch */
} mrl_div_reason_t;

typedef struct {
    mrl_div_reason_t reason;
    uint64_t lclk_now;
    uint64_t lclk_log;
    uint8_t  type_expected;  /* what the replayed program asked for */
    uint8_t  type_logged;    /* what the log contains (0 = end of log) */
    uint32_t event_index;
} mrl_divergence_t;

typedef struct {
    uint32_t consumed;       /* events consumed so far */
    uint32_t late_async;     /* async events delivered after their clock */
    bool     at_end;
} mrl_replay_stats_t;

mrl_err_t mrl_replay_begin(const uint8_t *data, size_t size, bool strict_heap);
void      mrl_replay_end(void);
/* Deliver the next asynchronous event if it is due. Call from the VM hook. */
bool      mrl_replay_poll(mrl_event_t *ev);
/* True (and consumed) if the recording was interrupted *during* the input
 * about to be replayed at the current clock (EXC_ASYNC / MRL_EXC_IN_IO). */
bool      mrl_replay_take_io_exc(void);
const mrl_divergence_t *mrl_replay_divergence(void); /* NULL if none */
const mrl_header_t     *mrl_replay_header(void);     /* header of the loaded log */
void      mrl_replay_stats(mrl_replay_stats_t *s);

/* ---- mode-independent helpers (what port hooks call) -------------------- */

/* Generic input:
 *  OFF:    no-op.
 *  RECORD: logs buf[0..*len).
 *  REPLAY: overwrites buf (capacity `cap`) with the logged payload, sets *len. */
mrl_err_t mrl_io(uint8_t type, void *buf, uint8_t *len, uint8_t cap);

/* Checkpoint. In REPLAY the value is compared with the recorded one
 * (MRL_CKPT_HEAP is only compared when strict_heap was requested). */
mrl_err_t mrl_checkpoint(uint8_t kind, uint32_t value);

/* Last words. RECORD: logs text and flushes. REPLAY: MRL_OK if the replayed
 * crash matches the recorded one, MRL_ERR_DIVERGED otherwise. */
mrl_err_t mrl_crash(const char *text, size_t len);

/* Named synchronization point. RECORD: logs text. REPLAY: the text must equal
 * the recorded one, otherwise the replay diverges. */
mrl_err_t mrl_mark(const char *text, size_t len);

/* Number of recorded events not yet consumed (0 when not replaying). */
uint32_t  mrl_replay_remaining(void);

#endif /* MRL_CORE_H */
