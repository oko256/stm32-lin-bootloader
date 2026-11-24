/**
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "responder.h"
#include "linboot_config.h"
#include "meta.h"
#include <string.h>

static void s_set_state(linboot_responder* self, proto_state state, proto_error error)
{
    self->state = state;
    self->error = error;
}

static void s_handle_offer(linboot_responder* self, const uint8_t* payload)
{
    const uint16_t version = proto_get_u16(&payload[1]);
    const uint32_t size    = proto_get_u24(&payload[3]);
    const bool     force   = (payload[6] & PROTO_OFFER_FLAG_FORCE) != 0;

    if (self->identity.app_valid && version == self->identity.firmware_version && !force) {
        s_set_state(self, PROTO_STATE_REJECTED, PROTO_ERR_NONE);
        return;
    }
    if (!image_begin(&self->image, size)) {
        s_set_state(self, PROTO_STATE_FAILED, PROTO_ERR_BAD_SIZE);
        return;
    }
    self->offered_version = version;
    self->next_seq        = 0;
    s_set_state(self, PROTO_STATE_RECEIVING, PROTO_ERR_NONE);
}

static void s_handle_finish(linboot_responder* self, const uint8_t* payload)
{
    if (self->state != PROTO_STATE_RECEIVING || image_received(&self->image) < self->image.size) {
        // FINISH too early (or repeated after the result): keep the state, the commander sees the
        // error and the actual progress from the STATUS.
        if (self->state == PROTO_STATE_RECEIVING) {
            self->error = PROTO_ERR_BAD_STATE;
        }
        return;
    }
    self->finish_crc = proto_get_u32(&payload[1]);
    s_set_state(self, PROTO_STATE_VERIFYING, PROTO_ERR_NONE);
}

static void s_handle_cmd(linboot_responder* self, const uint8_t* payload)
{
    switch (payload[0]) {
    case PROTO_CMD_OFFER:
        s_handle_offer(self, payload);
        break;
    case PROTO_CMD_FINISH:
        s_handle_finish(self, payload);
        break;
    case PROTO_CMD_ABORT:
        s_set_state(self, PROTO_STATE_IDLE, PROTO_ERR_NONE);
        break;
    case PROTO_CMD_BOOT:
        self->boot_requested = true;
        break;
    default:
        // Unknown commands are ignored (newer protocol versions).
        break;
    }
}

static void s_handle_data(linboot_responder* self, const uint8_t* payload)
{
    if (self->state != PROTO_STATE_RECEIVING) {
        return;
    }
    if (payload[0] != self->next_seq) {
        // Not the next chunk (lost or repeated frame): ignored, the commander resumes from the
        // next_offset in the STATUS.
        return;
    }
    // Ignored when the buffer is full: the commander didn't respect the window.
    if (image_put(&self->image, &payload[1], PROTO_CHUNK_SIZE)) {
        self->next_seq++;
    }
}

void linboot_responder_init(
    linboot_responder* self, const linboot_layout* layout, const linboot_identity* identity, uint32_t now_ms)
{
    memset(self, 0, sizeof(*self));
    self->identity         = *identity;
    self->state            = PROTO_STATE_IDLE;
    self->error            = PROTO_ERR_NONE;
    self->start_ms         = now_ms;
    self->last_activity_ms = now_ms;
    image_init(&self->image, layout);
}

void linboot_responder_on_frame(linboot_responder* self, uint8_t id, const uint8_t* payload, uint32_t now_ms)
{
    if (id == LINBOOT_ID_CMD) {
        s_handle_cmd(self, payload);
    } else if (id == LINBOOT_ID_DATA) {
        s_handle_data(self, payload);
    } else {
        return;
    }
    self->session_started  = true;
    self->last_activity_ms = now_ms;
}

bool linboot_responder_on_publish(linboot_responder* self, uint8_t id, uint8_t* out_payload, uint32_t now_ms)
{
    if (id != LINBOOT_ID_STATUS) {
        return false;
    }
    self->session_started  = true;
    self->last_activity_ms = now_ms;

    memset(out_payload, 0, PROTO_FRAME_SIZE);
    out_payload[0] = (uint8_t)self->state;
    switch (self->state) {
    case PROTO_STATE_IDLE:
    case PROTO_STATE_REJECTED: {
        proto_put_u16(&out_payload[1], self->identity.device_type);
        proto_put_u16(&out_payload[3], self->identity.hardware_version);
        proto_put_u16(&out_payload[5], self->identity.firmware_version);
        out_payload[7] = (uint8_t)((PROTO_VERSION << PROTO_STATUS_FLAGS_VERSION_SHIFT)
                                   | (self->identity.app_valid ? PROTO_STATUS_FLAG_APP_VALID : 0u));
    } break;
    case PROTO_STATE_RECEIVING:
    case PROTO_STATE_VERIFYING: {
        out_payload[1] = (uint8_t)self->error;
        proto_put_u24(&out_payload[2], image_received(&self->image));
        proto_put_u24(&out_payload[5], self->state == PROTO_STATE_RECEIVING ? image_window_end(&self->image) : 0u);
    } break;
    case PROTO_STATE_DONE:
    case PROTO_STATE_FAILED:
    default: {
        out_payload[1] = (uint8_t)self->error;
        proto_put_u16(&out_payload[2], self->identity.firmware_version);
    } break;
    }
    return true;
}

/// Commits the buffered data to flash and verifies the finished image, if needed.
static void s_do_flash_work(linboot_responder* self, uint32_t now_ms)
{
    const bool transferring = self->state == PROTO_STATE_RECEIVING || self->state == PROTO_STATE_VERIFYING;
    // FINISH may come right after the last chunk, so the last block may still be uncommitted when
    // verifying.
    if (transferring && image_commit_needed(&self->image)) {
        const bool ok = image_commit(&self->image);
        if (self->image.meta_erased) {
            // The old application is being overwritten.
            self->identity.app_valid        = false;
            self->identity.firmware_version = 0;
        }
        if (!ok) {
            s_set_state(self, PROTO_STATE_FAILED, PROTO_ERR_FLASH);
        }
    } else if (self->state == PROTO_STATE_VERIFYING) {
        const uint32_t crc = image_flash_crc(&self->image);
        if (crc != self->finish_crc) {
            s_set_state(self, PROTO_STATE_FAILED, PROTO_ERR_CRC);
        } else if (!meta_write(self->image.layout->meta_addr, self->offered_version, self->image.size, crc)) {
            s_set_state(self, PROTO_STATE_FAILED, PROTO_ERR_FLASH);
        } else {
            self->identity.firmware_version = self->offered_version;
            self->identity.app_valid        = true;
            self->done_ms                   = now_ms;
            s_set_state(self, PROTO_STATE_DONE, PROTO_ERR_NONE);
        }
    }
}

linboot_action linboot_responder_poll(linboot_responder* self, uint32_t now_ms)
{
    s_do_flash_work(self, now_ms);

    const bool app_valid = self->identity.app_valid;
    if (self->state == PROTO_STATE_DONE && (self->boot_requested || now_ms - self->done_ms >= LINBOOT_BOOT_DELAY_MS)) {
        return LINBOOT_ACTION_BOOT_APP;
    }
    if (self->boot_requested) {
        self->boot_requested = false;
        if (app_valid && self->state != PROTO_STATE_RECEIVING && self->state != PROTO_STATE_VERIFYING) {
            return LINBOOT_ACTION_BOOT_APP;
        }
    }
    if (!self->session_started) {
        if (app_valid && now_ms - self->start_ms >= LINBOOT_BOOT_TIMEOUT_MS) {
            return LINBOOT_ACTION_BOOT_APP;
        }
    } else if (now_ms - self->last_activity_ms >= LINBOOT_SESSION_TIMEOUT_MS) {
        if (app_valid) {
            return LINBOOT_ACTION_BOOT_APP;
        }
        // Nothing to boot: wait for a new session.
        self->session_started = false;
        s_set_state(self, PROTO_STATE_IDLE, PROTO_ERR_NONE);
    }
    return LINBOOT_ACTION_NONE;
}
