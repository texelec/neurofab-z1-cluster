/**
 * Z1 Onyx Node - SNN Execution Node
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Runs distributed spiking neural network on RP2350B hardware.
 * Handles spike processing, neuron updates, and bus communication.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/padsbank0.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "node_pins.h"
#include "../common/z1_onyx_bus/z1_bus.h"
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include "../common/psram/psram_rp2350.h"
#include "z1_snn_engine.h"

#define BUS_CLOCK_MHZ  10.0f
#define CONTROLLER_ID  16

static uint8_t my_node_id = 0xFF;

// LED state tracking (for dynamic status reporting)
static struct {
    uint8_t red;    // 0-255 brightness
    uint8_t green;  // 0-255 brightness
    uint8_t blue;   // 0-255 brightness
} g_led_state = {0, 0, 0};

// Persistent node ID (survives watchdog reset) - use hardware scratch register
#define NODE_ID_MAGIC   0xDEADBEEF
#define SCRATCH_NODE_ID_REG  4
#define NODE_ID_MAGIC_SHIFTED  0xDEADBE00

// OTA firmware update state
static volatile bool update_mode_active = false;

typedef struct {
    bool active;                    // Update session active
    uint32_t firmware_size;         // Total firmware size (bytes)
    uint32_t expected_crc32;        // Expected CRC32
    uint16_t chunk_size;            // Chunk size (bytes)
    uint16_t total_chunks;          // Total chunks expected
    uint16_t chunks_received;       // Chunks received count
    uint32_t chunks_bitmap[(4096+31)/32];  // Bitmap of received chunks (max 4096)
    uint8_t* firmware_buffer;       // PSRAM buffer for firmware
} node_ota_state_t;

static node_ota_state_t g_ota_state = {0};

// CRC32 table for firmware verification
static uint32_t crc32_table[256];
static bool crc32_initialized = false;

// Initialize CRC32 lookup table (IEEE 802.3)
static void init_crc32_table(void) {
    if (crc32_initialized) return;
    
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
    crc32_initialized = true;
}

// Calculate CRC32 of data buffer
static uint32_t calculate_crc32(const uint8_t* data, uint32_t length) {
    if (!crc32_initialized) init_crc32_table();
    
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < length; i++) {
        uint8_t index = (crc ^ data[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }
    return ~crc;
}

/**
 * Read raw GPIO pad value (workaround for SDK gpio_get() bug on GPIO 40-44)
 */
static inline bool raw_pad_value(uint8_t pin) {
    return (io_bank0_hw->io[pin].status >> 26) & 0x1;
}

/**
 * Force disable pull-up and pull-down resistors
 */
static inline void force_disable_pulls(uint8_t pin) {
    pads_bank0_hw->io[pin] &= ~(1 << 2);  // Clear PUE
    pads_bank0_hw->io[pin] &= ~(1 << 1);  // Clear PDE
}

/**
 * Read node ID from hardware pins (GPIO 40-43, 4-bit) or use hardcoded value
 */
static uint8_t read_node_id(void) {
#ifdef NODE_ID_HARDCODED
    // V1 hardware: Use compile-time hardcoded node ID
    printf("[Node ID Detection] Using hardcoded ID: %d\n", NODE_ID_HARDCODED);
    return NODE_ID_HARDCODED;
#else
    // V2 hardware: Read from GPIO pins
    gpio_init(NODE_ID_PIN0);
    gpio_init(NODE_ID_PIN1);
    gpio_init(NODE_ID_PIN2);
    gpio_init(NODE_ID_PIN3);
    
    gpio_set_dir(NODE_ID_PIN0, GPIO_IN);
    gpio_set_dir(NODE_ID_PIN1, GPIO_IN);
    gpio_set_dir(NODE_ID_PIN2, GPIO_IN);
    gpio_set_dir(NODE_ID_PIN3, GPIO_IN);
    
    force_disable_pulls(NODE_ID_PIN0);
    force_disable_pulls(NODE_ID_PIN1);
    force_disable_pulls(NODE_ID_PIN2);
    force_disable_pulls(NODE_ID_PIN3);
    
    sleep_ms(1);
    
    uint8_t id = 0;
    bool pin0 = raw_pad_value(NODE_ID_PIN0);
    bool pin1 = raw_pad_value(NODE_ID_PIN1);
    bool pin2 = raw_pad_value(NODE_ID_PIN2);
    bool pin3 = raw_pad_value(NODE_ID_PIN3);
    
    if (pin0) id |= (1 << 0);
    if (pin1) id |= (1 << 1);
    if (pin2) id |= (1 << 2);
    if (pin3) id |= (1 << 3);
    
    printf("[Node ID Detection] GPIO 40-43: %d %d %d %d = ID %d\n",
           pin0, pin1, pin2, pin3, id);
    
    return id;
#endif
}

/**
 * Initialize LED PWM for brightness control
 * Sets up PWM on LED pins for smooth brightness adjustment
 */
static void init_led_pwm(void) {
    // Configure LED pins for PWM output
    gpio_set_function(LED_RED_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_GREEN_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_BLUE_PIN, GPIO_FUNC_PWM);
    
    // Get PWM slices for each LED
    uint slice_red = pwm_gpio_to_slice_num(LED_RED_PIN);
    uint slice_green = pwm_gpio_to_slice_num(LED_GREEN_PIN);
    uint slice_blue = pwm_gpio_to_slice_num(LED_BLUE_PIN);
    
    // Set PWM frequency to ~1kHz (fast enough to avoid flicker)
    // Clock divider: 125MHz / (wrap+1) / divider = frequency
    // 125MHz / 256 / 488 ≈ 1kHz
    pwm_set_clkdiv(slice_red, 488.0f);
    pwm_set_clkdiv(slice_green, 488.0f);
    pwm_set_clkdiv(slice_blue, 488.0f);
    
    pwm_set_wrap(slice_red, 255);    // 8-bit resolution (0-255)
    pwm_set_wrap(slice_green, 255);
    pwm_set_wrap(slice_blue, 255);
    
    // Start PWM
    pwm_set_enabled(slice_red, true);
    pwm_set_enabled(slice_green, true);
    pwm_set_enabled(slice_blue, true);
}

/**
 * Set LED brightness with PWM (0-255)
 */
static inline void led_set(uint8_t pin, uint8_t brightness) {
    uint slice = pwm_gpio_to_slice_num(pin);
    uint channel = pwm_gpio_to_channel(pin);
    pwm_set_chan_level(slice, channel, brightness);
    
    // Update global state for status reporting
    if (pin == LED_RED_PIN) g_led_state.red = brightness;
    else if (pin == LED_GREEN_PIN) g_led_state.green = brightness;
    else if (pin == LED_BLUE_PIN) g_led_state.blue = brightness;
}

// Frame buffer - static at file scope to avoid initialization hang (1220 bytes)
static z1_frame_t g_frame_buffer;

static void init_system(void) {
#ifdef APP_PARTITION_MODE
    // Running from bootloader - hardware already initialized
    my_node_id = read_node_id();
    
    // Enable watchdog with long timeout for OTA resets
    // MUST be enabled before we can trigger it, but we'll update it regularly
    watchdog_enable(8000, false);  // 8 seconds, don't pause on debug
    
    printf("\n[APP] Node %d ready\n", my_node_id);
    
    // Initialize LED PWM
    init_led_pwm();
    
    // Turn off red LED (was on in bootloader)
    led_set(LED_RED_PIN, 0);
    
    // Turn on green LED at 12.5% brightness to indicate app running
    led_set(LED_GREEN_PIN, 32);
    
    // CRITICAL: Re-initialize bus to point DMA at APP's buffer addresses
    // Bootloader initialized bus with ITS buffer addresses, but app has separate copies
    // at different memory locations (0x00080000+ vs 0x00000000+)
    printf("[APP] Re-initializing bus for app memory space...\n");
    z1_bus_init_node();
    z1_bus_set_node_id(my_node_id);
    z1_bus_set_speed_mhz(BUS_CLOCK_MHZ);
    
    // Re-initialize broker for app memory space
    printf("[APP] Re-initializing broker for app memory space...\n");
    z1_broker_init();
    
    // PSRAM already initialized by bootloader - hardware state persists
    // Mark it as initialized so psram_write() and psram_read() work
    psram_mark_initialized(8 * 1024 * 1024);  // 8MB
    
    // SNN Engine
    printf("[APP] Initializing SNN engine...\n");
    z1_snn_init(my_node_id);
    
    printf("[APP] Initialization complete\n\n");
#else
    // Monolithic mode - initialize everything
    // Initialize USB serial FIRST so we can see any crashes
    stdio_init_all();
    sleep_ms(1000);  // Give USB time to enumerate
    
    printf("\n\n");
    printf("========================================\n");
    printf("Z1 ONYX NODE APP STARTING\n");
    printf("========================================\n");
    
    // Now configure voltage and clock
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(266000, true);
    sleep_ms(100);
    
    my_node_id = read_node_id();
    
    printf("\n========================================\n");
    printf("Z1 Onyx Node - SNN Execution Node\n");
    printf("========================================\n");
    printf("Node ID: %d\n", my_node_id);
    printf("Bus Speed: %.1f MHz\n\n", BUS_CLOCK_MHZ);
    
    // Initialize LED PWM
    init_led_pwm();
    
    // Turn off red LED (was on in bootloader), turn on green at 12.5%
    led_set(LED_RED_PIN, 0);
    led_set(LED_GREEN_PIN, 32);
    
    // Bus layer
    printf("[Node] Initializing bus @ %.1f MHz...\n", BUS_CLOCK_MHZ);
    z1_bus_init_node();
    z1_bus_set_node_id(my_node_id);
    z1_bus_set_speed_mhz(BUS_CLOCK_MHZ);
    
    // Broker layer
    printf("[Node] Initializing broker...\n");
    z1_broker_init();
    
    // PSRAM (133 MHz)
    printf("[Node] Initializing PSRAM @ 133 MHz...\n");
    psram_init();
    
    // SNN Engine
    printf("[Node] Initializing SNN engine...\n");
    z1_snn_init(my_node_id);
    
    printf("[Node] Ready\n\n");
#endif
}

/**
 * Handle command frames (CTRL stream)
 */
static void handle_command_frame(z1_frame_t* frame) {
    if (frame->length < 2) return;  // Need at least opcode
    
    uint16_t opcode = frame->payload[0];
    // printf("[Node %d] *** FRAME RECEIVED ***: type=%d, src=%d, dest=%d, len=%d\n", my_node_id, frame->frame_type, frame->src, frame->dest, frame->length);
    
    switch (opcode) {
        case OPCODE_PING: {
            printf("[CMD] PING from node %d\n", frame->src);
            
            // Respond with PONG
            uint16_t pong = OPCODE_PONG;
            z1_broker_send_command(&pong, 1, frame->src, STREAM_NODE_MGMT);
            break;
        }
        
        case OPCODE_RESET_TO_BOOTLOADER: {
            printf("[CMD] RESET_TO_BOOTLOADER from node %d\n", frame->src);
            printf("[RESET] Rebooting into bootloader in 100ms...\n");
            
            // Send ACK before reset
            uint16_t ack = OPCODE_RESET_TO_BOOTLOADER | 0x8000;
            z1_broker_send_command(&ack, 1, frame->src, STREAM_NODE_MGMT);
            
            // Store node ID in hardware scratch register (survives watchdog reset)
            uint32_t scratch_value = NODE_ID_MAGIC_SHIFTED | (uint32_t)my_node_id;
            printf("[RESET] Writing scratch[%d] = 0x%08lX (magic=0x%08lX, id=%d)\n", 
                   SCRATCH_NODE_ID_REG, scratch_value, NODE_ID_MAGIC_SHIFTED, my_node_id);
            watchdog_hw->scratch[SCRATCH_NODE_ID_REG] = scratch_value;
            printf("[RESET] Verify read back: 0x%08lX\n", watchdog_hw->scratch[SCRATCH_NODE_ID_REG]);
            
            // Give time for ACK to transmit
            sleep_ms(100);
            
            // Trigger watchdog reset (watchdog already enabled at startup)
            // This preserves scratch registers
            hw_set_bits(&watchdog_hw->ctrl, WATCHDOG_CTRL_TRIGGER_BITS);
            
            // Never returns
            while(1) tight_loop_contents();
        }
        
        case OPCODE_READ_STATUS: {
            printf("[CMD] READ_STATUS from node %d\n", frame->src);
            
            // Build status response (11 words = 22 bytes)
            // Format: [opcode, node_id, uptime_ms(2), memory_free(2), led_r, led_g, led_b, snn_running, neuron_count]
            // This matches the format expected by controller's handle_get_nodes()
            uint16_t response[32];  // 64 bytes max
            response[0] = OPCODE_STATUS_RESPONSE;
            response[1] = my_node_id;
            
            // Uptime in milliseconds (split into two 16-bit words)
            uint32_t uptime_ms = time_us_32() / 1000;
            response[2] = (uint16_t)(uptime_ms & 0xFFFF);
            response[3] = (uint16_t)(uptime_ms >> 16);
            
            // Memory free (8MB PSRAM total)
            // NOTE: Dynamic memory tracking not yet implemented - reports total PSRAM size
            // Real usage depends on SNN topology (neurons, weights stored in PSRAM)
            // Future enhancement: Add allocation tracking to psram_rp2350.c
            uint32_t memory_free = 8 * 1024 * 1024;  // 8 MB in bytes
            response[4] = (uint16_t)(memory_free & 0xFFFF);
            response[5] = (uint16_t)(memory_free >> 16);
            
            // LED state (R, G, B as separate words, 0-255 brightness)
            response[6] = g_led_state.red;
            response[7] = g_led_state.green;
            response[8] = g_led_state.blue;
            
            // SNN state
            response[9] = z1_snn_is_running() ? 1 : 0;
            response[10] = z1_snn_get_neuron_count();
            
            // Send response (11 words = 22 bytes)
            z1_broker_send_command(response, 11, frame->src, STREAM_NODE_MGMT);
            break;
        }
        
        case OPCODE_START_SNN: {
            printf("[CMD] START_SNN from node %d\n", frame->src);
            z1_snn_start();
            
            // Send ACK
            uint16_t ack = OPCODE_START_SNN | 0x8000;
            z1_broker_send_command(&ack, 1, frame->src, 0);
            break;
        }
        
        case OPCODE_STOP_SNN: {
            printf("[CMD] STOP_SNN from node %d\n", frame->src);
            z1_snn_stop();
            
            // Send ACK
            uint16_t ack = OPCODE_STOP_SNN | 0x8000;
            z1_broker_send_command(&ack, 1, frame->src, 0);
            break;
        }
        
        case OPCODE_PAUSE_SNN: {
            printf("[CMD] PAUSE_SNN from node %d\n", frame->src);
            z1_snn_pause();
            
            // Send ACK
            uint16_t ack = OPCODE_PAUSE_SNN | 0x8000;
            z1_broker_send_command(&ack, 1, frame->src, 0);
            break;
        }
        
        case OPCODE_RESUME_SNN: {
            printf("[CMD] RESUME_SNN from node %d\n", frame->src);
            z1_snn_resume();
            
            // Send ACK
            uint16_t ack = OPCODE_RESUME_SNN | 0x8000;
            z1_broker_send_command(&ack, 1, frame->src, 0);
            break;
        }
        
        case OPCODE_INJECT_SPIKE_BATCH: {
            // Format: [OPCODE, count, neuron_id_low, neuron_id_high, ...]
            uint16_t spike_count = frame->payload[1];
            printf("[CMD] INJECT_SPIKE_BATCH: %d spikes from node %d\n", spike_count, frame->src);
            
            for (uint16_t i = 0; i < spike_count; i++) {
                uint16_t neuron_id_low = frame->payload[2 + (i * 2)];
                uint16_t neuron_id_high = frame->payload[2 + (i * 2) + 1];
                
                // Decode spike
                z1_spike_t spike;
                spike.neuron_id = neuron_id_low | ((uint32_t)neuron_id_high << 16);
                spike.value = 1.0f;  // Default spike value
                
                // Inject into SNN engine
                z1_snn_inject_spike(spike);
            }
            break;
        }
        
        case OPCODE_GET_SNN_STATUS: {
            printf("[CMD] GET_SNN_STATUS from node %d\n", frame->src);
            
            // Build SNN status response (8 words = 16 bytes)
            // Format: [opcode, running, neuron_count, active_neurons, total_spikes(2 words), spike_rate(2 words)]
            // Used by controller's handle_global_snn_status() for cluster-wide statistics
            uint16_t response[8];
            response[0] = OPCODE_SNN_STATUS;  // Use response opcode, not request|0x8000
            response[1] = z1_snn_is_running() ? 1 : 0;
            response[2] = z1_snn_get_neuron_count();
            
            // Get statistics
            z1_snn_stats_t stats;
            z1_snn_get_stats(&stats);
            
            // Calculate metrics
            uint16_t active_neurons = z1_snn_get_neuron_count();  // All loaded neurons are "active"
            uint32_t total_spikes = stats.spikes_received + stats.spikes_generated;
            
            // Calculate spike rate (spikes per second)
            uint32_t current_time = z1_snn_get_current_time();
            uint32_t spike_rate_hz = 0;
            if (current_time > 0) {
                spike_rate_hz = (total_spikes * 1000000) / current_time;  // Convert us to Hz
            }
            
            response[3] = active_neurons;
            memcpy(&response[4], &total_spikes, 4);   // words 4-5: total_spikes (32-bit)
            memcpy(&response[6], &spike_rate_hz, 4);  // words 6-7: spike_rate_hz (32-bit)
            
            printf("[SNN] Status: running=%d, neurons=%u, total_spikes=%lu, rate=%lu Hz\n",
                   response[1], response[2], (unsigned long)total_spikes, (unsigned long)spike_rate_hz);
            
            z1_broker_send_command(response, 8, frame->src, STREAM_SNN_CONTROL);
            break;
        }
        
        case OPCODE_WRITE_MEMORY: {
            // Write binary data to PSRAM address
            // Frame format (words): [opcode, length, addr_low, addr_high, reserved(2), data...]
            // Total frame length in bytes = header(12 bytes) + data
            // Used by controller to deploy neuron tables and configuration
            if (frame->length < 14) break;  // Need header + at least 2 bytes data
            
            uint16_t length = frame->payload[1];
            uint32_t addr = ((uint32_t)frame->payload[2] | ((uint32_t)frame->payload[3] << 16));
            
            // printf("[CMD] WRITE_MEMORY addr=0x%08lX len=%d from node %d\n", addr, length, frame->src);
            
            // Data starts at payload[6] (12 bytes header)
            uint8_t* data_ptr = (uint8_t*)&frame->payload[6];
            
            // frame->length is in BYTES, payload is in WORDS
            // Header is 6 words (12 bytes), then data follows
            uint16_t header_bytes = 12;
            uint16_t expected_frame_bytes = header_bytes + length;
            
            // Verify we have enough data in frame
            if (frame->length >= expected_frame_bytes) {
                // Write to PSRAM (debug logging disabled for performance)
                psram_write(addr, data_ptr, length);
                
                // Send ACK
                uint16_t ack = OPCODE_WRITE_ACK;
                z1_broker_send_command(&ack, 1, frame->src, STREAM_MEMORY);
            } else {
                printf("  ERROR: Frame too short (%d bytes, need %d bytes)\n", 
                       frame->length, expected_frame_bytes);
            }
            break;
        }
        
        case OPCODE_DEPLOY_TOPOLOGY: {
            // Load neuron topology from PSRAM into active SNN engine
            // Frame format: [opcode, neuron_count, reserved...]
            // Assumes neuron table already written to PSRAM via WRITE_MEMORY at 0x00100000
            // Parses 256-byte neuron entries and builds runtime structures
            if (frame->length < 2) break;
            
            uint16_t neuron_count = frame->payload[1];
            printf("[CMD] DEPLOY_TOPOLOGY count=%d from node %d\n", neuron_count, frame->src);
            
            // Load neurons from PSRAM (assumes already written by WRITE_MEMORY)
            z1_snn_load_topology_from_psram();
            printf("  Loaded neurons from PSRAM\n");
            
            // Send ACK
            uint16_t ack = OPCODE_DEPLOY_ACK;
            z1_broker_send_command(&ack, 1, frame->src, STREAM_SNN_CONFIG);
            break;
        }
        
        // =====================================================================
        // OTA Firmware Update Commands
        // =====================================================================
        
        case Z1_OPCODE_UPDATE_MODE_ENTER: {
            printf("[UPDATE] Entering update mode (broadcast from node %d)\n", frame->src);
            update_mode_active = true;
            
            // Stop SNN processing to free resources
            if (z1_snn_is_running()) {
                printf("[UPDATE] Stopping SNN engine\n");
                z1_snn_stop();
            }
            
            // NOTE: PSRAM buffer and chunk bitmap allocated in UPDATE_START
            break;
        }
        
        case Z1_OPCODE_UPDATE_MODE_EXIT: {
            printf("[UPDATE] Exiting update mode (broadcast from node %d)\n", frame->src);
            update_mode_active = false;
            
            // Clear OTA state (PSRAM buffer freed by next allocation)
            g_ota_state.active = false;
            break;
        }
        
        case Z1_OPCODE_UPDATE_START: {
            printf("[UPDATE] UPDATE_START received from node %d\n", frame->src);
            
            if (frame->length < sizeof(z1_update_start_t)/2) {
                printf("[UPDATE] ERROR: Frame too short for UPDATE_START\n");
                break;
            }
            
            z1_update_start_t* cmd = (z1_update_start_t*)frame->payload;
            
            // Only respond if targeted at us
            if (cmd->target_node_id != my_node_id) {
                printf("[UPDATE] Not for us (target=%d, we=%d)\n", cmd->target_node_id, my_node_id);
                break;
            }
            
            printf("[UPDATE] Firmware size=%lu, CRC=0x%08lX, chunks=%d\n",
                   cmd->total_size, cmd->expected_crc32, cmd->total_chunks);
            
            // Allocate PSRAM buffer
            // PSRAM layout: 0x11000000-0x11FFFFFF (16MB available)
            // OTA buffer: 0x11010000 (start after 64KB reserved for other use)
            g_ota_state.firmware_buffer = (uint8_t*)0x11010000;
            g_ota_state.firmware_size = cmd->total_size;
            g_ota_state.expected_crc32 = cmd->expected_crc32;
            g_ota_state.chunk_size = cmd->chunk_size;
            g_ota_state.total_chunks = cmd->total_chunks;
            g_ota_state.chunks_received = 0;
            memset(g_ota_state.chunks_bitmap, 0, sizeof(g_ota_state.chunks_bitmap));
            g_ota_state.active = true;
            
            // Send READY response
            z1_update_ready_t resp;
            resp.opcode = Z1_OPCODE_UPDATE_READY;
            resp.node_id = my_node_id;
            resp.status = 0;  // 0 = ready
            resp.available_psram = 8 * 1024 * 1024;  // Report 8MB available
            
            // Copy to aligned buffer to avoid packed struct alignment warning
            uint16_t aligned_resp[4];  // sizeof(z1_update_ready_t) = 8 bytes = 4 words
            memcpy(aligned_resp, &resp, sizeof(resp));
            
            z1_broker_send_command(aligned_resp, sizeof(resp)/2, frame->src, STREAM_NODE_MGMT);
            printf("[UPDATE] Sent READY response\n");
            break;
        }
        
        case Z1_OPCODE_UPDATE_DATA_CHUNK: {
            if (!g_ota_state.active) {
                printf("[UPDATE] ERROR: No active update session\n");
                break;
            }
            
            if (frame->length < 4) {
                printf("[UPDATE] ERROR: Frame too short for DATA_CHUNK\n");
                break;
            }
            
            z1_update_data_chunk_t* hdr = (z1_update_data_chunk_t*)frame->payload;
            
            // Only accept if targeted at us
            if (hdr->target_node_id != my_node_id) {
                break;
            }
            
            uint16_t chunk_num = hdr->chunk_num;
            uint16_t data_size = hdr->data_size;
            
            if (chunk_num >= g_ota_state.total_chunks) {
                printf("[UPDATE] ERROR: Invalid chunk_num %d (max %d)\n",
                       chunk_num, g_ota_state.total_chunks - 1);
                break;
            }
            
            // Data starts after 4-word header
            uint8_t* chunk_data = (uint8_t*)&frame->payload[4];
            
            // Write to PSRAM
            uint32_t offset = chunk_num * g_ota_state.chunk_size;
            memcpy(g_ota_state.firmware_buffer + offset, chunk_data, data_size);
            
            // Mark chunk as received
            uint32_t word = chunk_num / 32;
            uint32_t bit = chunk_num % 32;
            g_ota_state.chunks_bitmap[word] |= (1UL << bit);
            g_ota_state.chunks_received++;
            
            printf("[UPDATE] Chunk %d received (%d bytes) - %d/%d complete\n",
                   chunk_num, data_size, g_ota_state.chunks_received, g_ota_state.total_chunks);
            
            // Send ACK
            uint16_t ack_frame[2];
            ack_frame[0] = Z1_OPCODE_UPDATE_ACK_CHUNK;
            ack_frame[1] = chunk_num;
            
            z1_broker_send_command(ack_frame, 2, frame->src, STREAM_NODE_MGMT);
            break;
        }
        
        case Z1_OPCODE_UPDATE_POLL: {
            // Check if this poll is for us
            if (frame->length >= 4) {
                z1_update_poll_t* poll = (z1_update_poll_t*)frame->payload;
                if (poll->poll_node_id == my_node_id) {
                    printf("[UPDATE] POLL for node %d, type=%d\n", my_node_id, poll->poll_type);
                    
                    if (poll->poll_type == Z1_POLL_TYPE_STATUS) {
                        // Send status response
                        z1_update_ready_t resp;
                        resp.opcode = Z1_OPCODE_UPDATE_READY;
                        resp.node_id = my_node_id;
                        resp.status = g_ota_state.active ? 0 : 1;
                        resp.available_psram = 8 * 1024 * 1024;
                        
                        // Copy to aligned buffer to avoid packed struct alignment warning
                        uint16_t aligned_resp[4];  // sizeof(z1_update_ready_t) = 8 bytes = 4 words
                        memcpy(aligned_resp, &resp, sizeof(resp));
                        
                        z1_broker_send_command(aligned_resp, sizeof(resp)/2,
                                                frame->src, STREAM_NODE_MGMT);
                    }
                    else if (poll->poll_type == Z1_POLL_TYPE_VERIFY) {
                        // Calculate CRC32 and verify
                        printf("[UPDATE] Calculating CRC32 of %lu bytes...\n", g_ota_state.firmware_size);
                        uint32_t computed_crc = calculate_crc32(g_ota_state.firmware_buffer,
                                                                 g_ota_state.firmware_size);
                        bool match = (computed_crc == g_ota_state.expected_crc32);
                        
                        printf("[UPDATE] CRC32: computed=0x%08lX, expected=0x%08lX, %s\n",
                               computed_crc, g_ota_state.expected_crc32,
                               match ? "PASS" : "FAIL");
                        
                        // Send verify response
                        uint16_t resp[4];
                        resp[0] = Z1_OPCODE_UPDATE_VERIFY_RESP;
                        resp[1] = match ? 0 : 1;  // 0=OK, 1=CRC_FAIL
                        resp[2] = (uint16_t)(computed_crc & 0xFFFF);
                        resp[3] = (uint16_t)(computed_crc >> 16);
                        
                        z1_broker_send_command(resp, 4, frame->src, STREAM_NODE_MGMT);
                    }
                }
            }
            break;
        }
        
        case Z1_OPCODE_UPDATE_COMMIT: {
            printf("[UPDATE] COMMIT command received - flashing firmware\n");
            
            if (!g_ota_state.active) {
                printf("[UPDATE] ERROR: No active update session\n");
                break;
            }
            
            // Application partition starts at 0x00080000 (512KB offset from flash base)
            const uint32_t APP_PARTITION_OFFSET = 0x00080000;
            const uint32_t APP_FLASH_SECTOR_SIZE = 4096;
            const uint32_t APP_FLASH_PAGE_SIZE = 256;
            
            bool flash_ok = true;
            
            // Calculate number of sectors to erase (round up)
            uint32_t sectors_needed = (g_ota_state.firmware_size + APP_FLASH_SECTOR_SIZE - 1) / APP_FLASH_SECTOR_SIZE;
            uint32_t erase_size = sectors_needed * APP_FLASH_SECTOR_SIZE;
            
            printf("[UPDATE] Erasing %lu bytes (%lu sectors) at offset 0x%08lX...\n",
                   erase_size, sectors_needed, APP_PARTITION_OFFSET);
            
            // Disable interrupts for flash operations (CRITICAL!)
            uint32_t ints = save_and_disable_interrupts();
            
            // Erase flash sectors
            flash_range_erase(APP_PARTITION_OFFSET, erase_size);
            
            printf("[UPDATE] Programming %lu bytes...\n", g_ota_state.firmware_size);
            
            // Program flash in 256-byte pages
            for (uint32_t offset = 0; offset < g_ota_state.firmware_size; offset += APP_FLASH_PAGE_SIZE) {
                uint32_t remaining = g_ota_state.firmware_size - offset;
                uint32_t write_size = (remaining < APP_FLASH_PAGE_SIZE) ? remaining : APP_FLASH_PAGE_SIZE;
                
                // If last page is partial, pad with 0xFF
                static uint8_t page_buffer[256];
                memset(page_buffer, 0xFF, sizeof(page_buffer));
                memcpy(page_buffer, g_ota_state.firmware_buffer + offset, write_size);
                
                flash_range_program(APP_PARTITION_OFFSET + offset, page_buffer, APP_FLASH_PAGE_SIZE);
                
                // Progress indicator every 64KB
                if ((offset % 65536) == 0 && offset > 0) {
                    printf("[UPDATE] Programmed %lu / %lu bytes\n", offset, g_ota_state.firmware_size);
                }
            }
            
            // Restore interrupts
            restore_interrupts(ints);
            
            printf("[UPDATE] Flash programming complete, verifying...\n");
            
            // Verify flash contents by reading back and comparing CRC32
            const uint8_t* flash_data = (const uint8_t*)(XIP_BASE + APP_PARTITION_OFFSET);
            uint32_t verify_crc = calculate_crc32(flash_data, g_ota_state.firmware_size);
            
            if (verify_crc == g_ota_state.expected_crc32) {
                printf("[UPDATE] Flash verification PASSED (CRC32=0x%08lX)\n", verify_crc);
                flash_ok = true;
            } else {
                printf("[UPDATE] Flash verification FAILED! (got 0x%08lX, expected 0x%08lX)\n",
                       verify_crc, g_ota_state.expected_crc32);
                flash_ok = false;
            }
            
            // Send response
            uint16_t resp[2];
            resp[0] = Z1_OPCODE_UPDATE_COMMIT_RESP;
            resp[1] = flash_ok ? 0 : 1;  // 0=success, 1=fail
            
            z1_broker_send_command(resp, 2, frame->src, STREAM_NODE_MGMT);
            
            // Clear OTA state
            if (flash_ok) {
                g_ota_state.active = false;
                printf("[UPDATE] Firmware update complete - ready for restart\n");
            }
            break;
        }
        
        case Z1_OPCODE_UPDATE_RESTART: {
            printf("[UPDATE] RESTART command received - rebooting in 1 second\n");
            sleep_ms(1000);
            
            // Perform watchdog reset (fastest method)
            watchdog_reboot(0, 0, 0);
            
            // Should never reach here
            while (1) {
                tight_loop_contents();
            }
            break;
        }
        
        default:
            printf("[CMD] Unknown opcode 0x%04X from node %d\n", opcode, frame->src);
            break;
    }
}

static void __attribute__((noinline)) idle_node_loop(void) {
    uint32_t loop_count = 0;
    uint32_t last_snn_step_us = 0;
    const uint32_t SNN_TIMESTEP_US = 1000;
    uint32_t heartbeat_cycle_start = 0;
    bool heartbeat_on = false;
    
    // Process frames and handle commands
    while (true) {
        loop_count++;
        if (loop_count % 10000000 == 0) {
            printf("[Node %d] Alive: %luM iterations\n", my_node_id, loop_count / 1000000);
        }
        
        // Update watchdog to prevent timeout (required for OTA reset mechanism)
        #ifdef APP_PARTITION_MODE
        watchdog_update();
        #endif
        
        // Heartbeat: pulse blue LED for 100ms every 3 seconds (20% brightness)
        uint32_t now_ms = time_us_32() / 1000;
        uint32_t elapsed = now_ms - heartbeat_cycle_start;
        
        if (elapsed >= 3000) {
            // Start new 3-second cycle
            heartbeat_cycle_start = now_ms;
            led_set(LED_BLUE_PIN, 51);  // 20% = 51/255
            heartbeat_on = true;
        } else if (heartbeat_on && elapsed >= 100) {
            // Turn off after 100ms
            led_set(LED_BLUE_PIN, 0);
            heartbeat_on = false;
        }
        
        z1_broker_task();
        
        // Check for incoming frames
        if (z1_broker_try_receive(&g_frame_buffer)) {
            printf("[Node %d] FRAME: type=%d, src=%d, dest=%d, stream=%d, len=%d\n", 
                   my_node_id, g_frame_buffer.type, g_frame_buffer.src, g_frame_buffer.dest, 
                   g_frame_buffer.stream, g_frame_buffer.length);
            
            // Handle CTRL frames (commands)
            if (g_frame_buffer.type == Z1_FRAME_TYPE_CTRL) {
                handle_command_frame(&g_frame_buffer);
                
                // CRITICAL: Immediately flush TX queue to send response
                // Without this, SNN processing can block before response is transmitted
                z1_broker_task();
            }
            // Handle UNICAST frames (spike injection from controller)
            else if (g_frame_buffer.type == Z1_FRAME_TYPE_UNICAST) {
                // Inject spike into local SNN engine
                // Payload: [local_neuron_id_low, local_neuron_id_high]
                // Controller sends these via POST /api/snn/input after decoding global neuron IDs
                if (g_frame_buffer.length >= 4) {
                    z1_spike_t spike;
                    spike.neuron_id = (uint32_t)g_frame_buffer.payload[0] | ((uint32_t)g_frame_buffer.payload[1] << 16);
                    spike.timestamp_us = time_us_32();
                    spike.value = 1.0f;  // Default spike value
                    
                    // printf("[Node %d] UNICAST spike received: neuron_id=%lu (0x%08lX)\n", my_node_id, spike.neuron_id, spike.neuron_id);
                    z1_snn_inject_spike(spike);
                } else {
                    printf("[Node %d] ERROR: Spike frame too short (len=%d)\n", my_node_id, g_frame_buffer.length);
                }
            }
            // Handle BROADCAST frames (spikes from other nodes or ourselves)
            else if (g_frame_buffer.type == Z1_FRAME_TYPE_BROADCAST) {
                // CRITICAL: Filter out our OWN broadcast spikes to prevent feedback loop
                // When we fire, we broadcast our spike. All nodes (including us) receive it.
                // If we process our own spike, it re-stimulates the neuron, causing infinite firing.
                // However, we still need to process broadcasts from OTHER nodes for inter-node communication.
                if (g_frame_buffer.src != my_node_id) {
                    // Process broadcast spike into SNN engine (payload is 2 words = 4 bytes)
                    if (g_frame_buffer.length >= 4) {
                        z1_spike_t spike;
                        spike.neuron_id = (uint32_t)g_frame_buffer.payload[0] | ((uint32_t)g_frame_buffer.payload[1] << 16);
                        spike.timestamp_us = time_us_32();
                        spike.value = 1.0f;  // Default spike value
                        
                        // printf("[Node %d] Injecting BROADCAST spike from node %d: neuron_id=%lu\n", my_node_id, g_frame_buffer.src, spike.neuron_id);
                        z1_snn_inject_spike(spike);
                    } else {
                        printf("[Node %d] ERROR: Broadcast spike frame too short (len=%d)\n", my_node_id, g_frame_buffer.length);
                    }
                }
                // Else: Our own broadcast - ignore to prevent feedback loop
            }
        }
        
        // Run SNN timestep if engine is running AND enough time has elapsed
        // Timestep: 1ms (1kHz update rate) - configurable based on network requirements
        if (z1_snn_is_running()) {
            uint32_t now_us = time_us_32();
            if ((now_us - last_snn_step_us) >= SNN_TIMESTEP_US) {
                last_snn_step_us = now_us;
                
                // CRITICAL: Poll for commands BEFORE SNN step
                // This ensures GET_SNN_STATUS and other commands are processed even during heavy spike activity
                z1_broker_task();
                if (z1_broker_try_receive(&g_frame_buffer)) {
                    // Process command immediately (reuse command handler logic)
                    printf("[Node %d] FRAME (during SNN): type=%d, src=%d, dest=%d, stream=%d, len=%d\n", 
                           my_node_id, g_frame_buffer.type, g_frame_buffer.src, g_frame_buffer.dest, 
                           g_frame_buffer.stream, g_frame_buffer.length);
                    
                    if (g_frame_buffer.dest == my_node_id || g_frame_buffer.dest == 31) {
                        if (g_frame_buffer.type == Z1_FRAME_TYPE_CTRL) {
                            handle_command_frame(&g_frame_buffer);
                        }
                    }
                }
                
                // Execute one SNN timestep (only if not paused)
                // Paused state allows stats collection without race conditions
                z1_snn_step();
                
                // Service broker IMMEDIATELY after SNN step (critical for responsiveness)
                z1_broker_task();
                
                // Broadcast output spikes to cluster (ONLY if still running)
                // Other nodes will receive and integrate based on their synaptic connections
                // CRITICAL: Limit broadcasts and yield frequently to maintain responsiveness
                // CRITICAL: Check running state again in case STOP command was received during step
                uint16_t spike_count = 0;
                const z1_spike_t* spikes = NULL;
                if (z1_snn_is_running()) {
                    spikes = z1_snn_get_output_spikes(&spike_count);
                }
                
                // Limit spikes per timestep to prevent blocking
                const uint16_t MAX_BROADCASTS_PER_TIMESTEP = 5;
                if (spike_count > MAX_BROADCASTS_PER_TIMESTEP) {
                    spike_count = MAX_BROADCASTS_PER_TIMESTEP;
                }
                
                for (uint16_t i = 0; i < spike_count; i++) {
                    // Pack spike into bus frame format
                    // Format: [neuron_id_low(16), neuron_id_high(8), value_scaled(16)]
                    uint16_t spike_data[3];
                    spike_data[0] = (uint16_t)(spikes[i].neuron_id & 0xFFFF);
                    spike_data[1] = (uint16_t)((spikes[i].neuron_id >> 16) & 0xFF);
                    spike_data[2] = (uint16_t)(spikes[i].value * 1000.0f);  // Scale float to int
                    
                    // BROADCAST to all nodes (dest=31)
                    // Each node (including this one) will receive and filter based on synaptic connections
                    // Stream 4 = spike stream (high priority)
                    if (!z1_broker_send_spike(spike_data, 3, 31, 4)) {
                        // Queue full - skip remaining spikes this timestep
                        break;
                    }
                    
                    // Yield to broker every spike to maintain responsiveness
                    z1_broker_task();
                }                
                // Service broker again after broadcasts
                z1_broker_task();
            }
        }
        
        // NO SLEEP - keep running at full speed to service bus
        tight_loop_contents();
    }
}

int main(void) {
#ifdef APP_PARTITION_MODE
    // Bootloader disabled interrupts - re-enable them FIRST (before any peripheral access)
    __asm__ volatile ("cpsie i" : : : "memory");
    
    // Enable FPU (required for idle_node_loop which uses floating point)
    // CPACR: Coprocessor Access Control Register
    volatile uint32_t *cpacr = (uint32_t *)0xE000ED88;
    *cpacr |= (0xF << 20);  // Enable CP10 and CP11 (FPU)
    __asm__ volatile ("dsb" : : : "memory");
    __asm__ volatile ("isb" : : : "memory");
    
    // Reinitialize USB stdio with app's vector table
    stdio_usb_init();
    sleep_ms(2000);  // Give USB time to enumerate
#endif
    
    // Now try initialization (stdio already active from bootloader in APP_PARTITION_MODE)
    init_system();
    
    // All nodes run the same idle command handler
    idle_node_loop();  // Never returns
    
    return 0;
}
