#include "crc32.h"
#include <unity.h>

void setUp(void) { }
void tearDown(void) { }

// Reference values from Python's zlib.crc32().

void test_check_value(void) { TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32("123456789", 9)); }

void test_empty(void) { TEST_ASSERT_EQUAL_HEX32(0x00000000u, crc32("", 0)); }

void test_single_byte(void) { TEST_ASSERT_EQUAL_HEX32(0xE8B7BE43u, crc32("a", 1)); }

void test_all_byte_values(void)
{
    uint8_t data[256];
    for (uint32_t i = 0; i < sizeof(data); ++i) {
        data[i] = (uint8_t)i;
    }
    TEST_ASSERT_EQUAL_HEX32(0x29058C73u, crc32(data, sizeof(data)));
}

void test_erased_flash(void)
{
    uint8_t data[1000];
    for (uint32_t i = 0; i < sizeof(data); ++i) {
        data[i] = 0xFF;
    }
    TEST_ASSERT_EQUAL_HEX32(0xE0533230u, crc32(data, sizeof(data)));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_check_value);
    RUN_TEST(test_empty);
    RUN_TEST(test_single_byte);
    RUN_TEST(test_all_byte_values);
    RUN_TEST(test_erased_flash);
    return UNITY_END();
}
