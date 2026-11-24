/**
 * @file
 * @brief Bootloader configuration for the host unit tests (replaces the generated one).
 *
 * Small pages and blocks, so that the tests cover multi-block pages and multi-page images without
 * big buffers. The flash layout itself is given at runtime (fake_flash.h).
 */
#pragma once

#define LINBOOT_DEVICE_TYPE        ((uint16_t)0x1234u)
#define LINBOOT_HW_VERSION_DEFAULT ((uint16_t)0x0003u)

#define LINBOOT_BAUD               19200u
#define LINBOOT_CLASSIC_CHECKSUM   0
#define LINBOOT_ID_STATUS          0x30u
#define LINBOOT_ID_CMD             0x31u
#define LINBOOT_ID_DATA            0x32u
#define LINBOOT_ID_QUERY           0x31u
#define LINBOOT_PROVIDER_COUNT     3u
#define LINBOOT_PROVIDER_OFFER_IDS {0x33u, 0x34u, 0x35u}

#define LINBOOT_BOOT_TIMEOUT_MS    2000u
#define LINBOOT_SESSION_TIMEOUT_MS 5000u
#define LINBOOT_BOOT_DELAY_MS      100u
#define LINBOOT_MAX_ATTEMPTS       3u
#define LINBOOT_POLL_RETRIES       3u
#define LINBOOT_READ_RETRIES       20u

// The flash geometry can be overridden (test/CMakeLists.txt builds the tests also with the block
// size equal to the page size, like on the STM32F042, and with 8-byte flash writes).
#ifndef LINBOOT_FLASH_PAGE_SIZE
#define LINBOOT_FLASH_PAGE_SIZE 256u
#endif
#ifndef LINBOOT_FLASH_WRITE_ALIGN
#define LINBOOT_FLASH_WRITE_ALIGN 2u
#endif
#ifndef LINBOOT_BLOCK_SIZE
#define LINBOOT_BLOCK_SIZE 128u
#endif
