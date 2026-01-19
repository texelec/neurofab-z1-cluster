/**
 * Z1 Onyx Controller - Dual Core Architecture
 * Code by NeuroFab Corp: 2025-2026
 * 
 * Core 0: Bus/Broker Engine + HTTP Server (dedicated to maximum bus throughput)
 * Core 1: OLED Display + Monitoring (application layer)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "controller_pins.h"
#include "z1_http_api.h"
#include "../common/z1_onyx_bus/z1_bus.h"
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include "../common/core_queue/core_queue.h"
#include "../common/psram/psram_rp2350.h"
#include "../common/sd_card/sd_card.h"
#include "../common/sd_card/z1_config.h"
#ifdef HW_V2
#include "../common/oled/ssd1306.h"
#endif
#include "w5500_eth.h"

// Hardware configuration
#define CONTROLLER_NODE_ID  16
#define BUS_CLOCK_MHZ       10.0f

// LED state tracking (for dynamic status reporting)
static struct {
    uint8_t red;    // 0-255 brightness
    uint8_t green;  // 0-255 brightness
    uint8_t blue;   // 0-255 brightness
} g_led_state = {0, 0, 0};

/**
 * Initialize LED PWM for brightness control
 */
static void init_led_pwm(void) {
    gpio_set_function(LED_RED_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_GREEN_PIN, GPIO_FUNC_PWM);
    gpio_set_function(LED_BLUE_PIN, GPIO_FUNC_PWM);
    
    uint slice_red = pwm_gpio_to_slice_num(LED_RED_PIN);
    uint slice_green = pwm_gpio_to_slice_num(LED_GREEN_PIN);
    uint slice_blue = pwm_gpio_to_slice_num(LED_BLUE_PIN);
    
    pwm_set_clkdiv(slice_red, 488.0f);
    pwm_set_clkdiv(slice_green, 488.0f);
    pwm_set_clkdiv(slice_blue, 488.0f);
    
    pwm_set_wrap(slice_red, 255);
    pwm_set_wrap(slice_green, 255);
    pwm_set_wrap(slice_blue, 255);
    
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
    
    if (pin == LED_RED_PIN) g_led_state.red = brightness;
    else if (pin == LED_GREEN_PIN) g_led_state.green = brightness;
    else if (pin == LED_BLUE_PIN) g_led_state.blue = brightness;
}

// Inter-core queues (shared memory)
core_queue_t rx_queue;  // Core 0 → Core 1 (incoming bus frames) - GLOBAL for HTTP access
static core_queue_t tx_queue;  // Core 1 → Core 0 (outgoing frames)

// Statistics (atomic reads only)
static volatile uint32_t core0_rx_count = 0;
static volatile uint32_t core0_tx_count = 0;

// ============================================================================
// Core 0: Bus/Broker Engine (Time-Critical)
// ============================================================================

void core0_main(void) {
    // Core 0 runs bus engine + HTTP (both need broker access)
    
    uint32_t debug_count = 0;
    uint32_t spikes_filtered = 0;
    
    while (true) {
        // Debug output every 10M iterations
        if (++debug_count % 10000000 == 0) {
            printf("[Core 0] Iterations=%luM, RX=%lu, TX=%lu, Spikes Filtered=%lu\n", 
                   debug_count / 1000000, core0_rx_count, core0_tx_count, spikes_filtered);
        }
        
        // Process HTTP server (needs broker access for API calls)
        w5500_eth_process();
        
        // Background spike injection processor (async, non-blocking)
        z1_http_api_process_spikes();
        
        // RX path: Let HTTP handlers drain broker directly
        // Spike broadcasts will accumulate but HTTP handlers filter them during waits
        // This avoids race condition where main loop steals controller responses
        
        // TX path: Core 1 → Bus
        z1_frame_t frame;
        if (core_queue_pop(&tx_queue, &frame)) {
            uint32_t num_words = frame.length / 2;
            uint8_t flags = frame.no_ack ? Z1_BROKER_NOACK : Z1_BROKER_ACK;
            // Cast num_words to uint8_t to match broker API (safe since max is 600 words)
            if (z1_broker_send((uint16_t*)frame.payload, (uint8_t)num_words, frame.dest, frame.stream, flags)) {
                core0_tx_count++;
            }
        }
        
        // Drain broker TX queue with CSMA collision avoidance
        z1_broker_task();
        
        // Minimal delay for DMA - broker has built-in backpressure
        sleep_us(1);
    }
}

// ============================================================================
// Core 1: Application Layer (HTTP, OLED, LEDs, SD)
// ============================================================================

void core1_main(void) {
    printf("[Core 1] Starting display/monitoring layer...\n");

#ifdef HW_V2
    // Update OLED display
    ssd1306_clear();
    ssd1306_write_line("Z1 Controller", 0);
    ssd1306_write_line(w5500_get_ip_string(), 1);  // Get IP from W5500 config
    ssd1306_write_line("Ready", 2);
    ssd1306_update();
#endif

    // Green LED at 12.5% brightness = ready
    led_set(LED_GREEN_PIN, 32);
    
    printf("[Core 1] Display ready\n\n");
    
    // Wait for system to fully stabilize before sending BOOT_NOW
    printf("[Core 1] Waiting 4.5 seconds for system to stabilize...\n");
    sleep_ms(4500);
    
    // Broadcast BOOT_NOW once to all nodes using proper broker API
    printf("[Core 1] Broadcasting BOOT_NOW to all nodes...\n");
    uint16_t boot_cmd = OPCODE_BOOTLOADER_BOOT_NOW;
    for (uint8_t node_id = 0; node_id < 16; node_id++) {
        z1_broker_send_command(&boot_cmd, 1, node_id, STREAM_NODE_MGMT);
    }
    
    // Pump broker to transmit
    for (int i = 0; i < 20; i++) {
        z1_broker_task();
        sleep_us(100);
    }
    printf("[Core 1] BOOT_NOW broadcast complete\n\n");
        // Main monitoring loop
    uint32_t loop_count = 0;
    uint32_t heartbeat_cycle_start = 0;
    bool heartbeat_on = false;
    
    while (true) {
        // Blue heartbeat: pulse for 100ms every 3 seconds (20% brightness)
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
        
        // Periodic display update every 10 seconds
        if (++loop_count % 10000000 == 0) {
#ifdef HW_V2
            // Update OLED with stats
            char line[20];
            snprintf(line, sizeof(line), "TX:%lu RX:%lu", core0_tx_count, core0_rx_count);
            ssd1306_write_line(line, 3);
            ssd1306_update();
#endif
        }
        
        // Process incoming bus frames (for monitoring/logging only)
        z1_frame_t frame;
        while (core_queue_pop(&rx_queue, &frame)) {
            printf("[RX] From node %d: type=%d, len=%d\n",
                   frame.src, frame.type, frame.length);
        }
        
        sleep_us(1000);  // 1ms sleep, less critical than Core 0
    }
}

// ============================================================================
// Main: Initialize and Launch Cores
// ============================================================================

int main(void) {
    // Boost voltage for 266 MHz
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(266000, true);
    
    stdio_init_all();
    sleep_ms(2000);  // Wait for serial monitor to settle
    
    printf("\n========================================\n");
    printf("Z1 Onyx Controller - Dual Core Mode\n");
    printf("========================================\n\n");
    
    // Initialize LED PWM
    init_led_pwm();
    
    // Red LED on at startup (indicates initialization)
    led_set(LED_RED_PIN, 255);

#ifdef HW_V2
    // Global reset pin (GPIO33 - active high) - V2 hardware only
    gpio_init(GLOBAL_RESET_PIN);
    gpio_set_dir(GLOBAL_RESET_PIN, GPIO_OUT);
    gpio_put(GLOBAL_RESET_PIN, 1);  // Assert reset
    printf("[Core 0] Resetting all nodes...\n");
    sleep_ms(100);                   // Hold reset for 100ms
    gpio_put(GLOBAL_RESET_PIN, 0);  // Release reset
    printf("[Core 0] Nodes booting...\n");
    sleep_ms(500);                   // Wait for nodes to boot
    printf("[Core 0] All nodes should be online\n\n");
#else
    // V1 hardware: No global reset, nodes boot independently
    printf("[Core 0] Waiting for nodes to boot...\n");
    sleep_ms(500);
    printf("[Core 0] All nodes should be online\n\n");
#endif
    
    /**
     * INITIALIZATION SEQUENCE
     * 
     * 1. Core queues (RX/TX between Core 0 and Core 1)
     * 2. PSRAM (8MB, zones: 0-64KB=FatFS, 64-128KB=HTTP, 128KB+=OTA)
     * 3. SD card (SPI1, FAT32)
     *    - Mount filesystem
     *    - Load z1.cfg if present
     *    - Extract IP/MAC configuration
     * 4. OLED (V2 only, I2C0)
     * 5. W5500 Ethernet
     *    - Apply IP/MAC from z1.cfg (if loaded)
     *    - Otherwise use hardcoded defaults in w5500_eth.c
     *    - Initialize hardware and start HTTP server
     * 6. Matrix bus (Core 0 background task)
     * 
     * Network Configuration Priority:
     *   1. z1.cfg on SD card (runtime, modifiable via zconfig tool)
     *   2. Hardcoded defaults in w5500_eth.c (fallback if SD missing/corrupt)
     */
    
    // Core queues
    core_queue_init(&rx_queue);
    core_queue_init(&tx_queue);
    
    // PSRAM
    printf("[Core 0] Initializing PSRAM...\n");
    psram_init();
    
    // PSRAM buffers will be initialized on first use
    // NOTE: Do NOT use memset() on cached PSRAM (0x11000000) - causes cache coherency issues!
    // If clearing needed, use psram_write() with uncached address (0x15000000)
    printf("[Core 0] PSRAM buffers ready\n");
    
    // SD Card
    printf("[Core 0] Initializing SD card...\n");
    z1_config_t config;
    bool config_loaded = false;
    
    if (sd_card_init()) {
        printf("[Core 0] SD card mounted\n");
        
        printf("[Core 0] Creating directory structure...\n");
        bool eng = sd_card_create_directory("engines");
        bool top = sd_card_create_directory("topologies");
        printf("[Core 0] Directories created (engines=%d, topologies=%d)\n", eng, top);
        
        printf("[Core 0] Loading config...\n");
        if (z1_config_load_or_default(&config)) {
            config_loaded = true;
            printf("[Core 0] ========== CONFIG FILE DEBUG ==========\n");
            printf("[Core 0] IP Address: %d.%d.%d.%d\n",
                   config.ip_address[0], config.ip_address[1],
                   config.ip_address[2], config.ip_address[3]);
            printf("[Core 0] MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n",
                   config.mac_address[0], config.mac_address[1],
                   config.mac_address[2], config.mac_address[3],
                   config.mac_address[4], config.mac_address[5]);
            printf("[Core 0] Engine: %s\n", config.current_engine);
            printf("[Core 0] HW Version: %d\n", config.hw_version);
            printf("[Core 0] Node Count: %d\n", config.node_count);
            printf("[Core 0] ========================================\n");
            printf("[Core 0] Config will be applied to W5500\n");
        }
    } else {
        printf("[Core 0] SD card init failed (continuing without SD)\n");
    }

#ifdef HW_V2
    // OLED
    ssd1306_init();
#endif
    
    // W5500 Ethernet - Apply config from SD card if available
    printf("[Core 0] Initializing W5500...\n");
    if (config_loaded) {
        w5500_set_network_config(config.ip_address, config.mac_address);
    }
    w5500_eth_init();
    w5500_eth_start_server(80);
    
    // Z1 Bus
    printf("[Core 0] Initializing Z1 bus @ %.1f MHz...\n", BUS_CLOCK_MHZ);
    z1_bus_init_controller();
    z1_bus_set_node_id(CONTROLLER_NODE_ID);
    z1_bus_set_speed_mhz(BUS_CLOCK_MHZ);
    
    // Broker
    printf("[Core 0] Initializing broker...\n");
    z1_broker_init();
    
    printf("[Core 0] Launching Core 1...\n\n");
    
    // Turn off red LED (initialization complete)
    led_set(LED_RED_PIN, 0);
    
    multicore_launch_core1(core1_main);
    
    printf("[Core 0] Starting bus engine...\n\n");
    
    // Enter bus engine loop
    core0_main();
    
    return 0;
}
