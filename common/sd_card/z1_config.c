/**
 * Z1 Onyx Cluster - Configuration File Implementation
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Handles z1.cfg in INI-style format:
 * 
 * [network]
 * ip=192.168.1.222
 * mac=02:5a:31:c3:d4:01
 * 
 * [system]
 * engine=lif_engine_v1.0.0
 * hw_version=2
 * node_count=16
 * 
 * Note: MAC address must use Z1 Onyx prefix 02:5A:31 (locally administered)
 */

#include "z1_config.h"
#include "sd_card.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Default configuration
static const z1_config_t DEFAULT_CONFIG = {
    .ip_address = {192, 168, 1, 222},
    .mac_address = {0x02, 0x5A, 0x31, 0xC3, 0xD4, 0x01},  // Z1 Onyx prefix: 02:5A:31
    .current_engine = "none",
    .hw_version = 2,
    .node_count = 16
};

/**
 * Parse IP address string "192.168.1.222" -> uint8_t[4]
 */
static bool parse_ip(const char* str, uint8_t* ip) {
    int a, b, c, d;
    if (sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    ip[0] = a; ip[1] = b; ip[2] = c; ip[3] = d;
    return true;
}

/**
 * Parse MAC address string "02:00:00:00:00:01" -> uint8_t[6]
 */
static bool parse_mac(const char* str, uint8_t* mac) {
    int a, b, c, d, e, f;
    if (sscanf(str, "%x:%x:%x:%x:%x:%x", &a, &b, &c, &d, &e, &f) != 6) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 ||
        d < 0 || d > 255 || e < 0 || e > 255 || f < 0 || f > 255) return false;
    mac[0] = a; mac[1] = b; mac[2] = c; mac[3] = d; mac[4] = e; mac[5] = f;
    return true;
}

/**
 * Trim whitespace from string
 */
static void trim(char* str) {
    // Trim leading
    char* start = str;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != str) memmove(str, start, strlen(start) + 1);
    
    // Trim trailing
    char* end = str + strlen(str) - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
}

/**
 * Load configuration from SD card
 */
bool z1_config_load(z1_config_t* config) {
    uint8_t* buffer;
    size_t size;
    
    if (!sd_card_read_file(Z1_CONFIG_FILE, &buffer, &size)) {
        printf("[Config] File not found: %s\n", Z1_CONFIG_FILE);
        return false;
    }
    
    // Parse line by line
    char* line = strtok((char*)buffer, "\n");
    while (line != NULL) {
        trim(line);
        
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == ';' || line[0] == '\0' || line[0] == '[') {
            line = strtok(NULL, "\n");
            continue;
        }
        
        // Parse key=value
        char* equals = strchr(line, '=');
        if (equals != NULL) {
            *equals = '\0';
            char* key = line;
            char* value = equals + 1;
            trim(key);
            trim(value);
            
            if (strcmp(key, "ip") == 0) {
                parse_ip(value, config->ip_address);
            } else if (strcmp(key, "mac") == 0) {
                parse_mac(value, config->mac_address);
            } else if (strcmp(key, "engine") == 0) {
                strncpy(config->current_engine, value, Z1_MAX_ENGINE_NAME - 1);
                config->current_engine[Z1_MAX_ENGINE_NAME - 1] = '\0';
            } else if (strcmp(key, "hw_version") == 0) {
                config->hw_version = atoi(value);
            } else if (strcmp(key, "node_count") == 0) {
                config->node_count = atoi(value);
            }
        }
        
        line = strtok(NULL, "\n");
    }
    
    free(buffer);
    printf("[Config] Loaded %s\n", Z1_CONFIG_FILE);
    return true;
}

/**
 * Save configuration to SD card
 */
bool z1_config_save(const z1_config_t* config) {
    char buffer[512];
    int len = snprintf(buffer, sizeof(buffer),
        "# Z1 Onyx Cluster Configuration\n"
        "# Generated automatically - edit with care\n"
        "\n"
        "[network]\n"
        "ip=%d.%d.%d.%d\n"
        "mac=%02x:%02x:%02x:%02x:%02x:%02x\n"
        "\n"
        "[system]\n"
        "engine=%s\n"
        "hw_version=%d\n"
        "node_count=%d\n",
        config->ip_address[0], config->ip_address[1],
        config->ip_address[2], config->ip_address[3],
        config->mac_address[0], config->mac_address[1],
        config->mac_address[2], config->mac_address[3],
        config->mac_address[4], config->mac_address[5],
        config->current_engine,
        config->hw_version,
        config->node_count
    );
    
    if (len < 0 || len >= (int)sizeof(buffer)) {
        printf("[Config] ERROR: Buffer overflow\n");
        return false;
    }
    
    if (!sd_card_write_file(Z1_CONFIG_FILE, buffer, len)) {
        printf("[Config] ERROR: Failed to write %s\n", Z1_CONFIG_FILE);
        return false;
    }
    
    printf("[Config] Saved %s\n", Z1_CONFIG_FILE);
    return true;
}

/**
 * Load config or create default if not found
 */
bool z1_config_load_or_default(z1_config_t* config) {
    if (z1_config_load(config)) {
        return true;
    }
    
    // Create default
    printf("[Config] Creating default configuration\n");
    *config = DEFAULT_CONFIG;
    
    if (!z1_config_save(config)) {
        printf("[Config] WARNING: Failed to save default config\n");
    }
    
    return true;
}
