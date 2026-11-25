#include "responder.h"
#include "crc32.h"
#include "fake_flash.h"
#include "meta.h"
#include "proto.h"
#include <string.h>
#include <unity.h>

static linboot_layout    s_layout;
static linboot_responder s_resp;
static uint32_t          s_now;
static uint8_t           s_image[FAKE_APP_SIZE];

/// DATA frames sent by the simulated commander; one in s_drop_every (on average, pseudo-randomly)
/// is lost if non-zero.
static uint32_t s_data_sent;
static uint32_t s_drop_every;
static uint32_t s_rand;

static void s_init(bool with_app, uint16_t version)
{
    linboot_identity identity = {.device_type = 0x1234, .hardware_version = 0x0003};
    if (with_app) {
        memset(g_flash, 0x5A, 500);
        TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, version, 500, crc32(g_flash, 500)));
    }
    meta_load_identity(&s_layout, true, &identity);
    linboot_responder_init(&s_resp, &s_layout, &identity, s_now);
}

void setUp(void)
{
    fake_flash_reset();
    s_layout = fake_flash_layout();
    s_now    = 1000;
    for (uint32_t i = 0; i < sizeof(s_image); ++i) {
        s_image[i] = (uint8_t)(i * 31u + 7u);
    }
    s_data_sent  = 0;
    s_drop_every = 0;
    s_rand       = 12345;
    s_init(true, 0x0100);
}

void tearDown(void) { TEST_ASSERT_FALSE(g_flash_error); }

static linboot_action s_poll(void) { return linboot_responder_poll(&s_resp, s_now); }

static void s_advance(uint32_t ms) { s_now += ms; }

/// Reads the STATUS frame.
static void s_status(uint8_t out[PROTO_FRAME_SIZE])
{
    s_advance(10);
    TEST_ASSERT_TRUE(linboot_responder_on_publish(&s_resp, LINBOOT_ID_STATUS, out, s_now));
}

static uint8_t s_state(void)
{
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    return status[0];
}

static void s_frame(uint8_t id, const uint8_t payload[PROTO_FRAME_SIZE])
{
    s_advance(10);
    linboot_responder_on_frame(&s_resp, id, payload, s_now);
}

static void s_offer(uint16_t version, uint32_t size, uint8_t flags)
{
    uint8_t cmd[PROTO_FRAME_SIZE] = {PROTO_CMD_OFFER};
    proto_put_u16(&cmd[1], version);
    proto_put_u24(&cmd[3], size);
    cmd[6] = flags;
    s_frame(LINBOOT_ID_CMD, cmd);
}

static void s_finish(uint32_t crc)
{
    uint8_t cmd[PROTO_FRAME_SIZE] = {PROTO_CMD_FINISH};
    proto_put_u32(&cmd[1], crc);
    s_frame(LINBOOT_ID_CMD, cmd);
}

static void s_simple_cmd(uint8_t code)
{
    const uint8_t cmd[PROTO_FRAME_SIZE] = {code};
    s_frame(LINBOOT_ID_CMD, cmd);
}

/// Sends the DATA frame of the chunk at @p offset of the image (padded with 0xFF).
static void s_data(uint32_t offset, uint32_t size)
{
    uint8_t frame[PROTO_FRAME_SIZE];
    memset(frame, 0xFF, sizeof(frame));
    frame[0] = (uint8_t)(offset / PROTO_CHUNK_SIZE);
    for (uint32_t i = 0; i < PROTO_CHUNK_SIZE && offset + i < size; ++i) {
        frame[1 + i] = s_image[offset + i];
    }
    s_data_sent++;
    s_rand = s_rand * 1103515245u + 12345u;
    if (s_drop_every != 0 && (s_rand >> 16) % s_drop_every == 0) {
        return; // lost on the bus
    }
    s_frame(LINBOOT_ID_DATA, frame);
}

/// Streams the image like a commander would: chunks up to the window, then STATUS, until all of
/// it has been received. Returns false if the state isn't RECEIVING at some point.
static bool s_stream(uint32_t size)
{
    for (uint32_t guard = 0; guard < 10000; ++guard) {
        TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
        uint8_t status[PROTO_FRAME_SIZE];
        s_status(status);
        if (status[0] != PROTO_STATE_RECEIVING) {
            return false;
        }
        const uint32_t next   = proto_get_u24(&status[2]);
        const uint32_t window = proto_get_u24(&status[5]);
        if (next >= size) {
            return true;
        }
        for (uint32_t offset = next; offset < window && offset < size; offset += PROTO_CHUNK_SIZE) {
            s_data(offset, size);
        }
    }
    TEST_FAIL_MESSAGE("transfer didn't finish");
    return false;
}

/// Runs a whole update; returns the final state after FINISH.
static uint8_t s_update(uint16_t version, uint32_t size, uint8_t flags)
{
    s_offer(version, size, flags);
    if (!s_stream(size)) {
        return s_state();
    }
    s_finish(crc32(s_image, size));
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    return s_state();
}

void test_idle_status_reports_identity(void)
{
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    const uint8_t expected[PROTO_FRAME_SIZE] = {
        PROTO_STATE_IDLE, 0x34, 0x12, 0x03, 0x00, 0x00, 0x01, (PROTO_VERSION << 4) | PROTO_STATUS_FLAG_APP_VALID};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, status, sizeof(expected));
}

void test_other_ids_are_ignored(void)
{
    uint8_t payload[PROTO_FRAME_SIZE] = {PROTO_CMD_BOOT};
    TEST_ASSERT_FALSE(linboot_responder_on_publish(&s_resp, 0x20, payload, s_now));
    linboot_responder_on_frame(&s_resp, 0x20, payload, s_now);
    TEST_ASSERT_FALSE(s_resp.session_started);
}

void test_boots_valid_app_after_boot_timeout(void)
{
    s_advance(LINBOOT_BOOT_TIMEOUT_MS - 1);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    s_advance(1);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_waits_forever_without_valid_app(void)
{
    fake_flash_reset();
    s_init(false, 0);
    for (uint32_t i = 0; i < 100; ++i) {
        s_advance(1000);
        TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    }
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_IDLE, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_VERSION << 4, status[7]);
    TEST_ASSERT_EQUAL_HEX16(0, proto_get_u16(&status[5]));
}

void test_heartbeat_keeps_bootloader_until_session_timeout(void)
{
    s_advance(1000);
    (void)s_state();
    s_advance(LINBOOT_BOOT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    s_advance(LINBOOT_SESSION_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_successful_update(void)
{
    const uint32_t size = 1500;
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0201, size, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, size);

    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_NONE, status[1]);
    TEST_ASSERT_EQUAL_HEX16(0x0201, proto_get_u16(&status[2]));

    meta_record record;
    TEST_ASSERT_TRUE(meta_read(s_layout.meta_addr, &record));
    TEST_ASSERT_EQUAL_HEX16(0x0201, record.firmware_version);
    TEST_ASSERT_EQUAL_UINT32(size, record.image_size);
    TEST_ASSERT_TRUE(meta_image_valid(&record, &s_layout, true));

    // Boots after the boot delay.
    s_advance(LINBOOT_BOOT_DELAY_MS - 30);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    s_advance(30);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_boot_command_after_update_boots_right_away(void)
{
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0201, 100, 0));
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_full_size_image(void) { TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0201, FAKE_APP_SIZE, 0)); }

void test_same_version_is_rejected_unless_forced(void)
{
    s_offer(0x0100, 1000, 0);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_REJECTED, status[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0100, proto_get_u16(&status[5]));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);

    // The commander may then boot the device...
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());

    // ...or force the update.
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0100, 700, PROTO_OFFER_FLAG_FORCE));
}

void test_older_version_is_accepted(void) { TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0001, 700, 0)); }

void test_same_version_accepted_without_valid_app(void)
{
    fake_flash_reset();
    s_init(false, 0);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0000, 700, 0));
}

void test_bad_sizes_fail_without_touching_the_app(void)
{
    s_offer(0x0200, FAKE_APP_SIZE + 1, 0);
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_FAILED, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_BAD_SIZE, status[1]);
    s_offer(0x0200, 0, 0);
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_BAD_SIZE, status[1]);
    // The old app is intact, so it can still be booted.
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_lost_data_frames_are_resent(void)
{
    s_drop_every = 5;
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0200, 1500, 0));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, 1500);
}

void test_repeated_and_wrong_chunks_are_ignored(void)
{
    s_offer(0x0200, 100, 0);
    s_data(0, 100);
    s_data(0, 100);  // repeated
    s_data(14, 100); // skipped one
    s_data(7, 100);
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_UINT32(14, proto_get_u24(&status[2]));
    TEST_ASSERT_TRUE(s_stream(100));
    s_finish(crc32(s_image, 100));
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_state());
}

void test_data_beyond_window_is_ignored(void)
{
    s_offer(0x0200, 1000, 0);
    // The whole first block plus more, without polling (committing) in between.
    for (uint32_t offset = 0; offset < FIRST_BLOCK_FULL_AT + 5 * PROTO_CHUNK_SIZE; offset += PROTO_CHUNK_SIZE) {
        s_data(offset, 1000);
    }
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_UINT32(FIRST_BLOCK_FULL_AT, proto_get_u24(&status[2]));
    TEST_ASSERT_EQUAL_UINT32(FIRST_BLOCK_FULL_AT, proto_get_u24(&status[5])); // busy: commit needed
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    s_status(status);
    TEST_ASSERT_EQUAL_UINT32(FIRST_BLOCK_FULL_AT, proto_get_u24(&status[2]));
    TEST_ASSERT_EQUAL_UINT32(2 * LINBOOT_BLOCK_SIZE, proto_get_u24(&status[5]));
}

void test_data_and_finish_outside_receiving_are_ignored(void)
{
    s_data(0, 100);
    s_finish(0);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_IDLE, s_state());
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
}

void test_finish_too_early_reports_error(void)
{
    s_offer(0x0200, 100, 0);
    s_data(0, 100);
    s_finish(crc32(s_image, 100));
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_RECEIVING, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_BAD_STATE, status[1]);
    // The transfer can still go on.
    TEST_ASSERT_TRUE(s_stream(100));
    s_finish(crc32(s_image, 100));
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_state());
}

void test_finish_right_after_last_chunk(void)
{
    // FINISH without a poll (commit) after the last chunk: the last block is committed first.
    const uint32_t size = 300;
    s_offer(0x0200, size, 0);
    for (uint32_t offset = 0; offset < size; offset += PROTO_CHUNK_SIZE) {
        if (offset >= image_window_end(&s_resp.image)) {
            TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
        }
        s_data(offset, size);
    }
    s_finish(crc32(s_image, size));
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_VERIFYING, s_state());
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_state());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, size);
}

void test_crc_mismatch_fails_and_can_be_retried(void)
{
    s_offer(0x0200, 600, 0);
    TEST_ASSERT_TRUE(s_stream(600));
    s_finish(crc32(s_image, 600) ^ 1u);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_FAILED, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_CRC, status[1]);
    TEST_ASSERT_EQUAL_HEX16(0, proto_get_u16(&status[2]));

    // The old app has been overwritten: BOOT does nothing, and neither does the session timeout.
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    meta_record record;
    TEST_ASSERT_FALSE(meta_read(s_layout.meta_addr, &record));

    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0200, 600, 0));
}

void test_abort_returns_to_idle(void)
{
    s_offer(0x0200, 600, 0);
    s_simple_cmd(PROTO_CMD_ABORT);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_IDLE, s_state());
    // Nothing was written, so the old app is still valid and bootable.
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_poll());
}

void test_boot_during_transfer_is_ignored(void)
{
    s_offer(0x0200, 600, 0);
    s_simple_cmd(PROTO_CMD_BOOT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    // Not remembered for later either.
    s_simple_cmd(PROTO_CMD_ABORT);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
}

void test_unknown_command_is_ignored(void)
{
    s_simple_cmd(0x7F);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_IDLE, s_state());
}

void test_session_timeout_without_app_goes_back_to_idle(void)
{
    s_offer(0x0200, 600, 0);
    TEST_ASSERT_TRUE(s_stream(600));
    s_advance(LINBOOT_SESSION_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    TEST_ASSERT_FALSE(s_resp.session_started);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_IDLE, s_state());
    // And the update can be done again.
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0200, 600, 0));
}

void test_flash_failure_fails_the_update(void)
{
    s_offer(0x0200, 600, 0);
    g_flash_fail_program_in = 1;
    TEST_ASSERT_FALSE(s_stream(600));
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_FAILED, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_FLASH, status[1]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_DONE, s_update(0x0200, 600, 0));
}

void test_metadata_write_failure_fails_the_update(void)
{
    s_offer(0x0200, 100, 0);
    TEST_ASSERT_TRUE(s_stream(100));
    g_flash_fail_program_in = 0;
    s_finish(crc32(s_image, 100));
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_poll());
    uint8_t status[PROTO_FRAME_SIZE];
    s_status(status);
    TEST_ASSERT_EQUAL_HEX8(PROTO_STATE_FAILED, status[0]);
    TEST_ASSERT_EQUAL_HEX8(PROTO_ERR_FLASH, status[1]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_idle_status_reports_identity);
    RUN_TEST(test_other_ids_are_ignored);
    RUN_TEST(test_boots_valid_app_after_boot_timeout);
    RUN_TEST(test_waits_forever_without_valid_app);
    RUN_TEST(test_heartbeat_keeps_bootloader_until_session_timeout);
    RUN_TEST(test_successful_update);
    RUN_TEST(test_boot_command_after_update_boots_right_away);
    RUN_TEST(test_full_size_image);
    RUN_TEST(test_same_version_is_rejected_unless_forced);
    RUN_TEST(test_older_version_is_accepted);
    RUN_TEST(test_same_version_accepted_without_valid_app);
    RUN_TEST(test_bad_sizes_fail_without_touching_the_app);
    RUN_TEST(test_lost_data_frames_are_resent);
    RUN_TEST(test_repeated_and_wrong_chunks_are_ignored);
    RUN_TEST(test_data_beyond_window_is_ignored);
    RUN_TEST(test_data_and_finish_outside_receiving_are_ignored);
    RUN_TEST(test_finish_too_early_reports_error);
    RUN_TEST(test_finish_right_after_last_chunk);
    RUN_TEST(test_crc_mismatch_fails_and_can_be_retried);
    RUN_TEST(test_abort_returns_to_idle);
    RUN_TEST(test_boot_during_transfer_is_ignored);
    RUN_TEST(test_unknown_command_is_ignored);
    RUN_TEST(test_session_timeout_without_app_goes_back_to_idle);
    RUN_TEST(test_flash_failure_fails_the_update);
    RUN_TEST(test_metadata_write_failure_fails_the_update);
    return UNITY_END();
}
