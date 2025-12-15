/**
 * Z1 Cluster HTTP REST API - Minimal Working Implementation
 * 
 * Simple API for node management and SNN control
 */

#ifndef Z1_HTTP_API_H
#define Z1_HTTP_API_H

#include <stdint.h>
#include <stdbool.h>

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
 */
int z1_http_api_route(const char* method, const char* path, const char* body,
                      char* response, int size);

#endif // Z1_HTTP_API_H
