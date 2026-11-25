/**
 * @file
 * @brief Image writer: collects the received image in a RAM block buffer and writes it to flash.
 *
 * The image is received sequentially in small pieces (chunks). They are collected into a block
 * buffer of LINBOOT_BLOCK_SIZE bytes; when the buffer is full (or the whole image has been
 * received), the role state machine commits it to the flash with image_commit(), which may take
 * tens of milliseconds. A piece that starts inside the current block may extend past its end: the
 * excess ("spill") is kept for the next block.
 *
 * The metadata page is erased right before the first block is written, so the old application is
 * considered invalid (in the identity given to image_init()) only once it actually starts to get
 * overwritten.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include "common.h"
#include "linboot_config.h"
#include "proto.h"
#include <stdbool.h>
#include <stdint.h>

/// Image writer state.
typedef struct {
    const linboot_layout* layout;       ///< Where the image goes.
    linboot_identity*     identity;     ///< Invalidated when the old application is erased.
    uint32_t              size;         ///< Size of the image being received.
    uint32_t              received;     ///< Bytes received so far (the next expected offset).
    uint32_t              block_offset; ///< Image offset of the first byte in the buffer.
    uint32_t              fill;         ///< Bytes in the buffer (may exceed the block size by the spill).
    bool                  meta_erased;  ///< The metadata page has been erased (old app invalidated).
    /// Block buffer plus room for the spill of one piece.
    uint8_t               buf[LINBOOT_BLOCK_SIZE + PROTO_CHUNK_SIZE];
} image_writer;

/// Initializes the writer for @p layout. When the old application starts to get overwritten,
/// @p identity is updated to say that there's no valid application. Both must outlive the writer.
void image_init(image_writer* self, const linboot_layout* layout, linboot_identity* identity);

/// Starts receiving a new image of @p size bytes. Returns false (and doesn't start) if the size is
/// zero or doesn't fit in the application area.
bool image_begin(image_writer* self, uint32_t size);

/// Returns the number of image bytes received so far, i.e. the offset of the next expected byte.
static inline uint32_t image_received(const image_writer* self) { return self->received; }

/// Returns the image offset before which a new piece must start to be accepted right now. If this
/// equals image_received(), the buffer is full and must be committed first.
uint32_t image_window_end(const image_writer* self);

/// Appends a piece of at most PROTO_CHUNK_SIZE bytes at offset image_received(). Bytes beyond the
/// image size are ignored. Returns false (and accepts nothing) if the buffer is full.
bool image_put(image_writer* self, const uint8_t* data, uint32_t len);

/// Returns true if the buffer must be committed before more data can be accepted (or to finish).
bool image_commit_needed(const image_writer* self);

/// Writes the buffered data to the flash (one block, or at the end of the image everything that's
/// left), erasing the metadata page first (if not yet erased) and each flash page as a block starts
/// it. Does nothing if no commit is needed. Returns false on a flash failure.
bool image_commit(image_writer* self);

/// Returns the CRC32 of the image as written in the flash.
uint32_t image_flash_crc(const image_writer* self);
