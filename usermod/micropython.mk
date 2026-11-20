# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
#
# Usage:  make -C ports/<port> USER_C_MODULES=/path/to/mpy-replay MPY_REPLAY_PORT=<unix|stm32>
#         add MPY_REPLAY_MODE=1 for a replay build (hosted ports only)

MPY_REPLAY_DIR := $(abspath $(USERMOD_DIR)/..)
MPY_REPLAY_PORT ?= unix
MPY_REPLAY_MODE ?= 0
MPY_REPLAY_TEST ?= 0

SRC_USERMOD_C += $(MPY_REPLAY_DIR)/usermod/modreplay.c
SRC_USERMOD_C += $(addprefix $(MPY_REPLAY_DIR)/replay/core/, mrl_util.c mrl_record.c mrl_reader.c mrl_replay.c)
SRC_USERMOD_C += $(MPY_REPLAY_DIR)/replay/ports/$(MPY_REPLAY_PORT)/replay_port_$(MPY_REPLAY_PORT).c

CFLAGS_USERMOD += -I$(MPY_REPLAY_DIR)/usermod -I$(MPY_REPLAY_DIR)/replay/core
CFLAGS_USERMOD += -include $(MPY_REPLAY_DIR)/usermod/mpy_replay_hooks.h
CFLAGS_USERMOD += '-DMICROPY_VM_HOOK_LOOP=mpy_replay_hook_loop();'
CFLAGS_USERMOD += -DMPY_REPLAY_MODE=$(MPY_REPLAY_MODE) -DMPY_REPLAY_TEST=$(MPY_REPLAY_TEST)
ifneq ($(MPY_REPLAY_FW_HASH),)
ifeq ($(shell printf '%s' '$(MPY_REPLAY_FW_HASH)' | grep -Ec '^[0-9a-fA-F]{40}$$'),0)
$(error MPY_REPLAY_FW_HASH must be exactly 40 hexadecimal characters, got '$(MPY_REPLAY_FW_HASH)')
endif
CFLAGS_USERMOD += '-DMPY_REPLAY_FW_HASH_HEX="$(MPY_REPLAY_FW_HASH)"'
endif

# STM32: write every event straight to backup SRAM
ifeq ($(MPY_REPLAY_PORT),stm32)
CFLAGS_USERMOD += -DMRL_FLUSH_EVERY_EVENT=1
endif
