#include "bus_stub.h"
#include "lin_bus.h"
#include <stddef.h>

void (*g_bus_on_header)(uint8_t id) = NULL;
uint32_t g_bus_header_count         = 0;
bool     g_bus_tx_fail              = false;

void bus_reset(void)
{
    g_bus_on_header    = NULL;
    g_bus_header_count = 0;
    g_bus_tx_fail      = false;
}

bool linboot_lin_send_header(uint8_t id)
{
    g_bus_header_count++;
    if (g_bus_tx_fail) {
        return false;
    }
    if (g_bus_on_header != NULL) {
        g_bus_on_header(id);
    }
    return true;
}
