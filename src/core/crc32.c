/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "crc32.h"

// Half-byte (nibble) table: small (64 bytes of flash) and still fast enough to verify the whole
// application on every boot (roughly 20 cycles per byte on a Cortex-M0).
static const uint32_t s_table[16] = {
    0x00000000u,
    0x1DB71064u,
    0x3B6E20C8u,
    0x26D930ACu,
    0x76DC4190u,
    0x6B6B51F4u,
    0x4DB26158u,
    0x5005713Cu,
    0xEDB88320u,
    0xF00F9344u,
    0xD6D6A3E8u,
    0xCB61B38Cu,
    0x9B64C2B0u,
    0x86D3D2D4u,
    0xA00AE278u,
    0xBDBDF21Cu,
};

uint32_t crc32(const void* data, size_t len)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t       crc   = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= bytes[i];
        crc = s_table[crc & 0x0Fu] ^ (crc >> 4);
        crc = s_table[crc & 0x0Fu] ^ (crc >> 4);
    }
    return crc ^ 0xFFFFFFFFu;
}
