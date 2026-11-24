/**
 * @file
 * @brief Port interface: everything the bootloader core needs from the platform.
 *
 * A port (src/port/<name>/ and cmake/ports/<name>.cmake) implements these functions, the
 * minlin_hw_*() functions of minlin/minlin_hw.h, the startup code and the vector table. The
 * receive interrupt of the LIN UART feeds every received byte to linboot_lin_rx_byte().
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Initializes the clocks, the millisecond tick and anything else the port needs before the LIN
/// interface is started. Called first thing from main().
void port_init(void);

/// Returns a free-running millisecond tick count.
uint32_t port_tick_ms(void);

/// Refreshes the watchdog, in case it is running (e.g. a hardware watchdog enabled by an option
/// byte). Called from the main loop.
void port_watchdog_refresh(void);

/// Starts the LIN interface (UART in LIN mode) at LINBOOT_BAUD, with the receive interrupt enabled.
void port_lin_init(void);

/// Erases the flash page starting at @p addr. Returns false on failure.
bool port_flash_erase_page(uintptr_t addr);

/// Programs @p len bytes of @p data to the (erased) flash at @p addr. Both @p addr and @p len are
/// multiples of LINBOOT_FLASH_WRITE_ALIGN. Returns false on failure (including a read-back
/// mismatch).
bool port_flash_program(uintptr_t addr, const uint8_t* data, uint32_t len);

/// De-initializes everything the bootloader has set up and starts the application at
/// @p app_addr (its vector table). Does not return.
_Noreturn void port_boot_app(uintptr_t app_addr);

/// Called by the port's LIN UART receive interrupt for every correctly received byte (bytes with
/// framing or noise errors, like the BREAK character itself, are dropped by the port). The port
/// reports the BREAK via minlin_hw_check_and_clear_break() while this is being called.
void linboot_lin_rx_byte(uint8_t received_byte);
