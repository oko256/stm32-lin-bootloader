#include "image.h"
#include "crc32.h"
#include "fake_flash.h"
#include <string.h>
#include <unity.h>

static linboot_layout   s_layout;
static linboot_identity s_identity;
static image_writer     s_writer;
static uint8_t          s_image[FAKE_APP_SIZE];

void setUp(void)
{
    fake_flash_reset();
    s_layout   = fake_flash_layout();
    s_identity = (linboot_identity){.firmware_version = 0x0100, .app_valid = true};
    image_init(&s_writer, &s_layout, &s_identity);
    for (uint32_t i = 0; i < sizeof(s_image); ++i) {
        s_image[i] = (uint8_t)(i * 13u + 5u);
    }
}

void tearDown(void) { TEST_ASSERT_FALSE(g_flash_error); }

/// Feeds the image in 7-byte chunks (committing whenever needed) until it is all written.
static void s_feed_all(uint32_t size)
{
    while (image_received(&s_writer) < size) {
        if (image_commit_needed(&s_writer)) {
            TEST_ASSERT_TRUE(image_commit(&s_writer));
            continue;
        }
        const uint32_t offset = image_received(&s_writer);
        TEST_ASSERT_LESS_THAN_UINT32(image_window_end(&s_writer), offset);
        uint8_t chunk[PROTO_CHUNK_SIZE];
        memset(chunk, 0xEE, sizeof(chunk)); // padding beyond the image must be ignored
        const uint32_t n = size - offset < PROTO_CHUNK_SIZE ? size - offset : PROTO_CHUNK_SIZE;
        memcpy(chunk, &s_image[offset], n);
        TEST_ASSERT_TRUE(image_put(&s_writer, chunk, PROTO_CHUNK_SIZE));
    }
    TEST_ASSERT_TRUE(image_commit(&s_writer));
    TEST_ASSERT_FALSE(image_commit_needed(&s_writer));
    TEST_ASSERT_EQUAL_UINT32(0, s_writer.fill);
}

void test_rejects_bad_sizes(void)
{
    TEST_ASSERT_FALSE(image_begin(&s_writer, 0));
    TEST_ASSERT_FALSE(image_begin(&s_writer, FAKE_APP_SIZE + 1));
    TEST_ASSERT_TRUE(image_begin(&s_writer, FAKE_APP_SIZE));
}

void test_put_before_begin_fails(void)
{
    const uint8_t chunk[PROTO_CHUNK_SIZE] = {0};
    TEST_ASSERT_FALSE(image_put(&s_writer, chunk, sizeof(chunk)));
}

void test_writes_image_of_various_sizes(void)
{
    const uint32_t sizes[] = {1, 2, 7, 127, 128, 129, 255, 256, 257, 1000, FAKE_APP_SIZE - 1, FAKE_APP_SIZE};
    for (uint32_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        setUp();
        const uint32_t size = sizes[i];
        TEST_ASSERT_TRUE(image_begin(&s_writer, size));
        s_feed_all(size);
        TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, size);
        TEST_ASSERT_EQUAL_HEX32(crc32(s_image, size), image_flash_crc(&s_writer));
        // Erased (padding) after the image, up to the end of the page.
        const uint32_t page_end = (size + LINBOOT_FLASH_PAGE_SIZE - 1) / LINBOOT_FLASH_PAGE_SIZE
            * LINBOOT_FLASH_PAGE_SIZE;
        for (uint32_t j = size; j < page_end; ++j) {
            TEST_ASSERT_EQUAL_HEX8(0xFF, g_flash[j]);
        }
        // The metadata page and each used page erased once.
        TEST_ASSERT_EQUAL_UINT32(1 + page_end / LINBOOT_FLASH_PAGE_SIZE, g_flash_erase_count);
        TEST_ASSERT_TRUE(s_writer.meta_erased);
        TEST_ASSERT_FALSE(s_identity.app_valid);
        TEST_ASSERT_EQUAL_HEX16(0, s_identity.firmware_version);
    }
}

void test_window_and_spill(void)
{
    TEST_ASSERT_TRUE(image_begin(&s_writer, 1000));
    TEST_ASSERT_EQUAL_UINT32(LINBOOT_BLOCK_SIZE, image_window_end(&s_writer));
    // 18 chunks = 126 bytes fit in the block, the 19th starts inside it and spills 5 bytes.
    uint8_t chunk[PROTO_CHUNK_SIZE];
    for (uint32_t i = 0; i < 19; ++i) {
        memcpy(chunk, &s_image[i * PROTO_CHUNK_SIZE], PROTO_CHUNK_SIZE);
        TEST_ASSERT_TRUE(image_put(&s_writer, chunk, PROTO_CHUNK_SIZE));
    }
    TEST_ASSERT_TRUE(image_commit_needed(&s_writer));
    TEST_ASSERT_EQUAL_UINT32(133, image_window_end(&s_writer)); // = received: full
    TEST_ASSERT_FALSE(image_put(&s_writer, chunk, PROTO_CHUNK_SIZE));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_program_count);

    TEST_ASSERT_TRUE(image_commit(&s_writer));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, LINBOOT_BLOCK_SIZE);
    TEST_ASSERT_EQUAL_UINT32(2 * LINBOOT_BLOCK_SIZE, image_window_end(&s_writer));
    TEST_ASSERT_EQUAL_UINT32(133, image_received(&s_writer));
    TEST_ASSERT_EQUAL_UINT32(5, s_writer.fill);
}

void test_meta_erased_only_on_first_commit(void)
{
    // A valid old record stays until the first block is actually written.
    g_flash[FAKE_APP_SIZE] = 0x00;
    TEST_ASSERT_TRUE(image_begin(&s_writer, 100));
    const uint8_t chunk[PROTO_CHUNK_SIZE] = {1, 2, 3, 4, 5, 6, 7};
    TEST_ASSERT_TRUE(image_put(&s_writer, chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL_HEX8(0x00, g_flash[FAKE_APP_SIZE]);
    TEST_ASSERT_FALSE(s_writer.meta_erased);
    TEST_ASSERT_TRUE(s_identity.app_valid);
}

void test_restart_rewrites_from_the_beginning(void)
{
    TEST_ASSERT_TRUE(image_begin(&s_writer, 600));
    s_feed_all(600);
    // A second transfer of a different image over the first one.
    for (uint32_t i = 0; i < sizeof(s_image); ++i) {
        s_image[i] = (uint8_t)~s_image[i];
    }
    TEST_ASSERT_TRUE(image_begin(&s_writer, 300));
    s_feed_all(300);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(s_image, g_flash, 300);
}

void test_flash_failures(void)
{
    // Metadata erase fails.
    TEST_ASSERT_TRUE(image_begin(&s_writer, 300));
    g_flash_fail_erase_in = 0;
    s_writer.fill         = LINBOOT_BLOCK_SIZE;
    TEST_ASSERT_FALSE(image_commit(&s_writer));

    // Page erase fails.
    setUp();
    TEST_ASSERT_TRUE(image_begin(&s_writer, 300));
    s_writer.fill         = LINBOOT_BLOCK_SIZE;
    g_flash_fail_erase_in = 1;
    TEST_ASSERT_FALSE(image_commit(&s_writer));

    // Programming fails.
    setUp();
    TEST_ASSERT_TRUE(image_begin(&s_writer, 300));
    s_writer.fill           = LINBOOT_BLOCK_SIZE;
    g_flash_fail_program_in = 0;
    TEST_ASSERT_FALSE(image_commit(&s_writer));
}

void test_commit_without_data_does_nothing(void)
{
    TEST_ASSERT_TRUE(image_commit(&s_writer));
    TEST_ASSERT_TRUE(image_begin(&s_writer, 300));
    TEST_ASSERT_TRUE(image_commit(&s_writer));
    TEST_ASSERT_EQUAL_UINT32(0, g_flash_erase_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rejects_bad_sizes);
    RUN_TEST(test_put_before_begin_fails);
    RUN_TEST(test_writes_image_of_various_sizes);
    RUN_TEST(test_window_and_spill);
    RUN_TEST(test_meta_erased_only_on_first_commit);
    RUN_TEST(test_restart_rewrites_from_the_beginning);
    RUN_TEST(test_flash_failures);
    RUN_TEST(test_commit_without_data_does_nothing);
    return UNITY_END();
}
