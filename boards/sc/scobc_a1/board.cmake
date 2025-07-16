# SPDX-License-Identifier: Apache-2.0

board_runner_args(openocd "--config=${BOARD_DIR}/support/scobc-a1.cfg")
board_runner_args(openocd --cmd-post-verify "fpga_power_cycle")
set(OPENOCD "${WEST_TOPDIR}/modules/tools/openocd/bin/openocd" CACHE FILEPATH "" FORCE)

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
