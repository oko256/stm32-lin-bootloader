/**
 * @file
 * @brief Simulated bootloader as a shared library, for testing the Python host tool against the
 * real role state machines (tools/linboot/tests).
 *
 * Uses the unit test configuration (stubs/linboot_config.h) and the simulated flash. Both roles are
 * available; the caller provides the time and plays the rest of the bus.
 */

#include "commander.h"
#include "crc32.h"
#include "fake_flash.h"
#include "lin_bus.h"
#include "meta.h"
#include "responder.h"
#include <string.h>

#define SIM_API __attribute__((visibility("default")))

typedef void (*sim_header_callback)(uint8_t id);

static linboot_layout      s_layout;
static linboot_responder   s_responder;
static linboot_commander   s_commander;
static sim_header_callback s_header_callback = NULL;

bool linboot_lin_send_header(uint8_t id)
{
    if (s_header_callback != NULL) {
        s_header_callback(id);
    }
    return true;
}

/// Resets the flash (with an application of @p app_size bytes and version @p version if non-zero)
/// and both state machines.
SIM_API void sim_reset(uint32_t app_size, uint16_t version, uint32_t now_ms)
{
    fake_flash_reset();
    s_layout = fake_flash_layout();
    if (app_size > 0) {
        memset(g_flash, 0x5A, app_size);
        (void)meta_write(s_layout.meta_addr, version, app_size, crc32(g_flash, app_size));
    }
    linboot_identity identity = {.device_type = LINBOOT_DEVICE_TYPE, .hardware_version = LINBOOT_HW_VERSION_DEFAULT};
    meta_load_identity(&s_layout, true, &identity);
    linboot_responder_init(&s_responder, &s_layout, &identity, now_ms);
    linboot_commander_init(&s_commander, &s_layout, &identity, now_ms);
}

/// Returns the installed application: copies it to @p out (up to @p max bytes) and returns its
/// size and version, or 0 if there's no valid application.
SIM_API uint32_t sim_installed(uint8_t* out, uint32_t max, uint16_t* out_version)
{
    linboot_identity identity = {0};
    meta_load_identity(&s_layout, true, &identity);
    meta_record record;
    if (!identity.app_valid || !meta_read(s_layout.meta_addr, &record)) {
        return 0;
    }
    const uint32_t size = record.image_size < max ? record.image_size : max;
    memcpy(out, g_flash, size);
    *out_version = record.firmware_version;
    return record.image_size;
}

SIM_API uint32_t sim_app_max_size(void) { return FAKE_APP_SIZE; }

SIM_API void sim_responder_frame(uint8_t id, const uint8_t* payload, uint32_t now_ms)
{
    linboot_responder_on_frame(&s_responder, id, payload, now_ms);
}

SIM_API bool sim_responder_publish(uint8_t id, uint8_t* out_payload, uint32_t now_ms)
{
    return linboot_responder_on_publish(&s_responder, id, out_payload, now_ms);
}

SIM_API int sim_responder_poll(uint32_t now_ms) { return (int)linboot_responder_poll(&s_responder, now_ms); }

SIM_API void sim_commander_set_header_callback(sim_header_callback callback) { s_header_callback = callback; }

SIM_API void sim_commander_frame(uint8_t id, const uint8_t* payload, uint32_t now_ms)
{
    linboot_commander_on_frame(&s_commander, id, payload, now_ms);
}

SIM_API bool sim_commander_publish(uint8_t id, uint8_t* out_payload, uint32_t now_ms)
{
    return linboot_commander_on_publish(&s_commander, id, out_payload, now_ms);
}

SIM_API int sim_commander_poll(uint32_t now_ms) { return (int)linboot_commander_poll(&s_commander, now_ms); }
