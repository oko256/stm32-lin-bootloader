/**
 * @file
 * @brief Blinky example application: toggles an LED every BLINKY_HALF_PERIOD_MS from SysTick.
 *
 * Runs on the reset clock (HSI, 8 MHz), which the bootloader restores before starting the
 * application.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "stm32f0xx.h"
#include <stdbool.h>
#include <stdint.h>

#define HSI_HZ 8000000u

#define LED_GPIO ((GPIO_TypeDef*)(GPIOA_BASE + BLINKY_LED_GPIO_INDEX * 0x400u))

static volatile uint32_t s_tick_ms = 0;

void SysTick_Handler(void) { s_tick_ms++; }

static void s_set_led(bool on)
{
    const bool high = on != (bool)BLINKY_LED_ACTIVE_LOW;
    LED_GPIO->BSRR  = high ? (1u << BLINKY_LED_PIN) : (1u << (BLINKY_LED_PIN + 16u));
}

int main(void)
{
    RCC->AHBENR |= RCC_AHBENR_GPIOAEN << BLINKY_LED_GPIO_INDEX;
    s_set_led(false);
    LED_GPIO->MODER = (LED_GPIO->MODER & ~(3u << (BLINKY_LED_PIN * 2u))) | (1u << (BLINKY_LED_PIN * 2u));

    (void)SysTick_Config(HSI_HZ / 1000u);

    bool     on      = false;
    uint32_t last_ms = 0;
    for (;;) {
        if (s_tick_ms - last_ms >= BLINKY_HALF_PERIOD_MS) {
            last_ms += BLINKY_HALF_PERIOD_MS;
            on = !on;
            s_set_led(on);
        }
        __WFI();
    }
}
