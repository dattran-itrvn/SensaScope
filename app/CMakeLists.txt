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
	src/audio.c
	src/sd_log.c
)

target_include_directories(app PRIVATE src)
