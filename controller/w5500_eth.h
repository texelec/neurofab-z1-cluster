/**
 * W5500 Ethernet Library
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Simplified HTTP server for Z1 Onyx Controller
 */

#ifndef W5500_ETH_H
#define W5500_ETH_H

#include <stdint.h>
#include <stdbool.h>

// Get the configured IP address as a string (for display/logging)
const char* w5500_get_ip_string(void);

// Set network configuration from config file (call before w5500_eth_init)
// Pass NULL for parameters you don't want to change
void w5500_set_network_config(const uint8_t* ip, const uint8_t* mac);

// Initialize W5500 and configure network
// Network settings are loaded from z1.cfg or use defaults if not available
bool w5500_eth_init(void);

// Start HTTP server on port 80 with multi-socket support
bool w5500_eth_start_server(uint16_t port);

// Process incoming connections (call in main loop)
void w5500_eth_process(void);

#endif // W5500_ETH_H
