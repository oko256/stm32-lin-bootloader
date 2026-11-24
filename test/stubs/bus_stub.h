/**
 * @file
 * @brief Simulated LIN bus for the COMMANDER role tests: linboot_lin_send_header() calls a hook,
 * which plays the other nodes (and the commander's own response) synchronously.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/// Called for every header the commander sends.
extern void (*g_bus_on_header)(uint8_t id);
/// Number of headers sent.
extern uint32_t g_bus_header_count;
/// Make linboot_lin_send_header() fail (no hook call).
extern bool     g_bus_tx_fail;

void bus_reset(void);
