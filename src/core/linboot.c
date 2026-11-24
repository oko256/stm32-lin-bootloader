/**
 * @file
 * @brief Bootloader main loop and the glue between minlin and the role state machine.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "common.h"
#include "hooks/linboot_hooks.h"
#include "lin_bus.h"
#include "linboot_config.h"
#include "meta.h"
#include "minlin/minlin.h"
#include "minlin/minlin_hw.h"
#include "port/port.h"
#include "proto.h"

#if defined(LINBOOT_ROLE_RESPONDER)
#include "responder.h"
#define ROLE_INIT    linboot_responder_init
#define ROLE_FRAME   linboot_responder_on_frame
#define ROLE_PUBLISH linboot_responder_on_publish
#define ROLE_POLL    linboot_responder_poll
static linboot_responder s_role;
#elif defined(LINBOOT_ROLE_COMMANDER)
#include "commander.h"
#define ROLE_INIT    linboot_commander_init
#define ROLE_FRAME   linboot_commander_on_frame
#define ROLE_PUBLISH linboot_commander_on_publish
#define ROLE_POLL    linboot_commander_poll
static linboot_commander s_role;
#else
#error "No LINBOOT_ROLE defined"
#endif

static const linboot_layout s_layout = {
    .app_addr     = LINBOOT_APP_ADDR,
    .app_max_size = LINBOOT_APP_MAX_SIZE,
    .meta_addr    = LINBOOT_META_ADDR,
    .page_size    = LINBOOT_FLASH_PAGE_SIZE,
};

#define LINBOOT_FRAME(frame_id, frame_type) {.id = (frame_id), .type = (frame_type), .payload_size = PROTO_FRAME_SIZE}

/// The frames of this node. The payload of a published frame is filled in by the role in the
/// publish callback.
static minlin_frame s_frames[] = {
#if defined(LINBOOT_ROLE_RESPONDER)
    LINBOOT_FRAME(LINBOOT_ID_STATUS, MINLIN_PUBLISH),
    LINBOOT_FRAME(LINBOOT_ID_CMD, MINLIN_SUBSCRIBE),
    LINBOOT_FRAME(LINBOOT_ID_DATA, MINLIN_SUBSCRIBE),
#else
    LINBOOT_FRAME(LINBOOT_ID_QUERY, MINLIN_PUBLISH),
    LINBOOT_FRAME(LINBOOT_ID_DATA, MINLIN_SUBSCRIBE),
    LINBOOT_PROVIDER_OFFER_FRAMES
#endif
};

static minlin_responder s_responder;

void linboot_lin_rx_byte(uint8_t received_byte) { (void)minlin_responder_receive_byte(&s_responder, received_byte); }

bool linboot_lin_send_header(uint8_t id)
{
    const uint8_t header[2] = {MINLIN_SYNC_BYTE, minlin_id_to_pid(id, NULL)};
    return minlin_hw_transmit_break() && minlin_hw_transmit_data(header, sizeof(header));
}

void minlin_responder_subscribe_callback(const minlin_frame* frame)
{
    ROLE_FRAME(&s_role, frame->id, frame->payload, port_tick_ms());
}

bool minlin_responder_publish_callback(minlin_frame* frame)
{
    return ROLE_PUBLISH(&s_role, frame->id, frame->payload, port_tick_ms());
}

static void s_lin_init(void)
{
    minlin_responder_init(&s_responder);
    s_responder.config.classic_checksum = LINBOOT_CLASSIC_CHECKSUM;
    (void)minlin_responder_set_frames(&s_responder, s_frames, (uint8_t)MINLIN_ARRAY_LEN(s_frames));
    port_lin_init();
}

int main(void)
{
    port_init();
    linboot_hook_board_init();

    linboot_identity identity = {
        .device_type      = LINBOOT_DEVICE_TYPE,
        .hardware_version = linboot_hook_hardware_version(),
    };
    meta_load_identity(&s_layout, LINBOOT_VERIFY_APP_ON_BOOT, &identity);

    ROLE_INIT(&s_role, &s_layout, &identity, port_tick_ms());
    s_lin_init();

    for (;;) {
        port_watchdog_refresh();
        (void)minlin_responder_task(&s_responder);
        if (ROLE_POLL(&s_role, port_tick_ms()) == LINBOOT_ACTION_BOOT_APP) {
            linboot_hook_before_boot();
            port_boot_app(LINBOOT_APP_ADDR);
        }
    }
}
