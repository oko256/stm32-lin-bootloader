#include "commander.h"
#include "bus_stub.h"
#include "crc32.h"
#include "fake_flash.h"
#include "meta.h"
#include "proto.h"
#include <string.h>
#include <unity.h>

static linboot_layout    s_layout;
static linboot_commander s_cmdr;
static uint32_t          s_now;
static uint8_t           s_image[FAKE_APP_SIZE];

static const uint8_t s_offer_ids[LINBOOT_PROVIDER_COUNT] = LINBOOT_PROVIDER_OFFER_IDS;

/// A simulated provider node.
typedef struct {
    bool     present;      ///< Powered on (responds to anything).
    bool     has_image;    ///< Responds to OFFER.
    uint32_t late;         ///< Responds to OFFER only after this many IDENTIFYs.
    uint16_t version;      ///< Offered version.
    uint8_t  flags;        ///< Offer flags.
    uint32_t size;         ///< Offered image size (the image is s_image).
    uint32_t crc_xor;      ///< Corrupts the CRC trailer.
    bool     slow;         ///< Doesn't advance by itself, applies READs only after `lag` DATA polls.
    uint32_t lag;          ///< See slow.
    uint32_t drop_every;   ///< Every Nth DATA response is lost (0: none).
    uint32_t ptr;          ///< Read pointer.
    uint32_t pending_ptr;  ///< Slow provider: READ not applied yet.
    uint32_t lag_left;     ///< Slow provider: DATA polls until pending_ptr is applied.
    uint32_t data_count;   ///< DATA responses so far.
    uint32_t read_count;   ///< READ requests for this provider.
    int32_t  last_result;  ///< Error code of the last RESULT for this provider (-1: none).
    uint32_t result_count; ///< RESULT frames for this provider.
} provider;

static provider s_providers[LINBOOT_PROVIDER_COUNT];
static int      s_selected; ///< Provider of the last READ (-1: none).
static uint8_t  s_last_identify[PROTO_FRAME_SIZE];
static uint32_t s_identify_count;

/// Byte at @p pos of the provider's data stream: the image followed by its CRC.
static uint8_t s_stream_byte(const provider* p, uint32_t pos)
{
    if (pos < p->size) {
        return s_image[pos];
    }
    if (pos < p->size + PROTO_TRAILER_SIZE) {
        const uint32_t crc = crc32(s_image, p->size) ^ p->crc_xor;
        return (uint8_t)(crc >> (8u * (pos - p->size)));
    }
    return 0xFF;
}

static void s_handle_query(void)
{
    uint8_t q[PROTO_FRAME_SIZE];
    if (!linboot_commander_on_publish(&s_cmdr, LINBOOT_ID_QUERY, q, s_now)) {
        return;
    }
    switch (q[0]) {
    case PROTO_QUERY_IDENTIFY:
        memcpy(s_last_identify, q, sizeof(q));
        s_identify_count++;
        break;
    case PROTO_QUERY_READ: {
        TEST_ASSERT_LESS_THAN(LINBOOT_PROVIDER_COUNT, q[1]);
        provider* p = &s_providers[q[1]];
        if (!p->present) {
            break;
        }
        s_selected = q[1];
        p->read_count++;
        const uint32_t offset = proto_get_u24(&q[2]);
        TEST_ASSERT_EQUAL_UINT32(0, offset % PROTO_CHUNK_SIZE);
        if (p->slow) {
            p->pending_ptr = offset;
            p->lag_left    = p->lag;
        } else {
            p->ptr = offset;
        }
    } break;
    case PROTO_QUERY_RESULT:
        TEST_ASSERT_LESS_THAN(LINBOOT_PROVIDER_COUNT, q[1]);
        s_providers[q[1]].last_result = q[2];
        s_providers[q[1]].result_count++;
        break;
    default:
        TEST_FAIL_MESSAGE("unknown query");
    }
}

static void s_handle_data(void)
{
    if (s_selected < 0 || !s_providers[s_selected].present) {
        return;
    }
    provider* p = &s_providers[s_selected];
    if (p->slow && p->lag_left > 0) {
        p->lag_left--;
        if (p->lag_left == 0) {
            p->ptr = p->pending_ptr;
        }
    }
    uint8_t frame[PROTO_FRAME_SIZE];
    frame[0] = (uint8_t)(p->ptr / PROTO_CHUNK_SIZE);
    for (uint32_t i = 0; i < PROTO_CHUNK_SIZE; ++i) {
        frame[1 + i] = s_stream_byte(p, p->ptr + i);
    }
    p->data_count++;
    if (!p->slow) {
        p->ptr += PROTO_CHUNK_SIZE;
    }
    if (p->drop_every != 0 && p->data_count % p->drop_every == 0) {
        return; // corrupted on the bus
    }
    linboot_commander_on_frame(&s_cmdr, LINBOOT_ID_DATA, frame, s_now);
}

static void s_on_header(uint8_t id)
{
    if (id == LINBOOT_ID_QUERY) {
        s_handle_query();
        return;
    }
    if (id == LINBOOT_ID_DATA) {
        s_handle_data();
        return;
    }
    for (uint8_t k = 0; k < LINBOOT_PROVIDER_COUNT; ++k) {
        const provider* p = &s_providers[k];
        if (id == s_offer_ids[k] && p->present && p->has_image && s_identify_count > p->late) {
            uint8_t offer[PROTO_FRAME_SIZE] = {0};
            proto_put_u16(&offer[0], p->version);
            proto_put_u24(&offer[2], p->size);
            offer[5] = p->flags;
            linboot_commander_on_frame(&s_cmdr, id, offer, s_now);
            return;
        }
    }
}

static void s_init(bool with_app, uint16_t version)
{
    linboot_identity identity = {.device_type = 0x1234, .hardware_version = 0x0003};
    if (with_app) {
        memset(g_flash, 0x5A, 500);
        TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, version, 500, crc32(g_flash, 500)));
    }
    meta_load_identity(&s_layout, true, &identity);
    linboot_commander_init(&s_cmdr, &s_layout, &identity, s_now);
}

void setUp(void)
{
    fake_flash_reset();
    bus_reset();
    g_bus_on_header = s_on_header;
    s_layout        = fake_flash_layout();
    s_now           = 1000;
    for (uint32_t i = 0; i < sizeof(s_image); ++i) {
        s_image[i] = (uint8_t)(i * 29u + 3u);
    }
    memset(s_providers, 0, sizeof(s_providers));
    for (uint8_t k = 0; k < LINBOOT_PROVIDER_COUNT; ++k) {
        s_providers[k].last_result = -1;
    }
    s_selected       = -1;
    s_identify_count = 0;
    memset(s_last_identify, 0, sizeof(s_last_identify));
    s_init(true, 0x0100);
}

void tearDown(void) { TEST_ASSERT_FALSE(g_flash_error); }

static void s_add_provider(uint8_t index, uint16_t version, uint32_t size)
{
    provider* p  = &s_providers[index];
    p->present   = true;
    p->has_image = true;
    p->version   = version;
    p->size      = size;
}

/// Polls the commander once per millisecond for at most @p max_ms; returns the action that ended
/// the run (NONE on time out).
static linboot_action s_run(uint32_t max_ms)
{
    for (uint32_t i = 0; i < max_ms; ++i) {
        const linboot_action action = linboot_commander_poll(&s_cmdr, s_now);
        if (action != LINBOOT_ACTION_NONE) {
            return action;
        }
        s_now++;
    }
    return LINBOOT_ACTION_NONE;
}

static void s_assert_installed(uint16_t version, uint32_t size)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, size);
    meta_record record;
    TEST_ASSERT_TRUE(meta_read(s_layout.meta_addr, &record));
    TEST_ASSERT_EQUAL_HEX16(version, record.firmware_version);
    TEST_ASSERT_EQUAL_UINT32(size, record.image_size);
    TEST_ASSERT_TRUE(meta_image_valid(&record, &s_layout, true));
}

void test_slot_time(void) { TEST_ASSERT_EQUAL_UINT32(11, LINBOOT_SLOT_MS); }

void test_identify_contents(void)
{
    (void)s_run(100);
    const uint8_t expected[PROTO_FRAME_SIZE] = {
        PROTO_QUERY_IDENTIFY, 0x34, 0x12, 0x03, 0x00, 0x00, 0x01, (PROTO_VERSION << 4) | PROTO_STATUS_FLAG_APP_VALID};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_last_identify, sizeof(expected));
}

void test_no_providers_boots_valid_app_after_timeout(void)
{
    const uint32_t start = s_now;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(LINBOOT_BOOT_TIMEOUT_MS + 100));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(start + LINBOOT_BOOT_TIMEOUT_MS, s_now);
    TEST_ASSERT_GREATER_THAN_UINT32(10, s_identify_count); // kept asking
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
}

void test_no_providers_and_no_app_keeps_looking(void)
{
    fake_flash_reset();
    s_init(false, 0);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(20000));
    // A provider shows up eventually.
    s_add_provider(1, 0x0001, 900);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0001, 900);
}

void test_failed_transmits_time_out(void)
{
    g_bus_tx_fail = true;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(LINBOOT_BOOT_TIMEOUT_MS + 100));
}

void test_up_to_date_boots_right_away(void)
{
    s_add_provider(0, 0x0100, 900);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(200));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
    TEST_ASSERT_EQUAL_UINT32(0, s_providers[0].read_count);
}

void test_update_from_single_provider(void)
{
    s_add_provider(0, 0x0102, 1500);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0102, 1500);
    TEST_ASSERT_EQUAL_INT32(PROTO_ERR_NONE, s_providers[0].last_result);
    TEST_ASSERT_EQUAL_UINT32(LINBOOT_RESULT_REPEATS, s_providers[0].result_count);
    TEST_ASSERT_EQUAL_UINT32(1, s_providers[0].read_count);
    // 1500 + 4 bytes in 7-byte chunks, no retries.
    TEST_ASSERT_EQUAL_UINT32((1504 + 6) / 7, s_providers[0].data_count);
}

void test_full_size_image(void)
{
    s_add_provider(0, 0x0102, FAKE_APP_SIZE);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0102, FAKE_APP_SIZE);
}

void test_older_version_is_installed_too(void)
{
    s_add_provider(2, 0x0001, 700);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0001, 700);
}

void test_highest_version_is_chosen(void)
{
    s_add_provider(0, 0x0102, 700);
    s_add_provider(2, 0x0105, 800);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0105, 800);
    TEST_ASSERT_EQUAL_UINT32(0, s_providers[0].read_count);
}

void test_equal_versions_choose_the_first(void)
{
    s_add_provider(1, 0x0200, 700);
    s_add_provider(2, 0x0200, 800);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0200, 700);
}

void test_offer_from_the_confirmation_round_counts(void)
{
    // Provider 0 starts offering only after the first IDENTIFY, so it misses the first round.
    s_add_provider(0, 0x0200, 700);
    s_add_provider(1, 0x0200, 800);
    s_providers[0].late = 1;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0200, 700);
    TEST_ASSERT_EQUAL_UINT32(0, s_providers[1].read_count);
}

void test_late_higher_version_is_chosen(void)
{
    s_add_provider(0, 0x0102, 700);
    s_add_provider(1, 0x0105, 800);
    s_providers[1].late = 1;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0105, 800);
}

void test_offer_of_the_first_round_counts_without_confirmation(void)
{
    // An offer that is missed in the confirmation round still counts.
    s_add_provider(0, 0x0200, 700);
    s_add_provider(1, 0x0100, 800);
    s_providers[0].late = 0;
    s_providers[1].late = 5; // Never in the first two rounds.
    // Provider 0 drops out after the first round.
    while (s_identify_count < 2) {
        TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(1));
    }
    s_providers[0].has_image = false;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run((LINBOOT_PROVIDER_COUNT + 3) * LINBOOT_SLOT_MS));
    // The transfer from provider 0 has started.
    TEST_ASSERT_EQUAL(0, s_selected);
}

void test_no_update_when_highest_is_installed(void)
{
    // Doesn't downgrade to the older one just because it's different.
    s_add_provider(0, 0x0001, 700);
    s_add_provider(1, 0x0100, 800);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(200));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
}

void test_force_wins(void)
{
    s_add_provider(0, 0x0300, 700);
    s_add_provider(1, 0x0100, 800);
    s_providers[1].flags = PROTO_OFFER_FLAG_FORCE;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(20000));
    s_assert_installed(0x0100, 800);
}

void test_oversized_offer_is_ignored(void)
{
    s_add_provider(0, 0x0200, FAKE_APP_SIZE + 1);
    s_add_provider(1, 0x0200, 0);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(LINBOOT_BOOT_TIMEOUT_MS + 100));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
}

void test_lost_data_frames_are_recovered(void)
{
    s_add_provider(0, 0x0200, 1500);
    s_providers[0].drop_every = 7;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(30000));
    s_assert_installed(0x0200, 1500);
}

void test_slow_provider(void)
{
    // Like a PC behind a USB adapter: serves only what was READ, and only after a while.
    s_add_provider(0, 0x0200, 500);
    s_providers[0].slow = true;
    s_providers[0].lag  = 2;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    s_assert_installed(0x0200, 500);
}

void test_crc_mismatch_retries_until_fixed(void)
{
    fake_flash_reset();
    s_init(false, 0);
    s_add_provider(0, 0x0200, 300);
    s_providers[0].crc_xor = 1;
    // No valid app to fall back to, so it keeps trying.
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(30000));
    TEST_ASSERT_EQUAL_INT32(PROTO_ERR_CRC, s_providers[0].last_result);
    TEST_ASSERT_GREATER_THAN_UINT32(LINBOOT_MAX_ATTEMPTS, s_providers[0].read_count);
    s_providers[0].crc_xor = 0;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(30000));
    s_assert_installed(0x0200, 300);
}

void test_failed_provider_is_skipped_for_another(void)
{
    // The newest image is corrupt: after it fails, the other provider's image is installed.
    s_add_provider(0, 0x0105, 700);
    s_add_provider(1, 0x0107, 800);
    s_providers[1].crc_xor = 1;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    s_assert_installed(0x0105, 700);
    TEST_ASSERT_EQUAL_INT32(PROTO_ERR_CRC, s_providers[1].last_result);
    TEST_ASSERT_EQUAL_UINT32(1, s_providers[1].read_count);
    TEST_ASSERT_EQUAL_INT32(PROTO_ERR_NONE, s_providers[0].last_result);
}

void test_all_failed_providers_get_retried(void)
{
    // Both are corrupt: they are tried in turns (the old app is gone after the first attempt).
    s_add_provider(0, 0x0105, 700);
    s_add_provider(2, 0x0107, 800);
    s_providers[0].crc_xor = 1;
    s_providers[2].crc_xor = 1;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(30000));
    TEST_ASSERT_GREATER_THAN_UINT32(2, s_providers[0].read_count);
    TEST_ASSERT_GREATER_THAN_UINT32(2, s_providers[2].read_count);
    // The newest one gets fixed, and is chosen again.
    s_providers[2].crc_xor = 0;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(30000));
    s_assert_installed(0x0107, 800);
}

void test_provider_disappears_before_writing_boots_old_app(void)
{
    // Disappears right after the first chunks (before the first block is written): the old app
    // is still intact, so after the attempts it is booted.
    s_add_provider(0, 0x0200, 1500);
    while (s_providers[0].data_count < 3) {
        TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(1));
    }
    s_providers[0].present = false;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
    meta_record record;
    TEST_ASSERT_TRUE(meta_read(s_layout.meta_addr, &record));
}

void test_provider_disappears_after_writing_waits_for_one(void)
{
    s_add_provider(0, 0x0200, 1500);
    while (s_providers[0].data_count < 50) {
        TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(1));
    }
    s_providers[0].present = false;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_NONE, s_run(60000));
    TEST_ASSERT_FALSE(s_cmdr.identity.app_valid);
    // Another one shows up.
    s_add_provider(2, 0x0100, 900);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(30000));
    s_assert_installed(0x0100, 900);
}

void test_flash_failure_is_reported(void)
{
    s_add_provider(0, 0x0200, 1500);
    g_flash_fail_program_in = 2;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    // The retry succeeded.
    s_assert_installed(0x0200, 1500);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2, s_providers[0].read_count);
}

void test_metadata_write_failure_is_reported(void)
{
    // The image blocks are programmed first, then the metadata record.
    s_add_provider(0, 0x0200, 300);
    g_flash_fail_program_in = (int32_t)BLOCKS_FOR(300);
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    s_assert_installed(0x0200, 300);
    TEST_ASSERT_EQUAL_UINT32(2, s_providers[0].read_count);
}

void test_gives_up_after_attempts_and_boots_old_app(void)
{
    // The provider offers, but none of its data gets through: nothing is written, and after the
    // attempts the old app is booted even though the provider is still there.
    s_add_provider(0, 0x0200, 1500);
    s_providers[0].drop_every = 1;
    TEST_ASSERT_EQUAL(LINBOOT_ACTION_BOOT_APP, s_run(60000));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
    TEST_ASSERT_EQUAL_INT32(PROTO_ERR_TIMEOUT, s_providers[0].last_result);
    TEST_ASSERT_EQUAL_UINT32(LINBOOT_MAX_ATTEMPTS * (LINBOOT_READ_RETRIES + 1), s_providers[0].read_count);
}

void test_frames_outside_transactions_are_ignored(void)
{
    uint8_t payload[PROTO_FRAME_SIZE] = {0};
    linboot_commander_on_frame(&s_cmdr, LINBOOT_ID_DATA, payload, s_now);
    TEST_ASSERT_FALSE(s_cmdr.rx_done);
    TEST_ASSERT_FALSE(linboot_commander_on_publish(&s_cmdr, LINBOOT_ID_QUERY, payload, s_now));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_slot_time);
    RUN_TEST(test_identify_contents);
    RUN_TEST(test_no_providers_boots_valid_app_after_timeout);
    RUN_TEST(test_no_providers_and_no_app_keeps_looking);
    RUN_TEST(test_failed_transmits_time_out);
    RUN_TEST(test_up_to_date_boots_right_away);
    RUN_TEST(test_update_from_single_provider);
    RUN_TEST(test_full_size_image);
    RUN_TEST(test_older_version_is_installed_too);
    RUN_TEST(test_highest_version_is_chosen);
    RUN_TEST(test_equal_versions_choose_the_first);
    RUN_TEST(test_offer_from_the_confirmation_round_counts);
    RUN_TEST(test_late_higher_version_is_chosen);
    RUN_TEST(test_offer_of_the_first_round_counts_without_confirmation);
    RUN_TEST(test_no_update_when_highest_is_installed);
    RUN_TEST(test_force_wins);
    RUN_TEST(test_oversized_offer_is_ignored);
    RUN_TEST(test_lost_data_frames_are_recovered);
    RUN_TEST(test_slow_provider);
    RUN_TEST(test_crc_mismatch_retries_until_fixed);
    RUN_TEST(test_failed_provider_is_skipped_for_another);
    RUN_TEST(test_all_failed_providers_get_retried);
    RUN_TEST(test_provider_disappears_before_writing_boots_old_app);
    RUN_TEST(test_provider_disappears_after_writing_waits_for_one);
    RUN_TEST(test_flash_failure_is_reported);
    RUN_TEST(test_metadata_write_failure_is_reported);
    RUN_TEST(test_gives_up_after_attempts_and_boots_old_app);
    RUN_TEST(test_frames_outside_transactions_are_ignored);
    return UNITY_END();
}
