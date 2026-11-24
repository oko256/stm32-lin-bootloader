/**
 * @file
 * @brief Bootloader LIN protocol: frame layouts, codes and byte order helpers.
 *
 * All bootloader frames are 8 bytes, multi-byte fields are little-endian. Image data is carried in
 * chunks of 7 bytes: a DATA frame is [seq][7 image bytes], where chunk k covers image offset 7*k
 * and seq is the low byte of k. See README.md for the full protocol description.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include <stdint.h>

/// Version of this protocol, reported in the STATUS flags (RESPONDER) and IDENTIFY (COMMANDER).
#define PROTO_VERSION 1u

/// Payload size of all bootloader frames.
#define PROTO_FRAME_SIZE     8u
/// Image bytes per DATA frame.
#define PROTO_CHUNK_SIZE     7u
/// Size of the CRC32 trailer that follows the image in the COMMANDER role data stream.
#define PROTO_TRAILER_SIZE   4u
/// Largest image size that fits in the 24-bit size fields.
#define PROTO_MAX_IMAGE_SIZE 0xFFFFFFu

/// Offer flag: install even if the version is the same as the installed one.
#define PROTO_OFFER_FLAG_FORCE 0x01u

/// RESPONDER role: STATUS flags (IDLE/REJECTED layout): a valid application is installed.
#define PROTO_STATUS_FLAG_APP_VALID      0x01u
/// RESPONDER role: STATUS flags: protocol version in the high nibble.
#define PROTO_STATUS_FLAGS_VERSION_SHIFT 4u

/// RESPONDER role: state byte (first byte) of the STATUS frame.
typedef enum {
    /// Waiting for an offer: [state][device_type:2][hw_version:2][fw_version:2][flags].
    PROTO_STATE_IDLE      = 0x01,
    /// The last offer was not taken (same version): layout as in IDLE.
    PROTO_STATE_REJECTED  = 0x02,
    /// Receiving the image: [state][error][next_offset:3][window_end:3].
    PROTO_STATE_RECEIVING = 0x10,
    /// Verifying the received image: layout as in RECEIVING.
    PROTO_STATE_VERIFYING = 0x11,
    /// Update done, booting soon: [state][error][fw_version:2][0...].
    PROTO_STATE_DONE      = 0x20,
    /// Update failed: layout as in DONE (fw_version is the installed one).
    PROTO_STATE_FAILED    = 0x21,
} proto_state;

/// RESPONDER role: command code (first byte) of the CMD frame.
typedef enum {
    /// [cmd][fw_version:2][image_size:3][flags][0]
    PROTO_CMD_OFFER  = 0x01,
    /// [cmd][crc32:4][0...]: all data sent, verify the image against the CRC.
    PROTO_CMD_FINISH = 0x02,
    /// [cmd][0...]: abandon the transfer.
    PROTO_CMD_ABORT  = 0x03,
    /// [cmd][0...]: boot the application now (if valid).
    PROTO_CMD_BOOT   = 0x04,
} proto_cmd;

/// COMMANDER role: request code (first byte) of the QUERY frame.
typedef enum {
    /// [req][device_type:2][hw_version:2][fw_version:2][flags]: who has firmware for me?
    PROTO_QUERY_IDENTIFY = 0x01,
    /// [req][provider_index][offset:3][0...]: selected provider, start serving DATA from offset.
    PROTO_QUERY_READ     = 0x02,
    /// [req][provider_index][error][0...]: result of the update (informational).
    PROTO_QUERY_RESULT   = 0x03,
} proto_query;

/// Error codes (RESPONDER STATUS error byte, COMMANDER RESULT error byte).
typedef enum {
    PROTO_ERR_NONE      = 0x00,
    PROTO_ERR_BAD_SIZE  = 0x01, ///< Offered image is empty or doesn't fit.
    PROTO_ERR_FLASH     = 0x02, ///< Erasing or programming the flash failed.
    PROTO_ERR_CRC       = 0x03, ///< The written image doesn't match the CRC.
    PROTO_ERR_BAD_STATE = 0x04, ///< Command not valid in the current state (e.g. FINISH too early).
    PROTO_ERR_TIMEOUT   = 0x05, ///< Provider stopped responding.
} proto_error;

/// Writes @p value as 2 little-endian bytes.
static inline void proto_put_u16(uint8_t* dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

/// Writes the low 24 bits of @p value as 3 little-endian bytes.
static inline void proto_put_u24(uint8_t* dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
}

/// Writes @p value as 4 little-endian bytes.
static inline void proto_put_u32(uint8_t* dst, uint32_t value)
{
    proto_put_u24(dst, value);
    dst[3] = (uint8_t)(value >> 24);
}

/// Reads 2 little-endian bytes.
static inline uint16_t proto_get_u16(const uint8_t* src) { return (uint16_t)(src[0] | (src[1] << 8)); }

/// Reads 3 little-endian bytes.
static inline uint32_t proto_get_u24(const uint8_t* src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16);
}

/// Reads 4 little-endian bytes.
static inline uint32_t proto_get_u32(const uint8_t* src) { return proto_get_u24(src) | ((uint32_t)src[3] << 24); }
