/**
 * @file
 * @brief STM32F0 port: LIN interface on a USART in LIN mode, and the minlin hardware functions.
 *
 * Received bytes are fed to minlin from the receive interrupt. A BREAK is received as a 0x00 byte
 * with a framing error, which is dropped; the LIN break detection flag is left set so that it is
 * reported with the following SYNC byte. Transmitting is blocking; the transceiver echoes
 * everything we send back to the receiver, which is how the commander role sees its own headers.
 *
 * SPDX-License-Identifier: MIT
 * Copyright 2026 Olli Kokko <https://github.com/oko256/stm32-lin-bootloader>
 */

#include "minlin/minlin_hw.h"
#include "port/port.h"
#include "port_stm32f0.h"

/// BREAK flag of the byte being processed by the receive interrupt.
static bool s_rx_break = false;

/// PRIMASK state saved on critical section entry; minlin never nests critical sections.
static uint32_t s_primask = 0;

/// Transmit time of one byte with a 1.5x margin, in ms (rounded up).
#define BYTE_TIMEOUT_MS ((15000u + LINBOOT_BAUD - 1u) / LINBOOT_BAUD)

/// Transmit timeout for @p byte_count bytes (a BREAK counts as 2), plus two ticks of margin.
static uint32_t s_timeout_ms(uint32_t byte_count) { return 2u + byte_count * BYTE_TIMEOUT_MS; }

/// Waits until @p flag is set (or cleared, if @p set is false) in the ISR. Returns false on timeout.
static bool s_wait_isr(uint32_t flag, bool set, uint32_t start_ms, uint32_t timeout_ms)
{
    while (((LIN_USART->ISR & flag) != 0) != set) {
        if (port_tick_ms() - start_ms > timeout_ms) {
            return false;
        }
    }
    return true;
}

void port_lin_init(void)
{
    port_gpio_set_af(LINBOOT_LIN_TX_GPIO_INDEX, LINBOOT_LIN_TX_PIN, LINBOOT_LIN_AF, false);
    port_gpio_set_af(LINBOOT_LIN_RX_GPIO_INDEX, LINBOOT_LIN_RX_PIN, LINBOOT_LIN_AF, true);

    // The USART is clocked from PCLK, which runs at the system clock.
    LIN_USART_ENR |= LIN_USART_ENR_BIT;
    LIN_USART->CR1 = 0;
    LIN_USART->BRR = (LINBOOT_SYSCLK_HZ + LINBOOT_BAUD / 2u) / LINBOOT_BAUD;
    // LIN mode with 11-bit break detection. Overrun detection is disabled: bytes lost while the
    // CPU is stalled by a flash operation only show up as broken frames, and reception continues.
    LIN_USART->CR2 = USART_CR2_LINEN | USART_CR2_LBDL;
    LIN_USART->CR3 = USART_CR3_OVRDIS;
    LIN_USART->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_SetPriority(LIN_USART_IRQn, 0);
    NVIC_EnableIRQ(LIN_USART_IRQn);
}

void port_lin_usart_irq_handler(void)
{
    const uint32_t isr = LIN_USART->ISR;
    if ((isr & USART_ISR_RXNE) == 0) {
        return;
    }
    const uint8_t byte = (uint8_t)LIN_USART->RDR;
    if ((isr & (USART_ISR_FE | USART_ISR_NE)) != 0) {
        // The BREAK character itself (framing error), or a byte with noise: dropped.
        LIN_USART->ICR = USART_ICR_FECF | USART_ICR_NCF;
        return;
    }
    s_rx_break = (isr & USART_ISR_LBDF) != 0;
    if (s_rx_break) {
        LIN_USART->ICR = USART_ICR_LBDCF;
    }
    linboot_lin_rx_byte(byte);
    s_rx_break = false;
}

bool minlin_hw_check_and_clear_break(void)
{
    const bool had_break = s_rx_break;
    s_rx_break           = false;
    return had_break;
}

bool minlin_hw_transmit_break(void)
{
    const uint32_t start_ms = port_tick_ms();
    if (!s_wait_isr(USART_ISR_TXE, true, start_ms, s_timeout_ms(2))) {
        return false;
    }
    LIN_USART->RQR = USART_RQR_SBKRQ;
    // The flag is set right away and cleared at the end of the break.
    return s_wait_isr(USART_ISR_SBKF, false, start_ms, s_timeout_ms(2));
}

bool minlin_hw_transmit_data(const uint8_t* data, uint8_t data_size)
{
    const uint32_t start_ms   = port_tick_ms();
    const uint32_t timeout_ms = s_timeout_ms(data_size);
    for (uint8_t i = 0; i < data_size; ++i) {
        if (!s_wait_isr(USART_ISR_TXE, true, start_ms, timeout_ms)) {
            return false;
        }
        LIN_USART->TDR = data[i];
    }
    return s_wait_isr(USART_ISR_TC, true, start_ms, timeout_ms);
}

void minlin_hw_enter_critical(void)
{
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_primask = primask;
}

void minlin_hw_exit_critical(void) { __set_PRIMASK(s_primask); }
