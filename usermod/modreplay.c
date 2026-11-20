/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * MicroPython glue: the `replay` module, the VM hook and the clock hooks.
 *
 * Build flags:
 *   MPY_REPLAY_MODE=1        replay build (hosted ports only): replay.start()
 *                            replays the log named by $MRL_REPLAY_LOG
 *   MPY_REPLAY_FW_HASH_HEX   40 hex chars, stored in the log header
 */
#include <string.h>
#include "py/runtime.h"
#include "py/objstr.h"
#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/nlr.h"
#include "py/mpstate.h"
#include "mrl_core.h"
#include "replay_port.h"
#include "mpy_replay_hooks.h"

#if MICROPY_ASYNC_KBD_INTR || MPY_REPLAY_TEST
#include <signal.h>
#endif

#if MPY_REPLAY_MODE
#include <stdio.h>
#include <stdlib.h>

/* The log image used by the replay engine. Freed when replaying ends or fails. */
static uint8_t *s_img;

static void free_image(void) {
    free(s_img);
    s_img = NULL;
}
#endif

#ifndef MPY_REPLAY_FW_HASH_HEX
#define MPY_REPLAY_FW_HASH_HEX ""
#endif
#ifndef MPY_REPLAY_TEST
#define MPY_REPLAY_TEST 0
#endif
_Static_assert(sizeof(MPY_REPLAY_FW_HASH_HEX) == 1 || sizeof(MPY_REPLAY_FW_HASH_HEX) == 41,
    "MPY_REPLAY_FW_HASH must be exactly 40 hex characters (or empty)");

MP_REGISTER_ROOT_POINTER(mp_obj_t replay_dispatch);
MP_REGISTER_ROOT_POINTER(mp_obj_t replay_test_irq);

/* Provenance string, kept in every firmware image that contains this module. */
static const char replay_about_str[] =
    "mpy-replay 0.1.0 - Copyright (c) 2026 Ahmed (lachiheb-ahmed) - MIT - "
    "https://github.com/lachiheb-ahmed";

/* True while a recorded hardware read runs: the logical clock is frozen and
 * asynchronous delivery is deferred (see replay__io). */
static bool s_in_io;

#if MICROPY_ASYNC_KBD_INTR
/* Some ports (unix) raise KeyboardInterrupt from inside the signal handler,
 * at an arbitrary instruction. That cannot be replayed, so replay.start()
 * swaps in a handler that only schedules the exception (delivered at a VM
 * branch, where the hook can log it). */
static struct sigaction s_old_sigint;
static bool s_sigint_installed;

static void sigint_sched(int sig) {
    (void)sig;
    mp_sched_keyboard_interrupt();
}

static void sigint_restore(void) {
    if (s_sigint_installed) {
        sigaction(SIGINT, &s_old_sigint, NULL);
        s_sigint_installed = false;
    }
}
#endif

/* ---- helpers ------------------------------------------------------------- */

static void replay_raise(mrl_err_t e) {
    const mrl_divergence_t *d = mrl_replay_divergence();
    if (e == MRL_ERR_DIVERGED && d != NULL) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("replay diverged (reason %d) at lclk %u: program asked 0x%02x, log has 0x%02x at lclk %u"),
            (int)d->reason, (unsigned)d->lclk_now, (unsigned)d->type_expected,
            (unsigned)d->type_logged, (unsigned)d->lclk_log);
    }
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("replay: %s"), mrl_strerror(e));
}

/* A full log must never kill the application: recording simply stops, the log
 * stays valid up to that point and replay.stop() reports the truncation. */
static bool rec_fatal(mrl_err_t e) {
    return e != MRL_OK && e != MRL_ERR_FULL;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Source revision this build identifies itself with (MPY_REPLAY_FW_HASH).
 * Returns 1 if a hash is set, 0 if none was given, -1 if it is malformed. */
static int build_hash(uint8_t out[20]) {
    const char *hex = MPY_REPLAY_FW_HASH_HEX;
    memset(out, 0, 20);
    bool any = false;
    for (size_t i = 0; i < 20 && hex[2 * i] && hex[2 * i + 1]; i++) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            memset(out, 0, 20);
            return -1; /* not hexadecimal */
        }
        out[i] = (uint8_t)((hi << 4) | lo);
        any = any || out[i] != 0;
    }
    return any ? 1 : 0;
}

static uint32_t heap_crc(void) {
    const mp_state_mem_area_t *a = &MP_STATE_MEM(area);
    return mrl_crc32(0, a->gc_pool_start, (size_t)(a->gc_pool_end - a->gc_pool_start));
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* ---- VM hook (slow path) -------------------------------------------------- */

void mpy_replay_vm_slow(void) {
    if (mrl_mode == MRL_MODE_RECORD) {
        if (s_in_io) {
            return;
        }
        /* An asynchronous exception (Ctrl+C...) is about to be delivered by the
         * check that follows this hook: log it at its delivery clock. */
        mp_obj_t exc = MP_STATE_THREAD(mp_pending_exception);
        if (exc != MP_OBJ_NULL) {
            uint8_t kind = mp_obj_is_type(exc, &mp_type_KeyboardInterrupt) ? MRL_EXC_KBD : MRL_EXC_OTHER;
            mrl_rec_event(MRL_EV_EXC_ASYNC, &kind, 1);
        }
    } else if (mrl_mode == MRL_MODE_REPLAY) {
        mrl_event_t ev;
        while (mrl_replay_poll(&ev)) {
            if (ev.type == MRL_EV_IRQ_SCHED && ev.len == 1) {
                if (MP_STATE_VM(replay_dispatch) == MP_OBJ_NULL) {
                    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("replay: IRQ event but no dispatcher (import replay_io)"));
                }
                if (!mp_sched_schedule(MP_STATE_VM(replay_dispatch), MP_OBJ_NEW_SMALL_INT(ev.payload[0]))) {
                    mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("replay: scheduler queue full, an IRQ event would be lost"));
                }
            } else if (ev.type == MRL_EV_EXC_ASYNC && ev.len == 1 && ev.payload[0] == MRL_EXC_KBD) {
                mp_sched_keyboard_interrupt();
            }
        }
    }
}

/* ---- clock hooks ---------------------------------------------------------- */

uint64_t mpy_replay_ticks(uint8_t type, uint64_t real) {
    if (mrl_mode == MRL_MODE_OFF) {
        return real;
    }
    uint8_t p[8];
    uint8_t len = sizeof p;
    put64(p, real);
    mrl_err_t e = mrl_io(type, p, &len, sizeof p);
    if (rec_fatal(e) || (e == MRL_OK && len != sizeof p)) {
        replay_raise(e != MRL_OK ? e : MRL_ERR_CORRUPT);
    }
    return get64(p);
}

/* ---- replay module -------------------------------------------------------- */

static mp_obj_t replay_start(void) {
    if (mrl_mode != MRL_MODE_OFF) {
        return mp_const_none;
    }
    /* The logical clock is relative to this call, so code that ran before it
     * (boot.py, imports) may differ between the board and the replay host. */
    mrl_lclk = 0;
    uint8_t hash_check[20];
    if (build_hash(hash_check) < 0) {
        mp_raise_msg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("replay: MPY_REPLAY_FW_HASH is not 40 hexadecimal characters"));
    }
    #if MPY_REPLAY_MODE
    const char *path = getenv("MRL_REPLAY_LOG");
    if (path == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("replay: set MRL_REPLAY_LOG"));
    }
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        mp_raise_OSError(MP_ENOENT);
    }
    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) {
        sz = ftell(f);
    }
    if (sz < (long)MRL_HEADER_SIZE || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("replay: log file unreadable or too short"));
    }
    free_image();
    uint8_t *img = s_img = malloc((size_t)sz);
    if (img == NULL || fread(img, 1, (size_t)sz, f) != (size_t)sz) {
        free_image();
        fclose(f);
        mp_raise_OSError(MP_EIO);
    }
    fclose(f);
    const char *strict = getenv("MRL_STRICT_HEAP");
    mrl_err_t e = mrl_replay_begin(img, (size_t)sz, strict != NULL && strict[0] == '1');
    if (e == MRL_OK) {
        /* Refuse to replay a log recorded by a different source revision,
         * unless either side has no hash or MRL_IGNORE_FW_HASH=1. */
        uint8_t mine[20];
        const uint8_t *theirs = mrl_replay_header()->fw_hash;
        static const uint8_t zero[20];
        const char *ign = getenv("MRL_IGNORE_FW_HASH");
        if (build_hash(mine) > 0 && memcmp(theirs, zero, 20) != 0 && memcmp(mine, theirs, 20) != 0
            && !(ign != NULL && ign[0] == '1')) {
            mrl_replay_end();
            free_image();
            mp_raise_msg_varg(&mp_type_RuntimeError,
                MP_ERROR_TEXT("replay: firmware hash mismatch (log %02x%02x%02x%02x.., build %02x%02x%02x%02x..)"),
                theirs[0], theirs[1], theirs[2], theirs[3], mine[0], mine[1], mine[2], mine[3]);
        }
    }
    #else
    mrl_header_t h;
    memset(&h, 0, sizeof h);
    build_hash(h.fw_hash);
    h.heap_size = replay_port_heap_size();
    h.board_id = replay_port_board_id();
    mrl_err_t e = mrl_rec_begin(&h);
    #endif
    if (e != MRL_OK) {
        #if MPY_REPLAY_MODE
        free_image();
        #endif
        replay_raise(e);
    }
    #if MICROPY_ASYNC_KBD_INTR
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigint_sched;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, &s_old_sigint);
    s_sigint_installed = true;
    #endif
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_start_obj, replay_start);

/* replay.stop([partial])
 * RECORD: flush and stop. REPLAY: stop; raises if recorded events were not
 * consumed, unless partial is true (explicit early end). */
static mp_obj_t replay_stop(size_t n_args, const mp_obj_t *args) {
    bool partial = n_args > 0 && mp_obj_is_true(args[0]);
    mrl_err_t e = MRL_OK;
    uint32_t left = 0;
    if (mrl_mode == MRL_MODE_RECORD) {
        e = mrl_rec_end();
    } else if (mrl_mode == MRL_MODE_REPLAY) {
        if (!partial && mrl_replay_divergence() == NULL) {
            left = mrl_replay_remaining();
        }
        mrl_replay_end();
        #if MPY_REPLAY_MODE
        free_image();
        #endif
    }
    #if MICROPY_ASYNC_KBD_INTR
    sigint_restore();
    #endif
    if (e != MRL_OK) {
        replay_raise(e);
    }
    if (left != 0) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("replay: %u recorded events were not consumed (stop(True) allows a partial replay)"),
            (unsigned)left);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(replay_stop_obj, 0, 1, replay_stop);

static mp_obj_t replay_about(void) {
    return mp_obj_new_str(replay_about_str, sizeof replay_about_str - 1);
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_about_obj, replay_about);

static mp_obj_t replay_mode(void) {
    return MP_OBJ_NEW_SMALL_INT(mrl_mode);
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_mode_obj, replay_mode);

static mp_obj_t replay_clock(void) {
    return mp_obj_new_int_from_ull(mrl_lclk);
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_clock_obj, replay_clock);

static mp_obj_t replay_mark(mp_obj_t text) {
    size_t n;
    const char *s = mp_obj_str_get_data(text, &n);
    mrl_err_t e = mrl_mark(s, n);
    if (rec_fatal(e)) {
        replay_raise(e);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(replay_mark_obj, replay_mark);

/* replay.checkpoint()      -> CRC of the GC heap (bit-exact replay only)
 * replay.checkpoint(value) -> application-defined integer */
static mp_obj_t replay_checkpoint(size_t n_args, const mp_obj_t *args) {
    mrl_err_t e;
    if (n_args == 0) {
        e = mrl_checkpoint(MRL_CKPT_HEAP, heap_crc());
    } else {
        e = mrl_checkpoint(MRL_CKPT_USER, (uint32_t)mp_obj_get_int_truncated(args[0]));
    }
    if (rec_fatal(e)) {
        replay_raise(e);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(replay_checkpoint_obj, 0, 1, replay_checkpoint);

/* replay.crash(text) -> True if a replayed crash matched the recorded one */
static mp_obj_t replay_crash(mp_obj_t text) {
    size_t n;
    const char *s = mp_obj_str_get_data(text, &n);
    bool replaying = (mrl_mode == MRL_MODE_REPLAY);
    mrl_err_t e = mrl_crash(s, n);
    if (rec_fatal(e)) {
        replay_raise(e);
    }
    return mp_obj_new_bool(replaying);
}
static MP_DEFINE_CONST_FUN_OBJ_1(replay_crash_obj, replay_crash);

/* replay.has_log() -> None | (valid_bytes, events, last_lclk, crash_text_or_None)
 * Inspects the log that is currently in storage (e.g. after a reset). */
static mp_obj_t replay_has_log(void) {
    mrl_scan_info_t si;
    if (mrl_storage_scan(&si) != MRL_OK) {
        return mp_const_none;
    }
    mp_obj_t items[4];
    items[0] = mp_obj_new_int_from_uint(si.valid_bytes);
    items[1] = mp_obj_new_int_from_uint(si.events);
    items[2] = mp_obj_new_int_from_ull(si.last_lclk);
    items[3] = si.has_crash ? mp_obj_new_str((const char *)si.crash, si.crash_len) : mp_const_none;
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_has_log_obj, replay_has_log);

/* replay.read(offset, n) -> bytes of the stored log (for export) */
static mp_obj_t replay_read(mp_obj_t off_in, mp_obj_t n_in) {
    mp_int_t off_i = mp_obj_get_int(off_in);
    mp_int_t n_i = mp_obj_get_int(n_in);
    size_t cap = 0;
    mrl_err_t ce = mrl_storage_capacity(&cap);
    if (ce != MRL_OK) {
        replay_raise(ce);
    }
    if (off_i < 0 || n_i < 0 || (size_t)off_i > cap || (size_t)n_i > cap - (size_t)off_i) {
        mp_raise_ValueError(MP_ERROR_TEXT("offset/length out of range"));
    }
    size_t off = (size_t)off_i;
    size_t n = (size_t)n_i;
    vstr_t vstr;
    vstr_init_len(&vstr, n);
    mrl_err_t e = mrl_storage_read(off, vstr.buf, n);
    if (e != MRL_OK) {
        vstr_clear(&vstr);
        replay_raise(e);
    }
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(replay_read_obj, replay_read);

/* replay.ticks_ms() / ticks_us() / ticks_cpu(): recorded clock sources for
 * code that cannot use the patched `time` module. */
static mp_obj_t replay_ticks_ms(void) {
    return mp_obj_new_int_from_ull(MPY_REPLAY_TICKS_MS(mp_hal_ticks_ms()));
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_ticks_ms_obj, replay_ticks_ms);

static mp_obj_t replay_ticks_us(void) {
    return mp_obj_new_int_from_ull(MPY_REPLAY_TICKS_US(mp_hal_ticks_us()));
}
static MP_DEFINE_CONST_FUN_OBJ_0(replay_ticks_us_obj, replay_ticks_us);

/* ---- internals used by replay_io.py ---------------------------------------- */

/* Runs fn(*args) with the logical clock frozen and the scheduler locked
 * (RECORD mode). A KeyboardInterrupt that escapes is logged as "inside a read". */
static mp_obj_t call_frozen(mp_obj_t fn, size_t n_args, const mp_obj_t *args) {
    const uint64_t saved = mrl_lclk;
    mp_obj_t res;
    nlr_buf_t nlr;
    mp_sched_lock();
    s_in_io = true;
    if (nlr_push(&nlr) == 0) {
        res = mp_call_function_n_kw(fn, n_args, 0, args);
        nlr_pop();
    } else {
        s_in_io = false;
        mrl_lclk = saved;
        mp_sched_unlock();
        if (mp_obj_is_type(MP_OBJ_FROM_PTR(nlr.ret_val), &mp_type_KeyboardInterrupt)) {
            uint8_t kind = MRL_EXC_IN_IO;
            mrl_rec_event(MRL_EV_EXC_ASYNC, &kind, 1);
        }
        nlr_jump(nlr.ret_val);
    }
    s_in_io = false;
    mrl_lclk = saved;
    mp_sched_unlock();
    return res;
}

/* replay._io(type, fetch) -> bytes.  type must be in 2..7.
 *   OFF:    returns fetch().
 *   RECORD: runs fetch() with the logical clock frozen and the scheduler
 *           locked, logs the bytes it returned, returns them.
 *   REPLAY: does NOT call fetch(); returns the recorded bytes.
 * Freezing the clock keeps record and replay on the same clock even though
 * only record executes fetch(). fetch() must return <= 255 bytes and must not
 * raise: a KeyboardInterrupt raised inside fetch() is recorded (and replayed
 * at the same point); any other exception is not recorded. */
static mp_obj_t replay__io(mp_obj_t type_in, mp_obj_t fetch) {
    mp_int_t type = mp_obj_get_int(type_in);
    if (type < MRL_EV_ADC || type > MRL_EV_RNG) {
        mp_raise_ValueError(MP_ERROR_TEXT("type must be 2..7"));
    }
    if (mrl_mode == MRL_MODE_OFF) {
        return mp_call_function_0(fetch);
    }
    uint8_t buf[MRL_MAX_PAYLOAD];
    uint8_t len = 0;
    if (mrl_mode == MRL_MODE_REPLAY) {
        if (mrl_replay_take_io_exc()) {
            /* the recorded run was interrupted inside this read */
            mp_sched_keyboard_interrupt();
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        }
        mrl_err_t e = mrl_io((uint8_t)type, buf, &len, sizeof buf);
        if (rec_fatal(e)) {
            replay_raise(e);
        }
        return mp_obj_new_bytes(buf, len);
    }

    /* RECORD */
    mp_obj_t data = call_frozen(fetch, 0, NULL);

    mp_buffer_info_t bi;
    mp_get_buffer_raise(data, &bi, MP_BUFFER_READ);
    if (bi.len > MRL_MAX_PAYLOAD) {
        mp_raise_ValueError(MP_ERROR_TEXT("payload > 255 bytes"));
    }
    mrl_err_t e = mrl_rec_event((uint8_t)type, bi.buf, (uint8_t)bi.len);
    if (rec_fatal(e)) {
        replay_raise(e);
    }
    return data;
}
static MP_DEFINE_CONST_FUN_OBJ_2(replay__io_obj, replay__io);

/* replay._act(fn, *args): a side effect on the hardware (GPIO write, UART write...).
 *   OFF:    fn(*args).
 *   RECORD: fn(*args) with the clock frozen.
 *   REPLAY: fn is NOT called (replay never drives the outputs); returns None. */
static mp_obj_t replay__act(size_t n_args, const mp_obj_t *args) {
    if (mrl_mode == MRL_MODE_OFF) {
        return mp_call_function_n_kw(args[0], n_args - 1, 0, args + 1);
    }
    if (mrl_mode == MRL_MODE_REPLAY) {
        if (mrl_replay_take_io_exc()) {
            mp_sched_keyboard_interrupt();
            mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        }
        return mp_const_none;
    }
    return call_frozen(args[0], n_args - 1, args + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR(replay__act_obj, 1, replay__act);

/* replay._irq(handler_id): called at the start of a recorded callback. */
static mp_obj_t replay__irq(mp_obj_t id_in) {
    if (mrl_mode == MRL_MODE_RECORD) {
        uint8_t id = (uint8_t)mp_obj_get_int(id_in);
        mrl_err_t e = mrl_rec_event(MRL_EV_IRQ_SCHED, &id, 1);
        if (rec_fatal(e)) {
            replay_raise(e);
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(replay__irq_obj, replay__irq);

/* replay._set_dispatch(fn): fn(handler_id) is scheduled during replay. */
static mp_obj_t replay__set_dispatch(mp_obj_t fn) {
    MP_STATE_VM(replay_dispatch) = fn;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(replay__set_dispatch_obj, replay__set_dispatch);

#if MPY_REPLAY_TEST
/* Test only: SIGUSR1 behaves like a hardware IRQ (it calls mp_sched_schedule
 * from asynchronous context, exactly as an ISR does). */
static void sigusr1_handler(int sig) {
    (void)sig;
    if (MP_STATE_VM(replay_test_irq) != MP_OBJ_NULL) {
        mp_sched_schedule(MP_STATE_VM(replay_test_irq), MP_OBJ_NEW_SMALL_INT(0));
    }
}

static mp_obj_t replay__test_irq(mp_obj_t fn) {
    MP_STATE_VM(replay_test_irq) = fn;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(replay__test_irq_obj, replay__test_irq);
#endif

static const mp_rom_map_elem_t replay_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_replay) },
    { MP_ROM_QSTR(MP_QSTR_start), MP_ROM_PTR(&replay_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop), MP_ROM_PTR(&replay_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_about), MP_ROM_PTR(&replay_about_obj) },
    { MP_ROM_QSTR(MP_QSTR_mode), MP_ROM_PTR(&replay_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_clock), MP_ROM_PTR(&replay_clock_obj) },
    { MP_ROM_QSTR(MP_QSTR_mark), MP_ROM_PTR(&replay_mark_obj) },
    { MP_ROM_QSTR(MP_QSTR_checkpoint), MP_ROM_PTR(&replay_checkpoint_obj) },
    { MP_ROM_QSTR(MP_QSTR_crash), MP_ROM_PTR(&replay_crash_obj) },
    { MP_ROM_QSTR(MP_QSTR_has_log), MP_ROM_PTR(&replay_has_log_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&replay_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_ms), MP_ROM_PTR(&replay_ticks_ms_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks_us), MP_ROM_PTR(&replay_ticks_us_obj) },
    { MP_ROM_QSTR(MP_QSTR__io), MP_ROM_PTR(&replay__io_obj) },
    { MP_ROM_QSTR(MP_QSTR__irq), MP_ROM_PTR(&replay__irq_obj) },
    { MP_ROM_QSTR(MP_QSTR__act), MP_ROM_PTR(&replay__act_obj) },
    #if MPY_REPLAY_TEST
    { MP_ROM_QSTR(MP_QSTR__test_irq), MP_ROM_PTR(&replay__test_irq_obj) },
    #endif
    { MP_ROM_QSTR(MP_QSTR__set_dispatch), MP_ROM_PTR(&replay__set_dispatch_obj) },
    { MP_ROM_QSTR(MP_QSTR_OFF), MP_ROM_INT(MRL_MODE_OFF) },
    { MP_ROM_QSTR(MP_QSTR_RECORD), MP_ROM_INT(MRL_MODE_RECORD) },
    { MP_ROM_QSTR(MP_QSTR_REPLAY), MP_ROM_INT(MRL_MODE_REPLAY) },
    { MP_ROM_QSTR(MP_QSTR_ADC), MP_ROM_INT(MRL_EV_ADC) },
    { MP_ROM_QSTR(MP_QSTR_GPIO), MP_ROM_INT(MRL_EV_GPIO) },
    { MP_ROM_QSTR(MP_QSTR_UART), MP_ROM_INT(MRL_EV_UART_RX) },
    { MP_ROM_QSTR(MP_QSTR_I2C), MP_ROM_INT(MRL_EV_I2C_RD) },
    { MP_ROM_QSTR(MP_QSTR_SPI), MP_ROM_INT(MRL_EV_SPI_RD) },
    { MP_ROM_QSTR(MP_QSTR_RNG), MP_ROM_INT(MRL_EV_RNG) },
};
static MP_DEFINE_CONST_DICT(replay_globals, replay_globals_table);

const mp_obj_module_t replay_user_cmodule = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&replay_globals,
};

MP_REGISTER_MODULE(MP_QSTR_replay, replay_user_cmodule);
