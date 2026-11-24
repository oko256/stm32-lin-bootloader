/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "image.h"
#include "crc32.h"
#include "port/port.h"
#include <string.h>

/// Erased flash value, used for padding the last block.
#define ERASED_BYTE 0xFFu

void image_init(image_writer* self, const linboot_layout* layout)
{
    self->layout       = layout;
    self->size         = 0;
    self->received     = 0;
    self->block_offset = 0;
    self->fill         = 0;
    self->meta_erased  = false;
}

bool image_begin(image_writer* self, uint32_t size)
{
    if (size == 0 || size > self->layout->app_max_size) {
        return false;
    }
    self->size         = size;
    self->received     = 0;
    self->block_offset = 0;
    self->fill         = 0;
    return true;
}

uint32_t image_window_end(const image_writer* self)
{
    if (image_commit_needed(self)) {
        return self->received;
    }
    return self->block_offset + LINBOOT_BLOCK_SIZE;
}

bool image_put(image_writer* self, const uint8_t* data, uint32_t len)
{
    if (self->size == 0 || image_commit_needed(self) || len > PROTO_CHUNK_SIZE) {
        return false;
    }
    uint32_t take = self->size - self->received;
    if (take > len) {
        take = len;
    }
    memcpy(&self->buf[self->fill], data, take);
    self->fill += take;
    self->received += take;
    return true;
}

bool image_commit_needed(const image_writer* self)
{
    if (self->size == 0) {
        return false;
    }
    return self->fill >= LINBOOT_BLOCK_SIZE || (self->received >= self->size && self->fill > 0);
}

bool image_commit(image_writer* self)
{
    if (!image_commit_needed(self)) {
        return true;
    }
    if (!self->meta_erased) {
        if (!port_flash_erase_page(self->layout->meta_addr)) {
            return false;
        }
        self->meta_erased = true;
    }

    const uintptr_t addr = self->layout->app_addr + self->block_offset;
    if ((self->block_offset & (self->layout->page_size - 1u)) == 0 && !port_flash_erase_page(addr)) {
        return false;
    }

    // A full block, or the last partial block padded up to the write granularity. Only full blocks
    // can have a spill, so the padding never overwrites it.
    const uint32_t len    = self->fill < LINBOOT_BLOCK_SIZE ? self->fill : LINBOOT_BLOCK_SIZE;
    const uint32_t spill  = self->fill - len;
    const uint32_t padded = ((len + LINBOOT_FLASH_WRITE_ALIGN - 1u) / LINBOOT_FLASH_WRITE_ALIGN)
        * LINBOOT_FLASH_WRITE_ALIGN;
    memset(&self->buf[len], ERASED_BYTE, padded - len);
    if (!port_flash_program(addr, self->buf, padded)) {
        return false;
    }
    if (spill > 0) {
        memmove(self->buf, &self->buf[len], spill);
    }
    self->block_offset += LINBOOT_BLOCK_SIZE;
    self->fill = spill;
    return true;
}

uint32_t image_flash_crc(const image_writer* self) { return crc32((const void*)self->layout->app_addr, self->size); }
