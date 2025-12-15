/**
 * Z1 Onyx Node - SNN Execution Node
 * 
 * Runs distributed spiking neural network on RP2350B hardware.
 * Handles spike processing, neuron updates, and bus communication.
 */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/vreg.h"
#include "hardware/clocks.h"
#include "hardware/regs/io_bank0.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/padsbank0.h"
#include "node_pins.h"
#include "../common/z1_onyx_bus/z1_bus.h"
#include "../common/z1_broker/z1_broker.h"
#include "../common/z1_commands/z1_commands.h"
#include "../common/psram/psram_rp2350.h"
#include "z1_snn_engine.h"

#define BUS_CLOCK_MHZ  10.0f
#define CONTROLLER_ID  16

static uint8_t my_node_id = 0xFF;

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
 * Read node ID from hardware pins (GPIO 40-43, 4-bit)
 */
static uint8_t read_node_id(void) {
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
}

static void init_system(void) {
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(266000, true);
    
    stdio_init_all();
    sleep_ms(500);
    
    my_node_id = read_node_id();
    
    printf("\n========================================\n");
    printf("Z1 Onyx Node - SNN Execution Node\n");
    printf("========================================\n");
    printf("Node ID: %d\n", my_node_id);
    printf("Bus Speed: %.1f MHz\n\n", BUS_CLOCK_MHZ);
    
    // LEDs
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);
    gpio_init(LED_BLUE_PIN);
    gpio_set_dir(LED_BLUE_PIN, GPIO_OUT);
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);
    
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
}

/**
 * Handle command frames (CTRL stream)
 */
static void handle_command_frame(z1_frame_t* frame) {
    if (frame->length < 2) return;  // Need at least opcode
    
    uint16_t opcode = frame->payload[0];
    
    switch (opcode) {
        case OPCODE_PING: {
            printf("[CMD] PING from node %d\n", frame->src);
            
            // Respond with PONG
            uint16_t pong = OPCODE_PONG;
            z1_broker_send_command(&pong, 1, frame->src, STREAM_NODE_MGMT);
            break;
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
            // TODO: Implement dynamic memory tracking - currently reports full PSRAM size
            // Real usage depends on SNN topology (neurons, weights stored in PSRAM)
            // Need to add allocation tracking to psram_rp2350.c or implement heap allocator
            uint32_t memory_free = 8 * 1024 * 1024;  // 8 MB in bytes
            response[4] = (uint16_t)(memory_free & 0xFFFF);
            response[5] = (uint16_t)(memory_free >> 16);
            
            // LED state (R, G, B as separate words for easy parsing)
            // TODO: Track actual LED state - for now return defaults
            response[6] = 0;    // Red (off)
            response[7] = 255;  // Green (on - indicates ready)
            response[8] = 0;    // Blue (off)
            
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
            
            printf("[CMD] WRITE_MEMORY addr=0x%08lX len=%d from node %d\n", addr, length, frame->src);
            
            // Data starts at payload[6] (12 bytes header)
            uint8_t* data_ptr = (uint8_t*)&frame->payload[6];
            
            // frame->length is in BYTES, payload is in WORDS
            // Header is 6 words (12 bytes), then data follows
            uint16_t header_bytes = 12;
            uint16_t expected_frame_bytes = header_bytes + length;
            
            // Verify we have enough data in frame
            if (frame->length >= expected_frame_bytes) {
                // Debug: Print first 32 bytes BEFORE write
                printf("[DEBUG] First 32 bytes to write:\n  ");
                for (int i = 0; i < 32 && i < length; i++) {
                    printf("%02X ", data_ptr[i]);
                    if ((i + 1) % 16 == 0) printf("\n  ");
                }
                printf("\n");
                
                printf("[DEBUG] About to call psram_write(0x%08lX, data_ptr, %d)\n", addr, length);
                psram_write(addr, data_ptr, length);
                printf("[DEBUG] psram_write() returned\n");
                printf("  Wrote %d bytes to PSRAM\n", length);
                
                // Debug: Read back and verify first 32 bytes
                if (length >= 32) {
                    uint8_t verify[32];
                    psram_read(addr, verify, 32);
                    printf("[DEBUG] First 32 bytes read from PSRAM:\n  ");
                    for (int i = 0; i < 32; i++) {
                        printf("%02X ", verify[i]);
                        if ((i + 1) % 16 == 0) printf("\n  ");
                    }
                    printf("\n");
                }
                
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
        
        default:
            printf("[CMD] Unknown opcode 0x%04X from node %d\n", opcode, frame->src);
            break;
    }
}

static void idle_node_loop(void) {
    printf("Node %d: Idle mode (processing commands) [BUILD: Dec 10, 2025]\n", my_node_id);
    printf("  Broker task running in background...\n\n");
    
    uint32_t loop_count = 0;
    uint32_t last_snn_step_us = 0;
    const uint32_t SNN_TIMESTEP_US = 1000;  // Run SNN at 1 kHz (1ms timestep)
    
    // Process frames and handle commands
    while (true) {
        loop_count++;
        if (loop_count % 10000000 == 0) {
            printf("[Node %d] Alive: %luM iterations\n", my_node_id, loop_count / 1000000);
        }
        
        z1_broker_task();
        
        // Check for incoming frames
        z1_frame_t frame;
        if (z1_broker_try_receive(&frame)) {
            printf("[Node %d] *** FRAME RECEIVED ***: type=%d, src=%d, dest=%d, len=%d\n", 
                   my_node_id, frame.type, frame.src, frame.dest, frame.length);
            
            // Handle CTRL frames (commands)
            if (frame.type == Z1_FRAME_TYPE_CTRL) {
                handle_command_frame(&frame);
            }
            // Handle UNICAST frames (spike injection from controller)
            else if (frame.type == Z1_FRAME_TYPE_UNICAST) {
                // Inject spike into local SNN engine
                // Payload: [local_neuron_id_low, local_neuron_id_high]
                // Controller sends these via POST /api/snn/input after decoding global neuron IDs
                if (frame.length >= 4) {
                    z1_spike_t spike;
                    spike.neuron_id = (uint32_t)frame.payload[0] | ((uint32_t)frame.payload[1] << 16);
                    spike.timestamp_us = time_us_32();
                    spike.value = 1.0f;  // Default spike value
                    
                    printf("[Node %d] Injecting spike: neuron_id=%lu\n", my_node_id, spike.neuron_id);
                    z1_snn_inject_spike(spike);
                } else {
                    printf("[Node %d] ERROR: Spike frame too short (len=%d)\n", my_node_id, frame.length);
                }
            }
            // Handle BROADCAST frames (spikes)
            else if (frame.type == Z1_FRAME_TYPE_BROADCAST) {
                // Inject spike into SNN engine (payload is 2 words = 4 bytes)
                if (frame.length >= 4) {
                    z1_spike_t spike;
                    spike.neuron_id = (uint32_t)frame.payload[0] | ((uint32_t)frame.payload[1] << 16);
                    spike.timestamp_us = time_us_32();
                    spike.value = 1.0f;  // Default spike value
                    
                    printf("[Node %d] Injecting BROADCAST spike: neuron_id=%lu\n", my_node_id, spike.neuron_id);
                    z1_snn_inject_spike(spike);
                } else {
                    printf("[Node %d] ERROR: Broadcast spike frame too short (len=%d)\n", my_node_id, frame.length);
                }
            }
        }
        
        // Run SNN timestep if engine is running AND enough time has elapsed
        // Timestep: 1ms (1kHz update rate) - configurable based on network requirements
        if (z1_snn_is_running()) {
            uint32_t now_us = time_us_32();
            if ((now_us - last_snn_step_us) >= SNN_TIMESTEP_US) {
                last_snn_step_us = now_us;
                
                // Execute one SNN timestep:
                // 1. Process queued spikes (integrate into membrane potentials)
                // 2. Apply leak to all neurons
                // 3. Check threshold and generate output spikes
                z1_snn_step();
                
                // Service broker IMMEDIATELY after SNN step (critical for responsiveness)
                z1_broker_task();
                
                // Broadcast output spikes to cluster
                // Other nodes will receive and integrate based on their synaptic connections
                uint16_t spike_count;
                const z1_spike_t* spikes = z1_snn_get_output_spikes(&spike_count);
                
                for (uint16_t i = 0; i < spike_count; i++) {
                    // Pack spike into bus frame format
                    // Format: [neuron_id_low(16), neuron_id_high(8), value_scaled(16)]
                    uint16_t spike_data[3];
                    spike_data[0] = (uint16_t)(spikes[i].neuron_id & 0xFFFF);
                    spike_data[1] = (uint16_t)((spikes[i].neuron_id >> 16) & 0xFF);
                    spike_data[2] = (uint16_t)(spikes[i].value * 1000.0f);  // Scale float to int
                    
                    // BROADCAST to all nodes (dest=31)
                    // Each node filters spikes based on their neurons' synaptic connections
                    // Stream 4 = spike stream (high priority)
                    z1_broker_send_spike(spike_data, 3, 31, 4);
                }
                
                // Service broker again after queueing spikes (ensures transmission)
                z1_broker_task();
            }
        }
        
        // NO SLEEP - keep running at full speed to service bus
        tight_loop_contents();
    }
}

int main(void) {
    init_system();
    
    // All nodes run the same idle command handler
    gpio_put(LED_GREEN_PIN, 1);  // Green = ready
    idle_node_loop();
    
    return 0;
}
