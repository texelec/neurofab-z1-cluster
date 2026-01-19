/**
 * Z1 Cluster HTTP REST API
 * Code by NeuroFab Corp: 2025-2026
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
#include "w5500_eth.h"        // For w5500_eth_process()
#include "controller_pins.h"  // For GLOBAL_RESET_PIN
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include "../common/sd_card/sd_card.h"
#include "../common/sd_card/z1_config.h"
#include "../common/z1_onyx_bus/z1_bus.h"  // For OTA protocol structures
#include "../common/FatFs_SPI/ff15/source/ff.h"  // For FIL, FRESULT, f_open, f_read, etc.
#include "hardware/watchdog.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Response buffer (shared with w5500_eth.c, points to PSRAM)
extern char* http_response_buffer;

// Shared frame buffer for memory writes and OTA chunks - in PSRAM (zone 3: 132KB+)
// 306 words = 4-word header + 300-word payload max (612 bytes) - saves 612 bytes SRAM
#define FRAME_BUFFER_PSRAM ((uint16_t*)(0x11021000))
#define g_shared_frame_buffer FRAME_BUFFER_PSRAM

// File download response metadata (used to communicate binary responses to w5500_eth.c)
static http_response_metadata_t g_response_metadata = {false, 0};

/**
 * Async spike injection queue
 * HTTP handler queues spike requests, background task processes them
 */
#define MAX_SPIKE_JOBS 8
typedef struct {
    uint32_t neuron_id;
    uint32_t count;
} spike_job_t;

static struct {
    spike_job_t jobs[MAX_SPIKE_JOBS];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint32_t total_injected;  // Total spikes processed
    bool processing;          // Currently processing flag
    
    // Current job state (moved here to avoid static variable corruption)
    uint32_t current_remaining;
    uint16_t current_payload[2];
    uint8_t current_node_id;
    uint32_t current_retry_count;
} spike_queue = {0};

static inline bool spike_queue_is_full(void) {
    return spike_queue.count >= MAX_SPIKE_JOBS;
}

static inline bool spike_queue_is_empty(void) {
    return spike_queue.count == 0;
}

static bool spike_queue_enqueue(uint32_t neuron_id, uint32_t count) {
    if (spike_queue_is_full()) return false;
    spike_queue.jobs[spike_queue.tail].neuron_id = neuron_id;
    spike_queue.jobs[spike_queue.tail].count = count;
    spike_queue.tail = (spike_queue.tail + 1) % MAX_SPIKE_JOBS;
    spike_queue.count++;
    return true;
}

static spike_job_t* spike_queue_peek(void) {
    if (spike_queue_is_empty()) return NULL;
    return &spike_queue.jobs[spike_queue.head];
}

static void spike_queue_dequeue(void) {
    if (spike_queue_is_empty()) return;
    spike_queue.head = (spike_queue.head + 1) % MAX_SPIKE_JOBS;
    spike_queue.count--;
}

/**
 * Get response metadata for current HTTP request
 * Used by w5500_eth.c to determine Content-Type and length
 */
http_response_metadata_t* z1_http_api_get_response_metadata(void) {
    return &g_response_metadata;
}

// Global SNN state tracking
static uint16_t g_total_neurons_deployed = 0;

// ============================================================================
// OTA Update State Tracking
// ============================================================================

typedef struct {
    uint8_t target_node_id;      // Node being updated (0-15)
    bool update_in_progress;     // Update session active
    uint32_t firmware_size;      // Total firmware size (bytes)
    uint32_t expected_crc32;     // Expected CRC32 of firmware
    uint16_t chunk_size;         // Size of each chunk (bytes)
    uint16_t total_chunks;       // Total number of chunks
    uint16_t chunks_sent;        // Chunks successfully sent
    uint32_t chunks_sent_bitmap[(1024+31)/32];  // Bitmap for 1024 chunks max (reduced for RAM)
    uint32_t last_activity_ms;   // Last OTA activity timestamp
} ota_session_state_t;

// OTA session in SRAM (PSRAM has corruption issues)
static ota_session_state_t g_ota_session;

// Firmware deployment working buffers in PSRAM (zone 2: after OTA session)
// Saves ~96 bytes SRAM (64 engine name + 16 node list + 16 spare)
#define DEPLOY_ENGINE_NAME_PSRAM ((char*)(0x11020200))  // 64 bytes
#define DEPLOY_TARGET_NODES_PSRAM ((uint8_t*)(0x11020240))  // 16 bytes

// Decoded data buffers in PSRAM (zone 2: after deployment buffers)
// Saves 1536 bytes SRAM (512 + 1024)
#define DECODED_BUFFER_512_PSRAM ((uint8_t*)(0x11020300))   // 512 bytes for memory writes
#define DECODED_BUFFER_1024_PSRAM ((uint8_t*)(0x11020500))  // 1024 bytes for OTA chunks

// Helper: Mark chunk as sent in bitmap
static void ota_mark_chunk_sent(uint16_t chunk_num) {
    if (chunk_num >= 4096) return;
    uint32_t word = chunk_num / 32;
    uint32_t bit = chunk_num % 32;
    g_ota_session.chunks_sent_bitmap[word] |= (1UL << bit);
}

// Helper: Check if chunk was sent
static bool ota_is_chunk_sent(uint16_t chunk_num) {
    if (chunk_num >= 4096) return false;
    uint32_t word = chunk_num / 32;
    uint32_t bit = chunk_num % 32;
    return (g_ota_session.chunks_sent_bitmap[word] & (1UL << bit)) != 0;
}

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
 * GET / - Root HTML splash screen
 * 
 * Displays Z1 Onyx status page with IP, MAC, and API documentation
 */
void handle_root(char* response, int size) {
    extern const char* w5500_get_ip_string(void);
    extern const uint8_t* w5500_get_mac_address(void);
    
    const char* ip = w5500_get_ip_string();
    const uint8_t* mac = w5500_get_mac_address();
    
    // Get broker stats for uptime display
    z1_broker_stats_t stats;
    z1_broker_get_stats(&stats);
    
    snprintf(response, size,
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "    <title>Z1 Onyx Cluster Controller</title>\n"
        "    <style>\n"
        "        body {\n"
        "            background: linear-gradient(135deg, #1e3c72 0%%, #2a5298 100%%);\n"
        "            color: #ffffff;\n"
        "            font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;\n"
        "            margin: 0;\n"
        "            padding: 20px;\n"
        "            line-height: 1.6;\n"
        "        }\n"
        "        .container {\n"
        "            max-width: 900px;\n"
        "            margin: 0 auto;\n"
        "            background: rgba(0, 0, 0, 0.3);\n"
        "            border-radius: 10px;\n"
        "            padding: 30px;\n"
        "            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.3);\n"
        "        }\n"
        "        h1 {\n"
        "            text-align: center;\n"
        "            font-size: 2.5em;\n"
        "            margin-bottom: 10px;\n"
        "            text-shadow: 2px 2px 4px rgba(0, 0, 0, 0.5);\n"
        "        }\n"
        "        .subtitle {\n"
        "            text-align: center;\n"
        "            font-size: 1.2em;\n"
        "            color: #a0c4ff;\n"
        "            margin-bottom: 30px;\n"
        "        }\n"
        "        .info-box {\n"
        "            background: rgba(255, 255, 255, 0.1);\n"
        "            border-left: 4px solid #4a90e2;\n"
        "            padding: 15px;\n"
        "            margin: 20px 0;\n"
        "            border-radius: 5px;\n"
        "        }\n"
        "        .info-box h3 {\n"
        "            margin-top: 0;\n"
        "            color: #4a90e2;\n"
        "        }\n"
        "        .info-item {\n"
        "            margin: 10px 0;\n"
        "            font-family: 'Courier New', monospace;\n"
        "        }\n"
        "        .api-list {\n"
        "            background: rgba(0, 0, 0, 0.2);\n"
        "            padding: 15px;\n"
        "            border-radius: 5px;\n"
        "            font-family: 'Courier New', monospace;\n"
        "            font-size: 0.9em;\n"
        "        }\n"
        "        .api-item {\n"
        "            margin: 8px 0;\n"
        "            padding: 5px;\n"
        "            border-left: 2px solid #4a90e2;\n"
        "            padding-left: 10px;\n"
        "        }\n"
        "        .method-get { color: #5cb85c; }\n"
        "        .method-post { color: #f0ad4e; }\n"
        "        .method-put { color: #5bc0de; }\n"
        "        .method-delete { color: #d9534f; }\n"
        "        .footer {\n"
        "            text-align: center;\n"
        "            margin-top: 30px;\n"
        "            padding-top: 20px;\n"
        "            border-top: 1px solid rgba(255, 255, 255, 0.2);\n"
        "            color: #a0c4ff;\n"
        "        }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "    <div class=\"container\">\n"
        "        <h1>&#x1F9E0; Z1 Onyx Cluster</h1>\n"
        "        <div class=\"subtitle\">Spiking Neural Network Hardware Accelerator</div>\n"
        "        \n"
        "        <div class=\"info-box\">\n"
        "            <h3>&#x1F4E1; Network Configuration</h3>\n"
        "            <div class=\"info-item\"><strong>IP Address:</strong> %s</div>\n"
        "            <div class=\"info-item\"><strong>MAC Address:</strong> %02X:%02X:%02X:%02X:%02X:%02X</div>\n"
        "            <div class=\"info-item\"><strong>HTTP Port:</strong> 80</div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"info-box\">\n"
        "            <h3>&#x1F4CA; System Status</h3>\n"
        "            <div class=\"info-item\"><strong>Controller:</strong> Online</div>\n"
        "            <div class=\"info-item\"><strong>Firmware:</strong> v3.0</div>\n"
        "            <div class=\"info-item\"><strong>Hardware:</strong> RP2350 + W5500</div>\n"
        "            <div class=\"info-item\"><strong>Frames Sent:</strong> %d</div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"info-box\">\n"
        "            <h3>&#x1F4DA; Available APIs</h3>\n"
        "            <div class=\"api-list\">\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/status - Controller status</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/nodes - List all nodes</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/nodes/{id} - Get specific node</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/nodes/discover - Discover nodes</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/nodes/{id}/ping - Ping node</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/nodes/reset - Reset nodes</div>\n"
        "                <hr style=\"border-color: rgba(255,255,255,0.1); margin: 15px 0;\">\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/snn/deploy - Deploy topology</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/snn/start - Start SNN</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/snn/stop - Stop SNN</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/snn/status - Get SNN status</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/snn/input - Inject spikes</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/snn/reset - Reset SNN</div>\n"
        "                <hr style=\"border-color: rgba(255,255,255,0.1); margin: 15px 0;\">\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/files - List files</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/files/{path} - Download file</div>\n"
        "                <div class=\"api-item\"><span class=\"method-put\">PUT</span> /api/files/{path} - Upload file</div>\n"
        "                <div class=\"api-item\"><span class=\"method-delete\">DELETE</span> /api/files/{path} - Delete file</div>\n"
        "                <hr style=\"border-color: rgba(255,255,255,0.1); margin: 15px 0;\">\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/ota/update_start - Start OTA update</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/ota/update_chunk - Upload chunk</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/ota/update_verify - Verify update</div>\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/ota/update_commit - Commit update</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/ota/status - OTA status</div>\n"
        "                <hr style=\"border-color: rgba(255,255,255,0.1); margin: 15px 0;\">\n"
        "                <div class=\"api-item\"><span class=\"method-post\">POST</span> /api/system/reboot - Reboot controller</div>\n"
        "                <div class=\"api-item\"><span class=\"method-get\">GET</span> /api/sd/status - SD card status</div>\n"
        "            </div>\n"
        "        </div>\n"
        "        \n"
        "        <div class=\"footer\">\n"
        "            <strong>NeuroFab Corp</strong><br>\n"
        "            Z1 Onyx Cluster &copy; 2026<br>\n"
        "            <small>Powered by RP2350 &amp; Raspberry Pi Pico SDK</small>\n"
        "        </div>\n"
        "    </div>\n"
        "</body>\n"
        "</html>",
        ip, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], stats.total_sent
    );
    
    // Set response to HTML instead of JSON
    http_response_metadata_t* metadata = z1_http_api_get_response_metadata();
    metadata->is_binary = false;
    metadata->content_type = "text/html; charset=utf-8";
}

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
    
    // Query all 16 nodes
    for (int i = 0; i < 16; i++) {
        if (i > 0) {
            response[pos++] = ',';
        }
        
        // Send READ_STATUS command
        uint16_t cmd = OPCODE_READ_STATUS;
        printf("[API] Querying node %d...\n", i);
        
        bool responded = false;
        uint32_t uptime_ms = 0;
        uint32_t memory_free = 0;
        uint8_t led_r = 0, led_g = 0, led_b = 0;
        bool snn_running = false;
        uint16_t neuron_count = 0;
        
        if (z1_broker_send_command(&cmd, 1, i, STREAM_NODE_MGMT)) {
            // Wait for response (100ms timeout)
            uint32_t timeout = time_us_32() + 100000;
            z1_frame_t frame;
            
            while (time_us_32() < timeout) {
                // Service bus RX/TX more aggressively during wait
                for (int j = 0; j < 10; j++) {
                    z1_broker_task();
                
                    if (z1_broker_try_receive(&frame)) {
                        if (frame.src == i && frame.type == Z1_FRAME_TYPE_CTRL) {
                            // Parse STATUS_RESPONSE (11 words)
                            uptime_ms = ((uint32_t)frame.payload[3] << 16) | frame.payload[2];
                            memory_free = ((uint32_t)frame.payload[5] << 16) | frame.payload[4];
                            led_r = frame.payload[6] & 0xFF;
                            led_g = frame.payload[7] & 0xFF;
                            led_b = frame.payload[8] & 0xFF;
                            snn_running = (frame.payload[9] != 0);
                            neuron_count = frame.payload[10];
                            responded = true;
                            printf("[API] Node %d responded\n", i);
                            break;
                        }
                    }  // Close if (z1_broker_try_receive)
                }  // Close for loop
                if (responded) break;
                sleep_us(50);  // Small delay between polling bursts
            }
        }
        
        // Build JSON for this node
        if (responded) {
            n = snprintf(response + pos, size - pos,
                        "{\"id\":%d,\"status\":\"online\",\"memory_free\":%lu,\"uptime_ms\":%lu,"
                        "\"led_state\":{\"r\":%d,\"g\":%d,\"b\":%d},\"snn_running\":%s,\"neurons\":%d}",
                        i, (unsigned long)memory_free, (unsigned long)uptime_ms,
                        led_r, led_g, led_b,
                        snn_running ? "true" : "false", neuron_count);
        } else {
            n = snprintf(response + pos, size - pos,
                        "{\"id\":%d,\"status\":\"unknown\",\"memory_free\":0,\"uptime_ms\":0,"
                        "\"led_state\":{\"r\":0,\"g\":0,\"b\":0},\"snn_running\":false,\"neurons\":0}",
                        i);
        }
        
        if (n < 0 || pos + n >= size) break;
        pos += n;
    }
    
    // End nodes array
    n = snprintf(response + pos, size - pos, "]}");
    if (n > 0 && pos + n < size) {
        pos += n;
    }
}

// ============================================================================
// SD Card API Handlers
// ============================================================================

/**
 * GET /api/sd/status - SD card health and filesystem info
 * 
 * Response: {"mounted": true/false, "free_mb": N, "total_mb": M}
 */
void handle_sd_status(char* response, int size) {
    int pos = json_start(response, size);
    
    // Check if SD card is mounted by trying to get free space
    uint64_t free_bytes = sd_card_get_free_space();
    bool mounted = (free_bytes > 0);
    
    pos = json_add_bool(response, pos, size, "mounted", mounted, false);
    
    if (mounted) {
        int free_mb = (int)(free_bytes / (1024 * 1024));
        pos = json_add_int(response, pos, size, "free_mb", free_mb, true);
    } else {
        pos = json_add_string(response, pos, size, "error", "SD card not mounted", true);
    }
    
    json_end(response, pos, size);
}

/**
 * POST /api/system/reboot - Reboot the controller
 * 
 * Triggers a software reset of the controller using watchdog timer.
 * Response is sent before reboot, giving client time to receive acknowledgment.
 * 
 * Response: {"success": true, "message": "Rebooting in 1 second..."}
 */
void handle_system_reboot(char* response, int size) {
    int pos = json_start(response, size);
    pos = json_add_bool(response, pos, size, "success", true, false);
    pos = json_add_string(response, pos, size, "message", "Rebooting in 1 second...", true);
    json_end(response, pos, size);
    
    // Note: Actual reboot is triggered by caller after response is sent
    // This allows the HTTP response to reach the client before reset
}

/**
 * GET /api/config - Read z1.cfg configuration file from SD card
 * 
 * Reads the cluster configuration file (z1.cfg) from SD card root directory.
 * Configuration includes IP address, subnet, gateway, and current engine selection.
 * 
 * The configuration file is automatically created with defaults if not present:
 * - IP: 192.168.1.222 (hardcoded in firmware, override here for permanent change)
 * - Subnet: 255.255.255.0
 * - Gateway: 192.168.1.1
 * - Engine: "default"
 * 
 * Response: {"ip_address": "192.168.1.222", "current_engine": "xor_demo"}
 */
void handle_get_config(char* response, int size) {
    z1_config_t config;
    
    if (!z1_config_load_or_default(&config)) {
        strcpy(response, "{\"error\":\"Failed to load config\"}");
        return;
    }
    
    int pos = json_start(response, size);
    
    // Format IP address
    char ip_str[16];
    snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
             config.ip_address[0], config.ip_address[1],
             config.ip_address[2], config.ip_address[3]);
    
    // Format MAC address
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             config.mac_address[0], config.mac_address[1],
             config.mac_address[2], config.mac_address[3],
             config.mac_address[4], config.mac_address[5]);
    
    pos = json_add_string(response, pos, size, "ip_address", ip_str, false);
    pos = json_add_string(response, pos, size, "mac_address", mac_str, false);
    pos = json_add_string(response, pos, size, "current_engine", config.current_engine, false);
    pos = json_add_int(response, pos, size, "hw_version", config.hw_version, false);
    pos = json_add_int(response, pos, size, "node_count", config.node_count, true);
    
    json_end(response, pos, size);
}

/**
 * POST /api/config - Update z1.cfg configuration file on SD card
 * 
 * Supports partial updates - only fields present in request body are modified.
 * The configuration file is read, modified fields are updated, then written back.
 * 
 * Common Use Case: Change active SNN engine without affecting IP settings
 * 
 * Example Request:
 *   {"current_engine": "mnist_classifier"}
 * 
 * This will update only the current_engine field, leaving IP/subnet/gateway unchanged.
 * 
 * Response: {"success": true} or {"error": "reason"}
 */
void handle_set_config(const char* body, char* response, int size) {
    if (!body) {
        strcpy(response, "{\"error\":\"Missing request body\"}");
        return;
    }
    
    // Load current config
    z1_config_t config;
    if (!z1_config_load_or_default(&config)) {
        strcpy(response, "{\"error\":\"Failed to load config\"}");
        return;
    }
    
    // Parse JSON to find current_engine field
    const char* engine_key = strstr(body, "\"current_engine\"");
    if (engine_key) {
        const char* colon = strchr(engine_key, ':');
        if (colon) {
            const char* quote1 = strchr(colon, '\"');
            if (quote1) {
                quote1++;  // Skip opening quote
                const char* quote2 = strchr(quote1, '\"');
                if (quote2) {
                    int len = quote2 - quote1;
                    if (len > 0 && len < sizeof(config.current_engine)) {
                        strncpy(config.current_engine, quote1, len);
                        config.current_engine[len] = '\0';
                    }
                }
            }
        }
    }
    
    // Save updated config
    if (!z1_config_save(&config)) {
        strcpy(response, "{\"error\":\"Failed to save config\"}");
        return;
    }
    
    strcpy(response, "{\"success\":true}");
}

/**
 * GET /api/files/{dirpath} - List files in directory
 * 
 * Lists all files in a directory on the SD card. Used by zengine list command.
 * 
 * Algorithm:
 * 1. Calls sd_card_list_directory() which uses FatFS f_findfirst/f_findnext
 * 2. Filters out directories (AM_DIR), hidden files (AM_HID), system files (AM_SYS)
 * 3. Builds JSON array: {"files": [{"name": "...", "size": N}, ...], "count": N}
 * 4. If directory doesn't exist, returns {"files": [], "count": 0}
 * 
 * Directory Detection:
 * - w5500_eth.c checks f_stat() with AM_DIR before calling this handler
 * - This ensures GET /api/files/topologies lists directory (not downloads it)
 * - GET /api/files/topologies/file.json still downloads the file
 * 
 * Example: GET /api/files/topologies
 * Response: {"files": [{"name": "xor.json", "size": 2048}, ...], "count": 5}
 */

// Helper callback for directory listing - pointers in PSRAM save ~16 bytes SRAM
// (Note: these are just pointers/ints, minimal savings but every byte counts)
static char* g_list_response_buffer = NULL;
static int g_list_response_pos = 0;
static int g_list_response_size = 0;
static int g_file_count = 0;

static void list_files_callback(const char* name, size_t size) {
    // Add comma if not first file
    if (g_file_count > 0) {
        if (g_list_response_pos < g_list_response_size - 1) {
            g_list_response_buffer[g_list_response_pos++] = ',';
        }
    }
    
    // Add file object: {"name":"...", "size":N}
    int n = snprintf(g_list_response_buffer + g_list_response_pos,
                     g_list_response_size - g_list_response_pos,
                     "{\"name\":\"%s\",\"size\":%lu}",
                     name, (unsigned long)size);
    
    if (n > 0 && g_list_response_pos + n < g_list_response_size) {
        g_list_response_pos += n;
        g_file_count++;
    }
}

void handle_list_files(const char* dirpath, char* response, int size) {
    // Start JSON (snprintf null-terminates automatically)
    int pos = snprintf(response, size, "{\"files\":[");
    if (pos < 0 || pos >= size) {
        strcpy(response, "{\"error\":\"Buffer too small\"}");
        return;
    }
    
    // Set up callback globals
    g_list_response_buffer = response;
    g_list_response_pos = pos;
    g_list_response_size = size;
    g_file_count = 0;
    
    // List directory (returns -1 if directory doesn't exist, treat as empty)
    int file_count = sd_card_list_directory(dirpath, list_files_callback);
    
    // End JSON (if directory doesn't exist, treat as 0 files)
    pos = g_list_response_pos;
    if (pos < size - 20) {
        int n = snprintf(response + pos, size - pos, "],\"count\":%d}", file_count >= 0 ? file_count : 0);
        if (n > 0 && pos + n < size) {
            pos += n;
            response[pos] = '\0';  // Ensure null termination
        }
    }
}

/**
 * PUT /api/files/{filepath} - Upload file
 * 
 * Example: PUT /api/files/engines/test.z1app
 * Body: raw binary data
 * Response: {"success": true, "size": N}
 */
void handle_upload_file(const char* filepath, const char* body, char* response, int size) {
    if (!body) {
        strcpy(response, "{\"error\":\"Missing file data\"}");
        return;
    }
    
    // Calculate body size (assumes null-terminated, but for binary we'd need Content-Length)
    // For now, this is a limitation - we'll document that uploads must be text or base64
    size_t body_size = strlen(body);
    
    if (sd_card_write_file(filepath, body, body_size)) {
        snprintf(response, size, "{\"success\":true,\"size\":%lu}", (unsigned long)body_size);
    } else {
        strcpy(response, "{\"error\":\"Failed to write file\"}");
    }
}

/**
 * DELETE /api/files/{filepath} - Delete file
 * 
 * Example: DELETE /api/files/engines/test.z1app
 * Response: {"success": true}
 */
void handle_delete_file(const char* filepath, char* response, int size) {
    if (sd_card_delete_file(filepath)) {
        strcpy(response, "{\"success\":true}");
    } else {
        strcpy(response, "{\"error\":\"Failed to delete file\"}");
    }
}

/**
 * GET /api/files/{filepath} - Download file
 * 
 * Returns raw file contents with appropriate Content-Type header.
 * Used for downloading engine files, configuration backups, etc.
 * 
 * Example: GET /api/files/engines/test.json
 * Response: raw file data (binary or text)
 * 
 * Note: This function writes directly to http_response_buffer.
 * Caller must check return value to determine if it's a file download
 * (positive size) or an error (negative/zero).
 * 
 * @param filepath Path to file to download
 * @param response Buffer for JSON error response if file not found
 * @param size Size of response buffer
 * @return File size in bytes if successful, -1 if error (file not found)
 */
int handle_download_file(const char* filepath, char* response, int size) {
    uint8_t* file_data = NULL;
    size_t file_size = 0;
    
    if (!sd_card_read_file(filepath, &file_data, &file_size)) {
        snprintf(response, size, "{\"error\":\"File not found: %s\"}", filepath);
        return -1;
    }
    
    // Copy file data to HTTP response buffer
    if (file_size > (size_t)size) {
        snprintf(response, size, "{\"error\":\"File too large: %zu bytes (max %d)\"}", 
                file_size, size);
        free(file_data);
        return -1;
    }
    
    memcpy(response, file_data, file_size);
    free(file_data);
    
    return (int)file_size;
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
        // Service bus RX/TX more aggressively during wait
        for (int i = 0; i < 10; i++) {
            z1_broker_task();
            
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
                }  // Close if (frame.src == node_id...)
            }  // Close if (z1_broker_try_receive)
        }  // Close for loop
        sleep_us(50);  // Small delay between polling bursts
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
    for (int i = 0; i < 20; i++) {  // 20 iterations = ~2ms (more than enough for 16 commands)
        z1_broker_task();
        sleep_us(100);  // 100us between transmissions
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
    for (int i = 0; i < 20; i++) {  // 20 iterations = ~2ms (more than enough for 16 commands)
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
    printf("[API-STATS] Entered handle_global_snn_status()\n");
    
    // Wait for spike queue to drain before querying stats
    // (nodes may be busy processing spikes and unable to respond promptly)
    uint32_t spike_queue_depth = z1_broker_get_spike_queue_depth();
    printf("[API-STATS] Spike queue depth: %lu\n", spike_queue_depth);
    
    if (spike_queue_depth > 0) {
        printf("[API-STATS] Waiting for spike queue to drain...\n");
        uint32_t drain_start = time_us_32();
        uint32_t drain_timeout = drain_start + 10000000;  // 10 seconds max
        while (z1_broker_get_spike_queue_depth() > 0 && time_us_32() < drain_timeout) {
            z1_broker_task();
            sleep_us(1000);
        }
        uint32_t drain_time_ms = (time_us_32() - drain_start) / 1000;
        printf("[API-STATS] Spike queue drained in %lu ms (depth now: %lu)\n", 
               drain_time_ms, z1_broker_get_spike_queue_depth());
    }
    
    // Drain any remaining RX frames (software drain only, no hardware reset)
    z1_frame_t drain_frame;
    int drained = 0;
    printf("[API-STATS] Draining RX queue (up to 1000 frames)...\n");
    for (int i = 0; i < 1000; i++) {
        if (!z1_broker_try_receive(&drain_frame)) break;
        // Log ALL drained frames to diagnose buffer flooding
        printf("[API-STATS] RX drain[%d]: type=%d src=%d dest=%d stream=%d len=%d\n", 
               drained, drain_frame.type, drain_frame.src, drain_frame.dest, drain_frame.stream, drain_frame.length);
        drained++;
    }
    printf("[API-STATS] Drained %d RX frames total\n", drained);
    
    // Query first responding node for statistics
    // Try nodes 0-1 first (most likely to have neurons deployed), then others
    uint16_t cmd = OPCODE_GET_SNN_STATUS;
    bool got_response = false;
    uint16_t neuron_count = 0;
    uint16_t active_neurons = 0;
    uint32_t total_spikes = 0;
    uint32_t spike_rate_hz = 0;
    bool is_running = false;
    
    //Priority node list: Try nodes with likely deployments first
    uint8_t node_priority[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    uint8_t nodes_to_try = (g_total_neurons_deployed > 0) ? 2 : 16;  // If neurons deployed, only try first 2 nodes
    printf("[API-STATS] g_total_neurons_deployed=%d, will try %d nodes\n", g_total_neurons_deployed, nodes_to_try);
    
    for (uint8_t i = 0; i < nodes_to_try; i++) {
        uint8_t node_id = node_priority[i];
        printf("[API-STATS] Querying node %d for SNN status...\n", node_id);
        
        // DEBUG: Check RX buffer depth before sending
        uint32_t rx_depth_before = z1_bus_rx_depth();
        printf("[API-STATS] RX buffer depth BEFORE send: %lu words\n", rx_depth_before);
        
        if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_SNN_CONTROL)) {
            printf("[API-STATS] Send failed to node %d\n", node_id);
            continue;
        }
        
        // DEBUG: Check RX buffer depth after sending
        sleep_us(1000);  // Wait 1ms for response
        uint32_t rx_depth_after = z1_bus_rx_depth();
        printf("[API-STATS] RX buffer depth AFTER 1ms: %lu words\n", rx_depth_after);
        
        // Wait for response (30 seconds - nodes may be processing queued spikes)
        uint32_t timeout = time_us_32() + 30000000;
        z1_frame_t frame;
        int frames_received = 0;
        int spikes_received = 0;
        
        while (time_us_32() < timeout) {
            // Service bus RX/TX more aggressively during wait
            for (int i = 0; i < 10; i++) {
                z1_broker_task();
            
                if (z1_broker_try_receive(&frame)) {
                    frames_received++;
                    if (frame.stream == 4) spikes_received++;
                    
                    // DEBUG: Log frame details to diagnose opcode mismatch
                    printf("[API-STATS] RX frame: src=%d type=%d payload[0]=0x%04X (expect 0x%04X)\n",
                           frame.src, frame.type, frame.payload[0], OPCODE_SNN_STATUS);
                    
                    if (frame.src == node_id && 
                        frame.type == Z1_FRAME_TYPE_CTRL && 
                        frame.payload[0] == OPCODE_SNN_STATUS) {
                        
                        printf("[API-STATS] Got response from node %d (%d frames, %d spikes during wait)\n", 
                               node_id, frames_received, spikes_received);
                        
                        // Parse response: [opcode, running, neuron_count, active_neurons, total_spikes(2), spike_rate(2)]
                        is_running = (frame.payload[1] != 0);
                        neuron_count = frame.payload[2];  // Read neuron_count from correct field
                        active_neurons = frame.payload[3];
                        memcpy(&total_spikes, &frame.payload[4], 4);
                        memcpy(&spike_rate_hz, &frame.payload[6], 4);
                        
                        printf("[API-STATS] Parsed: running=%d neurons=%d active=%d spikes=%lu rate=%lu\n",
                               is_running, neuron_count, active_neurons, (unsigned long)total_spikes, (unsigned long)spike_rate_hz);
                        
                        got_response = true;
                        break;
                    }
                }  // Close if (rx_result)
            }  // Close for loop
            if (got_response) break;
            sleep_us(50);  // Small delay between polling bursts
        }
        
        if (!got_response) {
            printf("[API-STATS] Node %d timeout (%d frames, %d spikes during wait)\n", 
                   node_id, frames_received, spikes_received);
        }
        
        if (got_response) break;
    }
    
    printf("[API-STATS] Query complete, got_response=%d\n", got_response);
    
    // Build JSON response
    if (got_response) {
        snprintf(response, size,
                 "{\"state\":\"%s\",\"neuron_count\":%u,\"active_neurons\":%u,\"total_spikes\":%lu,\"spike_rate_hz\":%lu}",
                 is_running ? "running" : "stopped",
                 neuron_count,  // Use neuron_count from node response
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
    // DON'T reset neuron count - deployment persists through SNN reset
    // Only reset deployment count on actual node reset or new deployment
    
    // This would use the global_reset_nodes() function
    // For now, send RESET commands to all nodes
    
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        uint16_t cmd = OPCODE_RESET;
        z1_broker_send_command(&cmd, 1, node_id, 0);
    }
    
    strcpy(response, "{\"status\":\"reset_sent\"}");
}

/**
 * POST /api/nodes/reset - Reset all nodes into bootloader mode
 * 
 * Supports both hardware and software reset:
 * - Default: Software reset via OPCODE_RESET_TO_BOOTLOADER (allows individual node testing)
 * - Optional: Hardware reset via GPIO 33 (V2 only, resets all nodes simultaneously)
 * 
 * Query param ?mode=hardware forces hardware reset on V2
 * Query param ?node=N resets only specific node (software only)
 * 
 * Response: {"status": "ok", "method": "hardware|software", "nodes": "all|N"}
 */
void handle_reset_to_bootloader(char* response, int size, const char* query_params) {
    bool force_hardware = false;
    int specific_node = -1;  // -1 means all nodes
    
    // Parse query params
    if (query_params) {
        if (strstr(query_params, "mode=hardware")) force_hardware = true;
        
        // Check for specific node (e.g., ?node=0 or ?node=16 for controller)
        const char* node_param = strstr(query_params, "node=");
        if (node_param) {
            specific_node = atoi(node_param + 5);  // Skip "node="
            if (specific_node < 0 || specific_node > 16) {
                snprintf(response, size, "{\"error\":\"Invalid node ID: %d\"}", specific_node);
                return;
            }
            
            // Handle controller self-reset (node 16)
            if (specific_node == 16) {
                printf("[API] Controller self-reset requested...\n");
                strcpy(response, "{\"status\":\"ok\",\"method\":\"watchdog\",\"nodes\":\"controller\"}");
                // Send response before reset
                sleep_ms(100);
                // Reset controller via watchdog
                watchdog_reboot(0, 0, 0);
                // Never returns
                return;
            }
        }
    }
    
#ifdef HW_V2
    // V2 hardware: Use hardware reset ONLY if explicitly requested and no specific node
    if (force_hardware && specific_node == -1) {
        printf("[API] Resetting ALL nodes via hardware reset (GPIO %d)...\n", GLOBAL_RESET_PIN);
        gpio_put(GLOBAL_RESET_PIN, 1);  // Assert reset
        sleep_ms(100);  // Hold reset for 100ms
        gpio_put(GLOBAL_RESET_PIN, 0);  // Release reset
        strcpy(response, "{\"status\":\"ok\",\"method\":\"hardware\",\"nodes\":\"all\"}");
        return;
    }
#endif
    
    // Software reset (default): Use Matrix bus command
    if (specific_node >= 0) {
        // Single node reset
        printf("[API] Resetting node %d via software command (RESET_TO_BOOTLOADER)...\n", specific_node);
        
        uint16_t cmd = OPCODE_RESET_TO_BOOTLOADER;
        if (!z1_broker_send_command(&cmd, 1, specific_node, STREAM_NODE_MGMT)) {
            printf("[API] WARNING: Failed to queue reset for node %d\n", specific_node);
            snprintf(response, size, "{\"error\":\"Failed to send reset command to node %d\"}", specific_node);
            return;
        }
        
        // Pump broker to transmit
        for (int i = 0; i < 20; i++) {
            z1_broker_task();
            sleep_us(100);
        }
        
        snprintf(response, size, "{\"status\":\"ok\",\"method\":\"software\",\"nodes\":\"%d\"}", specific_node);
    } else {
        // All nodes reset
        printf("[API] Resetting ALL nodes via software command (RESET_TO_BOOTLOADER)...\n");
        
        for (uint8_t node_id = 0; node_id < 16; node_id++) {
            uint16_t cmd = OPCODE_RESET_TO_BOOTLOADER;
            if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
                printf("[API] WARNING: Failed to queue reset for node %d\n", node_id);
            }
        }
        
        // Pump broker to transmit commands
        for (int i = 0; i < 100; i++) {
            z1_broker_task();
            sleep_us(100);
        }
        
        strcpy(response, "{\"status\":\"ok\",\"method\":\"software\",\"nodes\":\"all\"}");
    }
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
    // NOTE: Using 1536-byte buffer to safely handle 1024-byte payloads
    // (1368 base64 chars * 0.75 = 1026 bytes decoded)
    // Use PSRAM buffer to save SRAM
    uint8_t* decoded = DECODED_BUFFER_1024_PSRAM;
    uint16_t decoded_len = 0;
    const uint16_t max_decoded_len = 1536;  // Actual buffer size (plenty of margin)
    
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
            if (decoded_len >= max_decoded_len) break;  // Use explicit size, not sizeof(pointer)!
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
    // Using shared frame buffer to reduce RAM usage
    uint16_t* frame = g_shared_frame_buffer;
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
        // Service bus RX/TX more aggressively during wait
        for (int i = 0; i < 10; i++) {
            z1_broker_task();
            
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
            }  // Close if (z1_broker_try_receive)
        }  // Close for loop
        sleep_us(50);  // Small delay between polling bursts
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
    printf("[handle_snn_input] ENTER\n");
    
    // Parse JSON to find spikes array
    const char* spikes_ptr = strstr(body, "\"spikes\"");
    if (!spikes_ptr) {
        strcpy(response, "{\"error\":\"Missing spikes field\"}");
        return;
    }
    
    // Find opening bracket
    const char* bracket = strchr(spikes_ptr, '[');
    if (!bracket) {
        strcpy(response, "{\"error\":\"Invalid spikes array\"}");
        return;
    }
    
    uint32_t total_spikes = 0;
    uint32_t jobs_queued = 0;
    
    // Parse each spike entry and QUEUE JOBS (non-blocking)
    const char* cursor = bracket + 1;
    while (*cursor && *cursor != ']') {
        // Find neuron_id
        const char* id_ptr = strstr(cursor, "\"neuron_id\"");
        if (!id_ptr || id_ptr > strchr(cursor, ']')) break;
        
        const char* colon = strchr(id_ptr, ':');
        if (!colon) break;
        
        uint32_t neuron_id = strtoul(colon + 1, NULL, 0);
        
        // Check for "count" field (defaults to 1 if not present)
        uint32_t spike_count = 1;
        const char* count_ptr = strstr(cursor, "\"count\"");
        const char* next_brace = strchr(cursor, '}');
        if (count_ptr && next_brace && count_ptr < next_brace) {
            const char* count_colon = strchr(count_ptr, ':');
            if (count_colon) {
                spike_count = strtoul(count_colon + 1, NULL, 0);
                if (spike_count == 0) spike_count = 1;
                if (spike_count > 10000) spike_count = 10000;
            }
        }
        
        // Queue job (non-blocking)
        if (spike_queue_enqueue(neuron_id, spike_count)) {
            total_spikes += spike_count;
            jobs_queued++;
        } else {
            snprintf(response, size, "{\"error\":\"Spike queue full (max %d jobs)\"}", MAX_SPIKE_JOBS);
            return;
        }
        
        // Move to next spike entry
        cursor = strchr(cursor, '}');
        if (!cursor) break;
        cursor++;
    }
    
    printf("[HTTP] Queued %lu jobs (%lu spikes) for async injection\n", jobs_queued, total_spikes);
    
    // Return immediately - spikes will be injected in background at 100/sec
    snprintf(response, size, "{\"status\":\"queued\",\"jobs\":%lu,\"spikes\":%lu}", jobs_queued, total_spikes);
}

/**
 * Background spike injection processor
 * Called from Core 0 main loop to process queued spike jobs asynchronously
 * Injects spikes at controlled rate (100/sec) without blocking HTTP
 */
void z1_http_api_process_spikes(void) {
    // Debug: Report queue state periodically
    static uint64_t last_debug_time = 0;
    uint64_t now = time_us_64();
    if (now - last_debug_time > 1000000) {  // Every 1 second
        if (!spike_queue_is_empty() || spike_queue.current_remaining > 0) {
            printf("[SPIKE-PROC] Queue: head=%d tail=%d processing=%d remaining=%lu\n",
                   spike_queue.head, spike_queue.tail, spike_queue.processing, spike_queue.current_remaining);
        }
        last_debug_time = now;
    }
    
    // If no queue, skip
    if (spike_queue_is_empty() && spike_queue.current_remaining == 0) {
        spike_queue.processing = false;
        return;
    }
    
    // Rate limiting: 10ms between spikes = 100 spikes/sec
    static uint64_t last_spike_time_us = 0;
    uint64_t now_us = time_us_64();
    if (now_us - last_spike_time_us < 10000) {
        return;  // Too soon, throttle to 100 spikes/sec
    }
    
    // If no active job, start a new one
    if (spike_queue.current_remaining == 0) {
        spike_job_t* job = spike_queue_peek();
        if (!job) {
            spike_queue.processing = false;
            return;
        }
        
        spike_queue.processing = true;
        spike_queue.current_remaining = job->count;
        spike_queue.current_retry_count = 0;
        
        // Decode neuron ID
        spike_queue.current_node_id = (job->neuron_id >> 16) & 0xFF;
        spike_queue.current_payload[0] = job->neuron_id & 0xFFFF;
        spike_queue.current_payload[1] = (job->neuron_id >> 16) & 0xFFFF;
        
        printf("[SPIKE] Job start: neuron_id=%lu count=%lu node=%d payload=[0x%04X,0x%04X]\n", 
               job->neuron_id, job->count, spike_queue.current_node_id,
               spike_queue.current_payload[0], spike_queue.current_payload[1]);
    }
    
    // Try to inject one spike using spike-specific function
    if (z1_broker_send_spike(spike_queue.current_payload, 2, spike_queue.current_node_id, STREAM_SPIKE)) {
        spike_queue.current_remaining--;
        spike_queue.total_injected++;
        spike_queue.current_retry_count = 0;
        last_spike_time_us = now_us;
        
        // Job complete?
        if (spike_queue.current_remaining == 0) {
            spike_queue_dequeue();
            printf("[SPIKE] Job done (total: %lu)\n", spike_queue.total_injected);
        }
    } else {
        // Broker queue full - retry next iteration
        spike_queue.current_retry_count++;
        if (spike_queue.current_retry_count > 1000) {
            printf("[SPIKE] ERROR: Broker stuck after 1000 retries, aborting job\n");
            spike_queue.current_remaining = 0;
            spike_queue_dequeue();
        }
    }
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
    
    // Don't track here - wait for ACK to ensure successful deployment
    
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
        // Service bus RX/TX more aggressively during wait
        for (int i = 0; i < 10; i++) {
            z1_broker_task();
            
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
            }  // Close if (z1_broker_try_receive)
        }  // Close for loop
        sleep_us(50);  // Small delay between polling bursts
    }
    
    printf("[HTTP TIMEOUT] No DEPLOY_ACK received from node %d after 200ms (%lu attempts)\n", node_id, rx_attempts);

    
    strcpy(response, "{\"error\":\"Timeout waiting for ACK\"}");
}

// ============================================================================
// OTA Update API Handlers  
// ============================================================================

/**
 * POST /api/ota/update_start - Start OTA update session
 * 
 * Initiates firmware update for target node. Sends UPDATE_START command
 * with firmware metadata (size, CRC32, chunk count).
 * 
 * Request body:
 *   {"node_id": N, "firmware_size": S, "crc32": C, "chunk_size": CS}
 * 
 * Response: {"status": "ok", "node_ready": true/false}
 * Error: {"error": "reason"}
 */
void handle_ota_update_start(const char* body, char* response, int size) {
    if (!body) {
        strcpy(response, "{\"error\":\"Missing request body\"}");
        return;
    }
    
    // Parse node_id
    const char* node_ptr = strstr(body, "\"node_id\"");
    if (!node_ptr) {
        strcpy(response, "{\"error\":\"Missing node_id field\"}");
        return;
    }
    uint8_t node_id = atoi(strchr(node_ptr, ':') + 1);
    
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID (0-15)\"}");
        return;
    }
    
    // Parse firmware_size
    const char* size_ptr = strstr(body, "\"firmware_size\"");
    if (!size_ptr) {
        strcpy(response, "{\"error\":\"Missing firmware_size field\"}");
        return;
    }
    uint32_t firmware_size = strtoul(strchr(size_ptr, ':') + 1, NULL, 0);
    
    // Parse CRC32
    const char* crc_ptr = strstr(body, "\"crc32\"");
    if (!crc_ptr) {
        strcpy(response, "{\"error\":\"Missing crc32 field\"}");
        return;
    }
    uint32_t expected_crc32 = strtoul(strchr(crc_ptr, ':') + 1, NULL, 0);
    
    // Parse chunk_size (optional, default 4096)
    uint16_t chunk_size = 4096;
    const char* chunk_ptr = strstr(body, "\"chunk_size\"");
    if (chunk_ptr) {
        chunk_size = atoi(strchr(chunk_ptr, ':') + 1);
    }
    
    printf("[OTA] Starting update: node=%d, size=%lu, crc=0x%08lX, chunk_size=%d\n",
           node_id, firmware_size, expected_crc32, chunk_size);
    
    // Initialize session state
    memset(&g_ota_session, 0, sizeof(g_ota_session));
    g_ota_session.target_node_id = node_id;
    g_ota_session.firmware_size = firmware_size;
    g_ota_session.expected_crc32 = expected_crc32;
    g_ota_session.chunk_size = chunk_size;
    g_ota_session.total_chunks = (firmware_size + chunk_size - 1) / chunk_size;
    g_ota_session.update_in_progress = true;
    g_ota_session.last_activity_ms = time_us_32() / 1000;
    
    // Build UPDATE_START command
    z1_update_start_t cmd;
    cmd.opcode = Z1_OPCODE_UPDATE_START;
    cmd.target_node_id = node_id;
    cmd.reserved_byte = 0;
    cmd.total_size = firmware_size;
    cmd.expected_crc32 = expected_crc32;
    cmd.chunk_size = chunk_size;
    cmd.total_chunks = g_ota_session.total_chunks;
    
    // Copy to aligned buffer to avoid packed struct alignment warning
    uint16_t aligned_cmd[8];  // sizeof(z1_update_start_t) = 16 bytes = 8 words
    memcpy(aligned_cmd, &cmd, sizeof(cmd));
    
    // Send command
    if (!z1_broker_send_command(aligned_cmd, sizeof(cmd)/2, node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send UPDATE_START\"}");
        g_ota_session.update_in_progress = false;
        return;
    }
    
    // Allow time for command to be transmitted by broker task
    sleep_ms(100);
    
    // Wait for READY response
    printf("[OTA] Waiting for UPDATE_READY ACK from node %d...\n", node_id);
    uint32_t timeout = time_us_32() + 2000000;  // 2 seconds
    z1_frame_t frame;
    bool got_ready = false;
    int poll_count = 0;
    
    while (time_us_32() < timeout) {
        z1_broker_task();
        
        if (z1_broker_try_receive(&frame)) {
            poll_count++;
            printf("[OTA-DEBUG] RX frame: src=%d type=%d payload[0]=0x%04X\n",
                   frame.src, frame.type, frame.payload[0]);
            
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == Z1_OPCODE_UPDATE_READY) {
                
                uint8_t status = frame.payload[1] & 0xFF;
                got_ready = (status == 0);  // 0 = ready
                printf("[OTA] Node %d responded: %s\n", node_id,
                       got_ready ? "READY" : "BUSY/ERROR");
                break;
            }
        }
        
        sleep_us(100);
    }
    
    printf("[OTA] Wait loop done: got_ready=%d, polls=%d\n", got_ready, poll_count);
    
    if (got_ready) {
        snprintf(response, size,
                 "{\"status\":\"ok\",\"node_ready\":true,\"total_chunks\":%d}",
                 g_ota_session.total_chunks);
    } else {
        strcpy(response, "{\"error\":\"Node did not respond or is busy\"}");
        g_ota_session.update_in_progress = false;
    }
}

/**
 * POST /api/ota/update_chunk - Send firmware data chunk
 * 
 * Transmits single firmware chunk to target node. Node stores in PSRAM buffer.
 * 
 * Request body:
 *   {"chunk_num": N, "data": "<base64-encoded chunk data>"}
 * 
 * Response: {"status": "ok", "chunk_num": N, "ack": true/false}
 */
void handle_ota_update_chunk(const char* body, char* response, int size) {
    if (!g_ota_session.update_in_progress) {
        strcpy(response, "{\"error\":\"No update session active\"}");
        return;
    }
    
    if (!body) {
        strcpy(response, "{\"error\":\"Missing request body\"}");
        return;
    }
    
    // Parse chunk_num
    const char* num_ptr = strstr(body, "\"chunk_num\"");
    if (!num_ptr) {
        strcpy(response, "{\"error\":\"Missing chunk_num field\"}");
        return;
    }
    uint16_t chunk_num = atoi(strchr(num_ptr, ':') + 1);
    
    if (chunk_num >= g_ota_session.total_chunks) {
        snprintf(response, size, "{\"error\":\"Invalid chunk_num %d (max %d)\"}",
                 chunk_num, g_ota_session.total_chunks - 1);
        return;
    }
    
    // Parse base64 data
    const char* data_ptr = strstr(body, "\"data\"");
    if (!data_ptr) {
        strcpy(response, "{\"error\":\"Missing data field\"}");
        return;
    }
    // Find the colon after "data"
    data_ptr = strchr(data_ptr, ':');
    if (!data_ptr) {
        strcpy(response, "{\"error\":\"Invalid data format - no colon\"}");
        return;
    }
    data_ptr++;  // Skip colon
    
    // Skip whitespace
    while (*data_ptr == ' ' || *data_ptr == '\t') data_ptr++;
    
    // Find opening quote
    if (*data_ptr != '\"') {
        strcpy(response, "{\"error\":\"Invalid data format - no opening quote\"}");
        return;
    }
    data_ptr++;  // Skip opening quote
    
    const char* data_end = strchr(data_ptr, '\"');  // Find closing quote
    if (!data_end) {
        strcpy(response, "{\"error\":\"Unterminated data string\"}");
        return;
    }
    
    uint32_t b64_len = data_end - data_ptr;
    
    // Decode base64 (reuse existing decoder from memory write handler)
    uint8_t* decoded = DECODED_BUFFER_1024_PSRAM;  // Use PSRAM to save SRAM
    uint16_t decoded_len = 0;
    
    static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t val = 0;
    int bits = -8;
    
    for (uint32_t i = 0; i < b64_len && decoded_len < 1024; i++) {
        char c = data_ptr[i];
        if (c == '=') break;
        
        const char* p = strchr(b64_table, c);
        if (!p) continue;
        
        val = (val << 6) | (p - b64_table);
        bits += 6;
        
        if (bits >= 0) {
            decoded[decoded_len++] = (val >> bits) & 0xFF;
            bits -= 8;
        }
    }
    
    printf("[OTA] Sending chunk %d/%d (%d bytes)\n",
           chunk_num, g_ota_session.total_chunks - 1, decoded_len);
    
    // Build UPDATE_DATA_CHUNK frame
    // Header: opcode(1), target_node_id(0.5), reserved(0.5), chunk_num(1), data_size(1) = 4 words
    // Data: variable length
    uint16_t* frame = g_shared_frame_buffer;  // Use shared buffer
    z1_update_data_chunk_t* hdr = (z1_update_data_chunk_t*)frame;
    hdr->opcode = Z1_OPCODE_UPDATE_DATA_CHUNK;
    hdr->target_node_id = g_ota_session.target_node_id;
    hdr->reserved_byte = 0;
    hdr->chunk_num = chunk_num;
    hdr->data_size = decoded_len;
    
    // Copy data after header (4 words)
    uint8_t* data_out = (uint8_t*)&frame[4];
    memcpy(data_out, decoded, decoded_len);
    
    uint16_t total_words = 4 + ((decoded_len + 1) / 2);
    
    // Send command
    if (!z1_broker_send_command(frame, total_words, g_ota_session.target_node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to queue chunk\"}");
        return;
    }
    
    // Allow time for command to be transmitted before waiting for ACK
    sleep_ms(50);
    
    // Wait for ACK
    uint32_t timeout = time_us_32() + 500000;  // 500ms per chunk
    z1_frame_t ack_frame;
    bool got_ack = false;
    
    while (time_us_32() < timeout) {
        z1_broker_task();
        
        if (z1_broker_try_receive(&ack_frame)) {
            if (ack_frame.src == g_ota_session.target_node_id 
                && ack_frame.type == Z1_FRAME_TYPE_CTRL 
                && ack_frame.payload[0] == Z1_OPCODE_UPDATE_ACK_CHUNK) {
                
                uint16_t acked_chunk = ack_frame.payload[1];
                if (acked_chunk == chunk_num) {
                    got_ack = true;
                    ota_mark_chunk_sent(chunk_num);
                    g_ota_session.chunks_sent++;
                    printf("[OTA] Chunk %d ACKed (%d/%d complete)\n",
                           chunk_num, g_ota_session.chunks_sent, g_ota_session.total_chunks);
                    break;
                }
            }
        }
        
        sleep_us(100);
    }
    
    g_ota_session.last_activity_ms = time_us_32() / 1000;
    
    if (got_ack) {
        snprintf(response, size,
                 "{\"status\":\"ok\",\"chunk_num\":%d,\"ack\":true,\"progress\":\"%d/%d\"}",
                 chunk_num, g_ota_session.chunks_sent, g_ota_session.total_chunks);
    } else {
        snprintf(response, size,
                 "{\"status\":\"timeout\",\"chunk_num\":%d,\"ack\":false}",
                 chunk_num);
    }
}

/**
 * POST /api/ota/update_verify - Request CRC32 verification
 * 
 * Asks node to compute CRC32 of received firmware in PSRAM and compare
 * against expected value.
 * 
 * Request body: {} (uses session state)
 * Response: {"status": "ok"/"fail", "crc_match": true/false, "computed_crc": C}
 */
void handle_ota_update_verify(const char* body, char* response, int size) {
    if (!g_ota_session.update_in_progress) {
        strcpy(response, "{\"error\":\"No update session active\"}");
        return;
    }
    
    printf("[OTA] Requesting verification from node %d\n", g_ota_session.target_node_id);
    
    // Send UPDATE_VERIFY_REQ
    z1_update_poll_t cmd;
    cmd.opcode = Z1_OPCODE_UPDATE_POLL;
    cmd.poll_node_id = g_ota_session.target_node_id;
    cmd.poll_type = Z1_POLL_TYPE_VERIFY;
    cmd.reserved[0] = 0;
    cmd.reserved[1] = 0;
    
    // Copy to aligned buffer to avoid packed struct alignment warning
    uint16_t aligned_cmd[4];  // sizeof(z1_update_poll_t) = 8 bytes = 4 words
    memcpy(aligned_cmd, &cmd, sizeof(cmd));
    
    if (!z1_broker_send_command(aligned_cmd, sizeof(cmd)/2,
                                 g_ota_session.target_node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send VERIFY_REQ\"}");
        return;
    }
    
    // Wait for VERIFY_RESP (may take several seconds for large firmware)
    uint32_t timeout = time_us_32() + 5000000;  // 5 seconds
    z1_frame_t frame;
    bool got_response = false;
    bool crc_match = false;
    uint32_t computed_crc = 0;
    
    while (time_us_32() < timeout) {
        z1_broker_task();
        
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == g_ota_session.target_node_id 
                && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == Z1_OPCODE_UPDATE_VERIFY_RESP) {
                
                uint8_t status = frame.payload[1] & 0xFF;
                computed_crc = ((uint32_t)frame.payload[3] << 16) | frame.payload[2];
                crc_match = (status == 0);  // 0 = OK (CRC match)
                got_response = true;
                printf("[OTA] Verification: %s (computed=0x%08lX, expected=0x%08lX)\n",
                       crc_match ? "PASS" : "FAIL", computed_crc, g_ota_session.expected_crc32);
                break;
            }
        }
        
        sleep_us(100);
    }
    
    if (got_response) {
        snprintf(response, size,
                 "{\"status\":\"%s\",\"crc_match\":%s,\"computed_crc\":\"0x%08lX\",\"expected_crc\":\"0x%08lX\"}",
                 crc_match ? "ok" : "fail",
                 crc_match ? "true" : "false",
                 computed_crc,
                 g_ota_session.expected_crc32);
    } else {
        strcpy(response, "{\"error\":\"Verification timeout (5s)\"}");
    }
}

/**
 * POST /api/ota/update_commit - Commit firmware to flash
 * 
 * Commands node to write PSRAM buffer to application partition (0x00080000).
 * This is a destructive operation - old firmware is erased.
 * 
 * Request body: {} (uses session state)
 * Response: {"status": "ok"/"fail", "flash_ok": true/false}
 */
void handle_ota_update_commit(const char* body, char* response, int size) {
    if (!g_ota_session.update_in_progress) {
        strcpy(response, "{\"error\":\"No update session active\"}");
        return;
    }
    
    printf("[OTA] Requesting flash commit on node %d\n", g_ota_session.target_node_id);
    
    // Broadcast UPDATE_COMMIT (though we only expect one node to respond)
    uint16_t cmd = Z1_OPCODE_UPDATE_COMMIT;
    if (!z1_broker_send_command(&cmd, 1, g_ota_session.target_node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send COMMIT\"}");
        return;
    }
    
    // Wait for COMMIT_RESP (flash erase + program takes time)
    uint32_t timeout = time_us_32() + 30000000;  // 30 seconds
    z1_frame_t frame;
    bool got_response = false;
    bool flash_ok = false;
    
    printf("[OTA] Waiting for flash commit (up to 30s)...\n");
    
    while (time_us_32() < timeout) {
        z1_broker_task();
        
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == g_ota_session.target_node_id 
                && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == Z1_OPCODE_UPDATE_COMMIT_RESP) {
                
                uint8_t status = frame.payload[1] & 0xFF;
                flash_ok = (status == 0);  // 0 = success
                got_response = true;
                printf("[OTA] Flash commit: %s\n", flash_ok ? "SUCCESS" : "FAILED");
                break;
            }
        }
        
        sleep_us(1000);  // 1ms poll interval (flash is slow)
    }
    
    if (got_response) {
        snprintf(response, size,
                 "{\"status\":\"%s\",\"flash_ok\":%s}",
                 flash_ok ? "ok" : "fail",
                 flash_ok ? "true" : "false");
    } else {
        strcpy(response, "{\"error\":\"Flash commit timeout (30s)\"}");
    }
}

/**
 * POST /api/ota/update_restart - Restart node with new firmware
 * 
 * Commands node to perform soft reset. Bootloader will validate and launch
 * new firmware from application partition.
 * 
 * Request body: {} (uses session state)
 * Response: {"status": "ok"} (node will disconnect shortly)
 */
void handle_ota_update_restart(const char* body, char* response, int size) {
    if (!g_ota_session.update_in_progress) {
        strcpy(response, "{\"error\":\"No update session active\"}");
        return;
    }
    
    printf("[OTA] Requesting restart on node %d\n", g_ota_session.target_node_id);
    
    // Broadcast UPDATE_RESTART
    uint16_t cmd = Z1_OPCODE_UPDATE_RESTART;
    if (!z1_broker_send_command(&cmd, 1, g_ota_session.target_node_id, STREAM_NODE_MGMT)) {
        strcpy(response, "{\"error\":\"Failed to send RESTART\"}");
        return;
    }
    
    // Give node time to transmit command before response
    for (int i = 0; i < 100; i++) {
        z1_broker_task();
        sleep_us(100);
    }
    
    // Clear session state
    g_ota_session.update_in_progress = false;
    
    snprintf(response, size,
             "{\"status\":\"ok\",\"message\":\"Node %d restarting with new firmware\"}",
             g_ota_session.target_node_id);
}

/**
 * GET /api/ota/status - Get current OTA session status
 * 
 * Response: 
 *   {"active": true/false, "node_id": N, "progress": "X/Y chunks", 
 *    "firmware_size": S, "last_activity_ms": T}
 */
void handle_ota_status(char* response, int size) {
    if (!g_ota_session.update_in_progress) {
        strcpy(response, "{\"active\":false}");
        return;
    }
    
    snprintf(response, size,
             "{\"active\":true,\"node_id\":%d,\"progress\":\"%d/%d\","
             "\"firmware_size\":%lu,\"last_activity_ms\":%lu}",
             g_ota_session.target_node_id,
             g_ota_session.chunks_sent,
             g_ota_session.total_chunks,
             g_ota_session.firmware_size,
             g_ota_session.last_activity_ms);
}

// ============================================================================
// SD Card-Based OTA Update
// ============================================================================

/**
 * POST /api/nodes/{id}/update - Update node firmware from SD card file
 * 
 * Simpler, more reliable alternative to HTTP-based OTA. Leverages proven
 * file upload infrastructure and eliminates HTTP body streaming complexity.
 * 
 * Flow:
 *   1. Client uploads firmware via PUT /api/files/firmware/app.bin
 *   2. Client triggers update via this endpoint
 *   3. Controller reads file from SD in 512-byte chunks
 *   4. Controller sends chunks to node via bus (existing protocol)
 *   5. Node receives in bootloader (existing code - no changes)
 *   6. Node programs flash and reboots
 * 
 * Request: {"filepath": "firmware/node_app_16.bin"}
 * Response: {"status": "ok", "bytes_sent": N, "chunks": C, "time_ms": T}
 * 
 * Error handling:
 *   - Retries each chunk 3 times on ACK timeout
 *   - Aborts if node stops responding
 *   - Returns detailed error message
 */
void handle_node_update_from_sd(uint8_t node_id, const char* body, 
                                 char* response, int size) {
    if (node_id >= 16) {
        strcpy(response, "{\"error\":\"Invalid node ID\"}");
        return;
    }
    
    // Parse filepath from JSON body
    char filepath[256] = {0};
    const char* filepath_start = strstr(body, "\"filepath\"");
    if (!filepath_start) {
        strcpy(response, "{\"error\":\"Missing 'filepath' field\"}");
        return;
    }
    
    // Find value after colon
    const char* colon = strchr(filepath_start, ':');
    if (!colon) {
        strcpy(response, "{\"error\":\"Invalid JSON format\"}");
        return;
    }
    
    // Skip whitespace and opening quote
    const char* value = colon + 1;
    while (*value == ' ' || *value == '\t' || *value == '\n') value++;
    if (*value != '"') {
        strcpy(response, "{\"error\":\"filepath must be a string\"}");
        return;
    }
    value++;  // Skip opening quote
    
    // Copy until closing quote
    size_t path_len = 0;
    while (*value && *value != '"' && path_len < sizeof(filepath) - 1) {
        filepath[path_len++] = *value++;
    }
    filepath[path_len] = '\0';
    
    printf("[SD-OTA] Starting update for node %d from: %s\n", node_id, filepath);
    
    // Open firmware file
    FIL fil;
    FRESULT fr = f_open(&fil, filepath, FA_READ);
    if (fr != FR_OK) {
        snprintf(response, size, 
                 "{\"error\":\"Failed to open file '%s' (FRESULT=%d)\"}", 
                 filepath, fr);
        return;
    }
    
    FSIZE_t file_size = f_size(&fil);
    printf("[SD-OTA] File size: %lu bytes\n", (unsigned long)file_size);
    
    // Calculate total chunks (512 bytes each)
    uint32_t total_chunks = (file_size + 511) / 512;
    
    // Reset node to bootloader
    printf("[SD-OTA] Resetting node %d to bootloader...\n", node_id);
    uint16_t cmd = OPCODE_RESET_TO_BOOTLOADER;
    if (!z1_broker_send_command(&cmd, 1, node_id, STREAM_NODE_MGMT)) {
        f_close(&fil);
        strcpy(response, "{\"error\":\"Failed to send reset command\"}");
        return;
    }
    
    // Pump broker to transmit reset command
    for (int i = 0; i < 50; i++) {
        z1_broker_task();
        sleep_us(100);
    }
    
    // Wait for bootloader to start (6 second countdown + margin)
    printf("[SD-OTA] Waiting for bootloader...\n");
    sleep_ms(7000);
    
    // Send UPDATE_START command
    printf("[SD-OTA] Sending UPDATE_START...\n");
    z1_update_start_t start_cmd;
    start_cmd.opcode = Z1_OPCODE_UPDATE_START;
    start_cmd.target_node_id = node_id;
    start_cmd.reserved_byte = 0;
    start_cmd.total_size = file_size;
    start_cmd.expected_crc32 = 0;  // Calculate later if needed
    start_cmd.chunk_size = 512;
    start_cmd.total_chunks = total_chunks;
    
    uint16_t aligned_start[8];  // sizeof(z1_update_start_t) = 16 bytes = 8 words
    memcpy(aligned_start, &start_cmd, sizeof(start_cmd));
    
    if (!z1_broker_send_command(aligned_start, sizeof(start_cmd)/2, node_id, STREAM_NODE_MGMT)) {
        f_close(&fil);
        strcpy(response, "{\"error\":\"Failed to send UPDATE_START\"}");
        return;
    }
    
    // Wait for UPDATE_READY response
    uint32_t timeout = time_us_32() + 2000000;  // 2 seconds
    z1_frame_t frame;
    bool got_ready = false;
    
    while (time_us_32() < timeout) {
        z1_broker_task();
        
        if (z1_broker_try_receive(&frame)) {
            if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                && frame.payload[0] == Z1_OPCODE_UPDATE_READY) {
                got_ready = true;
                printf("[SD-OTA] Node ready for firmware\n");
                break;
            }
        }
        sleep_us(100);
    }
    
    if (!got_ready) {
        f_close(&fil);
        strcpy(response, "{\"error\":\"Node did not respond with UPDATE_READY\"}");
        return;
    }
    
    // Stream firmware chunks from SD card
    printf("[SD-OTA] Streaming %lu chunks...\n", (unsigned long)total_chunks);
    uint32_t start_time = time_us_32();
    uint32_t chunks_sent = 0;
    uint8_t chunk_buffer[512];
    bool update_failed = false;
    char error_msg[128] = {0};
    
    for (uint32_t chunk_num = 0; chunk_num < total_chunks && !update_failed; chunk_num++) {
        // Read chunk from SD card
        UINT bytes_read = 0;
        fr = f_read(&fil, chunk_buffer, 512, &bytes_read);
        if (fr != FR_OK || bytes_read == 0) {
            snprintf(error_msg, sizeof(error_msg), 
                     "SD read failed at chunk %lu (FRESULT=%d)", 
                     (unsigned long)chunk_num, fr);
            update_failed = true;
            break;
        }
        
        // Build UPDATE_DATA_CHUNK frame
        z1_update_data_chunk_t* hdr = (z1_update_data_chunk_t*)g_shared_frame_buffer;
        hdr->opcode = Z1_OPCODE_UPDATE_DATA_CHUNK;
        hdr->target_node_id = node_id;
        hdr->reserved_byte = 0;
        hdr->chunk_num = chunk_num;
        hdr->data_size = bytes_read;
        
        // Copy chunk data after header (4 words = 8 bytes)
        uint8_t* data_out = (uint8_t*)&g_shared_frame_buffer[4];
        memcpy(data_out, chunk_buffer, bytes_read);
        
        // Debug: Show first chunk header
        if (chunk_num == 0) {
            printf("[SD-OTA] Chunk 0 first 16 bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                   chunk_buffer[0], chunk_buffer[1], chunk_buffer[2], chunk_buffer[3],
                   chunk_buffer[4], chunk_buffer[5], chunk_buffer[6], chunk_buffer[7],
                   chunk_buffer[8], chunk_buffer[9], chunk_buffer[10], chunk_buffer[11],
                   chunk_buffer[12], chunk_buffer[13], chunk_buffer[14], chunk_buffer[15]);
        }
        
        uint16_t total_words = 4 + ((bytes_read + 1) / 2);
        
        // Retry logic: 3 attempts per chunk
        bool chunk_acked = false;
        for (int attempt = 0; attempt < 3 && !chunk_acked; attempt++) {
            // Send chunk
            if (!z1_broker_send_command(g_shared_frame_buffer, total_words, node_id, STREAM_NODE_MGMT)) {
                if (attempt == 2) {
                    snprintf(error_msg, sizeof(error_msg), 
                             "Failed to queue chunk %lu after 3 attempts", 
                             (unsigned long)chunk_num);
                    update_failed = true;
                    break;
                }
                sleep_ms(10);
                continue;
            }
            
            // Wait for ACK
            timeout = time_us_32() + 500000;  // 500ms per chunk
            
            while (time_us_32() < timeout) {
                z1_broker_task();
                
                if (z1_broker_try_receive(&frame)) {
                    if (frame.src == node_id && frame.type == Z1_FRAME_TYPE_CTRL 
                        && frame.payload[0] == Z1_OPCODE_UPDATE_ACK_CHUNK) {
                        
                        uint16_t acked_chunk = frame.payload[1];
                        if (acked_chunk == chunk_num) {
                            chunk_acked = true;
                            chunks_sent++;
                            
                            // Progress indicator every 10 chunks
                            if (chunk_num % 10 == 0) {
                                printf("[SD-OTA] Progress: %lu/%lu chunks (%d%%)\n",
                                       (unsigned long)chunks_sent, 
                                       (unsigned long)total_chunks,
                                       (int)((chunks_sent * 100) / total_chunks));
                            }
                            break;
                        }
                    }
                }
                
                sleep_us(100);
            }
            
            if (!chunk_acked && attempt < 2) {
                printf("[SD-OTA] Chunk %lu ACK timeout, retry %d/3\n", 
                       (unsigned long)chunk_num, attempt + 2);
                sleep_ms(50);
            }
        }
        
        if (!chunk_acked) {
            snprintf(error_msg, sizeof(error_msg), 
                     "Chunk %lu ACK timeout after 3 attempts", 
                     (unsigned long)chunk_num);
            update_failed = true;
        }
    }
    
    f_close(&fil);
    
    if (update_failed) {
        snprintf(response, size, "{\"error\":\"%s\"}", error_msg);
        return;
    }
    
    printf("[SD-OTA] All chunks sent successfully\n");
    
    // Send UPDATE_COMMIT to program flash
    printf("[SD-OTA] Committing to flash...\n");
    uint16_t commit_cmd = Z1_OPCODE_UPDATE_COMMIT;
    z1_broker_send_command(&commit_cmd, 1, node_id, STREAM_NODE_MGMT);
    
    // Give time for flash programming
    sleep_ms(5000);
    
    // Send UPDATE_MODE_EXIT to reboot
    printf("[SD-OTA] Rebooting node...\n");
    uint16_t exit_cmd = Z1_OPCODE_UPDATE_MODE_EXIT;
    z1_broker_send_command(&exit_cmd, 1, node_id, STREAM_NODE_MGMT);
    
    for (int i = 0; i < 100; i++) {
        z1_broker_task();
        sleep_us(100);
    }
    
    uint32_t elapsed_ms = (time_us_32() - start_time) / 1000;
    
    snprintf(response, size,
             "{\"status\":\"ok\",\"node_id\":%d,\"bytes_sent\":%lu,"
             "\"chunks\":%lu,\"time_ms\":%lu}",
             node_id, (unsigned long)file_size, (unsigned long)chunks_sent, 
             (unsigned long)elapsed_ms);
    
    printf("[SD-OTA] Update complete: %lu bytes in %lu ms\n", 
           (unsigned long)file_size, (unsigned long)elapsed_ms);
}

// POST /api/firmware/deploy - Deploy firmware from SD card to nodes
// Multi-node OTA deployment using streaming protocol and PSRAM buffering
// NOTE: Future enhancement - currently use 'nflash' CLI tool for OTA updates
void handle_firmware_deploy(const char* body, char* response, int size) {
    (void)body;  // Reserved for future JSON deployment config
    (void)size;
    
#ifdef HW_V2
    // FUTURE: Full multi-node deployment automation
    // - Parse JSON body for engine name and target node list
    // - Read .z1app from SD card or HTTP upload
    // - Coordinate concurrent OTA updates across multiple nodes
    // - Implement progress tracking and error recovery
    
    // For now, use 'nflash' CLI tool which provides full OTA functionality:
    //   python python_tools/bin/nflash node_app_16.bin --node 0
    
    strcpy(response, "{\"status\":\"not_implemented\",\"message\":\"Use nflash CLI tool for OTA updates\"}");
#else
    strcpy(response, "{\"error\":\"V1 hardware does not support global reset\"}");
#endif
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
    
    // Reset response metadata (default to JSON)
    g_response_metadata.is_binary = false;
    g_response_metadata.content_length = 0;
    g_response_metadata.content_type = NULL;
    
    // GET / - Root HTML splash screen
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        handle_root(response, size);
        return 200;
    }
    
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
        printf("[HTTP-ROUTE] Routing GET /api/snn/status to handle_global_snn_status()\n");
        handle_global_snn_status(response, size);
        printf("[HTTP-ROUTE] Returned from handle_global_snn_status()\n");
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
    
    // POST /api/nodes/reset - Reset all nodes to bootloader
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/nodes/reset", 16) == 0) {
        // Extract query string for mode parameter (e.g., ?mode=software, ?node=0)
        const char* query = strchr(path, '?');
        handle_reset_to_bootloader(response, size, query);
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
    
    // POST /api/nodes/{id}/update - SD card-based firmware update
    if (strcmp(method, "POST") == 0 && strstr(path, "/api/nodes/") != NULL && strstr(path, "/update") != NULL) {
        const char* id_start = path + 11;  // Skip "/api/nodes/"
        int node_id = atoi(id_start);
        printf("[HTTP API] SD-OTA update for node %d\n", node_id);
        handle_node_update_from_sd(node_id, body, response, size);
        return 200;
    }
    
    // POST /api/nodes/{id}/snn/load
    if (strcmp(method, "POST") == 0 && strstr(path, "/snn/load") != NULL) {
        const char* id_start = path + 11;
        int node_id = atoi(id_start);
        handle_load_topology(node_id, body, response, size);
        return 200;
    }
    
    // SD Card endpoints
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/sd/status") == 0) {
        handle_sd_status(response, size);
        return 200;
    }
    
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/config") == 0) {
        handle_get_config(response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/config") == 0) {
        handle_set_config(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/files/", 11) == 0) {
        const char* filepath = path + 11;
        
        // Check if it's a file or directory
        // Try to list as directory first (sd_card_list_directory returns -1 if not a dir)
        g_response_metadata.is_binary = false;
        g_response_metadata.content_length = 0;
        
        // Start with empty JSON array
        int pos = snprintf(response, size, "{\"files\":[");
        g_list_response_buffer = response;
        g_list_response_pos = pos;
        g_list_response_size = size;
        g_file_count = 0;
        
        int file_count = sd_card_list_directory(filepath, list_files_callback);
        
        if (file_count >= 0) {
            // It's a directory - complete the JSON response
            pos = g_list_response_pos;
            if (pos < size - 20) {
                snprintf(response + pos, size - pos, "],\"count\":%d}", file_count);
            }
            return 200;
        } else {
            // Not a directory - try as file download
            int file_size = handle_download_file(filepath, response, size);
            if (file_size > 0) {
                // Set metadata for binary file response
                g_response_metadata.is_binary = true;
                g_response_metadata.content_length = file_size;
                return 200;  // Success, response contains file data
            } else {
                // Error reading file - response contains JSON error
                return 404;  // File read error, response contains JSON error
            }
        }
    }
    
    if (strcmp(method, "PUT") == 0 && strncmp(path, "/api/files/", 11) == 0) {
        const char* filepath = path + 11;
        handle_upload_file(filepath, body, response, size);
        return 200;
    }
    
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/files/", 11) == 0) {
        const char* filepath = path + 11;
        handle_delete_file(filepath, response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/system/reboot") == 0) {
        handle_system_reboot(response, size);
        // Special return code to signal reboot needed
        return 299;  // Custom code: success + reboot pending
    }
    
    // OTA endpoints
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota/update_start") == 0) {
        handle_ota_update_start(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota/update_chunk") == 0) {
        handle_ota_update_chunk(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota/update_verify") == 0) {
        handle_ota_update_verify(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota/update_commit") == 0) {
        handle_ota_update_commit(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/ota/update_restart") == 0) {
        handle_ota_update_restart(body, response, size);
        return 200;
    }
    
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ota/status") == 0) {
        handle_ota_status(response, size);
        return 200;
    }
    
    // Firmware deployment (controller-based) - Phase 3
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/firmware/deploy") == 0) {
        handle_firmware_deploy(body, response, size);
        return 200;
    }
    
    // 404 Not Found
    strcpy(response, "{\"error\":\"Not found\"}");
    return 404;
}
