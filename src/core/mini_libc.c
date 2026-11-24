/**
 * @file
 * @brief Minimal byte-by-byte memcpy(), memmove() and memset().
 *
 * The C library versions are optimized for speed and cost several hundred bytes of flash; the
 * bootloader only copies small buffers, so these simple versions are good enough. They replace the
 * library ones at link time (LINBOOT_MINIMAL_LIBC).
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Keep the compiler from turning the loops back into calls to these very functions.
#define NO_BUILTIN_LOOPS __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_BUILTIN_LOOPS void* memmove(void* dst, const void* src, size_t n)
{
    uint8_t*       d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d < s) {
        for (size_t i = 0; i < n; ++i) {
            d[i] = s[i];
        }
    } else {
        while (n > 0) {
            n--;
            d[n] = s[n];
        }
    }
    return dst;
}

NO_BUILTIN_LOOPS void* memcpy(void* dst, const void* src, size_t n) { return memmove(dst, src, n); }

NO_BUILTIN_LOOPS void* memset(void* dst, int value, size_t n)
{
    uint8_t* d = (uint8_t*)dst;
    for (size_t i = 0; i < n; ++i) {
        d[i] = (uint8_t)value;
    }
    return dst;
}
