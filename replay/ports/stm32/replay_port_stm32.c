/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 *
 * STM32 port adapter: the log lives in the 4 KB backup SRAM (BKPSRAM), which
 * keeps its content across resets and, with VBAT supplied, across power loss.
 *
 * STATUS: untested on hardware (v0.1). Written for STM32F4/F7 devices that have
 * BKPSRAM. Other families need their own clock/power enable sequence.
 *
 * Build with: -DMRL_FLUSH_EVERY_EVENT=1 so that each event is written straight
 * to backup SRAM and survives a hard fault.
 */
#include <string.h>
#include "py/mphal.h"
#include STM32_HAL_H
#include "replay_port.h"

#if !defined(BKPSRAM_BASE) || !(defined(STM32F4) || defined(STM32F7))
#error "replay_port_stm32.c: backup SRAM adapter implemented for STM32F4/F7 only"
#endif

#ifndef MRL_STM32_REQUIRE_BKUP_REG
#define MRL_STM32_REQUIRE_BKUP_REG 1
#endif

#define BKP_SIZE 4096u

static bool ready;

bool replay_port_storage_init(size_t *capacity) {
    if (!ready) {
        __HAL_RCC_PWR_CLK_ENABLE();
        HAL_PWR_EnableBkUpAccess();
        __HAL_RCC_BKPSRAM_CLK_ENABLE();
        /* Keep the content when only VBAT is present: enable the backup
         * regulator and wait for it (the HAL has its own timeout and reports
         * HAL_TIMEOUT). A plain reset does not need it, so with
         * -DMRL_STM32_REQUIRE_BKUP_REG=0 a failure is tolerated; by default
         * initialization fails so that the retention is never silently absent. */
        if (HAL_PWREx_EnableBkUpReg() != HAL_OK && MRL_STM32_REQUIRE_BKUP_REG) {
            return false;
        }
        ready = true;
    }
    *capacity = BKP_SIZE;
    return true;
}

bool replay_port_storage_reset(void) {
    memset((void *)BKPSRAM_BASE, 0, BKP_SIZE);
    return true;
}

bool replay_port_storage_write(size_t off, const void *src, size_t n) {
    if (off > BKP_SIZE || n > BKP_SIZE - off) {
        return false;
    }
    memcpy((uint8_t *)BKPSRAM_BASE + off, src, n);
    return true;
}

bool replay_port_storage_read(size_t off, void *dst, size_t n) {
    if (off > BKP_SIZE || n > BKP_SIZE - off) {
        return false;
    }
    memcpy(dst, (const uint8_t *)BKPSRAM_BASE + off, n);
    return true;
}

bool replay_port_storage_flush(void) {
    return true; /* SRAM: nothing to flush */
}

uint32_t replay_port_board_id(void) {
    return HAL_GetDEVID();
}

uint32_t replay_port_heap_size(void) {
    extern uint32_t _heap_end, _heap_start;
    return (uint32_t)((uintptr_t)&_heap_end - (uintptr_t)&_heap_start);
}
