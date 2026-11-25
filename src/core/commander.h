/**
 * @file
 * @brief COMMANDER role: the bootloader is the bus commander and pulls its update from a provider.
 *
 * The bootloader broadcasts its identity in a QUERY frame (IDENTIFY) and polls the OFFER frame of
 * each possible provider. If one of them has a suitable image, the bootloader selects it with a
 * READ request and polls the DATA frame for the image chunks, followed by a CRC32 trailer.
 *
 * Every bus transaction is a header sent with linboot_lin_send_header(), followed by a response
 * from a provider (linboot_commander_on_frame()) or by this node itself
 * (linboot_commander_on_publish()), or a timeout after the frame slot.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include "common.h"
#include "image.h"
#include "linboot_config.h"
#include "proto.h"
#include <stdbool.h>
#include <stdint.h>

/// Frame slot (ms): the maximum time of a frame with 8 data bytes (1.4 * 124 bit times), rounded
/// up, plus one tick of margin.
#define LINBOOT_SLOT_MS (((1736u * 100u) + LINBOOT_BAUD - 1u) / LINBOOT_BAUD + 1u)

/// How many times RESULT is sent: it's only informational, but a provider may want to know when it
/// can stop, and there's no acknowledgement.
#define LINBOOT_RESULT_REPEATS 3u

/// Phase of the COMMANDER role state machine; each phase is one bus transaction.
typedef enum {
    LINBOOT_COMMANDER_IDENTIFY,  ///< Broadcast IDENTIFY.
    LINBOOT_COMMANDER_OFFER,     ///< Poll the OFFER of provider offer_index.
    LINBOOT_COMMANDER_READ,      ///< Send READ to the selected provider.
    LINBOOT_COMMANDER_POLL_DATA, ///< Poll DATA for the next chunk.
    LINBOOT_COMMANDER_RESULT,    ///< Send RESULT, then boot or retry.
} linboot_commander_phase;

/// An offer received from a provider.
typedef struct {
    bool     valid;   ///< The provider has offered an image.
    uint8_t  flags;   ///< PROTO_OFFER_FLAG_*
    uint16_t version; ///< Offered firmware version.
    uint32_t size;    ///< Offered image size.
} linboot_offer;

/// COMMANDER role state.
typedef struct {
    linboot_identity        identity; ///< Our identity (validity updates as the old app is erased).
    uint32_t                start_ms; ///< When the bootloader started (for the boot timeout).
    linboot_commander_phase phase;    ///< Current phase.

    // Current transaction.
    bool     in_transaction; ///< A header has been sent and the slot hasn't ended yet.
    uint8_t  tx_id;          ///< ID of the header.
    uint32_t tx_start_ms;    ///< When the header was sent.
    bool     rx_done;        ///< A response was received.
    bool     published;      ///< We published our own response.
    uint8_t  rx[PROTO_FRAME_SIZE];

    // Discovery.
    uint8_t       offer_index;
    linboot_offer offers[LINBOOT_PROVIDER_COUNT];
    /// The current round confirms the offers of the previous one (both rounds count).
    bool          confirming;
    /// Providers whose transfer failed (bit per provider index): skipped while others offer.
    uint16_t      failed_providers;

    // Transfer.
    uint8_t     provider;     ///< Index of the selected provider.
    uint16_t    version;      ///< Version being installed.
    uint32_t    stream_size;  ///< Image size plus the CRC trailer.
    uint32_t    chunk;        ///< Next chunk to receive.
    uint32_t    read_chunk;   ///< Chunk of the last READ (UINT32_MAX: none yet).
    uint8_t     poll_retries; ///< DATA polls without progress since the last READ.
    uint8_t     read_retries; ///< READs without progress.
    uint8_t     attempts;     ///< Failed update attempts.
    proto_error result;       ///< Result of the transfer, sent in RESULT.
    uint8_t     result_sends; ///< RESULT frames sent.
    uint8_t     trailer[PROTO_TRAILER_SIZE];

    image_writer image; ///< Writer of the received image.
} linboot_commander;

/// Initializes the state machine. @p layout must outlive the state machine.
void linboot_commander_init(
    linboot_commander* self, const linboot_layout* layout, const linboot_identity* identity, uint32_t now_ms);

/// A subscribed frame (OFFER or DATA) with an 8-byte payload was received.
void linboot_commander_on_frame(linboot_commander* self, uint8_t id, const uint8_t* payload, uint32_t now_ms);

/// The header of our own published frame (QUERY) was received: fills in the 8-byte
/// @p out_payload. Returns false if nothing should be sent.
bool linboot_commander_on_publish(linboot_commander* self, uint8_t id, uint8_t* out_payload, uint32_t now_ms);

/// Runs the bus transactions and decides about booting. Call from the main loop.
linboot_action linboot_commander_poll(linboot_commander* self, uint32_t now_ms);
