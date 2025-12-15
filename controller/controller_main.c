/**
 * Z1 Onyx Controller - Dual Core Architecture
 * 
 * Core 0: Bus/Broker Engine + HTTP Server (dedicated to maximum bus throughput)
 * Core 1: OLED Display + Monitoring (application layer)
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "controller_pins.h"
#include "../common/z1_onyx_bus/z1_bus.h"
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include "../common/core_queue/core_queue.h"
#include "../common/psram/psram_rp2350.h"
#ifdef HW_V2
#include "../common/oled/ssd1306.h"
#endif
#include "w5500_eth.h"

// Hardware configuration
#define CONTROLLER_NODE_ID  16
#define BUS_CLOCK_MHZ       10.0f

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
    
    while (true) {
        // Debug output every 10M iterations
        if (++debug_count % 10000000 == 0) {
            printf("[Core 0] Iterations=%luM, RX=%lu, TX=%lu\n", 
                   debug_count / 1000000, core0_rx_count, core0_tx_count);
        }
        
        // Process HTTP server (needs broker access for API calls)
        w5500_eth_process();
        
        // RX path: Bus → Core 1 application
        z1_frame_t frame;
        if (z1_broker_try_receive(&frame)) {
            if (core_queue_push(&rx_queue, &frame)) {
                core0_rx_count++;
            }
        }
        
        // TX path: Core 1 → Bus
        if (core_queue_pop(&tx_queue, &frame)) {
            uint32_t num_words = frame.length / 2;
            uint8_t flags = frame.no_ack ? Z1_BROKER_NOACK : Z1_BROKER_ACK;
            if (z1_broker_send((uint16_t*)frame.payload, num_words, frame.dest, frame.stream, flags)) {
                core0_tx_count++;
            }
        }
        
        // Drain broker TX queue with CSMA collision avoidance
        z1_broker_task();
        
        // Minimal delay for DMA
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
    ssd1306_write_line("192.168.1.222", 1);
    ssd1306_write_line("Ready", 2);
    ssd1306_update();
#endif

    // Green LED = ready
    gpio_put(LED_GREEN_PIN, 1);
    
    printf("[Core 1] Display ready\n\n");
    
    // Main monitoring loop
    uint32_t loop_count = 0;
    while (true) {
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
    sleep_ms(500);
    
    printf("\n========================================\n");
    printf("Z1 Onyx Controller - Dual Core Mode\n");
    printf("========================================\n\n");
    
    // Initialize LEDs
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    gpio_put(LED_RED_PIN, 1);

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
#endif    // Core queues
    core_queue_init(&rx_queue);
    core_queue_init(&tx_queue);
    
    // PSRAM
    printf("[Core 0] Initializing PSRAM...\n");
    psram_init();
    
#ifdef HW_V2
    // OLED
    ssd1306_init();
#endif
    
    // W5500 Ethernet
    printf("[Core 0] Initializing W5500...\n");
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
    
    gpio_put(LED_RED_PIN, 0);
    gpio_put(LED_BLUE_PIN, 1);
    
    multicore_launch_core1(core1_main);
    
    sleep_ms(100);
    gpio_put(LED_BLUE_PIN, 0);
    
    printf("[Core 0] Starting bus engine...\n\n");
    
    // Enter bus engine loop
    core0_main();
    
    return 0;
}
