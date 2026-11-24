/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "meta.h"
#include "crc32.h"
#include "linboot_config.h"
#include "port/port.h"
#include <stddef.h>
#include <string.h>

/// Size of the record rounded up to the flash write granularity.
#define META_WRITE_SIZE                                                                                                \
    (((sizeof(meta_record) + LINBOOT_FLASH_WRITE_ALIGN - 1u) / LINBOOT_FLASH_WRITE_ALIGN) * LINBOOT_FLASH_WRITE_ALIGN)

bool meta_read(uintptr_t meta_addr, meta_record* out_record)
{
    memcpy(out_record, (const void*)meta_addr, sizeof(meta_record));
    return out_record->magic == META_MAGIC
        && out_record->record_crc == crc32(out_record, offsetof(meta_record, record_crc));
}

bool meta_image_valid(const meta_record* record, const linboot_layout* layout, bool verify_image)
{
    if (record->image_size == 0 || record->image_size > layout->app_max_size) {
        return false;
    }
    if (verify_image && crc32((const void*)layout->app_addr, record->image_size) != record->image_crc) {
        return false;
    }
    return true;
}

void meta_load_identity(const linboot_layout* layout, bool verify_image, linboot_identity* identity)
{
    meta_record record;
    if (meta_read(layout->meta_addr, &record) && meta_image_valid(&record, layout, verify_image)) {
        identity->firmware_version = record.firmware_version;
        identity->app_valid        = true;
    } else {
        identity->firmware_version = 0;
        identity->app_valid        = false;
    }
}

bool meta_write(uintptr_t meta_addr, uint16_t firmware_version, uint32_t image_size, uint32_t image_crc)
{
    // Word aligned buffer, padded with the erased flash value.
    union {
        meta_record record;
        uint8_t     bytes[META_WRITE_SIZE];
    } buf;
    memset(&buf, 0xFF, sizeof(buf));
    buf.record.magic            = META_MAGIC;
    buf.record.firmware_version = firmware_version;
    buf.record.image_size       = image_size;
    buf.record.image_crc        = image_crc;
    buf.record.record_crc       = crc32(&buf.record, offsetof(meta_record, record_crc));
    return port_flash_program(meta_addr, buf.bytes, sizeof(buf.bytes));
}
