# SPDX-License-Identifier: Apache-2.0
#
# Standalone SD-stress firmware. Builds against the same custom board as the
# main app (via BOARD_ROOT) but pulls only the sd_log module — no PDM, no
# I²C, no BLE, no FSM, no LED. Goal: isolate SD R/W behaviour from the rest
# of the system so we can answer "is the empty-session bug from the SD path
# alone, or from interaction with audio/IMU/watchdog?" deterministically.
cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(sensapulse_sd_stress LANGUAGES C)

target_sources(app PRIVATE
    src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/sd_log.c
    # Production data-path modules — pulled in unchanged so phases 5/6
    # exercise the exact same code that runs in the main app session.
    # #25: sd_writer is the single FATFS owner; audio.c + imu_sampler.c
    # are now producers that push into sd_writer's FIFOs.
    ${CMAKE_CURRENT_LIST_DIR}/../src/audio.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/imu.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/imu_sampler.c
    ${CMAKE_CURRENT_LIST_DIR}/../src/sd_writer.c
)

target_include_directories(app PRIVATE
    src
    ${CMAKE_CURRENT_LIST_DIR}/../src
)
