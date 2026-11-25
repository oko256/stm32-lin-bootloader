/**
 * @file
 * @brief Simulated flash for the host unit tests, with STM32F0 like semantics.
 *
 * Programming is done in 16-bit units, and only to erased (0xFFFF) half-words, like on the real
 * hardware. The flash holds FAKE_APP_PAGES pages for the application followed by one metadata page.
 */
#pragma once

#include "common.h"
#include "linboot_config.h"
#include <stdbool.h>
#include <stdint.h>

#define FAKE_APP_PAGES   8u
#define FAKE_FLASH_PAGES (FAKE_APP_PAGES + 1u)
#define FAKE_FLASH_SIZE  (FAKE_FLASH_PAGES * LINBOOT_FLASH_PAGE_SIZE)
#define FAKE_APP_SIZE    (FAKE_APP_PAGES * LINBOOT_FLASH_PAGE_SIZE)

/// Image offset where the first block is full: the first chunk boundary at or after the block size
/// (the chunk that crosses the block boundary spills into the next block).
#define FIRST_BLOCK_FULL_AT (((LINBOOT_BLOCK_SIZE + 7u - 1u) / 7u) * 7u)

/// Number of blocks (flash program calls) an image of @p size bytes takes.
#define BLOCKS_FOR(size) (((size) + LINBOOT_BLOCK_SIZE - 1u) / LINBOOT_BLOCK_SIZE)

extern uint8_t  g_flash[FAKE_FLASH_SIZE];
extern uint32_t g_flash_erase_count;     ///< Page erases so far.
extern uint32_t g_flash_program_count;   ///< Program calls so far.
extern int32_t  g_flash_fail_erase_in;   ///< Fail the erase after this many successful ones (-1: never).
extern int32_t  g_flash_fail_program_in; ///< Fail the program after this many successful ones (-1: never).
extern bool     g_flash_error;           ///< A misuse was detected (unaligned, not erased, out of range).

/// Erases the whole flash and resets the counters and failure injection.
void fake_flash_reset(void);

/// Returns the layout of the simulated flash.
linboot_layout fake_flash_layout(void);

/// Returns the address of the metadata page.
static inline uintptr_t fake_flash_meta_addr(void) { return (uintptr_t)&g_flash[FAKE_APP_SIZE]; }
