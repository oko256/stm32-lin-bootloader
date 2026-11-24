#include "meta.h"
#include "crc32.h"
#include "fake_flash.h"
#include "proto.h"
#include <string.h>
#include <unity.h>

static linboot_layout s_layout;

void setUp(void)
{
    fake_flash_reset();
    s_layout = fake_flash_layout();
}

void tearDown(void) { TEST_ASSERT_FALSE(g_flash_error); }

/// Puts an image of @p size bytes into the application area and returns its CRC.
static uint32_t s_put_image(uint32_t size)
{
    for (uint32_t i = 0; i < size; ++i) {
        g_flash[i] = (uint8_t)(i * 7u + 1u);
    }
    return crc32(g_flash, size);
}

void test_erased_page_is_not_valid(void)
{
    meta_record record;
    TEST_ASSERT_FALSE(meta_read(s_layout.meta_addr, &record));
}

void test_write_and_read(void)
{
    const uint32_t crc = s_put_image(1000);
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0x0102, 1000, crc));

    meta_record record;
    TEST_ASSERT_TRUE(meta_read(s_layout.meta_addr, &record));
    TEST_ASSERT_EQUAL_HEX32(META_MAGIC, record.magic);
    TEST_ASSERT_EQUAL_HEX16(0x0102, record.firmware_version);
    TEST_ASSERT_EQUAL_UINT32(1000, record.image_size);
    TEST_ASSERT_EQUAL_HEX32(crc, record.image_crc);
    TEST_ASSERT_TRUE(meta_image_valid(&record, &s_layout, true));
}

void test_record_layout(void)
{
    // The layout is part of the "on-flash ABI": check it byte by byte.
    TEST_ASSERT_EQUAL_UINT32(24, sizeof(meta_record));
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0xA1B2, 0x00123456, 0xCAFEBABE));
    const uint8_t* p            = (const uint8_t*)s_layout.meta_addr;
    const uint8_t  expected[20] = {'L',  'B',  'M',  '1',  0xB2, 0xA1, 0xFF, 0xFF, 0x56, 0x34,
                                   0x12, 0x00, 0xBE, 0xBA, 0xFE, 0xCA, 0xFF, 0xFF, 0xFF, 0xFF};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, p, sizeof(expected));
    TEST_ASSERT_EQUAL_HEX32(crc32(p, 20), proto_get_u32(&p[20]));
}

void test_corrupt_record_is_not_valid(void)
{
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0x0102, 1000, 0));
    g_flash[FAKE_APP_SIZE + 4] = 0x00; // version byte
    meta_record record;
    TEST_ASSERT_FALSE(meta_read(s_layout.meta_addr, &record));
}

void test_corrupt_image_is_not_valid(void)
{
    const uint32_t crc = s_put_image(1000);
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0x0102, 1000, crc));
    g_flash[999] ^= 0x01;

    meta_record record;
    TEST_ASSERT_TRUE(meta_read(s_layout.meta_addr, &record));
    TEST_ASSERT_FALSE(meta_image_valid(&record, &s_layout, true));
    // Without the image check, only the record matters.
    TEST_ASSERT_TRUE(meta_image_valid(&record, &s_layout, false));
}

void test_bad_sizes_are_not_valid(void)
{
    meta_record record = {.image_size = 0};
    TEST_ASSERT_FALSE(meta_image_valid(&record, &s_layout, false));
    record.image_size = FAKE_APP_SIZE + 1;
    TEST_ASSERT_FALSE(meta_image_valid(&record, &s_layout, false));
    record.image_size = FAKE_APP_SIZE;
    TEST_ASSERT_TRUE(meta_image_valid(&record, &s_layout, false));
}

void test_load_identity(void)
{
    linboot_identity identity = {.firmware_version = 0xFFFF, .app_valid = true};
    meta_load_identity(&s_layout, true, &identity);
    TEST_ASSERT_FALSE(identity.app_valid);
    TEST_ASSERT_EQUAL_HEX16(0, identity.firmware_version);

    const uint32_t crc = s_put_image(300);
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0x0203, 300, crc));
    meta_load_identity(&s_layout, true, &identity);
    TEST_ASSERT_TRUE(identity.app_valid);
    TEST_ASSERT_EQUAL_HEX16(0x0203, identity.firmware_version);
}

void test_corrupt_app_is_not_loaded_as_valid(void)
{
    // The record is intact, but the application isn't (e.g. a flash bit error): it must not be
    // booted, unless the image check is disabled.
    const uint32_t crc = s_put_image(300);
    TEST_ASSERT_TRUE(meta_write(s_layout.meta_addr, 0x0203, 300, crc));
    g_flash[123] ^= 0x10;
    linboot_identity identity = {0};
    meta_load_identity(&s_layout, true, &identity);
    TEST_ASSERT_FALSE(identity.app_valid);
    TEST_ASSERT_EQUAL_HEX16(0, identity.firmware_version);
    meta_load_identity(&s_layout, false, &identity);
    TEST_ASSERT_TRUE(identity.app_valid);
    TEST_ASSERT_EQUAL_HEX16(0x0203, identity.firmware_version);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_erased_page_is_not_valid);
    RUN_TEST(test_write_and_read);
    RUN_TEST(test_record_layout);
    RUN_TEST(test_corrupt_record_is_not_valid);
    RUN_TEST(test_corrupt_image_is_not_valid);
    RUN_TEST(test_bad_sizes_are_not_valid);
    RUN_TEST(test_load_identity);
    RUN_TEST(test_corrupt_app_is_not_loaded_as_valid);
    return UNITY_END();
}
