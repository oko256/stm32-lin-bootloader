/**
 * @file
 * @brief RESPONDER role: the bootloader is updated by the LIN bus commander.
 *
 * The node has three frames: STATUS (published; its header is the heartbeat of the commander),
 * CMD and DATA (subscribed). The state machine is driven by the LIN frame callbacks and polled from
 * the main loop, which also does the slow flash operations.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include "common.h"
#include "image.h"
#include "proto.h"
#include <stdbool.h>
#include <stdint.h>

/// RESPONDER role state.
typedef struct {
    linboot_identity identity; ///< Reported identity (firmware version updates on success).
    proto_state      state; ///< Current protocol state.
    proto_error      error; ///< Last error.
    uint16_t         offered_version; ///< Version of the image being received.
    uint32_t         finish_crc; ///< CRC from FINISH, verified in the poll.
    uint8_t          next_seq; ///< Sequence number of the next expected chunk.
    bool             session_started; ///< A frame for this node has been seen.
    bool             boot_requested; ///< BOOT command received.
    uint32_t         start_ms; ///< When the bootloader started (for the boot timeout).
    uint32_t         last_activity_ms; ///< Last frame for this node (for the session timeout).
    uint32_t         done_ms; ///< When the update completed (for the boot delay).
    image_writer     image; ///< Writer of the received image.
} linboot_responder;

/// Initializes the state machine. @p layout must outlive the state machine.
void linboot_responder_init(
    linboot_responder* self, const linboot_layout* layout, const linboot_identity* identity, uint32_t now_ms);

/// A subscribed frame (CMD or DATA) with an 8-byte payload was received.
void linboot_responder_on_frame(linboot_responder* self, uint8_t id, const uint8_t* payload, uint32_t now_ms);

/// A header of a published frame (STATUS) was received: fills in the 8-byte @p out_payload.
/// Returns false if nothing should be sent.
bool linboot_responder_on_publish(linboot_responder* self, uint8_t id, uint8_t* out_payload, uint32_t now_ms);

/// Does the pending (slow) work and decides about booting. Call from the main loop.
linboot_action linboot_responder_poll(linboot_responder* self, uint32_t now_ms);
