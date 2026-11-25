/**
 * @file
 * @brief Firmware metadata record: version, size and CRC of the installed application.
 *
 * The record lives at the start of its own flash page. It is erased before the first block of a
 * new image is written and written only after the whole image has been verified, so a valid record
 * always describes a complete application.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include "common.h"
#include <stdbool.h>
#include <stdint.h>

/// Magic number of the record: "LBM1" in memory.
#define META_MAGIC 0x314D424Cu

/// The metadata record as stored in flash.
typedef struct {
    uint32_t magic;            ///< META_MAGIC
    uint16_t firmware_version; ///< Version of the application (major << 8 | minor).
    uint16_t reserved0;        ///< 0xFFFF
    uint32_t image_size;       ///< Size of the application image in bytes.
    uint32_t image_crc;        ///< CRC32 of the application image.
    uint32_t reserved1;        ///< 0xFFFFFFFF
    uint32_t record_crc;       ///< CRC32 of the fields above.
} meta_record;

/// Reads the record at @p meta_addr into @p out_record. Returns true if the record is intact (magic
/// and record CRC match); the image itself is not checked.
bool meta_read(uintptr_t meta_addr, meta_record* out_record);

/// Checks that @p record describes a valid application in @p layout: the size fits, and if
/// @p verify_image is set, the CRC of the image in flash matches.
bool meta_image_valid(const meta_record* record, const linboot_layout* layout, bool verify_image);

/// Reads the record from the layout and fills in the firmware version and the application validity
/// of @p identity.
void meta_load_identity(const linboot_layout* layout, bool verify_image, linboot_identity* identity);

/// Writes a new record to the (erased) metadata page. Returns false on flash failure.
bool meta_write(uintptr_t meta_addr, uint16_t firmware_version, uint32_t image_size, uint32_t image_crc);
