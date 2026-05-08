# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Tell Zephyr where to find our custom out-of-tree board
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensapulse_fw LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/led.c
	src/battery.c
	src/imu.c
	src/imu_sampler.c
	src/audio.c
	src/sd_log.c
	src/session.c
)

target_include_directories(app PRIVATE src)

# Embed git short SHA into firmware as FW_BUILD_HASH (used in meta.json).
# Falls back to "unknown" if not in a git checkout.
execute_process(
	COMMAND git rev-parse --short HEAD
	WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
	OUTPUT_VARIABLE FW_GIT_HASH
	OUTPUT_STRIP_TRAILING_WHITESPACE
	ERROR_QUIET
)
if(NOT FW_GIT_HASH)
	set(FW_GIT_HASH "unknown")
endif()
target_compile_definitions(app PRIVATE FW_BUILD_HASH="${FW_GIT_HASH}")
