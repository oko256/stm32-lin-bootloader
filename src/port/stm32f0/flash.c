/**
 * @file
 * @brief STM32F0 port: flash erase and programming (16 bits at a time).
 *
 * The code runs from the flash, so the CPU simply stalls while an operation is in progress.
 * Addresses inside the bootloader are refused, so a bug elsewhere can't brick the device.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "port/port.h"
#include "port_stm32f0.h"

/// First address the bootloader may modify.
#define WRITABLE_START (LINBOOT_FLASH_BASE + LINBOOT_BOOTLOADER_SIZE)
/// End of the flash.
#define WRITABLE_END   (LINBOOT_FLASH_BASE + LINBOOT_FLASH_SIZE)

static bool s_range_ok(uintptr_t addr, uint32_t len)
{
    return addr >= WRITABLE_START && addr <= WRITABLE_END && len <= WRITABLE_END - addr;
}

static void s_unlock(void)
{
    if ((FLASH->CR & FLASH_CR_LOCK) != 0) {
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
}

static void s_lock(void) { FLASH->CR |= FLASH_CR_LOCK; }

/// Waits for the current operation to finish and clears the status flags. Returns false if the
/// operation failed.
static bool s_wait(void)
{
    while ((FLASH->SR & FLASH_SR_BSY) != 0) { }
    const uint32_t sr = FLASH->SR;
    FLASH->SR         = FLASH_SR_EOP | FLASH_SR_PGERR | FLASH_SR_WRPRTERR;
    return (sr & (FLASH_SR_PGERR | FLASH_SR_WRPRTERR)) == 0;
}

bool port_flash_erase_page(uintptr_t addr)
{
    if (!s_range_ok(addr, LINBOOT_FLASH_PAGE_SIZE)) {
        return false;
    }
    s_unlock();
    (void)s_wait();
    FLASH->CR |= FLASH_CR_PER;
    FLASH->AR = (uint32_t)addr;
    FLASH->CR |= FLASH_CR_STRT;
    bool ok = s_wait();
    FLASH->CR &= ~FLASH_CR_PER;
    s_lock();

    const volatile uint32_t* words = (const volatile uint32_t*)addr;
    for (uint32_t i = 0; ok && i < LINBOOT_FLASH_PAGE_SIZE / 4u; ++i) {
        ok = (words[i] == 0xFFFFFFFFu);
    }
    return ok;
}

bool port_flash_program(uintptr_t addr, const uint8_t* data, uint32_t len)
{
    if (!s_range_ok(addr, len) || (addr % 2u) != 0 || (len % 2u) != 0) {
        return false;
    }
    s_unlock();
    (void)s_wait();
    FLASH->CR |= FLASH_CR_PG;
    bool ok = true;
    for (uint32_t i = 0; ok && i < len; i += 2u) {
        volatile uint16_t* dst      = (volatile uint16_t*)(addr + i);
        const uint16_t     halfword = (uint16_t)(data[i] | (data[i + 1u] << 8));
        *dst                        = halfword;
        ok                          = s_wait() && *dst == halfword;
    }
    FLASH->CR &= ~FLASH_CR_PG;
    s_lock();
    return ok;
}

#if LINBOOT_STM32F0_FORCE_FLASH_BOOT
void port_force_flash_boot(void)
{
    // FLASH_OBR has the user option byte at bits 15..8.
    const uint32_t obr = FLASH->OBR;
    if ((obr & FLASH_OBR_BOOT_SEL) == 0 && (obr & FLASH_OBR_nBOOT0) != 0) {
        return;
    }

    // The option bytes are half-words with the value in the low byte (RDP, USER, DATA0, DATA1,
    // WRP0); keep all but the boot selection as they are.
    volatile uint16_t* const options = (volatile uint16_t*)OB_BASE;
    uint8_t                  values[sizeof(OB_TypeDef) / 2u];
    for (uint32_t i = 0; i < sizeof(values); ++i) {
        values[i] = (uint8_t)options[i];
    }
    values[1] = (uint8_t)((values[1] | (FLASH_OBR_nBOOT0 >> 8)) & ~(FLASH_OBR_BOOT_SEL >> 8));

    s_unlock();
    FLASH->OPTKEYR = FLASH_OPTKEY1;
    FLASH->OPTKEYR = FLASH_OPTKEY2;
    FLASH->CR |= FLASH_CR_OPTER;
    FLASH->CR |= FLASH_CR_STRT;
    (void)s_wait();
    FLASH->CR = (FLASH->CR & ~FLASH_CR_OPTER) | FLASH_CR_OPTPG;
    for (uint32_t i = 0; i < sizeof(values); ++i) {
        // Erased values don't need programming (the hardware writes the complement byte).
        if (values[i] != 0xFFu) {
            options[i] = values[i];
            (void)s_wait();
        }
    }

    // Reload the option bytes: resets the device.
    FLASH->CR |= FLASH_CR_OBL_LAUNCH;
    for (;;) { }
}
#endif
