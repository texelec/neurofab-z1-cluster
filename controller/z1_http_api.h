/**
 * Z1 Cluster HTTP REST API - Minimal Working Implementation
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Simple API for node management and SNN control
 */

#ifndef Z1_HTTP_API_H
#define Z1_HTTP_API_H

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Response Metadata
// ============================================================================

/**
 * HTTP response metadata
 * Used to communicate binary file downloads vs JSON responses
 */
typedef struct {
    bool is_binary;        // true if response is binary file data
    int content_length;    // byte count for binary data (ignored for JSON)
    const char* content_type;  // NULL = auto-detect, else custom content type
} http_response_metadata_t;

/**
 * Get response metadata for current HTTP request
 * Used by w5500_eth.c to determine Content-Type and length
 */
http_response_metadata_t* z1_http_api_get_response_metadata(void);

// ============================================================================
// API Router
// ============================================================================

/**
 * Route HTTP request to appropriate handler
 * 
 * @param method HTTP method ("GET", "POST", etc.)
 * @param path   URL path ("/api/nodes")
 * @param body   Request body (for POST, can be NULL)
 * @param response Output buffer for JSON response
 * @param size   Response buffer size
 * @return HTTP status code (200, 404, 500, etc.)
 * 
 * Supported Endpoints:
 *   GET  /api/status              - Controller status
 *   GET  /api/nodes               - List all nodes (0-15)
 *   GET  /api/nodes/{id}          - Get specific node status
 *   POST /api/nodes/{id}/snn/start - Start SNN on node
 *   POST /api/nodes/{id}/snn/stop  - Stop SNN on node
 *   POST /api/snn/reset           - Global reset all nodes
 *   POST /api/ota/update_start    - Start OTA update session
 *   POST /api/ota/update_chunk    - Send firmware chunk
 *   POST /api/ota/update_verify   - Verify firmware CRC32
 *   POST /api/ota/update_commit   - Commit firmware to flash
 *   POST /api/ota/update_restart  - Restart node with new firmware
 *   GET  /api/ota/status          - Get OTA session status
 */
int z1_http_api_route(const char* method, const char* path, const char* body,
                      char* response, int size);

/**
 * Background spike injection processor
 * Call repeatedly from main loop to process queued spike jobs
 * Processes one batch per call to avoid blocking
 */
void z1_http_api_process_spikes(void);

// OTA Handler Functions (internal)
void handle_ota_update_start(const char* body, char* response, int size);
void handle_ota_update_chunk(const char* body, char* response, int size);
void handle_ota_update_verify(const char* body, char* response, int size);
void handle_ota_update_commit(const char* body, char* response, int size);
void handle_ota_update_restart(const char* body, char* response, int size);
void handle_ota_status(char* response, int size);

// Firmware Deployment (Controller-based) - Phase 3
void handle_firmware_deploy(const char* body, char* response, int size);

#endif // Z1_HTTP_API_H
