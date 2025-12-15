/**
 * W5500 Ethernet Library
 * Simplified HTTP server for Z1 Onyx Controller
 */

#ifndef W5500_ETH_H
#define W5500_ETH_H

#include <stdint.h>
#include <stdbool.h>

// Initialize W5500 and configure network
// IP: 192.168.1.222, Mask: 255.255.255.0, No Gateway
bool w5500_eth_init(void);

// Start HTTP server on port 80 with multi-socket support
bool w5500_eth_start_server(uint16_t port);

// Process incoming connections (call in main loop)
void w5500_eth_process(void);

#endif // W5500_ETH_H
