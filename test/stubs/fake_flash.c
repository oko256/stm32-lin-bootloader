#include "fake_flash.h"
#include "port/port.h"
#include <string.h>

_Alignas(8) uint8_t g_flash[FAKE_FLASH_SIZE];
uint32_t g_flash_erase_count     = 0;
uint32_t g_flash_program_count   = 0;
int32_t  g_flash_fail_erase_in   = -1;
int32_t  g_flash_fail_program_in = -1;
bool     g_flash_error           = false;

void fake_flash_reset(void)
{
    memset(g_flash, 0xFF, sizeof(g_flash));
    g_flash_erase_count     = 0;
    g_flash_program_count   = 0;
    g_flash_fail_erase_in   = -1;
    g_flash_fail_program_in = -1;
    g_flash_error           = false;
}

linboot_layout fake_flash_layout(void)
{
    return (linboot_layout){
        .app_addr     = (uintptr_t)g_flash,
        .app_max_size = FAKE_APP_SIZE,
        .meta_addr    = fake_flash_meta_addr(),
        .page_size    = LINBOOT_FLASH_PAGE_SIZE,
    };
}

/// Returns the offset of [addr, addr+len) in the flash, or -1 if it's not inside it.
static int64_t s_offset(uintptr_t addr, uint32_t len)
{
    const uintptr_t base = (uintptr_t)g_flash;
    if (addr < base || addr + len > base + sizeof(g_flash)) {
        return -1;
    }
    return (int64_t)(addr - base);
}

static bool s_inject(int32_t* countdown)
{
    if (*countdown == 0) {
        *countdown = -1;
        return true;
    }
    if (*countdown > 0) {
        (*countdown)--;
    }
    return false;
}

bool port_flash_erase_page(uintptr_t addr)
{
    const int64_t offset = s_offset(addr, LINBOOT_FLASH_PAGE_SIZE);
    if (offset < 0 || (offset % LINBOOT_FLASH_PAGE_SIZE) != 0) {
        g_flash_error = true;
        return false;
    }
    if (s_inject(&g_flash_fail_erase_in)) {
        return false;
    }
    memset(&g_flash[offset], 0xFF, LINBOOT_FLASH_PAGE_SIZE);
    g_flash_erase_count++;
    return true;
}

bool port_flash_program(uintptr_t addr, const uint8_t* data, uint32_t len)
{
    const int64_t offset = s_offset(addr, len);
    if (offset < 0 || (offset % LINBOOT_FLASH_WRITE_ALIGN) != 0 || (len % LINBOOT_FLASH_WRITE_ALIGN) != 0) {
        g_flash_error = true;
        return false;
    }
    if (s_inject(&g_flash_fail_program_in)) {
        return false;
    }
    for (uint32_t i = 0; i < len; i += 2) {
        uint8_t* dst = &g_flash[offset + i];
        if (dst[0] != 0xFF || dst[1] != 0xFF) {
            // Not erased: the hardware refuses this (PGERR).
            g_flash_error = true;
            return false;
        }
        dst[0] = data[i];
        dst[1] = data[i + 1];
    }
    g_flash_program_count++;
    return true;
}
