// The functions are compiled under other names (see CMakeLists.txt), so that the host C library
// keeps its own.
#include <stddef.h>
#include <stdint.h>
#include <unity.h>

void* mini_memcpy(void* dst, const void* src, size_t n);
void* mini_memmove(void* dst, const void* src, size_t n);
void* mini_memset(void* dst, int value, size_t n);

static uint8_t s_buf[32];

void setUp(void)
{
    for (uint32_t i = 0; i < sizeof(s_buf); ++i) {
        s_buf[i] = (uint8_t)i;
    }
}

void tearDown(void) { }

void test_memcpy(void)
{
    uint8_t dst[8] = {0};
    TEST_ASSERT_EQUAL_PTR(dst, mini_memcpy(dst, &s_buf[3], 5));
    const uint8_t expected[8] = {3, 4, 5, 6, 7, 0, 0, 0};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, dst, sizeof(dst));
    TEST_ASSERT_EQUAL_PTR(dst, mini_memcpy(dst, s_buf, 0));
}

void test_memmove_down_overlapping(void)
{
    // Like the spill move of the image writer.
    TEST_ASSERT_EQUAL_PTR(s_buf, mini_memmove(s_buf, &s_buf[2], 6));
    const uint8_t expected[8] = {2, 3, 4, 5, 6, 7, 6, 7};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_buf, sizeof(expected));
}

void test_memmove_up_overlapping(void)
{
    TEST_ASSERT_EQUAL_PTR(&s_buf[2], mini_memmove(&s_buf[2], s_buf, 6));
    const uint8_t expected[8] = {0, 1, 0, 1, 2, 3, 4, 5};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_buf, sizeof(expected));
}

void test_memset(void)
{
    TEST_ASSERT_EQUAL_PTR(&s_buf[1], mini_memset(&s_buf[1], 0x1AB, 3)); // only the low byte counts
    const uint8_t expected[6] = {0, 0xAB, 0xAB, 0xAB, 4, 5};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_buf, sizeof(expected));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_memcpy);
    RUN_TEST(test_memmove_down_overlapping);
    RUN_TEST(test_memmove_up_overlapping);
    RUN_TEST(test_memset);
    return UNITY_END();
}
