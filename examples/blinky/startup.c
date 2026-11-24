/**
 * @file
 * @brief Startup code and vector table of the blinky example application.
 *
 * A normal application startup: nothing here is specific to the bootloader. The bootloader has
 * already copied this vector table to the RAM (and remapped it) before jumping to Reset_Handler.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "stm32f0xx.h"
#include <stdint.h>
#include <string.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int  main(void);
void Reset_Handler(void);
void SysTick_Handler(void);

typedef void (*vector)(void);

static void s_default_handler(void)
{
    for (;;) { }
}

/// 16 system vectors and 32 IRQs (the most a Cortex-M0 has): the bootloader copies this many.
#define VECTOR_COUNT 48

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
__attribute__((section(".isr_vector"), used)) static const vector s_vectors[VECTOR_COUNT] = {
    [0 ... VECTOR_COUNT - 1] = s_default_handler,
    [0]                      = (vector)(uintptr_t)&_estack,
    [1]                      = Reset_Handler,
    [15]                     = SysTick_Handler,
};
#pragma GCC diagnostic pop

void Reset_Handler(void)
{
    memcpy(&_sdata, &_sidata, (size_t)((uintptr_t)&_edata - (uintptr_t)&_sdata));
    memset(&_sbss, 0, (size_t)((uintptr_t)&_ebss - (uintptr_t)&_sbss));
    (void)main();
    for (;;) { }
}
