/**
 * Z1 Onyx Cluster - Configuration File Management
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Handles z1.cfg file on SD card with system configuration:
 * - IP address and MAC address
 * - Current SNN engine installed
 * - Hardware configuration
 */

#ifndef Z1_CONFIG_H
#define Z1_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define Z1_CONFIG_FILE "z1.cfg"
#define Z1_MAX_ENGINE_NAME 64

/**
 * System configuration structure
 */
typedef struct {
    uint8_t ip_address[4];           // IP address (e.g., 192.168.1.222)
    uint8_t mac_address[6];          // MAC address
    char current_engine[Z1_MAX_ENGINE_NAME];  // Name of installed engine (e.g., "lif_engine_v1.0.0")
    uint8_t hw_version;              // Hardware version (1 or 2)
    uint8_t node_count;              // Number of nodes (12 or 16)
} z1_config_t;

/**
 * Load configuration from SD card
 * 
 * @param config Output parameter - loaded config
 * @return true on success, false if file not found or parse error
 */
bool z1_config_load(z1_config_t* config);

/**
 * Save configuration to SD card
 * 
 * @param config Config structure to save
 * @return true on success, false on write error
 */
bool z1_config_save(const z1_config_t* config);

/**
 * Load config or create default if not found
 * 
 * @param config Output parameter
 * @return true on success (loaded or created default)
 */
bool z1_config_load_or_default(z1_config_t* config);

#ifdef __cplusplus
}
#endif

#endif // Z1_CONFIG_H
