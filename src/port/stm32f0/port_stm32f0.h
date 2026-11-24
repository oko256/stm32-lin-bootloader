/**
 * @file
 * @brief Private definitions of the STM32F0 port.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */
#pragma once

#include "linboot_config.h"
#include "stm32f0xx.h"
#include <stdbool.h>
#include <stdint.h>

// The LIN USART.
#if LINBOOT_LIN_USART == 1
#define LIN_USART          USART1
#define LIN_USART_IRQn     USART1_IRQn
#define LIN_USART_ENR      RCC->APB2ENR
#define LIN_USART_ENR_BIT  RCC_APB2ENR_USART1EN
#define LIN_USART_RSTR     RCC->APB2RSTR
#define LIN_USART_RSTR_BIT RCC_APB2RSTR_USART1RST
#elif LINBOOT_LIN_USART == 2
#define LIN_USART          USART2
#define LIN_USART_IRQn     USART2_IRQn
#define LIN_USART_ENR      RCC->APB1ENR
#define LIN_USART_ENR_BIT  RCC_APB1ENR_USART2EN
#define LIN_USART_RSTR     RCC->APB1RSTR
#define LIN_USART_RSTR_BIT RCC_APB1RSTR_USART2RST
#else
#error "Unsupported LINBOOT_LIN_USART"
#endif

/// Returns the GPIO port with index @p index (0 = GPIOA); the ports are 0x400 apart.
#define PORT_GPIO(index) ((GPIO_TypeDef*)(GPIOA_BASE + (index) * 0x400u))

/// Enables the clock of the GPIO port with index @p index.
static inline void port_gpio_enable(uint32_t index) { RCC->AHBENR |= RCC_AHBENR_GPIOAEN << index; }

/// Configures @p pin of the GPIO port @p index to alternate function @p af.
void port_gpio_set_af(uint32_t index, uint32_t pin, uint32_t af, bool pull_up);

/// Makes sure that the device boots from the main flash regardless of the BOOT0 pin (BOOT_SEL = 0,
/// nBOOT0 = 1), reprogramming the option bytes (and resetting) if needed. In flash.c.
void port_force_flash_boot(void);

/// Interrupt handler of the LIN USART (in lin_usart.c), placed in the vector table by startup.c.
void port_lin_usart_irq_handler(void);

/// SysTick interrupt handler (in port_stm32f0.c).
void port_systick_handler(void);
