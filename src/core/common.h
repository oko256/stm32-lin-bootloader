/**
 * @file
 * @brief Types shared by the bootloader core modules.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Flash layout of the application and its metadata. Addresses are pointers to memory mapped flash
/// (on the host unit tests, to a RAM buffer simulating the flash).
typedef struct {
    uintptr_t app_addr;     ///< Start of the application (page aligned).
    uint32_t  app_max_size; ///< Maximum size of the application image in bytes.
    uintptr_t meta_addr;    ///< Start of the metadata page.
    uint32_t  page_size;    ///< Flash erase page size in bytes (a power of two).
} linboot_layout;

/// Identity of this device, as reported to the updater.
typedef struct {
    uint16_t device_type;      ///< Compile time device type.
    uint16_t hardware_version; ///< From linboot_hook_hardware_version().
    uint16_t firmware_version; ///< Version of the installed application (0 if none).
    bool     app_valid;        ///< A valid application is installed.
} linboot_identity;

/// What the main loop should do after polling the role state machine.
typedef enum {
    LINBOOT_ACTION_NONE,     ///< Keep running the bootloader.
    LINBOOT_ACTION_BOOT_APP, ///< Start the (valid) application.
} linboot_action;
