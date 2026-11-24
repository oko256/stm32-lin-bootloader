/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "hooks/linboot_hooks.h"
#include "linboot_config.h"

__attribute__((weak)) uint16_t linboot_hook_hardware_version(void) { return LINBOOT_HW_VERSION_DEFAULT; }

__attribute__((weak)) void linboot_hook_board_init(void) { }

__attribute__((weak)) void linboot_hook_before_boot(void) { }
