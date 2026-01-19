/**
 * Z1 Onyx Bootloader - Main Entry Point
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Fixed bootloader partition that:
 * 1. Initializes Matrix Bus, Broker, PSRAM
 * 2. Validates application partition (magic + CRC32)
 * 3. Jumps to application if valid
 * 4. Enters safe mode (OTA-only) if invalid
 * 
 * Partition Layout:
 * - Bootloader: 0x00000000 - 0x0007FFFF (512KB, this code)
 * - Application: 0x00080000 - 0x007FFFFF (7.5MB, SNN engine)
 * 
 * XIP Memory Map:
 * - Bootloader runs at: 0x10000000
 * - Application runs at: 0x10080000
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "boot/picoboot.h"
#include "boot/picobin.h"  // For PICOBIN_PARTITION_LOCATION_* defines
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/resets.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/systick.h"
#include "pico/platform.h"

// Disable interrupts (CMSIS-style inline)
static inline void __disable_irq(void) {
    __asm volatile ("cpsid i" : : : "memory");
}
#include "z1_bus.h"
#include "z1_broker.h"
#include "z1_commands.h"
#include "psram_rp2350.h"
#include "ota_engine.h"

// Hardware configuration - MUST match node_pins.h (GPIO 44/45/46)
#define LED_GREEN_PIN   44
#define LED_BLUE_PIN    45
#define LED_RED_PIN     46

#define NODE_ID_PIN0    40
#define NODE_ID_PIN1    41
#define NODE_ID_PIN2    42
#define NODE_ID_PIN3    43

// Node ID persistence across watchdog resets using hardware scratch registers
#define NODE_ID_MAGIC   0xDEADBEEF
#define SCRATCH_NODE_ID_REG  4
#define NODE_ID_MAGIC_SHIFTED  0xDEADBE00

// Forward declaration
static void enter_safe_mode(void);

#define BUS_CLOCK_MHZ   8.0f

// Node ID stored in uninitialized RAM (survives watchdog reset)
// Must be at same address in both bootloader and application
#define NODE_ID_MAGIC   0xDEADBEEF
__attribute__((section(".uninitialized_data"))) static uint32_t g_persistent_node_id_magic;
__attribute__((section(".uninitialized_data"))) static uint8_t g_persistent_node_id;

// Partition sizes (physical flash addresses)
#define BOOTLOADER_PARTITION_START  0x00000000
#define BOOTLOADER_PARTITION_SIZE   (2 * 1024 * 1024)  // 2MB
#define APP_PARTITION_START         0x00080000
#define APP_PARTITION_SIZE          (7680 * 1024)  // 7.5MB

// Debug flag - enables delays for serial monitoring
// Set to 0 for fast boot (production), 1 for debug logging delays
#define BOOTLOADER_DEBUG_DELAYS 1

// Bootloader version
#define BOOTLOADER_VERSION_MAJOR 1
#define BOOTLOADER_VERSION_MINOR 0
#define BOOTLOADER_VERSION_PATCH 0

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

// Global node ID (extern for OTA engine)
uint8_t g_node_id = 0;

// CRC32 lookup table (IEEE 802.3)
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

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
 * Calculate CRC32 of data buffer (IEEE 802.3)
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
 * Force disable pull resistors on GPIO pin
 */
static inline void force_disable_pulls(uint pin) {
    hw_write_masked(&pads_bank0_hw->io[pin],
                    0,  // Disable both pulls
                    PADS_BANK0_GPIO0_PUE_BITS | PADS_BANK0_GPIO0_PDE_BITS);
}

/**
 * Read raw pad value without pull resistors
 */
static inline bool raw_pad_value(uint pin) {
    return !!(sio_hw->gpio_in & (1u << pin));
}

/**
 * Read node ID from GPIO pins (hardware strapping)
 */
static uint8_t read_node_id(void) {
    printf("[Node ID Detection] Starting read_node_id()...\n");
    
    // ALWAYS check scratch register first (app writes it before reset)
    printf("[Node ID Detection] Reading scratch register %d...\n", SCRATCH_NODE_ID_REG);
    uint32_t scratch = watchdog_hw->scratch[SCRATCH_NODE_ID_REG];
    printf("[Node ID Detection] Scratch value: 0x%08X\n", scratch);
    
    uint32_t magic = scratch & 0xFFFFFF00;
    uint8_t stored_id = scratch & 0xFF;
    
    if (magic == NODE_ID_MAGIC_SHIFTED) {
        printf("[Node ID Detection] Using persistent ID from soft reset: %d\n", stored_id);
        // Clear scratch register after use
        watchdog_hw->scratch[SCRATCH_NODE_ID_REG] = 0;
        return stored_id;
    }
    
#ifdef BOOTLOADER_SKIP_NODE_ID
    // V1 hardware: Cold boot, no scratch register - return 0 (app will use NODE_ID_HARDCODED)
    printf("[Node ID Detection] V1 bootloader - cold boot, using placeholder ID 0 (app will handle)\n");
    return 0;
#elif defined(NODE_ID_HARDCODED)
    // Should not reach here, but just in case
    printf("[Node ID Detection] Using hardcoded ID: %d\n", NODE_ID_HARDCODED);
    return NODE_ID_HARDCODED;
#else
    // V2 hardware: No scratch register, read from GPIO pins
    printf("[Node ID Detection] No valid persistent ID, reading GPIOs...\n");
    
    // Cold boot - read from GPIO pins
    // CRITICAL: After watchdog reset, IO_BANK0 peripheral state is not reset!
    // We MUST explicitly reset it or GPIO reads return stale values
    reset_block(RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS);
    unreset_block_wait(RESETS_RESET_IO_BANK0_BITS | RESETS_RESET_PADS_BANK0_BITS);
    
    sleep_ms(1);  // Let hardware settle after reset
    
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
    
    sleep_ms(10);  // GPIOs need time to settle
    
    uint8_t id = 0;
    bool pin0 = raw_pad_value(NODE_ID_PIN0);
    bool pin1 = raw_pad_value(NODE_ID_PIN1);
    bool pin2 = raw_pad_value(NODE_ID_PIN2);
    bool pin3 = raw_pad_value(NODE_ID_PIN3);
    
    printf("[Node ID Detection] GPIO 40-43: %d %d %d %d = ID ", pin0, pin1, pin2, pin3);
    
    if (pin0) id |= (1 << 0);
    if (pin1) id |= (1 << 1);
    if (pin2) id |= (1 << 2);
    if (pin3) id |= (1 << 3);
    
    printf("%d\n", id);
    
    // Store in scratch register for next soft reset
    // Upper 24 bits: magic, Lower 8 bits: node_id
    watchdog_hw->scratch[SCRATCH_NODE_ID_REG] = NODE_ID_MAGIC_SHIFTED | (uint32_t)id;
    
    return id;
#endif
}

/**
 * Initialize system hardware
 */
static void init_system(void) {
    // Initialize LEDs FIRST (before anything else) as debug indicator
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    
    // Red LED on immediately to indicate bootloader mode
    gpio_put(LED_RED_PIN, 1);
    
    // Set voltage and overclock to 266 MHz
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(266000, true);
    
    // Turn off green/blue LEDs (red stays on to indicate bootloader mode)
    gpio_put(LED_GREEN_PIN, 0);
    gpio_put(LED_BLUE_PIN, 0);
    
    // Initialize stdio
    stdio_init_all();
    
#if BOOTLOADER_DEBUG_DELAYS
    // 5 second delay before logging starts (for serial monitor connection)
    sleep_ms(5000);
#else
    // Minimal delay for USB enumeration and serial connection
    sleep_ms(500);
#endif
    
    // Extra delay to ensure serial debug messages are captured
    sleep_ms(100);
    
    // Read node ID
    g_node_id = read_node_id();
    
    printf("\n========================================\n");
    printf("Z1 Onyx Bootloader v%d.%d.%d\n",
           BOOTLOADER_VERSION_MAJOR, BOOTLOADER_VERSION_MINOR, BOOTLOADER_VERSION_PATCH);
    printf("========================================\n");
    printf("Node ID: %d\n", g_node_id);
    printf("CPU Clock: 266 MHz\n");
    printf("Bus Clock: %.1f MHz\n\n", BUS_CLOCK_MHZ);
    
    // Initialize Matrix Bus
    printf("[BOOT] Initializing Matrix Bus @ %.1f MHz...\n", BUS_CLOCK_MHZ);
    z1_bus_init_node();
    z1_bus_set_node_id(g_node_id);
    z1_bus_set_speed_mhz(BUS_CLOCK_MHZ);
    
    // Initialize Broker
    printf("[BOOT] Initializing Z1 Broker...\n");
    z1_broker_init();
    
    // DEBUG: Print node ID again after broker init
    printf("[DEBUG] Node ID after broker init: %d\n", g_node_id);
    
    // Initialize PSRAM
    printf("[BOOT] Initializing PSRAM @ 133 MHz...\n");
    psram_init();
    
    // Initialize OTA engine
    printf("[BOOT] Initializing OTA engine...\n");
    ota_init();
    
    // Load partition table from flash (will fail - no PT embedded)
    // printf("[BOOT] Loading partition table...\n");
    // static uint8_t workarea[4 * 1024] __attribute__((aligned(4)));
    // int rc = rom_load_partition_table(workarea, sizeof(workarea), false);
    // if (rc != 0) {
    //     printf("[BOOT] No partition table (expected - using manual boot)\n");
    // }
    
    printf("[BOOT] System initialization complete\n\n");
}

/**
 * Validate application partition
 * Returns true if app is valid and ready to run
 */
static bool validate_app_partition(void) {
    printf("[BOOT] Validating application partition...\n");
    
    // Read app header from flash (XIP view at 0x10080000)
    app_header_t* header = (app_header_t*)0x10080000;
    
    // Check magic number
    if (header->magic != 0x5A314150) {
        printf("[BOOT] Invalid magic: 0x%08lX (expected 0x5A314150)\n", header->magic);
        return false;
    }
    
    // Check binary size
    if (header->binary_size == 0 || header->binary_size > 6*1024*1024) {
        printf("[BOOT] Invalid binary size: %lu bytes\n", header->binary_size);
        return false;
    }
    
    // Check entry point
    if (header->entry_point != 0xC0) {
        printf("[BOOT] Invalid entry point: 0x%08lX (expected 0x000000C0)\n", header->entry_point);
        return false;
    }
    
    printf("[BOOT] App header valid:\n");
    printf("  Name: %s\n", header->name);
    printf("  Version: %lu.%lu.%lu\n", header->version_major, header->version_minor, header->version_patch);
    printf("  Binary size: %lu bytes\n", header->binary_size);
    printf("  Entry point: 0x%08lX\n", header->entry_point);
    
    // Calculate CRC32 of binary (starts at 0x10080000 + 0xC0)
    printf("[BOOT] Calculating CRC32...\n");
    uint8_t* binary_start = (uint8_t*)(0x10080000 + 0xC0);
    
    // Use proper IEEE 802.3 CRC32 with lookup table (from ota_engine.c)
    // Note: CRC32 calculation on large binaries takes time
    // For a 60KB app at 266 MHz, expect ~100ms
    uint32_t calc_crc = calculate_crc32(binary_start, header->binary_size);
    
    // Compare with stored CRC32
    if (calc_crc != header->crc32) {
        printf("[BOOT] CRC32 mismatch:\n");
        printf("  Calculated: 0x%08lX\n", calc_crc);
        printf("  Stored:     0x%08lX\n", header->crc32);
        return false;
    }
    
    printf("[BOOT] CRC32 valid ✓ (0x%08lX)\n", header->crc32);
    return true;
}

/**
 * Jump to application partition using bootrom
 * Never returns
 */
static void jump_to_app(void) {
    printf("[BOOT] Rebooting to application partition at 0x00080000...\n");
    
#if BOOTLOADER_DEBUG_DELAYS
    // 5-second countdown with early abort on BOOT_NOW command
    printf("\nStarting application in 5 seconds (or on BOOT_NOW command)...\n");
    
    uint32_t start_time = time_us_32();
    uint32_t timeout = 5000000;  // 5 seconds in microseconds
    bool boot_now_received = false;
    
    while ((time_us_32() - start_time) < timeout && !boot_now_received) {
        // Update countdown display every second
        uint32_t elapsed_us = time_us_32() - start_time;
        uint32_t remaining_sec = (timeout - elapsed_us) / 1000000;
        static uint32_t last_displayed_sec = 6;  // Force first print
        
        if (remaining_sec != last_displayed_sec) {
            printf("  %lu...\n", remaining_sec);
            last_displayed_sec = remaining_sec;
        }
        
        // Poll for BOOT_NOW command (every 10ms)
        z1_broker_task();  // Service TX/RX
        
        z1_frame_t frame;
        if (z1_broker_try_receive(&frame)) {
            printf("[BOOT] RX: type=%d src=%d payload[0]=0x%04X\n",
                   frame.type, frame.src, frame.payload[0]);
            
            // Check for BOOT_NOW command
            if (frame.type == Z1_FRAME_TYPE_CTRL && frame.payload[0] == OPCODE_BOOTLOADER_BOOT_NOW) {
                printf("\n[BOOT] BOOT_NOW command received! Skipping countdown...\n");
                boot_now_received = true;
                break;
            }
            
            // Check for OTA commands - enter OTA mode instead of booting
            if (frame.type == Z1_FRAME_TYPE_CTRL) {
                uint16_t opcode = frame.payload[0];
                if (opcode == Z1_OPCODE_UPDATE_MODE_ENTER || opcode == Z1_OPCODE_UPDATE_START) {
                    printf("\n[BOOT] OTA command received (0x%04X)! Entering OTA mode...\n", opcode);
                    ota_handle_enter_update_mode();
                    
                    // Enter safe mode (OTA loop)
                    enter_safe_mode();
                    // Never returns - safe_mode loops forever or reboots
                }
            }
        }
        
        sleep_ms(10);  // Poll every 10ms
    }
    
    if (!boot_now_received) {
        printf("\n");
    }
#else
    // No delay in production mode
    printf("\nStarting application immediately...\n");
#endif
    
    // Direct jump to app (no rom_chain_image - app too large for SRAM copy)
    printf("[BOOT] Performing direct jump to app...\n");
    
    // App partition layout:
    //   0x00080000: 192-byte app header (magic, version, size, CRC, etc.)
    //   0x000800C0: Actual binary starts here (vector table is first)
    uint32_t app_header_start = 0x00080000;
    uint32_t app_binary_start = app_header_start + 192;  // Skip app header
    uint32_t *app_base = (uint32_t *)(0x10000000 + app_binary_start);
    
    printf("[BOOT] App base: 0x%08lX\n", (uint32_t)app_base);
    
    // Read vector table from actual binary (SP at offset 0, Reset handler at offset 4)
    uint32_t stack_pointer = app_base[0];
    uint32_t reset_handler = app_base[1];
    
    printf("[BOOT] SP=0x%08lX, Reset=0x%08lX\n", stack_pointer, reset_handler);
    stdio_flush();
    
    // DON'T deinit USB - app will continue using it
    // stdio_usb_deinit();
    sleep_ms(50);
    
    // Disable ALL interrupts before jump
    __asm__ volatile ("cpsid i" : : : "memory");
    
    // Set vector table offset register to app location
    uint32_t *vtor = (uint32_t *)0xE000ED08;
    *vtor = (uint32_t)app_base;
    __asm__ volatile ("dsb" : : : "memory");
    __asm__ volatile ("isb" : : : "memory");
    
    // Set stack pointer and jump to reset handler with interrupts disabled
    __asm__ volatile (
        "msr msp, %0\n"
        "bx %1\n"
        : : "r" (stack_pointer), "r" (reset_handler)
    );
    
    // Should never reach here
    printf("[ERROR] Jump to app failed\n");
    enter_safe_mode();
    while (1) {
        tight_loop_contents();
    }
    while(1) {
        gpio_put(LED_RED_PIN, 1);
#if BOOTLOADER_DEBUG_DELAYS
        sleep_ms(100);
#else
        sleep_ms(50);  // Faster blink in production
#endif
        gpio_put(LED_RED_PIN, 0);
#if BOOTLOADER_DEBUG_DELAYS
        sleep_ms(100);
#else
        sleep_ms(50);
#endif
    }
}

/**
 * Handle command frame in safe mode
 */
static void handle_safe_mode_command(z1_frame_t* frame) {
    if (frame->length < 2) return;
    
    uint16_t opcode = frame->payload[0];
    
    switch (opcode) {
        case OPCODE_PING: {
            // Respond with PONG
            uint16_t pong = OPCODE_PONG;
            z1_broker_send_command(&pong, 1, frame->src, STREAM_NODE_MGMT);
            break;
        }
        
        case OPCODE_READ_STATUS: {
            // Build status response
            uint16_t response[16];
            response[0] = OPCODE_STATUS_RESPONSE;
            response[1] = g_node_id;
            
            // Uptime
            uint32_t uptime_ms = time_us_32() / 1000;
            response[2] = (uint16_t)(uptime_ms & 0xFFFF);
            response[3] = (uint16_t)(uptime_ms >> 16);
            
            // Memory (full PSRAM available in safe mode)
            uint32_t memory_free = 8 * 1024 * 1024;
            response[4] = (uint16_t)(memory_free & 0xFFFF);
            response[5] = (uint16_t)(memory_free >> 16);
            
            // LED state (red = safe mode)
            response[6] = 255;  // Red
            response[7] = 0;    // Green
            response[8] = 0;    // Blue
            
            // SNN state (not running in bootloader)
            response[9] = 0;   // Not running
            response[10] = 0;  // No neurons
            
            z1_broker_send_command(response, 11, frame->src, STREAM_NODE_MGMT);
            break;
        }
        
        // OTA commands
        case Z1_OPCODE_UPDATE_MODE_ENTER:
            ota_handle_enter_update_mode();
            break;
        
        case Z1_OPCODE_UPDATE_START:
            // UPDATE_START is sent to prepare a specific node for firmware
            // In bootloader, we're already in update mode, just send ACK
            printf("[BOOT] Received UPDATE_START command\n");
            ota_handle_enter_update_mode();  // Ensure we're in update mode
            break;
            
        case Z1_OPCODE_UPDATE_DATA_CHUNK:
            ota_handle_data_chunk(frame);
            break;
            
        case Z1_OPCODE_UPDATE_POLL: {
            // Check poll_type field
            z1_update_poll_t* poll = (z1_update_poll_t*)frame->payload;
            if (poll->poll_node_id == g_node_id || poll->poll_node_id == 0xFF) {
                if (poll->poll_type == Z1_POLL_TYPE_VERIFY) {
                    ota_handle_verify();
                }
            }
            break;
        }
            
        case Z1_OPCODE_UPDATE_COMMIT:
            ota_handle_finalize();
            break;
            
        case Z1_OPCODE_UPDATE_MODE_EXIT:
            ota_handle_exit_update_mode();
            break;
            
        default:
            printf("[BOOT] Unknown opcode 0x%04X in safe mode\n", opcode);
            break;
    }
}

/**
 * Enter safe mode (OTA-only mode)
 * Used when application validation fails
 */
static void enter_safe_mode(void) {
    printf("[BOOT] Entering safe mode (OTA-only)\n");
    printf("[BOOT] Waiting for firmware update via Matrix bus...\n");
    
    // Red LED on (safe mode indicator)
    gpio_put(LED_GREEN_PIN, 0);
    gpio_put(LED_RED_PIN, 1);
    
    uint32_t last_blink = 0;
    bool led_state = true;
    
    while (1) {
        // Run broker task to process TX/RX queues
        z1_broker_task();
        
        // Poll for commands
        z1_frame_t frame;
        if (z1_broker_try_receive(&frame)) {
            // Only handle command frames (Type 1)
            if (frame.type == Z1_FRAME_TYPE_CTRL) {
                handle_safe_mode_command(&frame);
                // Give broker time to drain DMA buffer after processing
                z1_broker_task();
            }
        }
        
        // Blink red LED (1 Hz)
        uint32_t now = time_us_32() / 1000;
        if (now - last_blink > 500) {
            led_state = !led_state;
            gpio_put(LED_RED_PIN, led_state);
            last_blink = now;
        }
        
        sleep_us(10);  // Small delay to prevent DMA starvation
    }
}

/**
 * Main entry point
 */
int main(void) {
    // Normal bootloader flow
    // Initialize hardware
    init_system();
    
    // Validate application partition
    if (validate_app_partition()) {
        // App is valid - jump to it
        jump_to_app();
        // Never returns
    } else {
        // App is invalid - enter safe mode
        printf("[BOOT] Application validation failed\n");
        printf("[BOOT] Reason: Invalid or corrupted application partition\n");
        enter_safe_mode();
        // Never returns
    }
    
    // Should never reach here
    while(1);
}
