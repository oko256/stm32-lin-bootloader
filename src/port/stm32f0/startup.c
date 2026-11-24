/**
 * @file
 * @brief Startup code and a minimal vector table for the bootloader.
 *
 * The vector table only extends up to the LIN USART interrupt, which is the last interrupt the
 * bootloader uses; everything else goes to a handler that just resets the device.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "port_stm32f0.h"
#include <string.h>

// Symbols from the linker script.
extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);

void Reset_Handler(void);

typedef void (*vector)(void);

/// Unexpected interrupts and faults: start over.
static void s_default_handler(void) { NVIC_SystemReset(); }

/// Number of vectors: 16 system vectors plus the IRQs up to the LIN USART.
#define VECTOR_COUNT (16 + LIN_USART_IRQn + 1)

// The table is first filled with the default handler, and the used entries are then overridden.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) static const vector s_vectors[VECTOR_COUNT] = {
    [0 ... VECTOR_COUNT - 1] = s_default_handler,
    [0]                      = (vector)(uintptr_t)&_estack,
    [1]                      = Reset_Handler,
    [15]                     = port_systick_handler,
    [16 + LIN_USART_IRQn]    = port_lin_usart_irq_handler,
};
#pragma GCC diagnostic pop

/// C part of the reset handler: initialize the RAM and start main(). Not static, so that the name
/// stays the same for the assembly of Reset_Handler (also with LTO).
__attribute__((noreturn, used)) void port_reset_c(void);
__attribute__((noreturn, used)) void port_reset_c(void)
{
    // If the bootloader was started from the system memory (e.g. the ST bootloader jumped here), or
    // something left the RAM remapped, map the main flash to address 0 so that our vector table is
    // used.
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGCOMPEN;
    SYSCFG->CFGR1 &= ~SYSCFG_CFGR1_MEM_MODE;

    memcpy(&_sdata, &_sidata, (size_t)((uintptr_t)&_edata - (uintptr_t)&_sdata));
    memset(&_sbss, 0, (size_t)((uintptr_t)&_ebss - (uintptr_t)&_sbss));
    (void)main();
    for (;;) { }
}

/// Reset handler: set the stack pointer (it is not set by the hardware if we were started by
/// something else than a reset), then continue in C.
__attribute__((naked)) void Reset_Handler(void)
{
    __asm volatile("ldr r0, =_estack\n"
                   "mov sp, r0\n"
                   "bl port_reset_c\n"
                   // Literal pool for _estack right here (port_reset_c doesn't return).
                   ".ltorg\n");
}
