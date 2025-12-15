/**
 * Z1 Cluster HTTP REST API
 * 
 * Provides RESTful interface for node management, SNN control, and spike injection.
 * All endpoints return JSON responses. See API_REFERENCE.md for full API reference.
 * 
 * Architecture:
 * - Runs on controller's Core 0 (same core as bus broker for direct access)
 * - Uses z1_broker for all node communication
 * - All commands are queued, then z1_broker_task() pumps them onto the bus
 * - HTTP handlers block waiting for responses (with timeouts)
 * 
 * Key Design Decisions:
 * - Spike injection uses NO_ACK flag for maximum throughput
 * - Global commands (start/stop) broadcast to all 16 nodes
 * - Memory writes split into 384-byte chunks (max frame payload)
 * - Base64 encoding used for binary PSRAM data transfer
 */

#include "z1_http_api.h"
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Response buffer (shared with w5500_eth.c)
extern char http_response_buffer[4096];

// Global SNN state tracking
static uint16_t g_total_neurons_deployed = 0;

// ============================================================================
// JSON Helper Functions
// ============================================================================

static int json_start(char* buf, int size) {
    if (size < 2) return -1;
    buf[0] = '{';
    buf[1] = '\0';
    return 1;
}

static int json_end(char* buf, int pos, int size) {
    if (pos >= size - 2) return -1;
    buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}

static int json_add_string(char* buf, int pos, int size, const char* key, const char* val, bool last) {
    int n = snprintf(buf + pos, size - pos, "\"%s\":\"%s\"%s", key, val, last ? "" : ",");
    if (n < 0 || pos + n >= size) return -1;
    return pos + n;
}

static int json_add_int(char* buf, int pos, int size, const char* key, int val, bool last) {
    int n = snprintf(buf + pos, size - pos, "\"%s\":%d%s", key, val, last ? "" : ",");
    if (n < 0 || pos + n >= size) return -1;
    return pos + n;
}

static int json_add_bool(char* buf, int pos, int size, const char* key, bool val, bool last) {
    int n = snprintf(buf + pos, size - pos, "\"%s\":%s%s", key, val ? "true" : "false", last ? "" : ",");
    if (n < 0 || pos + n >= size) return -1;
    return pos + n;
}

// ============================================================================
// API Handlers
// ============================================================================

/**
 * GET /api/status - Controller health and bus statistics
 * 
 * Returns basic controller metrics including bus RX/TX counts.
 * Does NOT query individual nodes (use /api/nodes for that).
 * 
 * Response: {"bus_rx_count": N, "bus_tx_count": M, "uptime_ms": T}
 */
void handle_get_status(char* response, int size) {
    int pos = json_start(response, size);
    pos = json_add_string(response, pos, size, "controller", "Z1 Onyx", false);
    pos = json_add_string(response, pos, size, "version", "3.0", false);
    pos = json_add_bool(response, pos, size, "bus_active", true, false);
    
    // Get broker stats
    z1_broker_stats_t stats;
    z1_broker_get_stats(&stats);
    pos = json_add_int(response, pos, size, "frames_sent", stats.total_sent, false);
    pos = json_add_int(response, pos, size, "frames_dropped", stats.total_dropped, true);
    
    json_end(response, pos, size);
}

/**
 * GET /api/nodes - List all nodes with detailed status
 * 
 * Queries all 16 possible nodes (0-15) with READ_STATUS command.
 * Each node has 100ms timeout. Offline nodes are marked as "offline".
 * 
 * Response includes:
 * - Node ID, status (online/offline)
 * - Uptime, free memory
 * - SNN state (running/stopped), neuron count
 * 
 * Note: This can take up to 1.6 seconds for full scan.
 * Use /api/nodes/discover for faster active node detection.
 */
void handle_get_nodes(char* response, int size) {
    int pos = json_start(response, size);
    
    // Start nodes array
    int n = snprintf(response + pos, size - pos, "\"nodes\":[");
    if (n < 0 || pos + n >= size) return;
    pos += n;
    
    // Query nodes 0-15 (simple ping check)
    bool first = true;
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        // Send READ_STATUS command
        uint16_t cmd = OPCODE_READ_STATUS;
        if (z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
            // Wait for response (100ms timeout)
            uint32_t timeout = time_us_32() + 100000;
            z1_frame_t frame;
            bool responded = false;
            
            while (time_us_32() < timeout) {
                if (z1_broker_try_receive(&frame)) {
                    if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL) {
                        responded = true;
                        break;
                    }
                }
                z1_broker_task();
                sleep_us(100);
            }
            
            if (responded) {
                if (!first) {
                    n = snprintf(response + pos, size - pos, ",");
                    if (n < 0 || pos + n >= size) break;
                    pos += n;
                }
                first = false;
                
                // Parse STATUS_RESPONSE (11 words):
                // [0]=opcode, [1]=node_id, [2-3]=uptime_ms, [4-5]=memory_free,
                // [6]=led_r, [7]=led_g, [8]=led_b, [9]=snn_running, [10]=neuron_count
                uint32_t uptime_ms = ((uint32_t)frame.payload[3] << 16) | frame.payload[2];
                uint32_t memory_free = ((uint32_t)frame.payload[5] << 16) | frame.payload[4];
                uint8_t led_r = frame.payload[6] & 0xFF;
                uint8_t led_g = frame.payload[7] & 0xFF;
                uint8_t led_b = frame.payload[8] & 0xFF;
                bool snn_running = (frame.payload[9] != 0);
                uint16_t neuron_count = frame.payload[10];
                
                n = snprintf(response + pos, size - pos,
                            "{\"id\":%d,\"status\":\"online\",\"memory_free\":%lu,\"uptime_ms\":%lu,"
                            "\"led_state\":{\"r\":%d,\"g\":%d,\"b\":%d},\"snn_running\":%s,\"neurons\":%d}",
                            node_id, (unsigned long)memory_free, (unsigned long)uptime_ms,
                            led_r, led_g, led_b,
                            snn_running ? "true" : "false", neuron_count);
                if (n < 0 || pos + n >= size) break;
                pos += n;
            }
        }
    }
    
    // End array
    n = snprintf(response + pos, size - pos, "]}");
    if (n > 0 && pos + n < size) pos += n;
    response[pos] = '\0';
}

/**
 * GET /api/nodes/{id} - Get detailed status for specific node
 * 
 * Sends READ_STATUS command to target node, waits 100ms for response.
 * Returns single node object with all status fields.
 * 
 * Response on success:
 *   {"id": N, "status": "online", "uptime_ms": T, "memory_free": M, 
 *    "snn_running": bool, "neuron_count": C}
 * 
 * Response on timeout:
 *   {"error": "Timeout"}
 */
void handle_get_node(uint8_t node_id, char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    // Send READ_STATUS command
    uint16_t cmd = OPCODE_READ_STATUS;
    if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    
    // Wait for response
    uint32_t timeout = time_us_32() + 100000;
    z1_frame_t frame;

    
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == OPCODE_STATUS_RESPONSE) {
                
                // Parse new response format:
                // [0]=opcode, [1]=node_id, [2-3]=uptime_ms, [4-5]=memory_free,
                // [6]=led_r, [7]=led_g, [8]=led_b, [9]=snn_running, [10]=neuron_count
                
                uint32_t uptime_ms = ((uint32_t)frame.payload[3] << 16) | frame.payload[2];
                uint32_t memory_free = ((uint32_t)frame.payload[5] << 16) | frame.payload[4];
                uint8_t led_r = frame.payload[6] & 0xFF;
                uint8_t led_g = frame.payload[7] & 0xFF;
                uint8_t led_b = frame.payload[8] & 0xFF;
                bool snn_running = (frame.payload[9] != 0);
                uint16_t neuron_count = frame.payload[10];
                
                // Build JSON response
                int pos = json_start(response, size);
                pos = json_add_int(response, pos, size, "id", node_id, false);
                pos = json_add_bool(response, pos, size, "online", true, false);
                
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "\"uptime_ms\":%lu,", uptime_ms);
                if (pos + strlen(tmp) < size) {
                    strcpy(response + pos, tmp);
                    pos += strlen(tmp);
                }
                
                snprintf(tmp, sizeof(tmp), "\"memory_free\":%lu,", memory_free);
                if (pos + strlen(tmp) < size) {
                    strcpy(response + pos, tmp);
                    pos += strlen(tmp);
                }
                
                snprintf(tmp, sizeof(tmp), "\"led_state\":{\"r\":%d,\"g\":%d,\"b\":%d},", led_r, led_g, led_b);
                if (pos + strlen(tmp) < size) {
                    strcpy(response + pos, tmp);
                    pos += strlen(tmp);
                }
                
                pos = json_add_bool(response, pos, size, "snn_running", snn_running, false);
                pos = json_add_int(response, pos, size, "neurons", neuron_count, true);
                json_end(response, pos, size);
                return;
            }
        }
        z1_broker_task();
        sleep_us(100);
    }
    
    // Timeout - node offline
    int pos = json_start(response, size);
    pos = json_add_int(response, pos, size, "id", node_id, false);
    pos = json_add_bool(response, pos, size, "online", false, true);
    json_end(response, pos, size);
}

/**
 * POST /api/nodes/{id}/ping - Test node connectivity and measure latency
 * 
 * Sends PING command, measures round-trip time for PONG response.
 * Useful for diagnosing bus communication issues.
 * 
 * Response: {"node_id": N, "status": "online", "latency_us": T}
 * Timeout: 100ms (returns error if no response)
 */
void handle_ping_node(uint8_t node_id, char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    // Send PING command
    uint16_t cmd = OPCODE_PING;
    uint32_t start_time = time_us_32();
    
    if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    
    // Wait for PONG response
    uint32_t timeout = time_us_32() + 100000;  // 100ms
    z1_frame_t frame;

    
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == OPCODE_PONG) {
                
                uint32_t latency_us = time_us_32() - start_time;
                float latency_ms = latency_us / 1000.0f;
                
                // Build JSON response
                char tmp[128];
                snprintf(tmp, sizeof(tmp), "{\"status\":\"ok\",\"latency_ms\":%.2f}", latency_ms);
                strncpy(response, tmp, size);
                response[size-1] = '\0';
            
                return;
            }
        }
        z1_broker_task();
        sleep_us(100);
    }

    
    strcpy(response, "{\"error\":\"Node did not respond\"}");
    
    strcpy(response, "{\"error\":\"Node did not respond\"}");
}

/**
 * POST /api/nodes/discover - Fast discovery of all active nodes
 * 
 * Pings all 16 nodes with 50ms timeout each (vs 100ms for full status).
 * Much faster than GET /api/nodes when you only need active node IDs.
 * 
 * Response: {"active_nodes": [0, 1, 2, ...]}
 * Total time: ~800ms (50ms × 16 nodes)
 */
void handle_discover_nodes(char* response, int size) {
    printf("[HTTP API] Starting node discovery...\n");
    
    int pos = json_start(response, size);
    
    // Start active_nodes array
    int n = snprintf(response + pos, size - pos, "\"active_nodes\":[");
    if (n < 0 || pos + n >= size) return;
    pos += n;
    
    // Ping all nodes 0-15
    bool first = true;
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        uint16_t cmd = OPCODE_PING;
        if (z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
            // Wait for PONG (50ms per node for speed)
            uint32_t timeout = time_us_32() + 50000;
            z1_frame_t frame;
            
            while (time_us_32() < timeout) {
                z1_broker_task();  // CRITICAL: Transmit queued command!
                if (z1_broker_try_receive(&frame)) {
                    if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                        && frame.payload[0] == OPCODE_PONG) {
                        
                        // Node responded - add to list
                        if (!first) {
                            n = snprintf(response + pos, size - pos, ",");
                            if (n < 0 || pos + n >= size) break;
                            pos += n;
                        }
                        first = false;
                        
                        n = snprintf(response + pos, size - pos, "%d", node_id);
                        if (n < 0 || pos + n >= size) break;
                        pos += n;
                        
                        printf("  Node %d: ACTIVE\n", node_id);
                        break;
                    }
                }
                z1_broker_task();
                sleep_us(100);
            }
        }
    }
    
    // End array
    n = snprintf(response + pos, size - pos, "]}");
    if (n > 0 && pos + n < size) pos += n;
    response[pos] = '\0';
    
    printf("[HTTP API] Discovery complete\n");
}

/**
 * POST /api/snn/start - Start SNN execution on ALL nodes
 * 
 * Broadcasts START_SNN command to all 16 nodes (0-15).
 * Nodes begin processing spike queue and updating neuron states.
 * 
 * Note: Does NOT wait for ACKs (fire-and-forget for speed).
 * Use GET /api/snn/status to verify nodes started.
 * 
 * Response: {"status": "ok"}
 */
void handle_global_snn_start(char* response, int size) {
    printf("[HTTP API] Starting SNN on all nodes...\n");
    
    // Send START_SNN to all nodes 0-15
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        uint16_t cmd = OPCODE_START_SNN;
        if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_SNN_CONTROL)) {
            printf("[HTTP API] WARNING: Failed to queue START_SNN for node %d\n", node_id);
        }
    }
    
    // CRITICAL: Pump broker to transmit all 16 queued commands
    printf("[HTTP API] Transmitting START commands...\n");
    for (int i = 0; i < 100; i++) {  // Max 100 iterations
        z1_broker_task();
        sleep_us(100);  // 100us between transmissions
        // TODO: Check if queue is empty to exit early
    }
    printf("[HTTP API] START commands transmitted\n");
    
    strcpy(response, "{\"status\":\"ok\"}");
}

/**
 * POST /api/snn/stop - Stop SNN execution on ALL nodes
 * 
 * Broadcasts STOP_SNN command to all 16 nodes.
 * Nodes halt spike processing but retain neuron state.
 * 
 * Response: {"status": "ok"}
 */
void handle_global_snn_stop(char* response, int size) {
    printf("[HTTP API] Stopping SNN on all nodes...\n");
    
    // Send STOP_SNN to all nodes 0-15
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        uint16_t cmd = OPCODE_STOP_SNN;
        if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_SNN_CONTROL)) {
            printf("[HTTP API] WARNING: Failed to queue STOP_SNN for node %d\n", node_id);
        }
    }
    
    // CRITICAL: Pump broker to transmit all 16 queued commands
    printf("[HTTP API] Transmitting STOP commands...\n");
    for (int i = 0; i < 100; i++) {  // Max 100 iterations
        z1_broker_task();
        sleep_us(100);  // 100us between transmissions
    }
    printf("[HTTP API] STOP commands transmitted\n");
    
    strcpy(response, "{\"status\":\"ok\"}");
}

/**
 * GET /api/snn/status - Get cluster-wide SNN statistics
 * 
 * Queries first responding node (0-15) for SNN status.
 * Returns aggregated statistics for entire cluster.
 * 
 * Response:
 *   {"state": "running"/"stopped"/"unknown",
 *    "neuron_count": N,      // Total neurons deployed
 *    "active_neurons": A,    // Neurons that have fired
 *    "total_spikes": S,      // Cumulative spike count
 *    "spike_rate_hz": R}     // Current firing rate
 * 
 * Timeout: 100ms per node (tries all 16 until one responds)
 */
void handle_global_snn_status(char* response, int size) {
    // Query first active node for statistics
    uint16_t cmd = OPCODE_GET_SNN_STATUS;
    bool got_response = false;
    uint16_t active_neurons = 0;
    uint32_t total_spikes = 0;
    uint32_t spike_rate_hz = 0;
    bool is_running = false;
    
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_SNN_CONTROL)) {
            continue;
        }
        
        // Wait for response
        uint32_t timeout = time_us_32() + 100000;
        z1_frame_t frame;
        
        while (time_us_32() < timeout) {
            z1_broker_task();  // CRITICAL: Transmit queued command
            
            if (z1_broker_try_receive(&frame)) {
                if (frame.src == node_id && 
                    frame.type == Z1_FRAME_TYPE_CTRL && 
                    frame.payload[0] == OPCODE_SNN_STATUS) {
                    
                    // Parse response: [opcode, running, neuron_count, active_neurons, total_spikes(2), spike_rate(2)]
                    is_running = (frame.payload[1] != 0);
                    active_neurons = frame.payload[3];
                    memcpy(&total_spikes, &frame.payload[4], 4);
                    memcpy(&spike_rate_hz, &frame.payload[6], 4);
                    
                    got_response = true;
                    break;
                }
            }
            
            z1_broker_task();
            sleep_us(100);
        }
        
        if (got_response) break;
    }
    
    // Build JSON response
    if (got_response) {
        snprintf(response, size,
                 "{\"state\":\"%s\",\"neuron_count\":%u,\"active_neurons\":%u,\"total_spikes\":%lu,\"spike_rate_hz\":%lu}",
                 is_running ? "running" : "stopped",
                 g_total_neurons_deployed,
                 active_neurons,
                 (unsigned long)total_spikes,
                 (unsigned long)spike_rate_hz);
    } else {
        snprintf(response, size,
                 "{\"state\":\"unknown\",\"neuron_count\":%u,\"active_neurons\":0,\"total_spikes\":0,\"spike_rate_hz\":0}",
                 g_total_neurons_deployed);
    }
}

/**
 * POST /api/nodes/{id}/snn/start - Start SNN on specific node
 * 
 * Sends START_SNN command to single node.
 * Waits for ACK (100ms timeout).
 * 
 * Response: {"status": "ok"} or {"error": "Timeout waiting for ACK"}
 */
void handle_snn_start(uint8_t node_id, char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    uint16_t cmd = OPCODE_START_SNN;
    if (!z1_broker_send_command(&cmd, 1, node_id, 0)) {
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    
    // Wait for ACK
    uint32_t timeout = time_us_32() + 100000;
    z1_frame_t frame;
    
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL) {
                strcpy(response, "{\"status\":\"started\"}");
            
                return;
            }
        }
        z1_broker_task();
        sleep_us(100);
    }

    
    strcpy(response, "{\"error\":\"Node did not respond\"}");
}

/**
 * POST /api/nodes/{id}/snn/stop - Stop SNN on specific node
 * 
 * Sends STOP_SNN command to single node.
 * Waits for ACK (100ms timeout).
 * 
 * Response: {"status": "ok"} or {"error": "Timeout waiting for ACK"}
 */
void handle_snn_stop(uint8_t node_id, char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    uint16_t cmd = OPCODE_STOP_SNN;
    if (!z1_broker_send_command(&cmd, 1, node_id, 0)) {
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    
    // Wait for ACK
    uint32_t timeout = time_us_32() + 100000;
    z1_frame_t frame;

    
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL) {
                strcpy(response, "{\"status\":\"stopped\"}");
            
                return;
            }
        }
        z1_broker_task();
        sleep_us(100);
    }

    
    strcpy(response, "{\"error\":\"Node did not respond\"}");
}

/**
 * POST /api/snn/reset - Global reset of all nodes
 * 
 * Resets neuron spike counters and statistics on all nodes.
 * Does NOT clear topology or neuron state (use for statistics reset).
 * 
 * Note: Could use global_reset_nodes() hardware reset instead,
 * but currently uses software RESET commands for safety.
 * 
 * Response: {"status": "reset"}
 */
void handle_global_reset(char* response, int size) {
    // Reset neuron count tracking
    g_total_neurons_deployed = 0;
    
    // This would use the global_reset_nodes() function
    // For now, send RESET commands to all nodes
    
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        uint16_t cmd = OPCODE_RESET;
        z1_broker_send_command(&cmd, 1, node_id, 0);
    }
    
    strcpy(response, "{\"status\":\"reset_sent\"}");
}

/**
 * POST /api/nodes/{id}/memory - Write data to node PSRAM
 * Body: {"addr": 0, "data": "<base64-encoded binary>"}
 */
void handle_write_memory(uint8_t node_id, const char* body, char* response, int size) {
    printf("[HANDLE_WRITE_MEMORY] Called with node_id=%d\n", node_id);
    printf("[HANDLE_WRITE_MEMORY] body=%s\n", body ? body : "(NULL)");
    
    if (node_id >= 16) {
        printf("[HANDLE_WRITE_MEMORY] Invalid node_id=%d\n", node_id);
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    // Parse JSON body (simple parser for {"addr":X,"data":"..."})
    // Find addr value
    printf("[HANDLE_WRITE_MEMORY] Parsing JSON...\n");
    const char* addr_ptr = strstr(body, "\"addr\"");
    if (!addr_ptr) {
        printf("[HANDLE_WRITE_MEMORY] Missing addr field\n");
        strcpy(response, "{\"error\":\"Missing addr field\"}");
        return;
    }
    addr_ptr = strchr(addr_ptr, ':');
    if (!addr_ptr) {
        strcpy(response, "{\"error\":\"Invalid JSON\"}");
        return;
    }
    uint32_t addr = strtoul(addr_ptr + 1, NULL, 0);  // Support hex 0x prefix
    
    // Find data value (base64 string)
    printf("[HANDLE_WRITE_MEMORY] Looking for data field...\n");
    const char* data_ptr = strstr(body, "\"data\"");
    if (!data_ptr) {
        printf("[HANDLE_WRITE_MEMORY] Missing data field\n");
        strcpy(response, "{\"error\":\"Missing data field\"}");
        return;
    }
    printf("[HANDLE_WRITE_MEMORY] Found data field, looking for colon...\n");
    data_ptr = strchr(data_ptr, ':');
    if (!data_ptr) {
        printf("[HANDLE_WRITE_MEMORY] No colon after data\n");
        strcpy(response, "{\"error\":\"Invalid JSON\"}");
        return;
    }
    printf("[HANDLE_WRITE_MEMORY] Looking for opening quote...\n");
    // Skip to opening quote
    data_ptr = strchr(data_ptr, '\"');
    if (!data_ptr) {
        printf("[HANDLE_WRITE_MEMORY] No opening quote for data value\n");
        strcpy(response, "{\"error\":\"Invalid data format\"}");
        return;
    }
    data_ptr++;  // Skip opening quote
    
    printf("[HANDLE_WRITE_MEMORY] Looking for closing quote...\n");
    // Find closing quote
    const char* data_end = strchr(data_ptr, '\"');
    if (!data_end) {
        printf("[HANDLE_WRITE_MEMORY] No closing quote for data value\n");
        strcpy(response, "{\"error\":\"Unterminated data string\"}");
        return;
    }
    
    uint32_t b64_len = data_end - data_ptr;
    printf("[HANDLE_WRITE_MEMORY] Base64 string length: %lu\n", b64_len);
    // 1536 bytes decoded * 4/3 = 2048 base64 chars max
    // Using 2000 to leave margin for alignment
    if (b64_len == 0 || b64_len > 2000) {
        printf("[HANDLE_WRITE_MEMORY] Invalid b64_len=%lu (max 2000)\n", b64_len);
        strcpy(response, "{\"error\":\"Invalid data length\"}");
        return;
    }
    
    // Decode base64 (simple decoder - assumes clean input)
    // NOTE: Increased to 1536 bytes to safely handle 1024-byte payloads
    // (1368 base64 chars * 0.75 = 1026 bytes decoded + margin)
    // IMPORTANT: For payloads >1KB, client MUST fragment into multiple requests
    // with delays between calls to allow broker queue processing
    // Static to avoid stack overflow (1536 bytes is too large for stack)
    static uint8_t decoded[1536];
    uint16_t decoded_len = 0;
    
    // Base64 decode (simplified - good enough for neuron data)
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t val = 0;
    int bits = -8;
    
    for (uint32_t i = 0; i < b64_len; i++) {
        char c = data_ptr[i];
        if (c == '=') break;  // Padding
        
        const char* p = strchr(b64_table, c);
        if (!p) continue;  // Skip invalid chars
        
        val = (val << 6) | (p - b64_table);
        bits += 6;
        
        if (bits >= 0) {
            decoded[decoded_len++] = (val >> bits) & 0xFF;
            bits -= 8;
            if (decoded_len >= sizeof(decoded)) break;
        }
    }
    
    printf("[DEBUG] Base64 decode complete, decoded_len=%d\n", decoded_len);
    
    if (decoded_len == 0) {
        strcpy(response, "{\"error\":\"Failed to decode base64\"}");
        return;
    }
    
    printf("[API] Writing %d bytes to node %d PSRAM @ 0x%08lX\n", decoded_len, node_id, addr);
    
    // Build WRITE_MEMORY command frame
    // Max: 6-word header + 600-word payload = 606 words (1200 bytes payload)
    // WARNING: Payloads >1KB will saturate broker queue - client should fragment
    // Static to avoid stack overflow (1212 bytes is too large for stack)
    static uint16_t frame[606];
    frame[0] = OPCODE_WRITE_MEMORY;
    frame[1] = decoded_len;
    frame[2] = (uint16_t)(addr & 0xFFFF);
    frame[3] = (uint16_t)(addr >> 16);
    frame[4] = 0;
    frame[5] = 0;
    
    // Copy data
    uint8_t* data_out = (uint8_t*)&frame[6];
    for (uint16_t i = 0; i < decoded_len; i++) {
        data_out[i] = decoded[i];
    }
    
    uint16_t total_words = 6 + ((decoded_len + 1) / 2);
    
    printf("[HTTP API] Calling z1_broker_send_command(node=%d, words=%d)\n", node_id, total_words);
    if (!z1_broker_send_command(frame, total_words, node_id, STREAM_MEMORY)) {
        printf("[HTTP API] z1_broker_send_command() FAILED!\n");
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    printf("[HTTP API] z1_broker_send_command() SUCCESS - waiting for ACK...\n");
    
    // Wait for ACK
    uint32_t timeout = time_us_32() + 500000;
    z1_frame_t ack_frame;
    uint32_t rx_attempts = 0;

    printf("[HTTP WAIT] Starting ACK wait for node %d...\n", node_id);
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        if (z1_broker_try_receive(&ack_frame)) {
            rx_attempts++;
            printf("[HTTP RX] Frame received! src=%d, type=%d, payload[0]=0x%04X (attempt %lu)\n",
                   ack_frame.src, ack_frame.type, ack_frame.payload[0], rx_attempts);
            
            if (ack_frame.src == node_id && ack_frame.type == Z1_FRAME_TYPE_CTRL) {
                if (ack_frame.payload[0] == OPCODE_WRITE_ACK) {
                    printf("[HTTP ACK] Received valid ACK from node %d!\n", node_id);
                    snprintf(response, size, "{\"bytes_written\":%d}", decoded_len);
                
                    return;
                } else {
                    printf("[HTTP RX] Wrong opcode: expected 0x%04X, got 0x%04X\n",
                           OPCODE_WRITE_ACK, ack_frame.payload[0]);
                }
            } else {
                printf("[HTTP RX] Frame rejected: src=%d (expected %d), type=%d (expected %d)\n",
                       ack_frame.src, node_id, ack_frame.type, Z1_FRAME_TYPE_CTRL);
            }
        }
        z1_broker_task();
        sleep_us(100);
    }
    
    printf("[HTTP TIMEOUT] No ACK received from node %d after 500ms (%lu attempts)\n", node_id, rx_attempts);

    
    strcpy(response, "{\"error\":\"Timeout waiting for ACK\"}");
}

/**
 * POST /api/snn/input - Inject input spikes into cluster
 * 
 * Sends spikes to target nodes based on global neuron IDs.
 * Uses UNICAST frames with NO_ACK flag for maximum throughput.
 * 
 * Request body:
 *   {"spikes": [{"neuron_id": N, "value": V}, ...]}
 * 
 * Global neuron ID encoding:
 *   neuron_id = (node_id << 16) | local_neuron_id
 *   Example: 0x00010002 = Node 1, Local Neuron 2
 * 
 * Process:
 * 1. Parse JSON spike array
 * 2. Decode each neuron_id to extract target node
 * 3. Queue spike frame to broker (NO_ACK for speed)
 * 4. Pump broker for ~10ms to transmit all spikes
 * 
 * Note: "value" field is currently ignored (treated as 1.0).
 * Future: Could encode spike amplitude or timing.
 * 
 * Response: {"spikes_injected": N}
 */
void handle_snn_input(const char* body, char* response, int size) {
    printf("[HTTP API] handle_snn_input() CALLED\n");
    printf("[HTTP API] Body: %s\n", body ? body : "(null)");
    
    // Parse JSON to find spikes array
    const char* spikes_ptr = strstr(body, "\"spikes\"");
    if (!spikes_ptr) {
        printf("[HTTP API] ERROR: Missing spikes field\n");
        strcpy(response, "{\"error\":\"Missing spikes field\"}");
        return;
    }
    
    // Find opening bracket
    const char* bracket = strchr(spikes_ptr, '[');
    if (!bracket) {
        printf("[HTTP API] ERROR: Invalid spikes array\n");
        strcpy(response, "{\"error\":\"Invalid spikes array\"}");
        return;
    }
    
    printf("[HTTP API] Injecting spikes...\n");
    uint16_t spikes_injected = 0;
    
    // Parse each spike entry (simple parser for {"neuron_id": X, "value": Y})
    const char* cursor = bracket + 1;
    while (*cursor && *cursor != ']') {
        // Find neuron_id
        const char* id_ptr = strstr(cursor, "\"neuron_id\"");
        if (!id_ptr || id_ptr > strchr(cursor, ']')) break;
        
        const char* colon = strchr(id_ptr, ':');
        if (!colon) break;
        
        uint32_t neuron_id = strtoul(colon + 1, NULL, 0);
        
        // Decode global neuron ID
        uint8_t node_id = (neuron_id >> 16) & 0xFF;
        uint16_t local_id = neuron_id & 0xFFFF;
        
        // Send spike to target node (UNICAST frame, no ACK)
        uint16_t payload[2];
        payload[0] = local_id & 0xFFFF;
        payload[1] = (local_id >> 16) & 0xFFFF;
        
        if (z1_broker_send(payload, 2, node_id, STREAM_SPIKE, Z1_BROKER_NOACK)) {
            spikes_injected++;
            printf("  Queued spike to neuron %lu (node %d, local %d)\n", 
                   neuron_id, node_id, local_id);
        }
        
        // Move to next spike
        cursor = strchr(cursor, '}');
        if (!cursor) break;
        cursor++;
    }
    
    // CRITICAL: Pump broker to transmit all queued spikes
    printf("[HTTP API] Transmitting %d spikes...\n", spikes_injected);
    for (int i = 0; i < 200; i++) {  // Max 200 iterations (enough for 100 spikes)
        z1_broker_task();
        sleep_us(50);  // 50us between transmissions (faster than commands)
    }
    printf("[HTTP API] Spikes transmitted\n");
    
    snprintf(response, size, "{\"spikes_injected\":%d}", spikes_injected);
    printf("[HTTP API] Injected %d spikes\n", spikes_injected);
}

/**
 * POST /api/nodes/{id}/snn/load - Load neuron topology from PSRAM
 * 
 * Instructs node to parse neuron table from PSRAM and load into active memory.
 * Must be called AFTER writing neuron table via /api/nodes/{id}/memory.
 * 
 * Request body:
 *   {"neuron_count": N}
 * 
 * Process:
 * 1. Send LOAD_TOPOLOGY command with neuron count
 * 2. Node reads 256-byte entries from PSRAM (0x00100000 base)
 * 3. Node parses neurons and synapses into runtime structures
 * 4. Waits for ACK (100ms timeout)
 * 
 * Response: {"status": "loaded", "neuron_count": N}
 * Error: {"error": "Timeout waiting for ACK"}
 * 
 * Typical workflow:
 * 1. POST /api/nodes/0/memory (write neuron table)
 * 2. POST /api/nodes/0/snn/load (activate topology)
 * 3. POST /api/snn/start (begin execution)
 */
void handle_load_topology(uint8_t node_id, const char* body, char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    // Parse neuron_count from JSON
    const char* count_ptr = strstr(body, "\"neuron_count\"");
    if (!count_ptr) {
        strcpy(response, "{\"error\":\"Missing neuron_count field\"}");
        return;
    }
    count_ptr = strchr(count_ptr, ':');
    if (!count_ptr) {
        strcpy(response, "{\"error\":\"Invalid JSON\"}");
        return;
    }
    uint16_t neuron_count = atoi(count_ptr + 1);
    
    if (neuron_count == 0 || neuron_count > 16) {
        strcpy(response, "{\"error\":\"Invalid neuron count (1-16)\"}");
        return;
    }
    
    printf("[API] Loading %d neurons on node %d\n", neuron_count, node_id);
    
    // Track total neurons deployed globally
    g_total_neurons_deployed += neuron_count;
    printf("[API] Total neurons deployed: %d\n", g_total_neurons_deployed);
    
    uint16_t cmd[2];
    cmd[0] = OPCODE_DEPLOY_TOPOLOGY;
    cmd[1] = neuron_count;
    
    if (!z1_broker_send_command(cmd, 2, node_id, STREAM_SNN_CONFIG)) {
        strcpy(response, "{\"error\":\"Failed to send command\"}");
        return;
    }
    
    // Wait for ACK
    uint32_t timeout = time_us_32() + 200000;
    z1_frame_t ack_frame;
    uint32_t rx_attempts = 0;

    printf("[HTTP WAIT] Starting DEPLOY_ACK wait for node %d...\n", node_id);
    while (time_us_32() < timeout) {
        z1_broker_task();  // CRITICAL: Transmit queued command!
        
        if (z1_broker_try_receive(&ack_frame)) {
            rx_attempts++;
            printf("[HTTP RX] Frame received! src=%d, type=%d, payload[0]=0x%04X (attempt %lu)\n",
                   ack_frame.src, ack_frame.type, ack_frame.payload[0], rx_attempts);
            
            if (ack_frame.src == node_id && ack_frame.type == Z1_FRAME_TYPE_CTRL) {
                if (ack_frame.payload[0] == OPCODE_DEPLOY_ACK) {
                    printf("[HTTP ACK] Received valid DEPLOY_ACK from node %d!\n", node_id);
                    
                    // Track total neurons deployed across all nodes
                    g_total_neurons_deployed += neuron_count;
                    printf("[API] Total neurons deployed: %d\n", g_total_neurons_deployed);
                    
                    strcpy(response, "{\"status\":\"loaded\"}");
                
                    return;
                } else {
                    printf("[HTTP RX] Wrong opcode: expected 0x%04X, got 0x%04X\n",
                           OPCODE_DEPLOY_ACK, ack_frame.payload[0]);
                }
            } else {
                printf("[HTTP RX] Frame rejected: src=%d (expected %d), type=%d (expected %d)\n",
                       ack_frame.src, node_id, ack_frame.type, Z1_FRAME_TYPE_CTRL);
            }
        }
        z1_broker_task();
        sleep_us(100);
    }
    
    printf("[HTTP TIMEOUT] No DEPLOY_ACK received from node %d after 200ms (%lu attempts)\n", node_id, rx_attempts);

    
    strcpy(response, "{\"error\":\"Timeout waiting for ACK\"}");
}

// ============================================================================
// Main API Router
// ============================================================================

/**
 * Route HTTP request to appropriate handler
 * 
 * @param method HTTP method ("GET", "POST", etc.)
 * @param path   URL path ("/api/nodes")
 * @param body   Request body (for POST)
 * @param response Output buffer for JSON response
 * @param size   Response buffer size
 * @return HTTP status code (200, 404, etc.)
 */
int z1_http_api_route(const char* method, const char* path, const char* body,
                      char* response, int size) {
    printf("[HTTP API ROUTE] method=%s, path=%s\n", method, path);
    
    // GET /api/status
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        handle_get_status(response, size);
        return 200;
    }
    
    // GET /api/nodes
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/nodes") == 0) {
        handle_get_nodes(response, size);
        return 200;
    }
    
    // GET /api/nodes/{id}
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/nodes/", 11) == 0) {
        int node_id = atoi(path + 11);
        handle_get_node(node_id, response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/ping
    if (strcmp(method, "POST") == 0 && strstr(path, "/ping") != NULL) {
        const char* id_start = path + 11;  // After "/api/nodes/"
        int node_id = atoi(id_start);
        handle_ping_node(node_id, response, size);
        return 200;
    }
    
    // POST /api/nodes/discover
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/nodes/discover") == 0) {
        handle_discover_nodes(response, size);
        return 200;
    }
    
    // Global SNN control (MUST come BEFORE per-node routes to match correctly)
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/snn/start") == 0) {
        handle_global_snn_start(response, size);
        return 200;
    }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/snn/stop") == 0) {
        handle_global_snn_stop(response, size);
        return 200;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/snn/status") == 0) {
        handle_global_snn_status(response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/snn/start
    if (strcmp(method, "POST") == 0 && strstr(path, "/snn/start") != NULL) {
        // Extract node ID from path
        const char* id_start = path + 11;  // After "/api/nodes/"
        int node_id = atoi(id_start);
        handle_snn_start(node_id, response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/snn/stop
    if (strcmp(method, "POST") == 0 && strstr(path, "/snn/stop") != NULL) {
        const char* id_start = path + 11;
        int node_id = atoi(id_start);
        handle_snn_stop(node_id, response, size);
        return 200;
    }
    
    // POST /api/snn/reset
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/snn/reset") == 0) {
        handle_global_reset(response, size);
        return 200;
    }
    
    // POST /api/snn/input - Inject spikes
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/snn/input") == 0) {
        handle_snn_input(body, response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/memory
    if (strcmp(method, "POST") == 0 && strstr(path, "/memory") != NULL) {
        printf("[HTTP API] Matched /memory route\n");
        const char* id_start = path + 11;
        int node_id = atoi(id_start);
        printf("[HTTP API] Calling handle_write_memory(node=%d)\n", node_id);
        handle_write_memory(node_id, body, response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/snn/load
    if (strcmp(method, "POST") == 0 && strstr(path, "/snn/load") != NULL) {
        const char* id_start = path + 11;
        int node_id = atoi(id_start);
        handle_load_topology(node_id, body, response, size);
        return 200;
    }
    
    // 404 Not Found
    strcpy(response, "{\"error\":\"Not found\"}");
    return 404;
}
