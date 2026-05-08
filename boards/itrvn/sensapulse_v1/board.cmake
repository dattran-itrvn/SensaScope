# SPDX-License-Identifier: Apache-2.0
#
# Flash/debug runners for SensaPulse v1.0.
# Use J-Link via nrfjprog (default) or JLinkExe.

board_runner_args(jlink "--device=nrf52840" "--speed=4000")
board_runner_args(nrfjprog "--nrf-family=NRF52")

include(${ZEPHYR_BASE}/boards/common/nrfjprog.board.cmake)
include(${ZEPHYR_BASE}/boards/common/nrfutil.board.cmake)
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
