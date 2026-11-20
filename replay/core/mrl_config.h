/* SPDX-License-Identifier: MIT
 * mpy-replay - deterministic record & replay for MicroPython
 * Copyright (c) 2026 Ahmed (lachiheb-ahmed)
 */
#ifndef MRL_CONFIG_H
#define MRL_CONFIG_H

/* RAM staging buffer used by the recorder. Must be >= MRL_MAX_RECORD. */
#ifndef MRL_STAGE_SIZE
#define MRL_STAGE_SIZE 512
#endif

/* 1 = write every event straight to storage (survives a hard fault, slower).
 * 0 = stage in RAM and flush when full or on mrl_rec_flush(). */
#ifndef MRL_FLUSH_EVERY_EVENT
#define MRL_FLUSH_EVERY_EVENT 0
#endif

#endif /* MRL_CONFIG_H */
