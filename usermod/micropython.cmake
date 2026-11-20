# SPDX-License-Identifier: MIT
# mpy-replay - Copyright (c) 2026 Ahmed (lachiheb-ahmed)
# For CMake-based ports (rp2, esp32, ...). Untested in v0.1.
# Usage: cmake ... -DUSER_C_MODULES=/path/to/mpy-replay/usermod/micropython.cmake -DMPY_REPLAY_PORT=<port>

get_filename_component(MPY_REPLAY_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT MPY_REPLAY_PORT)
    message(FATAL_ERROR "mpy-replay: set MPY_REPLAY_PORT (a directory under replay/ports/ with a matching adapter)")
endif()

if(MPY_REPLAY_FW_HASH AND NOT MPY_REPLAY_FW_HASH MATCHES "^[0-9a-fA-F]+$")
    message(FATAL_ERROR "mpy-replay: MPY_REPLAY_FW_HASH must be hexadecimal")
endif()
if(MPY_REPLAY_FW_HASH)
    string(LENGTH "${MPY_REPLAY_FW_HASH}" _mrl_hash_len)
    if(NOT _mrl_hash_len EQUAL 40)
        message(FATAL_ERROR "mpy-replay: MPY_REPLAY_FW_HASH must be exactly 40 hexadecimal characters")
    endif()
endif()

add_library(usermod_replay INTERFACE)
target_sources(usermod_replay INTERFACE
    ${MPY_REPLAY_DIR}/usermod/modreplay.c
    ${MPY_REPLAY_DIR}/replay/core/mrl_util.c
    ${MPY_REPLAY_DIR}/replay/core/mrl_record.c
    ${MPY_REPLAY_DIR}/replay/core/mrl_reader.c
    ${MPY_REPLAY_DIR}/replay/core/mrl_replay.c
    ${MPY_REPLAY_DIR}/replay/ports/${MPY_REPLAY_PORT}/replay_port_${MPY_REPLAY_PORT}.c
)
target_include_directories(usermod_replay INTERFACE
    ${MPY_REPLAY_DIR}/usermod
    ${MPY_REPLAY_DIR}/replay/core
)
target_compile_options(usermod_replay INTERFACE
    -include ${MPY_REPLAY_DIR}/usermod/mpy_replay_hooks.h
)
target_compile_definitions(usermod_replay INTERFACE
    "MICROPY_VM_HOOK_LOOP=mpy_replay_hook_loop();"
    MPY_REPLAY_MODE=0
)
if(MPY_REPLAY_FW_HASH)
    target_compile_definitions(usermod_replay INTERFACE "MPY_REPLAY_FW_HASH_HEX=\"${MPY_REPLAY_FW_HASH}\"")
endif()
target_link_libraries(usermod INTERFACE usermod_replay)
