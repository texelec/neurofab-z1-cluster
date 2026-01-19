/**
 * Z1 Onyx - OTA Update Engine
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Handles firmware update commands from controller via Matrix bus.
 * Receives .z1app packages, validates them, and programs flash.
 * 
 * OTA Process:
 * 1. Controller sends UPDATE_MODE_ENTER broadcast
 * 2. Node enters OTA mode, prepares PSRAM buffer
 * 3. Controller sends DATA_CHUNK frames (2KB each)
 * 4. Node acknowledges each chunk (CHUNK_ACK)
 * 5. Controller sends UPDATE_FINALIZE
 * 6. Node validates header, checks CRC32
 * 7. Node erases app partition (0x00200000)
 * 8. Node programs new app to flash
 * 9. Controller sends UPDATE_MODE_EXIT
 * 10. Node reboots, bootloader validates and jumps to new app
 * 
 * CRITICAL BUG FIX (Jan 16, 2026):
 * ---------------------------------
 * PSRAM writes were using CACHED XIP address (0x11000000) which caused data
 * corruption due to RP2350 cache coherency issues. Symptoms:
 * - Magic number 0x5A314150 corrupted to 0x15130405
 * - Data verified correct immediately after write
 * - Data corrupted by the time finalize() reads it (5 seconds later)
 * 
 * Root cause: XIP cache not flushed properly, broker DMA/interrupts trigger
 * cache evictions that overwrite correct data with stale cached values.
 * 
 * Solution: psram_write() now uses UNCACHED XIP alias (0x15000000) which
 * bypasses cache entirely. See psram_rp2350.c for detailed explanation.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "z1_bus.h"
#include "z1_broker.h"
#include "z1_commands.h"
#include "ota_engine.h"
#include "psram_rp2350.h"

// External references
extern uint8_t g_node_id;  // Defined in bootloader_main.c

// OTA state machine
typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RECEIVING,
    OTA_STATE_VALIDATING,
    OTA_STATE_PROGRAMMING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ERROR
} ota_state_t;

// OTA error codes
typedef enum {
    OTA_ERROR_NONE = 0,
    OTA_ERROR_INVALID_MAGIC = 1,
    OTA_ERROR_CRC_MISMATCH = 2,
    OTA_ERROR_INVALID_SIZE = 3,
    OTA_ERROR_FLASH_ERROR = 4,
    OTA_ERROR_CHUNK_SEQ = 5
} ota_error_t;

// App header structure (must match .z1app format)
typedef struct __attribute__((packed)) {
    uint32_t magic;              // 0x5A314150 ("Z1AP")
    uint32_t version_major;
    uint32_t version_minor;
    uint32_t version_patch;
    uint32_t flags;
    uint32_t binary_size;        // Size of binary data (excludes header)
    uint32_t crc32;              // CRC32 of binary only
    uint32_t entry_point;        // Offset from partition start (should be 0xC0)
    char     name[32];           // Null-terminated app name
    char     description[64];    // Null-terminated description
    uint8_t  reserved[64];       // Reserved for future use
} app_header_t;

// OTA globals
static bool g_ota_mode = false;
static ota_state_t g_ota_state = OTA_STATE_IDLE;
static ota_error_t g_ota_error = OTA_ERROR_NONE;
static uint32_t g_bytes_received = 0;
static uint32_t g_expected_chunks = 0;
static uint32_t g_chunks_received = 0;
static uint8_t* g_ota_buffer = NULL;  // PSRAM buffer at 0x11010000

// CRC32 table (for fast calculation)
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

// Controller node ID
#define CONTROLLER_NODE_ID 16  // Controller is always node 16

// External node ID from bootloader_main
extern uint8_t g_node_id;

// PSRAM is BROKEN in bootloader - writes corrupt data
// Solution: Use SRAM buffer, write directly from SRAM to flash (bypass PSRAM entirely)

// SRAM buffer for OTA (48KB max firmware size)
#define OTA_BUFFER_SIZE (48 * 1024)
static uint8_t g_sram_ota_buffer[OTA_BUFFER_SIZE] __attribute__((aligned(4)));

// Flash app partition (physical addresses)
#define APP_PARTITION_OFFSET 0x00080000  // 512KB into flash
#define APP_PARTITION_SIZE (7680 * 1024)  // 7.5MB (0x780000)

/**
 * Initialize CRC32 lookup table
 */
static void init_crc32_table(void) {
    if (crc32_table_initialized) return;
    
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    
    crc32_table_initialized = true;
}

/**
 * Calculate CRC32 of data buffer
 */
static uint32_t calculate_crc32(const uint8_t* data, uint32_t length) {
    init_crc32_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t byte = data[i];
        crc = (crc >> 8) ^ crc32_table[(crc ^ byte) & 0xFF];
    }
    
    return ~crc;
}

/**
 * Send OTA error response to controller
 */
static void send_ota_error(ota_error_t error_code) {
    uint16_t response[2];
    response[0] = Z1_OPCODE_UPDATE_ERROR;
    response[1] = error_code;
    
    z1_broker_send_command(response, 2, CONTROLLER_NODE_ID, STREAM_NODE_MGMT);
    
    printf("[OTA] ERROR %d sent to controller\n", error_code);
    g_ota_error = error_code;
    g_ota_state = OTA_STATE_ERROR;
}

/**
 * Initialize OTA engine
 */
void ota_init(void) {
    printf("[OTA] Initializing OTA engine\n");
    
    g_ota_mode = false;
    g_ota_state = OTA_STATE_IDLE;
    g_ota_error = OTA_ERROR_NONE;
    g_bytes_received = 0;
    g_expected_chunks = 0;
    g_chunks_received = 0;
    
    // Use SRAM buffer (PSRAM proven unstable in bootloader)
    g_ota_buffer = g_sram_ota_buffer;
    
    init_crc32_table();
    
    printf("[OTA] Ready (SRAM buffer at 0x%08lX, 48KB)\n", (uint32_t)g_sram_ota_buffer);
}

/**
 * Handle UPDATE_MODE_ENTER command
 */
void ota_handle_enter_update_mode(void) {
    printf("[OTA] Entering update mode\n");
    
    g_ota_mode = true;
    g_ota_state = OTA_STATE_IDLE;
    g_ota_error = OTA_ERROR_NONE;
    g_bytes_received = 0;
    g_chunks_received = 0;
    
    // Clear SRAM buffer
    memset(g_ota_buffer, 0, 4096);
    
    // Send UPDATE_READY response
    z1_update_ready_t ready;
    ready.opcode = Z1_OPCODE_UPDATE_READY;
    ready.node_id = g_node_id;
    ready.status = 0;  // 0 = ready
    ready.available_psram = 7 * 1024 * 1024;  // 7MB available (8MB - 1MB safety)
    
    // Copy to aligned buffer
    uint16_t aligned_ready[4];  // sizeof(z1_update_ready_t) = 8 bytes = 4 words
    memcpy(aligned_ready, &ready, sizeof(ready));
    
    z1_broker_send_command(aligned_ready, sizeof(ready)/2, CONTROLLER_NODE_ID, STREAM_NODE_MGMT);
    
    printf("[OTA] Ready for firmware chunks\n");
}

/**
 * Handle DATA_CHUNK command
 * Frame header: z1_update_data_chunk_t
 *   [0] = opcode (0x0083)
 *   [1] = chunk_num
 *   [2] = data_size
 *   [3+] = actual chunk data
 */
void ota_handle_data_chunk(z1_frame_t* frame) {
    if (!g_ota_mode) {
        printf("[OTA] ERROR: Chunk received but not in OTA mode\n");
        return;
    }
    
    if (frame->length < 6) {  // Header is 6 bytes (3 words)
        printf("[OTA] ERROR: Chunk frame too short (%d bytes)\n", frame->length);
        return;
    }
    
    // Parse header - matches z1_update_data_chunk_t
    z1_update_data_chunk_t* hdr = (z1_update_data_chunk_t*)frame->payload;
    uint16_t chunk_num = hdr->chunk_num;
    uint16_t chunk_size = hdr->data_size;
    
    // Data starts after 8-byte header (4 words): opcode, target+reserved, chunk_num, data_size
    uint8_t* chunk_data = (uint8_t*)&frame->payload[4];
    
    // Validate chunk sequence
    if (chunk_num != g_chunks_received) {
        printf("[OTA] ERROR: Chunk sequence mismatch (expected %lu, got %d)\n",
               g_chunks_received, chunk_num);
        send_ota_error(OTA_ERROR_CHUNK_SEQ);
        return;
    }
    
    // Calculate buffer offset (chunks can be variable size)
    uint32_t offset = g_bytes_received;
    
    // Check buffer bounds
    if (offset + chunk_size > OTA_BUFFER_SIZE) {
        printf("[OTA] ERROR: Chunk would overflow buffer (offset=%lu, size=%d, max=%d)\n",
               offset, chunk_size, OTA_BUFFER_SIZE);
        send_ota_error(OTA_ERROR_INVALID_SIZE);
        return;
    }
    
    // Write chunk data to SRAM buffer (memcpy proven reliable)
    memcpy(g_ota_buffer + offset, chunk_data, chunk_size);
    
    g_bytes_received += chunk_size;
    g_chunks_received++;
    g_ota_state = OTA_STATE_RECEIVING;
    
    // Debug: Check EVERY chunk from 0-20 to pinpoint exact corruption point
    if (chunk_num <= 20) {
        volatile uint8_t* check = (volatile uint8_t*)g_ota_buffer;
        printf("[OTA-CHECK] After chunk %d, magic: %02X %02X %02X %02X\n",
               chunk_num, check[0], check[1], check[2], check[3]);
    }
    
    // Send ACK
    uint16_t ack[2];
    ack[0] = Z1_OPCODE_UPDATE_ACK_CHUNK;
    ack[1] = chunk_num;
    z1_broker_send_command(ack, 2, CONTROLLER_NODE_ID, STREAM_NODE_MGMT);
    
    printf("[OTA] Chunk %d received (%d bytes, total=%lu)\n",
           chunk_num, chunk_size, g_bytes_received);
}

/**
 * Handle UPDATE_VERIFY_REQ command
 * Calculate CRC32 of received data and send back to controller
 */
void ota_handle_verify(void) {
    if (!g_ota_mode) {
        printf("[OTA] ERROR: Verify received but not in OTA mode\n");
        return;
    }
    
    printf("[OTA] Verifying %lu bytes...\n", g_bytes_received);
    
    // Calculate CRC32 of all received data
    uint32_t calculated_crc = calculate_crc32(g_ota_buffer, g_bytes_received);
    
    printf("[OTA] Calculated CRC32: 0x%08lX\n", calculated_crc);
    
    // Build response using proper struct format
    z1_update_verify_resp_t resp;
    resp.opcode = Z1_OPCODE_UPDATE_VERIFY_RESP;
    resp.node_id = g_node_id;
    resp.status = 0;  // 0 = OK
    resp.calculated_crc32 = calculated_crc;
    resp.chunks_received = (uint16_t)(g_bytes_received / 512);  // Estimate
    resp.chunks_missing = 0;
    
    // Send response (12 bytes = 6 words)
    uint16_t words[6];
    memcpy(words, &resp, sizeof(resp));
    z1_broker_send_command(words, 6, CONTROLLER_NODE_ID, STREAM_NODE_MGMT);
    
    printf("[OTA] Sent VERIFY_RESP: CRC=0x%08lX\n", calculated_crc);
}

/**
 * Handle UPDATE_FINALIZE command
 * Validates header, checks CRC32, programs flash
 */
void ota_handle_finalize(void) {
    if (!g_ota_mode) {
        printf("[OTA] ERROR: Finalize received but not in OTA mode\n");
        return;
    }
    
    printf("[OTA] Finalizing update (%lu bytes received)\n", g_bytes_received);
    g_ota_state = OTA_STATE_VALIDATING;
    
    // Validate minimum size (header + some data)
    if (g_bytes_received < sizeof(app_header_t) + 256) {
        printf("[OTA] ERROR: File too small (%lu bytes)\n", g_bytes_received);
        send_ota_error(OTA_ERROR_INVALID_SIZE);
        return;
    }
    
    // Read header from SRAM buffer
    app_header_t header;
    memcpy(&header, g_sram_ota_buffer, sizeof(app_header_t));
    
    // Validate magic number
    if (header.magic != 0x5A314150) {
        printf("[OTA] ERROR: Invalid magic 0x%08lX (expected 0x5A314150)\n", header.magic);
        send_ota_error(OTA_ERROR_INVALID_MAGIC);
        return;
    }
    
    // Validate binary size
    if (header.binary_size == 0 || header.binary_size > APP_PARTITION_SIZE) {
        printf("[OTA] ERROR: Invalid binary size %lu bytes\n", header.binary_size);
        send_ota_error(OTA_ERROR_INVALID_SIZE);
        return;
    }
    
    // Validate total package size
    uint32_t expected_total = sizeof(app_header_t) + header.binary_size;
    if (g_bytes_received < expected_total) {
        printf("[OTA] ERROR: Incomplete package (%lu bytes, expected %lu)\n",
               g_bytes_received, expected_total);
        send_ota_error(OTA_ERROR_INVALID_SIZE);
        return;
    }
    
    printf("[OTA] Header valid: %s v%lu.%lu.%lu\n",
           header.name, header.version_major, header.version_minor, header.version_patch);
    printf("[OTA] Binary size: %lu bytes, CRC32: 0x%08lX\n",
           header.binary_size, header.crc32);
    
    // Calculate CRC32 of binary data (skip header) - from SRAM
    printf("[OTA] Calculating CRC32 from SRAM...\n");
    uint8_t* binary_start = g_sram_ota_buffer + sizeof(app_header_t);
    uint32_t calculated_crc = calculate_crc32(binary_start, header.binary_size);
    
    if (calculated_crc != header.crc32) {
        printf("[OTA] ERROR: CRC32 mismatch (calc=0x%08lX, stored=0x%08lX)\n",
               calculated_crc, header.crc32);
        send_ota_error(OTA_ERROR_CRC_MISMATCH);
        return;
    }
    
    printf("[OTA] CRC32 valid ✓\n");
    
    // Program flash
    g_ota_state = OTA_STATE_PROGRAMMING;
    printf("[OTA] Erasing app partition (0x%08X, %d MB)...\n",
           APP_PARTITION_OFFSET, APP_PARTITION_SIZE / (1024*1024));
    
    // Disable interrupts during flash operations
    uint32_t ints = save_and_disable_interrupts();
    
    // Erase app partition
    flash_range_erase(APP_PARTITION_OFFSET, APP_PARTITION_SIZE);
    
    printf("[OTA] Programming %lu bytes from SRAM to flash...\n", g_bytes_received);
    
    // Program in 4KB pages (flash_range_program requirement)
    uint32_t bytes_to_program = ((g_bytes_received + 4095) / 4096) * 4096;  // Round up to 4KB
    
    for (uint32_t offset = 0; offset < bytes_to_program; offset += 4096) {
        uint32_t remaining = g_bytes_received - offset;
        uint32_t chunk = (remaining > 4096) ? 4096 : remaining;
        
        // Read 4KB from SRAM buffer (not PSRAM!)
        uint8_t page_buffer[4096];
        memcpy(page_buffer, g_sram_ota_buffer + offset, 4096);
        
        // Program to flash
        flash_range_program(APP_PARTITION_OFFSET + offset, page_buffer, 4096);
        
        if ((offset / 4096) % 64 == 0) {
            printf("  Programmed %lu / %lu KB\n", offset / 1024, bytes_to_program / 1024);
        }
    }
    
    // Re-enable interrupts
    restore_interrupts(ints);
    
    printf("[OTA] Flash programming complete ✓\n");
    
    // CRITICAL: Verify flash contents match SRAM
    printf("[OTA] Verifying flash contents...\n");
    const uint8_t* flash_ptr = (const uint8_t*)(XIP_BASE + APP_PARTITION_OFFSET);
    
    // Check header (first 8 bytes at offset 0)
    printf("[OTA-VERIFY] SRAM header [0]: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           g_sram_ota_buffer[0], g_sram_ota_buffer[1], g_sram_ota_buffer[2], g_sram_ota_buffer[3],
           g_sram_ota_buffer[4], g_sram_ota_buffer[5], g_sram_ota_buffer[6], g_sram_ota_buffer[7]);
    printf("[OTA-VERIFY] Flash header [0]: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           flash_ptr[0], flash_ptr[1], flash_ptr[2], flash_ptr[3],
           flash_ptr[4], flash_ptr[5], flash_ptr[6], flash_ptr[7]);
    
    // Check binary start (first 8 bytes at offset 192 - should be vector table!)
    printf("[OTA-VERIFY] SRAM binary [192]: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           g_sram_ota_buffer[192], g_sram_ota_buffer[193], g_sram_ota_buffer[194], g_sram_ota_buffer[195],
           g_sram_ota_buffer[196], g_sram_ota_buffer[197], g_sram_ota_buffer[198], g_sram_ota_buffer[199]);
    printf("[OTA-VERIFY] Flash binary [192]: %02X %02X %02X %02X %02X %02X %02X %02X\n",
           flash_ptr[192], flash_ptr[193], flash_ptr[194], flash_ptr[195],
           flash_ptr[196], flash_ptr[197], flash_ptr[198], flash_ptr[199]);
    
    // Verify entire content
    bool verify_ok = true;
    for (uint32_t i = 0; i < g_bytes_received; i++) {
        if (flash_ptr[i] != g_sram_ota_buffer[i]) {
            printf("[OTA] ERROR: Flash verification failed at offset %lu (flash=0x%02X, sram=0x%02X)\n",
                   i, flash_ptr[i], g_sram_ota_buffer[i]);
            verify_ok = false;
            break;
        }
    }
    
    if (!verify_ok) {
        printf("[OTA] Flash verification FAILED!\n");
        send_ota_error(OTA_ERROR_FLASH_ERROR);
        return;
    }
    
    printf("[OTA] Flash verification complete ✓\n");
    
    // Send success ACK (using COMMIT_RESP to indicate flash complete)
    uint16_t ack = Z1_OPCODE_UPDATE_COMMIT_RESP;
    z1_broker_send_command(&ack, 1, CONTROLLER_NODE_ID, STREAM_NODE_MGMT);
    
    g_ota_state = OTA_STATE_COMPLETE;
    printf("[OTA] Update successful! Rebooting in 2 seconds...\n");
    
    // Give time for ACK to transmit
    sleep_ms(2000);
    
    // Trigger automatic reboot using hardware watchdog
    printf("[OTA] Triggering reboot...\n");
    hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
    
    while(1) tight_loop_contents();  // Never returns
}

/**
 * Handle UPDATE_MODE_EXIT command
 */
void ota_handle_exit_update_mode(void) {
    printf("[OTA] Exiting update mode\n");
    
    if (g_ota_state == OTA_STATE_COMPLETE) {
        printf("[OTA] Update successful - rebooting in 1 second...\n");
        sleep_ms(1000);
        
        // Use hardware watchdog trigger to preserve scratch registers
        hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
        
        while(1) tight_loop_contents();  // Never returns
    } else {
        printf("[OTA] Update incomplete (state=%d) - staying in bootloader\n", g_ota_state);
        g_ota_mode = false;
        g_ota_state = OTA_STATE_IDLE;
    }
}

/**
 * Check if in OTA mode
 */
bool ota_is_active(void) {
    return g_ota_mode;
}

/**
 * Get current OTA state
 */
uint8_t ota_get_state(void) {
    return (uint8_t)g_ota_state;
}
