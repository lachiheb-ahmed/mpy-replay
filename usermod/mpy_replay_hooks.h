/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * Force-included in every translation unit by micropython.mk / micropython.cmake
 * so that py/vm.c sees MICROPY_VM_HOOK_LOOP without editing any port file.
 */
#ifndef MPY_REPLAY_HOOKS_H
#define MPY_REPLAY_HOOKS_H

#include <stdint.h>
#include "mrl_core.h"

/* Slow path, only entered when recording or replaying. */
void mpy_replay_vm_slow(void);

/* Executed on every VM branch (py/vm.c: pending_exception_check). */
static inline void mpy_replay_hook_loop(void) {
    mrl_lclk++;
    if (mrl_mode != MRL_MODE_OFF) {
        mpy_replay_vm_slow();
    }
}

/* Records (or replays) a clock value; returns what the program must see. */
uint64_t mpy_replay_ticks(uint8_t type, uint64_t real);

#define MPY_REPLAY_TICKS_MS(real)  mpy_replay_ticks(MRL_EV_TICKS_MS, (real))
#define MPY_REPLAY_TICKS_US(real)  mpy_replay_ticks(MRL_EV_TICKS_US, (real))
#define MPY_REPLAY_TICKS_CPU(real) mpy_replay_ticks(MRL_EV_TICKS_CPU, (real))

#endif /* MPY_REPLAY_HOOKS_H */
