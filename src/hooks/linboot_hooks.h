/**
 * @file
 * @brief Hooks for the integrator of the bootloader.
 *
 * All hooks have weak default implementations (hooks_default.c). Override them by adding a source
 * file with your own implementations to LINBOOT_EXTRA_SOURCES.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stdint.h>

/// Returns the hardware version of the device (manufacturer specific), reported to the updater so
/// that it can choose a compatible firmware. Read it however the board needs: version resistors,
/// an ADC voltage divider, an EEPROM, ... Called once at startup, after linboot_hook_board_init().
/// The default returns LINBOOT_HW_VERSION_DEFAULT.
uint16_t linboot_hook_hardware_version(void);

/// Board specific initialization, called once at startup after the clocks have been set up and
/// before the LIN interface is started (e.g. to enable a LIN transceiver or power supplies). The
/// default does nothing.
void linboot_hook_board_init(void);

/// Called right before the bootloader starts the application, e.g. to undo what
/// linboot_hook_board_init() did. The default does nothing.
void linboot_hook_before_boot(void);
