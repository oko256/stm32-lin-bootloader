/**
 * @file
 * @brief CRC-32/ISO-HDLC (the "zlib" CRC32, same as Python's zlib.crc32()).
 *
 * Polynomial 0x04C11DB7 (reflected 0xEDB88320), initial value and final XOR 0xFFFFFFFF.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/// Returns the CRC32 of @p len bytes at @p data.
uint32_t crc32(const void* data, size_t len);
