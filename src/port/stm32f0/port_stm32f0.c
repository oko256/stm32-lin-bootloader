/**
 * @file
 * @brief STM32F0 port: clocks, tick, option bytes and starting the application.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "port_stm32f0.h"
#include "port/port.h"

/// Millisecond tick, incremented by SysTick.
static volatile uint32_t s_tick_ms = 0;

void port_systick_handler(void) { s_tick_ms++; }

uint32_t port_tick_ms(void) { return s_tick_ms; }

// Harmless if the IWDG isn't running.
void port_watchdog_refresh(void) { IWDG->KR = 0xAAAAu; }

void port_gpio_set_af(uint32_t index, uint32_t pin, uint32_t af, bool pull_up)
{
    GPIO_TypeDef* gpio = PORT_GPIO(index);
    port_gpio_enable(index);
    gpio->AFR[pin / 8u] = (gpio->AFR[pin / 8u] & ~(0xFu << ((pin % 8u) * 4u))) | (af << ((pin % 8u) * 4u));
    gpio->OSPEEDR |= 3u << (pin * 2u);
    gpio->PUPDR = (gpio->PUPDR & ~(3u << (pin * 2u))) | ((pull_up ? 1u : 0u) << (pin * 2u));
    gpio->MODER = (gpio->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));
}

static void s_clock_init(void)
{
#if defined(LINBOOT_CLOCK_HSI48)
    RCC->CR2 |= RCC_CR2_HSI48ON;
    while ((RCC->CR2 & RCC_CR2_HSI48RDY) == 0) { }
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY;
    RCC->CFGR  = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSI48;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI48) { }
#elif defined(LINBOOT_CLOCK_HSE)
#if LINBOOT_HSE_BYPASS
    RCC->CR |= RCC_CR_HSEBYP;
#endif
    RCC->CR |= RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY) == 0) { }
#if LINBOOT_SYSCLK_HZ > 24000000u
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY;
#endif
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_HSE;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSE) { }
#else
    // HSI (8 MHz) is the reset default: nothing to do.
#endif
}

/// Returns the clocks to the reset state (HSI), as the application expects.
static void s_clock_deinit(void)
{
    RCC->CFGR &= ~RCC_CFGR_SW;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_HSI) { }
#if defined(LINBOOT_CLOCK_HSI48)
    RCC->CR2 &= ~RCC_CR2_HSI48ON;
#elif defined(LINBOOT_CLOCK_HSE)
    RCC->CR &= ~(RCC_CR_HSEON | RCC_CR_HSEBYP);
#endif
    FLASH->ACR &= ~FLASH_ACR_LATENCY;
}

void port_init(void)
{
#if LINBOOT_STM32F0_FORCE_FLASH_BOOT
    port_force_flash_boot();
#endif
    s_clock_init();
    (void)SysTick_Config(LINBOOT_SYSCLK_HZ / 1000u);

#if defined(LINBOOT_LIN_EN_PIN)
    GPIO_TypeDef* gpio = PORT_GPIO(LINBOOT_LIN_EN_GPIO_INDEX);
    port_gpio_enable(LINBOOT_LIN_EN_GPIO_INDEX);
    gpio->BSRR  = LINBOOT_LIN_EN_ACTIVE_LOW ? (1u << (LINBOOT_LIN_EN_PIN + 16u)) : (1u << LINBOOT_LIN_EN_PIN);
    gpio->MODER = (gpio->MODER & ~(3u << (LINBOOT_LIN_EN_PIN * 2u))) | (1u << (LINBOOT_LIN_EN_PIN * 2u));
#endif
}

_Noreturn void port_boot_app(uintptr_t app_addr)
{
    __disable_irq();

    // Stop everything the bootloader has used, and return the clocks to the reset state.
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;
    NVIC->ICER[0] = 0xFFFFFFFFu;
    NVIC->ICPR[0] = 0xFFFFFFFFu;
    SCB->ICSR     = SCB_ICSR_PENDSTCLR_Msk;
    LIN_USART_RSTR |= LIN_USART_RSTR_BIT;
    LIN_USART_RSTR &= ~LIN_USART_RSTR_BIT;
    const uint32_t gpio_rst = RCC_AHBRSTR_GPIOARST | RCC_AHBRSTR_GPIOBRST | RCC_AHBRSTR_GPIOCRST | RCC_AHBRSTR_GPIOFRST;
    RCC->AHBRSTR |= gpio_rst;
    RCC->AHBRSTR &= ~gpio_rst;
    s_clock_deinit();

    // Cortex-M0 has no VTOR: copy the application's vector table to the start of the RAM and map
    // the RAM to address 0. This overwrites the bootloader's own variables, so nothing may use them
    // after this.
    const uint32_t*    src = (const uint32_t*)app_addr;
    volatile uint32_t* dst = (volatile uint32_t*)LINBOOT_RAM_BASE;
    for (uint32_t i = 0; i < LINBOOT_APP_VECTOR_RAM_SIZE / 4u; ++i) {
        dst[i] = src[i];
    }
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGCOMPEN;
    SYSCFG->CFGR1 = (SYSCFG->CFGR1 & ~SYSCFG_CFGR1_MEM_MODE) | SYSCFG_CFGR1_MEM_MODE;
    LIN_USART_ENR &= ~LIN_USART_ENR_BIT;
    RCC->APB1ENR = 0;

    // Load the application's stack pointer, enable interrupts like after a reset (nothing is
    // enabled in the NVIC any more), and jump to its reset handler.
    const uint32_t sp = src[0];
    const uint32_t pc = src[1];
    __DSB();
    __ISB();
    __asm volatile("msr msp, %0\n"
                   "cpsie i\n"
                   "bx %1\n"
                   :
                   : "r"(sp), "r"(pc)
                   : "memory");
    for (;;) { }
}
