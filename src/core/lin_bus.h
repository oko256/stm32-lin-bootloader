/**
 * @file
 * @brief LIN bus access for the COMMANDER role state machine.
 *
 * Implemented by the LIN glue in linboot.c (with minlin), and by a simulated bus in the unit tests.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Transmits a frame header (BREAK, SYNC, PID) for @p id. The response comes back through the
/// frame callbacks of the role state machine. Returns false if the transmission failed.
bool linboot_lin_send_header(uint8_t id);
