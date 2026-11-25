/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "commander.h"
#include "lin_bus.h"
#include "meta.h"
#include <string.h>

/// OFFER frame IDs; the index is the provider index.
static const uint8_t s_offer_ids[LINBOOT_PROVIDER_COUNT] = LINBOOT_PROVIDER_OFFER_IDS;

/// No READ sent yet for the transfer.
#define NO_READ_CHUNK UINT32_MAX

static void s_start_discovery(linboot_commander* self)
{
    self->phase       = LINBOOT_COMMANDER_IDENTIFY;
    self->offer_index = 0;
    self->confirming  = false;
    memset(self->offers, 0, sizeof(self->offers));
}

static void s_send_result(linboot_commander* self, proto_error error)
{
    self->result       = error;
    self->result_sends = 0;
    self->phase        = LINBOOT_COMMANDER_RESULT;
}

/// Chooses an offer: the first one with FORCE, otherwise the highest version (the first of equal
/// ones). Providers whose transfer has failed are skipped, unless they are all that's offered (so
/// that a broken provider can't keep the device from getting a working image from another one).
/// Returns the provider index, or -1 if there's no offer.
static int s_choose_offer(linboot_commander* self)
{
    uint16_t offering = 0;
    for (uint8_t i = 0; i < LINBOOT_PROVIDER_COUNT; ++i) {
        if (self->offers[i].valid) {
            offering |= (uint16_t)(1u << i);
        }
    }
    if ((offering & ~self->failed_providers) == 0) {
        // Everyone who offers has failed: give them all another chance.
        self->failed_providers = 0;
    }

    int best = -1;
    for (uint8_t i = 0; i < LINBOOT_PROVIDER_COUNT; ++i) {
        const linboot_offer* offer = &self->offers[i];
        if (!offer->valid || (self->failed_providers & (1u << i)) != 0) {
            continue;
        }
        if ((offer->flags & PROTO_OFFER_FLAG_FORCE) != 0) {
            return i;
        }
        if (best < 0 || offer->version > self->offers[best].version) {
            best = i;
        }
    }
    return best;
}

/// All offers have been polled: start the transfer, or decide to boot or to keep looking. The first
/// round with offers is followed by another one, and the offers of both count: a provider may start
/// offering only when it sees IDENTIFY, and miss its OFFER slot of the first round.
static linboot_action s_evaluate_offers(linboot_commander* self, uint32_t now_ms)
{
    const int best = s_choose_offer(self);
    if (best >= 0 && !self->confirming) {
        self->confirming  = true;
        self->phase       = LINBOOT_COMMANDER_IDENTIFY;
        self->offer_index = 0;
        return LINBOOT_ACTION_NONE;
    }
    if (best < 0) {
        // Nobody has anything for us (yet).
        if (self->identity.app_valid && now_ms - self->start_ms >= LINBOOT_BOOT_TIMEOUT_MS) {
            return LINBOOT_ACTION_BOOT_APP;
        }
        s_start_discovery(self);
        return LINBOOT_ACTION_NONE;
    }

    const linboot_offer* offer = &self->offers[best];
    const bool           force = (offer->flags & PROTO_OFFER_FLAG_FORCE) != 0;
    if (self->identity.app_valid && offer->version == self->identity.firmware_version && !force) {
        // Already up to date.
        return LINBOOT_ACTION_BOOT_APP;
    }

    // The size was validated when the offer was received, so this can't fail.
    (void)image_begin(&self->image, offer->size);
    self->provider     = (uint8_t)best;
    self->version      = offer->version;
    self->stream_size  = offer->size + PROTO_TRAILER_SIZE;
    self->chunk        = 0;
    self->read_chunk   = NO_READ_CHUNK;
    self->poll_retries = 0;
    self->read_retries = 0;
    self->phase        = LINBOOT_COMMANDER_READ;
    return LINBOOT_ACTION_NONE;
}

/// All chunks have been received: verify and record the new image.
static void s_finish_transfer(linboot_commander* self)
{
    const uint32_t expected_crc = proto_get_u32(self->trailer);
    const uint32_t crc          = image_flash_crc(&self->image);
    if (crc != expected_crc) {
        s_send_result(self, PROTO_ERR_CRC);
    } else if (!meta_write(self->image.layout->meta_addr, self->version, self->image.size, crc)) {
        s_send_result(self, PROTO_ERR_FLASH);
    } else {
        self->identity.firmware_version = self->version;
        self->identity.app_valid        = true;
        s_send_result(self, PROTO_ERR_NONE);
    }
}

/// Stores the 7 bytes of the current chunk: image bytes to the image writer, the rest to the CRC
/// trailer.
static bool s_accept_chunk(linboot_commander* self, const uint8_t* data)
{
    const uint32_t offset = self->chunk * PROTO_CHUNK_SIZE;
    const uint32_t size   = self->image.size;
    for (uint32_t i = 0; i < PROTO_CHUNK_SIZE; ++i) {
        const uint32_t pos = offset + i;
        if (pos >= size && pos < size + PROTO_TRAILER_SIZE) {
            self->trailer[pos - size] = data[i];
        }
    }
    if (offset < size) {
        const uint32_t n = (size - offset) < PROTO_CHUNK_SIZE ? (size - offset) : PROTO_CHUNK_SIZE;
        if (!image_put(&self->image, data, n)) {
            return false;
        }
    }
    self->chunk++;
    return image_commit(&self->image);
}

/// Goes back to READ for the current chunk (resynchronizing the provider), or fails the attempt if
/// that has been done too many times without progress.
static void s_resync(linboot_commander* self)
{
    if (self->read_chunk == self->chunk) {
        self->read_retries++;
        if (self->read_retries > LINBOOT_READ_RETRIES) {
            s_send_result(self, PROTO_ERR_TIMEOUT);
            return;
        }
    }
    self->phase = LINBOOT_COMMANDER_READ;
}

static void s_data_done(linboot_commander* self, bool received)
{
    if (received && self->rx[0] == (uint8_t)self->chunk) {
        if (!s_accept_chunk(self, &self->rx[1])) {
            s_send_result(self, PROTO_ERR_FLASH);
            return;
        }
        self->poll_retries = 0;
        self->read_retries = 0;
        if (self->chunk * PROTO_CHUNK_SIZE >= self->stream_size) {
            s_finish_transfer(self);
        }
        return;
    }
    if (received && self->read_chunk != self->chunk) {
        // The provider is serving some other chunk (it missed our progress, or we missed one of
        // its frames): tell it where we are right away.
        s_resync(self);
        return;
    }
    // No (valid) response, or the provider hasn't caught up with our READ yet: poll again a few
    // times before repeating the READ.
    self->poll_retries++;
    if (self->poll_retries > LINBOOT_POLL_RETRIES) {
        self->poll_retries = 0;
        s_resync(self);
    }
}

static void s_offer_done(linboot_commander* self, bool received)
{
    if (received) {
        linboot_offer* offer = &self->offers[self->offer_index];
        offer->version       = proto_get_u16(&self->rx[0]);
        offer->size          = proto_get_u24(&self->rx[2]);
        offer->flags         = self->rx[5];
        // Images that can't fit are ignored.
        offer->valid         = offer->size > 0 && offer->size <= self->image.layout->app_max_size;
    }
    self->offer_index++;
}

/// Handles the end of a transaction (response received, or slot ended).
static linboot_action s_transaction_done(linboot_commander* self, uint32_t now_ms)
{
    const bool received = self->rx_done;
    switch (self->phase) {
    case LINBOOT_COMMANDER_IDENTIFY:
        self->phase = LINBOOT_COMMANDER_OFFER;
        break;
    case LINBOOT_COMMANDER_OFFER:
        s_offer_done(self, received);
        if (self->offer_index >= LINBOOT_PROVIDER_COUNT) {
            return s_evaluate_offers(self, now_ms);
        }
        break;
    case LINBOOT_COMMANDER_READ:
        self->read_chunk   = self->chunk;
        self->poll_retries = 0;
        self->phase        = LINBOOT_COMMANDER_POLL_DATA;
        break;
    case LINBOOT_COMMANDER_POLL_DATA:
        s_data_done(self, received);
        break;
    case LINBOOT_COMMANDER_RESULT:
    default:
        self->result_sends++;
        if (self->result_sends < LINBOOT_RESULT_REPEATS) {
            break;
        }
        if (self->result == PROTO_ERR_NONE) {
            return LINBOOT_ACTION_BOOT_APP;
        }
        self->failed_providers |= (uint16_t)(1u << self->provider);
        self->attempts++;
        if (self->attempts >= LINBOOT_MAX_ATTEMPTS) {
            if (self->identity.app_valid) {
                // The old application wasn't touched yet: give up and run it.
                return LINBOOT_ACTION_BOOT_APP;
            }
            // Nothing to boot: keep trying.
            self->attempts = 0;
        }
        s_start_discovery(self);
        break;
    }
    return LINBOOT_ACTION_NONE;
}

/// Returns the ID of the header of the current phase.
static uint8_t s_phase_id(const linboot_commander* self)
{
    switch (self->phase) {
    case LINBOOT_COMMANDER_OFFER:
        return s_offer_ids[self->offer_index];
    case LINBOOT_COMMANDER_POLL_DATA:
        return LINBOOT_ID_DATA;
    case LINBOOT_COMMANDER_IDENTIFY:
    case LINBOOT_COMMANDER_READ:
    case LINBOOT_COMMANDER_RESULT:
    default:
        return LINBOOT_ID_QUERY;
    }
}

void linboot_commander_init(
    linboot_commander* self, const linboot_layout* layout, const linboot_identity* identity, uint32_t now_ms)
{
    memset(self, 0, sizeof(*self));
    self->identity = *identity;
    self->start_ms = now_ms;
    image_init(&self->image, layout, &self->identity);
    s_start_discovery(self);
}

void linboot_commander_on_frame(linboot_commander* self, uint8_t id, const uint8_t* payload, uint32_t now_ms)
{
    (void)now_ms; // UNUSED
    if (!self->in_transaction || id != self->tx_id || id == LINBOOT_ID_QUERY) {
        return;
    }
    memcpy(self->rx, payload, PROTO_FRAME_SIZE);
    self->rx_done = true;
}

bool linboot_commander_on_publish(linboot_commander* self, uint8_t id, uint8_t* out_payload, uint32_t now_ms)
{
    (void)now_ms; // UNUSED
    if (!self->in_transaction || id != LINBOOT_ID_QUERY || self->tx_id != LINBOOT_ID_QUERY) {
        return false;
    }
    memset(out_payload, 0, PROTO_FRAME_SIZE);
    switch (self->phase) {
    case LINBOOT_COMMANDER_IDENTIFY: {
        out_payload[0] = PROTO_QUERY_IDENTIFY;
        proto_put_identity(&out_payload[1], &self->identity);
    } break;
    case LINBOOT_COMMANDER_READ: {
        out_payload[0] = PROTO_QUERY_READ;
        out_payload[1] = self->provider;
        proto_put_u24(&out_payload[2], self->chunk * PROTO_CHUNK_SIZE);
    } break;
    case LINBOOT_COMMANDER_RESULT: {
        out_payload[0] = PROTO_QUERY_RESULT;
        out_payload[1] = self->provider;
        out_payload[2] = (uint8_t)self->result;
    } break;
    case LINBOOT_COMMANDER_OFFER:
    case LINBOOT_COMMANDER_POLL_DATA:
    default:
        return false;
    }
    self->published = true;
    return true;
}

linboot_action linboot_commander_poll(linboot_commander* self, uint32_t now_ms)
{
    if (self->in_transaction) {
        // A published frame has been transmitted already when this is called (the transmit is
        // blocking); a received one is complete.
        const bool done = self->rx_done || self->published || now_ms - self->tx_start_ms >= LINBOOT_SLOT_MS;
        if (!done) {
            return LINBOOT_ACTION_NONE;
        }
        self->in_transaction        = false;
        const linboot_action action = s_transaction_done(self, now_ms);
        if (action != LINBOOT_ACTION_NONE) {
            return action;
        }
    }

    // Start the transaction of the (new) phase.
    self->tx_id          = s_phase_id(self);
    self->tx_start_ms    = now_ms;
    self->rx_done        = false;
    self->published      = false;
    self->in_transaction = true;
    // A failed transmit is handled like a missing response: the slot times out.
    (void)linboot_lin_send_header(self->tx_id);
    return LINBOOT_ACTION_NONE;
}
